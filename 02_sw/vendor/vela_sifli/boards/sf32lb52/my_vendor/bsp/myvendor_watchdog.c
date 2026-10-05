/**
 * @file myvendor_watchdog.c
 * @brief 启动 IWDT；CPU（idle 或 work）与 UI 心跳都新鲜时由独立任务喂狗。
 *
 * 不在 SysTick / LPWORK 里喂：中断仍在、调度已死时也必须能复位。
 * idle / UI / work 只打时间戳；ioctl 只放在 iwdg_feed 里。
 *
 * MTP 大文件会长时间占满同优先级任务，idle 进不去；work 心跳表示
 * 传输仍在推进，避免误停喂。SD MTD bread/bwrite 同样只打 work 心跳
 *（不 usleep）：新镜像第一次 mkdir 会 lfs_alloc 扫树，否则 IWDT 在
 * bootinfo 之后复位。work 停而 idle 也停，仍会复位。
 *
 * 新鲜 work 也可顶上 UI：MTP 占住 LittleFS 时自行车循环可能堵在
 * fclose/open 上，ui_beat 停几秒。work 过期后仍要求 UI。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#if defined(CONFIG_MYVENDOR_WATCHDOG) && CONFIG_MYVENDOR_WATCHDOG

#include "myvendor_watchdog.h"
#include "myvendor_coredump.h"

#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <nuttx/timers/watchdog.h>

#include "bf0_hal.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifndef CONFIG_MYVENDOR_WATCHDOG_TIMEOUT_MS
#  define CONFIG_MYVENDOR_WATCHDOG_TIMEOUT_MS 15000
#endif

#ifndef CONFIG_MYVENDOR_WATCHDOG_FEED_MS
#  define CONFIG_MYVENDOR_WATCHDOG_FEED_MS 2000
#endif

#ifndef CONFIG_MYVENDOR_WATCHDOG_HEARTBEAT_MS
#  define CONFIG_MYVENDOR_WATCHDOG_HEARTBEAT_MS 10000
#endif

#ifndef CONFIG_MYVENDOR_WATCHDOG_UI_GRACE_MS
#  define CONFIG_MYVENDOR_WATCHDOG_UI_GRACE_MS 60000
#endif

#define WDT_DEVPATH           "/dev/watchdog0"
#define WDT_FEED_TASK_NAME    "iwdg_feed"
#define WDT_FEED_STACK        2048
#define WDT_FEED_PRIORITY     180

static uint8_t g_wdt_feed_stack[WDT_FEED_STACK]
    __attribute__((aligned(16)));

static volatile clock_t g_idle_beat;
static volatile clock_t g_work_beat;
static volatile clock_t g_ui_beat;
static volatile clock_t g_start_tick;
static volatile bool g_ui_seen;
static volatile bool g_work_seen;
static volatile bool g_started;
static volatile bool g_suppressed;
static volatile bool g_feeding;
static volatile bool g_ui_required;
static volatile bool g_dump_pet;
static clock_t g_busy_yield_tick;

static clock_t wdt_now(void)
{
  return clock_systime_ticks();
}

static uint32_t wdt_age_ms(clock_t last)
{
  clock_t now = wdt_now();
  clock_t dt = now - last;

  /* 滴答回退时 unsigned 下溢，会误判心跳过期而停喂狗。 */
  if (dt > ((clock_t)-1 / 2))
    {
      return 0;
    }

  return (uint32_t)TICK2MSEC(dt);
}

static bool wdt_fresh(clock_t last)
{
  return wdt_age_ms(last) < (uint32_t)CONFIG_MYVENDOR_WATCHDOG_HEARTBEAT_MS;
}

static bool wdt_ui_required_now(void)
{
  if (g_ui_seen)
    {
      return true;
    }

#if defined(CONFIG_MYVENDOR_BICYCLE_AUTOSTART) && CONFIG_MYVENDOR_BICYCLE_AUTOSTART
  return wdt_age_ms(g_start_tick) >=
         (uint32_t)CONFIG_MYVENDOR_WATCHDOG_UI_GRACE_MS;
#else
  return false;
#endif
}

static bool wdt_should_feed(void)
{
  bool cpu_ok;
  bool ui_req;
  bool ui_ok;

  if (g_suppressed)
    {
      return false;
    }

  cpu_ok = wdt_fresh(g_idle_beat) ||
           (g_work_seen && wdt_fresh(g_work_beat));
  ui_req = wdt_ui_required_now();
  ui_ok = !ui_req || (g_ui_seen && wdt_fresh(g_ui_beat)) ||
          (g_work_seen && wdt_fresh(g_work_beat));

  g_ui_required = ui_req;
  return cpu_ok && ui_ok;
}

