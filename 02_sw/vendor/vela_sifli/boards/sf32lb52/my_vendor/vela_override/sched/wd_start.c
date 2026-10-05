/**
 * @file wd_start.c
 * @brief 板级抽换 `nuttx/sched/wdog/wd_start.c`：g_wdactivelist 断链不 HardFault。
 *
 * CMake 从 lib `sched` 去掉上游同名源，改编本文件。相对上游：
 * 遍历/摘链前检查 next/prev 落在 HCPU SRAM 或 PSRAM（含 0x10000000
 * 别名与 0x60000000 SBUS）；断环则剪到哨兵后再插入。
 * factory MTP poll 曾在 wd_insert 对 curr=NULL 写 0x7d5 死机。
 * n004 骑行 / n005 factory 在 list_add_before 对 pos->prev==NULL 写 0
 * （DACCVIOL MMFAR=0）。走链认为节点 sane 之后仍可能 pos->prev 为空：
 * rescue 把还在链上的 TCB waitdog 的 next/prev 清掉再递归 wd_insert。
 * 剪尾会丢掉 iwdg_feed 的 2s usleep（排在链后部），IWDT 15s 后回 SFBL。
 * 剪完把各 TCB waitdog 里仍 WDOG_ISACTIVE 但不在活链上的节点重新挂回；
 * 必须在本次 insert 结束之后再 rescue，禁止在走链中递归 insert。
 *
 * 不修改 `nuttx/` 树。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/****************************************************************************
 * sched/wdog/wd_start.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sys/types.h>

/* ---- 抽换件标记（2026-09-18）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件 —— 2026-09-18 曾有个抽换件其实根本不在编译库里（已剔除），
 *    改了半天没生效，就靠这种标记一眼看出来；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉），不依赖任何编译选项（有些目标带 -w，会把 #warning 压掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : nuttx / sched/wdog/wd_start.c
 *   写入时 HEAD: 2ce740a0ac1052c5f51083a334ef3093f59ff780
 *   上游 blob  : f9d8340186a95e7f1d8602020c8da463ec247fe2     （git -C nuttx rev-parse HEAD:sched/wdog/wd_start.c 应等于它）
 *   为什么抽换 : PSRAM waitdog 视为合法；断链才剪环，避免 wd_insert HardFault
 *   版本漂移自查:
 *     git -C nuttx rev-parse HEAD:sched/wdog/wd_start.c   # 与上面的 blob 比对
 *     git -C nuttx diff -- sched/wdog/wd_start.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/sched/wd_start.c -- 上游 nuttx:sched/wdog/wd_start.c@f9d8340186a9 -- PSRAM waitdog 视为合法；断链才剪环，避免 wd_insert HardFault")
const char myvendor_override_marker_sched_wd_start_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/sched/wd_start.c -- 上游 nuttx:sched/wdog/wd_start.c@f9d8340186a9 -- PSRAM waitdog 视为合法；断链才剪环，避免 wd_insert HardFault";

#include <stdint.h>
#include <stdbool.h>
#include <sys/param.h>
#include <unistd.h>
#include <sched.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/list.h>
#include <nuttx/wdog.h>
#include <nuttx/sched_note.h>

#include "sched/sched.h"
#include "wdog/wdog.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SCHED_CRITMONITOR_MAXTIME_WDOG
#  define CONFIG_SCHED_CRITMONITOR_MAXTIME_WDOG 0
#endif

/* HCPU SRAM 512 KiB. TCB waitdogs after umem r1 live in PSRAM_DATA
 * (SBUS 0x60400000, HCPU alias 0x10000000). Too-narrow whitelist used
 * to clip ble_companion and drop iwdg_feed's usleep. */

#define WD_NODE_SRAM_BASE     0x20000000ul
#define WD_NODE_SRAM_END      0x20080000ul
#define WD_NODE_PSRAM_H_BASE  0x10000000ul
#define WD_NODE_PSRAM_H_END   0x11000000ul
#define WD_NODE_PSRAM_S_BASE  0x60000000ul
#define WD_NODE_PSRAM_S_END   0x61000000ul
#define WD_WALK_MAX           256u

#if CONFIG_SCHED_CRITMONITOR_MAXTIME_WDOG > 0
#  define CALL_FUNC(func, arg) \
     do \
       { \
         clock_t start; \
         clock_t elapsed; \
         sched_note_wdog(NOTE_WDOG_ENTER, func, (FAR void *)arg); \
         start = perf_gettime(); \
         func(arg); \
         elapsed = perf_gettime() - start; \
         sched_note_wdog(NOTE_WDOG_LEAVE, func, (FAR void *)arg); \
         if (elapsed > CONFIG_SCHED_CRITMONITOR_MAXTIME_WDOG) \
           { \
             CRITMONITOR_PANIC("WDOG %p, %s IRQ, execute too long %ju\n", \
                               func, up_interrupt_context() ? \
                               "IN" : "NOT", (uintmax_t)elapsed); \
           } \
       } \
     while (0)
#else
#  define CALL_FUNC(func, arg) \
      do \
        { \
          sched_note_wdog(NOTE_WDOG_ENTER, func, (FAR void *)arg); \
          func(arg); \
          sched_note_wdog(NOTE_WDOG_LEAVE, func, (FAR void *)arg); \
        } \
      while (0)

#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_SCHED_TICKLESS
static DEFINE_PER_CPU_BSS_BMP(unsigned int, g_wdtimernested);
#  define g_wdtimernested this_cpu_var_bmp(g_wdtimernested)
#endif

static volatile uint32_t g_wd_list_clips;
static volatile uintptr_t g_wd_clip_bad;
static volatile uintptr_t g_wd_clip_next;
static volatile uintptr_t g_wd_clip_prev;
static volatile unsigned g_wd_clip_walk;

