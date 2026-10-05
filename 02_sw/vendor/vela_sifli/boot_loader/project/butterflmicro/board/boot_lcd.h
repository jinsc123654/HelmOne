/**
 * @file boot_lcd.h
 * @brief NV3031A 240x320 QAD-SPI for 2SFBL (polling LCDC, no NuttX driver).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_LCD_H
#define BOOT_LCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_LCD_WIDTH   240
#define BOOT_LCD_HEIGHT  320

/** Pinmux, panel power, reset, LCDC init. Backlight stays off until boot_lcd_backlight_on(). */
int boot_lcd_init(void);

int boot_lcd_ready(void);

/** RGB565 blit; inclusive x1/y1. */
int boot_lcd_blit(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  const uint16_t *rgb565);

/** GPTIM1 CH4 PWM on PA01. Duty kept low: no series resistor on the LED. */
void boot_lcd_backlight_on(void);

/** PA01 GPIO low. Charge UI stays off (transflective). */
void boot_lcd_backlight_off(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_LCD_H */
