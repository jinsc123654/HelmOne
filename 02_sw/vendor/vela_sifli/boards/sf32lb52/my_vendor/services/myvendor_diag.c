/**
 * @file myvendor_diag.c
 * @brief 外设诊断线程。BLE / GNSS 以函数表注册。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_coredump.h"
#include "myvendor_diag.h"
#include "myvendor_diaglog.h"
#include "sf32lb_dvfs.h"
#include "sf32lb_sdio.h"

#include "myvendor_ble_log.h"
#include "myvendor_gnss.h"
#include "myvendor_mono.h"
#include "myvendor_schedmon.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#if defined(CONFIG_MYVENDOR_BLE_COMPANION) || defined(CONFIG_MYVENDOR_BICYCLE)
#  include "companion_bridge.h"
#endif

#ifdef CONFIG_BLUETOOTH
bool sf32lb52_bt_hci_rx_stalled(uint32_t stall_ms);
bool sf32lb52_bt_hci_cmd_stalled(uint32_t stall_ms);
bool sf32lb52_bt_hci_skip_sync(void);
int  sf32lb52_bt_controller_force_reset(void);
void sf32lb52_bt_hci_dump_stall(void);
#endif

#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
int board_restart_ble_companion(void);
bool board_ble_companion_pid_alive(void);
bool board_ble_companion_busy(void);
#endif

#define DIAG_MAX            8u
#define DIAG_TICK_MS        500u
#define DIAG_GRACE_MS       25000u
#define DIAG_STACK          4096
#define DIAG_BLE_STALL_MS   3000u
#define DIAG_BLE_CMD_MS     4000u
/** 命令沉默"保护链路"的硬上限：超过它就不再放行（见 cmdsilent 分支的注释）。
 *  三条恢复路径都靠"链路在"放行，而扫描/建链期间那个判据也是真 ——
 *  2026-09-18 手机和心率计同时连不上，就是这么饿死的。 */
#define DIAG_BLE_CMD_HARD_MS 20000u
#define DIAG_BLE_THREAD_MS  15000u

/* 传感器（central）链路是否占用射频：cmdsilent 分支的守卫用（见那里注释）。 */
extern bool ble_sensor_connect_busy(void);
extern uint8_t ble_sensor_ready_mask(void);

/* 日志落盘节流，理由见 diag_thread() 里的落盘块：
 * - FLUSH_MS：稳态多久刷一次（把 SD 忙等 I/O 从 2 次/秒降到 0.5 次/秒）。
 * - HIWATER_BYTES：环内积压达到它就提前刷（环总共只有 2 KB，刷屏期不能等）。 */
#define DIAGLOG_FLUSH_MS       2000u
#define DIAGLOG_FLUSH_HIWATER  1024u
#ifndef CONFIG_MYVENDOR_DIAG_PRIORITY
#  define DIAG_PRIORITY     9
#else
#  define DIAG_PRIORITY     CONFIG_MYVENDOR_DIAG_PRIORITY
#endif

struct diag_slot
{
  myvendor_diag_ops_t ops;
  uint32_t fail_since_ms;
  uint32_t last_restart_ms;
  uint16_t restarts;
};

static uint8_t g_diag_stack[DIAG_STACK]
    __attribute__((aligned(16)));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct diag_slot g_slot[DIAG_MAX];
static uint32_t g_flush_ms;     /* 上次日志落盘的时刻（0 = 还没刷过） */
static uint8_t g_n;
static bool g_started;
static uint32_t g_boot_ms;

static uint32_t diag_now(void)
{
  return myvendor_mono_ms();
}

static uint32_t diag_elapsed(uint32_t now, uint32_t then)
{
  return myvendor_mono_elapsed_ms(now, then);
}

#ifdef CONFIG_BLUETOOTH
static uint8_t g_ble_soft;
static uint32_t g_ble_why_ms;
/** @brief 最近一次 BLE 诊断异常的单调时刻（ms；0 = 从未）。状态栏拿它做"蓝牙图标红灯闪"。 */
static uint32_t g_ble_bad_ms;

/**
 * @brief 距最近一次 BLE 诊断异常的毫秒数（`UINT32_MAX` = 从未异常过）。
 *
 * @note 用户 2026-09-27：状态栏据此让**蓝牙图标红灯闪烁**。只要有新的异常上报，年龄就被
 *       刷新 ⇒ 闪烁窗口跟着续期。判据在状态栏侧（年龄 < 10 s 就闪），这里只提供读数。
 */
uint32_t myvendor_diag_ble_unhealthy_age_ms(void)
{
  if (g_ble_bad_ms == 0u)
    {
      return UINT32_MAX;
    }

  return diag_elapsed(diag_now(), g_ble_bad_ms);
}

static void diag_ble_log_unhealthy(const char *why)
{
  uint32_t now = diag_now();

  /* 每次上报都刷新（**在 2s 限频之前**）：闪烁窗口据此续期，不会"报了一下就熄"。 */
  g_ble_bad_ms = now ? now : 1u;

  if (g_ble_why_ms != 0 && diag_elapsed(now, g_ble_why_ms) < 2000u)
    {
      return;
    }

  g_ble_why_ms = now;
  syslog(LOG_ERR,
         "diag: ble not ok (%s) hb=%u alive=%d skip=%d rxstall=%d "         "cmdsilent=%d busy=%d phase=%s phone=%d\n",
         why,
#if defined(CONFIG_MYVENDOR_BLE_COMPANION) || defined(CONFIG_MYVENDOR_BICYCLE)
         (unsigned)companion_bridge_heartbeat_age_ms(),
         companion_bridge_alive_get() ? 1 : 0,
#else
         0u,
         0,
#endif
         sf32lb52_bt_hci_skip_sync() ? 1 : 0,
         sf32lb52_bt_hci_rx_stalled(DIAG_BLE_STALL_MS) ? 1 : 0,
         sf32lb52_bt_hci_cmd_stalled(DIAG_BLE_CMD_MS) ? 1 : 0,
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
         board_ble_companion_busy() ? 1 : 0,
#else
         0,
#endif
#if defined(CONFIG_MYVENDOR_BLE_COMPANION) || defined(CONFIG_MYVENDOR_BICYCLE)
         companion_bridge_phase_get(),
         companion_bridge_phone_get() ? 1 : 0
#else
         "-",
         0
#endif
         );
  sf32lb52_bt_hci_dump_stall();
}

