/**
 * @file boot_hw.c
 * @brief Piezo mute, power hold, ETA9184 GPIO, VBATS ADC SOC, WDT, DWT tick.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_hw.h"
#include "boot_flash.h"
#include "boot_lfs.h"
#include "board.h"
#include "bf0_hal.h"
#include "bf0_hal_adc.h"
#include "bf0_hal_efuse.h"
#include "bf0_hal_wdt.h"
#include "bf0_sys_cfg.h"
#include "register.h"
#include "myvendor_bat_soc.h"

#include <string.h>

#define BOOT_PIN_DISCHRG     24u  /* PA24 DISCHRG: low = discharging */
#define BOOT_PIN_STAT        25u  /* PA25 STAT:    low = charging */
#define BOOT_PIN_PULSE       26u  /* PA26 PULSE: untrusted; not used for SOC */
#define BOOT_PIN_ENBST       28u  /* PA28 ENBST:   keep off in 2SFBL */
#define BOOT_PIN_KEY1        30u  /* PA30 KEY1 active-low */
#define BOOT_PIN_KEY2        33u  /* PA33 KEY2 active-low */
#define BOOT_PIN_PWR         34u  /* PA34 PWR_KEY_READ: high = pressed */
#define BOOT_PULSE_WIN_MS    4000u
#define BOOT_FACTORY_WIN_MS  1000u

#define BOOT_BAT_CHAN            7
#define BOOT_BAT_K_DEFAULT     2010  /* ×1000：无 eFuse 时按 3.3 V 0.5 */
#define BOOT_BAT_K_MIN         1500
#define BOOT_BAT_K_MAX         4000
#define BOOT_EFUSE_ATE_OFF      256u
#define BOOT_BAT_SAMPLE_N      6
#define BOOT_BAT_SETTLE_US     (300u * 1000u)
#define BOOT_BAT_CACHE_MS      500u
#define BOOT_BAT_FULL_DEFAULT  4100
#define BOOT_BAT_FULL_MARGIN    20
#define BOOT_BAT_FULL_DEADBAND 50
#define BOOT_BAT_OBS_MIN       4050
#define BOOT_BAT_OBS_MAX       4650
#define BOOT_BAT_FULL_OK_MIN   4030
#define BOOT_BAT_FULL_OK_MAX   4630
#define BOOT_BAT_KV_KEY        "persist.battery.full_mv"
#define BOOT_BAT_KV_PATH       "/" BOOT_BAT_KV_KEY /* boot_lfs remaps to /db/ */

static int g_eta_pins;
static int g_pulse_prev = -1;
static unsigned g_pulse_rise;
static uint32_t g_pulse_win0;
static int g_key1_prev = 1;
static int g_key2_prev = 1;
static int g_unplug_n;
static int g_factory_seen;
static int g_factory_n;

static ADC_HandleTypeDef g_adc;
static int g_adc_ok;
static int32_t g_adc_ratio = 1000;
static int32_t g_adc_offset;
static int32_t g_vbat_k = BOOT_BAT_K_DEFAULT;
static int g_bat_mv = -1;
static int g_bat_pct = -1;
static uint32_t g_bat_ms;
static int g_bat_cache_ok;
static int g_full_mv = BOOT_BAT_FULL_DEFAULT;
static int g_full_have;
static int g_was_charging;
static int g_full_edge;

static int boot_pin_low(uint16_t pin);

void boot_hw_buzzer_off(void)
{
    HAL_PIN_Set(PAD_PA40, GPIO_A40, PIN_NOPULL, 1);
    BSP_GPIO_Set(40, 0, 1);
}

void boot_hw_power_hold(void)
{
    /* PWR_KEY_CTL: high keeps the latch; NuttX poweroff drives it low. */
    HAL_PIN_Set(PAD_PA29, GPIO_A29, PIN_PULLUP, 1);
    BSP_GPIO_Set(29, 1, 1);
    /* TPS63802 MODE / PWR_MD: high = FPWM, same as NuttX pinmux. */
    HAL_PIN_Set(PAD_PA27, GPIO_A27, PIN_PULLUP, 1);
    BSP_GPIO_Set(27, 1, 1);
}

