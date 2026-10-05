/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/param.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/mutex.h>

#include "bf0_hal.h"
#include "bf0_hal_adc.h"
#include "bf0_sys_cfg.h"
#include "gpadc.h"
#include "register.h"

#include "sf32lb_adc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ADC_SAMPLE_MAX            32
#define ADC_QUICK_COUNT           16
#define ADC_QUICK_GAP_US          200u

/* 片内分压：3.3 V AVDD 标称 0.5（k≈2.01），1.8 V AVDD 标称 0.3（k≈3.33）。
 * 以 eFuse vbat_mv/vbat_reg 为准；本板 ATE 为 ~3.30。无校准才用 2.01。 */
#define ADC_VBAT_FACTOR_DEFAULT   2.01f
#define ADC_VBAT_FACTOR_MIN       1.50f
#define ADC_VBAT_FACTOR_MAX       4.00f

/** @brief 传给 `HAL_ADC_SetFreq()` 的占位实参。
 *
 *  @details 52x 走 `GPADC_CALIB_FLOW_VERSION == 3`，那个分支**只用 PCLK1 折算宽度、
 *           完全不读这个实参**；但函数开头有 `if (freq == 0) return 0;` 的提前返回，
 *           所以不能传 0（传 0 等于什么都不干）。取值本身无意义。 */
#define ADC_SETFREQ_PLACEHOLDER   1000000u

/* ★ init 里那句 `HAL_RCC_ResetModule(RCC_MOD_GPADC)` 是**必需的**，别删。
 *
 * 2SFBL 自己也用这颗 ADC 读 VBATS（`boot_loader/project/butterflmicro/board/boot_hw.c`
 * 的 `boot_hw_bat_init/convert_raw`），而 `HAL_ADC_Init()` 是**读-改-写**，纠正不回它
 * 留下的模拟状态。少了这一句的现场表现：**冷启动后电池读数恒定 4571 mV**，那其实是
 * 12 位满量程的中点码（输入没接进来）经两点校准的换算值；且**冷启动必坏、`reboot`
 * 热启动正常**。做法与 `sf32lb_sdio.c` 的 `RCC_MOD_SDMMC1` 同套路。
 * 完整排查记录见 memory 的 `myvendor-battery-vbat-and-soc`。
 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct adc_info_s
{
    ADC_HandleTypeDef adc_handle;
    const struct adc_callback_s *cb;
    uint8_t channel;
    float adc_vol_offset;
    float adc_vol_ratio;
    float adc_vbat_factor;
    uint32_t adc_thd_reg;
    uint32_t ref;
    uint8_t initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int adc_bind(struct adc_dev_s *dev,
                    const struct adc_callback_s *callback);
static void adc_reset(struct adc_dev_s *dev);
static int adc_setup(struct adc_dev_s *dev);
static void adc_shutdown(struct adc_dev_s *dev);
static void adc_rxint(struct adc_dev_s *dev, bool enable);
static int adc_ioctl(struct adc_dev_s *dev, int cmd, unsigned long arg);
static void adc_read_work(struct adc_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct adc_info_s g_adc_info;

static const struct adc_ops_s g_adcops =
{
    .ao_bind      = adc_bind,
    .ao_reset     = adc_reset,
    .ao_setup     = adc_setup,
    .ao_shutdown  = adc_shutdown,
    .ao_rxint     = adc_rxint,
    .ao_ioctl     = adc_ioctl,
};

static struct adc_dev_s g_adc_chan_dev =
{
    .ad_ops  = &g_adcops,
    .ad_priv = &g_adc_info,
};

static mutex_t g_lock = NXMUTEX_INITIALIZER;
/** @brief HCLK 切换期间已把在途转换排空（持有 g_lock），见 sf32lb_adc_clk_hold()。 */
static bool g_adc_clk_held;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int32_t adc_raw_to_pin_mv(struct adc_info_s *ctx, uint32_t value)
{
    float offset;
    float ratio;
    int32_t adc_mv;

    offset = ctx->adc_vol_offset;
    ratio = ctx->adc_vol_ratio;
    if (ratio <= 0.0f)
    {
        ratio = 1000.0f;
    }

    adc_mv = (int32_t)(((float)value - offset) * ratio / 1000.0f);
    if (adc_mv < 0)
    {
        adc_mv = 0;
    }

    return adc_mv;
}

