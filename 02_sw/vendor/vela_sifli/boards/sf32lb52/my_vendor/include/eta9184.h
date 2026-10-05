/**
 * @file eta9184.h
 * @brief ETA9184 充电、Boost 与电量指示驱动。
 *
 * 引脚（开漏 / Hi-Z，MCU 内部上拉）：
 * - PA24 DISCHRG：放电中为低；电量 &lt;3% 约 1 Hz 拉低；未放电为高
 * - PA25 STAT：充电中为低；充满 / 未充电为高
 * - PA26 PULSE：4 秒内 1～4 次高脉冲对应约 25/50/75/100%；常高为充满。
 *   **不可信**：产品电量百分比只走 VBATS/GPADC CH7。PULSE 百分比仅
 *   `test eta` 调试，不要当 SOC。
 * - PA28 ENBST：5V Boost（功放），默认关。产品电量走 VBATS，不必为
 *   PULSE 常开升压；功放需要时再 `eta9184_boost_set(true)` / `test eta pwr 1`
 *
 * PULSE 仅在充电或放电时闪。充满判定用 STAT 高 + DISCHRG 高（仍插着线、
 * 未在充），不依赖 PULSE。
 * 4 秒窗口用独立软件定时器（wd_start），不用 LPWORK。
 *
 * 业务接口：
 * - eta9184_battery_percent()：PULSE 窗口估电量，**不可信**
 * - eta9184_charge_full()：充满 IO（给 VBATS 满电校准）
 * - eta9184_power_state()：充电中 / 放电中 / 低电量 / 空闲
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __VENDOR_MYVENDOR_ETA9184_H
#define __VENDOR_MYVENDOR_ETA9184_H

#include <nuttx/compiler.h>
#include <stdbool.h>

/**
 * @brief STAT 充电状态（调试 / 细粒度，一般用 eta9184_power_state()）。
 */
enum eta9184_chg_e
{
  ETA9184_CHG_IDLE = 0,   /**< 未充电 */
  ETA9184_CHG_CHARGING,   /**< 充电中（STAT 为低） */
  ETA9184_CHG_FULL,       /**< 充满（STAT 高且 DISCHRG 高：仍插线、未在充） */
};

/**
 * @brief DISCHRG 放电状态（调试 / 细粒度，一般用 eta9184_power_state()）。
 */
enum eta9184_dischg_e
{
  ETA9184_DISCHG_OFF = 0, /**< 未放电 */
  ETA9184_DISCHG_ON,      /**< 放电中（Boost 开，DISCHRG 为低） */
  ETA9184_DISCHG_LOW,     /**< 低电量（DISCHRG 约 1 Hz 闪） */
};

/**
 * @brief 电源综合状态。
 *
 * 优先级：充电中 &gt; 低电量 &gt; 放电中 &gt; 空闲。
 * 低电量请用本枚举，不要用百分比是否为 0 判断。
 */
enum eta9184_power_e
{
  ETA9184_POWER_IDLE = 0,        /**< 未充电、未放电 */
  ETA9184_POWER_CHARGING,        /**< 充电中 */
  ETA9184_POWER_DISCHARGING,     /**< 放电中 */
  ETA9184_POWER_LOW,             /**< 低电量（&lt;3%） */
};

/**
 * @brief 一次采样快照，供 `test eta read` 与调试。
 *
 * @p chrg_high / @p stat_high / @p pulse_high 为当前 GPIO 原始电平
 * （高为 true）。产品电量用 VBATS；@p bat_pct / @p pulse_pct 均不可信。
 */
