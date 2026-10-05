/**
 * @file myvendor_lcd_disp.c
 * @brief 双条带缓冲 LCD flush：LVGL SRAM 绘缓冲直送 ui_flush worker（无 PSRAM 拷贝）。
 *
 * LCDC QSPI PUTAREA 由 IRQ 完成；无 stall-watchdog 线程。支持 recover 重启 DMA/ worker。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#ifndef CONFIG_MYVENDOR_LCD_DISP_WORKER_STACKSIZE
#  define CONFIG_MYVENDOR_LCD_DISP_WORKER_STACKSIZE 2048
#endif

#include "myvendor_lcd_disp.h"

#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>
#include <nuttx/cache.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#if defined(CONFIG_LCD_USING_ST7789S) && !defined(CONFIG_LCD_ST7789S_SOFTSPI)
/* 后续剔除：Helm One 已改 NV3031A，不再走 lcd_spi2。 */
#include "lcd_spi2.h"
#endif

#ifndef MYVENDOR_LCD_PUTAREA_SLOW_MS
#  define MYVENDOR_LCD_PUTAREA_SLOW_MS 80u
#endif

#ifndef CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS
#  define CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS 60
#endif

#ifndef MYVENDOR_LCD_ABORT_GRACE_MS
#  define MYVENDOR_LCD_ABORT_GRACE_MS 150u
#endif

#ifndef MYVENDOR_LCD_RECOVER_COOLDOWN_MS
#  define MYVENDOR_LCD_RECOVER_COOLDOWN_MS 8000u
#endif

#ifndef MYVENDOR_LCD_RECOVER_SETTLE_US
#  define MYVENDOR_LCD_RECOVER_SETTLE_US 2000u
#endif

struct myvendor_lcd_flush_job_s
{
  bool              valid;
  myvendor_lcd_disp_area_t area;
  const uint8_t    *data;
  uint32_t          stride;
  bool              is_last;
  uint32_t          recover_gen;
};

struct myvendor_lcd_disp_ctx_s
{
  char              lcd_path[64];
  int               lcd_fd;
  uint16_t          hor_res;
  uint16_t          ver_res;
  uint16_t          strip_rows;
  uint32_t          stride;
  size_t            buf_bytes;

  pthread_t         worker;
  pthread_mutex_t   lock;
  pthread_cond_t    job_cond;
  pthread_cond_t    idle_cond;
  bool              sync_inited;
  bool              inited;
  bool              running;
  bool              job_ready;
  struct myvendor_lcd_flush_job_s job;

  myvendor_lcd_disp_done_cb_t done_cb;
  void             *done_user_data;
  myvendor_lcd_disp_refr_cb_t refr_cb;
  myvendor_lcd_disp_unblock_cb_t unblock_cb;
  void             *recover_user_data;

  uint32_t          recover_gen;
  atomic_bool       recover_in_progress;
  atomic_bool       recover_unblock_pending;
  atomic_bool       recover_ui_pending;
  atomic_bool       recover_hold_submit;
  atomic_bool       recover_refr_armed;
  atomic_uint       recover_quiet_until_ms;

  atomic_bool       worker_active;
  atomic_uint       busy_since_ms;

  atomic_uint       stat_submit_ok;
  atomic_uint       stat_submit_busy;
  atomic_uint       stat_submit_err;
  atomic_uint       stat_worker_done;
  atomic_uint       stat_putarea_fail;
  atomic_uint       stat_flush_ready;
  atomic_uint       stat_wait_calls;
  atomic_uint       stat_wait_ms_max;
  atomic_uint       stat_wait_timeout;
  atomic_uint       stat_flush_ready_force;
  atomic_uint       stat_submit_retry;
  atomic_uint       stat_wait_stall;
  atomic_uint       stat_putarea_ms_max;
  atomic_uint       stat_putarea_slow;
  atomic_uint       stat_recover;
};

static struct myvendor_lcd_disp_ctx_s g_lcd_disp =
{
  .lcd_fd = -1,
};

/* recover 可能 detach 卡住的 worker 再立刻起新线程：两块 BSS 轮换。 */
static uint8_t g_ui_flush_stack[2][CONFIG_MYVENDOR_LCD_DISP_WORKER_STACKSIZE]
    __attribute__((aligned(16)));
static atomic_bool g_ui_flush_stack_busy[2];

/** @brief 中止 DMA、重启 worker 与 LCD 设备。 */
static int myvendor_lcd_disp_recover(void);

/** @brief 持锁判断 worker/job/recover 是否忙。 */
static bool lcd_disp_is_busy_locked(void)
{
  return g_lcd_disp.job_ready ||
      atomic_load_explicit(&g_lcd_disp.worker_active, memory_order_acquire) ||
      atomic_load_explicit(&g_lcd_disp.recover_in_progress,
          memory_order_acquire);
}

/** @brief CLOCK_MONOTONIC 毫秒。 */
static uint32_t myvendor_lcd_mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                    (uint64_t)ts.tv_nsec / 1000000u);
}

/** @brief 记录 busy_since 时间戳。 */
static void myvendor_lcd_note_busy(void)
{
  atomic_store_explicit(&g_lcd_disp.busy_since_ms,
      myvendor_lcd_mono_ms(), memory_order_release);
}

