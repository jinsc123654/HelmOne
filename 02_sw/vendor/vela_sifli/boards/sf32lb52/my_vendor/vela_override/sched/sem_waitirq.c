/**
 * @file sem_waitirq.c
 * @brief 板级抽换 `nuttx/sched/semaphore/sem_waitirq.c`。
 *
 * 超时/信号可能晚于 sem_post：task_state 仍是 WAIT_SEM，waitobj 已空。
 * 上游 DEBUGASSERT 在 SysTick 里会递归 assert，核心转储写不上。
 * 记到 schedmon SRAM 后直接返回。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/****************************************************************************
 * sched/semaphore/sem_waitirq.c
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
 *   替的是上游 : nuttx / sched/semaphore/sem_waitirq.c
 *   写入时 HEAD: 2ce740a0ac1052c5f51083a334ef3093f59ff780
 *   上游 blob  : 70552158c26c9114a7e994a700be59ade9dfc87b     （git -C nuttx rev-parse HEAD:sched/semaphore/sem_waitirq.c 应等于它）
 *   为什么抽换 : waitobj==NULL 记 schedmon，不 DEBUGASSERT
 *   版本漂移自查:
 *     git -C nuttx rev-parse HEAD:sched/semaphore/sem_waitirq.c   # 与上面的 blob 比对
 *     git -C nuttx diff -- sched/semaphore/sem_waitirq.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/sched/sem_waitirq.c -- 上游 nuttx:sched/semaphore/sem_waitirq.c@70552158c26c -- waitobj==NULL 记 schedmon，不 DEBUGASSERT")
const char myvendor_override_marker_sched_sem_waitirq_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/sched/sem_waitirq.c -- 上游 nuttx:sched/semaphore/sem_waitirq.c@70552158c26c -- waitobj==NULL 记 schedmon，不 DEBUGASSERT";

#include <sched.h>
#include <assert.h>
#include <errno.h>

#include <nuttx/addrenv.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include "sched/sched.h"
#include "semaphore/semaphore.h"

#include "myvendor_schedmon.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline_function
void restore_context(FAR struct tcb_s *rtcb, FAR struct tcb_s *wtcb,
                     FAR sem_t *sem, int errcode, bool mutex)
{
  if (!mutex)
    {
      atomic_add(NXSEM_COUNT(sem), 1);
    }
  else if (dq_empty(SEM_WAITLIST(sem)))
    {
      atomic_and(NXSEM_MHOLDER(sem), ~NXSEM_MBLOCKING_BIT);
    }

  wtcb->waitobj = NULL;
  wtcb->errcode = (int16_t)errcode;

  if (nxsched_add_readytorun(wtcb))
    {
      nxsched_switch(wtcb, rtcb);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void nxsem_wait_irq(FAR struct tcb_s *wtcb, int errcode)
{
  FAR struct tcb_s *rtcb = this_task();
  FAR sem_t *sem = wtcb->waitobj;
  bool mutex;

#ifdef CONFIG_ARCH_ADDRENV
  FAR struct addrenv_s *oldenv;

  if (wtcb->group->tg_addrenv_own)
    {
      addrenv_select(wtcb->group->tg_addrenv_own, &oldenv);
    }
#endif

  /* 超时与 post 对撞：waiter 已醒，waitobj 已空。不要 DEBUGASSERT。 */

  if (sem == NULL)
    {
      myvendor_schedmon_waitirq_null(wtcb, errcode);
#ifdef CONFIG_ARCH_ADDRENV
      if (wtcb->group != NULL && wtcb->group->tg_addrenv_own)
        {
          addrenv_restore(oldenv);
        }
#endif
      return;
    }

  mutex = NXSEM_IS_MUTEX(sem);

  /* 上游在这里的两条 DEBUGASSERT：
   *
   *   DEBUGASSERT(mutex || atomic_read(NXSEM_COUNT(sem)) < 0);
   *   DEBUGASSERT(!mutex || NXSEM_MBLOCKING(atomic_read(NXSEM_MHOLDER(sem))));
   *
   * 与上面 waitobj==NULL 属同一类竞态：超时/信号晚于 sem_post，task 还挂在
   * 等待链上，计数或 holder 却已被另一方改过。
   *
   * 现场已经因为第一条吃过一次重启：
   *   [coredump] assert .../vela_override/sched/sem_waitirq.c:114 irq=1
   * 而本文件存在的全部理由就是"IRQ 上下文 assert 会递归，核心转储写不上"，
   * 所以这两条也必须和 NULL 那条一样：只记录，不 panic，继续正常唤醒路径。
   *
   * 代价：不变量不成立时继续走 restore_context()，非 mutex 分支会给计数 +1，
   * 可能多出一个 token（对信号型信号量表现为一次多余唤醒，waiter 会重新判断
   * 条件）。相比 panic + 重启 + 丢现场，这个代价可接受；要彻底消除得先定位是
   * 谁在 timeout 路径上没摘等待链，那是另一件事。
   */

  if (!mutex)
    {
      int32_t cnt = atomic_read(NXSEM_COUNT(sem));

      if (cnt >= 0)
        {
          myvendor_schedmon_waitirq_bad(wtcb, errcode, cnt, 0);
        }
    }
  else
    {
      int32_t mh = atomic_read(NXSEM_MHOLDER(sem));

      if (!NXSEM_MBLOCKING(mh))
        {
          myvendor_schedmon_waitirq_bad(wtcb, errcode, mh, 1);
        }
    }

  /* **信号打断了 mutex 等待：对"不可中断"的等待者必须照样唤醒它。**
   *
   * 上游的规则是"信号不能偷走 mutex" —— waiter 留在等待链上，等持有者 unlock
   * 时再正常转交（因为 mutex 有归属，凭空摘掉会让计数/holder 记账错乱）。
   * 但 `nxmutex_lock()`（`nxsem_wait_uninterruptible`）类等待者是**不该被信号
   * 打断**的：它醒来后会**再次**调用 `nxsem_wait_slow()`，而那里第一句就是
   *   `DEBUGASSERT(rtcb->waitobj == NULL)`  （nuttx/sched/semaphore/sem_wait.c:185）
   * 于是"waitobj 还留着"直接断言 → abort → `_exit` → group_kill_children →
   * `nxtask_delete(pthread)` 的 DEBUGPANIC（task_delete.c:103）→ 写 coredump 时
   * HardFault → **整机重启**。
   *
   * 现场（2026-09-18，开机 4532 s）：读线程在跨 `nxsem_tickwait` 的路上断言，
   * 栈就是 `gnss_read_some → nxsem_tickwait → nxsem_wait_slow:185`，而触发它的
   * 信号来自我们自己的 `gnss_reader_wake()`（SIGUSR1）✗。
   *
   * 所以按本文件既有的取舍（见上面注释："相比 panic + 重启 + 丢现场，代价可接受"）：
   * **对 BLOCKING 类等待者（`NXSEM_MBLOCKING`）也走完整唤醒** —— 清 waitobj、
   * 摘链、入就绪链；它被唤醒后由调用方（nxmutex_lock 的重试循环）重新判断条件，
   * 语义上等价于"信号被忽略"。可中断的 mutex 等待者仍按上游留在链上。 */
  if (mutex && (errcode == EINTR || errcode == ECANCELED) &&
      !NXSEM_MBLOCKING(atomic_read(NXSEM_MHOLDER(sem))))
    {
#ifdef CONFIG_ARCH_ADDRENV
      if (wtcb->group->tg_addrenv_own)
        {
          addrenv_restore(oldenv);
        }
#endif
    }
  else
    {
      nxsem_canceled(wtcb, sem);
      dq_rem((FAR dq_entry_t *)wtcb, SEM_WAITLIST(sem));

#ifdef CONFIG_ARCH_ADDRENV
      if (wtcb->group->tg_addrenv_own)
        {
          addrenv_restore(oldenv);
        }
#endif

      restore_context(rtcb, wtcb, sem, errcode, mutex);
    }
}
