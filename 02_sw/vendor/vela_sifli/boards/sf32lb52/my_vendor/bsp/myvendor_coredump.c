/**
 * @file myvendor_coredump.c
 * @brief 把崩溃时的 ARM 寄存器组写到 `/mnt/kv/coredump/nNNN_YYYYMMDD_HHMMSS.txt`。
 * 文件名和 stamp 用本地时（persist.ui.tz_min，默认 UTC+8）；unix= 仍是 UTC。
 *
 * 这不是 ELF 核心转储（不包含整段内存）。保存的是与 syslog
 * `up_dump_register` 同一套上下文：R0–R15、xPSR、BASEPRI、CONTROL、
 * EXC_RETURN、SCB fault，以及各任务 TCB 里挂起的 xcp.regs。
 *
 * HardFault / assert 里不写盘：先停 LCDC/EPIC DMA，把 LVGL 片内条带当
 * note / 崩溃栈 / SD 512 暂存。panic notifier 在 dump_assert_info 之前
 * 抄现场并退到裸机写 `/coredump/`，避免 SysTick 里递归 assert 丢文件。
 * LCD 未起来时用很小的 BSS 后备。SD 崩溃路径仍走 PIO（无 DMA IRQ）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#if defined(CONFIG_MYVENDOR_COREDUMP) && CONFIG_MYVENDOR_COREDUMP

#include "myvendor_coredump.h"
#include "myvendor_crash_lfs.h"
#include "myvendor_devctl.h"
#include "myvendor_gnss.h"
#include "myvendor_schedmon.h"
#include "myvendor_sound.h"
#include "myvendor_watchdog.h"
#include "ptab_sdmmc.h"
#include "ptab_table.h"
#include "sf32lb_sdio.h"
#include "bf0_hal.h"
#include "bf0_hal_rcc.h"

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/irq.h>
#include <nuttx/notifier.h>
#include <nuttx/panic_notifier.h>
#include <nuttx/sched.h>
#include <nuttx/wdog.h>

#include <arch/irq.h>
#include <sched.h>

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <nuttx/clock.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef CONFIG_MYVENDOR_COREDUMP_KEEP
#  define CONFIG_MYVENDOR_COREDUMP_KEEP 10
#endif

#define COREDUMP_SEQ_PATH   "/coredump/seq"
#define COREDUMP_DIR_LFS    "/coredump"
#define COREDUMP_BUF_BYTES  16384
#define COREDUMP_LFS_READ   512u
#define COREDUMP_LFS_BLOCK  4096u
#define COREDUMP_STACK_BYTES 8192u
#define COREDUMP_FB_WORK_BYTES \
  (COREDUMP_BUF_BYTES + COREDUMP_LFS_READ * 4u + 32u)
#define COREDUMP_FALLBACK_NOTE  2048
#define COREDUMP_FALLBACK_STACK_WORDS 512
#define HCPU_SRAM_BASE  0x20000000u
#define HCPU_SRAM_HEAP_END 0x2007FC00u

#define SCB_ICSR            0xe000ed04u
#define SCB_SHCSR           0xe000ed24u
#define SCB_CFSR            0xe000ed28u
#define SCB_HFSR            0xe000ed2cu
#define SCB_DFSR            0xe000ed30u
#define SCB_MMFAR           0xe000ed34u
#define SCB_BFAR            0xe000ed38u
#define SCB_AFSR            0xe000ed3cu

static char g_note_fallback[COREDUMP_FALLBACK_NOTE];
static uint32_t g_stack_fallback[COREDUMP_FALLBACK_STACK_WORDS]
                __attribute__((aligned(8)));
static uint8_t g_lfs_read_fb[COREDUMP_LFS_READ] __attribute__((aligned(8)));
static uint8_t g_lfs_prog_fb[COREDUMP_LFS_READ] __attribute__((aligned(8)));
static uint8_t g_lfs_look_fb[8] __attribute__((aligned(8)));
static uint8_t g_lfs_file_fb[COREDUMP_LFS_READ] __attribute__((aligned(8)));
static uint8_t g_sd_scratch_fb[COREDUMP_LFS_READ] __attribute__((aligned(8)));

static char *g_note;
static size_t g_note_cap;
static size_t g_note_len;
static uintptr_t g_crash_sp_top;
static uint8_t *g_lfs_read;
static uint8_t *g_lfs_prog;
static uint8_t *g_lfs_look;
static uint8_t *g_lfs_file;
static uint8_t *g_sd_scratch;
static volatile bool g_busy;
static bool g_nb_on;
static char g_fmt[192];
static struct tm g_crash_tm;
static bool g_crash_tm_ok;
static int16_t g_crash_tz_min;

static void *g_fb_a;
static size_t g_fb_a_bytes;
static void *g_fb_b;
static size_t g_fb_b_bytes;

static lfs_t g_crash_lfs;
static struct lfs_config g_crash_cfg;
static uint64_t g_kv_off;
static uint32_t g_kv_blocks;

static uint32_t scb_read(uint32_t addr)
{
  return *(volatile uint32_t *)addr;
}

static bool crash_ptr_in_sram(const void *p, size_t n)
{
  uintptr_t a = (uintptr_t)p;
  uintptr_t e;

  if (p == NULL || n == 0)
    {
      return false;
    }

  e = a + n;
  if (e < a)
    {
      return false;
    }

  return a >= HCPU_SRAM_BASE && e <= HCPU_SRAM_HEAP_END;
}

/** @brief 停 LCDC/EPIC，避免还在扫 LVGL 条带。 */
static void crash_halt_lcd_dma(void)
{
  up_disable_irq(LCDC1_IRQn + NVIC_IRQ_FIRST);
  up_disable_irq(EPIC_IRQn + NVIC_IRQ_FIRST);

  DMA1_Channel5->CCR = 0;
  DMA1_Channel7->CCR = 0;
  DMA1_Channel8->CCR = 0;

  /* 只复位，不走 HAL_RCC_DisableModule（里头会改 PRIMASK）。 */
  HAL_RCC_ResetModule(RCC_MOD_LCDC1);
  HAL_RCC_ResetModule(RCC_MOD_EPIC);

  __DSB();
  __ISB();
}

static void crash_use_fallback_bufs(void)
{
  g_note = g_note_fallback;
  g_note_cap = sizeof(g_note_fallback);
  g_crash_sp_top = (uintptr_t)&g_stack_fallback[COREDUMP_FALLBACK_STACK_WORDS];
  g_lfs_read = g_lfs_read_fb;
  g_lfs_prog = g_lfs_prog_fb;
  g_lfs_look = g_lfs_look_fb;
  g_lfs_file = g_lfs_file_fb;
  g_sd_scratch = g_sd_scratch_fb;
}

