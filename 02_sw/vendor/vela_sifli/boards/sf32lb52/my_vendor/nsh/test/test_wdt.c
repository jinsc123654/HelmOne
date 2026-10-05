/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_wdt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * "test wdt": probe and exercise the hardware watchdog (/dev/watchdog0).
 *
 * Production (CONFIG_MYVENDOR_WATCHDOG) starts IWDT after bringup and pets
 * it from iwdg_feed only while (idle or work) + UI heartbeats are fresh. These
 * commands pause that feeder so a deliberate no-feed test can still reset:
 *
 *     test wdt                 show HW status + production heartbeats
 *     test wdt status          same as above
 *     test wdt start [to_ms]   pause production feed, START, do NOT feed
 *     test wdt feed [to_ms] [period_ms]
 *                              pause production feed, START + always-feed task
 *     test wdt stop            stop test feeder and resume production feed
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <nuttx/sched.h>
#include <nuttx/timers/watchdog.h>

#include "myvendor_watchdog.h"
#include "test_demos.h"

#define LOGI(fmt, ...)  printf("test wdt: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("test wdt: ERROR " fmt "\n", ##__VA_ARGS__)

#define WDT_DEVPATH           "/dev/watchdog0"
#define WDT_DEFAULT_TIMEOUT   8000u
#define WDT_DEFAULT_PERIOD    2000u
#define WDT_MIN_TIMEOUT       100u
#define WDT_MAX_TIMEOUT       600000u

#define TEST_WDT_TASK_NAME    "test_wdt"
#define TEST_WDT_STACK        2048
#define TEST_WDT_PRIORITY     150

#ifndef CONFIG_NSH_PROC_MOUNTPOINT
#  define PROC_ROOT           "/proc"
#else
#  define PROC_ROOT           CONFIG_NSH_PROC_MOUNTPOINT
#endif

/* Handed to the feeder task (same address space until test command exits). */
static uint32_t g_wdt_timeout_ms;
static uint32_t g_wdt_period_ms;

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

static int wdt_find_task(void)
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

          if (strncmp(line, TEST_WDT_TASK_NAME,
                      strlen(TEST_WDT_TASK_NAME)) == 0)
            {
              char c = line[strlen(TEST_WDT_TASK_NAME)];
              if (c == '\0' || c == '\n' || c == ' ')
                {
                  int pid = atoi(entry->d_name);
                  fclose(fp);
                  closedir(dir);
                  return pid;
                }
            }

          break;
        }

      fclose(fp);
    }

  closedir(dir);
#endif /* CONFIG_FS_PROCFS */

  return -1;
}

static void wdt_print_status(int fd)
{
  struct watchdog_status_s st;

  memset(&st, 0, sizeof(st));
  if (ioctl(fd, WDIOC_GETSTATUS, (unsigned long)(uintptr_t)&st) < 0)
    {
      LOGE("WDIOC_GETSTATUS failed errno=%d (lower-half may lack getstatus)",
           errno);
      return;
    }

  LOGI("status flags=0x%08lx active=%d reset=%d capture=%d "
       "timeout=%lums timeleft=%lums",
       (unsigned long)st.flags,
       (st.flags & WDFLAGS_ACTIVE) ? 1 : 0,
       (st.flags & WDFLAGS_RESET) ? 1 : 0,
       (st.flags & WDFLAGS_CAPTURE) ? 1 : 0,
       (unsigned long)st.timeout,
       (unsigned long)st.timeleft);
}

