/**
 * @file test_time.c
 * @brief test time：多时钟源 uptime 快照。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <sys/sysinfo.h>

#include "test_demos.h"

#define LOGI(fmt, ...)  printf("test time: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("test time: ERROR " fmt "\n", ##__VA_ARGS__)

#define TEST_TIME_DEFAULT_SEC  5
#define TEST_TIME_REFRESH_SEC  1

/** @brief 打印 timespec 秒.微秒。 */
static void print_timespec(const char *label, const struct timespec *ts)
{
  printf("  %-18s %lu.%06lu s\n",
         label,
         (unsigned long)ts->tv_sec,
         (unsigned long)(ts->tv_nsec / 1000L));
}

/** @brief 打印各时钟源 uptime 快照。 */
static void print_snapshot(int pass, int total_sec)
{
  struct timespec mono;
  struct timespec real;
  struct timespec systime;
  struct sysinfo info;
  clock_t ticks;
  uint64_t tick_us;
  uint64_t tick_ms;

  if (total_sec > 1)
    {
      printf("\033[2J\033[1;1H");
      printf("test time: sample %d/%d s (compare with dmesg [sec.usec])\n",
             pass, total_sec);
    }
  else
    {
      printf("test time: snapshot (compare with dmesg [sec.usec])\n");
    }

  ticks = clock_systime_ticks();
  tick_us = (uint64_t)TICK2USEC(ticks);
  tick_ms = tick_us / 1000u;

  printf("  %-18s %" PRIu64 "  (USEC_PER_TICK=%ld us)\n",
         "systime_ticks", (uint64_t)ticks, (long)USEC_PER_TICK);
  printf("  %-18s %" PRIu64 " ms  (%lu.%06lu s)\n",
         "systime_tick_ms", tick_ms,
         (unsigned long)(tick_us / 1000000u),
         (unsigned long)(tick_us % 1000000u));

  if (clock_systime_timespec(&systime) == 0)
    {
      print_timespec("systime_ts", &systime);
    }
  else
    {
      printf("  %-18s (unavailable, errno=%d)\n", "systime_ts", errno);
    }

  memset(&info, 0, sizeof(info));
  if (sysinfo(&info) == 0)
    {
      printf("  %-18s %lu s\n", "sysinfo.uptime", info.uptime);
    }
  else
    {
      printf("  %-18s (unavailable, errno=%d)\n", "sysinfo.uptime", errno);
    }

  if (clock_gettime(CLOCK_MONOTONIC, &mono) == 0)
    {
      print_timespec("CLOCK_MONOTONIC", &mono);
    }
  else
    {
      printf("  %-18s (unavailable, errno=%d)\n", "CLOCK_MONOTONIC", errno);
    }

  if (clock_gettime(CLOCK_REALTIME, &real) == 0)
    {
      print_timespec("CLOCK_REALTIME", &real);
    }
  else
    {
      printf("  %-18s (unavailable, errno=%d)\n", "CLOCK_REALTIME", errno);
    }

  printf("  hint: dmesg [sec.usec] ~= systime_ts / CLOCK_MONOTONIC\n");
}

/**
 * @brief test time 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_time_main(int argc, FAR char *argv[])
{
  int seconds = 1;
  int pass;

  if (argc >= 2)
    {
      seconds = atoi(argv[1]);
      if (seconds <= 0)
        {
          LOGE("invalid duration '%s'", argv[1]);
          printf("Usage: test time [seconds]\n");
          return EXIT_FAILURE;
        }
    }

  if (seconds == 1)
    {
      print_snapshot(1, 1);
      return EXIT_SUCCESS;
    }

  LOGI("watch %d s, refresh every %d s", seconds, TEST_TIME_REFRESH_SEC);

  for (pass = 1; pass <= seconds; pass += TEST_TIME_REFRESH_SEC)
    {
      print_snapshot(pass, seconds);
      if (pass + TEST_TIME_REFRESH_SEC <= seconds)
        {
          sleep(TEST_TIME_REFRESH_SEC);
        }
    }

  LOGI("done");
  return EXIT_SUCCESS;
}
