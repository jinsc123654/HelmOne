/**
 * @file bsp_pinmux.c
 * @brief 板级引脚复用：PSRAM、SDIO、UART、I2C、ETA9184、LCD 背光 PWM 等。
 *
 * BSP_PIN_Init() 在 BSP_IO_Init 中调用；NAND XIP 启动时跳过 live PSRAM pad 重配。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_board.h"

#ifdef BSP_USING_PSRAM1
/** @brief APS 128Mb PSRAM MPI1 引脚复用。 */
static void board_pinmux_psram_func0()
{
    HAL_PIN_Set(PAD_SA01, MPI1_DIO0, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA02, MPI1_DIO1, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA03, MPI1_DIO2, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA04, MPI1_DIO3, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA05, MPI1_DIO4, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA06, MPI1_DIO5, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA07, MPI1_DIO6, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA08, MPI1_DIO7, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA09, MPI1_DQSDM, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA10, MPI1_CLK,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA11, MPI1_CS,   PIN_NOPULL, 1);

    HAL_PIN_Set_Analog(PAD_SA00, 1);
    HAL_PIN_Set_Analog(PAD_SA12, 1);
}

/** @brief APS 64/32Mb 或 Winbond PSRAM MPI1 引脚复用。 */
static void board_pinmux_psram_func1_2_4(int func)
{
    HAL_PIN_Set(PAD_SA01, MPI1_DIO0, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA02, MPI1_DIO1, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA03, MPI1_DIO2, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA04, MPI1_DIO3, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA08, MPI1_DIO4, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA09, MPI1_DIO5, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA10, MPI1_DIO6, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA11, MPI1_DIO7, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA07, MPI1_CLK,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA05, MPI1_CS,   PIN_NOPULL, 1);

#ifdef FPGA
    HAL_PIN_Set(PAD_SA00, MPI1_DM, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA06, MPI1_CLKB, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_PULLDOWN, 1);
#else
    switch (func)
    {
    case 1:             // APS 64P XCELLA
        HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_PULLDOWN, 1);
        HAL_PIN_Set_Analog(PAD_SA00, 1);
        HAL_PIN_Set_Analog(PAD_SA06, 1);
        break;
    case 2:             // APS 32P LEGACY
        HAL_PIN_Set(PAD_SA00, MPI1_DM, PIN_PULLDOWN, 1);
        HAL_PIN_Set(PAD_SA12, MPI1_DQS, PIN_PULLDOWN, 1);
        HAL_PIN_Set(PAD_SA06, MPI1_CLKB, PIN_NOPULL, 1);
        break;
    case 4:             // Winbond 32/64/128p
        //HAL_PIN_Set(PAD_SA06, MPI1_CLKB, PIN_NOPULL, 1);
        HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_NOPULL, 1);
        HAL_PIN_Set_Analog(PAD_SA00, 1);
        HAL_PIN_Set_Analog(PAD_SA06, 1);
        break;
    }
#endif
}

/** @brief APS 16Mb PSRAM MPI1 引脚复用。 */
static void board_pinmux_psram_func3()
{
    HAL_PIN_Set(PAD_SA09, MPI1_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA08, MPI1_CS,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA05, MPI1_DIO0, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA07, MPI1_DIO1, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA06, MPI1_DIO2, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_SA10, MPI1_DIO3, PIN_PULLUP, 1);

    HAL_PIN_Set_Analog(PAD_SA00, 1);
    HAL_PIN_Set_Analog(PAD_SA01, 1);
    HAL_PIN_Set_Analog(PAD_SA02, 1);
    HAL_PIN_Set_Analog(PAD_SA03, 1);
    HAL_PIN_Set_Analog(PAD_SA04, 1);
    HAL_PIN_Set_Analog(PAD_SA11, 1);
    HAL_PIN_Set_Analog(PAD_SA12, 1);
}

/** @brief MPI1 全部 SA 脚设为模拟（无 PSRAM）。 */
static void board_pinmux_mpi1_none(void)
{
    uint32_t i;

    for (i = 0; i <= 12; i++)
    {
        HAL_PIN_Set_Analog(PAD_SA00 + i, 1);
    }
}
#endif

