/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/lcd_softspi.h
 *
 * GPIO bit-bang SPI for TK024F3036 (ST7789S) bring-up.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __LCD_SOFTSPI_H
#define __LCD_SOFTSPI_H

/* Full vendor init + RGB565 bar test (SPI mode 3, PA37-PA42). */

void tk024f3036_softspi_bringup(void);

#endif /* __LCD_SOFTSPI_H */
