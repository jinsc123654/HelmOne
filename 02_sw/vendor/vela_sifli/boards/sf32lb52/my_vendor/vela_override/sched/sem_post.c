/**
 * @file sem_post.c
 * @brief 板级抽换 `nuttx/sched/semaphore/sem_post.c`。
 *
 * 上游 mutex post 要求 holder == nxsched_gettid()。gettid 在
 * task_state != RUNNING 时返回 -ESRCH；临界区里 ready 头已换成更高
 * 优先级线程时 this_task() 也不是真正在跑的 GNSS。DEBUGASSERT 会
 * abort 掉 GNSS pthread，_exit 再拆 IDLE 组，第二次 panic 在
 * task_delete.c。记 schedmon 后仍释放，避免整机被一条 UART unlock 拉垮。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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
 *   替的是上游 : nuttx / sched/semaphore/sem_post.c
 *   写入时 HEAD: 2ce740a0ac1052c5f51083a334ef3093f59ff780
 *   上游 blob  : 491012b9b144b1742637e8ea8282ef34688a60d6     （git -C nuttx rev-parse HEAD:sched/semaphore/sem_post.c 应等于它）
 *   为什么抽换 : mutex holder≠tid 记 schedmon，不 DEBUGASSERT
 *   版本漂移自查:
 *     git -C nuttx rev-parse HEAD:sched/semaphore/sem_post.c   # 与上面的 blob 比对
 *     git -C nuttx diff -- sched/semaphore/sem_post.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/sched/sem_post.c -- 上游 nuttx:sched/semaphore/sem_post.c@491012b9b144 -- mutex holder≠tid 记 schedmon，不 DEBUGASSERT")
const char myvendor_override_marker_sched_sem_post_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/sched/sem_post.c -- 上游 nuttx:sched/semaphore/sem_post.c@491012b9b144 -- mutex holder≠tid 记 schedmon，不 DEBUGASSERT";

#include <limits.h>
#include <errno.h>
#include <sched.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include "sched/sched.h"
#include "semaphore/semaphore.h"

#include "myvendor_schedmon.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline_function pid_t mutex_post_tid(void)
{
  pid_t tid = nxsched_gettid();
  FAR struct tcb_s *tcb;

  if (tid >= 0)
    {
      return tid;
    }

  tcb = this_task();
  if (tcb != NULL)
    {
      return tcb->pid;
    }

  tcb = g_running_task;
  if (tcb != NULL)
    {
      return tcb->pid;
    }

  return tid;
}

static inline_function
int get_blocking(FAR sem_t *sem, bool mutex, bool *blocking)
{
  int32_t mholder = NXSEM_NO_MHOLDER;
  int ret = OK;

  if (mutex)
    {
      pid_t tid;
      pid_t run_pid;
      int32_t holder;

      /* Mutex post from interrupt context is not allowed */

      DEBUGASSERT(!up_interrupt_context());

      /* Lock the mutex for us by setting the blocking bit */

      mholder = atomic_or(NXSEM_MHOLDER(sem), NXSEM_MBLOCKING_BIT);
      holder = mholder & (~NXSEM_MBLOCKING_BIT);
      tid = mutex_post_tid();
      run_pid = (g_running_task != NULL) ? g_running_task->pid : tid;

      /* 上游 DEBUGASSERT holder==tid。gettid 不可靠时用 running_task 再比一次。 */

      if (mholder != (NXSEM_MBLOCKING_BIT | NXSEM_NO_MHOLDER) &&
          holder != (int32_t)tid &&
          holder != (int32_t)run_pid)
        {
          /* 连**是哪把锁、从哪一行**一起记下来。
           *
           * 只有 holder/tid/线程名的话，这份记录只能说明"某个线程释放了一个它
           * 不持有的互斥量"，无法定位 —— 而这类 UB 的后果（blocking 位残留 →
           * 之后任何等它的线程永久阻塞）必须知道是哪把锁才能修。
           * `sem` 让同一把锁的多次误用归并到一起；`caller` 用
           * `arm-none-eabi-addr2line -e nuttx <addr>` 就能标出调用点。 */
          myvendor_schedmon_mutex_bad(mholder, tid, run_pid, sem,
                                      __builtin_return_address(0));

          /* **顺手把已经写坏的 holder 修回来。**
           *
           * 现场两类非法 holder（都不是合法 pid）：
           *   1) `NXSEM_NO_MHOLDER - N`（≥ 0x7FFFFFFD）—— 对互斥量做了
           *      **计数语义的减法**；
           *   2) `holder < 0`，尤其是 **-3（-ESRCH）** ——
           *      `nxmutex_wait` 快路径用 `_SCHED_GETTID()` 写入 holder，而
           *      `nxsched_gettid()` 在 `task_state != RUNNING` 时返回 -ESRCH
           *      （见 nuttx/include/nuttx/sched.h）。之后真正的 unlock
           *      （tid=正 pid）对不上 → mutexbad → blocking 位残留。
           *
           * 为什么必须修：下面那句 `if (!*blocking) atomic_set(NO_MHOLDER)`
           * 只在**没有** blocking 位时才清；而现场 blocking 位是立着的 →
           * 残留 → **之后任何等这把锁的线程会永久阻塞**（WDT 现场
           * companion 卡在 `sf32lb_sd_bread`/`nxmutex_wait`）。
           *
           * 判据故意收得很窄：非法编码才动手，绝不误清一个真持有者。 */
          if (holder > 32767 || holder < 0)
            {
              /* 清掉坏掉的 holder。下面仍按"有人在等"走唤醒路径，
               * 否则 blocking 位曾立过、等待者会永久卡在 nxmutex_wait。 */
              atomic_set(NXSEM_MHOLDER(sem),
                         NXSEM_NO_MHOLDER | NXSEM_MBLOCKING_BIT);
              mholder = NXSEM_NO_MHOLDER | NXSEM_MBLOCKING_BIT;
            }
        }

      *blocking = NXSEM_MBLOCKING(mholder);

      if (!(*blocking))
        {
          atomic_set(NXSEM_MHOLDER(sem), NXSEM_NO_MHOLDER);
        }
    }
  else
    {
      int32_t sem_count;

      /* Check the maximum allowable value */

      sem_count = atomic_read(NXSEM_COUNT(sem));
      do
        {
          if (sem_count >= SEM_VALUE_MAX)
            {
              ret = -EOVERFLOW;
              break;
            }
        }
      while (!atomic_try_cmpxchg_release(NXSEM_COUNT(sem), &sem_count,
                                         sem_count + 1));
      *blocking = sem_count < 0;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int nxsem_post_slow(FAR sem_t *sem)
{
  FAR struct tcb_s *stcb = NULL;
  irqstate_t flags;
#if defined(CONFIG_PRIORITY_INHERITANCE) || defined(CONFIG_PRIORITY_PROTECT)
  uint8_t proto;
#endif
  bool blocking = false;
  bool mutex = NXSEM_IS_MUTEX(sem);
  int ret = OK;

  flags = enter_critical_section();

  ret = get_blocking(sem, mutex, &blocking);

  if (ret == OK)
    {
      if (!mutex || blocking)
        {
          nxsem_release_holder(sem);
        }

#if defined(CONFIG_PRIORITY_INHERITANCE) || defined(CONFIG_PRIORITY_PROTECT)
      proto = sem->flags & SEM_PRIO_MASK;
      if (proto != SEM_PRIO_NONE)
        {
          sched_lock();
        }
#endif

      if (blocking)
        {
          stcb = (FAR struct tcb_s *)dq_remfirst(SEM_WAITLIST(sem));
          if (stcb != NULL)
            {
              FAR struct tcb_s *rtcb = this_task();

              if (mutex)
                {
                  int32_t blocking_bit = dq_empty(SEM_WAITLIST(sem)) ?
                    0 : NXSEM_MBLOCKING_BIT;
                  atomic_set(NXSEM_MHOLDER(sem), stcb->pid | blocking_bit);
                }
              else
                {
                  nxsem_add_holder_tcb(stcb, sem);
                }

              wd_cancel(&stcb->waitdog);
              stcb->waitobj = NULL;

              if (nxsched_add_readytorun(stcb))
                {
                  nxsched_switch(this_task(), rtcb);
                }
            }
        }

#if defined(CONFIG_PRIORITY_INHERITANCE) || defined(CONFIG_PRIORITY_PROTECT)
      if (proto != SEM_PRIO_NONE)
        {
          if (proto == SEM_PRIO_INHERIT)
            {
#  ifdef CONFIG_PRIORITY_INHERITANCE
              nxsem_restore_baseprio(stcb, sem);
#  endif
            }
          else if (proto == SEM_PRIO_PROTECT)
            {
#  ifdef CONFIG_PRIORITY_PROTECT
              nxsem_protect_post(sem);
#  endif
            }

          sched_unlock();
        }
#endif
    }

  leave_critical_section(flags);

  return ret;
}