/** @brief HCPU 通用外设 pinmux（SDIO/UART/I2C/按键/ETA9184/USB 等）。 */
static void BSP_PIN_Common(void)
{
#ifdef SOC_BF0_HCPU
    // HCPU pins

    uint32_t pid = (hwp_hpsys_cfg->IDR & HPSYS_CFG_IDR_PID_Msk) >> HPSYS_CFG_IDR_PID_Pos;

    pid &= 7;

#ifdef BSP_USING_PSRAM1
#if defined(CONFIG_BSP_USING_SPI_NAND)
    /* NAND boot: the application is executing XIP from PSRAM via MPI1 right
     * now, and the secondary bootloader already configured the MPI1/PSRAM
     * pads. Re-running HAL_PIN_Set() on these live pads glitches the
     * instruction-fetch path and intermittently crashes the CPU (it only
     * survives when the next instructions happen to already be in I-cache,
     * which is why the hang was random). Leave the pads exactly as the
     * bootloader set them. */
    (void)pid;
#else
    switch (pid)
    {
    case 5: //BOOT_PSRAM_APS_16P:
        board_pinmux_psram_func3();         // 16Mb APM QSPI PSRAM
        break;
    case 4: //BOOT_PSRAM_APS_32P:
        board_pinmux_psram_func1_2_4(2);    // 32Mb APM LEGACY PSRAM
        break;
    case 6: //BOOT_PSRAM_WINBOND:                // Winbond HYPERBUS PSRAM
        board_pinmux_psram_func1_2_4(4);
        break;
    case 3: // BOOT_PSRAM_APS_64P:
        board_pinmux_psram_func1_2_4(1);    // 64Mb APM XCELLA PSRAM
        break;
    case 2: //BOOT_PSRAM_APS_128P:
        board_pinmux_psram_func0();         // 128Mb APM XCELLA PSRAM
        break;
    default:
        board_pinmux_mpi1_none();
        break;
    }
#endif /* CONFIG_BSP_USING_SPI_NAND */
#endif /* BSP_USING_PSRAM1 */
#ifndef BSP_USING_BOARD_SF32LB52_LCD_52J_SD
#if defined(BSP_USING_SDIO) && defined(BSP_ENABLE_MPI2)
#error "SDIO and MPI2 cannot be used at the same time, please disable one of them"
#endif
#endif

#ifdef BSP_USING_SDIO
    HAL_PIN_Set(PAD_PA15, SD1_CMD, PIN_PULLUP, 1);
    HAL_Delay_us(20);
    HAL_PIN_Set(PAD_PA12, SD1_DIO2,  PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA13, SD1_DIO3, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA14, SD1_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA16, SD1_DIO0, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA17, SD1_DIO1, PIN_PULLUP, 1);
#endif
#ifdef BSP_ENABLE_MPI2
    // MPI2
    HAL_PIN_Set(PAD_PA16, MPI2_CLK,  PIN_NOPULL,   1);
    HAL_PIN_Set(PAD_PA12, MPI2_CS,   PIN_NOPULL,   1);
    HAL_PIN_Set(PAD_PA15, MPI2_DIO0, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA13, MPI2_DIO1, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA14, MPI2_DIO2, PIN_PULLUP,   1);
    HAL_PIN_Set(PAD_PA17, MPI2_DIO3, PIN_PULLUP, 1);
#endif
    // UART1 - debug
    HAL_PIN_Set(PAD_PA18, USART1_RXD, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA19, USART1_TXD, PIN_PULLUP, 1);

    /* UART2 - u-blox MAX-M10S-00B-01:
     * PA31 USART2_TXD -> GNSS_RX (module RX)
     * PA32 USART2_RXD <- GNSS_TX (module TX)
     * PA43 GNSS_PWR: module VCC (official low-power cut; host UART stays open) */
    HAL_PIN_Set(PAD_PA31, USART2_TXD, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA32, USART2_RXD, PIN_PULLUP, 1);
    /* GNSS_PWR PA43: idle off. Default active-low uses pull-up. */
#ifdef CONFIG_BOARD_L96_GNSS_PWR_ACTIVE_HIGH
    HAL_PIN_Set(PAD_PA43, GPIO_A43, PIN_PULLDOWN, 1);
#else
    HAL_PIN_Set(PAD_PA43, GPIO_A43, PIN_PULLUP, 1);
#endif
    /* PA29 is PWR_KEY_CTL on this board (not GNSS power). Hold HIGH. */
    HAL_PIN_Set(PAD_PA29, GPIO_A29, PIN_PULLUP, 1);
    BSP_GPIO_Set(29, 1, 1);

    /* 3.3V DCDC (TPS63802) MODE: only PWR_MD remains. HIGH = FPWM. */
    HAL_PIN_Set(PAD_PA27, GPIO_A27, PIN_PULLUP, 1);
    BSP_GPIO_Set(27, 1, 1);

    /* KEY1 PA30 / KEY2 PA33: pull-up input (press to GND). */
    HAL_PIN_Set(PAD_PA33, GPIO_A33, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA30, GPIO_A30, PIN_PULLUP, 1);

    /* PWR_KEY_READ PA34: pull-down input (press to HIGH).
     * 硬件长按约 10s 复位，软件不用管。 */
    HAL_PIN_Set(PAD_PA34, GPIO_A34, PIN_PULLDOWN, 1);

    // PA22 #XTAL32K_XI
    // PA23 #XTAL32K_XO

    // USBD
    HAL_PIN_Set_Analog(PAD_PA35, 1);                    // USB_DP
    HAL_PIN_Set_Analog(PAD_PA36, 1);                    // USB_DM

    /* I2C1 shared sensor/touch bus (ext 5.1k to VSYS+3V3):
     * PA39 SCL, PA38 SDA.  Do not remap these as SPI2 later. */
    HAL_PIN_Set(PAD_PA39, I2C1_SCL, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA38, I2C1_SDA, PIN_PULLUP, 1);

    /* ETA9184: open-drain / Hi-Z, MCU pull-up. */
    HAL_PIN_Set(PAD_PA24, GPIO_A24, PIN_PULLUP, 1);     /* DISCHRG */
    HAL_PIN_Set(PAD_PA25, GPIO_A25, PIN_PULLUP, 1);     /* STAT    */
    HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_PULLUP, 1);     /* PULSE   */
    HAL_PIN_Set(PAD_PA28, GPIO_A28, PIN_PULLDOWN, 1);   /* ETA_EN idle off */
    BSP_GPIO_Set(28, 0, 1);




    // HAL_PIN_Set(PAD_PA30, I2C1_SCL, PIN_PULLUP, 1);
    // PA30/PA33 are KEY1/KEY2 (were touch I2C SCL/SDA)
    // HAL_PIN_Set(PAD_PA33, I2C1_SDA, PIN_PULLUP, 1);

    // HAL_PIN_Set_DS0(PAD_PA24, 1, 1);
    // HAL_PIN_Set_DS0(PAD_PA25, 1, 1);
    // HAL_PIN_Set_DS0(PAD_PA28, 1, 1);
    // HAL_PIN_Set_DS0(PAD_PA29, 1, 1);
    //
    // HAL_PIN_Set_DS1(PAD_PA24, 1, 1);
    // HAL_PIN_Set_DS1(PAD_PA25, 1, 1);
    // HAL_PIN_Set_DS1(PAD_PA28, 1, 1);
    // HAL_PIN_Set_DS1(PAD_PA29, 1, 1);

    // GPIOs
    HAL_PIN_Set(PAD_PA21, GPIO_A21, PIN_PULLDOWN, 1);
    /* PA38 is I2C1 SDA; PA44 is TP_INT on the current board. */
    // HAL_PIN_Set(PAD_PA44, GPIO_A44, PIN_PULLDOWN, 1);