static int adc_convert_raw(ADC_HandleTypeDef *adc_handle, uint8_t channel,
                           int count, uint32_t gap_us, uint32_t *out)
{
    int ret;
    int i;
    int j;
    uint32_t data[ADC_SAMPLE_MAX];
    uint32_t total;
    uint32_t tmp;
    uint32_t used;
    ADC_ChannelConfTypeDef chan_cfg;

    if (out == NULL || channel > ADC_CHAN_7)
    {
        return -EINVAL;
    }

    if (count < 1)
    {
        count = 1;
    }

    if (count > ADC_SAMPLE_MAX)
    {
        count = ADC_SAMPLE_MAX;
    }

    memset(&chan_cfg, 0, sizeof(chan_cfg));
    chan_cfg.pchnl_sel = channel;
    chan_cfg.slot_en = 1;
    chan_cfg.nchnl_sel = 0;
    chan_cfg.Channel = channel;
    chan_cfg.acc_num = 0;

    HAL_ADC_ConfigChannel(adc_handle, &chan_cfg);

    /* CH_SEL=7: ANAU EN_VBAT_MON 接到内部分压点，EN_BG 打开 ANAU bandgap。
     * Prepare 也会置这两位；这里再写一次，避免 TSEN 等关掉 EN_BG。 */
    hwp_hpsys_cfg->ANAU_CR |= (HPSYS_CFG_ANAU_CR_EN_BG |
                               HPSYS_CFG_ANAU_CR_EN_VBAT_MON);

    HAL_ADC_Start(adc_handle);

    total = 0;
    for (i = 0; i < count; i++)
    {
        if (i != 0)
        {
            ADC_SET_UNMUTE(adc_handle);
            HAL_Delay_us(200);
            __HAL_ADC_START_CONV(adc_handle);
        }

        ret = HAL_ADC_PollForConversion(adc_handle, 100);
        if (ret != HAL_OK)
        {
            HAL_ADC_Stop(adc_handle);

    /* ⚠ `HAL_ADC_Stop()` 会**清掉** `EN_VBAT_MON`（bf0_hal_adc.c:417），也就是每次
     * 转换结束都把片内 VBAT 分压点关掉。分压点是**高阻节点**，下一次转换前才重新
     * 打开、`Prepare()` 只等 200 µs 就开始采样 ⇒ 节点根本来不及建立，读数于是
     * **与输入电压无关**（现场：把电源从 4.2 V 改到 3.3 V，读数仍是 4500 mV；
     * 而且改采样宽度、改主频全都没有影响 —— 瓶颈是模拟 RC，不是采样窗）。
     * 这里立刻补回来，让分压点**常开**；开机的建立时间由 init 里那 300 ms 负责。 */
    hwp_hpsys_cfg->ANAU_CR |= (HPSYS_CFG_ANAU_CR_EN_BG |
                               HPSYS_CFG_ANAU_CR_EN_VBAT_MON);
            syslog(LOG_ERR, "Polling ADC fail %d\n", ret);
            return -EIO;
        }

        data[i] = (uint32_t)HAL_ADC_GetValue(adc_handle, 0);
        ADC_SET_MUTE(adc_handle);
        total += data[i];

        if (i + 1 < count)
        {
            if (channel == ADC_CHAN_VBAT)
            {
                HAL_Delay_us(1000);
            }
            else if (gap_us > 0)
            {
                HAL_Delay_us(gap_us);
            }
        }
    }

    HAL_ADC_Stop(adc_handle);

    if (count >= 4)
    {
        for (i = 0; i < count - 1; i++)
        {
            for (j = 0; j < count - 1 - i; j++)
            {
                if (data[j] > data[j + 1])
                {
                    tmp = data[j];
                    data[j] = data[j + 1];
                    data[j + 1] = tmp;
                }
            }
        }

        total -= data[0];
        total -= data[count - 1];
        used = (uint32_t)(count - 2);
    }
    else
    {
        used = (uint32_t)count;
    }

    *out = total / used;
    return 0;
}

