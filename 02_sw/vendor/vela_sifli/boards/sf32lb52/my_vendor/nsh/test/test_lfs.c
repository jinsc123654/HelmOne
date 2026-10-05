/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_lfs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Layer 2: POSIX/VFS on the LittleFS volume at /mnt/lfs.
 *
 *   nsh> test lfs              seq 1 MiB + 4 KiB chunk 256 KiB（与 test fat 同）
 *   nsh> test lfs seq [KiB]    顺序写/读/校验（32 KiB 块）
 *   nsh> test lfs chunk [KiB]  4 KiB 写 + fsync（MTP 形态）
 *   nsh> test lfs small        inline-sized files (create/append/list/unlink)
 *   nsh> test lfs mkdir        mkdir / list / rmdir (dir-block alloc)
 *   nsh> test lfs large [KiB]  CTZ write/read/verify; reuses an existing file
 *   nsh> test lfs format -y    umount + forceformat (wipes volume), then speed
 *   nsh> test lfs grow         keep files, expand superblock to full card
 *
 * First lfs_alloc() after mount walks every used file (slow on SD PIO).
 * Creating or deleting files can also compact the root dir and allocate.
 * Large prefers an existing root file so open() is not O_CREAT.  Do not
 * use statfs() on this volume.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "myvendor_watchdog.h"
#include "sf32lb_sdio.h"
#include "test_demos.h"

#define LFSTEST_MNT      "/mnt/lfs"
#define LFSTEST_FSTYPE   "littlefs"
#define LFSTEST_NFILES   5
#define LFSTEST_MARKER   LFSTEST_MNT "/boot_marker.txt"
#define LFSTEST_HELLO    LFSTEST_MNT "/hello.txt"
#define LFSTEST_DIR      LFSTEST_MNT "/.lfstest_d"
#define LFSTEST_SUB      LFSTEST_MNT "/.lfstest_d/sub"
#define LFSTEST_SUBFILE  LFSTEST_MNT "/.lfstest_d/sub/x.txt"
#define LFSTEST_LARGE    LFSTEST_MNT "/.lfstest_large.bin"
#define LFSTEST_SPEED_DIR LFSTEST_MNT "/lftest"
#define LFSTEST_SEQ      LFSTEST_SPEED_DIR "/seq.bin"
#define LFSTEST_CHK      LFSTEST_SPEED_DIR "/chk.bin"
#define LFSTEST_PROBE    LFSTEST_SPEED_DIR "/probe.bin"
#define LFSTEST_CHUNK    4096
#define LFSTEST_CLUS     32768
#define LFSTEST_SEQ_KIB  1024
#define LFSTEST_CHK_KIB  256
#define LFSTEST_LARGE_KIB 32
#define LFSTEST_FORMAT_MIB 64
#define LFSTEST_GROW_MARK LFSTEST_MNT "/.grow_marker"
#define LFSTEST_NRES     8