#endif

}

#ifdef LCD_USING_ST7789S
/* 后续剔除：Helm One 已改 NV3031A。本段 ST7789 背光走 PA41/GPTIM2，
 * 与产品 PA40 蜂鸣片、PA01 背光无关。 */

#include "tim_config.h"
#include <string.h>

#define LCD_DC_GPIO                 42 /**< ST7789 DC 脚 PA42。 */

/* PA41 backlight: GPTIM2 CH1 PWM */
#define LCD_BL_PWM_FREQ_HZ          1000u   /**< 背光 PWM 频率。 */
#define LCD_BL_ON_BRIGHTNESS_PCT     30u    /**< 默认开屏亮度。 */
#define LCD_BL_MAX_BRIGHTNESS_PCT   100u    /**< 硬件最大亮度百分比。 */
#define LCD_BL_PWM_TIM_CHANNEL       GPT_CHANNEL_1 /**< GPTIM2 CH1。 */

static GPT_HandleTypeDef s_lcd_bl_pwm;       /**< ST7789 背光 PWM 句柄。 */
static bool s_lcd_bl_pwm_running;             /**< PWM 是否在输出。 */
static uint8_t s_bl_saved = LCD_BL_ON_BRIGHTNESS_PCT; /**< 策略保存亮度。 */
static uint8_t s_bl_applied;                  /**< 当前 PWM 应用亮度。 */

