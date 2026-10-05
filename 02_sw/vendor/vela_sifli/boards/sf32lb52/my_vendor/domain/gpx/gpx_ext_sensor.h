/**
 * @file gpx_ext_sensor.h
 * @brief 传感器扩展：心率 / 踏频 / 速度（gpxtpx v2）+ 功率（Garmin PowerExtension v1）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_EXT_SENSOR_H
#define MY_VENDOR_GPX_EXT_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * format 缓冲区建议容量（gpx_ext_sensor_register 内部 format 上限）。
 *
 * 四个字段同时有效时实测 371 字节（gpxtpx 块带命名空间约 106 + hr/cad/speed 各
 * 约 37/39/45 + 闭合 36，功率元素带命名空间约 116）。这个值会经
 * gpx_ext_fmt_max_per_trkpt() 加到写盘每点的缓冲上，取小了 format() 返回
 * -ENOMEM、整点扩展被丢，所以留足余量。
 */
#define GPX_EXT_SENSOR_FMT_MAX  512

/**
 * @brief 传感器扩展每点 payload。
 *
 * 挂到 gpx_point_t.ext_data[slot] 后，由 gpx_ext_sensor 的 format/parse 读写 GPX XML：
 *   - hr / cad / speed → Garmin **TrackPointExtension v2** 命名空间
 *     （<gpxtpx:hr> bpm、<gpxtpx:cad> rpm、<gpxtpx:speed> **m/s**）。
 *     speed 是 v2 才有的元素（v1 只有 atemp/wtemp/depth/hr/cad），而 v2 的元素是
 *     有序序列，所以按 schema 顺序 hr → cad → speed 写。
 *   - power → Garmin **PowerExtension v1**：<gpxpx:PowerInWatts>（W）。
 *     PowerExtensionv1.xsd 只定义这一个元素、直接挂在 <extensions> 下。gpxtpx 没有
 *     功率字段，而裸 <power> 没有任何 schema 定义，所以不写裸元素。
 *
 * 只写有 schema 定义的字段：**不再有风向** —— Garmin 的 gpxtpx/gpxx 与 gpxdata 都
 * 没有风向元素，只有一份提案里的 twd/tws（非 schema），所以撤掉了原来的自定义
 * mybike:windDir。
 *
 * 各 has_* 为 false 时，对应字段不参与 encode/decode（可只录部分传感器）。
 *
 * 单位提醒：速度按 gpxtpx schema 存 **m/s**（多数工具自己 ×3.6 显示 km/h），不存
 * km/h —— 同一字段混两种单位会让不同工具差 3.6 倍。
 */
typedef struct gpx_sensor_ext
{
  uint16_t hr;       /* 心率 (bpm)；has_hr 为 true 时写入 <gpxtpx:hr> */
  uint16_t cad;      /* 踏频 (rpm)；has_cad 为 true 时写入 <gpxtpx:cad> */
  float speed_mps;   /* 速度 (m/s)；has_speed 为 true 时写入 <gpxtpx:speed> */
  uint16_t power_w;  /* 功率 (W)；has_power 为 true 时写入 <gpxpx:PowerInWatts> */
  bool has_hr;       /* hr 是否有效；false 时不输出/不解析心率 */
  bool has_cad;      /* cad 是否有效；false 时不输出/不解析踏频 */
  bool has_speed;    /* speed_mps 是否有效；false 时不输出/不解析速度 */
  bool has_power;    /* power_w 是否有效；false 时不输出/不解析功率 */
} gpx_sensor_ext_t;

/**
 * @brief 注册传感器扩展（format + parse + print）。
 *
 * @return slot 编号 0 … GPX_EXT_SLOT_MAX-1，失败返回负 errno
 */
int gpx_ext_sensor_register(void);

/**
 * @brief 注销先前注册的传感器扩展 slot。
 *
 * @param slot  gpx_ext_sensor_register() 返回的 slot
 * @return 0 成功，负 errno 失败
 */
int gpx_ext_sensor_unregister(int slot);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_EXT_SENSOR_H */