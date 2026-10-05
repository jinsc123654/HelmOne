/**
 * @file ptab_nand.h
 * @brief sf32lb52-nano_a128r16（128MB NAND + 16MB HyperBus PSRAM）静态分区宏。
 *
 * 由 `boot_loader/config/nsh/ptab.nand.json` 生成，须与 SiFli SDK 同步。
 * 从 NAND 启动时由 ptab.h / my_vendor_boot_storage.h 选中。
 *
 * 布局（低 → 高）：ftab / config / KV / DFU → 槽 A（main）→ 槽 B（main base2）
 * → FS_REGION 100 MiB → 4 MiB 尾预留。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PTAB_NAND__H__
#define __PTAB_NAND__H__

/* flash2 (NAND SBUS) */
#undef  FLASH_TABLE_START_ADDR
#define FLASH_TABLE_START_ADDR                             (0x62000000)
#undef  FLASH_TABLE_SIZE
#define FLASH_TABLE_SIZE                                   (0x00020000)
#undef  FLASH_TABLE_OFFSET
#define FLASH_TABLE_OFFSET                                 (0x00000000)

#undef  AUTO_FLASH_MAC_ADDRESS
#define AUTO_FLASH_MAC_ADDRESS                             (1644167168)

#undef  FLASH_BOOT_LOADER_START_ADDR
#define FLASH_BOOT_LOADER_START_ADDR                       (0x62060000)
#undef  FLASH_BOOT_LOADER_SIZE
#define FLASH_BOOT_LOADER_SIZE                             (0x00010000)
#undef  FLASH_BOOT_LOADER_OFFSET
#define FLASH_BOOT_LOADER_OFFSET                           (0x00060000)

#undef  KVDB_DFU_REGION_START_ADDR
#define KVDB_DFU_REGION_START_ADDR                         (0x62070000)
#undef  KVDB_DFU_REGION_SIZE
#define KVDB_DFU_REGION_SIZE                               (0x00004000)
#undef  KVDB_DFU_REGION_OFFSET
#define KVDB_DFU_REGION_OFFSET                             (0x00070000)

#undef  KVDB_BLE_REGION_START_ADDR
#define KVDB_BLE_REGION_START_ADDR                         (0x62074000)
#undef  KVDB_BLE_REGION_SIZE
#define KVDB_BLE_REGION_SIZE                               (0x00004000)
#undef  KVDB_BLE_REGION_OFFSET
#define KVDB_BLE_REGION_OFFSET                             (0x00074000)

#undef  HCPU_FLASH_CODE_START_ADDR
#define HCPU_FLASH_CODE_START_ADDR                         (0x62240000)
#undef  HCPU_FLASH_CODE_SIZE
#define HCPU_FLASH_CODE_SIZE                               (0x00AE0000)
#undef  HCPU_FLASH_CODE_OFFSET
#define HCPU_FLASH_CODE_OFFSET                             (0x00240000)

#undef  HCPU_FLASH_CODE_LOAD_REGION2_START_ADDR
#define HCPU_FLASH_CODE_LOAD_REGION2_START_ADDR           (0x62D20000)
#undef  HCPU_FLASH_CODE_LOAD_REGION2_SIZE
#define HCPU_FLASH_CODE_LOAD_REGION2_SIZE                 (0x00AE0000)
#undef  HCPU_FLASH_CODE_LOAD_REGION2_OFFSET
#define HCPU_FLASH_CODE_LOAD_REGION2_OFFSET               (0x00D20000)

#undef  FS_REGION_START_ADDR
#define FS_REGION_START_ADDR                               (0x63800000)
#undef  FS_REGION_SIZE
#define FS_REGION_SIZE                                     (0x06400000)
#undef  FS_REGION_OFFSET
#define FS_REGION_OFFSET                                   (0x01800000)

/* psram1 (SBUS) */
#undef  PSRAM_DATA_START_ADDR
#define PSRAM_DATA_START_ADDR                              (0x60400000)
#undef  PSRAM_DATA_SIZE
#define PSRAM_DATA_SIZE                                    (0x00C00000)
#undef  PSRAM_DATA_OFFSET
#define PSRAM_DATA_OFFSET                                  (0x00400000)

/* psram1_cbus (code execution window) */
#undef  HCPU_PSRAM_CODE_START_ADDR
#define HCPU_PSRAM_CODE_START_ADDR                         (0x10000000)
#undef  HCPU_PSRAM_CODE_SIZE
#define HCPU_PSRAM_CODE_SIZE                               (0x00400000)
#undef  HCPU_PSRAM_CODE_OFFSET
#define HCPU_PSRAM_CODE_OFFSET                             (0x00000000)

#undef  CODE_START_ADDR
#define CODE_START_ADDR                                    HCPU_PSRAM_CODE_START_ADDR

/* hpsys_ram */
#undef  HCPU_RAM_DATA_START_ADDR
#define HCPU_RAM_DATA_START_ADDR                           (0x20000000)
#undef  HCPU_RAM_DATA_SIZE
#define HCPU_RAM_DATA_SIZE                                 (0x0007FC00)
#undef  HCPU_RAM_DATA_OFFSET
#define HCPU_RAM_DATA_OFFSET                               (0x00000000)

#undef  FLASH_BOOT_LOADER_XIP_START_ADDR
#define FLASH_BOOT_LOADER_XIP_START_ADDR                   (0x20020000)
#undef  FLASH_BOOT_LOADER_XIP_SIZE
#define FLASH_BOOT_LOADER_XIP_SIZE                         (0x00010000)

#undef  BOOTLOADER_RAM_DATA_START_ADDR
#define BOOTLOADER_RAM_DATA_START_ADDR                     (0x20040000)
#undef  BOOTLOADER_RAM_DATA_SIZE
#define BOOTLOADER_RAM_DATA_SIZE                           (0x00010000)

#undef  HPSYS_MBOX_START_ADDR
#define HPSYS_MBOX_START_ADDR                              (0x2007FC00)
#undef  HPSYS_MBOX_SIZE
#define HPSYS_MBOX_SIZE                                    (0x00000400)

#endif /* __PTAB_NAND__H__ */
