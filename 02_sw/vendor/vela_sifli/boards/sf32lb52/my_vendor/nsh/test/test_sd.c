/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_sd.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Layer 3: POSIX/VFS R/W at /mnt/lfs/.sdtest (open/read/write/fsync/unlink).
 * Same path MTP uses.  Aliased as "test posix".  Does not touch the rest of
 * the volume.  Use this after layer 1 (test sdio) and layer 2 (test lfs):
 *
 *   test posix / test sd           seq + 4KiB-chunk + random + mix
 *   test posix seq                 sequential 1 MiB x 2 write/read/verify
 *   test posix chunk               1 MiB written in 4 KiB chunks then fsync
 *   test posix rnd                 200 random 4 KiB ops on a 256 KiB file
 *   test posix mix                 15 s create/append/read/unlink churn
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "test_demos.h"

#define SDTEST_MNT   "/mnt/lfs"
#define SDTEST_DIR   "/mnt/lfs/.sdtest"
#define SDTEST_CHUNK 4096

static const char *g_sdtest_tag = "posix";

#define LOGI(fmt, ...) \
  do \
    { \
      printf("test %s: " fmt "\n", g_sdtest_tag, ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)
#define LOGE(fmt, ...) \
  do \
    { \
      printf("test %s: ERROR " fmt "\n", g_sdtest_tag, ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)

void __attribute__((weak)) sf32lb_sd_diag_dump(const char *tag)
{
  (void)tag;
}

#if defined(CONFIG_FS_LITTLEFS)

static uint64_t sdtest_now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static void sdtest_fill(FAR uint8_t *buf, size_t n, uint32_t seed)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      buf[i] = (uint8_t)((seed + (uint32_t)i) * 0x9eu + 0x5d);
    }
}

static int sdtest_check(FAR const uint8_t *buf, size_t n, uint32_t seed)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      uint8_t expect = (uint8_t)((seed + (uint32_t)i) * 0x9eu + 0x5d);

      if (buf[i] != expect)
        {
          LOGE("mismatch off=%zu got=0x%02x expect=0x%02x seed=%lu",
               i, buf[i], expect, (unsigned long)seed);
          return -1;
        }
    }

  return 0;
}

static void sdtest_rate(FAR const char *tag, size_t bytes, uint64_t us)
{
  if (us == 0)
    {
      us = 1;
    }

  LOGI("%s %zu bytes in %lu ms -> %lu KB/s",
       tag, bytes, (unsigned long)(us / 1000),
       (unsigned long)((bytes * 1000ull) / us));
}

static int sdtest_prepare(void)
{
  FAR DIR *dir;
  int ret;

  /* statfs() on this volume is unbounded (lfs_fs_size over ~1M blocks). */

  LOGI("prepare: opendir %s", SDTEST_MNT);
  dir = opendir(SDTEST_MNT);
  if (dir == NULL)
    {
      LOGE("%s not mounted (errno=%d)", SDTEST_MNT, errno);
      return -1;
    }

  closedir(dir);
  LOGI("prepare: mkdir %s (first real LFS block alloc, may take a while)",
       SDTEST_DIR);
  ret = mkdir(SDTEST_DIR, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      LOGE("mkdir(%s) errno=%d", SDTEST_DIR, errno);
      return -1;
    }

  LOGI("prepare: ok");
  return 0;
}

static void sdtest_rm(FAR const char *path)
{
  if (unlink(path) < 0 && errno != ENOENT)
    {
      LOGE("unlink(%s) errno=%d", path, errno);
    }
}

/****************************************************************************
 * seq: one big write, fsync, read-verify, delete.  Repeats `rounds` times.
 ****************************************************************************/

