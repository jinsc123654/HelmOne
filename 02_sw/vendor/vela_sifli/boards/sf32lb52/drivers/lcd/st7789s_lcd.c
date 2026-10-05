/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/st7789s_lcd.c
 *
 * ST7789S SPI LCD on SPI2 (PA37-PA42) via dedicated lcd_spi2 (not sifli_spi).
 * Portrait 240x320 is hardcoded here (TK024F3036 + bicycle UI); Kconfig
 * LCD_LANDSCAPE is ignored.
 *
 * 后续剔除：Helm One 已改 NV3031A，不再使用本文件。
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#include <sfconfig.h>

#ifdef CONFIG_LCD_USING_ST7789S

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/lcd/st7789.h>
#include <nuttx/video/fb.h>

#include <spawn.h>
#include "drv_io.h"
#include "lcd_spi2.h"
#include "tk024f3036_panel.h"

/* Vendor ioctl: myvendor_lcd_disp recover (userspace flush pipe). */
#ifndef MYVENDOR_LCDDEVIO_ABORT_XFER
#  define MYVENDOR_LCDDEVIO_ABORT_XFER   _LCDIOC(32)
#endif
#ifndef MYVENDOR_LCDDEVIO_RESTART_XFER
#  define MYVENDOR_LCDDEVIO_RESTART_XFER _LCDIOC(33)
#endif
#include <syslog.h>
#ifdef CONFIG_LCD_ST7789S_SOFTSPI
#include "lcd_softspi.h"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct tk024f_lcd_wrap_s
{
  struct lcd_dev_s      dev;
  FAR struct lcd_dev_s *inner;
  CODE int (*inner_putarea)(FAR struct lcd_dev_s *dev, fb_coord_t row_start,
                            fb_coord_t row_end, fb_coord_t col_start,
                            fb_coord_t col_end, FAR const uint8_t *buffer,
                            fb_coord_t stride);
  CODE int (*inner_putrun)(FAR struct lcd_dev_s *dev, fb_coord_t row,
                           fb_coord_t col, FAR const uint8_t *buffer,
                           size_t npixels);
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct spi_dev_s *g_spidev;
static FAR struct lcd_dev_s *g_lcd;
static struct tk024f_lcd_wrap_s g_lcdwrap;
static bool g_lcdwrap_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct tk024f_lcd_wrap_s *tk024f_lcd_wrap_of(FAR struct lcd_dev_s *dev)
{
  return (FAR struct tk024f_lcd_wrap_s *)dev;
}

static void tk024f_lcd_apply_portrait_mode(void)
{
  if (g_spidev != NULL)
    {
      tk024f3036_panel_apply_video_mode(g_spidev);
    }
}

static int tk024f_lcd_wrap_putarea(FAR struct lcd_dev_s *dev,
                                   fb_coord_t row_start, fb_coord_t row_end,
                                   fb_coord_t col_start, fb_coord_t col_end,
                                   FAR const uint8_t *buffer, fb_coord_t stride)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  tk024f_lcd_apply_portrait_mode();

  if (wrap->inner_putarea == NULL)
    {
      return -ENOTSUP;
    }

  return wrap->inner_putarea(wrap->inner, row_start, row_end,
                             col_start, col_end, buffer, stride);
}

#ifndef CONFIG_LCD_NOGETRUN
static int tk024f_lcd_wrap_putrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                                  fb_coord_t col, FAR const uint8_t *buffer,
                                  size_t npixels)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  tk024f_lcd_apply_portrait_mode();

  if (wrap->inner_putrun == NULL)
    {
      return -ENOTSUP;
    }

  return wrap->inner_putrun(wrap->inner, row, col, buffer, npixels);
}
#endif

static int tk024f_lcd_wrap_getvideoinfo(FAR struct lcd_dev_s *dev,
                                        FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);
  int ret;

  ret = wrap->inner->getvideoinfo(wrap->inner, vinfo);
  if (ret < 0)
    {
      return ret;
    }

  vinfo->xres = TK024F3036_FB_HOR_RES;
  vinfo->yres = TK024F3036_FB_VER_RES;
  return OK;
}

static int tk024f_lcd_wrap_getplaneinfo(FAR struct lcd_dev_s *dev,
                                        unsigned int planeno,
                                        FAR struct lcd_planeinfo_s *pinfo)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);
  int ret;

  ret = wrap->inner->getplaneinfo(wrap->inner, planeno, pinfo);
  if (ret < 0)
    {
      return ret;
    }

  wrap->inner_putarea = pinfo->putarea;
  wrap->inner_putrun = pinfo->putrun;
  pinfo->putarea = tk024f_lcd_wrap_putarea;
#ifndef CONFIG_LCD_NOGETRUN
  pinfo->putrun = tk024f_lcd_wrap_putrun;
#endif
  pinfo->dev = &wrap->dev;
  return OK;
}

