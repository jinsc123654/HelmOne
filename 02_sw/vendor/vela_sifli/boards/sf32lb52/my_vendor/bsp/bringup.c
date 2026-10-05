/**
 * @file bringup.c
 * @brief 板级晚初始化：按 g_bringup[] 顺序执行各 bringup_* 步骤。
 *
 * 开机顺序以 g_bringup[] 为准，改表即可，勿改 runner。每步为 int (*)(void)：
 * 0 成功，&lt;0 失败；abort_on_fail 为 true 时失败则中止后续步骤。
 *
 * ★ 动手改任何一步之前先读 g_bringup[] 上面的 **@section bootreset**：
 *   "2SFBL 用过的外设，App 侧必须先 `HAL_RCC_ResetModule()` 再配置" ——
 *   它们的 Init 多是读-改-写，纠正不回 bootloader 留下的状态。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <nuttx/wdog.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/timers/timer.h>
#ifdef CONFIG_SCHED_LPWORK
#  include <nuttx/wqueue.h>
#endif

#if defined(CONFIG_RTC) && defined(CONFIG_RTC_DRIVER)
#  include <nuttx/timers/rtc.h>
#  include "sf32lb_rtc.h"
#endif

#if defined(CONFIG_MYVENDOR_MTP_SIMPLE) || defined(CONFIG_LCD) || \
    defined(CONFIG_MYVENDOR_BICYCLE_AUTOSTART) || \
    defined(CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART)
#  include <spawn.h>
#endif

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
#  include "myvendor_mtp.h"
#endif

#include "myvendor_devctl.h"
#include "myvendor_fw_slot.h"
#include "myvendor_gpx.h"
#include "myvendor_identity.h"
#include "myvendor_ble_log.h"
#include "myvendor_sound.h"

#ifdef CONFIG_LCD
#  include <nuttx/sched.h>
extern int board_lcd_initialize(void);
#endif

#if defined(CONFIG_SPI) && defined(CONFIG_BSP_USING_SPI1)
#  include <nuttx/spi/spi.h>
#  include "sf32lb_spi.h"
#  ifdef CONFIG_SPI_DRIVER
#    include <nuttx/spi/spi_transfer.h>
#  endif
#endif

#if defined(CONFIG_SPI) && defined(CONFIG_MMCSD_SPI) && \
    defined(CONFIG_BSP_USING_SPI1)
#  include <nuttx/mmcsd.h>
#endif

#ifdef CONFIG_ADC
#  include "sf32lb_adc.h"
#endif

#if defined(CONFIG_BOARD_BMI270) || defined(CONFIG_BOARD_BMP388) || \
    defined(CONFIG_BOARD_MMC5983MA) || defined(CONFIG_ADC)
#  include "myvendor_board_sensor.h"
#endif

#include "myvendor_usbdev.h"
#include "board_malloc.h"
#include "sf32lb_dvfs.h"

#ifdef CONFIG_MTD
extern int sf32lb_nand_automount(int minor, uint32_t byte_offset,
                                 uint32_t byte_size);
extern int sf32lb_nand_mount_littlefs(int minor);
extern int sf32lb_sd_automount(int minor, uint32_t byte_offset,
                               uint32_t byte_size);
extern int sf32lb_sd_mount_littlefs(int minor);
extern int sf32lb_sd_mount_kv(uint32_t byte_offset, uint32_t byte_size);
extern int sf32lb_sd_mount_fat(uint32_t byte_offset, uint32_t byte_size);
extern void sf32lb_sd_reclock(void);
#endif
#ifdef CONFIG_LCD_USING_NV3031A
extern void sf32lb_lcd_reclock(void);
#endif

#ifdef CONFIG_WATCHDOG
extern void sf32lb_iwdginitialize(const char *devpath);
#endif

#include "myvendor_watchdog.h"

#ifdef CONFIG_MYVENDOR_COREDUMP
#  include "myvendor_coredump.h"
#endif

#ifdef CONFIG_UART_BTH4
extern int sf32lb52_bt_initialize(void);
#endif

#ifdef CONFIG_TIMER
#  include "sf32lb_timer.h"
#endif

#ifdef CONFIG_PWM
#  include "sf32lb_pwm.h"
#endif

#ifdef CONFIG_I2C
#  include <nuttx/i2c/i2c_master.h>
#  include "sf32lb_i2c.h"
#endif

#ifdef CONFIG_BOARD_BMP388
#  include "bmp388.h"
#endif

#ifdef CONFIG_BOARD_MMC5983MA
#  include "mmc5983ma.h"
#endif

#ifdef CONFIG_BOARD_BMI270
#  include "sf32lb52_bmi270.h"
#endif


#ifdef CONFIG_BOARD_L96_GNSS
#  include "sf32lb52_l96.h"
#  include "myvendor_gnss.h"
#endif

#include "myvendor_diag.h"

#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
#  include "companion_bridge.h"
#endif

#include "arm_internal.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "eta9184.h"
#include "myvendor_cpuload.h"
#include "ptab.h"
#include "ptab_table.h"
#include "sf32lb52_devkit_lcd.h"
#include "sifli_gpio.h"

#if defined(CONFIG_SPI) && defined(CONFIG_BSP_USING_SPI1)
#  define SF32LB52_SPI1_PORT           0
#endif

#if defined(CONFIG_SPI) && defined(CONFIG_MMCSD_SPI) && \
    defined(CONFIG_BSP_USING_SPI1)
#  define SF32LB52_TFCARD_SPI_PORT     0
#  define SF32LB52_TFCARD_SLOT         0
#  define SF32LB52_TFCARD_MINOR        0
#  define SF32LB52_TFCARD_MOUNTPOINT   "/data/tf"
#endif

#define BRINGUP_LOG(fmt, ...) \
  syslog(LOG_INFO, "BRINGUP: " fmt, ##__VA_ARGS__) /**< bringup 步骤 syslog 前缀。 */

/** @brief bringup 步骤函数类型：0 成功，负值失败。 */
typedef int (*bringup_fn_t)(void);

/** @brief g_bringup[] 中单步描述。 */
struct bringup_step_s
{
  const char *name;     /**< 步骤名（日志用）。 */
  bringup_fn_t fn;      /**< 步骤函数。 */
  bool abort_on_fail;   /**< true 时失败则中止后续步骤。 */
};

/** 存储 automount/LFS 失败时为 false；hclk240 步骤会跳过提频。 */
static bool g_storage_ok = true;

/** @brief 创建目录，EEXIST 视为成功。 */
static int bringup_mkdir(const char *path)
{
  clock_t t0;
  uint32_t ms;
  int ret;
  int err = 0;

  syslog(LOG_INFO, "INFO: mkdir %s ...\n", path);
  t0 = clock_systime_ticks();
  myvendor_watchdog_busy_pump();
  ret = mkdir(path, 0755);
  if (ret < 0)
    {
      err = errno;
    }

  ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
  syslog(LOG_INFO, "INFO: mkdir %s ret=%d errno=%d %ums\n",
         path, ret, err, (unsigned)ms);

  if (ret < 0 && err != EEXIST)
    {
      serr("WARN: mkdir %s failed: %d\n", path, err);
      return -err;
    }

  return OK;
}

/** @brief 产品 MTP 卷下的 GPX 目录（路书 / 本机骑行）。 */
static void bringup_gpx_dirs(void)
{
  (void)bringup_mkdir(MYVENDOR_GPX_MTP_ROOT);
  (void)bringup_mkdir(MYVENDOR_GPX_IMPORT_DIR);
  (void)bringup_mkdir(MYVENDOR_GPX_RECORD_DIR);
  (void)bringup_mkdir(MYVENDOR_NAVPTS_DIR);
}

#if defined(CONFIG_SPI) && defined(CONFIG_BSP_USING_SPI1)
static struct spi_dev_s *g_sf32lb52_spi1; /**< SPI1 总线单例。 */

/** @brief 获取或初始化 SPI1 总线。 */
static FAR struct spi_dev_s *sf32lb52_spi1_getbus(void)
{
  if (g_sf32lb52_spi1 == NULL)
    {
      g_sf32lb52_spi1 = sifli_spibus_initialize(SF32LB52_SPI1_PORT);
    }

  return g_sf32lb52_spi1;
}

#ifdef CONFIG_SPI_DRIVER
/** @brief 注册 SPI1 字符设备 /dev/spi1。 */
static int sf32lb52_spi1_register_char(void)
{
  FAR struct spi_dev_s *spi;
  int ret;

  spi = sf32lb52_spi1_getbus();
  if (spi == NULL)
    {
      return -ENODEV;
    }

  ret = spi_register(spi, 1);
  if (ret < 0 && ret != -EEXIST)
    {
      return ret;
    }

  return OK;
}
#endif

