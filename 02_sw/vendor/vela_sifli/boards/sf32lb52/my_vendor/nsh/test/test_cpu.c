/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_cpu.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * "test cpu": periodic CPU monitor via /proc (same data source as top).
 *
 *     test cpu -t <sec> [-p <pid>]...   start background print every <sec> s
 *     test cpu stop                       stop background monitor
 *
 * -t  print interval in seconds (required to start)
 * -p  show CPU for the given thread pid (repeatable; omit for all tasks)
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/sched.h>

#include "test_demos.h"

#define LOGI(fmt, ...)  printf("test cpu: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("test cpu: ERROR " fmt "\n", ##__VA_ARGS__)

#define TEST_CPU_DEFAULT_INTERVAL_SEC  1
#define TEST_CPU_MIN_INTERVAL_SEC      1
#define TEST_CPU_MAX_INTERVAL_SEC      3600
#define TEST_CPU_MAX_TASKS             24
#define TEST_CPU_LINE_MAX              128
#define TEST_CPU_STATUS_MAX            512
#define TEST_CPU_CMD_MAX               64
#define TEST_CPU_PATH_MAX              160
#define TEST_CPU_STACK                 4096
#define TEST_CPU_PRIORITY              100
#define TEST_CPU_MAX_FILTER_PIDS       16
#define TEST_CPU_TASK_NAME             "test_cpu"

struct cpu_filter_s
{
  int pids[TEST_CPU_MAX_FILTER_PIDS];
  int count;   /* 0 = monitor all tasks */
};

#ifndef CONFIG_NSH_PROC_MOUNTPOINT
#  define PROC_ROOT                    "/proc"
#else
#  define PROC_ROOT                    CONFIG_NSH_PROC_MOUNTPOINT
#endif

struct cpu_task_s
{
  int           pid;
  int           priority;
  char          state[24];
  char          cmdline[TEST_CPU_CMD_MAX];
  unsigned long stack_size;
  unsigned long stack_used;
  float         cpu_pct;
};

#if !defined(CONFIG_SCHED_CPULOAD_NONE) && defined(CONFIG_FS_PROCFS) \
    && !defined(CONFIG_FS_PROCFS_EXCLUDE_PROCESS)

static struct cpu_task_s g_tasks[TEST_CPU_MAX_TASKS];

/* Passed to the monitor worker (same address space until test command exits). */
static int g_mon_interval_sec;
static struct cpu_filter_s g_mon_filter;

static void trim_inplace(FAR char *s)
{
  FAR char *start;
  FAR char *end;

  if (s == NULL || *s == '\0')
    {
      return;
    }

  start = s;
  while (*start == ' ' || *start == '\t')
    {
      start++;
    }

  if (*start == '\0')
    {
      s[0] = '\0';
      return;
    }

  end = start + strlen(start) - 1;
  while (end > start && (*end == ' ' || *end == '\t' ||
         *end == '\n' || *end == '\r'))
    {
      *end-- = '\0';
    }

  if (start != s)
    {
      memmove(s, start, strlen(start) + 1);
    }
}

static bool is_pid_entry(FAR const char *name)
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

static int read_file(FAR const char *path, FAR char *buf, size_t buflen)
{
  FILE *fp;
  size_t n;

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return -errno;
    }

  n = fread(buf, 1, buflen - 1, fp);
  buf[n] = '\0';
  fclose(fp);
  return (int)n;
}

static FAR const char *status_value(FAR const char *line, FAR const char *key)
{
  size_t klen = strlen(key);

  if (strncmp(line, key, klen) != 0)
    {
      return NULL;
    }

  return line + klen;
}

static void parse_status(FAR struct cpu_task_s *task, FAR const char *text)
{
  FAR char *save = NULL;
  FAR char *line;
  FAR char buf[TEST_CPU_STATUS_MAX];

  strlcpy(buf, text, sizeof(buf));
  line = strtok_r(buf, "\n", &save);
  while (line != NULL)
    {
      FAR const char *val;

      val = status_value(line, "Name:");
      if (val != NULL)
        {
          trim_inplace((FAR char *)val);
          if (task->cmdline[0] == '\0')
            {
              strlcpy(task->cmdline, val, sizeof(task->cmdline));
            }
        }

      val = status_value(line, "Priority:");
      if (val != NULL)
        {
          task->priority = atoi(val);
        }

      val = status_value(line, "State:");
      if (val != NULL)
        {
          trim_inplace((FAR char *)val);
          strlcpy(task->state, val, sizeof(task->state));
        }

      line = strtok_r(NULL, "\n", &save);
    }
}

