/**
 * @file ptab_sdmmc.h
 * @brief SF32LB52 从 SD/eMMC（SDIO/SD1）启动时的静态分区宏。
 *
 * 源：`boot_loader/config/nsh/ptab.sdmmc.json`，必须与 JSON 保持同步。
 * 布局说明见 `docs/sd_partition.md`。
 *
 * 卡字节偏移 = SBUS 地址 − `0x62000000`。低 512 MiB 为杂项带，
 * 用户 LittleFS 从 512 MiB 起固定 512 MiB，FAT 从 1 GiB 起到卡末：
 *
 * - `[0, 1 MiB)`          boot：MBR + FLASH_TABLE + 160 KiB bootloader
 *                         + BOOT_RESERVE（垫满 1 MiB，不必挪 main）
 * - `[1 MiB, 5 MiB)`      单槽 firmware（main，4 MiB）
 * - `[5 MiB, 9 MiB)`      factory 槽（4 MiB）
 * - `[9 MiB, 137 MiB)`    COREDUMP_REGION 128 MiB（原始 dump，非 LittleFS）
 * - pad 到 256 MiB        LOW256_RESERVE（不要放用户 LFS）
 * - `[256 MiB, 512 MiB)`  KV_REGION → `/mnt/kv`（256 MiB LittleFS）
 * - `[512 MiB, 1 GiB)`    FS_REGION → `/mnt/lfs`（512 MiB LittleFS）
 * - `[1 GiB, 卡末)`       FAT_REGION → `/mnt/fat`（ptab `max_size=0`，
 *                         运行时按 CSD 余量填充；主机打包按 16GB 卡
 *                         CSD 14832 MiB，见 `FAT_REGION_PACK_*`；产品只读；
 *                         根下 map/ fonts/）
 *
 * bootloader 镜像 `max_size` 为 160 KiB，与 SRAM `FLASH_BOOT_LOADER`
 *（`0x20020000`，接到 `BOOTLOADER_RAM_DATA` @ `0x20048000`，200 KiB）一致。
 * ftab 拷贝长度跟该字段。再加长先挪 data RAM 或改跑 PSRAM。无 BOOT_FS 分区。
 *
 * 由 `ptab.h` 分派：app 侧看 `MY_VENDOR_BOOT_FROM_SD` / `EMMC`；
 * 二级 boot 看 `BOOT_DEFAULT_FROM_SD` / `EMMC`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PTAB_SDMMC__H__
#define __PTAB_SDMMC__H__

/** @name FLASH_TABLE（ftab）
 *  @brief 卡偏移 4 KiB，长 64 KiB。二级 boot 查表。
 *  @{
 */
#undef  FLASH_TABLE_START_ADDR
#define FLASH_TABLE_START_ADDR                             (0x62001000)
#undef  FLASH_TABLE_SIZE
#define FLASH_TABLE_SIZE                                   (0x00010000)
#undef  FLASH_TABLE_OFFSET
#define FLASH_TABLE_OFFSET                                 (0x00001000)
/** @} */

#undef  AUTO_FLASH_MAC_ADDRESS
#define AUTO_FLASH_MAC_ADDRESS                             (1644167168)

/** @name FLASH_BOOT_LOADER（二级 boot 镜像槽）
 *  @brief 卡偏移 0x11000，长 160 KiB。拷贝进 SRAM 0x20020000（同长）。
 *  @{
 */
#undef  FLASH_BOOT_LOADER_START_ADDR
#define FLASH_BOOT_LOADER_START_ADDR                       (0x62011000)
#undef  FLASH_BOOT_LOADER_SIZE
#define FLASH_BOOT_LOADER_SIZE                             (0x00028000)
#undef  FLASH_BOOT_LOADER_OFFSET
#define FLASH_BOOT_LOADER_OFFSET                           (0x00011000)
/** @} */

/** @name HCPU_FLASH_CODE（main 固件）
 *  @brief 卡偏移 1 MiB，长 4 MiB。单槽；后续镜像放 KV LittleFS。
 *  @{
 */
#undef  HCPU_FLASH_CODE_START_ADDR
#define HCPU_FLASH_CODE_START_ADDR                         (0x62100000)
#undef  HCPU_FLASH_CODE_SIZE
#define HCPU_FLASH_CODE_SIZE                               (0x00400000)
#undef  HCPU_FLASH_CODE_OFFSET
#define HCPU_FLASH_CODE_OFFSET                             (0x00100000)
/** @} */

/** @name COREDUMP_REGION
 *  @brief 卡偏移 5 MiB，长 128 MiB。原始 crash dump 占位，不是 LittleFS。
 *  @{
 */
#undef  COREDUMP_REGION_START_ADDR
#define COREDUMP_REGION_START_ADDR                         (0x62500000)
#undef  COREDUMP_REGION_SIZE
#define COREDUMP_REGION_SIZE                               (0x08000000)
#undef  COREDUMP_REGION_OFFSET
#define COREDUMP_REGION_OFFSET                             (0x00500000)
/** @} */

