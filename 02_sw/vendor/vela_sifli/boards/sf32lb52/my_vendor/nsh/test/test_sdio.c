/**
 * @file test_sdio.c
 * @brief test sdio / layers：第 1 层原始 SDIO/MTD 读写探针。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nuttx/compiler.h>

#include "sf32lb_sdio.h"
#include "test_demos.h"

#define LOGI(fmt, ...) printf("test sdio: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) printf("test sdio: ERROR " fmt "\n", ##__VA_ARGS__)

int __attribute__((weak)) sf32lb_sd_hwtest(unsigned flags, int seq_kib,
                                           int rnd_ops)
{
  UNUSED(flags);
  UNUSED(seq_kib);
  UNUSED(rnd_ops);
  LOGE("SD driver not linked");
  return -ENOSYS;
}

/** @brief 打印 test sdio 用法。 */
static void sdio_usage(void)
{
  printf("Usage: test sdio [read|write|seq [KiB]|rnd [ops]|all]\n"
         "  Layer 1 — raw SDIO/MTD /dev/sd0, 512 B CMD17 or 4 KiB CMD18, no FS.\n"
         "  (default)      seq + rnd reads only (safe with /mnt/lfs mounted)\n"
         "  read           seq + rnd only (never writes; seq also times CMD18)\n"
         "  write          4 KiB save/restore at FS window end (~4 GiB LBA)\n"
         "  seq [KiB]      sequential 512 B reads (256)\n"
         "  rnd [ops]      random 512 B reads in first 1 MiB (64)\n"
         "  all            seq + rnd + write\n");
}

/**
 * @brief test sdio 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_sdio_main(int argc, FAR char *argv[])
{
  unsigned flags = SF32LB_SD_HWTEST_SEQ | SF32LB_SD_HWTEST_RND;
  int seq_kib = 256;
  int rnd_ops = 64;
  int ret;

  if (argc >= 2)
    {
      if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0)
        {
          sdio_usage();
          return EXIT_SUCCESS;
        }
      else if (strcmp(argv[1], "read") == 0)
        {
          flags = SF32LB_SD_HWTEST_SEQ | SF32LB_SD_HWTEST_RND;
        }
      else if (strcmp(argv[1], "write") == 0)
        {
          flags = SF32LB_SD_HWTEST_WRITE;
        }
      else if (strcmp(argv[1], "seq") == 0)
        {
          flags = SF32LB_SD_HWTEST_SEQ;
          if (argc >= 3)
            {
              seq_kib = atoi(argv[2]);
            }
        }
      else if (strcmp(argv[1], "rnd") == 0)
        {
          flags = SF32LB_SD_HWTEST_RND;
          if (argc >= 3)
            {
              rnd_ops = atoi(argv[2]);
            }
        }
      else if (strcmp(argv[1], "all") == 0)
        {
          flags = SF32LB_SD_HWTEST_ALL;
        }
      else
        {
          sdio_usage();
          return EXIT_FAILURE;
        }
    }

  LOGI("==== layer 1 SDIO/MTD start ====");
  ret = sf32lb_sd_hwtest(flags, seq_kib, rnd_ops);
  if (ret < 0)
    {
      LOGE("==== layer 1 SDIO/MTD: FAIL (%d) ====", ret);
      return EXIT_FAILURE;
    }

  LOGI("==== layer 1 SDIO/MTD: PASS ====");
  return EXIT_SUCCESS;
}

/**
 * @brief test layers 分层 SD 联测入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_layers_main(int argc, FAR char *argv[])
{
  char *sdio_argv[] = { "sdio", "read", NULL };
  char *small_argv[] = { "lfs", "small", NULL };
  char *mkdir_argv[] = { "lfs", "mkdir", NULL };
  char *large_argv[] = { "lfs", "large", "32", NULL };
  int ret;

  UNUSED(argc);
  UNUSED(argv);

  printf("==== layered SD tests ====\n"
         "  1) SDIO read\n"
         "  2a) LittleFS large 32 KiB (reuse existing file, no O_CREAT)\n"
         "  2b) LittleFS small files\n"
         "  2c) LittleFS mkdir/rmdir (MTP new-folder analog)\n"
         "Do not run MTP.  Do not use statfs on /mnt/lfs.\n"
         "If mount fails EINVAL: nsh> test lfs format -y   (wipes volume)\n");

  printf("\n---- 1/4: test sdio read ----\n");
  ret = test_sdio_main(2, sdio_argv);
  if (ret != EXIT_SUCCESS)
    {
      printf("==== layered SD tests: FAIL at sdio ====\n");
      return EXIT_FAILURE;
    }

  printf("\n---- 2a/4: test lfs large 32 ----\n");
  ret = test_lfs_main(3, large_argv);
  if (ret != EXIT_SUCCESS)
    {
      printf("==== layered SD tests: FAIL at lfs large ====\n");
      return EXIT_FAILURE;
    }

  printf("\n---- 2b/4: test lfs small ----\n");
  ret = test_lfs_main(2, small_argv);
  if (ret != EXIT_SUCCESS)
    {
      printf("==== layered SD tests: FAIL at lfs small ====\n");
      return EXIT_FAILURE;
    }

  printf("\n---- 2c/4: test lfs mkdir ----\n");
  ret = test_lfs_main(2, mkdir_argv);
  if (ret != EXIT_SUCCESS)
    {
      printf("==== layered SD tests: FAIL at lfs mkdir ====\n");
      return EXIT_FAILURE;
    }

  printf("==== layered SD tests: PASS ====\n");
  return EXIT_SUCCESS;
}
