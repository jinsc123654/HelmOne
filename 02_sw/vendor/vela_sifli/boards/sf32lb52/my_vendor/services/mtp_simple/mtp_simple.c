/**
 * @file mtp_simple.c
 * @brief 轻量 USB MTP 应答器：LittleFS 卷、单线程 poll、无 GLib。
 *
 * 架构：主机 bulk/interrupt/EP0 → /dev/mtp/ep* → poll → dispatch_command()
 * → 数据集打包 / catalog / 可选特性（mtp_features.h）。
 *
 * 大缓冲与 scratch 在 PSRAM（mtp_psram.c、mtp_scratch.h）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp_simple.h"
#include "mtp_features.h"
#include "mtp_names.h"
#include "mtp_storage.h"
#include "mtp_psram.h"
#include "mtp_scratch.h"

#include "myvendor_mtp_internal.h"
#include "myvendor_fw_slot.h"
#include "myvendor_watchdog.h"
#include "board_malloc.h"

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/usb/usb.h>

/* ========================================================================== */
/* §1  USB 设备节点路径与 opcode 列表                        */
/* ========================================================================== */

#define MTP_DEV_PATH   "/dev/mtp"
#define MTP_EP0_PATH   MTP_DEV_PATH "/ep0"
#define MTP_EP_IN      MTP_DEV_PATH "/ep1"
#define MTP_EP_OUT     MTP_DEV_PATH "/ep2"
#define MTP_EP_INT     MTP_DEV_PATH "/ep3"

#define OBJINFO_PARENT       38
#define OBJINFO_FIXED        52
#define OBJINFO_FIXED_WIN64  56   /* +4 when 64-bit ObjectCompressedSize */

/* PTP_STR_MAX_CHARS lives in mtp_scratch.h */

static const uint16_t g_mtp_ops[] =
{
  PTP_OPCODE_GETDEVICEINFO,
  PTP_OPCODE_OPENSESSION,
  PTP_OPCODE_CLOSESESSION,
  PTP_OPCODE_GETSTORAGEIDS,
  PTP_OPCODE_GETSTORAGEINFO,
  PTP_OPCODE_GETNUMOBJECTS,
  PTP_OPCODE_GETOBJECTHANDLES,
  PTP_OPCODE_GETOBJECTINFO,
  PTP_OPCODE_GETOBJECT,
  MTP_OP_GETTHUMB
  MTP_OP_GETPARTIALOBJECT
  PTP_OPCODE_DELETEOBJECT,
  PTP_OPCODE_SENDOBJECTINFO,
  PTP_OPCODE_SENDOBJECT,
  MTP_OP_FORMATSTORE
  MTP_OP_CAPTURE
  PTP_OPCODE_GETDEVICEPROPDESC,
  PTP_OPCODE_GETDEVICEPROPVALUE,
  MTP_OP_SETDEVICEPROP
  MTP_OP_MOVEOBJECT
  MTP_OP_COPYOBJECT
  MTP_OP_OBJPROPS
  MTP_OP_SETOBJPROP
  MTP_OP_OBJREFS
  MTP_OP_UPDATEOBJECT
};

/* ========================================================================== */
/* §2  日志（会话/传输恒开；VERBOSE 可选）       */
/* ========================================================================== */

static bool g_usb_eps_open;

/* SiFli usbdev_fs memcpy's the user buffer.  Command/data scratch lives in
 * the PSRAM bump arena; a gadget memcpy with that pointer has hardfaulted
 * (BFAR 0x20080000 = SRAM_END) right after OpenSession.  Bounce every
 * bulk/interrupt packet through aligned SRAM. */
static uint8_t g_usb_pkt[MTP_MAX_PACKET_FS] __attribute__((aligned(4)));
/* usbdev_fs concatenates full 64B OUT reqs into one read().  Cap at the
 * gadget queue (NRDREQS×64=2KiB) so SendObject drains without 5k syscalls. */
#define MTP_USB_RX_BURST  2048
static uint8_t g_usb_rx[MTP_USB_RX_BURST] __attribute__((aligned(4)));
static uint8_t g_ep0_dummy;

/* Coalesce bulk IN into FS max-packet (64) URBs.  A mid-phase short write
 * (e.g. 12-byte PTP header, then an exact-64 payload) completes the host
 * URB early or leaves it waiting for a ZLP — GetObjectHandles of 15 files
 * is 4+15*4 = 64 payload bytes and trips libmtp "could not get object
 * handles". */
static uint8_t g_in_acc[MTP_MAX_PACKET_FS] __attribute__((aligned(4)));
static uint8_t g_in_pend[MTP_MAX_PACKET_FS] __attribute__((aligned(4)));
static uint8_t g_out_stash[MTP_MAX_PACKET_FS] __attribute__((aligned(4)));
static size_t  g_in_acc_used;
static size_t  g_in_phase_len;
static size_t  g_out_stash_len;
static bool    g_in_pend_full;

static void usb_in_reset(void)
{
  g_in_acc_used = 0;
  g_in_phase_len = 0;
  g_in_pend_full = false;
  g_out_stash_len = 0;
}

/**
 * @brief 格式化一行 syslog；VERBOSE 时镜像到 /dev/console。
 */

static void mtp_vlog(int priority, const char *fmt, va_list ap)
{
  char msg[160];
#if MTP_FEAT_VERBOSE
  static int console_fd = -1;
#endif

  vsnprintf(msg, sizeof(msg), fmt, ap);
  syslog(priority, "mtp_simple: %s", msg);

#if MTP_FEAT_VERBOSE
  if (console_fd < 0)
    {
      console_fd = open("/dev/console", O_WRONLY);
    }

  /* Do not mirror to /dev/console while MTP endpoints are open: on this board
   * console and USB gadget can share the same backend; writes fault in
   * usbdev_fs_write (memcpy memfault during OpenSession catalog scan). */
  if (console_fd >= 0 && !g_usb_eps_open)
    {
      dprintf(console_fd, "mtp_simple: %s\n", msg);
    }
#endif
}

/**
 * @brief INFO 由 MTP_FEAT_INFO 开关（默认关，调用点保留）；ERR 恒开。
 */

static void mtp_info(const char *fmt, ...)
{
#if MTP_FEAT_INFO
  va_list ap;

  va_start(ap, fmt);
  mtp_vlog(LOG_INFO, fmt, ap);
  va_end(ap);
#else
  (void)fmt;
#endif
}

static void mtp_err(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  mtp_vlog(LOG_ERR, fmt, ap);
  va_end(ap);
}

#if MTP_FEAT_VERBOSE
static void mtp_trace(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  mtp_vlog(LOG_DEBUG, fmt, ap);
  va_end(ap);
}
#else
#  define mtp_trace(fmt, ...)
#endif

static void mtp_activity_progress_step(uint64_t done)
{
  static uint64_t last_reported;

  if (done < last_reported)
    {
      last_reported = 0;
    }

  if (done == 0 || done - last_reported >= (uint64_t)MTP_IO_CHUNK)
    {
      myvendor_mtp_activity_progress(done);
      last_reported = done;
    }
}

/* ========================================================================== */
/* §3  类型与运行时状态                                                */
/* ========================================================================== */

/**
 * @brief 返回 LFS 全路径的文件名部分。
 */

static const char *mtp_basename(const char *path)
{
  const char *p = strrchr(path, '/');

  return p ? p + 1 : path;
}

/* In-memory mirror of one LFS file or directory.  Lives in PSRAM catalog[]. */

struct mtp_obj
{
  uint32_t handle;
  uint32_t parent;
  uint32_t storage;
  uint16_t format;
  uint64_t size;
  char     path[MTP_MAX_PATH];
  char     name[MTP_MAX_NAME];
};

static const char *obj_display_name(const struct mtp_obj *obj)
{
  if (obj == NULL)
    {
      return "";
    }

  if (obj->name[0] != '\0')
    {
      return obj->name;
    }

  return mtp_basename(obj->path);
}

/* Per-catalog-slot cache in PSRAM arena (parallel to g_ctx.catalog[]). */

struct mtp_catalog_meta
{
  uint32_t dir_sync_ms;  /* last catalog_sync_children() for folder slots */
  time_t   mtime;        /* cached stat; avoids stat on GetObjectInfo */
  time_t   ctime;
};

struct mtp_storage_cache
{
  uint32_t stat_ms;
  uint64_t cap;
  uint64_t freeb;
};

/* Session, USB fds, in-flight upload, and catalog pointer. */

struct mtp_ctx
{
  int      ep0;
  int      ep_in;
  int      ep_out;
  int      ep_int;
  uint32_t session;
  uint32_t next_handle;
  bool     cancelled;

  /* SendObjectInfo / SendObject (path in PSRAM MTP_UPLOAD_PATH) */
  uint64_t upload_size;
  uint64_t upload_got;
  int      upload_fd;
  uint32_t pending_handle;
  uint32_t upload_parent;
  uint16_t upload_format;
  bool     upload_is_folder;
  bool     upload_folder_committed;
  bool     upload_file_committed;  /* 0-byte file finalized in SendObjectInfo */

  /* Last async event (also served on GETEVENT control request). */
  uint8_t  pending_event[16];
  uint16_t pending_event_len;
  bool     have_pending_event;

  struct mtp_obj *catalog;
  int      obj_count;
  bool     catalog_dirty;  /* force next folder sync after local FS changes */
};

static struct mtp_ctx g_ctx;
static unsigned g_mtp_max_objects;

struct mtp_scratch *g_mtp_scratch;

unsigned mtp_max_objects(void)
{
  return g_mtp_max_objects > 0 ? g_mtp_max_objects : MTP_MAX_OBJECTS_MIN;
}

/** @brief catalog 有效条数，防止 obj_count 越界后扫进未映射 PSRAM。 */
static int catalog_nobj(void)
{
  int n = g_ctx.obj_count;
  int cap = (int)mtp_max_objects();

  if (n < 0)
    {
      return 0;
    }

  if (n > cap)
    {
      return cap;
    }

  return n;
}

/* PSRAM-backed (see mtp_buffers_init): catalog, snap, io, scratch. */

static struct catalog_ph_snap *g_catalog_ph_snap;
static struct mtp_catalog_meta  *g_cat_meta;
static struct mtp_storage_cache *g_storage_cache;
static uint8_t                *g_io_buf;

/* ========================================================================== */
/* §4  PTP string and date helpers (UTF-16LE <-> UTF-8, BMP only)           */
/* ========================================================================== */

/**
 * @brief 从 UTF-8 串读取下一个码点。
 */

static uint32_t utf8_next_codepoint(const char **p, const char *end)
{
  const uint8_t *s = (const uint8_t *)*p;
  uint32_t cp;

  if (s >= (const uint8_t *)end || *s == '\0')
    {
      return 0;
    }

  if (*s < 0x80)
    {
      cp = *s;
      *p  += 1;
      return cp;
    }

  if ((*s & 0xe0) == 0xc0 && s + 1 < (const uint8_t *)end)
    {
      cp = ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
      *p += 2;
      return cp;
    }

  if ((*s & 0xf0) == 0xe0 && s + 2 < (const uint8_t *)end)
    {
      cp = ((uint32_t)(s[0] & 0x0f) << 12) |
           ((uint32_t)(s[1] & 0x3f) << 6) |
           (s[2] & 0x3f);
      *p += 3;
      return cp;
    }

  *p += 1;
  return 0xfffd;
}

/**
 * @brief 计算 UTF-8 转 UTF-16LE 所需 uint16 个数。
 */

static int utf8_to_utf16le_count(const char *utf8)
{
  const char *p = utf8;
  const char *end = utf8 + strlen(utf8);
  int n = 1;  /* terminating wide NUL in PTP count */

  while (p < end && *p != '\0')
    {
      uint32_t cp = utf8_next_codepoint(&p, end);

      if (cp == 0)
        {
          break;
        }

      if (cp > 0xffff)
        {
          cp = 0xfffd;
        }

      n++;
    }

  return n;
}

/**
 * @brief UTF-8 转 UTF-16LE 写入缓冲。
 */

static int utf8_to_utf16le(const char *utf8, uint16_t *out, int max_units)
{
  const char *p = utf8;
  const char *end = utf8 + strlen(utf8);
  int n = 0;

  while (p < end && *p != '\0' && n + 1 < max_units)
    {
      uint32_t cp = utf8_next_codepoint(&p, end);

      if (cp == 0)
        {
          break;
        }

      if (cp > 0xffff)
        {
          cp = 0xfffd;
        }

      out[n++] = (uint16_t)cp;
    }

  out[n++] = 0;
  return n;
}

/**
 * @brief UTF-16LE 转 UTF-8 写入缓冲。
 */

