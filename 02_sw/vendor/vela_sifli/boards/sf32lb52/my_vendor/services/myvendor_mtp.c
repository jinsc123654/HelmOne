/**
 * @file myvendor_mtp.c
 * @brief MTP 服务：拉起 mtp_simple worker、owner 侧传输控制、LittleFS 静默。
 *
 * 应用侧调用 myvendor_mtp_init() 与 myvendor_mtp_transfer_begin()；worker 自行
 * 探测 USB 并跑协议。UI 只读状态，不按插拔启停 worker。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_mtp.h"
#include "myvendor_mtp_internal.h"
#include "myvendor_mono.h"
#include "mtp_simple/mtp_features.h"
#include "transfer_backend.h"   /* xfer_usb_page_active()：USB 占用强绑定 MTP 页面 */

#include <nuttx/config.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MYVENDOR_MTP_ACTIVITY_HOLD_MS 2000u
/** @brief `myvendor_mtp_lfs_hold()` 的保鲜期（毫秒）。
 *
 *  超过它还没归还、且当前没有会话/传输，就认定是**泄漏的残留 hold**，
 *  不再用它去挡星历注入（见 `myvendor_mtp_lfs_quiesce()`）。
 *  取 30 s：正常预约是秒级的，而 MTP 一次会话（列表/读/写）也不会超过它太多；
 *  真在做会话时 `g_session`/`g_transfer_active` 会独立命中，不依赖这个超时。 */
#define MYVENDOR_MTP_LFS_HOLD_MAX_MS 30000u

#if MTP_FEAT_INFO
#  define mtp_owner_info(...) syslog(LOG_INFO, __VA_ARGS__)
#else
#  define mtp_owner_info(...) ((void)0)
#endif

/** @brief 杀 worker 之前先摘掉它的 waitdog（定义在 `bsp/bringup.c`）。
 *
 *  worker 平时睡在 `usleep`/定时等待里，杀的瞬间它多半**正武装着** `waitdog`
 *  （`nxsem_tickwait` 那条路）。上游的退出钩子会给正常退出/被杀的任务撤掉它
 *  （`task_recover.c:75` ← `task_exithook.c:459`），所以这不修任何已知洞；
 *  它是**兜底**：kill 被延迟取消、或目标卡在不可中断等待里时，先撤再杀能保证
 *  链上不会留下指向已复用内存的节点（形状见 `vela_override/sched/wd_start.c`
 *  头注释的 n004/n005），顺便让日志里出现 `cancelled stale waitdog` 时能确认
 *  "它当时正武装着"。companion 的杀路径用的是同一个函数。
 *  （本文件不 include 板级头，沿用本工程既有做法在调用点 extern。） */
extern void myvendor_task_cancel_waitdog(pid_t pid, const char *tag);

static pthread_mutex_t g_mtp_lock = PTHREAD_MUTEX_INITIALIZER;
static myvendor_mtp_activity_t g_activity;
static bool g_activity_running;
static uint64_t g_activity_hold_until_ms;
static volatile myvendor_mtp_plug_t g_plug = MYVENDOR_MTP_PLUG_OFF;
static volatile bool g_session;
static volatile bool g_transfer_req;
static volatile bool g_transfer_active;
static volatile bool g_poll_abort;
static volatile bool g_shutdown;
static volatile bool g_link_lost;
static volatile int g_lfs_hold;
/** @brief 最近一次取 hold 的时刻（毫秒），用于识别**泄漏的残留 hold**。
 *
 *  hold 是计数式的"预约"，释放路径与 MTP worker 相关；而 USB/MTP 关闭时
 *  worker 根本不跑（日志里 `mtp worker skipped (USB controller off)`），
 *  于是取下的 hold 没人释放、永久为 1。现场就是这样把星历注入挡死的：
 *  注入门口的 `myvendor_mtp_lfs_quiesce()` 四项里只有 `hold=1` 命中
 *  （`xfer=0 hold=1 sess=0 plug=0`），手机刚下发的新星历就此被丢弃。 */
static volatile uint32_t g_lfs_hold_ms;
static volatile pid_t g_worker_pid;
static volatile bool g_spawn_pending;

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
extern int mtp_simple_main(int argc, char *argv[]);

static uint8_t g_mtp_simple_stack[CONFIG_MYVENDOR_MTP_SIMPLE_STACKSIZE]
    __attribute__((aligned(16)));
#endif

static bool mtp_worker_alive(pid_t pid)
{
  return pid > 0 && kill(pid, 0) == 0;
}

