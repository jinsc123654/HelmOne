/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __VENDOR_SIFLI_SF32LB52_SF32LB_FLASH_H
#define __VENDOR_SIFLI_SF32LB52_SF32LB_FLASH_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sf32lb_nand_automount(int minor, uint32_t byte_offset, uint32_t byte_size);

/* Mount LittleFS on /dev/configN at /mnt/lfs (requires flashed fs_root.bin). */

int sf32lb_nand_mount_littlefs(int minor);

#endif /* __VENDOR_SIFLI_SF32LB52_SF32LB_FLASH_H */
