/**
 * @file transfer_backend.h
 * @brief 按路径寻址的文件原语，供 BLE Companion FS（以及后续 MTP）使用。
 *
 * 不用 statfs() 遍历 LittleFS——在本卷上该调用无上界
 *（见 docs/ble/companion_bringup_notes.md）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_TRANSFER_BACKEND_H
#define MYVENDOR_TRANSFER_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_MYVENDOR_BLE_COMPANION_STORAGE_PATH
#  define XFER_STORAGE_ROOT "/mnt/lfs"  /**< 文件管理沙箱根。 */
#else
#  define XFER_STORAGE_ROOT CONFIG_MYVENDOR_BLE_COMPANION_STORAGE_PATH
#endif

/**
 * BLE OTA 独占槽。相对路径 "fw" / "fw/…" 以及绝对路径 /mnt/kv/fw/…
 *（以及遗留 /mnt/lfs/fw/…）都解析到这里，不在 LFS 沙箱内。
 * 2SFBL 以 /fw 读同一批文件。
 */
#define XFER_OTA_ROOT  "/mnt/kv/fw"

/**
 * BLE 常用点文件。相对路径 "favorites" / "favorites.tsv" 以及
 * /mnt/kv/bicycle_favorites.tsv 都解析到这里（不在 LFS 沙箱内）。
 */
#define XFER_FAV_PATH  "/mnt/kv/bicycle_favorites.tsv"

/**
 * 崩溃日志目录。相对路径 "coredump" / "coredump/…" 以及
 * /mnt/kv/coredump/… 都解析到这里（不在 LFS 沙箱内）。
 */
#define XFER_COREDUMP_ROOT  "/mnt/kv/coredump"

/**
 * 诊断日志目录（myvendor_diaglog 写的 dNNN_YYYYMMDD_HHMMSS.txt）。
 * 相对路径 "diag" / "diag/…" 以及 /mnt/kv/diag/… 都解析到这里
 *（不在 LFS 沙箱内）。与 #XFER_COREDUMP_ROOT 分开：两者命名规则相同，
 * 但目录和清理上限各自独立，所以路径命名空间也必须分开，
 * 否则 App 的 "coredump" 前缀会把 diag 也吞掉。
 */
#define XFER_DIAG_ROOT  "/mnt/kv/diag"

#define XFER_PATH_MAX  256  /**< 解析后路径缓冲上限。 */

/**
 * @brief xfer_stat() 结果。
 */
struct xfer_stat
{
  uint8_t  type;   /**< 0 文件，1 目录。 */
  uint32_t size;   /**< 字节；目录可为 0。 */
  uint32_t mtime;  /**< POSIX mtime。 */
};

/**
 * @brief 把协议路径（不必 NUL 结尾）解析进沙箱。
 *
 * "fw" / "fw/…"（以及 /mnt/kv/fw、遗留 /mnt/lfs/fw）走 #XFER_OTA_ROOT；
 * "coredump" / "coredump/…"（以及 /mnt/kv/coredump）走 #XFER_COREDUMP_ROOT；
 * "diag" / "diag/…"（以及 /mnt/kv/diag）走 #XFER_DIAG_ROOT；
 * "favorites" / "favorites.tsv" 走 #XFER_FAV_PATH；
 * 其余留在 #XFER_STORAGE_ROOT。拒绝 ".." 以及逃出根的路径。
 * 空输入解析为 LFS 根本身。
 *
 * @param in 输入路径。
 * @param inlen 输入字节数。
 * @param[out] out 输出缓冲。
 * @param outsz 输出容量。
 * @return 0 成功，正数为 errno。
 */
int xfer_resolve(const char *in, size_t inlen, char *out, size_t outsz);

/**
 * @brief 该目录项是否应对 BLE 隐藏（隐藏名、中断写入留下的 .part）。
 */
bool xfer_entry_skipped(const char *name);