#if defined(CONFIG_MMCSD_SPI)
/** @brief 挂载 TF 卡 vfat 到 /data/tf。 */
static int sf32lb52_tfcard_mount(void)
{
#ifndef CONFIG_DISABLE_MOUNTPOINT
  static const char *const devpaths[] =
    {
      "/dev/mmcsd0p0",
      "/dev/mmcsd0"
    };
  int ret = -ENODEV;
  size_t i;

  ret = bringup_mkdir(SF32LB52_TFCARD_MOUNTPOINT);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < sizeof(devpaths) / sizeof(devpaths[0]); i++)
    {
      ret = nx_mount(devpaths[i], SF32LB52_TFCARD_MOUNTPOINT, "vfat", 0,
                     NULL);
      if (ret >= 0 || ret == -EBUSY)
        {
          syslog(LOG_INFO, "INFO: TF mounted from %s to %s\n",
                 devpaths[i], SF32LB52_TFCARD_MOUNTPOINT);
          return OK;
        }
    }

  return ret;
#else
  return -ENOSYS;
#endif
}

/** @brief 初始化 SPI TF 卡槽并尝试挂载。 */
static int sf32lb52_tfcard_initialize(void)
{
  FAR struct spi_dev_s *spi;
  int ret;

  spi = sf32lb52_spi1_getbus();
  if (spi == NULL)
    {
      serr("ERROR: sifli_spibus_initialize(%d) failed\n",
           SF32LB52_TFCARD_SPI_PORT);
      return -ENODEV;
    }

  ret = mmcsd_spislotinitialize(SF32LB52_TFCARD_MINOR,
                                SF32LB52_TFCARD_SLOT,
                                spi);
  if (ret < 0)
    {
      serr("ERROR: mmcsd_spislotinitialize failed: %d\n", ret);
      return ret;
    }

  ret = sf32lb52_tfcard_mount();
  if (ret < 0)
    {
      serr("WARN: TF mount failed: %d\n", ret);
    }

  return ret;
}
#endif
#endif

#ifdef CONFIG_MYVENDOR_BICYCLE_AUTOSTART
extern int bicycle_main(int argc, FAR char *argv[]);
#endif

#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
extern int ble_companion_main(int argc, FAR char *argv[]);
#ifdef CONFIG_BLUETOOTH
int sf32lb52_bt_controller_force_reset(void);
void sf32lb52_bt_hci_skip_sync_set(bool on);
bool sf32lb52_bt_hci_skip_sync(void);
void sf32lb52_bt_hci_dump_stall(void);
#endif
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
void companion_bridge_teardown_skip_hci_set(bool on);
#endif

static uint8_t g_ble_companion_stack[CONFIG_MYVENDOR_BLE_COMPANION_STACKSIZE]
    __attribute__((aligned(16)));
static pid_t g_ble_companion_pid = (pid_t)-1;
static bool g_ble_companion_spawning;
static bool g_ble_companion_restarting;
static bool g_ble_start_requested;
static uint32_t g_ble_companion_stuck_n;  /**< 连续"收不回来"的次数。
                                           *  用 32 位：uint8 会在 256 轮（约 2 h）
                                           *  回绕到 0，让"试够 N 轮就放掉
                                           *  skip_sync"那条判据反复重新触发，
                                           *  日志里的 attempt 编号也跟着归零。 */
static bool g_ble_companion_reclaim_up;   /**< 回收任务已 spawn，别重复起。 */
static pid_t g_ble_companion_lost_pid = (pid_t)-1; /**< 被判"永远不会再被调度"、
                                           *  于是隔离掉的那具僵尸（只记账）。 */
static uint32_t g_ble_companion_lost_n;   /**< 隔离过几具；日志/遥测用，正常应为 0。 */
#ifdef CONFIG_SCHED_LPWORK
static struct work_s g_ble_deferred_work;
#endif

/** @brief 收不回来时的重试间隔；固定值，与 diag 的周期无关。 */
#define BLE_COMPANION_RETRY_MS       30000u

/** @brief 每 N 次重试打一行汇总，避免每 30 s 刷屏。 */
#define BLE_COMPANION_RETRY_LOG_N    4u

/** @brief 连续这么多轮都收不回来，就撤回 skip_sync（见下面失败分支）。 */
#define BLE_COMPANION_GIVEUP_SKIP_N  3u

/** @brief 回收任务栈（循环重试期间常驻，退出后不再占 CPU）。
 *
 *  **2048 → 8192（2026-09-23，转储 n586 实测）**：原来那 2048 **真的用满了**。
 *
 *  n586（旧固件 `202609231708`，18:50）是 `kind=assert` + `arm_hardfault.c` +
 *  `cfsr=0x00100000`（**UFSR.STKOF：硬件栈溢出**），任务名 `ble_reclaim`。
 *  同一份转储里的栈水位是决定性判据：
 *
 *      snap pid=57 ble_reclaim pri=100 st=6 wd=1 stk=1944/1944     ← 1944/1944，满
 *
 *  机制不是"这段代码吃栈"（`-fstack-usage` 实测本函数只有 24 B、
 *  `board_restart_ble_companion` 64 B），而是 **Cortex-M 的异常入栈压的是当前
 *  SP**：线程态用 PSP ⇒ 中断帧压在**任务自己的栈**上。栈本来就贴着 PSPLIM 时，
 *  一个定时器中断（GPT ISR 里还有 libgcc 的 64 位除法 `__aeabi_uldivmod`）
 *  就能把 SP 顶破 LIM ⇒ STKOF ⇒ HardFault ⇒ panic ⇒ 重启。
 *
 *  ⚠ 是"**用满**"而不是"偶尔擦边"：1944 是全部可用字节。所以这不是偶发，
 *  只是要等它跑到最深处、且那一刻正好来中断。
 *
 *  取值同 `ctl` 那次（4096→8192）的理由：PSRAM 有 4 MB，翻倍成本可忽略；
 *  而这个任务只在一路重试期间常驻，退出就不再占 CPU。 */
#define BLE_COMPANION_RECLAIM_STACK  8192

static uint8_t g_ble_companion_reclaim_stack[BLE_COMPANION_RECLAIM_STACK]
    __attribute__((aligned(16)));

static int ble_companion_reclaim_main(int argc, FAR char *argv[]);

static bool ble_companion_pid_alive(pid_t pid)
{
  if (pid <= 0)
    {
      return false;
    }

  return kill(pid, 0) == 0;
}

/* 判"这具僵尸**永远不会再被调度**"。
 *
 * 为什么需要单独判：上面的 `kill(pid, 0)` 对"活着但卡住"和"再也跑不起来"
 * 是同一个答案（都为真），而这两种在恢复策略上完全不同 —— 前者要等它自己
 * 退，后者等下去是白等。
 *
 * 现场依据（2026-09-23）：三连杀 + LCPU 复位连做 28 轮都没收回来，日志停在
 * `stuck … BLE down until it exits (check sys hr, or power cycle)`，也就是
 * **只能断电**；而那一次的 stall dump 里僵尸的栈只有一帧
 * `#0 nxtask_activate`（`task_activate.c:106`，就在 `nxsched_add_readytorun`
 * 那一段）⇒ 线程**建起来了但没被调度**，与 GNSS 读线程"被调度器丢掉"同一形状。
 *
 * 判据（与 myvendor_gnss.c 的 `gnss_reader_lost()` 同源，那次已实机验证）：
 *   状态是 RUNNING/READY + **没有 waitobj / waitdog / lockcount** +
 *   **不在 `this_task()` 起头的就绪链上** ⇒ 没有任何 waitlist 持有它，
 *   没有任何事件能把它挂回就绪链 ⇒ 它永远不会再运行，也就永远不会再碰 HCI。
 * 因此隔离它是安全的：TCB/栈留着（泄漏），但不会再与新 companion 抢控制器。
 *
 * 整段在 `sched_lock()` 里做：既拿到稳定的链快照，也保证"判完"到调用方
 * 动手之间没人把它挂回去。 */
static bool ble_companion_task_lost(pid_t pid)
{
  FAR struct tcb_s *tcb;
  bool lost = false;

  if (pid <= 0)
    {
      return false;
    }

  sched_lock();
  tcb = nxsched_get_tcb(pid);   /* 必须与 put 成对 */
  if (tcb != NULL && tcb->pid == pid &&
      (tcb->task_state == TSTATE_TASK_RUNNING ||
       tcb->task_state == TSTATE_TASK_READYTORUN) &&
      tcb->waitobj == NULL && tcb->lockcount == 0 &&
      !WDOG_ISACTIVE(&tcb->waitdog))
    {
      FAR struct tcb_s *t = this_task();
      int hops;

      lost = true;   /* 先假设丢了；在链上就翻案 */
      for (hops = 0; t != NULL && hops < 64; hops++, t = t->flink)
        {
          if (t == tcb)
            {
              lost = false;
              break;
            }
        }
    }

  if (tcb != NULL)
    {
      nxsched_put_tcb(tcb);
    }

  sched_unlock();
  return lost;
}