static int utf16le_to_utf8(const uint8_t *utf16, int nwide, char *out,
                           int outsz)
{
  int o = 0;
  int i;

  for (i = 0; i < nwide && o < outsz - 1; i++)
    {
      uint16_t ch = (uint16_t)(utf16[i * 2] | (utf16[i * 2 + 1] << 8));
      uint32_t cp = ch;

      if (cp == 0)
        {
          break;
        }

      if (cp < 0x80)
        {
          out[o++] = (char)cp;
        }
      else if (cp < 0x800)
        {
          if (o + 2 >= outsz)
            {
              break;
            }

          out[o++] = (char)(0xc0 | (cp >> 6));
          out[o++] = (char)(0x80 | (cp & 0x3f));
        }
      else
        {
          if (o + 3 >= outsz)
            {
              break;
            }

          out[o++] = (char)(0xe0 | (cp >> 12));
          out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
          out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }

  out[o] = '\0';
  return o;
}

/**
 * @brief 校验 PTP 字符串字段长度合法。
 */

static bool ptp_str_field_valid(const uint8_t *buf, int bufsz)
{
  int nchars;

  if (bufsz < 1)
    {
      return false;
    }

  nchars = buf[0];
  if (nchars < 1 || nchars > PTP_STR_MAX_CHARS)
    {
      return false;
    }

  return bufsz >= 1 + nchars * 2;
}

/**
 * @brief 清理文件名中的非法字符。
 */

static void mtp_name_sanitize(char *name)
{
  char *p;

  for (p = name; *p != '\0'; p++)
    {
      unsigned char c = (unsigned char)*p;

      if (c < 0x20 || strchr("\\/:*?\"<>|", c) != NULL)
        {
          *p = '_';
        }
    }

  while (name[0] != '\0')
    {
      size_t n = strlen(name);

      if (name[n - 1] != ' ' && name[n - 1] != '.')
        {
          break;
        }

      name[n - 1] = '\0';
    }

  if (name[0] == '\0')
    {
      strncpy(name, "Folder", MTP_MAX_NAME - 1);
      name[MTP_MAX_NAME - 1] = '\0';
    }
}

/**
 * @brief 把 UTF-8 串打包成 PTP UTF-16 数据集字段。
 */

static int ptp_str_put(uint8_t *buf, int bufsz, const char *utf8)
{
  uint16_t *units = MTP_SCRATCH_UTF16;
  int nchars;
  int i;

  if (utf8 == NULL)
    {
      utf8 = "";
    }

  nchars = utf8_to_utf16le_count(utf8);
  if (nchars > PTP_STR_MAX_CHARS || bufsz < 1 + nchars * 2)
    {
      return -1;
    }

  utf8_to_utf16le(utf8, units, nchars);
  buf[0] = (uint8_t)nchars;
  for (i = 0; i < nchars; i++)
    {
      buf[1 + i * 2] = (uint8_t)(units[i] & 0xff);
      buf[2 + i * 2] = (uint8_t)(units[i] >> 8);
    }

  return 1 + nchars * 2;
}

/**
 * @brief 从 PTP UTF-16 字段解析出 UTF-8 串。
 */

static int ptp_str_get(const uint8_t *buf, int bufsz, char *out, int outsz)
{
  int nchars;

  if (bufsz < 1 || outsz < 2)
    {
      return -1;
    }

  nchars = buf[0];
  if (nchars == 0)
    {
      out[0] = '\0';
      return 1;
    }

  if (!ptp_str_field_valid(buf, bufsz))
    {
      return -1;
    }

  utf16le_to_utf8(buf + 1, nchars - 1, out, outsz);
  mtp_name_sanitize(out);
  return 1 + nchars * 2;
}

/**
 * @brief ObjectInfo 数据集中文件名字段偏移。
 */

static size_t object_info_name_offset(const uint8_t *payload, size_t len)
{
  size_t off = OBJINFO_FIXED;

  if (ptp_str_field_valid(payload + off, (int)(len - off)))
    {
      return off;
    }

  if (len >= OBJINFO_FIXED_WIN64 &&
      ptp_str_field_valid(payload + OBJINFO_FIXED_WIN64,
                          (int)(len - OBJINFO_FIXED_WIN64)))
    {
      return OBJINFO_FIXED_WIN64;
    }

  /* Legacy libmtp quirk: leading count byte 0 with real count at +4. */

  if (len >= off + 5 && payload[off] == 0 &&
      ptp_str_field_valid(payload + off + 4, (int)(len - off - 4)))
    {
      return off + 4;
    }

  return off;
}

/* ========================================================================== */
/* §5  Object catalog — LFS tree ↔ stable MTP handles                         */
/* ========================================================================== */

static void catalog_sync_children(uint32_t parent);
static void catalog_sync_children_maybe(uint32_t parent);
static bool mtp_is_root_token(uint32_t p);
static bool mtp_is_store_root_handle(uint32_t h);
static uint32_t parent_norm(uint32_t p, uint32_t store_id);
static void handle_ep0(void);
static void mtp_pump_ep0(void);

/**
 * @brief 按 object handle 在 catalog 中查找。
 */

static struct mtp_obj *catalog_find(uint32_t handle)
{
  int i;
  int n = catalog_nobj();

  for (i = 0; i < n; i++)
    {
      if (g_ctx.catalog[i].handle == handle)
        {
          return &g_ctx.catalog[i];
        }
    }

  return NULL;
}

/**
 * @brief 按 LFS 路径在 catalog 中查找。
 */

static struct mtp_obj *catalog_find_by_path(const char *path)
{
  int i;

  for (i = 0; i < g_ctx.obj_count; i++)
    {
      if (strcmp(g_ctx.catalog[i].path, path) == 0)
        {
          return &g_ctx.catalog[i];
        }
    }

  return NULL;
}

/**
 * @brief 目录扫描时是否跳过该条目。
 */

static bool catalog_entry_skipped(const char *name, const char *fullpath)
{
  /* persist.* 在 /mnt/kv/db；KV 根上的残留不进 MTP。 */
  if (name != NULL && strncmp(name, "persist.", 8) == 0)
    {
      if (fullpath != NULL && strstr(fullpath, "/db/") != NULL)
        {
          return false;
        }

      return true;
    }

#if MTP_CATALOG_SKIP_ENABLE
  size_t rootlen;
  const char *skip = MTP_CATALOG_SKIP_DIR;
  const struct mtp_store *s = mtp_storage_for_path(fullpath);

  if (strcmp(name, skip) == 0)
    {
      return true;
    }

  if (s == NULL || s->path[0] == '\0')
    {
      return false;
    }

  rootlen = strlen(s->path);
  if (fullpath != NULL && strncmp(fullpath, s->path, rootlen) == 0 &&
      fullpath[rootlen] == '/' &&
      strncmp(fullpath + rootlen + 1, skip, strlen(skip)) == 0 &&
      (fullpath[rootlen + 1 + strlen(skip)] == '\0' ||
       fullpath[rootlen + 1 + strlen(skip)] == '/'))
    {
      return true;
    }
#else
  (void)name;
  (void)fullpath;
#endif

  return false;
}

/**
 * @brief 把 dir/name 写入 dst；装不下返回 -ENAMETOOLONG（dst 置空串）。
 */
static int mtp_path_join(char *dst, size_t dstsz, const char *dir,
                         const char *name)
{
  size_t dlen;
  size_t nlen;

  if (dst == NULL || dstsz == 0)
    {
      return -EINVAL;
    }

  dst[0] = '\0';
  if (dir == NULL || name == NULL)
    {
      return -EINVAL;
    }

  dlen = strlen(dir);
  nlen = strlen(name);
  if (dlen + 1u + nlen + 1u > dstsz)
    {
      return -ENAMETOOLONG;
    }

  memcpy(dst, dir, dlen);
  dst[dlen] = '/';
  memcpy(dst + dlen + 1u, name, nlen + 1u);
  return 0;
}

/**
 * @brief 把 a 与 b 首尾相接写入 dst；装不下返回 -ENAMETOOLONG。
 */
static int mtp_path_concat(char *dst, size_t dstsz, const char *a,
                           const char *b)
{
  size_t alen;
  size_t blen;

  if (dst == NULL || dstsz == 0)
    {
      return -EINVAL;
    }

  dst[0] = '\0';
  if (a == NULL || b == NULL)
    {
      return -EINVAL;
    }

  alen = strlen(a);
  blen = strlen(b);
  if (alen + blen + 1u > dstsz)
    {
      return -ENAMETOOLONG;
    }

  memcpy(dst, a, alen);
  memcpy(dst + alen, b, blen + 1u);
  return 0;
}

/**
 * @brief stat 文件并更新 catalog 元数据。
 */

static int catalog_stat_file(const char *path, struct stat *st)
{
  int fd;
  off_t end;

  /* stat() is a blocking LittleFS/SD read.  Pump EP0 first so a host
   * GET_DEVICE_STATUS that arrived while we were between files is not
   * left unanswered across this call.
   */

  mtp_pump_ep0();

  if (stat(path, st) == 0)
    {
      mtp_pump_ep0();
      return 0;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  if (fstat(fd, st) == 0 && S_ISREG(st->st_mode))
    {
      close(fd);
      mtp_pump_ep0();
      return 0;
    }

  /* Some LittleFS nodes open but fstat returns ENOENT / mode 0. */

  end = lseek(fd, 0, SEEK_END);
  close(fd);
  mtp_pump_ep0();
  if (end < 0)
    {
      return -1;
    }

  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG | 0644;
  st->st_size = end;
  return 0;
}

/**
 * @brief CLOCK_MONOTONIC 毫秒。
 */

static uint32_t mtp_now_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/* NAND/USB 热路径常把同优先级的 mtp_simple 占满，idle 进不去，IWDT 会停喂。
 * work 心跳证明传输仍在推进。USB 收数时禁止 yield/usleep：OUT 只有
 * NRDREQS×64B（32×64=2KiB），一让出 CPU，同优先级 UI 会把 365KB
 * SendObject 拖到 20s+，gvfs 进度停在半途并报 Could not send object。 */

#define MTP_RELAX_PERIOD_MS 80
#define MTP_RELAX_IDLE_US   1000

static bool g_mtp_usb_hot;
static uint8_t *g_upload_ram;
static size_t   g_upload_ram_len;
static size_t   g_upload_ram_off;
static uint32_t g_upload_ram_t0;
static volatile bool g_upload_flushing;
static volatile bool g_upload_flush_cancel;
static volatile bool g_upload_need_commit;

/** 有人要求释放缓冲、但 flush 线程还在写 —— 由 flush 线程退出前自己释放。
 *  见 upload_ram_drop()。 */
static volatile bool g_upload_drop_pend;
static pthread_t g_upload_flush_th;
static bool g_upload_flush_has;

/** abort 时等 flush 线程收尾的上限；超时只是**不再等**，释放已改为投递式
 *  （见 upload_ram_drop），所以不会因此产生 use-after-free。 */
#define MTP_UPLOAD_FLUSH_WAIT_MS 5000u

#define MTP_UPLOAD_FLUSH_STACK  12288
#define MTP_UPLOAD_FLUSH_STACK_ALIGN 16u
static void *g_upload_flush_stack_raw;
static void *g_upload_flush_stack;

static bool upload_flush_stack_alloc(void)
{
  uintptr_t aligned;

  if (g_upload_flush_stack != NULL)
    {
      return true;
    }

  g_upload_flush_stack_raw = board_malloc_psram(
      MTP_UPLOAD_FLUSH_STACK + MTP_UPLOAD_FLUSH_STACK_ALIGN - 1u);
  if (g_upload_flush_stack_raw == NULL ||
      !board_ptr_in_psram_pool(g_upload_flush_stack_raw))
    {
      board_mem_free(g_upload_flush_stack_raw);
      g_upload_flush_stack_raw = NULL;
      return false;
    }

  aligned = ((uintptr_t)g_upload_flush_stack_raw +
             MTP_UPLOAD_FLUSH_STACK_ALIGN - 1u) &
            ~((uintptr_t)MTP_UPLOAD_FLUSH_STACK_ALIGN - 1u);
  g_upload_flush_stack = (void *)aligned;
  return true;
}

static void upload_flush_stack_free(void)
{
  board_free_psram(g_upload_flush_stack_raw);
  g_upload_flush_stack_raw = NULL;
  g_upload_flush_stack = NULL;
}

static void upload_ram_drop(void);
static void upload_ram_flush_slice(void);
static bool upload_ram_flush_kick(void);
static void upload_ram_commit_if_idle(void);
static void upload_flush_wait_prior(void);

static bool upload_inflight_path(const char *path)
{
  if (path == NULL || path[0] == '\0' || MTP_UPLOAD_PATH[0] == '\0')
    {
      return false;
    }

  if (!g_upload_flushing && g_upload_ram == NULL)
    {
      return false;
    }

  return strcmp(path, MTP_UPLOAD_PATH) == 0;
}

/* LittleFS is single-locked.  opendir/stat during an in-flight write()
 * waits on that lock for the whole lfs_alloc (20s+ on a packed 14GiB
 * volume).  gvfs GetObjectHandles then stalls the copy dialog even though
 * USB and PTP already finished.  Catalog already has the published file. */

static bool upload_lfs_busy(void)
{
  return g_upload_flushing || g_upload_ram != NULL;
}

static void mtp_sched_relax(void)
{
  static uint32_t last_ms;
  uint32_t now = mtp_now_ms();

  myvendor_watchdog_work_beat();
  if (g_mtp_usb_hot)
    {
      return;
    }

  sched_yield();
  if (last_ms != 0 && (now - last_ms) < MTP_RELAX_PERIOD_MS)
    {
      return;
    }

  last_ms = now;
  usleep(MTP_RELAX_IDLE_US);
}

/**
 * @brief 标记 catalog 需下次同步。
 */

static void catalog_mark_dirty(void)
{
  g_ctx.catalog_dirty = true;
}

/**
 * @brief handle 对应的 catalog 槽下标。
 */

static int catalog_slot_index(const struct mtp_obj *obj)
{
  if (obj == NULL || g_ctx.catalog == NULL ||
      obj < g_ctx.catalog || obj >= g_ctx.catalog + mtp_max_objects())
    {
      return -1;
    }

  return (int)(obj - g_ctx.catalog);
}

/**
 * @brief 移动/清除/重置 catalog 元数据缓存。
 */

static void catalog_meta_move(int dst, int src)
{
  if (g_cat_meta == NULL || dst < 0 || src < 0 ||
      dst >= mtp_max_objects() || src >= mtp_max_objects())
    {
      return;
    }

  g_cat_meta[dst] = g_cat_meta[src];
}

static void catalog_meta_clear(int idx)
{
  if (g_cat_meta == NULL || idx < 0 || idx >= mtp_max_objects())
    {
      return;
    }

  memset(&g_cat_meta[idx], 0, sizeof(g_cat_meta[idx]));
}

static void catalog_meta_reset_all(void)
{
  if (g_cat_meta == NULL)
    {
      return;
    }

  memset(g_cat_meta, 0, mtp_max_objects() * sizeof(*g_cat_meta));
}

/**
 * @brief 设置 catalog 对象的 mtime/ctime 缓存。
 */

static void catalog_obj_set_times(int idx, const struct stat *st)
{
  if (g_cat_meta == NULL || idx < 0 || idx >= mtp_max_objects())
    {
      return;
    }

  g_cat_meta[idx].mtime = st->st_mtime;
  g_cat_meta[idx].ctime = st->st_ctime;
}

/**
 * @brief 补全懒 catalog 未 stat 的文件大小/mtime。
 *
 * GetObjectHandles 跳过逐文件 stat，避免主机超时。Windows 随后
 * GetObjectInfo 会补 size；Ubuntu gvfs/libmtp 多用
 * GetObjectPropList / GetObjectPropValue 的 ObjectSize 做多选拷贝，
 * size=0 会被当成空文件而跳过 GetObject。
 */

static void catalog_refresh_file_stat(struct mtp_obj *obj)
{
  struct stat st;

  if (obj == NULL || obj->format == PTP_FMT_ASSOCIATION)
    {
      return;
    }

  /* SendObject already told the host the ObjectInfo size.  Do not stat the
   * still-empty/partial LFS inode while the background flush is running —
   * gvfs treats size=0 as an empty file and the copy dialog crawls. */

  if (upload_inflight_path(obj->path))
    {
      if (g_ctx.upload_size != 0)
        {
          obj->size = g_ctx.upload_size;
        }

      return;
    }

  if (obj->size != 0)
    {
      return;
    }

  if (catalog_stat_file(obj->path, &st) == 0 && S_ISREG(st.st_mode))
    {
      obj->size = (uint64_t)st.st_size;
      catalog_obj_set_times(catalog_slot_index(obj), &st);
    }
}

/* Snapshot path→handle across catalog_rescan() to keep Windows handles stable. */

struct catalog_ph_snap
{
  uint32_t handle;
  char     path[MTP_MAX_PATH];
};

static int g_catalog_ph_snap_n;

/**
 * @brief 重命名/移动前保留旧 handle。
 */

static uint32_t catalog_old_handle(const struct catalog_ph_snap *old, int old_n,
                                  const char *path)
{
  int i;

  for (i = 0; i < old_n; i++)
    {
      if (strcmp(old[i].path, path) == 0)
        {
          return old[i].handle;
        }
    }

  return 0;
}

/**
 * @brief 快照当前 handle 列表。
 */

static void catalog_snap_handles(void)
{
  int i;

  g_catalog_ph_snap_n = g_ctx.obj_count;
  if (g_catalog_ph_snap_n > mtp_max_objects())
    {
      g_catalog_ph_snap_n = mtp_max_objects();
    }

  for (i = 0; i < g_catalog_ph_snap_n; i++)
    {
      g_catalog_ph_snap[i].handle = g_ctx.catalog[i].handle;
      strncpy(g_catalog_ph_snap[i].path, g_ctx.catalog[i].path,
              sizeof(g_catalog_ph_snap[i].path) - 1);
      g_catalog_ph_snap[i].path[sizeof(g_catalog_ph_snap[i].path) - 1] = '\0';
    }
}

/**
 * @brief 以指定 handle 插入 catalog 项。
 */

static uint32_t g_cat_cap_drops;   /* entries dropped: catalog full (debug) */

static void catalog_add_with_handle(uint32_t parent, uint16_t format,
                                    const char *path, const char *name,
                                    uint64_t size, uint32_t want_handle)
{
  struct mtp_obj *o;
  uint32_t h;

  if (g_ctx.obj_count >= mtp_max_objects())
    {
      g_cat_cap_drops++;
      return;
    }

  h = want_handle;
  if (h == 0)
    {
      h = g_ctx.next_handle++;
    }
  else if (h >= g_ctx.next_handle)
    {
      g_ctx.next_handle = h + 1;
    }

  o = &g_ctx.catalog[g_ctx.obj_count++];
  o->handle = h;
  o->parent = parent;
  o->format = format;
  o->size   = size;
  {
    const struct mtp_store *sr = mtp_storage_by_root(parent);
    const struct mtp_obj *par = catalog_find(parent);

    if (sr != NULL)
      {
        o->storage = sr->id;
      }
    else if (par != NULL)
      {
        o->storage = par->storage;
      }
    else
      {
        const struct mtp_store *s0 = mtp_storage_at(0);

        o->storage = s0 ? s0->id : MTP_STORAGE_ID;
      }
  }
  strncpy(o->path, path, sizeof(o->path) - 1);
  o->path[sizeof(o->path) - 1] = '\0';
  strncpy(o->name, name, sizeof(o->name) - 1);
  o->name[sizeof(o->name) - 1] = '\0';
}

/**
 * @brief 扫描目录并保留已有 handle。
 */

#if MTP_FEAT_FULL_CATALOG_SCAN

static void catalog_scan_dir_preserve(const char *dirpath, uint32_t parent,
                                      const struct catalog_ph_snap *old,
                                      int old_n)
{
  struct mtp_walk_slot *stk = MTP_WALK_STACK;
  int sp = 0;

  strncpy(stk[0].path, dirpath, MTP_MAX_PATH - 1);
  stk[0].path[MTP_MAX_PATH - 1] = '\0';
  stk[0].parent = parent;
  stk[0].phase  = 0;
  stk[0].dir    = NULL;
  sp = 1;

  while (sp > 0)
    {
      struct mtp_walk_slot *f = &stk[sp - 1];
      struct dirent *ent;

      if (f->dir == NULL)
        {
          f->dir = opendir(f->path);
          if (f->dir == NULL)
            {
              sp--;
              continue;
            }
        }

      ent = readdir(f->dir);
      if (ent == NULL)
        {
          closedir(f->dir);
          f->dir = NULL;
          sp--;
          continue;
        }

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
          continue;
        }

      if (mtp_path_join(f->path2, MTP_MAX_PATH, f->path, ent->d_name) < 0)
        {
          continue;
        }

      if (catalog_entry_skipped(ent->d_name, f->path2))
        {
          continue;
        }

      {
        struct stat st;
        uint32_t keep;

        if (catalog_stat_file(f->path2, &st) != 0)
          {
            continue;
          }

        keep = catalog_old_handle(old, old_n, f->path2);

        if (S_ISDIR(st.st_mode))
          {
            uint32_t h;

            catalog_add_with_handle(f->parent, PTP_FMT_ASSOCIATION, f->path2,
                                    ent->d_name, 0, keep);
            h = g_ctx.catalog[g_ctx.obj_count - 1].handle;

            if (sp >= MTP_WALK_MAX_DEPTH)
              {
                mtp_trace("catalog scan depth limit %d at %s\n",
                           MTP_WALK_MAX_DEPTH, f->path2);
                continue;
              }

            strncpy(stk[sp].path, f->path2, MTP_MAX_PATH - 1);
            stk[sp].path[MTP_MAX_PATH - 1] = '\0';
            stk[sp].parent = h;
            stk[sp].phase  = 0;
            stk[sp].dir    = NULL;
            sp++;
          }
        else
          {
            catalog_add_with_handle(f->parent, PTP_FMT_UNDEFINED, f->path2,
                                    ent->d_name, (uint64_t)st.st_size, keep);
          }
      }
    }
}

#endif /* MTP_FEAT_FULL_CATALOG_SCAN */

/**
 * @brief 重置 catalog 为存储根。
 */

static void catalog_reset_root(void)
{
  unsigned i;

  g_ctx.obj_count   = 0;
  g_ctx.next_handle = MTP_HANDLE_BASE;

  if (mtp_storage_count() == 0)
    {
      mtp_storage_init();
    }

  for (i = 0; i < mtp_storage_count(); i++)
    {
      const struct mtp_store *s = mtp_storage_at(i);
      struct mtp_obj root;

      if (s == NULL || g_ctx.obj_count >= mtp_max_objects())
        {
          break;
        }

      memset(&root, 0, sizeof(root));
      root.handle  = s->root_handle;
      root.parent  = 0;
      root.storage = s->id;
      root.format  = PTP_FMT_ASSOCIATION;
      strncpy(root.path, s->path, sizeof(root.path) - 1);
      strncpy(root.name, s->label, sizeof(root.name) - 1);
      g_ctx.catalog[g_ctx.obj_count++] = root;
    }

  catalog_meta_reset_all();
}

/**
 * @brief OpenSession 时初始化 catalog。
 */

static void catalog_init_session(void)
{
#if MTP_FEAT_FULL_CATALOG_SCAN
  unsigned i;
#endif

  mtp_storage_ensure_dirs();
  catalog_snap_handles();
  catalog_reset_root();
  g_ctx.catalog_dirty = true;

#if MTP_FEAT_FULL_CATALOG_SCAN
  for (i = 0; i < mtp_storage_count(); i++)
    {
      const struct mtp_store *s = mtp_storage_at(i);

      if (s == NULL)
        {
          continue;
        }

      catalog_scan_dir_preserve(s->path, s->root_handle,
                                g_catalog_ph_snap, g_catalog_ph_snap_n);
    }

  g_ctx.catalog_dirty = false;
#endif

  /* Lazy: 不在 OpenSession 里 opendir。主机还在等 RESPONSE；扫 /mnt/kv
   * 还会 rename persist，曾把握手拖到 160ms，随后 GetObjectHandles memcpy
   * HardFault。第一层目录由 GetObjectHandles / GetNumObjects 再 sync。 */

  mtp_trace("catalog session init %d objects, %u stores\n",
            g_ctx.obj_count, mtp_storage_count());
}

/* Full rebuild (FormatStore, legacy rescan). */

static void catalog_rescan(void)
{
  catalog_init_session();
}

/* Register one uploaded file in catalog (folders use catalog_commit_folder). */

/**
 * @brief SendObject 完成后写入 catalog。
 */

static int catalog_commit_upload(uint32_t handle, uint32_t parent,
                                 const char *path)
{
  struct mtp_obj *o;
  struct stat st;
  const char *base;
  int i;

  if (catalog_stat_file(path, &st) != 0)
    {
      mtp_trace("catalog_commit stat failed errno=%d %s\n", errno, path);
      return -1;
    }

  if (S_ISDIR(st.st_mode))
    {
      return -1;
    }

  base = strrchr(path, '/');
  base = base ? base + 1 : path;

  for (i = 0; i < g_ctx.obj_count; i++)
    {
      if (g_ctx.catalog[i].handle == handle ||
          strcmp(g_ctx.catalog[i].path, path) == 0)
        {
          o = &g_ctx.catalog[i];
          o->handle = handle;
          o->parent = parent;
          o->format = PTP_FMT_UNDEFINED;
          o->size   = (uint64_t)st.st_size;
          strncpy(o->path, path, sizeof(o->path) - 1);
          o->path[sizeof(o->path) - 1] = '\0';
          strncpy(o->name, base, sizeof(o->name) - 1);
          o->name[sizeof(o->name) - 1] = '\0';
          catalog_obj_set_times(i, &st);
          if (handle >= g_ctx.next_handle)
            {
              g_ctx.next_handle = handle + 1;
            }

          mtp_trace("catalog update %s handle=0x%x size=%llu\n",
                     path, handle, (unsigned long long)o->size);
          catalog_mark_dirty();
          if (myvendor_fw_slot_on_commit(path) > 0)
            {
              catalog_mark_dirty();
            }

          return 0;
        }
    }

  if (g_ctx.obj_count >= mtp_max_objects())
    {
      return -1;
    }

  catalog_add_with_handle(parent, PTP_FMT_UNDEFINED, path, base,
                          (uint64_t)st.st_size, handle);
  catalog_obj_set_times(g_ctx.obj_count - 1, &st);
  mtp_trace("catalog add %s handle=0x%x size=%llu\n",
             path, handle, (unsigned long long)st.st_size);
  catalog_mark_dirty();
  if (myvendor_fw_slot_on_commit(path) > 0)
    {
      catalog_mark_dirty();
    }

  return 0;
}

/**
 * @brief 从 catalog 删除一项。
 */

static void catalog_remove(uint32_t handle)
{
  int i;
  int j;

  for (i = 0; i < g_ctx.obj_count; i++)
    {
      if (g_ctx.catalog[i].handle == handle)
        {
          for (j = i; j < g_ctx.obj_count - 1; j++)
            {
              g_ctx.catalog[j] = g_ctx.catalog[j + 1];
              catalog_meta_move(j, j + 1);
            }

          catalog_meta_clear(g_ctx.obj_count - 1);
          g_ctx.obj_count--;
          return;
        }
    }
}

/**
 * @brief 递归删除 catalog 子树。
 */

static void catalog_remove_subtree(uint32_t handle)
{
  int i;

  for (i = g_ctx.obj_count - 1; i >= 0; i--)
    {
      if (g_ctx.catalog[i].parent == handle &&
          g_ctx.catalog[i].handle != handle)
        {
          catalog_remove_subtree(g_ctx.catalog[i].handle);
        }
    }

  catalog_remove(handle);
}

/**
 * @brief 同步文件夹下一级子项到 catalog。
 */

static void catalog_sync_children(uint32_t parent)
{
  struct mtp_obj *par;
  DIR *dir;
  struct dirent *ent;
  char *full;
  int i;
  int n_seen = 0;
  int n_added = 0;
  int n_skip = 0;
  int n_statfail = 0;
  uint32_t cap_drops0 = g_cat_cap_drops;
  uint32_t t0 = mtp_now_ms();

  parent = parent_norm(parent, 0);
  par = catalog_find(parent);
  if (par == NULL)
    {
      mtp_err("sync_children parent=0x%lx: not in catalog\n",
              (unsigned long)parent);
      return;
    }

  full = MTP_SCRATCH_PATH(3);
  dir = opendir(par->path);
  if (dir == NULL)
    {
      mtp_err("sync_children %s: opendir failed errno=%d\n", par->path, errno);
      return;
    }

  while ((ent = readdir(dir)) != NULL)
    {
      struct stat st;
      struct mtp_obj *existing;
      uint32_t keep;
      bool is_dir;
      bool have_stat = false;
      uint64_t size = 0;

      /* SD PIO stat() is hundreds of us per file; a large folder used to
       * block the MTP worker (no EP0) until the host timed out ("can see
       * the device but cannot open").  LittleFS fills d_type, so listing
       * can return handles immediately; size/mtime are filled lazily on
       * GetObjectInfo.  Pump EP0 so Cancel/GetStatus still work.
       */

      mtp_pump_ep0();
      if (g_ctx.cancelled || myvendor_mtp_worker_link_lost_pending())
        {
          closedir(dir);
          return;
        }

      mtp_sched_relax();

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
          continue;
        }

      n_seen++;

      if (mtp_path_join(full, MTP_MAX_PATH, par->path, ent->d_name) < 0)
        {
          n_skip++;
          continue;
        }

      if (catalog_entry_skipped(ent->d_name, full))
        {
          n_skip++;
          continue;
        }

      if (ent->d_type == DT_DIR)
        {
          is_dir = true;
        }
      else if (ent->d_type == DT_REG)
        {
          is_dir = false;
        }
      else if (catalog_stat_file(full, &st) != 0)
        {
          n_statfail++;
          mtp_err("sync_children: stat %s failed errno=%d\n", full, errno);
          continue;
        }
      else
        {
          have_stat = true;
          is_dir = S_ISDIR(st.st_mode);
          size = is_dir ? 0 : (uint64_t)st.st_size;
        }

      n_added++;

      existing = catalog_find_by_path(full);
      if (existing != NULL)
        {
          existing->parent = parent;
          if (upload_inflight_path(full))
            {
              if (g_ctx.upload_size != 0)
                {
                  existing->size = g_ctx.upload_size;
                }
            }
          else if (have_stat)
            {
              existing->size = size;
              catalog_obj_set_times(catalog_slot_index(existing), &st);
            }

          if (is_dir)
            {
              existing->format = PTP_FMT_ASSOCIATION;
              existing->size = 0;
            }

          continue;
        }

      keep = catalog_old_handle(g_catalog_ph_snap, g_catalog_ph_snap_n, full);
      if (is_dir)
        {
          catalog_add_with_handle(parent, PTP_FMT_ASSOCIATION, full,
                                  ent->d_name, 0, keep);
        }
      else
        {
          if (upload_inflight_path(full) && g_ctx.upload_size != 0)
            {
              size = g_ctx.upload_size;
            }

          catalog_add_with_handle(parent, PTP_FMT_UNDEFINED, full,
                                  ent->d_name, size, keep);
        }

      if (have_stat)
        {
          catalog_obj_set_times(g_ctx.obj_count - 1, &st);
        }
    }

  closedir(dir);

  mtp_info("sync_children %s: seen=%d listable=%d skip=%d statfail=%d "
           "%ums obj=%d/%d\n", par->path, n_seen, n_added, n_skip,
           n_statfail, (unsigned)(mtp_now_ms() - t0),
           g_ctx.obj_count, mtp_max_objects());

  /* Cap exhaustion silently truncates a folder listing; always flag it. */

  if (g_cat_cap_drops != cap_drops0)
    {
      mtp_err("sync_children %s: catalog full, dropped %lu entries "
              "(obj=%d/%d)\n", par->path,
              (unsigned long)(g_cat_cap_drops - cap_drops0),
              g_ctx.obj_count, mtp_max_objects());
    }

  {
    int pidx = catalog_slot_index(par);

    if (pidx >= 0 && g_cat_meta != NULL)
      {
        g_cat_meta[pidx].dir_sync_ms = mtp_now_ms();
      }
  }

#if MTP_FEAT_CATALOG_SESSION_CACHE
  if (!g_ctx.catalog_dirty)
    {
      return;
    }
#endif

  for (i = 0; i < g_ctx.obj_count; )
    {
      struct stat st;

      if (g_ctx.catalog[i].parent != parent ||
          g_ctx.catalog[i].handle == parent)
        {
          i++;
          continue;
        }

      if (upload_inflight_path(g_ctx.catalog[i].path))
        {
          i++;
          continue;
        }

      if (catalog_stat_file(g_ctx.catalog[i].path, &st) != 0)
        {
          catalog_remove_subtree(g_ctx.catalog[i].handle);
          continue;
        }

      i++;
    }
}

#if MTP_FEAT_LAZY_CATALOG

/**
 * @brief 驱逐文件夹下 catalog 缓存。
 */

static void catalog_evict_folder_contents(uint32_t folder_handle)
{
  bool removed;

  do
    {
      int i;

      removed = false;
      for (i = g_ctx.obj_count - 1; i >= 0; i--)
        {
      if (g_ctx.catalog[i].parent == folder_handle &&
          g_ctx.catalog[i].handle != folder_handle)
            {
              if (upload_inflight_path(g_ctx.catalog[i].path))
                {
                  continue;
                }

              mtp_trace("catalog evict %s\n", g_ctx.catalog[i].path);
              catalog_remove_subtree(g_ctx.catalog[i].handle);
              removed = true;
              break;
            }
        }
    }
  while (removed);
}

/**
 * @brief 驱逐未打开子文件夹 catalog 缓存。
 */

