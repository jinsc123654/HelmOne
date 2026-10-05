/**
 * @file transfer_backend.c
 * @brief 按路径寻址的文件原语实现（BLE Companion FS / MTP 共用）。
 *
 * 沙箱根 XFER_STORAGE_ROOT；OTA 路径映射到 XFER_OTA_ROOT。不用 statfs 遍历 LittleFS。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "transfer_backend.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/crc32.h>

#include "myvendor_mtp.h"
#include "myvendor_fw_slot.h"
#include "myvendor_watchdog.h"

/** 单次 write 切片：释放 LittleFS 锁，让 UI 扫盘/按键不跟 16KiB 落盘死锁。 */
#define XFER_WRITE_SLICE  4096u
#define XFER_WRITE_YIELD_US 2000u

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void put_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

/** @brief 向路径追加一个组件（拒绝 ..）。 */
static int append_component(char *out, size_t *olen, size_t outsz,
                            const char *comp, size_t clen)
{
  if (clen == 0 || (clen == 1 && comp[0] == '.'))
    {
      return 0;
    }

  if (clen == 2 && comp[0] == '.' && comp[1] == '.')
    {
      return EINVAL;
    }

  if (*olen + 1 + clen >= outsz)
    {
      return ENAMETOOLONG;
    }

  out[(*olen)++] = '/';
  memcpy(out + *olen, comp, clen);
  *olen += clen;
  out[*olen] = '\0';
  return 0;
}

/** @brief 生成 `*.part` 旁路路径。 */
static int make_part_path(const char *final_path, char *tmp_path, size_t tmp_sz)
{
  int ret;

  if (final_path == NULL || tmp_path == NULL)
    {
      return EINVAL;
    }

  ret = snprintf(tmp_path, tmp_sz, "%s.part", final_path);
  if (ret < 0 || (size_t)ret >= tmp_sz)
    {
      return ENAMETOOLONG;
    }

  return 0;
}

/** @brief 计算已有文件 CRC32 与大小（续传用）。 */
static int hash_existing(const char *path, uint32_t *crc, uint32_t *size)
{
  uint8_t buf[256];
  int     fd;
  ssize_t n;

  *crc  = 0;
  *size = 0;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
    }

  while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
      *crc   = crc32part(buf, (size_t)n, *crc);
      *size += (uint32_t)n;
    }

  close(fd);
  if (n < 0)
    {
      return errno != 0 ? errno : EIO;
    }

  return 0;
}

/** @brief 跳过绝对路径中的已知根前缀。 */
static int skip_abs_root(const char *in, size_t inlen, const char *root,
                         const char **rest, size_t *restlen)
{
  size_t n = strlen(root);

  if (inlen < n || memcmp(in, root, n) != 0)
    {
      return 0;
    }

  if (inlen > n && in[n] != '/')
    {
      return 0;
    }

  *rest    = in + n;
  *restlen = inlen - n;
  return 1;
}

/** @brief 判断是否 OTA/fw 路径请求。 */
static int ota_request(const char *in, size_t inlen,
                       const char **rest, size_t *restlen)
{
  const char *p;
  size_t      n;

  if (in == NULL || inlen == 0)
    {
      return 0;
    }

  if (skip_abs_root(in, inlen, XFER_OTA_ROOT, rest, restlen) ||
      skip_abs_root(in, inlen, "/mnt/lfs/fw", rest, restlen))
    {
      return 1;
    }

  p = in;
  n = inlen;
  while (n > 0 && *p == '/')
    {
      p++;
      n--;
    }

  if (n >= 2 && p[0] == 'f' && p[1] == 'w' && (n == 2 || p[2] == '/'))
    {
      *rest    = p + 2;
      *restlen = n - 2;
      return 1;
    }

  return 0;
}

/** @brief 判断是否崩溃日志路径请求。 */
static int coredump_request(const char *in, size_t inlen,
                            const char **rest, size_t *restlen)
{
  const char *p;
  size_t      n;

  if (in == NULL || inlen == 0)
    {
      return 0;
    }

  if (skip_abs_root(in, inlen, XFER_COREDUMP_ROOT, rest, restlen))
    {
      return 1;
    }

  p = in;
  n = inlen;
  while (n > 0 && *p == '/')
    {
      p++;
      n--;
    }

  if (n >= 8 && memcmp(p, "coredump", 8) == 0 && (n == 8 || p[8] == '/'))
    {
      *rest    = p + 8;
      *restlen = n - 8;
      return 1;
    }

  return 0;
}

