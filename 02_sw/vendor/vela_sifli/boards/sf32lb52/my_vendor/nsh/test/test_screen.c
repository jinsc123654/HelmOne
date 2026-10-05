/**
 * @file test_screen.c
 * @brief test screen：经 /dev/lcd0 刷 RGB 色条或测 FPS。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>

#include "test_demos.h"

#define LOGI(fmt, ...)   printf("test screen: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)   printf("test screen: ERROR " fmt "\n", ##__VA_ARGS__)

#define SCREEN_DEFAULT_SECONDS  5
#define SCREEN_LCDDEV           "/dev/lcd0"
#define SCREEN_OPEN_WAIT_SEC    15
#define SCREEN_HOLD_US          1000000ull

#ifndef CONFIG_LCD_MAXPOWER
#  define CONFIG_LCD_MAXPOWER 100
#endif

#ifdef CONFIG_LCD_DEV

static const uint16_t g_palette[] =
{
  0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0xffe0, 0x07ff, 0xf81f
};

static const char *const g_palette_name[] =
{
  "red", "green", "blue", "white", "black", "yellow", "cyan", "magenta"
};

#define SCREEN_NPALETTE (sizeof(g_palette) / sizeof(g_palette[0]))

/** @brief 读取 CLOCK_MONOTONIC 微秒时间戳。 */
static uint64_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/** @brief 用单色填充 RGB565 缓冲。 */
static void fill_rgb565(FAR uint16_t *buf, size_t npixels, uint16_t color)
{
  size_t i;

  for (i = 0; i < npixels; i++)
    {
      buf[i] = color;
    }
}

static int open_lcd0_wait(void)
{
  int fd;
  int waited;

  for (waited = 0; waited <= SCREEN_OPEN_WAIT_SEC; waited++)
    {
      fd = open(SCREEN_LCDDEV, O_RDWR);
      if (fd >= 0)
        {
          return fd;
        }

      if (waited == 0)
        {
          LOGI("waiting for %s (lcd_init is async)", SCREEN_LCDDEV);
        }

      sleep(1);
    }

  return -1;
}

/** @brief 打印 test screen 用法。 */
static void screen_usage(void)
{
  printf("Usage:\n");
  printf("  test screen [seconds]      hold each color ~1s (default %d)\n",
         SCREEN_DEFAULT_SECONDS);
  printf("  test screen fps [seconds]  hammer PUTAREA, print fps\n");
}

