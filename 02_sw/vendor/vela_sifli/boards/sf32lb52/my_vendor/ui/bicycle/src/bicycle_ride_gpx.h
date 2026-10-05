/**
 * @file bicycle_ride_gpx.h
 * @brief 骑行 REC：把轨迹点写入 /mnt/lfs/mtp/record 下的 GPX。
 */

#ifndef BICYCLE_RIDE_GPX_H
#define BICYCLE_RIDE_GPX_H

#include "bicycle_gpx_sim.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 只修正在录的那条（`.rec_active`）。工厂页无 splash 时用。 */
void bicycle_ride_gpx_salvage_dir(void);
/**
 * @brief 开机白屏泵一次：先 `.rec_active`，再扫目录（每拍最多 1 个）。
 *        能解析出末点则写入上次骑行终点，供地图开机中心使用。
 * @return 扫完为 true。
 */
bool bicycle_ride_gpx_salvage_boot_pump(void);

/**
 * @brief 开机扫过的 `.gpx` 个数（不含目录里其它文件）。
 * @return 已 `readdir` 到的轨迹文件数。splash 用来区分「无记录」和「已检查」。
 */
unsigned bicycle_ride_gpx_salvage_scanned(void);

/**
 * @brief 闭合 footer 或丢掉空/坏文件的条数。
 * @return 真正改过磁盘的次数。splash 右侧「已恢复」看这个。
 */
unsigned bicycle_ride_gpx_salvage_fixed(void);

/**
 * @brief splash 结束：可弹「已恢复」；白屏没扫完则后台继续。
 */
void bicycle_ride_gpx_salvage_scan(void);

/** @brief 占住空 TRK_*.gpx 并开始录；已在录制则为 no-op。 */
int bicycle_ride_gpx_begin(void);

/**
 * @brief 复制 src 到新的 TRK_*.gpx，去掉 footer，供下次 begin 追加。
 *
 * 不改、不删源文件。已在录制则失败。
 *
 * @param src  已有 GPX 完整路径
 * @return 0 成功，负 errno 失败
 */
int bicycle_ride_gpx_prepare_continue(const char * src);

/** @brief 放弃尚未 begin 的续录副本。 */
void bicycle_ride_gpx_cancel_continue(void);

/**
 * @brief 有效定位且未暂停时采点（约 1 Hz）。
 * @param fix 当前 GNSS 定位。
 * @details 磁盘满（`-ENOSPC`）后本趟停止写入，且失败也会节流，避免每点重试拖垮 UI。
 */
void bicycle_ride_gpx_push_fix(const bicycle_gnss_fix_t * fix);

/** @brief 刷盘关文件并保留；立即返回，close/fsync 在 gpx_record 线程。path 可为空。 */
int bicycle_ride_gpx_commit(char * path, size_t path_n);

/** @brief 关文件并删除（不保存）。立即返回，unlink 等 close 之后。 */
int bicycle_ride_gpx_discard(void);

/** @brief 等到当前 GPX close/fsync 结束（关机前调用）。 */
void bicycle_ride_gpx_wait_idle(void);

/**
 * @brief 本趟是否已占住一条 TRK_*.gpx 正在录。
 * @return 录制中为 true。
 */
bool bicycle_ride_gpx_active(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_RIDE_GPX_H */
