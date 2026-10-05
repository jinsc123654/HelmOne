/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host profiler: replay the on-device LittleFS mount against a raw fs_root.bin
 * and report exactly how many read() calls (and bytes) lfs_mount() performs.
 * Geometry mirrors the NuttX MTD mount so the numbers can be compared with the
 * SDPROF counters printed by sf32lb_sdio.c.
 *
 * Build (from openvela root):
 *   gcc -O2 -I nuttx/fs/littlefs/littlefs \
 *       vendor/my_vendor/scripts/mklfs_disk/lfs_mount_prof.c \
 *       nuttx/fs/littlefs/littlefs/lfs.c \
 *       nuttx/fs/littlefs/littlefs/lfs_util.c -o /tmp/lfs_mount_prof
 */

#define _GNU_SOURCE

#include "lfs.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fd = -1;
static unsigned long g_reads;
static unsigned long g_read_bytes;
static unsigned long g_erased_tail_reads;   /* reads that came back all-0x00 */
static int g_trace;

static int prof_read(const struct lfs_config *cfg, lfs_block_t block,
                     lfs_off_t off, void *buffer, lfs_size_t size)
{
  off_t pos = (off_t)block * cfg->block_size + (off_t)off;
  lfs_size_t remain = size;
  uint8_t *p = buffer;

  g_reads++;
  g_read_bytes += size;

  if (g_trace)
    {
      printf("  read #%lu block=%u off=%u size=%u\n",
             g_reads, block, off, size);
    }

  if (lseek(g_fd, pos, SEEK_SET) < 0)
    {
      return LFS_ERR_IO;
    }

  while (remain > 0)
    {
      ssize_t n = read(g_fd, p, remain);

      if (n < 0)
        {
          return LFS_ERR_IO;
        }

      if (n == 0)
        {
          memset(p, 0, remain);
          break;
        }

      p += n;
      remain -= (lfs_size_t)n;
    }

  {
    lfs_size_t i;

    for (i = 0; i < size; i++)
      {
        if (((uint8_t *)buffer)[i] != 0)
          {
            break;
          }
      }

    if (i == size)
      {
        g_erased_tail_reads++;
      }
  }

  return 0;
}

static int prof_prog(const struct lfs_config *cfg, lfs_block_t block,
                     lfs_off_t off, const void *buffer, lfs_size_t size)
{
  (void)cfg; (void)block; (void)off; (void)buffer; (void)size;
  printf("  !! prog during mount\n");
  return 0;
}

static int prof_erase(const struct lfs_config *cfg, lfs_block_t block)
{
  (void)cfg; (void)block;
  printf("  !! erase during mount\n");
  return 0;
}

static int prof_sync(const struct lfs_config *cfg)
{
  (void)cfg;
  return 0;
}

int main(int argc, char **argv)
{
  const char *path;
  uint32_t region_size;
  uint32_t read_size  = 2048;
  uint32_t block_size = 131072;
  struct lfs_config cfg;
  static uint8_t rbuf[8192];
  static uint8_t pbuf[8192];
  static uint8_t lbuf[8192];
  lfs_t lfs;
  int err;

  if (argc < 3)
    {
      fprintf(stderr,
              "Usage: %s <fs_root.bin> <region-size> "
              "[read_size] [block_size] [trace]\n"
              "  e.g. %s fs_root.bin 0xffa00000 2048 131072\n",
              argv[0], argv[0]);
      return 1;
    }

  path = argv[1];
  region_size = (uint32_t)strtoul(argv[2], NULL, 0);
  if (argc > 3)
    {
      read_size = (uint32_t)strtoul(argv[3], NULL, 0);
    }

  if (argc > 4)
    {
      block_size = (uint32_t)strtoul(argv[4], NULL, 0);
    }

  g_trace = (argc > 5);

  g_fd = open(path, O_RDONLY);
  if (g_fd < 0)
    {
      perror(path);
      return 1;
    }

  memset(&cfg, 0, sizeof(cfg));
  cfg.read  = prof_read;
  cfg.prog  = prof_prog;
  cfg.erase = prof_erase;
  cfg.sync  = prof_sync;
  cfg.read_size   = read_size;
  cfg.prog_size   = read_size;
  cfg.block_size  = block_size;
  cfg.block_count = region_size / block_size;
  cfg.cache_size  = read_size;
  cfg.lookahead_size = 2048;
  cfg.block_cycles   = 200;
  cfg.name_max       = 128;
  cfg.read_buffer     = rbuf;
  cfg.prog_buffer     = pbuf;
  cfg.lookahead_buffer = lbuf;

  printf("image=%s read/prog=%u block=%u count=%u (region 0x%x)\n",
         path, read_size, block_size, cfg.block_count, region_size);

  err = lfs_mount(&lfs, &cfg);
  printf("lfs_mount -> %d\n", err);
  printf("mount reads: %lu calls, %lu bytes (%lu KiB), "
         "%lu all-zero, ~%lu x 512B SD blocks\n",
         g_reads, g_read_bytes, g_read_bytes / 1024,
         g_erased_tail_reads, g_read_bytes / 512);

  if (err == 0)
    {
      lfs_dir_t dir;
      struct lfs_info info;
      unsigned long before = g_reads;

      if (lfs_dir_open(&lfs, &dir, "/") == 0)
        {
          while (lfs_dir_read(&lfs, &dir, &info) > 0)
            {
            }

          lfs_dir_close(&lfs, &dir);
          printf("readdir(/) cost %lu extra reads\n", g_reads - before);
        }

      lfs_unmount(&lfs);
    }

  close(g_fd);
  return err ? 1 : 0;
}