/* st7789_* handlers cast dev to st7789_dev_s; always forward with inner. */

static int tk024f_lcd_wrap_getpower(FAR struct lcd_dev_s *dev)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  return wrap->inner->getpower(wrap->inner);
}

static int tk024f_lcd_wrap_setpower(FAR struct lcd_dev_s *dev, int power)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  return wrap->inner->setpower(wrap->inner, power);
}

static int tk024f_lcd_wrap_getcontrast(FAR struct lcd_dev_s *dev)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  return wrap->inner->getcontrast(wrap->inner);
}

static int tk024f_lcd_wrap_setcontrast(FAR struct lcd_dev_s *dev,
                                       unsigned int contrast)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  return wrap->inner->setcontrast(wrap->inner, contrast);
}

static int tk024f_lcd_wrap_setframerate(FAR struct lcd_dev_s *dev, int rate)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  if (wrap->inner->setframerate == NULL)
    {
      return -ENOTSUP;
    }

  return wrap->inner->setframerate(wrap->inner, rate);
}

static int tk024f_lcd_wrap_getframerate(FAR struct lcd_dev_s *dev)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  if (wrap->inner->getframerate == NULL)
    {
      return -ENOTSUP;
    }

  return wrap->inner->getframerate(wrap->inner);
}

static int tk024f_lcd_wrap_getareaalign(FAR struct lcd_dev_s *dev,
                                        FAR struct lcddev_area_align_s *align)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  if (wrap->inner->getareaalign == NULL)
    {
      return -ENOTSUP;
    }

  return wrap->inner->getareaalign(wrap->inner, align);
}

static int tk024f_lcd_wrap_ioctl(FAR struct lcd_dev_s *dev, int cmd,
                                 unsigned long arg)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  if (cmd == MYVENDOR_LCDDEVIO_ABORT_XFER)
    {
      (void)arg;
      lcd_spi2_abort_active_xfer();
      return OK;
    }

  if (cmd == MYVENDOR_LCDDEVIO_RESTART_XFER)
    {
      (void)arg;
      lcd_spi2_restart_xfer_path();
      return OK;
    }

  if (wrap->inner->ioctl == NULL)
    {
      return -ENOTTY;
    }

  return wrap->inner->ioctl(wrap->inner, cmd, arg);
}

static int tk024f_lcd_wrap_open(FAR struct lcd_dev_s *dev)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  if (wrap->inner->open == NULL)
    {
      return OK;
    }

  return wrap->inner->open(wrap->inner);
}

static int tk024f_lcd_wrap_close(FAR struct lcd_dev_s *dev)
{
  FAR struct tk024f_lcd_wrap_s *wrap = tk024f_lcd_wrap_of(dev);

  if (wrap->inner->close == NULL)
    {
      return OK;
    }

  return wrap->inner->close(wrap->inner);
}

static void tk024f_lcd_wrap_bind(FAR struct lcd_dev_s *inner)
{
  FAR struct tk024f_lcd_wrap_s *wrap = &g_lcdwrap;

  memset(wrap, 0, sizeof(*wrap));
  wrap->inner = inner;
  wrap->dev.getvideoinfo = tk024f_lcd_wrap_getvideoinfo;
  wrap->dev.getplaneinfo = tk024f_lcd_wrap_getplaneinfo;
  wrap->dev.getpower     = tk024f_lcd_wrap_getpower;
  wrap->dev.setpower     = tk024f_lcd_wrap_setpower;
  wrap->dev.getcontrast  = tk024f_lcd_wrap_getcontrast;
  wrap->dev.setcontrast  = tk024f_lcd_wrap_setcontrast;
  wrap->dev.setframerate = tk024f_lcd_wrap_setframerate;
  wrap->dev.getframerate = tk024f_lcd_wrap_getframerate;
  wrap->dev.getareaalign = tk024f_lcd_wrap_getareaalign;
  wrap->dev.ioctl        = tk024f_lcd_wrap_ioctl;
  wrap->dev.open         = tk024f_lcd_wrap_open;
  wrap->dev.close        = tk024f_lcd_wrap_close;
  g_lcdwrap_ready = true;
}

static FAR struct lcd_dev_s *tk024f_lcd_public_dev(void)
{
  if (g_lcdwrap_ready)
    {
      return &g_lcdwrap.dev;
    }

  return g_lcd;
}

