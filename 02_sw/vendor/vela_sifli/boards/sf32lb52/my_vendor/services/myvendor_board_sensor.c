/**
 * @file myvendor_board_sensor.c
 * @brief 板卡传感器线程：I2C uORB 与 VBATS ADC，按函数表 poll/watch/reopen。
 *
 * 不另起电池线程。uORB read（含 I2C）和 ADC 都在本线程；g_lock 只保护
 * I2C 缓存、fd 与 supervisor。ADC 第一次只 TRIGGER，之后先 read 再
 * TRIGGER，结果双槽乒乓发布；sys/UI/BLE 只拷贝已发布槽。故障只重开
 * 对应那一路；从未出现过的气压/磁力计不轮询，避免拖死总线。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_board_sensor.h"
#include "myvendor_mono.h"

#ifdef CONFIG_ADC
#  include "eta9184.h"
#  include "myvendor_bat_soc.h"
#  include "myvendor_devctl.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#ifdef CONFIG_ADC
#  include <sys/ioctl.h>
#  include <nuttx/analog/adc.h>
#  include <nuttx/analog/ioctl.h>
#endif

#if defined(CONFIG_BOARD_BMI270) || defined(CONFIG_BOARD_BMP388) || \
    defined(CONFIG_BOARD_MMC5983MA)
#  include <nuttx/sensors/sensor.h>
#  include <nuttx/uorb.h>
#endif

#ifdef CONFIG_BOARD_BMI270
#  include "sf32lb52_bmi270.h"
#endif

#define LOGI(fmt, ...) syslog(LOG_INFO, "board_sensor: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) syslog(LOG_ERR, "board_sensor: " fmt "\n", ##__VA_ARGS__)

#define I2C_SENSOR_STALE_MS          3000u
#define I2C_SENSOR_IMU_MS            100u
#define I2C_SENSOR_G                 9.80665f
#define I2C_SENSOR_ACCEL_LP_A        0.16f
#define I2C_SENSOR_G_MIN             (0.72f * I2C_SENSOR_G)
#define I2C_SENSOR_G_MAX             (1.32f * I2C_SENSOR_G)
#define I2C_SENSOR_BARO_MS           500u
#define I2C_SENSOR_MAG_MS            500u
#define I2C_SENSOR_LOOP_MS           50u
#define I2C_SENSOR_IMU_BOOT_US       30000u
#define I2C_SENSOR_FAULT_WINDOW_MS   60000u
#define I2C_SENSOR_FAULT_THRESHOLD   3u
#define I2C_SENSOR_FAULT_SPACING_MS  2000u
#define I2C_SENSOR_STALL_MS          8000u
#define I2C_SENSOR_RETRY_MS          8000u
#define I2C_SENSOR_REOPEN_GAP_US     50000u
#define I2C_SENSOR_STACK_SIZE        4096u
#define I2C_SENSOR_PRIO              50
#define BOARD_SENSOR_BAT_MS          2000u
#define BOARD_SENSOR_BAT_EMA         8
#define BOARD_SENSOR_BAT_HYST        2
#define BOARD_SENSOR_ADC_PATH        "/dev/adc0"

/** @brief 满电校准要"条件连续成立"多久才允许改一次（毫秒）。
 *
 *  满充观测窗口（`MYVENDOR_DEVCTL_BAT_FULL_OBS_MIN..MAX`）很宽，而单次观测本身
 *  就有几十 mV 的移动，死区却只有 50 mV ⇒ 只要充电器还报"充满"，**每一拍（2 s）
 *  都可能重写一次**，`full` 就跟着读数走：现场实测 12 s 里 4397→4061 连走
 *  340 mV，全会话 264 条 `ctl: bat full cal`。而 `full` 是电量曲线拉伸的轴，
 *  它被推着走 = 显示的百分比被推着走。
 *
 *  所以改成：**充满判定 + 读数落在观测窗口内连续成立这么久，才提交一次**，
 *  提交后重新计时；中间任意一拍不成立就清零重来。
 *  60 s ≈ 4 倍电压 EMA（`BOARD_SENSOR_BAT_EMA`=8 × 2 s）的时间常数，读数已经稳了，
 *  而一次真正充满会持续很久，这个门槛不苛刻。 */
#define BOARD_SENSOR_BAT_CAL_HOLD_MS 60000u

enum i2c_read_kind
{
  I2C_READ_DATA = 0,
  I2C_READ_EMPTY,
  I2C_READ_ERR,
};

enum board_lane
{
  BOARD_LANE_IMU = 0,
  BOARD_LANE_BARO,
  BOARD_LANE_MAG,
  BOARD_LANE_BAT,
  BOARD_LANE_COUNT,
};

struct board_fault_window
{
  uint32_t first_ms;
  uint32_t last_ms;
  uint8_t  count;
};

struct board_lane_rt
{
  bool present;
  bool pending;
  bool busy;
  uint32_t retry_at;
  uint32_t last_ms;
};

struct board_lane_ops
{
  const char *name;
  uint32_t period_ms;
  bool always_retry;
  int  (*open)(void);
  void (*take_locked)(void);
  void (*close)(void);
  void (*install_locked)(bool ok);
  void (*poll)(uint32_t now_ms);
  void (*watch_locked)(uint32_t now_ms);
  void (*reopen_begin_locked)(void);
  void (*recover_hw)(void);
};

#if defined(CONFIG_BOARD_BMI270) || defined(CONFIG_BOARD_BMP388) || \
    defined(CONFIG_BOARD_MMC5983MA) || defined(CONFIG_ADC)

static uint8_t g_stack[I2C_SENSOR_STACK_SIZE] __attribute__((aligned(16)));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_started;
static bool g_running;