/** @name KV_REGION（/mnt/kv）
 *  @brief 卡偏移 256 MiB，长 256 MiB。VELA KVDB / 蓝牙 persist，与 /mnt/lfs 隔离。
 *  @{
 */
#undef  KV_REGION_START_ADDR
#define KV_REGION_START_ADDR                               (0x72000000)
#undef  KV_REGION_SIZE
#define KV_REGION_SIZE                                     (0x10000000)
#undef  KV_REGION_OFFSET
#define KV_REGION_OFFSET                                   (0x10000000)
/** @} */

/** @name FS_REGION（/mnt/lfs）
 *  @brief 卡偏移 512 MiB，长 512 MiB。用户 LittleFS（MTP/BLE/GPX），不含地图。
 *  @{
 */
#undef  FS_REGION_START_ADDR
#define FS_REGION_START_ADDR                               (0x82000000)
#undef  FS_REGION_SIZE
#define FS_REGION_SIZE                                     (0x20000000)
#undef  FS_REGION_OFFSET
#define FS_REGION_OFFSET                                   (0x20000000)
/** @} */

/** @name FAT_REGION（/mnt/fat）
 *  @brief 卡偏移 1 GiB。SIZE=0 表示运行时用 CSD 余量（FAT；产品 RO，工厂 RW）。
 *         编译进 ptab 的 size 是 uint32，装不下 14 GiB，所以不能写死。
 *         主机 `build-fs` / `pack-sd-img` 按 16GB 卡 CSD 14832 MiB
 *         打种子（FAT ≈ 13808 MiB）。15 GiB 种子在这种卡上会 EINVAL。
 *  @{
 */
#undef  FAT_REGION_START_ADDR
#define FAT_REGION_START_ADDR                              (0xA2000000)
#undef  FAT_REGION_SIZE
#define FAT_REGION_SIZE                                    (0x00000000)
#undef  FAT_REGION_OFFSET
#define FAT_REGION_OFFSET                                  (0x40000000)
#define FAT_REGION_PACK_CARD_MIB                           (14832u)
/** @} */

/** @name PSRAM_DATA（psram1 SBUS）
 *  @brief 与 NAND 布局相同的 12 MiB 数据窗。
 *  @{
 */
#undef  PSRAM_DATA_START_ADDR
#define PSRAM_DATA_START_ADDR                              (0x60400000)
#undef  PSRAM_DATA_SIZE
#define PSRAM_DATA_SIZE                                    (0x00C00000)
#undef  PSRAM_DATA_OFFSET
#define PSRAM_DATA_OFFSET                                  (0x00400000)
/** @} */

/** @name HCPU_PSRAM_CODE（psram1_cbus）
 *  @brief 代码执行窗，与 NAND 布局相同。
 *  @{
 */
#undef  HCPU_PSRAM_CODE_START_ADDR
#define HCPU_PSRAM_CODE_START_ADDR                         (0x10000000)
#undef  HCPU_PSRAM_CODE_SIZE
#define HCPU_PSRAM_CODE_SIZE                               (0x00400000)
#undef  HCPU_PSRAM_CODE_OFFSET
#define HCPU_PSRAM_CODE_OFFSET                             (0x00000000)
/** @} */

#undef  CODE_START_ADDR
#define CODE_START_ADDR                                    HCPU_PSRAM_CODE_START_ADDR

/** @name HCPU_RAM_DATA（hpsys_ram）
 *  @brief 与 NAND 布局相同。
 *  @{
 */
#undef  HCPU_RAM_DATA_START_ADDR
#define HCPU_RAM_DATA_START_ADDR                           (0x20000000)
#undef  HCPU_RAM_DATA_SIZE
#define HCPU_RAM_DATA_SIZE                                 (0x0007FC00)
#undef  HCPU_RAM_DATA_OFFSET
#define HCPU_RAM_DATA_OFFSET                               (0x00000000)
/** @} */

/** @name FLASH_BOOT_LOADER XIP（SRAM 拷贝目的）
 *  @brief 二级 boot 运行于 0x20020000，长度必须与卡上槽 160 KiB 一致。
 *  @{
 */
#undef  FLASH_BOOT_LOADER_XIP_START_ADDR
#define FLASH_BOOT_LOADER_XIP_START_ADDR                   (0x20020000)
#undef  FLASH_BOOT_LOADER_XIP_SIZE
#define FLASH_BOOT_LOADER_XIP_SIZE                         (0x00028000)
/** @} */

#undef  BOOTLOADER_RAM_DATA_START_ADDR
#define BOOTLOADER_RAM_DATA_START_ADDR                     (0x20048000)
#undef  BOOTLOADER_RAM_DATA_SIZE
#define BOOTLOADER_RAM_DATA_SIZE                           (0x00032000)

/** @name HPSYS_MBOX
 *  @brief HCPU 邮箱，与 NAND 布局相同。
 *  @{
 */
#undef  HPSYS_MBOX_START_ADDR
#define HPSYS_MBOX_START_ADDR                              (0x2007FC00)
#undef  HPSYS_MBOX_SIZE
#define HPSYS_MBOX_SIZE                                    (0x00000400)
/** @} */

#endif /* __PTAB_SDMMC__H__ */