void boot_hw_power_release(void)
{
    /* Drop the latch so USB/VIN is the only rail. Unplug is then a
     * hardware power-off; do not keep PA29 high while showing charge. */
    HAL_PIN_Set(PAD_PA29, GPIO_A29, PIN_NOPULL, 1);
    BSP_GPIO_Set(29, 0, 1);
    HAL_PIN_Set(PAD_PA27, GPIO_A27, PIN_PULLUP, 1);
    BSP_GPIO_Set(27, 1, 1);
}

void boot_hw_power_apply(void)
{
    boot_hw_eta_pins();
    if ((boot_hw_charging() || boot_hw_charge_full()) && !boot_hw_pwr_key())
    {
        boot_hw_power_release();
    }
    else
    {
        boot_hw_power_hold();
    }
}

void boot_hw_power_off(void)
{
    BSP_GPIO_Set(29, 0, 1);
    for (;;)
    {
        boot_hw_wdt_pet();
        HAL_Delay_us(100000);
    }
}

/* RC10K ~9 kHz when SEL_LPCLK=0; SF32LB52 HAL_Init switches to RC32K. */
static uint32_t boot_wdt_clk_hz(void)
{
    if ((hwp_pmuc->CR & PMUC_CR_SEL_LPCLK) != 0u)
    {
        return 32768u;
    }
    return 9000u;
}

static uint32_t boot_wdt_ticks(uint32_t ms)
{
    uint32_t t = (ms * boot_wdt_clk_hz()) / 1000u;

    if (t == 0u || (t & 0xff000000u) != 0u)
    {
        t = 0x00ffffffu;
    }
    return t;
}

#define BOOT_WDT_BOOT_MS      5000u
#define BOOT_WDT_HANDOFF_MS   15000u
#define BOOT_WDT_RELOAD2      100u

static void boot_wdt_stop_one(WDT_TypeDef *wdt)
{
    unsigned n;

    wdt->WDT_WP = WDT_RELEASE_MAGIC;
    wdt->WDT_CCR = WDT_CMD_STOP;
    for (n = 0; n < 8000u; n++)
    {
        if ((wdt->WDT_SR & WDT_WDT_SR_WDT_ACTIVE) == 0)
        {
            break;
        }
    }
}

static void boot_wdt_apply_one(WDT_TypeDef *wdt, uint32_t reload)
{
    unsigned n;

    /* HAL_WDT_Init stops first: CVR writes while ACTIVE are ignored, so a
     * leftover Mask ROM countdown keeps running through LCD init. */
    wdt->WDT_WP = WDT_RELEASE_MAGIC;
    wdt->WDT_CCR = WDT_CMD_STOP;
    for (n = 0; n < 8000u; n++)
    {
        if ((wdt->WDT_SR & WDT_WDT_SR_WDT_ACTIVE) == 0)
        {
            break;
        }
    }
    wdt->WDT_WP = WDT_RELEASE_MAGIC;
    wdt->WDT_CVR0 = reload;
    wdt->WDT_CVR1 = BOOT_WDT_RELOAD2;
    wdt->WDT_WP = WDT_RELEASE_MAGIC;
    wdt->WDT_CCR = WDT_CMD_START;
}

static void boot_wdt_apply_all(uint32_t ms)
{
    uint32_t reload = boot_wdt_ticks(ms);

    boot_wdt_apply_one(hwp_wdt1, reload);
    boot_wdt_apply_one(hwp_wdt2, reload);
    boot_wdt_apply_one(hwp_iwdt, reload);
}

void boot_hw_wdt_pet(void)
{
    static uint32_t last;
    static int armed;
    uint32_t now = HAL_GetTick();

    /* Do not touch SGL or ETA GPIOs here: hw_preinit0 calls this before BSS
     * is cleared. boot_sgl_tick used to be a no-op for that reason. */

    if (armed && (now - last) < 200u)
    {
        return;
    }
    armed = 1;
    last = now;
    boot_wdt_apply_all(BOOT_WDT_BOOT_MS);
}

void boot_hw_wdt_handoff(void)
{
    /* NuttX only feeds IWDT. WDT1/WDT2 would still fire after LPCLK→RC32K. */
    boot_wdt_stop_one(hwp_wdt1);
    boot_wdt_stop_one(hwp_wdt2);
    boot_wdt_apply_one(hwp_iwdt, boot_wdt_ticks(BOOT_WDT_HANDOFF_MS));
}

