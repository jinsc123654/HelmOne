/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/nsh/test/test_diag.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `test diag`：主动产出几条日志，用来验证异常日志到底有没有落盘。
 *
 * 为什么需要它：骑行时只能事后取证，而 /mnt/kv/diag/dNNN_*.txt（RAM 环 → 每
 * 500 ms 刷盘）平时看不出是否在写 —— 2026-09-17 那次 BLE 事故就是"目录里十份
 * 文件全是旧的、当天一份都没有"，当时无法分辨是"级别没触发"还是"根本没写"。
 * 这条命令把这件事变成五秒可验证：
 *   1. 打 INFO / WARN / ERR 各一条，带唯一 marker；
 *   2. 列一遍 /mnt/kv/diag（文件名 + 大小）；
 *   3. 等 >1 个刷盘周期再列一遍：出现新文件、且 grep 得到 marker，就是通的。
 * 文件只收 WARN/ERR，所以 INFO 不进文件是预期行为，一起打出来正好对照。
 ****************************************************************************/

#include <nuttx/config.h>

#include "myvendor_diaglog.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DIAG_TEST_DIR   "/mnt/kv/diag"

/** @brief 打一行通道状态：没装 / 待落盘多少字节 —— 一次分清两种失败。 */
static void diag_test_state(const char *when)
{
  syslog(LOG_WARNING, "diag-test state %s started=%d pending=%u\n", when,
         myvendor_diaglog_started() ? 1 : 0,
         (unsigned)myvendor_diaglog_pending());
}

/** @brief 打一行过滤器累计量：目录里没有新文件时，指出断在哪一段。
 *
 *  feed 不涨 = 通道没被调用；feed 涨而 keep 不涨、last_sev=-1 = 级别记号
 *  没认出来（前缀格式变了）；keep 涨而 ring 不归零 = flush 写不出去。
 *  详见 myvendor_diaglog_probe() 的说明。取两次之差看增量。 */
static void diag_test_probe(const char *when)
{
  unsigned int feed = 0;
  unsigned int keep = 0;
  unsigned int drop = 0;
  unsigned int ring = 0;
  int          sev  = -99;

  myvendor_diaglog_probe(&feed, &keep, &drop, &sev, &ring);
  syslog(LOG_WARNING,
         "diag-test probe %s feed=%u keep=%u drop=%u last_sev=%d ring=%u\n",
         when, feed, keep, drop, sev, ring);
}

/* 列出目录里的 d* 文件（名字 + 字节数），按 readdir 顺序打，便于看新增。 */
static void diag_test_ls(void)
{
  DIR *dir;
  struct dirent *de;
  struct stat st;
  char path[256];
  unsigned n = 0;

  dir = opendir(DIAG_TEST_DIR);
  if (dir == NULL)
    {
      syslog(LOG_WARNING, "diag-test: opendir %s failed errno=%d\n",
             DIAG_TEST_DIR, errno);
      return;
    }

  while ((de = readdir(dir)) != NULL)
    {
      if (de->d_name[0] != 'd')
        {
          continue;
        }

      snprintf(path, sizeof(path), "%s/%s", DIAG_TEST_DIR, de->d_name);
      if (stat(path, &st) == 0)
        {
          syslog(LOG_WARNING, "diag-test: %s %ld B\n", de->d_name,
                 (long)st.st_size);
        }
      else
        {
          syslog(LOG_WARNING, "diag-test: %s stat errno=%d\n", de->d_name,
                 errno);
        }

      n++;
    }

  closedir(dir);
  if (n == 0)
    {
      syslog(LOG_WARNING, "diag-test: %s is empty\n", DIAG_TEST_DIR);
    }
}

int test_diag_main(int argc, FAR char *argv[])
{
  unsigned marker = (unsigned)time(NULL);

  (void)argc;
  (void)argv;

  syslog(LOG_WARNING, "diag-test start marker=%u dir=%s\n", marker,
         DIAG_TEST_DIR);
  diag_test_state("before");
  diag_test_probe("before");
  diag_test_ls();

  /* INFO 只进控制台（异常日志通道只收 WARN/ERR）。这条**预期不在文件里**，
   * 打出来是为了区分"级别没触发"和"通道根本没写"。 */
  syslog(LOG_INFO, "diag-test info marker=%u console-only\n", marker);
  syslog(LOG_WARNING, "diag-test warn marker=%u expect-in-file\n", marker);
  syslog(LOG_ERR, "diag-test err marker=%u expect-in-file\n", marker);

  /* 三条都打完立刻读一次计数：feed/keep 涨没涨，决定后面往哪个方向查。
   * （这一行 probe 自己也会进通道，所以它的增量计入下一次读数。） */
  diag_test_probe("after-markers");

  /* 把过滤器**实际看到**的字节打出来。feed 在涨、keep 恒 0 时，只有这一行能
   * 说明为什么判不出级别（记号写法不同 / 前缀压根没进这条通道）。
   * 再打一份"上次判定时扫描器看到的行首"：两者对比即可定位是接收、攒行首，
   * 还是比对本身的问题。 */
  {
    char tail[512];
    char pfx[256];

    myvendor_diaglog_tail(tail, sizeof(tail));
    syslog(LOG_WARNING, "diag-test filter-saw: %s\n", tail);

    myvendor_diaglog_lastpfx(pfx, sizeof(pfx));
    syslog(LOG_WARNING, "diag-test last-scan: %s\n", pfx);
  }

  /* 主动刷一次：把"环里没有东西"与"500 ms 周期还没到"分开。
   * rc<0 = 落盘路径本身失败（open/write），此时字节仍留在环里。 */
  {
    int frc = myvendor_diaglog_flush();

    syslog(LOG_WARNING, "diag-test forced flush rc=%d\n", frc);
  }

  diag_test_probe("after-flush");

  usleep(1500 * 1000);

  syslog(LOG_WARNING, "diag-test after\n");
  diag_test_state("after");
  diag_test_probe("after");
  diag_test_ls();
  syslog(LOG_WARNING, "diag-test done marker=%u\n", marker);
  return EXIT_SUCCESS;
}
