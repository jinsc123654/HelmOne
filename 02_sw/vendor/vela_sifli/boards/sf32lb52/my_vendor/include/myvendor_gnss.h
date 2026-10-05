/**
 * @file myvendor_gnss.h
 * @brief 板载 u-blox MAX-M10S-00B-01 缓存：读 NMEA，供 sys / 码表转发。
 *
 * 模组订货号 MAX-M10S-00B-01。USART2 和 NSH 一样一直 open 并 poll 收；
 * 低功耗只切 PA43 模组 VCC。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_GNSS_H
#define MYVENDOR_GNSS_H

#include "myvendor_sys.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 NMEA 读取线程（可重复调用）。
 * @return 0 成功，负 errno；未编 L96 时为 -ENOTSUP。
 */
int myvendor_gnss_start(void);

/**
 * @brief 拷贝未过期的板载快照（搜星或定位）。
 * @param out 输出；NULL 则只查询是否有近期 NMEA。
 * @return 近期有 NMEA 则为 true；是否已定位看 @a out->valid。
 */
bool myvendor_gnss_get(myvendor_sys_gnss_t *out);

/**
 * @brief 星历上次写入时间（UTC unix）与建议下次同步时间。
 * @param last_utc 上次同步，可为 NULL。
 * @param next_utc 建议下次同步（上次 + 4 小时），可为 NULL。
 * @return 有有效星历文件则为 true。
 *
 * GPS 广播星历常用窗口约 4 小时；过期后请用 App 重新同步。
 * 下发星历和模组 dump 都写成 mga_<utc>.ubx，按文件名 UTC 一共保留 5 份。
 */
bool myvendor_gnss_eph_times(uint32_t *last_utc, uint32_t *next_utc);

/**
 * @brief 请求 GNSS 线程立刻把 `/mnt/lfs/eph` 最新 `mga_<utc>.ubx` 注入模组。
 *
 * 给 BLE/MTP 写完星历后热加载用；真正灌串口只在 GNSS 线程。
 * 未编 L96 时为空操作。
 */
void myvendor_gnss_eph_reload(void);

/**
 * @brief 立刻把 `/mnt/lfs/eph` 里多余的 `mga_*.ubx` 删到最多 5 份。
 *
 * 开机、dump 写完、注入前都会走；不依赖 eph_auto。未编 L96 时为空操作。
 */
void myvendor_gnss_eph_maintain(void);

/**
 * @brief 正在把最新 mga_*.ubx 灌进模组，或刚灌完在等 NMEA（开机、断电后、手机热加载、dump_in）。
 */
bool myvendor_gnss_eph_busy(void);

/**
 * @brief 上次注入用的文件路径。
 * @return 有路径则为 true。
 */
bool myvendor_gnss_inject_last_path(char *buf, size_t n);

/** 关机前倒星历的结果，给关机动画逐条说明。 */
typedef enum {
    MYVENDOR_GNSS_OFF_DUMPED = 0,     /**< 已写入 mga_<utc>.ubx。 */
    MYVENDOR_GNSS_OFF_SKIP_IDLE,      /**< 静止休眠，模组已下电。 */
    MYVENDOR_GNSS_OFF_SKIP_FACTORY,   /**< 工厂固件不倒库。 */
    MYVENDOR_GNSS_OFF_SKIP_AUTO,      /**< 星历自动同步关闭。 */
    MYVENDOR_GNSS_OFF_SKIP_NOFIX,     /**< 当前无定位。 */
    MYVENDOR_GNSS_OFF_SKIP_NODATA,    /**< 等不到 DBD。 */
    MYVENDOR_GNSS_OFF_SKIP,           /**< 未启动等其它跳过。 */
    MYVENDOR_GNSS_OFF_TIMEOUT         /**< 等待倒库超时。 */
} myvendor_gnss_off_t;

/**
 * @brief 关机前把导航库旁路倒成 mga_<utc>.ubx（NMEA 不停）。
 *
 * 仅在星历自动同步开启时执行。关闭自动同步、无定位、静止休眠已下电、
 * 或 5 秒收不到 DBD 则跳过。倒库在 GNSS 读线程；本函数只短等完成。
 * 应在切断电源前调用；未编 L96 时为空操作。
 */
myvendor_gnss_off_t myvendor_gnss_prepare_poweroff(void);

/**
 * @brief 静止休眠：按 MAX-M10S-00B-01 手册切模组 VCC（PA43）。
 *
 * USART2 和 NSH 一样保持 open 并继续 poll；只关模组电源。
 * @param sleep true 下电停车，false 上电并恢复 NMEA。
 */
void myvendor_gnss_idle_sleep(bool sleep);

/**
 * @brief 当前是否处于静止休眠（GNSS 已请求下电）。
 */
bool myvendor_gnss_idle_get(void);

