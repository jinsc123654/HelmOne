/**
 * @file myvendor_watchdog.h
 * @brief 硬件 IWDT：idle/work 停则停喂；UI 停且 work 也停则复位。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_WATCHDOG_H
#define MYVENDOR_WATCHDOG_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct myvendor_watchdog_status_s
{
  bool started;         /**< 已 WDIOC_START。 */
  bool suppressed;      /**< 测试暂停生产喂狗。 */
  bool feeding;         /**< 最近一轮判定为可喂。 */
  bool idle_ok;         /**< idle 心跳未过期。 */
  bool work_ok;         /**< MTP 等长任务 work 心跳未过期。 */
  bool ui_ok;           /**< UI 心跳未过期（未要求时为 true）。 */
  bool ui_seen;         /**< UI 主循环至少打过一次点。 */
  bool ui_required;     /**< 本轮是否强制要求 UI 心跳。 */
  uint32_t idle_age_ms; /**< 距上次 up_idle 心跳。 */
  uint32_t work_age_ms; /**< 距上次 work 心跳；未见过则为 0。 */
  uint32_t ui_age_ms;   /**< 距上次 UI 心跳；未见过则为 0。 */
  uint32_t timeout_ms;  /**< IWDT 超时。 */
};

#if defined(CONFIG_MYVENDOR_WATCHDOG) && CONFIG_MYVENDOR_WATCHDOG

/** @brief SETTIMEOUT + START，并拉起 iwdg_feed。 */
void myvendor_watchdog_start(void);

/** @brief idle 任务心跳（仅写时间戳，禁止 ioctl/syslog）。 */
void myvendor_watchdog_idle_beat(void);

/** @brief bicycle UI 主循环心跳。 */
void myvendor_watchdog_ui_beat(void);

/**
 * @brief 长任务进度心跳（MTP 读写等），可替代 idle；新鲜时也可顶上 UI。
 *
 * MTP/SD 占住 LittleFS 时 UI 可能数秒打不到点。work 过期后仍要求 idle 与 UI。
 */
void myvendor_watchdog_work_beat(void);

/**
 * @brief SysTick/HCLK 换档后同步 idle/work/ui 心跳时间戳，避免年龄算炸。
 *
 * DVFS `sifli_systick_reclock()` 之后必须调用；见 n580 误复位。
 */
void myvendor_watchdog_tick_reclock(void);

/**
 * @brief 长加载让出 CPU：每满 1s usleep 10ms。不打 UI/work 心跳。
 */
void myvendor_watchdog_busy_pump(void);

/**
 * @brief 硬件 IWDT 立刻续命（寄存器，不 ioctl）。
 *
 * 给 LittleFS 第一次 `lfs_alloc` / 长 SD 传输用：bringup 优先级高于
 * `iwdg_feed`，只打 work 时间戳喂不到狗。
 */
void myvendor_watchdog_hw_pet(void);

/**
 * @brief 暂停或恢复生产喂狗（给 `test wdt start/feed` 用）。
 * @param suppress true 停止喂，false 恢复。
 */
void myvendor_watchdog_suppress(bool suppress);

/** @brief 读心跳与喂狗判定。 @return 0 成功，-1 未编译/未启动。 */
int myvendor_watchdog_get_status(struct myvendor_watchdog_status_s *st);

/**
 * @brief 进入核心转储时喂一次 IWDT，给写盘留出完整超时窗口。
 *
 * 走寄存器，不 ioctl（HardFault 里 mutex 不可用）。整机只生效一次。
 */
void myvendor_watchdog_keepalive_once(void);

/**
 * @brief 崩溃复位前：再喂一次并 STOP IWDT。
 *
 * SYSRESETREQ 清不掉 IWDT，boot 加载会再被咬。STOP 后等 NVIC 复位。
 */
void myvendor_watchdog_halt(void);

#else

static inline void myvendor_watchdog_start(void)
{
}

static inline void myvendor_watchdog_idle_beat(void)
{
}

static inline void myvendor_watchdog_ui_beat(void)
{
}

static inline void myvendor_watchdog_work_beat(void)
{
}

static inline void myvendor_watchdog_tick_reclock(void)
{
}

static inline void myvendor_watchdog_busy_pump(void)
{
}

static inline void myvendor_watchdog_hw_pet(void)
{
}

static inline void myvendor_watchdog_suppress(bool suppress)
{
  (void)suppress;
}

static inline int myvendor_watchdog_get_status(
    struct myvendor_watchdog_status_s *st)
{
  (void)st;
  return -1;
}

static inline void myvendor_watchdog_keepalive_once(void)
{
}

static inline void myvendor_watchdog_halt(void)
{
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_WATCHDOG_H */