/** @brief 读 GPTIM2 定时器时钟。 */
static uint32_t lcd_bl_pwm_timclk(void)
{
#ifdef SF32LB52X
    if (s_lcd_bl_pwm.Instance == hwp_gptim2)
    {
        return 24000000u;
    }
#endif

    return HAL_RCC_GetPCLKFreq(GPTIM2_CORE, 1);
}

/** @brief 应用 ST7789 背光 PWM 占空比；0 关断。 */
static int lcd_bl_pwm_apply(uint8_t brightness_pct)
{
    GPT_OC_InitTypeDef oc_cfg;
    uint32_t timclk;
    uint64_t ticks;
    uint32_t prescaler;
    uint32_t period;
    uint32_t pulse;

    if (brightness_pct == 0)
    {
        if (s_lcd_bl_pwm_running)
        {
            HAL_GPT_PWM_Stop(&s_lcd_bl_pwm, LCD_BL_PWM_TIM_CHANNEL);
            s_lcd_bl_pwm_running = false;
        }

        s_bl_applied = 0;
        return 0;
    }

    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    if (s_lcd_bl_pwm.Instance == NULL)
    {
        memset(&s_lcd_bl_pwm, 0, sizeof(s_lcd_bl_pwm));
        s_lcd_bl_pwm.Instance = GPTIM2;
        s_lcd_bl_pwm.core     = GPTIM2_CORE;
    }

    timclk = lcd_bl_pwm_timclk();
    if (timclk == 0)
    {
        return -1;
    }

    ticks = (uint64_t)timclk / (uint64_t)LCD_BL_PWM_FREQ_HZ;
    if (ticks == 0)
    {
        ticks = 1;
    }

    prescaler = (uint32_t)((ticks + 65535ULL - 1ULL) / 65535ULL);
    if (prescaler == 0)
    {
        prescaler = 1;
    }

    period = (uint32_t)(ticks / prescaler);
    if (period == 0)
    {
        period = 1;
    }
    else if (period > 65535)
    {
        period = 65535;
    }

    pulse = (uint32_t)(((uint64_t)brightness_pct * (uint64_t)period) / 100ULL);
    if (pulse > period)
    {
        pulse = period;
    }

    if (s_lcd_bl_pwm_running && s_lcd_bl_pwm.Instance != NULL)
    {
        uint32_t per = s_lcd_bl_pwm.Init.Period + 1u;

        if (per == 0u)
        {
            per = 1u;
        }

        pulse = (uint32_t)(((uint64_t)brightness_pct * (uint64_t)per) / 100ULL);
        if (pulse > per)
        {
            pulse = per;
        }

        s_lcd_bl_pwm.Instance->CCR1 = pulse;
        s_bl_applied = brightness_pct;
        return 0;
    }

    if (s_lcd_bl_pwm_running)
    {
        HAL_GPT_PWM_Stop(&s_lcd_bl_pwm, LCD_BL_PWM_TIM_CHANNEL);
        s_lcd_bl_pwm_running = false;
    }

    s_lcd_bl_pwm.Init.Prescaler         = prescaler - 1;
    s_lcd_bl_pwm.Init.CounterMode       = GPT_COUNTERMODE_UP;
    s_lcd_bl_pwm.Init.Period            = period - 1;
    s_lcd_bl_pwm.Init.RepetitionCounter = 0;

    if (HAL_GPT_PWM_Init(&s_lcd_bl_pwm) != HAL_OK)
    {
        return -1;
    }

    oc_cfg.OCMode       = GPT_OCMODE_PWM1;
    oc_cfg.Pulse        = pulse;
    oc_cfg.OCPolarity   = GPT_OCPOLARITY_HIGH;
    oc_cfg.OCNPolarity  = GPT_OCNPOLARITY_LOW;
    oc_cfg.OCFastMode   = GPT_OCFAST_DISABLE;
    oc_cfg.OCIdleState  = GPT_OCIDLESTATE_RESET;
    oc_cfg.OCNIdleState = GPT_OCNIDLESTATE_RESET;

    if (HAL_GPT_PWM_ConfigChannel(&s_lcd_bl_pwm, &oc_cfg,
                                  LCD_BL_PWM_TIM_CHANNEL) != HAL_OK)
    {
        return -1;
    }

    if (HAL_GPT_PWM_Start(&s_lcd_bl_pwm, LCD_BL_PWM_TIM_CHANNEL) != HAL_OK)
    {
        return -1;
    }

    s_lcd_bl_pwm_running = true;
    s_bl_applied = brightness_pct;
    return 0;
}

