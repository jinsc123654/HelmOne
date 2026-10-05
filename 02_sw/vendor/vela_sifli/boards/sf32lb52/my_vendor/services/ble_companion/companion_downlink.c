/**
 * @file companion_downlink.c
 * @brief 重组 0xFF17 / 0xFF19 分片。
 *
 * 重组后的 payload 为 TLV。TAG_ICON 只带文件名；缺文件时发 FS_NEED。
 * NSH `test notif on` / `test ctrl on` 打印到达的数据。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "companion_bridge.h"
#include "companion_downlink.h"
#include "companion_fs.h"
#include "companion_proto.h"
#include "transfer_backend.h"
#include "board_malloc.h"

/**
 * @brief 本模块日志出口（官方规则：服务层统一走 `syslog()`，不用 `printf`）。
 *
 * @details 前缀固定 `ble_companion: `；正常收包/结束用 INFO，真故障才 ERROR
 *          （`/mnt/kv/diag` 归档只收 WARNING 及以上，别把例行事件写成 ERROR）。
 */
#define LOGI(fmt, ...) syslog(LOG_INFO, "ble_companion: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) syslog(LOG_ERR, "ble_companion: " fmt "\n", ##__VA_ARGS__)

struct frag_acc
{
  uint8_t  msg_id;
  uint8_t  type;
  uint8_t  flags_or;
  uint16_t total;
  uint16_t filled;
  bool     in_use;
  uint8_t *data;
};

static struct frag_acc g_notif;
static struct frag_acc g_nav;

/**
 * @brief 分片载荷放 BoardPSRAM。
 */
static bool frag_acc_ensure(struct frag_acc *acc, uint16_t max_total)
{
  if (acc->data != NULL)
    {
      return true;
    }

  acc->data = board_malloc_psram(max_total);
  if (acc->data == NULL)
    {
      return false;
    }

  memset(acc->data, 0, max_total);
  return true;
}

/**
 * @brief 读出小端 uint16。
 */
static uint16_t get_le16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/**
 * @brief 读出小端 uint32。
 */
static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief 把 @p len 字节拷进以 NUL 结尾的缓冲区。
 */
static void copy_trunc(char *dst, size_t dstsz, const uint8_t *src, uint16_t len)
{
  size_t n;

  if (dstsz == 0)
    {
      return;
    }

  n = (size_t)len;
  if (n >= dstsz)
    {
      n = dstsz - 1;
    }

  memcpy(dst, src, n);
  dst[n] = '\0';
}

/**
 * @brief 去掉用作字段分隔的尾部 '|'。
 */
static void strip_bar(char *s)
{
  char *p;

  for (p = s; *p != '\0'; p++)
    {
      if (*p == '|')
        {
          *p = '/';
        }
    }
}

/**
 * @brief 累加一次分片写；LAST 时置 complete。
 */
static bool frag_feed(struct frag_acc *acc, const uint8_t *value, uint16_t length,
                      uint16_t max_total, bool *complete)
{
  uint8_t  flags;
  uint8_t  msg_id;
  uint16_t total;
  uint16_t off;
  uint16_t pay_len;

  *complete = false;

  if (!frag_acc_ensure(acc, max_total))
    {
      return false;
    }

  if (length < COMPANION_NOTIF_HDR_LEN)
    {
      return false;
    }

  if (value[0] != COMPANION_NOTIF_HDR_VER)
    {
      return false;
    }

  flags   = value[2];
  msg_id  = value[3];
  total   = get_le16(value + 4);
  off     = get_le16(value + 6);
  pay_len = (uint16_t)(length - COMPANION_NOTIF_HDR_LEN);

  if (total == 0 || total > max_total || off > total)
    {
      acc->in_use = false;
      return false;
    }

  if ((flags & COMPANION_NOTIF_FLAG_FIRST) != 0)
    {
      acc->in_use   = true;
      acc->msg_id   = msg_id;
      acc->type     = value[1];
      acc->flags_or = flags;
      acc->total    = total;
      acc->filled   = 0;
    }
  else if (!acc->in_use || acc->msg_id != msg_id || acc->total != total)
    {
      return false;
    }
  else
    {
      acc->flags_or = (uint8_t)(acc->flags_or | flags);
    }

  if (off != acc->filled)
    {
      acc->in_use = false;
      return false;
    }

  if ((uint32_t)off + pay_len > acc->total)
    {
      acc->in_use = false;
      return false;
    }

  memcpy(acc->data + off, value + COMPANION_NOTIF_HDR_LEN, pay_len);
  acc->filled = (uint16_t)(off + pay_len);

  if ((flags & COMPANION_NOTIF_FLAG_LAST) != 0)
    {
      if (acc->filled != acc->total)
        {
          acc->in_use = false;
          return false;
        }

      *complete = true;
      acc->in_use = false;
    }

  return true;
}

