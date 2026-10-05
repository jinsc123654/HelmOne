/**
 * @file myvendor_schedmon.c
 * @brief 调度监控：NULL waitobj 环形记录 + 任务抽样。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_schedmon.h"

#include <nuttx/clock.h>
#include <nuttx/arch.h>        /* up_check_tcbstack（CONFIG_STACK_COLORATION 下可用） */
#include <nuttx/irq.h>
#include <nuttx/wdog.h>

#include <arch/irq.h>

#include <string.h>
#include <syslog.h>

#define SCHEDMON_LOG_MS  15000u
#ifndef CONFIG_USEC_PER_TICK
#  define CONFIG_USEC_PER_TICK 10000
#endif
#define SCHEDMON_TICKS_15S \
  ((clock_t)((SCHEDMON_LOG_MS * 1000ul) / CONFIG_USEC_PER_TICK))

static struct myvendor_schedmon_snap_s g_snap;
static struct myvendor_schedmon_null_s g_null[MYVENDOR_SCHEDMON_NULL];
static volatile uint32_t g_null_n;
static uint8_t g_null_w;
static uint32_t g_logged_null;
static uint32_t g_logged_mutex;
static uint32_t g_logged_ready_orphan;
static clock_t g_log_ticks;

static void schedmon_copy_name(char *dst, FAR struct tcb_s *tcb)
{
  const char *n = (tcb != NULL) ? get_task_name(tcb) : "?";
  size_t i;

  for (i = 0; i + 1u < MYVENDOR_SCHEDMON_NAME && n[i] != '\0'; i++)
    {
      dst[i] = n[i];
    }

  dst[i] = '\0';
}

void myvendor_schedmon_waitirq_null(FAR struct tcb_s *wtcb, int errcode)
{
  FAR struct tcb_s *rtcb = running_task();
  struct myvendor_schedmon_null_s *e;
  uint32_t n;

  n = g_null_n + 1u;
  if (n == 0u)
    {
      n = 1u;
    }

  g_null_n = n;
  e = &g_null[g_null_w];
  g_null_w = (uint8_t)((g_null_w + 1u) % MYVENDOR_SCHEDMON_NULL);

  memset(e, 0, sizeof(*e));
  e->seq = n;
  e->ticks = (uint32_t)clock_systime_ticks();
  e->ipsr = getipsr();
  e->errcode = (int16_t)errcode;
  if (wtcb != NULL)
    {
      e->wt_pid = (int16_t)wtcb->pid;
      e->wt_pri = (int16_t)wtcb->sched_priority;
      e->wt_state = (uint8_t)wtcb->task_state;
      schedmon_copy_name(e->wt_name, wtcb);
    }

  if (rtcb != NULL)
    {
      e->run_pid = (int16_t)rtcb->pid;
      e->run_lock = rtcb->lockcount;
      e->run_state = (uint8_t)rtcb->task_state;
      schedmon_copy_name(e->run_name, rtcb);
    }

  memcpy(&g_snap.last_null, e, sizeof(*e));
  g_snap.null_n = n;
}

void myvendor_schedmon_waitirq_bad(FAR struct tcb_s *wtcb, int errcode,
                                   int32_t value, int mutex)
{
  FAR struct tcb_s *rtcb = running_task();
  struct myvendor_schedmon_null_s *e = &g_snap.last_sembad;
  uint32_t n = g_snap.sembad_n + 1u;

  g_snap.sembad_n = (n == 0u) ? 1u : n;

  memset(e, 0, sizeof(*e));
  e->seq = g_snap.sembad_n;
  e->ticks = (uint32_t)clock_systime_ticks();
  e->ipsr = getipsr();
  e->errcode = (int16_t)errcode;
  e->value = value;
  e->bad_mutex = (uint8_t)(mutex ? 1 : 0);
  if (wtcb != NULL)
    {
      e->wt_pid = (int16_t)wtcb->pid;
      e->wt_pri = (int16_t)wtcb->sched_priority;
      e->wt_state = (uint8_t)wtcb->task_state;
      schedmon_copy_name(e->wt_name, wtcb);
    }

  if (rtcb != NULL)
    {
      e->run_pid = (int16_t)rtcb->pid;
      e->run_lock = rtcb->lockcount;
      e->run_state = (uint8_t)rtcb->task_state;
      schedmon_copy_name(e->run_name, rtcb);
    }
}

