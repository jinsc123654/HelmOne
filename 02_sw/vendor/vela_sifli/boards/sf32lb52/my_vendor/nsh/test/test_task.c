/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_task.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * "test task": background CPU load workers with health metrics.
 *
 *     test task [duty_pct]       start worker (default 50%%)
 *     test task busy             100%% spin
 *     test task status           seq / heartbeat / period overrun
 *     test task watch [seconds]  poll 1 Hz — detect stuck or congestion
 *     test task stop [slot|all]
 *
 * Health (per worker):
 *   OK     — seq advancing ~10/s, last heartbeat < 500ms
 *   SLOW   — seq rate below 80%% of expected (CPU contention)
 *   STUCK  — seq unchanged for 2+ watch intervals, or heartbeat stale
 *   LAG    — loop period > 150%% of target (scheduling delay)
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/sched.h>

#include "test_demos.h"

#define LOGI(fmt, ...)  printf("test task: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("test task: ERROR " fmt "\n", ##__VA_ARGS__)

#define TEST_TASK_MAX_SLOTS    8
#define TEST_TASK_STACK        2048
#define TEST_TASK_PRIORITY     100
#define TEST_TASK_PERIOD_MS    100u
#define TEST_TASK_DEFAULT_DUTY 50u
#define TEST_TASK_HB_LOG_EVERY 50u
#define TEST_TASK_STUCK_MS     500u
#define TEST_TASK_WATCH_DEFAULT_SEC 10

struct test_task_metrics_s
{
  volatile uint32_t seq;
  volatile uint32_t overrun;
  volatile uint32_t period_last_us;
  volatile uint32_t period_max_us;
  volatile uint64_t last_hb_us;
};

struct test_task_slot_s
{
  int      pid;
  uint32_t duty_pct;
  bool     pure_busy;
  char     name[16];
  struct test_task_metrics_s metrics;
};

static struct test_task_slot_s g_slots[TEST_TASK_MAX_SLOTS];

static uint32_t g_spawn_duty;
static int      g_spawn_slot;
static bool     g_spawn_busy;

static uint64_t task_now_us(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000000ull +
         (uint64_t)ts.tv_nsec / 1000ull;
}

static void task_metrics_reset(FAR struct test_task_slot_s *slot)
{
  memset((void *)&slot->metrics, 0, sizeof(slot->metrics));
}

static void task_spin_us(uint64_t us)
{
  volatile uint32_t salt = 0;
  uint64_t end = task_now_us() + us;

  while (task_now_us() < end)
    {
      salt = salt * 1664525u + 1013904223u;
    }

  (void)salt;
}

static void task_metrics_after_loop(FAR struct test_task_metrics_s *m,
                                    uint64_t loop_start_us,
                                    uint64_t period_us)
{
  uint64_t now = task_now_us();
  uint64_t elapsed = now - loop_start_us;
  uint32_t el_us = (elapsed > UINT32_MAX) ? UINT32_MAX : (uint32_t)elapsed;

  m->period_last_us = el_us;
  if (el_us > m->period_max_us)
    {
      m->period_max_us = el_us;
    }

  if (elapsed > period_us + (period_us / 2u))
    {
      m->overrun++;
    }

  m->seq++;
  m->last_hb_us = now;
}

