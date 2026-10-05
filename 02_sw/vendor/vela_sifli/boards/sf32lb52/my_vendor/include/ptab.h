/**
 * @file ptab.h
 * @brief 按启动介质分派分区宏（NAND 或 SD/eMMC）。
 *
 * NAND 与 SD/eMMC 布局完全不同（NAND：ftab@0、按块稀疏对齐；
 * SD/eMMC：MBR@0 + ftab@0x1000、密排）。启动介质在构建时由
 * `boot_loader/storage.conf` 选定，经 `my_vendor_boot_storage.h`
 * 暴露为 `MY_VENDOR_BOOT_FROM_{NAND,SD,EMMC}`。
 *
 * 必须选用与实际烧录镜像一致的分区头，这样 `FS_REGION`、`KV_REGION`
 * 等地址才对得上。SD 布局见 `docs/sd_partition.md`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PTAB__H__
#define __PTAB__H__

#include "my_vendor_boot_storage.h"

#if defined(MY_VENDOR_BOOT_FROM_SD) || defined(MY_VENDOR_BOOT_FROM_EMMC)
#  include "ptab_sdmmc.h"
#else
#  include "ptab_nand.h"
#endif

#endif /* __PTAB__H__ */