static uint8_t *crash_place_work(uint8_t *base)
{
  uint8_t *p = base;

  g_note = (char *)p;
  g_note_cap = COREDUMP_BUF_BYTES;
  p += COREDUMP_BUF_BYTES;
  g_lfs_read = p;
  p += COREDUMP_LFS_READ;
  g_lfs_prog = p;
  p += COREDUMP_LFS_READ;
  g_lfs_file = p;
  p += COREDUMP_LFS_READ;
  g_sd_scratch = p;
  p += COREDUMP_LFS_READ;
  g_lfs_look = p;
  p += 8;
  return p;
}

/** @brief 停显示 DMA，条带够则占用，否则 BSS 后备。 */
static void crash_claim_fb(void)
{
  uint8_t *work = NULL;
  size_t work_n = 0;
  uint8_t *stack = NULL;
  size_t stack_n = 0;

  crash_halt_lcd_dma();

  if (crash_ptr_in_sram(g_fb_a, g_fb_a_bytes) &&
      crash_ptr_in_sram(g_fb_b, g_fb_b_bytes) &&
      g_fb_a_bytes >= COREDUMP_FB_WORK_BYTES &&
      g_fb_b_bytes >= COREDUMP_STACK_BYTES)
    {
      if (g_fb_a_bytes >= g_fb_b_bytes)
        {
          work = (uint8_t *)g_fb_a;
          work_n = g_fb_a_bytes;
          stack = (uint8_t *)g_fb_b;
          stack_n = g_fb_b_bytes;
        }
      else
        {
          work = (uint8_t *)g_fb_b;
          work_n = g_fb_b_bytes;
          stack = (uint8_t *)g_fb_a;
          stack_n = g_fb_a_bytes;
        }
    }
  else if (crash_ptr_in_sram(g_fb_a, g_fb_a_bytes) &&
           g_fb_a_bytes >= COREDUMP_FB_WORK_BYTES + COREDUMP_STACK_BYTES)
    {
      work = (uint8_t *)g_fb_a;
      work_n = g_fb_a_bytes - COREDUMP_STACK_BYTES;
      stack = work + work_n;
      stack_n = COREDUMP_STACK_BYTES;
    }
  else if (crash_ptr_in_sram(g_fb_b, g_fb_b_bytes) &&
           g_fb_b_bytes >= COREDUMP_FB_WORK_BYTES + COREDUMP_STACK_BYTES)
    {
      work = (uint8_t *)g_fb_b;
      work_n = g_fb_b_bytes - COREDUMP_STACK_BYTES;
      stack = work + work_n;
      stack_n = COREDUMP_STACK_BYTES;
    }

  (void)work_n;

  if (work == NULL || stack == NULL)
    {
      crash_use_fallback_bufs();
      return;
    }

  (void)crash_place_work(work);
  g_crash_sp_top = ((uintptr_t)stack + stack_n) & ~7ul;
}

void myvendor_coredump_bind_fb(void *a, size_t a_bytes, void *b,
                               size_t b_bytes)
{
  if (!crash_ptr_in_sram(a, a_bytes) || !crash_ptr_in_sram(b, b_bytes))
    {
      g_fb_a = NULL;
      g_fb_a_bytes = 0;
      g_fb_b = NULL;
      g_fb_b_bytes = 0;
      syslog(LOG_ERR, "[coredump] bind_fb rejected (not HCPU SRAM)\n");
      return;
    }

  g_fb_a = a;
  g_fb_a_bytes = a_bytes;
  g_fb_b = b;
  g_fb_b_bytes = b_bytes;
  syslog(LOG_INFO,
         "[coredump] lcd fb A %p %u B %p %u\n",
         a, (unsigned)a_bytes, b, (unsigned)b_bytes);
}

void myvendor_coredump_unbind_fb(void)
{
  g_fb_a = NULL;
  g_fb_a_bytes = 0;
  g_fb_b = NULL;
  g_fb_b_bytes = 0;
}

static uint32_t read_msp(void)
{
  uint32_t msp;

  __asm__ __volatile__("mrs %0, msp" : "=r" (msp) : : "memory");
  return msp;
}

static int note_append(size_t *off, const char *fmt, ...)
{
  va_list ap;
  int n;
  size_t cap;

  if (g_note == NULL || g_note_cap < 2)
    {
      return -1;
    }

  cap = g_note_cap;
  if (*off >= cap - 1)
    {
      return 0;
    }

  va_start(ap, fmt);
  n = vsnprintf(g_note + *off, cap - *off, fmt, ap);
  va_end(ap);
  if (n < 0)
    {
      return -1;
    }

  if ((size_t)n >= cap - *off)
    {
      *off = cap - 1;
    }
  else
    {
      *off += (size_t)n;
    }

  return 0;
}

static void note_arm_regs(size_t *off, FAR const uint32_t *r)
{
  int i;

  if (r == NULL)
    {
      note_append(off, "regs=(null)\n");
      return;
    }

  note_append(off,
              "r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
              (unsigned long)r[REG_R0], (unsigned long)r[REG_R1],
              (unsigned long)r[REG_R2], (unsigned long)r[REG_R3]);
  note_append(off,
              "r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
              (unsigned long)r[REG_R4], (unsigned long)r[REG_R5],
              (unsigned long)r[REG_R6], (unsigned long)r[REG_R7]);
  note_append(off,
              "r8=%08lx r9=%08lx r10=%08lx r11=%08lx\n",
              (unsigned long)r[REG_R8], (unsigned long)r[REG_R9],
              (unsigned long)r[REG_R10], (unsigned long)r[REG_R11]);
  note_append(off,
              "r12=%08lx sp=%08lx lr=%08lx pc=%08lx\n",
              (unsigned long)r[REG_R12], (unsigned long)r[REG_SP],
              (unsigned long)r[REG_LR], (unsigned long)r[REG_PC]);
  note_append(off,
              "xpsr=%08lx basepri=%08lx control=%08lx exc_return=%08lx\n",
              (unsigned long)r[REG_XPSR],
#ifdef REG_BASEPRI
              (unsigned long)r[REG_BASEPRI],
#else
              0ul,
#endif
              (unsigned long)r[REG_CONTROL],
#ifdef REG_EXC_RETURN
              (unsigned long)r[REG_EXC_RETURN]
#else
              0ul
#endif
              );

#ifdef CONFIG_ARCH_FPU
  note_append(off, "fpscr=%08lx\n", (unsigned long)r[REG_FPSCR]);
#endif

  note_append(off, "rawregs:");
  for (i = 0; i < XCPTCONTEXT_REGS; i++)
    {
      if ((i & 7) == 0)
        {
          note_append(off, "\n");
        }

      note_append(off, " %08lx", (unsigned long)r[i]);
    }

  note_append(off, "\n");
}

