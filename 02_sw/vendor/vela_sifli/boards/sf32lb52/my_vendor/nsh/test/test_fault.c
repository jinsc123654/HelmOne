/**
 * @file test_fault.c
 * @brief test fault：板级故障注入（monitor / vela_elf_resolve.py 联调）。
 *
 * NSH：test fault [cases|bt|<case>]。串口标记 FAULT_CASE= / FAULT_BEGIN= /
 * FAULT_CASES_BEGIN/END 供主机脚本 grep。用例名与 test_fault_harness.py 同步。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <assert.h>
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

#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wdiv-by-zero"
#  pragma GCC diagnostic ignored "-Wuninitialized"
#  pragma GCC diagnostic ignored "-Wuse-after-free"
#  pragma GCC diagnostic ignored "-Winfinite-recursion"
#  pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#define LOGI(fmt, ...)  printf("test fault: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("test fault: ERROR " fmt "\n", ##__VA_ARGS__)

#define FAULT_ARM_SEC             2
#define FAULT_CHILD_STACK         768
#define FAULT_CHILD_PRIO          120

/** @brief 主机脚本 grep 的串口标记宏。 */
#define FAULT_TAG(case) \
  do { \
    printf("FAULT_CASE=%s\n", (case)); \
    fflush(stdout); \
  } while (0)

#define FAULT_BEGIN(case) \
  do { \
    printf("FAULT_BEGIN=%s\n", (case)); \
    fflush(stdout); \
  } while (0)

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef CODE void (*fault_trigger_t)(FAR const char *name);

enum fault_child_kind_e
{
  FAULT_CHILD_ASSERT = 0,
  FAULT_CHILD_NULL,
  FAULT_CHILD_UNDEF,
  FAULT_CHILD_DIV0,
  FAULT_CHILD_NKINDS
};

struct fault_case_s
{
  FAR const char *name;
  FAR const char *help;
  fault_trigger_t trigger;
  bool no_countdown;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile enum fault_child_kind_e g_child_kind;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/** @brief 触发前倒计时并打印 FAULT_TAG。 */
static void fault_countdown(FAR const char *name)
{
  FAULT_TAG(name);
  LOGI(">>> %s in %d s (monitor / vela_elf_resolve.py) <<<", name, FAULT_ARM_SEC);
  fflush(stdout);
  sleep(FAULT_ARM_SEC);
}

/** @brief 倒计时后执行故障触发函数。 */
static void fault_fire(FAR const char *name, fault_trigger_t body)
{
  fault_countdown(name);
  FAULT_BEGIN(name);
  body(name);
}

/* --- NuttX / libc paths --- */

static void fault_body_assert(FAR const char *name)
{
  (void)name;
  DEBUGASSERT(0);
}

static void fault_body_panic(FAR const char *name)
{
  (void)name;
  PANIC();
}

static void fault_body_verify(FAR const char *name)
{
  int rc = -EINVAL;

  (void)name;
  DEBUGVERIFY(rc);
}

static void fault_body_abort(FAR const char *name)
{
  (void)name;
  abort();
}

#ifdef CONFIG_LIBC_STRSIGNAL
static void fault_body_sigabrt(FAR const char *name)
{
  (void)name;
  raise(SIGABRT);
}

static void fault_body_sigsegv(FAR const char *name)
{
  (void)name;
  raise(SIGSEGV);
}

static void fault_body_sigfpe(FAR const char *name)
{
  (void)name;
  raise(SIGFPE);
}

static void fault_body_sigill(FAR const char *name)
{
  (void)name;
  raise(SIGILL);
}
#endif

/* --- CPU / memory access --- */

static void fault_body_nullwr(FAR const char *name)
{
  volatile int *p = NULL;

  (void)name;
  *p = 1;
}

static void fault_body_nullrd(FAR const char *name)
{
  volatile int x;
  volatile int *p = NULL;

  (void)name;
  x = *p;
  (void)x;
}

static void fault_body_badcode(FAR const char *name)
{
  void (*fn)(void) = (void (*)(void))0x00000001;

  (void)name;
  fn();
}

static void fault_body_execbad(FAR const char *name)
{
  void (*fn)(void) = (void (*)(void))0xDEADBEEF;

  (void)name;
  fn();
}

static void fault_body_badread(FAR const char *name)
{
  volatile uint32_t x;
  volatile uint32_t *p = (volatile uint32_t *)0xffffffff;

  (void)name;
  x = *p;
  (void)x;
}

static void fault_body_badwrite(FAR const char *name)
{
  volatile uint32_t *p = (volatile uint32_t *)0xe000ed00; /* SCB near 0xE000E000 */

  (void)name;
  *p = 0x12345678u;
}

static void fault_body_unaligned(FAR const char *name)
{
  uint8_t buf[8];
  volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)buf + 1);
  volatile uint32_t x;

  (void)name;
  x = *p;
  (void)x;
}

