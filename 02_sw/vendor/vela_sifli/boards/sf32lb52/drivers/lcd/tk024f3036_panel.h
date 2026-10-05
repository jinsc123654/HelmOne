/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/tk024f3036_panel.h
 *
 * TK024F3036 (ST7789S, 320x240) panel helpers
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __TK024F3036_PANEL_H
#define __TK024F3036_PANEL_H

#include <nuttx/lcd/lcd.h>
#include <nuttx/spi/spi.h>

/* Panel GRAM is 320x240; bicycle UI uses portrait 240x320 (hardcoded). */

#define TK024F3036_XSIZE_PHYS   320
#define TK024F3036_YSIZE_PHYS   240

#define TK024F3036_FB_HOR_RES   240
#define TK024F3036_FB_VER_RES   320

/* MADCTL 0x00 = portrait; 0x60 = MX+MV landscape (vendor LCD.c). */
#define TK024F3036_MADCTL       0x00

/* Vendor Lcd_Initialize() from TK024F3036 LCD.c. */

void tk024f3036_panel_init(FAR struct spi_dev_s *spi);

/* Re-apply COLMOD/MADCTL/INVON/DISPON after NuttX st7789 or fb flush. */

void tk024f3036_panel_apply_video_mode(FAR struct spi_dev_s *spi);

/* Boot-time RGB565 bars via vendor 8-bit SPI pixel order (Color>>8, Color). */

void tk024f3036_draw_rgb_bars_spi(FAR struct spi_dev_s *spi);

/* Boot-time RGB565 bar test via NuttX LCD putrun. */

void tk024f3036_draw_rgb_bars(FAR struct lcd_dev_s *lcd);

/* Boot-time RGB565 bar test via /dev/fb0 + FBIO_UPDATE (LVGL path). */

void tk024f3036_draw_rgb_bars_fb(FAR struct spi_dev_s *spi);

/* Clear full screen to black (fb path preferred when VIDEO_FB). */

void tk024f3036_clear_screen_spi(FAR struct spi_dev_s *spi);
void tk024f3036_clear_screen(FAR struct lcd_dev_s *lcd);
void tk024f3036_clear_screen_fb(FAR struct spi_dev_s *spi);

#endif /* __TK024F3036_PANEL_H */
