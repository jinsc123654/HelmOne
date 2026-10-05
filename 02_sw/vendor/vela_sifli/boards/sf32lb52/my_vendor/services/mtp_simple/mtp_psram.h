/**
 * @file mtp_psram.h
 * @brief mtp_simple 大/慢缓冲放在 PSRAM（PSRAM_DATA 尾部，kumm heap 外）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_PSRAM_H
#define MY_VENDOR_MTP_PSRAM_H

#include <nuttx/config.h>
#include <stddef.h>

#include "board_psram_layout.h"

#define MTP_PSRAM_DATA_BASE   BOARD_PSRAM_DATA_BASE
#define MTP_PSRAM_DATA_SIZE   BOARD_PSRAM_DATA_SIZE
#define MTP_PSRAM_RESERVE_SIZE BOARD_MTP_PSRAM_RESERVE_BYTES
#define MTP_PSRAM_ARENA_BASE  board_mtp_psram_arena_base()

/** PSRAM_DATA 尾部为 mtp_simple 保留的总字节（Kconfig）。 */
#define MTP_PSRAM_ARENA_BYTES  MTP_PSRAM_RESERVE_SIZE

/**
 * @brief 初始化 bump arena。
 * @return 0 成功。
 */
int mtp_psram_init(void);

/**
 * @brief 从 PSRAM arena 分配（无 free；生命周期 = mtp_simple 进程）。
 */
void *mtp_psram_malloc(size_t size);

/** @brief calloc 风格 bump 分配。 */
void *mtp_psram_calloc(size_t n, size_t size);

/** @brief arena 已分配字节（boardmem 统计用）。 */
size_t mtp_psram_used_bytes(void);

#endif /* MY_VENDOR_MTP_PSRAM_H */