static void note_scb_fault(size_t *off)
{
  note_append(off,
              "icsr=%08lx shcsr=%08lx cfsr=%08lx hfsr=%08lx\n",
              (unsigned long)scb_read(SCB_ICSR),
              (unsigned long)scb_read(SCB_SHCSR),
              (unsigned long)scb_read(SCB_CFSR),
              (unsigned long)scb_read(SCB_HFSR));
  note_append(off,
              "dfsr=%08lx mmfar=%08lx bfar=%08lx afsr=%08lx\n",
              (unsigned long)scb_read(SCB_DFSR),
              (unsigned long)scb_read(SCB_MMFAR),
              (unsigned long)scb_read(SCB_BFAR),
              (unsigned long)scb_read(SCB_AFSR));
}

static void note_hw_cpu(size_t *off)
{
  note_append(off,
              "hw ipsr=%08lx msp=%08lx psp=%08lx control=%08lx "
              "basepri=%02x\n",
              (unsigned long)getipsr(),
              (unsigned long)read_msp(),
              (unsigned long)getpsp(),
              (unsigned long)getcontrol(),
              (unsigned)getbasepri());
}

static void note_one_task(FAR struct tcb_s *tcb, FAR void *arg)
{
  size_t *off = (size_t *)arg;
  FAR uint32_t *r;

  if (tcb == NULL || off == NULL)
    {
      return;
    }

  note_append(off, "task pid=%d pri=%d st=%u lock=%d wait=%p wd=%d name=%s\n",
              (int)tcb->pid, (int)tcb->sched_priority,
              (unsigned)tcb->task_state, (int)tcb->lockcount,
              tcb->waitobj, WDOG_ISACTIVE(&tcb->waitdog) ? 1 : 0,
              get_task_name(tcb));

  r = tcb->xcp.regs;
  if (tcb == running_task() && !up_interrupt_context())
    {
      note_append(off, "regs=running (xcp stale)\n");
    }
    else
    {
      note_arm_regs(off, r);
    }
}

static void note_board_snap(size_t *off)
{
  /* 崩溃路径：快照结构体几百字节，不能放栈上。 */
  static struct myvendor_schedmon_snap_s ls;
  const struct myvendor_schedmon_snap_s *s = &ls;
  const struct myvendor_schedmon_null_s *e;
  bool ok;
  unsigned i;
  int n;

  /* 原来直接拿 myvendor_schedmon_snap() 的指针裸读，写侧虽然是 seqlock
   * （写时 gen 奇数）却没人校验 —— note 里可能混进两个 tick 的数据。
   * 这里改为拷贝 + 校验；拿不到一致副本就清零，宁可全 0 也不要假数据。 */
  ok = myvendor_schedmon_snap_copy(&ls);
  if (!ok)
    {
      memset(&ls, 0, sizeof(ls));
    }

  note_append(off, "schedmon copy_ok=%d\n", ok ? 1 : 0);
  note_append(off,
              "schedmon tasks=%u lockmax=%d waitsem=%u semnull=%u "
              "mutexbad=%u readyorphan=%u gen=%u\n",
              (unsigned)s->ntask, (int)s->lockmax,
              (unsigned)s->n_wait_sem, (unsigned)s->null_n,
              (unsigned)s->mutex_bad_n, (unsigned)s->ready_orphan_n,
              (unsigned)s->gen);

  if (s->null_n != 0)
    {
      e = &s->last_null;
      note_append(off,
                  "semnull n=%u err=%d irq=%lu "
                  "wt pid=%d %s st=%u pri=%d "
                  "run pid=%d %s st=%u lock=%d\n",
                  (unsigned)e->seq, (int)e->errcode,
                  (unsigned long)e->ipsr,
                  (int)e->wt_pid, e->wt_name, (unsigned)e->wt_state,
                  (int)e->wt_pri,
                  (int)e->run_pid, e->run_name, (unsigned)e->run_state,
                  (int)e->run_lock);
    }

  if (s->ready_orphan_n != 0)
    {
      e = &s->last_ready_orphan;
      note_append(off,
                  "readyorphan n=%u "
                  "head pid=%d %s st=%u pri=%d "
                  "idle pid=%d %s st=%u\n",
                  (unsigned)e->seq,
                  (int)e->wt_pid, e->wt_name, (unsigned)e->wt_state,
                  (int)e->wt_pri,
                  (int)e->run_pid, e->run_name, (unsigned)e->run_state);
    }
  if (s->mutex_bad_n != 0)
    {
      const struct myvendor_schedmon_mutex_s *m = &s->last_mutex;

      note_append(off,
                  "mutexbad n=%u holder=%ld tid=%d run=%d "
                  "this=%s st=%u run=%s st=%u\n",
                  (unsigned)m->seq, (long)m->holder,
                  (int)m->tid, (int)m->run_pid,
                  m->this_name, (unsigned)m->this_st,
                  m->run_name, (unsigned)m->run_st);
    }

  for (i = 0; i < s->ntask; i++)
    {
      const struct myvendor_schedmon_task_s *t = &s->task[i];

      /* stk=已用/总大小：栈打穿会表现成"内存被人写坏"的样子（现场 val 是垃圾、
       * 相关结构莫名错位），必须能从转储里一眼排除，否则每次都要回头查一遍。
       * 这两个字段快照里早就有（见 myvendor_schedmon_task_s），只是这里漏打了，
       * 于是 console 上的 `sched: task … stk=` 有水位、崩溃转储里反而没有。 */
      note_append(off,
                  "snap pid=%d %s pri=%d st=%u lock=%d wait=%p wd=%u stk=%u/%u\n",
                  (int)t->pid, t->name, (int)t->pri, (unsigned)t->state,
                  (int)t->lockcount, (void *)t->waitobj,
                  (unsigned)t->waitdog, (unsigned)t->stack_used,
                  (unsigned)t->stack_size);
    }

  n = myvendor_gnss_crash_format(g_fmt, sizeof(g_fmt));
  if (n > 0)
    {
      note_append(off, "%s\n", g_fmt);
    }

  n = myvendor_sound_crash_format(g_fmt, sizeof(g_fmt));
  if (n > 0)
    {
      note_append(off, "%s\n", g_fmt);
    }
}

static void coredump_enter_baremetal(size_t len);

static int crash_kv_read(const struct lfs_config *c, lfs_block_t block,
                         lfs_off_t off, void *buffer, lfs_size_t size)
{
  uint64_t addr = g_kv_off + (uint64_t)block * c->block_size + (uint64_t)off;
  uint8_t *dst = (uint8_t *)buffer;

  (void)c;
  while (size != 0)
    {
      lfs_size_t n = (size < COREDUMP_LFS_READ) ? size : COREDUMP_LFS_READ;
      uint32_t head = (uint32_t)(addr & (COREDUMP_LFS_READ - 1u));
      uint64_t aligned = addr - head;

      if (sf32lb_sd_crash_read512(aligned, g_sd_scratch) < 0)
        {
          return LFS_ERR_IO;
        }

      memcpy(dst, g_sd_scratch + head, n);
      dst += n;
      addr += n;
      size -= n;
    }

  return 0;
}