/** @brief 重置协议侧 plug/session/transfer 标志（持锁调用）。 */
static void mtp_reset_protocol_state_locked(void)
{
  g_plug = MYVENDOR_MTP_PLUG_OFF;
  g_session = false;
  g_transfer_req = false;
  g_transfer_active = false;
  g_poll_abort = false;
}

/** @brief 安全拷贝名称到固定长度缓冲。 */
static void mtp_copy_name(char *dst, size_t dstsz, const char *src)
{
  if (!dst || dstsz == 0)
    {
      return;
    }

  if (!src)
    {
      dst[0] = '\0';
      return;
    }

  strncpy(dst, src, dstsz - 1);
  dst[dstsz - 1] = '\0';
}

/** @brief CLOCK_MONOTONIC 毫秒。 */
static uint64_t mtp_monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/** @brief 活动快照 hold 窗口到期后清零（持锁）。 */
static void mtp_activity_expire_locked(uint64_t now_ms)
{
  if (g_activity_running || g_activity_hold_until_ms == 0)
    {
      return;
    }

  if (now_ms >= g_activity_hold_until_ms)
    {
      memset(&g_activity, 0, sizeof(g_activity));
      g_activity.op = MYVENDOR_MTP_OP_IDLE;
      g_activity_hold_until_ms = 0;
    }
}

/**
 * @brief 清空 UI 活动快照（worker/owner 均可调用）。
 */
void myvendor_mtp_activity_clear(void)
{
  pthread_mutex_lock(&g_mtp_lock);
  memset(&g_activity, 0, sizeof(g_activity));
  g_activity.op = MYVENDOR_MTP_OP_IDLE;
  g_activity_running = false;
  g_activity_hold_until_ms = 0;
  pthread_mutex_unlock(&g_mtp_lock);
}

/**
 * @brief 开始记录一次 MTP 操作进度。
 * @param op 操作类型。
 * @param target 主目标名（可为 NULL）。
 * @param total_bytes 总字节；未知传 0。
 */
void myvendor_mtp_activity_start(myvendor_mtp_op_t op, const char *target,
                                 uint64_t total_bytes)
{
  pthread_mutex_lock(&g_mtp_lock);
  g_activity_running = true;
  g_activity_hold_until_ms = 0;
  g_activity.op = op;
  g_activity.active = true;
  mtp_copy_name(g_activity.target, sizeof(g_activity.target), target);
  g_activity.detail[0] = '\0';
  g_activity.total_bytes = total_bytes;
  g_activity.done_bytes = 0;
  g_activity.seq++;
  pthread_mutex_unlock(&g_mtp_lock);
}

/**
 * @brief 更新活动详情（如重命名目的名）。
 * @param detail 详情字符串。
 */
void myvendor_mtp_activity_set_detail(const char *detail)
{
  pthread_mutex_lock(&g_mtp_lock);
  mtp_copy_name(g_activity.detail, sizeof(g_activity.detail), detail);
  if (g_activity_running || g_activity_hold_until_ms > 0)
    {
      g_activity.seq++;
    }

  pthread_mutex_unlock(&g_mtp_lock);
}

/**
 * @brief 更新已完成字节数。
 * @param done_bytes 已完成字节。
 */
void myvendor_mtp_activity_progress(uint64_t done_bytes)
{
  pthread_mutex_lock(&g_mtp_lock);
  if (g_activity_running)
    {
      g_activity.done_bytes = done_bytes;
    }

  pthread_mutex_unlock(&g_mtp_lock);
}

/** @brief 标记操作结束并进入 hold 窗口供 UI 显示。 */
void myvendor_mtp_activity_finish(void)
{
  uint64_t now_ms = mtp_monotonic_ms();

  pthread_mutex_lock(&g_mtp_lock);
  g_activity_running = false;
  g_activity_hold_until_ms = now_ms + MYVENDOR_MTP_ACTIVITY_HOLD_MS;
  g_activity.active = true;
  pthread_mutex_unlock(&g_mtp_lock);
}

/**
 * @brief 复制当前活动快照。
 * @param[out] activity 输出；NULL 则 -EINVAL。
 * @return 0 成功。
 */
int myvendor_mtp_get_activity(myvendor_mtp_activity_t *activity)
{
  uint64_t now_ms;

  if (!activity)
    {
      return -EINVAL;
    }

  now_ms = mtp_monotonic_ms();
  pthread_mutex_lock(&g_mtp_lock);
  mtp_activity_expire_locked(now_ms);
  *activity = g_activity;
  activity->active = g_activity_running ||
                     (g_activity_hold_until_ms > 0 &&
                      now_ms < g_activity_hold_until_ms);
  pthread_mutex_unlock(&g_mtp_lock);
  return 0;
}