/** @brief 刷新 D-Cache（PUTAREA 前）。 */
static void myvendor_lcd_dcache_clean(const void *addr, size_t len)
{
#ifdef CONFIG_ARCH_DCACHE
  if (addr != NULL && len > 0)
    {
      uintptr_t start = (uintptr_t)addr;
      uintptr_t end = start + len;
      up_clean_dcache(start, end);
    }
#else
  (void)addr;
  (void)len;
#endif
}

/** @brief ioctl LCDDEVIO_PUTAREA 提交一条带。 */
static int myvendor_lcd_putarea_locked(const struct myvendor_lcd_flush_job_s *job)
{
  struct lcddev_area_s lcd_area;
  size_t rows;
  size_t nbytes;

  if (g_lcd_disp.lcd_fd < 0 || job == NULL || !job->valid)
    {
      return -EINVAL;
    }

  rows = (size_t)(job->area.y2 - job->area.y1 + 1);
  nbytes = rows * (size_t)job->stride;

  lcd_area.row_start = job->area.y1;
  lcd_area.row_end   = job->area.y2;
  lcd_area.col_start = job->area.x1;
  lcd_area.col_end   = job->area.x2;
  lcd_area.stride    = (fb_coord_t)job->stride;
  lcd_area.data      = (uint8_t *)job->data;

  myvendor_lcd_dcache_clean(job->data, nbytes);

  return ioctl(g_lcd_disp.lcd_fd, LCDDEVIO_PUTAREA,
               (unsigned long)(uintptr_t)&lcd_area);
}

/** @brief 调用 LVGL flush done 回调。 */
static void myvendor_lcd_invoke_done_cb(void)
{
  myvendor_lcd_disp_done_cb_t cb;
  void *user_data;

  pthread_mutex_lock(&g_lcd_disp.lock);
  cb = g_lcd_disp.done_cb;
  user_data = g_lcd_disp.done_user_data;
  pthread_mutex_unlock(&g_lcd_disp.lock);

  if (cb != NULL)
    {
      cb(user_data);
    }
}

/** @brief 调用 recover 全屏 refr 回调。 */
static void myvendor_lcd_invoke_refr_cb(void)
{
  myvendor_lcd_disp_refr_cb_t cb;
  void *user_data;

  pthread_mutex_lock(&g_lcd_disp.lock);
  cb = g_lcd_disp.refr_cb;
  user_data = g_lcd_disp.recover_user_data;
  pthread_mutex_unlock(&g_lcd_disp.lock);

  if (cb != NULL)
    {
      cb(user_data);
    }
}

/** @brief 调用 recover unblock 回调。 */
static void myvendor_lcd_invoke_unblock_cb(void)
{
  myvendor_lcd_disp_unblock_cb_t cb;
  void *user_data;

  pthread_mutex_lock(&g_lcd_disp.lock);
  cb = g_lcd_disp.unblock_cb;
  user_data = g_lcd_disp.recover_user_data;
  pthread_mutex_unlock(&g_lcd_disp.lock);

  if (cb != NULL)
    {
      cb(user_data);
    }
}

/** @brief recover 后标记 UI 工作并唤醒 waiters（不在此调 LVGL）。 */
static void myvendor_lcd_finish_recover_ui(void)
{
  uint32_t now = myvendor_lcd_mono_ms();

  atomic_store_explicit(&g_lcd_disp.recover_quiet_until_ms,
      now + MYVENDOR_LCD_RECOVER_COOLDOWN_MS, memory_order_release);
  atomic_store_explicit(&g_lcd_disp.busy_since_ms, now, memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_unblock_pending, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_ui_pending, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_hold_submit, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_refr_armed, true,
      memory_order_release);
  atomic_store_explicit(&g_lcd_disp.recover_in_progress, false,
      memory_order_release);
  pthread_mutex_lock(&g_lcd_disp.lock);
  pthread_cond_broadcast(&g_lcd_disp.idle_cond);
  pthread_mutex_unlock(&g_lcd_disp.lock);
}

/** @brief LVGL 线程 poll：执行 unblock 回调。 */
void myvendor_lcd_disp_poll_recover_unblock(void)
{
  if (!atomic_exchange_explicit(&g_lcd_disp.recover_unblock_pending, false,
          memory_order_acq_rel))
    {
      return;
    }

  myvendor_lcd_invoke_unblock_cb();
}

/** @brief LVGL 线程 poll：invalidate + refr。 */
void myvendor_lcd_disp_poll_recover_ui(void)
{
  /* unblock 可能已在 flush_wait 执行；此处捕获 idle recover。 */
  myvendor_lcd_disp_poll_recover_unblock();

  if (!atomic_exchange_explicit(&g_lcd_disp.recover_ui_pending, false,
          memory_order_acq_rel))
    {
      return;
    }

  /* 重绘前丢弃中断 refresh 的 stale  partial strip。 */
  atomic_store_explicit(&g_lcd_disp.recover_hold_submit, false,
      memory_order_release);

  /* 仅在 LVGL refresh 外做全 invalidate + refr（loop 顶）。 */
  myvendor_lcd_invoke_refr_cb();
}