static bool diag_ble_ok(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
  if (board_ble_companion_busy())
    {
      return true;
    }
#endif

#if defined(CONFIG_MYVENDOR_BLE_COMPANION) || defined(CONFIG_MYVENDOR_BICYCLE)
  if (!companion_bridge_thread_ok(DIAG_BLE_THREAD_MS))
    {
      diag_ble_log_unhealthy("thread");
      return false;
    }
#endif

  if (sf32lb52_bt_hci_rx_stalled(DIAG_BLE_STALL_MS))
    {
      diag_ble_log_unhealthy("rxstall");
      return false;
    }

  /** 控制器静默：命令确实发出去了，DIAG_BLE_CMD_MS 内一个事件都没回来。
   *
   *  与上面那条互补 —— `rx_stalled()` 只看**收包失败**（回调返回 <0），
   *  控制器彻底哑掉时一次都不失败（现场 `rxf=0`），所以那条看不见。
   *  2026-09-17 那次就是：562 s 之后 RX 全停，健康检查一路绿灯，直到
   *  605 s companion 心跳停掉才报 "thread"，中间 40 s 白等；再往后
   *  三个 L2CAP/命令缓冲池被抽干，多个线程卡在不可中断等待里，连
   *  三连杀都收不回来。这条能在 4 s 内把它拉起来复位。
   */
  /* 命令沉默的起点（0 = 当前不沉默）。见下面那条"硬上限"。 */
  {
    static uint32_t cmd_silent_since_ms;

    if (!sf32lb52_bt_hci_cmd_stalled(DIAG_BLE_CMD_MS))
      {
        cmd_silent_since_ms = 0;
      }
    else if (cmd_silent_since_ms == 0)
      {
        cmd_silent_since_ms = diag_now() != 0 ? diag_now() : 1u;
      }

  if (sf32lb52_bt_hci_cmd_stalled(DIAG_BLE_CMD_MS))
    {
      /* **传感器链路占着射频时，命令沉默不算故障。**
       *
       * 现场：心率计一连上（我们=central），"开始广播"（`txop=0x200a`
       * = LE Set Advertising Enable）就被 LCPU 吞掉 —— 没有 Command Status、
       * 4.4 s 无任何 HCI 收发、`status=2`(NOT_READY)。以前这里一律判不健康 →
       * 复位 LCPU → **把心率计踢掉**（disconn 0x16）→ 8 s 后它连回来 → 再吞
       * 一次：自杀式循环。用户侧看到的就是"心率计活着时别的设备（含手机）
       * 搜不到板子；把心率计关机立刻就能搜到"。
       *
       * 所以：有传感器链路就记一行软告警放行，**不动适配器**；广播由
       * `g_ctx.adv_restart` 在链路空下来时自然接管。 */
      /* **放行必须有硬上限。**
       *
       * 2026-09-18 现场：手机连不上、心率计也连不上，而这里一直在放行 ——
       * 因为 `ble_sensor_connect_busy()` 在**扫描/建链期间**也为真，链路其实
       * 从来没建立起来。于是三条恢复路径同时被堵：这条放行、广播失败不计故障、
       * 而 `rxstall` 靠"RX 回调失败"计数（控制器哑掉时 `rxf=0`）也不触发 ——
       * 整机 BLE 饿死。
       *
       * 所以：沉默超过 `DIAG_BLE_CMD_HARD_MS` 就不再"保护链路"（那时链路本来
       * 就不可用），照常判不健康 → cycle → 升级阶梯。 */
      /* 判据是"**确实有链路**"（READY 槽位），不是"正在扫描/建链" ——
       * 2026-09-18 16:24 现场：什么都没连上（没有 connected、没有 connecting），
       * 只因为 sensor 侧在忙就放行了 ~15 s，白等了。 */
      if (ble_sensor_ready_mask() != 0u &&
          diag_elapsed(diag_now(), cmd_silent_since_ms) < DIAG_BLE_CMD_HARD_MS)
        {
          static bool deferred_logged;

          if (!deferred_logged)
            {
              deferred_logged = true;
              syslog(LOG_WARNING,
                     "diag: ble cmd silent with sensor link up — "
                     "adv deferred, no adapter cycle\n");
            }

          return true;
        }

      if (diag_elapsed(diag_now(), cmd_silent_since_ms) >= DIAG_BLE_CMD_HARD_MS)
        {
          static bool hard_logged;

          if (!hard_logged)
            {
              hard_logged = true;
              syslog(LOG_WARNING,
                     "diag: ble cmd silent over %u s — cycling anyway "
                     "(link protection capped)\n",
                     (unsigned)(DIAG_BLE_CMD_HARD_MS / 1000u));
            }
        }

      diag_ble_log_unhealthy("cmdsilent");
      return false;
    }
  }

  /* 适配器不可用 / 正在被我们自己恢复时，"命令没回应"是必然结果，不是新故障。
   * 这时候再叠一次恢复，会让 `bt_disable_mc()` 撞上还没 init 完的
   * `hdev->conn_ctx` —— 2026-09-18 n005 现场就是 `bt_conn_ref(&NULL->acl_conns[0])`
   * （DACCVIOL mmfar=0x124），从 `bt_conn_cleanup_all()` 进来的，而触发它的
   * 正是"adapter cycle 跑到一半，diag 又叠了一次 LCPU 强制复位"。
   * 软告警放行，等这轮恢复自己跑完。 */
  if (companion_ble_recovering())
    {
      static bool recovering_logged;

      if (!recovering_logged)
        {
          recovering_logged = true;
          syslog(LOG_WARNING,
                 "diag: ble cmd silent while adapter recovering — "
                 "no second recovery\n");
        }

      return true;
    }

  g_ble_soft = 0;
  return true;
}

