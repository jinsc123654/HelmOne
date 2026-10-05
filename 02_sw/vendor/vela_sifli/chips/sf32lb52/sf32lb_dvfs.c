/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/sf32lb_dvfs.c
 *
 * SF32LB52 HCPU gears: **144 (S0) and 240 (S1)** —— 72 与 96 已先后取消，
 * 见 g_gears[] 上的注释。PSRAM/NAND 总线已钉回 DLL2 @144 MHz（官方设计，
 * 见 sifli_allocateheap.c 的 NAND 分支）⇒ 降档不再连带降内存带宽。
 * D0/D1（24/48 MHz）从来没有用过。
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/**
 * @file sf32lb_dvfs.c
 * @brief SF32LB52 HCPU 交互感知动态调频实现。
 *
 * @details
 * 使用 PID0 idle 估算 CPU 需求，在 72/96/144/240 MHz 四档之间调节。
 * 升频立即执行，降频采用 Schmitt 回差、稳定等待和逐档下降。按键活动通过
 * sf32lb_dvfs_interact() 建立短时 144 MHz 下限，以弥补 CPU load 历史窗口
 * 无法预测首帧负载的问题。boot/map/charge/off hold 仍直接要求 240 MHz。
 */

#include "sf32lb_dvfs.h"

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include "arm_internal.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "myvendor_watchdog.h"
#include "eta9184.h"
#include "sf32lb_adc.h"

#ifdef CONFIG_MTD
#  include "sf32lb_sdio.h"
#endif

#ifdef CONFIG_LCD_USING_NV3031A
extern void sf32lb_lcd_reclock(void);
#endif

/* PID0 idle (same as `ps` IDLE). Ondemand: pick the lowest gear that
 * keeps projected idle ≥ DOWN; step up if projected idle would fall
 * below UP. 48/24 MHz are D-mode and turn off DLL2 — not in this table.
 */

#ifndef CONFIG_MYVENDOR_DVFS_IDLE_UP
#  define CONFIG_MYVENDOR_DVFS_IDLE_UP 20
#endif

#ifndef CONFIG_MYVENDOR_DVFS_IDLE_DOWN
#  define CONFIG_MYVENDOR_DVFS_IDLE_DOWN 30
#endif

#ifndef CONFIG_MYVENDOR_DVFS_INTERACTIVE_MHZ
#  define CONFIG_MYVENDOR_DVFS_INTERACTIVE_MHZ 144
#endif

#ifndef CONFIG_MYVENDOR_DVFS_INTERACTIVE_MS
#  define CONFIG_MYVENDOR_DVFS_INTERACTIVE_MS 1000
#endif

#ifndef CONFIG_MYVENDOR_DVFS_DOWN_HOLD_MS
#  define CONFIG_MYVENDOR_DVFS_DOWN_HOLD_MS 4000
#endif

#define DVFS_GOV_POLL_MS 250u

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* **96 MHz 已取消**（用户 2026-09-26）：现场一次 WDT 饥饿复位（uptime 136.5 s）
 * 之前 0.5 秒内连着换了三档 96 -> 144 -> 240，每次都跟着重配 SD 时钟
 * （`SD: clk div 3->4->6->10`），紧接着 UI 线程被占住 11.17 秒（日志静默窗口与
 * 心跳停摆等长）。96 相对 72 的收益有限，却正好插在"低档 <-> 高档"的过渡带上多出
 * 一次换档+重配；去掉它把换档路径从三段变两段。 */
/* **72 MHz 也已取消**（用户 2026-10-01）。同一轮改动把 PSRAM 总线钉回 DLL2、
 * 固定在 144 MHz（官方设计，见 `sifli_allocateheap.c` 的 NAND 分支），于是
 * HCLK 降档**不再**带来内存带宽的下降 —— 72 相对 144 只剩 CPU 一侧的省电，
 * 而它是最低档、最容易撞上"低档 <-> 高档"大跨度换档的那一端。
 * 只留 144（S0）与 240（S1）两档。（两者都在 S 模式 ⇒ DLL2 的 288 MHz
 * 上限对它们全部满足，`hpsys_dll2_limit[S0]=[S1]=288 MHz`。） */
static const uint32_t g_gears[] =
{
  SF32LB_DVFS_MHZ_144,
  SF32LB_DVFS_MHZ_240,
};

#define DVFS_NGEAR (sizeof(g_gears) / sizeof(g_gears[0]))

static mutex_t g_lock = NXMUTEX_INITIALIZER;
static bool g_hw_ready;
static uint32_t g_holds;
static uint32_t g_applied_mhz;
static uint32_t g_force_mhz;
static uint32_t g_gov_mhz = SF32LB_DVFS_MHZ_240;
static uint32_t g_down_candidate_mhz;
static clock_t g_down_ticks;
static bool g_interactive_active;
static uint32_t g_interactive_deadline;
static uint8_t g_idle_pct = 100;
static struct work_s g_gov_work;

/* 调频 worker 心跳。见 sf32lb_dvfs_gov_stat()：整条调频链挂在这一个自挂
 * work 上，重挂一旦失败就永久静默停摆，所以必须能从外面看出它还活着。 */
