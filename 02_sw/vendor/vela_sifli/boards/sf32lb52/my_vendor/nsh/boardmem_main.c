/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/src/boardmem_main.c
 *
 * NSH 内置命令 boardmem：打印板级内存堆用量与 PSRAM 物理布局。
 *
 * 输出分两大块：
 *
 *   1. Board memory heaps — 运行时堆统计（mallinfo）
 *      - Umem (total)     ：NuttX 用户堆合计（SRAM + PSRAM kumm），与 NSH "free" 一致
 *      - Umem SRAM        ：region 0 子项（片内）
 *      - Umem PSRAM kumm  ：region 1 子项（malloc 可用的 PSRAM 段）
 *      - BoardPSRAM       ：board_malloc_psram 独立堆，不含在 "free" 内
 *      - VPK heap         ：地图 cell .vpk 专用块（从 BoardPSRAM 扣下）
 *
 *   2. PSRAM_DATA map — 静态分区地图（与 board_psram_layout.h 一致）
 *      - kumm / BoardPSRAM pool / mtp bump arena 的起始地址与占用
 *
 *   3. Heap tail probe（仅 `boardmem check`）— 对各堆末 50 字节
 *      做写后读校验（Umem SRAM/PSRAM、BoardPSRAM、mtp bump）；原内容保存并恢复。
 *
 * 表格列说明：
 *   Used/Total  已用 KiB / 总 KiB
 *   Pct         使用百分比
 *   [Bar]       24 格水位条（见下方图例）
 *   Info1       堆区：Free KiB；地图区：起始地址或 "rsv"/"bump"
 *   Info2       堆区：MaxUsed KiB（峰值）；子 region / 地图行多为 "-"
 *
 * 水位条图例（按 KiB 粒度映射到 BOARDMEM_BAR_W=24 格）：
 *   x  当前已占用
 *   =  历史峰值占用、现已释放（仅 Umem total / BoardPSRAM 有峰值）
 *   -  从未使用
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>

#include <nuttx/cache.h>

#include "board_malloc.h"
#include "board_psram_layout.h"
#include "vmap_vpk_heap_stats.h"

#if defined(CONFIG_MYVENDOR_MTP_SIMPLE)
extern size_t mtp_psram_used_bytes(void);
#endif

/* 表格列宽：Name 左对齐，其余列定宽便于 tab 对齐 */

#define BOARDMEM_NAME_W   18
#define BOARDMEM_BAR_W    24
#define BOARDMEM_COL2_W   12
#define BOARDMEM_COL3_W   4
#define BOARDMEM_COL5_W   10
#define BOARDMEM_COL6_W   10

/**
 * 生成 ASCII 水位条。
 *
 * 将 [0, total_b] 均分为 BOARDMEM_BAR_W 段，每段约 chunk_kb KiB：
 *   end_kb <= used_kb  → 'x'（当前占用）
 *   end_kb <= max_kb   → '='（峰值水位，现可能已 free）
 *   否则               → '-'（空闲）
 */
static void boardmem_format_water_bar(char *out, size_t outlen,
                                      uint32_t used_b, uint32_t max_b,
                                      uint32_t total_b)
{
  uint32_t used_kb;
  uint32_t max_kb;
  uint32_t total_kb;
  uint32_t chunk_kb;
  unsigned i;

  if (outlen < BOARDMEM_BAR_W + 1)
    {
      return;
    }

  if (total_b == 0)
    {
      memset(out, '-', BOARDMEM_BAR_W);
      out[BOARDMEM_BAR_W] = '\0';
      return;
    }

  used_kb  = used_b / 1024u;
  max_kb   = max_b / 1024u;
  total_kb = total_b / 1024u;
  if (total_kb == 0)
    {
      total_kb = 1;
    }

  if (max_kb < used_kb)
    {
      max_kb = used_kb;
    }

  if (max_kb > total_kb)
    {
      max_kb = total_kb;
    }

  if (used_kb > total_kb)
    {
      used_kb = total_kb;
    }

  chunk_kb = (total_kb + BOARDMEM_BAR_W - 1u) / BOARDMEM_BAR_W;
  if (chunk_kb == 0)
    {
      chunk_kb = 1;
    }

  for (i = 0; i < BOARDMEM_BAR_W; i++)
    {
      uint32_t end_kb = (i + 1u) * chunk_kb;

      if (end_kb > total_kb)
        {
          end_kb = total_kb;
        }

      if (end_kb <= used_kb)
        {
          out[i] = 'x';
        }
      else if (end_kb <= max_kb)
        {
          out[i] = '=';
        }
      else
        {
          out[i] = '-';
        }
    }

  out[BOARDMEM_BAR_W] = '\0';
}