bool board_ble_companion_pid_alive(void)
{
  return ble_companion_pid_alive(g_ble_companion_pid);
}

bool board_ble_companion_busy(void)
{
  return g_ble_companion_spawning || g_ble_companion_restarting;
}

/** @brief 等 companion 退出；仍活着则禁止复用 g_ble_companion_stack。 */
static int ble_companion_wait_gone(pid_t pid, unsigned wait_ms)
{
  unsigned steps;
  unsigned i;

  if (pid <= 0)
    {
      return 0;
    }

  steps = (wait_ms + 99u) / 100u;
  for (i = 0; i < steps; i++)
    {
      if (!ble_companion_pid_alive(pid))
        {
          return 0;
        }

      usleep(100 * 1000);
    }

  return ble_companion_pid_alive(pid) ? -ETIMEDOUT : 0;
}

/** @brief 异步 task_spawn 启动 ble_companion。 */
void board_start_ble_companion_async(void)
{
  posix_spawnattr_t attr;
  FAR char *argv[] = { NULL };
  int pid;

  if (g_ble_companion_spawning ||
      ble_companion_pid_alive(g_ble_companion_pid))
    {
      syslog(LOG_ERR, "ble_companion: spawn refused pid=%d\n",
             (int)g_ble_companion_pid);
      return;
    }

  g_ble_companion_spawning = true;
  posix_spawnattr_init(&attr);
  attr.priority  = CONFIG_MYVENDOR_BLE_COMPANION_PRIORITY;
  attr.stacksize = sizeof(g_ble_companion_stack);
  posix_spawnattr_setstackaddr(&attr, g_ble_companion_stack);

  pid = task_spawn("ble_companion", ble_companion_main, NULL, &attr,
                   argv, NULL);
  if (pid < 0)
    {
      g_ble_companion_pid = (pid_t)-1;
      syslog(LOG_ERR, "ERROR: task_spawn ble_companion failed: %d\n", pid);
    }
  else
    {
      g_ble_companion_pid = (pid_t)pid;
      g_ble_companion_stuck_n = 0;
      syslog(LOG_INFO, "INFO: ble_companion async started pid=%d\n", pid);
    }

  g_ble_companion_spawning = false;
}

#ifdef CONFIG_SCHED_LPWORK
/* 杀任务前先摘掉它的 waitdog：**兜底**，不是唯一防线。
 *
 * 事实核对（2026-09-18 读码）：`nxtask_exithook()` 里的 `nxtask_recover()` 会
 * `wd_cancel(&tcb->waitdog)`（`sched/task/task_recover.c:75`，`task_exithook.c:459`），
 * 而 exithook 是正常退出和 `nxtask_terminate` 的共同尾巴 —— 也就是说"任务被杀"
 * 这条路上，waitdog 本来就会被撤掉。所以这里不是补"没人撤"的洞，而是覆盖
 * **退出钩子跑不到的那些情形**：kill 被延迟取消、目标卡在不可中断等待里、
 * 或者 TCB 已经被释放/复用而节点还留在 `g_wdactivelist` 上。
 *
 * 那个形状的后果就是 `vela_override/sched/wd_start.c` 头注释记的
 * `pos->prev==NULL` / `curr==NULL` 写 0x7d5（n004/n005）——链上留着指向
 * 已复用内存的节点，插链时写飞。先撤再杀的顺序也让现场更好读：日志里
 * `cancelled stale waitdog` 出现一次，就说明目标当时**正武装着**。
 *
 * 注：底层那个 `wd_cancel` 在本 port 上依赖 `vela_override/sched/wd_cancel.c`
 * （上游对已摘链的节点仍 `list_delete`）。
 *
 * 注：本函数放在 `board_restart_ble_companion()` 之前，且**不在任何 #ifdef 内**。 */
void myvendor_task_cancel_waitdog(pid_t pid, const char *tag)
{
  FAR struct tcb_s *tcb;

  if (pid <= 0)
    {
      return;
    }

  /* 需要成对：nxsched_put_tcb 会把 TCB 引用计数还回去。 */
  tcb = nxsched_get_tcb(pid);
  if (tcb == NULL)
    {
      return;
    }

  if (WDOG_ISACTIVE(&tcb->waitdog))
    {
      (void)wd_cancel(&tcb->waitdog);
      syslog(LOG_WARNING,
             "%s: cancelled stale waitdog pid=%d before kill\n",
             (tag != NULL) ? tag : "task", (int)pid);
    }

  nxsched_put_tcb(tcb);
}

/** @brief BLE companion 专用包装：日志前缀沿用历史文本。 */
static void ble_companion_cancel_waitdog(pid_t pid)
{
  myvendor_task_cancel_waitdog(pid, "ble_companion");
}

static void ble_companion_deferred_worker(FAR void *arg)
{
  (void)arg;
  syslog(LOG_INFO, "INFO: ble_companion deferred spawn\n");
  board_start_ble_companion_async();
}
#endif

/**
 * @brief 界面起来后再开 BLE：取消开机兜底，过一小会再 spawn。
 *
 * LCPU RF 校准会占 CPU，不能和开机音、首帧刷屏叠在一起。
 */
void board_ble_companion_start_after_ui(void)
{
#ifdef CONFIG_SCHED_LPWORK
  int ret;

  if (g_ble_start_requested ||
      g_ble_companion_spawning ||
      ble_companion_pid_alive(g_ble_companion_pid))
    {
      return;
    }

  g_ble_start_requested = true;
  (void)work_cancel(LPWORK, &g_ble_deferred_work);
  ret = work_queue(LPWORK, &g_ble_deferred_work, ble_companion_deferred_worker,
                   NULL, MSEC2TICK(1500));
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARN: ble_companion defer queue failed %d, spawn now\n", ret);
      board_start_ble_companion_async();
    }
  else
    {
      syslog(LOG_INFO, "INFO: ble_companion in 1.5s after UI\n");
    }
#else
  g_ble_start_requested = true;
  board_start_ble_companion_async();
#endif
}

