/**
 * @file lv_sifli_epic_cfg.c
 *
 */

/**
 * Copyright 2024 SiFli Technologies
 *
 * SPDX-License-Identifier: MIT
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_sifli_epic_cfg.h"
#include "lv_sifli_epic_osa.h"

#if LV_USE_SIFLI_EPIC
#include "../../../misc/lv_log.h"
#include "../../../tick/lv_tick.h"
#include "system_bf0_ap.h"
#include <string.h>

#if LV_USE_NUTTX
    #include <nuttx/arch.h>
    #include <nuttx/cache.h>
#endif

#if defined(__ZEPHYR__)
    #include <zephyr/arch/cache.h>
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

static EPIC_HandleTypeDef epic_handle;
static EPIC_TypeDef epic_ram_instance;
#ifdef HAL_EZIP_MODULE_ENABLED
    static EZIP_HandleTypeDef ezip_handle;
#endif
static bool epic_initialized = false;
static bool epic_cont_mode = false;
static lv_epic_cplt_cbk epic_async_cb;
/* SiFli drv_epic epic_sema: 1 = GPU idle, 0 = op in flight / cont session */
static volatile uint8_t epic_gpu_token = 1;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static HAL_StatusTypeDef epic_prepare_start(EPIC_HandleTypeDef * epic,
                                            lv_epic_cplt_cbk cb);
#if !LV_USE_NUTTX
static void epic_xfer_cplt_callback(EPIC_HandleTypeDef * epic);
static HAL_StatusTypeDef epic_wait_async_result(HAL_StatusTypeDef start_status);
static void lv_epic_wait_gpu_done(void);
#endif
static void lv_epic_flush_layers_cache(EPIC_LayerConfigTypeDef * input_layers, uint8_t input_layer_cnt,
                                       EPIC_LayerConfigTypeDef * output_layer);
static uint32_t epic_layer_cache_size(const EPIC_LayerConfigTypeDef * layer);
#if !LV_USE_NUTTX
static void lv_epic_reconcile_state(EPIC_HandleTypeDef * epic);
static void lv_epic_reconcile_done(EPIC_HandleTypeDef * epic);
static void epic_start_failed_cleanup(void);
#endif

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_epic_init(void)
{
    if(epic_initialized) {
        return;
    }

    memset(&epic_handle, 0, sizeof(epic_handle));
    epic_handle.Instance = LV_SIFLI_EPIC_INSTANCE;
    epic_handle.RamInstance = &epic_ram_instance;

#ifdef HAL_EZIP_MODULE_ENABLED
    memset(&ezip_handle, 0, sizeof(ezip_handle));
    ezip_handle.Instance = EZIP;
    epic_handle.hezip = &ezip_handle;

    if(HAL_EZIP_Init(epic_handle.hezip) != HAL_OK) {
        return;
    }
#endif

    if(HAL_EPIC_Init(&epic_handle) != HAL_OK) {
        return;
    }

    if(lv_epic_osa_init() != LV_RESULT_OK) {
        memset(&epic_handle, 0, sizeof(epic_handle));
#ifdef HAL_EZIP_MODULE_ENABLED
        memset(&ezip_handle, 0, sizeof(ezip_handle));
#endif
        return;
    }

    epic_initialized = true;
}

void lv_epic_deinit(void)
{
    if(!epic_initialized) {
        return;
    }

    lv_epic_wait();
    lv_epic_osa_deinit();

    memset(&epic_handle, 0, sizeof(epic_handle));
#ifdef HAL_EZIP_MODULE_ENABLED
    memset(&ezip_handle, 0, sizeof(ezip_handle));
#endif
    epic_async_cb = NULL;
    epic_initialized = false;
}

void lv_epic_reset(void)
{
    if(!epic_initialized) {
        return;
    }

    (void)HAL_EPIC_Init(&epic_handle);
}

bool lv_epic_is_initialized(void)
{
    return epic_initialized;
}

bool lv_epic_is_busy(void)
{
    if(!epic_initialized) {
        return false;
    }

    if(epic_handle.State == HAL_EPIC_STATE_BUSY) {
        return true;
    }

#ifdef HAL_EZIP_MODULE_ENABLED
    if(ezip_handle.State == HAL_EZIP_STATE_BUSY) {
        return true;
    }
#endif

    return false;
}