static unsigned boardmem_percent(uint32_t used, uint32_t total)
{
  if (total == 0)
    {
      return 0;
    }

  return (unsigned)((uint64_t)used * 100 / total);
}

/** 左对齐填充字符串到固定列宽。 */
static void boardmem_pad_field(char *buf, size_t len, const char *src)
{
  snprintf(buf, len, "%-*s", (int)(len - 1), src);
}

static void boardmem_format_kb_pair(char *buf, size_t len,
                                    uint32_t a_kb, uint32_t b_kb)
{
  char tmp[BOARDMEM_COL2_W + 1];

  snprintf(tmp, sizeof(tmp), "%4" PRIu32 "K/%4" PRIu32 "K", a_kb, b_kb);
  boardmem_pad_field(buf, len, tmp);
}

static void boardmem_format_kb(char *buf, size_t len, uint32_t kb)
{
  char tmp[BOARDMEM_COL5_W + 1];

  snprintf(tmp, sizeof(tmp), "%4" PRIu32 "K", kb);
  boardmem_pad_field(buf, len, tmp);
}

static void boardmem_print_legend(void)
{
  printf("  Bar: x=used  |=peak  -=free  (~1 KiB step, %u slots)\n\n",
         BOARDMEM_BAR_W);
}

static void boardmem_print_header(void)
{
  char c2[BOARDMEM_COL2_W + 1];
  char c3[BOARDMEM_COL3_W + 1];
  char c5[BOARDMEM_COL5_W + 1];
  char c6[BOARDMEM_COL6_W + 1];
  char hdrbar[BOARDMEM_BAR_W + 1];

  boardmem_pad_field(c2, sizeof(c2), "Used/Total");
  boardmem_pad_field(c3, sizeof(c3), "Pct");
  boardmem_pad_field(c5, sizeof(c5), "Info1");
  boardmem_pad_field(c6, sizeof(c6), "Info2");
  memset(hdrbar, '-', BOARDMEM_BAR_W);
  hdrbar[BOARDMEM_BAR_W] = '\0';

  printf("  %-*s\t%s\t%s\t[%s]\t%s\t%s\n",
         BOARDMEM_NAME_W, "Name",
         c2, c3, hdrbar, c5, c6);
}

static void boardmem_print_row(const char *name, const char *col2,
                               const char *col3, const char *bar,
                               const char *col5, const char *col6)
{
  char c2[BOARDMEM_COL2_W + 1];
  char c3[BOARDMEM_COL3_W + 1];
  char c5[BOARDMEM_COL5_W + 1];
  char c6[BOARDMEM_COL6_W + 1];

  boardmem_pad_field(c2, sizeof(c2), col2);
  boardmem_pad_field(c3, sizeof(c3), col3);
  boardmem_pad_field(c5, sizeof(c5), col5);
  boardmem_pad_field(c6, sizeof(c6), col6);

  printf("  %-*s\t%s\t%s\t[%s]\t%s\t%s\n",
         BOARDMEM_NAME_W, name,
         c2, c3, bar, c5, col6);
}

/**
 * 打印带 mallinfo 的堆行（含峰值 '=' 水位）。
 * Info1=fordblks 空闲 KiB，Info2=usmblks 峰值 KiB。
 */
static void boardmem_print_usage_row(const char *name, struct mallinfo *info)
{
  char bar[BOARDMEM_BAR_W + 1];
  char col2[BOARDMEM_COL2_W + 1];
  char col3[BOARDMEM_COL3_W + 1];
  char col5[BOARDMEM_COL5_W + 1];
  char col6[BOARDMEM_COL6_W + 1];
  uint32_t total;
  uint32_t used;
  uint32_t maxused;
  uint32_t freeb;

  total    = (uint32_t)info->arena;
  used     = (uint32_t)info->uordblks;
  maxused  = (uint32_t)info->usmblks;
  freeb    = (uint32_t)info->fordblks;
  boardmem_format_water_bar(bar, sizeof(bar), used, maxused, total);
  boardmem_format_kb_pair(col2, sizeof(col2), used / 1024u, total / 1024u);
  snprintf(col3, sizeof(col3), "%3u%%", boardmem_percent(used, total));
  boardmem_format_kb(col5, sizeof(col5), freeb / 1024u);
  boardmem_format_kb(col6, sizeof(col6), maxused / 1024u);

  boardmem_print_row(name, col2, col3, bar, col5, col6);
}