#define LOGI(fmt, ...) \
  do \
    { \
      printf("test lfs: " fmt "\n", ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)
#define LOGE(fmt, ...) \
  do \
    { \
      printf("test lfs: ERROR " fmt "\n", ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)

#if defined(CONFIG_FS_LITTLEFS) && defined(CONFIG_MTD)

static struct
{
  char tag[24];
  size_t bytes;
  uint64_t us;
} g_lfs_res[LFSTEST_NRES];
static unsigned g_lfs_nres;

static uint64_t lfstest_now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static void lfstest_log_us(FAR const char *what, uint64_t t0)
{
  uint64_t us = lfstest_now_us() - t0;

  LOGI("%s %llu us (%lu ms)", what,
       (unsigned long long)us,        (unsigned long)(us / 1000ull));
}

enum lfstest_mode_e
{
  LFSTEST_MODE_ALL = 0,
  LFSTEST_MODE_SEQ,
  LFSTEST_MODE_CHUNK,
  LFSTEST_MODE_SMALL,
  LFSTEST_MODE_MKDIR,
  LFSTEST_MODE_LARGE,
  LFSTEST_MODE_MOUNT,
  LFSTEST_MODE_FORMAT,
  LFSTEST_MODE_GROW
};

int __attribute__((weak)) sf32lb_sd_limit_fs_bytes(uint32_t bytes)
{
  UNUSED(bytes);
  return -ENOSYS;
}

static int lfstest_write_file(FAR const char *path, FAR const char *data,
                              size_t len, bool append)
{
  int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  ssize_t n;
  int fd;
  uint64_t t0;
  uint64_t t_open;
  uint64_t t_write;
  uint64_t t_sync;

  t0 = lfstest_now_us();
  fd = open(path, flags, 0644);
  t_open = lfstest_now_us() - t0;
  if (fd < 0)
    {
      LOGE("open(%s) for write failed: %d (open %llu us)",
           path, errno, (unsigned long long)t_open);
      return -errno;
    }

  n = write(fd, data, len);
  t_write = lfstest_now_us() - t0 - t_open;
  if (n < 0)
    {
      LOGE("write(%s) failed: %d", path, errno);
      close(fd);
      return -errno;
    }

  if ((size_t)n != len)
    {
      LOGE("short write(%s): %zd/%zu", path, n, len);
      close(fd);
      return -EIO;
    }

  fsync(fd);
  t_sync = lfstest_now_us() - t0 - t_open - t_write;
  close(fd);
  LOGI("wrote %zu bytes to %s%s in %llu us (open %llu write %llu fsync %llu)",
       len, path, append ? " (append)" : "",
       (unsigned long long)(lfstest_now_us() - t0),
       (unsigned long long)t_open,
       (unsigned long long)t_write,
       (unsigned long long)t_sync);
  return OK;
}

static int lfstest_verify_file(FAR const char *path, FAR const char *expect,
                               size_t len)
{
  FAR char *buf;
  ssize_t n;
  int fd;
  int ret = OK;

  buf = malloc(len + 1);
  if (buf == NULL)
    {
      LOGE("oom verifying %s", path);
      return -ENOMEM;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      LOGE("open(%s) for read failed: %d", path, errno);
      free(buf);
      return -errno;
    }

  n = read(fd, buf, len);
  close(fd);

  if (n < 0)
    {
      LOGE("read(%s) failed: %d", path, errno);
      free(buf);
      return -errno;
    }

  if ((size_t)n != len || memcmp(buf, expect, len) != 0)
    {
      LOGE("verify mismatch on %s (read %zd/%zu bytes)", path, n, len);
      ret = -EIO;
    }
  else
    {
      LOGI("verified %zu bytes of %s OK", len, path);
    }

  free(buf);
  return ret;
}

static bool lfs_is_mounted(void)
{
  FAR DIR *dir;

  dir = opendir(LFSTEST_MNT);
  if (dir == NULL)
    {
      return false;
    }

  closedir(dir);
  return true;
}

static FAR const char *lfstest_dev(void)
{
  static const char * const cands[] =
    {
      "/dev/sd0",
      "/dev/config0",
      "/dev/nand0",
      NULL
    };
  int i;

  for (i = 0; cands[i] != NULL; i++)
    {
      if (access(cands[i], F_OK) == 0)
        {
          return cands[i];
        }
    }

  return "/dev/sd0";
}

static int lfs_mount_volume(void)
{
  FAR const char *dev = lfstest_dev();
  int ret;
  uint64_t t0;

  t0 = lfstest_now_us();
  if (lfs_is_mounted())
    {
      LOGI("%s already mounted, reusing (%lu ms)",
           LFSTEST_MNT, (unsigned long)((lfstest_now_us() - t0) / 1000ull));
      return OK;
    }

  mkdir("/mnt", 0755);
  mkdir(LFSTEST_MNT, 0755);

  LOGI("mount(%s -> %s) begin", dev, LFSTEST_MNT);
  t0 = lfstest_now_us();
  ret = mount(dev, LFSTEST_MNT, LFSTEST_FSTYPE, 0, "autoformat");
  lfstest_log_us("mount returned", t0);
  if (ret == 0)
    {
      LOGI("mounted %s -> %s (%s)", dev, LFSTEST_MNT, LFSTEST_FSTYPE);
      return OK;
    }

  if (errno == EBUSY || errno == ENOTDIR || errno == EEXIST)
    {
      if (lfs_is_mounted())
        {
          LOGI("%s already mounted, reusing", LFSTEST_MNT);
          return OK;
        }
    }

  LOGE("mount(%s) failed: %d", dev, errno);
  return -errno;
}

static void lfstest_stamp(FAR const char *when)
{
  struct timespec mono;
  struct timespec real;
  struct timespec sys;
  time_t sec;
  struct tm tm;
  char cal[40];

  memset(&mono, 0, sizeof(mono));
  memset(&real, 0, sizeof(real));
  memset(&sys, 0, sizeof(sys));
  (void)clock_gettime(CLOCK_MONOTONIC, &mono);
  (void)clock_gettime(CLOCK_REALTIME, &real);
  (void)clock_systime_timespec(&sys);
  sec = real.tv_sec;
  cal[0] = '-';
  cal[1] = '\0';
  if (gmtime_r(&sec, &tm) != NULL)
    {
      snprintf(cal, sizeof(cal), "%04d-%02d-%02d %02d:%02d:%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

  syslog(LOG_INFO, "test lfs: %s stamp\n", when);
  LOGI("%s stamp systime=%lu.%06lu mono=%lu.%06lu utc=%s",
       when,
       (unsigned long)sys.tv_sec,
       (unsigned long)(sys.tv_nsec / 1000L),
       (unsigned long)mono.tv_sec,
       (unsigned long)(mono.tv_nsec / 1000L),
       cal);
}

static void lfstest_rate(FAR const char *tag, size_t bytes, uint64_t us)
{
  if (us == 0)
    {
      us = 1;
    }

  LOGI("%s %zu bytes in %lu ms -> %lu KB/s",
       tag, bytes, (unsigned long)(us / 1000ull),
       (unsigned long)((bytes * 1000ull) / us));
  if (g_lfs_nres < LFSTEST_NRES)
    {
      snprintf(g_lfs_res[g_lfs_nres].tag, sizeof(g_lfs_res[0].tag), "%s",
               tag);
      g_lfs_res[g_lfs_nres].bytes = bytes;
      g_lfs_res[g_lfs_nres].us = us;
      g_lfs_nres++;
    }
}

static void lfstest_result(void)
{
  unsigned i;

  LOGI("---- result /mnt/lfs ----");
  LOGI("  %-16s %8s %7s %8s", "case", "bytes", "ms", "KB/s");
  for (i = 0; i < g_lfs_nres; i++)
    {
      uint64_t us = g_lfs_res[i].us == 0 ? 1 : g_lfs_res[i].us;

      LOGI("  %-16s %8zu %7lu %8lu",
           g_lfs_res[i].tag, g_lfs_res[i].bytes,
           (unsigned long)(us / 1000ull),
           (unsigned long)((g_lfs_res[i].bytes * 1000ull) / us));
    }
}

static void lfstest_fill(FAR uint8_t *buf, size_t n, uint32_t seed)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      buf[i] = (uint8_t)((seed + (uint32_t)i) * 0x9eu + 0x5d);
    }
}

static int lfstest_check(FAR const uint8_t *buf, size_t n, uint32_t seed)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      uint8_t expect = (uint8_t)((seed + (uint32_t)i) * 0x9eu + 0x5d);

      if (buf[i] != expect)
        {
          LOGE("mismatch off=%zu got=0x%02x expect=0x%02x",
               i, buf[i], expect);
          return -1;
        }
    }

  return 0;
}