static int st7789s_register_devices(void)
{
  int ret;
  int retry;

#ifdef CONFIG_LCD_DEV
  syslog(LOG_INFO, "INFO: lcddev_register starting\n");
  for (retry = 0; retry < 30; retry++)
    {
      ret = lcddev_register(0);
      if (ret == OK || ret == -EEXIST)
        {
          syslog(LOG_INFO, "INFO: lcddev_register done\n");
          break;
        }

      if (ret != -ENOENT && ret != -ENODEV)
        {
          syslog(LOG_ERR, "ERROR: lcddev_register() failed: %d\n", ret);
          break;
        }

      usleep(100 * 1000);
    }
#endif

#ifdef CONFIG_VIDEO_FB
  syslog(LOG_INFO, "INFO: fb_register starting\n");
  for (retry = 0; retry < 30; retry++)
    {
      ret = fb_register(0, 0);
      if (ret == OK || ret == -EEXIST)
        {
          syslog(LOG_INFO, "INFO: fb_register done\n");
          break;
        }

      if (ret != -ENOENT && ret != -ENODEV)
        {
          syslog(LOG_ERR, "ERROR: fb_register() failed: %d\n", ret);
          break;
        }

      usleep(100 * 1000);
    }
#endif

  return OK;
}

static int st7789s_init_thread_entry(int argc, FAR char *argv[])
{
  int ret;

  syslog(LOG_INFO, "INFO: ST7789S hw-SPI init (mode %d, %d Hz) portrait %dx%d\n",
         CONFIG_LCD_ST7789_SPIMODE, CONFIG_LCD_ST7789_FREQUENCY,
         TK024F3036_FB_HOR_RES, TK024F3036_FB_VER_RES);

  tk024f3036_panel_init(g_spidev);

  g_lcd = st7789_lcdinitialize(g_spidev);
  if (g_lcd == NULL)
    {
      syslog(LOG_ERR, "ERROR: st7789_lcdinitialize() failed\n");
      return -ENODEV;
    }

  tk024f_lcd_apply_portrait_mode();
  tk024f_lcd_wrap_bind(g_lcd);

  ret = st7789s_register_devices();
  if (ret < 0)
    {
      return ret;
    }

  tk024f_lcd_apply_portrait_mode();

#ifdef CONFIG_LCD_ST7789S_RGB_BOOT_TEST
  syslog(LOG_INFO, "INFO: ST7789S RGB test start\n");

#  ifdef CONFIG_VIDEO_FB
  tk024f3036_draw_rgb_bars_fb(g_spidev);
#  else
  tk024f3036_draw_rgb_bars(tk024f_lcd_public_dev());
#  endif

  syslog(LOG_INFO, "INFO: ST7789S RGB test done\n");
#else
#  ifdef CONFIG_VIDEO_FB
  tk024f3036_clear_screen_fb(g_spidev);
#  else
  tk024f3036_clear_screen(tk024f_lcd_public_dev());
#  endif
#endif

  BSP_LCD_BL_Set(1);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_lcd_initialize(void)
{
  static bool initialized;

#ifdef CONFIG_LCD_ST7789S_SOFTSPI
  if (initialized)
    {
      return OK;
    }

  initialized = true;

  syslog(LOG_INFO, "INFO: board_lcd_initialize (ST7789S soft-SPI)\n");

  BSP_LCD_PowerUp();
  tk024f3036_softspi_bringup();

  return OK;
#else
  if (initialized)
    {
      return OK;
    }

  initialized = true;

  syslog(LOG_INFO, "INFO: board_lcd_initialize (ST7789S lcd_spi2)\n");

  BSP_LCD_PowerUp();
  usleep(100 * 1000);

  g_spidev = lcd_spi2_dev_initialize();
  if (g_spidev == NULL)
    {
      syslog(LOG_ERR, "ERROR: lcd_spi2_dev_initialize() failed\n");
      return -ENODEV;
    }

  {
    posix_spawnattr_t attr;
    FAR char *argv[] = { NULL };
    int pid;

    posix_spawnattr_init(&attr);
    attr.priority  = SCHED_PRIORITY_DEFAULT + 10;
    attr.stacksize = 8192;

    pid = task_spawn("lcd_init", st7789s_init_thread_entry, NULL, &attr,
                     argv, NULL);
    if (pid < 0)
      {
        syslog(LOG_ERR, "ERROR: lcd_init task_spawn failed: %d\n", pid);
        return pid;
      }
  }

  return OK;
#endif
}

struct lcd_dev_s *board_lcd_getdev(int devno)
{
#ifdef CONFIG_LCD_ST7789S_SOFTSPI
  (void)devno;
  return NULL;
#else
  if (g_lcd == NULL)
    {
      g_lcd = st7789_lcdinitialize(g_spidev);
      if (g_lcd == NULL)
        {
          lcderr("ERROR: Failed to bind lcd_spi2 to LCD %d\n", devno);
          return NULL;
        }

      tk024f_lcd_apply_portrait_mode();
      tk024f_lcd_wrap_bind(g_lcd);
    }

  return tk024f_lcd_public_dev();
#endif
}

void board_lcd_uninitialize(void)
{
  FAR struct lcd_dev_s *lcd = tk024f_lcd_public_dev();

  if (lcd != NULL && lcd->setpower != NULL)
    {
      lcd->setpower(lcd, 0);
    }

  BSP_LCD_PowerDown();
}

#endif /* CONFIG_LCD_USING_ST7789S */