static int task_worker_main(int argc, FAR char *argv[])
{
  uint32_t duty = g_spawn_duty;
  int slot = g_spawn_slot;
  FAR struct test_task_slot_s *s = &g_slots[slot];
  char name[16];

  (void)argc;
  (void)argv;

  if (slot < 0 || slot >= TEST_TASK_MAX_SLOTS)
    {
      return EXIT_FAILURE;
    }

  snprintf(name, sizeof(name), "test_load%d", slot);
  syslog(LOG_WARNING, "[test task] %s start duty=%u%% period=%ums",
         name, (unsigned)duty, (unsigned)TEST_TASK_PERIOD_MS);

  while (1)
    {
      uint64_t loop_start = task_now_us();
      uint64_t period_us = (uint64_t)TEST_TASK_PERIOD_MS * 1000ull;
      uint64_t work_us = period_us * (uint64_t)duty / 100ull;
      uint64_t sleep_us = period_us - work_us;
      uint32_t seq_snap;

      if (duty >= 100)
        {
          task_spin_us(period_us);
        }
      else if (duty == 0)
        {
          usleep((useconds_t)period_us);
        }
      else
        {
          if (work_us > 0)
            {
              task_spin_us(work_us);
            }

          if (sleep_us > 0)
            {
              usleep((useconds_t)sleep_us);
            }
        }

      task_metrics_after_loop(&s->metrics, loop_start, period_us);

      seq_snap = s->metrics.seq;
      if ((seq_snap % TEST_TASK_HB_LOG_EVERY) == 0u)
        {
          syslog(LOG_WARNING,
                 "[test task] %s alive duty=%u%% seq=%u last=%uus max=%uus ov=%u",
                 name, (unsigned)duty, (unsigned)seq_snap,
                 (unsigned)s->metrics.period_last_us,
                 (unsigned)s->metrics.period_max_us,
                 (unsigned)s->metrics.overrun);
        }
    }

  return EXIT_SUCCESS;
}

static int task_worker_busy_main(int argc, FAR char *argv[])
{
  int slot = g_spawn_slot;
  FAR struct test_task_slot_s *s = &g_slots[slot];
  volatile uint32_t salt = 0;
  uint64_t loop_start;
  uint64_t period_us = (uint64_t)TEST_TASK_PERIOD_MS * 1000ull;
  char name[16];

  (void)argc;
  (void)argv;

  snprintf(name, sizeof(name), "test_load%d", slot);
  syslog(LOG_WARNING, "[test task] %s start busy=100%% (pure spin)", name);

  loop_start = task_now_us();
  while (1)
    {
      salt = salt * 1664525u + 1013904223u;

      if ((task_now_us() - loop_start) >= period_us)
        {
          task_metrics_after_loop(&s->metrics, loop_start, period_us);
          loop_start = task_now_us();

          if ((s->metrics.seq % TEST_TASK_HB_LOG_EVERY) == 0u)
            {
              syslog(LOG_WARNING, "[test task] %s alive busy seq=%u",
                     name, (unsigned)s->metrics.seq);
            }
        }
    }

  return EXIT_SUCCESS;
}

static bool task_pid_alive(int pid)
{
  return pid > 0 && kill((pid_t)pid, 0) == 0;
}

static int task_alloc_slot(void)
{
  int i;

  for (i = 0; i < TEST_TASK_MAX_SLOTS; i++)
    {
      if (!task_pid_alive(g_slots[i].pid))
        {
          g_slots[i].pid = 0;
          g_slots[i].duty_pct = 0;
          g_slots[i].pure_busy = false;
          g_slots[i].name[0] = '\0';
          task_metrics_reset(&g_slots[i]);
          return i;
        }
    }

  return -1;
}

static const char *task_health_str(FAR struct test_task_slot_s *slot,
                                   uint32_t seq_delta, uint32_t hb_age_ms,
                                   bool have_rate)
{
  uint32_t expect = 1000u / TEST_TASK_PERIOD_MS;

  if (!task_pid_alive(slot->pid))
    {
      return "dead";
    }

  if (hb_age_ms > TEST_TASK_STUCK_MS)
    {
      return "STUCK";
    }

  if (have_rate)
    {
      if (seq_delta == 0)
        {
          return "STUCK";
        }

      if (seq_delta < (expect * 8u) / 10u)
        {
          return "SLOW";
        }
    }

  if (slot->metrics.period_max_us >
      (uint32_t)TEST_TASK_PERIOD_MS * 1500u)
    {
      return "LAG";
    }

  return have_rate ? "OK" : "run";
}