static int crash_kv_prog(const struct lfs_config *c, lfs_block_t block,
                         lfs_off_t off, const void *buffer, lfs_size_t size)
{
  uint64_t addr = g_kv_off + (uint64_t)block * c->block_size + (uint64_t)off;
  const uint8_t *src = (const uint8_t *)buffer;

  (void)c;
  while (size != 0)
    {
      lfs_size_t n = (size < COREDUMP_LFS_READ) ? size : COREDUMP_LFS_READ;
      uint32_t head = (uint32_t)(addr & (COREDUMP_LFS_READ - 1u));
      uint64_t aligned = addr - head;

      if (head != 0 || n != COREDUMP_LFS_READ)
        {
          if (sf32lb_sd_crash_read512(aligned, g_sd_scratch) < 0)
            {
              return LFS_ERR_IO;
            }

          memcpy(g_sd_scratch + head, src, n);
          if (sf32lb_sd_crash_write512(aligned, g_sd_scratch) < 0)
            {
              return LFS_ERR_IO;
            }
        }
      else if (sf32lb_sd_crash_write512(aligned, src) < 0)
        {
          return LFS_ERR_IO;
        }

      src += n;
      addr += n;
      size -= n;
    }

  return 0;
}

static int crash_kv_erase(const struct lfs_config *c, lfs_block_t block)
{
  uint64_t addr = g_kv_off + (uint64_t)block * c->block_size;
  unsigned i;

  (void)c;
  memset(g_sd_scratch, 0xff, COREDUMP_LFS_READ);
  for (i = 0; i < (COREDUMP_LFS_BLOCK / COREDUMP_LFS_READ); i++)
    {
      if (sf32lb_sd_crash_write512(addr, g_sd_scratch) < 0)
        {
          return LFS_ERR_IO;
        }

      addr += COREDUMP_LFS_READ;
    }

  return 0;
}

static int crash_kv_sync(const struct lfs_config *c)
{
  (void)c;
  return 0;
}

static int crash_lfs_put(const char *path, const void *data, lfs_size_t size)
{
  lfs_file_t file;
  struct lfs_file_config fcfg;
  lfs_ssize_t n;
  int err;

  memset(&fcfg, 0, sizeof(fcfg));
  fcfg.buffer = g_lfs_file;
  err = lfs_file_opencfg(&g_crash_lfs, &file, path,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fcfg);
  if (err != 0)
    {
      return err;
    }

  n = 0;
  if (size > 0)
    {
      n = lfs_file_write(&g_crash_lfs, &file, data, size);
      if (n < 0)
        {
          (void)lfs_file_close(&g_crash_lfs, &file);
          return (int)n;
        }
    }

  err = lfs_file_sync(&g_crash_lfs, &file);
  (void)lfs_file_close(&g_crash_lfs, &file);
  if (n != (lfs_ssize_t)size)
    {
      return LFS_ERR_IO;
    }

  return err;
}

static bool crash_tm_sane(const struct tm *tm)
{
  return tm != NULL &&
         tm->tm_year >= 70 && tm->tm_year < 200 &&
         tm->tm_mon >= 0 && tm->tm_mon <= 11 &&
         tm->tm_mday >= 1 && tm->tm_mday <= 31 &&
         tm->tm_hour >= 0 && tm->tm_hour <= 23 &&
         tm->tm_min >= 0 && tm->tm_min <= 59 &&
         tm->tm_sec >= 0 && tm->tm_sec <= 60;
}

/** @brief UTC 日历 → unix 秒。IRQ 里不用 mktime（会抢 TZ/timer 锁）。 */
static time_t crash_civil_unix(const struct tm *tm)
{
  int y;
  unsigned m;
  unsigned d;
  int64_t era;
  unsigned yoe;
  unsigned doy;
  unsigned doe;
  int64_t days;

  y = tm->tm_year + 1900;
  m = (unsigned)tm->tm_mon + 1u;
  d = (unsigned)tm->tm_mday;
  y -= (m <= 2u) ? 1 : 0;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153u * (m + (m > 2u ? -3u : 9u)) + 2u) / 5u + d - 1u;
  doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  days = era * 146097 + (int64_t)doe - 719468;
  return (time_t)(days * 86400 +
                   (int64_t)tm->tm_hour * 3600 +
                   (int64_t)tm->tm_min * 60 +
                   (int64_t)tm->tm_sec);
}

/** @brief unix 秒 → 日历（无 libc）。 */
static void crash_unix_civil(time_t sec, struct tm *tm)
{
  int64_t z;
  int64_t era;
  unsigned doe;
  unsigned yoe;
  int y;
  unsigned doy;
  unsigned mp;
  unsigned hh;
  unsigned mi;
  unsigned ss;
  int64_t days;
  int rem;

  memset(tm, 0, sizeof(*tm));
  if (sec < 0)
    {
      sec = 0;
    }

  days = (int64_t)sec / 86400;
  rem = (int)((int64_t)sec % 86400);
  hh = (unsigned)(rem / 3600);
  mi = (unsigned)((rem % 3600) / 60);
  ss = (unsigned)(rem % 60);

  z = days + 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);
  yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  y = (int)yoe + (int)era * 400;
  doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  mp = (5u * doy + 2u) / 153u;
  tm->tm_mday = (int)(doy - (153u * mp + 2u) / 5u + 1u);
  tm->tm_mon = (int)(mp < 10u ? mp + 3u : mp - 9u) - 1;
  tm->tm_year = y + (tm->tm_mon <= 1) - 1900;
  tm->tm_hour = (int)hh;
  tm->tm_min = (int)mi;
  tm->tm_sec = (int)ss;
}

/** @brief IRQ 里不走 clock_gettime（会抢 timer 锁）；直接读 RTC 寄存器。 */
static void crash_capture_time(time_t *unix_out)
{
  time_t utc = 0;

  memset(&g_crash_tm, 0, sizeof(g_crash_tm));
  g_crash_tm_ok = false;
  g_crash_tz_min = myvendor_devctl_tz_min_peek();
  if (unix_out != NULL)
    {
      *unix_out = 0;
    }

#if defined(CONFIG_RTC) && defined(CONFIG_RTC_DATETIME)
  if (up_rtc_getdatetime(&g_crash_tm) == 0 && crash_tm_sane(&g_crash_tm))
    {
      utc = crash_civil_unix(&g_crash_tm);
      g_crash_tm_ok = true;
    }
#endif

  if (!g_crash_tm_ok && !up_interrupt_context())
    {
      struct timespec ts;

      ts.tv_sec = 0;
      ts.tv_nsec = 0;
      if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 0)
        {
          utc = ts.tv_sec;
          crash_unix_civil(utc, &g_crash_tm);
          g_crash_tm_ok = crash_tm_sane(&g_crash_tm);
        }
    }

  if (unix_out != NULL)
    {
      *unix_out = utc;
    }

  /* 文件名 / stamp 用本地时，对齐表盘；unix= 仍是 UTC。 */
  if (g_crash_tm_ok && g_crash_tz_min != 0)
    {
      crash_unix_civil(utc + (time_t)g_crash_tz_min * 60, &g_crash_tm);
    }
}