static int lfstest_rm(FAR const char *path);

static int lfstest_speed_prepare(void)
{
  uint8_t one = 0xa5;
  ssize_t n;
  int fd;
  int ret;

  ret = mkdir(LFSTEST_SPEED_DIR, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      LOGE("mkdir(%s) errno=%d", LFSTEST_SPEED_DIR, errno);
      return -1;
    }

  fd = open(LFSTEST_PROBE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      LOGE("probe open errno=%d", errno);
      return -1;
    }

  n = write(fd, &one, 1);
  fsync(fd);
  close(fd);
  lfstest_rm(LFSTEST_PROBE);
  if (n != 1)
    {
      LOGE("probe write n=%zd errno=%d", n, errno);
      return -1;
    }

  return 0;
}

static int lfstest_xfer(FAR const char *path, size_t total, size_t iosz,
                        bool do_write)
{
  FAR uint8_t *buf;
  size_t off;
  uint64_t t0;
  uint64_t us;
  ssize_t n;
  int fd;
  int flags;
  int fail = 0;

  buf = malloc(iosz);
  if (buf == NULL)
    {
      LOGE("oom %zu", iosz);
      return 1;
    }

  flags = do_write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
  fd = open(path, flags, 0644);
  if (fd < 0)
    {
      LOGE("open(%s) %s errno=%d", path, do_write ? "write" : "read", errno);
      free(buf);
      return 1;
    }

  t0 = lfstest_now_us();
  for (off = 0; off < total; off += iosz)
    {
      size_t want = total - off;
      uint32_t seed = (uint32_t)off;

      if (want > iosz)
        {
          want = iosz;
        }

      myvendor_watchdog_work_beat();
      myvendor_watchdog_hw_pet();

      if (do_write)
        {
          lfstest_fill(buf, want, seed);
          n = write(fd, buf, want);
        }
      else
        {
          n = read(fd, buf, want);
        }

      if (n != (ssize_t)want)
        {
          LOGE("%s off=%zu n=%zd errno=%d",
               do_write ? "write" : "read", off, n, errno);
          fail++;
          break;
        }

      if (!do_write && lfstest_check(buf, want, seed) < 0)
        {
          fail++;
          break;
        }
    }

  if (do_write)
    {
      fsync(fd);
    }

  close(fd);
  us = lfstest_now_us() - t0;
  lfstest_rate(do_write ? "write+fsync" : "read+verify", off, us);
  free(buf);
  return fail;
}

static int lfstest_run_seq(size_t kib)
{
  size_t total = kib * 1024;
  int fail;

  LOGI("---- seq %zu KiB iosz=%u ----", kib, LFSTEST_CLUS);
  lfstest_rm(LFSTEST_SEQ);
  fail = lfstest_xfer(LFSTEST_SEQ, total, LFSTEST_CLUS, true);
  if (fail == 0)
    {
      fail = lfstest_xfer(LFSTEST_SEQ, total, LFSTEST_CLUS, false);
    }

  lfstest_rm(LFSTEST_SEQ);
  return fail;
}

static int lfstest_run_chunk(size_t kib)
{
  size_t total = kib * 1024;
  int fail;

  LOGI("---- chunk %zu KiB iosz=%u (MTP-like) ----", kib, LFSTEST_CHUNK);
  lfstest_rm(LFSTEST_CHK);
  fail = lfstest_xfer(LFSTEST_CHK, total, LFSTEST_CHUNK, true);
  if (fail == 0)
    {
      fail = lfstest_xfer(LFSTEST_CHK, total, LFSTEST_CHUNK, false);
    }

  lfstest_rm(LFSTEST_CHK);
  return fail;
}

static int lfstest_run_speed(bool do_seq, bool do_chunk, size_t seq_kib,
                             size_t chk_kib)
{
  int fail = 0;

  if (lfstest_speed_prepare() < 0)
    {
      return 1;
    }

  if (do_seq)
    {
      fail += lfstest_run_seq(seq_kib);
    }

  if (do_chunk)
    {
      fail += lfstest_run_chunk(chk_kib);
    }

  lfstest_result();
  return fail;
}