static void boot_gpio_in(uint16_t pin, uint32_t pad, pin_function func)
{
    GPIO_InitTypeDef gpio;

    HAL_PIN_Set(pad, func, PIN_PULLUP, 1);
    gpio.Pin = pin;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(hwp_gpio1, &gpio);
}

static int boot_pin_low(uint16_t pin)
{
    return HAL_GPIO_ReadPin(hwp_gpio1, pin) == GPIO_PIN_RESET;
}

static int boot_pin_high(uint16_t pin)
{
    return HAL_GPIO_ReadPin(hwp_gpio1, pin) == GPIO_PIN_SET;
}

void boot_hw_eta_pins(void)
{
    if (g_eta_pins)
    {
        return;
    }
    g_eta_pins = 1;
    boot_gpio_in(BOOT_PIN_DISCHRG, PAD_PA24, GPIO_A24);
    boot_gpio_in(BOOT_PIN_STAT, PAD_PA25, GPIO_A25);
    boot_gpio_in(BOOT_PIN_PULSE, PAD_PA26, GPIO_A26);
    boot_gpio_in(BOOT_PIN_KEY1, PAD_PA30, GPIO_A30);
    boot_gpio_in(BOOT_PIN_KEY2, PAD_PA33, GPIO_A33);
    HAL_PIN_Set(PAD_PA34, GPIO_A34, PIN_PULLDOWN, 1);
    {
        GPIO_InitTypeDef gpio;

        gpio.Pin = BOOT_PIN_PWR;
        gpio.Mode = GPIO_MODE_INPUT;
        gpio.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(hwp_gpio1, &gpio);
    }
    /* Boost stays off; product percent uses VBATS, not ENBST. */
    HAL_PIN_Set(PAD_PA28, GPIO_A28, PIN_PULLDOWN, 1);
    BSP_GPIO_Set(BOOT_PIN_ENBST, 0, 1);
    HAL_Delay_us(200);
}

int boot_hw_charging(void)
{
    boot_hw_eta_pins();
    return boot_pin_low(BOOT_PIN_STAT);
}

int boot_hw_unplugged(void)
{
    boot_hw_eta_pins();
    /* STAT high and DISCHRG low: cable gone. Full-on-cable is STAT high
     * with DISCHRG still high. */
    if (!boot_pin_low(BOOT_PIN_STAT) && boot_pin_low(BOOT_PIN_DISCHRG))
    {
        if (g_unplug_n < 12)
        {
            g_unplug_n++;
        }
        return g_unplug_n >= 12;
    }
    g_unplug_n = 0;
    return 0;
}

int boot_hw_charge_full(void)
{
    boot_hw_eta_pins();
    /* STAT high + DISCHRG high: still on the cable, not charging.
     * PULSE is untrusted and is not part of this. */
    return !boot_pin_low(BOOT_PIN_STAT) &&
           !boot_pin_low(BOOT_PIN_DISCHRG);
}

static int boot_bat_full_ok(int v)
{
    return v >= BOOT_BAT_FULL_OK_MIN && v <= BOOT_BAT_FULL_OK_MAX;
}

static int boot_parse_u32(const char *s, int n, int *out)
{
    int i;
    int v = 0;
    int any = 0;

    if (s == NULL || out == NULL || n <= 0)
    {
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        char c = s[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (any)
            {
                break;
            }
            continue;
        }
        if (c < '0' || c > '9')
        {
            break;
        }
        v = v * 10 + (c - '0');
        any = 1;
    }
    if (!any)
    {
        return -1;
    }
    *out = v;
    return 0;
}

static int boot_fmt_u32_nl(int v, char *buf, unsigned cap)
{
    char tmp[12];
    unsigned n = 0;
    unsigned i;

    if (v < 0 || buf == NULL || cap < 3u)
    {
        return -1;
    }
    do
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (v > 0 && n < sizeof(tmp));
    if (n + 2u > cap)
    {
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        buf[i] = tmp[n - 1u - i];
    }
    buf[n++] = '\n';
    buf[n] = 0;
    return (int)n;
}

