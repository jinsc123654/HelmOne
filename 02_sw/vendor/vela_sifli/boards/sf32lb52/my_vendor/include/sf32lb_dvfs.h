/**
 * @file sf32lb_dvfs.h
 * @brief HCPU 动态调频：72/96/144 MHz（S0）与 240 MHz（S1）。
 *
 * 不启用 Vela `CONFIG_PM`。切频走 `HAL_RCC_HCPU_ConfigHCLK()`，DLL2 保持
 * 288 MHz（PSRAM / NAND）。不下探 48/24 MHz（D 模式要关 DLL2）。
 *
 * 自动策略按 PID0 idle（与 `ps` IDLE 同口径）估算负载。预计 idle 低于
 * 20% 时立即升档；只有低档仍能保留 30% idle，且稳定 2 s 后才逐档下降。
 * 按键活动提供 144 MHz / 1 s 的交互下限。boot/map/charge/off 仍强制
 * 240 MHz；awake 只表示亮屏，不钉频。`ctl dvfs 72|96|144|240` 强制，
 * `auto` 恢复混合 governor。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SF32LB_DVFS_H
#define SF32LB_DVFS_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF32LB_DVFS_MHZ_72    72u   /**< S0。**已取消**（2026-10-01，见 g_gears[]）。 */
#define SF32LB_DVFS_MHZ_96    96u   /**< S0。**已取消**（2026-09-26）。 */
#define SF32LB_DVFS_MHZ_144  144u   /**< S0。 */
#define SF32LB_DVFS_MHZ_240  240u   /**< S1：掉电前/充电时钉住的高档。 */

/** 现役档位只有两档（`g_gears[]`）：144（S0）与 240（S1）。 */
#define SF32LB_DVFS_MHZ_LOW   SF32LB_DVFS_MHZ_144
#define SF32LB_DVFS_MHZ_HIGH  SF32LB_DVFS_MHZ_240

/** 随机跳频测试默认周期（ms）。 */
#define SF32LB_DVFS_HOP_DEFAULT_MS  10000u
/** 随机跳频测试允许的周期范围（ms）。 */
#define SF32LB_DVFS_HOP_MIN_MS        250u
#define SF32LB_DVFS_HOP_MAX_MS    3600000u

#define SF32LB_DVFS_HOLD_BOOT    (1u << 0)  /**< bringup → UI 接管。 */
#define SF32LB_DVFS_HOLD_AWAKE   (1u << 1)  /**< 亮屏（遥测；不钉 240）。 */
#define SF32LB_DVFS_HOLD_MAP     (1u << 2)  /**< 地图瓦片泵。 */
#define SF32LB_DVFS_HOLD_CHARGE  (1u << 3)  /**< ETA9184 充电中。 */
#define SF32LB_DVFS_HOLD_OFFING  (1u << 4)  /**< 关机动画 / 存盘。 */

/** 预测性高频：解码/充电/关机，不靠 idle 采样。 */
#define SF32LB_DVFS_HOLD_FLOOR \
  (SF32LB_DVFS_HOLD_BOOT | SF32LB_DVFS_HOLD_MAP | \
   SF32LB_DVFS_HOLD_CHARGE | SF32LB_DVFS_HOLD_OFFING)

#ifdef CONFIG_MYVENDOR_DVFS

/**
 * @brief 存储就绪后第一次提频到 240 MHz，并接管后续 hold。
 *
 * 同时重配 SysTick、SD、LCDC。背光仍由 bringup 按 splash 需要打开。
 */
void sf32lb_dvfs_boot(void);

/**
 * @brief 请求高频。可叠加，重复置位是空操作。
 * @param bits `SF32LB_DVFS_HOLD_*`。
 */
void sf32lb_dvfs_hold(uint32_t bits);

/**
 * @brief 释放高频请求。其它 floor hold 仍在则保持 240 MHz。
 * @param bits `SF32LB_DVFS_HOLD_*`。
 */
void sf32lb_dvfs_release(uint32_t bits);