/** @brief recover 期间是否抑制 submit。 */
bool myvendor_lcd_disp_recover_hold_active(void)
{
  return atomic_load_explicit(&g_lcd_disp.recover_hold_submit,
      memory_order_acquire);
}

/** @brief 两 timespec 间毫秒差。 */
static uint32_t myvendor_lcd_elapsed_ms(const struct timespec *t0,
    const struct timespec *t1)
{
  time_t sec = t1->tv_sec - t0->tv_sec;
  long nsec = t1->tv_nsec - t0->tv_nsec;

  if (nsec < 0)
    {
      sec--;
      nsec += 1000000000L;
    }

  if (sec < 0)
    {
      return 0;
    }

  return (uint32_t)((uint64_t)sec * 1000u + (uint64_t)nsec / 1000000u);
}

/** @brief timespec 加毫秒。 */
static void myvendor_lcd_timespec_add_ms(struct timespec *ts, uint32_t ms)
{
  ts->tv_sec  += (time_t)(ms / 1000u);
  ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
    }
}

/** @brief ui_flush worker 主循环。 */
static void *myvendor_lcd_worker_entry(void *arg);

/** @brief 持锁等待 job/worker 空闲。 */
static bool myvendor_lcd_wait_idle_locked(uint32_t max_ms,
    uint32_t * waited_ms_out)
{
  struct timespec t0;
  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &t0);

  while (lcd_disp_is_busy_locked())
    {
      if (max_ms == 0)
        {
          pthread_cond_wait(&g_lcd_disp.idle_cond, &g_lcd_disp.lock);
          continue;
        }

      clock_gettime(CLOCK_REALTIME, &now);
      {
        uint32_t elapsed = myvendor_lcd_elapsed_ms(&t0, &now);

        if (elapsed >= max_ms)
          {
            if (waited_ms_out != NULL)
              {
                *waited_ms_out = elapsed;
              }

            return !lcd_disp_is_busy_locked();
          }

        myvendor_lcd_timespec_add_ms(&now, max_ms - elapsed);
      }

      if (pthread_cond_timedwait(&g_lcd_disp.idle_cond, &g_lcd_disp.lock,
              &now) == ETIMEDOUT)
        {
          if (waited_ms_out != NULL)
            {
              clock_gettime(CLOCK_REALTIME, &now);
              *waited_ms_out = myvendor_lcd_elapsed_ms(&t0, &now);
            }

          return !lcd_disp_is_busy_locked();
        }
    }

  if (waited_ms_out != NULL)
    {
      clock_gettime(CLOCK_REALTIME, &now);
      *waited_ms_out = myvendor_lcd_elapsed_ms(&t0, &now);
    }

  return true;
}

/** @brief 关闭并重开 /dev/lcd0。 */
static int myvendor_lcd_reopen_device(void)
{
  if (g_lcd_disp.lcd_fd >= 0)
    {
      close(g_lcd_disp.lcd_fd);
      g_lcd_disp.lcd_fd = -1;
    }

  g_lcd_disp.lcd_fd = open(g_lcd_disp.lcd_path, O_RDWR);
  if (g_lcd_disp.lcd_fd < 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: reopen %s failed errno=%d",
          g_lcd_disp.lcd_path, errno);
      return -errno;
    }

  return 0;
}

/** @brief 创建 ui_flush pthread。 */
static int myvendor_lcd_start_worker(void)
{
  int ret;
  int slot = -1;
  int i;
  pthread_attr_t attr;

  for (i = 0; i < 2; i++)
    {
      bool expected = false;

      if (atomic_compare_exchange_strong_explicit(&g_ui_flush_stack_busy[i],
              &expected, true, memory_order_acq_rel, memory_order_acquire))
        {
          slot = i;
          break;
        }
    }

  if (slot < 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: no free ui_flush stack");
      return -ENOMEM;
    }

  g_lcd_disp.running = true;

  pthread_attr_init(&attr);
  ret = pthread_attr_setstack(&attr, g_ui_flush_stack[slot],
      sizeof(g_ui_flush_stack[slot]));
  if (ret == 0)
    {
      ret = pthread_create(&g_lcd_disp.worker, &attr,
          myvendor_lcd_worker_entry, (void *)(intptr_t)slot);
    }

  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_lcd_disp.running = false;
      atomic_store_explicit(&g_ui_flush_stack_busy[slot], false,
          memory_order_release);
      syslog(LOG_ERR, "myvendor_lcd_disp: worker create failed %d", ret);
      return -ret;
    }

#ifndef CONFIG_DISABLE_PTHREAD
  pthread_setname_np(g_lcd_disp.worker, "ui_flush");
#endif

  return 0;
}

/** @brief 中止进行中的 DMA/SPI 传输。 */
static void myvendor_lcd_abort_hw_xfer(int lcd_fd)
{
#if defined(CONFIG_LCD_USING_ST7789S) && !defined(CONFIG_LCD_ST7789S_SOFTSPI)
  (void)lcd_fd;
  lcd_spi2_abort_active_xfer();
#else
  if (lcd_fd < 0)
    {
      return;
    }

  if (ioctl(lcd_fd, MYVENDOR_LCDDEVIO_ABORT_XFER, 0) < 0)
    {
      syslog(LOG_WARNING, "[lcd_flush] ABORT_XFER ioctl failed errno=%d",
             errno);
    }
#endif
}