static int sdtest_seq(size_t kib, int rounds)
{
  FAR uint8_t *buf;
  char path[64];
  size_t total = kib * 1024;
  int fail = 0;
  int r;
  int fd;

  buf = malloc(SDTEST_CHUNK);
  if (buf == NULL)
    {
      LOGE("oom");
      return 1;
    }

  snprintf(path, sizeof(path), "%s/seq.bin", SDTEST_DIR);
  LOGI("---- seq %zu KiB x %d ----", kib, rounds);

  for (r = 0; r < rounds; r++)
    {
      size_t off;
      uint64_t t0;
      uint64_t us;
      ssize_t n;

      sdtest_rm(path);
      LOGI("seq round %d: open %s", r + 1, path);
      fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0)
        {
          LOGE("seq open write errno=%d", errno);
          fail++;
          continue;
        }

      t0 = sdtest_now_us();
      for (off = 0; off < total; off += SDTEST_CHUNK)
        {
          size_t want = total - off;
          uint32_t seed = (uint32_t)(off ^ ((uint32_t)r << 16));

          if (want > SDTEST_CHUNK)
            {
              want = SDTEST_CHUNK;
            }

          sdtest_fill(buf, want, seed);
          n = write(fd, buf, want);
          if (n != (ssize_t)want)
            {
              LOGE("seq write off=%zu n=%zd errno=%d", off, n, errno);
              fail++;
              break;
            }

          if ((off & 0x7fff) == 0)
            {
              LOGI("  wrote %zu / %zu", off + want, total);
            }
        }

      LOGI("seq round %d: fsync", r + 1);
      fsync(fd);
      close(fd);
      us = sdtest_now_us() - t0;
      sdtest_rate("seq write", total, us);

      fd = open(path, O_RDONLY);
      if (fd < 0)
        {
          LOGE("seq open read errno=%d", errno);
          fail++;
          continue;
        }

      t0 = sdtest_now_us();
      for (off = 0; off < total; off += SDTEST_CHUNK)
        {
          size_t want = total - off;
          uint32_t seed = (uint32_t)(off ^ ((uint32_t)r << 16));

          if (want > SDTEST_CHUNK)
            {
              want = SDTEST_CHUNK;
            }

          n = read(fd, buf, want);
          if (n != (ssize_t)want)
            {
              LOGE("seq read off=%zu n=%zd errno=%d", off, n, errno);
              fail++;
              break;
            }

          if (sdtest_check(buf, want, seed) < 0)
            {
              fail++;
              break;
            }
        }

      close(fd);
      us = sdtest_now_us() - t0;
      sdtest_rate("seq read ", total, us);
      sdtest_rm(path);
    }

  free(buf);
  return fail;
}

/****************************************************************************
 * chunk: MTP SendObject shape — 4 KiB writes, one fsync at the end.
 ****************************************************************************/

static int sdtest_chunk(size_t kib)
{
  FAR uint8_t *buf;
  char path[64];
  size_t total = kib * 1024;
  size_t off;
  uint64_t t0;
  uint64_t us;
  int fail = 0;
  int fd;
  ssize_t n;

  buf = malloc(SDTEST_CHUNK);
  if (buf == NULL)
    {
      return 1;
    }

  snprintf(path, sizeof(path), "%s/chunk.bin", SDTEST_DIR);
  LOGI("---- chunk %zu KiB in 4 KiB writes + fsync (MTP-like) ----", kib);
  sdtest_rm(path);

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      LOGE("chunk open errno=%d", errno);
      free(buf);
      return 1;
    }

  t0 = sdtest_now_us();
  for (off = 0; off < total; off += SDTEST_CHUNK)
    {
      size_t want = total - off;

      if (want > SDTEST_CHUNK)
        {
          want = SDTEST_CHUNK;
        }

      sdtest_fill(buf, want, (uint32_t)off);
      n = write(fd, buf, want);
      if (n != (ssize_t)want)
        {
          LOGE("chunk write off=%zu n=%zd errno=%d", off, n, errno);
          fail++;
          break;
        }

      if ((off & 0xffff) == 0 && off != 0)
        {
          LOGI("  wrote %zu / %zu", off, total);
        }
    }

  fsync(fd);
  close(fd);
  us = sdtest_now_us() - t0;
  sdtest_rate("chunk write+fsync", off, us);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      LOGE("chunk reopen errno=%d", errno);
      free(buf);
      sdtest_rm(path);
      return fail + 1;
    }

  t0 = sdtest_now_us();
  for (off = 0; off < total && fail == 0; off += SDTEST_CHUNK)
    {
      size_t want = total - off;

      if (want > SDTEST_CHUNK)
        {
          want = SDTEST_CHUNK;
        }

      n = read(fd, buf, want);
      if (n != (ssize_t)want || sdtest_check(buf, want, (uint32_t)off) < 0)
        {
          LOGE("chunk verify off=%zu n=%zd errno=%d", off, n, errno);
          fail++;
          break;
        }
    }

  close(fd);
  us = sdtest_now_us() - t0;
  sdtest_rate("chunk read", off, us);
  sdtest_rm(path);
  free(buf);
  return fail;
}