static void catalog_evict_closed_subfolders(uint32_t list_parent)
{
  int i;
  int before;

  list_parent = parent_norm(list_parent, 0);
  before = g_ctx.obj_count;
  for (i = 0; i < g_ctx.obj_count; i++)
    {
      struct mtp_obj *o = &g_ctx.catalog[i];
      int pidx;

      if (o->parent != list_parent || o->handle == list_parent)
        {
          continue;
        }

      if (o->format != PTP_FMT_ASSOCIATION)
        {
          continue;
        }

      catalog_evict_folder_contents(o->handle);
      pidx = catalog_slot_index(o);
      if (pidx >= 0 && g_cat_meta != NULL)
        {
          g_cat_meta[pidx].dir_sync_ms = 0;
        }
    }

  if (g_ctx.obj_count != before)
    {
      mtp_info("catalog evict parent=0x%lx objs %d -> %d\n",
               (unsigned long)list_parent, before, g_ctx.obj_count);
    }
}

#endif /* MTP_FEAT_LAZY_CATALOG */

/**
 * @brief 按需节流同步子目录。
 */

static void catalog_sync_children_maybe(uint32_t parent)
{
  unsigned i;

  parent = parent_norm(parent, 0);

  if (upload_lfs_busy())
    {
      return;
    }

#if MTP_FEAT_LAZY_CATALOG
  struct mtp_obj *par;

  /* Returning to test/ drops test/sub1/ children so parent can use slots. */
  catalog_evict_closed_subfolders(parent);

  par = catalog_find(parent);

  if (!g_ctx.catalog_dirty && par != NULL)
    {
      int pidx = catalog_slot_index(par);

      if (pidx >= 0 && g_cat_meta != NULL &&
          g_cat_meta[pidx].dir_sync_ms != 0)
        {
          return;
        }
    }

  if (!mtp_is_store_root_handle(parent) && catalog_find(parent) == NULL)
    {
      for (i = 0; i < mtp_storage_count(); i++)
        {
          const struct mtp_store *s = mtp_storage_at(i);

          if (s != NULL)
            {
              catalog_sync_children(s->root_handle);
            }
        }
    }

  catalog_sync_children(parent);
  g_ctx.catalog_dirty = false;
#else
  (void)parent;
  (void)i;
#endif
}

/**
 * @brief GetObjectHandles 前同步父目录。
 */

static void catalog_sync_list_parent(uint32_t parent, uint32_t store_id)
{
  unsigned i;

  if (mtp_is_root_token(parent) && mtp_storage_id_all(store_id))
    {
      for (i = 0; i < mtp_storage_count(); i++)
        {
          const struct mtp_store *s = mtp_storage_at(i);

          if (s != NULL)
            {
              catalog_sync_children_maybe(s->root_handle);
            }
        }

      return;
    }

  catalog_sync_children_maybe(parent_norm(parent, store_id));
}

/**
 * @brief 批量更新路径前缀。
 */

static void catalog_repath_prefix(const char *old_prefix, const char *new_prefix)
{
  size_t oldlen = strlen(old_prefix);
  int i;

  for (i = 0; i < g_ctx.obj_count; i++)
    {
      struct mtp_obj *o = &g_ctx.catalog[i];
      char *npath = MTP_SCRATCH_PATH(3);

      if (strncmp(o->path, old_prefix, oldlen) != 0)
        {
          continue;
        }

      if (o->path[oldlen] != '\0' && o->path[oldlen] != '/')
        {
          continue;
        }

      if (old_prefix[0] == '\0')
        {
          if (mtp_path_concat(npath, MTP_MAX_PATH, new_prefix, o->path) < 0)
            {
              continue;
            }
        }
      else if (o->path[oldlen] == '\0')
        {
          strncpy(npath, new_prefix, MTP_MAX_PATH - 1);
          npath[MTP_MAX_PATH - 1] = '\0';
        }
      else
        {
          if (mtp_path_concat(npath, MTP_MAX_PATH, new_prefix,
                              o->path + oldlen) < 0)
            {
              continue;
            }
        }

      strncpy(o->path, npath, sizeof(o->path) - 1);
      o->path[sizeof(o->path) - 1] = '\0';
      strncpy(o->name, mtp_basename(o->path), sizeof(o->name) - 1);
      o->name[sizeof(o->name) - 1] = '\0';
    }
}

/**
 * @brief SendObjectInfo 建目录后提交 catalog。
 */

static int catalog_commit_folder(uint32_t handle, uint32_t parent,
                                 const char *path)
{
  const char *base = mtp_basename(path);
  int i;

  for (i = 0; i < g_ctx.obj_count; i++)
    {
      if (strcmp(g_ctx.catalog[i].path, path) == 0)
        {
          g_ctx.catalog[i].handle = handle;
          g_ctx.catalog[i].parent = parent;
          g_ctx.catalog[i].format   = PTP_FMT_ASSOCIATION;
          g_ctx.catalog[i].size     = 0;
          strncpy(g_ctx.catalog[i].name, base, sizeof(g_ctx.catalog[i].name) - 1);
          g_ctx.catalog[i].name[sizeof(g_ctx.catalog[i].name) - 1] = '\0';
          if (handle >= g_ctx.next_handle)
            {
              g_ctx.next_handle = handle + 1;
            }

          return 0;
        }
    }

  if (g_ctx.obj_count >= mtp_max_objects())
    {
      return -1;
    }

  catalog_add_with_handle(parent, PTP_FMT_ASSOCIATION, path, base, 0, handle);
  return 0;
}

#if MTP_FEAT_FOLDER_DELETE || MTP_FEAT_COPY_OBJECT || MTP_FEAT_FORMAT_STORE

/**
 * @brief 迭代删除目录树。
 */

static int fs_remove_tree(const char *path)
{
  struct mtp_walk_slot *stk = MTP_WALK_STACK;
  int sp = 0;
  struct stat st;

  if (catalog_stat_file(path, &st) != 0)
    {
      return -1;
    }

  if (!S_ISDIR(st.st_mode))
    {
      return unlink(path);
    }

  strncpy(stk[0].path, path, MTP_MAX_PATH - 1);
  stk[0].path[MTP_MAX_PATH - 1] = '\0';
  stk[0].phase = 0;
  stk[0].dir   = NULL;
  sp = 1;

  while (sp > 0)
    {
      struct mtp_walk_slot *f = &stk[sp - 1];

      if (catalog_stat_file(f->path, &st) != 0)
        {
          return -1;
        }

      if (!S_ISDIR(st.st_mode))
        {
          if (unlink(f->path) != 0)
            {
              return -1;
            }

          sp--;
          continue;
        }

      if (f->phase == 0)
        {
          DIR *dir = opendir(f->path);
          struct dirent *ent;
          int pushed = 0;

          if (dir == NULL)
            {
              return -1;
            }

          while ((ent = readdir(dir)) != NULL)
            {
              if (strcmp(ent->d_name, ".") == 0 ||
                  strcmp(ent->d_name, "..") == 0)
                {
                  continue;
                }

              if (sp >= MTP_WALK_MAX_DEPTH)
                {
                  closedir(dir);
                  mtp_trace("remove depth limit %d at %s\n",
                             MTP_WALK_MAX_DEPTH, f->path);
                  return -1;
                }

              if (mtp_path_join(stk[sp].path, MTP_MAX_PATH, f->path,
                                ent->d_name) < 0)
                {
                  closedir(dir);
                  return -1;
                }

              stk[sp].phase = 0;
              stk[sp].dir   = NULL;
              sp++;
              pushed = 1;
            }

          closedir(dir);

          if (!pushed)
            {
              if (rmdir(f->path) != 0)
                {
                  return -1;
                }

              sp--;
            }
          else
            {
              f->phase = 1;
            }
        }
      else
        {
          if (rmdir(f->path) != 0)
            {
              return -1;
            }

          sp--;
        }
    }

  return 0;
}

#endif

#if MTP_FEAT_COPY_OBJECT

/**
 * @brief 复制单个文件。
 */

static int fs_copy_file(const char *src, const char *dst)
{
  int sfd;
  int dfd;
  ssize_t n;

  sfd = open(src, O_RDONLY);
  if (sfd < 0)
    {
      return -1;
    }

  dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (dfd < 0)
    {
      close(sfd);
      return -1;
    }

  while ((n = read(sfd, g_io_buf, MTP_IO_CHUNK)) > 0)
    {
      mtp_sched_relax();
      if (write(dfd, g_io_buf, (size_t)n) != n)
        {
          close(sfd);
          close(dfd);
          unlink(dst);
          return -1;
        }
    }

  close(sfd);
  close(dfd);
  return (n < 0) ? -1 : 0;
}

/**
 * @brief 迭代复制目录树。
 */

static int fs_copy_tree(const char *src, const char *dst)
{
  struct mtp_walk_slot *stk = MTP_WALK_STACK;
  int sp = 0;
  struct stat st;

  if (catalog_stat_file(src, &st) != 0)
    {
      return -1;
    }

  if (!S_ISDIR(st.st_mode))
    {
      return fs_copy_file(src, dst);
    }

  strncpy(stk[0].path, src, MTP_MAX_PATH - 1);
  stk[0].path[MTP_MAX_PATH - 1] = '\0';
  strncpy(stk[0].path2, dst, MTP_MAX_PATH - 1);
  stk[0].path2[MTP_MAX_PATH - 1] = '\0';
  stk[0].phase = 0;
  stk[0].dir   = NULL;
  sp = 1;

  while (sp > 0)
    {
      struct mtp_walk_slot *f = &stk[sp - 1];

      if (catalog_stat_file(f->path, &st) != 0)
        {
          return -1;
        }

      if (!S_ISDIR(st.st_mode))
        {
          if (fs_copy_file(f->path, f->path2) != 0)
            {
              return -1;
            }

          sp--;
          continue;
        }

      if (f->phase == 0)
        {
          DIR *dir;
          struct dirent *ent;
          int pushed = 0;

          if (mkdir(f->path2, 0755) != 0 && errno != EEXIST)
            {
              return -1;
            }

          dir = opendir(f->path);
          if (dir == NULL)
            {
              return -1;
            }

          while ((ent = readdir(dir)) != NULL)
            {
              if (strcmp(ent->d_name, ".") == 0 ||
                  strcmp(ent->d_name, "..") == 0)
                {
                  continue;
                }

              if (sp >= MTP_WALK_MAX_DEPTH)
                {
                  closedir(dir);
                  mtp_trace("copy depth limit %d at %s\n",
                             MTP_WALK_MAX_DEPTH, f->path);
                  return -1;
                }

              if (mtp_path_join(stk[sp].path, MTP_MAX_PATH, f->path,
                                ent->d_name) < 0 ||
                  mtp_path_join(stk[sp].path2, MTP_MAX_PATH, f->path2,
                                ent->d_name) < 0)
                {
                  closedir(dir);
                  return -1;
                }

              stk[sp].phase = 0;
              stk[sp].dir   = NULL;
              sp++;
              pushed = 1;
            }

          closedir(dir);

          if (!pushed)
            {
              sp--;
            }
          else
            {
              f->phase = 1;
            }
        }
      else
        {
          sp--;
        }
    }

  return 0;
}

#endif /* MTP_FEAT_COPY_OBJECT */

/* ========================================================================== */
/* §6  Catalog filters and filesystem helpers (feature-gated)               */
/* ========================================================================== */

/**
 * @brief 规范化 parent handle。
 */

static bool mtp_is_root_token(uint32_t p)
{
  return p == PTP_OBJECTHANDLE_ROOT || p == PTP_FORMATCODE_ALL;
}

static bool mtp_is_store_root_handle(uint32_t h)
{
  return mtp_storage_by_root(h) != NULL;
}

static uint32_t parent_norm(uint32_t p, uint32_t store_id)
{
  const struct mtp_store *s;

  if (!mtp_is_root_token(p))
    {
      return p;
    }

  s = mtp_storage_by_id(store_id);
  if (s == NULL)
    {
      s = mtp_storage_at(0);
    }

  return s ? s->root_handle : MTP_ROOT_HANDLE;
}

static uint32_t store_id_for_parent(uint32_t parent, uint32_t hint)
{
  const struct mtp_store *s = mtp_storage_by_root(parent);
  const struct mtp_obj *obj;

  if (s != NULL)
    {
      return s->id;
    }

  obj = catalog_find(parent);
  if (obj != NULL && obj->storage != 0)
    {
      return obj->storage;
    }

  s = mtp_storage_by_id(hint);
  if (s != NULL)
    {
      return s->id;
    }

  s = mtp_storage_at(0);
  return s ? s->id : MTP_STORAGE_ID;
}

static uint32_t dest_store_from_cmd(uint32_t store)
{
  const struct mtp_store *s;

  if (store == 0 || mtp_storage_id_all(store))
    {
      s = mtp_storage_at(0);
      return s ? s->id : MTP_STORAGE_ID;
    }

  return store;
}

/**
 * @brief 对象 format 是否匹配过滤器。
 */

static bool fmt_matches(uint16_t obj_fmt, uint32_t fmt_filter)
{
  if (fmt_filter == 0 || fmt_filter == PTP_FORMATCODE_ALL)
    {
      return true;
    }

  return obj_fmt == (uint16_t)fmt_filter;
}

static bool obj_in_store(const struct mtp_obj *o, uint32_t store_id)
{
  if (o == NULL)
    {
      return false;
    }

  if (mtp_storage_id_all(store_id))
    {
      return true;
    }

  return o->storage == store_id;
}

static bool obj_is_listed_child(const struct mtp_obj *o, uint32_t parent,
                                uint32_t store_id, uint32_t fmt_filter)
{
  if (o == NULL || o->handle == parent ||
      !fmt_matches(o->format, fmt_filter) ||
      !obj_in_store(o, store_id))
    {
      return false;
    }

  if (mtp_is_root_token(parent))
    {
      if (mtp_storage_id_all(store_id))
        {
          return mtp_is_store_root_handle(o->parent);
        }

      {
        const struct mtp_store *s = mtp_storage_by_id(store_id);

        return s != NULL && o->parent == s->root_handle;
      }
    }

  return o->parent == parent_norm(parent, store_id);
}

/**
 * @brief 统计父 handle 下子对象数。
 */

static int catalog_count_children(uint32_t parent, uint32_t store_id,
                                  uint32_t fmt_filter)
{
  int i;
  int n = 0;
  int nobj = catalog_nobj();

  for (i = 0; i < nobj; i++)
    {
      if (obj_is_listed_child(&g_ctx.catalog[i], parent, store_id,
                              fmt_filter))
        {
          n++;
        }
    }

  return n;
}

/* ========================================================================== */
/* §7  USB 传输（bulk I/O、ZLP）, container read/write                    */
/* ========================================================================== */

static void usb_close(void);

/**
 * @brief 打开 /dev/mtp/ep* 端点。
 */

static int usb_open(void)
{
  g_ctx.ep0    = open(MTP_EP0_PATH, O_RDWR);
  g_ctx.ep_in  = open(MTP_EP_IN, O_RDWR);
  g_ctx.ep_out = open(MTP_EP_OUT, O_RDWR);
  g_ctx.ep_int = open(MTP_EP_INT, O_RDWR);

  if (g_ctx.ep0 < 0 || g_ctx.ep_in < 0 || g_ctx.ep_out < 0)
    {
      syslog(LOG_ERR, "mtp_simple: open ep failed (%d %d %d)\n",
             g_ctx.ep0, g_ctx.ep_in, g_ctx.ep_out);
      return -1;
    }

  if (g_ctx.ep_int < 0)
    {
      syslog(LOG_WARNING,
             "mtp_simple: open %s failed (%d); host may not auto-refresh\n",
             MTP_EP_INT, errno);
    }

  g_usb_eps_open = true;
  return 0;
}

/**
 * @brief 关闭 USB MTP 端点。
 */

static void usb_close(void)
{
  upload_ram_drop();
  if (g_ctx.upload_fd >= 0)
    {
      close(g_ctx.upload_fd);
      g_ctx.upload_fd = -1;
    }

  if (g_ctx.ep0 >= 0)
    {
      close(g_ctx.ep0);
    }

  if (g_ctx.ep_in >= 0)
    {
      close(g_ctx.ep_in);
    }

  if (g_ctx.ep_out >= 0)
    {
      close(g_ctx.ep_out);
    }

  if (g_ctx.ep_int >= 0)
    {
      close(g_ctx.ep_int);
    }

  g_ctx.ep0 = g_ctx.ep_in = g_ctx.ep_out =   g_ctx.ep_int = -1;
  g_usb_eps_open = false;
  usb_in_reset();
}

/* Dummy pointer for write(fd, buf, 0): sf32lb USB stack rejects NULL buf. */

#define MTP_IN_ZLP_BUF  ((void *)(uintptr_t)0xfee1dead)

/**
 * @brief 经 SRAM bounce 写出最多一包。
 */

static ssize_t usb_ep_write_pkt(int fd, const void *src, size_t len)
{
  if (len == 0)
    {
      return write(fd, MTP_IN_ZLP_BUF, 0);
    }

  if (len > sizeof(g_usb_pkt))
    {
      len = sizeof(g_usb_pkt);
    }

  memcpy(g_usb_pkt, src, len);
  return write(fd, g_usb_pkt, len);
}

/**
 * @brief 经 SRAM bounce 读入最多一包。
 */

static ssize_t usb_ep_read_pkt(int fd, void *dest, size_t want)
{
  ssize_t n;

  if (want > sizeof(g_usb_pkt))
    {
      want = sizeof(g_usb_pkt);
    }

  if (g_out_stash_len > 0 && fd == g_ctx.ep_out)
    {
      n = (ssize_t)g_out_stash_len;
      if ((size_t)n > want)
        {
          n = (ssize_t)want;
        }

      if (dest != NULL)
        {
          memcpy(dest, g_out_stash, (size_t)n);
        }

      if ((size_t)n < g_out_stash_len)
        {
          memmove(g_out_stash, g_out_stash + n, g_out_stash_len - (size_t)n);
          g_out_stash_len -= (size_t)n;
        }
      else
        {
          g_out_stash_len = 0;
        }

      return n;
    }

  n = read(fd, g_usb_pkt, want);
  if (n > 0 && dest != NULL)
    {
      memcpy(dest, g_usb_pkt, (size_t)n);
    }

  return n;
}

/**
 * @brief 从 bulk OUT 读入 dest（经 SRAM bounce，可一次吃掉整段已完成 URB）。
 */
static ssize_t usb_out_read_burst(int fd, void *dest, size_t want)
{
  ssize_t n;
  size_t take;

  if (want == 0)
    {
      return 0;
    }

  if (g_out_stash_len > 0 && fd == g_ctx.ep_out)
    {
      take = g_out_stash_len < want ? g_out_stash_len : want;
      if (dest != NULL)
        {
          memcpy(dest, g_out_stash, take);
        }

      if (take < g_out_stash_len)
        {
          memmove(g_out_stash, g_out_stash + take, g_out_stash_len - take);
          g_out_stash_len -= take;
        }
      else
        {
          g_out_stash_len = 0;
        }

      return (ssize_t)take;
    }

  take = want > sizeof(g_usb_rx) ? sizeof(g_usb_rx) : want;
  n = read(fd, g_usb_rx, take);
  if (n > 0 && dest != NULL)
    {
      memcpy(dest, g_usb_rx, (size_t)n);
    }

  return n;
}

/* ========================================================================== */
/* §8  异步 MTP 事件 (interrupt IN + EP0 GETEVENT)                         */
/* ========================================================================== */

/**
 * @brief 打包异步 MTP 事件容器。
 */

static void mtp_event_pack(uint8_t *buf, uint16_t *outlen,
                           uint16_t code, uint32_t param0)
{
  struct ptp_header *h = (struct ptp_header *)buf;

  h->len  = (uint32_t)(sizeof(*h) + sizeof(param0));
  h->type = PTP_CONTAINER_EVENT;
  h->code = code;
  h->tid  = 0;
  memcpy(buf + sizeof(*h), &param0, sizeof(param0));
  *outlen = (uint16_t)h->len;
}

/**
 * @brief 经 interrupt 端点发送 MTP 事件。
 */

static int mtp_send_event(uint16_t code, uint32_t param0)
{
  ssize_t n;

  if (g_ctx.session == 0)
    {
      return -1;
    }

  mtp_event_pack(g_ctx.pending_event, &g_ctx.pending_event_len,
                 code, param0);
  g_ctx.have_pending_event = true;

  if (g_ctx.ep_int < 0)
    {
      mtp_trace("event 0x%x dropped (no %s)\n", code, MTP_EP_INT);
      return -1;
    }

  n = usb_ep_write_pkt(g_ctx.ep_int, g_ctx.pending_event,
                       g_ctx.pending_event_len);
  if (n != (ssize_t)g_ctx.pending_event_len)
    {
      mtp_err("event 0x%x ep_int write %d errno=%d\n",
              code, (int)n, errno);
      return -1;
    }

  return 0;
}

/**
 * @brief 发送 ObjectInfoChanged 事件。
 */

static void mtp_event_object_info_changed(uint32_t handle)
{
#if MTP_FEAT_EXTRA_EVENTS
  mtp_send_event(PTP_EVENTCODE_OBJECTINFOCHANGED, handle);
#else
  (void)handle;
#endif
}

#if MTP_FEAT_FOLDERS

/**
 * @brief ObjectInfo 是否表示文件夹。
 */