/** @brief recover 后重启硬件 pipe。 */
static void myvendor_lcd_restart_hw_pipe(int lcd_fd)
{
#if defined(CONFIG_LCD_USING_ST7789S) && !defined(CONFIG_LCD_ST7789S_SOFTSPI)
  (void)lcd_fd;
  lcd_spi2_restart_xfer_path();
#else
  if (lcd_fd < 0)
    {
      return;
    }

  if (ioctl(lcd_fd, MYVENDOR_LCDDEVIO_RESTART_XFER, 0) < 0)
    {
      syslog(LOG_WARNING, "[lcd_flush] RESTART_XFER ioctl failed errno=%d",
             errno);
    }
#endif
}

/** @brief recover：detach 卡住的 worker（不 cancel）。 */
static void myvendor_lcd_stop_worker_force(void)
{
  pthread_t worker;
  int lcd_fd;

  pthread_mutex_lock(&g_lcd_disp.lock);
  g_lcd_disp.running = false;
  worker = g_lcd_disp.worker;
  lcd_fd = g_lcd_disp.lcd_fd;
  pthread_cond_broadcast(&g_lcd_disp.job_cond);
  pthread_cond_broadcast(&g_lcd_disp.idle_cond);
  pthread_mutex_unlock(&g_lcd_disp.lock);

  if (worker == 0)
    {
      return;
    }

  myvendor_lcd_abort_hw_xfer(lcd_fd);
  usleep(MYVENDOR_LCD_ABORT_GRACE_MS * 1000u);
  myvendor_lcd_abort_hw_xfer(lcd_fd);

  /* Drop /dev/lcd0 so a stuck PUTAREA ioctl can return. */
  if (lcd_fd >= 0)
    {
      close(lcd_fd);
    }

  pthread_mutex_lock(&g_lcd_disp.lock);
  if (g_lcd_disp.lcd_fd == lcd_fd)
    {
      g_lcd_disp.lcd_fd = -1;
    }

  pthread_mutex_unlock(&g_lcd_disp.lock);

  /* Do not pthread_cancel — worker may hold or be unlocking g_lcd_disp.lock.
   * Stale worker exits via pthread_self() != g_lcd_disp.worker after PUTAREA. */
  pthread_detach(worker);

  pthread_mutex_lock(&g_lcd_disp.lock);
  if (g_lcd_disp.worker == worker)
    {
      g_lcd_disp.worker = 0;
    }

  pthread_mutex_unlock(&g_lcd_disp.lock);
  syslog(LOG_ERR, "[lcd_flush] ui_flush detached (recover, no cancel)");
}

/** @brief deinit：join worker。 */
static void myvendor_lcd_stop_worker_join(void)
{
  pthread_t worker;

  pthread_mutex_lock(&g_lcd_disp.lock);
  g_lcd_disp.running = false;
  worker = g_lcd_disp.worker;
  pthread_cond_broadcast(&g_lcd_disp.job_cond);
  pthread_cond_broadcast(&g_lcd_disp.idle_cond);
  pthread_mutex_unlock(&g_lcd_disp.lock);

  if (worker != 0)
    {
      pthread_join(worker, NULL);
      g_lcd_disp.worker = 0;
    }
}

/**
 * End-of-job: clear worker_active and broadcast idle_cond under lock.
 * Returns true if this pthread is still the active ui_flush worker and
 * may continue the job loop.
 */
static bool myvendor_lcd_worker_job_done(
    const struct myvendor_lcd_flush_job_s *local, bool call_done_cb)
{
  bool stale = false;

  if (g_lcd_disp.worker == 0 || pthread_self() != g_lcd_disp.worker)
    {
      stale = true;
    }
  else if (local != NULL &&
           local->recover_gen != g_lcd_disp.recover_gen)
    {
      stale = true;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);

  if (!stale &&
      (pthread_self() != g_lcd_disp.worker ||
       (local != NULL &&
        local->recover_gen != g_lcd_disp.recover_gen) ||
       !g_lcd_disp.running))
    {
      stale = true;
    }

  atomic_store_explicit(&g_lcd_disp.worker_active, false,
      memory_order_release);
  pthread_cond_broadcast(&g_lcd_disp.idle_cond);
  pthread_mutex_unlock(&g_lcd_disp.lock);

  if (stale)
    {
      if (local != NULL &&
          local->recover_gen != g_lcd_disp.recover_gen)
        {
          syslog(LOG_ERR,
              "[lcd_flush] ui_flush stale gen=%u (now %u) — exit",
              (unsigned)local->recover_gen,
              (unsigned)g_lcd_disp.recover_gen);
        }
      else if (g_lcd_disp.worker != 0 &&
               pthread_self() != g_lcd_disp.worker)
        {
          syslog(LOG_ERR, "[lcd_flush] ui_flush stale thread — exit");
        }

      return false;
    }

  atomic_store_explicit(&g_lcd_disp.recover_refr_armed, false,
      memory_order_release);

  if (call_done_cb && local != NULL && local->valid &&
      g_lcd_disp.running &&
      pthread_self() == g_lcd_disp.worker)
    {
      myvendor_lcd_invoke_done_cb();
    }

  return g_lcd_disp.running && pthread_self() == g_lcd_disp.worker;
}