static void boot_bat_kv_load(void)
{
    lfs_file_t file;
    char buf[32];
    lfs_ssize_t n;
    int v;

    g_full_mv = BOOT_BAT_FULL_DEFAULT;
    g_full_have = 0;
    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return;
        }
    }
    if (boot_lfs_file_open_ro(BOOT_BAT_KV_PATH, &file) != 0)
    {
        return;
    }
    n = boot_lfs_file_read(&file, buf, sizeof(buf) - 1u);
    (void)boot_lfs_file_close(&file);
    if (n <= 0)
    {
        return;
    }
    buf[n] = 0;
    if (boot_parse_u32(buf, (int)n, &v) != 0 || !boot_bat_full_ok(v))
    {
        return;
    }
    g_full_mv = v;
    g_full_have = 1;
}

static void boot_bat_kv_store(int cal)
{
    char buf[16];
    int n;

    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return;
        }
    }
    n = boot_fmt_u32_nl(cal, buf, sizeof(buf));
    if (n <= 0)
    {
        return;
    }
    (void)boot_lfs_put(BOOT_BAT_KV_PATH, buf, (lfs_size_t)n);
}

static void boot_bat_note(int mv)
{
    int cal;
    int delta;

    if (mv < BOOT_BAT_OBS_MIN || mv > BOOT_BAT_OBS_MAX)
    {
        return;
    }
    cal = mv - BOOT_BAT_FULL_MARGIN;
    if (!boot_bat_full_ok(cal))
    {
        return;
    }
    if (!g_full_have)
    {
        boot_bat_kv_load();
    }
    if (g_full_have)
    {
        delta = cal - g_full_mv;
        if (delta < 0)
        {
            delta = -delta;
        }
        if (delta < BOOT_BAT_FULL_DEADBAND)
        {
            return;
        }
    }
    g_full_mv = cal;
    g_full_have = 1;
    boot_bat_kv_store(cal);
}

static void boot_bat_track_full(void)
{
    int charging = boot_hw_charging();
    int full = boot_hw_charge_full();

    if (charging)
    {
        g_was_charging = 1;
        g_full_edge = 0;
    }
    else if (full && g_was_charging)
    {
        g_full_edge = 1;
        g_was_charging = 0;
    }
    else if (!full)
    {
        g_full_edge = 0;
        g_was_charging = 0;
    }
}

static int boot_adc_pin_mv_from_raw(uint32_t raw)
{
    int32_t pin_mv;

    pin_mv = ((int32_t)raw - g_adc_offset) * g_adc_ratio / 1000;
    if (pin_mv < 0)
    {
        pin_mv = 0;
    }
    return (int)pin_mv;
}

static void boot_adc_apply_two_point(uint16_t vol10, uint16_t vol25,
                                     uint16_t low_mv, uint16_t high_mv)
{
    int32_t gap1;
    int32_t gap2;

    vol10 &= 0x7fff;
    vol25 &= 0x7fff;
    gap1 = (int32_t)vol10 - (int32_t)vol25;
    if (gap1 < 0)
    {
        gap1 = -gap1;
    }
    gap2 = (int32_t)low_mv - (int32_t)high_mv;
    if (gap2 < 0)
    {
        gap2 = -gap2;
    }
    if (gap1 < 1)
    {
        return;
    }
    g_adc_ratio = gap2 * 1000 / gap1;
    if (g_adc_ratio < 1)
    {
        g_adc_ratio = 1000;
    }
    g_adc_offset = (int32_t)vol10 - ((int32_t)low_mv * 1000 / g_adc_ratio);
}

static void boot_adc_apply_vbat_k(uint16_t vbat_reg, uint16_t vbat_mv)
{
    int sample;
    int32_t k;

    g_vbat_k = BOOT_BAT_K_DEFAULT;
    if (vbat_reg == 0 || vbat_mv == 0)
    {
        return;
    }
    sample = boot_adc_pin_mv_from_raw(vbat_reg);
    if (sample < 200)
    {
        return;
    }
    k = (int32_t)vbat_mv * 1000 / sample;
    if (k < BOOT_BAT_K_MIN || k > BOOT_BAT_K_MAX)
    {
        return;
    }
    g_vbat_k = k;
}