static void fault_body_undef(FAR const char *name)
{
  (void)name;
#if defined(__GNUC__) && defined(__thumb__)
  __asm volatile (".inst.n 0xde00" ::: "memory");
#else
  __asm volatile (".word 0xe7ffffff" ::: "memory");
#endif
}

static void fault_body_bkpt(FAR const char *name)
{
  (void)name;
#if defined(__GNUC__) && defined(__thumb__)
  __asm volatile ("bkpt #0" ::: "memory");
#else
  __asm volatile ("int3" ::: "memory");
#endif
}

static void fault_body_div0(FAR const char *name)
{
  volatile int a = 1;
  volatile int b = 0;
  volatile int c;

  (void)name;
  c = a / b;
  (void)c;
}

static void fault_body_execram(FAR const char *name)
{
  uint8_t buf[16];
  void (*fn)(void);

  (void)name;
  memset(buf, 0x00, sizeof(buf));
  fn = (void (*)(void))(uintptr_t)buf;
  fn();
}

/* --- stack --- */

static int fault_stack_child(int argc, FAR char *argv[])
{
  volatile char pad[192];

  (void)argc;
  (void)argv;
  pad[0] = pad[sizeof(pad) - 1];
  return fault_stack_child(argc, argv);
}

static void fault_body_stack(FAR const char *name)
{
  pid_t pid;

  LOGI("spawn fault_stack stack=%d", FAULT_CHILD_STACK);
  pid = task_create("fault_stack", FAULT_CHILD_PRIO,
                    FAULT_CHILD_STACK, fault_stack_child, NULL);
  if (pid < 0)
    {
      LOGE("task_create failed errno=%d", errno);
      return;
    }

  LOGI("child pid=%d", pid);
  for (; ; )
    {
      sleep(1);
    }
}

static void fault_recurse_hw(volatile unsigned n)
{
  volatile char pad[256];

  pad[0] = (char)n;
  if (n > 0)
    {
      fault_recurse_hw(n - 1);
    }
  else
    {
      fault_recurse_hw(32);
    }
}

static void fault_body_stackhw(FAR const char *name)
{
  (void)name;
  fault_recurse_hw(64);
}

static void fault_body_stackovf(FAR const char *name)
{
  volatile char pad[1024];

  (void)name;
  pad[0] = pad[sizeof(pad) - 1];
  fault_body_stackovf(name);
}

/* --- heap --- */

static void fault_body_heapdf(FAR const char *name)
{
  void *p;

  (void)name;
  p = malloc(48);
  if (p == NULL)
    {
      LOGE("malloc failed");
      return;
    }

  free(p);
  free(p);
}

static void fault_body_heapuaf(FAR const char *name)
{
  volatile uint8_t *p;

  (void)name;
  p = (volatile uint8_t *)malloc(64);
  if (p == NULL)
    {
      LOGE("malloc failed");
      return;
    }

  p[0] = 0xaa;
  free((void *)p);
  p[32] = 0x55;
}

static void fault_body_heapovf(FAR const char *name)
{
  uint8_t *p;

  (void)name;
  p = (uint8_t *)malloc(16);
  if (p == NULL)
    {
      LOGE("malloc failed");
      return;
    }

  memset(p, 0xcc, 256);
  free(p);
}

/* --- child task faults --- */