/** 链表快照：裁剪前把还在表上的前 N 个 wdog 节点记下来（2026-09-27 增）。
 *
 *  为什么光有 bad/next/prev 不够：那天现场坏节点的邻居"正好是工作队列自己的
 *  定时器"——而 worker 的唤醒就靠它（`wd_start_abstick(&wqueue->timer, …,
 *  work_timer_expired, …)` ⇒ `nxsem_post`），于是"队列里有活、worker 却在睡"
 *  的丢唤醒现场就这么来的。要一眼看出"坏的是哪一类定时器、表上还剩谁"，
 *  必须把整条表拍下来；`func` 一栏事后用 ELF 解出归属
 *  （`work_timer_expired`=NuttX wqueue / `work_timeout`=zblue / `nxsem_timeout`=任务等待）。
 *
 *  节点布局（include/nuttx/wdog.h）：node.next, node.prev, arg, func, expired。 */
#define WD_CLIP_SNAP 8u
static volatile unsigned  g_wd_clip_snap_n;
static volatile uintptr_t g_wd_clip_snap[WD_CLIP_SNAP][6];
/** 裁剪时的判据细节：why 位掩码 + 两个反向指针的实际指向。
 *  1=node 不合法 2=next 不合法 4=prev 不合法 8=prev->next!=node 16=next->prev!=node
 *  32=走超限。**"邻居没指回来"就是重复挂链的签名**（2026-09-18 两次裁剪都是它）。 */
static volatile uint8_t   g_wd_clip_why;
static volatile uintptr_t g_wd_clip_pp;
static volatile uintptr_t g_wd_clip_np;
static volatile unsigned g_wd_rescued;
/** "自称在跑、但链表链接不可信"的节点被按位置摘掉的次数（见 wd_unlink_by_walk）。 */
static volatile uint32_t g_wd_stale_unlinks;

/** 最近的看门狗操作环：裁剪时把最后几次操作打出来，回答"谁是第一个坏"。
 *
 *  现场（2026-09-18）两次裁剪形状不同：一次坏节点是 hpwork 的延时定时器、
 *  一次是 **gnss 任务的 waitdog 且 next/prev 已被清成 NULL**（`wd_expiration`
 *  摘节点时 `list_delete()` 就会清链接）。最能在两者之间做出判决的证据是
 *  "裁剪之前发生过什么"：同一个地址被 START 两次而中间没有 EXPIRE = 重复插入。
 *  记的是**调用者 PC**（`__builtin_return_address(0)`），事后 addr2line 就能
 *  点名是谁发起的。只写内存、不打日志，可以安全地在临界区里记。 */
#define WD_OP_RING 16

enum wd_op_kind
{
  WD_OP_NONE = 0,
  WD_OP_START,     /**< wd_start/wd_start_abstick：装一个定时器。 */
  WD_OP_EXPIRE,    /**< wd_expiration 摘表头（list_delete 会把节点链接清空）。 */
  WD_OP_RESCUE,    /**< wd_rescue_orphans 重插一个"孤儿"。 */
  WD_OP_CLIP,      /**< wd_clip_list：裁剪。 */
  WD_OP_UNLINK,    /**< wd_unlink_by_walk：按位置摘掉链接不可信的节点。 */
  WD_OP_CANCEL,    /**< wd_cancel：取消一个定时器（wd_cancel.c 记的）。 */
  WD_OP_CANCEL_FIX,/**< wd_cancel 里反向指针不吻合、改走按位置摘（见 wd_cancel.c）。 */
};

struct wd_op_rec
{
  uint32_t  pc;
  uintptr_t addr;
  uint32_t  ticks;
  uint8_t   op;
};

static struct wd_op_rec g_wd_ops[WD_OP_RING];
static volatile unsigned g_wd_op_n;

static inline_function void wd_op_rec_add(uint8_t op, uintptr_t addr,
                                          clock_t ticks, uint32_t pc)
{
  unsigned i = g_wd_op_n % WD_OP_RING;

  g_wd_ops[i].op    = op;
  g_wd_ops[i].pc    = pc;
  g_wd_ops[i].addr  = addr;
  g_wd_ops[i].ticks = (uint32_t)ticks;
  g_wd_op_n++;
}

#define WD_OP_ADD(op, addr, ticks) \
  wd_op_rec_add((op), (uintptr_t)(addr), (ticks), \
                (uint32_t)(uintptr_t)__builtin_return_address(0))

static const char *wd_op_name(uint8_t op)
{
  switch (op)
    {
      case WD_OP_START:  return "START";
      case WD_OP_EXPIRE: return "EXPIRE";
      case WD_OP_RESCUE: return "RESCUE";
      case WD_OP_CLIP:   return "CLIP";
      case WD_OP_UNLINK: return "UNLINK";
      case WD_OP_CANCEL: return "CANCEL";
      case WD_OP_CANCEL_FIX: return "CXFIX";
      default:           return "?";
    }
}

/* 本文件的静态辅助函数定义在后面，包装要先声明它。 */
static bool wd_unlink_by_walk(FAR struct wdog_s *wdog);

/** @brief 供 wd_cancel.c 记一条操作（它不共享本文件的静态环）。 */
void myvendor_wd_op(uint8_t op, uintptr_t addr, uint32_t pc)
{
  wd_op_rec_add(op, addr, 0, pc);
}

/** @brief 供 wd_cancel.c 用：按位置摘掉一个链接不可信的节点。
 *  @return true = 找到并摘掉了。 */
bool myvendor_wd_unlink_by_walk(FAR struct wdog_s *wdog);