/**
 * @brief USB 是否正占用 LittleFS —— **强绑定 MTP 传输页**。
 *
 * 用户 2026-09-20 定："只有 MTP 页面弹出才给锁给标志"。
 * 页面弹出 ⇒ 给锁（BLE FS 返回 EBUSY）+ 给状态标志（app 显示占用）；
 * 页面不在 ⇒ 都不给。判据只有一个：`xfer_usb_page_active()`。
 */
bool xfer_usb_busy(void);

/**
 * @brief 设置 / 读取"UI 停在 MTP 传输页"这一位。
 *
 * 线程约定：**UI 线程写**（`bicycle_mtp_ui.c`：页面 push 成功、leave 完成各一次），
 * 其它线程读。单个 bool、单一写者，读侧容忍旧值。
 */
void xfer_usb_page_set(bool on);

/**
 * @brief 见 #xfer_usb_page_set。
 */
bool xfer_usb_page_active(void);

/**
 * @brief BLE FS 事务期间拦住地图瓦片读 LFS。可嵌套，acquire/release 必须配对。
 */
void xfer_lfs_acquire(void);

/**
 * @brief 与 #xfer_lfs_acquire 配对。
 */
void xfer_lfs_release(void);

/**
 * @brief 查询路径类型与大小。
 * @return 0 成功，负值为 -errno。
 */
int xfer_stat(const char *path, struct xfer_stat *out);

/**
 * @brief 顺序打包目录项。
 *
 * @p cursor 为已消费的可见项个数，按本次打包数递增。
 * 碰到 readdir 结尾则置 @p last。每项为
 * `[type:1][size:4 le][mtime:4 le][name_len:1][name...]`。
 *
 * @return 打包字节数，或负 errno。
 */
int xfer_list_pack(const char *dir, uint16_t *cursor, uint8_t *out,
                   uint16_t out_max, bool *last);

/**
 * @brief 读文件一段（每次 open/close，适合小块）。
 */
int xfer_read(const char *path, uint32_t offset, uint8_t *buf, uint16_t len,
              uint16_t *got);

/**
 * @brief 跨 BLE READ notify 页保持读 fd。xfer_read() 每次开关会拖慢 LittleFS 下载。
 */
int  xfer_read_open(const char *path, int *fd);

/**
 * @brief 把保持的读 fd seek 到 offset。
 */
int  xfer_read_seek(int fd, uint32_t offset);

/**
 * @brief 从保持的读 fd 读数据。
 */
int  xfer_read_data(int fd, uint8_t *buf, uint16_t len, uint16_t *got);

/**
 * @brief 关闭保持的读 fd。
 */
void xfer_read_close(int fd);

/**
 * @brief 建目录。
 */
int xfer_mkdir(const char *path);

/**
 * @brief 删文件或空目录。
 */
int xfer_delete(const char *path);

/**
 * @brief 重命名（可跨目录，仍须在沙箱内）。
 */
int xfer_rename(const char *from, const char *to);

/**
 * @brief 打开旁路 `*.part` 开始写；调用方校验 size+CRC32 后再 commit rename。
 *
 * resume=true 保留已有 *.part（BLE 重连）。@p out_off / @p out_crc 返回已写前缀。
 */
int xfer_write_open(const char *final_path, char *tmp_path, size_t tmp_sz,
                    int *fd, bool resume, uint32_t *out_off,
                    uint32_t *out_crc);

/**
 * @brief 向 *.part 追加数据。
 */
int xfer_write_data(int fd, const uint8_t *buf, size_t len);

/**
 * @brief 校验完成后 rename 提交。
 */
int xfer_write_commit(int fd, const char *tmp_path, const char *final_path);

/**
 * @brief 关掉 fd，磁盘上留下 *.part。
 */
void xfer_write_suspend(int fd);

/**
 * @brief 中止写入并删除 *.part。
 */
void xfer_write_abort(int fd, const char *tmp_path);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_TRANSFER_BACKEND_H */
