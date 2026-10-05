/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tool: pack a directory into a raw LittleFS image for SF32LB52
 * FS_REGION. Geometry MUST match the NuttX MTD mount of the target medium:
 *   SD/eMMC (sf32lb_sdio.c): read/prog = 512,  block = 4096
 *   SPI NAND (sf32lb_nand.c): read/prog = 2048, block = 131072
 * Pass -r/-b to select; -s sets the image size (SD ~4 GiB, NAND 100 MiB).
 *
 * Host images are sparse: erase punches holes (or no-ops) so multi-GiB
 * geometries do not materialize a dense file. gen_fs_root_sparse.py then
 * extracts only used blocks for flash / pack-sd-img.
 *
 * NOT the SiFli SDK mklfsimg_nand (dhara FTL) format — that targets RT-Thread.
 */

#define _GNU_SOURCE

#include "lfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Defaults target SD/eMMC; -r/-b switch to the NAND page/block geometry. */

#define LFS_PROG_SIZE_DEFAULT   512u
#define LFS_BLOCK_SIZE_DEFAULT  4096u
#define LFS_IMAGE_SIZE_DEFAULT  (100u * 1024u * 1024u)

static uint32_t g_prog_size = LFS_PROG_SIZE_DEFAULT;
static uint32_t g_block_size = LFS_BLOCK_SIZE_DEFAULT;

/* Must match NuttX CONFIG_FS_LITTLEFS_NAME_MAX / CONFIG_NAME_MAX. */
#define MKLFS_NAME_MAX  128u

static int g_image_fd = -1;
static off_t g_high_water = 0; /* highest exclusive offset written by prog */

static int file_read(const struct lfs_config *cfg, lfs_block_t block,
                     lfs_off_t off, void *buffer, lfs_size_t size)
{
  off_t pos = (off_t)block * cfg->block_size + (off_t)off;

  (void)cfg;
  if (lseek(g_image_fd, pos, SEEK_SET) < 0)
    {
      return LFS_ERR_IO;
    }

  while (size > 0)
    {
      ssize_t n = read(g_image_fd, buffer, size);

      if (n < 0)
        {
          return LFS_ERR_IO;
        }

      if (n == 0)
        {
          /* Past truncated host image: treat as erased (0x00). */
          memset(buffer, 0, size);
          return 0;
        }

      buffer = (uint8_t *)buffer + n;
      size -= (lfs_size_t)n;
    }

  return 0;
}

static int file_prog(const struct lfs_config *cfg, lfs_block_t block,
                     lfs_off_t off, const void *buffer, lfs_size_t size)
{
  off_t pos = (off_t)block * cfg->block_size + (off_t)off;
  lfs_size_t remain = size;
  const uint8_t *p = buffer;

  (void)cfg;
  if (lseek(g_image_fd, pos, SEEK_SET) < 0)
    {
      return LFS_ERR_IO;
    }

  while (remain > 0)
    {
      ssize_t n = write(g_image_fd, p, remain);

      if (n < 0)
        {
          return LFS_ERR_IO;
        }

      p += n;
      remain -= (lfs_size_t)n;
    }

  {
    off_t end = pos + (off_t)size;

    if (end > g_high_water)
      {
        g_high_water = end;
      }
  }

  return 0;
}

static int file_erase(const struct lfs_config *cfg, lfs_block_t block)
{
  /* Host image starts as a sparse zero file (ftruncate). lfs_format would
   * otherwise punch/erase every block (~32k for a 4 GiB FS) and dominate
   * build-fs time. Zeros are treated as blank by gen_fs_root_sparse. */
  (void)cfg;
  (void)block;
  return 0;
}

static int file_sync(const struct lfs_config *cfg)
{
  /* Defer fsync to process exit — per-op sync on a multi-GiB sparse file
   * is extremely slow and unnecessary for a one-shot host packer. */
  (void)cfg;
  return 0;
}

