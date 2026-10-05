/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/myvendor_idle_stat.c
 *
 * True idle: DWT cycles accumulated across WFI in up_idle().
 * On-demand measurement via test idle (no background syslog).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "myvendor_idle_stat.h"

#include <nuttx/clock.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef CONFIG_MYVENDOR_IDLE_CPU_HZ
#  define CONFIG_MYVENDOR_IDLE_CPU_HZ 240000000u
#endif

#define MYVENDOR_DWT_CTRL   (*(volatile uint32_t *)0xe0001000u)
#define MYVENDOR_DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define MYVENDOR_DEMCR      (*(volatile uint32_t *)0xe000edfcu)
#define MYVENDOR_DEMCR_TRCENA      (1u << 24)
#define MYVENDOR_DWT_CYCCNTENA     (1u << 0)
#define MYVENDOR_ITM_LAR    (*(volatile uint32_t *)0xe0000fb0u)
#define MYVENDOR_ITM_LAR_KEY       0xc5acce55u

/* ---- DWT 数据观察点 -------------------------------------------------------
 *
 * 目的：给"谁改坏了这块内存"点名。n004 的 GATT 回调表、2026-09-18 的 wdog
 * 活动链表都是被外力改坏的，日志只留下"值变成什么"，没有"谁写的"。用法：
 *   `ctl wt <addr> [1|2|4]`  在某个地址上挂一个比较器（共 4 个）；
 *   `ctl wt`                 列出现场（含 MATCHED 位）；
 *   `ctl wt clear`           全部撤掉。
 *
 * 两条观察通道，第一条不依赖异常交付：
 *   1. `DWT_FUNCTIONn.MATCHED`(bit24)：**硬件置位**，只要有人访问过这个地址
 *      就为 1，事后 `ctl wt` 直接读得到 —— "到底有没有人碰它"永远可查；
 *   2. 命中会触发 debug 事件。本板常态无调试器，DEMCR.MON_EN=1 让事件被
 *      **交付**（否则可能静默丢弃）：走 arm_m 的 exception_common → 我们既有
 *      的 coredump，`pc` 就是那条访问指令，按 n004 的老办法 addr2line 反解。
 *
 * 注意：DWT 比较器接在核的 load/store 通路上，**DMA 写不会命中**。
 * "值被改坏但 MATCHED=0" 本身就是结论：不是 CPU 写的（DMA / LCPU 邮箱）。
 *
 * ARMv8-M 布局（nuttx/arch/arm/src/arm_m/dwt.h、CMSIS core_cm33.h）：
 *   COMPn=+0x20+0x10n, FUNCTIONn=+0x28+0x10n, MASKn=+0x2c+0x10n
 *   FUNCTION = MATCH[3:0] | (DATAVSIZE[1:0] << 9)
 *   MATCH=0b0100 数据地址观察点；DATAVSIZE: 0=byte 1=half 2=word
 */
#define MYVENDOR_DWT_COMP(n)     (*(volatile uint32_t *)(0xe0001020u + 0x10u * (n)))
#define MYVENDOR_DWT_FUNCTION(n) (*(volatile uint32_t *)(0xe0001028u + 0x10u * (n)))
#define MYVENDOR_DWT_MASK(n)     (*(volatile uint32_t *)(0xe000102cu + 0x10u * (n)))
#define MYVENDOR_DEMCR_MON_EN      (1u << 16)
#define MYVENDOR_DWT_MATCH_DIS     0x0u
#define MYVENDOR_DWT_MATCH_DATA    0x4u
#define MYVENDOR_DWT_MATCHED_BIT   (1u << 24)
#define MYVENDOR_DWT_NCOMP         4u

static uint8_t g_dwt_watch_used;   /* bit0..3 = 比较器已占用 */

static bool g_dwt_ready;

static uint64_t g_win_start_cyc;
static uint64_t g_win_idle_cyc;

static void myvendor_dwt_init(void)
{
  if (g_dwt_ready)
    {
      return;
    }

  MYVENDOR_DEMCR |= MYVENDOR_DEMCR_TRCENA;
  MYVENDOR_ITM_LAR = MYVENDOR_ITM_LAR_KEY;
  MYVENDOR_DWT_CTRL |= MYVENDOR_DWT_CYCCNTENA;
  g_dwt_ready = true;
}

static uint32_t myvendor_dwt_now(void)
{
  return MYVENDOR_DWT_CYCCNT;
}

static uint32_t myvendor_ps_idle_x10(void)
{
#ifdef CONFIG_SCHED_CPULOAD_NONE
  return 0;
#else
  struct cpuload_s cpuload;

  if (clock_cpuload(0, &cpuload) != 0 || cpuload.total == 0)
    {
      return 0;
    }

  return (uint32_t)(1000u - (1000ull * cpuload.active) / cpuload.total);
#endif
}

static void myvendor_idle_stat_fill_snap(struct myvendor_idle_stat_snap_s *snap)
{
  uint32_t now_cyc = myvendor_dwt_now();
  uint64_t total_cyc = (uint64_t)(now_cyc - (uint32_t)g_win_start_cyc);
  uint32_t true_x10;

  if (total_cyc == 0)
    {
      total_cyc = 1;
    }

  true_x10 = (uint32_t)(g_win_idle_cyc * 1000ull / total_cyc);

  snap->true_idle_x10 = true_x10;
  snap->ps_idle_x10 = myvendor_ps_idle_x10();
  snap->win_ms = (uint32_t)(total_cyc * 1000ull / CONFIG_MYVENDOR_IDLE_CPU_HZ);
  snap->idle_cyc = g_win_idle_cyc;
  snap->total_cyc = total_cyc;
}