static int diag_ble_restart(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
  if (board_ble_companion_busy())
    {
      return 0;
    }

  if (!board_ble_companion_pid_alive())
    {
      g_ble_soft = 0;
      syslog(LOG_ERR, "diag: ble companion thread respawn\n");
      return board_restart_ble_companion();
    }
#endif

#if defined(CONFIG_MYVENDOR_BLE_COMPANION) || defined(CONFIG_MYVENDOR_BICYCLE)
  /* 心跳已停：companion 堵在不可中断 HCI 等待上。只扳 LCPU 并清
   * skip_sync 会让它继续卡死，task_delete 也杀不掉，随后 skip respawn。 */
  if (!companion_bridge_thread_ok(DIAG_BLE_THREAD_MS))
    {
      g_ble_soft = 0;
      syslog(LOG_ERR, "diag: ble companion restart (thread stalled)\n");
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
      return board_restart_ble_companion();
#else
      return sf32lb52_bt_controller_force_reset();
#endif
    }

  if (g_ble_soft == 0)
    {
      g_ble_soft = 1;
      if (companion_bridge_alive_get())
        {
          syslog(LOG_WARNING, "diag: ble request companion adapter cycle\n");
          companion_bridge_diag_cycle_post();
          return 0;
        }
    }
#endif

  /* 恢复中就别再叠一次：`bt_disable_mc()` 撞上还没 init 完的 conn_ctx 会
   * DACCVIOL（n005）。让这一轮跑完，diag 有 cooldown，之后还会再判。 */
  if (companion_ble_recovering())
    {
      static bool defer_logged;

      if (!defer_logged)
        {
          defer_logged = true;
          syslog(LOG_WARNING,
                 "diag: ble restart deferred (adapter cycle in flight)\n");
        }

      return 0;
    }

  syslog(LOG_ERR, "diag: ble force LCPU reset\n");
  BLE_LOG("diag force LCPU skip=%d rxstall=%d",
          sf32lb52_bt_hci_skip_sync() ? 1 : 0,
          sf32lb52_bt_hci_rx_stalled(DIAG_BLE_STALL_MS) ? 1 : 0);
  return sf32lb52_bt_controller_force_reset();
}

static const myvendor_diag_ops_t g_ble_ops =
{
  .name = "ble",
  .ok = diag_ble_ok,
  .restart = diag_ble_restart,
  .start = NULL,
  .stop = NULL,
  .fail_ms = 4000u,
  .cooldown_ms = 20000u,
};
#endif

#ifdef CONFIG_BOARD_L96_GNSS
static bool diag_gnss_ok(void)
{
  return myvendor_gnss_diag_ok();
}

static int diag_gnss_restart(void)
{
  syslog(LOG_WARNING, "diag: gnss restart\n");
  return myvendor_gnss_diag_restart();
}

static const myvendor_diag_ops_t g_gnss_ops =
{
  .name = "gnss",
  .ok = diag_gnss_ok,
  .restart = diag_gnss_restart,
  .start = NULL,
  .stop = NULL,
  .fail_ms = 12000u,
  .cooldown_ms = 30000u,
};
#endif

#ifdef CONFIG_MYVENDOR_DVFS
/**
 * @brief 调频 worker 心跳是否正常。
 *
 * @details 整个调频体系（ondemand / charge hold / hop）都挂在 gov_worker 这
 *          一个自我重挂的 work 上，它一旦停摆，调频会永久静默失效 —— 现场表现
 *          就是"跑久了 DVFS 不调频了"（约 2000 秒量级）。判据用心跳年龄：
 *          正常只该是几十毫秒，远小于这里的阈值。
 */
static bool diag_dvfs_ok(void)
{
  uint32_t age_ms = 0;

  sf32lb_dvfs_gov_stat(&age_ms, NULL);
  return age_ms <= SF32LB_DVFS_GOV_STALE_MS;
}

static int diag_dvfs_restart(void)
{
  int ret = sf32lb_dvfs_gov_kick();

  if (ret < 0)
    {
      syslog(LOG_ERR, "diag: dvfs gov kick failed %d\n", ret);
    }

  return ret;
}

static const myvendor_diag_ops_t g_dvfs_ops =
{
  .name = "dvfs",
  .ok = diag_dvfs_ok,
  .restart = diag_dvfs_restart,
  .start = NULL,
  .stop = NULL,
  .fail_ms = 2000u,     /* 心跳 250ms，2 秒没动就是真停了，不用等太久 */
  .cooldown_ms = 10000u,
};
#endif

#ifdef CONFIG_MTD
/* sf32lb_sd_fs_ok / _fs_heal 来自 chips/sf32lb52/include/sf32lb_sdio.h（已在上方
 * include）。本文件属于 board 目标，芯片层头文件在它的 include 路径里 ——
 * 注意 **ctl / sys 不是**（它们是 nuttx_add_application，只有 board/include），
 * 那两个文件里的本地 extern 是必需的，别照抄成 include。
 *
 * 两个函数的完整契约（FAT 当"卡还活着"的独立证据、为什么不能直接重调
 * sf32lb_sd_mount_kv、为什么不能在持总线锁时调用）见头文件里的 Doxygen。 */

/**
 * @brief FS 槽的 `ok()`：两个 LFS 挂载点是否还可用。
 *
 * @details 判据在 sf32lb_sd_fs_ok() 里（卡能不能读用 FAT 探、LFS 能不能用常态用
 *          stat 探、stat 挂了才升到 statfs 探）。这里只是转调，让 diag 的注册表能
 *          按统一签名调用。
 *
 *          **别把这里的探针换成 statfs**：本槽是 DIAG_TICK_MS(500ms) 级的
 *          心跳，而 `littlefs_statfs()` 要遍历整棵元数据树且不缓存，两个挂载点
 *          一判就是每 tick ~110 ms 的纯 CPU（实测 diag 占 22.3%）。
 *
 *          卡判死期间一律算"健康" —— 那是 SD 层的事，由判死 + 硬复位负责。
 *          本槽只管"卡回来了但 FS 还是坏的"这一种：此时 LittleFS 的 RAM 内
 *          状态不会自愈，必须重挂（就是下面的 restart）。
 */
static bool diag_fs_ok(void)
{
  return sf32lb_sd_fs_ok();
}

/**
 * @brief FS 槽的 `restart()`：重挂坏掉的 LFS（内部已用 FAT 探过卡）。
 *
 * @note 可能阻塞（mount 要走 SD I/O，且卡不好时一个块 2~4 s），这是 diag 槽
 *       的已知代价，与 GNSS 断电约 1.5 s 同类。
 */