int board_restart_ble_companion(void)
{
  pid_t pid;

  if (g_ble_companion_spawning || g_ble_companion_restarting)
    {
      syslog(LOG_WARNING, "ble_companion: restart busy\n");
      return -EBUSY;
    }

  g_ble_companion_restarting = true;
  pid = g_ble_companion_pid;
  BLE_LOG("restart begin pid=%d spawn=%d",
          (int)pid, g_ble_companion_spawning ? 1 : 0);

  if (ble_companion_pid_alive(pid))
    {
      syslog(LOG_ERR, "ble_companion: stall dump pid=%d phase=%s\n",
             (int)pid,
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
             companion_bridge_phase_get()
#else
             "-"
#endif
             );
#ifdef CONFIG_BLUETOOTH
      sf32lb52_bt_hci_dump_stall();
#endif
#ifdef CONFIG_SCHED_BACKTRACE
      sched_dumpstack(pid);
#endif
    }

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_teardown_skip_hci_set(true);
#endif
#ifdef CONFIG_BLUETOOTH
  /* 先让 HCI 轮询到 skip：LCPU 复位成功也不要清 skip，
   * 否则 companion 仍堵在不可中断等待里会再发同步命令。 */
  sf32lb52_bt_hci_skip_sync_set(true);
#endif

  if (ble_companion_pid_alive(pid))
    {
      ble_companion_cancel_waitdog(pid);
      syslog(LOG_WARNING, "ble_companion: SIGTERM pid=%d\n", (int)pid);
      (void)kill(pid, SIGTERM);
      if (ble_companion_wait_gone(pid, 1500u) != 0)
        {
          syslog(LOG_ERR, "ble_companion: SIGKILL pid=%d\n", (int)pid);
          (void)kill(pid, SIGKILL);
          (void)ble_companion_wait_gone(pid, 500u);
        }

      if (ble_companion_pid_alive(pid))
        {
          syslog(LOG_ERR, "ble_companion: task_delete pid=%d\n", (int)pid);
          (void)task_delete(pid);
          (void)ble_companion_wait_gone(pid, 1000u);
        }
    }

  if (ble_companion_pid_alive(pid) ||
      ble_companion_pid_alive(g_ble_companion_pid))
    {
      g_ble_companion_stuck_n++;
      syslog(LOG_ERR,
             "ble_companion: still alive pid=%d, skip respawn (attempt %u)\n",
             (int)(g_ble_companion_pid > 0 ? g_ble_companion_pid : pid),
             (unsigned)g_ble_companion_stuck_n);
      BLE_LOG("restart abort still-alive keep skip_sync attempt=%u",
              (unsigned)g_ble_companion_stuck_n);

      /** ② 收不回来也要把控制器拉起来。
       *
       *  下面的 `force reset` 只在「杀干净」那条路上（本函数末尾），僵尸
       *  不退就永远走不到 —— 现场就是 LCPU 停在哑巴状态、`skip=1`、BLE
       *  直到重启都回不来（见 docs/gnss_ble_diag_log.md §2.8）。
       *
       *  僵尸九成卡在 `net_buf_alloc` / HCI 命令等待这类不可中断等待上，
       *  而它等的 buffer 恰恰要靠控制器把 Number Of Completed Packets 送
       *  回来才归还。所以「把 LCPU 拉起来」是唯一能让它拿到 buffer、走回
       *  取消点、自己退出的动作 —— `nxnotify_cancellation()` 早把 pending
       *  cancel 挂在它身上了。拉不起来也只是维持现状，不会更坏。
       *
       *  force_reset 内部自己会置 skip、按调用前的值还回去（本路径由上面
       *  置过，所以保持），不需要在这里再动 skip。
       */
      syslog(LOG_ERR,
             "ble_companion: zombie pid=%d not reapable, reset LCPU to unblock\n",
             (int)(g_ble_companion_pid > 0 ? g_ble_companion_pid : pid));
#ifdef CONFIG_BLUETOOTH
      (void)sf32lb52_bt_controller_force_reset();
#endif

      /** ③ 连试 N 轮都收不回来，就别再把 HCI 发送路径关着。
       *
       *  `skip_sync` 是在三连杀之前置的，作用是让僵尸手里的 HCI 命令等待
       *  能退出（patch_hci_core.py 的轮询判据）。但僵尸要是永远不退，它就
       *  一直留着 —— `host_send()` 对一切命令返回 -ENODEV，连 ble_sensor
       *  重连、手机重连这些**跟 companion 无关**的通路也一起废掉，把
       *  「companion 挂了」升级成「整块 BLE 挂了」。
       *
       *  判定不可恢复之后撤回它：僵尸该卡还是卡（它卡的不是这个），但
       *  剩下的通路还能自己救回来。回收循环继续重试三连杀，也继续每轮
       *  force reset，僵尸真退出的那一轮照样能 spawn（成功路径会把 skip
       *  清掉，这里提前清只是不让它挡住别人）。
       */
      if (g_ble_companion_stuck_n >= BLE_COMPANION_GIVEUP_SKIP_N)
        {
#ifdef CONFIG_BLUETOOTH
          if (sf32lb52_bt_hci_skip_sync())
            {
              syslog(LOG_ERR,
                     "ble_companion: %u attempts, release skip_sync so the "
                     "rest of BLE can recover\n",
                     (unsigned)g_ble_companion_stuck_n);
              sf32lb52_bt_hci_skip_sync_set(false);
            }
#endif
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
          companion_bridge_teardown_skip_hci_set(false);
#endif
        }

      /** 收不回来的任务不能就此永久放弃。
       *
       *  僵尸任务卡在**不可中断等待**上时，SIGTERM / SIGKILL / task_delete
       *  都要等它回到取消点才生效（与 GNSS 那条"持锁的尸体"同形，见
       *  docs/gnss_ble_diag_log.md §2.4）。原先这里是纯 `return -EBUSY`：
       *  僵尸不退出 → 下一次调用照样卡在同一处 → BLE 永久停在
       *  「skip=1 + 没有 companion」，现场表现就是手机再也搜不到码表。
       *
       *  这里起一个**独立的小任务**按固定间隔重试，不依赖 diag 那条 20 s
       *  路径，也不占 LPWORK（`board_restart_ble_companion()` 自己最多会
       *  阻塞 3 s 等三连杀，压在共享工作队列上会连累别人）。
       *  每轮重走上面的 SIGTERM/SIGKILL/task_delete，僵尸真退出后下一轮
       *  就能 spawn 回来（`stuck_n` 随之归零，回收任务退出）。
       */
      if (!g_ble_companion_reclaim_up)
        {
          posix_spawnattr_t attr;
          FAR char *argv[] = { NULL };
          int rpid;

          posix_spawnattr_init(&attr);
          attr.priority  = SCHED_PRIORITY_DEFAULT;
          attr.stacksize = sizeof(g_ble_companion_reclaim_stack);
          posix_spawnattr_setstackaddr(&attr,
                                       g_ble_companion_reclaim_stack);

          rpid = task_spawn("ble_reclaim", ble_companion_reclaim_main, NULL,
                            &attr, argv, NULL);
          if (rpid > 0)
            {
              g_ble_companion_reclaim_up = true;
            }
          else
            {
              syslog(LOG_ERR, "ble_companion: reclaim spawn failed: %d\n",
                     rpid);
            }
        }

      /* ⭐ 2026-09-23 加：僵尸若"**永远不会再被调度**"，就别再等它退出。
       *
       * 现场：三连杀 + LCPU 复位连做 28 轮都收不回来，日志停在
       * `stuck … BLE down until it exits (check sys hr, or power cycle)`
       * —— 也就是**只能断电**才能让蓝牙回来（用户观察到的就是这个）。
       * 而那具僵尸的 stall dump 只有一帧 `#0 nxtask_activate`
       * （`task_activate.c:106` 就在 `nxsched_add_readytorun` 那段）⇒
       * 它是"建起来了但没被调度"，与 GNSS 读线程被调度器丢掉同一形状：
       * 不在任何等待队列上，没有任何事件能把它挂回就绪链 ⇒ **它永远不会
       * 再运行、也永远不会再碰 HCI**。
       *
       * 于是这里把它**隔离**（记下 pid、下一轮起不再杀它），落在下面那条
       * "杀干净"的成功路径上照常 spawn 新的 companion —— 僵尸的 TCB/栈留着
       * （泄漏，但不可达），换来的是**不需要断电**。
       *
       * ⚠ 自证伪条件（写死在这里，别忘）：这条判据的前提是"不在就绪链上"。
       * 若日志里出现**两具 companion 交替说话**（HCI 行里两套 pid 交错，
       * 或 `sys hr` 里同时看到两个 companion），说明那具"僵尸"其实还会被
       * 调度 ⇒ **撤回本改动**。隔离次数见 `lost_n=`（正常应长期为 0）。 */
      {
        pid_t zp = (g_ble_companion_pid > 0) ? g_ble_companion_pid : pid;

        if (g_ble_companion_stuck_n >= BLE_COMPANION_GIVEUP_SKIP_N &&
            ble_companion_task_lost(zp))
          {
            g_ble_companion_lost_pid = zp;
            g_ble_companion_lost_n++;
            syslog(LOG_WARNING,
                   "ble_companion: zombie pid=%d unreachable (not on any ready "
                   "list) -> quarantine + respawn (lost_n=%u)\n",
                   (int)zp, (unsigned)g_ble_companion_lost_n);
          }
        else
          {
            if ((g_ble_companion_stuck_n % BLE_COMPANION_RETRY_LOG_N) == 0u)
              {
                syslog(LOG_ERR,
                       "ble_companion: stuck pid=%d, %u attempts; BLE down "
                       "until it exits (check `sys hr`, or power cycle)\n",
                       (int)zp, (unsigned)g_ble_companion_stuck_n);
              }

            g_ble_companion_restarting = false;
            return -EBUSY;
          }
      }
    }

  g_ble_companion_pid = (pid_t)-1;
  ble_companion_reap_if_dead();
#ifdef CONFIG_BLUETOOTH
  BLE_LOG("restart lcpu reset then clear skip and spawn");
  (void)sf32lb52_bt_controller_force_reset();
  sf32lb52_bt_hci_skip_sync_set(false);
#endif
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_teardown_skip_hci_set(false);
#endif
  usleep(100 * 1000);
  board_start_ble_companion_async();
  g_ble_companion_restarting = false;
  BLE_LOG("restart spawn pid=%d", (int)g_ble_companion_pid);
  return g_ble_companion_pid > 0 ? 0 : -EIO;
}

