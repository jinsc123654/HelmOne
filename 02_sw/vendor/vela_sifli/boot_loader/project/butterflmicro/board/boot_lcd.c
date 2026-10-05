/**
 * @file boot_lcd.c
 * @brief NV3031A 240x320 QAD-SPI via LCDC1 (polling). Helm One pinout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_lcd.h"
#include "boot_flash.h"
#include "boot_hw.h"
#include "board.h"
#include "bf0_hal.h"
#include "register.h"

#include <string.h>
#include <stdint.h>

#define LCD_RESET_PIN  0
#define LCD_BL_PIN     1
#define LCD_PWR_PIN    10

#define LCD_BL_PWM_FREQ_HZ         20000u
#define LCD_BL_ON_BRIGHTNESS_PCT   16u
#define LCD_BL_MAX_BRIGHTNESS_PCT  80u
#define LCD_BL_PWM_TIM_CHANNEL     GPT_CHANNEL_4

#define REG_SLEEP_OUT            0x11
#define REG_DISPLAY_ON           0x29
#define REG_WRITE_RAM            0x2C
#define REG_CASET                0x2A
#define REG_RASET                0x2B
#define REG_TEARING_EFFECT_OFF   0x34
#define REG_MADCTL               0x36
#define REG_COLOR_MODE           0x3A
#define REG_CONTINUE_WRITE_RAM   0x3C

static LCDC_HandleTypeDef g_lcdc;
static GPT_HandleTypeDef g_bl_pwm;
static int g_bl_running;
static int g_ready;

static void boot_lcd_delay_ms(uint32_t ms)
{
    while (ms--)
    {
        boot_hw_wdt_pet();
        boot_hw_factory_poll();
        HAL_Delay_us(1000);
    }
}

static void boot_lcd_wr(uint16_t reg, uint8_t *data, uint32_t n)
{
    uint32_t cmd;

    if ((reg == REG_WRITE_RAM) || (reg == REG_CONTINUE_WRITE_RAM))
    {
        cmd = (0x32u << 24) | ((uint32_t)reg << 8);
    }
    else
    {
        cmd = (0x02u << 24) | ((uint32_t)reg << 8);
    }
    HAL_LCDC_WriteU32Reg(&g_lcdc, cmd, data, n);
}

static void boot_lcd_set_region(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t p[4];

    HAL_LCDC_SetROIArea(&g_lcdc, x0, y0, x1, y1);

    p[0] = (uint8_t)(x0 >> 8);
    p[1] = (uint8_t)(x0 & 0xFF);
    p[2] = (uint8_t)(x1 >> 8);
    p[3] = (uint8_t)(x1 & 0xFF);
    boot_lcd_wr(REG_CASET, p, 4);

    p[0] = (uint8_t)(y0 >> 8);
    p[1] = (uint8_t)(y0 & 0xFF);
    p[2] = (uint8_t)(y1 >> 8);
    p[3] = (uint8_t)(y1 & 0xFF);
    boot_lcd_wr(REG_RASET, p, 4);
}

static void boot_lcd_pinmux(void)
{
    HAL_PIN_Set(PAD_PA00, GPIO_A0, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA10, GPIO_A10, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA03, LCDC1_SPI_CS, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA04, LCDC1_SPI_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA05, LCDC1_SPI_DIO0, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA06, LCDC1_SPI_DIO1, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA07, LCDC1_SPI_DIO2, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA08, LCDC1_SPI_DIO3, PIN_NOPULL, 1);

    BSP_GPIO_Set(LCD_PWR_PIN, 1, 1);
    HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
    BSP_GPIO_Set(LCD_BL_PIN, 0, 1);
    BSP_GPIO_Set(LCD_RESET_PIN, 0, 1);
}

static void boot_lcd_panel_init(void)
{
    uint8_t p[8];

    BSP_GPIO_Set(LCD_RESET_PIN, 1, 1);
    boot_lcd_delay_ms(20);
    BSP_GPIO_Set(LCD_RESET_PIN, 0, 1);
    boot_lcd_delay_ms(220);
    BSP_GPIO_Set(LCD_RESET_PIN, 1, 1);
    boot_lcd_delay_ms(120);

    p[0] = 0x06;
    p[1] = 0x08;
    boot_lcd_wr(0xFD, p, 2);
    p[0] = 0x07;
    p[1] = 0x07;
    boot_lcd_wr(0x61, p, 2);
    p[0] = 0x70;
    boot_lcd_wr(0x73, p, 1);
    p[0] = 0x00;
    boot_lcd_wr(0x73, p, 1);
    p[0] = 0x00;
    p[1] = 0x44;
    p[2] = 0x40;
    boot_lcd_wr(0x62, p, 3);
    p[0] = 0x41;
    p[1] = 0x07;
    p[2] = 0x12;
    p[3] = 0x12;
    boot_lcd_wr(0x63, p, 4);
    p[0] = 0x37;
    boot_lcd_wr(0x64, p, 1);
    p[0] = 0x09;
    p[1] = 0x10;
    p[2] = 0x21;
    boot_lcd_wr(0x65, p, 3);
    boot_lcd_wr(0x66, p, 3);
    p[0] = 0x21;
    p[1] = 0x40;
    boot_lcd_wr(0x67, p, 2);
    p[0] = 0x60;
    p[1] = 0x60;
    p[2] = 0x3C;
    p[3] = 0x1C;
    boot_lcd_wr(0x68, p, 4);
    p[0] = 0x0F;
    p[1] = 0x02;
    p[2] = 0x03;
    boot_lcd_wr(0xB1, p, 3);
    p[0] = 0x01;
    boot_lcd_wr(0xB4, p, 1);
    p[0] = 0x02;
    p[1] = 0x02;
    p[2] = 0x0A;
    p[3] = 0x14;
    boot_lcd_wr(0xB5, p, 4);
    p[0] = 0x44;
    p[1] = 0x01;
    p[2] = 0x9F;
    p[3] = 0x00;
    p[4] = 0x02;
    boot_lcd_wr(0xB6, p, 5);
    p[0] = 0x11;
    boot_lcd_wr(0xDF, p, 1);

    p[0] = 0x04;
    p[1] = 0x04;
    p[2] = 0x0C;
    p[3] = 0x0E;
    p[4] = 0x10;
    p[5] = 0x0F;
    p[6] = 0x13;
    p[7] = 0x17;
    boot_lcd_wr(0xE0, p, 8);
    p[0] = 0x17;
    p[1] = 0x13;
    p[2] = 0x0D;
    p[3] = 0x0B;
    p[4] = 0x0F;
    p[5] = 0x0C;
    p[6] = 0x05;
    p[7] = 0x05;
    boot_lcd_wr(0xE3, p, 8);
    p[0] = 0x0A;
    p[1] = 0x68;
    boot_lcd_wr(0xE1, p, 2);
    p[0] = 0x68;
    p[1] = 0x1E;
    boot_lcd_wr(0xE4, p, 2);
    p[0] = 0x05;
    p[1] = 0x06;
    p[2] = 0x05;
    p[3] = 0x33;
    p[4] = 0x34;
    p[5] = 0x3A;
    boot_lcd_wr(0xE2, p, 6);
    p[0] = 0x3A;
    p[1] = 0x35;
    p[2] = 0x32;
    p[3] = 0x05;
    p[4] = 0x06;
    p[5] = 0x05;
    boot_lcd_wr(0xE5, p, 6);
    p[0] = 0x00;
    p[1] = 0xFF;
    boot_lcd_wr(0xE6, p, 2);
    p[0] = 0x01;
    p[1] = 0x04;
    p[2] = 0x03;
    p[3] = 0x03;
    p[4] = 0x00;
    p[5] = 0x12;
    boot_lcd_wr(0xE7, p, 6);
    p[0] = 0x00;
    p[1] = 0x70;
    p[2] = 0x00;
    boot_lcd_wr(0xE8, p, 3);
    p[0] = 0x52;
    boot_lcd_wr(0xEC, p, 1);
    p[0] = 0x01;
    p[1] = 0xAA;
    p[2] = 0xAB;
    boot_lcd_wr(0xF1, p, 3);
    p[0] = 0x01;
    p[1] = 0x30;
    p[2] = 0x00;
    p[3] = 0x00;
    boot_lcd_wr(0xF6, p, 4);
    p[0] = 0xFA;
    p[1] = 0xFC;
    boot_lcd_wr(0xFD, p, 2);

    p[0] = 0x55;
    boot_lcd_wr(REG_COLOR_MODE, p, 1);
    boot_lcd_wr(REG_TEARING_EFFECT_OFF, NULL, 0);
    p[0] = 0x00;
    boot_lcd_wr(REG_MADCTL, p, 1);

    p[0] = 0x00;
    boot_lcd_wr(REG_SLEEP_OUT, p, 1);
    boot_lcd_delay_ms(120);
    p[0] = 0x00;
    boot_lcd_wr(REG_DISPLAY_ON, p, 1);
    boot_lcd_delay_ms(300);

    HAL_LCDC_Next_Frame_TE(&g_lcdc, 0);
    boot_lcd_set_region(0, 0, BOOT_LCD_WIDTH - 1, BOOT_LCD_HEIGHT - 1);
    HAL_LCDC_LayerSetFormat(&g_lcdc, HAL_LCDC_LAYER_DEFAULT, LCDC_PIXEL_FORMAT_RGB565);
    HAL_LCDC_LayerDisable(&g_lcdc, HAL_LCDC_LAYER_DEFAULT);
    HAL_LCDC_SetBgColor(&g_lcdc, 0, 0, 0);
    HAL_LCDC_SendLayerData2Reg(&g_lcdc, ((0x32u << 24) | (REG_WRITE_RAM << 8)), 4);
    HAL_LCDC_LayerEnable(&g_lcdc, HAL_LCDC_LAYER_DEFAULT);
}

int boot_lcd_init(void)
{
    static const LCDC_InitTypeDef cfg =
    {
        .lcd_itf = LCDC_INTF_SPI_DCX_4DATA,
        .freq = 24000000,
        .color_mode = LCDC_PIXEL_FORMAT_RGB565,
        .cfg =
        {
            .spi =
            {
                .dummy_clock = 0,
                .syn_mode = HAL_LCDC_SYNC_DISABLE,
                .vsyn_polarity = 0,
                .vsyn_delay_us = 0,
                .hsyn_num = 0,
            },
        },
    };

    if (g_ready)
    {
        return 0;
    }

    boot_hw_wdt_pet();
    boot_lcd_pinmux();
    BSP_GPIO_Set(LCD_PWR_PIN, 0, 1);
    HAL_Delay_us(20000);

    memset(&g_lcdc, 0, sizeof(g_lcdc));
    g_lcdc.Instance = LCDC1;
    memcpy(&g_lcdc.Init, &cfg, sizeof(cfg));
    if (HAL_LCDC_Init(&g_lcdc) != HAL_OK)
    {
        return -1;
    }
    /* memset left total_width=0; LCDC then uses 0-byte stride and SGL
     * blits are invisible. NuttX calls LayerReset (INVALID_TOTAL_WIDTH). */
    HAL_LCDC_LayerReset(&g_lcdc, HAL_LCDC_LAYER_DEFAULT);
    HAL_LCDC_LayerSetFormat(&g_lcdc, HAL_LCDC_LAYER_DEFAULT,
                            LCDC_PIXEL_FORMAT_RGB565);

    boot_lcd_panel_init();
    g_ready = 1;
    /* Leave BL off: charger UI is transflective; splash turns it on. */
    boot_lcd_backlight_off();
    return 0;
}

