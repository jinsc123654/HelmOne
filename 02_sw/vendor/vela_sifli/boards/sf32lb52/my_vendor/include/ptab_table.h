/**
 * @file ptab_table.h
 * @brief 编译进固件的分区表（由 ptab JSON 生成，勿手改）。
 *
 * 源：ptab.sdmmc.json（BOOT_STORAGE=sd）。生成脚本：
 * `boot_loader/scripts/gen_ptab_table.py`。SD 布局见 `docs/sd_partition.md`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __PTAB_TABLE_H__
#define __PTAB_TABLE_H__

#include <stdint.h>

#define PTAB_BOOT_STORAGE  "sd"  /**< 启动介质名（nand/sd/emmc）。 */
#define PTAB_SOURCE_JSON   "ptab.sdmmc.json"  /**< 生成此表的 JSON 文件名。 */
#define PTAB_ENTRY_COUNT   22u  /**< `g_ptab_table[]` 元素个数。 */

/**
 * @brief 分区表一行（一个 mem 块里的一个 region）。
 */
struct ptab_entry
{
    const char *mem;     /**< 存储块名，如 "sd" / "flash2" / "psram1"。 */
    const char *img;     /**< 镜像名或 ""（如 "main" / "bootloader" / "fs_root"）。 */
    const char *tags;    /**< 区域 tag，以 '|' 拼接，或 ""。 */
    uint32_t    base;    /**< 存储块基址。 */
    uint32_t    offset;  /**< 块内偏移（卡上字节偏移）。 */
    uint32_t    addr;    /**< 绝对地址（base + offset）。 */
    uint32_t    size;    /**< region max_size（字节）；0 表示运行时余量。 */
};

extern const struct ptab_entry g_ptab_table[PTAB_ENTRY_COUNT];
extern const unsigned g_ptab_table_count;

/**
 * @brief 按镜像名查找（如 "fs_root"、"main"）。
 * @param img 镜像名；NULL 则返回 NULL。
 * @return 第一处匹配（ptab.json 中靠前的存储块优先），没有则为 NULL。
 */
const struct ptab_entry *ptab_find_img(const char *img);

/**
 * @brief 按 tag 查找（如 "FS_REGION"、"KV_REGION"）。
 *
 * 匹配 `tags` 里以 '|' 分隔的某一个 token。
 *
 * @param tag tag 字符串；NULL 或空串则返回 NULL。
 * @return 第一处匹配，没有则为 NULL。
 */
const struct ptab_entry *ptab_find_tag(const char *tag);

#endif /* __PTAB_TABLE_H__ */
