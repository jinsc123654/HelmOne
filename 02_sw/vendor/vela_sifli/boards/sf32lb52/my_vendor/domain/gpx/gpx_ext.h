/**
 * @file gpx_ext.h
 * @brief 可插拔 GPX 1.1 <extensions> 注册表（每 slot 独立 format / parse / print）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_EXT_H
#define MY_VENDOR_GPX_EXT_H

#include "gpx_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GPX_EXT_SLOT_MAX
#  define GPX_EXT_SLOT_MAX  8
#endif

/* <extensions> 包裹标签固定开销："    <extensions>\n" + "    </extensions>\n" */

#define GPX_EXT_WRAP_OVERHEAD  32

/**
 * @brief ext_data 是否像可写 RAM（SRAM/PSRAM）。
 *
 * 排除 NULL、0xffffffff、flash、未对齐垃圾。解码端须同时置 ext_mask，
 * 否则栈残留指针会被当成 payload。
 */
static inline bool gpx_ext_data_bound(const void *p)
{
  uintptr_t a = (uintptr_t)p;

  return p != NULL && (a & 3u) == 0 && a >= 0x20000000ul;
}

/**
 * @brief 格式化单个轨迹点扩展块（写入 <extensions> 内部，不含外层包裹）。
 *
 * @param data  每点扩展结构体指针（registered data_size 字节）
 * @param buf   输出缓冲区
 * @param size  缓冲区容量（字节）
 * @return 写入字节数（>0），无内容时 0，失败返回负 errno
 */
typedef int (*gpx_ext_format_fn)(const void *data, char *buf, size_t size);

/**
 * @brief 解析 <extensions> … </extensions> 内部 XML 片段。
 *
 * @param xml   扩展 XML（不必 NUL 结尾，使用 len）
 * @param len   xml 长度（字节）
 * @param data  输出结构体（registered data_size 字节）
 * @return 0 匹配并解析成功，1 不匹配（跳过），负 errno 错误
 */
typedef int (*gpx_ext_parse_fn)(const char *xml, size_t len, void *data);

/**
 * @brief 将已解析扩展格式化为可读文本（解码 / NSH 打印用）。
 *
 * @param data  每点扩展结构体指针
 * @param buf   输出文本（通常追加在 lat/lon/ele/time 之后）
 * @param size  缓冲区容量（字节）
 * @return 写入字节数（>0），无内容时 0，失败返回负 errno
 */
typedef int (*gpx_ext_print_fn)(const void *data, char *buf, size_t size);

#ifndef GPX_EXT_PRINT_DEFAULT_MAX
#  define GPX_EXT_PRINT_DEFAULT_MAX  64
#endif

/** 扩展注册描述符。 */
typedef struct gpx_ext_desc
{
  const char *name;           /* 调试 / 日志标签 */
  size_t data_size;           /* 每点 C 结构体大小（parse 输出用） */
  size_t fmt_max;             /* format() 单点 XML 最大字节数 */
  size_t print_max;           /* print() 单点文本最大字节数；0 用默认 64 */
  gpx_ext_format_fn format;   /* 必填：格式化回调 */
  gpx_ext_parse_fn parse;     /* 可选；NULL 表示只写不读 */
  gpx_ext_print_fn print;     /* 可选；NULL 表示解码时不显示 */
} gpx_ext_desc_t;

/**
 * @brief 注册扩展（format 必填，parse / print 可选）。
 *
 * @param desc  扩展描述符
 * @return slot 编号 0 … GPX_EXT_SLOT_MAX-1，失败返回负 errno
 */
int gpx_ext_register(const gpx_ext_desc_t *desc);

/**
 * @brief 注销指定 slot 的扩展。
 *
 * @param slot  先前 gpx_ext_register() 返回的 slot
 * @return 0 成功，负 errno 失败
 */
int gpx_ext_unregister(int slot);

/** 注销所有已注册扩展。 */
void gpx_ext_unregister_all(void);

/**
 * @brief 查询当前已注册扩展数量。
 *
 * @return 已占用 slot 个数
 */
unsigned gpx_ext_registered_count(void);

/**
 * @brief 查询指定 slot 的 data_size。
 *
 * @param slot  slot 编号
 * @return data_size；未使用 slot 返回 0
 */
size_t gpx_ext_data_size(int slot);

/**
 * @brief 计算所有已注册扩展单点 fmt_max 之和，含 <extensions> 包裹开销。
 *
 * @return 单点 XML 扩展部分理论最大字节数；无注册扩展时 0
 */
size_t gpx_ext_fmt_max_per_trkpt(void);

/**
 * @brief 格式化轨迹点上所有活跃扩展（含 <extensions> 包裹）。
 *
 * @param point  轨迹点（ext_mask / ext_data 有效）
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 0 无内容，>0 写入字节数，负 errno 失败
 */
int gpx_ext_format_trkpt(const gpx_point_t *point, char *buf, size_t size);

/**
 * @brief 将 <extensions> 内部 XML 解析到 point->ext_data[slot]。
 *
 * 对已注册 parse 的 slot，ext_data[slot] 须指向至少 gpx_ext_data_size(slot)
 * 字节的缓冲区；未绑定或指针非法则跳过该 slot，不写。
 * 解析成功时设置 ext_mask 对应位。
 *
 * @param xml    扩展 XML 片段
 * @param len    xml 长度（字节）
 * @param point  输出轨迹点
 * @return 0 至少一个扩展匹配，1 无匹配，负 errno 错误
 */
int gpx_ext_parse_trkpt(const char *xml, size_t len, gpx_point_t *point);

/**
 * @brief 打印轨迹点上所有活跃扩展的可读字段（空格前缀字段）。
 *
 * @param point  轨迹点
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 0 无内容，>0 写入字节数，负 errno 失败
 */
int gpx_ext_print_trkpt(const gpx_point_t *point, char *buf, size_t size);

/**
 * @brief 深拷贝 src 的扩展 payload 到 dst（每 slot gpx_port_malloc）。
 *
 * 不拷贝标准字段，仅 ext_mask / ext_data。
 *
 * @param dst  目标轨迹点
 * @param src  源轨迹点
 * @return 0 成功，负 errno 失败（失败时释放 dst 已分配扩展）
 */
int gpx_ext_point_clone_ext(gpx_point_t *dst, const gpx_point_t *src);

/**
 * @brief 释放 point 拥有的所有 ext_data[slot] 并清零 ext_mask。
 *
 * @param point  轨迹点
 */
void gpx_ext_point_free_ext(gpx_point_t *point);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_EXT_H */