static void *myvendor_lcd_worker_entry(void *arg)
{
  int slot = (int)(intptr_t)arg;

#ifndef CONFIG_DISABLE_PTHREAD
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
#endif

  while (1)
    {
      struct myvendor_lcd_flush_job_s local;

      if (g_lcd_disp.worker == 0 ||
          pthread_self() != g_lcd_disp.worker)
        {
          goto out;
        }

      pthread_mutex_lock(&g_lcd_disp.lock);

      while (g_lcd_disp.running && !g_lcd_disp.job_ready)
        {
          pthread_cond_wait(&g_lcd_disp.job_cond, &g_lcd_disp.lock);
        }

      if (!g_lcd_disp.running)
        {
          pthread_mutex_unlock(&g_lcd_disp.lock);
          goto out;
        }

      if (pthread_self() != g_lcd_disp.worker)
        {
          pthread_mutex_unlock(&g_lcd_disp.lock);
          goto out;
        }

      local = g_lcd_disp.job;
      g_lcd_disp.job_ready = false;
      atomic_store_explicit(&g_lcd_disp.worker_active, true,
          memory_order_release);
      pthread_mutex_unlock(&g_lcd_disp.lock);

      if (local.valid && g_lcd_disp.lcd_fd >= 0)
        {
          struct timespec t0;
          struct timespec t1;
          uint32_t put_ms;
          int ret;

          clock_gettime(CLOCK_MONOTONIC, &t0);
          ret = myvendor_lcd_putarea_locked(&local);
          clock_gettime(CLOCK_MONOTONIC, &t1);

          put_ms = myvendor_lcd_elapsed_ms(&t0, &t1);

          if (put_ms > atomic_load_explicit(&g_lcd_disp.stat_putarea_ms_max,
                  memory_order_relaxed))
            {
              atomic_store_explicit(&g_lcd_disp.stat_putarea_ms_max, put_ms,
                  memory_order_relaxed);
            }

          if (put_ms >= MYVENDOR_LCD_PUTAREA_SLOW_MS)
            {
              atomic_fetch_add_explicit(&g_lcd_disp.stat_putarea_slow, 1,
                  memory_order_relaxed);
              syslog(LOG_WARNING,
                  "[lcd_flush] PUTAREA slow %ums area=(%d,%d)-(%d,%d)",
                  (unsigned)put_ms,
                  (int)local.area.x1, (int)local.area.y1,
                  (int)local.area.x2, (int)local.area.y2);
            }

          if (ret < 0)
            {
              atomic_fetch_add_explicit(&g_lcd_disp.stat_putarea_fail, 1,
                  memory_order_relaxed);
              syslog(LOG_ERR, "myvendor_lcd_disp: PUTAREA failed %d", ret);
            }
        }

      atomic_fetch_add_explicit(&g_lcd_disp.stat_worker_done, 1,
          memory_order_relaxed);

      if (!myvendor_lcd_worker_job_done(&local, true))
        {
          goto out;
        }
    }

out:
  if (slot >= 0 && slot < 2)
    {
      atomic_store_explicit(&g_ui_flush_stack_busy[slot], false,
          memory_order_release);
    }

  return NULL;
}

/**
 * @brief 打开 LCD 并启动 ui_flush worker。
 * @return 0 成功，负 errno。
 */
int myvendor_lcd_disp_init(const char *lcd_path)
{
  struct fb_videoinfo_s vinfo;
  int ret;

  if (g_lcd_disp.inited)
    {
      return 0;
    }

  if (lcd_path == NULL)
    {
      lcd_path = "/dev/lcd0";
    }

  memset(&g_lcd_disp, 0, sizeof(g_lcd_disp));
  g_lcd_disp.lcd_fd = -1;

  strncpy(g_lcd_disp.lcd_path, lcd_path, sizeof(g_lcd_disp.lcd_path) - 1u);
  g_lcd_disp.lcd_path[sizeof(g_lcd_disp.lcd_path) - 1u] = '\0';

  g_lcd_disp.lcd_fd = open(lcd_path, O_RDWR);
  if (g_lcd_disp.lcd_fd < 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: open %s failed errno=%d",
             lcd_path, errno);
      return -errno;
    }

  ret = ioctl(g_lcd_disp.lcd_fd, LCDDEVIO_GETVIDEOINFO,
              (unsigned long)(uintptr_t)&vinfo);
  if (ret < 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: GETVIDEOINFO failed %d", ret);
      goto errout;
    }

  g_lcd_disp.hor_res = (uint16_t)vinfo.xres;
  g_lcd_disp.ver_res = (uint16_t)vinfo.yres;

  if (g_lcd_disp.hor_res == 0 || g_lcd_disp.ver_res == 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: bad resolution %ux%u",
             g_lcd_disp.hor_res, g_lcd_disp.ver_res);
      ret = -EINVAL;
      goto errout;
    }

  g_lcd_disp.strip_rows = (uint16_t)CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS;

  if (g_lcd_disp.strip_rows < 8)
    {
      g_lcd_disp.strip_rows = 8;
    }

  if (g_lcd_disp.strip_rows > g_lcd_disp.ver_res)
    {
      g_lcd_disp.strip_rows = g_lcd_disp.ver_res;
    }

  g_lcd_disp.stride = (uint32_t)g_lcd_disp.hor_res * 2u;
  g_lcd_disp.buf_bytes = (size_t)g_lcd_disp.stride * g_lcd_disp.strip_rows;

  if (g_lcd_disp.buf_bytes == 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: strip size is zero");
      ret = -EINVAL;
      goto errout;
    }

  if (pthread_mutex_init(&g_lcd_disp.lock, NULL) != 0 ||
      pthread_cond_init(&g_lcd_disp.job_cond, NULL) != 0 ||
      pthread_cond_init(&g_lcd_disp.idle_cond, NULL) != 0)
    {
      syslog(LOG_ERR, "myvendor_lcd_disp: pthread sync init failed");
      ret = -EIO;
      goto errout;
    }

  g_lcd_disp.sync_inited = true;
  ret = myvendor_lcd_start_worker();
  if (ret < 0)
    {
      goto errout;
    }

  g_lcd_disp.inited = true;

  syslog(LOG_INFO,
         "myvendor_lcd_disp: %ux%u strip=%u stride=%lu direct-sram path=%s "
         "(ui_flush double-buffer)",
         g_lcd_disp.hor_res, g_lcd_disp.ver_res, g_lcd_disp.strip_rows,
         (unsigned long)g_lcd_disp.stride, lcd_path);

  return 0;