bool lv_epic_is_hardware_active(void)
{
    if(!epic_initialized || epic_handle.Instance == NULL) {
        return false;
    }

    /* Only IA_BUSY reflects in-flight GPU ops. HAL State==BUSY can outlive HW
     * when EOF IRQ is not serviced (NuttX); polling completion fixes that. */
    if((epic_handle.Instance->STATUS & EPIC_STATUS_IA_BUSY_Msk) != 0U) {
        return true;
    }

#ifdef HAL_EZIP_MODULE_ENABLED
    if(ezip_handle.State == HAL_EZIP_STATE_BUSY) {
        return true;
    }
#endif

    return false;
}

void lv_epic_poll_pending_completion(void)
{
    EPIC_HandleTypeDef * epic = lv_epic_get_handle();

    if(epic == NULL || epic->Instance == NULL) {
        return;
    }

    if(epic->IntXferCpltCallback == NULL && epic->XferCpltCallback == NULL) {
        return;
    }

    uint32_t eof = epic->Instance->EOF_IRQ;
    bool irq_pending = ((eof & EPIC_EOF_IRQ_IRQ_STATUS_Msk) != 0U)
                       || ((eof & EPIC_EOF_IRQ_IRQ_CAUSE_Msk) != 0U);
    bool hw_idle = ((epic->Instance->STATUS & EPIC_STATUS_IA_BUSY_Msk) == 0U);

    if(irq_pending || hw_idle) {
        HAL_EPIC_IRQHandler(epic);
    }
}

void lv_epic_gpu_token_release(void)
{
    epic_gpu_token = 1;
}

void lv_epic_gpu_check_done(void)
{
#if LV_USE_NUTTX
    /* NuttX uses sync HAL (EPIC_WaitDone inside HAL); no async OSA wait. */
    return;
#else
    /* SiFli drv_gpu_check_done(): ensure async IT completed */
    if(epic_gpu_token != 0U && !lv_epic_is_hardware_active()) {
        return;
    }

    lv_epic_poll_pending_completion();
    if(lv_epic_is_hardware_active()) {
        lv_epic_wait();
    }
    lv_epic_reconcile_done(lv_epic_get_handle());
#endif
}

void lv_epic_run(void)
{
    epic_osa_cfg_t * cfg = epic_get_default_cfg();
    if(cfg && cfg->epic_run) {
        cfg->epic_run();
    }
}

void lv_epic_wait(void)
{
    epic_osa_cfg_t * cfg = epic_get_default_cfg();
    if(cfg && cfg->epic_wait) {
        cfg->epic_wait();
    }
}

void lv_epic_gpu_reset(void)
{
    if(!epic_initialized) {
        return;
    }

#if LV_USE_NUTTX
    irqstate_t flags = up_irq_save();
#else
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
#endif

    epic_handle.State = HAL_EPIC_STATE_READY;
    epic_handle.ErrorCode = 0;
    epic_handle.IntXferCpltCallback = NULL;
    epic_handle.XferCpltCallback = NULL;

#ifdef HAL_EZIP_MODULE_ENABLED
    ezip_handle.State = HAL_EZIP_STATE_READY;
    ezip_handle.ErrorCode = 0;
    ezip_handle.CpltCallback = NULL;
    HAL_RCC_ResetModule(RCC_MOD_EZIP);
    HAL_NVIC_ClearPendingIRQ(LV_SIFLI_EZIP_IRQn);
#endif

    HAL_RCC_ResetModule(RCC_MOD_EPIC);
    HAL_NVIC_ClearPendingIRQ(LV_SIFLI_EPIC_IRQn);
    epic_async_cb = NULL;
    epic_cont_mode = false;
    epic_gpu_token = 1;

#if LV_USE_NUTTX
    up_irq_restore(flags);
#else
    if(primask == 0U) {
        __enable_irq();
    }
#endif

    (void)HAL_EPIC_Init(&epic_handle);
#ifdef HAL_EZIP_MODULE_ENABLED
    (void)HAL_EZIP_Init(&ezip_handle);
#endif

    lv_epic_osa_set_idle();
}

EPIC_HandleTypeDef * lv_epic_get_handle(void)
{
    if(!epic_initialized) {
        return NULL;
    }

    return &epic_handle;
}

