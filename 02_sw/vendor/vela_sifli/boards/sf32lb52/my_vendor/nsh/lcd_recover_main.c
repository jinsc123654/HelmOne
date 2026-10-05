/**
 * @file lcd_recover_main.c
 * @brief NSH 命令 lcd_recover：手动重启 ui_flush 刷新管道。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#ifndef CONFIG_MYVENDOR_LCD_DISP
#  error lcd_recover requires CONFIG_MYVENDOR_LCD_DISP
#endif

#include "myvendor_lcd_disp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/** @brief 打印 lcd_recover 用法。 */
static void lcd_recover_usage(void)
{
  printf("Usage: lcd_recover [status]\n");
  printf("  lcd_recover         reopen /dev/lcd0, restart ui_flush\n");
  printf("  lcd_recover status  print flush pipe busy flag and stats\n");
  printf("Note: bicycle_ui must be running for LVGL full-screen refresh after recover.\n");
}

/** @brief 打印 flush 管道 busy 标志与统计。 */
static void lcd_recover_print_status(void)
{
  myvendor_lcd_disp_stats_t stats;
  myvendor_lcd_disp_info_t info;
  int ret;

  ret = myvendor_lcd_disp_get_info(&info);
  if (ret < 0)
    {
      printf("lcd_disp: not initialized (start bicycle first)\n");
      return;
    }

  myvendor_lcd_disp_stats_snapshot(&stats);

  printf("lcd_disp: %ux%u strip=%u busy=%d\n",
         info.hor_res, info.ver_res, info.strip_rows,
         (int)myvendor_lcd_disp_flush_busy());
  printf("  submit_ok=%u busy=%u err=%u worker=%u ready=%u recover=%u\n",
         (unsigned)stats.submit_ok, (unsigned)stats.submit_busy,
         (unsigned)stats.submit_err, (unsigned)stats.worker_done,
         (unsigned)stats.flush_ready, (unsigned)stats.recover);
  printf("  putarea_max=%ums putarea_fail=%u wait_timeout=%u\n",
         (unsigned)stats.putarea_ms_max, (unsigned)stats.putarea_fail,
         (unsigned)stats.wait_timeout);
}

/**
 * @brief NSH 命令 lcd_recover：手动重启 ui_flush 刷新管道。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, FAR char *argv[])
{
  myvendor_lcd_disp_info_t info;
  int ret;

  if (argc >= 2)
    {
      if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
          lcd_recover_usage();
          return 0;
        }

      if (strcmp(argv[1], "status") == 0)
        {
          lcd_recover_print_status();
          return 0;
        }

      lcd_recover_usage();
      return 1;
    }

  if (myvendor_lcd_disp_get_info(&info) < 0)
    {
      printf("lcd_recover: lcd_disp not ready — start bicycle first\n");
      return 1;
    }

  printf("lcd_recover: restarting ui_flush pipe ...\n");
  ret = myvendor_lcd_disp_trigger_recover();

  if (ret == 0)
    {
      printf("lcd_recover: done (recover_count see status)\n");
      return 0;
    }

  if (ret == -EBUSY)
    {
      printf("lcd_recover: recover already in progress\n");
      return 1;
    }

  printf("lcd_recover: failed %d (%s)\n", ret, strerror(-ret));
  return 1;
}