errout:
  myvendor_lcd_disp_deinit();
  return ret;
}

/** @brief 停止 worker 并关闭 LCD。 */
void myvendor_lcd_disp_deinit(void)
{
  if (g_lcd_disp.sync_inited)
    {
      myvendor_lcd_stop_worker_join();
    }

  if (g_lcd_disp.lcd_fd >= 0)
    {
      close(g_lcd_disp.lcd_fd);
      g_lcd_disp.lcd_fd = -1;
    }

  if (g_lcd_disp.sync_inited)
    {
      pthread_mutex_destroy(&g_lcd_disp.lock);
      pthread_cond_destroy(&g_lcd_disp.job_cond);
      pthread_cond_destroy(&g_lcd_disp.idle_cond);
    }

  memset(&g_lcd_disp, 0, sizeof(g_lcd_disp));
  g_lcd_disp.lcd_fd = -1;
}

/**
 * @brief 返回分辨率与条带缓冲信息。
 * @return 0 成功。
 */
int myvendor_lcd_disp_get_info(myvendor_lcd_disp_info_t *info)
{
  if (info == NULL || !g_lcd_disp.inited || g_lcd_disp.lcd_fd < 0)
    {
      return -EINVAL;
    }

  info->hor_res    = g_lcd_disp.hor_res;
  info->ver_res    = g_lcd_disp.ver_res;
  info->strip_rows = g_lcd_disp.strip_rows;
  info->stride     = g_lcd_disp.stride;
  info->buf_bytes  = g_lcd_disp.buf_bytes;
  return 0;
}

/** @brief 注册 strip flush 完成回调。 */
int myvendor_lcd_disp_register_done_cb(myvendor_lcd_disp_done_cb_t cb,
                                       void *user_data)
{
  if (!g_lcd_disp.inited || g_lcd_disp.lcd_fd < 0)
    {
      return -ENODEV;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);
  g_lcd_disp.done_cb = cb;
  g_lcd_disp.done_user_data = user_data;
  pthread_mutex_unlock(&g_lcd_disp.lock);
  return 0;
}

/** @brief 注销 done 回调。 */
void myvendor_lcd_disp_unregister_done_cb(void)
{
  if (!g_lcd_disp.sync_inited)
    {
      return;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);
  g_lcd_disp.done_cb = NULL;
  g_lcd_disp.done_user_data = NULL;
  pthread_mutex_unlock(&g_lcd_disp.lock);
}

/** @brief 注册 recover refr/unblock 回调。 */
int myvendor_lcd_disp_register_recover_cbs(myvendor_lcd_disp_refr_cb_t refr_cb,
                                           myvendor_lcd_disp_unblock_cb_t
                                               unblock_cb,
                                           void *user_data)
{
  if (!g_lcd_disp.inited)
    {
      return -ENODEV;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);
  g_lcd_disp.refr_cb = refr_cb;
  g_lcd_disp.unblock_cb = unblock_cb;
  g_lcd_disp.recover_user_data = user_data;
  pthread_mutex_unlock(&g_lcd_disp.lock);
  return 0;
}

/** @brief 注销 recover 回调。 */
void myvendor_lcd_disp_unregister_recover_cbs(void)
{
  if (!g_lcd_disp.sync_inited)
    {
      return;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);
  g_lcd_disp.refr_cb = NULL;
  g_lcd_disp.unblock_cb = NULL;
  g_lcd_disp.recover_user_data = NULL;
  pthread_mutex_unlock(&g_lcd_disp.lock);
}

/**
 * @brief 等待 flush 空闲。
 * @param max_ms 0 表示无限等待。
 */
bool myvendor_lcd_disp_wait_idle(uint32_t max_ms, uint32_t * waited_ms_out)
{
  bool ok;

  if (!g_lcd_disp.sync_inited)
    {
      if (waited_ms_out != NULL)
        {
          *waited_ms_out = 0;
        }

      return true;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);
  ok = myvendor_lcd_wait_idle_locked(max_ms, waited_ms_out);
  pthread_mutex_unlock(&g_lcd_disp.lock);
  return ok;
}