#ifdef HAL_EZIP_MODULE_ENABLED
EZIP_HandleTypeDef * lv_ezip_get_handle(void)
{
    if(!epic_initialized) {
        return NULL;
    }

    return &ezip_handle;
}
#endif

void lv_epic_flush_cache_range(const void * data, uint32_t size)
{
    if(data == NULL || size == 0U) {
        return;
    }

#if defined(__ZEPHYR__)
    (void)arch_dcache_flush_range((void *)data, size);
#elif LV_USE_NUTTX
#ifdef CONFIG_ARCH_DCACHE
    {
      uintptr_t start = (uintptr_t)data;
      uintptr_t end = start + (uintptr_t)size;

      up_clean_dcache(start, end);
    }
#endif
#else
    (void)mpu_dcache_clean((void *)data, size);
#endif
}

void lv_epic_invalidate_cache_range(const void * data, uint32_t size)
{
    if(data == NULL || size == 0U) {
        return;
    }

#if defined(__ZEPHYR__)
    (void)arch_dcache_invd_range((void *)data, size);
#elif LV_USE_NUTTX
#ifdef CONFIG_ARCH_DCACHE
    {
      uintptr_t start = (uintptr_t)data;
      uintptr_t end = start + (uintptr_t)size;

      up_invalidate_dcache(start, end);
    }
#endif
#else
    (void)mpu_dcache_invalidate((void *)data, size);
#endif
}

bool lv_epic_is_cached_ram(uint32_t start, uint32_t len)
{
    /* Platform-specific implementation
     * This should check if the memory region is in cached RAM.
     * For now, return a conservative default.
     */
    LV_UNUSED(start);
    LV_UNUSED(len);

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    /* If D-Cache is present, assume memory might be cached */
    return true;
#else
    return false;
#endif
}

HAL_StatusTypeDef lv_epic_fill(EPIC_LayerConfigTypeDef * input_layers, uint8_t input_layer_cnt,
                               EPIC_LayerConfigTypeDef * output_layer)
{
    EPIC_HandleTypeDef * epic = lv_epic_get_handle();

    if(epic_prepare_start(epic, NULL) != HAL_OK) {
        return HAL_ERROR;
    }

    lv_epic_flush_layers_cache(input_layers, input_layer_cnt, output_layer);

#if LV_USE_NUTTX
    HAL_StatusTypeDef ret;

    if(input_layer_cnt == 0U) {
        EPIC_FillingCfgTypeDef param;

        HAL_EPIC_FillDataInit(&param);
        param.start = output_layer->data;
        param.color_mode = output_layer->color_mode;
        param.width = output_layer->width;
        param.height = output_layer->height;
        param.total_width = output_layer->total_width;
        param.color_r = output_layer->color_r;
        param.color_g = output_layer->color_g;
        param.color_b = output_layer->color_b;
        param.alpha = EPIC_LAYER_OPAQUE;
        ret = HAL_EPIC_FillStart(epic, &param);
    }
    else {
        ret = HAL_EPIC_BlendStartEx(epic, input_layers, input_layer_cnt, output_layer);
    }
    lv_epic_osa_set_idle();
    return ret;
#else
    HAL_StatusTypeDef start_status = HAL_EPIC_BlendStartEx_IT(epic, input_layers, input_layer_cnt, output_layer);
    return epic_wait_async_result(start_status);
#endif
}

HAL_StatusTypeDef lv_epic_blend(EPIC_LayerConfigTypeDef * input_layers, uint8_t input_layer_cnt,
                                EPIC_LayerConfigTypeDef * output_layer)
{
    EPIC_HandleTypeDef * epic = lv_epic_get_handle();

    if(epic_prepare_start(epic, NULL) != HAL_OK) {
        return HAL_ERROR;
    }

    lv_epic_flush_layers_cache(input_layers, input_layer_cnt, output_layer);

#if LV_USE_NUTTX
    HAL_StatusTypeDef ret = HAL_EPIC_BlendStartEx(epic, input_layers, input_layer_cnt, output_layer);
    lv_epic_osa_set_idle();
    return ret;
#else
    HAL_StatusTypeDef start_status = HAL_EPIC_BlendStartEx_IT(epic, input_layers, input_layer_cnt, output_layer);
    return epic_wait_async_result(start_status);
#endif
}