static void crash_format_name(char *path, size_t path_sz, unsigned slot)
{
  if (g_crash_tm_ok)
    {
      snprintf(path, path_sz,
               COREDUMP_DIR_LFS "/n%03u_%04d%02d%02d_%02d%02d%02d.txt",
               slot,
               g_crash_tm.tm_year + 1900,
               g_crash_tm.tm_mon + 1,
               g_crash_tm.tm_mday,
               g_crash_tm.tm_hour,
               g_crash_tm.tm_min,
               g_crash_tm.tm_sec);
    }
  else
    {
      snprintf(path, path_sz, COREDUMP_DIR_LFS "/n%03u.txt", slot);
    }
}

static bool crash_is_note_name(const char *name)
{
  size_t len;

  if (name == NULL || name[0] != 'n')
    {
      return false;
    }

  len = strlen(name);
  return len >= 8 && memcmp(name + len - 4, ".txt", 4) == 0;
}

/** @brief 从 nNNN… 取槽号（0..999）。日历戳可能异常，裁剪只看编号。 */
static bool crash_note_seq(const char *name, unsigned *seq_out)
{
  unsigned slot = 0;

  if (name == NULL || name[0] != 'n' || seq_out == NULL)
    {
      return false;
    }

  if (sscanf(name, "n%u", &slot) != 1)
    {
      return false;
    }

  *seq_out = slot % 1000u;
  return true;
}

/** @brief 相对最新槽的年龄：0=最新，越大越旧（模 1000）。 */
static unsigned crash_seq_age(unsigned newest, unsigned seq)
{
  return (newest + 1000u - (seq % 1000u)) % 1000u;
}

static void crash_prune_old(unsigned keep, const char *keep_name)
{
  char path[64];
  char oldest[64];
  lfs_dir_t dir;
  struct lfs_info info;
  unsigned count;
  unsigned newest = 0;
  unsigned oldest_age;
  unsigned seq;
  unsigned age;

  if (keep_name != NULL && crash_note_seq(keep_name, &newest))
    {
      /* keep_name 即刚写入的最新槽。 */
    }
  else
    {
      newest = 0;
    }

  for (;;)
    {
      count = 0;
      oldest[0] = '\0';
      oldest_age = 0;

      if (lfs_dir_open(&g_crash_lfs, &dir, COREDUMP_DIR_LFS) != 0)
        {
          return;
        }

      while (lfs_dir_read(&g_crash_lfs, &dir, &info) > 0)
        {
          if (!crash_is_note_name(info.name))
            {
              continue;
            }

          if (!crash_note_seq(info.name, &seq))
            {
              continue;
            }

          if (keep_name != NULL && strcmp(info.name, keep_name) == 0)
            {
              count++;
              continue;
            }

          count++;
          age = crash_seq_age(newest, seq);
          if (oldest[0] == '\0' || age > oldest_age ||
              (age == oldest_age && strcmp(info.name, oldest) < 0))
            {
              oldest_age = age;
              snprintf(oldest, sizeof(oldest), "%s", info.name);
            }
        }

      (void)lfs_dir_close(&g_crash_lfs, &dir);
      if (count <= keep || oldest[0] == '\0')
        {
          return;
        }

      snprintf(path, sizeof(path), COREDUMP_DIR_LFS "/%s", oldest);
      (void)lfs_remove(&g_crash_lfs, path);
    }
}

static unsigned crash_read_seq(void)
{
  lfs_file_t file;
  struct lfs_file_config fcfg;
  char buf[16];
  lfs_ssize_t n;
  long v;
  int err;

  memset(&fcfg, 0, sizeof(fcfg));
  fcfg.buffer = g_lfs_file;
  err = lfs_file_opencfg(&g_crash_lfs, &file, COREDUMP_SEQ_PATH,
                         LFS_O_RDONLY, &fcfg);
  if (err != 0)
    {
      return 0;
    }

  n = lfs_file_read(&g_crash_lfs, &file, buf, sizeof(buf) - 1);
  (void)lfs_file_close(&g_crash_lfs, &file);
  if (n <= 0)
    {
      return 0;
    }

  buf[n] = '\0';
  v = strtol(buf, NULL, 10);
  return (v < 0) ? 0 : (unsigned)v;
}

static void crash_commit_note(void)
{
  char path[64];
  char seqbuf[16];
  const char *keep_name;
  unsigned seq;
  unsigned keep = (unsigned)CONFIG_MYVENDOR_COREDUMP_KEEP;
  int n;

  (void)lfs_mkdir(&g_crash_lfs, COREDUMP_DIR_LFS);
  seq = crash_read_seq();
  crash_format_name(path, sizeof(path), seq % 1000u);

  /* 把 diag 日志环里**尚未落盘**的内容一起写进这个转储文件 —— 这就是
   * "死机随 coredump 一起写入"。这里只做内存拷贝，磁盘那半交给下面
   * crash_lfs_put() 那条已经在用的裸 LittleFS 路径，不新增任何 I/O 方式。
   *
   * 用固定字符串 + memcpy，不用 snprintf：崩溃上下文里越少依赖越好。 */
  {
    extern size_t myvendor_diaglog_snapshot(char *out, size_t n);
    static const char k_diag_hdr[] = "\n--- diag log ring (unflushed) ---\n";
    size_t            hdr = sizeof(k_diag_hdr) - 1u;
    size_t            room;

    if (g_note != NULL && g_note_cap > g_note_len + hdr + 2u)
      {
        room = g_note_cap - g_note_len - hdr;
        memcpy(g_note + g_note_len, k_diag_hdr, hdr);
        g_note_len += hdr;
        g_note_len += myvendor_diaglog_snapshot(g_note + g_note_len, room);
      }
  }

  if (crash_lfs_put(path, g_note, (lfs_size_t)g_note_len) != 0)
    {
      return;
    }

  keep_name = strrchr(path, '/');
  keep_name = (keep_name != NULL) ? keep_name + 1 : path;
  crash_prune_old(keep, keep_name);

  n = snprintf(seqbuf, sizeof(seqbuf), "%u\n", seq + 1u);
  if (n > 0)
    {
      (void)crash_lfs_put(COREDUMP_SEQ_PATH, seqbuf, (lfs_size_t)n);
    }
}

