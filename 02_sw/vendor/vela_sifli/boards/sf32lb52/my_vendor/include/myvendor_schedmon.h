/**
 * @file myvendor_schedmon.h
 * @brief 调度监控：IRQ 里只记 SRAM，diag 线程再 syslog；崩溃 note 直接抄。
 *
 * sem_waitirq 超时可能撞上已唤醒的 waiter（waitobj 已空）。上游
 * DEBUGASSERT 会在 SysTick 里再走 dump_assert_info，容易递归复位、
 * 核心转储写不上盘。这里记下现场并返回。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_SCHEDMON_H
#define MYVENDOR_SCHEDMON_H

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include <nuttx/sched.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYVENDOR_SCHEDMON_NAME  12
#define MYVENDOR_SCHEDMON_TASKS 24
#define MYVENDOR_SCHEDMON_NULL  4

struct myvendor_schedmon_task_s
{
  int16_t pid;
  int16_t pri;
  int16_t lockcount;
  /** @brief TCB 的 errcode：等待被打断/超时时由内核写入，能区分"等到了"和"被叫醒"。
   *         与 state 一起看，是判断"线程卡在哪"最直接的两个字段。 */
  int16_t errcode;
  /** @brief 栈高水位（字节，由栈染色扫描得到）与栈总大小。
   *         栈打穿同样会表现成"永久卡住 + 信号无效 + 无法回收"，
   *         所以卡死现场必须能一眼排除它。需要 CONFIG_STACK_COLORATION。 */
  uint16_t stack_used;
  uint16_t stack_size;
  uint8_t state;
  uint8_t waitdog;
  uintptr_t waitobj;
  char name[MYVENDOR_SCHEDMON_NAME];
};

struct myvendor_schedmon_null_s
{
  uint32_t seq;
  uint32_t ticks;
  uint32_t ipsr;
  int16_t wt_pid;
  int16_t run_pid;
  int16_t wt_pri;
  int16_t run_lock;
  int16_t errcode;
  uint8_t wt_state;
  uint8_t run_state;
  char wt_name[MYVENDOR_SCHEDMON_NAME];
  char run_name[MYVENDOR_SCHEDMON_NAME];
  /** @brief waitirq_bad 用：NXSEM_COUNT / NXSEM_MHOLDER 的不变量原值。 */
  int32_t value;
  /** @brief waitirq_bad 用：非 0 表示走的是 mutex 不变量。 */
  uint8_t bad_mutex;
  uint8_t pad_[3];
};

struct myvendor_schedmon_mutex_s
{
  uint32_t seq;
  uint32_t ticks;
  int32_t holder;
  int16_t tid;
  int16_t run_pid;
  uint8_t this_st;
  uint8_t run_st;
  /** @brief 出问题的**互斥量地址**。
   *
   *  没有它这份记录只能说明"读线程释放了一个它不持有的互斥量"，无法点名是
   *  哪一个 —— 而这类 UB 的后果（blocking 位残留 → 之后任何等它的线程永久
   *  阻塞）必须知道是谁才能修。同一个地址再次出现即可认定为同一把锁。 */
  const void *sem;
  /** @brief 调用点地址（`__builtin_return_address(0)`）。
   *
   *  用来区分"哪个调用路径在把互斥量当信号量 post"—— 同一个互斥量被不同
   *  路径误用，修法完全不同。符号化：`arm-none-eabi-addr2line -e nuttx <addr>`。 */
  const void *caller;
  char this_name[MYVENDOR_SCHEDMON_NAME];
  char run_name[MYVENDOR_SCHEDMON_NAME];
};

struct myvendor_schedmon_snap_s
{
  uint32_t gen;
  uint32_t ticks;
  uint16_t ntask;
  uint16_t n_wait_sem;
  int16_t lockmax;
  uint32_t null_n;
  uint32_t mutex_bad_n;
  /** @brief waitobj 非空但不变量不成立的累计次数（与 null_n 分开计）。 */
  uint32_t sembad_n;
  /** @brief 就绪链头的 flink 为空（IDLE 丢了）：remove_readytorun 自愈次数。 */
  uint32_t ready_orphan_n;
  struct myvendor_schedmon_task_s task[MYVENDOR_SCHEDMON_TASKS];
  struct myvendor_schedmon_null_s last_null;
  struct myvendor_schedmon_mutex_s last_mutex;
  struct myvendor_schedmon_null_s last_sembad;
  /** @brief 最近一次就绪链自愈：wt=被摘的头，run=拼回去的 IDLE。 */
  struct myvendor_schedmon_null_s last_ready_orphan;
};

