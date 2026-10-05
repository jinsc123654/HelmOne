/**
 * @file gpx_example_record.h
 * @brief 例程录制：采样辅助 API + 后台 NuttX task。
 *
 * 轨迹点与扩展 payload 分别使用 gpx_types.h / gpx_ext_sensor.h 中的类型。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_EXAMPLE_RECORD_H
#define MY_VENDOR_GPX_EXAMPLE_RECORD_H

#include "gpx_ext_sensor.h"
#include "gpx_types.h"

#include <stdbool.h>

#include <nuttx/compiler.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单次录制会话的空间起点（例程随机；产品可换为首点 GPS）。 */
typedef struct gpx_example_origin
{
  float lat;  /* 起点纬度 (°，WGS84) */
  float lon;  /* 起点经度 (°，WGS84) */
  float ele;  /* 起点海拔 (m) */
} gpx_example_origin_t;

/**
 * @brief 生成随机起点（北京附近区域）。
 *
 * 产品集成：可改为读首点 GPS，或固定测试坐标。
 *
 * @param origin  输出起点
 * @param salt    盐值（多录制器时用 0/1/2 区分）
 */
void gpx_example_origin_random(gpx_example_origin_t *origin, unsigned salt);

/**
 * @brief 填充演示轨迹点及传感器扩展（例程用公式 + 系统时钟）。
 *
 * 产品集成：在此替换为真实 GPS / 传感器读数，直接写入 point 与 sensor。
 *
 * @param point   输出 GPX 轨迹点
 * @param sensor  输出扩展 payload（挂到 point->ext_data[slot]）
 * @param origin  会话起点
 * @param index   点序号（0 …）
 * @param slot    gpx_ext_sensor_register() 返回的 slot
 */
void gpx_example_fill_point(gpx_point_t *point, gpx_sensor_ext_t *sensor,
                            const gpx_example_origin_t *origin,
                            int index, int slot);

/**
 * @brief 根据录制秒数计算批写大小 N。
 *
 * @param seconds  录制秒数
 * @return batch_size
 */
unsigned gpx_example_batch_size(int seconds);

/**
 * @brief 在后台 NuttX 任务中启动传感器扩展录制演示。
 *
 * @param outpath  输出 GPX 文件路径
 * @param seconds  录制时长（秒）；<=0 时使用默认 10
 * @return 0 成功；-EBUSY 已有任务在运行；其他负值为 errno
 */
int gpx_example_record_start(const char *outpath, int seconds);

/**
 * @brief 录制演示任务入口（由 task_spawn 调用）。
 *
 * @param argc  参数个数
 * @param argv  argv[1]=输出路径，argv[2]=可选秒数
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
int gpx_example_record_task(int argc, FAR char *argv[]);

/**
 * @brief 查询演示录制是否仍在进行。
 *
 * @return true  后台任务仍在运行
 * @return false 空闲
 */
bool gpx_example_record_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_EXAMPLE_RECORD_H */