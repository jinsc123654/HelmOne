/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/lcd_softspi.c
 *
 * GPIO bit-bang SPI for TK024F3036 (ST7789S) timing bring-up.
 * Matches vendor STM32 HAL SPI mode 3 (CPOL=1, CPHA=2nd edge).
 *
 * 后续剔除：Helm One 已改 NV3031A，不再使用本文件。
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#include <sfconfig.h>

#ifdef CONFIG_LCD_USING_ST7789S
#ifdef CONFIG_LCD_ST7789S_SOFTSPI

#include <nuttx/config.h>

#include <stdint.h>
#include <unistd.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "bf0_hal.h"
#include "drv_io.h"
#include "lcd_softspi.h"
#include "tk024f3036_panel.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LCD_MOSI_GPIO           37
#define LCD_SCK_GPIO            39
#define LCD_CS_GPIO             40

/* Bit-bang half-period; increase if the panel misses pixels. */

#ifndef CONFIG_LCD_ST7789S_SOFTSPI_HALF_US
#  define CONFIG_LCD_ST7789S_SOFTSPI_HALF_US 2
#endif

#define LCD_SOFTSPI_HALF_US     CONFIG_LCD_ST7789S_SOFTSPI_HALF_US

/****************************************************************************
 * Private Data
 ****************************************************************************/

static GPIO_TypeDef *g_lcd_gpio = hwp_gpio1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline void lcd_softspi_pin_set(int pin, int val)
{
  HAL_GPIO_WritePin(g_lcd_gpio, pin, (GPIO_PinState)val);
}

static inline void lcd_softspi_half_period(void)
{
  up_udelay(LCD_SOFTSPI_HALF_US);
}

static void lcd_softspi_write_byte(uint8_t byte)
{
  int i;

  for (i = 7; i >= 0; i--)
    {
      lcd_softspi_pin_set(LCD_MOSI_GPIO, (byte >> i) & 1);
      lcd_softspi_half_period();
      lcd_softspi_pin_set(LCD_SCK_GPIO, 0);
      lcd_softspi_half_period();
      lcd_softspi_pin_set(LCD_SCK_GPIO, 1);
      lcd_softspi_half_period();
    }
}

static void lcd_softspi_write_cmd(uint8_t cmd)
{
  lcd_softspi_pin_set(LCD_CS_GPIO, 0);
  BSP_LCD_DC_Set(1);
  lcd_softspi_write_byte(cmd);
}

static void lcd_softspi_write_data(uint8_t data)
{
  BSP_LCD_DC_Set(0);
  lcd_softspi_pin_set(LCD_CS_GPIO, 0);
  lcd_softspi_write_byte(data);
}

static void lcd_softspi_block_write(uint16_t xstart, uint16_t xend,
                                    uint16_t ystart, uint16_t yend)
{
  lcd_softspi_write_cmd(0x2a);
  lcd_softspi_write_data(xstart >> 8);
  lcd_softspi_write_data(xstart & 0xff);
  lcd_softspi_write_data(xend >> 8);
  lcd_softspi_write_data(xend & 0xff);

  lcd_softspi_write_cmd(0x2b);
  lcd_softspi_write_data(ystart >> 8);
  lcd_softspi_write_data(ystart & 0xff);
  lcd_softspi_write_data(yend >> 8);
  lcd_softspi_write_data(yend & 0xff);

  lcd_softspi_write_cmd(0x2c);
}

static void lcd_softspi_fill_rgb565(uint16_t xstart, uint16_t ystart,
                                    uint16_t xlong, uint16_t ylong,
                                    uint16_t color)
{
  uint32_t pixels = (uint32_t)xlong * (uint32_t)ylong;
  uint32_t i;
  uint8_t hi = (uint8_t)(color >> 8);
  uint8_t lo = (uint8_t)(color & 0xff);

  lcd_softspi_block_write(xstart, xstart + xlong - 1,
                          ystart, ystart + ylong - 1);

  BSP_LCD_DC_Set(0);
  lcd_softspi_pin_set(LCD_CS_GPIO, 0);

  for (i = 0; i < pixels; i++)
    {
      lcd_softspi_write_byte(hi);
      lcd_softspi_write_byte(lo);
    }

  lcd_softspi_pin_set(LCD_CS_GPIO, 1);
}