/**
 * @brief 僵尸 companion 的定期重试（见 `board_restart_ble_companion()` 末尾）。
 *
 * @details
 * 只在「上一具任务收不回来」时被 spawn，一次生命周期里循环重试：
 *
 *   - 每 `BLE_COMPANION_RETRY_MS` 调一次 `board_restart_ble_companion()`
 *     （它会重走 SIGTERM/SIGKILL/task_delete 三连杀并尝试 spawn）；
 *   - `g_ble_companion_stuck_n` 归零表示新任务起来了 → 退出；
 *   - 退出前清 `g_ble_companion_reclaim_up`，允许下一次卡死时再起一个。
 *
 * 刻意不放在 LPWORK 上：`board_restart_ble_companion()` 自己最多阻塞 3 s
 * 等三连杀，压在共享工作队列上会连累别的延迟工作。也刻意不用 diag 去驱动
 * —— diag 那条 20 s 路径本来就在跑，但没有它（或它被 `restarting` 挡掉）时
 * 也需要有人重试。
 */
static int ble_companion_reclaim_main(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;

  do
    {
      usleep(BLE_COMPANION_RETRY_MS * 1000u);

      syslog(LOG_WARNING, "ble_companion: retry reclaim (attempt %u)\n",
             (unsigned)(g_ble_companion_stuck_n + 1u));
      (void)board_restart_ble_companion();
    }
  while (g_ble_companion_stuck_n != 0u);

  g_ble_companion_reclaim_up = false;
  return 0;
}
#endif

#ifdef CONFIG_MYVENDOR_BICYCLE_AUTOSTART
static uint8_t g_bicycle_stack[CONFIG_MYVENDOR_BICYCLE_STACKSIZE]
    __attribute__((aligned(16)));

/** @brief 异步 task_spawn 启动 bicycle UI。 */
static void board_start_bicycle_async(void)
{
  posix_spawnattr_t attr;
  FAR char *argv[] = { NULL };
  int pid;

  posix_spawnattr_init(&attr);
  attr.priority  = CONFIG_MYVENDOR_BICYCLE_PRIORITY;
  attr.stacksize = sizeof(g_bicycle_stack);
  posix_spawnattr_setstackaddr(&attr, g_bicycle_stack);

  pid = task_spawn("bicycle", bicycle_main, NULL, &attr, argv, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "ERROR: task_spawn bicycle failed: %d\n", pid);
    }
  else
    {
      syslog(LOG_INFO, "INFO: bicycle UI async started pid=%d\n", pid);
    }
}
#endif

#ifdef CONFIG_LCD
#define LCD_BRINGUP_STACKSIZE 8192
#define LCD_BRINGUP_STACK_ALIGN 16u

/* One-shot boot task: retain its small BoardPSRAM stack after exit rather
 * than reserving 8 KiB of scarce HCPU SRAM forever. */
static void *g_lcd_bringup_stack_raw;
static void *g_lcd_bringup_stack;

static void *lcd_bringup_stack_alloc(void)
{
  uintptr_t aligned;

  if (g_lcd_bringup_stack != NULL)
    {
      return g_lcd_bringup_stack;
    }

  g_lcd_bringup_stack_raw = board_malloc_psram(
      LCD_BRINGUP_STACKSIZE + LCD_BRINGUP_STACK_ALIGN - 1u);
  if (g_lcd_bringup_stack_raw == NULL ||
      !board_ptr_in_psram_pool(g_lcd_bringup_stack_raw))
    {
      board_mem_free(g_lcd_bringup_stack_raw);
      g_lcd_bringup_stack_raw = NULL;
      return NULL;
    }

  aligned = ((uintptr_t)g_lcd_bringup_stack_raw +
             LCD_BRINGUP_STACK_ALIGN - 1u) &
            ~((uintptr_t)LCD_BRINGUP_STACK_ALIGN - 1u);
  g_lcd_bringup_stack = (void *)aligned;
  return g_lcd_bringup_stack;
}

/** @brief lcd_bringup 任务入口，调用 board_lcd_initialize。 */
static int lcd_bringup_main(int argc, FAR char *argv[])
{
  int ret;

  (void)argc;
  (void)argv;

  syslog(LOG_INFO, "INFO: lcd_bringup starting\n");
  ret = board_lcd_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_lcd_initialize failed: %d\n", ret);
    }

  return ret;
}

/** @brief 异步 task_spawn 启动 LCD 初始化。 */
static void board_start_lcd_init_async(void)
{
  posix_spawnattr_t attr;
  FAR char *argv[] = { NULL };
  void *stack;
  int pid;

  stack = lcd_bringup_stack_alloc();
  posix_spawnattr_init(&attr);
  attr.priority  = SCHED_PRIORITY_DEFAULT + 10;
  attr.stacksize = LCD_BRINGUP_STACKSIZE;
  if (stack != NULL)
    {
      posix_spawnattr_setstackaddr(&attr, stack);
    }
  else
    {
      syslog(LOG_WARNING,
             "WARN: lcd_bringup BoardPSRAM stack unavailable; use task heap\n");
    }

  pid = task_spawn("lcd_bringup", lcd_bringup_main, NULL, &attr, argv, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "ERROR: task_spawn lcd_bringup failed: %d\n", pid);
    }
  else
    {
      syslog(LOG_INFO, "INFO: lcd_bringup async started pid=%d\n", pid);
    }
}
#endif

/** @brief 挂载 procfs、/data 与 tmpfs。 */
static int bringup_vfs(void)
{
#ifdef CONFIG_FS_PROCFS
  int pret;

  (void)bringup_mkdir("/proc");
  pret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (pret < 0 && pret != -EBUSY)
    {
      serr("WARN: mount procfs failed: %d\n", pret);
    }
#endif

  (void)bringup_mkdir("/data");

#ifdef CONFIG_FS_TMPFS
  {
    int tret = nx_mount(NULL, "/data", "tmpfs", 0, NULL);

    if (tret < 0 && tret != -EBUSY)
      {
        serr("WARN: mount tmpfs on /data failed: %d\n", tret);
      }
  }
#endif

  return OK;
}

/** @brief 注册 RTC 并设置/读取启动时间。 */
static int bringup_rtc(void)
{
#if defined(CONFIG_RTC) && defined(CONFIG_RTC_DRIVER)
  struct rtc_lowerhalf_s *rtclower;
  int ret;
  int rtcfd;

  rtclower = sf32lb_rtc_lowerhalf();
  if (rtclower == NULL)
    {
      serr("ERROR: Failed to instantiate RTC lower-half\n");
      return -ENOMEM;
    }

  ret = rtc_initialize(0, rtclower);
  if (ret < 0)
    {
      serr("ERROR: rtc_initialize failed: %d\n", ret);
      return ret;
    }

  rtcfd = open("/dev/rtc0", O_RDWR);
  if (rtcfd < 0)
    {
      rtcfd = open("/dev/rtc0", O_RDONLY);
    }

  if (rtcfd < 0)
    {
      serr("ERROR: open /dev/rtc0 failed: %d\n", errno);
    }
  else
    {
      struct rtc_time rt;
      bool trusted = sf32lb_rtc_trust_is_valid();

      memset(&rt, 0, sizeof(rt));
      if (!trusted)
        {
          rt.tm_year = 100;
          rt.tm_mon  = 0;
          rt.tm_mday = 1;
          if (ioctl(rtcfd, RTC_SET_TIME,
                    (unsigned long)(uintptr_t)&rt) < 0)
            {
              serr("ERROR: RTC_SET_TIME baseline failed: %d\n", errno);
            }
          else
            {
              trusted = true;
            }
        }

      memset(&rt, 0, sizeof(rt));
      if (ioctl(rtcfd, RTC_RD_TIME, (unsigned long)(uintptr_t)&rt) < 0)
        {
          serr("ERROR: RTC_RD_TIME failed: %d\n", errno);
        }
      else
        {
          syslog(LOG_WARNING,
                 "[rtc] boot %04d-%02d-%02d %02d:%02d:%02d trusted=%d "
                 "(magic=0x%08x; use 'test rtc' to change)\n",
                 rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
                 rt.tm_hour, rt.tm_min, rt.tm_sec,
                 trusted ? 1 : 0, SF32LB_RTC_TRUST_MAGIC);
        }

      close(rtcfd);
    }
#endif

  return OK;
}

/** @brief 初始化 ADC 字符设备。 */
static int bringup_adc(void)
{
#ifdef CONFIG_ADC
  int ret = sf32lb_adc_init("/dev/adc0");

  if (ret < 0)
    {
      serr("ERROR: sf32lb_adc_init failed: %d\n", ret);
      return ret;
    }
#endif

  return OK;
}

/** @brief 初始化 NuttX GPIO 驱动。 */
static int bringup_gpio(void)
{
#ifdef CONFIG_DEV_GPIO
  int ret = sifli_gpio_initialize();

  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: sifli_gpio_initialize failed: %d\n", ret);
      return ret;
    }