int boot_lcd_ready(void)
{
    return g_ready;
}

int boot_lcd_blit(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  const uint16_t *rgb565)
{
    if (!g_ready || rgb565 == NULL || x0 > x1 || y0 > y1)
    {
        return -1;
    }

    boot_lcd_set_region(x0, y0, x1, y1);
    HAL_LCDC_LayerSetData(&g_lcdc, HAL_LCDC_LAYER_DEFAULT, (uint8_t *)rgb565,
                          x0, y0, x1, y1);
    HAL_LCDC_Next_Frame_TE(&g_lcdc, 0);
    if (HAL_LCDC_SendLayerData2Reg(&g_lcdc,
                                   ((0x32u << 24) | (REG_WRITE_RAM << 8)), 4) != HAL_OK)
    {
        return -1;
    }
    return 0;
}

void boot_lcd_backlight_on(void)
{
    GPT_OC_InitTypeDef oc_cfg;
    uint32_t timclk;
    uint64_t ticks;
    uint32_t prescaler;
    uint32_t period;
    uint32_t pulse;
    uint8_t pct = LCD_BL_ON_BRIGHTNESS_PCT;

    if (!g_ready)
    {
        return;
    }
    if (pct > LCD_BL_MAX_BRIGHTNESS_PCT)
    {
        pct = LCD_BL_MAX_BRIGHTNESS_PCT;
    }

    if (g_bl_pwm.Instance == NULL)
    {
        memset(&g_bl_pwm, 0, sizeof(g_bl_pwm));
        g_bl_pwm.Instance = GPTIM1;
        g_bl_pwm.core = CORE_ID_HCPU;
    }

    HAL_RCC_EnableModule(RCC_MOD_GPTIM1);
    timclk = 24000000u;
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
    pulse = (uint32_t)(((uint64_t)pct * (uint64_t)period) / 100ULL);
    if (pulse > period)
    {
        pulse = period;
    }

    if (g_bl_running)
    {
        HAL_GPT_PWM_Stop(&g_bl_pwm, LCD_BL_PWM_TIM_CHANNEL);
        g_bl_running = 0;
    }

    g_bl_pwm.Init.Prescaler = prescaler - 1;
    g_bl_pwm.Init.CounterMode = GPT_COUNTERMODE_UP;
    g_bl_pwm.Init.Period = period - 1;
    g_bl_pwm.Init.RepetitionCounter = 0;
    if (HAL_GPT_PWM_Init(&g_bl_pwm) != HAL_OK)
    {
        HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
        BSP_GPIO_Set(LCD_BL_PIN, 0, 1);
        return;
    }

    memset(&oc_cfg, 0, sizeof(oc_cfg));
    oc_cfg.OCMode = GPT_OCMODE_PWM1;
    oc_cfg.Pulse = pulse;
    oc_cfg.OCPolarity = GPT_OCPOLARITY_HIGH;
    oc_cfg.OCNPolarity = GPT_OCNPOLARITY_LOW;
    oc_cfg.OCFastMode = GPT_OCFAST_DISABLE;
    oc_cfg.OCIdleState = GPT_OCIDLESTATE_RESET;
    oc_cfg.OCNIdleState = GPT_OCNIDLESTATE_RESET;
    if (HAL_GPT_PWM_ConfigChannel(&g_bl_pwm, &oc_cfg, LCD_BL_PWM_TIM_CHANNEL) != HAL_OK)
    {
        HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
        BSP_GPIO_Set(LCD_BL_PIN, 0, 1);
        return;
    }

    HAL_PIN_Set(PAD_PA01, GPTIM1_CH4, PIN_NOPULL, 1);
    if (HAL_GPT_PWM_Start(&g_bl_pwm, LCD_BL_PWM_TIM_CHANNEL) != HAL_OK)
    {
        HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
        BSP_GPIO_Set(LCD_BL_PIN, 0, 1);
        return;
    }
    g_bl_running = 1;
}

void boot_lcd_backlight_off(void)
{
    if (g_bl_running)
    {
        HAL_GPT_PWM_Stop(&g_bl_pwm, LCD_BL_PWM_TIM_CHANNEL);
        g_bl_running = 0;
    }
    HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
    BSP_GPIO_Set(LCD_BL_PIN, 0, 1);
}
