/**
 * @file sched_removereadytorun.c
 * @brief 板级抽换 `nuttx/sched/sched/sched_removereadytorun.c`。
 *
 * UP 路径要求就绪链头被摘时 flink 非空（至少还有 IDLE）。现场
 * gnss 在 poll→nxsem_tickwait→remove_self 时 flink==NULL，DEBUGASSERT
 * 后 abort→_exit 拆 IDLE 组 → task_delete(pthread) 二次 panic → 递归
 * assert 吃栈直到 SFBL。SMP 路径本来就直接切到 g_idletcb。这里对 UP
 * 做同样兜底：记 schedmon，把 pid0 IDLE 拼回链，继续切换。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* ---- 抽换件标记（2026-09-21）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : nuttx / sched/sched/sched_removereadytorun.c
 *   写入时 HEAD: 2ce740a0ac1052c5f51083a334ef3093f59ff780
 *   上游 blob  : 1c2271018abad3273874f5ebdbf1103826513af4
 *   为什么抽换 : UP 就绪链头 flink==NULL 时拼回 IDLE，记 schedmon，不 DEBUGASSERT
 *   版本漂移自查:
 *     git -C nuttx rev-parse HEAD:sched/sched/sched_removereadytorun.c
 *     git -C nuttx diff -- sched/sched/sched_removereadytorun.c
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/sched/sched_removereadytorun.c -- 上游 nuttx:sched/sched/sched_removereadytorun.c@1c2271018aba -- UP flink==NULL 拼回 IDLE，记 schedmon，不 DEBUGASSERT")
const char myvendor_override_marker_sched_removereadytorun_c[]
  __attribute__((used, section(".myvendor_marker"))) =
  "vela_override/sched/sched_removereadytorun.c -- 上游 nuttx:sched/sched/sched_removereadytorun.c@1c2271018aba -- UP flink==NULL 拼回 IDLE，记 schedmon，不 DEBUGASSERT";

#include <stdbool.h>
#include <assert.h>

#include <nuttx/sched_note.h>

#include "irq/irq.h"
#include "sched/queue.h"
#include "sched/sched.h"

#include "myvendor_schedmon.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifndef CONFIG_SMP
bool nxsched_remove_readytorun(FAR struct tcb_s *rtcb)
{
  FAR dq_queue_t *tasklist;
  bool doswitch = false;

  tasklist = TLIST_HEAD(rtcb);

  /* Check if the TCB to be removed is at the head of the ready to run list.
   * There is only one list, g_readytorun, and it always contains the
   * currently running task.  If we are removing the head of this list,
   * then we are removing the currently active task.
   */

  if (rtcb->blink == NULL && TLIST_ISRUNNABLE(rtcb->task_state))
    {
      /* There must always be at least one task in the list (the IDLE task)
       * after the TCB being removed.
       */

      FAR struct tcb_s *nxttcb = (FAR struct tcb_s *)rtcb->flink;

      if (nxttcb == NULL)
        {
          /* UP 不像 SMP：没有 g_idletcb 直切。pid0 就是 IDLE（SMP_NCPUS=1
           * 时 is_idle_task 也认 pid<1）。把 IDLE 拼到头后，dq_rem 后它
           * 自然成新头 —— 与"链本来就健康"时的布局一致。 */

          nxttcb = g_pidhash[0];
          myvendor_schedmon_ready_orphan(rtcb, nxttcb);
          if (nxttcb == NULL || nxttcb == rtcb || !is_idle_task(nxttcb))
            {
              /* 连 IDLE 都找不着：再 assert 也救不了，至少别静默空转。 */

              DEBUGASSERT(nxttcb != NULL && nxttcb != rtcb);
              return false;
            }

          /* IDLE 可能带着半残 blink/flink；覆盖成"头的唯一后继"。 */

          nxttcb->flink = NULL;
          nxttcb->blink = rtcb;
          rtcb->flink = nxttcb;
          tasklist->tail = (FAR dq_entry_t *)nxttcb;
        }

      nxttcb->task_state = TSTATE_TASK_RUNNING;
      up_update_task(nxttcb);
      doswitch = true;
    }

  /* Remove the TCB from the ready-to-run list.  In the non-SMP case, this
   * is always the g_readytorun list.
   */

  dq_rem((FAR dq_entry_t *)rtcb, tasklist);

  /* Since the TCB is not in any list, it is now invalid */

  rtcb->task_state = TSTATE_TASK_INVALID;

  return doswitch;
}

void nxsched_remove_self(FAR struct tcb_s *tcb)
{
  nxsched_remove_readytorun(tcb);
  if (list_pendingtasks()->head)
    {
      nxsched_merge_pending();
    }
}
#endif /* !CONFIG_SMP */

/****************************************************************************
 * Name: nxsched_remove_readytorun (SMP — 原样保留上游)
 ****************************************************************************/

#ifdef CONFIG_SMP
static void nxsched_remove_running(FAR struct tcb_s *tcb)
{
  FAR struct tcb_s *nxttcb;
  int cpu;

  DEBUGASSERT(tcb->cpu == this_cpu() &&
              tcb->task_state == TSTATE_TASK_RUNNING);

  cpu = tcb->cpu;
  nxttcb = &per_cpu_var_smp(g_idletcb, cpu);
  tcb->task_state = TSTATE_TASK_INVALID;
  nxttcb->task_state = TSTATE_TASK_RUNNING;
  per_cpu_var_smp(g_assignedtasks, cpu) = nxttcb;
  up_update_task(nxttcb);
  nxsched_switch_running(tcb->cpu, false);
}

void nxsched_remove_self(FAR struct tcb_s *tcb)
{
  nxsched_remove_running(tcb);
}

bool nxsched_remove_readytorun(FAR struct tcb_s *tcb)
{
  if (tcb->task_state == TSTATE_TASK_RUNNING)
    {
      nxsched_remove_running(tcb);
      return true;
    }
  else
    {
      FAR dq_queue_t *tasklist;

      tasklist = TLIST_HEAD(tcb, tcb->cpu);
      dq_rem((FAR dq_entry_t *)tcb, tasklist);
      tcb->task_state = TSTATE_TASK_INVALID;
    }

  return false;
}
#endif /* CONFIG_SMP */