/** 通用水位行：col3/col5/col6 由调用方填入（用于 PSRAM 地图段）。 */
static void boardmem_print_water_row(const char *name,
                                     uint32_t used_b, uint32_t max_b,
                                     uint32_t total_b,
                                     const char *col3,
                                     const char *col5, const char *col6)
{
  char bar[BOARDMEM_BAR_W + 1];
  char col2[BOARDMEM_COL2_W + 1];

  boardmem_format_water_bar(bar, sizeof(bar), used_b, max_b, total_b);
  boardmem_format_kb_pair(col2, sizeof(col2),
                          used_b / 1024u, total_b / 1024u);
  boardmem_print_row(name, col2, col3, bar, col5, col6);
}

/**
 * Umem 子 region 行：无 NuttX per-region 峰值，水位条 max=used（无 '='）。
 * Info2 显示 "-"。
 */
static void boardmem_print_region_row(const char *name, struct mallinfo *info)
{
  char bar[BOARDMEM_BAR_W + 1];
  char col2[BOARDMEM_COL2_W + 1];
  char col3[BOARDMEM_COL3_W + 1];
  char col5[BOARDMEM_COL5_W + 1];
  char col6[BOARDMEM_COL6_W + 1];
  uint32_t total;
  uint32_t used;
  uint32_t freeb;

  total = (uint32_t)info->arena;
  used  = (uint32_t)info->uordblks;
  freeb = (uint32_t)info->fordblks;
  boardmem_format_water_bar(bar, sizeof(bar), used, used, total);
  boardmem_format_kb_pair(col2, sizeof(col2), used / 1024u, total / 1024u);
  snprintf(col3, sizeof(col3), "%3u%%", boardmem_percent(used, total));
  boardmem_format_kb(col5, sizeof(col5), freeb / 1024u);
  boardmem_pad_field(col6, sizeof(col6), "-");
  boardmem_print_row(name, col2, col3, bar, col5, col6);
}

/** 第一节：运行时堆统计。 */
static void boardmem_print_heaps(void)
{
  struct mallinfo umem;
  struct mallinfo psram;
  struct mallinfo sub;
  char bar[BOARDMEM_BAR_W + 1];

  umem = mallinfo();

  printf("Board memory heaps\n");
  printf("  (NSH \"free\" = Umem only; BoardPSRAM is separate.)\n");
  printf("  (Info1=Free, Info2=MaxUsed KiB; VPK Info2=resident cells)\n");
  boardmem_print_legend();
  boardmem_print_header();
  boardmem_print_usage_row("Umem (total)", &umem);

#if CONFIG_MM_REGIONS > 1
  if (board_umem_region_mallinfo(0, &sub))
    {
      boardmem_print_region_row("Umem SRAM", &sub);
    }

  if (board_umem_region_mallinfo(1, &sub))
    {
      boardmem_print_region_row("Umem PSRAM kumm", &sub);
    }
#endif

  board_psram_heap_init();
  psram = board_psram_mallinfo();
  if (board_psram_heap_ready())
    {
      size_t vpk_arena = 0;
      size_t vpk_used = 0;
      uint32_t vpk_cells = 0;

      boardmem_print_usage_row("BoardPSRAM", &psram);
      if (vmap_vpk_heap_stats(&vpk_arena, &vpk_used, &vpk_cells)
          && vpk_arena > 0)
        {
          char col3[BOARDMEM_COL3_W + 1];
          char col5[BOARDMEM_COL5_W + 1];
          char col6[BOARDMEM_COL6_W + 1];
          uint32_t total = (uint32_t)vpk_arena;
          uint32_t used = (uint32_t)vpk_used;
          uint32_t freeb = (total > used) ? (total - used) : 0;
          char nbuf[BOARDMEM_COL6_W + 1];

          snprintf(col3, sizeof(col3), "%3u%%",
                   boardmem_percent(used, total));
          boardmem_format_kb(col5, sizeof(col5), freeb / 1024u);
          snprintf(nbuf, sizeof(nbuf), "%4" PRIu32 "c", vpk_cells);
          boardmem_pad_field(col6, sizeof(col6), nbuf);
          boardmem_print_water_row("VPK heap", used, used, total,
                                   col3, col5, col6);
        }
    }
  else
    {
      boardmem_format_water_bar(bar, sizeof(bar), 0, 0, 0);
      boardmem_print_row("BoardPSRAM", "-/-", "off", bar, "-", "-");
    }
}