static void crash_reset(void) __attribute__((noreturn));

static void crash_reset(void)
{
  /* IWDT 过 SYSRESETREQ 还在跑；先 STOP，避免第一次 boot 加载被咬。 */
  myvendor_watchdog_halt();
  up_systemreset();
}

static void crash_baremetal_main(void) __attribute__((noreturn));

static void crash_baremetal_main(void)
{
  const struct ptab_entry *kv;
  uint32_t kv_sz = KV_REGION_SIZE;

  myvendor_watchdog_keepalive_once();

  kv = ptab_find_tag("KV_REGION");
  g_kv_off = KV_REGION_OFFSET;
  if (kv != NULL && kv->size != 0)
    {
      g_kv_off = kv->offset;
      kv_sz = kv->size;
    }

  g_kv_blocks = kv_sz / COREDUMP_LFS_BLOCK;
  if (g_kv_blocks < 4u)
    {
      crash_reset();
    }

  if (sf32lb_sd_crash_reinit() < 0)
    {
      crash_reset();
    }

  memset(&g_crash_cfg, 0, sizeof(g_crash_cfg));
  memset(&g_crash_lfs, 0, sizeof(g_crash_lfs));
  g_crash_cfg.read = crash_kv_read;
  g_crash_cfg.prog = crash_kv_prog;
  g_crash_cfg.erase = crash_kv_erase;
  g_crash_cfg.sync = crash_kv_sync;
  g_crash_cfg.read_size = COREDUMP_LFS_READ;
  g_crash_cfg.prog_size = COREDUMP_LFS_READ;
  g_crash_cfg.block_size = COREDUMP_LFS_BLOCK;
  g_crash_cfg.cache_size = COREDUMP_LFS_READ;
  g_crash_cfg.lookahead_size = 8;
  g_crash_cfg.block_cycles = -1;
  g_crash_cfg.read_buffer = g_lfs_read;
  g_crash_cfg.prog_buffer = g_lfs_prog;
  g_crash_cfg.lookahead_buffer = g_lfs_look;
  g_crash_cfg.name_max = 128;
  g_crash_cfg.file_max = 2147483647;
  g_crash_cfg.attr_max = 1022;
  g_crash_cfg.block_count = g_kv_blocks;

  if (lfs_mount(&g_crash_lfs, &g_crash_cfg) != 0)
    {
      crash_reset();
    }

  crash_commit_note();
  (void)lfs_unmount(&g_crash_lfs);
  crash_reset();
}

static void coredump_enter_baremetal(size_t len)
{
  uint32_t *frame;
  uint32_t fn;
  uint32_t ipsr;
  uintptr_t top;

  g_note_len = len;
  top = g_crash_sp_top & ~7ul;
  if (top < 32u)
    {
      crash_use_fallback_bufs();
      top = g_crash_sp_top & ~7ul;
    }

  __asm__ __volatile__("mrs %0, ipsr" : "=r" (ipsr));
  __asm__ __volatile__("cpsid i" ::: "memory");

  fn = (uint32_t)crash_baremetal_main | 1u;

  if (ipsr == 0)
    {
      /* 线程态（IWDT 喂狗任务栈只有 2K）：切到崩溃栈再跑裸机。 */
      __asm__ __volatile__
        (
          "mov r0, #0\n"
          "msr control, r0\n"
          "isb\n"
          "msr msp, %0\n"
          "dsb\n"
          "isb\n"
          "bx %1\n"
          :
          : "r" (top), "r" (fn)
          : "r0", "memory"
        );
    }

  /* Handler mode → Thread + MSP，EXC_RETURN 0xFFFFFFF9（无 FPU 栈帧）。 */
  frame = (uint32_t *)top;
  frame -= 8;
  frame[0] = 0;
  frame[1] = 0;
  frame[2] = 0;
  frame[3] = 0;
  frame[4] = 0;
  frame[5] = 0xffffffffu;
  frame[6] = fn;
  frame[7] = 0x01000000u;

  __asm__ __volatile__
    (
      "mov r0, #0\n"
      "msr control, r0\n"
      "isb\n"
      "msr msp, %0\n"
      "dsb\n"
      "isb\n"
      "bx %1\n"
      :
      : "r" (frame), "r" (0xfffffff9u)
      : "r0", "memory"
    );

  crash_baremetal_main();
}

static void coredump_fill_common(size_t *off, const char *kind)
{
  time_t unix_sec = 0;

  crash_capture_time(&unix_sec);

  note_append(off, "MAGIC=helm-regs-1\n");
  note_append(off, "kind=%s\n", kind != NULL ? kind : "unknown");
  note_append(off, "unix=%ld\n", (long)unix_sec);
  note_append(off, "tz_min=%d\n", (int)g_crash_tz_min);
  if (g_crash_tm_ok)
    {
      note_append(off, "stamp=%04d-%02d-%02dT%02d:%02d:%02d\n",
                  g_crash_tm.tm_year + 1900,
                  g_crash_tm.tm_mon + 1,
                  g_crash_tm.tm_mday,
                  g_crash_tm.tm_hour,
                  g_crash_tm.tm_min,
                  g_crash_tm.tm_sec);
    }

  note_append(off, "xcpt_regs=%d\n", (int)XCPTCONTEXT_REGS);
}

static void coredump_puts_line(const char *s)
{
#ifdef CONFIG_ARCH_LOWPUTC
  up_puts(s);
#else
  syslog(LOG_ERR, "%s", s);
#endif
}