static int wdt_status(void)
{
  int fd = open(WDT_DEVPATH, O_RDONLY);
  int feeder;
  struct myvendor_watchdog_status_s prod;

  if (fd < 0)
    {
      LOGE("open %s failed errno=%d", WDT_DEVPATH, errno);
      return EXIT_FAILURE;
    }

  wdt_print_status(fd);
  close(fd);

  if (myvendor_watchdog_get_status(&prod) == 0)
    {
      LOGI("production started=%d feeding=%d suppress=%d "
           "idle_ok=%d idle_age=%ums work_ok=%d work_age=%ums "
           "ui_ok=%d ui_seen=%d ui_req=%d "
           "ui_age=%ums timeout=%ums",
           prod.started ? 1 : 0,
           prod.feeding ? 1 : 0,
           prod.suppressed ? 1 : 0,
           prod.idle_ok ? 1 : 0,
           (unsigned)prod.idle_age_ms,
           prod.work_ok ? 1 : 0,
           (unsigned)prod.work_age_ms,
           prod.ui_ok ? 1 : 0,
           prod.ui_seen ? 1 : 0,
           prod.ui_required ? 1 : 0,
           (unsigned)prod.ui_age_ms,
           (unsigned)prod.timeout_ms);
    }

  feeder = wdt_find_task();
  if (feeder >= 0)
    {
      LOGI("test feeder task running pid=%d", feeder);
    }
  else
    {
      LOGI("test feeder task not running");
    }

  return EXIT_SUCCESS;
}

static int wdt_start_nofeed(uint32_t timeout_ms)
{
  int fd;

  myvendor_watchdog_suppress(true);

  fd = open(WDT_DEVPATH, O_RDONLY);

  if (fd < 0)
    {
      LOGE("open %s failed errno=%d", WDT_DEVPATH, errno);
      myvendor_watchdog_suppress(false);
      return EXIT_FAILURE;
    }

  if (ioctl(fd, WDIOC_SETTIMEOUT, (unsigned long)timeout_ms) < 0)
    {
      LOGE("WDIOC_SETTIMEOUT(%lu) failed errno=%d",
           (unsigned long)timeout_ms, errno);
    }

  if (ioctl(fd, WDIOC_START, 0) < 0)
    {
      LOGE("WDIOC_START failed errno=%d", errno);
      close(fd);
      myvendor_watchdog_suppress(false);
      return EXIT_FAILURE;
    }

  wdt_print_status(fd);
  close(fd);

  LOGI("started timeout=%lums, NOT feeding", (unsigned long)timeout_ms);
  LOGI("=> if NuttX controls the HW dog, board resets in ~%lums",
       (unsigned long)timeout_ms);
  return EXIT_SUCCESS;
}

static int wdt_feed_task(int argc, FAR char *argv[])
{
  uint32_t timeout_ms = g_wdt_timeout_ms;
  uint32_t period_ms = g_wdt_period_ms;
  uint32_t feeds = 0;
  int fd;

  (void)argc;
  (void)argv;

  fd = open(WDT_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "[test wdt] open %s failed errno=%d",
             WDT_DEVPATH, errno);
      return EXIT_FAILURE;
    }

  if (ioctl(fd, WDIOC_SETTIMEOUT, (unsigned long)timeout_ms) < 0)
    {
      syslog(LOG_WARNING, "[test wdt] SETTIMEOUT(%u) failed errno=%d",
             (unsigned)timeout_ms, errno);
    }

  if (ioctl(fd, WDIOC_START, 0) < 0)
    {
      syslog(LOG_WARNING, "[test wdt] START failed errno=%d", errno);
    }

  syslog(LOG_WARNING, "[test wdt] feeder start timeout=%ums period=%ums",
         (unsigned)timeout_ms, (unsigned)period_ms);

  for (;;)
    {
      if (ioctl(fd, WDIOC_KEEPALIVE, 0) < 0)
        {
          syslog(LOG_ERR, "[test wdt] KEEPALIVE failed errno=%d", errno);
        }

      feeds++;

      /* Heartbeat roughly every ~30s so we can see it survive 4295s. */
      if (period_ms == 0 || (feeds % (30000u / (period_ms ? period_ms : 1) + 1)) == 0)
        {
          struct timespec mono;

          clock_gettime(CLOCK_MONOTONIC, &mono);
          syslog(LOG_WARNING,
                 "[test wdt] fed #%u mono=%lu.%03lus (timeout=%ums)",
                 (unsigned)feeds, (unsigned long)mono.tv_sec,
                 (unsigned long)(mono.tv_nsec / 1000000L),
                 (unsigned)timeout_ms);
        }

      usleep(period_ms * 1000u);
    }

  close(fd);
  return EXIT_SUCCESS;
}