#endif

  return OK;
}

/** @brief 初始化 CPU 负载 EXTCLK 采样。 */
static int bringup_cpuload(void)
{
#ifdef CONFIG_MYVENDOR_CPULOAD_EXTCLK
  int ret = myvendor_cpuload_extclk_init();

  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARN: cpuload EXTCLK init failed: %d\n", ret);
      return ret;
    }
#endif

  return OK;
}

/** @brief 注册通用定时器 /dev/timer0。 */
static int bringup_timer(void)
{
#if defined(CONFIG_TIMER) && SF32LB_TIMER_DEFAULT_INDEX >= 0
  FAR struct timer_lowerhalf_s *timer_lower;
  FAR void *timer_upper;

  timer_lower = sf32lb_timer_initialize(SF32LB_TIMER_DEFAULT_INDEX, 1000000);
  if (timer_lower == NULL)
    {
      syslog(LOG_ERR, "ERROR: sf32lb_timer_initialize failed\n");
      return -ENODEV;
    }

  timer_upper = timer_register("/dev/timer0", timer_lower);
  if (timer_upper == NULL)
    {
      syslog(LOG_ERR, "ERROR: timer_register(/dev/timer0) failed: %d\n",
             errno);
      return -errno;
    }
#endif

  return OK;
}

/** @brief 注册 PWM 设备 /dev/pwm0。 */
static int bringup_pwm(void)
{
#if defined(CONFIG_PWM) && SF32LB_PWM_DEFAULT_INDEX >= 0
  FAR struct pwm_lowerhalf_s *pwm_lower;
  int ret;

  pwm_lower = sf32lb_pwm_initialize(SF32LB_PWM_DEFAULT_INDEX);
  if (pwm_lower == NULL)
    {
      syslog(LOG_ERR, "ERROR: sf32lb_pwm_initialize failed\n");
      return -ENODEV;
    }

  ret = pwm_register("/dev/pwm0", pwm_lower);
  if (ret < 0 && ret != -EEXIST)
    {
      syslog(LOG_ERR, "ERROR: pwm_register(/dev/pwm0) failed: %d\n", ret);
      return ret;
    }
#endif

  return OK;
}

/** @brief 注册按键输入 /dev/buttons。 */
static int bringup_buttons(void)
{
#ifdef CONFIG_INPUT_BUTTONS
  int ret = sf32lb52_button_initialize("/dev/buttons");

  if (ret < 0 && ret != -EEXIST)
    {
      syslog(LOG_ERR, "ERROR: sf32lb52_button_initialize() failed: %d\n",
             ret);
      return ret;
    }
#endif

  return OK;
}

/** @brief 初始化 ETA9184：STAT/DISCHRG/PULSE 采样；产品电量走 VBATS。 */
static int bringup_eta9184(void)
{
  int ret = eta9184_init();

  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARN: eta9184_init failed: %d\n", ret);
      return ret;
    }

  ret = eta9184_start();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARN: eta9184_start failed: %d\n", ret);
    }

  return OK;
}

/** @brief SD/NAND automount、LittleFS 与 KV 分区挂载。 */
static int bringup_storage(void)
{
#ifdef CONFIG_MTD
  FAR const struct ptab_entry *fs;
  uint32_t fs_off = FS_REGION_OFFSET;
  uint32_t fs_sz = FS_REGION_SIZE;
  int ret;

  fs = ptab_find_tag("FS_REGION");
  if (fs == NULL)
    {
      fs = ptab_find_img("fs_root");
    }

  if (fs != NULL)
    {
      fs_off = fs->offset;
      fs_sz = fs->size;
      if (fs_sz == 0)
        {
          syslog(LOG_INFO,
                 "INFO: FS_REGION from ptab: off=0x%lx size=remainder of card "
                 "(%s)\n",
                 (unsigned long)fs_off,
                 fs->mem != NULL ? fs->mem : "?");
        }
      else
        {
          syslog(LOG_INFO,
                 "INFO: FS_REGION from ptab: off=0x%lx size=0x%lx (%s)\n",
                 (unsigned long)fs_off, (unsigned long)fs_sz,
                 fs->mem != NULL ? fs->mem : "?");
        }
    }
  else
    {
      syslog(LOG_WARNING,
             "WARN: FS_REGION missing in ptab, fallback macros "
             "off=0x%lx size=0x%lx\n",
             (unsigned long)fs_off, (unsigned long)fs_sz);
    }

#if defined(MY_VENDOR_BOOT_FROM_SD) || defined(MY_VENDOR_BOOT_FROM_EMMC)
  syslog(LOG_INFO, "INFO: SD/eMMC automount starting...\n");
  ret = sf32lb_sd_automount(0, fs_off, fs_sz);
  if (ret < 0)
    {
      serr("WARN: sf32lb_sd_automount failed: %d\n", ret);
      g_storage_ok = false;
      return ret;
    }

  ret = sf32lb_sd_mount_littlefs(0);
  if (ret < 0)
    {
      serr("WARN: sf32lb_sd_mount_littlefs failed: %d "
           "(continue KV/FAT)\n", ret);
    }

  {
    FAR const struct ptab_entry *kv;
    uint32_t kv_off = KV_REGION_OFFSET;
    uint32_t kv_sz = KV_REGION_SIZE;

    kv = ptab_find_tag("KV_REGION");
    if (kv != NULL && kv->size != 0)
      {
        kv_off = kv->offset;
        kv_sz = kv->size;
      }

    syslog(LOG_INFO,
           "INFO: KV_REGION off=0x%lx size=0x%lx -> /mnt/kv\n",
           (unsigned long)kv_off, (unsigned long)kv_sz);
    ret = sf32lb_sd_mount_kv(kv_off, kv_sz);
    if (ret < 0)
      {
        serr("WARN: sf32lb_sd_mount_kv failed: %d (continue FAT)\n", ret);
      }
    else
      {
        (void)bringup_mkdir("/mnt/kv/fw");
        (void)myvendor_fw_slot_prune();
#ifdef CONFIG_MYVENDOR_COREDUMP
        myvendor_coredump_init();
#endif

        /* 日志落盘：注册一个只写 RAM 环的 syslog 通道，之后所有日志自动进环，
         * 由 diag 线程每 500 ms 刷到 /mnt/kv/diag/d*.txt（**自己的目录**，不与
         * 崩溃转储混放；命名沿用 coredump 那套，前缀 N->D，只收异常级别）。
         *
         * 位置很关键：必须在 KV 挂载成功**之后**（文件名要落在 /mnt/kv 下）。
         * 失败不致命 —— 只是没有落盘日志，控制台照旧。 */
        {
          extern int myvendor_diaglog_start(void);
          extern int myvendor_diaglog_flush(void);

          if (myvendor_diaglog_start() != 0)
            {
              serr("WARN: diaglog start failed, persistent log off\n");
            }
          else
            {
              /* 开机这一批挂载信息最值钱，立刻落一次，别等 500 ms。 */
              (void)myvendor_diaglog_flush();
            }
        }

        (void)myvendor_boot_slot();
      }
  }

  {
    FAR const struct ptab_entry *fat;
    uint32_t fat_off = FAT_REGION_OFFSET;
    uint32_t fat_sz = FAT_REGION_SIZE;

    fat = ptab_find_tag("FAT_REGION");
    if (fat == NULL)
      {
        fat = ptab_find_img("fat_root");
      }

    if (fat != NULL)
      {
        fat_off = fat->offset;
        fat_sz = fat->size;
      }

    syslog(LOG_INFO,
           "INFO: FAT_REGION off=0x%lx size=%s -> /mnt/fat (%s)\n",
           (unsigned long)fat_off,
           fat_sz == 0 ? "remainder of card" : "fixed",
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
           "rw");
#else
           "ro");
#endif
    if (fat_sz != 0)
      {
        syslog(LOG_INFO, "INFO: FAT_REGION size=0x%lx\n",
               (unsigned long)fat_sz);
      }

    ret = sf32lb_sd_mount_fat(fat_off, fat_sz);
    if (ret < 0)
      {
        serr("WARN: sf32lb_sd_mount_fat failed: %d (LiveMap empty until FAT is present)\n",
             ret);
      }
  }

  bringup_gpx_dirs();
#else
  syslog(LOG_INFO, "INFO: NAND automount starting...\n");
  ret = sf32lb_nand_automount(0, fs_off, fs_sz);
  if (ret < 0)
    {
      serr("WARN: sf32lb_nand_automount failed: %d\n", ret);
      g_storage_ok = false;
      return ret;
    }

  ret = sf32lb_nand_mount_littlefs(0);
  if (ret < 0)
    {
      serr("WARN: sf32lb_nand_mount_littlefs failed: %d\n", ret);
      g_storage_ok = false;
      return ret;
    }

  (void)bringup_mkdir("/mnt/lfs/fw");
  bringup_gpx_dirs();