static void parse_stack(FAR struct cpu_task_s *task, FAR const char *text)
{
  FAR char *save = NULL;
  FAR char *line;
  FAR char buf[TEST_CPU_STATUS_MAX];

  strlcpy(buf, text, sizeof(buf));
  line = strtok_r(buf, "\n", &save);
  while (line != NULL)
    {
      FAR const char *val;

      val = status_value(line, "StackSize:");
      if (val != NULL)
        {
          task->stack_size = strtoul(val, NULL, 0);
        }

      val = status_value(line, "StackUsed:");
      if (val != NULL)
        {
          task->stack_used = strtoul(val, NULL, 0);
        }

      line = strtok_r(NULL, "\n", &save);
    }
}

static void parse_loadavg(FAR struct cpu_task_s *task, FAR const char *text)
{
  FAR char buf[TEST_CPU_LINE_MAX];

  strlcpy(buf, text, sizeof(buf));
  trim_inplace(buf);
  task->cpu_pct = strtof(buf, NULL);
}

static void parse_cmdline(FAR struct cpu_task_s *task, FAR const char *text)
{
  FAR char *save = NULL;
  FAR char *line;
  FAR char buf[TEST_CPU_STATUS_MAX];

  strlcpy(buf, text, sizeof(buf));
  line = strtok_r(buf, "\n", &save);
  if (line != NULL)
    {
      trim_inplace(line);
      if (line[0] != '\0')
        {
          strlcpy(task->cmdline, line, sizeof(task->cmdline));
        }
    }
}

static int collect_one(FAR struct cpu_task_s *task, int pid)
{
  char path[TEST_CPU_PATH_MAX];
  char buf[TEST_CPU_STATUS_MAX];
  int ret;

  memset(task, 0, sizeof(*task));
  task->pid = pid;

  snprintf(path, sizeof(path), "%s/%d/status", PROC_ROOT, pid);
  ret = read_file(path, buf, sizeof(buf));
  if (ret < 0)
    {
      return ret;
    }

  parse_status(task, buf);

  snprintf(path, sizeof(path), "%s/%d/loadavg", PROC_ROOT, pid);
  if (read_file(path, buf, sizeof(buf)) >= 0)
    {
      parse_loadavg(task, buf);
    }

  snprintf(path, sizeof(path), "%s/%d/stack", PROC_ROOT, pid);
  if (read_file(path, buf, sizeof(buf)) >= 0)
    {
      parse_stack(task, buf);
    }

  snprintf(path, sizeof(path), "%s/%d/cmdline", PROC_ROOT, pid);
  if (read_file(path, buf, sizeof(buf)) >= 0)
    {
      parse_cmdline(task, buf);
    }

  if (task->cmdline[0] == '\0')
    {
      snprintf(task->cmdline, sizeof(task->cmdline), "<pid%d>", pid);
    }

  return 0;
}

static int collect_tasks(void)
{
  DIR *dir;
  FAR struct dirent *entry;
  int count = 0;

  dir = opendir(PROC_ROOT);
  if (dir == NULL)
    {
      LOGE("opendir %s failed: %d", PROC_ROOT, errno);
      return -errno;
    }

  while ((entry = readdir(dir)) != NULL && count < TEST_CPU_MAX_TASKS)
    {
      int pid;

      if (!is_pid_entry(entry->d_name))
        {
          continue;
        }

      pid = atoi(entry->d_name);
      if (collect_one(&g_tasks[count], pid) == 0)
        {
          count++;
        }
    }

  closedir(dir);
  return count;
}

static int cmp_cpu(FAR const void *a, FAR const void *b)
{
  FAR const struct cpu_task_s *ta = a;
  FAR const struct cpu_task_s *tb = b;

  if (ta->cpu_pct < tb->cpu_pct)
    {
      return 1;
    }

  if (ta->cpu_pct > tb->cpu_pct)
    {
      return -1;
    }

  return ta->pid - tb->pid;
}