/****************************************************************************
 * rnd: random 4 KiB write/read on a pre-sized file.
 ****************************************************************************/

static int sdtest_rnd(int ops)
{
  FAR uint8_t *buf;
  char path[64];
  const size_t filesz = 256 * 1024;
  const int slots = (int)(filesz / SDTEST_CHUNK);
  uint64_t t0;
  int fail = 0;
  int fd;
  int i;

  buf = malloc(SDTEST_CHUNK);
  if (buf == NULL)
    {
      return 1;
    }

  snprintf(path, sizeof(path), "%s/rnd.bin", SDTEST_DIR);
  LOGI("---- rnd %d ops x 4 KiB on 256 KiB file ----", ops);
  sdtest_rm(path);

  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      LOGE("rnd open errno=%d", errno);
      free(buf);
      return 1;
    }

  memset(buf, 0, SDTEST_CHUNK);
  for (i = 0; i < slots; i++)
    {
      if (write(fd, buf, SDTEST_CHUNK) != SDTEST_CHUNK)
        {
          LOGE("rnd prefill errno=%d", errno);
          close(fd);
          free(buf);
          return 1;
        }
    }

  t0 = sdtest_now_us();
  for (i = 0; i < ops; i++)
    {
      int slot = (i * 17 + 3) % slots;
      off_t off = (off_t)slot * SDTEST_CHUNK;
      uint32_t seed = 0x10000u + (uint32_t)i;
      ssize_t n = -1;

      sdtest_fill(buf, SDTEST_CHUNK, seed);
      if (lseek(fd, off, SEEK_SET) < 0 ||
          write(fd, buf, SDTEST_CHUNK) != SDTEST_CHUNK)
        {
          LOGE("rnd write op=%d errno=%d", i, errno);
          fail++;
          continue;
        }

      if (lseek(fd, off, SEEK_SET) < 0 ||
          (n = read(fd, buf, SDTEST_CHUNK)) != SDTEST_CHUNK ||
          sdtest_check(buf, SDTEST_CHUNK, seed) < 0)
        {
          LOGE("rnd read op=%d n=%zd errno=%d", i, n, errno);
          fail++;
        }
    }

  fsync(fd);
  close(fd);
  sdtest_rate("rnd write+read", (size_t)ops * SDTEST_CHUNK,
              sdtest_now_us() - t0);
  sdtest_rm(path);
  free(buf);
  return fail;
}

/****************************************************************************
 * mix: create / append / read / unlink churn for `sec` seconds.
 ****************************************************************************/

static int sdtest_mix(int sec)
{
  FAR uint8_t *buf;
  uint64_t t0;
  uint64_t deadline;
  int fail = 0;
  int nfile = 0;
  int nops = 0;
  unsigned rng = 1;

  buf = malloc(SDTEST_CHUNK);
  if (buf == NULL)
    {
      return 1;
    }

  LOGI("---- mix churn %d s ----", sec);
  t0 = sdtest_now_us();
  deadline = t0 + (uint64_t)sec * 1000000ull;

  while (sdtest_now_us() < deadline)
    {
      char path[64];
      int fd;
      int k = (int)((rng = rng * 1103515245u + 12345u) % 8);
      size_t nwr = 64 + (rng % (SDTEST_CHUNK - 64));

      snprintf(path, sizeof(path), "%s/m%02d.bin", SDTEST_DIR, k);
      sdtest_fill(buf, nwr, rng);

      fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd < 0)
        {
          LOGE("mix open %s errno=%d", path, errno);
          fail++;
          continue;
        }

      if (write(fd, buf, nwr) != (ssize_t)nwr)
        {
          LOGE("mix write %s errno=%d", path, errno);
          fail++;
        }

      close(fd);
      nops++;

      fd = open(path, O_RDONLY);
      if (fd >= 0)
        {
          (void)read(fd, buf, SDTEST_CHUNK);
          close(fd);
          nops++;
        }

      if ((nops & 7) == 0)
        {
          sdtest_rm(path);
          nfile++;
        }
    }

  {
    int k;

    for (k = 0; k < 8; k++)
      {
        char path[64];

        snprintf(path, sizeof(path), "%s/m%02d.bin", SDTEST_DIR, k);
        sdtest_rm(path);
      }
  }

  LOGI("mix %d s: ops=%d unlinks~=%d fail=%d", sec, nops, nfile, fail);
  free(buf);
  return fail;
}

