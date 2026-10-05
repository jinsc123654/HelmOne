/**
 * @file board_psram_layout.h
 * @brief PSRAM_DATA 分区布局（低地址 → 高地址）的单一来源。
 *
 * board_malloc、mtp_psram、boardmem 经此处宏/内联计算各区基址与大小。
 *
 * 扣减顺序与字节数必须与 chips/sf32lb52/sifli_allocateheap.c 中
 * arm_addregion() 完全一致，否则 kumm 与 BoardPSRAM / MTP 会重叠。
 * 物理分区须与 ptab（PSRAM_DATA @ 0x60400000, 12 MiB）一致。
 *
 * 默认布局（12 MiB，Kconfig 可改 pool / MTP 预留，kumm 为余量）：
 *
 *   0x60400000  Umem PSRAM kumm（malloc/free，NSH free）— 产品 1 MiB
 *   board_psram_pool_base()  BoardPSRAM 独立堆（mm_initialize）— 余量
 *   board_mtp_psram_arena_base()  MTP bump 区（尾端固定，无 free）
 *   0x61000000
 *
 * 产品 nsh：pool 11008 KiB + MTP 256 KiB → kumm 1024 KiB。
 *
 * Kconfig：MYVENDOR_BOARD_PSRAM_POOL_KB、MYVENDOR_MTP_PSRAM_RESERVE_KB。
 * 宏：*_KB 供 #if；*_BYTES 供运行时 size_t。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_BOARD_PSRAM_LAYOUT_H
#define MY_VENDOR_BOARD_PSRAM_LAYOUT_H

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

/* PSRAM_DATA 分区：HyperBus 堆窗口，与 sifli_allocateheap.c / ptab 一致 */

#define BOARD_PSRAM_DATA_BASE  0x60400000u
#define BOARD_PSRAM_DATA_SIZE  0x00C00000u  /* 12 MiB (16 MiB chip − 4 MiB code XIP) */

/*
 * MTP 尾端 bump 预留（KiB）。
 * 由 mtp_psram.c 顺序分配大缓存；不参与 NuttX 堆，一般无对应 free。
 * 未启用 MYVENDOR_MTP_SIMPLE 时为 0。
 */

#if defined(CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB)
#  define BOARD_MTP_PSRAM_RESERVE_KB CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB
#elif defined(CONFIG_MYVENDOR_MTP_SIMPLE)
#  define BOARD_MTP_PSRAM_RESERVE_KB 256
#else
#  define BOARD_MTP_PSRAM_RESERVE_KB 0
#endif

#define BOARD_MTP_PSRAM_RESERVE_BYTES \
  ((size_t)BOARD_MTP_PSRAM_RESERVE_KB * 1024u)

/*
 * BoardPSRAM 独立堆大小（KiB）。
 * board_malloc.c 在此区域 mm_initialize()；从 kumm 之前切出，与 NSH free 无关。
 * MYVENDOR_BOARD_PSRAM_POOL_KB=0 时整段 PSRAM 仅 kumm（及可选 MTP 尾）。
 */

#if defined(CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB)
#  define BOARD_PSRAM_POOL_KB CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB
#elif defined(CONFIG_BSP_USING_PSRAM)
#  define BOARD_PSRAM_POOL_KB 11008
#else
#  define BOARD_PSRAM_POOL_KB 0
#endif

#define BOARD_PSRAM_POOL_BYTES ((size_t)BOARD_PSRAM_POOL_KB * 1024u)

/**
 * @brief 交给 kumm 的 PSRAM 字节数（region 1 长度）。
 *
 * 算法：PSRAM_DATA 总长 − BoardPSRAM 池 − MTP 尾预留。
 * 须与 sifli_allocateheap.c::arm_addregion() 中 psram_heap 扣减相同。
 */
static inline size_t board_psram_kumm_bytes(void)
{
  size_t avail = BOARD_PSRAM_DATA_SIZE;

  if (BOARD_PSRAM_POOL_BYTES > 0 && avail > BOARD_PSRAM_POOL_BYTES)
    {
      avail -= BOARD_PSRAM_POOL_BYTES;
    }

  if (BOARD_MTP_PSRAM_RESERVE_BYTES > 0 &&
      avail > BOARD_MTP_PSRAM_RESERVE_BYTES)
    {
      avail -= BOARD_MTP_PSRAM_RESERVE_BYTES;
    }

  return avail;
}

/**
 * @brief BoardPSRAM 独立堆起始物理地址（紧接 kumm 之后）。
 */
