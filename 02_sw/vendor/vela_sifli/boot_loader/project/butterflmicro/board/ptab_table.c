/**
 * @file ptab_table.c
 * @brief 编译进固件的分区表实现（由 ptab JSON 生成，勿手改）。
 *
 * 源：ptab.sdmmc.json（BOOT_STORAGE=sd）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "ptab_table.h"

const struct ptab_entry g_ptab_table[PTAB_ENTRY_COUNT] =
{
    { "sd", "", "MBR", 0x62000000u, 0x00000000u, 0x62000000u, 0x00001000u },
    { "sd", "ftab", "FLASH_TABLE", 0x62000000u, 0x00001000u, 0x62001000u, 0x00010000u },
    { "sd", "bootloader", "", 0x62000000u, 0x00011000u, 0x62011000u, 0x00028000u },
    { "sd", "", "BOOT_RESERVE", 0x62000000u, 0x00039000u, 0x62039000u, 0x000C7000u },
    { "sd", "main", "HCPU_FLASH_CODE", 0x62000000u, 0x00100000u, 0x62100000u, 0x00400000u },
    { "sd", "factory", "HCPU_FACTORY_CODE", 0x62000000u, 0x00500000u, 0x62500000u, 0x00400000u },
    { "sd", "", "COREDUMP_REGION", 0x62000000u, 0x00900000u, 0x62900000u, 0x08000000u },
    { "sd", "", "LOW256_RESERVE", 0x62000000u, 0x08900000u, 0x6A900000u, 0x07700000u },
    { "sd", "kv_root", "KV_REGION", 0x62000000u, 0x10000000u, 0x72000000u, 0x10000000u },
    { "sd", "fs_root", "FS_REGION", 0x62000000u, 0x20000000u, 0x82000000u, 0x20000000u },
    { "sd", "fat_root", "FAT_REGION", 0x62000000u, 0x40000000u, 0xA2000000u, 0x00000000u },
    { "psram1", "", "", 0x60000000u, 0x00000000u, 0x60000000u, 0x00400000u },
    { "psram1", "", "PSRAM_DATA", 0x60000000u, 0x00400000u, 0x60400000u, 0x00C00000u },
    { "psram1_cbus", "", "HCPU_PSRAM_CODE", 0x10000000u, 0x00000000u, 0x10000000u, 0x00400000u },
    { "hpsys_ram", "", "HCPU_RAM_DATA", 0x20000000u, 0x00000000u, 0x20000000u, 0x0007FC00u },
    { "hpsys_ram", "", "FLASH_BOOT_LOADER", 0x20000000u, 0x00020000u, 0x20020000u, 0x00028000u },
    { "hpsys_ram", "", "BOOTLOADER_RAM_DATA", 0x20000000u, 0x00048000u, 0x20048000u, 0x00032000u },
    { "hpsys_ram", "", "HCPU_RO_DATA", 0x20000000u, 0x0007FC00u, 0x2007FC00u, 0x00000000u },
    { "hpsys_ram", "", "HPSYS_MBOX", 0x20000000u, 0x0007FC00u, 0x2007FC00u, 0x00000400u },
    { "hpsys_ram", "", "HCPU2LCPU_MB_CH2_BUF", 0x20000000u, 0x0007FC00u, 0x2007FC00u, 0x00000200u },
    { "hpsys_ram", "", "HCPU2LCPU_MB_CH1_BUF", 0x20000000u, 0x0007FE00u, 0x2007FE00u, 0x00000200u },
    { "lpsys_ram", "", "LPSYS_RAM", 0x20400000u, 0x00000000u, 0x20400000u, 0x00006000u },
};

const unsigned g_ptab_table_count = PTAB_ENTRY_COUNT;

/**
 * @brief 按镜像名查找。
 * @param img 镜像名；NULL 则返回 NULL。
 * @return 第一处匹配，没有则为 NULL。
 */
const struct ptab_entry *ptab_find_img(const char *img)
{
    unsigned i;

    if (img == NULL)
    {
        return NULL;
    }

    for (i = 0; i < PTAB_ENTRY_COUNT; i++)
    {
        if (g_ptab_table[i].img[0] != '\0' &&
            strcmp(g_ptab_table[i].img, img) == 0)
        {
            return &g_ptab_table[i];
        }
    }

    return NULL;
}

/**
 * @brief 按 tag 查找（匹配 tags 中以 '|' 分隔的一个 token）。
 * @param tag tag 字符串；NULL 或空串则返回 NULL。
 * @return 第一处匹配，没有则为 NULL。
 */
const struct ptab_entry *ptab_find_tag(const char *tag)
{
    unsigned i;
    size_t taglen;

    if (tag == NULL || tag[0] == '\0')
    {
        return NULL;
    }

    taglen = strlen(tag);

    for (i = 0; i < PTAB_ENTRY_COUNT; i++)
    {
        const char *p = g_ptab_table[i].tags;

        while (p != NULL && *p != '\0')
        {
            const char *sep = strchr(p, '|');
            size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);

            if (len == taglen && strncmp(p, tag, taglen) == 0)
            {
                return &g_ptab_table[i];
            }

            if (sep == NULL)
            {
                break;
            }

            p = sep + 1;
        }
    }

    return NULL;
}
