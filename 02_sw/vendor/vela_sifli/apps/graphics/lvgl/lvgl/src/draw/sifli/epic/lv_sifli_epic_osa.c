/**
 * @file lv_sifli_epic_osa.c
 *
 * NuttX: GPU completion is detected by polling HAL state (like EPIC_WaitDone /
 * drv_gpu_check_done), not nxsem_tickwait — IRQ context cannot reliably wake
 * thread waiters with our sem usage pattern on NuttX.
 */

/**
 * Copyright 2024 SiFli Technologies
 *
 * SPDX-License-Identifier: MIT
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_sifli_epic_osa.h"
#include "lv_sifli_epic_cfg.h"

#if LV_USE_SIFLI_EPIC
#include "../../../misc/lv_log.h"
#include "../../../osal/lv_os_private.h"
#include "../../../tick/lv_tick.h"

#if defined(__ZEPHYR__)
    #include <zephyr/kernel.h>
    #include <zephyr/irq.h>
#endif

#if LV_USE_OS == LV_OS_RTTHREAD
    #include "rtthread.h"
#endif

#if LV_USE_NUTTX
    #include <nuttx/irq.h>
    #include <nuttx/arch.h>
#ifndef NX_IRQ
    #define NX_IRQ(irqn) ((irqn) + 16)
#endif
#endif

/*********************
 *      DEFINES
 *********************/

#ifndef LV_SIFLI_EPIC_WAIT_TIMEOUT_MS
    #define LV_SIFLI_EPIC_WAIT_TIMEOUT_MS 500
#endif

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void _epic_interrupt_init(void);
static void _epic_interrupt_deinit(void);
static void _epic_run(void);
static void _epic_wait(void);
static void _epic_irq_enter(void);
static void _epic_irq_leave(void);

#if LV_USE_OS
    static void _epic_gpu_lock(void);
    static void _epic_gpu_unlock(void);
    static void _epic_wait_hw_done(void);
#endif

#if defined(__ZEPHYR__)
    static void _epic_zephyr_irq_handler(const void * arg);
    #ifdef HAL_EZIP_MODULE_ENABLED
        static void _ezip_zephyr_irq_handler(const void * arg);
    #endif
#endif

#if LV_USE_NUTTX
    static int _nuttx_epic_isr(int irq, FAR void * context, FAR void * arg);
    #ifdef HAL_EZIP_MODULE_ENABLED
        static int _nuttx_ezip_isr(int irq, FAR void * context, FAR void * arg);
    #endif
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

#if LV_USE_OS
    static lv_thread_sync_t epic_sync;
    static lv_mutex_t epic_gpu_mutex;
    static bool epic_gpu_mutex_inited;
#endif
static volatile bool epic_idle = true;
#if LV_USE_NUTTX
static volatile bool epic_hw_done = false;
#endif