/** @brief plug 枚举转日志字符串。 */
static const char *plug_name(myvendor_mtp_plug_t p)
{
  switch (p)
    {
    case MYVENDOR_MTP_PLUG_OFF:
      return "OFF";
    case MYVENDOR_MTP_PLUG_ENUM:
      return "ENUM";
    case MYVENDOR_MTP_PLUG_ACTIVE:
      return "ACTIVE";
    default:
      return "?";
    }
}

/** @brief 设置 plug 阶段并打日志（持锁）。 */
static void mtp_set_plug_locked(myvendor_mtp_plug_t plug)
{
  myvendor_mtp_plug_t prev = g_plug;

  if (prev == plug)
    {
      return;
    }

  g_plug = plug;
  mtp_owner_info("myvendor_mtp: plug %s -> %s",
         plug_name(prev), plug_name(plug));
}

/**
 * @brief worker 上报 USB 链路阶段。
 * @param state #myvendor_mtp_plug_t 值。
 */
void myvendor_mtp_lfs_set_link(myvendor_mtp_link_state_t state)
{
  pthread_mutex_lock(&g_mtp_lock);
  mtp_set_plug_locked((myvendor_mtp_plug_t)state);
  pthread_mutex_unlock(&g_mtp_lock);
}

/**
 * @brief worker 上报 MTP session 开关。
 * @param active true 为 OPEN。
 */
void myvendor_mtp_lfs_set_session(bool active)
{
  if (g_session == active)
    {
      return;
    }

  g_session = active;
  mtp_owner_info("myvendor_mtp: session %s (plug=%s)",
         active ? "OPEN" : "CLOSE", plug_name(g_plug));
}

/**
 * @brief worker 进入/退出协议 poll ACTIVE。
 * @param active true 表示正在 poll。
 */
void myvendor_mtp_worker_set_active(bool active)
{
  pthread_mutex_lock(&g_mtp_lock);
  g_transfer_active = active;
  g_poll_abort = false;
  if (active)
    {
      g_link_lost = false;
      mtp_set_plug_locked(MYVENDOR_MTP_PLUG_ACTIVE);
    }
  else if (g_plug == MYVENDOR_MTP_PLUG_ACTIVE)
    {
      mtp_set_plug_locked(MYVENDOR_MTP_PLUG_ENUM);
    }

  pthread_mutex_unlock(&g_mtp_lock);
}

/** @brief I/O 中断链时由 worker 调用。 */
void myvendor_mtp_worker_link_lost(void)
{
  bool was_busy;

  pthread_mutex_lock(&g_mtp_lock);
  was_busy = g_transfer_active || g_session;
  g_transfer_active = false;
  g_link_lost = was_busy;

  /* 链路丢了就是主机不在了 —— 不管当时有没有会话/传输，plug 都必须回到 OFF。
   *
   * 2026-09-19 现场：**弹出 MTP 界面后快速拔线**，plug 停在 ACTIVE 再也不动，
   * UI 的 `host_attached()` 对 ACTIVE 是无条件 true ⇒ 永远判不出"已离开"⇒
   * MTP 页永远退不出去。原因就在这里：原来只有 `was_busy` 为真才置 OFF，
   * 而"刚枚举完、还没有任何传输"时 was_busy 是假，状态就被留在 ENUM/ACTIVE。 */
  if (was_busy || g_plug >= MYVENDOR_MTP_PLUG_ENUM)
    {
      g_session = false;
      mtp_set_plug_locked(MYVENDOR_MTP_PLUG_OFF);
    }

  pthread_mutex_unlock(&g_mtp_lock);

  if (was_busy)
    {
      mtp_owner_info("myvendor_mtp: link lost during I/O");
    }
}

/** @brief UI 是否应提示链路丢失。 */
bool myvendor_mtp_worker_link_lost_pending(void)
{
  return g_link_lost;
}

/** @brief 清除链路丢失挂起标志。 */
void myvendor_mtp_worker_clear_link_lost(void)
{
  pthread_mutex_lock(&g_mtp_lock);
  g_link_lost = false;
  pthread_mutex_unlock(&g_mtp_lock);
}

/**
 * @brief worker 阻塞直到可开始 poll 或应退出。
 * @return true 可开始；false 应退出。
 */
bool myvendor_mtp_worker_wait_begin(void)
{
  for (; ; )
    {
      if (g_shutdown)
        {
          return false;
        }

      if (g_transfer_req && g_plug >= MYVENDOR_MTP_PLUG_ENUM)
        {
          return true;
        }

      if (g_plug == MYVENDOR_MTP_PLUG_OFF)
        {
          return false;
        }

      usleep(100 * 1000);
    }
}