static myvendor_sys_vec3_t g_accel;
static myvendor_sys_vec3_t g_accel_f;
static myvendor_sys_vec3_t g_gyro;
static bool g_accel_f_have;
static myvendor_sys_baro_t g_baro;
static myvendor_sys_mag_t  g_mag;
static uint32_t g_accel_ms;
static uint32_t g_gyro_ms;
static uint32_t g_baro_ms;
static uint32_t g_mag_ms;

static int g_accel_fd = -1;
static int g_gyro_fd  = -1;
static int g_baro_fd  = -1;
static int g_mag_fd   = -1;

static int g_hold_accel = -1;
static int g_hold_gyro  = -1;
static int g_hold_baro  = -1;
static int g_hold_mag   = -1;
static int g_new_accel  = -1;
static int g_new_gyro   = -1;
static int g_new_baro   = -1;
static int g_new_mag    = -1;

#ifdef CONFIG_ADC
struct board_bat_slot
{
  int mv;
  int pct;
  bool ok;
};

static struct board_bat_slot g_bat_slot[2];
static volatile uint8_t g_bat_pub;
static int g_adc_fd = -1;
static int g_adc_hold = -1;
static int g_adc_new = -1;
static bool g_adc_conv;
static int g_bat_ema = -1;
static int g_bat_pct_ui = -1;
static uint32_t g_bat_ms;
/** @brief 满电校准"连续成立"的累计时长（ms）与上一拍时刻，见 bat_maybe_cal()。 */
static uint32_t g_bat_cal_hold_ms;
static uint32_t g_bat_cal_last_ms;
#endif

static struct board_fault_window g_fault_win[BOARD_LANE_COUNT];
static uint16_t g_fault_tot[BOARD_LANE_COUNT];
static uint32_t g_stall_ms[BOARD_LANE_COUNT];
static struct board_lane_rt g_lane[BOARD_LANE_COUNT];
static uint16_t g_reopen_cycles;

static uint32_t sensor_now_ms(void)
{
  return myvendor_mono_ms();
}

static bool sensor_lane_may_retry(uint8_t id);

static const char *lane_name(uint8_t id)
{
  static const char *const names[BOARD_LANE_COUNT] =
    {
      [BOARD_LANE_IMU] = "imu",
      [BOARD_LANE_BARO] = "baro",
      [BOARD_LANE_MAG] = "mag",
      [BOARD_LANE_BAT] = "bat",
    };

  if (id >= BOARD_LANE_COUNT)
    {
      return "unknown";
    }

  return names[id];
}

static void close_fd(int *fd)
{
  if (fd != NULL && *fd >= 0)
    {
      close(*fd);
      *fd = -1;
    }
}

static int sensor_open_one(const char *path, unsigned boot_us)
{
  int fd;

  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      return -1;
    }

  if (boot_us > 0)
    {
      usleep(boot_us);
    }

  return fd;
}

static enum i2c_read_kind sensor_classify_read(ssize_t n, size_t expect)
{
  if (n == (ssize_t)expect)
    {
      return I2C_READ_DATA;
    }

  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    {
      return I2C_READ_EMPTY;
    }

  return I2C_READ_ERR;
}

/** 已就 NULL 缓冲拒绝过一次读取（只报一次，避免刷屏）。 */
static bool g_null_read_seen;

/**
 * @brief 读一个 uORB sensor 节点，入口挡掉 NULL 缓冲。
 *
 * @details
 * 上游 `nuttx/drivers/sensors/sensor.c` 的 `sensor_read()` 在 fetch 模式分支：
 *
 * @code
 *     nxrmutex_lock(&upper->lock);
 *     if (lower->ops->fetch)
 *       {
 *         if (buffer == NULL)
 *           {
 *             return -EINVAL;      // 没解锁就 return
 *           }
 * @endcode
 *
 * 一次 `read(fd, NULL, n)` 会永久泄漏该节点的递归互斥量：只有发起这次调用的
 * 线程还能重入，其它线程会全部阻塞在该节点的 read/open/close/ioctl 上，表现为
 * "这个传感器突然只对某一个线程可用"。
 *
 * 对 **push** 模式设备，`buffer == NULL` 反而是上游认可的取尺寸惯用法
 * （`sensor_read()` 的 `lower->persist` 分支返回 `state.esize`），只是 fetch
 * 分支既没沿用这个写法、又忘了解锁 —— 所以按惯用法做尺寸探测的通用代码一碰
 * fetch 节点就中招。本板读的 accel/gyro/baro/mag 全是 fetch 节点。
 *
 * 这里是**板级自我保护**，挡不住第三方代码直接调 `read()`；要根治得用
 * vela_override 替换 `drivers/sensors/sensor.c`。
 *
 * 不在生产路径上断言/panic（与 sched 那两处 sem_waitirq / sem_post 的处理
 * 保持一致）：返回 -EINVAL 并记一次 ERR，让自测能发现，而不是把节点搞死。
 *
 * @param fd 传感器节点 fd。
 * @param buf 输出缓冲；NULL 视为调用方编码错误。
 * @param len 期望字节数。
 * @return read() 的返回值；参数非法时为 -EINVAL。
 */
static ssize_t sensor_node_read(int fd, void *buf, size_t len)
{
  if (fd < 0 || buf == NULL || len == 0u)
    {
      if (!g_null_read_seen)
        {
          g_null_read_seen = true;
          LOGE("refuse read(fd=%d buf=%s len=%u): upstream sensor_read() "
               "leaks upper->lock on a NULL buffer",
               fd, (buf == NULL) ? "NULL" : "ok", (unsigned)len);
        }

      return -EINVAL;
    }

  return read(fd, buf, len);
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
  return deadline_ms == 0 ||
         (int32_t)(now_ms - deadline_ms) >= 0;
}