static int wdt_hw_start_now(void)
{
  int fd;

  fd = open(WDT_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "[wdt] open %s failed errno=%d\n", WDT_DEVPATH, errno);
      return ERROR;
    }

  if (ioctl(fd, WDIOC_SETTIMEOUT,
            (unsigned long)CONFIG_MYVENDOR_WATCHDOG_TIMEOUT_MS) < 0)
    {
      syslog(LOG_WARNING, "[wdt] SETTIMEOUT(%u) errno=%d\n",
             (unsigned)CONFIG_MYVENDOR_WATCHDOG_TIMEOUT_MS, errno);
    }

  if (ioctl(fd, WDIOC_START, 0) < 0)
    {
      syslog(LOG_ERR, "[wdt] START errno=%d\n", errno);
      close(fd);
      return ERROR;
    }

  close(fd);
  g_started = true;
  syslog(LOG_INFO,
         "[wdt] IWDT start timeout=%ums clk=%uHz feed=%ums heartbeat=%ums\n",
         (unsigned)CONFIG_MYVENDOR_WATCHDOG_TIMEOUT_MS,
         (hwp_pmuc->CR & PMUC_CR_SEL_LPCLK) ? 32768u : 9000u,
         (unsigned)CONFIG_MYVENDOR_WATCHDOG_FEED_MS,
         (unsigned)CONFIG_MYVENDOR_WATCHDOG_HEARTBEAT_MS);
  return OK;
}

static int wdt_feed_task(int argc, FAR char *argv[])
{
  uint32_t starve_log = 0;
  clock_t feeder_last = 0;
  bool feeder_have_last = false;
  int fd;

  (void)argc;
  (void)argv;

  fd = open(WDT_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "[wdt] feeder open %s failed errno=%d\n",
             WDT_DEVPATH, errno);
      return ERROR;
    }

  for (; ; )
    {
      /* 喂狗线程自己"回来晚了" —— 系统级停顿的**独立证据**（用户 2026-09-26）。
       *
       * 为什么放这里：它 prio 180、只做喂狗这一件事，任何远大于喂狗周期的间隔都
       * 说明有别的活把 CPU/中断占住了。现场两次 WDT 饥饿复位的形态都是"升档后
       * SD 时钟重配 ⇒ 日志静默窗口长度 == idle/work 心跳停摆长度（11.17 s /
       * 11.98 s），而 ui 心跳只有 10 ms"；当时地图页不可见、泵不跑，[vperf] 那套
       * 探针根本没机会执行 —— 放这里就与页面无关了。
       *
       * ⚠ **只在线程里打**（这里不是中断上下文；喂狗本身走 ioctl，ISR 里绝不打印）。
       * 时间基准沿用本文件既有的 tick（与 wdt_should_feed / wdt_age_ms 同源），
       * 所以它与看门狗判定看到的是同一条时间轴。 */
      {
        clock_t now = clock_systime_ticks();

        if (feeder_have_last)
          {
            uint32_t gap_ms = (uint32_t)TICK2MSEC(now - feeder_last);

            if (gap_ms > (uint32_t)(CONFIG_MYVENDOR_WATCHDOG_FEED_MS + 500))
              {
                syslog(LOG_WARNING,
                       "[wdt] feeder late %ums (>%ums) idle=%ums work=%ums ui=%ums\n",
                       (unsigned)gap_ms,
                       (unsigned)(CONFIG_MYVENDOR_WATCHDOG_FEED_MS + 500),
                       (unsigned)wdt_age_ms(g_idle_beat),
                       g_work_seen ? (unsigned)wdt_age_ms(g_work_beat) : 0u,
                       g_ui_seen ? (unsigned)wdt_age_ms(g_ui_beat) : 0u);
              }
          }
        feeder_last = now;
        feeder_have_last = true;
      }

      bool feed = wdt_should_feed();

      g_feeding = feed;
      if (feed)
        {
          if (ioctl(fd, WDIOC_KEEPALIVE, 0) < 0)
            {
              syslog(LOG_ERR, "[wdt] KEEPALIVE errno=%d\n", errno);
            }

          starve_log = 0;
        }
      else if (starve_log == 0)
        {
          syslog(LOG_ERR,
                 "[wdt] stop feed idle=%ums work=%ums ui=%ums "
                 "ui_seen=%d ui_req=%d suppress=%d\n",
                 (unsigned)wdt_age_ms(g_idle_beat),
                 g_work_seen ? (unsigned)wdt_age_ms(g_work_beat) : 0u,
                 g_ui_seen ? (unsigned)wdt_age_ms(g_ui_beat) : 0u,
                 g_ui_seen ? 1 : 0,
                 g_ui_required ? 1 : 0,
                 g_suppressed ? 1 : 0);
          myvendor_coredump_wdt();
          starve_log = 1;
        }

      usleep((useconds_t)CONFIG_MYVENDOR_WATCHDOG_FEED_MS * 1000u);
    }

  close(fd);
  return OK;
}

