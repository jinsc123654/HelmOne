/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/tk024f3036_panel.c
 *
 * TK024F3036 helpers. Gamma/MADCTL from vendor LCD.c; RGB test on boot.
 *
 * 后续剔除：Helm One 已改 NV3031A，不再使用本文件。
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#include <sfconfig.h>

#ifdef CONFIG_LCD_USING_ST7789S

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/video/fb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <syslog.h>
#include <debug.h>

#include "drv_io.h"
#include "tk024f3036_panel.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void tk024f3036_spi_config(FAR struct spi_dev_s *spi)
{
  /* st7789_lcdinitialize() leaves the bus in 16-bit pixel mode; panel
   * register writes must use 8-bit command/data frames.
   */

  SPI_SETMODE(spi, CONFIG_LCD_ST7789_SPIMODE);
  SPI_SETBITS(spi, 8);
  SPI_SETFREQUENCY(spi, CONFIG_LCD_ST7789_FREQUENCY);
}

static void tk024f3036_write_cmd(FAR struct spi_dev_s *spi, uint8_t cmd)
{
  SPI_LOCK(spi, true);
  tk024f3036_spi_config(spi);
  SPI_SELECT(spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(spi, SPIDEV_DISPLAY(0), true);
  SPI_SEND(spi, cmd);
  SPI_SELECT(spi, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(spi, false);
}

static void tk024f3036_write_data(FAR struct spi_dev_s *spi, uint8_t data)
{
  SPI_LOCK(spi, true);
  tk024f3036_spi_config(spi);
  SPI_SELECT(spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(spi, SPIDEV_DISPLAY(0), false);
  SPI_SEND(spi, data);
  SPI_SELECT(spi, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(spi, false);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static void tk024f3036_write_power_gamma(FAR struct spi_dev_s *spi)
{
  tk024f3036_write_cmd(spi, 0xb2);
  tk024f3036_write_data(spi, 0x1f);
  tk024f3036_write_data(spi, 0x1f);
  tk024f3036_write_data(spi, 0x00);
  tk024f3036_write_data(spi, 0x33);
  tk024f3036_write_data(spi, 0x33);

  tk024f3036_write_cmd(spi, 0xb7);
  tk024f3036_write_data(spi, 0x12);

  tk024f3036_write_cmd(spi, 0xbb);
  tk024f3036_write_data(spi, 0x66);

  tk024f3036_write_cmd(spi, 0xc0);
  tk024f3036_write_data(spi, 0x2c);

  tk024f3036_write_cmd(spi, 0xc2);
  tk024f3036_write_data(spi, 0x01);

  tk024f3036_write_cmd(spi, 0xc3);
  tk024f3036_write_data(spi, 0x15);

  tk024f3036_write_cmd(spi, 0xc4);
  tk024f3036_write_data(spi, 0x20);

  tk024f3036_write_cmd(spi, 0xc6);
  tk024f3036_write_data(spi, 0x13);

  tk024f3036_write_cmd(spi, 0xd0);
  tk024f3036_write_data(spi, 0xa4);
  tk024f3036_write_data(spi, 0xa1);

  tk024f3036_write_cmd(spi, 0xd6);
  tk024f3036_write_data(spi, 0xa1);

  tk024f3036_write_cmd(spi, 0xe0);
  tk024f3036_write_data(spi, 0xf0);
  tk024f3036_write_data(spi, 0x06);
  tk024f3036_write_data(spi, 0x0d);
  tk024f3036_write_data(spi, 0x0b);
  tk024f3036_write_data(spi, 0x0a);
  tk024f3036_write_data(spi, 0x07);
  tk024f3036_write_data(spi, 0x2e);
  tk024f3036_write_data(spi, 0x43);
  tk024f3036_write_data(spi, 0x45);
  tk024f3036_write_data(spi, 0x38);
  tk024f3036_write_data(spi, 0x14);
  tk024f3036_write_data(spi, 0x13);
  tk024f3036_write_data(spi, 0x25);
  tk024f3036_write_data(spi, 0x29);

  tk024f3036_write_cmd(spi, 0xe1);
  tk024f3036_write_data(spi, 0xf0);
  tk024f3036_write_data(spi, 0x07);
  tk024f3036_write_data(spi, 0x0a);
  tk024f3036_write_data(spi, 0x08);
  tk024f3036_write_data(spi, 0x07);
  tk024f3036_write_data(spi, 0x23);
  tk024f3036_write_data(spi, 0x2e);
  tk024f3036_write_data(spi, 0x33);
  tk024f3036_write_data(spi, 0x44);
  tk024f3036_write_data(spi, 0x3a);
  tk024f3036_write_data(spi, 0x16);
  tk024f3036_write_data(spi, 0x17);
  tk024f3036_write_data(spi, 0x26);
  tk024f3036_write_data(spi, 0x2c);
}

static void tk024f3036_block_write(FAR struct spi_dev_s *spi,
                                   uint16_t xstart, uint16_t xend,
                                   uint16_t ystart, uint16_t yend)
{
  tk024f3036_write_cmd(spi, 0x2a);
  tk024f3036_write_data(spi, (uint8_t)(xstart >> 8));
  tk024f3036_write_data(spi, (uint8_t)(xstart & 0xff));
  tk024f3036_write_data(spi, (uint8_t)(xend >> 8));
  tk024f3036_write_data(spi, (uint8_t)(xend & 0xff));

  tk024f3036_write_cmd(spi, 0x2b);
  tk024f3036_write_data(spi, (uint8_t)(ystart >> 8));
  tk024f3036_write_data(spi, (uint8_t)(ystart & 0xff));
  tk024f3036_write_data(spi, (uint8_t)(yend >> 8));
  tk024f3036_write_data(spi, (uint8_t)(yend & 0xff));

  tk024f3036_write_cmd(spi, 0x2c);
}

static void tk024f3036_fill_rgb565(FAR struct spi_dev_s *spi,
                                   uint16_t xstart, uint16_t ystart,
                                   uint16_t xlong, uint16_t ylong,
                                   uint16_t color)
{
  static uint8_t pixbuf[512];
  uint32_t pixels = (uint32_t)xlong * (uint32_t)ylong;
  uint32_t sent = 0;
  uint8_t hi = (uint8_t)(color >> 8);
  uint8_t lo = (uint8_t)(color & 0xff);

  tk024f3036_block_write(spi, xstart, (uint16_t)(xstart + xlong - 1),
                         ystart, (uint16_t)(ystart + ylong - 1));

  SPI_LOCK(spi, true);
  tk024f3036_spi_config(spi);
  SPI_SELECT(spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(spi, SPIDEV_DISPLAY(0), false);

  while (sent < pixels)
    {
      uint32_t chunk = pixels - sent;
      uint32_t i;

      if (chunk > (sizeof(pixbuf) / 2))
        {
          chunk = sizeof(pixbuf) / 2;
        }

      for (i = 0; i < chunk; i++)
        {
          pixbuf[i * 2]     = hi;
          pixbuf[i * 2 + 1] = lo;
        }

      SPI_SNDBLOCK(spi, pixbuf, chunk * 2);
      sent += chunk;
    }

  SPI_SELECT(spi, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(spi, false);
}

void tk024f3036_panel_init(FAR struct spi_dev_s *spi)
{
  tk024f3036_write_cmd(spi, 0x11);
  usleep(120 * 1000);

  tk024f3036_write_cmd(spi, 0x36);
  tk024f3036_write_data(spi, 0x00);

  tk024f3036_write_cmd(spi, 0x3a);
  tk024f3036_write_data(spi, 0x06);

  tk024f3036_write_power_gamma(spi);

  tk024f3036_write_cmd(spi, 0x3a);
  tk024f3036_write_data(spi, 0x05);

  tk024f3036_write_cmd(spi, 0x36);
  tk024f3036_write_data(spi, TK024F3036_MADCTL);

  tk024f3036_write_cmd(spi, 0x20);
  tk024f3036_write_cmd(spi, 0x29);
  usleep(100 * 1000);
}

void tk024f3036_panel_apply_video_mode(FAR struct spi_dev_s *spi)
{
  tk024f3036_write_cmd(spi, 0x3a);
  tk024f3036_write_data(spi, 0x05);

  tk024f3036_write_cmd(spi, 0x36);
  tk024f3036_write_data(spi, TK024F3036_MADCTL);

  tk024f3036_write_cmd(spi, 0x20);
  tk024f3036_write_cmd(spi, 0x29);
}

void tk024f3036_clear_screen_spi(FAR struct spi_dev_s *spi)
{
  tk024f3036_fill_rgb565(spi, 0, 0, TK024F3036_FB_HOR_RES, TK024F3036_FB_VER_RES,
                         0x0000);
}

void tk024f3036_clear_screen(FAR struct lcd_dev_s *lcd)
{
  struct fb_videoinfo_s vinfo;
  struct lcd_planeinfo_s pinfo;
  uint16_t runbuf[TK024F3036_FB_HOR_RES];
  int y;

  if (lcd == NULL)
    {
      return;
    }

  if (lcd->getvideoinfo(lcd, &vinfo) < 0 ||
      lcd->getplaneinfo(lcd, 0, &pinfo) < 0 ||
      pinfo.putrun == NULL)
    {
      return;
    }

  for (y = 0; y < vinfo.yres; y++)
    {
      int i;

      for (i = 0; i < vinfo.xres; i++)
        {
          runbuf[i] = 0x0000;
        }

      pinfo.putrun(lcd, y, 0, (FAR uint8_t *)runbuf, vinfo.xres);
    }
}

void tk024f3036_clear_screen_fb(FAR struct spi_dev_s *spi)
{
  int fd;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  struct fb_area_s area;
  int y;

  fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    {
      return;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)(uintptr_t)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)(uintptr_t)&pinfo) < 0 ||
      pinfo.fbmem == NULL || pinfo.bpp != 16)
    {
      close(fd);
      return;
    }

  for (y = 0; y < vinfo.yres; y++)
    {
      FAR uint16_t *row =
        (FAR uint16_t *)((FAR uint8_t *)pinfo.fbmem + y * pinfo.stride);
      int x;

      for (x = 0; x < vinfo.xres; x++)
        {
          row[x] = 0x0000;
        }
    }

#ifdef CONFIG_FB_UPDATE
  tk024f3036_panel_apply_video_mode(spi);

#ifdef CONFIG_ARCH_DCACHE
  up_clean_dcache((uintptr_t)pinfo.fbmem,
                  (uintptr_t)pinfo.fbmem + (size_t)pinfo.stride * vinfo.yres);
#endif

  area.x = 0;
  area.y = 0;
  area.w = vinfo.xres;
  area.h = vinfo.yres;
  (void)ioctl(fd, FBIO_UPDATE, (unsigned long)(uintptr_t)&area);
#endif

  close(fd);
}

void tk024f3036_draw_rgb_bars_spi(FAR struct spi_dev_s *spi)
{
  int bar_w = TK024F3036_FB_HOR_RES / 3;
  int rem = TK024F3036_FB_HOR_RES - (2 * bar_w);

  syslog(LOG_INFO, "INFO: TK024F3036 SPI RGB test %dx%d\n",
         TK024F3036_FB_HOR_RES, TK024F3036_FB_VER_RES);

  tk024f3036_fill_rgb565(spi, 0, 0, bar_w, TK024F3036_FB_VER_RES, 0xf800);
  tk024f3036_fill_rgb565(spi, bar_w, 0, bar_w, TK024F3036_FB_VER_RES, 0x07e0);
  tk024f3036_fill_rgb565(spi, (uint16_t)(2 * bar_w), 0, rem,
                         TK024F3036_FB_VER_RES, 0x001f);
}

void tk024f3036_draw_rgb_bars(FAR struct lcd_dev_s *lcd)
{
  struct fb_videoinfo_s vinfo;
  struct lcd_planeinfo_s pinfo;
  uint16_t runbuf[TK024F3036_FB_HOR_RES];
  int bar_w;
  int y;

  if (lcd == NULL)
    {
      return;
    }

  if (lcd->getvideoinfo(lcd, &vinfo) < 0 ||
      lcd->getplaneinfo(lcd, 0, &pinfo) < 0 ||
      pinfo.putrun == NULL)
    {
      lcderr("ERROR: RGB test: LCD plane info unavailable\n");
      return;
    }

  bar_w = vinfo.xres / 3;
  if (bar_w < 1)
    {
      return;
    }

  lcdinfo("RGB bar test %dx%d\n", vinfo.xres, vinfo.yres);

  for (y = 0; y < vinfo.yres; y++)
    {
      int i;
      int rem;

      for (i = 0; i < bar_w; i++)
        {
          runbuf[i] = 0xf800;
        }

      pinfo.putrun(lcd, y, 0, (FAR uint8_t *)runbuf, bar_w);

      for (i = 0; i < bar_w; i++)
        {
          runbuf[i] = 0x07e0;
        }

      pinfo.putrun(lcd, y, bar_w, (FAR uint8_t *)runbuf, bar_w);

      rem = vinfo.xres - (2 * bar_w);
      for (i = 0; i < rem; i++)
        {
          runbuf[i] = 0x001f;
        }

      pinfo.putrun(lcd, y, 2 * bar_w, (FAR uint8_t *)runbuf, rem);
    }
}

void tk024f3036_draw_rgb_bars_fb(FAR struct spi_dev_s *spi)
{
  int fd;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  struct fb_area_s area;
  int bar_w;
  int y;

  fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    {
      syslog(LOG_ERR, "ERROR: RGB fb test: open /dev/fb0 failed: %d\n", errno);
      return;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)(uintptr_t)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)(uintptr_t)&pinfo) < 0 ||
      pinfo.fbmem == NULL)
    {
      syslog(LOG_ERR, "ERROR: RGB fb test: FBIOGET_* failed\n");
      close(fd);
      return;
    }

  if (pinfo.bpp != 16)
    {
      syslog(LOG_ERR, "ERROR: RGB fb test: expected 16bpp, got %u\n",
             pinfo.bpp);
      close(fd);
      return;
    }

  bar_w = vinfo.xres / 3;
  if (bar_w < 1)
    {
      close(fd);
      return;
    }

  syslog(LOG_INFO, "INFO: RGB bar fb test %dx%d stride=%d\n",
         vinfo.xres, vinfo.yres, pinfo.stride);

  for (y = 0; y < vinfo.yres; y++)
    {
      FAR uint16_t *row =
        (FAR uint16_t *)((FAR uint8_t *)pinfo.fbmem + y * pinfo.stride);
      int x;

      for (x = 0; x < bar_w; x++)
        {
          row[x] = 0xf800;
        }

      for (x = bar_w; x < (2 * bar_w); x++)
        {
          row[x] = 0x07e0;
        }

      for (x = (2 * bar_w); x < vinfo.xres; x++)
        {
          row[x] = 0x001f;
        }
    }

#ifdef CONFIG_FB_UPDATE
  tk024f3036_panel_apply_video_mode(spi);

#ifdef CONFIG_ARCH_DCACHE
  up_clean_dcache((uintptr_t)pinfo.fbmem,
                  (uintptr_t)pinfo.fbmem + (size_t)pinfo.stride * vinfo.yres);
#endif

  area.x = 0;
  area.y = 0;
  area.w = vinfo.xres;
  area.h = vinfo.yres;

  syslog(LOG_INFO, "INFO: RGB fb FBIO_UPDATE %ux%u\n", area.w, area.h);

  if (ioctl(fd, FBIO_UPDATE, (unsigned long)(uintptr_t)&area) < 0)
    {
      syslog(LOG_ERR, "ERROR: RGB fb test: FBIO_UPDATE failed: %d\n", errno);
    }

  syslog(LOG_INFO, "INFO: RGB bar fb test done\n");
#else
  syslog(LOG_WARNING, "WARN: CONFIG_FB_UPDATE disabled, fb not flushed\n");
#endif

  close(fd);
}

#endif /* CONFIG_LCD_USING_ST7789S */