static int myvendor_lcd_disp_recover(void)
{
  if (!g_lcd_disp.inited || !g_lcd_disp.sync_inited)
    {
      return -ENODEV;
    }

  if (atomic_exchange_explicit(&g_lcd_disp.recover_in_progress, true,
          memory_order_acq_rel))
    {
      return -EBUSY;
    }

  syslog(LOG_ERR, "[lcd_flush] RECOVER: abort DMA, stop worker, restart pipe");

  {
    uint32_t now = myvendor_lcd_mono_ms();

    atomic_store_explicit(&g_lcd_disp.recover_quiet_until_ms,
        now + MYVENDOR_LCD_RECOVER_COOLDOWN_MS, memory_order_release);
    atomic_store_explicit(&g_lcd_disp.busy_since_ms, now, memory_order_release);
  }

  pthread_mutex_lock(&g_lcd_disp.lock);
  g_lcd_disp.recover_gen++;
  g_lcd_disp.job_ready = false;
  g_lcd_disp.running = false;
  memset(&g_lcd_disp.job, 0, sizeof(g_lcd_disp.job));
  pthread_cond_broadcast(&g_lcd_disp.job_cond);
  pthread_cond_broadcast(&g_lcd_disp.idle_cond);
  pthread_mutex_unlock(&g_lcd_disp.lock);

  atomic_store_explicit(&g_lcd_disp.worker_active, false,
      memory_order_release);

  /* Abort DMA, detach stuck ui_flush (never pthread_join — ioctl may not return). */
  myvendor_lcd_stop_worker_force();

  if (myvendor_lcd_reopen_device() < 0)
    {
      syslog(LOG_ERR, "[lcd_flush] RECOVER: reopen failed");
      myvendor_lcd_finish_recover_ui();
      return -EIO;
    }

  /* Worker gone — reopen /dev/lcd0 and restart ui_flush. */
  myvendor_lcd_restart_hw_pipe(g_lcd_disp.lcd_fd);

  usleep(MYVENDOR_LCD_RECOVER_SETTLE_US);

  if (myvendor_lcd_start_worker() < 0)
    {
      syslog(LOG_ERR, "[lcd_flush] RECOVER: worker restart failed");
      myvendor_lcd_finish_recover_ui();
      return -EIO;
    }

  atomic_fetch_add_explicit(&g_lcd_disp.stat_recover, 1,
      memory_order_relaxed);
  myvendor_lcd_finish_recover_ui();

  syslog(LOG_ERR,
         "[lcd_flush] RECOVER: done (pipe restarted, worker running)");
  return 0;
}

/** @brief 手动触发 recover（NSH/诊断）。 */
int myvendor_lcd_disp_trigger_recover(void)
{
  syslog(LOG_WARNING, "[lcd_flush] manual recover requested");
  return myvendor_lcd_disp_recover();
}

/**
 * @brief 提交一条 LVGL 条带到 worker。
 * @return 0 成功；-EBUSY/-EIO/-EINVAL。
 */
int myvendor_lcd_disp_submit_flush(const myvendor_lcd_disp_area_t *area,
                                   const void *color_p,
                                   uint32_t stride_bytes,
                                   bool is_last)
{
  uint32_t waited_ms = 0;

  (void)is_last;

  if (!g_lcd_disp.inited || g_lcd_disp.lcd_fd < 0 ||
      area == NULL || color_p == NULL)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
          memory_order_relaxed);
      return -EINVAL;
    }

  if (atomic_load_explicit(&g_lcd_disp.recover_hold_submit,
          memory_order_acquire))
    {
      return 0;
    }

  /* Wait for previous strip DMA so the other SRAM buffer can be submitted. */
  if (!myvendor_lcd_disp_wait_idle(0, &waited_ms))
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
          memory_order_relaxed);
      return -EIO;
    }

  myvendor_lcd_disp_poll_recover_unblock();

  if (atomic_load_explicit(&g_lcd_disp.recover_hold_submit,
          memory_order_acquire))
    {
      return 0;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);

  if (lcd_disp_is_busy_locked())
    {
      pthread_mutex_unlock(&g_lcd_disp.lock);
      atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_busy, 1,
          memory_order_relaxed);
      return -EBUSY;
    }

  g_lcd_disp.job.valid = true;
  g_lcd_disp.job.area  = *area;
  g_lcd_disp.job.data  = (const uint8_t *)color_p;
  g_lcd_disp.job.stride = stride_bytes;
  g_lcd_disp.job.is_last = is_last;
  g_lcd_disp.job.recover_gen = g_lcd_disp.recover_gen;
  g_lcd_disp.job_ready = true;
  myvendor_lcd_note_busy();

  pthread_cond_signal(&g_lcd_disp.job_cond);
  pthread_mutex_unlock(&g_lcd_disp.lock);

  atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_ok, 1,
      memory_order_relaxed);
  return 0;
}