/** @brief worker poll 是否应中止。 */
bool myvendor_mtp_worker_poll_abort(void)
{
  return g_poll_abort || g_shutdown || !g_transfer_req;
}

/** @brief worker 是否收到 shutdown。 */
bool myvendor_mtp_worker_should_exit(void)
{
  return g_shutdown;
}

/** @brief worker 退出前清 pid 绑定。 */
void myvendor_mtp_worker_detach(void)
{
  pid_t self = getpid();

  pthread_mutex_lock(&g_mtp_lock);
  if (g_worker_pid == self)
    {
      g_worker_pid = 0;
    }

  pthread_mutex_unlock(&g_mtp_lock);
}

/**
 * @brief worker 启动后登记 pid。
 * @return 0 成功；-EEXIST 已有其他 worker。
 */
int myvendor_mtp_worker_attach(void)
{
  pid_t self = getpid();

  pthread_mutex_lock(&g_mtp_lock);
  if (g_worker_pid > 0 && g_worker_pid != self)
    {
      pthread_mutex_unlock(&g_mtp_lock);
      return -EEXIST;
    }

  g_worker_pid = self;
  g_spawn_pending = false;
  g_shutdown = false;
  pthread_mutex_unlock(&g_mtp_lock);
  mtp_owner_info("myvendor_mtp: worker attach pid=%ld", (long)self);
  return 0;
}

/**
 * @brief 启动后台 mtp_simple worker（幂等）。
 * @return 0 成功，负值为错误码。
 */
int myvendor_mtp_init(void)
{
#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  posix_spawnattr_t attr;
  FAR char *argv[] = { NULL };
  int pid;

  pthread_mutex_lock(&g_mtp_lock);
  g_shutdown = false;
  g_link_lost = false;

  if (g_worker_pid > 0 && !mtp_worker_alive(g_worker_pid))
    {
      syslog(LOG_WARNING, "myvendor_mtp: stale worker pid=%ld, respawning",
             (long)g_worker_pid);
      g_worker_pid = 0;
      g_spawn_pending = false;
      mtp_reset_protocol_state_locked();
    }

  if (g_worker_pid > 0 || g_spawn_pending)
    {
      pthread_mutex_unlock(&g_mtp_lock);
      return 0;
    }

  g_spawn_pending = true;
  pthread_mutex_unlock(&g_mtp_lock);

  posix_spawnattr_init(&attr);
  attr.priority  = CONFIG_MYVENDOR_MTP_SIMPLE_PRIORITY;
  attr.stacksize = sizeof(g_mtp_simple_stack);
  posix_spawnattr_setstackaddr(&attr, g_mtp_simple_stack);

  pid = task_spawn("mtp_simple", mtp_simple_main, NULL, &attr, argv, NULL);
  if (pid < 0)
    {
      pthread_mutex_lock(&g_mtp_lock);
      g_spawn_pending = false;
      pthread_mutex_unlock(&g_mtp_lock);
      syslog(LOG_ERR, "myvendor_mtp: task_spawn failed: %d", pid);
      return pid;
    }

  pthread_mutex_lock(&g_mtp_lock);
  g_worker_pid = (pid_t)pid;
  g_spawn_pending = false;
  pthread_mutex_unlock(&g_mtp_lock);
  mtp_owner_info("myvendor_mtp: worker started pid=%d", pid);
  return 0;
#else
  return -ENOTSUP;
#endif
}

/** @brief 请求 shutdown 并等待 worker 退出。 */
void myvendor_mtp_deinit(void)
{
  pid_t pid;
  int i;

  pthread_mutex_lock(&g_mtp_lock);
  g_shutdown = true;
  g_transfer_req = false;
  g_poll_abort = true;
  pid = g_worker_pid;
  pthread_mutex_unlock(&g_mtp_lock);
  myvendor_mtp_activity_clear();
  mtp_owner_info("myvendor_mtp: deinit (worker pid=%ld)", (long)pid);

  if (pid > 0)
    {
      for (i = 0; i < 30; i++)
        {
          if (!mtp_worker_alive(pid))
            {
              break;
            }

          usleep(100 * 1000);
        }

      if (mtp_worker_alive(pid))
        {
          /* 先撤 waitdog 再 SIGTERM：worker 此刻多半正睡在定时等待里。 */
          myvendor_task_cancel_waitdog(pid, "myvendor_mtp");
          kill(pid, SIGTERM);
          for (i = 0; i < 10; i++)
            {
              if (!mtp_worker_alive(pid))
                {
                  break;
                }

              usleep(100 * 1000);
            }
        }
    }

  pthread_mutex_lock(&g_mtp_lock);
  if (!mtp_worker_alive(g_worker_pid))
    {
      g_worker_pid = 0;
      g_spawn_pending = false;
      mtp_reset_protocol_state_locked();
    }

  pthread_mutex_unlock(&g_mtp_lock);
}

