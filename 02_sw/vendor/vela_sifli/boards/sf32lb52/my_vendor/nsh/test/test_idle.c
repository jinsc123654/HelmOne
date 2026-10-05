/**
 * @file test_idle.c
 * @brief test idle：对比真实 idle（WFI+DWT）与 cpuload。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "myvendor_idle_stat.h"
#include "test_demos.h"

#define TEST_IDLE_DEFAULT_WAIT_SEC 5

#define C_RST   "\033[0m"
#define C_HDR   "\033[1;36m"
#define C_OK    "\033[32m"
#define C_WARN  "\033[33m"
#define C_DIM   "\033[2m"
#define C_BAD   "\033[31m"

#if defined(CONFIG_MYVENDOR_IDLE_STAT) && CONFIG_MYVENDOR_IDLE_STAT

/** @brief 格式化打印 true idle 与 ps_idle 对比。 */
static void test_idle_print(const struct myvendor_idle_stat_snap_s *snap)
{
  uint32_t true_x10 = snap->true_idle_x10;
  uint32_t ps_x10 = snap->ps_idle_x10;
  int32_t gap_x10 = (int32_t)true_x10 - (int32_t)ps_x10;
  uint32_t win_ms = snap->win_ms;

  printf(C_HDR "test idle" C_RST "  "
         C_DIM "(true=WFI+DWT, ps_idle=NuttX cpuload PID0)" C_RST "\n");
  printf("  true%%    " C_OK "%3u.%u%%" C_RST "  trustworthy\n",
         (unsigned)(true_x10 / 10u), (unsigned)(true_x10 % 10u));
  printf("  ps_idle%%  " C_WARN "%3u.%u%%" C_RST "  tick sampling (ps)\n",
         (unsigned)(ps_x10 / 10u), (unsigned)(ps_x10 % 10u));
  printf("  window    " C_DIM "%u.%03us" C_RST "\n",
         (unsigned)(win_ms / 1000u), (unsigned)(win_ms % 1000u));

  if (gap_x10 >= 200)
    {
      printf("  " C_WARN "gap %u.%u%% — ps/cpuload understates idle"
             C_RST "\n",
             (unsigned)(gap_x10 / 10u), (unsigned)(gap_x10 % 10u));
    }
  else if (gap_x10 <= -200)
    {
      printf("  " C_BAD "gap %u.%u%% — unusual (ps > true)"
             C_RST "\n",
             (unsigned)((-gap_x10) / 10u), (unsigned)((-gap_x10) % 10u));
    }
}

/**
 * @brief test idle 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_idle_main(int argc, FAR char *argv[])
{
  int wait_sec = TEST_IDLE_DEFAULT_WAIT_SEC;
  struct myvendor_idle_stat_snap_s snap;

  if (argc >= 2)
    {
      wait_sec = atoi(argv[1]);
      if (wait_sec <= 0)
        {
          printf("Usage: test idle [wait_sec]\n");
          return EXIT_FAILURE;
        }
    }

  printf(C_DIM "test idle: measure %d s..." C_RST "\n", wait_sec);
  myvendor_idle_stat_window_begin();
  sleep((unsigned)wait_sec);

  myvendor_idle_stat_snapshot(&snap);
  test_idle_print(&snap);
  return EXIT_SUCCESS;
}

#else

int test_idle_main(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;
  printf(C_BAD "test idle" C_RST ": need CONFIG_MYVENDOR_IDLE_STAT=y\n");
  return EXIT_FAILURE;
}

#endif