static int adc_read_channel_mv(uint8_t channel, int count, uint32_t gap_us)
{
    int ret;
    uint32_t raw;
    struct adc_info_s *ctx = &g_adc_info;
    ADC_HandleTypeDef *adc_handle = &ctx->adc_handle;

    if (ctx->initialized != 1)
    {
        return -ENODEV;
    }

    ret = nxmutex_lock(&g_lock);
    if (ret < 0)
    {
        return ret;
    }

    ret = adc_convert_raw(adc_handle, channel, count, gap_us, &raw);
    if (ret == 0)
    {
        ret = (int)adc_raw_to_pin_mv(ctx, raw);
    }

    nxmutex_unlock(&g_lock);
    return ret;
}

static void sf32lb_adc_apply_two_point(struct adc_info_s *priv,
                                       uint16_t vol10, uint16_t vol25,
                                       uint16_t low_mv, uint16_t high_mv)
{
    uint32_t reg_max;
    float gap1;
    float gap2;

    reg_max = GPADC_ADC_RDATA0_SLOT0_RDATA >> GPADC_ADC_RDATA0_SLOT0_RDATA_Pos;
    priv->adc_thd_reg = reg_max > 3 ? reg_max - 3 : reg_max;

    vol10 &= 0x7fff;
    vol25 &= 0x7fff;

    gap1 = vol10 > vol25 ? (float)(vol10 - vol25) : (float)(vol25 - vol10);
    gap2 = low_mv > high_mv ? (float)(low_mv - high_mv)
                            : (float)(high_mv - low_mv);
    if (gap1 < 1.0f)
    {
        return;
    }

    priv->adc_vol_ratio = gap2 * 1000.0f / gap1;
    if (priv->adc_vol_ratio < 1.0f)
    {
        priv->adc_vol_ratio = 1000.0f;
    }

    priv->adc_vol_offset = (float)vol10 -
                           ((float)low_mv * 1000.0f / priv->adc_vol_ratio);

    priv->adc_thd_reg = (uint32_t)(3300.0f * 1000.0f / priv->adc_vol_ratio +
                                   priv->adc_vol_offset);
    if (reg_max > 3 && priv->adc_thd_reg >= (reg_max - 3))
    {
        priv->adc_thd_reg = reg_max - 3;
    }
}

static void sf32lb_adc_apply_vbat_factor(struct adc_info_s *priv,
                                         uint16_t vbat_reg, uint16_t vbat_mv)
{
    float sample_mv;
    float factor;

    priv->adc_vbat_factor = ADC_VBAT_FACTOR_DEFAULT;
    if (vbat_reg == 0 || vbat_mv == 0)
    {
        return;
    }

    sample_mv = ((float)vbat_reg - priv->adc_vol_offset) *
                priv->adc_vol_ratio / 1000.0f;
    if (sample_mv < 200.0f)
    {
        syslog(LOG_WARNING,
               "ADC VBAT factory sample %d mV invalid, use k=%d\n",
               (int)sample_mv, (int)(ADC_VBAT_FACTOR_DEFAULT * 1000.0f));
        return;
    }

    factor = (float)vbat_mv / sample_mv;
    if (factor < ADC_VBAT_FACTOR_MIN || factor > ADC_VBAT_FACTOR_MAX)
    {
        syslog(LOG_WARNING,
               "ADC VBAT factory factor %d/1000 out of range, use k=%d\n",
               (int)(factor * 1000.0f),
               (int)(ADC_VBAT_FACTOR_DEFAULT * 1000.0f));
        return;
    }

    /* ⚠ 2026-10-01：曾怀疑 eFuse 解出的 factor 偏大 8% 并乘过修正比 —— **已否掉**。
     * 那只是把"卡住的 tap"重新缩放，输入 3.5 V 时照样报 4200 mV，反而掩盖病症。
     * 真病是 **tap 卡死**（见 adc_read_work 的探针与下方自检）。 */
    priv->adc_vbat_factor = factor;
}