static void print_total_cpu(void)
{
  char buf[TEST_CPU_LINE_MAX];
  char path[48];

  snprintf(path, sizeof(path), "%s/cpuload", PROC_ROOT);
  if (read_file(path, buf, sizeof(buf)) >= 0)
    {
      trim_inplace(buf);
      printf("CPU busy: %s\n", buf);
    }
}

static void print_header(void)
{
  printf("%5s %3s %-12s %7s %7s %6s %s\n",
         "PID", "PRI", "STATE", "STACK", "USED", "CPU", "COMMAND");
}

static void print_rows(int count)
{
  int i;

  for (i = 0; i < count; i++)
    {
      FAR struct cpu_task_s *task = &g_tasks[i];
      int fill_x10 = 0;

      if (task->stack_size > 0 && task->stack_used > 0)
        {
          fill_x10 = (int)(1000ull * task->stack_used / task->stack_size);
        }

      printf("%5d %3d %-12s %7lu %7lu %5.1f%%",
             task->pid, task->priority, task->state,
             task->stack_size, task->stack_used, task->cpu_pct);

      if (fill_x10 >= 800)
        {
          printf("!");
        }

      printf(" %s\n", task->cmdline);
    }
}

static void print_one_pid_line(int pid)
{
  struct cpu_task_s task;

  if (collect_one(&task, pid) != 0)
    {
      printf("pid %d: not found or exited\n", pid);
      return;
    }

  printf("pid %d pri=%d state=%-12s cpu=%.1f%% %s\n",
         task.pid, task.priority, task.state, task.cpu_pct, task.cmdline);
}

static void print_filtered_pids(FAR const struct cpu_filter_s *filter)
{
  int i;

  print_total_cpu();
  for (i = 0; i < filter->count; i++)
    {
      print_one_pid_line(filter->pids[i]);
    }
}

static void print_snapshot(uint32_t seq, FAR const struct cpu_filter_s *filter)
{
  int count;

  printf("\n--- test cpu #%u ---\n", (unsigned)seq);

  if (filter->count > 0)
    {
      print_filtered_pids(filter);
      return;
    }

  print_total_cpu();
  print_header();

  count = collect_tasks();
  if (count < 0)
    {
      return;
    }

  qsort(g_tasks, count, sizeof(g_tasks[0]), cmp_cpu);
  print_rows(count);
  printf("(%d tasks, sorted by CPU)\n", count);
}