static bool object_info_is_folder(const uint8_t *payload, size_t len,
                                  uint16_t format)
{
  uint16_t assoc = 0;

  if (format == PTP_FMT_ASSOCIATION)
    {
      return true;
    }

  /* Some hosts use Undefined + AssociationType for folders only; do not treat
   * Text/Executable/etc. with a stray AssociationType as a directory. */
  if (format == PTP_FMT_UNDEFINED && len >= 44)
    {
      memcpy(&assoc, payload + 42, 2);
      if (assoc == PTP_ASSOCIATIONTYPE_FOLDER)
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief 发布文件夹新增事件。
 */

static int folder_publish(uint32_t handle, uint32_t parent, const char *path)
{
  if (catalog_commit_folder(handle, parent, path) < 0)
    {
      return -1;
    }

  g_ctx.upload_folder_committed = true;
  mtp_info("mkdir handle=0x%x (%s)\n", handle, mtp_basename(path));
  mtp_send_event(PTP_EVENTCODE_OBJECTADDED, handle);
  return 0;
}

#endif /* MTP_FEAT_FOLDERS */

/**
 * @brief 发布文件新增事件。
 */

static int file_publish(uint32_t handle, uint32_t parent, const char *path,
                        uint16_t format, uint64_t size)
{
  const char *base = strrchr(path, '/');

  base = base ? base + 1 : path;

  if (g_ctx.obj_count >= mtp_max_objects())
    {
      return -1;
    }

  catalog_add_with_handle(parent,
                          format ? format : PTP_FMT_UNDEFINED,
                          path, base, size, handle);
  return 0;
}

/**
 * @brief bulk IN 发送零长度包。
 */

static int usb_send_in_zlp(void)
{
  if (write(g_ctx.ep_in, MTP_IN_ZLP_BUF, 0) < 0)
    {
      return -1;
    }

  return 0;
}

/**
 * @brief 提交已组好的 IN 包（len 为 0 时发 ZLP；否则 1..64）。
 */

static int usb_in_submit_raw(const void *buf, size_t len)
{
  const uint8_t *p = buf;
  size_t sent = 0;

  if (len == 0)
    {
      return usb_send_in_zlp();
    }

  while (sent < len)
    {
      ssize_t n;

      if (myvendor_mtp_worker_poll_abort() ||
          myvendor_mtp_worker_link_lost_pending())
        {
          myvendor_mtp_worker_link_lost();
          return -1;
        }

      mtp_pump_ep0();
      if (g_ctx.cancelled)
        {
          return -1;
        }

      mtp_sched_relax();
      n = usb_ep_write_pkt(g_ctx.ep_in, p + sent, len - sent);
      if (n <= 0)
        {
          myvendor_mtp_worker_link_lost();
          return -1;
        }

      sent += (size_t)n;
    }

  return 0;
}

/**
 * @brief 提交上一拍已满的 64 字节包（后面还有数据，中间允许满包）。
 */

static int usb_in_flush_pending(void)
{
  if (!g_in_pend_full)
    {
      return 0;
    }

  if (usb_in_submit_raw(g_in_pend, MTP_MAX_PACKET_FS) < 0)
    {
      return -1;
    }

  g_in_pend_full = false;
  return 0;
}

/**
 * @brief bulk IN 写出：满 64 先挂起，确认不是相位末尾再发出。
 */

static int usb_write_bulk(const void *buf, size_t len)
{
  const uint8_t *p = buf;

  while (len > 0)
    {
      size_t room = MTP_MAX_PACKET_FS - g_in_acc_used;
      size_t n = (len < room) ? len : room;

      memcpy(g_in_acc + g_in_acc_used, p, n);
      g_in_acc_used += n;
      g_in_phase_len += n;
      p += n;
      len -= n;

      if (g_in_acc_used == MTP_MAX_PACKET_FS)
        {
          if (usb_in_flush_pending() < 0)
            {
              usb_in_reset();
              return -1;
            }

          memcpy(g_in_pend, g_in_acc, MTP_MAX_PACKET_FS);
          g_in_pend_full = true;
          g_in_acc_used = 0;
        }
    }

  return 0;
}

/**
 * @brief 结束一次 DATA/RESPONSE：最后一包必须是短包（满 64 则拆成 63+1）。
 *
 * 本板 write(ZLP) 不可靠：主机 URB 已按 PTP 长度收满后，多余 ZLP 会被
 * 下一笔 IN 吃掉，gvfs 空等约 20s。短包结束相位，不再发 ZLP。
 */

static void usb_in_end(void)
{
  if (g_in_acc_used > 0)
    {
      (void)usb_in_flush_pending();
      (void)usb_in_submit_raw(g_in_acc, g_in_acc_used);
    }
  else if (g_in_pend_full)
    {
      (void)usb_in_submit_raw(g_in_pend, MTP_MAX_PACKET_FS - 1);
      (void)usb_in_submit_raw(g_in_pend + (MTP_MAX_PACKET_FS - 1), 1);
    }

  usb_in_reset();
}

/**
 * @brief poll 等待 fd 可读/可写。
 */

static int usb_wait_fd(int fd, short events, int timeout_ms)
{
  struct pollfd pfd;

  pfd.fd     = fd;
  pfd.events = events;

  for (; ; )
    {
      int pr;

      if (myvendor_mtp_worker_poll_abort() ||
          myvendor_mtp_worker_link_lost_pending())
        {
          return -1;
        }

      pr = poll(&pfd, 1, timeout_ms);
      if (pr < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -1;
        }

      if (pr == 0)
        {
          return 0;
        }

      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
          return -1;
        }

      if (pfd.revents & events)
        {
          return 1;
        }
    }
}

/**
 * @brief bulk 读满指定字节。
 */

#define MTP_USB_IO_SLICE_MS  100
#define MTP_USB_ZLP_SKIP_MAX 8

static int usb_read_exact(int fd, void *buf, size_t len)
{
  uint8_t *p = buf;
  size_t got = 0;
  int zlp_skip = 0;

  while (got < len)
    {
      struct pollfd pfds[2];
      int nfds = 0;
      int out_i;
      int pr;
      ssize_t n;

      if (myvendor_mtp_worker_poll_abort() ||
          myvendor_mtp_worker_link_lost_pending() || g_ctx.cancelled)
        {
          myvendor_mtp_worker_link_lost();
          return -1;
        }

      if (g_mtp_usb_hot)
        {
          myvendor_watchdog_work_beat();
        }
      else
        {
          mtp_sched_relax();
        }

      /* Never block solely on bulk OUT: the host (gvfs/libmtp/Explorer)
       * polls GET_DEVICE_STATUS on EP0 throughout SendObject.  A blocking
       * read() here starved EP0, the host aborted after one 4 KiB URB, and
       * the upload failed with "read failed errno=0 remain=...".
       */

      if (g_ctx.ep0 >= 0)
        {
          pfds[nfds].fd     = g_ctx.ep0;
          pfds[nfds].events = POLLIN | POLLPRI | POLLERR | POLLHUP;
          nfds++;
        }

      out_i = nfds;
      pfds[nfds].fd     = fd;
      pfds[nfds].events = POLLIN | POLLERR | POLLHUP;
      nfds++;

      pr = poll(pfds, nfds, MTP_USB_IO_SLICE_MS);
      if (pr < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          myvendor_mtp_worker_link_lost();
          return -1;
        }

      if (pr == 0)
        {
          continue;
        }

      if (g_ctx.ep0 >= 0)
        {
          if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            {
              myvendor_mtp_worker_link_lost();
              return -1;
            }

          if (pfds[0].revents & POLLIN)
            {
              handle_ep0();
              if (g_ctx.cancelled ||
                  myvendor_mtp_worker_link_lost_pending())
                {
                  myvendor_mtp_worker_link_lost();
                  return -1;
                }
            }
        }

      if (pfds[out_i].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
          myvendor_mtp_worker_link_lost();
          return -1;
        }

      if (!(pfds[out_i].revents & POLLIN))
        {
          continue;
        }

      n = usb_out_read_burst(fd, p + got, len - got);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          myvendor_mtp_worker_link_lost();
          return -1;
        }

      if (n == 0)
        {
          /* Leading ZLP between URBs is benign; a mid-container ZLP is a
           * truncated transfer.  Bound consecutive leading ZLPs so a host
           * abort (read()==0 forever) cannot spin until the link drops.
           */

          if (got != 0)
            {
              myvendor_mtp_worker_link_lost();
              return -1;
            }

          if (++zlp_skip > MTP_USB_ZLP_SKIP_MAX)
            {
              mtp_info("too many OUT ZLPs, treating as hangup\n");
              myvendor_mtp_worker_link_lost();
              return -1;
            }

          mtp_trace("skip leading OUT ZLP\n");
          continue;
        }

      zlp_skip = 0;
      got += (size_t)n;
    }

  return 0;
}

/* usb_read_container_ex() return codes. */

#define MTP_CONTAINER_OK    0   /* container read into buf/outlen           */
#define MTP_CONTAINER_ERR  (-1) /* genuine link loss (errno set by driver)  */
#define MTP_CONTAINER_ZLP   1   /* leading zero-length packet, no container */
#define MTP_CONTAINER_SOFT  2   /* benign desync/stale header: drain+re-poll*/

/**
 * @brief 读 PTP 容器头+载荷。
 */

static int usb_read_container_ex(int fd, uint8_t *buf, size_t bufsz,
                                 size_t *outlen, bool poll_managed)
{
  struct ptp_header *h = (struct ptp_header *)buf;

  if (poll_managed)
    {
      ssize_t n = usb_ep_read_pkt(fd, buf, sizeof(*h));
      if (n < 0)
        {
          if (errno == EINTR)
            {
              return MTP_CONTAINER_ZLP;   /* nothing consumed; re-poll */
            }

          myvendor_mtp_worker_link_lost();
          return MTP_CONTAINER_ERR;
        }

      if (n == 0)
        {
          return MTP_CONTAINER_ZLP;       /* leading ZLP: re-poll, keep EP0 */
        }

      if ((size_t)n < sizeof(*h))
        {
          /* Header split across packets: real data is arriving, finish it. */

          if (usb_read_exact(fd, buf + n, sizeof(*h) - (size_t)n) < 0)
            {
              return MTP_CONTAINER_ERR;
            }
        }
    }
  else if (usb_read_exact(fd, buf, sizeof(*h)) < 0)
    {
      return MTP_CONTAINER_ERR;
    }

  if (h->len < sizeof(*h) || h->len > bufsz)
    {
      /* The bytes we just read are not a valid PTP command header.  This is a
       * desync (leftover/partial container re-delivered from the driver's
       * un-flushed OUT queue after a reconnect, or the tail of an aborted
       * data phase), NOT a genuine disconnect.  errno is whatever a prior
       * libc call left behind, so it is meaningless here.  In the poll-managed
       * command loop, report it as SOFT so the caller drains and re-polls
       * instead of tearing the link down (which caused the reconnect storm).
       * In a data phase (poll_managed == false) a bad header is fatal.
       */

      return poll_managed ? MTP_CONTAINER_SOFT : MTP_CONTAINER_ERR;
    }

  if (h->len > sizeof(*h))
    {
      if (usb_read_exact(fd, buf + sizeof(*h), h->len - sizeof(*h)) < 0)
        {
          return MTP_CONTAINER_ERR;
        }
    }

  *outlen = h->len;
  return MTP_CONTAINER_OK;
}

static int usb_read_container(int fd, uint8_t *buf, size_t bufsz, size_t *outlen)
{
  return usb_read_container_ex(fd, buf, bufsz, outlen, false);
}

/* Host sends ZLP when a data phase length is a multiple of 64 (FS). */

/**
 * @brief DATA 后按需排空 OUT ZLP。
 */

static void usb_drain_out_zlp_if_needed(uint32_t container_len)
{
  ssize_t n;
  int wr;

  if (container_len == 0 || (container_len % MTP_MAX_PACKET_FS) != 0)
    {
      return;
    }

  wr = usb_wait_fd(g_ctx.ep_out, POLLIN, MTP_ZLP_DRAIN_MS);
  if (wr <= 0)
    {
      mtp_trace("OUT ZLP skip (phase len=%u wait=%d)\n", container_len, wr);
      return;
    }

  n = usb_ep_read_pkt(g_ctx.ep_out, g_usb_pkt, sizeof(g_usb_pkt));
  if (n == 0)
    {
      mtp_trace("drained OUT ZLP (phase len=%u)\n", container_len);
    }
  else if (n < 0)
    {
      mtp_trace("drain OUT ZLP read err=%d\n", errno);
    }
  else
    {
      /* Host often pipelines SendObject right after SendObjectInfo.  A
       * length that happens to be a multiple of 64 is not proof the next
       * packet is a ZLP — keep it as lookahead instead of dropping the
       * next command (PC progress then sits at 0). */
      memcpy(g_out_stash, g_usb_pkt, (size_t)n);
      g_out_stash_len = (size_t)n;
      mtp_info("ZLP drain kept %dB lookahead (phase len=%u)\n",
               (int)n, container_len);
    }
}

/* ========================================================================== */
/* §9  PTP response / data container helpers                                  */
/* ========================================================================== */

/**
 * @brief 发送 PTP Response 容器。
 */

static int send_response(uint32_t tid, uint16_t code,
                         const uint32_t *params, int nparam)
{
  uint8_t *buf = MTP_SCRATCH_D64;
  struct ptp_header *h = (struct ptp_header *)buf;
  size_t len = sizeof(*h) + (size_t)nparam * sizeof(uint32_t);
  int i;
  int ret;

  /* Drop aborted DATA remainder so RESPONSE is a fresh IN phase. */
  usb_in_reset();

  h->len  = (uint32_t)len;
  h->type = PTP_CONTAINER_RESPONSE;
  h->code = code;
  h->tid  = tid;

  for (i = 0; i < nparam; i++)
    {
      memcpy(buf + sizeof(*h) + i * 4, &params[i], 4);
    }

  ret = usb_write_bulk(buf, len);
  if (ret < 0)
    {
      mtp_trace("send_response 0x%x failed\n", code);
      return ret;
    }

  usb_in_end();
  return 0;
}

/**
 * @brief 发送 PTP Data 容器。
 */

static int send_data(uint32_t tid, uint16_t code,
                     const void *payload, size_t payload_len)
{
  uint8_t hdr[12];

  ((struct ptp_header *)hdr)->len  = (uint32_t)(sizeof(hdr) + payload_len);
  ((struct ptp_header *)hdr)->type = PTP_CONTAINER_DATA;
  ((struct ptp_header *)hdr)->code = code;
  ((struct ptp_header *)hdr)->tid  = tid;

  if (usb_write_bulk(hdr, sizeof(hdr)) < 0)
    {
      return -1;
    }

  if (payload_len > 0 && usb_write_bulk(payload, payload_len) < 0)
    {
      return -1;
    }

  usb_in_end();
  return 0;
}

/**
 * @brief 解析 Command 容器。
 */

static int parse_cmd(const uint8_t *buf, size_t len, struct ptp_cmd *cmd)
{
  memset(cmd, 0, sizeof(*cmd));
  if (len < sizeof(struct ptp_header))
    {
      return -1;
    }

  memcpy(&cmd->hdr, buf, sizeof(cmd->hdr));
  cmd->nparam = (int)((len - sizeof(cmd->hdr)) / 4);
  if (cmd->nparam > 5)
    {
      cmd->nparam = 5;
    }

  memcpy(cmd->params, buf + sizeof(cmd->hdr), (size_t)cmd->nparam * 4);
  return 0;
}

/* ========================================================================== */
/* §10 Dataset packers — binary blobs inside DATA containers                  */
/* ========================================================================== */

/**
 * @brief 打包 GetDeviceInfo 数据集。
 */

static int pack_device_info(uint8_t *buf, int bufsz)
{
  static const uint16_t devprops[] =
  {
    MTP_PROPERTY_PERCEIVEDDEVICETYPE,
  };

  static const uint16_t events[] =
  {
    PTP_EVENTCODE_OBJECTADDED,
    PTP_EVENTCODE_OBJECTREMOVED,
    PTP_EVENTCODE_STOREADDED,
#if MTP_FEAT_EXTRA_EVENTS
    PTP_EVENTCODE_STOREREMOVED,
    PTP_EVENTCODE_OBJECTINFOCHANGED,
#endif
  };

  static const uint16_t fmts[] =
  {
    PTP_FMT_ASSOCIATION,
    PTP_FMT_UNDEFINED,
  };

  int off = 0;
  uint16_t ver = 100;
  uint32_t ext_id = 6;
  uint16_t ext_ver = 100;
  uint16_t mode = 0;
  uint32_t count;
  int i;
  int n;

  if (bufsz < 256)
    {
      return -1;
    }

  memcpy(buf + off, &ver, 2);
  off += 2;
  memcpy(buf + off, &ext_id, 4);
  off += 4;
  memcpy(buf + off, &ext_ver, 2);
  off += 2;

  n = ptp_str_put(buf + off, bufsz - off, mtp_name(MTP_NAME_MTP_EXTENSION));
  if (n < 0)
    {
      return -1;
    }

  off += n;
  memcpy(buf + off, &mode, 2);
  off += 2;

  count = sizeof(g_mtp_ops) / sizeof(g_mtp_ops[0]);
  memcpy(buf + off, &count, 4);
  off += 4;
  for (i = 0; i < (int)count; i++)
    {
      memcpy(buf + off, &g_mtp_ops[i], 2);
      off += 2;
    }

  count = sizeof(events) / sizeof(events[0]);
  memcpy(buf + off, &count, 4);
  off += 4;
  for (i = 0; i < (int)count; i++)
    {
      memcpy(buf + off, &events[i], 2);
      off += 2;
    }

  count = sizeof(devprops) / sizeof(devprops[0]);
  memcpy(buf + off, &count, 4);
  off += 4;
  for (i = 0; i < (int)count; i++)
    {
      memcpy(buf + off, &devprops[i], 2);
      off += 2;
    }

  count = 0;
  memcpy(buf + off, &count, 4);
  off += 4;

  count = sizeof(fmts) / sizeof(fmts[0]);
  memcpy(buf + off, &count, 4);
  off += 4;
  for (i = 0; i < (int)count; i++)
    {
      memcpy(buf + off, &fmts[i], 2);
      off += 2;
    }

  n = ptp_str_put(buf + off, bufsz - off, mtp_name(MTP_NAME_MANUFACTURER));
  if (n < 0)
    {
      return -1;
    }

  off += n;
  n = ptp_str_put(buf + off, bufsz - off, mtp_name(MTP_NAME_MODEL));
  if (n < 0)
    {
      return -1;
    }

  off += n;
  n = ptp_str_put(buf + off, bufsz - off, mtp_name(MTP_NAME_VERSION));
  if (n < 0)
    {
      return -1;
    }

  off += n;
  n = ptp_str_put(buf + off, bufsz - off, mtp_name(MTP_NAME_SERIAL));
  if (n < 0)
    {
      return -1;
    }

  off += n;
  return off;
}

static uint64_t mtp_mtd_window_bytes(const char *mtddev)
{
  const char *cands[4];
  struct mtd_geometry_s geo;
  int i;
  int n = 0;
  int fd;

  if (mtddev != NULL && mtddev[0] != '\0')
    {
      cands[n++] = mtddev;
    }

  cands[n++] = "/dev/sd0";
  cands[n++] = "/dev/nand0";
  cands[n] = NULL;

  for (i = 0; cands[i] != NULL; i++)
    {
      fd = open(cands[i], O_RDONLY);
      if (fd < 0)
        {
          continue;
        }

      memset(&geo, 0, sizeof(geo));
      if (ioctl(fd, MTDIOC_GEOMETRY, &geo) == 0 && geo.neraseblocks > 0 &&
          geo.erasesize > 0)
        {
          close(fd);
          return (uint64_t)geo.neraseblocks * (uint64_t)geo.erasesize;
        }

      close(fd);
    }

  return 64ull * 1024ull * 1024ull;
}

/**
 * @brief 打包 GetStorageInfo 数据集。
 */

static int pack_storage_info(uint8_t *buf, int bufsz,
                             const struct mtp_store *s)
{
  uint16_t stype = PTP_STORAGETYPE_REMOVABLERAM;
  uint16_t fstype = PTP_FILESYSTEMTYPE_HIERARCHICAL;
  uint16_t access = PTP_STORAGEACCESS_RWD;
  uint64_t cap = 64ull * 1024ull * 1024ull;
  uint64_t freeb = cap;
  uint32_t freeobj = 0xffff;
  int off = 0;
  int n;
  struct mtp_storage_cache *cache = NULL;
  const char *desc;
  const char *label;

  if (s == NULL)
    {
      return -1;
    }

  if (g_storage_cache != NULL && s->index < MTP_STORAGE_MAX)
    {
      cache = &g_storage_cache[s->index];
    }

  if (cache != NULL && cache->cap != 0)
    {
      cap   = cache->cap;
      freeb = cache->freeb;
    }
  else
    {
      cap   = mtp_mtd_window_bytes(s->mtddev);
      freeb = cap;
      if (cache != NULL)
        {
          cache->cap     = cap;
          cache->freeb   = freeb;
          cache->stat_ms = 1;
        }
    }

  desc = s->desc[0] != '\0' ? s->desc : mtp_name(MTP_NAME_STORAGE_DESC);
  label = s->label[0] != '\0' ? s->label : mtp_name(MTP_NAME_VOLUME_LABEL);

  memcpy(buf + off, &stype, 2);
  off += 2;
  memcpy(buf + off, &fstype, 2);
  off += 2;
  memcpy(buf + off, &access, 2);
  off += 2;
  memcpy(buf + off, &cap, 8);
  off += 8;
  memcpy(buf + off, &freeb, 8);
  off += 8;
  memcpy(buf + off, &freeobj, 4);
  off += 4;

  n = ptp_str_put(buf + off, bufsz - off, desc);
  if (n < 0)
    {
      return -1;
    }

  off += n;
  n = ptp_str_put(buf + off, bufsz - off, label);
  if (n < 0)
    {
      return -1;
    }

  off += n;
  return off;
}

/**
 * @brief 打包设备属性描述。
 */

static int pack_device_prop_desc(uint8_t *buf, int bufsz, uint16_t prop_code,
                                 uint16_t data_type, uint8_t get_set,
                                 uint32_t value)
{
  int off = 0;

  if (bufsz < 15)
    {
      return -1;
    }

  memcpy(buf + off, &prop_code, 2);
  off += 2;
  memcpy(buf + off, &data_type, 2);
  off += 2;
  buf[off++] = get_set;
  memcpy(buf + off, &value, 4);
  off += 4;
  memcpy(buf + off, &value, 4);
  off += 4;
  buf[off++] = PTP_FORMFLAGS_NONE;
  return off;
}

/**
 * @brief 处理 GetDevicePropDesc。
 */