static int diag_fs_restart(void)
{
  int ret = sf32lb_sd_fs_heal();

  if (ret < 0 && ret != -EAGAIN)
    {
      syslog(LOG_ERR, "diag: fs remount failed %d\n", ret);
    }

  return ret;
}

static const myvendor_diag_ops_t g_fs_ops =
{
  .name = "fs",
  .ok = diag_fs_ok,
  .restart = diag_fs_restart,
  .start = NULL,
  .stop = NULL,
  .fail_ms = 3000u,
  .cooldown_ms = 15000u,
};
#endif

int myvendor_diag_register(const myvendor_diag_ops_t *ops)
{
  uint8_t i;

  if (ops == NULL || ops->name == NULL || ops->ok == NULL ||
      ops->restart == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < g_n; i++)
    {
      if (strcmp(g_slot[i].ops.name, ops->name) == 0)
        {
          g_slot[i].ops = *ops;
          pthread_mutex_unlock(&g_lock);
          return 0;
        }
    }

  if (g_n >= DIAG_MAX)
    {
      pthread_mutex_unlock(&g_lock);
      return -ENOMEM;
    }

  memset(&g_slot[g_n], 0, sizeof(g_slot[g_n]));
  g_slot[g_n].ops = *ops;
  g_n++;
  pthread_mutex_unlock(&g_lock);
  syslog(LOG_INFO, "diag: register %s fail=%u cooldown=%u\n",
         ops->name, (unsigned)ops->fail_ms, (unsigned)ops->cooldown_ms);
  return 0;
}

/* ---------------------------------------------------------------------------
 * 诊断时基（watch）：见 include/myvendor_diag.h 的用法与踩坑说明。
 *
 * 与上面那套 ops/slot 的分工：slot 管"不健康就重启"，watch 管"**在它还活着、
 * 只是卡在某个等待里**的时候把它踢一下"。前者代价是重启 + 断档，后者只发一个
 * 信号，所以 watch 的阈值可以取得比 fail_ms 小得多（默认 1.5 s vs 8~12 s）。
 * ------------------------------------------------------------------------- */

#define DIAG_WATCH_MAX      4u      /**< 现在只有 GNSS 读线程一个用户，留余量给其它线程。 */
#define DIAG_WATCH_LOG_MS   5000u   /**< 踢的日志节流：同一个"没进展"窗口最多 5 s 一条。 */
/** hold 的租约上限：客户端说"我在忙"最长只信这么久，到期按"可能卡死"处理。 */
#define DIAG_WATCH_HOLD_MAX_MS 10000u

struct diag_watch
{
  const char *name;
  pthread_t   tid;
  int         sig;
  uint32_t    stall_ms;
  uint32_t  (*progress_ms)(void);
  uint32_t    seen_ms;     /**< 上次观察到的进度时间戳。 */
  uint32_t    kicked_ms;   /**< 本轮已踢过、等它动（0 = 本轮还没踢）。 */
  uint32_t    hold_until_ms; /**< hold 的租约到期时刻（0 = 没 hold）。 */
  uint32_t    log_ms;      /**< 日志节流。 */
  uint16_t    kicks;
  bool        used;
  bool        hold;        /**< 客户端声明"我在忙，别踢"。 */
};

static struct diag_watch g_watch[DIAG_WATCH_MAX];

/** @brief 找已经注册的条目。**调用方必须持有 g_lock**。 */
static int diag_watch_find(pthread_t tid)
{
  uint8_t i;

  for (i = 0; i < DIAG_WATCH_MAX; i++)
    {
      if (g_watch[i].used && pthread_equal(g_watch[i].tid, tid))
        {
          return (int)i;
        }
    }

  return -1;
}

int myvendor_diag_watch(const char *name, pthread_t tid, int sig,
                        uint32_t stall_ms, uint32_t (*progress_ms)(void))
{
  int i;

  if (name == NULL || progress_ms == NULL)
    {
      return -EINVAL;
    }

  if (stall_ms == 0)
    {
      stall_ms = MYVENDOR_DIAG_WATCH_MS_DEFAULT;
    }

  pthread_mutex_lock(&g_lock);

  i = diag_watch_find(tid);
  if (i < 0)
    {
      for (i = 0; i < (int)DIAG_WATCH_MAX; i++)
        {
          if (!g_watch[i].used)
            {
              memset(&g_watch[i], 0, sizeof(g_watch[i]));
              g_watch[i].used = true;
              break;
            }
        }
    }

  if (i >= (int)DIAG_WATCH_MAX)
    {
      pthread_mutex_unlock(&g_lock);
      return -ENOSPC;
    }

  g_watch[i].name        = name;
  g_watch[i].tid         = tid;
  g_watch[i].sig         = sig;
  g_watch[i].stall_ms    = stall_ms;
  g_watch[i].progress_ms = progress_ms;
  g_watch[i].seen_ms     = progress_ms();   /* 注册即取一次，避免"刚注册就踢" */
  g_watch[i].kicked_ms   = 0;
  g_watch[i].log_ms      = 0;
  g_watch[i].hold        = false;

  pthread_mutex_unlock(&g_lock);

  syslog(LOG_INFO, "diag: watch %s on (stall=%u ms sig=%d)\n",
         name, (unsigned)stall_ms, sig);
  return 0;
}

void myvendor_diag_watch_hold(pthread_t tid, bool hold)
{
  int i;

  pthread_mutex_lock(&g_lock);
  i = diag_watch_find(tid);
  if (i >= 0)
    {
      g_watch[i].hold = hold;
      if (hold)
        {
          g_watch[i].kicked_ms = 0;   /* 忙完重新开始算 */
          /* **租约**：hold 只在对端"活着且能回来"时才有意义。2026-09-19 现场：
           * 读线程卡在被 hold 的那段里（waitdog 被剪掉之后）→ hold 永远放不回去
           * → 时基把自己关掉，恰好发生在最需要它的时候（日志里只有 watch on、
           * 一条 kick 都没有）。所以给 hold 一个上限：到期自动失效，
           * 宁可误踢一次长 sleep，也不能让"卡死"没人管。 */
          g_watch[i].hold_until_ms = diag_now() + DIAG_WATCH_HOLD_MAX_MS;
        }
      else
        {
          g_watch[i].hold_until_ms = 0;
        }
    }
  pthread_mutex_unlock(&g_lock);
}