/**
 * 第二节：PSRAM_DATA 静态地图（低→高）。
 * kumm 行仅显示预留大小与基址；BoardPSRAM 行可带实时用量；MTP 行显示 bump 已用。
 */
static void boardmem_print_layout(void)
{
  char col5[BOARDMEM_COL5_W + 1];
  uint32_t kumm_kb = (uint32_t)(board_psram_kumm_bytes() / 1024u);

  printf("\nPSRAM_DATA map (0x%08x, %u MiB)\n",
         (unsigned)BOARD_PSRAM_DATA_BASE,
         (unsigned)(BOARD_PSRAM_DATA_SIZE / (1024u * 1024u)));
  printf("  (Info1=Address, Info2=-)\n");
  boardmem_print_legend();
  boardmem_print_header();

  snprintf(col5, sizeof(col5), "0x%08" PRIx32,
           (uint32_t)BOARD_PSRAM_DATA_BASE);
  boardmem_print_water_row("kumm (Umem r1)", 0, 0,
                           kumm_kb * 1024u, "rsv", col5, "-");

#if BOARD_PSRAM_POOL_KB > 0
  snprintf(col5, sizeof(col5), "0x%08" PRIx32,
           (uint32_t)board_psram_pool_base());
  if (board_psram_heap_ready())
    {
      struct mallinfo psram = board_psram_mallinfo();
      char col3[BOARDMEM_COL3_W + 1];
      uint32_t total;
      uint32_t used;
      uint32_t maxused;

      total   = (uint32_t)psram.arena;
      used    = (uint32_t)psram.uordblks;
      maxused = (uint32_t)psram.usmblks;
      snprintf(col3, sizeof(col3), "%3u%%", boardmem_percent(used, total));
      boardmem_print_water_row("BoardPSRAM pool", used, maxused, total,
                               col3, col5, "-");
    }
  else
    {
      boardmem_print_water_row("BoardPSRAM pool", 0, 0,
                               BOARD_PSRAM_POOL_BYTES, "rsv", col5, "-");
    }
#else
  {
    char bar[BOARDMEM_BAR_W + 1];

    boardmem_format_water_bar(bar, sizeof(bar), 0, 0, 0);
    boardmem_print_row("BoardPSRAM pool", "off", "off", bar, "-", "-");
  }
#endif

#if BOARD_MTP_PSRAM_RESERVE_KB > 0
  {
    uint32_t total_b = (uint32_t)BOARD_MTP_PSRAM_RESERVE_BYTES;
    uint32_t used_b = 0;

#if defined(CONFIG_MYVENDOR_MTP_SIMPLE)
    used_b = (uint32_t)mtp_psram_used_bytes();
#endif

    snprintf(col5, sizeof(col5), "0x%08" PRIx32,
             (uint32_t)board_mtp_psram_arena_base());
    boardmem_print_water_row("mtp bump arena", used_b, used_b, total_b,
                             "bump", col5, "-");
  }
#endif
}

#define BOARDMEM_TAIL_PROBE_BYTES 50u

/**
 * 对 [base, base+probe_len) 做写后读校验（调用方提供名称与起始地址）。
 *
 * @return 0 通过，-1 失败
 */