/** @brief ST7789：仅保存亮度，不立刻改 PWM。 */
void BSP_LCD_BL_PrefPct(uint8_t brightness_pct)
{
    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    s_bl_saved = brightness_pct;
}

/**
 * @brief ST7789：设置背光并立即应用 PWM。
 * @return 0 成功，-1 失败。
 */
int BSP_LCD_BL_SetPct(uint8_t brightness_pct)
{
    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    s_bl_saved = brightness_pct;
    return lcd_bl_pwm_apply(brightness_pct);
}

/** @brief ST7789：只改 PWM，不改策略保存值。 */
int BSP_LCD_BL_PwmPct(uint8_t brightness_pct)
{
    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    return lcd_bl_pwm_apply(brightness_pct);
}

/** @brief ST7789：读当前 PWM 亮度。 */
uint8_t BSP_LCD_BL_GetPct(void)
{
    return s_bl_applied;
}

/** @brief ST7789：读保存的亮度策略值。 */
uint8_t BSP_LCD_BL_GetSavedPct(void)
{
    return s_bl_saved;
}

/** @brief ST7789：最大亮度上限。 */
uint8_t BSP_LCD_BL_GetMaxPct(void)
{
    return LCD_BL_MAX_BRIGHTNESS_PCT;
}

/** @brief ST7789：开关背光。 */
void BSP_LCD_BL_Set(uint8_t on)
{
    lcd_bl_pwm_apply(on ? s_bl_saved : 0);
}

/** @brief 设置 ST7789 DC：低命令、高数据。 */
void BSP_LCD_DC_Set(uint8_t is_cmd)
{
    /* DC low = command, DC high = data */

    BSP_GPIO_Set(LCD_DC_GPIO, is_cmd ? 0 : 1, 1);
}

/** @brief ST7789 SPI + 背光 PWM + DC 引脚复用。 */
void BSP_PIN_LCD(void)
{
    HAL_PIN_Set(PAD_PA37, SPI2_DIO, PIN_NOPULL, 1);   /* MOSI */
    /* PA38/PA39 are I2C1 SDA/SCL — leave them, do not mux as SPI2 MISO/SCK. */
    HAL_PIN_Set(PAD_PA40, SPI2_CS,  PIN_NOPULL, 1);   /* CS */
    HAL_PIN_Set(PAD_PA41, GPTIM2_CH1, PIN_NOPULL, 1);   /* BL PWM */
    HAL_PIN_Set(PAD_PA42, GPIO_A42, PIN_NOPULL, 1);   /* DC */

    BSP_LCD_BL_Set(0);
    BSP_LCD_DC_Set(1);
}

#ifdef LCD_ST7789S_SOFTSPI
/** @brief ST7789 软 SPI 引脚复用（PA39 保留 I2C）。 */
void BSP_PIN_LCD_SOFTSPI(void)
{
    HAL_PIN_Set(PAD_PA37, GPIO_A37, PIN_NOPULL, 1);   /* MOSI */
    /* PA39 is I2C1 SCL — do not bit-bang SCK on this pad. */
    HAL_PIN_Set(PAD_PA40, GPIO_A40, PIN_NOPULL, 1);   /* CS */
    HAL_PIN_Set(PAD_PA41, GPTIM2_CH1, PIN_NOPULL, 1);   /* BL PWM */
    HAL_PIN_Set(PAD_PA42, GPIO_A42, PIN_NOPULL, 1);   /* DC */

    /* GPIO outputs for bit-bang SPI (mode 3 idle: CS high). */

    BSP_GPIO_Set(37, 0, 1);
    BSP_GPIO_Set(40, 1, 1);
    BSP_LCD_BL_Set(0);
    BSP_LCD_DC_Set(0);
}
#endif /* LCD_ST7789S_SOFTSPI */

