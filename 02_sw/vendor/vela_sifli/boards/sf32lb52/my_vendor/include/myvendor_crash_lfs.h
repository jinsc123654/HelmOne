/**
 * @file myvendor_crash_lfs.h
 * @brief 崩溃裸机 LittleFS：符号加 cdump_ 前缀，避免和 NuttX libfs 冲突。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_CRASH_LFS_H
#define MYVENDOR_CRASH_LFS_H

#define LFS_NO_DEBUG
#define LFS_NO_WARN
#define LFS_NO_ERROR
#define LFS_NO_ASSERT
#define LFS_NO_MALLOC
#define LFS_NAME_MAX 128

#define lfs_crc                cdump_lfs_crc
#define lfs_format             cdump_lfs_format
#define lfs_mount              cdump_lfs_mount
#define lfs_unmount            cdump_lfs_unmount
#define lfs_remove             cdump_lfs_remove
#define lfs_rename             cdump_lfs_rename
#define lfs_stat               cdump_lfs_stat
#define lfs_getattr            cdump_lfs_getattr
#define lfs_setattr            cdump_lfs_setattr
#define lfs_removeattr         cdump_lfs_removeattr
#define lfs_file_open          cdump_lfs_file_open
#define lfs_file_opencfg       cdump_lfs_file_opencfg
#define lfs_file_close         cdump_lfs_file_close
#define lfs_file_sync          cdump_lfs_file_sync
#define lfs_file_read          cdump_lfs_file_read
#define lfs_file_write         cdump_lfs_file_write
#define lfs_file_seek          cdump_lfs_file_seek
#define lfs_file_truncate      cdump_lfs_file_truncate
#define lfs_file_tell          cdump_lfs_file_tell
#define lfs_file_rewind        cdump_lfs_file_rewind
#define lfs_file_size          cdump_lfs_file_size
#define lfs_file_path          cdump_lfs_file_path
#define lfs_file_getattr       cdump_lfs_file_getattr
#define lfs_file_setattr       cdump_lfs_file_setattr
#define lfs_mkdir              cdump_lfs_mkdir
#define lfs_dir_open           cdump_lfs_dir_open
#define lfs_dir_close          cdump_lfs_dir_close
#define lfs_dir_read           cdump_lfs_dir_read
#define lfs_dir_seek           cdump_lfs_dir_seek
#define lfs_dir_tell           cdump_lfs_dir_tell
#define lfs_dir_rewind         cdump_lfs_dir_rewind
#define lfs_dir_path           cdump_lfs_dir_path
#define lfs_fs_stat            cdump_lfs_fs_stat
#define lfs_fs_size            cdump_lfs_fs_size
#define lfs_fs_traverse        cdump_lfs_fs_traverse
#define lfs_fs_mkconsistent    cdump_lfs_fs_mkconsistent
#define lfs_migrate            cdump_lfs_migrate
#define lfs_fs_rawtraverse     cdump_lfs_fs_rawtraverse
#define lfs_fs_rawmkconsistent cdump_lfs_fs_rawmkconsistent

#include "lfs.h"

#endif /* MYVENDOR_CRASH_LFS_H */