/** @brief 自行车重启时清 owner 侧 hold/传输标志。 */
void myvendor_mtp_owner_reset(void)
{
  pthread_mutex_lock(&g_mtp_lock);
  g_shutdown = false;
  g_lfs_hold = 0;
  g_link_lost = false;
  g_transfer_req = false;
  g_poll_abort = false;
  pthread_mutex_unlock(&g_mtp_lock);
  myvendor_mtp_activity_clear();
  mtp_owner_info("myvendor_mtp: owner reset");
}

/** @brief 关机前放开传输/LFS hold（不停 worker）。 */
void myvendor_mtp_prepare_poweroff(void)
{
  pthread_mutex_lock(&g_mtp_lock);
  g_lfs_hold = 0;
  pthread_mutex_unlock(&g_mtp_lock);
  myvendor_mtp_activity_clear();
  mtp_owner_info("myvendor_mtp: prepare poweroff");
}

/**
 * @brief 允许 worker 跑协议（init 后调用一次）。
 * @return 0 已接受；-EINVAL 主机还不到 ENUM。
 */
int myvendor_mtp_transfer_begin(void)
{
  pthread_mutex_lock(&g_mtp_lock);
  g_transfer_req = true;
  g_poll_abort = false;

  if (g_transfer_active)
    {
      pthread_mutex_unlock(&g_mtp_lock);
      return 0;
    }

  if (g_plug < MYVENDOR_MTP_PLUG_ENUM)
    {
      pthread_mutex_unlock(&g_mtp_lock);
      mtp_owner_info("myvendor_mtp: transfer_begin queued (awaiting ENUM)");
      return -EINVAL;
    }

  pthread_mutex_unlock(&g_mtp_lock);
  mtp_owner_info("myvendor_mtp: transfer_begin (enabled)");
  return 0;
}

/** @brief 旧接口：只停 owner 侧 poll 请求。 */
void myvendor_mtp_transfer_end(void)
{
  pthread_mutex_lock(&g_mtp_lock);
  g_transfer_req = false;
  g_poll_abort = true;
  pthread_mutex_unlock(&g_mtp_lock);
  myvendor_mtp_activity_clear();
  mtp_owner_info("myvendor_mtp: transfer_end (owner request)");
}

/** @brief 最近几次 lfs hold 的取用者（只用于诊断：残留 hold 归谁）。
 *
 *  hold 是计数式的、**没有 owner 字段**：泄漏时只知道"有人没还"，不知道是谁。
 *  留一圈最近取用记录（谁、什么时候），在判定残留或挡住别人时打出来 ——
 *  现场就能直接指名，而不必再从"释放路径在谁手里"往回推。
 */
#define MYVENDOR_MTP_HOLD_TRACE 4

struct mtp_hold_trace_s
{
  const char *who;
  uint32_t    ms;
};

static struct mtp_hold_trace_s g_hold_trace[MYVENDOR_MTP_HOLD_TRACE];
static uint8_t g_hold_trace_next;

static void mtp_hold_trace_add(const char *who)
{
  g_hold_trace[g_hold_trace_next].who = (who != NULL) ? who : "?";
  g_hold_trace[g_hold_trace_next].ms = (uint32_t)myvendor_mono_ms();
  g_hold_trace_next =
      (uint8_t)((g_hold_trace_next + 1u) % MYVENDOR_MTP_HOLD_TRACE);
}

/** @brief 环形记录按"最老→最新"写成 `who+ageMs`（age 相对当前时刻）。 */
static void mtp_hold_trace_fmt(char *buf, size_t n)
{
  uint32_t now = (uint32_t)myvendor_mono_ms();
  size_t k = 0;
  uint8_t i;

  if (buf == NULL || n == 0u)
    {
      return;
    }

  buf[0] = '\0';
  for (i = 0; i < MYVENDOR_MTP_HOLD_TRACE; i++)
    {
      uint8_t idx =
          (uint8_t)((g_hold_trace_next + i) % MYVENDOR_MTP_HOLD_TRACE);
      int w;

      if (g_hold_trace[idx].who == NULL || k + 24u >= n)
        {
          continue;
        }

      w = snprintf(buf + k, n - k, "%s%s+%ums", (k > 0u) ? " " : "",
                   g_hold_trace[idx].who,
                   (unsigned)(now - g_hold_trace[idx].ms));
      if (w <= 0 || (size_t)w >= n - k)
        {
          buf[k] = '\0';
          break;
        }

      k += (size_t)w;
    }

  if (k == 0u)
    {
      snprintf(buf, n, "-");
    }
}