void myvendor_diag_unwatch(pthread_t tid)
{
  int i;

  pthread_mutex_lock(&g_lock);
  i = diag_watch_find(tid);
  if (i >= 0)
    {
      syslog(LOG_INFO, "diag: watch %s off (kicks=%u)\n",
             g_watch[i].name, (unsigned)g_watch[i].kicks);
      memset(&g_watch[i], 0, sizeof(g_watch[i]));
    }
  pthread_mutex_unlock(&g_lock);
}

/**
 * @brief 每 tick 看一遍注册表：谁超过 stall_ms 没动就发信号踢一下。
 *
 * 纪律与 `diag_tick_one()` 一致：**外部代码（进度回调、pthread_kill、syslog）
 * 一律在锁外调**，锁里只做快照/写回。这样即便某个客户端在注册表里留了脏状态，
 * diag 自己也不会被堵住 —— diag 停摆等于整块自愈能力停摆。
 */
static void diag_watch_tick(uint32_t now)
{
  uint8_t i;

  for (i = 0; i < DIAG_WATCH_MAX; i++)
    {
      struct diag_watch w;
      uint32_t p;
      uint32_t age;
      bool moved;
      bool kick;
      bool shout = false;

      pthread_mutex_lock(&g_lock);
      if (!g_watch[i].used)
        {
          pthread_mutex_unlock(&g_lock);
          continue;
        }
      w = g_watch[i];
      pthread_mutex_unlock(&g_lock);

      p     = w.progress_ms();
      moved = (p != w.seen_ms);
      age   = diag_elapsed(now, p);

      /* 同一窗口只踢一次：踢完等它动（`moved` 会把 kicked_ms 清掉），
       * 没动就到下一个窗口再踢 —— 既不放过卡死，也不至于每 500 ms 一发。 */
      /* hold 是**租约**：到点自动失效（见 myvendor_diag_watch_hold 的说明）。 */
      bool held = (w.hold && w.hold_until_ms != 0 &&
                   (int32_t)(w.hold_until_ms - now) > 0);

      if (w.hold && !held && w.hold_until_ms != 0)
        {
          /* 只报一次：下一次 tick 起 hold_until_ms 保持过期，不会再进来。 */
          syslog(LOG_WARNING,
                 "diag: watch %s hold expired (>%u ms), watching again\n",
                 w.name, (unsigned)DIAG_WATCH_HOLD_MAX_MS);
          pthread_mutex_lock(&g_lock);
          if (g_watch[i].used && pthread_equal(g_watch[i].tid, w.tid))
            {
              g_watch[i].hold_until_ms = 0;
            }
          pthread_mutex_unlock(&g_lock);
        }

      kick = (!moved && !held && age >= w.stall_ms &&
              (w.kicked_ms == 0 ||
               diag_elapsed(now, w.kicked_ms) >= w.stall_ms));

      pthread_mutex_lock(&g_lock);
      if (g_watch[i].used && pthread_equal(g_watch[i].tid, w.tid))
        {
          g_watch[i].seen_ms = p;
          if (moved)
            {
              g_watch[i].kicked_ms = 0;
            }

          if (kick)
            {
              g_watch[i].kicked_ms = now;
              g_watch[i].kicks++;
              if (w.log_ms == 0 ||
                  diag_elapsed(now, w.log_ms) >= DIAG_WATCH_LOG_MS)
                {
                  g_watch[i].log_ms = now;
                  shout = true;
                }
            }
        }
      pthread_mutex_unlock(&g_lock);

      if (kick)
        {
          int rc = pthread_kill(w.tid, w.sig);

          if (shout || rc != 0)
            {
              syslog(LOG_WARNING,
                     "diag: kick %s (no progress %u ms, kicks=%u) rc=%d\n",
                     w.name, (unsigned)age, (unsigned)(w.kicks + 1u), rc);
            }
        }
    }
}

static void diag_tick_one(struct diag_slot *s, uint32_t now)
{
  struct myvendor_diag_ops_s ops;
  bool ok;
  uint32_t fail_ms;
  uint32_t cooldown;
  uint32_t unhealthy_ms;
  uint16_t restarts;
  bool do_restart;

  /* 注册可以**运行期**发生（头文件明确允许），所以对 slot 的读改写都要在同一
   * 把锁下 —— 以前只有注册侧持锁、tick 侧裸写，等于没锁（最坏读到被替换到
   * 一半的 ops.name/ops.ok）。
   *
   * `ok()` / `restart()` 是外部代码（可能做盘 I/O、可能几秒），**必须在锁外
   * 调**，否则一次 restart 会把运行期注册一起堵住。所以分四段：
   * 锁内取 ops → 锁外问 ok → 锁内跑状态机（并记下要打什么）→ 锁外打日志 + restart。 */
  pthread_mutex_lock(&g_lock);
  ops = s->ops;
  pthread_mutex_unlock(&g_lock);

  if (ops.ok == NULL)
    {
      return;
    }

  ok = ops.ok();

  pthread_mutex_lock(&g_lock);

  if (ok)
    {
      s->fail_since_ms = 0;
      pthread_mutex_unlock(&g_lock);
      return;
    }

  if (s->fail_since_ms == 0)
    {
      s->fail_since_ms = now;
      pthread_mutex_unlock(&g_lock);
      syslog(LOG_WARNING, "diag: %s unhealthy\n", ops.name);
      return;
    }

  fail_ms = ops.fail_ms != 0 ? ops.fail_ms : 5000u;
  cooldown = ops.cooldown_ms != 0 ? ops.cooldown_ms : 15000u;
  if (diag_elapsed(now, s->fail_since_ms) < fail_ms)
    {
      pthread_mutex_unlock(&g_lock);
      return;
    }

  if (s->last_restart_ms != 0 &&
      diag_elapsed(now, s->last_restart_ms) < cooldown)
    {
      pthread_mutex_unlock(&g_lock);
      return;
    }

  unhealthy_ms = diag_elapsed(now, s->fail_since_ms);
  s->last_restart_ms = now;
  s->fail_since_ms = now;
  if (s->restarts < UINT16_MAX)
    {
      s->restarts++;
    }

  restarts = s->restarts;
  do_restart = (ops.restart != NULL);

  pthread_mutex_unlock(&g_lock);

  syslog(LOG_ERR, "diag: %s restart (unhealthy %u ms, n=%u)\n",
         ops.name, (unsigned)unhealthy_ms, (unsigned)restarts);

  if (do_restart)
    {
      (void)ops.restart();
    }
}