static int wdt_feed_start(uint32_t timeout_ms, uint32_t period_ms)
{
  int pid;

  if (wdt_find_task() >= 0)
    {
      LOGE("feeder already running — 'test wdt stop' first");
      return EXIT_FAILURE;
    }

  if (period_ms >= timeout_ms)
    {
      LOGE("period (%lums) must be < timeout (%lums)",
           (unsigned long)period_ms, (unsigned long)timeout_ms);
      return EXIT_FAILURE;
    }

  g_wdt_timeout_ms = timeout_ms;
  g_wdt_period_ms = period_ms;

  myvendor_watchdog_suppress(true);

  pid = task_create(TEST_WDT_TASK_NAME, TEST_WDT_PRIORITY,
                    TEST_WDT_STACK, wdt_feed_task, NULL);
  if (pid < 0)
    {
      LOGE("task_create failed errno=%d", errno);
      myvendor_watchdog_suppress(false);
      return EXIT_FAILURE;
    }

  LOGI("feeder started pid=%d timeout=%lums period=%lums",
       pid, (unsigned long)timeout_ms, (unsigned long)period_ms);
  LOGI("=> survives past ~4295s  => reset WAS the watchdog (feeding fixes it)");
  return EXIT_SUCCESS;
}

static int wdt_stop(void)
{
  int pid = wdt_find_task();
  int fd;

  if (pid >= 0)
    {
      if (nxtask_delete((pid_t)pid) != OK)
        {
          LOGE("nxtask_delete(%d) failed errno=%d", pid, errno);
        }
      else
        {
          LOGI("feeder stopped pid=%d", pid);
        }
    }
  else
    {
      LOGI("feeder not running");
    }

  fd = open(WDT_DEVPATH, O_RDONLY);
  if (fd >= 0)
    {
      if (ioctl(fd, WDIOC_STOP, 0) < 0)
        {
          LOGI("WDIOC_STOP failed errno=%d (HW dog likely cannot be stopped)",
               errno);
        }
      else
        {
          LOGI("WDIOC_STOP ok");
        }

      close(fd);
    }

  myvendor_watchdog_suppress(false);

  return EXIT_SUCCESS;
}

static uint32_t wdt_parse_ms(FAR const char *s, uint32_t dflt)
{
  long v;

  if (s == NULL)
    {
      return dflt;
    }

  v = strtol(s, NULL, 10);
  if (v < (long)WDT_MIN_TIMEOUT || v > (long)WDT_MAX_TIMEOUT)
    {
      return dflt;
    }

  return (uint32_t)v;
}

int test_wdt_main(int argc, FAR char *argv[])
{
  if (argc < 2 || strcmp(argv[1], "status") == 0)
    {
      return wdt_status();
    }

  if (strcmp(argv[1], "start") == 0)
    {
      uint32_t to = wdt_parse_ms(argc >= 3 ? argv[2] : NULL,
                                 WDT_DEFAULT_TIMEOUT);
      return wdt_start_nofeed(to);
    }

  if (strcmp(argv[1], "feed") == 0)
    {
      uint32_t to = wdt_parse_ms(argc >= 3 ? argv[2] : NULL,
                                 WDT_DEFAULT_TIMEOUT);
      uint32_t period = wdt_parse_ms(argc >= 4 ? argv[3] : NULL,
                                     WDT_DEFAULT_PERIOD);
      return wdt_feed_start(to, period);
    }

  if (strcmp(argv[1], "stop") == 0)
    {
      return wdt_stop();
    }

  LOGE("unknown arg '%s'", argv[1]);
  printf("Usage: test wdt [status | start [to_ms] | "
         "feed [to_ms] [period_ms] | stop]\n");
  return EXIT_FAILURE;
}
