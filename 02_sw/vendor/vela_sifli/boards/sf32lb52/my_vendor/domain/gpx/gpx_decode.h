/**
 * @file gpx_decode.h
 * @brief GPX 解码器（句柄 + 同步拉取；无独立线程）。
 *
 * 底层仍使用 gpx_reader_next() 流式解析；本层封装 open / 批量 read /
 * stride 抽稀（stride=0 时按文件大小自动）/ limit / close 与统计。
 * 跳过的点走 gpx_reader_skip()，不解析 ele/time/扩展。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_DECODE_H
#define MY_VENDOR_GPX_DECODE_H

#include "gpx_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 不透明解码句柄（单线程同步使用）。 */
typedef struct gpx_decode gpx_decode_t;

/** 打开解码器时的 read 行为配置。 */
typedef struct gpx_decode_cfg
{
  unsigned batch_max;  /* 单次 read 最多返回点数；0 = 默认 32 */
  unsigned stride;     /* 抽稀步长：1=全要；0=按文件大小自动（需 limit>0） */
  unsigned limit;      /* 累计最多输出点数（含首末）；0 = 不限 */
} gpx_decode_cfg_t;

#ifndef GPX_DECODE_POINT_MAX
#  define GPX_DECODE_POINT_MAX  2048u
#endif

#ifndef GPX_DECODE_TRKPT_AVG_BYTES
#  define GPX_DECODE_TRKPT_AVG_BYTES  140u
#endif

/**
 * @brief 填自动抽稀配置：最多 limit 点（0 则 GPX_DECODE_POINT_MAX）。
 *
 * stride=0，打开时按文件体积估算步长，跳过点不解析 ele/time/扩展，
 * 始终保留首点与末点。
 */
void gpx_decode_cfg_sparse(gpx_decode_cfg_t *cfg, unsigned limit);

/** 解码统计（get_stats 输出快照）。 */
typedef struct gpx_decode_stats
{
  uint32_t points_delivered;  /* 已返回给调用方的点数 */
  uint32_t points_skipped;    /* stride 跳过的文件点数 */
  uint32_t file_points;       /* 已从文件成功解析的 trkpt 总数 */
  double length_m;            /* 全文件相邻点折线里程（含跳过点） */
  bool eof;                   /* 文件已无更多 trkpt */
  bool limit_reached;         /* 已达 cfg.limit */
} gpx_decode_stats_t;

/**
 * @brief 打开 GPX 文件并创建解码句柄。
 *
 * @param path  GPX 文件路径
 * @param cfg   read 配置；NULL 使用默认（batch_max=32, stride=1, limit=0）
 * @param out   输出句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_decode_open(const char *path, const gpx_decode_cfg_t *cfg,
                    gpx_decode_t **out);

/**
 * @brief 绑定扩展 payload 缓冲（解码 HR 等时用；预览可不绑）。
 *
 * 必须绑到句柄，不要把栈垃圾指针放进 points[].ext_data。
 *
 * @param dec   解码句柄
 * @param slot  gpx_ext_register 返回的 slot
 * @param buf   每点 payload（至少 gpx_ext_data_size(slot) 字节）
 * @return 0 成功，负 errno 失败
 */
int gpx_decode_bind_ext(gpx_decode_t *dec, int slot, void *buf);

/**
 * @brief 批量读取轨迹点（同步，当前线程阻塞）。
 *
 * 扩展字段：先 gpx_decode_bind_ext()。read 会清零输出点再套绑定，
 * 栈上未初始化的 batch 不会再被当成 ext_data。
 *
 * @param dec     解码句柄
 * @param points  输出数组
 * @param max     本次最多读取点数（通常 ≤ batch_max）
 * @param n_out   实际写入点数
 * @return 0 成功且文件可能还有更多点；
 *         1 EOF 或 limit 已达（*n_out 为本次读到的数量，可为 0）；
 *         负 errno 解析 / I/O 错误
 */
int gpx_decode_read(gpx_decode_t *dec, gpx_point_t *points, unsigned max,
                    unsigned *n_out);

/**
 * @brief 关闭文件并释放解码句柄。
 *
 * @param dec  句柄指针；成功后置 *dec = NULL
 */
void gpx_decode_close(gpx_decode_t **dec);

/**
 * @brief 获取解码统计快照。
 *
 * @param dec    解码句柄
 * @param stats  输出统计
 * @return 0 成功，-EINVAL 参数无效
 */
int gpx_decode_get_stats(gpx_decode_t *dec, gpx_decode_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_DECODE_H */