/* ---- 周期计数器表（板级，2026-09-24）-------------------------------------
 *
 * 一张表管"哪些模块的计数器要周期性打"，选择是一个位掩码：
 *   · **默认全开**（KV 里没有 `persist.diag.cnt_mask` ⇒ 0xffffffff）；
 *   · `ctl cnt` 改开关、改动落 KV（owner 是 devctl，同 log mask / dvfs charge 的取舍）；
 *   · diag 每 `DIAG_CNT_PERIOD_MS` 扫一遍表，把选中的模块**无条件**各打一行。
 *
 * 与"异常判定 + 自愈"分开：后者（`report(false)`）**每拍都跑、与开关无关** ——
 * 免得用户关掉报表就把自愈也关掉了。
 *
 * 打印一律在本线程（中断侧只累加计数），见各模块自己的说明。
 */
#define DIAG_CNT_PERIOD_MS 60000u

struct diag_cnt_mod
{
  const char *name;               /**< ctl 里用的名字。 */
  void      (*report)(bool force);/**< true = 无条件打一行计数器。 */
};

static uint32_t g_cnt_mask = 0xffffffffu;
static uint32_t g_cnt_last_ms;

static void diag_cnt_ble(bool force)
{
  extern void sf32lb52_bt_rx_chain_report(bool force);

  sf32lb52_bt_rx_chain_report(force);
}

static void diag_cnt_gnss(bool force)
{
  extern void myvendor_gnss_uart_cnt_report(void);
  extern void myvendor_gnss_uart_check_anomaly(void);
  extern void myvendor_gnss_link_cnt_report(void);

  if (force)
    {
      /* 周期报表（受 `ctl cnt` 开关管）：UART 侧 + **解析/链路侧**各一行 ——
       * 后者是"没收到 vs 收到没解析出来"的分叉判据（见其注释）。 */
      myvendor_gnss_uart_cnt_report();
      myvendor_gnss_link_cnt_report();
      return;
    }

  /* 每拍跑：异常直报（LOG_ERR，**与开关无关**）。 */
  myvendor_gnss_uart_check_anomaly();
}

static void diag_cnt_isrprint(bool force)
{
  extern uint32_t sifli_isr_console_chars(void);

  if (force)
    {
      syslog(LOG_INFO, "diag: cnt isrprint console-chars-in-isr=%lu\n",
             (unsigned long)sifli_isr_console_chars());
    }
}

/** @brief 手机通知的过滤计数（板级，2026-09-24）。
 *
 *  `merged` / `throttled` 增长 = **过滤在起作用**（常驻通知在刷）⇒ WARN 限频报；
 *  `evicted` 增长 = inbox 满**真丢了通知** ⇒ ERROR 直报。
 */
static void diag_cnt_notif(bool force)
{
  extern void companion_bridge_notif_stats(uint32_t *rx, uint32_t *merged,
                                           uint32_t *throttled,
                                           uint32_t *evicted);
  static uint32_t s_thr_seen;
  static uint32_t s_evict_seen;
  static uint32_t s_evict_log_ms;
  static uint32_t s_flt_log_ms;
  uint32_t rx = 0;
  uint32_t merged = 0;
  uint32_t throttled = 0;
  uint32_t evicted = 0;
  uint32_t now;

  companion_bridge_notif_stats(&rx, &merged, &throttled, &evicted);

  if (force)
    {
      syslog(LOG_INFO,
             "diag: cnt notif rx=%lu merged=%lu throttled=%lu evicted=%lu\n",
             (unsigned long)rx, (unsigned long)merged,
             (unsigned long)throttled, (unsigned long)evicted);
      s_thr_seen = throttled;
      s_evict_seen = evicted;
      return;
    }

  now = diag_now();

  /* 真丢通知：ERROR（关不掉）。 */
  if (evicted != s_evict_seen)
    {
      s_evict_seen = evicted;

      if (s_evict_log_ms == 0u ||
          (int32_t)(now - s_evict_log_ms) >= 5000)
        {
          s_evict_log_ms = now ? now : 1u;
          syslog(LOG_ERR,
                 "diag: ERROR notif inbox full, dropped %lu notification(s) "
                 "(total %lu) — 通知比 UI 快\n",
                 (unsigned long)evicted, (unsigned long)evicted);
        }
    }

  /* 过滤在工作（常驻通知刷屏被合并/限流）：WARN，30 s 限频。 */
  if (throttled != s_thr_seen || merged != 0u)
    {
      if (s_flt_log_ms == 0u || (int32_t)(now - s_flt_log_ms) >= 30000)
        {
          s_flt_log_ms = now ? now : 1u;
          s_thr_seen = throttled;
          syslog(LOG_WARNING,
                 "diag: notif filter active: merged=%lu throttled=%lu rx=%lu "
                 "(常驻通知只更新、不重弹)\n",
                 (unsigned long)merged, (unsigned long)throttled,
                 (unsigned long)rx);
        }
    }
}

static const struct diag_cnt_mod g_cnt_mods[] =
{
  { "ble",      diag_cnt_ble },
  { "gnss",     diag_cnt_gnss },
  { "notif",    diag_cnt_notif },
  { "isrprint", diag_cnt_isrprint },
};

#define DIAG_CNT_MODS (sizeof(g_cnt_mods) / sizeof(g_cnt_mods[0]))

static void diag_cnt_dump_enabled(void)
{
  unsigned i;

  for (i = 0; i < DIAG_CNT_MODS; i++)
    {
      if (((g_cnt_mask >> i) & 1u) != 0u)
        {
          g_cnt_mods[i].report(true);
        }
    }
}

