/**
 * @file myvendor_diag.h
 * @brief 外设诊断线程：注册 start/stop/restart/ok，独立于业务线程做监管。
 *
 * HCI 卡住时 companion 可能堵在同步命令上，监督逻辑永远跑不到。
 * 心跳超时（须长于 zblue 10 s HCI 超时）表示主循环卡死，不是进程已死：
 * 先置 skip_sync 让 HCI 轮询退出，再 SIGTERM / 复位 LCPU / respawn。
 * 不要在线程已卡死时只 force_reset 又清 skip_sync：k_sem_take 不可中断，
 * task_delete 会 deferred，随后 skip respawn，BLE 再也起不来。
 * 本线程不走 HPWORK / ble_companion，只做判定和重启。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_DIAG_H
#define MYVENDOR_DIAG_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一套外设监管方法。未实现的指针可为 NULL。
 */
typedef struct myvendor_diag_ops_s
{
  const char *name;          /**< 日志名，例如 "ble" / "gnss"。 */
  bool (*ok)(void);           /**< true=正常。 */
  int  (*restart)(void);      /**< 异常后的恢复（可含断电）。 */
  int  (*start)(void);        /**< 可选。 */
  int  (*stop)(void);         /**< 可选。 */
  uint32_t fail_ms;          /**< 连续不健康多久才 restart。 */
  uint32_t cooldown_ms;      /**< 两次 restart 最短间隔。 */
} myvendor_diag_ops_t;

/**
 * @brief 注册一套监管函数。开机后、线程跑起来前后均可。
 * @return 0 成功；-EINVAL / -ENOMEM。
 */
int myvendor_diag_register(const myvendor_diag_ops_t *ops);

/**
 * @brief 启动诊断线程（可重复调用）。
 */
int myvendor_diag_start(void);

/* ---------------------------------------------------------------------------
 * 诊断时基（watch）：把"我这个线程还在动"的依据交给 diag 线程盯着
 *
 * **为什么由 diag 提供时基**：它是唯一一条只做判定、不跑业务循环的线程
 * （`DIAG_TICK_MS` 500 ms 一个 tick），其时基不挂在任何业务线程的等待路径上。
 *
 * **为什么需要它**：本 fork 里"定时等一会儿"这类原语（`poll()` 的超时、
 * `usleep`、`clock_nanosleep`、`nxsem_tickwait`…）都把超时挂在**调用线程 TCB
 * 的 waitdog** 上（`nuttx/sched/semaphore/sem_tickwait.c:87`
 * `wd_start(&rtcb->waitdog, …)`）。而 `g_wdactivelist` 会断链，板级抽换件
 * `vela_override/sched/wd_start.c` 的策略是"断链不崩、剪环保命" —— 被剪掉的
 * 节点就是一个**永不触发的超时**。2026-09-19 实测：GNSS 读线程因此卡在
 * `poll()` 里 20 s（diag 行 `wd=0`、`in_poll` 一直涨、心跳停），最后只能靠
 * 12 s 的线程重启救回来（代价：模组 VCC 周期 + 十几秒断档）。
 *
 * **怎么用**：客户端给一个**无锁、无阻塞**的进度回调（就一次内存读，返回单调
 * ms 时间戳，"动一下"就更一次）；超过 `stall_ms` 没动，diag 就朝该线程发 `sig`
 * 把它从阻塞等待里踢出来 —— 信号**不经过 tick**，`nxsem_tickwait/nxsem_wait`
 * 会返回 `-EINTR`，线程自己回主循环重走一遍。不重启线程、不断电。
 *
 * 约定（都是踩过的坑）：
 *  - diag 只发信号，不代客户端做任何事；**收方必须自己处理 `-EINTR`**，handler
 *    要装，且**不能**设 `SA_RESTART`（设了信号就白发）。
 *  - 回调在 diag 线程里调用：必须无锁、无阻塞、可重入，通常就是读一个
 *    `volatile` 时间戳。`name` 要用**静态字符串**（表里只存指针）。
 *  - 已知会长时间不更新进度的操作（断电重启、等 ACK、批量刷盘），用
 *    `myvendor_diag_watch_hold()` 明说"我在忙"，否则会被当成卡死。
 *  - `stall_ms` 传 0 = 用 `MYVENDOR_DIAG_WATCH_MS_DEFAULT`；同一个 `tid` 重复
 *    注册 = 更新（改阈值/换回调）。表满返回 `-ENOSPC`。
 *  - 踢不会刷屏：同一个"没进展"窗口最多踢一次，日志再节流到 5 s。
 *  - 只发信号能解决的才有用；卡在 `nxmutex_lock()` 那种**信号免疫**的等待里，
 *    仍然要靠各自的重启路径（见 `myvendor_diag_ops_t`）。
 */
#define MYVENDOR_DIAG_WATCH_MS_DEFAULT 1500u  /**< 3× 200 ms poll 预算：够宽，又远小于 8 s 的停顿阈值 */

int  myvendor_diag_watch(const char *name, pthread_t tid, int sig,
                         uint32_t stall_ms, uint32_t (*progress_ms)(void));
void myvendor_diag_watch_hold(pthread_t tid, bool hold);
void myvendor_diag_unwatch(pthread_t tid);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_DIAG_H */