static void sensor_copy_vec3(myvendor_sys_vec3_t *dst,
                             const myvendor_sys_vec3_t *src,
                             uint32_t stamp_ms, uint32_t now_ms)
{
  if (dst == NULL)
    {
      return;
    }

  if (src != NULL && src->valid && stamp_ms != 0 &&
      myvendor_mono_elapsed_ms(now_ms, stamp_ms) < I2C_SENSOR_STALE_MS)
    {
      *dst = *src;
      return;
    }

  memset(dst, 0, sizeof(*dst));
}

static void sensor_copy_baro(myvendor_sys_baro_t *dst,
                             const myvendor_sys_baro_t *src,
                             uint32_t stamp_ms, uint32_t now_ms)
{
  if (dst == NULL)
    {
      return;
    }

  if (src != NULL && src->valid && stamp_ms != 0 &&
      myvendor_mono_elapsed_ms(now_ms, stamp_ms) < I2C_SENSOR_STALE_MS)
    {
      *dst = *src;
      return;
    }

  memset(dst, 0, sizeof(*dst));
}

static void sensor_copy_mag(myvendor_sys_mag_t *dst,
                            const myvendor_sys_mag_t *src,
                            uint32_t stamp_ms, uint32_t now_ms)
{
  if (dst == NULL)
    {
      return;
    }

  if (src != NULL && src->valid && stamp_ms != 0 &&
      myvendor_mono_elapsed_ms(now_ms, stamp_ms) < I2C_SENSOR_STALE_MS)
    {
      *dst = *src;
      return;
    }

  memset(dst, 0, sizeof(*dst));
}

/** @brief 调用方已持有 g_lock。 */
static void sensor_report_fault_locked(uint8_t id, uint32_t now_ms)
{
  struct board_fault_window *win;

  if (id >= BOARD_LANE_COUNT)
    {
      return;
    }

  win = &g_fault_win[id];
  if (win->count == 0 ||
      (uint32_t)(now_ms - win->first_ms) > I2C_SENSOR_FAULT_WINDOW_MS)
    {
      win->first_ms = now_ms;
      win->last_ms = 0;
      win->count = 0;
    }

  if (win->last_ms != 0 &&
      (uint32_t)(now_ms - win->last_ms) < I2C_SENSOR_FAULT_SPACING_MS)
    {
      return;
    }

  win->last_ms = now_ms;
  if (win->count < UINT8_MAX)
    {
      win->count++;
    }

  if (g_fault_tot[id] < UINT16_MAX)
    {
      g_fault_tot[id]++;
    }

  if (win->count >= I2C_SENSOR_FAULT_THRESHOLD &&
      sensor_lane_may_retry(id) &&
      !g_lane[id].busy && !g_lane[id].pending &&
      (g_lane[id].retry_at == 0 ||
       deadline_reached(now_ms, g_lane[id].retry_at)))
    {
      g_lane[id].pending = true;
      g_lane[id].retry_at = 0;
      LOGE("request reopen %s window=%u total=%u",
           lane_name(id), (unsigned)win->count,
           (unsigned)g_fault_tot[id]);
    }
}

static void sensor_lane_finish_locked(uint8_t id, bool ok, uint32_t now_ms)
{
  g_lane[id].busy = false;
  g_lane[id].pending = false;
  if (ok)
    {
      g_lane[id].present = true;
    }

  g_stall_ms[id] = 0;
  if (ok)
    {
      memset(&g_fault_win[id], 0, sizeof(g_fault_win[id]));
      g_lane[id].retry_at = 0;
      LOGI("reopen %s ok, cycles=%u",
           lane_name(id), (unsigned)g_reopen_cycles);
      return;
    }

  g_lane[id].retry_at = now_ms + I2C_SENSOR_RETRY_MS;
  LOGE("reopen %s failed, retry in %u ms",
       lane_name(id), (unsigned)I2C_SENSOR_RETRY_MS);
}

/** @brief 调用方已持有 g_lock。按时间戳判断停滞，保留旧样本不算故障。 */
static void sensor_watch_stale_locked(uint8_t id, bool want,
                                      uint32_t stamp_ms, uint32_t now_ms)
{
  uint32_t *stall;

  if (!want || g_lane[id].busy)
    {
      g_stall_ms[id] = 0;
      return;
    }

  if (stamp_ms != 0 &&
      myvendor_mono_elapsed_ms(now_ms, stamp_ms) < I2C_SENSOR_STALL_MS)
    {
      g_stall_ms[id] = 0;
      return;
    }

  stall = &g_stall_ms[id];
  if (*stall == 0)
    {
      *stall = now_ms;
      return;
    }

  if ((uint32_t)(now_ms - *stall) >= I2C_SENSOR_STALL_MS)
    {
      *stall = now_ms;
      sensor_report_fault_locked(id, now_ms);
    }
}

#ifdef CONFIG_ADC
static int bat_charging(void)
{
  return eta9184_power_state() == ETA9184_POWER_CHARGING;
}

static int bat_pct_from_mv(int mv)
{
  int full;
  int charging;
  int full_io;

  if (mv < 0)
    {
      return -1;
    }

  full = myvendor_devctl_bat_full_mv_get();
  charging = bat_charging();
  full_io = eta9184_charge_full() ? 1 : 0;
  return myvendor_bat_soc_pct(mv, full, charging, full_io);
}

/**
 * @brief 用滤波后的毫伏得到产品百分比（回差）。
 *
 * 不因充满 IO 钉 100%：该 IO 与空闲相同（STAT 高 + DISCHRG 高），钉死会
 * 让 3.98 V 仍显示满电。真满电由 OCV 曲线映射到 100%。
 */