static void handle_get_device_prop_desc(const struct ptp_cmd *cmd)
{
  uint8_t buf[32];
  int n;
  uint16_t prop = (uint16_t)cmd->params[0];

  if (prop != MTP_PROPERTY_PERCEIVEDDEVICETYPE)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_PROP_NOTSUPPORTED, NULL, 0);
      return;
    }

  n = pack_device_prop_desc(buf, sizeof(buf),
                            MTP_PROPERTY_PERCEIVEDDEVICETYPE,
                            PTP_DATATYPE_UINT32,
                            PTP_PROPGETSET_GETONLY,
                            g_mtp_perceived_type);
  if (n < 0 || send_data(cmd->hdr.tid, cmd->hdr.code, buf, (size_t)n) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 处理 GetDevicePropValue。
 */

static void handle_get_device_prop_value(const struct ptp_cmd *cmd)
{
  uint8_t buf[4];
  uint16_t prop = (uint16_t)cmd->params[0];
  uint32_t val;

  if (prop != MTP_PROPERTY_PERCEIVEDDEVICETYPE)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_PROP_NOTSUPPORTED, NULL, 0);
      return;
    }

  val = g_mtp_perceived_type;
  memcpy(buf, &val, sizeof(val));
  if (send_data(cmd->hdr.tid, cmd->hdr.code, buf, sizeof(buf)) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 打包 GetObjectInfo 数据集。
 */

static int pack_object_info(const struct mtp_obj *obj, uint8_t *buf, int bufsz)
{
  uint16_t prot = 0;
  uint32_t thumb_sz = 0;
  uint16_t thumb_fmt = 0;
  uint32_t dim = 0;
  uint16_t assoc = 0;
  uint32_t assoc_desc = 0;
  uint32_t seq = 0;
  uint32_t objsize;
  int off = 0;
  int n;

  uint32_t store_id = obj->storage ? obj->storage : MTP_STORAGE_ID;
  uint32_t parent_id = obj->parent;

  if (obj->format == PTP_FMT_ASSOCIATION)
    {
      assoc = PTP_ASSOCIATIONTYPE_FOLDER;
      /* Spec allows 0xFFFFFFFF ("undefined") for folders; gvfs/Nautilus
       * adds that as a real 4 GiB per directory (two stores → ~8.6 GB). */
      objsize = 0;
    }
  else
    {
      objsize = (uint32_t)obj->size;
    }

  /* Windows expects 0xffffffff for objects directly under the store root. */

  if (mtp_is_store_root_handle(parent_id))
    {
      parent_id = PTP_FORMATCODE_ALL;
    }

  if (bufsz < 128)
    {
      return -1;
    }

  memcpy(buf + off, &store_id, 4);
  off += 4;
  memcpy(buf + off, &obj->format, 2);
  off += 2;
  memcpy(buf + off, &prot, 2);
  off += 2;
  memcpy(buf + off, &objsize, 4);
  off += 4;
  memcpy(buf + off, &thumb_fmt, 2);
  off += 2;
  memcpy(buf + off, &thumb_sz, 4);
  off += 4;
  memcpy(buf + off, &dim, 4);
  off += 4;
  memcpy(buf + off, &dim, 4);
  off += 4;
  memcpy(buf + off, &dim, 4);
  off += 4;
  memcpy(buf + off, &dim, 4);
  off += 4;
  memcpy(buf + off, &dim, 4);
  off += 4;
  memcpy(buf + off, &parent_id, 4);
  off += 4;
  memcpy(buf + off, &assoc, 2);
  off += 2;
  memcpy(buf + off, &assoc_desc, 4);
  off += 4;
  memcpy(buf + off, &seq, 4);
  off += 4;

  n = ptp_str_put(buf + off, bufsz - off, obj->name);
  if (n < 0)
    {
      return -1;
    }

  off += n;

  /* Empty capture/modification dates: LFS mtime is not reliable. */

  n = ptp_str_put(buf + off, bufsz - off, "");
  if (n < 0)
    {
      return -1;
    }

  off += n;
  n = ptp_str_put(buf + off, bufsz - off, "");
  if (n < 0)
    {
      return -1;
    }

  off += n;
  n = ptp_str_put(buf + off, bufsz - off, "");
  if (n < 0)
    {
      return -1;
    }

  off += n;
  return off;
}

#if MTP_FEAT_OBJECT_PROPLIST

static const uint16_t g_obj_props[] =
{
  MTP_OBJPROP_OBJECTSIZE,
  MTP_OBJPROP_FILENAME,
};

/**
 * @brief 返回对象支持的属性列表。
 */

static bool obj_prop_supported(uint16_t prop)
{
  unsigned i;

  for (i = 0; i < sizeof(g_obj_props) / sizeof(g_obj_props[0]); i++)
    {
      if (g_obj_props[i] == prop)
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief 打包对象属性描述。
 */

static int pack_obj_prop_desc(uint8_t *buf, int bufsz, uint16_t prop,
                              uint16_t dtype)
{
  uint32_t group = MTP_PROP_GROUPCODE_GENERAL;
  uint32_t def_u32 = 0;
  int off = 0;

  if (bufsz < 16)
    {
      return -1;
    }

  memcpy(buf + off, &prop, 2);
  off += 2;
  memcpy(buf + off, &dtype, 2);
  off += 2;
  buf[off++] = (prop == MTP_OBJPROP_FILENAME && MTP_FEAT_SET_OBJECT_PROP) ?
               PTP_PROPGETSET_GETSET : PTP_PROPGETSET_GETONLY;
  if (dtype == PTP_DATATYPE_STRING)
    {
      buf[off++] = 0;
    }
  else
    {
      memcpy(buf + off, &def_u32, 4);
      off += 4;
    }

  memcpy(buf + off, &group, 4);
  off += 4;
  buf[off++] = PTP_FORMFLAGS_NONE;
  return off;
}

/**
 * @brief 读取对象属性值。
 */

static int obj_prop_value(struct mtp_obj *obj, uint16_t prop,
                          uint8_t *buf, int bufsz, uint16_t *out_dtype)
{
  if (obj == NULL)
    {
      return -1;
    }

  if (prop == MTP_OBJPROP_OBJECTSIZE)
    {
      catalog_refresh_file_stat(obj);
    }

  switch (prop)
    {
    case MTP_OBJPROP_OBJECTSIZE:
      {
        uint32_t sz = (obj->format == PTP_FMT_ASSOCIATION) ?
                      0u : (uint32_t)obj->size;

        *out_dtype = PTP_DATATYPE_UINT32;
        if (bufsz < 4)
          {
            return -1;
          }

        memcpy(buf, &sz, 4);
        return 4;
      }

    case MTP_OBJPROP_FILENAME:
      *out_dtype = PTP_DATATYPE_STRING;
      return ptp_str_put(buf, bufsz, obj->name);

    default:
      return -1;
    }
}

/**
 * @brief 处理 GetObjectPropsSupported。
 */

static void handle_get_object_props_supported(const struct ptp_cmd *cmd)
{
  uint8_t buf[32];
  uint32_t count = sizeof(g_obj_props) / sizeof(g_obj_props[0]);
  int off = 0;
  unsigned i;

  memcpy(buf + off, &count, 4);
  off += 4;
  for (i = 0; i < count; i++)
    {
      memcpy(buf + off, &g_obj_props[i], 2);
      off += 2;
    }

  if (send_data(cmd->hdr.tid, cmd->hdr.code, buf, (size_t)off) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 处理 GetObjectPropDesc。
 */

static void handle_get_object_prop_desc(const struct ptp_cmd *cmd)
{
  uint8_t buf[32];
  uint16_t prop = (uint16_t)cmd->params[0];
  uint16_t dtype;
  int n;

  if (!obj_prop_supported(prop))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_PROP_NOTSUPPORTED, NULL, 0);
      return;
    }

  if (prop == MTP_OBJPROP_OBJECTSIZE)
    {
      dtype = PTP_DATATYPE_UINT32;
    }
  else
    {
      dtype = PTP_DATATYPE_STRING;
    }

  n = pack_obj_prop_desc(buf, sizeof(buf), prop, dtype);
  if (n < 0 || send_data(cmd->hdr.tid, cmd->hdr.code, buf, (size_t)n) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 处理 GetObjectPropValue。
 */

static void handle_get_object_prop_value(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  uint8_t *buf = MTP_SCRATCH_D512;
  uint16_t prop = (uint16_t)cmd->params[1];
  uint16_t dtype;
  int n;

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  if (!obj_prop_supported(prop))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_PROP_NOTSUPPORTED, NULL, 0);
      return;
    }

  n = obj_prop_value(obj, prop, buf, (int)sizeof(g_mtp_scratch->data512),
                     &dtype);
  if (n < 0 || send_data(cmd->hdr.tid, cmd->hdr.code, buf, (size_t)n) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 处理 GetObjectPropList。
 *
 * Ubuntu libmtp 会用 handle=0xFFFFFFFF 拉全部对象属性；若只回
 * INVALID_OBJ_HANDLE（无 DATA 阶段）容易把 bulk 会话打乱，后续
 * GetObjectHandles / 多选拷贝失败。懒 catalog 下 ALL 只返回已缓存
 * 对象，并始终带 DATA 阶段。
 */

static bool obj_matches_proplist(const struct mtp_obj *o, uint32_t handle,
                                 uint32_t depth, uint32_t fmt_filter)
{
  if (o == NULL || !fmt_matches(o->format, fmt_filter))
    {
      return false;
    }

  if (handle == 0 || handle == PTP_FORMATCODE_ALL)
    {
      return !mtp_is_store_root_handle(o->handle);
    }

  if (o->handle == handle)
    {
      return true;
    }

  if (depth == 0)
    {
      return false;
    }

  if (o->parent == handle)
    {
      return true;
    }

  if (depth != PTP_FORMATCODE_ALL)
    {
      return false;
    }

  {
    uint32_t p = o->parent;
    int guard;

    for (guard = 0; guard < MTP_WALK_MAX_DEPTH; guard++)
      {
        if (p == handle)
          {
            return true;
          }

        if (mtp_is_store_root_handle(p) || p == 0)
          {
            return false;
          }

        {
          struct mtp_obj *par = catalog_find(p);

          if (par == NULL)
            {
              return false;
            }

          p = par->parent;
        }
      }
  }

  return false;
}

static int pack_obj_prop_list_entry(struct mtp_obj *obj, uint32_t prop_filter,
                                    uint8_t *buf, int bufsz, uint32_t *count)
{
  int off = 0;
  unsigned i;

  catalog_refresh_file_stat(obj);

  for (i = 0; i < sizeof(g_obj_props) / sizeof(g_obj_props[0]); i++)
    {
      uint16_t prop = g_obj_props[i];
      uint16_t dtype;
      int vlen;

      if (prop_filter != PTP_FORMATCODE_ALL && prop_filter != prop)
        {
          continue;
        }

      if (off + 8 > bufsz)
        {
          return -1;
        }

      vlen = obj_prop_value(obj, prop, buf + off + 8, bufsz - off - 8, &dtype);
      if (vlen < 0)
        {
          continue;
        }

      memcpy(buf + off, &obj->handle, 4);
      off += 4;
      memcpy(buf + off, &prop, 2);
      off += 2;
      memcpy(buf + off, &dtype, 2);
      off += 2;
      off += vlen;
      (*count)++;
    }

  return off;
}

static int send_object_prop_list(uint32_t tid, uint16_t code, uint32_t handle,
                                 uint32_t depth, uint32_t fmt_filter,
                                 uint32_t prop_filter)
{
  uint8_t hdr[sizeof(struct ptp_header)];
  uint8_t *out = g_io_buf;
  uint32_t count = 0;
  size_t payload = 4;
  int used;
  int i;

  for (i = 0; i < g_ctx.obj_count; i++)
    {
      uint32_t nprop = 0;
      int n;

      if (!obj_matches_proplist(&g_ctx.catalog[i], handle, depth, fmt_filter))
        {
          continue;
        }

      n = pack_obj_prop_list_entry(&g_ctx.catalog[i], prop_filter,
                                   MTP_SCRATCH_D512,
                                   (int)sizeof(g_mtp_scratch->data512),
                                   &nprop);
      if (n > 0)
        {
          payload += (size_t)n;
          count += nprop;
        }
    }

  ((struct ptp_header *)hdr)->len  = (uint32_t)(sizeof(hdr) + payload);
  ((struct ptp_header *)hdr)->type = PTP_CONTAINER_DATA;
  ((struct ptp_header *)hdr)->code = code;
  ((struct ptp_header *)hdr)->tid  = tid;

  if (usb_write_bulk(hdr, sizeof(hdr)) < 0)
    {
      return -1;
    }

  memcpy(out, &count, 4);
  used = 4;

  for (i = 0; i < g_ctx.obj_count; i++)
    {
      uint32_t nprop = 0;
      int n;

      if (!obj_matches_proplist(&g_ctx.catalog[i], handle, depth, fmt_filter))
        {
          continue;
        }

      n = pack_obj_prop_list_entry(&g_ctx.catalog[i], prop_filter,
                                   MTP_SCRATCH_D512,
                                   (int)sizeof(g_mtp_scratch->data512),
                                   &nprop);
      if (n <= 0)
        {
          continue;
        }

      if (used + n > MTP_IO_CHUNK)
        {
          if (usb_write_bulk(out, (size_t)used) < 0)
            {
              return -1;
            }

          used = 0;
        }

      memcpy(out + used, MTP_SCRATCH_D512, (size_t)n);
      used += n;
    }

  if (used > 0 && usb_write_bulk(out, (size_t)used) < 0)
    {
      return -1;
    }

  usb_in_end();
  return 0;
}

static void handle_get_object_prop_list(const struct ptp_cmd *cmd)
{
  uint32_t handle = cmd->params[0];
  uint32_t fmt_filter = cmd->params[1];
  uint32_t prop_filter = cmd->params[2];
  uint32_t depth = cmd->params[4];
  bool all = (handle == 0 || handle == PTP_FORMATCODE_ALL);
  uint32_t t0 = mtp_now_ms();

  /* Spec uses 0xFFFFFFFF = all properties; some hosts send 0. */

  if (prop_filter == 0)
    {
      prop_filter = PTP_FORMATCODE_ALL;
    }

  if (!all)
    {
      if (catalog_find(handle) == NULL)
        {
          send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE,
                        NULL, 0);
          return;
        }

      if (depth != 0)
        {
          catalog_sync_list_parent(handle, 0);
        }
    }

  if (send_object_prop_list(cmd->hdr.tid, cmd->hdr.code, handle, depth,
                            fmt_filter, prop_filter) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_info("GetObjectPropList h=0x%lx depth=%lu fmt=0x%lx prop=0x%lx %ums\n",
           (unsigned long)handle, (unsigned long)depth,
           (unsigned long)fmt_filter, (unsigned long)prop_filter,
           (unsigned)(mtp_now_ms() - t0));
}

#endif /* MTP_FEAT_OBJECT_PROPLIST */

/* ========================================================================== */
/* §11  上传路径辅助 — SendObjectInfo parse and LFS path build          */
/* ========================================================================== */

/**
 * @brief 解析 SendObjectInfo 数据集。
 */

static int parse_send_object_info(const uint8_t *payload, size_t len,
                                  char *name, int namesz,
                                  uint32_t *parent, uint64_t *size,
                                  uint16_t *format)
{
  const uint8_t *p = payload;
  size_t str_off;
  uint32_t sz32;

  if (len < OBJINFO_FIXED + 1)
    {
      return -1;
    }

  memcpy(format, p + 4, 2);
  memcpy(parent, p + OBJINFO_PARENT, 4);
  memcpy(&sz32, p + 8, 4);
  *size = sz32;

  str_off = object_info_name_offset(p, len);
  return ptp_str_get(p + str_off, (int)(len - str_off), name, namesz);
}

/**
 * @brief 校验上传文件名合法。
 */

static bool upload_name_ok(const char *name)
{
  if (name[0] == '\0' || strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0)
    {
      return false;
    }

  return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

/**
 * @brief 构造 SendObject 目标 LFS 路径。
 */

static int build_upload_path(uint32_t parent, uint32_t store_id,
                             const char *name, char *out, int outsz)
{
  const struct mtp_obj *par;

  parent = parent_norm(parent, store_id);
  par = catalog_find(parent);
  if (par == NULL)
    {
      return -1;
    }

  if (outsz <= 0)
    {
      return -1;
    }

  return mtp_path_join(out, (size_t)outsz, par->path, name) < 0 ? -1 : 0;
}

/* Absolute LFS path for a child name under a catalog parent handle. */

/**
 * @brief 构造父路径下的子路径。
 */

static int build_child_path(uint32_t parent, uint32_t store_id,
                            const char *name, char *out, int outsz)
{
  return build_upload_path(parent, store_id, name, out, outsz);
}

/* ========================================================================== */
/* §12 Core command handlers — always compiled                                */
/* ========================================================================== */

/* --- §12.1 Session ------------------------------------------------------ */

/**
 * @brief 处理 GetDeviceInfo。
 */

static void handle_get_device_info(const struct ptp_cmd *cmd)
{
  uint8_t *buf = MTP_SCRATCH_D512;
  int n = pack_device_info(buf, sizeof(g_mtp_scratch->data512));

  if (n < 0 || send_data(cmd->hdr.tid, cmd->hdr.code, buf, (size_t)n) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 处理 OpenSession。
 */

static void handle_open_session(const struct ptp_cmd *cmd)
{
  if (g_ctx.session != 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_SESSIONALREADYOPEN, NULL, 0);
      return;
    }

  g_ctx.session = cmd->params[0];
  if (g_ctx.session == 0)
    {
      g_ctx.session = 1;
    }

  /* 先回 OK，再铺 catalog。主机超时重发 OpenSession / 抢发下一命令时，
   * 设备还在 LittleFS+migrate 里，bulk IN memcpy 会 HardFault。 */
  myvendor_mtp_lfs_set_session(true);
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  catalog_init_session();

  /* Do not emit StoreAdded here.  The volumes already exist in DeviceInfo /
   * GetStorageIDs; a burst of interrupt-IN events right after OpenSession
   * raced the host's next bulk-OUT command and hardfaulted mtp_simple
   * (factory: /mnt/lfs + /mnt/kv).  StoreAdded is for hot-plug during an
   * already-open session (see FormatStore). */

  mtp_info("session open (%u stores)\n", mtp_storage_count());
}

/**
 * @brief 处理 CloseSession。
 */

static void handle_close_session(const struct ptp_cmd *cmd)
{
  g_ctx.session = 0;
  myvendor_mtp_lfs_set_session(false);
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_info("session close\n");
}

/* --- §12.2 Storage ------------------------------------------------------ */

/**
 * @brief 处理 GetStorageIds。
 */

static void handle_get_storage_ids(const struct ptp_cmd *cmd)
{
  uint8_t buf[4 + 4 * MTP_STORAGE_MAX];
  uint32_t n = mtp_storage_count();
  unsigned i;

  if (n > MTP_STORAGE_MAX)
    {
      n = MTP_STORAGE_MAX;
    }

  memcpy(buf, &n, 4);
  for (i = 0; i < n; i++)
    {
      const struct mtp_store *s = mtp_storage_at(i);
      uint32_t id = s ? s->id : (MTP_STORAGE_ID + i * 0x00010000u);

      memcpy(buf + 4 + i * 4, &id, 4);
    }

  send_data(cmd->hdr.tid, cmd->hdr.code, buf, 4 + n * 4);
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 处理 GetStorageInfo。
 */

static void handle_get_storage_info(const struct ptp_cmd *cmd)
{
  uint8_t *buf = MTP_SCRATCH_D128;
  const struct mtp_store *s = mtp_storage_by_id(cmd->params[0]);
  int n;
  uint32_t t0 = mtp_now_ms();

  if (s == NULL)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID, NULL, 0);
      return;
    }

  n = pack_storage_info(buf, sizeof(g_mtp_scratch->data128), s);
  if (n < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_data(cmd->hdr.tid, cmd->hdr.code, buf, (size_t)n);
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_info("GetStorageInfo id=0x%lx %s %ums\n",
           (unsigned long)cmd->params[0], s->path,
           (unsigned)(mtp_now_ms() - t0));
}

/* --- §12.3 Browse (handles / object info) ------------------------------- */

/**
 * @brief 处理 GetNumObjects。
 */

static void handle_get_num_objects(const struct ptp_cmd *cmd)
{
  uint32_t n;
  uint32_t store_id = cmd->params[0];
  uint32_t fmt_filter = cmd->params[1];
  uint32_t parent = cmd->params[2];
  uint32_t p[1];
  uint32_t t0 = mtp_now_ms();

  if (!mtp_storage_id_all(store_id) && !mtp_storage_id_known(store_id))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID, NULL, 0);
      return;
    }

  catalog_sync_list_parent(parent, store_id);
  n = (uint32_t)catalog_count_children(parent, store_id, fmt_filter);
  p[0] = n;
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, p, 1);
  mtp_info("GetNumObjects parent=0x%lx store=0x%lx n=%u %ums\n",
           (unsigned long)parent, (unsigned long)store_id, (unsigned)n,
           (unsigned)(mtp_now_ms() - t0));
}

/**
 * @brief 处理 GetObjectHandles。
 */

static void handle_get_object_handles(const struct ptp_cmd *cmd)
{
  uint8_t hdr[sizeof(struct ptp_header)];
  uint32_t chunk[16];
  uint32_t store_id = cmd->params[0];
  uint32_t fmt_filter = cmd->params[1];
  uint32_t parent = cmd->params[2];
  uint32_t count = 0;
  uint32_t t0 = mtp_now_ms();
  uint32_t t_sync;
  size_t payload;
  struct mtp_obj *par;
  int i;
  int nobj;
  unsigned nc;

  if (!mtp_storage_id_all(store_id) && !mtp_storage_id_known(store_id))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID, NULL, 0);
      return;
    }

  catalog_sync_list_parent(parent, store_id);
  t_sync = mtp_now_ms() - t0;
  nobj = catalog_nobj();
  for (i = 0; i < nobj; i++)
    {
      if (obj_is_listed_child(&g_ctx.catalog[i], parent, store_id,
                              fmt_filter))
        {
          count++;
        }
    }

  payload = 4 + (size_t)count * 4;
  par = catalog_find(parent_norm(parent, store_id));
  mtp_info("GetObjectHandles parent=0x%lx store=0x%lx n=%u payload=%u "
           "sync=%ums path=%s\n",
           (unsigned long)parent, (unsigned long)store_id,
           (unsigned)count, (unsigned)payload, (unsigned)t_sync,
           par != NULL ? par->path : "-");

  /* 不把 handle 表堆进 PSRAM 再一次性 send_data：usbdev_fs memcpy
   * 用户缓冲时，PSRAM 指针会在 SRAM_END(0x20080000) BusFault。经 SRAM
   * 小块 usb_write_bulk（已有 g_in_acc bounce）。 */
  ((struct ptp_header *)hdr)->len  = (uint32_t)(sizeof(hdr) + payload);
  ((struct ptp_header *)hdr)->type = PTP_CONTAINER_DATA;
  ((struct ptp_header *)hdr)->code = cmd->hdr.code;
  ((struct ptp_header *)hdr)->tid  = cmd->hdr.tid;
  if (usb_write_bulk(hdr, sizeof(hdr)) < 0 ||
      usb_write_bulk(&count, sizeof(count)) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  nc = 0;
  for (i = 0; i < nobj; i++)
    {
      if (!obj_is_listed_child(&g_ctx.catalog[i], parent, store_id,
                               fmt_filter))
        {
          continue;
        }

      chunk[nc++] = g_ctx.catalog[i].handle;
      if (nc == (unsigned)(sizeof(chunk) / sizeof(chunk[0])))
        {
          if (usb_write_bulk(chunk, nc * sizeof(uint32_t)) < 0)
            {
              send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
              return;
            }

          nc = 0;
        }
    }

  if (nc > 0 && usb_write_bulk(chunk, nc * sizeof(uint32_t)) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  usb_in_end();
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_info("GetObjectHandles send=%ums rem64=%u\n",
           (unsigned)(mtp_now_ms() - t0 - t_sync),
           (unsigned)((12u + payload) % 64u));
}

/**
 * @brief 处理 GetObjectInfo。
 */

static void handle_get_object_info(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  uint8_t *buf = MTP_SCRATCH_D512;
  int n;
  uint32_t t0 = mtp_now_ms();
  uint32_t t_stat;
  static uint32_t s_n;
  static uint32_t s_ms;

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  /* Listing skipped per-file stat() so GetObjectHandles can return before
   * the host times out.  Fill size/mtime here, one file at a time.
   */

  catalog_refresh_file_stat(obj);
  t_stat = mtp_now_ms() - t0;

  n = pack_object_info(obj, buf, (int)sizeof(g_mtp_scratch->data512));
  if (n < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_data(cmd->hdr.tid, cmd->hdr.code, buf, (size_t)n);
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);

  {
    uint32_t t_end = mtp_now_ms();

    s_n++;
    s_ms += t_end - t0;
    if (t_stat >= 8 || (t_end - t0) >= 15 || (s_n % 10u) == 1u)
      {
        mtp_info("GetObjectInfo #%u %s sz=%llu dlen=%d stat=%ums send=%ums "
                 "(sum %ums)\n",
                 (unsigned)s_n, obj->name,
                 (unsigned long long)obj->size, n,
                 (unsigned)t_stat,
                 (unsigned)(t_end - t0 - t_stat),
                 (unsigned)s_ms);
      }
  }
}

/**
 * @brief 处理 GetObject。
 */

/* SD block driver diagnostics (sf32lb_sdio.c); no-op weak fallback if the
 * board is not on SD so this stays link-safe on the NAND build.
 */

void __attribute__((weak)) sf32lb_sd_diag_dump(const char *tag)
{
  (void)tag;
}

static void handle_get_object(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  int fd;
  uint8_t hdr[12];
  uint64_t remain;
  uint64_t total;
  ssize_t nread;
  struct stat st;

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL || obj->format == PTP_FMT_ASSOCIATION)
    {
      mtp_err("GetObject h=0x%lx: %s\n", (unsigned long)cmd->params[0],
              obj == NULL ? "handle not in catalog" : "is a folder");
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  fd = open(obj->path, O_RDONLY);
  if (fd < 0)
    {
      mtp_err("GetObject %s: open failed errno=%d\n", obj->path, errno);
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
    {
      total = (uint64_t)st.st_size;
    }
  else
    {
      off_t end = lseek(fd, 0, SEEK_END);

      /* LittleFS: open ok, fstat ENOENT/mode 0 — size from lseek or catalog. */

      if (end >= 0)
        {
          total = (uint64_t)end;
          (void)lseek(fd, 0, SEEK_SET);
        }
      else if (obj->size > 0)
        {
          total = obj->size;
          (void)lseek(fd, 0, SEEK_SET);
        }
      else
        {
          mtp_err("GetObject %s: fstat rc/mode bad errno=%d mode=0%lo\n",
                  obj->path, errno, (unsigned long)st.st_mode);
          close(fd);
          send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
          return;
        }
    }
  if (total > 0xffffffffu)
    {
      total = 0xffffffffu;
    }

  mtp_trace("GetObject h=0x%lx %s size=%llu\n", (unsigned long)cmd->params[0],
            obj->path, (unsigned long long)total);

  myvendor_mtp_activity_start(MYVENDOR_MTP_OP_DOWNLOAD, obj_display_name(obj), total);

  ((struct ptp_header *)hdr)->len  = (uint32_t)(sizeof(hdr) + total);
  ((struct ptp_header *)hdr)->type = PTP_CONTAINER_DATA;
  ((struct ptp_header *)hdr)->code = cmd->hdr.code;
  ((struct ptp_header *)hdr)->tid  = cmd->hdr.tid;

  if (usb_write_bulk(hdr, sizeof(hdr)) < 0)
    {
      mtp_err("GetObject %s: header write failed errno=%d\n", obj->path, errno);
      close(fd);
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  remain = total;
  while (remain > 0 && !g_ctx.cancelled)
    {
      size_t want = remain > MTP_IO_CHUNK ? MTP_IO_CHUNK : (size_t)remain;

      mtp_sched_relax();
      nread = read(fd, g_io_buf, want);
      if (nread <= 0)
        {
          mtp_err("GetObject %s: read failed off=%llu want=%zu "
                  "nread=%zd errno=%d\n", obj->path,
                  (unsigned long long)(total - remain), want, nread, errno);
          break;
        }

      mtp_pump_ep0();
      if (g_ctx.cancelled)
        {
          break;
        }

      if (usb_write_bulk(g_io_buf, (size_t)nread) < 0)
        {
          mtp_err("GetObject %s: bulk write failed off=%llu errno=%d\n",
                  obj->path, (unsigned long long)(total - remain), errno);
          break;
        }

      remain -= (uint64_t)nread;
      mtp_activity_progress_step(total - remain);
    }

  close(fd);

  if (remain == 0)
    {
      usb_in_end();
      mtp_info("download %s (%llu bytes)\n", obj->name,
               (unsigned long long)total);
    }
  else if (!g_ctx.cancelled)
    {
      mtp_err("GetObject %s: aborted, %llu/%llu bytes left\n", obj->path,
              (unsigned long long)remain, (unsigned long long)total);
      sf32lb_sd_diag_dump("GetObject-abort");
    }

  myvendor_mtp_activity_finish();
  send_response(cmd->hdr.tid,
                remain == 0 ? PTP_RESPONSE_OK : PTP_RESPONSE_GEN_ERROR,
                NULL, 0);
}

#if MTP_FEAT_GET_PARTIAL_OBJECT

/**
 * @brief 处理 GetPartialObject。
 */

static void handle_get_partial_object(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  int fd;
  uint8_t hdr[12];
  uint32_t offset = cmd->params[1];
  uint32_t maxbytes = cmd->params[2];
  uint32_t to_send;
  uint32_t sent = 0;
  uint32_t resp_p[1];
  uint64_t fsize;
  struct stat st;
  ssize_t nread;

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL || obj->format == PTP_FMT_ASSOCIATION)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  fd = open(obj->path, O_RDONLY);
  if (fd < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
      close(fd);
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  fsize = (uint64_t)st.st_size;
  if (offset >= fsize)
    {
      to_send = 0;
    }
  else
    {
      uint64_t avail = fsize - offset;

      to_send = (uint32_t)(maxbytes > avail ? avail : maxbytes);
    }

  ((struct ptp_header *)hdr)->len  = (uint32_t)(sizeof(hdr) + to_send);
  ((struct ptp_header *)hdr)->type = PTP_CONTAINER_DATA;
  ((struct ptp_header *)hdr)->code = cmd->hdr.code;
  ((struct ptp_header *)hdr)->tid  = cmd->hdr.tid;

  if (usb_write_bulk(hdr, sizeof(hdr)) < 0)
    {
      close(fd);
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  if (to_send > 0)
    {
      size_t remain;

      if (lseek(fd, (off_t)offset, SEEK_SET) < 0)
        {
          close(fd);
          send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
          return;
        }

      remain = to_send;
      while (remain > 0 && !g_ctx.cancelled)
        {
          size_t want = remain > MTP_IO_CHUNK ? MTP_IO_CHUNK : remain;

          mtp_sched_relax();
          nread = read(fd, g_io_buf, want);
          if (nread <= 0)
            {
              close(fd);
              send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
              return;
            }

          if (usb_write_bulk(g_io_buf, (size_t)nread) < 0)
            {
              close(fd);
              send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
              return;
            }

          mtp_pump_ep0();
          if (g_ctx.cancelled)
            {
              close(fd);
              send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
              return;
            }

          sent += (uint32_t)nread;
          remain -= (size_t)nread;
        }
    }

  close(fd);

  usb_in_end();

  resp_p[0] = sent;
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, resp_p, 1);
}

#endif /* MTP_FEAT_GET_PARTIAL_OBJECT */

/**
 * @brief 处理 DeleteObject。
 */

static void handle_delete_object(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  uint32_t handle = cmd->params[0];

  obj = catalog_find(handle);
  if (obj == NULL || mtp_is_store_root_handle(obj->handle))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  myvendor_mtp_activity_start(MYVENDOR_MTP_OP_DELETE, obj_display_name(obj), 0);

  if (obj->format == PTP_FMT_ASSOCIATION)
    {
#if MTP_FEAT_FOLDER_DELETE
      if (fs_remove_tree(obj->path) != 0)
        {
          send_response(cmd->hdr.tid, PTP_RESPONSE_PARTIAL_DELETION, NULL, 0);
          return;
        }

      catalog_remove_subtree(handle);
#else
      send_response(cmd->hdr.tid, PTP_RESPONSE_OP_NOT_SUPPORTED, NULL, 0);
      return;
#endif
    }
  else if (unlink(obj->path) != 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }
  else
    {
      catalog_remove(handle);
    }

  mtp_info("delete %s\n", obj->name);
  catalog_mark_dirty();
  myvendor_mtp_activity_finish();
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_send_event(PTP_EVENTCODE_OBJECTREMOVED, handle);
}

/**
 * @brief 中止进行中的上传。
 */

static void upload_fd_close(void)
{
  if (g_ctx.upload_fd >= 0)
    {
      close(g_ctx.upload_fd);
      g_ctx.upload_fd = -1;
    }
}

static void upload_ram_free(void)
{
  board_mem_free(g_upload_ram);
  g_upload_ram = NULL;
  g_upload_ram_len = 0;
  g_upload_ram_off = 0;
  g_upload_ram_t0 = 0;
}

/** @brief 释放上传缓冲；**有 flush 线程在写就投递给它自己释放**。
 *
 * @details 以前这里无条件 free，而调用方里有 `usb_close()`（worker 线程）——
 *          它会在 flush 线程还停在 `upload_ram_flush_slice()` 里往这块内存写的时候
 *          就把缓冲释放掉：use-after-free 的形状（review 2026-09-18 指出）。
 *          现在置 cancel + pending，由 flush 线程退出前释放 —— 那一刻它是唯一写者。
 *          连带的好处：等待方不必再"等到 flush 真停"，投递本身是安全的。
 */
static void upload_ram_drop(void)
{
  if (g_upload_flushing)
    {
      g_upload_flush_cancel = true;
      g_upload_drop_pend = true;
      return;
    }

  upload_ram_free();
}

static void upload_abort(uint32_t tid, uint16_t code)
{
  uint32_t t0 = mtp_now_ms();

  g_upload_flush_cancel = true;
  while (g_upload_flushing)
    {
      myvendor_watchdog_work_beat();
      usleep(2000);

      /* 有超时：不能把 worker 无限期挂在一个卡住的 flush 上。
       * 超时**不是**不安全 —— upload_ram_drop() 已经是投递式的，flush 线程
       * 退出前会自己释放（见它的注释）。 */
      if (mtp_now_ms() - t0 >= MTP_UPLOAD_FLUSH_WAIT_MS)
        {
          /* 用 syslog 而不是 mtp_info：后者在本配置下整个被编译掉
           * （`strings mtp_simple.c.o` 里没有任何 mtp_info 串），而这一条正是
           * 事后要看的 —— "abort 等了 5 s 还没等到 flush 收尾"。 */
          syslog(LOG_WARNING,
                 "mtp: SendObject flush still running after %u ms, free deferred\n",
                 (unsigned)MTP_UPLOAD_FLUSH_WAIT_MS);
          break;
        }
    }

  g_upload_need_commit = false;
  upload_ram_drop();
  upload_fd_close();

  if (MTP_UPLOAD_PATH[0] != '\0')
    {
      if (g_ctx.upload_is_folder && !g_ctx.upload_folder_committed)
        {
          rmdir(MTP_UPLOAD_PATH);
        }
      else if (!g_ctx.upload_is_folder)
        {
          unlink(MTP_UPLOAD_PATH);
        }

      MTP_UPLOAD_PATH[0] = '\0';
    }

  if (g_ctx.pending_handle != 0)
    {
      catalog_remove(g_ctx.pending_handle);
      g_ctx.pending_handle = 0;
    }

  g_ctx.upload_is_folder         = false;
  g_ctx.upload_folder_committed  = false;
  g_ctx.upload_file_committed    = false;
  myvendor_mtp_activity_finish();
  send_response(tid, code, NULL, 0);
}

/**
 * @brief 处理 SendObjectInfo 数据阶段。
 */

static void handle_send_object_info_data(const struct ptp_cmd *cmd,
                                         const uint8_t *buf, size_t len)
{
  const uint8_t *payload;
  size_t plen;
  char *fname = MTP_SCRATCH_NAME(0);
  uint32_t parent;
  uint32_t hparent;
  uint64_t size;
  uint16_t format;
  uint32_t store = dest_store_from_cmd(cmd->params[0]);
  uint32_t resp_p[3];
  const struct ptp_header *dh = (const struct ptp_header *)buf;

  upload_flush_wait_prior();
  MTP_UPLOAD_PATH[0] = '\0';
  g_ctx.upload_folder_committed = false;
  g_ctx.upload_file_committed   = false;

  if (!mtp_storage_id_known(store))
    {
      upload_abort(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID);
      return;
    }

  if (len <= sizeof(*dh) || dh->type != PTP_CONTAINER_DATA ||
      dh->code != cmd->hdr.code || dh->tid != cmd->hdr.tid)
    {
      mtp_trace("SendObjectInfo bad data: len=%u type=%u code=0x%x "
                 "want 0x%x tid=%u want %u\n",
                 (unsigned)len, dh->type, dh->code, cmd->hdr.code,
                 dh->tid, cmd->hdr.tid);
      upload_abort(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR);
      return;
    }

  payload = buf + sizeof(*dh);
  plen    = len - sizeof(*dh);

  if (parse_send_object_info(payload, plen, fname, MTP_MAX_NAME, &parent,
                             &size, &format) < 0 ||
      !upload_name_ok(fname))
    {
      mtp_trace("SendObjectInfo parse failed (plen=%u)\n", (unsigned)plen);
      upload_abort(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR);
      return;
    }

  if (cmd->params[1] != 0)
    {
      hparent = parent_norm(cmd->params[1], store);
    }
  else if (cmd->params[2] != 0)
    {
      hparent = parent_norm(cmd->params[2], store);
    }
  else
    {
      hparent = parent_norm(parent, store);
    }

  if (!mtp_is_store_root_handle(hparent) && catalog_find(hparent) == NULL)
    {
      catalog_sync_children_maybe(hparent);
      if (catalog_find(hparent) == NULL)
        {
          mtp_trace("SendObjectInfo bad parent 0x%x\n", hparent);
          upload_abort(cmd->hdr.tid, PTP_RESPONSE_INVALIDPARENT);
          return;
        }
    }

  if (store_id_for_parent(hparent, store) != store)
    {
      mtp_trace("SendObjectInfo cross-store parent=0x%x store=0x%x\n",
                 hparent, store);
      upload_abort(cmd->hdr.tid, PTP_RESPONSE_ACCESS_DENIED);
      return;
    }

  if (build_upload_path(hparent, store, fname, MTP_UPLOAD_PATH,
                        sizeof(g_mtp_scratch->path[0])) < 0)
    {
      mtp_trace("SendObjectInfo bad path parent=0x%x name=%s\n",
                 hparent, fname);
      upload_abort(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR);
      return;
    }

  g_ctx.upload_size    = size;
  g_ctx.upload_got     = 0;
  g_ctx.upload_fd      = -1;
  g_ctx.upload_parent  = hparent;
  g_ctx.upload_format  = format;
  g_ctx.upload_is_folder = false;
  g_ctx.pending_handle = g_ctx.next_handle++;

#if !MTP_FEAT_FOLDERS
  if (format == PTP_FMT_ASSOCIATION)
    {
      mtp_trace("SendObjectInfo folder rejected (MTP_FEAT_FOLDERS=0)\n");
      upload_abort(cmd->hdr.tid, PTP_RESPONSE_OP_NOT_SUPPORTED);
      return;
    }
#endif

#if MTP_FEAT_FOLDERS
  if (object_info_is_folder(payload, plen, format))
    {
      mtp_sched_relax();
      myvendor_mtp_activity_start(MYVENDOR_MTP_OP_MKDIR, fname, 0);
      if (mkdir(MTP_UPLOAD_PATH, 0755) != 0 && errno != EEXIST)
        {
          mtp_trace("SendObjectInfo mkdir failed errno=%d %s\n",
                     errno, MTP_UPLOAD_PATH);
          upload_abort(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR);
          return;
        }

      g_ctx.upload_is_folder = true;
      if (folder_publish(g_ctx.pending_handle, hparent, MTP_UPLOAD_PATH) < 0)
        {
          rmdir(MTP_UPLOAD_PATH);
          upload_abort(cmd->hdr.tid, PTP_RESPONSE_STOREFULL);
          return;
        }
    }
  else
#endif
    {
      int tfd;

      mtp_sched_relax();
      myvendor_mtp_activity_start(MYVENDOR_MTP_OP_UPLOAD, fname, size);
      tfd = open(MTP_UPLOAD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);

      if (tfd < 0)
        {
          mtp_info("SendObjectInfo creat failed errno=%d %s\n",
                   errno, MTP_UPLOAD_PATH);
          upload_abort(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR);
          return;
        }

      /* Keep the fd.  Re-opening with O_TRUNC in SendObject blocks LittleFS
       * while the host is already blasting DATA; the 2KiB OUT queue stalls
       * and gvfs reports "Could not send object" at 0 bytes. */
      g_ctx.upload_fd = tfd;

      if (file_publish(g_ctx.pending_handle, hparent, MTP_UPLOAD_PATH,
                       format, size) < 0)
        {
          upload_abort(cmd->hdr.tid, PTP_RESPONSE_STOREFULL);
          return;
        }

      /* Explorer "New Text Document" often stops after SendObjectInfo (like
       * new folder).  Publish ObjectAdded now so the host refreshes. */
      if (size == 0)
        {
          upload_fd_close();
          g_ctx.upload_file_committed = true;
          catalog_mark_dirty();
          mtp_info("create handle=0x%x (%s)\n", g_ctx.pending_handle,
                   mtp_basename(MTP_UPLOAD_PATH));
          mtp_send_event(PTP_EVENTCODE_OBJECTADDED, g_ctx.pending_handle);
          myvendor_mtp_activity_finish();
        }
    }

  resp_p[0] = store;
  resp_p[1] = mtp_is_store_root_handle(hparent) ? 0xffffffffu : hparent;
  resp_p[2] = g_ctx.pending_handle;
  mtp_info("SendObjectInfo OK -> %s (%llu bytes) handle=0x%x\n",
           MTP_UPLOAD_PATH, (unsigned long long)size,
           g_ctx.pending_handle);
  if (send_response(cmd->hdr.tid, PTP_RESPONSE_OK, resp_p, 3) < 0)
    {
      mtp_trace("SendObjectInfo response send failed\n");
    }

#if MTP_FEAT_FOLDERS
  if (g_ctx.upload_is_folder)
    {
      myvendor_mtp_activity_finish();
    }
#endif
}

/**
 * @brief 处理 SendObjectInfo 命令。
 */

static void handle_send_object_info(const struct ptp_cmd *cmd)
{
  size_t len;
  uint32_t store = dest_store_from_cmd(cmd->params[0]);

  mtp_trace("SendObjectInfo cmd store=0x%x p1=0x%x p2=0x%x tid=%u\n",
             store, cmd->params[1], cmd->params[2], cmd->hdr.tid);

  if (!mtp_storage_id_known(store))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID, NULL, 0);
      return;
    }

  /* Hosts may send 2 or 3 params (Windows/libmtp often duplicate parent in p2). */

  if (usb_read_container(g_ctx.ep_out, g_io_buf, MTP_IO_CHUNK, &len) < 0)
    {
      mtp_trace("SendObjectInfo data read failed errno=%d\n", errno);
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  /* Do not drain ZLP here: the command loop already skips leading ZLPs,
   * and a wait+read can swallow the following SendObject. */
  handle_send_object_info_data(cmd, g_io_buf, len);
}

#define MTP_UPLOAD_RAM_MAX     (2u * 1024u * 1024u)
#define MTP_UPLOAD_WRITE_CHUNK (32u * 1024u)

/**
 * @brief 每次写一块 LittleFS。catalog 提交留给 MTP poll，避免和
 * GetObjectInfo 抢同一份 catalog。
 */
static void upload_ram_flush_slice(void)
{
  size_t want;
  ssize_t n;
  uint32_t t_slice;

  if (g_upload_ram == NULL)
    {
      return;
    }

  if (g_upload_flush_cancel)
    {
      upload_ram_drop();
      upload_fd_close();
      MTP_UPLOAD_PATH[0] = '\0';
      g_upload_need_commit = false;
      myvendor_mtp_activity_finish();
      return;
    }

  if (g_ctx.upload_fd < 0)
    {
      mtp_info("SendObject lfs flush no fd, drop %zu\n", g_upload_ram_len);
      upload_ram_drop();
      myvendor_mtp_activity_finish();
      return;
    }

  if (g_upload_ram_t0 == 0)
    {
      g_upload_ram_t0 = mtp_now_ms();
    }

  want = g_upload_ram_len - g_upload_ram_off;
  if (want > MTP_UPLOAD_WRITE_CHUNK)
    {
      want = MTP_UPLOAD_WRITE_CHUNK;
    }

  if (want > 0)
    {
      myvendor_watchdog_work_beat();
      t_slice = mtp_now_ms();
      n = write(g_ctx.upload_fd, g_upload_ram + g_upload_ram_off, want);
      t_slice = mtp_now_ms() - t_slice;
      if (n != (ssize_t)want)
        {
          mtp_info("SendObject lfs write failed errno=%d off=%zu\n",
                   errno, g_upload_ram_off);
          upload_ram_drop();
          upload_fd_close();
          MTP_UPLOAD_PATH[0] = '\0';
          g_upload_need_commit = false;
          myvendor_mtp_activity_finish();
          return;
        }

      g_upload_ram_off += want;
      g_ctx.upload_got = (uint64_t)g_upload_ram_off;
      mtp_activity_progress_step(g_ctx.upload_got);
      mtp_info("SendObject lfs slice %zu/%zu %ums\n",
               g_upload_ram_off, g_upload_ram_len, (unsigned)t_slice);
    }

  if (g_upload_ram_off < g_upload_ram_len)
    {
      sched_yield();
      return;
    }

  mtp_info("SendObject lfs write %ums bytes=%zu ret=0\n",
           mtp_now_ms() - g_upload_ram_t0, g_upload_ram_len);

  upload_fd_close();
  upload_ram_drop();
  g_upload_need_commit = true;
}

static void upload_ram_commit_if_idle(void)
{
  char path[MTP_MAX_PATH];

  if (!g_upload_need_commit || g_upload_flushing || g_upload_ram != NULL)
    {
      return;
    }

  g_upload_need_commit = false;
  strncpy(path, MTP_UPLOAD_PATH, MTP_MAX_PATH - 1);
  path[MTP_MAX_PATH - 1] = '\0';
  if (path[0] != '\0')
    {
      (void)catalog_commit_upload(g_ctx.pending_handle,
                                  g_ctx.upload_parent, path);
    }

  MTP_UPLOAD_PATH[0] = '\0';
  myvendor_mtp_activity_finish();
}

static void *upload_ram_flush_thread(void *arg)
{
  (void)arg;

  while (g_upload_ram != NULL &&
         g_upload_ram_off < g_upload_ram_len &&
         !g_upload_flush_cancel)
    {
      upload_ram_flush_slice();
    }

  if (g_upload_flush_cancel && g_upload_ram != NULL)
    {
      upload_ram_flush_slice();
    }

  /* 有人在我们写的时候要求释放（usb_close / abort / 完成路径）：现在本线程是
   * 唯一写者，释放是安全的。必须在清 flushing 之前做 —— 清了之后别人可能立刻
   * 起一轮新的 flush，那样又变成两个写者。 */
  if (g_upload_drop_pend)
    {
      g_upload_drop_pend = false;
      upload_ram_free();
    }

  g_upload_flushing = false;
  return NULL;
}

static bool upload_ram_flush_kick(void)
{
  pthread_attr_t attr;
  pthread_t th;
  struct sched_param sp;
  int ret;
  int prio;

  if (g_upload_ram == NULL || g_upload_flushing)
    {
      return g_upload_flushing;
    }

  if (g_upload_flush_has)
    {
      (void)pthread_join(g_upload_flush_th, NULL);
      g_upload_flush_has = false;
      upload_flush_stack_free();
    }

  if (!upload_flush_stack_alloc())
    {
      g_upload_flushing = false;
      mtp_info("SendObject BoardPSRAM flush stack alloc failed, poll slices\n");
      return false;
    }

  g_upload_flush_cancel = false;
  g_upload_flushing = true;

  pthread_attr_init(&attr);
  ret = pthread_attr_setstack(&attr, g_upload_flush_stack,
                              MTP_UPLOAD_FLUSH_STACK);
  prio = CONFIG_MYVENDOR_MTP_SIMPLE_PRIORITY;
  if (prio > 10)
    {
      prio -= 10;
    }

  sp.sched_priority = prio;
#ifdef PTHREAD_EXPLICIT_SCHED
  (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
#endif
  (void)pthread_attr_setschedparam(&attr, &sp);
  if (ret == 0)
    {
      ret = pthread_create(&th, &attr, upload_ram_flush_thread, NULL);
    }

  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      g_upload_flushing = false;
      upload_flush_stack_free();
      mtp_info("SendObject lfs flush thread failed %d, poll slices\n", ret);
      return false;
    }

  g_upload_flush_th = th;
  g_upload_flush_has = true;
  mtp_info("SendObject lfs flush in background bytes=%zu\n", g_upload_ram_len);
  return true;
}

static void upload_flush_wait_prior(void)
{
  uint32_t t0 = mtp_now_ms();

  while (g_upload_flushing)
    {
      mtp_pump_ep0();
      myvendor_watchdog_work_beat();
      usleep(2000);
    }

  if (g_upload_flush_has)
    {
      (void)pthread_join(g_upload_flush_th, NULL);
      g_upload_flush_has = false;
      upload_flush_stack_free();
    }

  while (g_upload_ram != NULL)
    {
      upload_ram_flush_slice();
      mtp_pump_ep0();
    }

  upload_ram_commit_if_idle();
  if (mtp_now_ms() - t0 >= 20)
    {
      mtp_info("prior lfs flush wait %ums\n",
               (unsigned)(mtp_now_ms() - t0));
    }
}

/**
 * @brief 接收 DATA 阶段载荷。
 */

static int recv_data_payload(uint32_t expect_tid, uint16_t expect_code,
                             size_t *payload_len)
{
  uint8_t hdr[sizeof(struct ptp_header)];
  struct ptp_header *h = (struct ptp_header *)hdr;
  size_t remain;
  uint8_t *bulk = NULL;
  uint32_t t0;
  uint32_t t_usb = 0;
  int ret = -1;

  g_mtp_usb_hot = true;
  t0 = mtp_now_ms();

  if (usb_read_exact(g_ctx.ep_out, hdr, sizeof(hdr)) < 0)
    {
      mtp_info("SendObject hdr read failed errno=%d\n", errno);
      goto done;
    }

  if (h->len < sizeof(*h) || h->type != PTP_CONTAINER_DATA ||
      h->code != expect_code || h->tid != expect_tid)
    {
      mtp_info("SendObject bad hdr len=%u type=%u code=0x%x tid=%u\n",
               (unsigned)h->len, h->type, h->code, h->tid);
      goto done;
    }

  remain = h->len - sizeof(*h);
  *payload_len = remain;

  mtp_info("SendObject data phase %zu bytes fd=%d\n",
           remain, g_ctx.upload_fd);

  {
    uint64_t total = g_ctx.upload_size;

    if (total == 0)
      {
        total = remain;
      }

    myvendor_mtp_activity_start(MYVENDOR_MTP_OP_UPLOAD,
                                mtp_basename(MTP_UPLOAD_PATH), total);
  }

  if (remain > 0 && remain <= MTP_UPLOAD_RAM_MAX)
    {
      bulk = board_malloc_psram(remain);
      if (bulk != NULL && !board_ptr_in_psram_pool(bulk))
        {
          board_mem_free(bulk);
          bulk = NULL;
        }
    }

  if (remain == 0 || bulk != NULL)
    {
      if (remain > 0 &&
          usb_read_exact(g_ctx.ep_out, bulk, remain) < 0)
        {
          mtp_info("SendObject usb read failed errno=%d remain=%zu\n",
                   errno, remain);
          goto done;
        }

      t_usb = mtp_now_ms() - t0;

      /* USB payload is off the wire; host is waiting for the response.
       * Do not write LittleFS yet: lfs_alloc on a packed 14GiB volume
       * took 24s for 365KB and gvfs timed out.  Stash RAM and flush
       * after send_response(). */
      g_mtp_usb_hot = false;
      mtp_pump_ep0();

      if (g_ctx.upload_fd < 0)
        {
          g_ctx.upload_fd = open(MTP_UPLOAD_PATH,
                                 O_WRONLY | O_CREAT, 0666);
          if (g_ctx.upload_fd < 0)
            {
              mtp_info("SendObject open failed errno=%d %s\n",
                       errno, MTP_UPLOAD_PATH);
              goto done;
            }
        }

      upload_ram_drop();
      g_upload_ram     = bulk;
      g_upload_ram_len = remain;
      g_upload_ram_off = 0;
      g_upload_ram_t0  = 0;
      bulk             = NULL;
      g_ctx.upload_got = (uint64_t)remain;
    }
  else
    {
      mtp_info("SendObject chunked remain=%zu (PSRAM alloc failed or too big)\n",
               remain);

      if (g_ctx.upload_fd < 0)
        {
          g_ctx.upload_fd = open(MTP_UPLOAD_PATH,
                                 O_WRONLY | O_CREAT, 0666);
          if (g_ctx.upload_fd < 0)
            {
              mtp_info("SendObject open failed errno=%d %s\n",
                       errno, MTP_UPLOAD_PATH);
              goto done;
            }
        }

      g_ctx.upload_got = 0;
      while (remain > 0 && !g_ctx.cancelled)
        {
          size_t want = remain > MTP_IO_CHUNK ? MTP_IO_CHUNK : remain;

          g_mtp_usb_hot = true;
          if (usb_read_exact(g_ctx.ep_out, g_io_buf, want) < 0)
            {
              mtp_info("SendObject read failed errno=%d remain=%zu\n",
                       errno, remain);
              goto done;
            }

          g_mtp_usb_hot = false;
          mtp_pump_ep0();
          if (g_ctx.cancelled)
            {
              goto done;
            }

          if (write(g_ctx.upload_fd, g_io_buf, want) != (ssize_t)want)
            {
              mtp_info("SendObject write failed errno=%d\n", errno);
              goto done;
            }

          remain -= want;
          g_ctx.upload_got += (uint64_t)want;
          mtp_activity_progress_step(g_ctx.upload_got);
        }

      t_usb = mtp_now_ms() - t0;
    }

  myvendor_mtp_activity_progress(g_ctx.upload_got);
  usb_drain_out_zlp_if_needed(h->len);
  mtp_info("SendObject rx %ums write deferred bytes=%zu ram=%zu\n",
           t_usb, *payload_len, g_upload_ram_len);
  ret = 0;

done:
  g_mtp_usb_hot = false;
  board_mem_free(bulk);
  return ret;
}

/**
 * @brief 丢弃未消费的 DATA 载荷。
 */

static int drain_data_payload(uint32_t expect_tid, uint16_t expect_code,
                              size_t *payload_len)
{
  uint8_t hdr[sizeof(struct ptp_header)];
  struct ptp_header *h = (struct ptp_header *)hdr;
  size_t remain;

  if (usb_read_exact(g_ctx.ep_out, hdr, sizeof(hdr)) < 0)
    {
      return -1;
    }

  if (h->len < sizeof(*h) || h->type != PTP_CONTAINER_DATA ||
      h->code != expect_code || h->tid != expect_tid)
    {
      return -1;
    }

  remain = h->len - sizeof(*h);
  *payload_len = remain;

  g_mtp_usb_hot = true;
  while (remain > 0 && !g_ctx.cancelled)
    {
      size_t want = remain > MTP_IO_CHUNK ? MTP_IO_CHUNK : remain;

      if (usb_read_exact(g_ctx.ep_out, g_io_buf, want) < 0)
        {
          g_mtp_usb_hot = false;
          return -1;
        }

      remain -= want;
    }

  g_mtp_usb_hot = false;

  usb_drain_out_zlp_if_needed(h->len);
  return 0;
}

/**
 * @brief 可选丢弃 DATA 载荷。
 */

static int drain_data_payload_optional(uint32_t expect_tid,
                                       uint16_t expect_code,
                                       size_t *payload_len)
{
  struct pollfd pfd;
  int pr;

  pfd.fd     = g_ctx.ep_out;
  pfd.events = POLLIN;
  pr         = poll(&pfd, 1, 300);
  if (pr <= 0 || !(pfd.revents & POLLIN))
    {
      *payload_len = 0;
      return 0;
    }

  return drain_data_payload(expect_tid, expect_code, payload_len);
}

/**
 * @brief 处理 SendObject（主机上传）。
 */

static void handle_send_object(const struct ptp_cmd *cmd)
{
  size_t plen;
  uint32_t handle = g_ctx.pending_handle;
  uint32_t parent = g_ctx.upload_parent;
  char *path = MTP_SCRATCH_PATH(1);

  mtp_info("SendObject cmd tid=%u path=%s size=%llu committed=%d folder=%d\n",
           cmd->hdr.tid, MTP_UPLOAD_PATH,
           (unsigned long long)g_ctx.upload_size,
           g_ctx.upload_file_committed ? 1 : 0,
           g_ctx.upload_is_folder ? 1 : 0);

  if (MTP_UPLOAD_PATH[0] == '\0')
    {
      mtp_trace("SendObject without prior SendObjectInfo\n");
      send_response(cmd->hdr.tid, PTP_RESPONSE_NOVALID_OBJINFO, NULL, 0);
      return;
    }

  strncpy(path, MTP_UPLOAD_PATH, MTP_MAX_PATH - 1);
  path[MTP_MAX_PATH - 1] = '\0';

  if (g_ctx.upload_is_folder)
    {
#if MTP_FEAT_FOLDERS
      bool committed = g_ctx.upload_folder_committed;

      (void)drain_data_payload_optional(cmd->hdr.tid, cmd->hdr.code, &plen);

      MTP_UPLOAD_PATH[0]           = '\0';
      g_ctx.upload_is_folder         = false;
      g_ctx.upload_folder_committed  = false;

      if (!committed)
        {
          myvendor_mtp_activity_start(MYVENDOR_MTP_OP_MKDIR,
                                      mtp_basename(path), 0);
          if (catalog_commit_folder(handle, parent, path) < 0)
            {
              rmdir(path);
              send_response(cmd->hdr.tid, PTP_RESPONSE_STOREFULL, NULL, 0);
              return;
            }

          mtp_info("mkdir handle=0x%x (%s)\n", handle, mtp_basename(path));
          catalog_mark_dirty();
          /* Host already has the handle from SendObjectInfo; skip
           * ObjectAdded so gvfs's event thread does not race the next
           * bulk command (Ubuntu multi-select copy). */
        }

      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
      return;
#else
      send_response(cmd->hdr.tid, PTP_RESPONSE_OP_NOT_SUPPORTED, NULL, 0);
      return;
#endif
    }

  if (g_ctx.upload_size == 0 || g_ctx.upload_file_committed)
    {
      bool committed = g_ctx.upload_file_committed;

      if (drain_data_payload_optional(cmd->hdr.tid, cmd->hdr.code, &plen) < 0)
        {
          upload_abort(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR);
          return;
        }

      MTP_UPLOAD_PATH[0]         = '\0';
      g_ctx.upload_file_committed = false;

      if (!committed)
        {
          mtp_info("create handle=0x%x (%s)\n", handle, mtp_basename(path));
          if (catalog_commit_upload(handle, parent, path) < 0)
            {
              catalog_sync_children_maybe(parent);
            }
        }

      catalog_mark_dirty();
      send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
      return;
    }

  if (recv_data_payload(cmd->hdr.tid, cmd->hdr.code, &plen) < 0)
    {
      mtp_err("upload failed %s\n", mtp_basename(path));
      upload_abort(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR);
      return;
    }

  /* Reply before LittleFS: packed-volume lfs_alloc can take tens of
   * seconds for a 365KB file, which trips libmtp's SendObject timeout.
   * Flush runs on a lower-priority thread so GetObjectInfo can report
   * the ObjectInfo size while SD is still programming. */
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);

  if (g_upload_ram != NULL)
    {
      mtp_info("upload %s (%zu bytes) lfs deferred\n",
               mtp_basename(path), plen);
      (void)upload_ram_flush_kick();
      return;
    }

  mtp_info("upload %s (%zu bytes)\n", mtp_basename(path), plen);
  MTP_UPLOAD_PATH[0] = '\0';

  if (catalog_commit_upload(handle, parent, path) < 0)
    {
      struct mtp_obj *obj;

      mtp_trace("catalog_commit_upload failed, sync parent+lookup\n");
      catalog_sync_children_maybe(parent);
      obj = catalog_find_by_path(path);
      if (obj != NULL)
        {
          handle = obj->handle;
        }
    }

  if (catalog_find(handle) == NULL)
    {
      mtp_trace("warn: handle 0x%x not in catalog after upload\n", handle);
    }

  myvendor_mtp_activity_finish();
  upload_fd_close();
  /* Do not send ObjectAdded here: SendObjectInfo already returned the
   * new handle.  Interrupt-IN during a multi-file SendObject batch races
   * Ubuntu gvfs (libmtp is not thread-safe with the event poller). */
}

/* ========================================================================== */
/* §13 Optional command handlers — gated by mtp_features.h                  */
/* ========================================================================== */

#if MTP_FEAT_GET_THUMB

/**
 * @brief 处理 GetThumb（空缩略图）。
 */

static void handle_get_thumb(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  uint8_t hdr[sizeof(struct ptp_header)];

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL || obj->format == PTP_FMT_ASSOCIATION)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  ((struct ptp_header *)hdr)->len  = sizeof(hdr);
  ((struct ptp_header *)hdr)->type = PTP_CONTAINER_DATA;
  ((struct ptp_header *)hdr)->code = cmd->hdr.code;
  ((struct ptp_header *)hdr)->tid  = cmd->hdr.tid;

  if (usb_write_bulk(hdr, sizeof(hdr)) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  usb_in_end();
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

#endif /* MTP_FEAT_GET_THUMB */

#if MTP_FEAT_MOVE_OBJECT

/**
 * @brief 处理 MoveObject。
 */

static void handle_move_object(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  uint32_t handle = cmd->params[0];
  uint32_t store = cmd->params[1];
  uint32_t dest;
  uint32_t new_parent;
  char *newpath = MTP_SCRATCH_PATH(1);
  char *oldpath = MTP_SCRATCH_PATH(2);

  if (store != 0 && !mtp_storage_id_all(store) &&
      !mtp_storage_id_known(store))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID, NULL, 0);
      return;
    }

  dest = (store == 0 || mtp_storage_id_all(store)) ? 0 : store;
  new_parent = parent_norm(cmd->params[2], dest != 0 ? dest : 0);
  dest = store_id_for_parent(new_parent, dest);

  obj = catalog_find(handle);
  if (obj == NULL || mtp_is_store_root_handle(obj->handle))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  if (obj->storage != dest)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_ACCESS_DENIED, NULL, 0);
      return;
    }

  if (!mtp_is_store_root_handle(new_parent) &&
      catalog_find(new_parent) == NULL)
    {
      catalog_sync_children_maybe(new_parent);
      if (catalog_find(new_parent) == NULL)
        {
          send_response(cmd->hdr.tid, PTP_RESPONSE_INVALIDPARENT, NULL, 0);
          return;
        }
    }

  if (build_child_path(new_parent, dest, obj->name, newpath,
                       MTP_MAX_PATH) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  if (strcmp(obj->path, newpath) == 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
      return;
    }

  strncpy(oldpath, obj->path, MTP_MAX_PATH - 1);
  oldpath[MTP_MAX_PATH - 1] = '\0';

  myvendor_mtp_activity_start(MYVENDOR_MTP_OP_MOVE, obj_display_name(obj), 0);
  myvendor_mtp_activity_set_detail(mtp_basename(newpath));

  if (rename(oldpath, newpath) != 0)
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  catalog_repath_prefix(oldpath, newpath);
  obj = catalog_find(handle);
  if (obj != NULL)
    {
      obj->parent = new_parent;
    }

  mtp_info("move %s -> parent 0x%x\n", mtp_basename(newpath), new_parent);
  catalog_mark_dirty();
  if (myvendor_fw_slot_on_commit(newpath) > 0)
    {
      catalog_mark_dirty();
    }

  myvendor_mtp_activity_finish();
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_event_object_info_changed(handle);
}

#endif /* MTP_FEAT_MOVE_OBJECT */

#if MTP_FEAT_COPY_OBJECT

/**
 * @brief 处理 CopyObject。
 */

static void handle_copy_object(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  uint32_t handle = cmd->params[0];
  uint32_t store = cmd->params[1];
  uint32_t dest;
  uint32_t new_parent;
  char *dstpath = MTP_SCRATCH_PATH(1);
  char suffix[16];
  uint32_t new_handle;

  if (store != 0 && !mtp_storage_id_all(store) &&
      !mtp_storage_id_known(store))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID, NULL, 0);
      return;
    }

  dest = (store == 0 || mtp_storage_id_all(store)) ? 0 : store;
  new_parent = parent_norm(cmd->params[2], dest != 0 ? dest : 0);
  dest = store_id_for_parent(new_parent, dest);

  obj = catalog_find(handle);
  if (obj == NULL || mtp_is_store_root_handle(obj->handle))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  if (obj->storage != dest)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_ACCESS_DENIED, NULL, 0);
      return;
    }

  if (!mtp_is_store_root_handle(new_parent) &&
      catalog_find(new_parent) == NULL)
    {
      catalog_sync_children_maybe(new_parent);
      if (catalog_find(new_parent) == NULL)
        {
          send_response(cmd->hdr.tid, PTP_RESPONSE_INVALIDPARENT, NULL, 0);
          return;
        }
    }

  if (build_child_path(new_parent, dest, obj->name, dstpath,
                       MTP_MAX_PATH) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  {
    struct stat st;

    /* Same-folder duplicate or name already taken: append _copy.
     * Cross-folder copy (Nautilus multi-select) must keep the original
     * name or gvfs treats the operation as failed. */

    if (catalog_find_by_path(dstpath) != NULL ||
        catalog_stat_file(dstpath, &st) == 0)
      {
        const char *dot = strrchr(dstpath, '.');

        snprintf(suffix, sizeof(suffix), "_copy");
        if (dot != NULL && obj->format != PTP_FMT_ASSOCIATION)
          {
            char *base = MTP_SCRATCH_PATH(2);
            const char *ext = dot;

            snprintf(base, MTP_MAX_PATH, "%.*s%s%s",
                     (int)(dot - dstpath), dstpath, suffix, ext);
            strncpy(dstpath, base, MTP_MAX_PATH - 1);
            dstpath[MTP_MAX_PATH - 1] = '\0';
          }
        else
          {
            strncat(dstpath, suffix, MTP_MAX_PATH - strlen(dstpath) - 1);
          }
      }
  }

  myvendor_mtp_activity_start(MYVENDOR_MTP_OP_COPY, obj_display_name(obj), 0);
  myvendor_mtp_activity_set_detail(mtp_basename(dstpath));

  if (fs_copy_tree(obj->path, dstpath) != 0)
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  catalog_sync_children_maybe(new_parent);
  {
    struct mtp_obj *copy = catalog_find_by_path(dstpath);

    new_handle = copy ? copy->handle : 0;
  }

  mtp_info("copy %s -> %s\n", obj->name, mtp_basename(dstpath));
  catalog_mark_dirty();
  if (myvendor_fw_slot_on_commit(dstpath) > 0)
    {
      catalog_mark_dirty();
    }

  myvendor_mtp_activity_finish();
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, &new_handle, 1);
}