void myvendor_schedmon_mutex_bad(int32_t holder, pid_t tid, pid_t run_pid,
                                 FAR const void *sem, FAR const void *caller)
{
  FAR struct tcb_s *rtcb = this_task();
  FAR struct tcb_s *gtcb = g_running_task;
  struct myvendor_schedmon_mutex_s *e = &g_snap.last_mutex;
  uint32_t n;

  n = g_snap.mutex_bad_n + 1u;
  if (n == 0u)
    {
      n = 1u;
    }

  g_snap.mutex_bad_n = n;
  memset(e, 0, sizeof(*e));
  e->seq = n;
  e->ticks = (uint32_t)clock_systime_ticks();
  e->holder = holder;
  e->tid = (int16_t)tid;
  e->run_pid = (int16_t)run_pid;
  e->sem = sem;
  e->caller = caller;
  if (rtcb != NULL)
    {
      e->this_st = (uint8_t)rtcb->task_state;
      schedmon_copy_name(e->this_name, rtcb);
    }

  if (gtcb != NULL)
    {
      e->run_st = (uint8_t)gtcb->task_state;
      schedmon_copy_name(e->run_name, gtcb);
    }
}

void myvendor_schedmon_ready_orphan(FAR struct tcb_s *rtcb,
                                    FAR struct tcb_s *idle)
{
  struct myvendor_schedmon_null_s *e = &g_snap.last_ready_orphan;
  uint32_t n;

  n = g_snap.ready_orphan_n + 1u;
  if (n == 0u)
    {
      n = 1u;
    }

  g_snap.ready_orphan_n = n;
  memset(e, 0, sizeof(*e));
  e->seq = n;
  e->ticks = (uint32_t)clock_systime_ticks();
  e->ipsr = (uint32_t)up_interrupt_context();
  if (rtcb != NULL)
    {
      e->wt_pid = (int16_t)rtcb->pid;
      e->wt_pri = (int16_t)rtcb->sched_priority;
      e->wt_state = (uint8_t)rtcb->task_state;
      e->errcode = rtcb->errcode;
      schedmon_copy_name(e->wt_name, rtcb);
    }

  if (idle != NULL)
    {
      e->run_pid = (int16_t)idle->pid;
      e->run_lock = idle->lockcount;
      e->run_state = (uint8_t)idle->task_state;
      schedmon_copy_name(e->run_name, idle);
    }
}

static void schedmon_one(FAR struct tcb_s *tcb, FAR void *arg)
{
  struct myvendor_schedmon_snap_s *s = arg;
  struct myvendor_schedmon_task_s *d;

  if (tcb == NULL || s == NULL || s->ntask >= MYVENDOR_SCHEDMON_TASKS)
    {
      return;
    }

  d = &s->task[s->ntask];
  s->ntask++;
  d->pid = (int16_t)tcb->pid;
  d->pri = (int16_t)tcb->sched_priority;
  d->lockcount = tcb->lockcount;
  d->errcode = tcb->errcode;
  d->state = (uint8_t)tcb->task_state;
  d->waitdog = WDOG_ISACTIVE(&tcb->waitdog) ? 1u : 0u;
  d->waitobj = (uintptr_t)tcb->waitobj;
#ifdef CONFIG_STACK_COLORATION
  /* 栈高水位：扫染色标记，返回已用字节。代价 O(已用/4) 次比较，对 19 个任务
   * 每 500 ms 一次可以忽略；而它能把"栈打穿"这一整类原因（同样表现为永久卡住、
   * 信号无效、无法回收）从卡死现场直接排除掉。 */
  d->stack_size = (uint16_t)tcb->adj_stack_size;
  d->stack_used = (uint16_t)up_check_tcbstack(tcb, tcb->adj_stack_size);
#else
  d->stack_size = 0;
  d->stack_used = 0;
#endif
  schedmon_copy_name(d->name, tcb);

  if (tcb->lockcount > s->lockmax)
    {
      s->lockmax = tcb->lockcount;
    }

  if (tcb->task_state == TSTATE_WAIT_SEM)
    {
      s->n_wait_sem++;
    }
}

uint32_t myvendor_schedmon_null_count(void)
{
  return g_null_n;
}

const struct myvendor_schedmon_snap_s *myvendor_schedmon_snap(void)
{
  return &g_snap;
}