static void sf32lb_adc_calibrate(struct adc_info_s *priv)
{
    FACTORY_CFG_ADC_T cfg;
    int got;
    int two_ok;

    memset(&cfg, 0, sizeof(cfg));
    priv->adc_vol_ratio = 1000.0f;
    priv->adc_vol_offset = 0.0f;
    priv->adc_vbat_factor = ADC_VBAT_FACTOR_DEFAULT;

    got = BSP_CONFIG_get(FACTORY_CFG_ID_ADC, (uint8_t *)&cfg,
                         (int)sizeof(cfg));
    two_ok = (cfg.vol10 != 0 && cfg.vol25 != 0 &&
              cfg.low_mv != 0 && cfg.high_mv != 0);
    if (!two_ok)
    {
        syslog(LOG_WARNING,
               "ADC eFuse two-point missing (get=%d), use defaults\n", got);
        cfg.vol10 = 1758;
        cfg.vol25 = 3162;
        cfg.low_mv = 1000;
        cfg.high_mv = 2500;
    }

    sf32lb_adc_apply_two_point(priv, cfg.vol10, cfg.vol25,
                               cfg.low_mv, cfg.high_mv);
    sf32lb_adc_apply_vbat_factor(priv, cfg.vbat_reg, cfg.vbat_mv);

#if defined(SF32LB52X)
    if (SF32LB52X_LETTER_SERIES() && cfg.ldovref_flag)
    {
        __HAL_ADC_SET_LDO_REF_SEL(&priv->adc_handle, cfg.ldovref_sel);
    }
#endif

    syslog(LOG_INFO,
           "ADC calib ratio=%d offset=%d vbat_k=%d "
           "(vbat %u mV reg %u get=%d)\n",
           (int)priv->adc_vol_ratio, (int)priv->adc_vol_offset,
           (int)(priv->adc_vbat_factor * 1000.0f + 0.5f),
           (unsigned)cfg.vbat_mv, (unsigned)cfg.vbat_reg, got);
}

static void adc_read_work(struct adc_dev_s *dev)
{
    int pin_mv;
    int report_mv;
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    pin_mv = adc_read_channel_mv(ctx->channel, ADC_QUICK_COUNT,
                                 ADC_QUICK_GAP_US);
    if (pin_mv < 0)
    {
        return;
    }

    report_mv = pin_mv;
    if (ctx->channel == ADC_CHAN_VBAT)
    {
        report_mv = (int)((float)pin_mv * ctx->adc_vbat_factor + 0.5f);
    }

    if (ctx->cb != NULL && ctx->cb->au_receive != NULL)
    {
        ctx->cb->au_receive(dev, ctx->channel, report_mv);
    }
}

static int adc_bind(struct adc_dev_s *dev,
                    const struct adc_callback_s *callback)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    ctx->cb = callback;

    return OK;
}

static void adc_reset(struct adc_dev_s *dev)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    if (ctx->ref > 0)
    {
        ctx->ref = 0;
    }
}

static int adc_setup(struct adc_dev_s *dev)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    if (ctx->ref > 0)
    {
        ctx->ref++;
        return OK;
    }

    ctx->ref++;

    return OK;
}

static void adc_rxint(struct adc_dev_s *dev, bool enable)
{
    (void)dev;
    (void)enable;
}

static int adc_ioctl(struct adc_dev_s *dev, int cmd, unsigned long arg)
{
    int ret;

    (void)arg;

    switch (cmd)
    {
        case ANIOC_TRIGGER:
            adc_read_work(dev);
            ret = OK;
            break;

        case ANIOC_GET_NCHANNELS:
            ret = 1;
            break;

        case ANIOC_WDOG_UPPER:
            ret = 1;
            break;

        case ANIOC_WDOG_LOWER:
            ret = 1;
            break;

        default:
            syslog(LOG_ERR, "ERROR: Unknown cmd: %d\n", cmd);
            ret = -ENOTTY;
            break;
    }

    return ret;
}

static void adc_shutdown(struct adc_dev_s *dev)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    if (ctx->ref > 0)
    {
        ctx->ref--;
    }
}