/**
 * @brief 请求 GNSS 线程旁路倒库到 /mnt/lfs/eph/mga_<utc>.ubx（NMEA 不停）。
 *
 * 需要当前有有效定位。真正收包只在 GNSS 线程。
 *
 * @return 0 已排队；-ENODEV 未启动；-EBUSY 正在倒库或正在注入；-ENODATA 无定位；
 *         -EACCES 工厂固件禁止倒库；-ENOTSUP 未编 L96。
 */
int myvendor_gnss_dump_request(void);

/**
 * @brief 倒库请求已排队或正在收 DBD。
 */
bool myvendor_gnss_dump_busy(void);

/**
 * @brief 上次 dump_out 写到的文件路径。
 * @return 有路径则为 true。
 */
bool myvendor_gnss_dump_last_path(char *buf, size_t n);

/**
 * @brief 请求把最新 mga_*.ubx 灌进模组（ctl dump_in）。
 *
 * @return 0 已排队；-ENODEV 未启动；-EBUSY 正在倒库或正在注入；
 *         -ENOTSUP 未编 L96。
 */
int myvendor_gnss_inject_request(void);

/**
 * @brief 诊断：读线程心跳新鲜，且近期有 NMEA（无定位也算健康）。
 *
 * 断电/灌星历/倒库/静止下电窗口内只看心跳，避免刚 kick 完又被判不健康。
 */
bool myvendor_gnss_diag_ok(void);

/**
 * @brief 诊断：只切 MAX-M10S-00B-01 VCC（PA43），不 close UART；心跳超时才重拉读线程。
 */
int myvendor_gnss_diag_restart(void);

/** CFG-NAVSPG-DYNMODEL：自行车。水平速度大约 30 m/s。 */
#define MYVENDOR_GNSS_DYN_BIKE  10u
/** CFG-NAVSPG-DYNMODEL：车载。汽车 / 长下坡用，避免 SOG 被 sanity 成 0。 */
#define MYVENDOR_GNSS_DYN_AUTO  4u

/**
 * @brief 请求 GNSS 线程改动态模型（UBX-CFG-VALSET RAM）。
 *
 * 真正写串口只在 GNSS 线程。默认自行车；轨迹均速已经出来且
 * 明显高于自行车包络、或 SOG 被 sanity 成 0 时切车载。
 * 未编 L96 时为空操作。
 *
 * @param model @c MYVENDOR_GNSS_DYN_BIKE 或 @c MYVENDOR_GNSS_DYN_AUTO。
 */
void myvendor_gnss_dynmodel_set(uint8_t model);

/**
 * @brief splash / 主界面已起来，允许 GNSS 线程再灌星历。
 *
 * 开机先出 NMEA、把 LittleFS 留给 splash 地图；灌 DBD 会占 UART 十几秒。
 * 未编 L96 时为空操作。
 */
void myvendor_gnss_ui_ready(void);

/**
 * @brief 停车切 `CFG-NAVSPG-DYNMODEL = stationary(2)`（零速约束）的开关。
 *
 * @details
 * **默认关闭**（2026-09-18）。它的本意是室内/车棚静止时靠零速约束维持解，但
 * 2026-09-17 那次骑行的轨迹质量回退把它顶上了嫌疑名单：进入条件里的"没有可信
 * 速度就算停"在骑行中也能凑满 20 s，而一旦进去，模组自己把速度压向 0 ——
 * **恰好压住了它的退出判据**（要连续 3 s 可信速度 > 3 km/h），于是可能多停几秒，
 * 期间位置被拖住、速度被压低。
 *
 * 机制完整保留，`ctl gnss still on` 可随时打开做 A/B。
 *
 * @param on true 打开自动切换；false 关闭并把当前状态清回（模型交回 automotive）。
 */
void myvendor_gnss_dyn_still_enable(bool on);

/**
 * @brief 星座实验（供 `ctl gnss bdsonly|b1c|ver`）：把"北斗不出句子"分成
 *        "前端频带"还是"模组没解调"两半。
 *
 * @param mode 0=无；1=只留北斗；2=恢复；3=试开 B1C（1575.42，与 GPS L1 同频）；
 *             4=立刻打一行 UBX-MON-VER（固件/硬件版本）。
 *
 * @note 真正写串口只在 GNSS 线程；本函数只落一个请求位，50 ms 内生效。
 *       只留北斗是**临时**的：模组掉电即恢复，`mode=2` 立即恢复。
 */
void myvendor_gnss_probe(int mode);

/**
 * @brief 崩溃 note 用：最近一次 link 行 + 当前 idle/eph。IRQ 安全。
 * @return 写入字节数（不含 NUL）；buf 太小则为 0。
 */
int myvendor_gnss_crash_format(char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_GNSS_H */
