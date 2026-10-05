/**
 * @file mtpstat_main.c
 * @brief NSH 命令 mtpstat：打印 MTP 链路/会话状态，可选 begin/end 调试。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "myvendor_mtp.h"

/** @brief SD 块驱动错误计数转储（弱符号，非 SD 配置时为空实现）。 */

/** @brief SD 块驱动错误计数转储（弱符号占位）。 */
void __attribute__((weak)) sf32lb_sd_diag_dump(const char *tag)
{
  (void)tag;
}

/**
 * @brief NSH 命令 mtpstat：打印 MTP 链路/会话状态，可选 begin/end 调试。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, char *argv[])
{
  if (argc >= 2)
    {
      if (strcmp(argv[1], "begin") == 0)
        {
          int ret = myvendor_mtp_transfer_begin();
          printf("mtp transfer_begin ret=%d\n", ret);
        }
      else if (strcmp(argv[1], "end") == 0)
        {
          myvendor_mtp_transfer_end();
          printf("mtp transfer_end\n");
        }
      else
        {
          printf("usage: mtpstat [begin|end]\n");
          return 1;
        }
    }

  myvendor_mtp_log_status();
  sf32lb_sd_diag_dump("mtpstat");
  return 0;
}