void myvendor_watchdog_idle_beat(void)
{
  g_idle_beat = wdt_now();
}

void myvendor_watchdog_ui_beat(void)
{
  g_ui_beat = wdt_now();
  g_ui_seen = true;
}

void myvendor_watchdog_work_beat(void)
{
  g_work_beat = wdt_now();
  g_work_seen = true;
}

/**
 * @brief SysTick / HCLK 换档后同步心跳时间戳。
 *
 * `wdt_age_ms()` 用 `clock_systime_ticks()` + 编译期 `TICK2MSEC`。DVFS
 * 重配 SysTick 后，旧时间戳会算出天文数字年龄（n580：idle_age≈58 min
 * 而 pc 却在 up_idle）→ 误停喂 → 误复位。切频后把三条 beat 写成"现在"，
 * 只抹掉换档造成的断层；之后 10 s 内仍无新 beat 照样饿死复位。
 */
void myvendor_watchdog_tick_reclock(void)
{
  clock_t now = wdt_now();

  g_idle_beat = now;
  if (g_work_seen)
    {
      g_work_beat = now;
    }

  if (g_ui_seen)
    {
      g_ui_beat = now;
    }

  g_start_tick = now;
  g_busy_yield_tick = now;
}

void myvendor_watchdog_busy_pump(void)
{
  myvendor_watchdog_work_beat();

  if (g_busy_yield_tick == 0)
    {
      g_busy_yield_tick = wdt_now();
      return;
    }

  if (wdt_age_ms(g_busy_yield_tick) >= 1000u)
    {
      usleep(10000);
      g_busy_yield_tick = wdt_now();
    }
}

static void wdt_refresh_hw(void);

void myvendor_watchdog_hw_pet(void)
{
  if (!g_started || g_suppressed)
    {
      return;
    }

  wdt_refresh_hw();
}

void myvendor_watchdog_suppress(bool suppress)
{
  g_suppressed = suppress;
  syslog(LOG_WARNING, "[wdt] production feed %s\n",
         suppress ? "paused" : "resumed");
}

static void wdt_refresh_hw(void)
{
  WDT_HandleTypeDef h;

  if (!g_started)
    {
      return;
    }

  memset(&h, 0, sizeof(h));
  h.Instance = hwp_iwdt;
  (void)HAL_WDT_Refresh(&h);
}

void myvendor_watchdog_keepalive_once(void)
{
  if (g_dump_pet)
    {
      return;
    }

  g_dump_pet = true;
  wdt_refresh_hw();
}

void myvendor_watchdog_halt(void)
{
  wdt_refresh_hw();
  __HAL_IWDT_DISABLE();
}

int myvendor_watchdog_get_status(struct myvendor_watchdog_status_s *st)
{
  bool ui_req;

  if (st == NULL)
    {
      return -1;
    }

  ui_req = wdt_ui_required_now();
  st->started = g_started;
  st->suppressed = g_suppressed;
  st->feeding = g_feeding;
  st->idle_ok = wdt_fresh(g_idle_beat);
  st->work_ok = g_work_seen && wdt_fresh(g_work_beat);
  st->ui_seen = g_ui_seen;
  st->ui_required = ui_req;
  st->ui_ok = !ui_req || (g_ui_seen && wdt_fresh(g_ui_beat)) ||
              (g_work_seen && wdt_fresh(g_work_beat));
  st->idle_age_ms = wdt_age_ms(g_idle_beat);
  st->work_age_ms = g_work_seen ? wdt_age_ms(g_work_beat) : 0;
  st->ui_age_ms = g_ui_seen ? wdt_age_ms(g_ui_beat) : 0;
  st->timeout_ms = (uint32_t)CONFIG_MYVENDOR_WATCHDOG_TIMEOUT_MS;
  return 0;
}

void myvendor_watchdog_start(void)
{
  static bool spawned;
  int pid;
  clock_t now = wdt_now();

  if (spawned)
    {
      return;
    }

  g_idle_beat = now;
  g_work_beat = 0;
  g_start_tick = now;
  g_ui_beat = 0;
  g_ui_seen = false;
  g_work_seen = false;
  g_busy_yield_tick = 0;
  g_suppressed = false;
  g_feeding = false;
  g_started = false;

  if (wdt_hw_start_now() < 0)
    {
      return;
    }

  pid = task_create_with_stack(WDT_FEED_TASK_NAME, WDT_FEED_PRIORITY,
                               g_wdt_feed_stack, sizeof(g_wdt_feed_stack),
                               wdt_feed_task, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "[wdt] task_create failed errno=%d\n", errno);
      return;
    }

  spawned = true;
  syslog(LOG_INFO, "[wdt] feeder pid=%d prio=%d\n",
         pid, WDT_FEED_PRIORITY);
}

#endif /* CONFIG_MYVENDOR_WATCHDOG */
