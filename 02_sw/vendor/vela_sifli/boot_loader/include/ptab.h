/**
 * @file ptab.h
 * @brief 二级 bootloader 的分区宏分派（NAND 或 SD/eMMC）。
 *
 * 构建时由 `boot_loader/storage.conf` 经 `build.sh` 写成
 * `BOOT_DEFAULT_FROM_*`：
 *
 * - `BOOT_DEFAULT_FROM_NAND`     → `ptab.nand.json`（ftab @ 0x62000000）
 * - `BOOT_DEFAULT_FROM_SD/EMMC`  → `ptab.sdmmc.json`（ftab @ 0x62001000）
 * - nor（预留）                  → `ptab.nor.json`（ftab @ 0x12000000）
 *
 * SD 布局见 `docs/sd_partition.md`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PTAB__H__
#define __PTAB__H__

#if defined(BOOT_DEFAULT_FROM_SD) || defined(BOOT_DEFAULT_FROM_EMMC)
#  include "ptab_sdmmc.h"
#else
#  include "ptab_nand_layout.h"
#endif

#endif /* __PTAB__H__ */