bool myvendor_schedmon_snap_copy(struct myvendor_schedmon_snap_s *out)
{
  unsigned tries;

  if (out == NULL)
    {
      return false;
    }

  /* seqlock 读：写侧在 g_snap = tmp 期间把 gen 置为奇数，写完置为偶数。
   * 读 gen -> 拷贝 -> 再读 gen，两次相同才说明拷贝期间没被写打断。
   * 编译屏障防止编译器把 gen 的第二次读或结构体拷贝重排到屏障之外。
   * 不加锁、不开中断 —— 本函数要能在崩溃路径上跑。 */
  for (tries = 0; tries < MYVENDOR_SCHEDMON_SNAP_TRIES; tries++)
    {
      uint32_t g0 = g_snap.gen;

      if ((g0 & 1u) != 0u)
        {
          continue; /* 正在写 */
        }

      *out = g_snap;
      __asm__ volatile ("" ::: "memory");
      if (g_snap.gen == g0)
        {
          return true;
        }
    }

  return false;
}

void myvendor_schedmon_tick(void)
{
  struct myvendor_schedmon_snap_s tmp;
  irqstate_t flags;
  clock_t now = clock_systime_ticks();
  uint32_t null_n;
  uint32_t mutex_n;
  uint32_t ready_n;
  bool due;
  bool new_null;
  bool new_mutex;
  bool new_ready;

  memset(&tmp, 0, sizeof(tmp));
  tmp.ticks = (uint32_t)now;
  nxsched_foreach(schedmon_one, &tmp);

  flags = up_irq_save();
  tmp.gen = g_snap.gen + 1u;
  tmp.null_n = g_null_n;
  tmp.mutex_bad_n = g_snap.mutex_bad_n;
  tmp.sembad_n = g_snap.sembad_n;
  tmp.ready_orphan_n = g_snap.ready_orphan_n;
  tmp.last_null = g_snap.last_null;
  tmp.last_mutex = g_snap.last_mutex;
  tmp.last_sembad = g_snap.last_sembad;
  tmp.last_ready_orphan = g_snap.last_ready_orphan;
  g_snap = tmp;
  g_snap.gen = tmp.gen + 1u;
  null_n = g_null_n;
  mutex_n = tmp.mutex_bad_n;
  ready_n = tmp.ready_orphan_n;
  up_irq_restore(flags);

  new_null = (null_n != g_logged_null);
  new_mutex = (mutex_n != g_logged_mutex);
  new_ready = (ready_n != g_logged_ready_orphan);
  due = (g_log_ticks == 0) ||
        ((clock_t)(now - g_log_ticks) >= SCHEDMON_TICKS_15S);

  if (new_null)
    {
      const struct myvendor_schedmon_null_s *e = &g_snap.last_null;

      if (null_n <= 4u || (null_n & 0xfu) == 0u)
        {
          syslog(LOG_ERR,
                 "sched: semnull n=%u err=%d irq=%lu "
                 "wt pid=%d %s st=%u pri=%d "
                 "run pid=%d %s st=%u lock=%d\n",
                 (unsigned)e->seq, (int)e->errcode,
                 (unsigned long)e->ipsr,
                 (int)e->wt_pid, e->wt_name, (unsigned)e->wt_state,
                 (int)e->wt_pri,
                 (int)e->run_pid, e->run_name, (unsigned)e->run_state,
                 (int)e->run_lock);
        }

      g_logged_null = null_n;
    }

  if (new_ready)
    {
      const struct myvendor_schedmon_null_s *e = &g_snap.last_ready_orphan;

      syslog(LOG_ERR,
             "sched: ready-orphan n=%u "
             "head pid=%d %s st=%u pri=%d "
             "idle pid=%d %s st=%u\n",
             (unsigned)e->seq,
             (int)e->wt_pid, e->wt_name, (unsigned)e->wt_state,
             (int)e->wt_pri,
             (int)e->run_pid, e->run_name, (unsigned)e->run_state);
      g_logged_ready_orphan = ready_n;
    }
  if (new_mutex)
    {
      const struct myvendor_schedmon_mutex_s *m = &g_snap.last_mutex;

      if (mutex_n <= 4u || (mutex_n & 0xfu) == 0u)
        {
          syslog(LOG_ERR,
                 "sched: mutexbad n=%u holder=%ld tid=%d run=%d "
                 "this=%s st=%u run=%s st=%u\n",
                 (unsigned)m->seq, (long)m->holder,
                 (int)m->tid, (int)m->run_pid,
                 m->this_name, (unsigned)m->this_st,
                 m->run_name, (unsigned)m->run_st);
        }

      g_logged_mutex = mutex_n;
    }

  if (!due)
    {
      return;
    }

  g_log_ticks = now;
#ifdef CONFIG_MYVENDOR_SCHEDMON_LOG
  /* 稳态汇总：健康时每 15 s 一模一样，20 行左右（含下面逐任务清单），会把监控
   * 环冲掉。默认不参与构建；卡死现场的同一份数据在崩溃转储的 snap 段里，要在
   * 控制台看趋势时再打开它。 */
  syslog(LOG_DEBUG,
         "sched: mon tasks=%u lockmax=%d waitsem=%u semnull=%u mutexbad=%u sembad=%u\n",
         (unsigned)g_snap.ntask, (int)g_snap.lockmax,
         (unsigned)g_snap.n_wait_sem, (unsigned)null_n,
         (unsigned)mutex_n, (unsigned)g_snap.sembad_n);
#endif

  /* 诊断：waitobj 非空但计数/holder 不变量不成立。上游这里是 DEBUGASSERT，
   * 而 IRQ 上下文 assert 会递归、连核心转储都写不上（现场直接重启过），
   * 所以改成记录 + 继续。这条详情行给出当时的值和双方任务。 */
  if (g_snap.sembad_n != 0)
    {
      const struct myvendor_schedmon_null_s *b = &g_snap.last_sembad;

      syslog(LOG_ERR,
             "sched: sembad n=%u irq=%lu bad_mutex=%u value=%ld err=%d wt pid=%d %s st=%u pri=%d run pid=%d %s st=%u lock=%d\n",
             (unsigned)b->seq, (unsigned long)b->ipsr,
             (unsigned)b->bad_mutex, (long)b->value, (int)b->errcode,
             (int)b->wt_pid, b->wt_name, (unsigned)b->wt_state,
             (int)b->wt_pri,
             (int)b->run_pid, b->run_name, (unsigned)b->run_state,
             (int)b->run_lock);
    }

  /* 诊断：把最近一次 non-owner mutex post 的现场并进 15 s 汇总行。
   * 详情行只在计数变化时才打，过滤过的日志里经常看不到；非持有者解锁
   * 是未定义行为（可能留下卡死的 blocking 位），必须能直接定位到线程。 */
  if (mutex_n != 0)
    {
      const struct myvendor_schedmon_mutex_s *m = &g_snap.last_mutex;

      syslog(LOG_ERR,
             "sched: mutexbad last n=%u holder=%ld tid=%d run=%d "
             "this=%s st=%u runner=%s st=%u sem=%p call=%p\n",
             (unsigned)m->seq, (long)m->holder, (int)m->tid, (int)m->run_pid,
             m->this_name, (unsigned)m->this_st,
             m->run_name, (unsigned)m->run_st,
             m->sem, m->caller);
    }

#ifdef CONFIG_MYVENDOR_SCHEDMON_LOG
  for (unsigned i = 0; i < g_snap.ntask; i++)
    {
      const struct myvendor_schedmon_task_s *t = &g_snap.task[i];
      /* 只跳过"正常运行且没拿锁"的任务；**其余一律打印**。
       *
       * 这里原来是反过来的：`lockcount<=0 && state != TASK_WAIT_SEM &&
       * waitobj==0 && waitdog==0` 就 `continue`。那条过滤恰好把现场最要命的
       * 一类状态藏了起来 —— 一个"没有等任何东西、也没有看门狗在跑，却又不
       * 运行"的线程（GNSS 读线程卡死时就是它：日志里 tasks=19 只打 16 行，
       * 而 waitsem=10 与 10 条 st=5 完全吻合，说明卡住的线程不在其中）。
       *
       * 卡死于"某次 poll 里的等待"时 waitdog 必为 1；而现场是 waitdog==0、
       * waitobj==0 —— 那要么是 TCB 根本没跑起来（PENDING/INACTIVE，
       * 于是 phase 字符串是上一代的陈旧值、pid 也不会被写），要么是卡在
       * 驱动层自旋（RUNNING 却不在等信号量）。这两种只需 state 一个字段就能
       * 分开，所以必须把它打出来。 */
      if ((t->state == TSTATE_TASK_READYTORUN ||
           t->state == TSTATE_TASK_RUNNING) && t->lockcount <= 0)
        {
          continue;
        }

      syslog(LOG_DEBUG,
             "sched: task pid=%d %s pri=%d st=%u err=%d lock=%d wait=%p wd=%u stk=%u/%u\n",
             (int)t->pid, t->name, (int)t->pri, (unsigned)t->state,
             (int)t->errcode, (int)t->lockcount, (void *)t->waitobj,
             (unsigned)t->waitdog, (unsigned)t->stack_used,
             (unsigned)t->stack_size);
    }
#endif /* CONFIG_MYVENDOR_SCHEDMON_LOG */
}