static void lfs_usage(void)
{
  printf("Usage: test lfs [seq [KiB]|chunk [KiB]|all|small|mkdir|large [KiB]|format|grow] [-y] [-k] [-u] [-m]\n"
         "  Layer 2 — POSIX/VFS on %s (not raw lfs_*).\n"
         "  (default all)  seq 1 MiB + chunk 256 KiB, then print KB/s\n"
         "  seq [KiB]      sequential write/read/verify, 32 KiB I/O\n"
         "  chunk [KiB]    4 KiB writes + fsync, then read (MTP-like)\n"
         "  small          inline-sized create/append/list/unlink\n"
         "  mkdir          mkdir/list/rmdir (dir-block alloc; MTP new folder)\n"
         "  large [KiB]    CTZ write/read (%d); reuses hello/marker if present\n"
         "  format [-y] [size] umount + forceformat (WIPES %s), then speed\n"
         "                 e.g. test lfs format -y 16\n"
         "                 size: MiB, or 4g/full for remaining-card window\n"
         "                 default %d MiB; disconnect MTP first (umount EBUSY)\n"
         "  grow           keep files, expand superblock to full CSD window\n"
         "                 try: test lfs format -y 16  then  test lfs grow\n"
         "  -m             mount only\n"
         "  -k             keep test files\n"
         "  -u             unmount when finished\n"
         "  -y             required with format (confirm wipe)\n"
         "Do not use statfs.  Stop MTP/test_app before format (umount EBUSY).\n",
         LFSTEST_MNT, LFSTEST_LARGE_KIB, LFSTEST_MNT, LFSTEST_FORMAT_MIB);
}

static bool lfstest_parse_format_size(FAR const char *sz, FAR int *mib)
{
  if (strcmp(sz, "4g") == 0 || strcmp(sz, "4G") == 0 ||
      strcmp(sz, "full") == 0)
    {
      *mib = -1;
      return true;
    }

  if (sz[0] >= '0' && sz[0] <= '9')
    {
      *mib = atoi(sz);
      return true;
    }

  return false;
}

static void persist_counter(void)
{
  char buf[32];
  unsigned long cnt = 0;
  int fd;
  ssize_t n;

  fd = open(LFSTEST_MARKER, O_RDONLY);
  if (fd >= 0)
    {
      n = read(fd, buf, sizeof(buf) - 1);
      if (n > 0)
        {
          buf[n] = '\0';
          cnt = strtoul(buf, NULL, 10);
        }

      close(fd);
      LOGI("persistent marker found: previous count = %lu", cnt);
    }
  else
    {
      LOGI("no persistent marker yet (first run on this volume)");
    }

  cnt++;
  n = snprintf(buf, sizeof(buf), "%lu", cnt);
  if (lfstest_write_file(LFSTEST_MARKER, buf, (size_t)n, false) == OK)
    {
      LOGI("persistent marker updated: count = %lu (survives reboot)", cnt);
    }
}

static int lfstest_list(FAR const char *dirpath)
{
  FAR DIR *dir;
  FAR struct dirent *de;
  int count = 0;
  uint64_t t0;

  LOGI("opendir %s begin", dirpath);
  t0 = lfstest_now_us();
  dir = opendir(dirpath);
  lfstest_log_us("opendir returned", t0);
  if (dir == NULL)
    {
      LOGE("opendir(%s) failed: %d", dirpath, errno);
      return -1;
    }

  t0 = lfstest_now_us();
  while ((de = readdir(dir)) != NULL)
    {
      LOGI("  %s", de->d_name);
      count++;
    }

  closedir(dir);
  lfstest_log_us("readdir+closedir", t0);
  LOGI("directory entries: %d", count);
  return 0;
}

static int lfstest_rm(FAR const char *path)
{
  if (unlink(path) < 0 && errno != ENOENT)
    {
      LOGE("unlink(%s) failed: %d", path, errno);
      return -1;
    }

  return 0;
}

static int lfstest_run_small(bool keep)
{
  char path[48];
  char content[96];
  int failures = 0;
  int i;
  uint64_t t_sec;

  t_sec = lfstest_now_us();
  LOGI("==== small files (inline-sized) begin ====");
  persist_counter();

  LOGI("---- write/read single file ----");
  snprintf(content, sizeof(content), "hello lfs from littlefs!");
  snprintf(path, sizeof(path), "%s/hello.txt", LFSTEST_MNT);
  if (lfstest_write_file(path, content, strlen(content), false) < 0 ||
      lfstest_verify_file(path, content, strlen(content)) < 0)
    {
      failures++;
    }

  LOGI("---- append ----");
  {
    char appended[160];
    const char *more = " ...appended line.";

    if (lfstest_write_file(path, more, strlen(more), true) < 0)
      {
        failures++;
      }
    else
      {
        snprintf(appended, sizeof(appended), "%s%s", content, more);
        if (lfstest_verify_file(path, appended, strlen(appended)) < 0)
          {
            failures++;
          }
      }
  }

  LOGI("---- create %d files ----", LFSTEST_NFILES);
  for (i = 0; i < LFSTEST_NFILES; i++)
    {
      snprintf(path, sizeof(path), "%s/file_%02d.dat", LFSTEST_MNT, i);
      snprintf(content, sizeof(content),
               "file index %d payload 0123456789ABCDEF", i);
      if (lfstest_write_file(path, content, strlen(content), false) < 0 ||
          lfstest_verify_file(path, content, strlen(content)) < 0)
        {
          failures++;
        }
    }

  if (lfstest_list(LFSTEST_MNT) < 0)
    {
      failures++;
    }

  if (keep)
    {
      LOGI("---- small: keep files (-k) ----");
      lfstest_log_us("==== small files end", t_sec);
      return failures;
    }

  LOGI("---- delete small files ----");
  for (i = 0; i < LFSTEST_NFILES; i++)
    {
      snprintf(path, sizeof(path), "%s/file_%02d.dat", LFSTEST_MNT, i);
      if (unlink(path) < 0)
        {
          LOGE("unlink(%s) failed: %d", path, errno);
          failures++;
        }
      else
        {
          LOGI("deleted %s", path);
        }
    }

  snprintf(path, sizeof(path), "%s/hello.txt", LFSTEST_MNT);
  if (unlink(path) < 0)
    {
      LOGE("unlink(%s) failed: %d", path, errno);
      failures++;
    }
  else
    {
      LOGI("deleted %s", path);
    }

  lfstest_log_us("==== small files end", t_sec);
  return failures;
}