/**
 * @brief 报告一次用户交互，立即启用短时交互频率下限。
 *
 * 连续调用会刷新期限；调试 force 和 240 MHz floor hold 优先级更高。
 */
void sf32lb_dvfs_interact(void);

/**
 * @brief 调试强制频率；0 恢复自动。
 * @param mhz 0、72、96、144 或 240。
 * @return 0 成功，负值为错误。
 */
int sf32lb_dvfs_force(uint32_t mhz);

/**
 * @brief 开关"**充电时也按负载动态调频**"（`ctl dvfs charge on|off`）。
 *
 * @details
 * 默认关。开启后，`SF32LB_DVFS_HOLD_CHARGE` 不再把 HCLK 抬到 240 MHz
 * —— 插电调试时跑的就是**电池使用时的同一套负载策略**，现场可以复现。
 * `HOLD_CHARGE` 位本身照旧记录（`holds=` 里仍能看到 `charge`），只是不抬地板。
 * `BOOT` / `MAP` / `OFFING` 仍钉 240（那三个是"不能低"的场合）。
 *
 * 与 `sf32lb_dvfs_hop_start()` 的区别：hop 走 force 路径、把 hold/governor/
 * 交互下限**全部绕过**，是合成扫描；本模式只摘一位地板，其余策略不动。
 *
 * @param on true = 开。
 * @return true = 已处在请求的状态。
 */
bool sf32lb_dvfs_charge_dyn_set(bool on);

/**
 * @brief 查询"充电时也按负载动态调频"当前是否开启。
 */
bool sf32lb_dvfs_charge_dyn(void);

/**
 * @brief 获取当前已应用的 HCLK。
 *
 * @return HCLK，单位 MHz；尚未启动 DVFS 时返回 HAL 读数。
 */
uint32_t sf32lb_dvfs_hclk_mhz(void);

/**
 * @brief 读取 governor 最近一次采样的 PID0 idle。
 *
 * @details 直接返回缓存，不加锁、不读 RCC / cpuload，供 UI 轮询。
 *
 * @return 0～100 的 idle 百分比。
 */
uint8_t sf32lb_dvfs_idle_pct(void);

/**
 * @brief 获取当前 hold 位图。
 *
 * @return SF32LB_DVFS_HOLD_* 位图。
 */
uint32_t sf32lb_dvfs_holds(void);

/**
 * @brief 获取当前调试强制频率。
 *
 * @return 强制频率，单位 MHz；0 表示自动模式。
 */
uint32_t sf32lb_dvfs_force_mhz(void);

/**
 * @brief 启动随机跳频自检：每 @p period_ms 在 72/96/144/240 间随机跳一档。
 *
 * @details
 * 走强制通道（等价于 `ctl dvfs <gear>`），因此 hold、ondemand governor、
 * 交互下限全部被绕过，测到的就是切频路径本身。每次保证跳到与当前**不同**
 * 的档位，所以每次跳频都真的会切换 HCLK 并重配外设。第一个跳变立即发生，
 * 便于确认命令已生效，之后按周期执行。
 *
 * 用于长跑验证：PSRAM/NAND XIP、LCDC、SD、SysTick 在反复切频下是否稳定。
 *
 * @param period_ms 跳频周期；0 表示用 SF32LB_DVFS_HOP_DEFAULT_MS。
 * @return 0 成功；-EINVAL 周期超范围。
 */
int sf32lb_dvfs_hop_start(uint32_t period_ms);

/**
 * @brief 停止随机跳频，清掉强制档位并交还 governor。
 */
void sf32lb_dvfs_hop_stop(void);

/**
 * @brief 随机跳频是否在跑。
 *
 * @return true 表示正在按周期随机跳频。
 */
bool sf32lb_dvfs_hop_active(void);

/**
 * @brief 随机跳频周期。
 *
 * @return 周期，单位 ms；未开启时返回配置的默认值。
 */
uint32_t sf32lb_dvfs_hop_period_ms(void);

/**
 * @brief 随机跳频累计跳变次数。
 *
 * @return 自上次启动以来的跳变次数。
 */
uint32_t sf32lb_dvfs_hop_count(void);