/**
 * @brief 在重组后的通知载荷里查找 TLV 标签。
 */
static void tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag,
                     const uint8_t **val, uint16_t *vlen)
{
  uint16_t i = 0;

  *val  = NULL;
  *vlen = 0;

  while ((uint32_t)i + 3 <= len)
    {
      uint8_t  t = buf[i];
      uint16_t l = get_le16(buf + i + 1);

      i = (uint16_t)(i + 3);
      if ((uint32_t)i + l > len)
        {
          return;
        }

      if (t == tag)
        {
          *val  = buf + i;
          *vlen = l;
          return;
        }

      i = (uint16_t)(i + l);
    }
}

/**
 * @brief 清洗 Android 包名，用作图标文件名。
 */
static void sanitize_pkg(char *dst, size_t dstsz, const char *src)
{
  size_t i = 0;
  size_t o = 0;

  if (dstsz == 0)
    {
      return;
    }

  while (src[i] != '\0' && o + 1 < dstsz)
    {
      char c = src[i++];

      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
        {
          dst[o++] = c;
        }
      else
        {
          dst[o++] = '_';
        }
    }

  dst[o] = '\0';
}

/**
 * @brief 取 basename 再清洗，拒绝 `.` / `..`。
 */
static void icon_basename(char *dst, size_t dstsz, const char *src)
{
  const char *base = src;
  const char *p;

  for (p = src; *p != '\0'; p++)
    {
      if (*p == '/' || *p == '\\')
        {
          base = p + 1;
        }
    }

  sanitize_pkg(dst, dstsz, base);
  if (strcmp(dst, ".") == 0 || strcmp(dst, "..") == 0)
    {
      dst[0] = '\0';
    }
}

/**
 * @brief 一条完整的 0xFF17 消息；`test notif on` 时打印。
 */
static void notif_complete(const uint8_t *payload, uint16_t len, uint8_t type)
{
  const uint8_t *title_v = NULL;
  const uint8_t *body_v  = NULL;
  const uint8_t *pkg_v   = NULL;
  const uint8_t *icon_v  = NULL;
  const uint8_t *name_v  = NULL;
  uint16_t title_l = 0;
  uint16_t body_l  = 0;
  uint16_t pkg_l   = 0;
  uint16_t icon_l  = 0;
  uint16_t name_l  = 0;
  char title[COMPANION_NOTIF_TITLE_MAX + 1];
  char body[COMPANION_NOTIF_BODY_MAX + 1];
  char pkg[COMPANION_NOTIF_PACKAGE_MAX + 1];
  char icon_name[COMPANION_NOTIF_ICON_NAME_MAX + 1];
  char icon_abs[XFER_PATH_MAX];
  char rel[96];
  const char *show_title;
  bool found = false;

  tlv_find(payload, len, COMPANION_NOTIF_TAG_TITLE, &title_v, &title_l);
  tlv_find(payload, len, COMPANION_NOTIF_TAG_BODY, &body_v, &body_l);
  tlv_find(payload, len, COMPANION_NOTIF_TAG_PACKAGE, &pkg_v, &pkg_l);
  tlv_find(payload, len, COMPANION_NOTIF_TAG_ICON, &icon_v, &icon_l);
  tlv_find(payload, len, COMPANION_NOTIF_TAG_APP_NAME, &name_v, &name_l);

  title[0] = '\0';
  body[0]  = '\0';
  pkg[0]   = '\0';
  icon_name[0] = '\0';
  icon_abs[0] = '\0';

  if (title_v != NULL)
    {
      copy_trunc(title, sizeof(title), title_v, title_l);
    }
  else if (name_v != NULL)
    {
      copy_trunc(title, sizeof(title), name_v, name_l);
    }

  if (body_v != NULL)
    {
      copy_trunc(body, sizeof(body), body_v, body_l);
    }

  if (pkg_v != NULL)
    {
      char raw[COMPANION_NOTIF_PACKAGE_MAX + 1];

      copy_trunc(raw, sizeof(raw), pkg_v, pkg_l);
      sanitize_pkg(pkg, sizeof(pkg), raw);
    }

  if (icon_v != NULL && icon_l > 0)
    {
      char raw[COMPANION_NOTIF_ICON_NAME_MAX + 1];

      copy_trunc(raw, sizeof(raw), icon_v, icon_l);
      icon_basename(icon_name, sizeof(icon_name), raw);
    }
  else if (pkg[0] != '\0')
    {
      snprintf(icon_name, sizeof(icon_name), "%s.png", pkg);
      if (strcmp(icon_name, ".") == 0 || strcmp(icon_name, "..") == 0)
        {
          icon_name[0] = '\0';
        }
    }

  strip_bar(title);
  strip_bar(body);

  show_title = title[0] != '\0' ? title : "通知";
  if (body[0] == '\0')
    {
      strncpy(body, show_title, sizeof(body) - 1);
      body[sizeof(body) - 1] = '\0';
    }

  if (companion_bridge_test_notif_get())
    {
      LOGI("notif type=%u title=%s body=%s icon=%s", (unsigned)type,
           show_title, body, icon_name[0] != '\0' ? icon_name : "-");
    }

  if (!companion_bridge_policy_notif())
    {
      return;
    }

  if (companion_bridge_policy_calls_only() &&
      type != COMPANION_NOTIF_TYPE_CALL)
    {
      return;
    }

  if (icon_name[0] != '\0')
    {
      snprintf(rel, sizeof(rel), "%s/%s", COMPANION_NOTIF_ICON_DIR, icon_name);
      if (xfer_resolve(rel, strlen(rel), icon_abs, sizeof(icon_abs)) == 0 &&
          companion_fs_icon_usable(icon_abs))
        {
          found = true;
        }
      else
        {
          companion_fs_need_file(icon_name);
          icon_abs[0] = '\0';
        }
    }

  {
    struct companion_notif_item item;

    memset(&item, 0, sizeof(item));
    item.type = type;
    strncpy(item.title, show_title, sizeof(item.title) - 1);
    strncpy(item.body, body, sizeof(item.body) - 1);
    if (found)
      {
        strncpy(item.icon, icon_abs, sizeof(item.icon) - 1);
      }

    companion_bridge_notif_post(&item);
  }
}