static uint32_t g_gov_last_ticks;
static uint32_t g_gov_runs;
static uint32_t g_gov_recoveries;

/* 验证用跳频自检（`ctl dvfs hop`）。默认关闭；开启后由 gov_worker 按周期
 * 选下一个档位，走 g_force_mhz 强制生效——这样 hold、governor、交互下限
 * 全部被绕过，只有切频本身被测到。档位选择见 hop_pick_locked()。
 */
static bool g_hop_active;
static uint32_t g_hop_period_ms = SF32LB_DVFS_HOP_DEFAULT_MS;
static uint32_t g_hop_next_ticks;
static uint16_t g_hop_n;
static uint8_t g_hop_pos;  /* 当前档位下标 */

/** @brief 充电时也按负载动态调频（`ctl dvfs charge on`）。**默认关**。
 *
 *  由来（2026-09-23 用户要求："debug 的时候插电也能像正常使用一样调频"）：
 *  `SF32LB_DVFS_HOLD_FLOOR` 里含 `HOLD_CHARGE`，插上充电器就把 HCLK 钉在 240 MHz
 *  —— 于是"插电调试"与"电池使用"跑的是**两种频率行为**，现场复现不了。
 *  `ctl dvfs hop` 虽然插电也能跳频，但它走的是 **force 路径**（hold / governor /
 *  交互下限**全部被绕过**），那是合成扫描，不是"像正常使用一样"。
 *
 *  本模式只做一件事：**把 CHARGE 位从 FLOOR 掩码里摘掉**，其余策略原样。
 *  - CHARGE 位本身**照旧记录**（`holds=` 里仍能看到 `charge`），只是不再抬地板
 *    ⇒ 既能复现电池行为，又不丢"正在充电"这个遥测事实；
 *  - BOOT/MAP/OFFING 仍钉 240（那三个是"不能低"的场合，与调试复现无关）。
 *
 *  默认关：量产/日常插电时原来的高频策略不变。 */
static bool g_charge_dyn;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief 读取当前 HCPU HCLK。
 *
 * @return 当前 HCLK，单位 MHz；HAL 暂时不可用时返回最近应用值。
 */
static uint32_t hclk_mhz_now(void)
{
  uint32_t hz = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);

  if (hz == 0)
    {
      return g_applied_mhz;
    }

  return (hz + 500000u) / 1000000u;
}

/**
 * @brief 判断频率是否为受支持的离散档位。
 *
 * @param mhz 待检查频率，单位 MHz。
 * @return true 表示是 72/96/144/240 MHz 之一，否则返回 false。
 */