static void coredump_write_assert(uintptr_t sp, FAR struct tcb_s *tcb,
                                  FAR const char *filename, int lineno,
                                  FAR const char *msg, FAR void *regs)
{
  size_t off = 0;
  FAR uint32_t *assert_regs = (FAR uint32_t *)regs;
  FAR uint32_t *fault_regs = NULL;

  if (g_busy)
    {
      return;
    }

  g_busy = true;
  myvendor_watchdog_keepalive_once();
  crash_claim_fb();
  memset(g_note, 0, g_note_cap);
  coredump_fill_common(&off, "assert");
  note_append(&off, "wdt_pet=1\n");

  note_append(&off, "file=%s\n", filename != NULL ? filename : "?");
  note_append(&off, "line=%d\n", lineno);
  note_append(&off, "msg=%s\n", msg != NULL ? msg : "");
  note_append(&off, "irq=%d\n", up_interrupt_context() ? 1 : 0);
  note_append(&off, "sp=0x%lx\n", (unsigned long)sp);

  if (tcb != NULL)
    {
      note_append(&off, "pid=%d name=%s pri=%d st=%u lock=%d wait=%p\n",
                  (int)tcb->pid, get_task_name(tcb),
                  (int)tcb->sched_priority,
                  (unsigned)tcb->task_state, (int)tcb->lockcount,
                  tcb->waitobj);
    }

  snprintf(g_fmt, sizeof(g_fmt),
           "\n[coredump] assert %s:%d irq=%d writing kv\n",
           filename != NULL ? filename : "?", lineno,
           up_interrupt_context() ? 1 : 0);
  coredump_puts_line(g_fmt);

  if (up_interrupt_context())
    {
      fault_regs = (FAR uint32_t *)running_regs();
    }
  else if (tcb != NULL)
    {
      fault_regs = tcb->xcp.regs;
    }

  if (fault_regs == NULL)
    {
      fault_regs = assert_regs;
    }

  note_hw_cpu(&off);
  note_scb_fault(&off);

  note_append(&off, "fault_regs\n");
  note_arm_regs(&off, fault_regs);

  if (assert_regs != NULL && assert_regs != fault_regs)
    {
      note_append(&off, "assert_regs\n");
      note_arm_regs(&off, assert_regs);
    }

  note_board_snap(&off);

  if (!up_interrupt_context())
    {
      nxsched_foreach(note_one_task, &off);
    }
  else
    {
      note_append(&off, "tasks=schedmon (irq, skip foreach)\n");
    }

  coredump_enter_baremetal(off);
}

static int coredump_on_panic(FAR struct notifier_block *nb,
                             unsigned long action, FAR void *data)
{
  FAR struct panic_notifier_s *p = data;

  (void)nb;
  if (action != PANIC_KERNEL || p == NULL)
    {
      return 0;
    }

  coredump_write_assert(up_getsp(), p->rtcb, p->filename, p->linenum,
                        p->msg, p->regs);
  return 0;
}

void myvendor_coredump_early(void)
{
  static struct notifier_block nb =
    {
      .notifier_call = coredump_on_panic,
      .priority = 1000,
    };

  if (g_nb_on)
    {
      return;
    }

  g_nb_on = true;
  panic_notifier_chain_register(&nb);
}

/**
 * @brief 开机时按 VFS 裁剪 `/mnt/kv/coredump`：只留最新 KEEP 个 n*.txt。
 *
 * 崩溃路径里的 `crash_prune_old` 只在裸机写盘时跑；若多次 soft dump /
 * 旧 KEEP=20 遗留，目录会在正常运行中无限堆。开机 KV 挂上后用普通
 * opendir/unlink 清一次，与 diaglog 的 KEEP=10 对齐。
 *
 * 谁旧只看文件名里的 nNNN（模 1000），**不看**日历戳——异常时间会把新旧弄反。
 */
static unsigned coredump_vfs_read_seq(void)
{
  char path[96];
  char buf[16];
  int fd;
  ssize_t n;
  long v;

  snprintf(path, sizeof(path), "%s/seq", MYVENDOR_COREDUMP_DIR);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return 0;
    }

  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    {
      return 0;
    }

  buf[n] = '\0';
  v = strtol(buf, NULL, 10);
  return (v < 0) ? 0 : (unsigned)v;
}

/**
 * @brief 目录里**除 `seq` 与 coredump note 外，一律当垃圾清掉**
 *        （用户 2026-09-25 定调："除了 seq 和日志，其他的都当垃圾清理"）。
 *
 * @details 判"是 note"要**同时**过两关：`crash_is_note_name()`（n 开头 + ..txt）
 *          并且能解出槽号 —— 只有前者的话 `nonsense.txt` 也会被当 note 留着。
 *          与 diag 侧同一口径：认不出的就是垃圾。
 *          注意：`.` / `..` 必须显式跳过。
 *          崩溃路径写盘时本函数不会并发（写盘在裸机、独占 CPU 且重置了 SDIO/LFS），
 *          所以不存在"删到正在写的 note"的竞态。
 */
static void coredump_scrub_unknown(void)
{
  DIR           *dir;
  struct dirent *de;

  dir = opendir(MYVENDOR_COREDUMP_DIR);
  if (dir == NULL)
    {
      return;
    }

  while ((de = readdir(dir)) != NULL)
    {
      const char *n = de->d_name;
      char        path[96];

      if (n[0] == '\0' || strcmp(n, ".") == 0 || strcmp(n, "..") == 0 ||
          strcmp(n, "seq") == 0)         /* 序号文件保留 */
        {
          continue;
        }

      /* note 保留 —— 但必须**同时**能解出槽号才算数（`crash_is_note_name()` 只要求
       * "n 开头 + ..txt"，像 `nonsense.txt` 这种也能过）。与 diag 侧同一口径：
       * "像日志但不是" 的一律当垃圾。 */
      if (crash_is_note_name(n))
        {
          unsigned seq;

          if (crash_note_seq(n, &seq))
            {
              continue;
            }
        }

      snprintf(path, sizeof(path), "%s/%s", MYVENDOR_COREDUMP_DIR, n);
      if (unlink(path) != 0)
        {
          syslog(LOG_WARNING, "[coredump] scrub unlink %s errno=%d\n",
                 path, errno);
        }
      else
        {
          syslog(LOG_INFO, "[coredump] scrub %s (not seq/note)\n", n);
        }
    }

  closedir(dir);
}

/**
 * @brief 清垃圾 + 按槽号顺序删到只剩 `keep` 个 note。
 *
 * @details "最新槽"从 `/mnt/kv/seq` 推（seq 记的是**下一次**编号，故最新 = seq-1）；
 *          seq 缺失时退化成"目录里出现的最大槽号"，没有环绕信息就只能这么兜底。
 *          先清垃圾再数，两者互不阻塞。
 *
 * @param keep 要保留的 note 个数（= `CONFIG_MYVENDOR_COREDUMP_KEEP`）。
 * @return true = 跑完了（目录已 ≤ keep）；false = 中途失败/卡住，调用方按节流再试。
 */