/**
 * @brief SysTick / 临界区：waitobj 已空。禁止 syslog。
 */
void myvendor_schedmon_waitirq_null(FAR struct tcb_s *wtcb, int errcode);

/**
 * @brief waitobj 非空、但计数/holder 不变量不成立（超时与 post 对撞）。
 *
 * @details
 * `sem_waitirq.c` 里上游有两条 DEBUGASSERT：
 *   - `mutex || NXSEM_COUNT(sem) < 0`      —— 计数型信号量应有等待者却非负
 *   - `!mutex || NXSEM_MBLOCKING(mholder)` —— mutex 未置 blocking 位
 * 与 `waitobj == NULL` 属同一类竞态。那条已经改成"记 schedmon 后返回"，
 * 这两条也必须同样处理：**在 SysTick/IRQ 上下文 assert 会递归，连核心转储
 * 都写不上**（现场就出现过 `assert sem_waitirq.c:114 irq=1` 直接重启）。
 *
 * 只记录，不做仲裁；调用方继续走正常唤醒路径。
 *
 * @param wtcb 被唤醒的等待者。
 * @param errcode 唤醒原因。
 * @param value 不变量原值（NXSEM_COUNT 或 NXSEM_MHOLDER）。
 * @param mutex 非 0 表示 mutex 那条不变量不成立。
 */
void myvendor_schedmon_waitirq_bad(FAR struct tcb_s *wtcb, int errcode,
                                   int32_t value, int mutex);

/**
 * @brief mutex post 时 holder 对不上当前 tid。临界区，禁止 syslog。
 *
 * @param holder  互斥量的 NXSEM_MHOLDER 原值（blocking 位未剥离）。
 * @param tid     调用方的 tid。
 * @param run_pid 记录时刻的运行线程 pid（tid 判据不可靠时的第二判据）。
 * @param sem     **出问题的互斥量地址**（可为 NULL）。没有它就无法点名是哪把锁。
 * @param caller  调用点地址（`__builtin_return_address(0)`），用来定位是哪条路径误用。
 */
void myvendor_schedmon_mutex_bad(int32_t holder, pid_t tid, pid_t run_pid,
                                 FAR const void *sem, FAR const void *caller);

/**
 * @brief 就绪链头 flink==NULL（本应至少还有 IDLE）。临界区，禁止 syslog。
 *
 * @param rtcb 正被摘下的就绪链头（通常是即将阻塞的当前任务）。
 * @param idle 找到的 IDLE TCB（可为 NULL，表示连 pid0 都没有）。
 */
void myvendor_schedmon_ready_orphan(FAR struct tcb_s *rtcb,
                                    FAR struct tcb_s *idle);

/** @brief diag 500 ms：抽样 TCB，有新 semnull 或到点则打日志。 */
void myvendor_schedmon_tick(void);

uint32_t myvendor_schedmon_null_count(void);

const struct myvendor_schedmon_snap_s *myvendor_schedmon_snap(void);

/** seqlock 读的一致快照重试次数。 */
#define MYVENDOR_SCHEDMON_SNAP_TRIES  8u

/**
 * @brief seqlock 方式拷出一份一致快照。
 *
 * @details
 * 写侧（myvendor_schedmon_tick）本来就按 seqlock 协议：写期间 gen 为奇数、
 * 写完为偶数。但原来没有任何读者校验 gen —— 崩溃 note 里可能是两个 tick
 * 混起来的数据。本函数读 gen、拷贝、再读 gen，一致才返回 true，否则重试。
 *
 * 供崩溃路径使用：只用编译屏障，不加锁、不开中断。失败时 @p out 内容不可信，
 * 调用者应先清零再使用。
 *
 * @param out 输出快照。
 * @return true 表示拷到一致快照；false 表示重试次数内始终在写。
 */
bool myvendor_schedmon_snap_copy(struct myvendor_schedmon_snap_s *out);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_SCHEDMON_H */
