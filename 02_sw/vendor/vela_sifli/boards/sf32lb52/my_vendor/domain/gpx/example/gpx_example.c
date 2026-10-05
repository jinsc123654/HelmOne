/**
 * @file gpx_example.c
 * @brief 一键例程：3 个录制器并行录制 → 依次 decode 展示。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_example.h"

#include <nuttx/config.h>

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#include "gpx_example_record.h"

#include "gpx_decode.h"
#include "gpx_reader.h"
#include "gpx_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GPX_EXAMPLE_DEMO_TRACKS
#  define GPX_EXAMPLE_DEMO_TRACKS  3
#endif

#ifndef GPX_EXAMPLE_DEMO_SECONDS
#  define GPX_EXAMPLE_DEMO_SECONDS  5
#endif

/**
 * @brief 打印例程步骤说明。
 *
 * @param step  步骤描述
 */
static void gpx_example_print_step(const char *step)
{
  printf("gpx example: %s\n", step);
  fflush(stdout);
}

/**
 * @brief 打开 GPX 文件并 decode 打印所有轨迹点（含已注册扩展）。
 *
 * @param path  GPX 文件路径
 * @param slot  传感器扩展 slot
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
static int gpx_example_decode_file(const char *path, int slot)
{
  gpx_decode_t *dec = NULL;
  gpx_point_t point;
  gpx_sensor_ext_t sensor_buf;
  char line[GPX_READER_LINE_MAX];
  gpx_decode_stats_t stats;
  int count = 0;
  int ret;

  ret = gpx_decode_open(path, NULL, &dec);
  if (ret != 0)
    {
      printf("gpx example: open %s failed (%d)\n", path, ret);
      return EXIT_FAILURE;
    }

  memset(&point, 0, sizeof(point));
  (void)gpx_decode_bind_ext(dec, slot, &sensor_buf);
  point.ext_data[slot] = &sensor_buf;
  point.ext_mask = (uint8_t)(1u << slot);

  for (;;)
    {
      unsigned n;
      int nfmt;

      memset(&sensor_buf, 0, sizeof(sensor_buf));
      ret = gpx_decode_read(dec, &point, 1, &n);
      if (n > 0)
        {
          count++;
          nfmt = gpx_reader_format_trkpt(&point, line, sizeof(line));
          if (nfmt < 0)
            {
              printf("  #%d (format err %d)\n", count, nfmt);
            }
          else
            {
              printf("  #%d %s\n", count, line);
            }
        }

      if (ret == 1)
        {
          break;
        }

      if (ret < 0)
        {
          printf("gpx example: decode error on %s (%d)\n", path, ret);
          gpx_decode_close(&dec);
          return EXIT_FAILURE;
        }
    }

  gpx_decode_get_stats(dec, &stats);
  gpx_decode_close(&dec);

  printf("gpx example: decoded %d points from %s (file=%lu skipped=%lu)\n",
         count, path,
         (unsigned long)stats.file_points,
         (unsigned long)stats.points_skipped);
  return EXIT_SUCCESS;
}

/**
 * @brief 一键演示：3 录制器并行录 → 依次 decode。
 *
 * @param seconds  每个文件录制秒数；<=0 用默认
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
static int gpx_example_run_demo(int seconds)
{
  static const char *paths[GPX_EXAMPLE_DEMO_TRACKS] =
  {
    "/mnt/lfs/custom_demo_1.gpx",
    "/mnt/lfs/custom_demo_2.gpx",
    "/mnt/lfs/custom_demo_3.gpx",
  };

  gpx_record_t *rec[GPX_EXAMPLE_DEMO_TRACKS] = { NULL };
  gpx_example_origin_t origins[GPX_EXAMPLE_DEMO_TRACKS];
  gpx_record_cfg_t cfg;
  gpx_meta_t meta;
  gpx_record_stats_t stats;
  int slot = -1;
  int i;
  int t;
  int ret;

  if (seconds <= 0)
    {
      seconds = GPX_EXAMPLE_DEMO_SECONDS;
    }

  gpx_example_print_step("[1/6] 注册传感器扩展 (gpx_ext_sensor)");
  slot = gpx_ext_sensor_register();
  if (slot < 0)
    {
      printf("gpx example: register failed (%d)\n", slot);
      return EXIT_FAILURE;
    }

  cfg.batch_size = gpx_example_batch_size(seconds);
  cfg.queue_depth = 0;

  gpx_example_print_step("[2/6] 创建 3 个独立录制器 (各 1 线程)");
  for (i = 0; i < GPX_EXAMPLE_DEMO_TRACKS; i++)
    {
      ret = gpx_record_create(&cfg, &rec[i]);
      if (ret != 0)
        {
          printf("gpx example: create recorder #%d failed (%d)\n", i + 1, ret);
          goto fail;
        }
    }

  memset(&meta, 0, sizeof(meta));
  meta.creator = CONFIG_MYVENDOR_PRODUCT_NAME " gpx example";
  meta.meta_name = "sensor ext demo";
  meta.meta_desc = "hr/cad/speed gpxtpx v2 + power gpxpx";

  gpx_example_print_step("[3/6] 启动 3 路录制 (随机起点各不同)");
  for (i = 0; i < GPX_EXAMPLE_DEMO_TRACKS; i++)
    {
      gpx_example_origin_random(&origins[i], (unsigned)i);
      meta.track_name = paths[i];
      ret = gpx_record_start(rec[i], paths[i], &meta);
      if (ret != 0)
        {
          printf("gpx example: start #%d failed (%d)\n", i + 1, ret);
          goto fail;
        }

      printf("  recorder #%d -> %s  origin lat=%.6f lon=%.6f ele=%.1f\n",
             i + 1, paths[i],
             (double)origins[i].lat, (double)origins[i].lon,
             (double)origins[i].ele);
    }

  printf("gpx example: [4/6] 同时录制 %d 秒 (每秒每路 push 1 点)\n", seconds);
  for (t = 0; t < seconds; t++)
    {
      for (i = 0; i < GPX_EXAMPLE_DEMO_TRACKS; i++)
        {
          gpx_point_t point;
          gpx_sensor_ext_t sensor;

          gpx_example_fill_point(&point, &sensor, &origins[i], t, slot);
          if (gpx_record_push(rec[i], &point) != 0)
            {
              printf("  t=%d rec#%d push failed\n", t + 1, i + 1);
            }
        }

      sleep(1);
    }

  gpx_example_print_step("[5/6] 停止并释放 3 个录制器 (刷盘 close)");
  for (i = 0; i < GPX_EXAMPLE_DEMO_TRACKS; i++)
    {
      gpx_record_stop(rec[i]);
      (void)gpx_record_wait_close(rec[i]);
      gpx_record_get_stats(rec[i], &stats);
      printf("  #%d written=%lu dropped=%lu batches=%lu\n",
             i + 1,
             (unsigned long)stats.points_written,
             (unsigned long)stats.points_dropped,
             (unsigned long)stats.batches_written);
      gpx_record_release(&rec[i]);
      rec[i] = NULL;
    }

  gpx_example_print_step("[6/6] 依次 decode 并展示");
  for (i = 0; i < GPX_EXAMPLE_DEMO_TRACKS; i++)
    {
      printf("gpx example: --- decode %s ---\n", paths[i]);
      gpx_example_decode_file(paths[i], slot);
    }

  gpx_ext_sensor_unregister(slot);
  gpx_example_print_step("演示完成");
  return EXIT_SUCCESS;

fail:
  for (i = 0; i < GPX_EXAMPLE_DEMO_TRACKS; i++)
    {
      if (rec[i] != NULL)
        {
          gpx_record_release(&rec[i]);
        }
    }

  if (slot >= 0)
    {
      gpx_ext_sensor_unregister(slot);
    }

  return EXIT_FAILURE;
}

/** 处理 `gpx example record` 子命令。 */
static int gpx_example_cmd_record(int argc, char *argv[])
{
  const char *outpath;
  int seconds = 10;
  int ret;

  if (argc < 2)
    {
      printf("usage: gpx example record <out.gpx> [seconds]\n");
      return EXIT_FAILURE;
    }

  outpath = argv[1];
  if (argc >= 3)
    {
      seconds = atoi(argv[2]);
    }

  ret = gpx_example_record_start(outpath, seconds);
  if (ret == -EBUSY)
    {
      printf("gpx example: record already running\n");
      return EXIT_FAILURE;
    }

  if (ret != 0)
    {
      printf("gpx example: start failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}

/** 处理 `gpx example decode` 子命令。 */
static int gpx_example_cmd_decode(int argc, char *argv[])
{
  int slot;
  int ret;

  if (argc < 2)
    {
      printf("usage: gpx example decode <file.gpx>\n");
      return EXIT_FAILURE;
    }

  slot = gpx_ext_sensor_register();
  if (slot < 0)
    {
      printf("gpx example: sensor ext register failed (%d)\n", slot);
      return EXIT_FAILURE;
    }

  ret = gpx_example_decode_file(argv[1], slot);
  gpx_ext_sensor_unregister(slot);
  return ret;
}

/** 处理 `gpx example status` 子命令。 */
static int gpx_example_cmd_status(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  printf("gpx example: background record %s\n",
         gpx_example_record_is_running() ? "running" : "idle");
  return EXIT_SUCCESS;
}

/**
 * @brief NSH `gpx example` 入口：无参数时一键 demo，否则 dispatch 子命令。
 *
 * @param argc  0 = 一键演示；否则 argv[0] 为子命令
 * @param argv  参数列表
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
int gpx_example_main(int argc, char *argv[])
{
  if (argc == 0)
    {
      return gpx_example_run_demo(GPX_EXAMPLE_DEMO_SECONDS);
    }

  if (argc == 1 && strcmp(argv[0], "demo") == 0)
    {
      return gpx_example_run_demo(GPX_EXAMPLE_DEMO_SECONDS);
    }

  if (argc == 2 && strcmp(argv[0], "demo") == 0)
    {
      return gpx_example_run_demo(atoi(argv[1]));
    }

  if (strcmp(argv[0], "record") == 0)
    {
      return gpx_example_cmd_record(argc, argv);
    }

  if (strcmp(argv[0], "decode") == 0)
    {
      return gpx_example_cmd_decode(argc, argv);
    }

  if (strcmp(argv[0], "status") == 0)
    {
      return gpx_example_cmd_status(argc, argv);
    }

  printf("usage:\n");
  printf("  gpx example              一键: 3 录制器并行录 + 依次 decode\n");
  printf("  gpx example demo [sec]   同上，可指定秒数\n");
  printf("  gpx example record <file.gpx> [sec]  (后台 task)\n");
  printf("  gpx example decode <file.gpx>\n");
  printf("  gpx example status\n");
  return EXIT_FAILURE;
}