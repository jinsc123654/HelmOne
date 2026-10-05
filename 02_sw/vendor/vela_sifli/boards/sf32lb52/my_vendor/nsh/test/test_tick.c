/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_tick.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * "test tick": spawn a dedicated NuttX task that logs uptime every N ms.
 * Does not auto-start on boot.  Survives after the "test" command returns.
 *
 *     test tick [period_ms]   start (default 1000ms)
 *     test tick stop          kill the test_tick task
 *     test tick status        show whether test_tick is running
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <nuttx/timers/rtc.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>

#include "test_demos.h"

#define LOGI(fmt, ...)  printf("test tick: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("test tick: ERROR " fmt "\n", ##__VA_ARGS__)

#define TEST_TICK_DEFAULT_MS  1000u
#define TEST_TICK_MIN_MS      10u
#define TEST_TICK_MAX_MS      60000u
#define TEST_TICK_STACK       4096
#define TEST_TICK_PRIORITY    100
#define TEST_TICK_TASK_NAME   "test_tick"

#ifndef CONFIG_NSH_PROC_MOUNTPOINT
#  define PROC_ROOT           "/proc"
#else
#  define PROC_ROOT           CONFIG_NSH_PROC_MOUNTPOINT
#endif

/* Passed to the worker task (same address space until test command exits). */
static uint32_t g_task_period_ms;

/* Independent uint64 counter: +1 each full second in the worker task. */
static volatile uint64_t g_own_sec;
static struct timespec g_next_sec;

static void timespec_add_sec(struct timespec * ts, time_t sec)
{
  ts->tv_sec += sec;
}

static bool proc_is_pid_dir(FAR const char *name)
{
  if (name == NULL || *name == '\0')
    {
      return false;
    }

  while (*name != '\0')
    {
      if (!isdigit((unsigned char)*name))
        {
          return false;
        }

      name++;
    }

  return true;
}

static int tick_find_task(void)
{
#if defined(CONFIG_FS_PROCFS)
  DIR *dir;
  FAR struct dirent *entry;
  char path[192];
  char buf[256];
  FILE *fp;
  char *line;

  dir = opendir(PROC_ROOT);
  if (dir == NULL)
    {
      return -1;
    }

  while ((entry = readdir(dir)) != NULL)
    {
      int pid;

      if (!proc_is_pid_dir(entry->d_name))
        {
          continue;
        }

      snprintf(path, sizeof(path), "%s/%s/status", PROC_ROOT, entry->d_name);
      fp = fopen(path, "r");
      if (fp == NULL)
        {
          continue;
        }

      while (fgets(buf, sizeof(buf), fp) != NULL)
        {
          if (strncmp(buf, "Name:", 5) != 0)
            {
              continue;
            }

          line = buf + 5;
          while (*line == ' ' || *line == '\t')
            {
              line++;
            }

          if (strncmp(line, TEST_TICK_TASK_NAME, strlen(TEST_TICK_TASK_NAME)) != 0)
            {
              break;
            }

          if (line[strlen(TEST_TICK_TASK_NAME)] != '\0' &&
              line[strlen(TEST_TICK_TASK_NAME)] != '\n' &&
              line[strlen(TEST_TICK_TASK_NAME)] != ' ')
            {
              break;
            }

          pid = atoi(entry->d_name);
          fclose(fp);
          closedir(dir);
          return pid;
        }

      fclose(fp);
    }

  closedir(dir);
#endif /* CONFIG_FS_PROCFS */

  return -1;
}

static void tick_own_sec_advance(const struct timespec * mono)
{
  while (mono->tv_sec > g_next_sec.tv_sec ||
         (mono->tv_sec == g_next_sec.tv_sec &&
          mono->tv_nsec >= g_next_sec.tv_nsec))
    {
      g_own_sec++;
      timespec_add_sec(&g_next_sec, 1);
    }
}

static void tick_log_line(uint32_t seq, uint64_t own_sec, uint32_t period_ms,
                          const struct timespec *mono,
                          const struct timespec *systime, unsigned long uptime,
                          int rtcfd)
{
  char rtcbuf[80];
  struct rtc_time rt;

  rtcbuf[0] = '\0';
  if (rtcfd >= 0 &&
      ioctl(rtcfd, RTC_RD_TIME, (unsigned long)(uintptr_t)&rt) >= 0)
    {
      snprintf(rtcbuf, sizeof(rtcbuf),
               " rtc=%04d-%02d-%02d %02d:%02d:%02d",
               rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
               rt.tm_hour, rt.tm_min, rt.tm_sec);
    }

  syslog(LOG_WARNING,
         "[test tick] #%u cnt_s=%" PRIu64 " period=%ums "
         "mono=%lu.%06lu systime=%lu.%06lu uptime=%lus%s",
         (unsigned)seq, own_sec, (unsigned)period_ms,
         (unsigned long)mono->tv_sec,
         (unsigned long)(mono->tv_nsec / 1000L),
         (unsigned long)systime->tv_sec,
         (unsigned long)(systime->tv_nsec / 1000L),
         uptime, rtcbuf);
}