#endif /* MTP_FEAT_COPY_OBJECT */

#if MTP_FEAT_FORMAT_STORE

/**
 * @brief 清空存储路径内容。
 */

static void storage_wipe_contents(const char *root)
{
  DIR *dir;
  struct dirent *ent;
  char *child = MTP_SCRATCH_PATH(3);

  if (root == NULL || root[0] == '\0')
    {
      return;
    }

  dir = opendir(root);
  if (dir == NULL)
    {
      return;
    }

  while ((ent = readdir(dir)) != NULL)
    {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
          continue;
        }

      if (mtp_path_join(child, MTP_MAX_PATH, root, ent->d_name) < 0)
        {
          continue;
        }

      if (catalog_entry_skipped(ent->d_name, child))
        {
          continue;
        }

      fs_remove_tree(child);
    }

  closedir(dir);
}

/**
 * @brief 处理 FormatStore。
 */

static void handle_format_store(const struct ptp_cmd *cmd)
{
  const struct mtp_store *s = mtp_storage_by_id(cmd->params[0]);

  if (s == NULL)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_STORE_ID, NULL, 0);
      return;
    }

  if (!s->format_ok)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_ACCESS_DENIED, NULL, 0);
      return;
    }

  storage_wipe_contents(s->path);
  catalog_rescan();
  mtp_info("format store %s\n", s->path);
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
#if MTP_FEAT_EXTRA_EVENTS
  mtp_send_event(PTP_EVENTCODE_STOREREMOVED, s->id);
  mtp_send_event(PTP_EVENTCODE_STOREADDED, s->id);