static int cpu_find_monitor(void)
{
  DIR *dir;
  FAR struct dirent *entry;
  char path[TEST_CPU_PATH_MAX];
  char buf[256];
  FILE *fp;
  FAR char *line;

  dir = opendir(PROC_ROOT);
  if (dir == NULL)
    {
      return -1;
    }

  while ((entry = readdir(dir)) != NULL)
    {
      int pid;

      if (!is_pid_entry(entry->d_name))
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

          if (strncmp(line, TEST_CPU_TASK_NAME, strlen(TEST_CPU_TASK_NAME)) != 0)
            {
              break;
            }

          if (line[strlen(TEST_CPU_TASK_NAME)] != '\0' &&
              line[strlen(TEST_CPU_TASK_NAME)] != '\n' &&
              line[strlen(TEST_CPU_TASK_NAME)] != ' ')
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
  return -1;
}

static int cpu_monitor_main(int argc, FAR char *argv[])
{
  int interval_sec = g_mon_interval_sec;
  struct cpu_filter_s filter = g_mon_filter;
  uint32_t seq = 0;
  int i;

  (void)argc;
  (void)argv;

  if (interval_sec < TEST_CPU_MIN_INTERVAL_SEC)
    {
      interval_sec = TEST_CPU_DEFAULT_INTERVAL_SEC;
    }

  printf("test cpu: monitor start interval=%ds", interval_sec);
  if (filter.count > 0)
    {
      printf(" pids=");
      for (i = 0; i < filter.count; i++)
        {
          printf("%s%d", i > 0 ? "," : "", filter.pids[i]);
        }
    }
  else
    {
      printf(" (all tasks)");
    }

  printf("\n");

  while (1)
    {
      seq++;
      print_snapshot(seq, &filter);
      sleep((unsigned int)interval_sec);
    }

  return EXIT_SUCCESS;
}

static int cpu_stop(void)
{
  int pid = cpu_find_monitor();

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

static int cpu_start(int interval_sec, FAR const struct cpu_filter_s *filter)
{
  int pid;
  int running;
  int i;

  if (interval_sec < TEST_CPU_MIN_INTERVAL_SEC ||
      interval_sec > TEST_CPU_MAX_INTERVAL_SEC)
    {
      LOGE("interval must be %d..%d seconds",
           TEST_CPU_MIN_INTERVAL_SEC, TEST_CPU_MAX_INTERVAL_SEC);
      return EXIT_FAILURE;
    }

  running = cpu_find_monitor();
  if (running >= 0)
    {
      LOGE("already running pid=%d — use 'test cpu stop' first", running);
      return EXIT_FAILURE;
    }

  g_mon_interval_sec = interval_sec;
  g_mon_filter = *filter;

  pid = task_create(TEST_CPU_TASK_NAME, TEST_CPU_PRIORITY,
                    TEST_CPU_STACK, cpu_monitor_main, NULL);
  if (pid < 0)
    {
      LOGE("task_create failed errno=%d", errno);
      return EXIT_FAILURE;
    }

  LOGI("started pid=%d interval=%ds%s",
       pid, interval_sec,
       filter->count > 0 ? " (filtered)" : " (all tasks)");
  if (filter->count > 0)
    {
      printf("test cpu: watching");
      for (i = 0; i < filter->count; i++)
        {
          printf(" %d", filter->pids[i]);
        }

      printf("\n");
    }

  LOGI("stop: test cpu stop");
  return EXIT_SUCCESS;
}

static bool cpu_filter_has_pid(FAR const struct cpu_filter_s *filter, int pid)
{
  int i;

  for (i = 0; i < filter->count; i++)
    {
      if (filter->pids[i] == pid)
        {
          return true;
        }
    }

  return false;
}

static int cpu_filter_add(FAR struct cpu_filter_s *filter, int pid)
{
  if (pid < 0)
    {
      return -EINVAL;
    }

  if (cpu_filter_has_pid(filter, pid))
    {
      return 0;
    }

  if (filter->count >= TEST_CPU_MAX_FILTER_PIDS)
    {
      LOGE("at most %d -p options", TEST_CPU_MAX_FILTER_PIDS);
      return -EINVAL;
    }

  filter->pids[filter->count++] = pid;
  return 0;
}

static int cpu_parse_args(int argc, FAR char *argv[], int *interval_sec,
                          FAR struct cpu_filter_s *filter)
{
  int i;

  *interval_sec = 0;
  memset(filter, 0, sizeof(*filter));

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-t") == 0)
        {
          if (i + 1 >= argc)
            {
              LOGE("-t requires a value");
              return -EINVAL;
            }

          *interval_sec = atoi(argv[++i]);
          if (*interval_sec <= 0)
            {
              LOGE("invalid interval '%s'", argv[i]);
              return -EINVAL;
            }

          continue;
        }

      if (strcmp(argv[i], "-p") == 0)
        {
          int pid;
          int ret;

          if (i + 1 >= argc)
            {
              LOGE("-p requires a pid");
              return -EINVAL;
            }

          pid = atoi(argv[++i]);
          ret = cpu_filter_add(filter, pid);
          if (ret != 0)
            {
              if (ret == -EINVAL && pid < 0)
                {
                  LOGE("invalid pid '%s'", argv[i]);
                }

              return ret;
            }

          continue;
        }

      LOGE("unknown arg '%s'", argv[i]);
      return -EINVAL;
    }

  if (*interval_sec <= 0)
    {
      LOGE("missing -t <seconds>");
      return -EINVAL;
    }

  return 0;
}

int test_cpu_main(int argc, FAR char *argv[])
{
  int interval_sec;
  struct cpu_filter_s filter;
  int ret;

  if (argc >= 2 && strcmp(argv[1], "stop") == 0)
    {
      return cpu_stop();
    }

  ret = cpu_parse_args(argc, argv, &interval_sec, &filter);
  if (ret != 0)
    {
      printf("Usage:\n");
      printf("  test cpu -t <sec> [-p <pid>]...   print every <sec> s\n");
      printf("  test cpu stop                     stop background monitor\n");
      return EXIT_FAILURE;
    }

  return cpu_start(interval_sec, &filter);
}

#else /* cpuload / procfs not available */

int test_cpu_main(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;
  LOGE("need CONFIG_SCHED_CPULOAD_SYSCLK and CONFIG_FS_PROCFS");
  return EXIT_FAILURE;
}

#endif