/** @brief ST7789 上电延时。 */
void BSP_LCD_PowerUp(void)
{
    HAL_Delay_us(500);
}

/** @brief ST7789 下电：关背光。 */
void BSP_LCD_PowerDown(void)
{
    BSP_LCD_BL_Set(0);
}

#elif defined(LCD_USING_NV3031A)

#include "tim_config.h"
#include <string.h>

#define LCD_RESET_PIN               0  /**< NV3031A RST PA00。 */
#define LCD_BL_PIN                  1  /**< NV3031A 背光 PA01。 */
#define LCD_PWR_PIN                 10 /**< NV3031A 屏电源 PA10，低有效。 */

/* PA01 backlight: GPTIM1 CH4 PWM.
 * Board has no LED series resistor (LEDA=VCC, LEDK via Q5). PWM only
 * cuts average current; peak If while Q5 is on is still unlimited.
 * Keep duty conservative: UI 100% maps to 80% PWM. */
#define LCD_BL_PWM_FREQ_HZ          20000u  /**< 背光 PWM 20 kHz。 */
#define LCD_BL_ON_BRIGHTNESS_PCT    16u     /**< 默认开屏 = UI 20%。 */
#define LCD_BL_MAX_BRIGHTNESS_PCT    80u    /**< UI 100% 对应的硬件 PWM。 */
#define LCD_BL_PWM_TIM_CHANNEL      GPT_CHANNEL_4 /**< GPTIM1 CH4。 */

static GPT_HandleTypeDef s_lcd_bl_pwm;       /**< NV3031A 背光 PWM 句柄。 */
static bool s_lcd_bl_pwm_running;            /**< PWM 是否在输出。 */
static uint8_t s_bl_saved = LCD_BL_ON_BRIGHTNESS_PCT; /**< 策略保存亮度。 */
static uint8_t s_bl_applied;                 /**< 当前应用亮度。 */

/** @brief NV3031A：背光切回 GPIO 并拉低。 */
static void lcd_bl_gpio_off(void)
{
    HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
    BSP_GPIO_Set(LCD_BL_PIN, 0, 1);
}

static uint32_t lcd_bl_pwm_timclk(void)
{
#ifdef SF32LB52X
    return 24000000u;
#else
    return HAL_RCC_GetPCLKFreq(GPTIM1_CORE, 1);
#endif
}