/** @brief 打印最近的操作（调用方在临界区外）。 */
static void wd_op_dump(void)
{
  unsigned n = g_wd_op_n;
  unsigned i;

  if (n == 0)
    {
      return;
    }

  if (n > WD_OP_RING)
    {
      n = WD_OP_RING;
    }

  syslog(LOG_ERR, "wdog: last %u ops (oldest first, pc=caller):\n", n);
  for (i = 0; i < n; i++)
    {
      unsigned idx = (g_wd_op_n - n + i) % WD_OP_RING;

      syslog(LOG_ERR, "wdog: op %-6s addr=%08lx ticks=%lu pc=%08lx\n",
             wd_op_name(g_wd_ops[idx].op),
             (unsigned long)g_wd_ops[idx].addr,
             (unsigned long)g_wd_ops[idx].ticks,
             (unsigned long)g_wd_ops[idx].pc);
    }
}
static int g_wd_rescuing;
static int g_wd_need_rescue;

/** 裁剪时**被丢出链表**的 TCB waitdog（谁的超时没了）。裁剪发生在临界区里，
 *  而 syslog 不安全，所以先在临界区里记下来、出来再打。这条信息是"这次裁剪
 *  到底害了谁"的唯一来源：2026-09-18 现场里 DVFS 心跳停摆 2.1 s、HCI 命令
 *  超时不再触发，而同一时刻一次裁剪救回了 7 个孤儿节点 —— 如果没有这份名单，
 *  就只能靠时间上的巧合去猜。 */
#define WD_CLIP_MAX_DROP 6

static volatile uint8_t g_wd_clip_drop_n;
static volatile int16_t g_wd_clip_drop_pid[WD_CLIP_MAX_DROP];
static char g_wd_clip_drop_name[WD_CLIP_MAX_DROP][12];

static bool wd_insert(FAR struct wdog_s *wdog, clock_t expired,
                      wdentry_t wdentry, wdparm_t arg);

/****************************************************************************
 * Name: wd_node_sane
 *
 * Description:
 *   True if node looks like a list_node in HCPU SRAM or PSRAM_DATA
 *   (not NULL / 0x7d5 / LCPU mailbox).
 ****************************************************************************/

static inline_function bool wd_node_sane(FAR const struct list_node *node)
{
  uintptr_t a = (uintptr_t)node;

  if (node == NULL || (a & 0x3ul) != 0)
    {
      return false;
    }

  if (a >= WD_NODE_SRAM_BASE && a < WD_NODE_SRAM_END)
    {
      return true;
    }

  if (a >= WD_NODE_PSRAM_H_BASE && a < WD_NODE_PSRAM_H_END)
    {
      return true;
    }

  return a >= WD_NODE_PSRAM_S_BASE && a < WD_NODE_PSRAM_S_END;
}

/****************************************************************************
 * Name: wd_on_list
 *
 * Description:
 *   True if wdog's node has sane next/prev (safe to list_delete).
 ****************************************************************************/

static inline_function bool wd_on_list(FAR const struct wdog_s *wdog)
{
  return wd_node_sane(wdog->node.next) && wd_node_sane(wdog->node.prev);
}

/****************************************************************************
 * Name: wd_can_insert_before
 *
 * Description:
 *   list_add_before(pos) does pos->prev->next = node. prev==NULL is MMFAR=0.
 ****************************************************************************/

static inline_function bool wd_can_insert_before(FAR const struct list_node *pos)
{
  return wd_node_sane(pos) && wd_node_sane(pos->prev) &&
         pos->prev->next == pos;
}

/****************************************************************************
 * Name: wd_list_walkable
 *
 * Description:
 *   True if the live list can be walked without hitting a corrupt node.
 ****************************************************************************/