/** @brief 每拍跑一遍所有模块的**异常判定**（`report(false)`）：**与开关无关**。
 *
 *  约定（各模块的 `report(bool)` 都按这条实现）：
 *    · `report(true)`  = 周期报表，受 `ctl cnt` 掩码管，打 WARN 级计数行；
 *    · `report(false)` = 异常判定，**每拍都跑、掩码关不掉**，真异常一律 `LOG_ERR`
 *      直报（这样 `ctl log err` 只剩错误级时也看得见）。
 */
static void diag_cnt_check_all(void)
{
  unsigned i;

  for (i = 0; i < DIAG_CNT_MODS; i++)
    {
      g_cnt_mods[i].report(false);
    }
}

/** @brief 供 ctl 读/改开关（位序同 {@link g_cnt_mods}）。 */
uint32_t myvendor_diag_cnt_mask_get(void)
{
  return g_cnt_mask;
}

void myvendor_diag_cnt_mask_set(uint32_t mask)
{
  g_cnt_mask = mask;
}

/** @brief 立即打一遍（`ctl cnt now`）：不用等下一个周期。 */
void myvendor_diag_cnt_dump_now(void)
{
  diag_cnt_dump_enabled();
}

/** @brief 按名字找模块下标；找不到返回 -1（`ctl cnt <name> on|off` 用）。 */
int myvendor_diag_cnt_find(const char *name)
{
  unsigned i;

  if (name == NULL)
    {
      return -1;
    }

  for (i = 0; i < DIAG_CNT_MODS; i++)
    {
      if (strcmp(g_cnt_mods[i].name, name) == 0)
        {
          return (int)i;
        }
    }

  return -1;
}

/** @brief 打印模块清单（`ctl cnt` 的表头；由 ctl 拼行）。 */
const char *myvendor_diag_cnt_name(unsigned idx)
{
  return (idx < DIAG_CNT_MODS) ? g_cnt_mods[idx].name : NULL;
}

unsigned myvendor_diag_cnt_count(void)
{
  return (unsigned)DIAG_CNT_MODS;
}