static bool mhz_is_gear(uint32_t mhz)
{
  unsigned i;

  for (i = 0; i < DVFS_NGEAR; i++)
    {
      if (g_gears[i] == mhz)
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief 将频率向上取整到最近的受支持档位。
 *
 * @param mhz 最低需求频率，单位 MHz。
 * @return 不低于 @p mhz 的档位；超过最高档时返回 240 MHz。
 */
static uint32_t gear_ceil(uint32_t mhz)
{
  unsigned i;

  for (i = 0; i < DVFS_NGEAR; i++)
    {
      if (g_gears[i] >= mhz)
        {
          return g_gears[i];
        }
    }

  return SF32LB_DVFS_MHZ_240;
}

/**
 * @brief 获取当前档位的相邻低档。
 *
 * @param mhz 当前档位，单位 MHz。
 * @return 相邻低档；输入为最低档时仍返回 72 MHz。
 */
static uint32_t gear_prev(uint32_t mhz)
{
  unsigned i;

  for (i = 1; i < DVFS_NGEAR; i++)
    {
      if (g_gears[i] >= mhz)
        {
          return g_gears[i - 1];
        }
    }

  return g_gears[DVFS_NGEAR - 2];
}

/**
 * @brief 计算无符号 32 位整数的向上整除。
 *
 * @param num 被除数。
 * @param den 除数；为 0 时直接返回 @p num。
 * @return 向上取整后的商。
 */
static uint32_t ceil_div_u32(uint32_t num, uint32_t den)
{
  if (den == 0)
    {
      return num;
    }

  return (num + den - 1u) / den;
}

/**
 * @brief 获取 PID0 idle 百分比。
 *
 * @return 0～100 的 idle 百分比；CPU load 不可用时返回 100。
 */
static uint8_t idle_pct_now(void)
{
#ifndef CONFIG_SCHED_CPULOAD_NONE
  struct cpuload_s load;

  if (clock_cpuload(0, &load) == 0 && load.total > 0)
    {
      return (uint8_t)((100ull * (uint64_t)load.active) /
                       (uint64_t)load.total);
    }
#endif

  return 100;
}

/**
 * @brief 以支持 32 位回绕的方式判断当前 tick 是否早于截止 tick。
 *
 * @param now 当前 tick。
 * @param deadline 截止 tick。
 * @return 尚未到期返回 true，已到期返回 false。
 */
static bool tick_before(uint32_t now, uint32_t deadline)
{
  return (int32_t)(now - deadline) < 0;
}

/**
 * @brief 清理已经到期的交互 boost。
 *
 * @note 调用者必须持有 g_lock。
 */
static void interactive_expire_locked(void)
{
  if (g_interactive_active &&
      !tick_before((uint32_t)clock_systime_ticks(),
                   g_interactive_deadline))
    {
      g_interactive_active = false;
    }
}

/**
 * @brief 获取交互频率下限对应的合法档位。
 *
 * @return 配置值向上取整后的频率档位，单位 MHz。
 */
static uint32_t interactive_mhz(void)
{
  return gear_ceil(CONFIG_MYVENDOR_DVFS_INTERACTIVE_MHZ);
}

/**
 * @brief 根据当前 idle 和频率预测合适的目标档位。
 *
 * @details
 * 先计算等效需求 `need = cur * (100 - idle) / 100`。升档使用
 * IDLE_UP 预留，降档使用更大的 IDLE_DOWN 预留，从而形成 Schmitt 回差。
 *
 * @param idle 当前 PID0 idle 百分比。
 * @param cur 当前实际 HCLK，单位 MHz。
 * @return 按容量预测得到的离散频率档位。
 */
static uint32_t gov_pick_mhz(uint8_t idle, uint32_t cur)
{
  uint32_t need;
  uint32_t up_min;
  uint32_t down_min;
  uint32_t busy;

  if (cur == 0)
    {
      cur = SF32LB_DVFS_MHZ_240;
    }

  if (idle > 100)
    {
      idle = 100;
    }

  busy = 100u - (uint32_t)idle;
  need = (busy * cur) / 100u;
  if (need == 0)
    {
      need = 1;
    }

  up_min = ceil_div_u32(need * 100u, 100u - CONFIG_MYVENDOR_DVFS_IDLE_UP);
  down_min = ceil_div_u32(need * 100u, 100u - CONFIG_MYVENDOR_DVFS_IDLE_DOWN);

  if (cur < up_min)
    {
      return gear_ceil(up_min);
    }

  if (cur > down_min)
    {
      return gear_ceil(down_min);
    }

  return cur;
}

/**
 * @brief 用最新负载样本更新 governor 档位。
 *
 * @details
 * 升档立即生效；降档必须保持同一候选档达到配置时长，并且每次最多下降一档。
 *
 * @param idle 最新 PID0 idle 百分比。
 * @note 调用者必须持有 g_lock。
 */
static void gov_update_load_locked(uint8_t idle)
{
  uint32_t candidate;
  uint32_t cur;
  uint32_t pick;
  clock_t now;

  g_idle_pct = idle;
  cur = g_applied_mhz ? g_applied_mhz : hclk_mhz_now();
  pick = gov_pick_mhz(idle, cur);
  now = clock_systime_ticks();

  if (pick > g_gov_mhz)
    {
      g_gov_mhz = pick;
      g_down_candidate_mhz = 0;
    }
  else if (pick < g_gov_mhz)
    {
      /* Drop at most one gear, and only after the same lower target has
       * remained valid for the full hold interval.  This is the down side
       * of the Schmitt trigger; transient idle spikes cannot collapse HCLK.
       */

      candidate = gear_prev(g_gov_mhz);
      if (candidate != g_down_candidate_mhz)
        {
          g_down_candidate_mhz = candidate;
          g_down_ticks = now;
        }
      else if ((clock_t)(now - g_down_ticks) >=
               MSEC2TICK(CONFIG_MYVENDOR_DVFS_DOWN_HOLD_MS))
        {
          g_gov_mhz = candidate;
          g_down_candidate_mhz = 0;
        }
    }
  else
    {
      g_down_candidate_mhz = 0;
    }
}

/**
 * @brief 合并 force、强制 hold、负载档位和交互下限。
 *
 * @return 最终要求应用的 HCLK 档位，单位 MHz。
 * @note 调用者必须持有 g_lock。
 */
static uint32_t target_mhz_locked(void)
{
  uint32_t target;
  uint32_t floor_mask;

  if (mhz_is_gear(g_force_mhz))
    {
      return g_force_mhz;
    }

  interactive_expire_locked();

  /* 充电动态调频模式：把 CHARGE 摘出地板掩码（见 g_charge_dyn 的说明）。
   * CHARGE 位本身仍在 g_holds 里，`holds=` 那行照旧显示，只是不再抬地板。 */
  floor_mask = SF32LB_DVFS_HOLD_FLOOR;
  if (g_charge_dyn)
    {
      floor_mask &= ~SF32LB_DVFS_HOLD_CHARGE;
    }

  if ((g_holds & floor_mask) != 0)
    {
      return SF32LB_DVFS_MHZ_240;
    }

#ifdef CONFIG_SCHED_CPULOAD_NONE
  target = g_holds ? SF32LB_DVFS_MHZ_240 : SF32LB_DVFS_MHZ_144;
#else
  target = g_gov_mhz;
#endif

  if (g_interactive_active && target < interactive_mhz())
    {
      target = interactive_mhz();
    }

  return target;
}

/**
 * @brief 根据 ETA9184 充电状态维护充电强制高频 hold。
 *
 * @note 调用者必须持有 g_lock。
 */
static void charge_hold_locked(void)
{
  if (eta9184_power_state() == ETA9184_POWER_CHARGING)
    {
      g_holds |= SF32LB_DVFS_HOLD_CHARGE;
    }
  else
    {
      g_holds &= ~SF32LB_DVFS_HOLD_CHARGE;
    }
}

/**
 * @brief HCLK 切换后重配所有依赖 HCLK 的外设，改完**统一**报告结果。
 *
 * @details
 * 包括 SysTick LOAD（不重启 arch_timer）、SD、LCDC、背光 PWM，以及 **ADC 的采样
 * 时序**（GPADC 没有专属分频器，转换计数时钟就是 PCLK1 = HCLK>>PDIV1，采样宽度
 * 按它算 —— 见 sf32lb_adc_reclock）。背光已关闭时不会被重新打开。USART 波特率走
 * SystemFixClock，不在这里 HAL_UART_Init（会清 DMAR，GNSS 假死）。
 *
 * 频率是一次改的，**验收也一次**：全部改完之后由调用方报一条带 `reclock=` 的
 * 结论行，而不是每个外设各报各的。
 *
 * @return 0 全部跟上新时钟；-EIO 有外设报告自己没跟上（目前只有 ADC 能自证）。
 */
static int reclock_peripherals(void)
{
  int rc;

  sifli_systick_reclock();
  /* SysTick LOAD 变了 → tick 换算失真；先把 WDT 心跳钉到"现在"。 */
  myvendor_watchdog_tick_reclock();
#ifdef CONFIG_LCD_USING_NV3031A
  sf32lb_lcd_reclock();
#endif
#ifdef CONFIG_MTD
  sf32lb_sd_reclock();
#endif
  /* ADC 采样时序。漏了它 = 在错的时钟下做 SAR，读数系统性偏高
   * （现场 4439 -> 4571 mV 跟着档位走）。 */
  rc = sf32lb_adc_reclock();
#ifdef CONFIG_LCD_USING_NV3031A
  {
    uint8_t pct = BSP_LCD_BL_GetPct();

    /* Idle sleep has backlight off; do not turn it back on. */
    if (pct > 0)
      {
        (void)BSP_LCD_BL_PwmPct(pct);
      }
  }
#endif
  return rc;
}

/**
 * @brief 将仲裁后的目标档位应用到硬件。
 *
 * @return 0 表示成功或无需切换，-EIO 表示 HAL 切频失败。
 * @note 调用者必须持有 g_lock。
 */
static int apply_locked(void)
{
  irqstate_t flags;
  HAL_StatusTypeDef st;
  uint32_t want;
  uint32_t dll2;

  if (!g_hw_ready)
    {
      return 0;
    }

  want = target_mhz_locked();
  if (want == g_applied_mhz)
    {
      return 0;
    }

  /* HCLK 切换前先把 SD 时钟停掉。SDCLK 是 HCLK 分频出来的，切换瞬间不管
   * 分频表怎么算，卡上都会看到一段非预期的时钟；向上跳档（72->240，分频
   * 3 -> 10）更是直接给到 80 MHz。同时 reclock_peripherals() 里的
   * sf32lb_sd_reclock() 要拿 g_sd_lock，文件系统忙时实测被拖后 4.86 s
   * （日志 SD: clk div 240 -> 16 比 hop 晚 4.86 s），这段时间卡一直在跑旧
   * 分频。停钟之后这两件事都不再影响卡。
   */
  sf32lb_sd_clk_hold();

  /* ADC 同理：它的转换计数时钟就是 PCLK1，切频瞬间还在跑的转换结果不可信。
   * 拿住驱动锁把在途转换排空（锁由 sf32lb_adc_reclock() 释放）。
   * ⚠ 下面**两条出口都要经过 reclock_peripherals()** —— 它才会放锁；
   *   切频失败那条也一样，否则 ADC 就永远锁死了。 */
  (void)sf32lb_adc_clk_hold();

  flags = enter_critical_section();
  SCB_InvalidateICache();
  st = HAL_RCC_HCPU_ConfigHCLK(want);
  leave_critical_section(flags);

  if (st != HAL_OK)
    {
      /* 切频失败，但 SDCLK 已经被 clk_hold 停掉了，必须放回去，
       * 否则卡就此没有时钟。ADC 的锁同样在这里放掉。 */
      int rrc = reclock_peripherals();

      syslog(LOG_ERR,
             "dvfs: ConfigHCLK(%u) st=%d holds=0x%x idle=%u gov=%u "
             "boost=%u dll2=%lu reclock=%s\n",
             (unsigned)want, (int)st, (unsigned)g_holds,
             (unsigned)g_idle_pct, (unsigned)g_gov_mhz,
             (unsigned)(g_interactive_active ? interactive_mhz() : 0),
             (unsigned long)HAL_RCC_HCPU_GetDLL2Freq(),
             rrc == 0 ? "ok" : "FAIL");
      return -EIO;
    }

  g_applied_mhz = want;
  {
    int rrc = reclock_peripherals();

    dll2 = HAL_RCC_HCPU_GetDLL2Freq();
    syslog(rrc == 0 ? LOG_INFO : LOG_ERR,
           "dvfs: HCLK=%u MHz holds=0x%x force=%u idle=%u gov=%u "
           "boost=%u dll2=%lu chgdyn=%u reclock=%s\n",
           (unsigned)g_applied_mhz, (unsigned)g_holds,
           (unsigned)g_force_mhz, (unsigned)g_idle_pct,
           (unsigned)g_gov_mhz,
           (unsigned)(g_interactive_active ? interactive_mhz() : 0),
           (unsigned long)dll2,
           /* 必须有这一栏：开了充电动态模式之后 `holds` 里**仍然**带 charge 位
            * （那是遥测事实），而 HCLK 却可能落在低档 —— 两栏并列会自相矛盾，
            * 光看日志没法判断是"策略如此"还是"hold 记账坏了"。 */
           g_charge_dyn ? 1u : 0u,
           /* 频率是一次改的，验收也一次：这一栏是**全部**依赖 HCLK 的外设改完
            * 之后的统一结论（目前能自证的只有 ADC，见 sf32lb_adc_reclock）。 */
           rrc == 0 ? "ok" : "FAIL");
  }
  return 0;
}

/**
 * @brief 将 hold 位图转换为便于诊断的名称列表。
 *
 * @param buf 输出缓冲区。
 * @param n 输出缓冲区长度。
 * @param holds hold 位图。
 */
static void holds_put(char *buf, size_t n, uint32_t holds)
{
  size_t used = 0;
  unsigned i;
  static const struct
  {
    uint32_t bit;
    const char *name;
  } names[] =
    {
      { SF32LB_DVFS_HOLD_BOOT,   "boot" },
      { SF32LB_DVFS_HOLD_AWAKE,  "awake" },
      { SF32LB_DVFS_HOLD_MAP,    "map" },
      { SF32LB_DVFS_HOLD_CHARGE, "charge" },
      { SF32LB_DVFS_HOLD_OFFING, "off" },
    };

  if (n == 0)
    {
      return;
    }

  buf[0] = '\0';
  if (holds == 0)
    {
      (void)snprintf(buf, n, "-");
      return;
    }

  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
      int w;

      if ((holds & names[i].bit) == 0)
        {
          continue;
        }

      w = snprintf(buf + used, n - used, "%s%s",
                   used ? "," : "", names[i].name);
      if (w < 0 || (size_t)w >= n - used)
        {
          return;
        }

      used += (size_t)w;
    }
}

/**
 * @brief 按下标取档位，带显式边界钳制。
 *
 * @details
 * g_hop_pos 只在 [0, DVFS_NGEAR) 内取模推进，但它是跨函数改写的静态量，
 * 静态分析无法证明每个使用点的下标都在范围内。跳频的所有档位取值都走这个
 * 访问器，越界索引在编译期即可证不存在。
 *
 * @param pos 档位下标。
 * @return 档位频率，单位 MHz。
 */
static uint32_t gear_at(uint8_t pos)
{
  if (pos >= (uint8_t)DVFS_NGEAR)
    {
      pos = 0u;
    }

  return g_gears[pos];
}

/**
 * @brief 把下标向前推进一格（模 DVFS_NGEAR）。
 *
 * @param pos 当前下标。
 * @param step 步长，必须非 0；取 DVFS_NGEAR 的互质值可遍历全部档位。
 * @return 推进后的下标，恒小于 DVFS_NGEAR。
 */
static uint8_t gear_step(uint8_t pos, uint32_t step)
{
  uint32_t next = ((uint32_t)pos + step) % (uint32_t)DVFS_NGEAR;

  return (uint8_t)next;
}

/**
 * @brief 挑下一个跳频档位：按与档位数互质的步长轮转。
 *
 * @details
 * 这里不做伪随机数。跳频自检的目的是让切频路径（ConfigHCLK + 外设重配）
 * 被反复走到，而不是产生不可预测序列，所以用"相邻必不同、每轮遍历全部档位"
 * 的轮转更合适：
 *   - 步长取 DVFS_NGEAR-1，与档位数恒互质 ⇒ 连续 DVFS_NGEAR 次跳变覆盖
 *     全部档位；纯随机可能很久碰不到某个档，覆盖不完整；
 *   - 步长非 0 ⇒ 每次跳变都真的换频，不会原地不动白跳一次；
 *   - 步长 N-1 让顺序是"跨大步"而不是单调爬升：两档时即 144↔240 交替，
 *     正是最大跨度、要测的场景；
 *   - 起点在 hop_start 时轮换，所以每次自检的档位次序不同，看起来像乱跳。
 *
 * @param cur 当前 HCLK，单位 MHz，用于兜底保证目标不等于它。
 * @return 目标档位，单位 MHz，必定不等于 @p cur。
 * @note 调用者必须持有 g_lock。
 */
static uint32_t hop_pick_locked(uint32_t cur)
{
  uint32_t pick;

  g_hop_pos = gear_step(g_hop_pos, DVFS_NGEAR - 1u);
  pick = gear_at(g_hop_pos);

  if (pick == cur)
    {
      /* 只在起点与当前档重合时发生一次，推一格即可。 */
      g_hop_pos = gear_step(g_hop_pos, 1u);
      pick = gear_at(g_hop_pos);
    }

  return pick;
}

/**
 * @brief 到点则随机跳一档；走强制通道，hold / governor 全部让路。
 *
 * @note 调用者必须持有 g_lock。由 gov_worker 每 DVFS_GOV_POLL_MS 驱动。
 */
static void hop_tick_locked(void)
{
  uint32_t now;
  uint32_t cur;
  uint32_t pick;

  if (!g_hop_active)
    {
      return;
    }

  now = (uint32_t)clock_systime_ticks();
  if (g_hop_next_ticks != 0 && (int32_t)(now - g_hop_next_ticks) < 0)
    {
      return;
    }

  g_hop_next_ticks = now + (uint32_t)MSEC2TICK(g_hop_period_ms);
  cur = g_applied_mhz ? g_applied_mhz : hclk_mhz_now();
  pick = hop_pick_locked(cur);
  g_force_mhz = pick;
  g_hop_n++;
  syslog(LOG_WARNING, "dvfs: hop #%u %u -> %u MHz (period %u ms)\n",
         (unsigned)g_hop_n, (unsigned)cur, (unsigned)pick,
         (unsigned)g_hop_period_ms);
  (void)apply_locked();
}

/**
 * @brief LPWORK 周期负载采样与调频入口。
 *
 * @param arg work queue 参数，当前未使用。
 */
static void gov_worker(FAR void *arg)
{
  int ret;

  (void)arg;

  g_gov_last_ticks = (uint32_t)clock_systime_ticks();
  g_gov_runs++;

  nxmutex_lock(&g_lock);
  if (g_hw_ready)
    {
      gov_update_load_locked(idle_pct_now());
      charge_hold_locked();
      hop_tick_locked();
      (void)apply_locked();
    }

  nxmutex_unlock(&g_lock);

  ret = work_queue(LPWORK, &g_gov_work, gov_worker, NULL,
                   MSEC2TICK(DVFS_GOV_POLL_MS));
  if (ret < 0)
    {
      /* 自愈第一层。work_queue_start() 在重挂前本来就会 work_remove 掉残留的
       * 排队状态，所以正常不该失败；真失败（-EINVAL 之类）说明 work_s 或队列
       * 本身出了问题。清一次状态重挂，比永久停摆好 —— 停摆意味着 ondemand、
       * charge hold、hop 全部一起失效，而且以前完全静默。第二层兜底在 diag
       * 线程（它按心跳年龄判定，见 sf32lb_dvfs_gov_kick）。 */
      syslog(LOG_ERR, "dvfs: gov re-arm FAILED %d after %lu runs, self-heal retry\n",
             ret, (unsigned long)g_gov_runs);

      (void)work_cancel(LPWORK, &g_gov_work);
      ret = work_queue(LPWORK, &g_gov_work, gov_worker, NULL,
                       MSEC2TICK(DVFS_GOV_POLL_MS));
      if (ret < 0)
        {
          syslog(LOG_ERR, "dvfs: gov self-heal FAILED %d; diag must kick\n", ret);
        }
      else
        {
          g_gov_recoveries++;
        }
    }
}

/**
 * @brief 外部恢复：把调频 worker 重新挂上（供 diag 线程心跳超时后调用）。
 *
 * @details 与 gov_worker 内部的自愈互补：这里是从**另一个线程**动手，所以
 *          即使 worker 已经彻底不再运行也能把它接回来。先 work_cancel 摘掉
 *          可能的残留排队项再挂，重复挂载会被 work_queue_start 内部合并。
 *
 * @return 0 已重新挂上；负值为 work_queue 的错误码。
 */
int sf32lb_dvfs_gov_kick(void)
{
  int ret;

  (void)work_cancel(LPWORK, &g_gov_work);
  ret = work_queue(LPWORK, &g_gov_work, gov_worker, NULL,
                   MSEC2TICK(DVFS_GOV_POLL_MS));
  if (ret == 0)
    {
      g_gov_recoveries++;
      syslog(LOG_WARNING, "dvfs: gov re-armed by diag (recoveries=%lu)\n",
             (unsigned long)g_gov_recoveries);
    }

  return ret;
}

/**
 * @brief 调频 worker 被外部接回来的累计次数。
 */
uint32_t sf32lb_dvfs_gov_recoveries(void)
{
  return g_gov_recoveries;
}

/**
 * @brief 调频 worker 心跳，供 `ctl dvfs` / `sys` 判"调频还活着吗"。
 */
void sf32lb_dvfs_gov_stat(uint32_t *age_ms, uint32_t *runs)
{
  if (runs != NULL)
    {
      *runs = g_gov_runs;
    }

  if (age_ms == NULL)
    {
      return;
    }

  if (g_gov_last_ticks == 0)
    {
      *age_ms = 0xffffffffu;
      return;
    }

  *age_ms = (uint32_t)TICK2MSEC((uint32_t)clock_systime_ticks() - g_gov_last_ticks);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 启动 DVFS 并注册周期 governor。
 */
void sf32lb_dvfs_boot(void)
{
  nxmutex_lock(&g_lock);
  g_hw_ready = true;
  g_holds |= SF32LB_DVFS_HOLD_BOOT;
  g_applied_mhz = 0;
  g_gov_mhz = SF32LB_DVFS_MHZ_240;
  g_down_candidate_mhz = 0;
  g_interactive_active = false;
  g_idle_pct = idle_pct_now();
  charge_hold_locked();
  (void)apply_locked();
  nxmutex_unlock(&g_lock);
  (void)work_queue(LPWORK, &g_gov_work, gov_worker, NULL,
                   MSEC2TICK(DVFS_GOV_POLL_MS));
}

/**
 * @brief 设置一个或多个 DVFS hold。
 *
 * @param bits SF32LB_DVFS_HOLD_* 位图。
 */
void sf32lb_dvfs_hold(uint32_t bits)
{
  uint32_t old;

  if (bits == 0)
    {
      return;
    }

  nxmutex_lock(&g_lock);
  old = g_holds;
  g_holds |= bits;
  if (old != g_holds)
    {
      (void)apply_locked();
    }

  nxmutex_unlock(&g_lock);
}

/**
 * @brief 清除一个或多个 DVFS hold。
 *
 * @param bits SF32LB_DVFS_HOLD_* 位图。
 */
void sf32lb_dvfs_release(uint32_t bits)
{
  uint32_t old;

  if (bits == 0)
    {
      return;
    }

  nxmutex_lock(&g_lock);
  old = g_holds;
  g_holds &= ~bits;
  if (old != g_holds)
    {
      (void)apply_locked();
    }

  nxmutex_unlock(&g_lock);
}

/**
 * @brief 报告用户交互并立即建立短时交互频率下限。
 *
 * @details
 * 连续输入会刷新截止时间和降档计时；force 与 240 MHz floor hold 优先。
 */
void sf32lb_dvfs_interact(void)
{
  uint32_t floor;
  uint32_t duration;
  uint32_t now;

  nxmutex_lock(&g_lock);
  now = (uint32_t)clock_systime_ticks();
  duration = (uint32_t)MSEC2TICK(CONFIG_MYVENDOR_DVFS_INTERACTIVE_MS);
  if (duration == 0)
    {
      duration = 1;
    }

  g_interactive_deadline = now + duration;
  g_interactive_active = true;
  floor = interactive_mhz();
  if (g_gov_mhz < floor)
    {
      g_gov_mhz = floor;
    }

  /* The down hold starts again from the last input edge.  Once interaction
   * stops, the governor therefore steps down one gear at a time instead of
   * dropping straight to the lowest when the short floor expires. */

  g_down_candidate_mhz = 0;
  (void)apply_locked();
  nxmutex_unlock(&g_lock);
}

/**
 * @brief 设置或取消调试强制频率。
 *
 * @param mhz 0 表示恢复自动；其它值必须为支持的离散档位。
 * @return 0 表示成功，-EINVAL 表示档位无效，-EIO 表示硬件切换失败。
 */
int sf32lb_dvfs_force(uint32_t mhz)
{
  int ret;

  if (mhz != 0 && !mhz_is_gear(mhz))
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_lock);
  /* 显式指定档位即接管控制权：停掉随机跳频，避免下一次跳变把它覆盖掉。 */
  if (g_hop_active)
    {
      g_hop_active = false;
      g_hop_next_ticks = 0;
      syslog(LOG_WARNING,
             "dvfs: hop stop after %u jumps (ctl dvfs %u)\n",
             (unsigned)g_hop_n, (unsigned)mhz);
    }

  g_force_mhz = mhz;
  ret = apply_locked();
  nxmutex_unlock(&g_lock);
  return ret;
}

/**
 * @brief 开关"充电时也按负载动态调频"（见 `g_charge_dyn` 的说明）。
 *
 * @return true = 模式已处在请求的状态（含"本来就是"的情况）。
 * @note 与 `hop` 互不影响：hop 走 force 路径，本模式只改地板掩码。
 */
bool sf32lb_dvfs_charge_dyn_set(bool on)
{
  nxmutex_lock(&g_lock);

  if (g_charge_dyn != on)
    {
      g_charge_dyn = on;
      syslog(LOG_WARNING,
             "dvfs: charge-dyn %s — 充电时 %s（%s）\n",
             on ? "on" : "off",
             on ? "按负载调频，不钉 240" : "恢复钉 240",
             on ? "插电调试=电池行为" : "原策略");
    }

  /* 立刻生效：正在充电时开模式要**马上**从 240 落到 governor 档位，
   * 关模式要马上回到 240；否则要等下一个 gov tick（最长 DVFS_GOV_POLL_MS）。
   *
   * 但 `sf32lb_dvfs_boot()` 之前（bringup 早段从 KV 恢复本模式时会走到这里）
   * 只记标志、不切频：那时外设重配还没准备好，`g_applied_mhz` 仍是 0。
   * boot 时那次 apply 会自然带上这个档位。 */
  if (g_applied_mhz != 0)
    {
      (void)apply_locked();
    }

  nxmutex_unlock(&g_lock);

  return true;
}

/**
 * @brief 查询"充电时也按负载动态调频"模式当前是否开启。
 */
bool sf32lb_dvfs_charge_dyn(void)
{
  bool on;

  nxmutex_lock(&g_lock);
  on = g_charge_dyn;
  nxmutex_unlock(&g_lock);

  return on;
}

int sf32lb_dvfs_hop_start(uint32_t period_ms)
{
  if (period_ms == 0u)
    {
      period_ms = SF32LB_DVFS_HOP_DEFAULT_MS;
    }

  if (period_ms < SF32LB_DVFS_HOP_MIN_MS ||
      period_ms > SF32LB_DVFS_HOP_MAX_MS)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_lock);
  g_hop_period_ms = period_ms;
  g_hop_next_ticks = 0; /* 首次立即跳，便于确认命令生效 */
  g_hop_n = 0;
  /* 起点按开机时刻轮换：每次启动自检的档位次序不同，但每轮仍覆盖全部档位。 */
  g_hop_pos = gear_step(0u, (uint32_t)clock_systime_ticks() / 7u);
  g_hop_active = true;
  syslog(LOG_WARNING,
         "dvfs: hop start period=%u ms start=%u MHz gears=144/240 "
         "(force path)\n",
         (unsigned)period_ms, (unsigned)gear_at(g_hop_pos));
  hop_tick_locked();
  nxmutex_unlock(&g_lock);
  return 0;
}