static int lfstest_run_mkdir(bool keep)
{
  int failures = 0;
  int ret;
  uint64_t t_sec;
  uint64_t t0;

  t_sec = lfstest_now_us();
  LOGI("==== mkdir (dir-block alloc, same as MTP new folder) begin ====");
  LOGI("if the next line never appears, LFS is in lfs_alloc/dir compact");
  LOGI("calling mkdir(%s) ...", LFSTEST_DIR);
  t0 = lfstest_now_us();
  ret = mkdir(LFSTEST_DIR, 0755);
  lfstest_log_us("mkdir(dir) returned", t0);
  if (ret < 0 && errno != EEXIST)
    {
      LOGE("mkdir(%s) failed: %d", LFSTEST_DIR, errno);
      return 1;
    }

  LOGI("mkdir(%s) ok (errno was %d)", LFSTEST_DIR, ret < 0 ? errno : 0);

  LOGI("calling mkdir(%s) ...", LFSTEST_SUB);
  t0 = lfstest_now_us();
  ret = mkdir(LFSTEST_SUB, 0755);
  lfstest_log_us("mkdir(sub) returned", t0);
  if (ret < 0 && errno != EEXIST)
    {
      LOGE("mkdir(%s) failed: %d", LFSTEST_SUB, errno);
      failures++;
    }
  else
    {
      LOGI("mkdir(%s) ok", LFSTEST_SUB);
    }

  if (lfstest_write_file(LFSTEST_SUBFILE, "in subdir", 9, false) < 0 ||
      lfstest_verify_file(LFSTEST_SUBFILE, "in subdir", 9) < 0)
    {
      failures++;
    }

  if (lfstest_list(LFSTEST_DIR) < 0)
    {
      failures++;
    }

  if (lfstest_list(LFSTEST_SUB) < 0)
    {
      failures++;
    }

  if (keep)
    {
      LOGI("---- mkdir: keep dirs (-k) ----");
      lfstest_log_us("==== mkdir end", t_sec);
      return failures;
    }

  LOGI("---- rmdir ----");
  if (lfstest_rm(LFSTEST_SUBFILE) < 0)
    {
      failures++;
    }

  if (rmdir(LFSTEST_SUB) < 0)
    {
      LOGE("rmdir(%s) failed: %d", LFSTEST_SUB, errno);
      failures++;
    }
  else
    {
      LOGI("rmdir %s", LFSTEST_SUB);
    }

  if (rmdir(LFSTEST_DIR) < 0)
    {
      LOGE("rmdir(%s) failed: %d", LFSTEST_DIR, errno);
      failures++;
    }
  else
    {
      LOGI("rmdir %s", LFSTEST_DIR);
    }

  lfstest_log_us("==== mkdir end", t_sec);
  return failures;
}

