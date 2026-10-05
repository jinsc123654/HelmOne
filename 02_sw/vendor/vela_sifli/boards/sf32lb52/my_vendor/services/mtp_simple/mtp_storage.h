/**
 * @file mtp_storage.h
 * @brief 表驱动 MTP 卷列表。
 *
 * Kconfig EXTRA_PATHS 或 mtp_storage.c g_known 增一行即可暴露新盘，无需改 catalog 逻辑。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_STORAGE_H
#define MY_VENDOR_MTP_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "mtp_names.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** 同时挂载的最大 MTP 卷数。 */
#define MTP_STORAGE_MAX 4

/** @brief 一个 MTP 存储卷描述。 */
struct mtp_store
{
  unsigned index;
  uint32_t id;            /**< 0x00010001, 0x00020001, … */
  uint32_t root_handle;   /**< catalog 根 handle：1, 2, 3, … */
  char     path[MTP_STORAGE_PATH_MAX];
  char     desc[MTP_NAME_MAX];
  char     label[MTP_NAME_MAX];
  char     mtddev[64];    /**< 空则容量回退 */
  bool     format_ok;     /**< 是否允许 FormatStore */
};

/** @brief 构建卷表。 */
void mtp_storage_init(void);

/** @brief 确保各卷根目录存在。 */
void mtp_storage_ensure_dirs(void);

/** @brief 卷数量。 */
unsigned mtp_storage_count(void);

/** @brief 按下标取卷。 */
const struct mtp_store *mtp_storage_at(unsigned i);

/** @brief 按 storage id 取卷。 */
const struct mtp_store *mtp_storage_by_id(uint32_t id);

/** @brief 按 root handle 取卷。 */
const struct mtp_store *mtp_storage_by_root(uint32_t handle);

/** @brief 按 LFS 路径最长前缀匹配卷。 */
const struct mtp_store *mtp_storage_for_path(const char *path);

/** @brief id 为 0 或 0xffffffff 表示“全部存储”（GetObjectHandles 等）。 */
bool mtp_storage_id_all(uint32_t id);

/** @brief id 是否为已知卷。 */
bool mtp_storage_id_known(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_MTP_STORAGE_H */