static int bat_pct_ui(int filt_mv)
{
  int pct;
  int charging;

  if (filt_mv < 0)
    {
      return g_bat_pct_ui;
    }

  pct = bat_pct_from_mv(filt_mv);
  if (pct < 0)
    {
      return g_bat_pct_ui;
    }

  charging = bat_charging();
  if (charging && g_bat_pct_ui >= 0 && pct < g_bat_pct_ui)
    {
      return g_bat_pct_ui;
    }

  if (g_bat_pct_ui < 0)
    {
      g_bat_pct_ui = pct;
      return pct;
    }

  if (pct >= g_bat_pct_ui + BOARD_SENSOR_BAT_HYST ||
      pct <= g_bat_pct_ui - BOARD_SENSOR_BAT_HYST)
    {
      g_bat_pct_ui = pct;
    }

  return g_bat_pct_ui;
}

/**
 * @brief 满电校准：门条件**连续成立** `BOARD_SENSOR_BAT_CAL_HOLD_MS` 之后才提交一次。
 *
 * 门条件（两条入口，任一成立即可）：
 *   ① 坐在充满电 IO 上（电平，或刚到的上升沿）；
 *   ② 充电中、且 KV 还是旧默认（< `MYVENDOR_BAT_SOC_STALE_FULL_MAX`）而电压已到 CV。
 * 无论走哪条，`mv` 都必须落在满充观测窗口内 —— 窗口本身就挡掉了"使用中的 3.98 V"。
 *
 * 这两条以前是**每一拍都直接提交**，于是充满期间 `full` 被读数推着走（见
 * `BOARD_SENSOR_BAT_CAL_HOLD_MS` 的注释）；现在统一由这里的连续计时把关。
 *
 * @param mv     本拍电压（mV，已过 EMA）。
 * @param now_ms 本拍时刻，用来累计真实经过的时间（轮询会被监督器跳拍，
 *               所以不能按"每次 +2000 ms"算）。
 */
static void bat_maybe_cal(int mv, uint32_t now_ms)
{
  uint32_t dt;
  bool valid;
  bool armed;
  int stored;

  /* 不管本拍是否成立，都先把"上一拍到现在"备好：不成立时这段会被丢掉。 */
  dt = (g_bat_cal_last_ms != 0u && now_ms > g_bat_cal_last_ms)
           ? now_ms - g_bat_cal_last_ms
           : 0u;
  g_bat_cal_last_ms = now_ms;

  if (mv >= MYVENDOR_DEVCTL_BAT_FULL_OBS_MIN &&
      mv <= MYVENDOR_DEVCTL_BAT_FULL_OBS_MAX)
    {
      valid = true;
    }
  else
    {
      valid = false;
    }

  armed = valid && (eta9184_take_full_edge() || eta9184_charge_full());

  if (!armed && valid && bat_charging() && !myvendor_devctl_bat_full_have())
    {
      stored = myvendor_devctl_bat_full_mv_get();
      armed = (stored < MYVENDOR_BAT_SOC_STALE_FULL_MAX &&
               mv >= MYVENDOR_BAT_SOC_STALE_OBS_MIN);
    }

  if (!armed)
    {
      g_bat_cal_hold_ms = 0u;
      return;
    }

  g_bat_cal_hold_ms += dt;
  if (g_bat_cal_hold_ms < BOARD_SENSOR_BAT_CAL_HOLD_MS)
    {
      return;
    }

  myvendor_devctl_bat_full_note(mv);
  g_bat_cal_hold_ms = 0u;   /* 重新计时：改一次至少再等一个完整窗口 */
}

static void bat_publish(int mv, int pct, uint32_t now_ms)
{
  uint8_t w = (uint8_t)(g_bat_pub ^ 1u);
  struct board_bat_slot slot;

  slot.mv = mv;
  slot.pct = pct;
  slot.ok = true;
  g_bat_slot[w] = slot;
  __asm__ volatile ("" ::: "memory");
  g_bat_pub = w;
  g_bat_ms = now_ms;
}

static struct board_bat_slot bat_read_pub(void)
{
  uint8_t i;
  struct board_bat_slot slot;

  i = g_bat_pub;
  __asm__ volatile ("" ::: "memory");
  if (i > 1u)
    {
      i = 0u;
    }

  slot = g_bat_slot[i];
  return slot;
}

static int bat_open(void)
{
  g_adc_new = open(BOARD_SENSOR_ADC_PATH, O_RDONLY);
  g_adc_conv = false;
  return g_adc_new >= 0 ? 0 : -1;
}

static void bat_take_locked(void)
{
  g_adc_hold = g_adc_fd;
  g_adc_fd = -1;
  g_adc_conv = false;
}

static void bat_close(void)
{
  close_fd(&g_adc_hold);
  close_fd(&g_adc_new);
  close_fd(&g_adc_fd);
  g_adc_conv = false;
}

static void bat_install_locked(bool ok)
{
  g_adc_fd = ok ? g_adc_new : -1;
  g_adc_new = -1;
  g_adc_conv = false;
}

static void bat_fail_fd(int fd)
{
  pthread_mutex_lock(&g_lock);
  if (g_adc_fd == fd)
    {
      g_adc_fd = -1;
    }

  pthread_mutex_unlock(&g_lock);
  if (fd >= 0)
    {
      close(fd);
    }

  g_adc_conv = false;
}

/**
 * @brief 第一次只 TRIGGER；之后先 read 上次转换，再 TRIGGER 下一次。
 *
 * 不要在 TRIGGER 后立刻 read：FIFO 空时会 SemWait。转换结果在 FIFO 里
 * 等到下一拍再取，本拍只启动下一次转换。
 */