#endif
#endif

  return OK;
}

/** @brief 创建 GNSS 星历目录，并立刻把 mga_*.ubx 收到 5 份。 */
static int bringup_gnss_prep(void)
{
#ifdef CONFIG_BOARD_L96_GNSS
  int ret = bringup_mkdir(SF32LB52_GNSS_EPH_DIR);

  if (ret == OK)
    {
      BRINGUP_LOG("GNSS eph dir %s\n", SF32LB52_GNSS_EPH_DIR);
      myvendor_gnss_eph_maintain();
    }

  return ret;
#else
  return OK;
#endif
}

/** @brief 存储就绪后将 HCPU 提频至 240 MHz，并交给 DVFS 接管。 */
static int bringup_hclk240(void)
{
#if defined(CONFIG_BSP_USING_SPI_NAND) && defined(SOC_BF0_HCPU)
  if (!g_storage_ok)
    {
      return OK;
    }

  syslog(LOG_INFO, "INFO: raising HCPU to 240 MHz...\n");
#ifdef CONFIG_MYVENDOR_DVFS
  sf32lb_dvfs_boot();
#ifdef CONFIG_LCD_USING_NV3031A
  /* GPTIM1 可能被提频打一下；立刻把背光 PWM 接回来，避免 splash 黑一下。 */
  BSP_LCD_BL_Set(1);
#endif
#else
  BSP_Board_EnableHclk240();
#ifdef CONFIG_LCD_USING_NV3031A
  BSP_LCD_BL_Set(1);
  sf32lb_lcd_reclock();
#endif
  sifli_systick_reclock();
#ifdef CONFIG_MTD
  sf32lb_sd_reclock();
#endif
  /* ADC 的采样宽度是按 PCLK1（= HCLK>>PDIV1）算的，抬完频必须重算一次，
   * 否则 SAR 在错的时钟下被截断、电池读数系统性偏高。开了 DVFS 的那条路由
   * sf32lb_dvfs_boot() → reclock_peripherals() 负责，这里补的是没开 DVFS
   * （工厂固件）的那一半。
   * 此刻还没人开 ADC（board_sensor 在第 21 步才起），所以不用先 clk_hold。 */
  (void)sf32lb_adc_reclock();
#endif
#endif

  return OK;
}

/** @brief 打印 HCPU / MPI 时钟诊断信息。 */
static int bringup_clocks(void)
{
#ifdef SOC_BF0_HCPU
  BSP_Board_PrintClocks();
#endif

  return OK;
}

/** @brief 配置 GNSS USART2 并启动读线程；MAX-M10S-00B-01 上电等待放到读线程。 */
static int bringup_gnss(void)
{
#ifdef CONFIG_BOARD_L96_GNSS
  int ret;

  BRINGUP_LOG("GNSS MAX-M10S-00B-01 pinmux PA31 TX / PA32 RX / PA43 VCC\n");
  HAL_PIN_Set(PAD_PA31, USART2_TXD, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA32, USART2_RXD, PIN_PULLUP, 1);
  ret = myvendor_gnss_start();
  if (ret < 0)
    {
      serr("WARN: myvendor_gnss_start failed: %d\n", ret);
    }
  else
    {
      BRINGUP_LOG("GNSS reader started (module settle in gnss thread)\n");
    }
#endif

  return OK;
}

/** @brief 独立诊断线程：BLE / GNSS 异常时重启，不依赖 companion 主循环。 */
static int bringup_diag(void)
{
  int ret;

  ret = myvendor_diag_start();
  if (ret < 0)
    {
      serr("WARN: myvendor_diag_start failed: %d\n", ret);
    }

  return OK;
}

/** @brief PA40 陶瓷蜂鸣片 PWM 声音线程。 */
static int bringup_sound(void)
{
  int ret;

  ret = myvendor_sound_start();
  if (ret < 0)
    {
      serr("WARN: myvendor_sound_start failed: %d\n", ret);
    }

  return OK;
}

/** @brief 初始化 I2C1 并注册板载传感器。 */
static int bringup_i2c(void)
{
#ifdef CONFIG_I2C
  struct i2c_master_s *i2c0;
  int ret;

  BRINGUP_LOG("I2C1 pinmux PA39 SCL / PA38 SDA\n");
  HAL_PIN_Set(PAD_PA39, I2C1_SCL, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA38, I2C1_SDA, PIN_PULLUP, 1);

  i2c0 = sifli_i2cbus_initialize(0);
  if (i2c0 == NULL)
    {
      syslog(LOG_ERR, "ERROR: sifli_i2cbus_initialize(0) failed\n");
      return -ENODEV;
    }

  ret = i2c_register(i2c0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: i2c_register(/dev/i2c0) failed: %d\n", ret);
      return ret;
    }

#ifdef CONFIG_BOARD_BMI270
  {
    int attempt;

    /* Probe IMU first. Missing baro/mag can NACK or hang 5s and must not
     * delay or tear down BMI270. */
    for (attempt = 0; attempt < 3; attempt++)
      {
        ret = sf32lb52_bmi270_register(0, i2c0, CONFIG_BOARD_BMI270_I2C_ADDR);
        if (ret == 0)
          {
            break;
          }

        syslog(LOG_ERR,
               "ERROR: sf32lb52_bmi270_register failed: %d (try %d/3)\n",
               ret, attempt + 1);
        usleep(200000);
      }
  }
#endif

#ifdef CONFIG_BOARD_BMP388
  ret = bmp388_register(0, i2c0);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARN: bmp388 absent (register %d), IMU still used\n", ret);
    }
#endif

#ifdef CONFIG_BOARD_MMC5983MA
  ret = mmc5983ma_register(0, i2c0);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARN: mmc5983 absent (register %d), IMU still used\n", ret);
    }
#endif

  BRINGUP_LOG("I2C sensor bringup complete\n");
#endif

  return OK;
}

/** @brief 板卡传感器线程：I2C uORB + VBATS ADC 乒乓。须在 I2C 注册之后。 */
static int bringup_board_sensor(void)
{
#if defined(CONFIG_BOARD_BMI270) || defined(CONFIG_BOARD_BMP388) || \
    defined(CONFIG_BOARD_MMC5983MA) || defined(CONFIG_ADC)
  int ret = myvendor_board_sensor_start();

  if (ret < 0 && ret != -ENOTSUP)
    {
      syslog(LOG_WARNING, "WARN: board_sensor start failed: %d\n", ret);
    }
#endif

  return OK;
}

/** @brief 注册 SPI1 字符设备与 TF 卡。 */
static int bringup_spi(void)
{
#if defined(CONFIG_SPI) && defined(CONFIG_BSP_USING_SPI1) && \
    defined(CONFIG_SPI_DRIVER)
  {
    int ret = sf32lb52_spi1_register_char();

    if (ret < 0)
      {
        serr("WARN: SPI1 char device register failed: %d\n", ret);
      }
  }
#endif

#if defined(CONFIG_SPI) && defined(CONFIG_MMCSD_SPI) && \
    defined(CONFIG_BSP_USING_SPI1)
  {
    int ret = sf32lb52_tfcard_initialize();

    if (ret < 0)
      {
        serr("WARN: TF card init failed: %d\n", ret);
      }
  }
#endif

  return OK;
}

/** @brief 从 persist.* 载入 devctl 策略。 */
static int bringup_devctl(void)
{
  myvendor_devctl_load();
  return OK;
}

/** @brief 异步启动 LCD 驱动初始化。 */
static int bringup_lcd(void)
{
#ifdef CONFIG_LCD
  board_start_lcd_init_async();
#endif

  return OK;
}

/** @brief 注册 /dev/watchdog0 并立刻 START，接管 boot 留下的 IWDT。 */
static int bringup_watchdog(void)
{
#ifdef CONFIG_WATCHDOG
  sf32lb_iwdginitialize("/dev/watchdog0");
#endif
#ifdef CONFIG_MYVENDOR_WATCHDOG
  myvendor_watchdog_start();
#endif
#ifdef CONFIG_MYVENDOR_COREDUMP
  myvendor_coredump_early();
#endif

  return OK;
}

/** @brief 读取并缓存产品 identity 名称。 */
static int bringup_identity(void)
{
  (void)myvendor_identity_name();
  return OK;
}

