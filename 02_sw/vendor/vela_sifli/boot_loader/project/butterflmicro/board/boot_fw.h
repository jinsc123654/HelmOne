/**
 * @file boot_fw.h
 * @brief Load product OVNX from versioned files on KV_REGION /fw.
 *
 * Upload as /mnt/kv/fw/<version>.bin (BLE OTA / factory MTP). Boot already
 * mounts KV; tries the highest X.Y.Z first; same version uses OVNX
 * build_unix. Keep at least two copies.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_FW_H
#define BOOT_FW_H

#include "boot_ovnx.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_FW_DIR     "/fw"
#define BOOT_FW_MAX     12
#define BOOT_FW_NAME_MAX 64

/**
 * Scan KV /fw, try newest valid OVNX then older. KV stays mounted.
 * On success the payload is already in PSRAM at @p dest (CRC checked).
 */
int boot_fw_try_load(uint32_t dest, uint32_t dest_max,
                     struct boot_ovnx_image_info *info);

/** msh `fw`: list KV /fw/*.bin newest-first. */
void boot_fw_list(void);

#define BOOT_FW_CHECK_OK     0
#define BOOT_FW_CHECK_CRC   (-1)
#define BOOT_FW_CHECK_MOUNT (-2)
#define BOOT_FW_CHECK_EMPTY (-3)

/**
 * CRC-check KV /fw/*.bin in the same pick order as autoboot. No PSRAM load.
 * On success copies the chosen file name into @p name_out.
 * Returns BOOT_FW_CHECK_OK / _CRC / _MOUNT / _EMPTY.
 */
int boot_fw_check(struct boot_ovnx_image_info *info, char *name_out,
                  unsigned name_max, int *jumpable_out);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_FW_H */