static void bat_poll(uint32_t now_ms)
{
  int fd;
  int ret;
  ssize_t n;
  struct adc_msg_s msg;
  int mv;

  pthread_mutex_lock(&g_lock);
  fd = g_adc_fd;
  pthread_mutex_unlock(&g_lock);
  if (fd < 0)
    {
      return;
    }

  if (g_adc_conv)
    {
      n = read(fd, &msg, sizeof(msg));
      if (n != (ssize_t)sizeof(msg))
        {
          bat_fail_fd(fd);
          return;
        }

      mv = (int)msg.am_data;
      if (g_bat_ema < 0)
        {
          g_bat_ema = mv;
        }
      else
        {
          g_bat_ema += (mv - g_bat_ema) / BOARD_SENSOR_BAT_EMA;
        }

      bat_maybe_cal(g_bat_ema, now_ms);
      (void)bat_pct_ui(g_bat_ema);
      bat_publish(mv, g_bat_pct_ui, now_ms);
    }
  else
    {
      (void)ioctl(fd, ANIOC_RESET_FIFO, 0);
    }

  ret = ioctl(fd, ANIOC_TRIGGER, 0);
  if (ret < 0)
    {
      bat_fail_fd(fd);
      return;
    }

  g_adc_conv = true;
}

static void bat_watch_locked(uint32_t now_ms)
{
  sensor_watch_stale_locked(BOARD_LANE_BAT,
                            g_adc_fd >= 0 && !g_lane[BOARD_LANE_BAT].busy,
                            g_bat_ms, now_ms);
}
#endif

#ifdef CONFIG_BOARD_BMI270
static int imu_open(void)
{
  g_new_accel = sensor_open_one("/dev/uorb/sensor_accel0",
                                I2C_SENSOR_IMU_BOOT_US);
  g_new_gyro  = sensor_open_one("/dev/uorb/sensor_gyro0", 0);
  if (g_new_accel < 0 || g_new_gyro < 0)
    {
      LOGE("open IMU uORB failed accel=%d gyro=%d",
           g_new_accel, g_new_gyro);
      close_fd(&g_new_accel);
      close_fd(&g_new_gyro);
      return -1;
    }

  return 0;
}

static void imu_take_locked(void)
{
  g_hold_accel = g_accel_fd;
  g_hold_gyro  = g_gyro_fd;
  g_accel_fd = -1;
  g_gyro_fd  = -1;
}

static void imu_close(void)
{
  close_fd(&g_hold_accel);
  close_fd(&g_hold_gyro);
  close_fd(&g_new_accel);
  close_fd(&g_new_gyro);
}

static void imu_install_locked(bool ok)
{
  g_accel_fd = ok ? g_new_accel : -1;
  g_gyro_fd  = ok ? g_new_gyro : -1;
  g_new_accel = -1;
  g_new_gyro  = -1;
}

static void imu_reopen_begin_locked(void)
{
  g_accel_f_have = false;
  g_accel_f.valid = false;
}

static void imu_recover_hw(void)
{
  int hw = sf32lb52_bmi270_recover();

  LOGI("bmi270 recover ret=%d", hw);
}

static void sensor_accel_lp_locked(float x, float y, float z)
{
  float n = sqrtf(x * x + y * y + z * z);

  if (n < I2C_SENSOR_G_MIN || n > I2C_SENSOR_G_MAX)
    {
      return;
    }

  if (!g_accel_f_have)
    {
      g_accel_f.x = x;
      g_accel_f.y = y;
      g_accel_f.z = z;
      g_accel_f.valid = true;
      g_accel_f_have = true;
      return;
    }

  g_accel_f.x += I2C_SENSOR_ACCEL_LP_A * (x - g_accel_f.x);
  g_accel_f.y += I2C_SENSOR_ACCEL_LP_A * (y - g_accel_f.y);
  g_accel_f.z += I2C_SENSOR_ACCEL_LP_A * (z - g_accel_f.z);
}

static void imu_poll(uint32_t now_ms)
{
  int accel_fd;
  int gyro_fd;
  struct sensor_accel accel;
  struct sensor_gyro gyro;
  enum i2c_read_kind accel_kind = I2C_READ_EMPTY;
  enum i2c_read_kind gyro_kind = I2C_READ_EMPTY;
  ssize_t n;

  pthread_mutex_lock(&g_lock);
  accel_fd = g_accel_fd;
  gyro_fd  = g_gyro_fd;
  pthread_mutex_unlock(&g_lock);
  if (accel_fd < 0 || gyro_fd < 0)
    {
      return;
    }

  n = sensor_node_read(accel_fd, &accel, sizeof(accel));
  accel_kind = sensor_classify_read(n, sizeof(accel));
  n = sensor_node_read(gyro_fd, &gyro, sizeof(gyro));
  gyro_kind = sensor_classify_read(n, sizeof(gyro));

  pthread_mutex_lock(&g_lock);
  if (accel_kind == I2C_READ_DATA)
    {
      g_accel.valid = true;
      g_accel.x = accel.x;
      g_accel.y = accel.y;
      g_accel.z = accel.z;
      g_accel_ms = now_ms;
      sensor_accel_lp_locked(accel.x, accel.y, accel.z);
    }
  else if (accel_kind == I2C_READ_ERR)
    {
      sensor_report_fault_locked(BOARD_LANE_IMU, now_ms);
    }

  if (gyro_kind == I2C_READ_DATA)
    {
      g_gyro.valid = true;
      g_gyro.x = gyro.x;
      g_gyro.y = gyro.y;
      g_gyro.z = gyro.z;
      g_gyro_ms = now_ms;
    }
  else if (gyro_kind == I2C_READ_ERR)
    {
      sensor_report_fault_locked(BOARD_LANE_IMU, now_ms);
    }

  pthread_mutex_unlock(&g_lock);
}