/** @brief UI 准备期间提前占用 LFS（计数 +1）。 */
void myvendor_mtp_lfs_hold(const char *who)
{
  int hold;

  pthread_mutex_lock(&g_mtp_lock);
  g_lfs_hold++;
  g_lfs_hold_ms = myvendor_mono_ms();   /* 保鲜期起点，见 quiesce() */
  mtp_hold_trace_add(who);
  hold = g_lfs_hold;
  pthread_mutex_unlock(&g_mtp_lock);
  mtp_owner_info("myvendor_mtp: lfs_hold by %s count=%d",
                 (who != NULL) ? who : "?", hold);
  (void)hold;
}

/** @brief 与 #myvendor_mtp_lfs_hold 配对（计数 -1）。 */
void myvendor_mtp_lfs_release(const char *who)
{
  pthread_mutex_lock(&g_mtp_lock);
  if (g_lfs_hold > 0)
    {
      g_lfs_hold--;

      if (g_lfs_hold == 0)
        {
          g_lfs_hold_ms = 0;   /* 全部归还：保鲜期也结束 */
        }
    }
  else
    {
      /* 多还了。两种来源：配对写错，或者泄漏的 hold 刚被 quiesce() 回收、
       * 晚到的 release 才到。两种都值得知道，但只需知道一次 ——
       * 否则又是一条"按次数刷屏"的日志源。 */
      static bool underflow_logged;

      if (!underflow_logged)
        {
          underflow_logged = true;
          syslog(LOG_WARNING,
                 "mtp: lfs_release by %s with hold=0 (多还 / 回收后晚到)\n",
                 (who != NULL) ? who : "?");
        }
    }

  pthread_mutex_unlock(&g_mtp_lock);
}

/** @brief 设备 UI 是否停在 MTP 传输页（= quiesce 里"与 MTP 有关"的那个成因）。 */
bool myvendor_mtp_page_active(void)
{
  return xfer_usb_page_active();
}

/** @brief 自行车/vmap 是否必须推迟 LittleFS I/O。 */
bool myvendor_mtp_lfs_quiesce(void)
{
  bool q;
  bool hold_live = false;

  pthread_mutex_lock(&g_mtp_lock);

  if (g_lfs_hold > 0)
    {
      /* hold 只在**保鲜期内**才算数 —— "它附近确实有会话/传输"不再参与判定
       * （USB 状态不再是 LFS 让位的理由，见下面的 q）。
       *
       * 只看保鲜期，是为了不让一个**泄漏的残留 hold 永久挡死星历注入** ——
       * 现场就是 `xfer=0 hold=1 sess=0 plug=0`：只有 hold 命中，而释放路径
       * 要么在 MTP worker 里（USB 关闭时 worker 不跑），要么在某个提前返回的
       * 分支上被漏掉了。手机刚下发的新星历因此被整段丢弃，
       * 表现就是"手机推了星历，GNSS 却没同步"。
       *
       * hold 的本意是"UI/App 马上要用 LFS，别人让一让"：天然是秒级预约；
       * 超过保鲜期还没归还，就是泄漏而不是预约。 */
      hold_live = myvendor_mono_elapsed_ms(myvendor_mono_ms(),
                                           g_lfs_hold_ms) <
                  MYVENDOR_MTP_LFS_HOLD_MAX_MS;

      if (!hold_live)
        {
          /* 泄漏的残留 hold：**在源头回收，而不只是"这次忽略它"**。
           *
           * 只忽略的话这个计数会永远挂着：下一个取用方从 1 起算（本该从 0），
           * 而且每个消费者都得各自等过这 30 s —— 现场就是倒库/注入被白挡
           * 二十多秒（还按字节率刷日志）。这里直接清零，让它回到"没人占"；
           * 晚到的 release 由下溢保护挡住，不会变成负数。
           *
           * 这是缺陷信号、不是常态，所以最多打三次。 */
          char tr[96];
          static uint32_t stale_n;

          g_lfs_hold = 0;
          g_lfs_hold_ms = 0;

          if (stale_n < 3u)
            {
              stale_n++;
              mtp_hold_trace_fmt(tr, sizeof(tr));
              syslog(LOG_WARNING,
                     "mtp: leaked lfs_hold reclaimed (age>=%u ms, sess=0 xfer=0 "
                     "plug=0, last takes: %s) — LFS unblocked\n",
                     (unsigned)MYVENDOR_MTP_LFS_HOLD_MAX_MS, tr);
            }
        }
    }

  /* **强绑定 MTP 页面**（用户 2026-09-20 定："只有 MTP 页面弹出才给锁给标志"）：
   * 页面弹出（= `xfer_usb_page_active()`）或有人显式 hold LFS 才让路。
   *
   * 历史：这里或过 `g_plug >= ENUM`（插线就冻）、`g_session`、`g_transfer_active`
   * —— 那些在"插线充电、只枚举不选 MTP、插着线一直不退出"时都会把 LFS 冻住
   * （手机一连上就报 USB 占用、星历被判 mtp busy）。插线/会话/协议 poll 照旧在
   * `quiesce_why()` 与 status 里看得见，仅作上下文。 */
  q = xfer_usb_page_active() || hold_live;

  pthread_mutex_unlock(&g_mtp_lock);
  return q;
}

