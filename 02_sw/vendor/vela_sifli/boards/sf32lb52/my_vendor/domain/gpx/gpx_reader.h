/**
 * @file gpx_reader.h
 * @brief 增量式 GPX 轨迹点解码器（<trkpt> … </trkpt>）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_READER_H
#define MY_VENDOR_GPX_READER_H

#include "gpx_types.h"
#include "gpx_port.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * <extensions> … </extensions> 正文的读取块上限（字节）。
 *
 * 取的是 **max(本组件已注册扩展的 fmt_max 之和, 这个值)**：我们自己的块约
 * 410 字节，但读的多半是别人的文件 —— Garmin 把 atemp/wtemp/depth/hr/cad/
 * speed/course/bearing 全带上、再叠几个命名空间就到 600～800 字节。块不够时
 * `read_until_marker` 只存前缀、后面的元素静默丢失（轨迹点本身不受影响）。
 *
 * 该块是**临时**的：优先用调用方的 io->scratch（gpx_decode 的 scratch 是
 * 1024），不够时单次 malloc，读完即放；分配走 gpx_port_malloc → PSRAM。
 * 与写盘无关（写盘每点缓冲按用户注册的 fmt_max 算）。
 */
#ifndef GPX_EXT_PARSE_BLOCK_MAX
#  define GPX_EXT_PARSE_BLOCK_MAX  1024
#endif

/** 字节流 I/O 抽象（可绑定文件、内存等后端）。 */
typedef struct gpx_io
{
  int (*available)(void *ctx);  /* 是否可读；可选 */
  int (*read)(void *ctx);       /* 读一字节；EOF/错误返回负值 */
  void *ctx;                    /* 后端上下文 */
  char *scratch;                /* 可选：PSRAM 扩展块，避免每点 malloc */
  size_t scratch_cap;
} gpx_io_t;

/** gpx_reader_next() 返回值。 */
typedef enum gpx_read_result
{
  GPX_READ_OK = 0,    /* 成功读到一个点 */
  GPX_READ_EOF = 1,   /* 无更多 <trkpt> */
  GPX_READ_ERR = -1,  /* 解析或 I/O 错误 */
} gpx_read_result_t;

/**
 * @brief 从流中读取下一个轨迹点。
 *
 * @param io     字节流
 * @param point  输出轨迹点；解码扩展前须绑定 ext_data[slot] 且置 ext_mask
 * @return GPX_READ_OK / GPX_READ_EOF / GPX_READ_ERR
 */
gpx_read_result_t gpx_reader_next(gpx_io_t *io, gpx_point_t *point);

/**
 * @brief 跳过一个 <trkpt>…</trkpt>（不解析 ele/time/扩展）。
 *
 * @param io     字节流
 * @param point  非 NULL 时写入 lat/lon；NULL 则只前进流
 * @return GPX_READ_OK / GPX_READ_EOF / GPX_READ_ERR
 */
gpx_read_result_t gpx_reader_skip(gpx_io_t *io, gpx_point_t *point);

/**
 * @brief 格式化已解码轨迹点为可读文本（标准字段 + 已注册扩展 print）。
 *
 * @param point  轨迹点
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 写入字节数（>0），失败返回负 errno
 */
int gpx_reader_format_trkpt(const gpx_point_t *point, char *buf, size_t size);

/** 单行 decode 输出建议上限（含扩展 print）。 */
#define GPX_READER_LINE_MAX  320

/** POSIX 文件后端（见 gpx_port_file_t）。 */
typedef gpx_port_file_t gpx_file_io_t;

/**
 * @brief 以只读方式打开 GPX 文件。
 *
 * @param fio   文件 I/O 上下文
 * @param path  文件路径
 * @return 0 成功，负 errno 失败
 */
int gpx_file_io_open(gpx_file_io_t *fio, const char *path);

/**
 * @brief 关闭 GPX 文件。
 *
 * @param fio  文件 I/O 上下文
 */
void gpx_file_io_close(gpx_file_io_t *fio);

/**
 * @brief 将 gpx_file_io 绑定到 gpx_io 字节流接口。
 *
 * @param fio  文件 I/O 上下文
 * @param io   输出 gpx_io 实例
 */
void gpx_file_io_bind(gpx_file_io_t *fio, gpx_io_t *io);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_READER_H */