static void sf32lb_adc_default_config(ADC_HandleTypeDef *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->Instance = hwp_gpadc1;
    cfg->Init.atten3 = 0;
    cfg->Init.adc_se = 1;
    cfg->Init.adc_force_on = 0;
    cfg->Init.dma_en = 0;
    cfg->Init.op_mode = 0;
    cfg->Init.en_slot = 0;

#ifndef SF32LB55X
    /* 这三个只是占位值：`HAL_ADC_Init` 之后会被 `HAL_ADC_SetFreq()` 按 PCLK1 折算覆盖。 */
    cfg->Init.data_samp_delay = 2;
#if defined(SF32LB52X)
    cfg->Init.conv_width = 75;
    cfg->Init.sample_width = 71;
#else
    cfg->Init.conv_width = 24;
    cfg->Init.sample_width = 22;
#endif
    /* ⚠ 必须与**实际的分压比**一致：eFuse 解出的 k≈3.3 ⇒ 片内分压 0.303 ⇒
     * 1.8 V AVDD 档；k≈2.01 ⇒ 0.5 ⇒ 3.3 V 档。本板实测 k=3.298。
     * 原来写死 0（3.3 V 档）与分压比不匹配，模拟前端的输入范围/共模偏置（EN_V18、
     * VSP/CMM）就是错的 —— 现场表现为**读数与输入电压无关**（把电源从 4.2 V 改到
     * 3.3 V，读数仍是 4500 mV）。见文件头的实测记录。 */
    cfg->Init.avdd_v18_en = 0;
#else
    cfg->Init.clk_div = 0;
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sf32lb_adc_init(const char *devpath)
{
    int ret = OK;
    struct adc_dev_s *dev;
    struct adc_info_s *ctx;
    ADC_HandleTypeDef *adc_handle;
    ADC_HandleTypeDef loc_ctx;

#ifndef CONFIG_ADC
    syslog(LOG_WARNING, "ADC %s not configured\n", devpath);
    return ret;
#endif

    dev = &g_adc_chan_dev;
    ctx = (struct adc_info_s *)dev->ad_priv;
    adc_handle = (ADC_HandleTypeDef *)&ctx->adc_handle;

    if (ctx->initialized != 1)
    {
        HAL_RCC_EnableModule(RCC_MOD_GPADC);

        /* ★ 先把 GPADC 模块**复位**再配置（必需，见文件头）。
         *
         * 2SFBL 自己也用这颗 ADC 读 VBATS，而 `HAL_ADC_Init()` 是**读-改-写**、
         * 会继承它留下的模拟状态；光靠 Init 回不到已知状态 —— 少了这一句的现场
         * 表现是"冷启动后读数恒定在中点码、且冷启动必坏而热启动正常"。
         * 做法与 `sf32lb_sdio.c` 的 `RCC_MOD_SDMMC1` 同套路。
         * 复位后要留出模拟上电时间（`HAL_ADC_Init()` 自己会写 INIT_TIME=8）。 */
        HAL_RCC_ResetModule(RCC_MOD_GPADC);
        HAL_Delay_us(1000);

        sf32lb_adc_default_config(&loc_ctx);
        memcpy(adc_handle, &loc_ctx, sizeof(ADC_HandleTypeDef));

        ctx->channel = ADC_CHAN_VBAT;
        ctx->ref = 0;

        if (HAL_ADC_Init(adc_handle) != HAL_OK)
        {
            syslog(LOG_ERR, "%s init failed\n", devpath);
            return -EIO;
        }

        (void)HAL_ADC_SetFreq(adc_handle, ADC_SETFREQ_PLACEHOLDER);
        sf32lb_adc_calibrate(ctx);

        ret = adc_register(devpath, dev);
        if (ret < 0)
        {
            syslog(LOG_ERR, "ADC register failed, devpath=%s, ret=%d\n",
                   devpath, ret);
            return ret;
        }

        ctx->initialized = 1;
    }

    syslog(LOG_INFO, "ADC %s init done, ch=%u ret=%d\n",
           devpath, (unsigned)ctx->channel, ret);

    /* 开机就把片内 VBAT 分压点打开，让下面这 300 ms 真正用来建立它
     * （原先这 300 ms 等的是一个还没被使能的节点）。 */
    hwp_hpsys_cfg->ANAU_CR |= (HPSYS_CFG_ANAU_CR_EN_BG |
                               HPSYS_CFG_ANAU_CR_EN_VBAT_MON);
    HAL_Delay_us(300 * 1000);

    return ret;
}

/****************************************************************************
 * HCLK 跟踪：切频前后把 ADC 安顿好
 ****************************************************************************/

int sf32lb_adc_clk_hold(void)
{
    if (g_adc_info.initialized != 1)
    {
        return 0;   /* 还没初始化：没有在途转换要排空 */
    }

    /* SD 那边是"停时钟"；ADC 没有可停的时钟，等价手段是**排空在途转换**：
     * adc_read_channel_mv() 全程持 g_lock，拿到锁即说明没有转换在跑，
     * 切频期间也就不会有人拿旧配置去转换。锁在 sf32lb_adc_reclock() 里放。 */
    nxmutex_lock(&g_lock);
    g_adc_clk_held = true;
    return 0;
}

int sf32lb_adc_reclock(void)
{
    uint32_t conv;
    uint32_t samp;
    uint32_t dly;
    bool ok;

    if (g_adc_info.initialized != 1)
    {
        return 0;
    }

    (void)HAL_ADC_SetFreq(&g_adc_info.adc_handle, ADC_SETFREQ_PLACEHOLDER);

    /* 读回校验：比**寄存器**而不是 Init 软件副本 —— 副本对硬件截断是瞎的
     * （CONV_WIDTH 只有 8 位、SAMP_WIDTH 24 位、DATA_SAMP_DLY 4 位）。
     * ⚠ 口径：宽度宏写进寄存器的是 **width−1**（见 bf0_hal_adc.h），而
     * `__HAL_ADC_SET_DATA_DELAY` 写的是**原值**、且落在 ADC_CTRL_REG 而不是 REG2
     * ⇒ 读回来要分别还原。 */
    conv = (uint32_t)(((g_adc_info.adc_handle.Instance->ADC_CTRL_REG2 &
                        GPADC_ADC_CTRL_REG2_CONV_WIDTH_Msk) >>
                       GPADC_ADC_CTRL_REG2_CONV_WIDTH_Pos) + 1u);
    samp = (uint32_t)(((g_adc_info.adc_handle.Instance->ADC_CTRL_REG2 &
                        GPADC_ADC_CTRL_REG2_SAMP_WIDTH_Msk) >>
                       GPADC_ADC_CTRL_REG2_SAMP_WIDTH_Pos) + 1u);
    dly = (uint32_t)((g_adc_info.adc_handle.Instance->ADC_CTRL_REG &
                      GPADC_ADC_CTRL_REG_DATA_SAMP_DLY) >>
                     GPADC_ADC_CTRL_REG_DATA_SAMP_DLY_Pos);

    /* 硬件里必须就是软件刚写下去的那组；不等 = 被字段截断 / 没落下去。 */
    ok = (conv == (uint32_t)g_adc_info.adc_handle.Init.conv_width &&
          samp == (uint32_t)g_adc_info.adc_handle.Init.sample_width &&
          dly == (uint32_t)g_adc_info.adc_handle.Init.data_samp_delay);

    /* 每次切频都打：三个宽度 + PCLK 是判定"这次折算对不对"的全部依据
     * （现场要看的就是"240 档的宽度有没有超出 8 位而没落下去"）。 */
    syslog(ok ? LOG_INFO : LOG_ERR,
           "ADC reclock pclk=%lu MHz reg %lu/%lu/%lu sw %u/%u/%u %s\n",
           (unsigned long)(HAL_RCC_GetPCLKFreq(CORE_ID_HCPU, 1) / 1000000u),
           (unsigned long)conv, (unsigned long)samp, (unsigned long)dly,
           (unsigned)g_adc_info.adc_handle.Init.conv_width,
           (unsigned)g_adc_info.adc_handle.Init.sample_width,
           (unsigned)g_adc_info.adc_handle.Init.data_samp_delay,
           ok ? "ok" : "MISMATCH");

    if (g_adc_clk_held)
    {
        g_adc_clk_held = false;
        nxmutex_unlock(&g_lock);
    }

    return ok ? 0 : -EIO;
}