static int boot_adc_cfg_from_efuse(FACTORY_CFG_ADC_T *cfg)
{
    uint8_t data[CFG_SYS_SIZE];

    memset(cfg, 0, sizeof(*cfg));
    if (HAL_EFUSE_Init() != HAL_OK)
    {
        return -1;
    }
    if (HAL_EFUSE_Read(BOOT_EFUSE_ATE_OFF, data, CFG_SYS_SIZE) !=
        (int32_t)CFG_SYS_SIZE)
    {
        return -1;
    }
    if (data[0] == 0)
    {
        return -1;
    }

    /* 3.3 V AVDD 布局（与 NuttX avdd_v18_en=0 一致）。 */
    cfg->vol10 = (uint16_t)data[4] | ((uint16_t)(data[5] & 0xf) << 8);
    cfg->low_mv = (uint16_t)(((data[5] & 0xf0) >> 4) | ((data[6] & 1) << 4));
    cfg->vol25 = (uint16_t)(((data[6] & 0xfe) >> 1) |
                            ((uint16_t)(data[7] & 0x1f) << 7));
    cfg->high_mv = (uint16_t)(((data[7] & 0xe0) >> 5) | ((data[8] & 0x3) << 3));
    cfg->vbat_reg = (uint16_t)(((data[8] & 0xfc) >> 2) |
                               ((uint16_t)(data[9] & 0x3f) << 6));
    cfg->vbat_mv = (uint16_t)(((data[9] & 0xc0) >> 6) | ((data[10] & 0xf) << 2));
    cfg->low_mv = (uint16_t)(cfg->low_mv * 100u);
    cfg->high_mv = (uint16_t)(cfg->high_mv * 100u);
    cfg->vbat_mv = (uint16_t)(cfg->vbat_mv * 100u);
    return 0;
}

static void boot_adc_calib(void)
{
    FACTORY_CFG_ADC_T cfg;
    int two_ok;

    memset(&cfg, 0, sizeof(cfg));
    g_adc_ratio = 1000;
    g_adc_offset = 0;
    g_vbat_k = BOOT_BAT_K_DEFAULT;

    if (BSP_CONFIG_get(FACTORY_CFG_ID_ADC, (uint8_t *)&cfg,
                       (int)sizeof(cfg)) <= 0)
    {
        (void)boot_adc_cfg_from_efuse(&cfg);
    }

    two_ok = (cfg.vol10 != 0 && cfg.vol25 != 0 &&
              cfg.low_mv != 0 && cfg.high_mv != 0);
    if (!two_ok)
    {
        cfg.vol10 = 1758;
        cfg.vol25 = 3162;
        cfg.low_mv = 1000;
        cfg.high_mv = 2500;
    }

    boot_adc_apply_two_point(cfg.vol10, cfg.vol25, cfg.low_mv, cfg.high_mv);
    boot_adc_apply_vbat_k(cfg.vbat_reg, cfg.vbat_mv);

#if defined(SF32LB52X)
    if (SF32LB52X_LETTER_SERIES() && cfg.ldovref_flag)
    {
        __HAL_ADC_SET_LDO_REF_SEL(&g_adc, cfg.ldovref_sel);
    }
#endif
}

static int boot_adc_convert_raw(uint32_t *out)
{
    ADC_ChannelConfTypeDef chan;
    uint32_t data[BOOT_BAT_SAMPLE_N];
    uint32_t total = 0;
    uint32_t used;
    int i;
    int j;

    memset(&chan, 0, sizeof(chan));
    chan.pchnl_sel = BOOT_BAT_CHAN;
    chan.slot_en = 1;
    chan.nchnl_sel = 0;
    chan.Channel = BOOT_BAT_CHAN;
    chan.acc_num = 0;
    HAL_ADC_ConfigChannel(&g_adc, &chan);
    hwp_hpsys_cfg->ANAU_CR |= (HPSYS_CFG_ANAU_CR_EN_BG |
                               HPSYS_CFG_ANAU_CR_EN_VBAT_MON);
    HAL_ADC_Start(&g_adc);

    for (i = 0; i < BOOT_BAT_SAMPLE_N; i++)
    {
        if (i != 0)
        {
            ADC_SET_UNMUTE(&g_adc);
            HAL_Delay_us(200);
            __HAL_ADC_START_CONV(&g_adc);
        }

        if (HAL_ADC_PollForConversion(&g_adc, 100) != HAL_OK)
        {
            HAL_ADC_Stop(&g_adc);
            return -1;
        }

        data[i] = HAL_ADC_GetValue(&g_adc, 0);
        ADC_SET_MUTE(&g_adc);
        total += data[i];
        if (i + 1 < BOOT_BAT_SAMPLE_N)
        {
            HAL_Delay_us(1000);
        }
    }
    HAL_ADC_Stop(&g_adc);

    for (i = 0; i < BOOT_BAT_SAMPLE_N - 1; i++)
    {
        for (j = 0; j < BOOT_BAT_SAMPLE_N - 1 - i; j++)
        {
            if (data[j] > data[j + 1])
            {
                uint32_t tmp = data[j];

                data[j] = data[j + 1];
                data[j + 1] = tmp;
            }
        }
    }
    total -= data[0];
    total -= data[BOOT_BAT_SAMPLE_N - 1];
    used = (uint32_t)(BOOT_BAT_SAMPLE_N - 2);
    *out = total / used;
    return 0;
}