static int task_stop_slot(int slot)
{
  if (slot < 0 || slot >= TEST_TASK_MAX_SLOTS)
    {
      LOGE("invalid slot %d", slot);
      return EXIT_FAILURE;
    }

  if (!task_pid_alive(g_slots[slot].pid))
    {
      g_slots[slot].pid = 0;
      LOGI("slot %d not running", slot);
      return EXIT_SUCCESS;
    }

  if (nxtask_delete((pid_t)g_slots[slot].pid) != OK)
    {
      LOGE("stop slot %d pid=%d failed errno=%d",
           slot, g_slots[slot].pid, errno);
      return EXIT_FAILURE;
    }

  LOGI("stopped slot %d pid=%d (%s)",
       slot, g_slots[slot].pid, g_slots[slot].name);
  g_slots[slot].pid = 0;
  g_slots[slot].duty_pct = 0;
  g_slots[slot].pure_busy = false;
  g_slots[slot].name[0] = '\0';
  task_metrics_reset(&g_slots[slot]);
  return EXIT_SUCCESS;
}

static int task_stop_all(void)
{
  int stopped = 0;
  int i;

  for (i = 0; i < TEST_TASK_MAX_SLOTS; i++)
    {
      if (task_pid_alive(g_slots[i].pid))
        {
          if (task_stop_slot(i) == EXIT_SUCCESS)
            {
              stopped++;
            }
        }
      else
        {
          g_slots[i].pid = 0;
        }
    }

  LOGI("stopped %d worker(s)", stopped);
  return EXIT_SUCCESS;
}

static void task_print_slot_row(int slot, uint32_t seq_delta, uint32_t hb_age_ms,
                                bool have_rate)
{
  FAR struct test_task_slot_s *s = &g_slots[slot];
  FAR const char *health = task_health_str(s, seq_delta, hb_age_ms, have_rate);

  printf(" %d  %5d %3u%% %-5s %6u ",
         slot, s->pid, (unsigned)s->duty_pct, s->name,
         (unsigned)s->metrics.seq);

  if (have_rate)
    {
      printf("%4u/s ", (unsigned)seq_delta);
    }
  else
    {
      printf("  --- ");
    }

  printf("%4ums %5uus %5uus %4u %s\n",
         (unsigned)hb_age_ms,
         (unsigned)s->metrics.period_last_us,
         (unsigned)s->metrics.period_max_us,
         (unsigned)s->metrics.overrun,
         health);
}

static void task_status_header(void)
{
  printf("sl  pid   duty name    seq   rate hb_age last  p_max  ov  health\n");
}

static int task_status_once(uint32_t * prev_seq)
{
  int running = 0;
  uint64_t now = task_now_us();
  int i;

  task_status_header();
  for (i = 0; i < TEST_TASK_MAX_SLOTS; i++)
    {
      uint32_t seq_delta = 0;
      uint32_t hb_age_ms = 9999;
      bool priming = false;
      bool show_rate = false;

      if (!task_pid_alive(g_slots[i].pid))
        {
          if (prev_seq != NULL)
            {
              prev_seq[i] = 0;
            }

          continue;
        }

      running++;
      if (g_slots[i].metrics.last_hb_us > 0 && now >= g_slots[i].metrics.last_hb_us)
        {
          hb_age_ms = (uint32_t)((now - g_slots[i].metrics.last_hb_us) / 1000ull);
        }

      if (prev_seq != NULL)
        {
          priming = (prev_seq[i] == 0 && g_slots[i].metrics.seq > 0);

          if (priming)
            {
              prev_seq[i] = g_slots[i].metrics.seq;
            }
          else
            {
              if (g_slots[i].metrics.seq >= prev_seq[i])
                {
                  seq_delta = g_slots[i].metrics.seq - prev_seq[i];
                }

              prev_seq[i] = g_slots[i].metrics.seq;
              show_rate = true;
            }
        }

      task_print_slot_row(i, seq_delta, hb_age_ms, show_rate);
    }

  if (running == 0)
    {
      LOGI("no workers (max %d slots)", TEST_TASK_MAX_SLOTS);
    }

  return running;
}

static int task_status(void)
{
  if (task_status_once(NULL) > 0)
    {
      LOGI("rate=seq/s over last watch interval; use 'test task watch' to sample");
      LOGI("STUCK=heartbeat stale; SLOW=seq<8/s; LAG=period_max>150ms");
    }

  return EXIT_SUCCESS;
}