void sf32lb_dvfs_hop_stop(void)
{
  nxmutex_lock(&g_lock);
  if (g_hop_active)
    {
      g_hop_active = false;
      g_hop_next_ticks = 0;
      /* 交还 governor：清掉跳频期间设的强制档，否则会一直钉在最后一档。 */
      g_force_mhz = 0;
      syslog(LOG_WARNING, "dvfs: hop stop after %u jumps -> auto\n",
             (unsigned)g_hop_n);
      (void)apply_locked();
    }

  nxmutex_unlock(&g_lock);
}

bool sf32lb_dvfs_hop_active(void)
{
  return g_hop_active;
}

uint32_t sf32lb_dvfs_hop_period_ms(void)
{
  return g_hop_period_ms;
}

uint32_t sf32lb_dvfs_hop_count(void)
{
  return (uint32_t)g_hop_n;
}

/**
 * @brief 获取当前已应用的 HCLK。
 *
 * @return HCLK，单位 MHz。
 */
uint32_t sf32lb_dvfs_hclk_mhz(void)
{
  uint32_t mhz = g_applied_mhz;

  /* 已应用档直接返回，避免 UI 轮询堵在切频持锁路径上。 */
  if (mhz != 0)
    {
      return mhz;
    }

  nxmutex_lock(&g_lock);
  mhz = g_applied_mhz ? g_applied_mhz : hclk_mhz_now();
  nxmutex_unlock(&g_lock);
  return mhz;
}