#endif
}

#endif /* MTP_FEAT_FORMAT_STORE */

#if MTP_FEAT_CAPTURE

/**
 * @brief 处理 InitiateCapture（桩）。
 */

static void handle_initiate_capture(const struct ptp_cmd *cmd)
{
  (void)cmd;
  send_response(cmd->hdr.tid, PTP_RESPONSE_OP_NOT_SUPPORTED, NULL, 0);
}

/**
 * @brief 处理 TerminateCapture（桩）。
 */

static void handle_terminate_capture(const struct ptp_cmd *cmd)
{
  (void)cmd;
  send_response(cmd->hdr.tid, PTP_RESPONSE_OP_NOT_SUPPORTED, NULL, 0);
}

#endif /* MTP_FEAT_CAPTURE */

#if MTP_FEAT_SET_DEVICE_PROP

/**
 * @brief 处理 SetDevicePropValue。
 */

static void handle_set_device_prop_value(const struct ptp_cmd *cmd)
{
  (void)cmd;
  send_response(cmd->hdr.tid, PTP_RESPONSE_PROP_NOTSUPPORTED, NULL, 0);
}

#endif /* MTP_FEAT_SET_DEVICE_PROP */

#if MTP_FEAT_SET_OBJECT_PROP

/**
 * @brief 处理 SetObjectPropValue。
 */

static void handle_set_object_prop_value(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  size_t len;
  char *newname = MTP_SCRATCH_NAME(0);
  char *newpath = MTP_SCRATCH_PATH(1);
  char *oldpath = MTP_SCRATCH_PATH(2);
  uint16_t prop = (uint16_t)cmd->params[1];
  const struct ptp_header *dh;

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL || mtp_is_store_root_handle(obj->handle))
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  if (prop != MTP_OBJPROP_FILENAME)
    {
      prop = (uint16_t)cmd->params[2];
    }

  if (prop != MTP_OBJPROP_FILENAME)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_PROP_NOTSUPPORTED, NULL, 0);
      return;
    }

  myvendor_mtp_activity_start(MYVENDOR_MTP_OP_RENAME, obj_display_name(obj), 0);

  if (usb_read_container(g_ctx.ep_out, g_io_buf, MTP_IO_CHUNK, &len) < 0)
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  usb_drain_out_zlp_if_needed((uint32_t)len);
  dh = (const struct ptp_header *)g_io_buf;
  if (len <= sizeof(*dh) || dh->type != PTP_CONTAINER_DATA)
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  if (ptp_str_get(g_io_buf + sizeof(*dh), (int)(len - sizeof(*dh)),
                  newname, MTP_MAX_NAME) < 0 ||
      !upload_name_ok(newname))
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  myvendor_mtp_activity_set_detail(newname);

  if (build_child_path(obj->parent, obj->storage, newname, newpath,
                       MTP_MAX_PATH) < 0)
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  if (strcmp(obj->path, newpath) == 0)
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
      return;
    }

  strncpy(oldpath, obj->path, MTP_MAX_PATH - 1);
  oldpath[MTP_MAX_PATH - 1] = '\0';

  if (rename(oldpath, newpath) != 0)
    {
      myvendor_mtp_activity_finish();
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  catalog_repath_prefix(oldpath, newpath);
  mtp_info("rename %s -> %s\n", mtp_basename(oldpath), newname);
  catalog_mark_dirty();
  myvendor_mtp_activity_finish();
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_event_object_info_changed(obj->handle);
}

#endif /* MTP_FEAT_SET_OBJECT_PROP */

