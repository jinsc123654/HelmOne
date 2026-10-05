/**
 * @file board_malloc.h
 * @brief 板级显式 SRAM / PSRAM 分配。
 *
 * PSRAM 池在固定 buffer 上通过 NuttX mm_initialize() 注册为独立堆
 *（类似 RT-Thread memheap），可用 mm_mallinfo 查看 free/used。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_BOARD_MALLOC_H
#define MY_VENDOR_BOARD_MALLOC_H

#include <malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 PSRAM 独立堆（幂等；首次 board_malloc_psram 也会自动调用）。
 *
 * @return 0 成功；-ENOTSUP 未配置 PSRAM 池
 */
int board_psram_heap_init(void);

/** PSRAM 独立堆是否已就绪。 */
bool board_psram_heap_ready(void);

/** 查询 PSRAM 独立堆用量（NuttX mallinfo，含 arena/fordblks/uordblks）。 */
struct mallinfo board_psram_mallinfo(void);

/**
 * 查询 Umem 子 region 用量（TLSF walk，按 KiB 粒度与 boardmem 一致）。
 *
 * region 0：片内 SRAM（malloc 主 region）
 * region 1：PSRAM kumm（CONFIG_MM_REGIONS>1 时）
 *
 * @param region  region 索引
 * @param info    输出；usmblks 无 per-region 峰值，与 uordblks 相同
 * @return true 成功；false region 无效或未初始化
 */
bool board_umem_region_mallinfo(int region, struct mallinfo *info);

/**
 * 返回 Umem 子 region 尾部探测窗口起始地址（末 probe_len 字节，位于 end
 * guard 节点之前，不破坏 mm 元数据）。
 *
 * @return true 成功；false region 无效或区域过小
 */
bool board_umem_region_tail_addr(int region, size_t probe_len,
                                 uintptr_t *addr);

/**
 * 返回 BoardPSRAM 池尾部探测窗口起始地址（堆已初始化时用 mm_heapend 前
 * probe_len 字节；否则用物理池末 probe_len 字节）。
 *
 * @return true 成功；false 未配置 PSRAM 池或区域过小
 */
bool board_psram_pool_tail_addr(size_t probe_len, uintptr_t *addr);

/**
 * 从 SRAM 主堆分配（NuttX malloc，线程安全）。
 *
 * @param size  字节数
 * @return 指针；失败 NULL
 */
void *board_malloc_sram(size_t size);

/**
 * 从 PSRAM 独立堆分配（mm_malloc；池满或未启用时回退 SRAM）。
 *
 * 池大小：Kconfig MYVENDOR_BOARD_PSRAM_POOL_KB。
 *
 * @param size  字节数
 * @return 指针；失败 NULL
 */
void *board_malloc_psram(size_t size);

/**
 * @brief 调整 board_malloc_psram / board_mem_free 得到的指针（BoardPSRAM 或 SRAM 回退）。
 *
 * oldmem 为 NULL 时等同 board_malloc_psram()。
 *
 * @param ptr 原指针，可为 NULL。
 * @param size 新字节数。
 * @return 新指针；失败 NULL。
 */
void *board_realloc_psram(void *ptr, size_t size);

/** 释放 board_malloc_sram() 分配的内存。 */
void board_free_sram(void *ptr);

/** 释放 board_malloc_psram() 分配的内存。 */
void board_free_psram(void *ptr);

/** 按地址自动选择 SRAM / PSRAM 堆并释放。 */
void board_mem_free(void *ptr);

/** 指针是否落在 BoardPSRAM 独立堆内（非 SRAM 回退）。 */
bool board_ptr_in_psram_pool(const void *ptr);

/**
 * @brief 画布专属 arena（板级 MPU 给它的区间开 write-back）。
 *
 * 位置/大小由 `board_psram_layout.h` 的 `BOARD_PSRAM_CANVAS_ARENA_*` 决定；
 * 它是**固定地址、不属于任何堆**（BoardPSRAM 堆被拆成 arena 前/后两块加进去，
 * 见 `board_malloc.c` 的 `board_psram_init()`），所以**不能 free**。
 *
 * @param[out] bytes 可为 NULL；输出 arena 字节数。
 * @return 基址；不可用（未启用 PSRAM / 与 Umem 重叠）时 NULL。
 */
void *board_psram_canvas_arena(size_t *bytes);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_BOARD_MALLOC_H */