HAL_StatusTypeDef lv_epic_fill_grad(EPIC_GradCfgTypeDef * param)
{
    EPIC_HandleTypeDef * epic = lv_epic_get_handle();

    if(epic_prepare_start(epic, NULL) != HAL_OK) {
        return HAL_ERROR;
    }

    if(param != NULL && param->start != NULL) {
        uint32_t size = (uint32_t)param->total_width * param->height
                        * (HAL_EPIC_GetColorDepth(param->color_mode) / 8U);
        lv_epic_flush_cache_range(param->start, size);
    }

#if LV_USE_NUTTX
    HAL_StatusTypeDef ret = HAL_EPIC_FillGrad(epic, param);
    lv_epic_osa_set_idle();
    return ret;
#else
    HAL_StatusTypeDef start_status = HAL_EPIC_FillGrad_IT(epic, param);
    return epic_wait_async_result(start_status);
#endif
}

HAL_StatusTypeDef lv_epic_cont_blend(EPIC_LayerConfigTypeDef * input_layers, uint8_t input_layer_cnt,
                                      EPIC_LayerConfigTypeDef * output_layer)
{
    EPIC_HandleTypeDef * epic = lv_epic_get_handle();
    EPIC_LayerConfigTypeDef * fg_layer;
    EPIC_LayerConfigTypeDef * mask_layer;
    HAL_StatusTypeDef ret;

    if(epic == NULL || input_layers == NULL || output_layer == NULL || input_layer_cnt < 2) {
        return HAL_ERROR;
    }

    fg_layer = &input_layers[1];
    mask_layer = (input_layer_cnt > 2) ? &input_layers[2] : NULL;

    if(!epic_cont_mode) {
        if(epic_prepare_start(epic, NULL) != HAL_OK) {
            return HAL_ERROR;
        }
        lv_epic_flush_layers_cache(input_layers, input_layer_cnt, output_layer);

        ret = HAL_EPIC_ContBlendStart(epic, fg_layer, mask_layer, output_layer);
        if(ret == HAL_OK) {
            epic_cont_mode = true;
        }
        else {
            lv_epic_osa_set_idle();
        }
        return ret;
    }

    if(fg_layer->data != NULL && fg_layer->data_size > 0U) {
        lv_epic_flush_cache_range(fg_layer->data, fg_layer->data_size);
    }
    if(mask_layer != NULL && mask_layer->data != NULL && mask_layer->data_size > 0U) {
        lv_epic_flush_cache_range(mask_layer->data, mask_layer->data_size);
    }

    return HAL_EPIC_ContBlendRepeat(epic, fg_layer, mask_layer, output_layer);
}

