/**
 * @file boot_lfs.h
 * @brief 二级 boot 挂载 KV_REGION 或 FS_REGION LittleFS（与 NuttX 同几何）。
 * 同一时刻只挂一卷（SRAM 缓冲只有一份）。msh 默认浏览 KV；产品 /fw 也在 KV 上。
 *        persist.* 在 /db/（NuttX /mnt/kv/db）。
 *        USB MTP 在工厂 NuttX 固件里。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_LFS_H
#define BOOT_LFS_H

#include "lfs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_LFS_NAME_MAX 64

typedef struct boot_lfs_comp
{
    char common[BOOT_LFS_NAME_MAX];
    unsigned nmatch;
    int unique_dir;
} boot_lfs_comp_t;

int  boot_lfs_mount(void);
int  boot_lfs_mount_fs(void);
int  boot_lfs_mount_tag(const char *tag);
void boot_lfs_unmount(void);
int  boot_lfs_mounted(void);
int  boot_lfs_ls(const char *path);
int  boot_lfs_cat(const char *path);
void boot_lfs_print_info(void);
int  boot_lfs_complete(const char *dir, const char *prefix, boot_lfs_comp_t *out);
void boot_lfs_complete_list(const char *dir, const char *prefix);

uint64_t boot_lfs_size(void);
uint64_t boot_lfs_free(void);

int        boot_lfs_file_open_ro(const char *path, lfs_file_t *file);
lfs_ssize_t boot_lfs_file_read(lfs_file_t *file, void *buf, lfs_size_t size);
lfs_soff_t boot_lfs_file_seek(lfs_file_t *file, lfs_soff_t off);
lfs_soff_t boot_lfs_file_size(lfs_file_t *file);
int        boot_lfs_file_close(lfs_file_t *file);

/** Create/truncate @p path and write @p size bytes. Uses the shared file cache. */
int boot_lfs_put(const char *path, const void *data, lfs_size_t size);
/** Remove a file. LFS_ERR_NOENT if missing. */
int boot_lfs_unlink(const char *path);

int boot_lfs_dir_open(const char *path, lfs_dir_t *dir);
int boot_lfs_dir_read(lfs_dir_t *dir, struct lfs_info *info);
int boot_lfs_dir_close(lfs_dir_t *dir);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_LFS_H */