static void imu_watch_locked(uint32_t now_ms)
{
  sensor_watch_stale_locked(BOARD_LANE_IMU,
                            g_accel_fd >= 0 && !g_lane[BOARD_LANE_IMU].busy,
                            g_accel_ms, now_ms);
}
#endif

#ifdef CONFIG_BOARD_BMP388
static int baro_open(void)
{
  g_new_baro = sensor_open_one("/dev/uorb/sensor_baro0", 0);
  if (g_new_baro < 0)
    {
      LOGI("baro uORB absent, continue without BMP388");
      return -1;
    }

  return 0;
}

static void baro_take_locked(void)
{
  g_hold_baro = g_baro_fd;
  g_baro_fd = -1;
}

static void baro_close(void)
{
  close_fd(&g_hold_baro);
  close_fd(&g_new_baro);
}

static void baro_install_locked(bool ok)
{
  g_baro_fd = ok ? g_new_baro : -1;
  g_new_baro = -1;
}

static void baro_poll(uint32_t now_ms)
{
  int fd;
  struct sensor_baro baro;
  enum i2c_read_kind kind;
  ssize_t n;

  pthread_mutex_lock(&g_lock);
  fd = g_baro_fd;
  pthread_mutex_unlock(&g_lock);
  if (fd < 0)
    {
      return;
    }

  n = sensor_node_read(fd, &baro, sizeof(baro));
  kind = sensor_classify_read(n, sizeof(baro));

  pthread_mutex_lock(&g_lock);
  if (kind == I2C_READ_DATA)
    {
      g_baro.valid = true;
      g_baro.hpa = baro.pressure;
      g_baro.temp_c = baro.temperature;
      g_baro_ms = now_ms;
    }
  else if (kind == I2C_READ_ERR)
    {
      sensor_report_fault_locked(BOARD_LANE_BARO, now_ms);
    }

  pthread_mutex_unlock(&g_lock);
}

static void baro_watch_locked(uint32_t now_ms)
{
  sensor_watch_stale_locked(BOARD_LANE_BARO,
                            g_baro_fd >= 0 && !g_lane[BOARD_LANE_BARO].busy,
                            g_baro_ms, now_ms);
}
#endif

#ifdef CONFIG_BOARD_MMC5983MA
static int mag_open(void)
{
  g_new_mag = sensor_open_one("/dev/uorb/sensor_mag0", 0);
  if (g_new_mag < 0)
    {
      LOGI("mag uORB absent, continue without MMC5983");
      return -1;
    }

  return 0;
}

static void mag_take_locked(void)
{
  g_hold_mag = g_mag_fd;
  g_mag_fd = -1;
}

static void mag_close(void)
{
  close_fd(&g_hold_mag);
  close_fd(&g_new_mag);
}

static void mag_install_locked(bool ok)
{
  g_mag_fd = ok ? g_new_mag : -1;
  g_new_mag = -1;
}

static void mag_poll(uint32_t now_ms)
{
  int fd;
  struct sensor_mag mag;
  enum i2c_read_kind kind;
  ssize_t n;

  pthread_mutex_lock(&g_lock);
  fd = g_mag_fd;
  pthread_mutex_unlock(&g_lock);
  if (fd < 0)
    {
      return;
    }

  n = sensor_node_read(fd, &mag, sizeof(mag));
  kind = sensor_classify_read(n, sizeof(mag));

  pthread_mutex_lock(&g_lock);
  if (kind == I2C_READ_DATA)
    {
      g_mag.valid = true;
      g_mag.x = mag.x;
      g_mag.y = mag.y;
      g_mag.z = mag.z;
      g_mag.temp_c = mag.temperature;
      g_mag_ms = now_ms;
    }
  else if (kind == I2C_READ_ERR)
    {
      sensor_report_fault_locked(BOARD_LANE_MAG, now_ms);
    }

  pthread_mutex_unlock(&g_lock);
}

static void mag_watch_locked(uint32_t now_ms)
{
  sensor_watch_stale_locked(BOARD_LANE_MAG,
                            g_mag_fd >= 0 && !g_lane[BOARD_LANE_MAG].busy,
                            g_mag_ms, now_ms);
}
#endif

static const struct board_lane_ops g_ops[BOARD_LANE_COUNT] =
{
#ifdef CONFIG_BOARD_BMI270
  [BOARD_LANE_IMU] =
    {
      .name = "imu",
      .period_ms = I2C_SENSOR_IMU_MS,
      .always_retry = true,
      .open = imu_open,
      .take_locked = imu_take_locked,
      .close = imu_close,
      .install_locked = imu_install_locked,
      .poll = imu_poll,
      .watch_locked = imu_watch_locked,
      .reopen_begin_locked = imu_reopen_begin_locked,
      .recover_hw = imu_recover_hw,
    },
#endif
#ifdef CONFIG_BOARD_BMP388
  [BOARD_LANE_BARO] =
    {
      .name = "baro",
      .period_ms = I2C_SENSOR_BARO_MS,
      .always_retry = false,
      .open = baro_open,
      .take_locked = baro_take_locked,
      .close = baro_close,
      .install_locked = baro_install_locked,
      .poll = baro_poll,
      .watch_locked = baro_watch_locked,
    },
#endif
#ifdef CONFIG_BOARD_MMC5983MA
  [BOARD_LANE_MAG] =
    {
      .name = "mag",
      .period_ms = I2C_SENSOR_MAG_MS,
      .always_retry = false,
      .open = mag_open,
      .take_locked = mag_take_locked,
      .close = mag_close,
      .install_locked = mag_install_locked,
      .poll = mag_poll,
      .watch_locked = mag_watch_locked,
    },
#endif
#ifdef CONFIG_ADC
  [BOARD_LANE_BAT] =
    {
      .name = "bat",
      .period_ms = BOARD_SENSOR_BAT_MS,
      .always_retry = true,
      .open = bat_open,
      .take_locked = bat_take_locked,
      .close = bat_close,
      .install_locked = bat_install_locked,
      .poll = bat_poll,
      .watch_locked = bat_watch_locked,
    },
#endif
};