static bool lfstest_exists(FAR const char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

static int lfstest_run_large(int kib, bool keep)
{
  FAR uint8_t *buf;
  char marker_saved[32];
  FAR const char *path;
  size_t marker_len = 0;
  size_t total;
  size_t off;
  uint64_t t0;
  uint64_t t_open;
  uint64_t t_w;
  uint64_t t_sec;
  uint64_t us;
  ssize_t n;
  int fd;
  int flags;
  int failures = 0;
  bool created = false;
  bool restore_marker = false;

  if (kib <= 0)
    {
      kib = LFSTEST_LARGE_KIB;
    }

  total = (size_t)kib * 1024u;
  t_sec = lfstest_now_us();
  buf = malloc(LFSTEST_CHUNK);
  if (buf == NULL)
    {
      LOGE("oom");
      return 1;
    }

  /* O_CREAT after small-file create/delete often compact the root dir and
   * calls lfs_alloc(), which walks every used file. Reuse a file that is
   * already in the root so open() stays a metadata lookup.
   */

  if (lfstest_exists(LFSTEST_HELLO))
    {
      path = LFSTEST_HELLO;
    }
  else if (lfstest_exists(LFSTEST_LARGE))
    {
      path = LFSTEST_LARGE;
    }
  else if (lfstest_exists(LFSTEST_MARKER))
    {
      path = LFSTEST_MARKER;
      fd = open(path, O_RDONLY);
      if (fd >= 0)
        {
          n = read(fd, marker_saved, sizeof(marker_saved) - 1);
          close(fd);
          if (n > 0)
            {
              marker_saved[n] = '\0';
              marker_len = (size_t)n;
              restore_marker = true;
            }
        }
    }
  else
    {
      path = LFSTEST_LARGE;
      created = true;
    }

  LOGI("==== large file %d KiB at %s begin ====", kib, path);
  if (created)
    {
      flags = O_WRONLY | O_CREAT | O_TRUNC;
      LOGI("open O_CREAT %s (may stall in dir compact / lfs_alloc)", path);
    }
  else
    {
      flags = O_WRONLY;
      LOGI("open existing %s (no O_CREAT)", path);
    }

  t_open = lfstest_now_us();
  fd = open(path, flags, 0644);
  LOGI("open returned fd=%d errno=%d in %llu us",
       fd, fd < 0 ? errno : 0,
       (unsigned long long)(lfstest_now_us() - t_open));
  if (fd < 0)
    {
      LOGE("open write errno=%d", errno);
      free(buf);
      return 1;
    }

  LOGI("write %zu B starting (first 4 KiB may stall on lfs_alloc)", total);
  t0 = lfstest_now_us();
  for (off = 0; off < total; off += LFSTEST_CHUNK)
    {
      size_t want = total - off;
      size_t i;

      if (want > LFSTEST_CHUNK)
        {
          want = LFSTEST_CHUNK;
        }

      for (i = 0; i < want; i++)
        {
          buf[i] = (uint8_t)((off + i) * 0x9eu + 0x5d);
        }

      t_w = lfstest_now_us();
      n = write(fd, buf, want);
      if (n != (ssize_t)want)
        {
          LOGE("write off=%zu n=%zd errno=%d", off, n, errno);
          failures++;
          break;
        }

      LOGI("  wrote %zu / %zu in %llu us",
           off + want, total,
           (unsigned long long)(lfstest_now_us() - t_w));
    }

  LOGI("fsync %s begin", path);
  t_w = lfstest_now_us();
  fsync(fd);
  lfstest_log_us("fsync returned", t_w);
  close(fd);
  us = lfstest_now_us() - t0;
  if (us == 0)
    {
      us = 1;
    }

  LOGI("large write %zu B in %llu us -> %lu KB/s",
       off, (unsigned long long)us,
       (unsigned long)((off * 1000ull) / us));

  LOGI("open O_RDONLY %s", path);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      LOGE("open read errno=%d", errno);
      free(buf);
      if (!keep && created)
        {
          lfstest_rm(LFSTEST_LARGE);
        }

      return failures + 1;
    }

  t0 = lfstest_now_us();
  for (off = 0; off < total && failures == 0; off += LFSTEST_CHUNK)
    {
      size_t want = total - off;
      size_t i;

      if (want > LFSTEST_CHUNK)
        {
          want = LFSTEST_CHUNK;
        }

      n = read(fd, buf, want);
      if (n != (ssize_t)want)
        {
          LOGE("read off=%zu n=%zd errno=%d", off, n, errno);
          failures++;
          break;
        }

      for (i = 0; i < want; i++)
        {
          uint8_t expect = (uint8_t)((off + i) * 0x9eu + 0x5d);

          if (buf[i] != expect)
            {
              LOGE("mismatch off=%zu", off + i);
              failures++;
              break;
            }
        }

      if ((off & 0x7fff) == 0)
        {
          LOGI("  read %zu / %zu", off + want, total);
        }
    }

  close(fd);
  us = lfstest_now_us() - t0;
  if (us == 0)
    {
      us = 1;
    }

  LOGI("large read %zu B in %llu us -> %lu KB/s",
       off, (unsigned long long)us,
       (unsigned long)((off * 1000ull) / us));

  if (restore_marker && marker_len > 0)
    {
      LOGI("restore %s (%zu bytes)", LFSTEST_MARKER, marker_len);
      if (lfstest_write_file(LFSTEST_MARKER, marker_saved, marker_len,
                             false) < 0)
        {
          failures++;
        }
    }
  else if (!keep && strcmp(path, LFSTEST_HELLO) == 0)
    {
      if (lfstest_rm(LFSTEST_HELLO) == 0)
        {
          LOGI("deleted %s", LFSTEST_HELLO);
        }
    }
  else if (!keep && strcmp(path, LFSTEST_LARGE) == 0)
    {
      if (lfstest_rm(LFSTEST_LARGE) == 0)
        {
          LOGI("deleted %s", LFSTEST_LARGE);
        }
    }
  else if (keep)
    {
      LOGI("keep %s (-k)", path);
    }

  free(buf);
  lfstest_log_us("==== large file end", t_sec);
  return failures;
}

static int lfstest_forceformat(uint32_t bytes)
{
  FAR const char *dev = lfstest_dev();
  uint64_t t0;
  int ret;

  if (bytes == 0)
    {
      bytes = (uint32_t)LFSTEST_FORMAT_MIB * 1024u * 1024u;
    }

  LOGI("==== FORCE FORMAT (destroys every file on %s) ====", LFSTEST_MNT);
  if (bytes == SF32LB_SD_LFS_WINDOW_FULL)
    {
      LOGI("dev=%s  logical size=full remaining-card window", dev);
    }
  else
    {
      LOGI("dev=%s  logical size=%u MiB (0x%lx)",
           dev, (unsigned)(bytes / (1024u * 1024u)), (unsigned long)bytes);
    }

  ret = umount(LFSTEST_MNT);
  if (ret < 0)
    {
      LOGI("umount(%s) -> errno=%d (ok if not mounted)", LFSTEST_MNT, errno);
    }
  else
    {
      LOGI("unmounted %s", LFSTEST_MNT);
    }

  ret = sf32lb_sd_limit_fs_bytes(bytes);
  if (ret < 0 && ret != -ENOSYS)
    {
      LOGE("sf32lb_sd_limit_fs_bytes(0x%lx) failed: %d",
           (unsigned long)bytes, ret);
      return -1;
    }

  mkdir("/mnt", 0755);
  mkdir(LFSTEST_MNT, 0755);

  LOGI("mount(..., \"forceformat\") starting");
  t0 = lfstest_now_us();
  ret = mount(dev, LFSTEST_MNT, LFSTEST_FSTYPE, 0, "forceformat");
  LOGI("forceformat+mount ret=%d errno=%d in %llu us",
       ret, ret < 0 ? errno : 0,
       (unsigned long long)(lfstest_now_us() - t0));
  if (ret < 0)
    {
      LOGE("forceformat failed (stop MTP/test_app if errno=16 EBUSY)");
      return -1;
    }

  LOGI("formatted empty LittleFS at %s", LFSTEST_MNT);
  return 0;
}