/** @brief flush 是否仍在进行。 */
bool myvendor_lcd_disp_flush_busy(void)
{
  bool busy;

  if (!g_lcd_disp.sync_inited)
    {
      return false;
    }

  pthread_mutex_lock(&g_lcd_disp.lock);
  busy = lcd_disp_is_busy_locked();
  pthread_mutex_unlock(&g_lcd_disp.lock);
  return busy;
}

/** @brief 复制诊断计数快照。 */
void myvendor_lcd_disp_stats_snapshot(myvendor_lcd_disp_stats_t * stats)
{
  if (!stats)
    {
      return;
    }

  stats->submit_ok = atomic_load_explicit(&g_lcd_disp.stat_submit_ok,
      memory_order_relaxed);
  stats->submit_busy = atomic_load_explicit(&g_lcd_disp.stat_submit_busy,
      memory_order_relaxed);
  stats->submit_err = atomic_load_explicit(&g_lcd_disp.stat_submit_err,
      memory_order_relaxed);
  stats->worker_done = atomic_load_explicit(&g_lcd_disp.stat_worker_done,
      memory_order_relaxed);
  stats->putarea_fail = atomic_load_explicit(&g_lcd_disp.stat_putarea_fail,
      memory_order_relaxed);
  stats->flush_ready = atomic_load_explicit(&g_lcd_disp.stat_flush_ready,
      memory_order_relaxed);
  stats->wait_calls = atomic_load_explicit(&g_lcd_disp.stat_wait_calls,
      memory_order_relaxed);
  stats->wait_ms_max = atomic_load_explicit(&g_lcd_disp.stat_wait_ms_max,
      memory_order_relaxed);
  stats->wait_timeout = atomic_load_explicit(&g_lcd_disp.stat_wait_timeout,
      memory_order_relaxed);
  stats->flush_ready_force = atomic_load_explicit(
      &g_lcd_disp.stat_flush_ready_force, memory_order_relaxed);
  stats->submit_retry = atomic_load_explicit(&g_lcd_disp.stat_submit_retry,
      memory_order_relaxed);
  stats->wait_stall = atomic_load_explicit(&g_lcd_disp.stat_wait_stall,
      memory_order_relaxed);
  stats->putarea_ms_max = atomic_load_explicit(&g_lcd_disp.stat_putarea_ms_max,
      memory_order_relaxed);
  stats->putarea_slow = atomic_load_explicit(&g_lcd_disp.stat_putarea_slow,
      memory_order_relaxed);
  stats->recover = atomic_load_explicit(&g_lcd_disp.stat_recover,
      memory_order_relaxed);
}

/** @brief 清零诊断计数窗口。 */
void myvendor_lcd_disp_stats_reset_window(void)
{
  atomic_store_explicit(&g_lcd_disp.stat_submit_ok, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_submit_busy, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_submit_err, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_worker_done, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_putarea_fail, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_flush_ready, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_calls, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_ms_max, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_timeout, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_flush_ready_force, 0,
      memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_submit_retry, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_wait_stall, 0, memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_putarea_ms_max, 0,
      memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_putarea_slow, 0,
      memory_order_relaxed);
  atomic_store_explicit(&g_lcd_disp.stat_recover, 0, memory_order_relaxed);
}

/** @brief LVGL 记录 flush_ready。 */
void myvendor_lcd_disp_stats_note_flush_ready(void)
{
  atomic_fetch_add_explicit(&g_lcd_disp.stat_flush_ready, 1,
      memory_order_relaxed);
}

/** @brief 记录 wait_idle 统计。 */
void myvendor_lcd_disp_stats_note_wait(uint32_t waited_ms, bool timeout,
    bool forced)
{
  uint32_t prev;

  atomic_fetch_add_explicit(&g_lcd_disp.stat_wait_calls, 1,
      memory_order_relaxed);
  if (timeout)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_wait_timeout, 1,
          memory_order_relaxed);
    }
  if (forced)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_flush_ready_force, 1,
          memory_order_relaxed);
    }
  if (waited_ms >= 100u)
    {
      atomic_fetch_add_explicit(&g_lcd_disp.stat_wait_stall, 1,
          memory_order_relaxed);
    }

  prev = atomic_load_explicit(&g_lcd_disp.stat_wait_ms_max,
      memory_order_relaxed);
  if (waited_ms > prev)
    {
      atomic_store_explicit(&g_lcd_disp.stat_wait_ms_max, waited_ms,
          memory_order_relaxed);
    }
}

/** @brief 记录 submit 重试。 */
void myvendor_lcd_disp_stats_note_submit_retry(void)
{
  atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_retry, 1,
      memory_order_relaxed);
}

/** @brief 记录 submit 错误。 */
void myvendor_lcd_disp_stats_note_submit_err(void)
{
  atomic_fetch_add_explicit(&g_lcd_disp.stat_submit_err, 1,
      memory_order_relaxed);
}

/** @brief 抑制自动 recover 直到指定毫秒。 */
void myvendor_lcd_disp_suppress_recover_ms(uint32_t ms)
{
  uint32_t now;
  uint32_t until;

  if (!g_lcd_disp.sync_inited || ms == 0)
    {
      return;
    }

  now = myvendor_lcd_mono_ms();
  until = now + ms;

  atomic_store_explicit(&g_lcd_disp.recover_quiet_until_ms, until,
      memory_order_release);
}