struct eta9184_status_s
{
  bool chrg_high;              /**< PA25 STAT，高=未在充电 */
  bool stat_high;              /**< PA24 DISCHRG，高=未在放电 */
  bool pulse_high;             /**< PA26 PULSE 原始电平 */
  bool boost_on;               /**< ENBST / 5V Boost 是否打开 */
  int bat_level;               /**< 0 初始化，1～4 四分位，5 满电（PULSE，不可信） */
  int bat_pct;                 /**< 恒为 -1；产品百分比走 VBATS */
  int pulse_pct;               /**< PULSE 映射 20/40/60/80/100，不可信 */
  bool pulse_trusted;           /**< 恒为 false */
  int pulse_count;             /**< 当前 4 秒窗口内已计脉冲数 */
  int dischg_edges;            /**< 当前窗口内 DISCHRG 边沿数 */
  unsigned int timer_ticks;    /**< 4 秒软件定时器到期次数 */
  unsigned int pulse_irqs;     /**< 启动后累计 PULSE 上升沿中断 */
  unsigned int dischg_irqs;    /**< 启动后累计 DISCHRG 边沿 */
  unsigned int windows;        /**< 已完成的 4 秒电量窗口数 */
  enum eta9184_chg_e chg;      /**< STAT 细粒度状态 */
  enum eta9184_dischg_e dischg;/**< DISCHRG 细粒度状态 */
  enum eta9184_power_e power;  /**< 综合电源状态 */
};

/**
 * @brief 配置引脚与上拉，Boost / EN 默认关闭。可重复调用。
 *
 * 不上报电量、不启动采样。开机百分比应为 -1。
 *
 * @return 0 成功，负值为错误码。
 */
int eta9184_init(void);

/**
 * @brief 挂 GPIO 中断，用 4 秒软件定时器开始采 STAT / DISCHRG / PULSE。
 *
 * 不打开 Boost。已在运行则直接返回 0。
 *
 * @return 0 成功，负值为错误码。
 */
int eta9184_start(void);

/**
 * @brief 停止 4 秒软件定时器。不改变 Boost。
 */
void eta9184_stop(void);

/**
 * @brief 采样是否在运行。
 *
 * @return true 正在采样。
 */
bool eta9184_running(void);

/**
 * @brief 打开或关闭 5V Boost（ENBST，功放供电）。
 *
 * 打开后芯片进入放电（功放 5V）。手册建议使能时浮空、关闭时拉低；
 * 当前实现用上拉开、下拉关。默认关，产品电量不依赖此脚。
 *
 * @param on true 打开，false 关闭。
 */
void eta9184_boost_set(bool on);

/**
 * @brief 读取软件记录的 Boost 开关。
 *
 * @return true 已请求打开。
 */
bool eta9184_boost_get(void);

/**
 * @brief PULSE 窗口估电量（不可信，仅 `test eta`）。
 *
 * 产品 SOC 用 `myvendor_sys_battery_percent()`（VBATS）。
 *
 * @return -1 初始化中（尚未采满一个 4 秒窗口）；
 *         0～100 为 PULSE 次数映射（20/40/60/80/100）。
 */
int eta9184_battery_percent(void);

/**
 * @brief 充电器报充满：STAT 高且 DISCHRG 高（仍插线、未在充）。
 *
 * 不看 PULSE。给 VBATS 满电电压校准用。
 *
 * @return true 充满 IO 有效。
 */
bool eta9184_charge_full(void);

/**
 * @brief 取出并清除充电→充满边沿（给 VBATS 校准；未读则一直保持）。
 */
bool eta9184_take_full_edge(void);

/**
 * @brief 读取充电 / 放电 / 低电量 / 空闲。
 *
 * @return #ETA9184_POWER_CHARGING、#ETA9184_POWER_DISCHARGING、
 *         #ETA9184_POWER_LOW 或 #ETA9184_POWER_IDLE。
 */
enum eta9184_power_e eta9184_power_state(void);

/**
 * @brief 复制当前采样快照。
 *
 * @param[out] out 输出结构，NULL 则忽略。
 */
void eta9184_get_status(FAR struct eta9184_status_s *out);

#endif /* __VENDOR_MYVENDOR_ETA9184_H */