static bool sensor_lane_may_retry(uint8_t id)
{
  if (id >= BOARD_LANE_COUNT || g_ops[id].open == NULL)
    {
      return false;
    }

  if (g_ops[id].always_retry)
    {
      return true;
    }

  return g_lane[id].present;
}

static bool sensor_any_lane_busy_or_pending(void)
{
  uint8_t i;

  for (i = 0; i < BOARD_LANE_COUNT; i++)
    {
      if (g_ops[i].open == NULL)
        {
          continue;
        }

      if (g_lane[i].busy || g_lane[i].pending)
        {
          return true;
        }
    }

  return false;
}

static void sensor_supervisor_tick_locked(uint32_t now_ms)
{
  uint8_t i;

  for (i = 0; i < BOARD_LANE_COUNT; i++)
    {
      if (!sensor_lane_may_retry(i) ||
          g_lane[i].busy || g_lane[i].pending ||
          g_lane[i].retry_at == 0 ||
          !deadline_reached(now_ms, g_lane[i].retry_at))
        {
          continue;
        }

      g_lane[i].pending = true;
      g_lane[i].retry_at = 0;
      LOGI("retry %s", lane_name(i));
    }

  for (i = 0; i < BOARD_LANE_COUNT; i++)
    {
      if (g_ops[i].watch_locked != NULL)
        {
          g_ops[i].watch_locked(now_ms);
        }
    }
}

/** @brief close/open 走 I2C activate，必须在 g_lock 外执行。只动这一路。 */
static void sensor_reopen_lane(uint8_t id, uint32_t now_ms)
{
  const struct board_lane_ops *ops;
  bool ok = false;

  if (id >= BOARD_LANE_COUNT)
    {
      return;
    }

  ops = &g_ops[id];
  if (ops->open == NULL || ops->take_locked == NULL ||
      ops->close == NULL || ops->install_locked == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  if (g_lane[id].busy)
    {
      pthread_mutex_unlock(&g_lock);
      return;
    }

  g_lane[id].busy = true;
  g_lane[id].pending = false;
  if (g_reopen_cycles < UINT16_MAX)
    {
      g_reopen_cycles++;
    }

  if (ops->reopen_begin_locked != NULL)
    {
      ops->reopen_begin_locked();
    }

  ops->take_locked();
  pthread_mutex_unlock(&g_lock);

  LOGI("reopen %s begin cycles=%u",
       lane_name(id), (unsigned)g_reopen_cycles);

  ops->close();
  usleep(I2C_SENSOR_REOPEN_GAP_US);
  if (ops->recover_hw != NULL)
    {
      ops->recover_hw();
    }

  ok = (ops->open() == 0);
  pthread_mutex_lock(&g_lock);
  ops->install_locked(ok);
  if (!ok)
    {
      pthread_mutex_unlock(&g_lock);
      ops->close();
      pthread_mutex_lock(&g_lock);
    }

  sensor_lane_finish_locked(id, ok, now_ms);
  pthread_mutex_unlock(&g_lock);
}

static void sensor_open_all(void)
{
  uint8_t i;
  uint32_t now = sensor_now_ms();

  for (i = 0; i < BOARD_LANE_COUNT; i++)
    {
      bool ok;

      if (g_ops[i].open == NULL || g_ops[i].install_locked == NULL)
        {
          continue;
        }

      ok = (g_ops[i].open() == 0);
      pthread_mutex_lock(&g_lock);
      g_ops[i].install_locked(ok);
      g_lane[i].present = ok;
      if (!ok && g_ops[i].always_retry)
        {
          g_lane[i].retry_at = now + I2C_SENSOR_RETRY_MS;
          LOGE("initial open %s failed", lane_name(i));
        }

      pthread_mutex_unlock(&g_lock);
      if (!ok && g_ops[i].close != NULL)
        {
          g_ops[i].close();
        }
    }
}

static void sensor_close_all(void)
{
  uint8_t i;

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < BOARD_LANE_COUNT; i++)
    {
      if (g_ops[i].take_locked != NULL)
        {
          g_ops[i].take_locked();
        }
    }

  pthread_mutex_unlock(&g_lock);

  for (i = 0; i < BOARD_LANE_COUNT; i++)
    {
      if (g_ops[i].close != NULL)
        {
          g_ops[i].close();
        }
    }
}

static void *sensor_thread(void *arg)
{
  (void)arg;

  sensor_open_all();
  pthread_mutex_lock(&g_lock);
  g_running = true;
  pthread_mutex_unlock(&g_lock);

  while (g_running)
    {
      uint32_t now = sensor_now_ms();
      int reopen_id = -1;
      uint8_t i;

      for (i = 0; i < BOARD_LANE_COUNT; i++)
        {
          if (g_ops[i].poll == NULL || g_ops[i].period_ms == 0 ||
              g_lane[i].busy)
            {
              continue;
            }

          if (g_lane[i].last_ms != 0 &&
              (uint32_t)(now - g_lane[i].last_ms) < g_ops[i].period_ms)
            {
              continue;
            }

          g_ops[i].poll(now);
          g_lane[i].last_ms = now;
        }

      pthread_mutex_lock(&g_lock);
      sensor_supervisor_tick_locked(now);
      for (i = 0; i < BOARD_LANE_COUNT; i++)
        {
          if (g_lane[i].pending && !g_lane[i].busy)
            {
              reopen_id = (int)i;
              break;
            }
        }

      pthread_mutex_unlock(&g_lock);

      if (reopen_id >= 0)
        {
          sensor_reopen_lane((uint8_t)reopen_id, now);
        }

      usleep(I2C_SENSOR_LOOP_MS * 1000);
    }

  sensor_close_all();
  return NULL;
}