static int pack_file(lfs_t *lfs, const char *root, const char *relpath)
{
  char src[512];
  char dst[384];
  struct stat st;
  uint8_t *buf;
  lfs_file_t file;
  int fd;
  ssize_t n;
  int err;

  snprintf(src, sizeof(src), "%s/%s", root, relpath);
  snprintf(dst, sizeof(dst), "/%s", relpath);

  if (stat(src, &st) != 0)
    {
      fprintf(stderr, "stat(%s) failed\n", src);
      return -1;
    }

  if (S_ISDIR(st.st_mode))
    {
      return 0;
    }

  buf = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
  if (buf == NULL)
    {
      return -1;
    }

  fd = open(src, O_RDONLY);
  if (fd < 0)
    {
      free(buf);
      return -1;
    }

  n = read(fd, buf, (size_t)st.st_size);
  close(fd);
  if (n < 0)
    {
      free(buf);
      return -1;
    }

  err = lfs_file_open(lfs, &file, dst, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
  if (err < 0)
    {
      fprintf(stderr, "lfs_file_open(%s) failed: %d\n", dst, err);
      free(buf);
      return -1;
    }

  if (n > 0)
    {
      err = lfs_file_write(lfs, &file, buf, (lfs_size_t)n);
      if (err < 0)
        {
          lfs_file_close(lfs, &file);
          free(buf);
          return -1;
        }
    }

  lfs_file_close(lfs, &file);
  free(buf);
  return 0;
}

static int pack_tree(lfs_t *lfs, const char *root, const char *relpath)
{
  char path[512];
  DIR *dir;
  struct dirent *ent;
  int ret = 0;

  if (relpath[0] == '\0')
    {
      snprintf(path, sizeof(path), "%s", root);
    }
  else
    {
      snprintf(path, sizeof(path), "%s/%s", root, relpath);
    }

  dir = opendir(path);
  if (dir == NULL)
    {
      fprintf(stderr, "opendir(%s) failed\n", path);
      return -1;
    }

  while ((ent = readdir(dir)) != NULL)
    {
      char child[512];
      struct stat st;

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
          continue;
        }

      if (relpath[0] == '\0')
        {
          snprintf(child, sizeof(child), "%s", ent->d_name);
        }
      else
        {
          snprintf(child, sizeof(child), "%s/%s", relpath, ent->d_name);
        }

      snprintf(path, sizeof(path), "%s/%s", root, child);
      if (stat(path, &st) != 0)
        {
          continue;
        }

      if (S_ISDIR(st.st_mode))
        {
          char lfsdir[384];
          int merr;

          snprintf(lfsdir, sizeof(lfsdir), "/%s", child);
          merr = lfs_mkdir(lfs, lfsdir);
          if (merr < 0 && merr != LFS_ERR_EXIST)
            {
              fprintf(stderr, "lfs_mkdir(%s) failed: %d\n", lfsdir, merr);
              ret = -1;
              break;
            }

          if (pack_tree(lfs, root, child) < 0)
            {
              ret = -1;
              break;
            }
        }
      else
        {
          if (pack_file(lfs, root, child) < 0)
            {
              ret = -1;
              break;
            }
        }
    }

  closedir(dir);
  return ret;
}

static void usage(const char *prog)
{
  fprintf(stderr,
          "Usage: %s -c <pack-dir> -i <output.bin> [-s <image-bytes>]\n"
          "          [-r <read/prog-size>] [-b <block-size>]\n"
          "  -s  FS image size in bytes (hex/dec), multiple of block size\n"
          "      default %u (100 MiB)\n"
          "  -r  read/prog size, default %u (SD); NAND uses 2048\n"
          "  -b  erase block size, default %u (SD); NAND uses 131072\n",
          prog, LFS_IMAGE_SIZE_DEFAULT,
          LFS_PROG_SIZE_DEFAULT, LFS_BLOCK_SIZE_DEFAULT);
}

static int parse_u32(const char *s, uint32_t *out)
{
  char *end = NULL;
  unsigned long long v;

  errno = 0;
  v = strtoull(s, &end, 0);
  if (errno || end == s || *end != '\0' || v > 0xffffffffULL)
    {
      return -1;
    }

  *out = (uint32_t)v;
  return 0;
}

