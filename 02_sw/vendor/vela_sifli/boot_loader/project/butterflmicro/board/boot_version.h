/**
 * @file boot_version.h
 * @brief 2SFBL version string. Independent of NuttX PRODUCT_VERSION.
 *
 * Printed on the msh banner, written to persist.boot.version for NuttX/BLE.
 * Bump when shipping a new bootloader.bin.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_VERSION_H
#define BOOT_VERSION_H

#define BOOT_LOADER_VERSION  "1.2.0"

#define BOOT_VERSION_KEY   "persist.boot.version"
#define BOOT_VERSION_PATH  "/" BOOT_VERSION_KEY /* boot_lfs remaps to /db/ */

#ifdef __cplusplus
extern "C" {
#endif

/** Write persist.boot.version if missing or different.  KV must be mountable. */
int boot_version_publish(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_VERSION_H */