static int lfstest_write_marker(void)
{
  int fd;
  ssize_t n;

  fd = open(LFSTEST_GROW_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      LOGE("open(%s) for write failed: %d", LFSTEST_GROW_MARK, errno);
      return -1;
    }

  n = write(fd, "grow-ok\n", 8);
  close(fd);
  if (n != 8)
    {
      LOGE("write(%s) failed", LFSTEST_GROW_MARK);
      return -1;
    }

  return 0;
}

static int lfstest_check_marker(void)
{
  char buf[16];
  int fd;
  ssize_t n;

  fd = open(LFSTEST_GROW_MARK, O_RDONLY);
  if (fd < 0)
    {
      LOGE("marker missing after grow: %s (errno=%d)",
           LFSTEST_GROW_MARK, errno);
      return -1;
    }

  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 8)
    {
      LOGE("marker short after grow (%zd bytes)", n);
      return -1;
    }

  buf[n] = '\0';
  if (strncmp(buf, "grow-ok", 7) != 0)
    {
      LOGE("marker corrupt after grow: %s", buf);
      return -1;
    }

  LOGI("marker still there: %s", LFSTEST_GROW_MARK);
  return 0;
}

static int lfstest_grow(void)
{
  FAR const char *dev = lfstest_dev();
  uint64_t t0;
  int ret;

  LOGI("==== GROW (keep files, expand superblock to full CSD window) ====");
  LOGI("hint: test lfs format -y 16   then   test lfs grow");

  if (!lfs_is_mounted())
    {
      mkdir("/mnt", 0755);
      mkdir(LFSTEST_MNT, 0755);
      ret = mount(dev, LFSTEST_MNT, LFSTEST_FSTYPE, 0, NULL);
      if (ret < 0)
        {
          LOGE("mount before grow failed: errno=%d "
               "(format a small volume first)", errno);
          return -1;
        }
    }

  if (lfstest_write_marker() < 0)
    {
      return -1;
    }

  ret = umount(LFSTEST_MNT);
  if (ret < 0)
    {
      LOGE("umount(%s) failed: errno=%d (stop MTP/test_app)",
           LFSTEST_MNT, errno);
      return -1;
    }

  ret = sf32lb_sd_limit_fs_bytes(SF32LB_SD_LFS_WINDOW_FULL);
  if (ret < 0 && ret != -ENOSYS)
    {
      LOGE("sf32lb_sd_limit_fs_bytes(full) failed: %d", ret);
      return -1;
    }

  LOGI("sf32lb_sd_grow_lfs starting");
  t0 = lfstest_now_us();
  ret = sf32lb_sd_grow_lfs();
  LOGI("grow superblock ret=%d in %llu us",
       ret, (unsigned long long)(lfstest_now_us() - t0));
  if (ret < 0 && ret != -ENOENT)
    {
      LOGE("sf32lb_sd_grow_lfs failed: %d", ret);
      return -1;
    }

  mkdir("/mnt", 0755);
  mkdir(LFSTEST_MNT, 0755);

  LOGI("mount after grow starting");
  t0 = lfstest_now_us();
  ret = mount(dev, LFSTEST_MNT, LFSTEST_FSTYPE, 0, NULL);
  LOGI("mount ret=%d errno=%d in %llu us",
       ret, ret < 0 ? errno : 0,
       (unsigned long long)(lfstest_now_us() - t0));
  if (ret < 0)
    {
      LOGE("grow mount failed");
      return -1;
    }

  if (lfstest_check_marker() < 0)
    {
      return -1;
    }

  LOGI("grow done — files kept, window is remaining-card size");
  return 0;
}