/** @brief 判断是否诊断日志路径请求。 */
static int diag_request(const char *in, size_t inlen,
                        const char **rest, size_t *restlen)
{
  const char *p;
  size_t      n;

  if (in == NULL || inlen == 0)
    {
      return 0;
    }

  if (skip_abs_root(in, inlen, XFER_DIAG_ROOT, rest, restlen))
    {
      return 1;
    }

  p = in;
  n = inlen;
  while (n > 0 && *p == '/')
    {
      p++;
      n--;
    }

  /* 只认整个 "diag" 分量，避免 "diagnostics" 之类被误吞。 */
  if (n >= 4 && memcmp(p, "diag", 4) == 0 && (n == 4 || p[4] == '/'))
    {
      *rest    = p + 4;
      *restlen = n - 4;
      return 1;
    }

  return 0;
}

/** @brief 判断是否常用点 TSV 路径请求。 */
static int fav_request(const char *in, size_t inlen)
{
  const char *p;
  size_t      n;

  if (in == NULL || inlen == 0)
    {
      return 0;
    }

  if (inlen == sizeof(XFER_FAV_PATH) - 1 &&
      memcmp(in, XFER_FAV_PATH, sizeof(XFER_FAV_PATH) - 1) == 0)
    {
      return 1;
    }

  p = in;
  n = inlen;
  while (n > 0 && *p == '/')
    {
      p++;
      n--;
    }

  if (n == 9 && memcmp(p, "favorites", 9) == 0)
    {
      return 1;
    }

  if (n == 13 && memcmp(p, "favorites.tsv", 13) == 0)
    {
      return 1;
    }

  if (n == sizeof("kv/bicycle_favorites.tsv") - 1 &&
      memcmp(p, "kv/bicycle_favorites.tsv",
             sizeof("kv/bicycle_favorites.tsv") - 1) == 0)
    {
      return 1;
    }

  return 0;
}

/** @brief 在 root 下解析相对路径组件。 */
static int resolve_under(const char *root, const char *in, size_t inlen,
                         char *out, size_t outsz)
{
  size_t rootlen = strlen(root);
  size_t i;
  size_t olen;
  int    ret;

  if (out == NULL || outsz < rootlen + 1)
    {
      return EINVAL;
    }

  memcpy(out, root, rootlen);
  out[rootlen] = '\0';
  olen = rootlen;

  if (in == NULL || inlen == 0)
    {
      return 0;
    }

  i = 0;
  while (i < inlen)
    {
      const char *comp;
      size_t      clen;

      while (i < inlen && in[i] == '/')
        {
          i++;
        }

      if (i >= inlen)
        {
          break;
        }

      comp = in + i;
      clen = 0;
      while (i + clen < inlen && in[i + clen] != '/')
        {
          clen++;
        }

      ret = append_component(out, &olen, outsz, comp, clen);
      if (ret != 0)
        {
          return ret;
        }

      i += clen;
    }

  return 0;
}