#if MTP_FEAT_OBJECT_REFERENCES

/**
 * @brief 处理 GetObjectReferences。
 */

static void handle_get_object_references(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;
  uint8_t buf[8];
  uint32_t count = 0;

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  memcpy(buf, &count, 4);
  if (send_data(cmd->hdr.tid, cmd->hdr.code, buf, 4) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

/**
 * @brief 处理 SetObjectReferences。
 */

static void handle_set_object_references(const struct ptp_cmd *cmd)
{
  size_t len;

  if (usb_read_container(g_ctx.ep_out, g_io_buf, MTP_IO_CHUNK, &len) < 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_GEN_ERROR, NULL, 0);
      return;
    }

  usb_drain_out_zlp_if_needed((uint32_t)len);
  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
}

#endif /* MTP_FEAT_OBJECT_REFERENCES */

#if MTP_FEAT_UPDATE_OBJECT

/**
 * @brief 处理 UpdateObject。
 */

static void handle_update_object(const struct ptp_cmd *cmd)
{
  struct mtp_obj *obj;

  obj = catalog_find(cmd->params[0]);
  if (obj == NULL)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_INVALID_OBJ_HANDLE, NULL, 0);
      return;
    }

  send_response(cmd->hdr.tid, PTP_RESPONSE_OK, NULL, 0);
  mtp_event_object_info_changed(cmd->params[0]);
}

#endif /* MTP_FEAT_UPDATE_OBJECT */

/* ========================================================================== */
/* §14  命令分发 — maps opcode → handler                               */
/* ========================================================================== */

/**
 * @brief 按 opcode 分发 PTP 命令。
 */

static void dispatch_command(const struct ptp_cmd *cmd)
{
  uint32_t t0 = mtp_now_ms();

  mtp_sched_relax();

  if (cmd->hdr.code != PTP_OPCODE_GETDEVICEINFO &&
      cmd->hdr.code != PTP_OPCODE_GETDEVICEPROPDESC &&
      cmd->hdr.code != PTP_OPCODE_GETDEVICEPROPVALUE &&
      cmd->hdr.code != PTP_OPCODE_OPENSESSION && g_ctx.session == 0)
    {
      send_response(cmd->hdr.tid, PTP_RESPONSE_SESSIONNOTOPEN, NULL, 0);
      return;
    }

  switch (cmd->hdr.code)
    {
    case PTP_OPCODE_GETDEVICEINFO:
      handle_get_device_info(cmd);
      break;
    case PTP_OPCODE_OPENSESSION:
      handle_open_session(cmd);
      break;
    case PTP_OPCODE_CLOSESESSION:
      handle_close_session(cmd);
      break;
    case PTP_OPCODE_GETSTORAGEIDS:
      handle_get_storage_ids(cmd);
      break;
    case PTP_OPCODE_GETSTORAGEINFO:
      handle_get_storage_info(cmd);
      break;
    case PTP_OPCODE_GETNUMOBJECTS:
      handle_get_num_objects(cmd);
      break;
    case PTP_OPCODE_GETOBJECTHANDLES:
      handle_get_object_handles(cmd);
      break;
    case PTP_OPCODE_GETOBJECTINFO:
      handle_get_object_info(cmd);
      break;
    case PTP_OPCODE_GETOBJECT:
      handle_get_object(cmd);
      break;
#if MTP_FEAT_GET_THUMB
    case PTP_OPCODE_GETTHUMB:
      handle_get_thumb(cmd);
      break;
#endif
#if MTP_FEAT_OBJECT_PROPLIST
    case MTP_OPCODE_GETOBJECTPROPSUPPORTED:
      handle_get_object_props_supported(cmd);
      break;
    case MTP_OPCODE_GETOBJECTPROPDESC:
      handle_get_object_prop_desc(cmd);
      break;
    case MTP_OPCODE_GETOBJECTPROPVALUE:
      handle_get_object_prop_value(cmd);
      break;
    case MTP_OPCODE_GETOBJECTPROPLIST:
      handle_get_object_prop_list(cmd);
      break;
#endif
#if MTP_FEAT_GET_PARTIAL_OBJECT
    case PTP_OPCODE_GETPARTIALOBJECT:
      handle_get_partial_object(cmd);
      break;
#endif
    case PTP_OPCODE_DELETEOBJECT:
      handle_delete_object(cmd);
      break;
    case PTP_OPCODE_SENDOBJECTINFO:
      handle_send_object_info(cmd);
      break;
    case PTP_OPCODE_SENDOBJECT:
      handle_send_object(cmd);
      break;
#if MTP_FEAT_FORMAT_STORE
    case PTP_OPCODE_FORMATSTORE:
      handle_format_store(cmd);
      break;
#endif
#if MTP_FEAT_CAPTURE
    case PTP_OPCODE_INITIATECAPTURE:
      handle_initiate_capture(cmd);
      break;
    case PTP_OPCODE_TERMINATECAPTURE:
      handle_terminate_capture(cmd);
      break;
#endif
    case PTP_OPCODE_GETDEVICEPROPDESC:
      handle_get_device_prop_desc(cmd);
      break;
    case PTP_OPCODE_GETDEVICEPROPVALUE:
      handle_get_device_prop_value(cmd);
      break;
#if MTP_FEAT_SET_DEVICE_PROP
    case PTP_OPCODE_SETDEVICEPROPVALUE:
      handle_set_device_prop_value(cmd);
      break;
#endif
#if MTP_FEAT_MOVE_OBJECT
    case PTP_OPCODE_MOVEOBJECT:
      handle_move_object(cmd);
      break;
#endif
#if MTP_FEAT_COPY_OBJECT
    case PTP_OPCODE_COPYOBJECT:
      handle_copy_object(cmd);
      break;
#endif
#if MTP_FEAT_SET_OBJECT_PROP
    case MTP_OPCODE_SETOBJECTPROPVALUE:
      handle_set_object_prop_value(cmd);
      break;
#endif
#if MTP_FEAT_OBJECT_REFERENCES
    case MTP_OPCODE_GETOBJECTREFERENCES:
      handle_get_object_references(cmd);
      break;
    case MTP_OPCODE_SETOBJECTREFERENCES:
      handle_set_object_references(cmd);
      break;
#endif
#if MTP_FEAT_UPDATE_OBJECT
    case MTP_OPCODE_UPDATEOBJECT:
      handle_update_object(cmd);
      break;
#endif
    default:
      mtp_trace("unsupported opcode 0x%x tid=%u\n",
                 cmd->hdr.code, cmd->hdr.tid);
      send_response(cmd->hdr.tid, PTP_RESPONSE_OP_NOT_SUPPORTED, NULL, 0);
      break;
    }

  {
    uint16_t code = cmd->hdr.code;
    uint32_t dt = mtp_now_ms() - t0;

    if (code != PTP_OPCODE_GETOBJECTINFO &&
        code != PTP_OPCODE_GETOBJECTHANDLES &&
        code != PTP_OPCODE_GETNUMOBJECTS &&
        code != PTP_OPCODE_GETSTORAGEINFO &&
        code != MTP_OPCODE_GETOBJECTPROPLIST &&
        dt >= 15)
      {
        mtp_info("op 0x%04x p0=0x%lx p1=0x%lx p2=0x%lx %ums\n",
                 code,
                 (unsigned long)cmd->params[0],
                 (unsigned long)cmd->params[1],
                 (unsigned long)cmd->params[2],
                 (unsigned)dt);
      }
  }
}

/* ========================================================================== */
/* §15 EP0 PTP class requests and main poll loop                              */
/* ========================================================================== */

/**
 * @brief 处理 EP0 PTP 类请求。
 */

static void handle_ep0(void)
{
  struct usb_ctrlreq_s req;
  ssize_t n;

  n = read(g_ctx.ep0, &req, sizeof(req));
  if (n < (ssize_t)sizeof(req))
    {
      return;
    }

  if ((req.type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS)
    {
      if (req.req == USB_PTPREQUEST_GETEVENT && USB_REQ_ISIN(req.type))
        {
          if (g_ctx.have_pending_event && g_ctx.pending_event_len > 0)
            {
              usb_ep_write_pkt(g_ctx.ep0, g_ctx.pending_event,
                               g_ctx.pending_event_len);
              g_ctx.have_pending_event = false;
            }
          else
            {
              read(g_ctx.ep0, &g_ep0_dummy, 0);
            }

          return;
        }
    }

  switch (req.req)
    {
    case USB_PTPREQUEST_CANCELIO:
      g_ctx.cancelled = true;
      read(g_ctx.ep0, &g_ep0_dummy, 0);
      break;

    case USB_PTPREQUEST_RESET:
      g_ctx.session = 0;
      myvendor_mtp_lfs_set_session(false);
      g_ctx.cancelled = false;
      MTP_UPLOAD_PATH[0] = '\0';
      read(g_ctx.ep0, &g_ep0_dummy, 0);
      break;

    case USB_PTPREQUEST_GETSTATUS:
      {
        struct
        {
          uint16_t len;
          uint16_t code;
        } st;

        st.len  = 4;
        st.code = PTP_RESPONSE_OK;
        usb_ep_write_pkt(g_ctx.ep0, &st, sizeof(st));
      }
      break;

    default:
      read(g_ctx.ep0, &g_ep0_dummy, 0);
      break;
    }
}

/**
 * @brief 泵 EP0 控制传输。
 */

static void mtp_pump_ep0(void)
{
  struct pollfd pfd;

  if (g_ctx.ep0 < 0)
    {
      return;
    }

  pfd.fd     = g_ctx.ep0;
  pfd.events = POLLIN | POLLPRI | POLLERR | POLLHUP;
  if (poll(&pfd, 1, 0) <= 0)
    {
      return;
    }

  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      myvendor_mtp_worker_link_lost();
      return;
    }

  if (pfd.revents & POLLIN)
    {
      handle_ep0();
    }
}

/**
 * @brief USB 断链后重置协议状态。
 */

static void mtp_reset_link_state(void)
{
  g_ctx.session                 = 0;
  g_ctx.upload_size             = 0;
  g_ctx.upload_got              = 0;
  g_ctx.pending_handle          = 0;
  g_ctx.upload_parent           = 0;
  g_ctx.upload_format           = 0;
  g_ctx.upload_is_folder        = false;
  g_ctx.upload_folder_committed = false;
  g_ctx.upload_file_committed   = false;
  g_ctx.cancelled               = false;
  g_ctx.pending_event_len       = 0;
  g_ctx.have_pending_event      = false;
  myvendor_mtp_lfs_set_session(false);
}

#define MTP_POLL_EVENTS (POLLIN | POLLPRI | POLLERR | POLLHUP)

static bool mtp_poll_hangup(const struct pollfd *pfds, int nfds)
{
  int i;

  for (i = 0; i < nfds; i++)
    {
      if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
          return true;
        }
    }

  return false;
}

/* Consecutive benign OUT glitches tolerated before declaring link loss.
 * Also bounds usb_drain_out_stale() so a pathological OUT stream cannot spin.
 */

#define MTP_BULK_SOFT_MAX        64
#define MTP_BULK_SOFT_BACKOFF_US 1000

/**
 * @brief 排空 stale OUT 数据。
 */

static int usb_drain_out_stale(int fd)
{
  int drained = 0;
  int i;

  for (i = 0; i < MTP_BULK_SOFT_MAX; i++)
    {
      ssize_t n;

      if (usb_wait_fd(fd, POLLIN, 0) != 1)
        {
          break;
        }

      n = usb_ep_read_pkt(fd, NULL, sizeof(g_usb_pkt));
      if (n <= 0)
        {
          break;
        }

      drained++;
    }

  return drained;
}

/**
 * @brief 探测主机是否仍在线。
 */

static bool mtp_probe_host_link(void)
{
  struct pollfd pfds[2];
  int pr;

  if (access(MTP_EP_OUT, F_OK) != 0)
    {
      return false;
    }

  if (usb_open() < 0)
    {
      return false;
    }

  pfds[0].fd     = g_ctx.ep0;
  pfds[0].events = MTP_POLL_EVENTS;
  pfds[1].fd     = g_ctx.ep_out;
  pfds[1].events = MTP_POLL_EVENTS;

  pr = poll(pfds, 2, MTP_HOST_PROBE_MS);
  if (pr < 0 || mtp_poll_hangup(pfds, 2))
    {
      usb_close();
      return false;
    }

  if (pr == 0)
    {
      /* Stale ep2 after disconnect: open succeeds but no host activity. */
      usb_close();
      return false;
    }

  myvendor_mtp_lfs_set_link(MYVENDOR_MTP_LINK_ENUM);
  return true;
}

/**
 * @brief 等待 USB 枚举与 ep2 就绪。
 */

static bool wait_for_mtp_host(void)
{
  bool logged = false;

  for (; ; )
    {
      if (myvendor_mtp_worker_should_exit())
        {
          return false;
        }

      if (mtp_probe_host_link())
        {
          return true;
        }

      if (!logged)
        {
          mtp_info("waiting for USB host (%s)...\n", MTP_EP_OUT);
          logged = true;
        }

      sleep(1);
    }
}

/**
 * @brief MTP 协议主 poll 循环。
 */

static int mtp_simple_loop(void)
{
  /* Host command containers are small (header + up to 5 params). */
  uint8_t *buf = MTP_SCRATCH_CMD;
  size_t len;
  struct ptp_cmd cmd;
  struct pollfd pfds[2];

  for (; ; )
    {
      if (!wait_for_mtp_host())
        {
          break;
        }

      if (!myvendor_mtp_worker_wait_begin())
        {
          usb_close();
          myvendor_mtp_lfs_set_link(MYVENDOR_MTP_LINK_OFF);
          if (myvendor_mtp_worker_should_exit())
            {
              break;
            }

          continue;
        }

      myvendor_mtp_lfs_set_link(MYVENDOR_MTP_LINK_UP);
      myvendor_mtp_worker_set_active(true);
      {
        unsigned i;
        const struct mtp_store *s0 = mtp_storage_at(0);

        mtp_info("ready %u store(s) primary=%s (owner enabled)\n",
                 mtp_storage_count(),
                 s0 != NULL ? s0->path : "?");
        for (i = 1; i < mtp_storage_count(); i++)
          {
            const struct mtp_store *s = mtp_storage_at(i);

            if (s != NULL)
              {
                mtp_info("  extra store[%u] %s id=0x%lx\n",
                         i, s->path, (unsigned long)s->id);
              }
          }
      }

      pfds[0].fd     = g_ctx.ep0;
      pfds[0].events = MTP_POLL_EVENTS;
      pfds[1].fd     = g_ctx.ep_out;
      pfds[1].events = MTP_POLL_EVENTS;

      bool host_hangup = false;
      int soft_fail = 0;

      for (; ; )
        {
          int to = 500;

          if (g_upload_flushing)
            {
              to = 20;
            }
          else if (g_upload_ram != NULL)
            {
              to = 0;
            }

          int pr = poll(pfds, 2, to);
          if (pr < 0)
            {
              if (errno == EINTR)
                {
                  continue;
                }

              mtp_info("poll error errno=%d\n", errno);
              break;
            }

          if (myvendor_mtp_worker_poll_abort())
            {
              mtp_info("owner stop transfer\n");
              break;
            }

          if (myvendor_mtp_worker_link_lost_pending())
            {
              mtp_info("io link lost (abort poll)\n");
              break;
            }

          if (mtp_poll_hangup(pfds, 2))
            {
              host_hangup = true;
              mtp_info("usb hangup ep0=0x%x ep_out=0x%x\n",
                       pfds[0].revents, pfds[1].revents);
              break;
            }

          g_ctx.cancelled = false;

          if (pfds[0].revents & POLLIN)
            {
              handle_ep0();
            }

          if (pfds[1].revents & POLLIN)
            {
              int rc = usb_read_container_ex(g_ctx.ep_out, buf,
                                             MTP_CMD_BUF_SIZE, &len, true);
              /* SOFT = the OUT read returned bytes that are not a valid PTP
               * command header (leftover/partial container re-delivered from
               * the driver's un-flushed OUT queue after a reconnect, or the
               * tail of an aborted data phase).  This is NOT a disconnect
               * (a real one surfaces as MTP_CONTAINER_ERR with errno set, or
               * POLLHUP handled above), so drain the stale bytes and keep
               * polling instead of tearing the link down.  Tearing down here
               * caused a self-sustaining ACTIVE->ENUM->OFF re-enumeration
               * storm ("can't open").  Bounded by MTP_BULK_SOFT_MAX so a
               * permanently wedged stream still falls back to a clean
               * reconnect.
               */

              if (rc == MTP_CONTAINER_SOFT)
                {
                  if (!myvendor_mtp_worker_link_lost_pending() &&
                      ++soft_fail <= MTP_BULK_SOFT_MAX)
                    {
                      usb_drain_out_stale(g_ctx.ep_out);
                      usleep(MTP_BULK_SOFT_BACKOFF_US);
                      continue;
                    }

                  mtp_info("bulk desync giveup soft=%d, reconnecting\n",
                           soft_fail);
                  break;
                }

              if (rc == MTP_CONTAINER_ERR)
                {
                  mtp_info("bulk read failed errno=%d soft=%d\n",
                           errno, soft_fail);
                  break;
                }

              soft_fail = 0;

              if (rc == MTP_CONTAINER_ZLP)
                {
                  /* Leading ZLP: no command this cycle.  Return to poll()
                   * so EP0 control requests stay serviced (prevents the
                   * "host can't open until replug" hang).
                   */

                  continue;
                }

              if (parse_cmd(buf, len, &cmd) < 0)
                {
                  continue;
                }

              if (cmd.hdr.type != PTP_CONTAINER_COMMAND)
                {
                  mtp_trace("ignore bulk type=%u code=0x%x len=%u\n",
                             cmd.hdr.type, cmd.hdr.code, (unsigned)len);
                  continue;
                }

              dispatch_command(&cmd);
            }
          else if (g_upload_ram != NULL && !g_upload_flushing)
            {
              /* Fallback if the background thread failed to start. */
              upload_ram_flush_slice();
            }

          upload_ram_commit_if_idle();
        }

      usb_close();
      myvendor_mtp_worker_set_active(false);
      mtp_reset_link_state();

      if (myvendor_mtp_worker_link_lost_pending() || host_hangup)
        {
          if (host_hangup)
            {
              mtp_info("disconnected\n");
            }

          myvendor_mtp_lfs_set_link(MYVENDOR_MTP_LINK_OFF);
          myvendor_mtp_worker_clear_link_lost();
          if (host_hangup)
            {
              sleep(MTP_HOST_DISCONNECT_COOLDOWN_SEC);
            }
        }
      else if (access(MTP_EP_OUT, F_OK) != 0)
        {
          myvendor_mtp_lfs_set_link(MYVENDOR_MTP_LINK_OFF);
        }
      else if (myvendor_mtp_worker_poll_abort())
        {
          /* Owner ended transfer; host still connected. */
          myvendor_mtp_lfs_set_link(MYVENDOR_MTP_LINK_ENUM);
        }
      else
        {
          mtp_info("link lost (poll/read error)\n");
          myvendor_mtp_lfs_set_link(MYVENDOR_MTP_LINK_OFF);
        }

      if (myvendor_mtp_worker_should_exit())
        {
          break;
        }
    }

  usb_close();
  myvendor_mtp_worker_detach();
  return 0;
}

/****************************************************************************
 * Public
 ****************************************************************************/

/**
 * @brief 按 PSRAM 容量计算 catalog 上限。
 */

static unsigned mtp_compute_max_objects(size_t arena_bytes)
{
  const size_t fixed = sizeof(struct mtp_storage_cache) * MTP_STORAGE_MAX
                       + (size_t)MTP_IO_CHUNK
                       + sizeof(struct mtp_scratch) + sizeof(uint32_t);
  const size_t per_obj = sizeof(struct mtp_obj)
                         + sizeof(struct catalog_ph_snap)
                         + sizeof(struct mtp_catalog_meta) + sizeof(uint32_t);
  unsigned n;

  if (arena_bytes <= fixed || per_obj == 0)
    {
      return MTP_MAX_OBJECTS_MIN;
    }

  n = (unsigned)((arena_bytes - fixed) / per_obj);
  if (n < MTP_MAX_OBJECTS_MIN)
    {
      n = MTP_MAX_OBJECTS_MIN;
    }

  return n;
}

/**
 * @brief 在 PSRAM 分配大缓冲。
 */

static int mtp_buffers_init(void)
{
  unsigned cap;
  unsigned i;

  mtp_storage_init();
  mtp_psram_init();
  cap = mtp_compute_max_objects(MTP_PSRAM_ARENA_BYTES);
  g_mtp_max_objects = cap;

  g_ctx.catalog = mtp_psram_calloc(cap, sizeof(struct mtp_obj));
  g_catalog_ph_snap =
    mtp_psram_calloc(cap, sizeof(struct catalog_ph_snap));
  g_cat_meta = mtp_psram_calloc(cap, sizeof(struct mtp_catalog_meta));
  g_storage_cache = mtp_psram_calloc(MTP_STORAGE_MAX,
                                     sizeof(struct mtp_storage_cache));
  g_io_buf = mtp_psram_malloc(MTP_IO_CHUNK);
  g_mtp_scratch = mtp_psram_calloc(1, sizeof(struct mtp_scratch));

  if (g_ctx.catalog == NULL || g_catalog_ph_snap == NULL ||
      g_cat_meta == NULL || g_storage_cache == NULL ||
      g_io_buf == NULL || g_mtp_scratch == NULL)
    {
      syslog(LOG_ERR, "mtp_simple: PSRAM buffer alloc failed\n");
      return -1;
    }

  for (i = 0; i < mtp_storage_count() && i < MTP_STORAGE_MAX; i++)
    {
      const struct mtp_store *s = mtp_storage_at(i);

      g_storage_cache[i].cap     = mtp_mtd_window_bytes(s ? s->mtddev : NULL);
      g_storage_cache[i].freeb   = g_storage_cache[i].cap;
      g_storage_cache[i].stat_ms = 1;
    }

#if MTP_FEAT_INFO
  syslog(LOG_INFO,
         "mtp_simple: PSRAM arena %u KiB, catalog cap %u objects "
         "(catalog %u snap %u meta %u io %u scratch %u, bump used %u)\n",
         (unsigned)(MTP_PSRAM_ARENA_BYTES / 1024), cap,
         (unsigned)(cap * sizeof(struct mtp_obj)),
         (unsigned)(cap * sizeof(struct catalog_ph_snap)),
         (unsigned)(cap * sizeof(struct mtp_catalog_meta)),
         (unsigned)MTP_IO_CHUNK,
         (unsigned)sizeof(struct mtp_scratch),
         (unsigned)mtp_psram_used_bytes());
#endif
  return 0;
}

/**
 * @brief mtp_simple 任务入口。
 */

int mtp_simple_main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  if (myvendor_mtp_worker_attach() != 0)
    {
      syslog(LOG_ERR, "mtp_simple: duplicate worker, exiting");
      return EXIT_FAILURE;
    }

  memset(&g_ctx, 0, sizeof(g_ctx));
  g_ctx.ep0 = g_ctx.ep_in = g_ctx.ep_out = g_ctx.ep_int = -1;
  g_ctx.upload_fd = -1;
  g_ctx.next_handle = MTP_HANDLE_BASE;

  if (mtp_buffers_init() < 0)
    {
      return EXIT_FAILURE;
    }

  return mtp_simple_loop();
}
