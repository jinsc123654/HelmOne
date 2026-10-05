/**
 * @file myvendor_fw_slot.h
 * @brief KV `/mnt/kv/fw` 产品固件槽：只保留两份 *.bin，超出或工厂删除都去掉最老一份。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_FW_SLOT_H
#define MYVENDOR_FW_SLOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MYVENDOR_FW_SLOT_DIR       "/mnt/kv/fw"
#define MYVENDOR_FW_SLOT_KEEP      2u
#define MYVENDOR_FW_SLOT_NAME_MAX  64u
#define MYVENDOR_FW_SLOT_SCAN_MAX  16u

struct myvendor_fw_slot_info
{
  char     name[MYVENDOR_FW_SLOT_NAME_MAX];
  uint32_t ver[3];
};

/**
 * @brief 列出槽内 *.bin，最新在前。
 * @return 条目数。
 */
unsigned myvendor_fw_slot_list(struct myvendor_fw_slot_info *out,
                               unsigned maxn);

/**
 * @brief 删除超出 #MYVENDOR_FW_SLOT_KEEP 的旧 *.bin（及同名 .txt）。
 * @return 删除的 bin 个数。
 */
int myvendor_fw_slot_prune(void);

/**
 * @brief 写入完成后：路径若是槽内直接 *.bin 则 prune。
 * @return prune 删除个数，非槽文件为 0。
 */
int myvendor_fw_slot_on_commit(const char *path);

/**
 * @brief 删除最老一份 *.bin。仅当槽内 ≥2 份时允许。
 * @return 0 成功；-ENOENT 空槽；-EBUSY 只剩一份；其它为 -errno。
 */
int myvendor_fw_slot_delete_oldest(char *deleted_name, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_FW_SLOT_H */