/** @brief 确保 OTA 根目录存在。 */
static void ensure_ota_dir(const char *path)
{
  size_t n = sizeof(XFER_OTA_ROOT) - 1;

  if (path == NULL)
    {
      return;
    }

  if (strncmp(path, XFER_OTA_ROOT, n) != 0)
    {
      return;
    }

  if (path[n] != '\0' && path[n] != '/')
    {
      return;
    }

  (void)mkdir(XFER_OTA_ROOT, 0777);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int xfer_resolve(const char *in, size_t inlen, char *out, size_t outsz)
{
  const char *rest;
  size_t      restlen;
  const char *p;
  size_t      n;

  if (in == NULL || inlen == 0 ||
      (inlen == 1 && (in[0] == '/' || in[0] == '.')))
    {
      return resolve_under(XFER_STORAGE_ROOT, NULL, 0, out, outsz);
    }

  if (ota_request(in, inlen, &rest, &restlen))
    {
      return resolve_under(XFER_OTA_ROOT, rest, restlen, out, outsz);
    }

  if (coredump_request(in, inlen, &rest, &restlen))
    {
      return resolve_under(XFER_COREDUMP_ROOT, rest, restlen, out, outsz);
    }

  if (diag_request(in, inlen, &rest, &restlen))
    {
      return resolve_under(XFER_DIAG_ROOT, rest, restlen, out, outsz);
    }

  if (fav_request(in, inlen))
    {
      size_t favlen = sizeof(XFER_FAV_PATH) - 1;

      if (outsz <= favlen)
        {
          return ENAMETOOLONG;
        }

      memcpy(out, XFER_FAV_PATH, favlen + 1);
      return 0;
    }

  p = in;
  n = inlen;
  if (p[0] == '/')
    {
      if (!skip_abs_root(p, n, XFER_STORAGE_ROOT, &rest, &restlen))
        {
          return EACCES;
        }

      p = rest;
      n = restlen;
    }

  return resolve_under(XFER_STORAGE_ROOT, p, n, out, outsz);
}

/** @brief 目录项是否应对 BLE 隐藏。 */
bool xfer_entry_skipped(const char *name)
{
  size_t n;

  if (name == NULL || name[0] == '\0' || name[0] == '.')
    {
      return true;
    }

  n = strlen(name);
  if (n >= 5 && memcmp(name + n - 5, ".part", 5) == 0)
    {
      return true;
    }

  return false;
}

/** @brief 设备 UI 是否停在 MTP 传输页（`bicycle_mtp_ui.c` 写，别的线程读）。 */
static volatile bool g_usb_page_active;

void xfer_usb_page_set(bool on)
{
  g_usb_page_active = on;
}

bool xfer_usb_page_active(void)
{
  return g_usb_page_active;
}

/** @brief USB 是否占用 LFS —— **强绑定 MTP 传输页**。
 *
 *  用户 2026-09-20 定："就不能强绑定 MTP 页面吗，**只有 MTP 页面弹出才给锁给标志**。"
 *  所以判据就一条：设备 UI 是否停在 USB/MTP 传输页（`g_usb_page_active`）。
 *    页面弹出 ⇒ 给 LFS 锁（BLE FS 返回 EBUSY）+ 给状态标志（app 显示占用）；
 *    页面不在 ⇒ 锁和标志都不给。
 *
 *  历史（都废弃）：
 *   - `myvendor_mtp_session_active()`（主机选了 MTP 配置即真）→ 插线充电、
 *     只枚举不选 MTP 都误报，而且插上就退不出来；
 *   - 短暂的"恒 false"→ 上位机永远看不到占用，用户要的是"页面在才给"。
 *
 *  注意 app 侧的 `isTransferBusy` 是 `FS_BUSY | USB_MTP_BUSY` 或起来的
 *  （flutter lib/ble/companion_proto.dart）：BLE FS 事务未收尾时（bit4）
 *  app 也会说"USB/MTP 占用中"—— 那一半由 `companion_fs.c` 的
 *  "取消订阅即收事务" 负责，与本标志无关。 */
bool xfer_usb_busy(void)
{
  return g_usb_page_active;
}

/** @brief 获取 LFS hold（嵌套计数）。 */
void xfer_lfs_acquire(void)
{
  myvendor_mtp_lfs_hold("companion");
}

/** @brief 释放 LFS hold。 */
void xfer_lfs_release(void)
{
  myvendor_mtp_lfs_release("companion");
}

/**
 * @brief 查询路径 stat。
 * @return 0 成功，正数 errno。
 */
int xfer_stat(const char *path, struct xfer_stat *out)
{
  struct stat st;

  if (path == NULL || out == NULL)
    {
      return EINVAL;
    }

  if (stat(path, &st) != 0)
    {
      return errno != 0 ? errno : EIO;
    }

  out->type  = S_ISDIR(st.st_mode) ? 1 : 0;
  out->size  = (uint32_t)st.st_size;
  out->mtime = (uint32_t)st.st_mtime;
  return 0;
}

/**
 * @brief 打包目录列表页。
 * @return 字节数或负 errno。
 */
int xfer_list_pack(const char *dir, uint16_t *cursor, uint8_t *out,
                   uint16_t out_max, bool *last)
{
  DIR           *dp;
  struct dirent *ent;
  uint16_t       skip;
  uint16_t       packed = 0;
  uint16_t       off = 0;
  int            err = 0;

  if (dir == NULL || cursor == NULL || out == NULL || last == NULL)
    {
      return -EINVAL;
    }

  *last = false;
  skip  = *cursor;

  dp = opendir(dir);
  if (dp == NULL)
    {
      return errno != 0 ? -errno : -EIO;
    }

  while ((ent = readdir(dp)) != NULL)
    {
      struct xfer_stat st;
      char             full[XFER_PATH_MAX];
      size_t           nlen;
      uint16_t         need;
      int              ret;

      if (xfer_entry_skipped(ent->d_name))
        {
          continue;
        }

      if (skip > 0)
        {
          skip--;
          continue;
        }

      nlen = strlen(ent->d_name);
      if (nlen > 255)
        {
          nlen = 255;
        }

      need = (uint16_t)(1 + 4 + 4 + 1 + nlen);
      if (off + need > out_max)
        {
          /* This page is full; caller will resume with the updated cursor. */

          goto done;
        }

      ret = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
      if (ret < 0 || (size_t)ret >= sizeof(full))
        {
          continue;
        }

      ret = xfer_stat(full, &st);
      if (ret != 0)
        {
          continue;
        }

      out[off] = st.type;
      put_le32(out + off + 1, st.size);
      put_le32(out + off + 5, st.mtime);
      out[off + 9] = (uint8_t)nlen;
      memcpy(out + off + 10, ent->d_name, nlen);
      off    = (uint16_t)(off + need);
      packed++;
    }

  *last = true;

done:
  if (closedir(dp) != 0 && err == 0)
    {
      err = errno;
    }

  *cursor = (uint16_t)(*cursor + packed);
  if (err != 0)
    {
      return -err;
    }

  return (int)off;
}

/** @brief 打开读 fd（跨 notify 保持）。 */
int xfer_read_open(const char *path, int *fd)
{
  int f;

  if (path == NULL || fd == NULL)
    {
      return EINVAL;
    }

  f = open(path, O_RDONLY);
  if (f < 0)
    {
      return errno != 0 ? errno : EIO;
    }

  *fd = f;
  return 0;
}

/** @brief seek 保持的读 fd。 */
int xfer_read_seek(int fd, uint32_t offset)
{
  if (fd < 0)
    {
      return EINVAL;
    }

  if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)-1)
    {
      return errno != 0 ? errno : EIO;
    }

  return 0;
}

