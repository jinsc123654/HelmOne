/**
 * @file gpx_main.c
 * @brief NSH 入口：
 *
 * gpx decode <file>
 * gpx record <out.gpx> [seconds] [batch N] [queue depth]
 * gpx example              一键 3 路录制 + decode
 * gpx example record | decode | status
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "example/gpx_example.h"
#include "gpx_decode.h"
#include "gpx_reader.h"
#include "gpx_record.h"
#include "gpx_types.h"

#ifndef CONFIG_MYVENDOR_GPX_TRACK_DIR
#  define CONFIG_MYVENDOR_GPX_TRACK_DIR "/mnt/lfs/mtp/record"
#endif

/**
 * @brief 处理 `gpx decode` 子命令。
 *
 * 句柄式解码；扩展字段需事先 gpx_ext_register() 并在 read 前绑定 ext_data。
 *
 * @param argc  argv[1]=文件；可选 argv[2]=limit argv[3]=stride argv[4]=batch
 * @param argv  参数列表
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
static int gpx_cmd_decode(int argc, char *argv[])
{
  gpx_decode_t *dec = NULL;
  gpx_decode_cfg_t cfg;
  gpx_decode_stats_t stats;
  gpx_point_t point;
  char line[GPX_READER_LINE_MAX];
  unsigned limit = 0;
  unsigned stride = 1;
  unsigned batch = 0;
  int count = 0;
  int ret;

  if (argc < 2)
    {
      printf("usage: gpx decode <file.gpx> [limit] [stride] [batch]\n");
      printf("  stride=0 且 limit>0：按文件大小自动抽稀，最多 limit 点\n");
      return EXIT_FAILURE;
    }

  if (argc >= 3)
    {
      limit = (unsigned)atoi(argv[2]);
    }

  if (argc >= 4)
    {
      stride = (unsigned)atoi(argv[3]);
    }

  if (argc >= 5)
    {
      batch = (unsigned)atoi(argv[4]);
    }

  memset(&cfg, 0, sizeof(cfg));
  memset(&point, 0, sizeof(point));
  cfg.batch_max = batch;
  cfg.stride = stride;
  cfg.limit = limit;

  ret = gpx_decode_open(argv[1], &cfg, &dec);
  if (ret != 0)
    {
      printf("gpx: open %s failed (%d)\n", argv[1], ret);
      return EXIT_FAILURE;
    }

  for (;;)
    {
      unsigned n;
      int nfmt;

      ret = gpx_decode_read(dec, &point, 1, &n);
      if (n > 0)
        {
          count++;
          nfmt = gpx_reader_format_trkpt(&point, line, sizeof(line));
          if (nfmt < 0)
            {
              printf("#%d lat=%.6f lon=%.6f (ext print err %d)\n",
                     count, (double)point.latitude, (double)point.longitude,
                     nfmt);
            }
          else
            {
              printf("#%d %s\n", count, line);
            }
        }

      if (ret == 1)
        {
          break;
        }

      if (ret < 0)
        {
          printf("gpx: decode error (%d)\n", ret);
          gpx_decode_close(&dec);
          return EXIT_FAILURE;
        }
    }

  gpx_decode_get_stats(dec, &stats);
  gpx_decode_close(&dec);

  printf("gpx: decoded %d points (file=%lu skipped=%lu)\n",
         count,
         (unsigned long)stats.file_points,
         (unsigned long)stats.points_skipped);
  return EXIT_SUCCESS;
}

/**
 * @brief 填充内置演示轨迹点（无扩展数据）。
 *
 * @param point  输出轨迹点
 * @param index  点序号（用于生成变化坐标）
 */
static void gpx_fill_demo_point(gpx_point_t *point, int index)
{
  struct timespec ts;

  memset(point, 0, sizeof(*point));
  point->latitude = 39.907415f + (index * 0.0001f);
  point->longitude = 116.391332f + (index * 0.0001f);
  point->altitude = 45.0f + (float)index;
  point->has_altitude = true;
  point->has_time = true;

  clock_gettime(CLOCK_REALTIME, &ts);
  point->time.year = 2026;
  point->time.month = 6;
  point->time.day = 16;
  point->time.hour = (uint8_t)((ts.tv_sec / 3600) % 24);
  point->time.minute = (uint8_t)((ts.tv_sec / 60) % 60);
  point->time.second = (uint8_t)(ts.tv_sec % 60);
}