void myvendor_mtp_lfs_quiesce_why(char *buf, size_t n)
{
  bool     xfer;
  bool     sess;
  int      hold;
  unsigned plug;

  if (buf == NULL || n == 0u)
    {
      return;
    }

  pthread_mutex_lock(&g_mtp_lock);
  xfer = g_transfer_active;
  hold = g_lfs_hold;
  sess = g_session;
  plug = (unsigned)g_plug;
  pthread_mutex_unlock(&g_mtp_lock);

  /* plug 的语义见 myvendor_mtp_plug_t：0=OFF（未插/仅充电）, 1=ENUM, 2=ACTIVE。
   *
   * **这些字段全是"上下文"，不参与 quiesce 判定**（2026-09-20 起判定只剩 hold）：
   * 插线状态、session、协议 poll 都不再让 LFS 让位，也不再有"USB 占用"上报。 */
  /* `page` = **设备 UI 是否停在 MTP 传输页** —— USB 占用的唯一判据（用户
   * 2026-09-20 定："把这个 USB 占用换为 MTP 页面强绑定"：页面弹出才算占用，
   * 屏上是地图/骑行页就一律不算，哪怕线插着、gadget 在枚举、MTP 在 poll）。
   * 其余字段（xfer/sess/plug）只作上下文，方便区分"谁在动"。 */
  snprintf(buf, n, "page=%d hold=%d (上下文: xfer=%d sess=%d plug=%u)",
           xfer_usb_page_active() ? 1 : 0, hold,
           xfer ? 1 : 0, sess ? 1 : 0, plug);

  /* hold 命中时把"最近是谁取的"也带上：这个字符串会出现在
   * `gnss: dbd dump skip (mtp busy: …)` / `eph inject skip (mtp busy: …)`
   * 里，于是"谁把 hold 挂住了"在同一行就能看到。 */
  if (hold > 0)
    {
      char tr[96];
      size_t used = strlen(buf);

      mtp_hold_trace_fmt(tr, sizeof(tr));
      snprintf(buf + used, (n > used) ? n - used : 0u,
               " hold_by[%s]", tr);
    }
}

/**
 * @brief 复制链路快照。
 * @param[out] status 输出；NULL 则 -EINVAL。
 * @return 0 成功。
 */
/** 端点文件消失多久就认定主机已拔（防"快速拔线"漏状态）。 */
#define MTP_EP_GONE_OFF_MS  1500u

/** 端点消失的起点（0 = 端点还在）。 */
static uint32_t g_ep_gone_since;

/**
 * @brief 兜底：plug 还在 ENUM/ACTIVE，但 USB 端点文件已经没了 ⇒ 强制回 OFF。
 *
 * 端点文件（`MYVENDOR_MTP_EP_OUT_PATH`）由 USB class 驱动在枚举时建立、拔线时
 * 移除，是"主机还在不在"最直接的事实。快速拔线时 worker 可能来不及报
 * `active=false` / `link_lost`（或报了但当时没有会话 ⇒ 见上面那条修复之前的
 * 行为），plug 就会停在 ACTIVE —— 而 UI 的 `host_attached()` 对 ACTIVE 是无条件
 * true，MTP 页于是永远退不出去。这里用"端点消失 1.5 s"把状态拉回来，保证无论
 * 哪条路径漏了通知，状态机都能自愈。
 *
 * **调用方必须持有 g_mtp_lock。**
 */