static void *diag_thread(void *arg)
{
  (void)arg;
  g_boot_ms = diag_now();
  syslog(LOG_INFO, "diag: thread up, grace %u ms\n",
         (unsigned)DIAG_GRACE_MS);

  for (;;)
    {
      uint32_t now = diag_now();
      uint8_t i;
      uint8_t n;

      if (diag_elapsed(now, g_boot_ms) >= DIAG_GRACE_MS)
        {
          pthread_mutex_lock(&g_lock);
          n = g_n;
          pthread_mutex_unlock(&g_lock);
          for (i = 0; i < n && i < DIAG_MAX; i++)
            {
              diag_tick_one(&g_slot[i], now);
            }
        }

      /* 诊断时基：卡在等待里的线程，先踢一脚把它叫回来（比 restart 便宜得多，
       * 所以**不受 GRACE 门控** —— 开机头 25 秒也可能需要它）。 */
      diag_watch_tick(now);

      /* companion 主动求助：适配器 cycle 与 LCPU 复位都救不回来 ⇒ 由**本线程**
       * 执行任务重建（companion 自己调等于自杀）。限频在 companion 侧（10 分钟）。 */
      if (companion_bridge_diag_restart_take())
        {
          syslog(LOG_WARNING,
                 "diag: ble task restart requested by companion\n");
          (void)board_restart_ble_companion();
        }

      myvendor_schedmon_tick();

      /* 日志落盘：**节流**到 2 s 一次，或环内积压超过高水位时提前刷。
       *
       * 为什么不再每 tick 都刷：`myvendor_diaglog_flush()` 每次落盘都要在
       * `/mnt/kv`（SD 上的 LittleFS）上 open + write + close，而 SD 的块等待
       * 是 PIO **忙等**（见 chips/sf32lb52/sf32lb_sdio.c 的
       * `sd1_wait_write()` / `sd1_wait_card_idle()` / `sd1_wait_dat0()`），
       * 那段时间全部记在 diag 线程头上 —— 实测 diag 占 18.2% CPU 主要就是
       * 这条路径。2 s 一次把稳态 I/O 砍到 1/4；高水位那一路保证刷屏期不丢
       * （环只有 2 KB，等满 2 s 会溢出）。
       *
       * 位置刻意放在**主循环体**里、不在上面那个被 GRACE 门控的槽位遍历内 ——
       * 否则开机头 25 秒的日志不落盘，而开机那段正是 SD/LFS 挂载信息最密集、
       * 最值钱的地方。
       *
       * 也刻意**只在 diag 线程**刷：出错点那边（比如打完 SD 判死日志）往往正
       * 持着 g_sd_lock，在那里往文件系统写会自锁。 */
      {
        extern int myvendor_diaglog_flush(void);
        extern size_t myvendor_diaglog_pending(void);

        if (g_flush_ms == 0 ||
            diag_elapsed(now, g_flush_ms) >= DIAGLOG_FLUSH_MS ||
            myvendor_diaglog_pending() >= DIAGLOG_FLUSH_HIWATER)
          {
            (void)myvendor_diaglog_flush();
            g_flush_ms = now;
          }
      }

      /** 每拍：**看门狗活动链表的只读巡检**（`wd_start.c` 里的 `myvendor_wd_audit`）。
       *
       *  目的只有一个：**抓到"链表变坏的那一拍"**。clip 要等到有人 `wd_start`
       *  才被发现，而 2026-09-27 一整天才两次 ⇒ 靠 clip 反推凶手太慢；巡检只在
       *  "好↔坏"翻转时打一行，正常静默（关中断、有界、只读，不改行为）。 */
      {
        extern void myvendor_wd_audit(void);

        myvendor_wd_audit();
      }

      /** 每拍：**日志文件的运行期清理**（两边各自限频，默认 10 分钟）。
       *
       *  为什么要有这一条（2026-09-25 用户反馈）：`/mnt/kv/diag` 与 `/mnt/kv/coredump`
       *  原先只在**开机**（coredump 另加"写新 dump 时"、diag 另加"按天滚动"）裁剪 ⇒
       *  只要设备不重启、不跨天就只涨不落；而旧 diag 侧删除失败还是**静默**的，
       *  堆到撑满 /mnt/kv 都没人知道。现在两条都按"最小序号/最老戳"删到 KEEP=10，
       *  失败一律 WARNING（`prune stuck` / `prune unlink … failed`）。 */
      (void)myvendor_diaglog_prune();
      myvendor_coredump_prune();

      /** 每拍：**所有模块的异常判定**（`report(false)`）—— 与 `ctl cnt` 开关无关，
       *  真异常一律 LOG_ERR 直报；周期报表（`report(true)`）才受掩码管。 */
      {
        extern uint32_t sifli_isr_console_chars(void);
        static uint32_t s_isr_chars_last;
        static uint32_t s_isr_chars_ms;
        uint32_t isr_chars;

        diag_cnt_check_all();

        /* 哨兵：**中断里走轮询式控制台输出**（`arm_lowputc()`，即 `up_putc()` 那条路）。
         *
         *  只算这一条路，**不算**驱动逐字节写硬件（`sifli_send()`）—— 后者由
         *  NuttX 的发送完成中断调用，属设计如此（现场 delta=9 的误报已更正）。
         *  例外：fault/panic 路径本来就会在中断里 `up_putc`，那是崩溃取证、不算病。
         *  限频 5 s，且报的是这段窗口里的**增量**；用 ERROR 级（关不掉）。
         *
         *  **点名**：`sifli_isrprint_take()` 交出这次窗口的冒犯者快照 —— IPSR（哪个
         *  中断/异常）、CFSR/HFSR（是否故障路径）、以及**输出正文开头**（模块前缀就在
         *  里面，例如 `dump_assert_info:` / `gnss:` / `diag:`）。正文做单行化，免得一条
         *  ERROR 撑成多行、破坏"真错误只认 `[ ERROR]` 标签行"的口径。 */
        isr_chars = sifli_isr_console_chars();
        if (isr_chars != s_isr_chars_last &&
            (int32_t)(now - s_isr_chars_ms) >= 5000)
          {
            extern void sifli_isrprint_take(uint32_t *ipsr, uint32_t *cfsr,
                                            uint32_t *hfsr, char *text,
                                            size_t n);
            uint32_t ipsr = 0;
            uint32_t cfsr = 0;
            uint32_t hfsr = 0;
            char raw[64];
            char text[64];
            char irq[24];
            const char *who;
            size_t i;

            sifli_isrprint_take(&ipsr, &cfsr, &hfsr, raw, sizeof(raw));

            for (i = 0; raw[i] != '\0' && i < sizeof(text) - 1u; i++)
              {
                unsigned char c = (unsigned char)raw[i];
                text[i] = (c >= 0x20u && c < 0x7fu) ? (char)c : '.';
              }

            text[i] = '\0';

            switch (ipsr)
              {
              case 0u:  who = "thread/panic"; break;
              case 2u:  who = "NMI";          break;
              case 3u:  who = "HardFault";    break;
              case 4u:  who = "MemManage";    break;
              case 5u:  who = "BusFault";     break;
              case 6u:  who = "UsageFault";   break;
              case 11u: who = "SVCall";       break;
              case 14u: who = "PendSV";       break;
              case 15u: who = "SysTick";      break;
              default:  who = NULL;           break;   /* >=16 外部中断 */
              }

            if (who != NULL)
              {
                (void)snprintf(irq, sizeof(irq), "%lu(%s)",
                               (unsigned long)ipsr, who);
              }
            else if (ipsr >= 16u)
              {
                (void)snprintf(irq, sizeof(irq), "%lu(IRQ%lu)",
                               (unsigned long)ipsr,
                               (unsigned long)(ipsr - 16u));
              }
            else
              {
                /* 保留号 / 未命名核内异常：直接报号，别做减法（会无符号下溢）。 */
                (void)snprintf(irq, sizeof(irq), "%lu", (unsigned long)ipsr);
              }

            syslog(LOG_ERR,
                   "diag: ERROR polled-console chars written in ISR: %lu (+%lu in %u ms; fault/panic 除外) irq=%s cfsr=%08lx hfsr=%08lx text=\"%s\"\n",
                   (unsigned long)isr_chars,
                   (unsigned long)(isr_chars - s_isr_chars_last),
                   (unsigned)(now - s_isr_chars_ms),
                   irq,
                   (unsigned long)cfsr,
                   (unsigned long)hfsr,
                   text);

            s_isr_chars_last = isr_chars;
            s_isr_chars_ms = now;
          }

        /* 周期报表：按掩码把选中的模块各打一行计数器（默认全开）。 */
        if ((int32_t)(now - g_cnt_last_ms) >= (int32_t)DIAG_CNT_PERIOD_MS)
          {
            g_cnt_last_ms = now ? now : 1u;
            diag_cnt_dump_enabled();
          }
      }

      usleep(DIAG_TICK_MS * 1000u);
    }

  return NULL;
}

int myvendor_diag_start(void)
{
  pthread_attr_t attr;
  struct sched_param sp;
  pthread_t th;
  int ret;

  if (g_started)
    {
      return 0;
    }

#ifdef CONFIG_BOARD_L96_GNSS
  (void)myvendor_diag_register(&g_gnss_ops);
#endif
#ifdef CONFIG_MYVENDOR_DVFS
  (void)myvendor_diag_register(&g_dvfs_ops);
#endif
#ifdef CONFIG_MTD
  (void)myvendor_diag_register(&g_fs_ops);
#endif
#ifdef CONFIG_BLUETOOTH
  (void)myvendor_diag_register(&g_ble_ops);
#endif

  pthread_attr_init(&attr);
  ret = pthread_attr_setstack(&attr, g_diag_stack, sizeof(g_diag_stack));
  sp.sched_priority = DIAG_PRIORITY;
#ifdef PTHREAD_EXPLICIT_SCHED
  (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
#endif
  (void)pthread_attr_setschedparam(&attr, &sp);
  if (ret == 0)
    {
      ret = pthread_create(&th, &attr, diag_thread, NULL);
    }

  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      syslog(LOG_ERR, "diag: thread create failed %d\n", ret);
      return -ret;
    }

#ifndef CONFIG_DISABLE_PTHREAD
  pthread_setname_np(th, "diag");
#endif
  pthread_detach(th);
  g_started = true;
  syslog(LOG_INFO, "diag: started prio=%d\n", DIAG_PRIORITY);
  return 0;
}