/**
 * @brief 处理 `gpx record` 子命令。
 *
 * 同步录制演示：每秒 push 一点，使用 gpx_record 后台写盘。
 *
 * @param argc  参数个数（含 argv[0]）
 * @param argv  argv[1]=输出路径，argv[2]=秒数，argv[3]=batch，argv[4]=queue
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
static int gpx_cmd_record(int argc, char *argv[])
{
  const char *outpath;
  int seconds = 5;
  unsigned batch = CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE;
  unsigned queue_depth = 0;
  gpx_record_t *rec = NULL;
  gpx_meta_t meta;
  gpx_record_cfg_t cfg;
  gpx_record_stats_t stats;
  gpx_record_runtime_t runtime;
  int i;
  int ret;

  if (argc < 2)
    {
      printf("usage: gpx record <out.gpx> [seconds] [batch N] [queue depth]\n");
      return EXIT_FAILURE;
    }

  outpath = argv[1];
  if (argc >= 3)
    {
      seconds = atoi(argv[2]);
      if (seconds <= 0)
        {
          seconds = 5;
        }
    }

  if (argc >= 4)
    {
      batch = (unsigned)atoi(argv[3]);
      if (batch == 0)
        {
          batch = CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE;
        }
    }

  if (argc >= 5)
    {
      queue_depth = (unsigned)atoi(argv[4]);
    }

  memset(&meta, 0, sizeof(meta));
  meta.creator = CONFIG_MYVENDOR_PRODUCT_NAME " gpx demo";
  meta.meta_name = "demo ride";
  meta.track_name = outpath;

  cfg.batch_size = batch;
  cfg.queue_depth = queue_depth;

  ret = gpx_record_create(&cfg, &rec);
  if (ret != 0)
    {
      printf("gpx: recorder create failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  ret = gpx_record_start(rec, outpath, &meta);
  if (ret != 0)
    {
      printf("gpx: record start failed (%d)\n", ret);
      gpx_record_release(&rec);
      return EXIT_FAILURE;
    }

  gpx_record_get_runtime(rec, &runtime);
  printf("gpx: recording %d s -> %s (N=%u queue=%u)\n",
         seconds, outpath, runtime.batch_size, runtime.queue_depth);

  for (i = 0; i < seconds; i++)
    {
      gpx_point_t point;

      gpx_fill_demo_point(&point, i);
      if (gpx_record_push(rec, &point) != 0)
        {
          printf("gpx: queue full at point %d\n", i);
        }

      sleep(1);
    }

  gpx_record_stop(rec);
  (void)gpx_record_wait_close(rec);
  gpx_record_get_stats(rec, &stats);
  printf("gpx: written=%lu dropped=%lu batches=%lu\n",
         (unsigned long)stats.points_written,
         (unsigned long)stats.points_dropped,
         (unsigned long)stats.batches_written);
  gpx_record_release(&rec);
  return EXIT_SUCCESS;
}

/**
 * @brief gpx NSH 应用入口。
 *
 * @param argc  参数个数
 * @param argv  argv[1]=子命令 decode | record | example
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
int main(int argc, char *argv[])
{
  if (argc < 2)
    {
      printf("usage:\n");
      printf("  gpx decode <file.gpx> [limit] [stride] [batch]\n");
      printf("  gpx record <out.gpx> [seconds] [batch N] [queue depth]\n");
      printf("  gpx example [demo [sec]]  3 路录制 + decode 一键演示\n");
      printf("  gpx example record <out.gpx> [seconds]  (background)\n");
      printf("  gpx example decode <file.gpx>\n");
      printf("  gpx example status\n");
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "decode") == 0)
    {
      return gpx_cmd_decode(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "record") == 0)
    {
      return gpx_cmd_record(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "example") == 0)
    {
      return gpx_example_main(argc - 2, argv + 2);
    }

  printf("gpx: unknown command %s\n", argv[1]);
  return EXIT_FAILURE;
}