static int boardmem_probe_range(const char *name, uintptr_t base)
{
  static const uint8_t patterns[] =
  {
    0xaau,
    0x55u,
    0x00u,
    0xffu,
    0xa5u,
  };
  static uint8_t saved[BOARDMEM_TAIL_PROBE_BYTES];
  const uintptr_t end = base + BOARDMEM_TAIL_PROBE_BYTES;
  volatile uint8_t *bytes = (volatile uint8_t *)base;
  unsigned p;
  unsigned i;

  for (i = 0; i < BOARDMEM_TAIL_PROBE_BYTES; i++)
    {
      saved[i] = bytes[i];
    }

  for (p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++)
    {
      const uint8_t pat = patterns[p];

      for (i = 0; i < BOARDMEM_TAIL_PROBE_BYTES; i++)
        {
          bytes[i] = (uint8_t)(pat ^ (uint8_t)i);
        }

#ifdef CONFIG_ARCH_DCACHE
      up_clean_dcache(base, end);
      up_invalidate_dcache(base, end);
#endif

      for (i = 0; i < BOARDMEM_TAIL_PROBE_BYTES; i++)
        {
          const uint8_t expect = (uint8_t)(pat ^ (uint8_t)i);
          const uint8_t got = bytes[i];

          if (got != expect)
            {
              printf("  FAIL %-18s @ 0x%08" PRIx32 " pat[%u] byte[%u]"
                     ": expect 0x%02" PRIx32 " got 0x%02" PRIx32 "\n",
                     name, (uint32_t)(base + i), p, i,
                     (uint32_t)expect, (uint32_t)got);
              goto restore;
            }
        }
    }

restore:
  for (i = 0; i < BOARDMEM_TAIL_PROBE_BYTES; i++)
    {
      bytes[i] = saved[i];
    }

#ifdef CONFIG_ARCH_DCACHE
  up_clean_dcache(base, end);
#endif

  if (p < sizeof(patterns) / sizeof(patterns[0]))
    {
      return -1;
    }

  printf("  PASS %-18s @ 0x%08" PRIx32 "\n", name, (uint32_t)base);
  return 0;
}

/** 对各已知堆/区域末 50 字节依次探测。 */
static int boardmem_run_heap_checks(void)
{
  uintptr_t addr;
  int rc = 0;
  int n = 0;

  printf("\nHeap tail probe (last %u bytes each)\n",
         (unsigned)BOARDMEM_TAIL_PROBE_BYTES);

  if (board_umem_region_tail_addr(0, BOARDMEM_TAIL_PROBE_BYTES, &addr))
    {
      rc |= boardmem_probe_range("Umem SRAM", addr);
      n++;
    }
  else
    {
      printf("  SKIP Umem SRAM\n");
    }

#if CONFIG_MM_REGIONS > 1
  if (board_umem_region_tail_addr(1, BOARDMEM_TAIL_PROBE_BYTES, &addr))
    {
      rc |= boardmem_probe_range("Umem PSRAM kumm", addr);
      n++;
    }
  else
    {
      printf("  SKIP Umem PSRAM kumm\n");
    }
#endif

#if BOARD_PSRAM_POOL_KB > 0
  if (board_psram_pool_tail_addr(BOARDMEM_TAIL_PROBE_BYTES, &addr))
    {
      rc |= boardmem_probe_range("BoardPSRAM pool", addr);
      n++;
    }
  else
    {
      printf("  SKIP BoardPSRAM pool\n");
    }
#endif

#if BOARD_MTP_PSRAM_RESERVE_KB > 0
  if (BOARD_MTP_PSRAM_RESERVE_BYTES >= BOARDMEM_TAIL_PROBE_BYTES)
    {
      addr = board_mtp_psram_arena_base() + BOARD_MTP_PSRAM_RESERVE_BYTES -
             BOARDMEM_TAIL_PROBE_BYTES;
      rc |= boardmem_probe_range("mtp bump arena", addr);
      n++;
    }
  else
    {
      printf("  SKIP mtp bump arena\n");
    }
#endif

  if (n == 0)
    {
      printf("  (no regions to probe)\n");
      return -1;
    }

  if (rc == 0)
    {
      printf("  ALL PASS (%d region%s)\n", n, n == 1 ? "" : "s");
    }
  else
    {
      printf("  CHECK FAILED\n");
    }

  return rc;
}

int main(int argc, FAR char *argv[])
{
  int rc = EXIT_SUCCESS;
  bool do_check = false;

  if (argc > 1)
    {
      if (strcmp(argv[1], "check") == 0)
        {
          do_check = true;
        }
      else
        {
          printf("Usage: boardmem [check]\n");
          printf("  boardmem        heap usage + PSRAM map\n");
          printf("  boardmem check  + heap tail 50B probe (all regions)\n");
          return EXIT_FAILURE;
        }
    }

  boardmem_print_heaps();
  boardmem_print_layout();

  if (do_check && boardmem_run_heap_checks() != 0)
    {
      rc = EXIT_FAILURE;
    }

  printf("\nTip: boardmem check — probe each heap tail; cat /proc/meminfo\n");
  return rc;
}