static inline uintptr_t board_psram_pool_base(void)
{
  return BOARD_PSRAM_DATA_BASE + board_psram_kumm_bytes();
}

/**
 * @brief MTP bump 区起始（PSRAM_DATA 末尾向上预留）。
 */
static inline uintptr_t board_mtp_psram_arena_base(void)
{
  return BOARD_PSRAM_DATA_BASE + BOARD_PSRAM_DATA_SIZE -
         BOARD_MTP_PSRAM_RESERVE_BYTES;
}

/*
 * ============================ 画布专属 arena ============================
 * 2026-09-27：vmap 的常驻画布（~470 KB）原来从 BoardPSRAM 池里 malloc。
 * 现在从池子里**挖出一块固定、1 MiB 对齐的 1 MiB**给画布专用：画布地址从此确定，
 * 不再和几百 KB 的其它分配抢同一片堆（也顺手把画布的取舍从"堆有多大"里独立出来）。
 *
 * ⚠ 关于"让画布变快"（2026-09-27 两轮实测，结论已收口）：
 *   ① **PSRAM 现在默认写回**（`CONFIG_MYVENDOR_PSRAM_CACHE_WB=y`）：128 KiB 顺序写
 *      22 → 15 周期/字节（−33%），CPU 搬运段（`cyc_begin`）−40%；
 *   ② **vmap 的 EPIC 搬运已打开**（`MYVENDOR_BLIT_EPIC=1`）⇒ 同一搬运段再 −61%
 *      （合计约 −75%）；
 *   ③ 但**瓦片光栅化（`cyc_tile`）纹丝不动**：画布 457 KB vs D-cache 16 KB，逐像素散写
 *      缓存帮不上；要动它只能走算法或把大块纯色填充也交给 EPIC。
 *   ⚠ 给 arena **单加一个 MPU region** 那条路仍然是死的（WB/WT 各试一次都引导循环，
 *     region 表回读完全正确 ⇒ 机制未明）—— 别再试；`mpu_config()` 保持出厂那份。
 *   ⚠ 别把 `0xE0080000` 的 irange/drange 当成缓存开关 —— 那是**缓存性能计数器**
 *     （profiler），只选给哪个 MPI 计数。
 *   证据：`docs/psram_cache_wb_audit.md`（仓库内）+ `zcode/analysis/MPU_WB_ATTEMPT.md`。
 *
 * ⚠ 两块约束，改这里之前先读：
 *  ① arena **必须 2 的幂对齐 + 2 的幂大小**：现在保留这个形状不用钱，而将来若真要
 *     给它单独配 MPU region / 缓存窗口，那是硬要求；
 *  ② 地址一律由下面的宏推导，不要在别处手写常量；`board_psram_canvas_arena()`
 *     开机打一行地址，且自带三条自检（越界 / 对齐 / 池子是否真被挖开）。
 *
 * 选择偏移 9 MiB（= 池内偏低的大块留给既有用户，尾端 768 KiB 仍归池子）。
 */
#define BOARD_PSRAM_CANVAS_ARENA_BYTES  (1024u * 1024u)
#define BOARD_PSRAM_CANVAS_ARENA_ALIGN  (1024u * 1024u)
#define BOARD_PSRAM_CANVAS_ARENA_OFFSET (9u * 1024u * 1024u)

/** @brief 画布 arena 的物理地址（池内固定偏移；编译期常量表达式）。 */
static inline uintptr_t board_psram_canvas_arena_base(void)
{
  return board_psram_pool_base() + BOARD_PSRAM_CANVAS_ARENA_OFFSET;
}

/**
 * @brief 池子里落在 arena **之前**的字节数（BoardPSRAM 堆 region 0）。
 */
static inline size_t board_psram_pool_lo_bytes(void)
{
  if (BOARD_PSRAM_POOL_BYTES <= BOARD_PSRAM_CANVAS_ARENA_OFFSET)
    {
      return 0;
    }

  return BOARD_PSRAM_CANVAS_ARENA_OFFSET;
}

/**
 * @brief 池子里落在 arena **之后**的字节数（BoardPSRAM 堆 region 1）。
 */
static inline size_t board_psram_pool_hi_bytes(void)
{
  const size_t skip =
      BOARD_PSRAM_CANVAS_ARENA_OFFSET + BOARD_PSRAM_CANVAS_ARENA_BYTES;

  if (BOARD_PSRAM_POOL_BYTES <= skip)
    {
      return 0;
    }

  return BOARD_PSRAM_POOL_BYTES - skip;
}

#endif /* MY_VENDOR_BOARD_PSRAM_LAYOUT_H */
