/**
 * @file gpx_record.h
 * @brief GPX 录制器（句柄 + 独立线程；data 队列写点，cmd 队列控制生命周期）。
 *
 * 每个 gpx_record_create() 分配句柄与队列；工作线程在 start 时创建，
 * stop 后仍存活，release 才 join 并释放。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_RECORD_H
#define MY_VENDOR_GPX_RECORD_H

#include "gpx_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 不透明录制器句柄（每个句柄一个 port 线程）。 */
typedef struct gpx_record gpx_record_t;

/** 录制统计信息。 */
typedef struct gpx_record_stats
{
  uint32_t points_written;   /* 成功写入点数 */
  uint32_t points_dropped;   /* 队列满或 clone 失败丢弃数 */
  uint32_t batches_written;  /* 批写次数 */
} gpx_record_stats_t;

/** 创建录制器时的队列 / 批写配置。 */
typedef struct gpx_record_cfg
{
  unsigned batch_size;   /* N：每批写盘点数，须 > 0 */
  unsigned queue_depth;  /* data 队列容量；0 = ceil(N * 1.2) */
} gpx_record_cfg_t;

/** 当前运行时的 batch / queue 参数。 */
typedef struct gpx_record_runtime
{
  unsigned batch_size;
  unsigned queue_depth;
  bool recording;          /* 文件已 open，可 push */
  bool paused;               /* 暂停刷写（data 仍可入队） */
} gpx_record_runtime_t;

/**
 * @brief 创建录制器句柄（不创建线程；prepare / 首次 start 时拉起工作线程）。
 *
 * @param cfg   批写 / data 队列配置；可为 NULL
 * @param out   输出句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_create(const gpx_record_cfg_t *cfg, gpx_record_t **out);

/**
 * @brief 预拉工作线程，避免首次 start 在调用线程上 pthread_create。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_prepare(gpx_record_t *rec);

/**
 * @brief 投递 start：工作线程异步打开 GPX。调用立即返回，不 round-trip fopen。
 *
 * @param rec       录制器句柄
 * @param filepath  输出路径
 * @param meta      元数据；可为 NULL
 * @return 0 已投递，-EBUSY 已在录制，其他负 errno 失败
 */
int gpx_record_start(gpx_record_t *rec, const char *filepath,
                     const gpx_meta_t *meta);

/**
 * @brief 投递 start：工作线程异步打开已有 GPX 并追加轨迹点。
 *
 * 文件须已去掉 footer；不写 GPX 头。
 *
 * @param rec       录制器句柄
 * @param filepath  已有 GPX 路径
 * @param meta      元数据；可为 NULL（追加时不写头）
 * @return 0 已投递，-EBUSY 已在录制，其他负 errno 失败
 */
int gpx_record_start_append(gpx_record_t *rec, const char *filepath,
                            const gpx_meta_t *meta);

/**
 * @brief 向 data 队列 push 轨迹点（扩展 payload 深拷贝）。
 *
 * @param rec    录制器句柄
 * @param point  输入点
 * @return 0 成功；-EAGAIN 未 start；-ENOSPC 队列满；其他负 errno
 */
int gpx_record_push(gpx_record_t *rec, const gpx_point_t *point);

/**
 * @brief 暂停刷写（data 仍可 push，恢复后继续写盘）。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_pause(gpx_record_t *rec);

/**
 * @brief 恢复刷写。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_resume(gpx_record_t *rec);

/**
 * @brief 结束当前录制会话：投递 STOP 后立即返回。工作线程刷尾批并 close。
 *
 * @param rec  录制器句柄
 * @return 0 已投递或已空闲，负 errno 失败
 */
int gpx_record_stop(gpx_record_t *rec);

/**
 * @brief 等到 start fopen 或 stop close 结束。未在 I/O 则立即返回。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_wait_close(gpx_record_t *rec);

/**
 * @brief 释放录制器：发送 release 命令，join 线程并 free 句柄。
 *
 * 若仍在录制，会先 stop 再退出。调用后 *rec 置 NULL。
 *
 * @param rec  句柄指针（可为 NULL）
 * @return 0 成功，负 errno 失败
 */
int gpx_record_release(gpx_record_t **rec);

/**
 * @brief 是否处于 start 已投递或文件已打开、尚未 stop。
 *
 * @param rec  录制器句柄
 * @return true 正在录制（含 fopen 尚未完成）
 */
bool gpx_record_is_recording(gpx_record_t *rec);

/**
 * @brief 工作线程正在 close/fsync（STOP 已投递、文件尚未关完）。
 *
 * @param rec  录制器句柄
 * @return true 正在落盘关闭
 */
bool gpx_record_is_closing(gpx_record_t *rec);

/**
 * @brief 工作线程是否仍在运行（create 后、release 前）。
 *
 * @param rec  录制器句柄
 * @return true 线程存活
 */
bool gpx_record_is_alive(const gpx_record_t *rec);

/**
 * @brief 获取统计信息。
 *
 * @param rec    录制器句柄
 * @param stats  输出统计
 * @return 0 成功，负 errno 失败
 */
int gpx_record_get_stats(gpx_record_t *rec, gpx_record_stats_t *stats);

/**
 * @brief 获取运行时参数与状态。
 *
 * @param rec      录制器句柄
 * @param runtime  输出
 * @return 0 成功，负 errno 失败
 */
int gpx_record_get_runtime(gpx_record_t *rec, gpx_record_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_RECORD_H */