static epic_osa_cfg_t _epic_default_cfg = {
    .epic_interrupt_init = _epic_interrupt_init,
    .epic_interrupt_deinit = _epic_interrupt_deinit,
    .epic_run = _epic_run,
    .epic_wait = _epic_wait,
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

#if defined(__ZEPHYR__)
static void _epic_zephyr_irq_handler(const void * arg)
{
    LV_UNUSED(arg);
    EPIC_IRQHandler();
}

#ifdef HAL_EZIP_MODULE_ENABLED
static void _ezip_zephyr_irq_handler(const void * arg)
{
    LV_UNUSED(arg);
    EZIP_IRQHandler();
}
#endif
#endif

void EPIC_IRQHandler(void)
{
    _epic_irq_enter();

    if(lv_epic_is_initialized()) {
        EPIC_HandleTypeDef * epic_handle = lv_epic_get_handle();
        if(epic_handle != NULL) {
            HAL_EPIC_IRQHandler(epic_handle);
        }
    }

    _epic_irq_leave();
}

#ifdef HAL_EZIP_MODULE_ENABLED
void EZIP_IRQHandler(void)
{
    _epic_irq_enter();

    if(lv_epic_is_initialized()) {
        EZIP_HandleTypeDef * ezip_handle = lv_ezip_get_handle();
        if(ezip_handle != NULL) {
            HAL_EZIP_IRQHandler(ezip_handle);
        }
    }

    _epic_irq_leave();
}
#endif

#if LV_USE_NUTTX
static int _nuttx_epic_isr(int irq, FAR void * context, FAR void * arg)
{
    LV_UNUSED(irq);
    LV_UNUSED(context);
    LV_UNUSED(arg);
    EPIC_IRQHandler();
    return 0;
}

#ifdef HAL_EZIP_MODULE_ENABLED
static int _nuttx_ezip_isr(int irq, FAR void * context, FAR void * arg)
{
    LV_UNUSED(irq);
    LV_UNUSED(context);
    LV_UNUSED(arg);
    EZIP_IRQHandler();
    return 0;
}
#endif
#endif

epic_osa_cfg_t * epic_get_default_cfg(void)
{
    return &_epic_default_cfg;
}

lv_result_t lv_epic_osa_init(void)
{
#if LV_USE_OS
    if(lv_mutex_init(&epic_gpu_mutex) != LV_RESULT_OK) {
        return LV_RESULT_INVALID;
    }
    epic_gpu_mutex_inited = true;

    if(lv_epic_osa_thread_sync_init() != LV_RESULT_OK) {
        (void)lv_mutex_delete(&epic_gpu_mutex);
        epic_gpu_mutex_inited = false;
        return LV_RESULT_INVALID;
    }
#endif
    _epic_interrupt_init();
    return LV_RESULT_OK;
}

void lv_epic_osa_deinit(void)
{
    _epic_interrupt_deinit();
#if LV_USE_OS
    lv_epic_osa_thread_sync_delete();
    if(epic_gpu_mutex_inited) {
        (void)lv_mutex_delete(&epic_gpu_mutex);
        epic_gpu_mutex_inited = false;
    }
#endif
}

lv_result_t lv_epic_osa_thread_sync_init(void)
{
#if LV_USE_OS
    if(lv_thread_sync_init(&epic_sync) != LV_RESULT_OK) {
        return LV_RESULT_INVALID;
    }
#endif
    return LV_RESULT_OK;
}

lv_result_t lv_epic_osa_thread_sync_wait(void)
{
#if LV_USE_OS
    if(lv_thread_sync_wait(&epic_sync) != LV_RESULT_OK) {
        return LV_RESULT_INVALID;
    }
#endif
    return LV_RESULT_OK;
}

void lv_epic_osa_thread_sync_signal_isr(void)
{
    epic_idle = true;
#if LV_USE_NUTTX
    epic_hw_done = true;
#endif
#if LV_USE_OS && !LV_USE_NUTTX
    (void)lv_thread_sync_signal_isr(&epic_sync);
#endif
}

void lv_epic_osa_set_idle(void)
{
    epic_idle = true;
}

void lv_epic_osa_thread_sync_delete(void)
{
#if LV_USE_OS
    (void)lv_thread_sync_delete(&epic_sync);
#endif
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#if LV_USE_OS
static void _epic_gpu_lock(void)
{
    if(epic_gpu_mutex_inited) {
        (void)lv_mutex_lock(&epic_gpu_mutex);
    }
}

static void _epic_gpu_unlock(void)
{
    if(epic_gpu_mutex_inited) {
        (void)lv_mutex_unlock(&epic_gpu_mutex);
    }
}

/* Poll HAL until async EPIC/EZIP job completes (SiFli EPIC_WaitDone / drv_gpu_check_done). */
static void _epic_wait_hw_done(void)
{
    uint32_t start = lv_tick_get();

    while(lv_epic_is_hardware_active()
#if LV_USE_NUTTX
          && !epic_hw_done
#endif
         ) {
        lv_epic_poll_pending_completion();
        if(!lv_epic_is_hardware_active()
#if LV_USE_NUTTX
           || epic_hw_done
#endif
          ) {
            break;
        }
        if(lv_tick_elaps(start) > LV_SIFLI_EPIC_WAIT_TIMEOUT_MS) {
            LV_LOG_WARN("EPIC wait timeout (%d ms), resetting GPU", LV_SIFLI_EPIC_WAIT_TIMEOUT_MS);
            lv_epic_gpu_reset();
            break;
        }
        lv_delay_ms(1);
    }

    epic_idle = true;
#if LV_USE_NUTTX
    epic_hw_done = false;
#endif
}
#endif

static void _epic_irq_enter(void)
{
#if LV_USE_OS == LV_OS_RTTHREAD
    rt_interrupt_enter();
#endif
}

static void _epic_irq_leave(void)
{
#if LV_USE_OS == LV_OS_RTTHREAD
    rt_interrupt_leave();
#endif
}

static void _epic_interrupt_init(void)
{
#if defined(__ZEPHYR__)
    IRQ_CONNECT(LV_SIFLI_EPIC_IRQn, LV_SIFLI_EPIC_IRQ_PRIORITY, _epic_zephyr_irq_handler, NULL,
                LV_SIFLI_EPIC_ZEPHYR_IRQ_FLAGS);
    irq_enable(LV_SIFLI_EPIC_IRQn);
#elif LV_USE_OS == LV_OS_FREERTOS && defined(LV_SIFLI_EPIC_FREERTOS_IRQ_PRIORITY)
    HAL_NVIC_SetPriority(LV_SIFLI_EPIC_IRQn, LV_SIFLI_EPIC_FREERTOS_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(LV_SIFLI_EPIC_IRQn);
#elif LV_USE_NUTTX
    if(irq_attach(NX_IRQ(LV_SIFLI_EPIC_IRQn), _nuttx_epic_isr, NULL) == 0) {
        HAL_NVIC_SetPriority(LV_SIFLI_EPIC_IRQn, LV_SIFLI_EPIC_IRQ_PRIORITY, 0);
        up_enable_irq(NX_IRQ(LV_SIFLI_EPIC_IRQn));
    }
    else {
        LV_LOG_WARN("EPIC irq_attach failed");
    }
#else
    HAL_NVIC_SetPriority(LV_SIFLI_EPIC_IRQn, LV_SIFLI_EPIC_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(LV_SIFLI_EPIC_IRQn);
#endif

#ifdef HAL_EZIP_MODULE_ENABLED
#if defined(__ZEPHYR__)
    IRQ_CONNECT(LV_SIFLI_EZIP_IRQn, LV_SIFLI_EPIC_IRQ_PRIORITY, _ezip_zephyr_irq_handler, NULL,
                LV_SIFLI_EPIC_ZEPHYR_IRQ_FLAGS);
    irq_enable(LV_SIFLI_EZIP_IRQn);
#elif LV_USE_OS == LV_OS_FREERTOS && defined(LV_SIFLI_EPIC_FREERTOS_IRQ_PRIORITY)
    HAL_NVIC_SetPriority(LV_SIFLI_EZIP_IRQn, LV_SIFLI_EPIC_FREERTOS_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(LV_SIFLI_EZIP_IRQn);
#elif LV_USE_NUTTX
    if(irq_attach(NX_IRQ(LV_SIFLI_EZIP_IRQn), _nuttx_ezip_isr, NULL) == 0) {
        HAL_NVIC_SetPriority(LV_SIFLI_EZIP_IRQn, LV_SIFLI_EPIC_IRQ_PRIORITY, 0);
        up_enable_irq(NX_IRQ(LV_SIFLI_EZIP_IRQn));
    }
    else {
        LV_LOG_WARN("EZIP irq_attach failed");
    }
#else
    HAL_NVIC_SetPriority(LV_SIFLI_EZIP_IRQn, LV_SIFLI_EPIC_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(LV_SIFLI_EZIP_IRQn);
#endif
#endif

    epic_idle = true;
}

static void _epic_interrupt_deinit(void)
{
#if defined(__ZEPHYR__)
    irq_disable(LV_SIFLI_EPIC_IRQn);
#elif LV_USE_NUTTX
    up_disable_irq(NX_IRQ(LV_SIFLI_EPIC_IRQn));
    irq_detach(NX_IRQ(LV_SIFLI_EPIC_IRQn));
#else
    HAL_NVIC_DisableIRQ(LV_SIFLI_EPIC_IRQn);
#endif

#ifdef HAL_EZIP_MODULE_ENABLED
#if defined(__ZEPHYR__)
    irq_disable(LV_SIFLI_EZIP_IRQn);
#elif LV_USE_NUTTX
    up_disable_irq(NX_IRQ(LV_SIFLI_EZIP_IRQn));
    irq_detach(NX_IRQ(LV_SIFLI_EZIP_IRQn));
#else
    HAL_NVIC_DisableIRQ(LV_SIFLI_EZIP_IRQn);
#endif
#endif

    epic_idle = true;
}

static void _epic_run(void)
{
#if LV_USE_OS
    _epic_gpu_lock();
#endif

#if LV_USE_NUTTX
    /* Sync HAL on NuttX: no pre-flight async wait. */
#elif LV_USE_OS
    (void)lv_thread_sync_delete(&epic_sync);
    (void)lv_thread_sync_init(&epic_sync);
#else
    if(lv_epic_is_hardware_active()) {
        _epic_wait_hw_done();
    }
#endif

    epic_idle = false;

#if LV_USE_OS
    _epic_gpu_unlock();
#endif
}

static void _epic_wait(void)
{
#if LV_USE_OS
    _epic_gpu_lock();
#endif

#if LV_USE_NUTTX
    epic_idle = true;
#elif LV_USE_OS
    if(epic_idle && !lv_epic_is_hardware_active()) {
        _epic_gpu_unlock();
        return;
    }

    if(lv_epic_osa_thread_sync_wait() == LV_RESULT_OK) {
        epic_idle = true;
    }
#else
    if(epic_idle && !lv_epic_is_hardware_active()) {
#if LV_USE_OS
        _epic_gpu_unlock();
#endif
        return;
    }

    _epic_wait_hw_done();
#endif

#if LV_USE_OS
    _epic_gpu_unlock();
#endif
}

#endif /*LV_USE_SIFLI_EPIC */