/** @brief 从保持的读 fd 读数据。 */
int xfer_read_data(int fd, uint8_t *buf, uint16_t len, uint16_t *got)
{
  ssize_t n;

  if (fd < 0 || buf == NULL || got == NULL)
    {
      return EINVAL;
    }

  *got = 0;
  n = read(fd, buf, len);
  if (n < 0)
    {
      return errno != 0 ? errno : EIO;
    }

  *got = (uint16_t)n;
  return 0;
}

/** @brief 关闭保持的读 fd。 */
void xfer_read_close(int fd)
{
  if (fd >= 0)
    {
      close(fd);
    }
}

int xfer_read(const char *path, uint32_t offset, uint8_t *buf, uint16_t len,
              uint16_t *got)
{
  int fd;
  int ret;

  if (got != NULL)
    {
      *got = 0;
    }

  ret = xfer_read_open(path, &fd);
  if (ret != 0)
    {
      return ret;
    }

  ret = xfer_read_seek(fd, offset);
  if (ret != 0)
    {
      xfer_read_close(fd);
      return ret;
    }

  ret = xfer_read_data(fd, buf, len, got);
  xfer_read_close(fd);
  return ret;
}

/** @brief 建目录。 */
int xfer_mkdir(const char *path)
{
  if (path == NULL)
    {
      return EINVAL;
    }

  ensure_ota_dir(path);

  if (mkdir(path, 0777) != 0)
    {
      return errno != 0 ? errno : EIO;
    }

  return 0;
}

/** @brief 删文件/空目录及对应 .part。 */
int xfer_delete(const char *path)
{
  struct stat st;
  char        part[XFER_PATH_MAX];

  if (path == NULL)
    {
      return EINVAL;
    }

  if (make_part_path(path, part, sizeof(part)) == 0)
    {
      unlink(part);
    }

  if (stat(path, &st) != 0)
    {
      return errno != 0 ? errno : EIO;
    }

  if (S_ISDIR(st.st_mode))
    {
      if (rmdir(path) != 0)
        {
          return errno != 0 ? errno : EIO;
        }
    }
  else if (unlink(path) != 0)
    {
      return errno != 0 ? errno : EIO;
    }

  return 0;
}

