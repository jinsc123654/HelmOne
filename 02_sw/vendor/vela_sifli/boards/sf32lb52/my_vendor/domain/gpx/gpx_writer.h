/**
 * @file gpx_writer.h
 * @brief 流式 GPX 1.1 轨迹写入器（POSIX 文件 I/O）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_WRITER_H
#define MY_VENDOR_GPX_WRITER_H

#include "gpx_types.h"
#include "gpx_ext.h"
#include "gpx_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GPX 写入器句柄。 */
typedef struct gpx_writer
{
  gpx_port_file_t file;  /* 输出文件 */
  bool open;             /* 是否已 open 且 trkseg 未关闭 */
} gpx_writer_t;

/**
 * @brief 创建 GPX 文件并写入头部与 <trkseg> 起始标签。
 *
 * @param writer  写入器句柄
 * @param path    输出文件路径（自动创建父目录）
 * @param meta    元数据；可为 NULL
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_open(gpx_writer_t *writer, const char *path,
                    const gpx_meta_t *meta);

/**
 * @brief 打开已有 GPX 继续写 <trkpt>，不写头部。
 *
 * 调用方须先去掉 </trkseg></trk></gpx>。close 时再写尾。
 *
 * @param writer  写入器句柄
 * @param path    已存在的 GPX 路径
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_open_append(gpx_writer_t *writer, const char *path);

/**
 * @brief 将轨迹点格式化为 GPX XML 文本（含扩展）。
 *
 * @param point  轨迹点
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 写入字节数（>0），失败返回负 errno
 */
int gpx_writer_format_trkpt(const gpx_point_t *point, char *buf, size_t size);

/**
 * @brief 格式化并写入单个轨迹点。
 *
 * @param writer  写入器句柄
 * @param point   轨迹点
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_write_trkpt(gpx_writer_t *writer, const gpx_point_t *point);

/**
 * @brief 向 GPX 文件写入原始字节（批写用）。
 *
 * @param writer  写入器句柄
 * @param data    数据指针
 * @param len     字节数
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_write_bytes(gpx_writer_t *writer, const void *data, size_t len);

/**
 * @brief 尽量把缓存 write() 进文件系统（不 fsync）。
 *
 * @param writer  写入器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_flush(gpx_writer_t *writer);

/**
 * @brief 把已写入的 GPX 刷到存储（含 fsync）。仅 close 路径使用。
 *
 * @param writer  写入器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_sync(gpx_writer_t *writer);

/**
 * @brief 写入 </trkseg></trk> 尾部并关闭文件。
 *
 * @param writer  写入器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_close(gpx_writer_t *writer);

/** 不含扩展的标准 <trkpt> 单点 XML 理论最大字节数。 */
#define GPX_TRKPT_BASE_FMT_MAX  256

/**
 * @brief 计算单点格式化最大字节数（标准字段 + 已注册扩展）。
 *
 * @return 单点 XML 最大字节数
 */
size_t gpx_writer_trkpt_fmt_max(void);

/** @deprecated 请使用 gpx_writer_trkpt_fmt_max()；仅含标准字段大小。 */
#define GPX_TRKPT_FMT_MAX  GPX_TRKPT_BASE_FMT_MAX

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_WRITER_H */