/** @brief NV3031A：应用背光 PWM；0 时 GPIO 关断。 */
static int lcd_bl_pwm_apply(uint8_t brightness_pct)
{
    GPT_OC_InitTypeDef oc_cfg;
    uint32_t timclk;
    uint64_t ticks;
    uint32_t prescaler;
    uint32_t period;
    uint32_t pulse;

    if (brightness_pct == 0)
    {
        if (s_lcd_bl_pwm_running)
        {
            HAL_GPT_PWM_Stop(&s_lcd_bl_pwm, LCD_BL_PWM_TIM_CHANNEL);
            s_lcd_bl_pwm_running = false;
        }

        lcd_bl_gpio_off();
        s_bl_applied = 0;
        return 0;
    }

    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    /* Already running: only CCR. Stop+Init on every menu step hung bicycle_ui. */
    if (s_lcd_bl_pwm_running && s_lcd_bl_pwm.Instance != NULL)
    {
        uint32_t per = s_lcd_bl_pwm.Init.Period + 1u;

        if (per == 0u)
        {
            per = 1u;
        }

        pulse = (uint32_t)(((uint64_t)brightness_pct * (uint64_t)per) / 100ULL);
        if (pulse > per)
        {
            pulse = per;
        }

        s_lcd_bl_pwm.Instance->CCR4 = pulse;
        s_bl_applied = brightness_pct;
        return 0;
    }

    if (s_lcd_bl_pwm.Instance == NULL)
    {
        memset(&s_lcd_bl_pwm, 0, sizeof(s_lcd_bl_pwm));
        s_lcd_bl_pwm.Instance = GPTIM1;
        s_lcd_bl_pwm.core     = GPTIM1_CORE;
    }

    HAL_RCC_EnableModule(RCC_MOD_GPTIM1);

    timclk = lcd_bl_pwm_timclk();
    if (timclk == 0)
    {
        lcd_bl_gpio_off();
        return -1;
    }

    ticks = (uint64_t)timclk / (uint64_t)LCD_BL_PWM_FREQ_HZ;
    if (ticks == 0)
    {
        ticks = 1;
    }

    prescaler = (uint32_t)((ticks + 65535ULL - 1ULL) / 65535ULL);
    if (prescaler == 0)
    {
        prescaler = 1;
    }

    period = (uint32_t)(ticks / prescaler);
    if (period == 0)
    {
        period = 1;
    }
    else if (period > 65535)
    {
        period = 65535;
    }

    pulse = (uint32_t)(((uint64_t)brightness_pct * (uint64_t)period) / 100ULL);
    if (pulse > period)
    {
        pulse = period;
    }

    if (s_lcd_bl_pwm_running)
    {
        HAL_GPT_PWM_Stop(&s_lcd_bl_pwm, LCD_BL_PWM_TIM_CHANNEL);
        s_lcd_bl_pwm_running = false;
    }

    s_lcd_bl_pwm.Init.Prescaler         = prescaler - 1;
    s_lcd_bl_pwm.Init.CounterMode       = GPT_COUNTERMODE_UP;
    s_lcd_bl_pwm.Init.Period            = period - 1;
    s_lcd_bl_pwm.Init.RepetitionCounter = 0;

    if (HAL_GPT_PWM_Init(&s_lcd_bl_pwm) != HAL_OK)
    {
        lcd_bl_gpio_off();
        return -1;
    }

    oc_cfg.OCMode       = GPT_OCMODE_PWM1;
    oc_cfg.Pulse        = pulse;
    oc_cfg.OCPolarity   = GPT_OCPOLARITY_HIGH;
    oc_cfg.OCNPolarity  = GPT_OCNPOLARITY_LOW;
    oc_cfg.OCFastMode   = GPT_OCFAST_DISABLE;
    oc_cfg.OCIdleState  = GPT_OCIDLESTATE_RESET;
    oc_cfg.OCNIdleState = GPT_OCNIDLESTATE_RESET;

    if (HAL_GPT_PWM_ConfigChannel(&s_lcd_bl_pwm, &oc_cfg,
                                  LCD_BL_PWM_TIM_CHANNEL) != HAL_OK)
    {
        lcd_bl_gpio_off();
        return -1;
    }

    HAL_PIN_Set(PAD_PA01, GPTIM1_CH4, PIN_NOPULL, 1);

    if (HAL_GPT_PWM_Start(&s_lcd_bl_pwm, LCD_BL_PWM_TIM_CHANNEL) != HAL_OK)
    {
        lcd_bl_gpio_off();
        return -1;
    }

    s_lcd_bl_pwm_running = true;
    s_bl_applied = brightness_pct;
    return 0;
}

/** @brief NV3031A：驱动 RST 脚。 */
void BSP_LCD_Reset(uint8_t high1_low0)
{
    BSP_GPIO_Set(LCD_RESET_PIN, high1_low0, 1);
}

/** @brief NV3031A：仅保存亮度。 */
void BSP_LCD_BL_PrefPct(uint8_t brightness_pct)
{
    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    s_bl_saved = brightness_pct;
}

/** @brief NV3031A：设置并应用背光 PWM。 */
int BSP_LCD_BL_SetPct(uint8_t brightness_pct)
{
    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    s_bl_saved = brightness_pct;
    return lcd_bl_pwm_apply(brightness_pct);
}