int test_lfs_main(int argc, FAR char *argv[])
{
  enum lfstest_mode_e mode = LFSTEST_MODE_ALL;
  bool keep = false;
  bool do_unmount = false;
  bool format_yes = false;
  int large_kib = LFSTEST_LARGE_KIB;
  size_t seq_kib = LFSTEST_SEQ_KIB;
  size_t chk_kib = LFSTEST_CHK_KIB;
  int format_mib = LFSTEST_FORMAT_MIB;
  int failures = 0;
  int i;
  int ret;
  uint64_t t_all;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-m") == 0)
        {
          mode = LFSTEST_MODE_MOUNT;
        }
      else if (strcmp(argv[i], "-k") == 0)
        {
          keep = true;
        }
      else if (strcmp(argv[i], "-u") == 0)
        {
          do_unmount = true;
        }
      else if (strcmp(argv[i], "-y") == 0)
        {
          format_yes = true;
        }
      else if (strcmp(argv[i], "seq") == 0)
        {
          mode = LFSTEST_MODE_SEQ;
          if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            {
              seq_kib = (size_t)strtoul(argv[++i], NULL, 0);
            }
        }
      else if (strcmp(argv[i], "chunk") == 0)
        {
          mode = LFSTEST_MODE_CHUNK;
          if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            {
              chk_kib = (size_t)strtoul(argv[++i], NULL, 0);
            }
        }
      else if (strcmp(argv[i], "small") == 0)
        {
          mode = LFSTEST_MODE_SMALL;
        }
      else if (strcmp(argv[i], "mkdir") == 0 ||
               strcmp(argv[i], "dir") == 0)
        {
          mode = LFSTEST_MODE_MKDIR;
        }
      else if (strcmp(argv[i], "format") == 0)
        {
          mode = LFSTEST_MODE_FORMAT;
          if (i + 1 < argc &&
              lfstest_parse_format_size(argv[i + 1], &format_mib))
            {
              i++;
            }
        }
      else if (strcmp(argv[i], "grow") == 0)
        {
          mode = LFSTEST_MODE_GROW;
        }
      else if (mode == LFSTEST_MODE_FORMAT &&
               lfstest_parse_format_size(argv[i], &format_mib))
        {
          /* `test lfs format -y 4000` — size after -y */
        }
      else if (strcmp(argv[i], "large") == 0)
        {
          mode = LFSTEST_MODE_LARGE;
          if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            {
              large_kib = atoi(argv[++i]);
            }
        }
      else if (strcmp(argv[i], "all") == 0 ||
               strcmp(argv[i], "speed") == 0)
        {
          mode = LFSTEST_MODE_ALL;
        }
      else if (argv[i][0] >= '0' && argv[i][0] <= '9' &&
               mode == LFSTEST_MODE_ALL)
        {
          seq_kib = (size_t)strtoul(argv[i], NULL, 0);
        }
      else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "help") == 0)
        {
          lfs_usage();
          return EXIT_SUCCESS;
        }
      else
        {
          lfs_usage();
          return EXIT_FAILURE;
        }
    }

  if (mode == LFSTEST_MODE_FORMAT)
    {
      if (!format_yes)
        {
          LOGE("format wipes %s — pass -y to confirm (e.g. test lfs format -y)",
               LFSTEST_MNT);
          return EXIT_FAILURE;
        }

      {
          uint32_t fmt_bytes;

          if (format_mib < 0 || format_mib >= 4096)
            {
              fmt_bytes = SF32LB_SD_LFS_WINDOW_FULL;
            }
          else
            {
              fmt_bytes = (uint32_t)format_mib * 1024u * 1024u;
            }

          if (lfstest_forceformat(fmt_bytes) < 0)
            {
              return EXIT_FAILURE;
            }
        }

      mode = LFSTEST_MODE_ALL;
    }

  if (mode == LFSTEST_MODE_GROW)
    {
      return lfstest_grow() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (mode == LFSTEST_MODE_MOUNT)
    {
      LOGI("==== mount %s -> %s ====", lfstest_dev(), LFSTEST_MNT);
    }
  else
    {
      LOGI("==== layer 2 LittleFS start ====");
    }

  t_all = lfstest_now_us();
  g_lfs_nres = 0;
  LOGI("probe %s via opendir (not statfs)", LFSTEST_MNT);
  if (lfs_mount_volume() < 0)
    {
      return EXIT_FAILURE;
    }

  if (mode == LFSTEST_MODE_MOUNT)
    {
      LOGI("==== mount OK ====");
      return EXIT_SUCCESS;
    }

  if (seq_kib == 0)
    {
      seq_kib = LFSTEST_SEQ_KIB;
    }

  if (chk_kib == 0)
    {
      chk_kib = LFSTEST_CHK_KIB;
    }

  if (mode == LFSTEST_MODE_ALL || mode == LFSTEST_MODE_SEQ ||
      mode == LFSTEST_MODE_CHUNK)
    {
      lfstest_stamp("begin");
      failures += lfstest_run_speed(mode != LFSTEST_MODE_CHUNK,
                                    mode != LFSTEST_MODE_SEQ,
                                    seq_kib, chk_kib);
      lfstest_stamp("end");
    }

  /* Legacy POSIX checks: not part of default speed. */

  if (mode == LFSTEST_MODE_LARGE)
    {
      failures += lfstest_run_large(large_kib, keep);
    }

  if (mode == LFSTEST_MODE_SMALL)
    {
      failures += lfstest_run_small(keep);
    }

  if (mode == LFSTEST_MODE_MKDIR)
    {
      failures += lfstest_run_mkdir(keep);
    }

  if (do_unmount)
    {
      ret = umount(LFSTEST_MNT);
      if (ret < 0)
        {
          LOGE("umount(%s) failed: %d", LFSTEST_MNT, errno);
        }
      else
        {
          LOGI("unmounted %s", LFSTEST_MNT);
        }
    }

  if (failures == 0)
    {
      lfstest_log_us("==== layer 2 LittleFS: PASS", t_all);
      return EXIT_SUCCESS;
    }

  lfstest_log_us("==== layer 2 LittleFS: FAIL", t_all);
  LOGE("==== layer 2 LittleFS: FAIL (%d failures) ====", failures);
  return EXIT_FAILURE;
}

#else /* CONFIG_FS_LITTLEFS && CONFIG_MTD */

int test_lfs_main(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);
  printf("test lfs: requires CONFIG_FS_LITTLEFS and CONFIG_MTD\n");
  return EXIT_FAILURE;
}

#endif /* CONFIG_FS_LITTLEFS && CONFIG_MTD */