/** @brief 打开或关闭 USB 控制器（不停 MTP worker）。工厂固件不看 persist。 */
static int bringup_usbmtp(void)
{
#if defined(CONFIG_USBDEV) || defined(CONFIG_SF32LB52_USBDEV)
  int ret;

  if (myvendor_is_factory())
    {
      syslog(LOG_INFO, "INFO: factory USB+MTP on (ignore persist.usb.mtp)\n");
    }
  else if (!myvendor_devctl_mtp_get())
    {
      syslog(LOG_INFO, "INFO: USB controller off (persist.usb.mtp=0)\n");
      return myvendor_usbdev_set(false);
    }

  ret = myvendor_usbdev_set(true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: USB controller on failed %d\n", ret);
      return ret;
    }
#endif

  return OK;
}

/** @brief 开机自动启动 MTP worker（与 USB 控制器开关独立）。 */
static int bringup_mtp(void)
{
#ifdef CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART
  int ret;

  if (!myvendor_is_factory() && !myvendor_devctl_mtp_get())
    {
      syslog(LOG_INFO, "INFO: mtp worker skipped (USB controller off)\n");
      return OK;
    }

  BRINGUP_LOG("myvendor_mtp_init (autostart)\n");
  ret = myvendor_mtp_init();
  if (ret != 0)
    {
      syslog(LOG_ERR, "ERROR: myvendor_mtp_init failed\n");
      return ret;
    }

  ret = myvendor_mtp_transfer_begin();
  if (ret != 0)
    {
      syslog(LOG_INFO,
             "myvendor_mtp_transfer_begin deferred until ENUM (%d)\n",
             ret);
    }
#endif

  return OK;
}

/** @brief 初始化蓝牙 H4 UART 接口。 */
static int bringup_bth4(void)
{
#ifdef CONFIG_UART_BTH4
  int ret = sf32lb52_bt_initialize();

  if (ret < 0 && ret != -EEXIST)
    {
      serr("WARN: sf32lb52_bt_initialize failed: %d\n", ret);
      return ret;
    }
#endif

  return OK;
}

/** @brief 记下要开 BLE。有自行车 UI 时等界面起来再 spawn，避免开机音被 RF 校准拉长。 */
static int bringup_ble(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
  if (myvendor_is_factory())
    {
      syslog(LOG_INFO, "INFO: factory BLE companion on (ignore persist.bt.radio)\n");
    }

#ifdef CONFIG_MYVENDOR_BICYCLE_AUTOSTART
  syslog(LOG_INFO, "INFO: BLE companion waits for UI (RF cal after splash)\n");
#else
  board_start_ble_companion_async();
#endif
#endif

  return OK;
}

/** @brief 异步启动 bicycle UI 应用。 */
static int bringup_bicycle(void)
{
#ifdef CONFIG_MYVENDOR_BICYCLE_AUTOSTART
  board_start_bicycle_async();
#endif

  return OK;
}

/**
 * @brief 开机步骤表（顺序即执行顺序）。
 *
 * watchdog 最先：注册 /dev/watchdog0 并 START（接管 2SFBL 留下的 IWDT）。
 * lcd 在 gpio/pwm 之后异步初始化；bicycle 等 storage+devctl。
 * GNSS / I2C 非首帧必需。BLE HCI 只注册设备；companion 等 UI 起来再 spawn。
 *
 * @section bootreset ★ 规则：**2SFBL 用过的外设，App 侧必须先复位再配置**
 *
 * 起因（2026-10-01）：电池读数在冷启动后恒定输出 4571 mV（= 12 位中点码），
 * 查了一整轮才发现是 **GPADC 模块没被复位** —— 2SFBL 自己也用这颗 ADC 读
 * VBATS（`boot_loader/project/butterflmicro/board/boot_hw.c` 的
 * `boot_hw_bat_init/convert_raw`），而 `HAL_ADC_Init()` 只做**读-改-写**，
 * 纠正不回它留下的模拟状态。判据是"**冷启动必坏 / `reboot` 热启动正常**"。
 *
 * **症状签名**（比"完全不能用"危险得多，别等它自己暴露）：
 *   · 功能只在**一个方向上**错（例如读数恒定、贴在量程中点）；
 *   · 寄存器 dump **逐字节全都对**，两条代码路径用同一组值却行为不同；
 *   · **复位整个模块**就好，而逐位去改寄存器怎么都修不好。
 *
 * 2SFBL 会碰的外设（`boot_loader/project/butterflmicro/board/`）：
 *   `boot_lcd` 的 LCDC、`boot_flash`/`sd_*` 的 SD/eMMC、`board_psram` 的 MPI、
 *   `boot_hw` 的 GPADC + ETA9184 引脚 + IWDT、`efuse`、`boot_lfs` 的 flash。
 *
 * **多数外设不用我们操心**：SiFli HAL 的 Init 会**无条件复位自己的模块** ——
 * `HAL_I2C_Init`（`bf0_hal_i2c.c:208`）、`LCDC_HW_Init`（`bf0_hal_lcdc.c:2273`）
 * 等等，而且都是 `HAL_RCC_EnableModule()` + `HAL_RCC_ResetModule()` 成对出现。
 * **`HAL_ADC_Init()` 是例外**：既不复位也不 Enable（要驱动自己 `HAL_RCC_EnableModule`）。
 *
 * 已按此规则处理的先例：
 *   · GPADC → `HAL_RCC_ResetModule(RCC_MOD_GPADC)`（`sf32lb_adc.c` 的 `sf32lb_adc_init`，
 *             复位后 `HAL_Delay_us(1000)` 留模拟上电时间）；
 *   · SD1   → `HAL_RCC_ResetModule(RCC_MOD_SDMMC1)`（`sf32lb_sdio.c` 的 `sd1_init`）；
 *   · EPIC/EZIP/LCDC1 → `lv_sifli_epic_cfg.c`、`myvendor_coredump.c` 已有复位。
 *
 * **改动/新增步骤时先问一句"2SFBL 碰过这个外设吗"**；碰过就先复位。
 */
static const struct bringup_step_s g_bringup[] =
{
  { "watchdog",  bringup_watchdog,  false },
  { "vfs",       bringup_vfs,       false },
  { "rtc",       bringup_rtc,       true  },
  { "adc",       bringup_adc,       true  },
  { "gpio",      bringup_gpio,      true  },
  { "pwm",       bringup_pwm,       true  },
  { "lcd",       bringup_lcd,       false },
  { "cpuload",   bringup_cpuload,   false },
  { "timer",     bringup_timer,     true  },
  { "buttons",   bringup_buttons,   true  },
  { "sound",     bringup_sound,     false },
  { "eta9184",   bringup_eta9184,   false },
  { "storage",   bringup_storage,   false },
  { "devctl",    bringup_devctl,    false },
  { "hclk240",   bringup_hclk240,   false },
  { "clocks",    bringup_clocks,    false },
  { "bicycle",   bringup_bicycle,   false },
  { "gnss_prep", bringup_gnss_prep, false },
  { "gnss",      bringup_gnss,      false },
  { "i2c",       bringup_i2c,       true  },
  { "sensors",   bringup_board_sensor, false },
  { "spi",       bringup_spi,       false },
  { "identity",  bringup_identity,  false },
  { "usbmtp",    bringup_usbmtp,    false },
  { "mtp",       bringup_mtp,       false },
  { "bth4",      bringup_bth4,      false },
  { "ble",       bringup_ble,       false },
  { "diag",      bringup_diag,      false },
};

/**
 * @brief 按 g_bringup[] 顺序执行板级晚初始化。
 * @return 0 全部完成或仅非 abort 步骤失败；负值为 abort_on_fail 步骤失败码。
 */
int sf32lb52_devkit_lcd_bringup(void)
{
  const size_t n = sizeof(g_bringup) / sizeof(g_bringup[0]);
  size_t i;
  int ret;

  BRINGUP_LOG("enter, %u steps\n", (unsigned)n);

  for (i = 0; i < n; i++)
    {
      BRINGUP_LOG("[%u/%u] %s\n",
                  (unsigned)(i + 1), (unsigned)n, g_bringup[i].name);
      myvendor_watchdog_busy_pump();
      ret = g_bringup[i].fn();
      if (ret < 0)
        {
          if (g_bringup[i].abort_on_fail)
            {
              syslog(LOG_ERR, "BRINGUP: %s abort %d\n",
                     g_bringup[i].name, ret);
#ifdef CONFIG_MYVENDOR_WATCHDOG
              myvendor_watchdog_start();
#endif
              return ret;
            }

          syslog(LOG_WARNING, "BRINGUP: %s fail %d, continue\n",
                 g_bringup[i].name, ret);
        }
    }

#ifdef CONFIG_MYVENDOR_WATCHDOG
  myvendor_watchdog_start();
#endif

  BRINGUP_LOG("done\n");
  return OK;
}