static int boot_adc_read_mv(void)
{
    uint32_t raw;
    int32_t pin_mv;

    if (!g_adc_ok)
    {
        return -1;
    }
    if (boot_adc_convert_raw(&raw) != 0)
    {
        return -1;
    }
    pin_mv = boot_adc_pin_mv_from_raw(raw);
    return (int)(((int32_t)pin_mv * g_vbat_k + 500) / 1000);
}

static int boot_bat_pct_from_mv(int mv, int charging, int full_io)
{
    int full = g_full_mv;
    int pct;

    if (mv < 0)
    {
        return -1;
    }
    if (full <= MYVENDOR_BAT_SOC_EMPTY_MV)
    {
        full = BOOT_BAT_FULL_DEFAULT;
    }
    pct = myvendor_bat_soc_pct(mv, full, charging, full_io);
    if (charging && g_bat_pct >= 0 && pct >= 0 && pct < g_bat_pct)
    {
        return g_bat_pct;
    }
    return pct;
}

void boot_hw_bat_init(void)
{
    if (g_adc_ok)
    {
        boot_bat_kv_load();
        return;
    }

    memset(&g_adc, 0, sizeof(g_adc));
    HAL_RCC_EnableModule(RCC_MOD_GPADC);
    g_adc.Instance = hwp_gpadc1;
    g_adc.Init.atten3 = 0;
    g_adc.Init.adc_se = 1;
    g_adc.Init.adc_force_on = 0;
    g_adc.Init.dma_en = 0;
    g_adc.Init.op_mode = 0;
    g_adc.Init.en_slot = 0;
    g_adc.Init.data_samp_delay = 2;
    g_adc.Init.conv_width = 75;
    g_adc.Init.sample_width = 71;
    g_adc.Init.avdd_v18_en = 0;
    if (HAL_ADC_Init(&g_adc) != HAL_OK)
    {
        return;
    }
    (void)HAL_ADC_SetFreq(&g_adc, 240000);
    boot_adc_calib();
    boot_hw_wdt_pet();
    HAL_Delay_us(BOOT_BAT_SETTLE_US);
    boot_hw_wdt_pet();
    g_adc_ok = 1;
    boot_bat_kv_load();
}

void boot_hw_eta_poll(void)
{
    int hi;
    uint32_t now;

    boot_hw_eta_pins();
    hi = !boot_pin_low(BOOT_PIN_PULSE);
    now = HAL_GetTick();

    if (g_pulse_prev < 0)
    {
        g_pulse_prev = hi;
        g_pulse_win0 = now;
        return;
    }
    if (hi && g_pulse_prev == 0)
    {
        g_pulse_rise++;
    }
    g_pulse_prev = hi;

    if ((now - g_pulse_win0) < BOOT_PULSE_WIN_MS)
    {
        return;
    }
    g_pulse_rise = 0;
    g_pulse_win0 = now;
}

int boot_hw_bat_mv(void)
{
    uint32_t now;

    if (!g_adc_ok)
    {
        boot_hw_bat_init();
        if (!g_adc_ok)
        {
            return -1;
        }
    }
    now = HAL_GetTick();
    if (g_bat_cache_ok && (now - g_bat_ms) < BOOT_BAT_CACHE_MS)
    {
        return g_bat_mv;
    }
    g_bat_mv = boot_adc_read_mv();
    g_bat_ms = now;
    g_bat_cache_ok = (g_bat_mv >= 0);
    return g_bat_mv;
}

