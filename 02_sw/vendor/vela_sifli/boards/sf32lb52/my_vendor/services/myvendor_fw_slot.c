/**
 * @file myvendor_fw_slot.c
 * @brief /mnt/kv/fw 只保留两份产品固件；排序规则与 2SFBL 文件名 X.Y.Z 一致。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_fw_slot.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

struct fw_cand
{
  char     name[MYVENDOR_FW_SLOT_NAME_MAX];
  uint32_t ver[3];
  uint32_t mtime;
};

static int name_is_bin(const char *name)
{
  size_t n;

  if (name == NULL)
    {
      return 0;
    }

  n = strlen(name);
  if (n < 5 || n >= MYVENDOR_FW_SLOT_NAME_MAX)
    {
      return 0;
    }

  return name[n - 4] == '.' &&
         (name[n - 3] == 'b' || name[n - 3] == 'B') &&
         (name[n - 2] == 'i' || name[n - 2] == 'I') &&
         (name[n - 1] == 'n' || name[n - 1] == 'N');
}

static void parse_ver_name(const char *name, uint32_t ver[3])
{
  const char *p = name;
  unsigned i;

  ver[0] = ver[1] = ver[2] = 0;
  if (*p == 'v' || *p == 'V')
    {
      p++;
    }

  for (i = 0; i < 3; i++)
    {
      uint32_t v = 0;

      if (*p < '0' || *p > '9')
        {
          break;
        }

      while (*p >= '0' && *p <= '9')
        {
          v = v * 10u + (uint32_t)(*p - '0');
          p++;
        }

      ver[i] = v;
      if (*p != '.')
        {
          break;
        }

      p++;
    }
}

static int cand_newer(const struct fw_cand *a, const struct fw_cand *b)
{
  unsigned i;

  for (i = 0; i < 3; i++)
    {
      if (a->ver[i] != b->ver[i])
        {
          return a->ver[i] > b->ver[i];
        }
    }

  if (a->mtime != b->mtime)
    {
      return a->mtime > b->mtime;
    }

  return strcmp(a->name, b->name) > 0;
}

static void cand_insert(struct fw_cand *cands, unsigned *n, unsigned max,
                        const struct fw_cand *in)
{
  unsigned i;
  unsigned j;

  for (i = 0; i < *n; i++)
    {
      if (cand_newer(in, &cands[i]))
        {
          break;
        }
    }

  if (*n < max)
    {
      j = *n;
      (*n)++;
    }
  else if (i >= max)
    {
      return;
    }
  else
    {
      j = max - 1u;
    }

  while (j > i)
    {
      cands[j] = cands[j - 1u];
      j--;
    }

  cands[i] = *in;
}

static unsigned scan_bins(struct fw_cand *cands, unsigned maxn)
{
  DIR *dir;
  struct dirent *de;
  unsigned n = 0;

  dir = opendir(MYVENDOR_FW_SLOT_DIR);
  if (dir == NULL)
    {
      return 0;
    }

  while ((de = readdir(dir)) != NULL)
    {
      char path[sizeof(MYVENDOR_FW_SLOT_DIR) + MYVENDOR_FW_SLOT_NAME_MAX];
      struct stat st;
      struct fw_cand in;

      if (!name_is_bin(de->d_name))
        {
          continue;
        }

      memset(&in, 0, sizeof(in));
      strncpy(in.name, de->d_name, sizeof(in.name) - 1u);
      parse_ver_name(in.name, in.ver);
      snprintf(path, sizeof(path), "%s/%s", MYVENDOR_FW_SLOT_DIR, in.name);
      if (stat(path, &st) == 0)
        {
          in.mtime = (uint32_t)st.st_mtime;
        }

      cand_insert(cands, &n, maxn, &in);
    }

  closedir(dir);
  return n;
}

static void unlink_sidecar(const char *bin_name)
{
  char path[sizeof(MYVENDOR_FW_SLOT_DIR) + MYVENDOR_FW_SLOT_NAME_MAX];
  char txt[MYVENDOR_FW_SLOT_NAME_MAX];
  size_t n;

  n = strlen(bin_name);
  if (n < 5 || n >= sizeof(txt))
    {
      return;
    }

  memcpy(txt, bin_name, n - 4u);
  memcpy(txt + n - 4u, ".txt", 5);
  snprintf(path, sizeof(path), "%s/%s", MYVENDOR_FW_SLOT_DIR, txt);
  (void)unlink(path);
}

static int unlink_bin(const char *name)
{
  char path[sizeof(MYVENDOR_FW_SLOT_DIR) + MYVENDOR_FW_SLOT_NAME_MAX];
  int ret;

  snprintf(path, sizeof(path), "%s/%s", MYVENDOR_FW_SLOT_DIR, name);
  ret = unlink(path);
  if (ret == 0)
    {
      unlink_sidecar(name);
      syslog(LOG_INFO, "fw_slot: removed %s\n", name);
    }

  return ret;
}

unsigned myvendor_fw_slot_list(struct myvendor_fw_slot_info *out,
                               unsigned maxn)
{
  struct fw_cand cands[MYVENDOR_FW_SLOT_SCAN_MAX];
  unsigned n;
  unsigned i;

  n = scan_bins(cands, MYVENDOR_FW_SLOT_SCAN_MAX);
  if (out == NULL || maxn == 0)
    {
      return n;
    }

  if (n > maxn)
    {
      n = maxn;
    }

  for (i = 0; i < n; i++)
    {
      memset(&out[i], 0, sizeof(out[i]));
      strncpy(out[i].name, cands[i].name, sizeof(out[i].name) - 1u);
      memcpy(out[i].ver, cands[i].ver, sizeof(out[i].ver));
    }

  return n;
}

int myvendor_fw_slot_prune(void)
{
  int removed = 0;
  unsigned first = 0;

  for (;;)
    {
      DIR *dir;
      struct dirent *de;
      struct fw_cand oldest;
      unsigned count = 0;
      int have = 0;

      dir = opendir(MYVENDOR_FW_SLOT_DIR);
      if (dir == NULL)
        {
          return removed;
        }

      memset(&oldest, 0, sizeof(oldest));
      while ((de = readdir(dir)) != NULL)
        {
          char path[sizeof(MYVENDOR_FW_SLOT_DIR) + MYVENDOR_FW_SLOT_NAME_MAX];
          struct stat st;
          struct fw_cand in;

          if (!name_is_bin(de->d_name))
            {
              continue;
            }

          memset(&in, 0, sizeof(in));
          strncpy(in.name, de->d_name, sizeof(in.name) - 1u);
          parse_ver_name(in.name, in.ver);
          snprintf(path, sizeof(path), "%s/%s", MYVENDOR_FW_SLOT_DIR, in.name);
          if (stat(path, &st) == 0)
            {
              in.mtime = (uint32_t)st.st_mtime;
            }

          count++;
          if (!have || cand_newer(&oldest, &in))
            {
              oldest = in;
              have = 1;
            }
        }

      closedir(dir);

      if (first == 0)
        {
          first = count;
          if (count > MYVENDOR_FW_SLOT_KEEP)
            {
              syslog(LOG_INFO, "fw_slot: prune %u -> %u\n",
                     count, (unsigned)MYVENDOR_FW_SLOT_KEEP);
            }
        }

      if (count <= MYVENDOR_FW_SLOT_KEEP || !have)
        {
          return removed;
        }

      if (unlink_bin(oldest.name) == 0)
        {
          removed++;
        }
      else
        {
          return removed;
        }
    }
}

int myvendor_fw_slot_on_commit(const char *path)
{
  const char *base;
  size_t prefix = sizeof(MYVENDOR_FW_SLOT_DIR) - 1u;

  if (path == NULL)
    {
      return 0;
    }

  if (strncmp(path, MYVENDOR_FW_SLOT_DIR, prefix) != 0)
    {
      return 0;
    }

  if (path[prefix] != '/')
    {
      return 0;
    }

  base = path + prefix + 1u;
  if (strchr(base, '/') != NULL || !name_is_bin(base))
    {
      return 0;
    }

  return myvendor_fw_slot_prune();
}

int myvendor_fw_slot_delete_oldest(char *deleted_name, size_t n)
{
  struct fw_cand cands[MYVENDOR_FW_SLOT_SCAN_MAX];
  unsigned count;
  unsigned last;

  count = scan_bins(cands, MYVENDOR_FW_SLOT_SCAN_MAX);
  if (count == 0)
    {
      return -ENOENT;
    }

  if (count < 2)
    {
      return -EBUSY;
    }

  last = count - 1u;
  if (unlink_bin(cands[last].name) != 0)
    {
      return errno != 0 ? -errno : -EIO;
    }

  if (deleted_name != NULL && n > 0)
    {
      strncpy(deleted_name, cands[last].name, n - 1u);
      deleted_name[n - 1u] = '\0';
    }

  return 0;
}