/**
 * @brief 调频 worker 心跳被判"已停摆"的年龄阈值（ms）。
 *
 * @details 正常心跳周期是 DVFS_GOV_POLL_MS(250 ms)，所以心跳年龄只该是几十
 *          毫秒级；超过这个值就认为调频已经停了，diag 线程据此触发外部恢复。
 */
#define SF32LB_DVFS_GOV_STALE_MS  2000u

/**
 * @brief 调频 worker 的心跳：距上次运行多久、累计跑了多少次。
 *
 * @details 整个调频体系（ondemand、charge hold、hop）都挂在 gov_worker 这一个
 *          自我重挂的 work 上。它一旦停止重挂，调频会**永久静默停摆**，以前
 *          没有任何痕迹可查 —— 现场表现就是"跑久了 DVFS 不调频了"。
 *          age 应该始终是个位数毫秒级（远小于 DVFS_GOV_POLL_MS 的数倍）；
 *          如果 age 一直很大且不再变小，就是 worker 已经死了。
 *
 * @param[out] age_ms 距上次运行的毫秒数；从未跑过时为 0xffffffff。
 * @param[out] runs   累计运行次数；可为 NULL。
 */
void sf32lb_dvfs_gov_stat(uint32_t *age_ms, uint32_t *runs);

/**
 * @brief 外部恢复：把调频 worker 重新挂上（diag 线程心跳超时后调用）。
 * @return 0 已重新挂上；负值为 work_queue 的错误码。
 */
int sf32lb_dvfs_gov_kick(void);

/**
 * @brief 调频 worker 被外部接回来的累计次数（正常应恒为 0）。
 */
uint32_t sf32lb_dvfs_gov_recoveries(void);

/**
 * @brief 向 NSH 输出 HCLK、governor、boost、hold 和 DLL2 状态。
 */
void sf32lb_dvfs_print(void);

#else

static inline void sf32lb_dvfs_boot(void)
{
}

static inline void sf32lb_dvfs_hold(uint32_t bits)
{
  (void)bits;
}

static inline void sf32lb_dvfs_release(uint32_t bits)
{
  (void)bits;
}

static inline void sf32lb_dvfs_interact(void)
{
}

static inline int sf32lb_dvfs_force(uint32_t mhz)
{
  (void)mhz;
  return -1;
}

static inline bool sf32lb_dvfs_charge_dyn_set(bool on)
{
  (void)on;
  return false;
}

static inline bool sf32lb_dvfs_charge_dyn(void)
{
  return false;
}

static inline uint32_t sf32lb_dvfs_hclk_mhz(void)
{
  return 0;
}

static inline uint8_t sf32lb_dvfs_idle_pct(void)
{
  return 100;
}

static inline uint32_t sf32lb_dvfs_holds(void)
{
  return 0;
}

static inline uint32_t sf32lb_dvfs_force_mhz(void)
{
  return 0;
}

static inline int sf32lb_dvfs_hop_start(uint32_t period_ms)
{
  (void)period_ms;
  return -1;
}

static inline void sf32lb_dvfs_gov_stat(uint32_t *age_ms, uint32_t *runs)
{
  if (age_ms != NULL)
    {
      *age_ms = 0xffffffffu;
    }

  if (runs != NULL)
    {
      *runs = 0;
    }
}

static inline int sf32lb_dvfs_gov_kick(void)
{
  return 0;
}

static inline uint32_t sf32lb_dvfs_gov_recoveries(void)
{
  return 0;
}

static inline void sf32lb_dvfs_hop_stop(void)
{
}

static inline bool sf32lb_dvfs_hop_active(void)
{
  return false;
}

static inline uint32_t sf32lb_dvfs_hop_period_ms(void)
{
  return 0;
}

static inline uint32_t sf32lb_dvfs_hop_count(void)
{
  return 0;
}

static inline void sf32lb_dvfs_print(void)
{
}

#endif /* CONFIG_MYVENDOR_DVFS */

#ifdef __cplusplus
}
#endif

#endif /* SF32LB_DVFS_H */