void myvendor_idle_stat_init(void)
{
  myvendor_dwt_init();

  if (g_win_start_cyc == 0 && g_dwt_ready)
    {
      g_win_start_cyc = myvendor_dwt_now();
    }
}

void myvendor_idle_stat_window_begin(void)
{
  myvendor_dwt_init();
  g_win_start_cyc = myvendor_dwt_now();
  g_win_idle_cyc = 0;
}

void myvendor_idle_stat_on_wfi(uint32_t cycles)
{
  if (!g_dwt_ready)
    {
      myvendor_idle_stat_init();
    }

  g_win_idle_cyc += cycles;
}

void myvendor_idle_stat_snapshot(FAR struct myvendor_idle_stat_snap_s * snap)
{
  if (snap == NULL)
    {
      return;
    }

  myvendor_idle_stat_fill_snap(snap);
}

/****************************************************************************
 * DWT 数据观察点：给"谁改坏了这块内存"点名。
 *
 * 板级/ctl 的调用点用局部 extern 声明（芯片头在这些目标里不可见，见本工程
 * 既有的包含层次约定）。
 ****************************************************************************/

int myvendor_dwt_watch_arm(uint32_t addr, unsigned size_bytes)
{
  unsigned idx;
  unsigned vsize;

  switch (size_bytes)
    {
      case 1:
        vsize = 0;
        break;
      case 2:
        vsize = 1;
        break;
      case 4:
        vsize = 2;
        break;
      default:
        return -EINVAL;
    }

  if ((addr & 0x3u) != 0)
    {
      return -EINVAL;
    }

  myvendor_dwt_init();

  for (idx = 0; idx < MYVENDOR_DWT_NCOMP; idx++)
    {
      if ((g_dwt_watch_used & (1u << idx)) != 0)
        {
          continue;
        }

      /* 先写 COMP/MASK，再开 MATCH；顺序反了会先命中一次旧地址。 */
      MYVENDOR_DWT_COMP(idx) = addr;
      MYVENDOR_DWT_MASK(idx) = 0u;                     /* 只匹配这一个地址 */
      MYVENDOR_DWT_FUNCTION(idx) =
          MYVENDOR_DWT_MATCH_DATA | (vsize << 9);

      /* 无调试器时也保证事件被交付（否则可能静默丢弃）：交付后走
       * exception_common → coredump，pc 就是那条访问指令。 */
      MYVENDOR_DEMCR |= MYVENDOR_DEMCR_MON_EN;

      g_dwt_watch_used |= (uint8_t)(1u << idx);
      return 0;
    }

  return -ENOSPC;
}

void myvendor_dwt_watch_clear(void)
{
  unsigned idx;

  for (idx = 0; idx < MYVENDOR_DWT_NCOMP; idx++)
    {
      MYVENDOR_DWT_FUNCTION(idx) = MYVENDOR_DWT_MATCH_DIS;
      MYVENDOR_DWT_COMP(idx) = 0u;
      MYVENDOR_DWT_MASK(idx) = 0u;
    }

  g_dwt_watch_used = 0u;
}

int myvendor_dwt_watch_status(char *buf, size_t buflen)
{
  unsigned idx;
  int n = 0;
  int used = 0;

  if (buf == NULL || buflen == 0u)
    {
      return 0;
    }

  for (idx = 0; idx < MYVENDOR_DWT_NCOMP; idx++)
    {
      uint32_t fn = MYVENDOR_DWT_FUNCTION(idx);
      uint32_t match = fn & 0xfu;
      uint32_t size_bits = (fn >> 9) & 0x3u;
      unsigned bytes = (size_bits == 0u) ? 1u : (size_bits == 1u ? 2u : 4u);
      int w;

      if (match == MYVENDOR_DWT_MATCH_DIS && (g_dwt_watch_used & (1u << idx)) == 0u)
        {
          continue;
        }

      used++;
      w = snprintf(buf + n, (size_t)((int)buflen - n),
                   "wt[%u] addr=%08lx size=%u %s%s  ", idx,
                   (unsigned long)MYVENDOR_DWT_COMP(idx), bytes,
                   match == MYVENDOR_DWT_MATCH_DATA ? "data" : "?",
                   (fn & MYVENDOR_DWT_MATCHED_BIT) ? " MATCHED" : "");
      if (w <= 0 || w >= (int)buflen - n)
        {
          return n;
        }

      n += w;
    }

  if (used == 0)
    {
      int w = snprintf(buf, buflen, "wt: none armed (MON_EN=%u)\n",
                       (unsigned)((MYVENDOR_DEMCR & MYVENDOR_DEMCR_MON_EN) ? 1u : 0u));
      return w > 0 ? w : 0;
    }

  {
    int w = snprintf(buf + n, (size_t)((int)buflen - n),
                     "MON_EN=%u\n",
                     (unsigned)((MYVENDOR_DEMCR & MYVENDOR_DEMCR_MON_EN) ? 1u : 0u));
    if (w > 0 && w < (int)buflen - n)
      {
        n += w;
      }
  }

  return n;
}
