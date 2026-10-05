/**
 * @file test_main.c
 * @brief NSH 命令 test：板级自测 demo 统一分发入口。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_demos.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct test_demo_s
{
  FAR const char *name;
  FAR const char *help;
  CODE int (*entry)(int argc, FAR char *argv[]);
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct test_demo_s g_test_demos[] =
{
  { "sdio", "Layer 1 raw SDIO/MTD [read|write|seq|rnd|all] (see test sdio -h)",
    test_sdio_main },
  { "lfs",  "LFS /mnt/lfs RW speed (same as fat) [seq|chunk|all]",
    test_lfs_main },
  { "fat",  "FAT /mnt/fat RW speed (errors if RO) [seq|chunk|all]",
    test_fat_main },
  { "posix","Layer 3 POSIX/VFS [seq|chunk|rnd|mix|all] (alias of test sd)",
    test_sd_main },
  { "sd",   "Layer 3 POSIX/VFS stress [seq|chunk|rnd|mix|all] (see test sd -h)",
    test_sd_main },
  { "layers", "sdio read + lfs small + large 32 + mkdir",
    test_layers_main },
  { "diag", "emit INFO/WARN/ERR + list /mnt/kv/diag (verify log lands)",
    test_diag_main },
  { "led",  "LED/GPIO blink test",                         test_led_main  },
  { "iopin", "GPIO table toggle 2.5s (g_iopins[], never two HIGH)", test_iopin_main },
  { "ioread", "KEY1/KEY2 pull-up + PWR_KEY_READ pull-down loop", test_ioread_main },
  { "iowrite", "PWR_MD PA27 / GNSS_PWR PA43 [pwr|gnss|all] [0|1|loop]",
    test_iowrite_main },
  { "sound", "PA40 PWM piezo [name|all|4k|8k|hz n|duty pct|dc|sweep]", test_sound_main },
  { "eta", "ETA9184 [start|stop|read [N]|pwr 0|1]", test_eta_main },
  { "screen", "RGB bars on /dev/lcd0 [seconds]|fps [seconds]", test_screen_main },
  { "cpu",  "CPU monitor [-t sec] [-p pid]... | stop", test_cpu_main  },
  { "task", "CPU load worker [duty|busy|status|watch [s]|stop]", test_task_main },
  { "idle", "True vs ps idle %% once [wait_sec, default 5]", test_idle_main },
  { "time", "Uptime / clock sources [seconds, default one-shot]", test_time_main },
  { "tick", "Uptime log task [period_ms|stop|status]", test_tick_main },
  { "wdt",  "Watchdog probe [status|start [to_ms]|feed [to_ms] [period_ms]|stop]", test_wdt_main },
  { "rtc",  "Read/force-set RTC [year [mon] [day] [hour] [min] [sec]]", test_rtc_main },
  { "fault", "Fault inject for monitor/py [cases|bt|assert|nullwr|...]", test_fault_main },
#ifdef CONFIG_KVDB
  { "kv", "VELA KVDB property API (not LittleFS; see test kv -h)", test_kv_main },
#endif
#ifdef CONFIG_BOARD_BMP388
  { "bmp388", "BMP388 barometer (/dev/uorb/sensor_baro0) [count]", test_bmp388_main },
#endif
#ifdef CONFIG_BOARD_MMC5983MA
  { "mmc5983ma", "MMC5983MA magnetometer (/dev/uorb/sensor_mag0) [count]", test_mmc5983ma_main },
#endif
#ifdef CONFIG_BOARD_BMI270
  { "bmi270", "BMI270 accel/gyro (/dev/uorb/sensor_accel0,gyro0) [count]", test_bmi270_main },
#endif
#ifdef CONFIG_BOARD_L96_GNSS
  { "gnss", "MAX-M10S-00B-01 NMEA [count|raw [sec]|eph [file]|eph dump [file]]", test_gnss_main },
#endif
#ifdef CONFIG_BSP_USING_UART2
  { "uart2loop", "USART2 loopback PA31<->PA32 [count] [baud|listen]", test_uart2loop_main },
#endif
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  { "sensor", "BLE HR/CSC/CPS [scan|connect|read|disconnect|off]",
    test_sensor_main },
  { "notif",  "Phone 0xFF17 notify print [on|off]", test_notif_main },
  { "ctrl",   "Phone 0xFF13/0xFF19 print [on|off] (no UI nav)",
    test_ctrl_main },
#endif
};

#define TEST_NDEMOS (sizeof(g_test_demos) / sizeof(g_test_demos[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/** @brief 列出所有 test 子命令。 */
static void test_usage(void)
{
  size_t i;

  printf("Usage: test <demo> [demo-args...]\n");
  printf("Available demos:\n");
  for (i = 0; i < TEST_NDEMOS; i++)
    {
      printf("  %-8s %s\n", g_test_demos[i].name, g_test_demos[i].help);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief NSH 命令 test：板级自测 demo 统一分发入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, FAR char *argv[])
{
  size_t i;

  if (argc < 2)
    {
      test_usage();
      return EXIT_FAILURE;
    }

  for (i = 0; i < TEST_NDEMOS; i++)
    {
      if (strcmp(argv[1], g_test_demos[i].name) == 0)
        {
          /* Forward remaining args with argv[0] = sub-command name so the
           * demo can parse its own options independently.
           */

          return g_test_demos[i].entry(argc - 1, &argv[1]);
        }
    }

  printf("test: unknown demo '%s'\n", argv[1]);
  test_usage();
  return EXIT_FAILURE;
}