/**
 * @brief 一条完整的 0xFF19 折线；存进 bridge（不启动 UI 导航）。
 */
static void nav_complete(const uint8_t *payload, uint16_t len, uint8_t flags)
{
  uint8_t  n;
  uint8_t  i;
  int32_t  lat[COMPANION_NAV_MAX_PTS] = {0};
  int32_t  lon[COMPANION_NAV_MAX_PTS] = {0};

  if (len < COMPANION_NAV_PT_LEN || (len % COMPANION_NAV_PT_LEN) != 0)
    {
      LOGE("nav payload len %u not a multiple of %u", len,
           COMPANION_NAV_PT_LEN);
      return;
    }

  n = (uint8_t)(len / COMPANION_NAV_PT_LEN);
  if (n > COMPANION_NAV_MAX_PTS)
    {
      n = COMPANION_NAV_MAX_PTS;
    }

  for (i = 0; i < n; i++)
    {
      lat[i] = (int32_t)get_le32(payload + (uint16_t)i * COMPANION_NAV_PT_LEN);
      lon[i] = (int32_t)get_le32(payload + (uint16_t)i * COMPANION_NAV_PT_LEN + 4);
    }

  companion_bridge_nav_store(lat, lon, n);
  if (companion_bridge_test_ctrl_get())
    {
      LOGI("nav %u pts start=%d (print only, no UI)", n,
           (flags & COMPANION_NAV_FLAG_START) != 0);
      for (i = 0; i < n && i < 8; i++)
        {
          LOGI("  [%u] lat_e7=%ld lon_e7=%ld", i,
               (long)lat[i], (long)lon[i]);
        }

      if (n > 8)
        {
          LOGI("  ... %u more", (unsigned)(n - 8));
        }
    }
}

/**
 * @brief 处理一次 0xFF17 写（可能只是一个分片）。
 */
uint16_t companion_notif_on_write(const uint8_t *value, uint16_t length)
{
  bool complete = false;

  if (value == NULL)
    {
      return length;
    }

  if (!frag_feed(&g_notif, value, length, COMPANION_NOTIF_MAX_TOTAL, &complete))
    {
      return length;
    }

  if (complete)
    {
      notif_complete(g_notif.data, g_notif.total, g_notif.type);
    }

  return length;
}

/**
 * @brief 处理一次 0xFF19 写（可能只是一个分片）。
 */
uint16_t companion_nav_on_write(const uint8_t *value, uint16_t length)
{
  bool complete = false;

  if (value == NULL)
    {
      return length;
    }

  if (!frag_feed(&g_nav, value, length, COMPANION_NAV_MAX_TOTAL, &complete))
    {
      return length;
    }

  if (complete)
    {
      nav_complete(g_nav.data, g_nav.total, g_nav.flags_or);
    }

  return length;
}

/**
 * @brief 丢弃进行中的通知/导航分片（手机断开）。
 */
void companion_downlink_reset(void)
{
  g_notif.in_use = false;
  g_nav.in_use   = false;
}