static int task_watch(int seconds)
{
  uint32_t prev_seq[TEST_TASK_MAX_SLOTS];
  int pass;

  memset(prev_seq, 0, sizeof(prev_seq));
  LOGI("watch %d s (1 Hz) — seq/s should be ~%u for %ums period",
       seconds, (unsigned)(1000u / TEST_TASK_PERIOD_MS),
       (unsigned)TEST_TASK_PERIOD_MS);

  for (pass = 1; pass <= seconds; pass++)
    {
      int running;

      printf("\n--- watch %d/%d ---\n", pass, seconds);
      running = task_status_once(prev_seq);
      if (running == 0)
        {
          LOGI("no workers left");
          break;
        }

      if (pass == 1)
        {
          printf("(first sample primes rate — see watch 2+)\n");
        }

      if (pass < seconds)
        {
          sleep(1);
        }
    }

  LOGI("done — if health=STUCK/SLOW grows with more workers, system is congested");
  return EXIT_SUCCESS;
}

static int task_start(uint32_t duty, bool pure_busy)
{
  int slot;
  int pid;
  main_t entry;

  slot = task_alloc_slot();
  if (slot < 0)
    {
      LOGE("all %d slots in use — 'test task stop all' first",
           TEST_TASK_MAX_SLOTS);
      return EXIT_FAILURE;
    }

  g_spawn_slot = slot;
  g_spawn_duty = duty;
  g_spawn_busy = pure_busy;
  snprintf(g_slots[slot].name, sizeof(g_slots[slot].name),
           "test_load%d", slot);
  task_metrics_reset(&g_slots[slot]);

  entry = pure_busy ? task_worker_busy_main : task_worker_main;
  pid = task_create(g_slots[slot].name, TEST_TASK_PRIORITY,
                    TEST_TASK_STACK, entry, NULL);
  if (pid < 0)
    {
      LOGE("task_create failed errno=%d", errno);
      g_slots[slot].name[0] = '\0';
      return EXIT_FAILURE;
    }

  g_slots[slot].pid = pid;
  g_slots[slot].duty_pct = pure_busy ? 100u : duty;
  g_slots[slot].pure_busy = pure_busy;

  LOGI("started slot=%d pid=%d name=%s duty=%u%% period=%ums",
       slot, pid, g_slots[slot].name,
       (unsigned)g_slots[slot].duty_pct,
       (unsigned)TEST_TASK_PERIOD_MS);
  LOGI("monitor: test task watch 10");
  return EXIT_SUCCESS;
}

int test_task_main(int argc, FAR char *argv[])
{
  if (argc >= 2)
    {
      if (strcmp(argv[1], "stop") == 0)
        {
          if (argc >= 3)
            {
              if (strcmp(argv[2], "all") == 0)
                {
                  return task_stop_all();
                }

              return task_stop_slot(atoi(argv[2]));
            }

          return task_stop_all();
        }

      if (strcmp(argv[1], "status") == 0)
        {
          return task_status();
        }

      if (strcmp(argv[1], "watch") == 0)
        {
          int sec = TEST_TASK_WATCH_DEFAULT_SEC;

          if (argc >= 3)
            {
              sec = atoi(argv[2]);
              if (sec <= 0)
                {
                  LOGE("invalid watch seconds '%s'", argv[2]);
                  goto usage;
                }
            }

          return task_watch(sec);
        }

      if (strcmp(argv[1], "busy") == 0)
        {
          LOGI("warning: 100%% spin — stop soon if WDT is enabled");
          return task_start(100, true);
        }

      {
        long duty = strtol(argv[1], NULL, 10);

        if (duty < 0 || duty > 100)
          {
            LOGE("duty must be 0..100, got '%s'", argv[1]);
            goto usage;
          }

        return task_start((uint32_t)duty, false);
      }
    }

  return task_start(TEST_TASK_DEFAULT_DUTY, false);

usage:
  printf("Usage:\n");
  printf("  test task [duty_pct]     start worker (default %u%%)\n",
         (unsigned)TEST_TASK_DEFAULT_DUTY);
  printf("  test task busy           100%% spin\n");
  printf("  test task status         health / seq / period stats\n");
  printf("  test task watch [sec]    poll 1 Hz (default %d)\n",
         TEST_TASK_WATCH_DEFAULT_SEC);
  printf("  test task stop [slot|all]\n");
  return EXIT_FAILURE;
}
