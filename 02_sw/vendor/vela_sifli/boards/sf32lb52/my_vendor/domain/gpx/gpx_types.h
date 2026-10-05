/**
 * @file gpx_types.h
 * @brief GPX 组件公共类型定义。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_TYPES_H
#define MY_VENDOR_GPX_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifndef GPX_EXT_SLOT_MAX
#  define GPX_EXT_SLOT_MAX  8
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** GPX ISO8601 时间（UTC，Z 后缀）。 */
typedef struct gpx_time
{
  int16_t year;    /**< 年。 */
  uint8_t month;   /**< 月 1～12。 */
  uint8_t day;     /**< 日。 */
  uint8_t hour;    /**< 时。 */
  uint8_t minute;  /**< 分。 */
  uint8_t second;  /**< 秒。 */
} gpx_time_t;

/** 单个轨迹点：标准字段 + 可插拔扩展 payload。 */
typedef struct gpx_point
{
  float latitude;                    /**< 纬度 (WGS84)。 */
  float longitude;                   /**< 经度 (WGS84)。 */
  float altitude;                    /**< 海拔 (m)。 */
  gpx_time_t time;                   /**< 点时间。 */
  bool has_altitude;                 /**< altitude 是否有效。 */
  bool has_time;                     /**< time 是否有效。 */
  uint8_t ext_mask;                  /**< 位 i 置位表示 ext_data[i] 有效。 */
  void *ext_data[GPX_EXT_SLOT_MAX];  /**< 各 slot 扩展 payload 指针。 */
} gpx_point_t;

/** GPX 文件 / 轨迹元数据（字符串由调用方或 record 模块持有）。 */
typedef struct gpx_meta
{
  const char *creator;      /* <gpx creator="…"> */
  const char *meta_name;    /* <metadata><name> */
  const char *meta_desc;    /* <metadata><desc> */
  const char *track_name;   /* <trk><name> */
  const char *track_desc;   /* <trk><desc> */
} gpx_meta_t;

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_TYPES_H */