int boot_hw_bat_pct(void)
{
    int mv = boot_hw_bat_mv();
    int charging;
    int full;

    boot_bat_track_full();
    charging = boot_hw_charging();
    full = boot_hw_charge_full();
    if (mv >= 0)
    {
        if (g_full_edge || full)
        {
            boot_bat_note(mv);
            g_full_edge = 0;
        }
        else if (charging && g_full_have &&
                 g_full_mv < MYVENDOR_BAT_SOC_STALE_FULL_MAX &&
                 mv >= MYVENDOR_BAT_SOC_STALE_OBS_MIN)
        {
            boot_bat_note(mv);
        }
    }
    g_bat_pct = boot_bat_pct_from_mv(mv, charging, full);
    return g_bat_pct;
}

void boot_hw_keys_arm(void)
{
    boot_hw_eta_pins();
    g_key1_prev = boot_pin_low(BOOT_PIN_KEY1);
    g_key2_prev = boot_pin_low(BOOT_PIN_KEY2);
}

int boot_hw_both_keys(void)
{
    boot_hw_eta_pins();
    return boot_pin_low(BOOT_PIN_KEY1) && boot_pin_low(BOOT_PIN_KEY2);
}

int boot_hw_boot_key(void)
{
    return boot_hw_any_key();
}

int boot_hw_pwr_key(void)
{
    boot_hw_eta_pins();
    return boot_pin_high(BOOT_PIN_PWR);
}

int boot_hw_any_key(void)
{
    boot_hw_eta_pins();
    return boot_pin_low(BOOT_PIN_KEY1) ||
           boot_pin_low(BOOT_PIN_KEY2) ||
           boot_pin_high(BOOT_PIN_PWR);
}

int boot_hw_key_edge(void)
{
    int k1;
    int k2;
    int hit = 0;

    boot_hw_eta_pins();
    k1 = boot_pin_low(BOOT_PIN_KEY1);
    k2 = boot_pin_low(BOOT_PIN_KEY2);
    if (k1 && !g_key1_prev)
    {
        hit = 1;
    }
    if (k2 && !g_key2_prev)
    {
        hit = 1;
    }
    g_key1_prev = k1;
    g_key2_prev = k2;
    return hit;
}

static int boot_combo_held(void)
{
    return boot_hw_charging() && boot_hw_both_keys();
}

int boot_hw_factory_combo(void)
{
    int i;

    boot_hw_eta_pins();

    for (i = 0; i < 3; i++)
    {
        if (!boot_combo_held())
        {
            return 0;
        }
        HAL_Delay_us(2000);
    }
    g_factory_seen = 1;
    return 1;
}

int boot_hw_factory_pending(void)
{
    return g_factory_seen;
}

void boot_hw_factory_poll(void)
{
    if (g_factory_seen || !g_eta_pins)
    {
        return;
    }
    if (HAL_GetTick() >= BOOT_FACTORY_WIN_MS)
    {
        return;
    }
    if (boot_pin_low(BOOT_PIN_STAT) && boot_pin_low(BOOT_PIN_KEY1) &&
        boot_pin_low(BOOT_PIN_KEY2))
    {
        if (g_factory_n < 3)
        {
            g_factory_n++;
        }
        if (g_factory_n >= 3)
        {
            g_factory_seen = 1;
        }
    }
    else
    {
        g_factory_n = 0;
    }
}

#define BOOT_DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define BOOT_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define BOOT_DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define BOOT_DEMCR_TRCENA      (1u << 24)
#define BOOT_DWT_CYCCNTENA     (1u << 0)

static void boot_hw_dwt_start(void)
{
    BOOT_DEMCR |= BOOT_DEMCR_TRCENA;
    BOOT_DWT_CYCCNT = 0;
    BOOT_DWT_CTRL |= BOOT_DWT_CYCCNTENA;
}

uint32_t HAL_GetTick(void)
{
    static uint32_t mhz;

    if ((BOOT_DWT_CTRL & BOOT_DWT_CYCCNTENA) == 0u)
    {
        boot_hw_dwt_start();
    }
    if (mhz == 0)
    {
        mhz = HAL_RCC_GetHCLKFreq(CORE_ID_DEFAULT) / 1000000u;
        if (mhz == 0)
        {
            mhz = 48;
        }
    }
    return BOOT_DWT_CYCCNT / (mhz * 1000u);
}