uint8_t sf32lb_dvfs_idle_pct(void)
{
  return g_idle_pct;
}

/**
 * @brief 获取当前 DVFS hold 位图。
 *
 * @return SF32LB_DVFS_HOLD_* 位图。
 */
uint32_t sf32lb_dvfs_holds(void)
{
  uint32_t holds;

  nxmutex_lock(&g_lock);
  holds = g_holds;
  nxmutex_unlock(&g_lock);
  return holds;
}

/**
 * @brief 获取当前调试强制频率。
 *
 * @return 强制频率，单位 MHz；0 表示自动模式。
 */
uint32_t sf32lb_dvfs_force_mhz(void)
{
  uint32_t mhz;

  nxmutex_lock(&g_lock);
  mhz = g_force_mhz;
  nxmutex_unlock(&g_lock);
  return mhz;
}

/**
 * @brief 输出当前 DVFS 状态到 NSH。
 */
void sf32lb_dvfs_print(void)
{
  char names[48];
  uint32_t holds;
  uint32_t applied;
  uint32_t force;
  uint32_t dll2;
  uint32_t gov;
  uint32_t boost;
  uint32_t boost_ms;
  uint32_t now;
  uint8_t idle;

  nxmutex_lock(&g_lock);
  interactive_expire_locked();
  holds = g_holds;
  applied = g_applied_mhz ? g_applied_mhz : hclk_mhz_now();
  force = g_force_mhz;
  idle = g_idle_pct;
  gov = g_gov_mhz;
  boost = 0;
  boost_ms = 0;
  if (g_interactive_active)
    {
      now = (uint32_t)clock_systime_ticks();
      boost = interactive_mhz();
      boost_ms = (uint32_t)TICK2MSEC(
          (clock_t)(g_interactive_deadline - now));
    }

  dll2 = HAL_RCC_HCPU_GetDLL2Freq();
  nxmutex_unlock(&g_lock);

  holds_put(names, sizeof(names), holds);

  /* 格式串写成**单个字面量**（别拆成相邻两段）：本仓写入门禁会把拆开的格式串
   * 当高危拦掉，合成一条也更难出错。末尾新增 charge-dyn 一栏（见 apply_locked
   * 里那条 syslog 的同名说明：开了模式后 holds 仍含 charge 位，不并列显示会
   * 让人误判 hold 记账坏了）。 */
  printf("  dvfs    HCLK %u MHz  %s  idle %u%%  gov %u  boost %u/%ums  holds %s (0x%x)  dll2 %lu  charge-dyn %s\n",
         (unsigned)applied,
         force ? "force" : "auto",
         (unsigned)idle,
         (unsigned)gov,
         (unsigned)boost,
         (unsigned)boost_ms,
         names,
         (unsigned)holds,
         (unsigned long)dll2,
         g_charge_dyn ? "on(charge->load-driven)" : "off");
}
