/**
 * @file mtp_storage.c
 * @brief 构建 MTP 卷列表：主 Kconfig 路径 + EXTRA_PATHS。
 *
 * 表驱动 g_known 映射友好名称与 MTD 节点（GetStorageInfo 不用 statvfs）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp_storage.h"

#include "myvendor_gpx.h"
#include "myvendor_identity.h"

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH
#  define CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH "/mnt/lfs/mtp"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_DESC
#  define CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_DESC "LittleFS"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_VOLUME_LABEL
#  define CONFIG_MYVENDOR_MTP_SIMPLE_VOLUME_LABEL "LFS"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_EXTRA_PATHS
#  define CONFIG_MYVENDOR_MTP_SIMPLE_EXTRA_PATHS ""
#endif

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

/* Known mounts: labels + MTD node for GetStorageInfo (no statvfs).
 * Add a row here when a new board mount should show a friendly name.
 */

struct mtp_store_known
{
  const char *path;
  const char *desc;
  const char *label;
  const char *mtddev;
  bool        format_ok;
};

static const struct mtp_store_known g_known[] =
{
  { "/mnt/lfs/mtp", "Helm One", "Helm One", "/dev/sd0",  true  },
  { "/mnt/lfs",     "file",     "file",     "/dev/sd0",  false },
  { "/mnt/kv",      "kv",       "kv",       "/dev/sdkv", false },
  { "/mnt/fat",     "fat",      "fat",      "/dev/sdfat", false },
};

static struct mtp_store g_stores[MTP_STORAGE_MAX];
static unsigned g_nstore;

/** @brief 查已知挂载点元数据。 */
static const struct mtp_store_known *known_lookup(const char *path)
{
  unsigned i;
  unsigned best = 0;
  const struct mtp_store_known *match = NULL;

  if (path == NULL)
    {
      return NULL;
    }

  for (i = 0; i < (unsigned)(sizeof(g_known) / sizeof(g_known[0])); i++)
    {
      if (strcmp(g_known[i].path, path) == 0)
        {
          return &g_known[i];
        }
    }

  for (i = 0; i < (unsigned)(sizeof(g_known) / sizeof(g_known[0])); i++)
    {
      size_t n = strlen(g_known[i].path);

      if (n == 0 || n < best)
        {
          continue;
        }

      if (strncmp(path, g_known[i].path, n) == 0 && path[n] == '/')
        {
          best = n;
          match = &g_known[i];
        }
    }

  return match;
}

/** @brief 路径 basename。 */
static const char *path_basename(const char *path)
{
  const char *s = strrchr(path, '/');

  if (s == NULL || s[1] == '\0')
    {
      return path;
    }

  return s + 1;
}

/** @brief 路径是否已在卷表。 */
static bool store_path_used(const char *path)
{
  unsigned i;

  for (i = 0; i < g_nstore; i++)
    {
      if (strcmp(g_stores[i].path, path) == 0)
        {
          return true;
        }
    }

  return false;
}

/** @brief 追加一卷到 g_stores。 */
static void add_store(const char *path, const char *desc, const char *label,
                      const char *mtddev)
{
  struct mtp_store *s;
  const struct mtp_store_known *k;

  if (path == NULL || path[0] != '/' || g_nstore >= MTP_STORAGE_MAX ||
      store_path_used(path))
    {
      return;
    }

  k = known_lookup(path);
  s = &g_stores[g_nstore];
  memset(s, 0, sizeof(*s));
  s->index = g_nstore;
  s->id = 0x00010001u + g_nstore * 0x00010000u;
  s->root_handle = 1u + g_nstore;
  strncpy(s->path, path, sizeof(s->path) - 1);

  if (desc != NULL && desc[0] != '\0')
    {
      strncpy(s->desc, desc, sizeof(s->desc) - 1);
    }
  else if (k != NULL)
    {
      strncpy(s->desc, k->desc, sizeof(s->desc) - 1);
    }
  else
    {
      strncpy(s->desc, path, sizeof(s->desc) - 1);
    }

  if (label != NULL && label[0] != '\0')
    {
      strncpy(s->label, label, sizeof(s->label) - 1);
    }
  else if (k != NULL)
    {
      strncpy(s->label, k->label, sizeof(s->label) - 1);
    }
  else
    {
      strncpy(s->label, path_basename(path), sizeof(s->label) - 1);
    }

  if (mtddev != NULL && mtddev[0] != '\0')
    {
      strncpy(s->mtddev, mtddev, sizeof(s->mtddev) - 1);
    }
  else if (k != NULL && k->mtddev != NULL)
    {
      strncpy(s->mtddev, k->mtddev, sizeof(s->mtddev) - 1);
    }

  s->format_ok = (k != NULL) ? k->format_ok : true;

  g_nstore++;
}

