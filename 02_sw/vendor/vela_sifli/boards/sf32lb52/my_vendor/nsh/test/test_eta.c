/**
 * @file test_eta.c
 * @brief test eta：ETA9184 电量计采样与 5V boost 控制。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eta9184.h"
#include "myvendor_sys.h"
#include "test_demos.h"

#define ETA_COLW     16
#define ETA_LOOP_US  1000000u

static FAR const char *eta_chg_str(enum eta9184_chg_e chg)
{
  switch (chg)
    {
      case ETA9184_CHG_CHARGING:
        return "charging";
      case ETA9184_CHG_FULL:
        return "full";
      default:
        return "idle";
    }
}

static FAR const char *eta_dischg_str(enum eta9184_dischg_e dis)
{
  switch (dis)
    {
      case ETA9184_DISCHG_ON:
        return "dischg";
      case ETA9184_DISCHG_LOW:
        return "low";
      default:
        return "idle";
    }
}

static FAR const char *eta_power_str(enum eta9184_power_e power)
{
  switch (power)
    {
      case ETA9184_POWER_CHARGING:
        return "charging";
      case ETA9184_POWER_DISCHARGING:
        return "discharging";
      case ETA9184_POWER_LOW:
        return "low";
      default:
        return "idle";
    }
}

/** @brief 打印 test eta 子命令列表。 */
static void eta_usage(void)
{
  printf("test eta commands:\n");
  printf("  start       start battery / charge-discharge sampling\n");
  printf("  stop        stop 4s software timer\n");
  printf("  read        print charge, discharge, pulse (untrusted), adc, pwr once\n");
  printf("  read N      print N times, 1 s apart\n");
  printf("  pwr 0|1     5V boost EN (audio PA)\n");
}

/** @brief 打印一次 ETA9184 状态表（PULSE 百分比不可信）。 */
static void eta_print_status(void)
{
  struct eta9184_status_s st;
  /* 这个缓冲只是拼给 %-*s 用的临时串，列宽是后面单独传的实参，所以不必
   * 等于 ETA_COLW：按列宽开会让 "%d%% untrusted"（int 最长 22 字节）截断。 */
  char pulse[ETA_COLW + 8];
  int adc_mv;
  int adc_pct;
  int full_mv;
  bool full_cal;

  eta9184_get_status(&st);

  if (st.pulse_pct >= 0)
    {
      snprintf(pulse, sizeof(pulse), "%d%% untrusted", st.pulse_pct);
    }
  else
    {
      snprintf(pulse, sizeof(pulse), "-- untrusted");
    }

  printf("%-*s%-*s%-*s%-*s%-*s\n",
         ETA_COLW, "charge",
         ETA_COLW, "discharge",
         ETA_COLW, "pulse",
         ETA_COLW, "state",
         ETA_COLW, "pwr");
  printf("%-*s%-*s%-*s%-*s%-*s\n",
         ETA_COLW, eta_chg_str(st.chg),
         ETA_COLW, eta_dischg_str(st.dischg),
         ETA_COLW, pulse,
         ETA_COLW, eta_power_str(st.power),
         ETA_COLW, st.boost_on ? "on" : "off");
  printf("pins stat=%c dischg=%c pulse=%c  pulse_n=%d dischg_n=%d  full_io=%s\n",
         st.chrg_high ? 'H' : 'L',
         st.stat_high ? 'H' : 'L',
         st.pulse_high ? 'H' : 'L',
         st.pulse_count,
         st.dischg_edges,
         (st.chg == ETA9184_CHG_FULL) ? "yes" : "no");
  printf("run  poll=%u  edge_pulse=%u  edge_dischg=%u  win=%u\n",
         st.timer_ticks,
         st.pulse_irqs,
         st.dischg_irqs,
         st.windows);

  {
    myvendor_sys_snapshot_t snap;

    myvendor_sys_snapshot(&snap);
    adc_mv = snap.power.mv;
    adc_pct = snap.power.pct;
    full_mv = snap.power.full_mv;
    full_cal = snap.power.full_cal;
  }

  if (adc_pct < 0)
    {
      printf("adc  --  (%d)  cal %d mV%s  (product SOC)\n\n",
             adc_mv, full_mv, full_cal ? "" : " (default)");
    }
  else
    {
      printf("adc  %d%%  %d mV  cal %d mV%s  (product SOC)\n\n",
             adc_pct, adc_mv, full_mv, full_cal ? "" : " (default)");
    }

  fflush(stdout);
}

/** @brief 执行 read 子命令。 */
static int eta_cmd_read(int argc, FAR char *argv[])
{
  int n = 1;
  int i;

  if (argc > 2)
    {
      n = atoi(argv[2]);
      if (n <= 0)
        {
          printf("test eta: read count must be > 0\n");
          return EXIT_FAILURE;
        }
    }

  if (!eta9184_running())
    {
      printf("test eta: not started (test eta start)\n");
    }

  for (i = 0; i < n; i++)
    {
      if (i > 0)
        {
          usleep(ETA_LOOP_US);
        }

      eta_print_status();
    }

  return EXIT_SUCCESS;
}

/** @brief 执行 pwr 0|1 子命令。 */
static int eta_cmd_pwr(int argc, FAR char *argv[])
{
  bool on;

  if (argc < 3)
    {
      printf("test eta: usage: test eta pwr 0|1\n");
      return EXIT_FAILURE;
    }

  if (eta9184_init() < 0)
    {
      printf("test eta: init failed\n");
      return EXIT_FAILURE;
    }

  if (strcmp(argv[2], "1") == 0 || strcmp(argv[2], "on") == 0)
    {
      on = true;
    }
  else if (strcmp(argv[2], "0") == 0 || strcmp(argv[2], "off") == 0)
    {
      on = false;
    }
  else
    {
      printf("test eta: usage: test eta pwr 0|1\n");
      return EXIT_FAILURE;
    }

  eta9184_boost_set(on);
  printf("test eta: pwr %s\n", on ? "on" : "off");
  return EXIT_SUCCESS;
}

/**
 * @brief test eta 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_eta_main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      eta_usage();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "start") == 0)
    {
      if (eta9184_running())
        {
          printf("test eta: already started\n");
          return EXIT_SUCCESS;
        }

      if (eta9184_start() < 0)
        {
          printf("test eta: start failed\n");
          return EXIT_FAILURE;
        }

      printf("test eta: started\n");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "stop") == 0)
    {
      if (!eta9184_running())
        {
          printf("test eta: already stopped\n");
          return EXIT_SUCCESS;
        }

      eta9184_stop();
      printf("test eta: stopped\n");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "read") == 0)
    {
      return eta_cmd_read(argc, argv);
    }

  if (strcmp(argv[1], "pwr") == 0)
    {
      return eta_cmd_pwr(argc, argv);
    }

  printf("test eta: unknown command '%s'\n", argv[1]);
  eta_usage();
  return EXIT_FAILURE;
}