/**
 * @brief test screen 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_screen_main(int argc, FAR char *argv[])
{
  struct fb_videoinfo_s vinfo;
  struct lcd_planeinfo_s pinfo;
  struct lcddev_area_s area;
  FAR uint16_t *buf = NULL;
  int seconds = SCREEN_DEFAULT_SECONDS;
  int fps_mode = 0;
  uint64_t t_start;
  uint64_t t_end;
  uint64_t t_next_log;
  uint64_t elapsed_us;
  uint64_t put_sum_us = 0;
  uint64_t fill_sum_us = 0;
  uint64_t min_put_us = UINT64_MAX;
  uint64_t max_put_us = 0;
  unsigned long frames = 0;
  size_t npixels;
  size_t frame_bytes;
  int fd;
  int power;
  int argi;

  for (argi = 1; argi < argc; argi++)
    {
      if (strcmp(argv[argi], "fps") == 0 || strcmp(argv[argi], "-f") == 0)
        {
          fps_mode = 1;
        }
      else if (strcmp(argv[argi], "-h") == 0 ||
               strcmp(argv[argi], "help") == 0)
        {
          screen_usage();
          return EXIT_SUCCESS;
        }
      else
        {
          seconds = atoi(argv[argi]);
          if (seconds <= 0)
            {
              seconds = SCREEN_DEFAULT_SECONDS;
            }
        }
    }

  fd = open_lcd0_wait();
  if (fd < 0)
    {
      LOGE("open(%s) failed: %d", SCREEN_LCDDEV, errno);
      return EXIT_FAILURE;
    }

  memset(&vinfo, 0, sizeof(vinfo));
  memset(&pinfo, 0, sizeof(pinfo));

  if (ioctl(fd, LCDDEVIO_GETVIDEOINFO, (unsigned long)(uintptr_t)&vinfo) < 0 ||
      ioctl(fd, LCDDEVIO_GETPLANEINFO, (unsigned long)(uintptr_t)&pinfo) < 0)
    {
      LOGE("GETVIDEOINFO/GETPLANEINFO failed: %d", errno);
      close(fd);
      return EXIT_FAILURE;
    }

  if (vinfo.xres == 0 || vinfo.yres == 0)
    {
      LOGE("bad resolution %ux%u", vinfo.xres, vinfo.yres);
      close(fd);
      return EXIT_FAILURE;
    }

  if (pinfo.bpp != 16)
    {
      LOGE("expected 16bpp, got %u", pinfo.bpp);
      close(fd);
      return EXIT_FAILURE;
    }

  power = CONFIG_LCD_MAXPOWER;
  if (ioctl(fd, LCDDEVIO_SETPOWER, (unsigned long)power) < 0)
    {
      LOGI("SETPOWER ignored: %d", errno);
    }

  npixels = (size_t)vinfo.xres * (size_t)vinfo.yres;
  frame_bytes = npixels * sizeof(uint16_t);
  buf = (FAR uint16_t *)malloc(frame_bytes);
  if (buf == NULL)
    {
      LOGE("malloc %zu failed", frame_bytes);
      close(fd);
      return EXIT_FAILURE;
    }

  LOGI("start: %s %ux%u bpp=%u bytes=%zu dur=%ds mode=%s",
       SCREEN_LCDDEV, vinfo.xres, vinfo.yres, pinfo.bpp,
       frame_bytes, seconds, fps_mode ? "fps" : "hold-1s");
  if (!fps_mode)
    {
      LOGI("each color stays ~1s; bicycle UI may paint over it");
    }

  area.row_start = 0;
  area.row_end = (fb_coord_t)(vinfo.yres - 1);
  area.col_start = 0;
  area.col_end = (fb_coord_t)(vinfo.xres - 1);
  area.stride = (fb_coord_t)(vinfo.xres * 2);
  area.data = (FAR uint8_t *)buf;

  t_start = now_us();
  t_end = t_start + (uint64_t)seconds * 1000000ull;
  t_next_log = t_start + 1000000ull;

  for (; ; )
    {
      uint64_t t_fill0;
      uint64_t t_put0;
      uint64_t t_put1;
      uint64_t put_us;
      uint64_t now;
      unsigned pal = (unsigned)(frames % SCREEN_NPALETTE);

      t_fill0 = now_us();
      fill_rgb565(buf, npixels, g_palette[pal]);
      fill_sum_us += now_us() - t_fill0;

      t_put0 = now_us();
      if (ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area) < 0)
        {
          LOGE("PUTAREA failed: %d", errno);
          free(buf);
          close(fd);
          return EXIT_FAILURE;
        }

      t_put1 = now_us();
      put_us = t_put1 - t_put0;
      put_sum_us += put_us;
      if (put_us < min_put_us)
        {
          min_put_us = put_us;
        }

      if (put_us > max_put_us)
        {
          max_put_us = put_us;
        }

      frames++;

      if (!fps_mode)
        {
          uint64_t shown = now_us() - t_fill0;

          LOGI("t=%lu/%ds  %s  put=%lu.%03lu ms",
               (unsigned long)((now_us() - t_start) / 1000000ull + 1ull),
               seconds,
               g_palette_name[pal],
               (unsigned long)(put_us / 1000ull),
               (unsigned long)(put_us % 1000ull));

          if (shown < SCREEN_HOLD_US)
            {
              usleep((useconds_t)(SCREEN_HOLD_US - shown));
            }
        }
      else if (t_put1 >= t_next_log)
        {
          LOGI("t=%lu.%01lus frames=%lu color=%s",
               (unsigned long)((t_put1 - t_start) / 1000000ull),
               (unsigned long)(((t_put1 - t_start) / 100000ull) % 10ull),
               frames,
               g_palette_name[pal]);
          t_next_log += 1000000ull;
        }

      now = now_us();
      if (now >= t_end)
        {
          break;
        }
    }

  elapsed_us = now_us() - t_start;
  free(buf);
  close(fd);

  if (frames == 0 || elapsed_us == 0)
    {
      LOGE("no frames completed");
      return EXIT_FAILURE;
    }

  {
    unsigned long fps_x100 =
      (unsigned long)((uint64_t)frames * 100000000ull / elapsed_us);
    unsigned long loop_avg_us = (unsigned long)(elapsed_us / frames);
    unsigned long put_avg_us = (unsigned long)(put_sum_us / frames);
    unsigned long fill_avg_us = (unsigned long)(fill_sum_us / frames);
    unsigned long kibps =
      (unsigned long)((uint64_t)frames * frame_bytes * 1000000ull /
                      elapsed_us / 1024ull);

    LOGI("==== RESULT ====");
    LOGI("mode        : %s", fps_mode ? "fps" : "hold-1s");
    LOGI("frames      : %lu", frames);
    LOGI("elapsed     : %lu.%03lu s  (CLOCK_MONOTONIC)",
         (unsigned long)(elapsed_us / 1000000ull),
         (unsigned long)((elapsed_us / 1000ull) % 1000ull));
    LOGI("fps         : %lu.%02lu", fps_x100 / 100, fps_x100 % 100);
    LOGI("loop avg    : %lu.%03lu ms",
         loop_avg_us / 1000, loop_avg_us % 1000);
    LOGI("fill avg    : %lu.%03lu ms",
         fill_avg_us / 1000, fill_avg_us % 1000);
    LOGI("put avg     : %lu.%03lu ms",
         put_avg_us / 1000, put_avg_us % 1000);
    LOGI("put min     : %lu.%03lu ms",
         (unsigned long)(min_put_us / 1000), (unsigned long)(min_put_us % 1000));
    LOGI("put max     : %lu.%03lu ms",
         (unsigned long)(max_put_us / 1000), (unsigned long)(max_put_us % 1000));
    LOGI("throughput  : %lu KiB/s", kibps);
  }

  return EXIT_SUCCESS;
}

#else /* CONFIG_LCD_DEV */

int test_screen_main(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);
  printf("test screen: requires CONFIG_LCD_DEV (/dev/lcd0)\n");
  return EXIT_FAILURE;
}

#endif /* CONFIG_LCD_DEV */