int main(int argc, char **argv)
{
  const char *pack_dir = NULL;
  const char *out_path = NULL;
  uint32_t image_size = LFS_IMAGE_SIZE_DEFAULT;
  uint32_t block_count;
  uint32_t lookahead;
  struct lfs_config cfg;
  lfs_t lfs;
  uint8_t *cache;
  uint8_t *lookahead_buf;
  uint8_t *progbuf;
  int opt;
  int err;

  while ((opt = getopt(argc, argv, "c:i:s:r:b:h")) != -1)
    {
      switch (opt)
        {
        case 'c':
          pack_dir = optarg;
          break;
        case 'i':
          out_path = optarg;
          break;
        case 's':
          if (parse_u32(optarg, &image_size) < 0)
            {
              fprintf(stderr, "invalid -s size: %s\n", optarg);
              return 1;
            }
          break;
        case 'r':
          if (parse_u32(optarg, &g_prog_size) < 0 || g_prog_size == 0)
            {
              fprintf(stderr, "invalid -r size: %s\n", optarg);
              return 1;
            }
          break;
        case 'b':
          if (parse_u32(optarg, &g_block_size) < 0 || g_block_size == 0)
            {
              fprintf(stderr, "invalid -b size: %s\n", optarg);
              return 1;
            }
          break;
        default:
          usage(argv[0]);
          return 1;
        }
    }

  if (pack_dir == NULL || out_path == NULL)
    {
      usage(argv[0]);
      return 1;
    }

  if ((g_block_size % g_prog_size) != 0)
    {
      fprintf(stderr, "block %u must be a multiple of read/prog %u\n",
              g_block_size, g_prog_size);
      return 1;
    }

  if (image_size < g_block_size || (image_size % g_block_size) != 0)
    {
      fprintf(stderr,
              "size 0x%x must be non-zero multiple of block %u\n",
              image_size, g_block_size);
      return 1;
    }

  block_count = image_size / g_block_size;
  lookahead = 256;
  if (block_count > 4096)
    {
      lookahead = 1024;
    }

  g_image_fd = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (g_image_fd < 0)
    {
      perror(out_path);
      return 1;
    }

  if (ftruncate(g_image_fd, (off_t)image_size) != 0)
    {
      perror("ftruncate");
      return 1;
    }

  cache = calloc(1, g_prog_size);
  progbuf = calloc(1, g_prog_size);
  lookahead_buf = calloc(1, lookahead);
  if (cache == NULL || progbuf == NULL || lookahead_buf == NULL)
    {
      return 1;
    }

  memset(&cfg, 0, sizeof(cfg));
  cfg.read = file_read;
  cfg.prog = file_prog;
  cfg.erase = file_erase;
  cfg.sync = file_sync;
  cfg.read_size = g_prog_size;
  cfg.prog_size = g_prog_size;
  cfg.block_size = g_block_size;
  cfg.block_count = block_count;
  cfg.cache_size = g_prog_size;
  cfg.lookahead_size = lookahead;
  cfg.block_cycles = 500;
  cfg.name_max = MKLFS_NAME_MAX;
  cfg.read_buffer = cache;
  cfg.prog_buffer = progbuf;
  cfg.lookahead_buffer = lookahead_buf;

  fprintf(stderr,
          "mklfs: size=0x%x (%u MiB), read/prog=%u, block=%u, blocks=%u, "
          "lookahead=%u\n",
          image_size, image_size / (1024u * 1024u), g_prog_size,
          g_block_size, block_count, lookahead);

  err = lfs_format(&lfs, &cfg);
  if (err < 0)
    {
      fprintf(stderr, "lfs_format failed: %d\n", err);
      return 1;
    }

  err = lfs_mount(&lfs, &cfg);
  if (err < 0)
    {
      fprintf(stderr, "lfs_mount failed: %d\n", err);
      return 1;
    }

  /* Prefer low block numbers so host fs_root.bin / pack-sd-img trim small. */
  lfs.free.off = 0;
  lfs.free.i = 0;
  lfs.free.ack = block_count;

  if (pack_tree(&lfs, pack_dir, "") < 0)
    {
      fprintf(stderr, "pack failed\n");
      return 1;
    }

  lfs_unmount(&lfs);

  /* Shrink host file to used payload only. Superblock still advertises the
   * full block_count; on-device MTD size comes from ptab, not this file. */
  {
    off_t keep = g_high_water;

    if (keep < (off_t)(2 * g_block_size))
      {
        keep = (off_t)(2 * g_block_size);
      }

    keep = ((keep + (off_t)g_block_size - 1) / (off_t)g_block_size) *
           (off_t)g_block_size;
    if (keep > (off_t)image_size)
      {
        keep = (off_t)image_size;
      }

    if (ftruncate(g_image_fd, keep) != 0)
      {
        perror("ftruncate(shrink)");
        return 1;
      }

    fprintf(stderr,
            "mklfs: host file shrunk to %lld bytes (geometry still %u blocks)\n",
            (long long)keep, block_count);
  }

  if (fsync(g_image_fd) != 0)
    {
      perror("fsync");
      return 1;
    }

  close(g_image_fd);
  printf("Wrote %s (geometry 0x%x / %u blocks, host file truncated) from %s\n",
         out_path, image_size, block_count, pack_dir);
  return 0;
}
