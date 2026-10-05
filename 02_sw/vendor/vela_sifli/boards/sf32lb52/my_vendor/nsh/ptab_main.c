/**
 * @file ptab_main.c
 * @brief NSH 命令 ptab：打印编译时分区表（来自 ptab*.json）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptab_table.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/** @brief 打印 ptab 用法。 */
static void ptab_print_usage(void)
{
  printf("Usage: ptab [tag|img]\n");
  printf("  ptab              print full partition table\n");
  printf("  ptab FS_REGION    lookup region by tag\n");
  printf("  ptab fs_root      lookup region by image name\n");
  printf("Source: %s (BOOT_STORAGE=%s, %u entries)\n",
         PTAB_SOURCE_JSON, PTAB_BOOT_STORAGE, PTAB_ENTRY_COUNT);
}

/** @brief 打印分区表表头。 */
static void ptab_print_header(void)
{
  printf("Partition table (BOOT_STORAGE=%s, source=%s, %u entries)\n",
         PTAB_BOOT_STORAGE, PTAB_SOURCE_JSON, PTAB_ENTRY_COUNT);
  printf("%-12s %-12s %-22s %10s %10s %10s %10s\n",
         "mem", "img", "tags", "base", "offset", "addr", "size");
  printf("%-12s %-12s %-22s %10s %10s %10s %10s\n",
         "------------", "------------", "----------------------",
         "----------", "----------", "----------", "----------");
}

/** @brief 打印单条分区表项。 */
static void ptab_print_entry(FAR const struct ptab_entry *entry)
{
  if (entry == NULL)
    {
      return;
    }

  printf("%-12s %-12s %-22s 0x%08" PRIX32 " 0x%08" PRIX32
         " 0x%08" PRIX32 " 0x%08" PRIX32 "\n",
         entry->mem,
         entry->img[0] ? entry->img : "-",
         entry->tags[0] ? entry->tags : "-",
         entry->base, entry->offset, entry->addr, entry->size);
}

/** @brief 打印完整分区表。 */
static void ptab_print_all(void)
{
  unsigned i;

  ptab_print_header();
  for (i = 0; i < PTAB_ENTRY_COUNT; i++)
    {
      ptab_print_entry(&g_ptab_table[i]);
    }
}

static void ptab_print_one(FAR const char *key)
{
  FAR const struct ptab_entry *entry;

  entry = ptab_find_tag(key);
  if (entry == NULL)
    {
      entry = ptab_find_img(key);
    }

  if (entry == NULL)
    {
      printf("ptab: no region for tag/img '%s'\n", key);
      return;
    }

  ptab_print_header();
  ptab_print_entry(entry);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief NSH 命令 ptab：打印编译时分区表（来自 ptab*.json）。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, FAR char *argv[])
{
  if (argc >= 2 &&
      (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
      ptab_print_usage();
      return EXIT_SUCCESS;
    }

  if (argc >= 2)
    {
      ptab_print_one(argv[1]);
    }
  else
    {
      ptab_print_all();
    }

  return EXIT_SUCCESS;
}