void lv_epic_cont_blend_reset(void)
{
    EPIC_HandleTypeDef * epic = lv_epic_get_handle();

    if(epic == NULL || !epic_cont_mode) {
        return;
    }

    (void)HAL_EPIC_ContBlendStop(epic);
    epic_cont_mode = false;
    epic_gpu_token = 1;
    lv_epic_osa_set_idle();
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#if !LV_USE_NUTTX

static void epic_xfer_cplt_callback(EPIC_HandleTypeDef * epic)
{
    LV_UNUSED(epic);
    lv_epic_osa_thread_sync_signal_isr();
    lv_epic_gpu_token_release();

    if(epic_async_cb != NULL) {
        lv_epic_cplt_cbk cb = epic_async_cb;
        epic_async_cb = NULL;
        cb(epic);
    }
}

static HAL_StatusTypeDef epic_wait_async_result(HAL_StatusTypeDef start_status)
{
    if(start_status != HAL_OK) {
        LV_LOG_WARN("EPIC start failed: status=%d", (int)start_status);
        epic_start_failed_cleanup();
        return start_status;
    }

    lv_epic_wait();
    lv_epic_reconcile_done(&epic_handle);
    return HAL_OK;
}

#endif /* !LV_USE_NUTTX */

static uint32_t epic_layer_cache_size(const EPIC_LayerConfigTypeDef * layer)
{
    if(layer == NULL || layer->data == NULL) {
        return 0U;
    }

    if(layer->data_size > 0U) {
        return layer->data_size;
    }

    uint32_t bpp = HAL_EPIC_GetColorDepth(layer->color_mode);
    return ((uint32_t)layer->total_width * layer->height * bpp + 7U) / 8U;
}

static void lv_epic_flush_layers_cache(EPIC_LayerConfigTypeDef * input_layers, uint8_t input_layer_cnt,
                                       EPIC_LayerConfigTypeDef * output_layer)
{
    uint8_t i;

    for(i = 0; i < input_layer_cnt; i++) {
        uint32_t size = epic_layer_cache_size(&input_layers[i]);
        if(size > 0U) {
            lv_epic_flush_cache_range(input_layers[i].data, size);
        }
    }

    if(output_layer != NULL) {
        uint32_t size = epic_layer_cache_size(output_layer);
        if(size > 0U) {
            lv_epic_flush_cache_range(output_layer->data, size);
        }
    }
}

#if !LV_USE_NUTTX

static void lv_epic_reconcile_state(EPIC_HandleTypeDef * epic)
{
    if(epic == NULL) {
        return;
    }

    lv_epic_poll_pending_completion();

    if(!lv_epic_is_hardware_active()) {
        epic->State = HAL_EPIC_STATE_READY;
        epic->IntXferCpltCallback = NULL;
    }
}

static void lv_epic_reconcile_done(EPIC_HandleTypeDef * epic)
{
    lv_epic_reconcile_state(epic);
    if(epic != NULL && !lv_epic_is_hardware_active() && epic_gpu_token == 0U) {
        epic_gpu_token = 1U;
    }
}

static void lv_epic_wait_gpu_done(void)
{
    uint32_t start = lv_tick_get();

    /* SiFli wait_gpu_done(): cont_blend_reset + drv_gpu_take */
    lv_epic_cont_blend_reset();

    while(epic_gpu_token == 0U) {
        lv_epic_poll_pending_completion();
        if(!lv_epic_is_hardware_active()) {
            lv_epic_reconcile_done(&epic_handle);
        }
        else {
            lv_epic_wait();
        }
        if(epic_gpu_token != 0U) {
            break;
        }
        if(lv_tick_elaps(start) > LV_SIFLI_EPIC_WAIT_TIMEOUT_MS) {
            LV_LOG_WARN("EPIC wait_gpu_done timeout");
            lv_epic_gpu_reset();
            break;
        }
        lv_delay_ms(1);
    }

    epic_gpu_token = 0U;
}

#endif /* !LV_USE_NUTTX */

#if LV_USE_NUTTX

static HAL_StatusTypeDef epic_prepare_sync_nuttx(EPIC_HandleTypeDef * epic)
{
    uint32_t start;

    lv_epic_cont_blend_reset();

    if(epic == NULL || epic->Instance == NULL) {
        return HAL_ERROR;
    }

    if(!lv_epic_is_hardware_active()) {
        epic->State = HAL_EPIC_STATE_READY;
        return HAL_OK;
    }

    start = lv_tick_get();
    while(lv_epic_is_hardware_active()) {
        if(lv_tick_elaps(start) > 50U) {
            lv_epic_gpu_reset();
            break;
        }
    }
    epic->State = HAL_EPIC_STATE_READY;
    return HAL_OK;
}

#endif /* LV_USE_NUTTX */

static HAL_StatusTypeDef epic_prepare_start(EPIC_HandleTypeDef * epic,
                                            lv_epic_cplt_cbk cb)
{
    if(epic == NULL) {
        return HAL_ERROR;
    }

#if LV_USE_NUTTX
    return epic_prepare_sync_nuttx(epic);
#else
    lv_epic_wait_gpu_done();
    lv_epic_reconcile_state(epic);

    if(lv_epic_is_hardware_active()) {
        lv_epic_wait();
        lv_epic_reconcile_state(epic);
    }

    if(lv_epic_is_hardware_active()) {
        LV_LOG_WARN("EPIC HW still busy before start, resetting GPU");
        lv_epic_gpu_reset();
    }

    epic_async_cb = cb;
    epic->XferCpltCallback = epic_xfer_cplt_callback;
    return HAL_OK;
#endif
}

#if !LV_USE_NUTTX

static void epic_start_failed_cleanup(void)
{
    epic_handle.XferCpltCallback = NULL;
    epic_async_cb = NULL;
    epic_gpu_token = 1;
    lv_epic_osa_set_idle();
}

#endif /* !LV_USE_NUTTX */

#endif /*LV_USE_SIFLI_EPIC*/