static bool wd_list_walkable(void)
{
  FAR struct list_node *list = &g_wdactivelist;
  FAR struct list_node *node;
  unsigned n = 0;

  if (!wd_can_insert_before(list) || !wd_node_sane(list->next))
    {
      return false;
    }

  for (node = list->next; node != list; node = node->next)
    {
      if (!wd_node_sane(node) || !wd_node_sane(node->next) ||
          !wd_node_sane(node->prev) || node->prev->next != node ||
          node->next->prev != node || ++n > WD_WALK_MAX)
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: wd_clip_list
 *
 * Description:
 *   Close the circular list after last_good; drop the corrupt tail.
 ****************************************************************************/

static inline_function void wd_clip_list(FAR struct list_node *last_good,
                                         uintptr_t bad, uintptr_t nxt,
                                         uintptr_t prv, unsigned walk)
{
  FAR struct list_node *list = &g_wdactivelist;

  WD_OP_ADD(WD_OP_CLIP, bad, 0);

  g_wd_list_clips++;
  g_wd_clip_bad = bad;
  g_wd_clip_next = nxt;
  g_wd_clip_prev = prv;
  g_wd_clip_walk = walk;
  g_wd_clip_drop_n = 0;

  /* 链表快照（裁剪前，趁表还没接回环形）：addr/next/prev/arg/func/expired。 */
  {
    FAR struct list_node *snap = list->next;
    unsigned              i = 0;

    while (snap != list && i < WD_CLIP_SNAP &&
           wd_node_sane(snap) && wd_node_sane(snap->next))
      {
        FAR const uint32_t *raw = (FAR const uint32_t *)snap;

        g_wd_clip_snap[i][0] = (uintptr_t)snap;
        g_wd_clip_snap[i][1] = (uintptr_t)snap->next;
        g_wd_clip_snap[i][2] = (uintptr_t)snap->prev;
        g_wd_clip_snap[i][3] = (uintptr_t)raw[2];    /* arg */
        g_wd_clip_snap[i][4] = (uintptr_t)raw[3];    /* func */
        g_wd_clip_snap[i][5] = (uintptr_t)raw[4];    /* expired（tick） */

        i++;
        snap = snap->next;
      }

    g_wd_clip_snap_n = i;
  }

  if (last_good == list || !wd_node_sane(last_good))
    {
      list_initialize(list);
      return;
    }

  last_good->next = list;
  list->prev = last_good;
}

/****************************************************************************
 * Name: wd_node_on_live_list
 ****************************************************************************/

/**
 * @brief 三态判定：这个节点在活动链表上吗。
 *
 * @return 1 = 在表上；0 = **确定**不在（可以是孤儿，允许 rescue 重插）；
 *         -1 = 链表走不动，未知。
 *
 * 为什么必须是三态（2026-09-18 现场）：以前这里是 bool，"走不动"和"不在表上"
 * 都返回 false。后果是**链表里只要有一个坏节点，整轮 walk 就在那里中断**，于是
 * 每个任务都被判成孤儿 —— rescue 把它们的链接逐个清成 NULL 再重插，
 * 于是重复节点、下一次裁剪再丢一批，自己把自己养大。现场证据：`rescued=7`
 * 而真正坏的只有一个节点；被清成 `00000000 00000000` 的 gnss waitdog 节点
 * 就是这套动作留下的形状。
 */
static inline_function int wd_node_live_state(FAR const struct list_node *want)
{
  FAR struct list_node *list = &g_wdactivelist;
  FAR struct list_node *node;
  unsigned n = 0;

  if (want == NULL || !wd_node_sane(list->next))
    {
      return -1;
    }

  for (node = list->next; node != list; node = node->next)
    {
      if (!wd_node_sane(node) || ++n > WD_WALK_MAX)
        {
          return -1;
        }

      if (node == want)
        {
          return 1;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: wd_rescue_orphans
 *
 * Description:
 *   Re-queue TCB waitdogs that are still marked active but were dropped
 *   by a clip. iwdg_feed's usleep sits on waitdog near the tail (~2 s).
 *   Caller holds the wdog critical section. Do not nxsched_get/put_tcb:
 *   put_tcb can usleep if refs hit the exit path.
 *   Walk abort used to look like "not on list" and then NULL a live
 *   node's prev; the next list_add_before wrote MMFAR=0 (n004/n005).
 ****************************************************************************/

static void wd_rescue_orphans(void)
{
  int ndx;
  int npidhash;
  unsigned rescued = 0;

  g_wd_need_rescue = 0;
  if (g_wd_rescuing)
    {
      return;
    }

  g_wd_rescuing = 1;
  if (g_pidhash == NULL || !wd_list_walkable())
    {
      g_wd_rescuing = 0;
      return;
    }

  npidhash = g_npidhash;

  for (ndx = 0; ndx < npidhash; ndx++)
    {
      FAR struct tcb_s *tcb = g_pidhash[ndx];
      FAR struct wdog_s *wdog;
      wdentry_t func;
      wdparm_t arg;
      clock_t expired;

      if (tcb == NULL)
        {
          continue;
        }

      wdog = &tcb->waitdog;
      if (!wd_node_sane(&wdog->node) || !WDOG_ISACTIVE(wdog))
        {
          continue;
        }

      /* 只有**确定**不在表上才当孤儿重插（见 wd_node_live_state 的注释）。 */
      if (wd_node_live_state(&wdog->node) != 0)
        {
          continue;
        }

      func = wdog->func;
      arg = wdog->arg;
      expired = wdog->expired;
      wdog->node.next = NULL;
      wdog->node.prev = NULL;
      WD_OP_ADD(WD_OP_RESCUE, wdog, expired);
      (void)wd_insert(wdog, expired, func, arg);
      rescued++;

      /* 名单：这次裁剪害了谁（只在有待上报的裁剪时记，见 g_wd_clip_drop_*）。 */
      if (g_wd_list_clips != 0 && g_wd_clip_drop_n < WD_CLIP_MAX_DROP)
        {
          unsigned slot = g_wd_clip_drop_n;
          unsigned k = 0;

          g_wd_clip_drop_pid[slot] = (int16_t)tcb->pid;
          while (k < 11u && tcb->name[k] != '\0')
            {
              g_wd_clip_drop_name[slot][k] = tcb->name[k];
              k++;
            }

          g_wd_clip_drop_name[slot][k] = '\0';
          g_wd_clip_drop_n = (uint8_t)(slot + 1u);
        }
    }

  g_wd_rescued = rescued;
  g_wd_rescuing = 0;
}

/****************************************************************************
 * Name: wd_clip_report_owner
 *
 * Description:
 *   Best-effort naming of an address that should belong to a wdog node:
 *   a TCB waitdog (named), or something else (work-queue timer / static
 *   wdog) which is exactly the blind spot of wd_rescue_orphans.
 ****************************************************************************/

static void wd_clip_report_owner(FAR const char *what, uintptr_t addr)
{
  int ndx;
  int npidhash = g_npidhash;

  if (g_pidhash != NULL)
    {
      for (ndx = 0; ndx < npidhash; ndx++)
        {
          FAR struct tcb_s *tcb = g_pidhash[ndx];

          if (tcb == NULL)
            {
              continue;
            }

          if ((uintptr_t)&tcb->waitdog.node == addr ||
              (uintptr_t)&tcb->waitdog == addr)
            {
              syslog(LOG_ERR,
                     "wdog: clip %s=%08lx is waitdog.node of pid=%d name=%s\n",
                     what, (unsigned long)addr, (int)tcb->pid, tcb->name);
              return;
            }

          if ((uintptr_t)tcb == addr)
            {
              syslog(LOG_ERR, "wdog: clip %s=%08lx is tcb pid=%d name=%s\n",
                     what, (unsigned long)addr, (int)tcb->pid, tcb->name);
              return;
            }
        }
    }

  syslog(LOG_ERR,
         "wdog: clip %s=%08lx is no tcb waitdog "
         "(work-queue timer or static wdog)\n",
         what, (unsigned long)addr);
}

/****************************************************************************
 * Name: wd_report_clips
 *
 * Description:
 *   Log outside the critical section (syslog is not CS-safe).
 ****************************************************************************/

static inline_function void wd_report_clips(void)
{
  uint32_t n = g_wd_list_clips;

  if (g_wd_stale_unlinks != 0 && !up_interrupt_context())
    {
      uint32_t stale = g_wd_stale_unlinks;

      g_wd_stale_unlinks = 0;
      syslog(LOG_ERR,
             "wdog: unlinked %lu node(s) with stale links that were still on "
             "the list (would otherwise be inserted twice)\n",
             (unsigned long)stale);
    }

  if (n != 0 && !up_interrupt_context())
    {
      unsigned rescued = g_wd_rescued;
      unsigned i;

      g_wd_list_clips = 0;
      syslog(LOG_ERR,
             "wdog: clipped corrupt g_wdactivelist (%u) "
             "bad=%08lx n=%08lx p=%08lx walk=%u rescued=%u\n",
             (unsigned)n,
             (unsigned long)g_wd_clip_bad,
             (unsigned long)g_wd_clip_next,
             (unsigned long)g_wd_clip_prev,
             g_wd_clip_walk,
             rescued);

      syslog(LOG_ERR,
             "wdog: clip why=0x%02x prev.next=%08lx next.prev=%08lx\n",
             (unsigned)g_wd_clip_why,
             (unsigned long)g_wd_clip_pp, (unsigned long)g_wd_clip_np);

      /* 三个地址分别属于谁：TCB waitdog（点名 pid/任务名），还是别的
       * （work-queue 定时器 / 静态 wdog —— 那正是 wd_rescue_orphans 的盲区，
       * 它只救 TCB waitdog）。 */
      wd_clip_report_owner("bad", g_wd_clip_bad);
      wd_clip_report_owner("next", g_wd_clip_next);
      wd_clip_report_owner("prev", g_wd_clip_prev);

      /* 坏节点原地的字节：指针形状、ASCII、还是 trap（n004 是 0xfee7fee7）。 */
      if (wd_node_sane((FAR const struct list_node *)g_wd_clip_bad))
        {
          FAR const uint32_t *raw =
              (FAR const uint32_t *)g_wd_clip_bad;

          syslog(LOG_ERR,
                 "wdog: clip raw @%08lx: %08lx %08lx %08lx %08lx "
                 "%08lx %08lx %08lx %08lx\n",
                 (unsigned long)g_wd_clip_bad,
                 (unsigned long)raw[0], (unsigned long)raw[1],
                 (unsigned long)raw[2], (unsigned long)raw[3],
                 (unsigned long)raw[4], (unsigned long)raw[5],
                 (unsigned long)raw[6], (unsigned long)raw[7]);
        }
      else
        {
          syslog(LOG_ERR, "wdog: clip bad=%08lx out of reachable RAM, no raw dump\n",
                 (unsigned long)g_wd_clip_bad);
        }

      /* 被这次裁剪丢出链表、又救回来的 TCB：这些就是"超时不再触发"的受害者。 */
      if (g_wd_clip_drop_n == 0)
        {
          syslog(LOG_ERR, "wdog: clip dropped no tcb waitdog (corruption hit a non-tcb node?)\n");
        }
      else
        {
          for (i = 0; i < g_wd_clip_drop_n; i++)
            {
              syslog(LOG_ERR, "wdog: clip dropped pid=%d name=%s\n",
                     (int)g_wd_clip_drop_pid[i], g_wd_clip_drop_name[i]);
            }
        }

      g_wd_clip_drop_n = 0;

      /* **链表快照**：坏节点之外表上还有谁。`func` 一栏是归属判据
       * （`work_timer_expired`=NuttX 工作队列的唤醒定时器 / `work_timeout`=zblue
       * 自己的工作队列 / `nxsem_timeout`=任务在等超时）；`exp` 与稍后的
       * `op START ticks=` 对得上就说明这个定时器"还在表上但永远不到期"。 */
      for (i = 0; i < g_wd_clip_snap_n; i++)
        {
          syslog(LOG_ERR,
                 "wdog: clip snap[%u] addr=%08lx next=%08lx prev=%08lx "
                 "arg=%08lx func=%08lx exp=%lu\n",
                 i,
                 (unsigned long)g_wd_clip_snap[i][0],
                 (unsigned long)g_wd_clip_snap[i][1],
                 (unsigned long)g_wd_clip_snap[i][2],
                 (unsigned long)g_wd_clip_snap[i][3],
                 (unsigned long)g_wd_clip_snap[i][4],
                 (unsigned long)g_wd_clip_snap[i][5]);
        }

      g_wd_clip_snap_n = 0;

      /* 裁剪现场的最后几次操作 —— 回答"第一个坏的到底是哪一步"。 */
      wd_op_dump();
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wd_expiration
 *
 * Description:
 *   Check if the timer for the watchdog at the head of list is ready to
 *   run. If so, remove the watchdog from the list and execute it.
 *   Note: This function should always be called in the interrupt context.
 *
 * Input Parameters:
 *   ticks - current time in ticks
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static inline_function clock_t wd_expiration(clock_t ticks)
{
  FAR struct wdog_s *wdog;
  irqstate_t         flags;
  wdentry_t          func;
  wdparm_t           arg;
  clock_t            ret = CLOCK_MAX;

  flags = enter_critical_section();

#ifdef CONFIG_SCHED_TICKLESS
  /* Increment the nested watchdog timer count to handle cases where wd_start
   * is called in the watchdog callback functions.
   */

  g_wdtimernested++;
#endif

  /* Process the watchdog at the head of the list as well as any
   * other watchdogs that became ready to run at this time
   */

  while (!list_is_empty(&g_wdactivelist))
    {
      FAR struct list_node *first = g_wdactivelist.next;

      if (!wd_node_sane(first) || !wd_node_sane(first->next) ||
          !wd_node_sane(first->prev))
        {
          uintptr_t nxt = 0;
          uintptr_t prv = 0;

          if (wd_node_sane(first))
            {
              nxt = (uintptr_t)first->next;
              prv = (uintptr_t)first->prev;
            }

          g_wd_clip_why = 64u;      /* 表头分支 */
          g_wd_clip_pp = 0;
          g_wd_clip_np = 0;
          wd_clip_list(&g_wdactivelist, (uintptr_t)first, nxt, prv, 0);
          g_wd_need_rescue = 1;
          wd_rescue_orphans();
          continue;
        }

      wdog = list_first_entry(&g_wdactivelist, struct wdog_s, node);

      /* Check if expected time is expired */

      if (!clock_compare(wdog->expired, ticks))
        {
          ret = wdog->expired - ticks;
          break;
        }

      /* Remove the watchdog from the head of the list */

      WD_OP_ADD(WD_OP_EXPIRE, wdog, ticks);
      list_delete(&wdog->node);

      /* Indicate that the watchdog is no longer active. */

      func = wdog->func;
      arg = wdog->arg;
      wdog->func = NULL;

      /* Execute the watchdog function */

      if (func != NULL)
        {
          up_setpicbase(wdog->picbase);

          CALL_FUNC(func, arg);
        }
    }

#ifdef CONFIG_SCHED_TICKLESS
  /* Decrement the nested watchdog timer count */

  g_wdtimernested--;
#endif

  leave_critical_section(flags);

  return ret;
}

bool myvendor_wd_unlink_by_walk(FAR struct wdog_s *wdog)
{
  return wd_unlink_by_walk(wdog);
}

/****************************************************************************
 * Name: wd_insert
 *
 * Description:
 *   Insert the timer into the global list to ensure that
 *   the list is sorted in increasing order of expiration absolute time.
 *
 * Input Parameters:
 *   wdog     - Watchdog ID
 *   expired  - expired absolute time in clock ticks
 *   wdentry  - Function to call on timeout
 *   arg      - Parameter to pass to wdentry
 *
 * Assumptions:
 *   wdog and wdentry is not NULL.
 *
 * Returned Value:
 *   Whether the head of the watchdog list has changed.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: wd_unlink_by_walk
 *
 * Description:
 *   Unlink a watchdog that still claims to be active but whose own node links
 *   are not trustworthy, **by walking the list and using the verified
 *   neighbour relationship** instead of the node's own prev/next.
 *
 *   Why this exists (2026-09-18 clip report):
 *     wd_start_abstick only deletes an already-active wdog when
 *     wd_on_list(wdog) says its links are sane.  When they are not, the node may
 *     *still be on the list* -- and the following wd_insert() then links the same
 *     node a second time.  That produces exactly the shape the clip reported:
 *     the bad node's own next/prev pointed at two live waitdogs and its func was
 *     the work-queue timer callback (g_hpwork+0x20, func=work_timer_expired),
 *     yet the walk failed on "neighbour does not point back" -- i.e. a
 *     double-linked node.  Losing that node cost six tcb timeouts (iwdg_feed,
 *     bicycle_ui, gnss, board_sensor, mtp_simple, diag) and the DVFS governor
 *     timer, and BLE never came back afterwards.
 *
 *   Caller holds the wdog critical section; no logging here (syslog is not
 *   CS-safe) -- the count is reported later by wd_report_clips().
 *
 * Returned Value:
 *   true = the node was found and removed; false = not found, or the list
 *   itself is not clean (leave it to the clip path).
 *
 ****************************************************************************/

static bool wd_unlink_by_walk(FAR struct wdog_s *wdog)
{
  FAR struct list_node *list = &g_wdactivelist;
  FAR struct list_node *prev = list;
  FAR struct list_node *node;
  unsigned n = 0;

  if (!wd_node_sane(list->next))
    {
      return false;
    }

  for (node = list->next; node != list; node = node->next)
    {
      if (!wd_node_sane(node) || !wd_node_sane(node->next) ||
          node->prev != prev || ++n > WD_WALK_MAX)
        {
          return false;
        }

      if (node == &wdog->node)
        {
          FAR struct list_node *next = node->next;

          prev->next = next;
          next->prev = prev;
          node->next = NULL;
          node->prev = NULL;
          WD_OP_ADD(WD_OP_UNLINK, wdog, 0);
          return true;
        }

      prev = node;
    }

  return false;
}

static bool wd_insert(FAR struct wdog_s *wdog, clock_t expired,
                      wdentry_t wdentry, wdparm_t arg)
{
  FAR struct list_node *list = &g_wdactivelist;
  FAR struct list_node *node;
  FAR struct list_node *pos;
  FAR struct list_node *last_good;
  FAR struct wdog_s *curr;
  FAR struct wdog_s *head;
  unsigned n = 0;

  if (!wd_can_insert_before(list) || !wd_node_sane(list->next))
    {
      uintptr_t nxt = (uintptr_t)list->next;
      uintptr_t prv = (uintptr_t)list->prev;

      wd_clip_list(list, (uintptr_t)list, nxt, prv, 0);
      g_wd_need_rescue = 1;
    }

  /* Sentinel as first_entry when the list is empty is intentional. */

  head = list_first_entry(list, struct wdog_s, node);
  pos = list;
  last_good = list;

  for (node = list->next; node != list; )
    {
      FAR struct list_node *nxt;

      if (!wd_node_sane(node) || !wd_node_sane(node->next) ||
          !wd_node_sane(node->prev) || node->prev->next != node ||
          node->next->prev != node || ++n > WD_WALK_MAX)
        {
          uintptr_t bad_n = wd_node_sane(node) ? (uintptr_t)node->next : 0;
          uintptr_t bad_p = wd_node_sane(node) ? (uintptr_t)node->prev : 0;
          bool n_sane = wd_node_sane(node);
          bool nx_sane = n_sane && wd_node_sane(node->next);
          bool pv_sane = n_sane && wd_node_sane(node->prev);

          g_wd_clip_why = (uint8_t)((!n_sane ? 1u : 0u) |
                                    (!nx_sane ? 2u : 0u) |
                                    (!pv_sane ? 4u : 0u) |
                                    (pv_sane && node->prev->next != node ? 8u : 0u) |
                                    (nx_sane && node->next->prev != node ? 16u : 0u) |
                                    (n >= WD_WALK_MAX ? 32u : 0u));
          g_wd_clip_pp = (pv_sane ? (uintptr_t)node->prev->next : 0);
          g_wd_clip_np = (nx_sane ? (uintptr_t)node->next->prev : 0);

          wd_clip_list(last_good, (uintptr_t)node, bad_n, bad_p, n);
          g_wd_need_rescue = 1;
          pos = list;
          head = list_first_entry(list, struct wdog_s, node);
          break;
        }

      curr = list_entry(node, struct wdog_s, node);
      nxt = node->next;

      /* Until curr->expired has not timed out relative to expired */

      if (!clock_compare(curr->expired, expired))
        {
          pos = node;
          break;
        }

      last_good = node;
      node = nxt;
    }

  /* pos is the first node that has not expired, or the sentinel. */

  if (!wd_can_insert_before(pos))
    {
      list_initialize(list);
      pos = list;
      g_wd_need_rescue = 1;
    }

  list_add_before(pos, &wdog->node);

  /* **插入后自检**：新挂上的这条链接必须双向一致。
   * 现场（2026-09-19）wdog 链表反复损坏，但"重复插入"防线从未报过，说明是别的路径
   * 把某个节点的 next/prev 写歪 —— 与其等 head 走到坏节点才 CLIP，不如在**写歪的那一次**
   * 就报出来：不成立时把调用者 PC、pos、当前节点都打出来（限频，避免刷屏）。 */
  if (wd_node_sane(pos) && wd_node_sane((FAR struct list_node *)wdog->node.next))
    {
      FAR struct list_node *nx = (FAR struct list_node *)wdog->node.next;

      if (nx->prev != &wdog->node)
        {
          static unsigned s_bad_link;

          if (s_bad_link < 8u)
            {
              s_bad_link++;
              syslog(LOG_ERR,
                     "wdog: INSERT LINK BAD(%u) wdog=%08lx pos=%08lx next=%08lx "
                     "next->prev=%08lx pc=%08lx\n",
                     s_bad_link, (unsigned long)wdog, (unsigned long)pos,
                     (unsigned long)nx, (unsigned long)nx->prev,
                     (unsigned long)(uintptr_t)__builtin_return_address(0));
            }
        }
    }

  wdog->func = wdentry;
  up_getpicbase(&wdog->picbase);
  wdog->arg = arg;
  wdog->expired = expired;

  /* Return whether the head of the watchdog list has changed. */

  return head == list_entry(pos, struct wdog_s, node);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wd_start_abstick
 *
 * Description:
 *   This function adds a watchdog timer to the active timer queue.  The
 *   specified watchdog function at 'wdentry' will be called from the
 *   interrupt level after the specified number of ticks has reached.
 *   Watchdog timers may be started from the interrupt level.
 *
 *   Watchdog timers execute in the address environment that was in effect
 *   when wd_start() is called.
 *
 *   Watchdog timers execute only once.
 *
 *   To replace either the timeout delay or the function to be executed,
 *   call wd_start again with the same wdog; only the most recent wdStart()
 *   on a given watchdog ID has any effect.
 *
 * Input Parameters:
 *   wdog     - Watchdog ID
 *   ticks    - Absolute time in clock ticks
 *   wdentry  - Function to call on timeout
 *   arg      - Parameter to pass to wdentry.
 *
 *   NOTE:  The parameter must be of type wdparm_t.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is return to
 *   indicate the nature of any failure.
 *
 * Assumptions:
 *   The watchdog routine runs in the context of the timer interrupt handler
 *   and is subject to all ISR restrictions.
 *
 ****************************************************************************/

int wd_start_abstick(FAR struct wdog_s *wdog, clock_t ticks,
                     wdentry_t wdentry, wdparm_t arg)
{
  int ret = -EINVAL;
  irqstate_t flags;
  bool reassess = false;

  /* Verify the wdog and setup parameters */

  if (wdog != NULL && wdentry != NULL)
    {
      /* NOTE:  There is a race condition here... the caller may receive
       * the watchdog between the time that wd_start_abstick is called and
       * the critical section is established.
       */

      flags = enter_critical_section();
      WD_OP_ADD(WD_OP_START, wdog, ticks);

      /* If the wdog is canceling, restarting the wdog is not allowed. */

#ifdef CONFIG_SCHED_TICKLESS
      /* We need to reassess timer if the watchdog
       * list head has changed.
       */

      if (WDOG_ISACTIVE(wdog))
        {
          if (wd_on_list(wdog))
            {
              reassess |= list_is_head(&g_wdactivelist, &wdog->node);
              list_delete(&wdog->node);
            }
          else if (wd_unlink_by_walk(wdog))
            {
              /* 链接不可信但**还在表上**：以前会把它再插一次 ⇒ 重复节点 ⇒
               * 下一次裁剪丢一串超时。按位置摘掉再插，计数留给锁外报。 */
              g_wd_stale_unlinks++;
            }
        }

      reassess |= wd_insert(wdog, ticks, wdentry, arg);
      if (g_wd_need_rescue)
        {
          wd_rescue_orphans();
        }

      /* If wd_start is called in the expiration callbacks,
       * the reassess proccess is disabled.
       */

      reassess &= !g_wdtimernested;

      leave_critical_section(flags);

      if (reassess)
        {
          /* Resume the interval timer that will generate the next
           * interval event. If the timer at the head of the list
           * changed, then this will pick that new delay.
           */

          nxsched_reassess_timer();
        }

      wd_report_clips();
#else
      UNUSED(reassess);

      /* Check if the watchdog has been started. If so, delete it. */

      WD_OP_ADD(WD_OP_START, wdog, ticks);

      if (WDOG_ISACTIVE(wdog))
        {
          if (wd_on_list(wdog))
            {
              list_delete(&wdog->node);
            }
          else if (wd_unlink_by_walk(wdog))
            {
              g_wd_stale_unlinks++;
            }
        }

      wd_insert(wdog, ticks, wdentry, arg);
      if (g_wd_need_rescue)
        {
          wd_rescue_orphans();
        }

      leave_critical_section(flags);

      wd_report_clips();
#endif
      sched_note_wdog(NOTE_WDOG_START, wdentry,
                      (FAR void *)(uintptr_t)ticks);
      ret = OK;
    }

  return ret;
}

/****************************************************************************
 * Name: wd_start
 *
 * Description:
 *   This function adds a watchdog timer to the active timer queue.  The
 *   specified watchdog function at 'wdentry' will be called from the
 *   interrupt level after the specified number of ticks has elapsed.
 *   Watchdog timers may be started from the interrupt level.
 *
 *   Watchdog timers execute in the address environment that was in effect
 *   when wd_start() is called.
 *
 *   Watchdog timers execute only once.
 *
 *   To replace either the timeout delay or the function to be executed,
 *   call wd_start again with the same wdog; only the most recent wdStart()
 *   on a given watchdog ID has any effect.
 *
 * Input Parameters:
 *   wdog     - Watchdog ID
 *   delay    - Delay count in clock ticks
 *   wdentry  - Function to call on timeout
 *   arg      - Parameter to pass to wdentry
 *
 *   NOTE:  The parameter must be of type wdparm_t.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is return to
 *   indicate the nature of any failure.
 *
 * Assumptions:
 *   The watchdog routine runs in the context of the timer interrupt handler
 *   and is subject to all ISR restrictions.
 *
 ****************************************************************************/

int wd_start(FAR struct wdog_s *wdog, clock_t delay,
             wdentry_t wdentry, wdparm_t arg)
{
  int ret = -EINVAL;

  /* Ensure delay is within the range the wdog can handle. */

  if (delay <= WDOG_MAX_DELAY)
    {
      ret = wd_start_abstick(wdog, clock_delay2abstick(delay), wdentry, arg);
    }

  return ret;
}

/****************************************************************************
 * Name: wd_timer
 *
 * Description:
 *   This function is called from the timer interrupt handler to determine
 *   if it is time to execute a watchdog function.  If so, the watchdog
 *   function will be executed in the context of the timer interrupt
 *   handler.
 *
 * Input Parameters:
 *   ticks - If CONFIG_SCHED_TICKLESS is defined then the number of ticks
 *     in the interval that just expired is provided.  Otherwise,
 *     this function is called on each timer interrupt and a value of one
 *     is implicit.
 *   noswitches - True: Can't do context switches now.
 *
 * Returned Value:
 *   If CONFIG_SCHED_TICKLESS is defined then the number of ticks for the
 *   next delay is provided (CLOCK_MAX if no delay). Otherwise, this function
 *   has no returned value.
 *
 * Assumptions:
 *   Called from interrupt handler logic with interrupts disabled.
 *
 ****************************************************************************/

#ifdef CONFIG_SCHED_TICKLESS
clock_t wd_timer(clock_t ticks, bool noswitches)
{
  FAR struct wdog_s *wdog;
  irqstate_t flags;
  clock_t    ret = CLOCK_MAX;

  /* Check if the watchdog at the head of the list is ready to run */

  if (!noswitches)
    {
      ret = wd_expiration(ticks);
    }
  else
    {
      flags = enter_critical_section();

      /* Return the delay for the next watchdog to expire */

      if (!list_is_empty(&g_wdactivelist))
        {
          /* Notice that if noswitches, expired - g_wdtickbase
           * may get negative value.
           */

          wdog = list_first_entry(&g_wdactivelist, struct wdog_s, node);
          ret  = !clock_compare(wdog->expired, ticks) ?
                 wdog->expired - ticks : 0u;
        }

      leave_critical_section(flags);
    }

  return ret;
}

#else
void wd_timer(clock_t ticks)
{
  /* Check if there are any active watchdogs to process */

  wd_expiration(ticks);
}
#endif /* CONFIG_SCHED_TICKLESS */

/****************************************************************************
 * Name: myvendor_wd_audit
 *
 * Description:
 *   看门狗活动链表的**只读巡检**（由 diag 线程每拍调用）。
 *
 *   为什么要它：clip 只在"有人 wd_start/wd_cancel"那一刻才被发现，而现场要的
 *   是**坏掉的那一拍**（好去它前面几行里找凶手）。2026-09-27 一整天才两次 clip，
 *   靠 clip 等不去。这里每秒只做一件事：**环完整性**（每个节点的 next/prev 必须
 *   互相指回；节点数有上限）。**只在"好→坏"或"坏→好"翻转时打一行**，正常情况
 *   一个字都不打。关中断、有界遍历、只读 ⇒ 不改变被测行为。
 *
 ****************************************************************************/

void myvendor_wd_audit(void)
{
  static bool           s_bad;
  irqstate_t            flags;
  FAR struct list_node *list = &g_wdactivelist;
  FAR struct list_node *node;
  FAR struct list_node *bad = NULL;
  unsigned              n = 0;

  flags = enter_critical_section();

  node = list->next;
  while (node != list && n < WD_WALK_MAX)
    {
      if (!wd_node_sane(node) || !wd_node_sane(node->next) ||
          !wd_node_sane(node->prev) || node->next->prev != node ||
          node->prev->next != node)
        {
          bad = node;
          break;
        }

      node = node->next;
      n++;
    }

  /* 走到上限还没回到表头：也算坏（成环 / 超长）。 */
  if (bad == NULL && node != list)
    {
      bad = node;
    }

  leave_critical_section(flags);

  if (bad != NULL && !s_bad)
    {
      s_bad = true;
      syslog(LOG_ERR, "wdog: audit broken (onset) node=%08lx n=%u\n",
             (unsigned long)bad, n);
    }
  else if (bad == NULL && s_bad)
    {
      s_bad = false;
      syslog(LOG_ERR, "wdog: audit ok again (n=%u)\n", n);
    }
}
