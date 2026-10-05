/**
 * @file gpx_example_record.c
 * @brief GPX 传感器扩展录制演示 — NuttX task_spawn 后台任务。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_example_record.h"

#include "gpx_record.h"

#include <nuttx/config.h>

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE
#  define CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE 32
#endif

#ifndef CONFIG_MYVENDOR_GPX_EXAMPLE_STACKSIZE
#  define CONFIG_MYVENDOR_GPX_EXAMPLE_STACKSIZE 8192
#endif

#ifndef CONFIG_MYVENDOR_GPX_EXAMPLE_PRIORITY
#  define CONFIG_MYVENDOR_GPX_EXAMPLE_PRIORITY 100
#endif

#define GPX_EXAMPLE_LAT_MIN   39.850000f
#define GPX_EXAMPLE_LAT_SPAN  0.150000f
#define GPX_EXAMPLE_LON_MIN   116.300000f
#define GPX_EXAMPLE_LON_SPAN  0.200000f
#define GPX_EXAMPLE_ELE_MIN   40.0f
#define GPX_EXAMPLE_ELE_SPAN  30.0f

/**
 * @brief gpx_example_origin_random 接口。
 */
void gpx_example_origin_random(gpx_example_origin_t *origin, unsigned salt)
{
  struct timespec ts;
  unsigned seed;

  if (origin == NULL)
    {
      return;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  seed = (unsigned)ts.tv_sec ^ (unsigned)ts.tv_nsec ^
         (unsigned)getpid() ^ (salt * 7919u);
  srand(seed);

  origin->lat = GPX_EXAMPLE_LAT_MIN +
                (float)(rand() % 10000) / 10000.0f * GPX_EXAMPLE_LAT_SPAN;
  origin->lon = GPX_EXAMPLE_LON_MIN +
                (float)(rand() % 10000) / 10000.0f * GPX_EXAMPLE_LON_SPAN;
  origin->ele = GPX_EXAMPLE_ELE_MIN +
                (float)(rand() % 1000) / 1000.0f * GPX_EXAMPLE_ELE_SPAN;
}

/**
 * @brief gpx_example_fill_point 接口。
 */
void gpx_example_fill_point(gpx_point_t *point, gpx_sensor_ext_t *sensor,
                            const gpx_example_origin_t *origin,
                            int index, int slot)
{
  struct timespec ts;

  if (point == NULL || sensor == NULL || origin == NULL)
    {
      return;
    }

  memset(point, 0, sizeof(*point));
  point->latitude = origin->lat + (index * 0.00012f);
  point->longitude = origin->lon + (index * 0.00008f);
  point->altitude = origin->ele + (float)(index % 5);
  point->has_altitude = true;
  point->has_time = true;

  clock_gettime(CLOCK_REALTIME, &ts);
  point->time.year = 2026;
  point->time.month = 6;
  point->time.day = 16;
  point->time.hour = (uint8_t)((ts.tv_sec / 3600) % 24);
  point->time.minute = (uint8_t)((ts.tv_sec / 60) % 60);
  point->time.second = (uint8_t)(ts.tv_sec % 60);

  memset(sensor, 0, sizeof(*sensor));
  sensor->hr = (uint16_t)(120 + (index % 20));
  sensor->cad = (uint16_t)(80 + (index % 15));
  /* 速度按 gpxtpx 的单位给 m/s（约 25 km/h 起、逐点递增），功率给 150 W 起的
   * 假值：示例要覆盖 gpx_ext_sensor 的全部字段，板上 `gpx example decode` 一眼
   * 就能确认四个读数都写进去了。 */
  sensor->speed_mps = 7.0f + (float)(index % 10) * 0.5f;
  sensor->power_w = (uint16_t)(150 + (index % 40) * 3);
  sensor->has_hr = true;
  sensor->has_cad = true;
  sensor->has_speed = true;
  sensor->has_power = true;

  point->ext_data[slot] = sensor;
  point->ext_mask = (uint8_t)(1u << slot);
}

/**
 * @brief gpx_example_batch_size 接口。
 * @return 请求的值。
 */
unsigned gpx_example_batch_size(int seconds)
{
  unsigned batch = (unsigned)seconds;

  if (batch == 0)
    {
      batch = 1;
    }

  if (batch > (unsigned)CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE)
    {
      batch = (unsigned)CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE;
    }

  return batch;
}

static volatile bool g_example_running;
static int g_sensor_slot = -1;

/**
 * @brief 打印带前缀的例程日志（printf + syslog）。
 *
 * @param fmt  printf 格式串
 * @param ...  可变参数
 */
static void gpx_example_log(const char *fmt, ...)
{
  va_list ap;
  char buf[160];

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  printf("%s\n", buf);
  fflush(stdout);
  syslog(LOG_INFO, "%s", buf);
}

/** task_spawn 入口：单路后台录制演示。 */
int gpx_example_record_task(int argc, FAR char *argv[])
{
  const char *outpath;
  int seconds = 10;
  gpx_record_t *rec = NULL;
  gpx_example_origin_t origin;
  gpx_meta_t meta;
  gpx_record_cfg_t cfg;
  gpx_record_stats_t stats;
  gpx_record_runtime_t runtime;
  int i;
  int ret;

  if (argc < 2 || argv[1] == NULL)
    {
      gpx_example_log("gpx example: task usage <out.gpx> [seconds]");
      g_example_running = false;
      return EXIT_FAILURE;
    }

  outpath = argv[1];
  if (argc >= 3)
    {
      seconds = atoi(argv[2]);
      if (seconds <= 0)
        {
          seconds = 10;
        }
    }

  g_sensor_slot = gpx_ext_sensor_register();
  if (g_sensor_slot < 0)
    {
      gpx_example_log("gpx example: sensor ext register failed (%d)",
                      g_sensor_slot);
      g_sensor_slot = -1;
      goto out;
    }

  memset(&meta, 0, sizeof(meta));
  meta.creator = CONFIG_MYVENDOR_PRODUCT_NAME " gpx example";
  meta.meta_name = "sensor ext demo";
  meta.meta_desc = "hr/cad/speed gpxtpx v2 + power gpxpx";
  meta.track_name = outpath;

  cfg.batch_size = gpx_example_batch_size(seconds);
  cfg.queue_depth = 0;

  ret = gpx_record_create(&cfg, &rec);
  if (ret != 0)
    {
      gpx_example_log("gpx example: recorder create failed (%d)", ret);
      gpx_ext_sensor_unregister(g_sensor_slot);
      g_sensor_slot = -1;
      goto out;
    }

  ret = gpx_record_start(rec, outpath, &meta);
  if (ret != 0)
    {
      gpx_example_log("gpx example: record start failed (%d)", ret);
      gpx_record_release(&rec);
      gpx_ext_sensor_unregister(g_sensor_slot);
      g_sensor_slot = -1;
      goto out;
    }

  gpx_example_origin_random(&origin, 0);
  gpx_record_get_runtime(rec, &runtime);
  gpx_example_log("gpx example: origin lat=%.6f lon=%.6f ele=%.1f",
                  (double)origin.lat, (double)origin.lon, (double)origin.ele);
  gpx_example_log("gpx example: task recording %d s -> %s (slot=%d N=%u queue=%u)",
                  seconds, outpath, g_sensor_slot,
                  runtime.batch_size, runtime.queue_depth);

  for (i = 0; i < seconds; i++)
    {
      gpx_point_t point;
      gpx_sensor_ext_t sensor;

      gpx_example_fill_point(&point, &sensor, &origin, i, g_sensor_slot);
      if (gpx_record_push(rec, &point) != 0)
        {
          gpx_example_log("gpx example: push failed at point %d", i + 1);
        }
      else
        {
          gpx_example_log("gpx example: point %d/%d pushed", i + 1, seconds);
        }

      sleep(1);
    }

  if (gpx_record_stop(rec) != 0)
    {
      gpx_example_log("gpx example: record stop failed");
    }

  (void)gpx_record_wait_close(rec);

  gpx_record_get_stats(rec, &stats);
  gpx_record_release(&rec);
  gpx_ext_sensor_unregister(g_sensor_slot);
  g_sensor_slot = -1;

  gpx_example_log("gpx example: done written=%lu dropped=%lu batches=%lu",
                  (unsigned long)stats.points_written,
                  (unsigned long)stats.points_dropped,
                  (unsigned long)stats.batches_written);

out:
  g_example_running = false;
  return EXIT_SUCCESS;
}

/**
 * @brief 在后台 NuttX 任务中启动传感器扩展录制演示。
 *
 * @param outpath  输出 GPX 文件路径
 * @param seconds  录制时长（秒）；<=0 时使用默认 10
 * @return 0 成功；-EBUSY 已有任务；-其他负值为 errno
 */
int gpx_example_record_start(const char *outpath, int seconds)
{
  static char pathbuf[256];
  static char secbuf[16];
  static FAR char *argv[4];
  posix_spawnattr_t attr;
  int pid;

  if (outpath == NULL || outpath[0] == '\0')
    {
      return -EINVAL;
    }

  if (seconds <= 0)
    {
      seconds = 10;
    }

  if (g_example_running)
    {
      return -EBUSY;
    }

  snprintf(pathbuf, sizeof(pathbuf), "%s", outpath);
  snprintf(secbuf, sizeof(secbuf), "%d", seconds);
  argv[0] = (FAR char *)"gpx_exrec";
  argv[1] = pathbuf;
  argv[2] = secbuf;
  argv[3] = NULL;

  g_example_running = true;

  posix_spawnattr_init(&attr);
  attr.priority  = CONFIG_MYVENDOR_GPX_EXAMPLE_PRIORITY;
  attr.stacksize = CONFIG_MYVENDOR_GPX_EXAMPLE_STACKSIZE;

  pid = task_spawn("gpx_exrec", gpx_example_record_task, NULL, &attr,
                   argv, NULL);
  if (pid < 0)
    {
      g_example_running = false;
      return pid;
    }

  gpx_example_log("gpx example: record task started pid=%d (%d s -> %s)",
                  pid, seconds, outpath);
  return 0;
}

/**
 * @brief 查询演示录制是否仍在进行。
 *
 * @return true  后台任务仍在运行
 * @return false 空闲
 */
bool gpx_example_record_is_running(void)
{
  return g_example_running;
}