static void mtp_ep_failsafe_locked(uint32_t now)
{
  bool ep_present;

  if (g_plug == MYVENDOR_MTP_PLUG_OFF)
    {
      g_ep_gone_since = 0;
      return;
    }

  ep_present = access(MYVENDOR_MTP_EP_OUT_PATH, F_OK) == 0;
  if (ep_present)
    {
      g_ep_gone_since = 0;
      return;
    }

  if (g_ep_gone_since == 0)
    {
      g_ep_gone_since = now ? now : 1u;
      return;
    }

  if ((uint32_t)(now - g_ep_gone_since) < MTP_EP_GONE_OFF_MS)
    {
      return;
    }

  syslog(LOG_WARNING,
         "myvendor_mtp: plug forced OFF (no endpoint for %u ms, was %s)\n",
         (unsigned)MTP_EP_GONE_OFF_MS, plug_name(g_plug));
  g_ep_gone_since = 0;
  g_session = false;
  g_transfer_active = false;
  g_transfer_req = false;
  mtp_set_plug_locked(MYVENDOR_MTP_PLUG_OFF);
}

int myvendor_mtp_get_status(myvendor_mtp_status_t *status)
{
  if (!status)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_mtp_lock);

  /* 先跑兜底，再填状态：调用方（UI 每帧一次）拿到的一定是已经纠正过的 plug。 */
  mtp_ep_failsafe_locked((uint32_t)myvendor_mono_ms());

  status->plug = g_plug;
  status->session_open = g_session;
  status->ep_present = access(MYVENDOR_MTP_EP_OUT_PATH, F_OK) == 0;
  status->worker_running = g_worker_pid > 0 && !g_shutdown;
  status->transfer_pending = g_transfer_req && !g_transfer_active;
  status->transfer_active = g_transfer_active;
  /* `lfs_quiesce` 与 `myvendor_mtp_lfs_quiesce()` 同口径：**只有未过期的 hold**。
   * USB（插线/session/协议 poll）不参与（用户 2026-09-20："USB 占用不要了"）。
   * 这里不能直接调那个函数 —— 它自己也拿 g_mtp_lock，会自锁。 */
  /* `lfs_quiesce` 与 `myvendor_mtp_lfs_quiesce()` 同口径：**MTP 页面弹出（或未过期的
   * hold）**。插线/session/协议 poll 不参与（用户 2026-09-20 定："把这个 USB 占用
   * 换为 MTP 页面强绑定"）。这里不能直接调那个函数 —— 它自己也拿 g_mtp_lock，会自锁。 */
  status->lfs_quiesce =
      xfer_usb_page_active() ||
      (g_lfs_hold > 0 &&
       myvendor_mono_elapsed_ms(myvendor_mono_ms(), g_lfs_hold_ms) <
           MYVENDOR_MTP_LFS_HOLD_MAX_MS);
  pthread_mutex_unlock(&g_mtp_lock);
  return 0;
}

/** @brief USB 主机是否在线（枚举或 ACTIVE）。 */
bool myvendor_mtp_host_present(void)
{
  myvendor_mtp_status_t st;

  myvendor_mtp_get_status(&st);
  return st.plug >= MYVENDOR_MTP_PLUG_ENUM || st.ep_present;
}

/** @brief 主机是否已选 MTP 配置。 */
bool myvendor_mtp_host_is_mtp(void)
{
  myvendor_mtp_status_t st;

  myvendor_mtp_get_status(&st);
  return st.plug >= MYVENDOR_MTP_PLUG_ENUM;
}

/** @brief 是否**正在跑** MTP 会话（ACTIVE，宿主已选 MTP 配置并在交互）。
 *
 * 与 `myvendor_mtp_host_is_mtp()` 的区别：后者只要"枚举过"（>= ENUM）就算真，
 * 导致**插着充电器也会**在状态帧里报 `USB_MTP_BUSY` —— app 于是显示"USB 占用中"。
 * 状态位只应在真有 MTP 会话时亮。 */
bool myvendor_mtp_session_active(void)
{
  myvendor_mtp_status_t st;

  myvendor_mtp_get_status(&st);
  return st.plug >= MYVENDOR_MTP_PLUG_ACTIVE;
}

/** @brief 把状态打到 printf，供 NSH 诊断。 */
void myvendor_mtp_log_status(void)
{
  myvendor_mtp_status_t st;

  myvendor_mtp_get_status(&st);
  printf("mtp plug=%s session=%s ep2=%s worker=%d pending=%d active=%d quiesce=%d hold=%d\n",
         plug_name(st.plug),
         st.session_open ? "OPEN" : "CLOSE",
         st.ep_present ? "present" : "missing",
         (int)st.worker_running,
         (int)st.transfer_pending,
         (int)st.transfer_active,
         (int)st.lfs_quiesce,
         g_lfs_hold);
}