/** @brief NV3031A：只改 PWM，不改策略保存值。 */
int BSP_LCD_BL_PwmPct(uint8_t brightness_pct)
{
    if (brightness_pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        brightness_pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    return lcd_bl_pwm_apply(brightness_pct);
}

/** @brief NV3031A：读当前 PWM 亮度。 */
uint8_t BSP_LCD_BL_GetPct(void)
{
    return s_bl_applied;
}

/** @brief NV3031A：读保存亮度。 */
uint8_t BSP_LCD_BL_GetSavedPct(void)
{
    return s_bl_saved;
}

/** @brief NV3031A：最大亮度上限。 */
uint8_t BSP_LCD_BL_GetMaxPct(void)
{
    return LCD_BL_MAX_BRIGHTNESS_PCT;
}

/** @brief NV3031A：开关背光。 */
void BSP_LCD_BL_Set(uint8_t on)
{
    lcd_bl_pwm_apply(on ? s_bl_saved : 0);
}

/** @brief NV3031A LCDC SPI 与电源脚复用。 */
void BSP_PIN_LCD(void)
{
    HAL_PIN_Set(PAD_PA00, GPIO_A0, PIN_NOPULL, 1);      /* RST */
#ifdef NV3031A_APP_REINIT_PANEL
    HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);      /* BL GPIO until PWM on */
#endif
    HAL_PIN_Set(PAD_PA10, GPIO_A10, PIN_NOPULL, 1);     /* PWR, active-low */

    HAL_PIN_Set(PAD_PA03, LCDC1_SPI_CS, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA04, LCDC1_SPI_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA05, LCDC1_SPI_DIO0, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA06, LCDC1_SPI_DIO1, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA07, LCDC1_SPI_DIO2, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA08, LCDC1_SPI_DIO3, PIN_NOPULL, 1);

#ifndef NV3031A_APP_REINIT_PANEL
    /* 沿用 2SFBL：保持屏电和复位释放，不要拉低背光。 */
    BSP_GPIO_Set(LCD_PWR_PIN, 0, 1);
    BSP_GPIO_Set(LCD_RESET_PIN, 1, 1);
#else
    /* Idle: panel power off (HIGH), backlight off (LOW), reset held low. */

    BSP_GPIO_Set(LCD_PWR_PIN, 1, 1);
    lcd_bl_gpio_off();
    BSP_GPIO_Set(LCD_RESET_PIN, 0, 1);
#endif
}

/** @brief NV3031A：上电序列（pinmux + 屏电源）。 */
void BSP_LCD_PowerUp(void)
{
    BSP_PIN_LCD();
    BSP_GPIO_Set(LCD_PWR_PIN, 0, 1);
    HAL_Delay_us(20000);
}

/** @brief NV3031A：下电关背光并断屏电源。 */
void BSP_LCD_PowerDown(void)
{
    BSP_LCD_BL_Set(0);
    BSP_GPIO_Set(LCD_RESET_PIN, 0, 1);
    BSP_GPIO_Set(LCD_PWR_PIN, 1, 1);
}

#else /* no LCD panel */

/** @brief 无 LCD 板型：背光空实现。 */
void BSP_LCD_BL_Set(uint8_t on)
{
    (void)on;
}

void BSP_LCD_BL_PrefPct(uint8_t brightness_pct)
{
    (void)brightness_pct;
}

int BSP_LCD_BL_SetPct(uint8_t brightness_pct)
{
    (void)brightness_pct;
    return -1;
}

int BSP_LCD_BL_PwmPct(uint8_t brightness_pct)
{
    (void)brightness_pct;
    return -1;
}

uint8_t BSP_LCD_BL_GetPct(void)
{
    return 0;
}

uint8_t BSP_LCD_BL_GetSavedPct(void)
{
    return 0;
}

uint8_t BSP_LCD_BL_GetMaxPct(void)
{
    return 0;
}

#endif /* LCD_USING_ST7789S / LCD_USING_NV3031A */

/**
 * @brief 板级 pinmux 总入口：通用脚 + LCD + 恢复 I2C/GNSS UART。
 *
 * 由 BSP_IO_Init() 在 HAL_MspInit 中调用。
 */
void BSP_PIN_Init(void)
{
    BSP_PIN_Common();

#ifdef LCD_USING_ST7789S
/* 后续剔除：ST7789 pinmux。产品走下面 NV3031A。 */
#ifdef LCD_ST7789S_SOFTSPI
    BSP_PIN_LCD_SOFTSPI();
#else
    BSP_PIN_LCD();
#endif
#elif defined(LCD_USING_NV3031A)
    BSP_PIN_LCD();
#endif

#ifdef SOC_BF0_HCPU
    /* LCD pinmux used to claim PA38/PA39 as SPI2. Re-apply I2C last. */
    HAL_PIN_Set(PAD_PA39, I2C1_SCL, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA38, I2C1_SDA, PIN_PULLUP, 1);
    /* GNSS USART2 must stay on PA31/PA32 (not the old PA25/PA28). */
    HAL_PIN_Set(PAD_PA31, USART2_TXD, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA32, USART2_RXD, PIN_PULLUP, 1);
    /* Helm One: PA40 无源陶瓷蜂鸣片（GPTIM2 CH1 PWM，声音线程里切功能）。 */
    HAL_PIN_Set(PAD_PA40, GPIO_A40, PIN_NOPULL, 1);
    BSP_GPIO_Set(40, 0, 1);
#endif
}