static bool coredump_vfs_prune(unsigned keep)
{
  unsigned seq_file;
  unsigned newest;

  coredump_scrub_unknown();

  seq_file = coredump_vfs_read_seq();

  /* seq 文件是「下一次」编号；最新已写槽 = (seq-1) mod 1000。 */
  newest = (seq_file > 0u) ? ((seq_file - 1u) % 1000u) : 0u;

  /* seq 缺失时：用目录里出现的最大槽号当 newest（无环绕信息时的兜底）。 */
  if (seq_file == 0u)
    {
      DIR *dir;
      struct dirent *de;
      unsigned seq;
      bool have = false;

      dir = opendir(MYVENDOR_COREDUMP_DIR);
      if (dir != NULL)
        {
          while ((de = readdir(dir)) != NULL)
            {
              if (!crash_is_note_name(de->d_name) ||
                  !crash_note_seq(de->d_name, &seq))
                {
                  continue;
                }

              if (!have || seq > newest)
                {
                  newest = seq;
                  have = true;
                }
            }

          closedir(dir);
        }
    }

  for (; ; )
    {
      DIR *dir;
      struct dirent *de;
      char oldest[64];
      unsigned oldest_age = 0;
      unsigned count = 0;
      unsigned seq;
      unsigned age;

      oldest[0] = '\0';
      dir = opendir(MYVENDOR_COREDUMP_DIR);
      if (dir == NULL)
        {
          syslog(LOG_WARNING, "[coredump] prune opendir %s errno=%d\n",
                 MYVENDOR_COREDUMP_DIR, errno);
          return false;
        }

      while ((de = readdir(dir)) != NULL)
        {
          if (!crash_is_note_name(de->d_name))
            {
              continue;
            }

          if (!crash_note_seq(de->d_name, &seq))
            {
              continue;
            }

          count++;
          age = crash_seq_age(newest, seq);
          if (oldest[0] == '\0' || age > oldest_age ||
              (age == oldest_age && strcmp(de->d_name, oldest) < 0))
            {
              oldest_age = age;
              snprintf(oldest, sizeof(oldest), "%s", de->d_name);
            }
        }

      closedir(dir);

      if (count <= keep || oldest[0] == '\0')
        {
          if (count > keep)
            {
              syslog(LOG_WARNING,
                     "[coredump] prune stuck count=%u keep=%u\n",
                     count, keep);
              return false;
            }

          if (count > 0)
            {
              syslog(LOG_INFO, "[coredump] prune ok count=%u keep=%u\n",
                     count, keep);
            }

          return true;
        }

      {
        char path[96];

        snprintf(path, sizeof(path), "%s/%s", MYVENDOR_COREDUMP_DIR, oldest);
        if (unlink(path) != 0)
          {
            syslog(LOG_WARNING, "[coredump] unlink %s errno=%d\n",
                   path, errno);
            return false;
          }

        syslog(LOG_INFO, "[coredump] prune %s by seq (%u > %u)\n",
               oldest, count, keep);
      }
    }
}

/** 本次运行**成功**清过一次就不再清（用户 2026-09-25 定调："一次运行只需要清理成功
 *  一次就不清理了"）；失败才按下面的节流重试 —— 失败每次都打 WARNING，节流是为了
 *  不把日志刷爆。开机那次若已成功，这里直接就是空转。 */
static bool s_prune_done;

/** 失败后的重试间隔（秒）。成功路径不进这个节流。 */
#define MYVENDOR_COREDUMP_PRUNE_RETRY_S 600u

/**
 * @brief 运行期周期性清理入口（线程上下文；内部限频，默认 10 分钟）。
 *
 * @details 崩溃路径的裁剪只在"写新 dump"时跑，开机那次也只在开机跑 —— 长时间不重启、
 *          期间只有 soft dump 时目录会只涨不落（2026-09-25 用户反馈）。本入口由 diag
 *          线程每拍调用，自身限频；成功**一次**即整场空转，失败才按
 *          `MYVENDOR_COREDUMP_PRUNE_RETRY_S` 重试（失败由 `prune stuck` 出声）。
 */
void myvendor_coredump_prune(void)
{
  static clock_t s_retry;

  if (s_prune_done)
    {
      return;
    }

  if (s_retry != 0 &&
      (clock_t)(clock_systime_ticks() - s_retry) <
        (clock_t)MYVENDOR_COREDUMP_PRUNE_RETRY_S * (clock_t)TICK_PER_SEC)
    {
      return;
    }

  if (coredump_vfs_prune((unsigned)CONFIG_MYVENDOR_COREDUMP_KEEP))
    {
      s_prune_done = true;
      return;
    }

  s_retry = clock_systime_ticks();
}

void myvendor_coredump_init(void)
{
  int ret;

  myvendor_coredump_early();
  ret = mkdir(MYVENDOR_COREDUMP_DIR, 0755);

  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_WARNING, "[coredump] mkdir %s errno=%d\n",
             MYVENDOR_COREDUMP_DIR, errno);
      return;
    }

  /* 开机这次若成功，运行期入口（diag 拍）就空转 —— 一次运行清一次就够。 */
  if (coredump_vfs_prune((unsigned)CONFIG_MYVENDOR_COREDUMP_KEEP))
    {
      s_prune_done = true;
    }

  syslog(LOG_INFO, "[coredump] dir %s keep=%u\n",
         MYVENDOR_COREDUMP_DIR, (unsigned)CONFIG_MYVENDOR_COREDUMP_KEEP);
}

void myvendor_coredump_wdt(void)
{
  struct myvendor_watchdog_status_s st;
  size_t off = 0;

  if (g_busy)
    {
      return;
    }

  g_busy = true;
  myvendor_watchdog_keepalive_once();
  crash_claim_fb();
  memset(g_note, 0, g_note_cap);
  coredump_fill_common(&off, "wdt");
  note_append(&off, "wdt_pet=1\n");

  memset(&st, 0, sizeof(st));
  if (myvendor_watchdog_get_status(&st) == 0)
    {
      note_append(&off,
                  "wdt started=%d feeding=%d suppress=%d "
                  "idle_ok=%d idle_age_ms=%u work_ok=%d work_age_ms=%u "
                  "ui_ok=%d ui_seen=%d "
                  "ui_req=%d ui_age_ms=%u timeout_ms=%u\n",
                  st.started ? 1 : 0,
                  st.feeding ? 1 : 0,
                  st.suppressed ? 1 : 0,
                  st.idle_ok ? 1 : 0,
                  (unsigned)st.idle_age_ms,
                  st.work_ok ? 1 : 0,
                  (unsigned)st.work_age_ms,
                  st.ui_ok ? 1 : 0,
                  st.ui_seen ? 1 : 0,
                  st.ui_required ? 1 : 0,
                  (unsigned)st.ui_age_ms,
                  (unsigned)st.timeout_ms);
    }

  note_hw_cpu(&off);
  note_scb_fault(&off);
  note_board_snap(&off);
  nxsched_foreach(note_one_task, &off);

  coredump_enter_baremetal(off);
}

#ifdef CONFIG_BOARD_CRASHDUMP_CUSTOM
void board_crashdump(uintptr_t sp, FAR struct tcb_s *tcb,
                     FAR const char *filename, int lineno,
                     FAR const char *msg, FAR void *regs)
{
  coredump_write_assert(sp, tcb, filename, lineno, msg, regs);
}
#endif /* CONFIG_BOARD_CRASHDUMP_CUSTOM */

#endif /* CONFIG_MYVENDOR_COREDUMP */