static void lcd_softspi_panel_init(void)
{
  /* Vendor Lcd_Initialize() from TK024F3036 LCD.c */

  lcd_softspi_write_cmd(0x11);
  usleep(120 * 1000);

  lcd_softspi_write_cmd(0x36);
  lcd_softspi_write_data(0x00);

  lcd_softspi_write_cmd(0x3a);
  lcd_softspi_write_data(0x06);

  lcd_softspi_write_cmd(0xb2);
  lcd_softspi_write_data(0x1f);
  lcd_softspi_write_data(0x1f);
  lcd_softspi_write_data(0x00);
  lcd_softspi_write_data(0x33);
  lcd_softspi_write_data(0x33);

  lcd_softspi_write_cmd(0xb7);
  lcd_softspi_write_data(0x12);

  lcd_softspi_write_cmd(0xbb);
  lcd_softspi_write_data(0x66);

  lcd_softspi_write_cmd(0xc0);
  lcd_softspi_write_data(0x2c);

  lcd_softspi_write_cmd(0xc2);
  lcd_softspi_write_data(0x01);

  lcd_softspi_write_cmd(0xc3);
  lcd_softspi_write_data(0x15);

  lcd_softspi_write_cmd(0xc4);
  lcd_softspi_write_data(0x20);

  lcd_softspi_write_cmd(0xc6);
  lcd_softspi_write_data(0x13);

  lcd_softspi_write_cmd(0xd0);
  lcd_softspi_write_data(0xa4);
  lcd_softspi_write_data(0xa1);

  lcd_softspi_write_cmd(0xd6);
  lcd_softspi_write_data(0xa1);

  lcd_softspi_write_cmd(0xe0);
  lcd_softspi_write_data(0xf0);
  lcd_softspi_write_data(0x06);
  lcd_softspi_write_data(0x0d);
  lcd_softspi_write_data(0x0b);
  lcd_softspi_write_data(0x0a);
  lcd_softspi_write_data(0x07);
  lcd_softspi_write_data(0x2e);
  lcd_softspi_write_data(0x43);
  lcd_softspi_write_data(0x45);
  lcd_softspi_write_data(0x38);
  lcd_softspi_write_data(0x14);
  lcd_softspi_write_data(0x13);
  lcd_softspi_write_data(0x25);
  lcd_softspi_write_data(0x29);

  lcd_softspi_write_cmd(0xe1);
  lcd_softspi_write_data(0xf0);
  lcd_softspi_write_data(0x07);
  lcd_softspi_write_data(0x0a);
  lcd_softspi_write_data(0x08);
  lcd_softspi_write_data(0x07);
  lcd_softspi_write_data(0x23);
  lcd_softspi_write_data(0x2e);
  lcd_softspi_write_data(0x33);
  lcd_softspi_write_data(0x44);
  lcd_softspi_write_data(0x3a);
  lcd_softspi_write_data(0x16);
  lcd_softspi_write_data(0x17);
  lcd_softspi_write_data(0x26);
  lcd_softspi_write_data(0x2c);

  lcd_softspi_write_cmd(0x3a);
  lcd_softspi_write_data(0x05);

  lcd_softspi_write_cmd(0x36);
  lcd_softspi_write_data(TK024F3036_MADCTL);

  lcd_softspi_write_cmd(0x20);
  lcd_softspi_write_cmd(0x29);
}

static void lcd_softspi_draw_rgb_bars(void)
{
  int bar_w = TK024F3036_FB_HOR_RES / 3;
  int rem = TK024F3036_FB_HOR_RES - (2 * bar_w);

  syslog(LOG_INFO, "INFO: soft-SPI RGB test %dx%d\n",
         TK024F3036_FB_HOR_RES, TK024F3036_FB_VER_RES);

  lcd_softspi_fill_rgb565(0, 0, bar_w, TK024F3036_FB_VER_RES, 0xf800);
  lcd_softspi_fill_rgb565(bar_w, 0, bar_w, TK024F3036_FB_VER_RES, 0x07e0);
  lcd_softspi_fill_rgb565(2 * bar_w, 0, rem, TK024F3036_FB_VER_RES, 0x001f);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void tk024f3036_softspi_bringup(void)
{
  syslog(LOG_INFO, "INFO: ST7789S soft-SPI bringup start (half=%d us)\n",
         LCD_SOFTSPI_HALF_US);

  /* Idle SPI mode 3: SCK high, CS deasserted. */

  lcd_softspi_pin_set(LCD_SCK_GPIO, 1);
  lcd_softspi_pin_set(LCD_CS_GPIO, 1);
  lcd_softspi_pin_set(LCD_MOSI_GPIO, 0);
  BSP_LCD_BL_Set(0);
  BSP_LCD_DC_Set(0);

  /* Vendor reset pulse: CS high -> low, then wait. */

  lcd_softspi_pin_set(LCD_CS_GPIO, 1);
  usleep(500);
  lcd_softspi_pin_set(LCD_CS_GPIO, 0);
  usleep(100 * 1000);

  lcd_softspi_panel_init();
#ifdef CONFIG_LCD_ST7789S_RGB_BOOT_TEST
  lcd_softspi_draw_rgb_bars();
#else
  {
    int bar_w = TK024F3036_FB_HOR_RES;

    lcd_softspi_fill_rgb565(0, 0, bar_w, TK024F3036_FB_VER_RES, 0x0000);
  }
#endif
  BSP_LCD_BL_Set(1);

  syslog(LOG_INFO, "INFO: ST7789S soft-SPI bringup done\n");
}

#endif /* CONFIG_LCD_ST7789S_SOFTSPI */
#endif /* CONFIG_LCD_USING_ST7789S */