static int fault_child_entry(int argc, FAR char *argv[])
{
  enum fault_child_kind_e kind = g_child_kind;

  (void)argc;
  (void)argv;

  switch (kind)
    {
      case FAULT_CHILD_ASSERT:
        DEBUGASSERT(0);
        break;
      case FAULT_CHILD_NULL:
        *(volatile int *)0 = 1;
        break;
      case FAULT_CHILD_UNDEF:
#if defined(__GNUC__) && defined(__thumb__)
        __asm volatile (".inst.n 0xde00" ::: "memory");
#else
        __asm volatile (".word 0xe7ffffff" ::: "memory");
#endif
        break;
      case FAULT_CHILD_DIV0:
        {
          volatile int c = 1 / 0;
          (void)c;
        }
        break;
      default:
        PANIC();
        break;
    }

  return EXIT_FAILURE;
}

static void fault_spawn_child(FAR const char *name, enum fault_child_kind_e kind)
{
  char tname[20];
  pid_t pid;

  snprintf(tname, sizeof(tname), "fault_%s", name);
  g_child_kind = kind;
  pid = task_create(tname, FAULT_CHILD_PRIO, FAULT_CHILD_STACK,
                    fault_child_entry, NULL);
  if (pid < 0)
    {
      LOGE("task_create(%s) errno=%d", tname, errno);
      return;
    }

  LOGI("child %s pid=%d", tname, pid);
  for (; ; )
    {
      sleep(1);
    }
}

static void fault_body_childassert(FAR const char *name)
{
  fault_spawn_child(name, FAULT_CHILD_ASSERT);
}

static void fault_body_childnull(FAR const char *name)
{
  fault_spawn_child(name, FAULT_CHILD_NULL);
}

static void fault_body_childundef(FAR const char *name)
{
  fault_spawn_child(name, FAULT_CHILD_UNDEF);
}

static void fault_body_childdiv0(FAR const char *name)
{
  fault_spawn_child(name, FAULT_CHILD_DIV0);
}

/* --- wrappers with countdown --- */

static void fault_run(FAR const char *name, fault_trigger_t body)
{
  if (body == NULL)
    {
      return;
    }

  fault_fire(name, body);
  LOGI("case '%s' returned (unexpected)", name);
}

/****************************************************************************
 * Case table — names synced with test_fault_harness.py
 ****************************************************************************/

static const struct fault_case_s g_fault_cases[] =
{
  /* meta */
  { "bt",          "sched_dumpstack [pid], no crash", NULL, true },

  /* NuttX / libc */
  { "assert",      "DEBUGASSERT(0)", fault_body_assert },
  { "panic",       "PANIC()", fault_body_panic },
  { "verify",      "DEBUGVERIFY(-EINVAL)", fault_body_verify },
  { "abort",       "abort()", fault_body_abort },
#ifdef CONFIG_LIBC_STRSIGNAL
  { "sigabrt",     "raise(SIGABRT)", fault_body_sigabrt },
  { "sigsegv",     "raise(SIGSEGV)", fault_body_sigsegv },
  { "sigfpe",      "raise(SIGFPE)", fault_body_sigfpe },
  { "sigill",      "raise(SIGILL)", fault_body_sigill },
#endif

  /* CPU / bus */
  { "nullwr",      "store through NULL", fault_body_nullwr },
  { "nullrd",      "load through NULL", fault_body_nullrd },
  { "badcode",     "branch to 0x1 (HardFault)", fault_body_badcode },
  { "execbad",     "call 0xDEADBEEF", fault_body_execbad },
  { "badread",     "load from 0xFFFFFFFF", fault_body_badread },
  { "badwrite",    "store to SCB-ish 0xE000ED00", fault_body_badwrite },
  { "unaligned",   "unaligned uint32_t load", fault_body_unaligned },
  { "undef",       "UDF #0 instruction", fault_body_undef },
  { "bkpt",        "BKPT #0", fault_body_bkpt },
  { "div0",        "integer divide by zero", fault_body_div0 },
  { "execram",     "call into SRAM buffer", fault_body_execram },

  /* stack */
  { "stack",       "child task stack overflow (768B stack)", fault_body_stack },
  { "stackhw",     "deep recursion (HW stack check)", fault_body_stackhw },
  { "stackovf",    "recursive large stack frames (current task)", fault_body_stackovf },

  /* heap */
  { "heapdf",      "double free", fault_body_heapdf },
  { "heapuaf",     "use-after-free", fault_body_heapuaf },
  { "heapovf",     "write past malloc chunk", fault_body_heapovf },

  /* child task */
  { "childassert", "child DEBUGASSERT", fault_body_childassert },
  { "childnull",   "child NULL store", fault_body_childnull },
  { "childundef",  "child UDF", fault_body_childundef },
  { "childdiv0",   "child divide by zero", fault_body_childdiv0 },

  /* legacy aliases */
  { "null",        "alias of nullwr", fault_body_nullwr },
  { "heap",        "alias of heapdf", fault_body_heapdf },
};