static int tick_task_main(int argc, FAR char *argv[])
{
  uint32_t seq = 0;
  uint32_t period_ms = g_task_period_ms;
  struct timespec start_mono;
  int rtcfd;

  (void)argc;
  (void)argv;

  if (period_ms < TEST_TICK_MIN_MS)
    {
      period_ms = TEST_TICK_DEFAULT_MS;
    }

  rtcfd = open("/dev/rtc0", O_RDONLY);

  if (clock_gettime(CLOCK_MONOTONIC, &start_mono) != 0)
    {
      syslog(LOG_ERR, "[test tick] clock_gettime failed errno=%d", errno);
      return EXIT_FAILURE;
    }

  g_own_sec = 0;
  g_next_sec = start_mono;
  timespec_add_sec(&g_next_sec, 1);

  syslog(LOG_WARNING, "[test tick] task start period=%ums", (unsigned)period_ms);

  while (1)
    {
      struct timespec mono;
      struct timespec systime;
      struct sysinfo info;
      unsigned long uptime = 0;
      uint64_t own_snap;

      if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0)
        {
          syslog(LOG_ERR, "[test tick] clock_gettime failed errno=%d", errno);
          break;
        }

      tick_own_sec_advance(&mono);
      own_snap = g_own_sec;

      if (clock_systime_timespec(&systime) != 0)
        {
          memset(&systime, 0, sizeof(systime));
        }

      memset(&info, 0, sizeof(info));
      if (sysinfo(&info) == 0)
        {
          uptime = info.uptime;
        }

      seq++;
      tick_log_line(seq, own_snap, period_ms, &mono, &systime, uptime, rtcfd);
      usleep(period_ms * 1000u);
    }

  if (rtcfd >= 0)
    {
      close(rtcfd);
    }

  syslog(LOG_WARNING, "[test tick] task exit samples=%u cnt_s=%" PRIu64,
         (unsigned)seq, (uint64_t)g_own_sec);
  return EXIT_SUCCESS;
}

static int tick_stop(void)
{
  int pid = tick_find_task();

  if (pid < 0)
    {
      LOGI("not running");
      return EXIT_SUCCESS;
    }

  if (nxtask_delete((pid_t)pid) != OK)
    {
      LOGE("nxtask_delete(%d) failed errno=%d", pid, errno);
      return EXIT_FAILURE;
    }

  LOGI("stopped pid=%d", pid);
  return EXIT_SUCCESS;
}

static int tick_status(void)
{
  int pid = tick_find_task();

  if (pid < 0)
    {
      LOGI("not running");
      return EXIT_SUCCESS;
    }

  if (kill((pid_t)pid, 0) != 0)
    {
      LOGI("stale pid=%d (errno=%d), use 'test tick' to start", pid, errno);
      return EXIT_SUCCESS;
    }

  LOGI("running pid=%d (grep '[test tick]' in dmesg)", pid);
  return EXIT_SUCCESS;
}

static int tick_start(uint32_t period_ms)
{
  int pid;
  int running;

  running = tick_find_task();
  if (running >= 0)
    {
      LOGE("already running pid=%d period=%ums — use 'test tick stop' first",
           running, (unsigned)period_ms);
      return EXIT_FAILURE;
    }

  g_task_period_ms = period_ms;

  pid = task_create(TEST_TICK_TASK_NAME, TEST_TICK_PRIORITY,
                    TEST_TICK_STACK, tick_task_main, NULL);
  if (pid < 0)
    {
      LOGE("task_create failed errno=%d", errno);
      return EXIT_FAILURE;
    }

  LOGI("started pid=%d period=%ums", pid, (unsigned)period_ms);
  LOGI("cnt_s: uint64 +1/s in task; mono/systime from NuttX");
  LOGI("hint: mono->0 without new start => reboot; cnt_s resets on restart");
  return EXIT_SUCCESS;
}

int test_tick_main(int argc, FAR char *argv[])
{
  uint32_t period_ms = TEST_TICK_DEFAULT_MS;

  if (argc >= 2)
    {
      if (strcmp(argv[1], "stop") == 0)
        {
          return tick_stop();
        }

      if (strcmp(argv[1], "status") == 0)
        {
          return tick_status();
        }

      {
        long v = strtol(argv[1], NULL, 10);

        if (v < (long)TEST_TICK_MIN_MS || v > (long)TEST_TICK_MAX_MS)
          {
            LOGE("unknown arg '%s'", argv[1]);
            printf("Usage: test tick [period_ms] | test tick stop | test tick status\n");
            return EXIT_FAILURE;
          }

        period_ms = (uint32_t)v;
      }
    }

  return tick_start(period_ms);
}
