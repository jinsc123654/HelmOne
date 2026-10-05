/**
 * @file wd_cancel.c
 * @brief 板级抽换 `nuttx/sched/wdog/wd_cancel.c`：未入链的 wdog 不 list_delete。
 *
 * CMake 从 lib `sched` 去掉上游同名源。WDOG_ISACTIVE 只看 func，
 * next 已是 NULL 时上游仍 list_delete，会写低地址。
 *
 * 不修改 `nuttx/` 树。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/****************************************************************************
 * sched/wdog/wd_cancel.c
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

#include <nuttx/config.h>

/* ---- 抽换件标记（2026-09-18）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件 —— 2026-09-18 曾有个抽换件其实根本不在编译库里（已剔除），
 *    改了半天没生效，就靠这种标记一眼看出来；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉），不依赖任何编译选项（有些目标带 -w，会把 #warning 压掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : nuttx / sched/wdog/wd_cancel.c
 *   写入时 HEAD: 2ce740a0ac1052c5f51083a334ef3093f59ff780
 *   上游 blob  : 5056f617b1230cc81f1c0606d5299ddc8fdb35bd     （git -C nuttx rev-parse HEAD:sched/wdog/wd_cancel.c 应等于它）
 *   为什么抽换 : 未入链不 list_delete
 *   版本漂移自查:
 *     git -C nuttx rev-parse HEAD:sched/wdog/wd_cancel.c   # 与上面的 blob 比对
 *     git -C nuttx diff -- sched/wdog/wd_cancel.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/sched/wd_cancel.c -- 上游 nuttx:sched/wdog/wd_cancel.c@5056f617b123 -- 未入链不 list_delete")
const char myvendor_override_marker_sched_wd_cancel_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/sched/wd_cancel.c -- 上游 nuttx:sched/wdog/wd_cancel.c@5056f617b123 -- 未入链不 list_delete";

#include <stdbool.h>
#include <assert.h>
#include <errno.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/wdog.h>
#include <nuttx/sched_note.h>

#include "sched/sched.h"
#include "wdog/wdog.h"

/* Keep in sync with wd_start.c: PSRAM TCB waitdogs are valid. */

#define WD_NODE_SRAM_BASE     0x20000000ul
#define WD_NODE_SRAM_END      0x20080000ul
#define WD_NODE_PSRAM_H_BASE  0x10000000ul
#define WD_NODE_PSRAM_H_END   0x11000000ul
#define WD_NODE_PSRAM_S_BASE  0x60000000ul
#define WD_NODE_PSRAM_S_END   0x61000000ul

static inline bool wd_ptr_sane(uintptr_t a)
{
  if (a == 0 || (a & 3ul) != 0)
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
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wd_cancel
 *
 * Description:
 *   This function cancels a currently running watchdog timer. Watchdog
 *   timers may be canceled from the interrupt level.
 *
 * Input Parameters:
 *   wdog - ID of the watchdog to cancel.
 *
 * Returned Value:
 *   Zero (OK) is returned on success;  A negated errno value is returned to
 *   indicate the nature of any failure.
 *
 ****************************************************************************/

extern void myvendor_wd_op(uint8_t op, uintptr_t addr, uint32_t pc);
extern bool myvendor_wd_unlink_by_walk(FAR struct wdog_s *wdog);

/* 与 wd_start.c 的 enum wd_op_kind 对应（故意不引头文件：这里只要两个编号）。 */
#define WD_OP_CANCEL      6u
#define WD_OP_CANCEL_FIX  7u

int wd_cancel(FAR struct wdog_s *wdog)
{
  irqstate_t flags;
  int ret   = -EINVAL;
  bool head = false;

  if (wdog != NULL)
    {
      sched_note_wdog(NOTE_WDOG_CANCEL, (FAR void *)wdog->func,
                      (FAR void *)(uintptr_t)wdog->expired);
    }

  flags = enter_critical_section();

  /* Make sure that the watchdog is valid and still active. */

  if (wdog != NULL && WDOG_ISACTIVE(wdog))
    {
      /* list_delete needs next/prev.  func != NULL after a clip or
       * half-cancelled node must not load [NULL,#4].
       */

      myvendor_wd_op(WD_OP_CANCEL, (uintptr_t)wdog,
                     (uint32_t)(uintptr_t)__builtin_return_address(0));

      if (wdog->node.next != NULL && wdog->node.prev != NULL)
        {
          uintptr_t np = (uintptr_t)wdog->node.next;
          uintptr_t pp = (uintptr_t)wdog->node.prev;

          if (wd_ptr_sane(np) && wd_ptr_sane(pp) &&
              wdog->node.prev->next == &wdog->node &&
              wdog->node.next->prev == &wdog->node)
            {
              head = list_is_head(&g_wdactivelist, &wdog->node);
              list_delete(&wdog->node);
            }
          else if (myvendor_wd_unlink_by_walk(wdog))
            {
              /* **反向指针不吻合，但它其实还在表上。**
               *
               * 以前这里什么也不做、下面又无条件 `wdog->func = NULL`：节点留在
               * 链表里却被打上"不活跃"，下一次 wd_start 看见 !WDOG_ISACTIVE 就
               * 跳过"先删再插"，于是**同一个节点被挂进链表两次** —— 邻居不再
               * 指回来，下一次走链就判定"坏"并裁剪（2026-09-18 两次裁剪的坏
               * 节点都是这种反复 arm/cancel 的工作队列定时器）。
               * 现在改成：这里按位置把它摘掉，保证"取消 = 真的不在表上"。 */
              myvendor_wd_op(WD_OP_CANCEL_FIX, (uintptr_t)wdog,
                             (uint32_t)(uintptr_t)__builtin_return_address(0));
            }
        }
      else if (myvendor_wd_unlink_by_walk(wdog))
        {
          /* **链接不全（next/prev 有 NULL），但它其实还在表上。**
           *
           * 这是原来漏掉的一格：上面 `next != NULL && prev != NULL` 不成立时整块
           * 被跳过，接着下面无条件 `wdog->func = NULL` —— 节点**留在链表里却被标成
           * 不活跃**。下一次 `wd_start` 看到 !WDOG_ISACTIVE 就会跳过"先摘再插"，
           * `wd_insert` 把同一个 node 挂第二次 ⇒ 邻居不再指回来 ⇒ 下一次走链判坏
           * 并裁剪（现场 2026-09-19：坏节点 prev 与链表 prev 不一致、CLIP 紧跟
           * CANCEL、且**没有** CXFIX，正是走的这条被跳过的路）。
           *
           * 修法：这里也按位置摘掉，保证"取消 = 真的不在表上"。 */
          myvendor_wd_op(WD_OP_CANCEL_FIX, (uintptr_t)wdog,
                         (uint32_t)(uintptr_t)__builtin_return_address(0));
        }

      /* Mark the watchdog inactive */

      wdog->func = NULL;

      ret = OK;
    }

  leave_critical_section(flags);

  if (head)
    {
      /* If the watchdog is at the head of the timer queue, then
       * we will need to re-adjust the interval timer that will
       * generate the next interval event.
       */

      nxsched_reassess_timer();
    }

  return ret;
}