static void sdtest_usage(void)
{
  printf("Usage: test posix|sd [seq|chunk|rnd|mix|all] [arg]\n"
         "  Layer 3 — POSIX/VFS on %s (open/read/write/fsync).\n"
         "  (default all)\n"
         "  seq   [KiB [rounds]]  sequential write/read/verify  (1024 2)\n"
         "  chunk [KiB]           4 KiB writes + fsync, MTP-like (1024)\n"
         "  rnd   [ops]           random 4 KiB on 256 KiB file   (200)\n"
         "  mix   [sec]           create/append/read/unlink      (15)\n"
         "  all                   seq + chunk + rnd + mix\n"
         "Files live in %s and are deleted afterwards.\n",
         SDTEST_MNT, SDTEST_DIR);
}

int test_sd_main(int argc, FAR char *argv[])
{
  FAR const char *mode = "all";
  int a1 = -1;
  int a2 = -1;
  int fail = 0;

  if (argc >= 1 && argv[0] != NULL)
    {
      g_sdtest_tag = argv[0][0] != '\0' ? argv[0] : "posix";
    }

  if (argc >= 2)
    {
      if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0)
        {
          sdtest_usage();
          return EXIT_SUCCESS;
        }

      mode = argv[1];
    }

  if (argc >= 3)
    {
      a1 = atoi(argv[2]);
    }

  if (argc >= 4)
    {
      a2 = atoi(argv[3]);
    }

  LOGI("==== layer 3 POSIX/VFS start (%s) ====", mode);
  sf32lb_sd_diag_dump("sdtest-begin");

  if (sdtest_prepare() < 0)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(mode, "seq") == 0)
    {
      fail += sdtest_seq(a1 > 0 ? (size_t)a1 : 1024, a2 > 0 ? a2 : 2);
    }
  else if (strcmp(mode, "chunk") == 0)
    {
      fail += sdtest_chunk(a1 > 0 ? (size_t)a1 : 1024);
    }
  else if (strcmp(mode, "rnd") == 0)
    {
      fail += sdtest_rnd(a1 > 0 ? a1 : 200);
    }
  else if (strcmp(mode, "mix") == 0)
    {
      fail += sdtest_mix(a1 > 0 ? a1 : 15);
    }
  else if (strcmp(mode, "all") == 0)
    {
      fail += sdtest_seq(1024, 2);
      fail += sdtest_chunk(1024);
      fail += sdtest_rnd(200);
      fail += sdtest_mix(15);
    }
  else
    {
      sdtest_usage();
      return EXIT_FAILURE;
    }

  rmdir(SDTEST_DIR);
  sf32lb_sd_diag_dump("sdtest-end");

  if (fail == 0)
    {
      LOGI("==== layer 3 POSIX/VFS: PASS ====");
      return EXIT_SUCCESS;
    }

  LOGE("==== layer 3 POSIX/VFS: FAIL (%d) ====", fail);
  return EXIT_FAILURE;
}

#else /* CONFIG_FS_LITTLEFS */

int test_sd_main(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);
  printf("test posix: requires CONFIG_FS_LITTLEFS\n");
  return EXIT_FAILURE;
}

#endif