/** @brief 解析逗号分隔 EXTRA_PATHS。 */
static void add_extra_paths(const char *list)
{
  char buf[256];
  char *save = NULL;
  char *tok;

  if (list == NULL || list[0] == '\0')
    {
      return;
    }

  strncpy(buf, list, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  tok = strtok_r(buf, ",;", &save);
  while (tok != NULL)
    {
      while (*tok == ' ' || *tok == '\t')
        {
          tok++;
        }

      if (*tok != '\0')
        {
          if (access(tok, R_OK) != 0)
            {
              tok = strtok_r(NULL, ",;", &save);
              continue;
            }

          add_store(tok, NULL, NULL, NULL);
        }

      tok = strtok_r(NULL, ",;", &save);
    }
}

/** @brief mkdir 各卷根（不存在则创建）。 */
void mtp_storage_ensure_dirs(void)
{
  unsigned i;

  for (i = 0; i < g_nstore; i++)
    {
      if (g_stores[i].path[0] != '\0')
        {
          if (mkdir(g_stores[i].path, 0755) != 0 && errno != EEXIST)
            {
              /* Mount may not be up yet; OpenSession retries this. */
            }
        }
    }

  (void)mkdir(MYVENDOR_GPX_MTP_ROOT, 0755);
  (void)mkdir(MYVENDOR_GPX_IMPORT_DIR, 0755);
  (void)mkdir(MYVENDOR_GPX_RECORD_DIR, 0755);
  (void)mkdir(MYVENDOR_NAVPTS_DIR, 0755);
}

/** @brief 构建卷表并 ensure_dirs。 */
void mtp_storage_init(void)
{
  const struct mtp_store_known *k;
  const char *cfg_desc = CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_DESC;
  const char *desc = cfg_desc;
  const char *label = CONFIG_MYVENDOR_MTP_SIMPLE_VOLUME_LABEL;
  const char *id;

  mtp_names_apply_identity();
  id = myvendor_identity_name();
  if (strcmp(desc, CONFIG_MYVENDOR_PRODUCT_NAME) == 0)
    {
      desc = id;
    }

  if (strcmp(label, CONFIG_MYVENDOR_PRODUCT_NAME) == 0)
    {
      label = id;
    }

  g_nstore = 0;
  k = known_lookup(CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH);

  /* Product disk (/mnt/lfs/mtp): Kconfig "Helm One" → MAC suffix.
   * Factory /mnt/lfs: g_known names (file). Extra volumes always use g_known.
   */
  if (k != NULL && strcmp(cfg_desc, CONFIG_MYVENDOR_PRODUCT_NAME) != 0)
    {
      desc = k->desc;
      label = k->label;
    }

  add_store(CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH, desc, label,
            k ? k->mtddev : "/dev/sd0");
  add_extra_paths(CONFIG_MYVENDOR_MTP_SIMPLE_EXTRA_PATHS);
  mtp_storage_ensure_dirs();
}

/** @brief 卷数量。 */
unsigned mtp_storage_count(void)
{
  return g_nstore > 0 ? g_nstore : 0;
}

/** @brief 按下标取卷。 */
const struct mtp_store *mtp_storage_at(unsigned i)
{
  if (i >= g_nstore)
    {
      return NULL;
    }

  return &g_stores[i];
}

/** @brief 按 storage id 取卷。 */
const struct mtp_store *mtp_storage_by_id(uint32_t id)
{
  unsigned i;

  for (i = 0; i < g_nstore; i++)
    {
      if (g_stores[i].id == id)
        {
          return &g_stores[i];
        }
    }

  return NULL;
}

/** @brief 按 root handle 取卷。 */
const struct mtp_store *mtp_storage_by_root(uint32_t handle)
{
  unsigned i;

  for (i = 0; i < g_nstore; i++)
    {
      if (g_stores[i].root_handle == handle)
        {
          return &g_stores[i];
        }
    }

  return NULL;
}

/** @brief 按 LFS 路径最长前缀匹配卷。 */
const struct mtp_store *mtp_storage_for_path(const char *path)
{
  unsigned i;
  unsigned best = 0;
  const struct mtp_store *match = NULL;

  if (path == NULL)
    {
      return mtp_storage_at(0);
    }

  for (i = 0; i < g_nstore; i++)
    {
      size_t n = strlen(g_stores[i].path);

      if (n == 0 || n < best)
        {
          continue;
        }

      if (strncmp(path, g_stores[i].path, n) == 0 &&
          (path[n] == '\0' || path[n] == '/'))
        {
          best = n;
          match = &g_stores[i];
        }
    }

  return match != NULL ? match : mtp_storage_at(0);
}

/** @brief id 是否表示“全部存储”。 */
bool mtp_storage_id_all(uint32_t id)
{
  return id == 0 || id == 0xffffffffu;
}

/** @brief id 是否为已知卷。 */
bool mtp_storage_id_known(uint32_t id)
{
  return mtp_storage_by_id(id) != NULL;
}