/** @brief 重命名并同步 .part 旁路。 */
int xfer_rename(const char *from, const char *to)
{
  char from_part[XFER_PATH_MAX];
  char to_part[XFER_PATH_MAX];

  if (from == NULL || to == NULL)
    {
      return EINVAL;
    }

  if (rename(from, to) != 0)
    {
      return errno != 0 ? errno : EIO;
    }

  if (make_part_path(from, from_part, sizeof(from_part)) == 0 &&
      make_part_path(to, to_part, sizeof(to_part)) == 0)
    {
      rename(from_part, to_part);
    }

  return 0;
}

/** @brief 打开 *.part 写会话（可 resume）。 */
int xfer_write_open(const char *final_path, char *tmp_path, size_t tmp_sz,
                    int *fd, bool resume, uint32_t *out_off, uint32_t *out_crc)
{
  uint32_t crc  = 0;
  uint32_t size = 0;
  int      ret;
  int      flags;

  if (final_path == NULL || tmp_path == NULL || fd == NULL)
    {
      return EINVAL;
    }

  ensure_ota_dir(final_path);

  ret = make_part_path(final_path, tmp_path, tmp_sz);
  if (ret != 0)
    {
      return ret;
    }

  if (resume)
    {
      ret = hash_existing(tmp_path, &crc, &size);
      if (ret != 0)
        {
          return ret;
        }
    }

  flags = O_WRONLY | O_CREAT;
  if (!resume)
    {
      flags |= O_TRUNC;
      crc   = 0;
      size  = 0;
    }

  *fd = open(tmp_path, flags, 0666);
  if (*fd < 0)
    {
      return errno != 0 ? errno : EIO;
    }

  if (resume && size > 0 && lseek(*fd, 0, SEEK_END) == (off_t)-1)
    {
      int e = errno != 0 ? errno : EIO;
      close(*fd);
      *fd = -1;
      return e;
    }

  if (out_off != NULL)
    {
      *out_off = size;
    }

  if (out_crc != NULL)
    {
      *out_crc = crc;
    }

  return 0;
}

/** @brief 向 *.part 追加数据。 */
int xfer_write_data(int fd, const uint8_t *buf, size_t len)
{
  if (fd < 0 || buf == NULL)
    {
      return EINVAL;
    }

  while (len > 0)
    {
      size_t  chunk = len;
      ssize_t n;

      if (chunk > XFER_WRITE_SLICE)
        {
          chunk = XFER_WRITE_SLICE;
        }

      myvendor_watchdog_work_beat();
      myvendor_watchdog_hw_pet();
      n = write(fd, buf, chunk);
      if (n < 0)
        {
          return errno != 0 ? errno : EIO;
        }

      if (n == 0)
        {
          return ENOSPC;
        }

      buf += (size_t)n;
      len -= (size_t)n;
      myvendor_watchdog_work_beat();
      myvendor_watchdog_hw_pet();
      if (len > 0)
        {
          /* 让出 LFS 互斥，bicycle_ui 才能跑 lv_timer_handler。 */
          usleep(XFER_WRITE_YIELD_US);
        }
    }

  return 0;
}

/** @brief rename 提交正式文件。 */
int xfer_write_commit(int fd, const char *tmp_path, const char *final_path)
{
  if (fd >= 0)
    {
      myvendor_watchdog_hw_pet();
      close(fd);
    }

  if (tmp_path == NULL || final_path == NULL)
    {
      return EINVAL;
    }

  unlink(final_path);
  if (rename(tmp_path, final_path) != 0)
    {
      unlink(tmp_path);
      return errno != 0 ? errno : EIO;
    }

  (void)myvendor_fw_slot_on_commit(final_path);
  return 0;
}

/** @brief 关闭 fd 保留 *.part。 */
void xfer_write_suspend(int fd)
{
  if (fd >= 0)
    {
      fsync(fd);
      close(fd);
    }
}

/** @brief 中止写入并删 *.part。 */
void xfer_write_abort(int fd, const char *tmp_path)
{
  if (fd >= 0)
    {
      close(fd);
    }

  if (tmp_path != NULL && tmp_path[0] != '\0')
    {
      unlink(tmp_path);
    }
}