#define FAULT_NCASES (sizeof(g_fault_cases) / sizeof(g_fault_cases[0]))

/** @brief 打印机器可读用例列表（FAULT_CASES_BEGIN/END）。 */
static void fault_print_machine_list(void)
{
  size_t i;

  printf("FAULT_CASES_BEGIN\n");
  for (i = 0; i < FAULT_NCASES; i++)
    {
      if (g_fault_cases[i].trigger != NULL)
        {
          printf("%s\n", g_fault_cases[i].name);
        }
    }
  printf("FAULT_CASES_END\n");
}

/** @brief 打印 test fault 用法与用例表。 */
static void fault_usage(void)
{
  size_t i;

  printf("Usage: test fault <case>\n");
  printf("       test fault cases     machine-readable list for host scripts\n");
  printf("Host markers: FAULT_CASE= / FAULT_BEGIN= / FAULT_CASES_BEGIN\n");
  printf("Cases:\n");
  for (i = 0; i < FAULT_NCASES; i++)
    {
      printf("  %-12s %s\n", g_fault_cases[i].name, g_fault_cases[i].help);
    }
}

/** @brief 执行 bt 子命令：sched_dumpstack，不崩溃。 */
static int fault_run_bt(int argc, FAR char *argv[])
{
  pid_t tid;

  if (argc >= 2)
    {
      tid = (pid_t)atoi(argv[1]);
      if (tid <= 0)
        {
          LOGE("invalid pid '%s'", argv[1]);
          return EXIT_FAILURE;
        }
    }
  else
    {
      tid = getpid();
    }

#ifndef CONFIG_SCHED_BACKTRACE
  LOGE("CONFIG_SCHED_BACKTRACE is disabled");
  return EXIT_FAILURE;
#else
  FAULT_TAG("bt");
  LOGI("sched_dumpstack(tid=%d)", tid);
  sched_dumpstack(tid);
  return EXIT_SUCCESS;
#endif
}

/** @brief 按名称查找故障用例表项。 */
static const struct fault_case_s *fault_lookup(FAR const char *name)
{
  size_t i;

  for (i = 0; i < FAULT_NCASES; i++)
    {
      if (strcmp(name, g_fault_cases[i].name) == 0)
        {
          return &g_fault_cases[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief test fault 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_fault_main(int argc, FAR char *argv[])
{
  FAR const char *name;
  FAR const struct fault_case_s *fc;

  if (argc < 2)
    {
      fault_usage();
      fault_print_machine_list();
      return EXIT_SUCCESS;
    }

  name = argv[1];
  if (strcmp(name, "help") == 0 || strcmp(name, "list") == 0)
    {
      fault_usage();
      return EXIT_SUCCESS;
    }

  if (strcmp(name, "cases") == 0)
    {
      fault_print_machine_list();
      return EXIT_SUCCESS;
    }

  if (strcmp(name, "bt") == 0)
    {
      return fault_run_bt(argc - 1, &argv[1]);
    }

  fc = fault_lookup(name);
  if (fc == NULL)
    {
      LOGE("unknown case '%s'", name);
      fault_usage();
      return EXIT_FAILURE;
    }

  if (fc->trigger == NULL)
    {
      LOGE("case '%s' has no trigger", name);
      return EXIT_FAILURE;
    }

  fault_run(name, fc->trigger);
  return EXIT_FAILURE;
}