int myvendor_board_sensor_start(void)
{
  pthread_attr_t attr;
  pthread_t th;
  struct sched_param sp;
  int ret;

  if (g_started)
    {
      return 0;
    }

  pthread_attr_init(&attr);
  ret = pthread_attr_setstack(&attr, g_stack, sizeof(g_stack));
  sp.sched_priority = I2C_SENSOR_PRIO;
#ifdef PTHREAD_EXPLICIT_SCHED
  (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
#endif
  (void)pthread_attr_setschedparam(&attr, &sp);
  if (ret == 0)
    {
      ret = pthread_create(&th, &attr, sensor_thread, NULL);
    }

  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      LOGE("thread create failed %d", ret);
      return -ret;
    }

#ifndef CONFIG_DISABLE_PTHREAD
  pthread_setname_np(th, "board_sensor");
#endif
  pthread_detach(th);
  g_started = true;
  LOGI("board sensor thread started prio=%d", I2C_SENSOR_PRIO);
  return 0;
}

bool myvendor_board_sensor_alive(void)
{
  return g_started && g_running;
}

void myvendor_board_sensor_request_reopen(void)
{
  uint8_t i;

  pthread_mutex_lock(&g_lock);
  if (!g_started)
    {
      pthread_mutex_unlock(&g_lock);
      return;
    }

  for (i = 0; i < BOARD_LANE_COUNT; i++)
    {
      if (g_lane[i].busy || !sensor_lane_may_retry(i))
        {
          continue;
        }

      g_lane[i].pending = true;
      g_lane[i].retry_at = 0;
    }

  LOGI("reopen requested");
  pthread_mutex_unlock(&g_lock);
}

bool myvendor_board_sensor_recovering(void)
{
  bool busy;

  pthread_mutex_lock(&g_lock);
  busy = sensor_any_lane_busy_or_pending();
  pthread_mutex_unlock(&g_lock);
  return busy;
}

void myvendor_board_sensor_accel_get(myvendor_sys_vec3_t *out)
{
  uint32_t now = sensor_now_ms();

  pthread_mutex_lock(&g_lock);
  sensor_copy_vec3(out, &g_accel, g_accel_ms, now);
  pthread_mutex_unlock(&g_lock);
}

void myvendor_board_sensor_accel_filt_get(myvendor_sys_vec3_t *out)
{
  uint32_t now = sensor_now_ms();

  pthread_mutex_lock(&g_lock);
  if (g_accel_f_have)
    {
      sensor_copy_vec3(out, &g_accel_f, g_accel_ms, now);
    }
  else
    {
      sensor_copy_vec3(out, &g_accel, g_accel_ms, now);
    }
  pthread_mutex_unlock(&g_lock);
}

void myvendor_board_sensor_gyro_get(myvendor_sys_vec3_t *out)
{
  uint32_t now = sensor_now_ms();

  pthread_mutex_lock(&g_lock);
  sensor_copy_vec3(out, &g_gyro, g_gyro_ms, now);
  pthread_mutex_unlock(&g_lock);
}

void myvendor_board_sensor_baro_get(myvendor_sys_baro_t *out)
{
  uint32_t now = sensor_now_ms();

  pthread_mutex_lock(&g_lock);
  sensor_copy_baro(out, &g_baro, g_baro_ms, now);
  pthread_mutex_unlock(&g_lock);
}

void myvendor_board_sensor_mag_get(myvendor_sys_mag_t *out)
{
  uint32_t now = sensor_now_ms();

  pthread_mutex_lock(&g_lock);
  sensor_copy_mag(out, &g_mag, g_mag_ms, now);
  pthread_mutex_unlock(&g_lock);
}

int myvendor_board_sensor_bat_mv(void)
{
#ifdef CONFIG_ADC
  struct board_bat_slot slot = bat_read_pub();

  return slot.ok ? slot.mv : -ENODEV;
#else
  return -ENOSYS;
#endif
}

int myvendor_board_sensor_bat_pct(void)
{
#ifdef CONFIG_ADC
  struct board_bat_slot slot = bat_read_pub();

  if (!slot.ok || slot.pct < 0)
    {
      return -1;
    }

  return slot.pct;
#else
  return -1;
#endif
}

#else /* no board I2C sensors and no ADC */

int myvendor_board_sensor_start(void)
{
  return -ENOTSUP;
}

bool myvendor_board_sensor_alive(void)
{
  return false;
}

void myvendor_board_sensor_request_reopen(void)
{
}

bool myvendor_board_sensor_recovering(void)
{
  return false;
}

void myvendor_board_sensor_accel_get(myvendor_sys_vec3_t *out)
{
  if (out != NULL)
    {
      memset(out, 0, sizeof(*out));
    }
}

void myvendor_board_sensor_accel_filt_get(myvendor_sys_vec3_t *out)
{
  myvendor_board_sensor_accel_get(out);
}

void myvendor_board_sensor_gyro_get(myvendor_sys_vec3_t *out)
{
  if (out != NULL)
    {
      memset(out, 0, sizeof(*out));
    }
}

void myvendor_board_sensor_baro_get(myvendor_sys_baro_t *out)
{
  if (out != NULL)
    {
      memset(out, 0, sizeof(*out));
    }
}

void myvendor_board_sensor_mag_get(myvendor_sys_mag_t *out)
{
  if (out != NULL)
    {
      memset(out, 0, sizeof(*out));
    }
}

int myvendor_board_sensor_bat_mv(void)
{
  return -ENOSYS;
}

int myvendor_board_sensor_bat_pct(void)
{
  return -1;
}

#endif
