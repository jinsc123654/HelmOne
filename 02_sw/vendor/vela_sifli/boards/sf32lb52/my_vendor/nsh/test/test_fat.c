/**
 * @file test_fat.c
 * @brief `test fat`：`/mnt/fat` POSIX 读写速度。
 *
 * 产品挂 MS_RDONLY，探测写失败即报错退出。工厂（或以后 remount rw）
 * 才能跑测速。
 *
 *   nsh> test fat              seq 1 MiB + 4 KiB chunk 256 KiB
 *   nsh> test fat seq [KiB]    顺序写/读/校验（32 KiB 块，对齐簇）
 *   nsh> test fat chunk [KiB]  4 KiB 写 + fsync（MTP 形态）
 *
 * 开始/结束打 systime + CLOCK_MONOTONIC 时间戳（对照 dmesg [sec.usec]）。
 * 文件落在 `/mnt/fat/fttest/`，不碰 map/ fonts。测完删除。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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
#include <sys/stat.h>

#include "myvendor_watchdog.h"
#include "test_demos.h"

#define FATTEST_MNT      "/mnt/fat"
#define FATTEST_DIR      "/mnt/fat/fttest"
#define FATTEST_SEQ      FATTEST_DIR "/seq.bin"
#define FATTEST_CHUNK    FATTEST_DIR "/chk.bin"
#define FATTEST_PROBE    FATTEST_DIR "/probe.bin"
#define FATTEST_CLUS     32768
#define FATTEST_PAGE     4096
#define FATTEST_SEQ_KIB  1024
#define FATTEST_CHK_KIB  256
#define FATTEST_NRES     8

#define LOGI(fmt, ...) \
  do \
    { \
      printf("test fat: " fmt "\n", ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)
#define LOGE(fmt, ...) \
  do \
    { \
      printf("test fat: ERROR " fmt "\n", ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)

#if defined(CONFIG_FS_FAT)

static struct
{
  char tag[24];
  size_t bytes;
  uint64_t us;
} g_fat_res[FATTEST_NRES];
static unsigned g_fat_nres;

static uint64_t fattest_now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/** @brief 打印可与 dmesg `[sec.usec]` 对齐的时间戳。 */
static void fattest_stamp(FAR const char *when)
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

  syslog(LOG_INFO, "test fat: %s stamp\n", when);
  LOGI("%s stamp systime=%lu.%06lu mono=%lu.%06lu utc=%s",
       when,
       (unsigned long)sys.tv_sec,
       (unsigned long)(sys.tv_nsec / 1000L),
       (unsigned long)mono.tv_sec,
       (unsigned long)(mono.tv_nsec / 1000L),
       cal);
}

static void fattest_rate(FAR const char *tag, size_t bytes, uint64_t us)
{
  if (us == 0)
    {
      us = 1;
    }

  LOGI("%s %zu bytes in %lu ms -> %lu KB/s",
       tag, bytes, (unsigned long)(us / 1000ull),
       (unsigned long)((bytes * 1000ull) / us));
  if (g_fat_nres < FATTEST_NRES)
    {
      snprintf(g_fat_res[g_fat_nres].tag, sizeof(g_fat_res[0].tag), "%s",
               tag);
      g_fat_res[g_fat_nres].bytes = bytes;
      g_fat_res[g_fat_nres].us = us;
      g_fat_nres++;
    }
}

static void fattest_result(void)
{
  unsigned i;

  LOGI("---- result /mnt/fat ----");
  LOGI("  %-16s %8s %7s %8s", "case", "bytes", "ms", "KB/s");
  for (i = 0; i < g_fat_nres; i++)
    {
      uint64_t us = g_fat_res[i].us == 0 ? 1 : g_fat_res[i].us;

      LOGI("  %-16s %8zu %7lu %8lu",
           g_fat_res[i].tag, g_fat_res[i].bytes,
           (unsigned long)(us / 1000ull),
           (unsigned long)((g_fat_res[i].bytes * 1000ull) / us));
    }
}

static void fattest_fill(FAR uint8_t *buf, size_t n, uint32_t seed)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      buf[i] = (uint8_t)((seed + (uint32_t)i) * 0x9eu + 0x5d);
    }
}

static int fattest_check(FAR const uint8_t *buf, size_t n, uint32_t seed)
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

static void fattest_rm(FAR const char *path)
{
  if (unlink(path) < 0 && errno != ENOENT)
    {
      LOGE("unlink(%s) errno=%d", path, errno);
    }
}

static void fattest_ro_error(int err)
{
  LOGE("%s is read-only (errno=%d); remount rw or boot factory",
       FATTEST_MNT, err);
}

/** @brief 探测卷是否可写。只读（产品 MS_RDONLY）立刻失败。 */
static int fattest_require_rw(void)
{
  FAR DIR *dir;
  uint8_t one = 0xa5;
  ssize_t n;
  int fd;
  int ret;

  dir = opendir(FATTEST_MNT);
  if (dir == NULL)
    {
      LOGE("%s not mounted (errno=%d)", FATTEST_MNT, errno);
      return -1;
    }

  closedir(dir);

  ret = mkdir(FATTEST_DIR, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      if (errno == EROFS || errno == EACCES)
        {
          fattest_ro_error(errno);
        }
      else if (errno == ENODEV)
        {
          LOGE("mkdir(%s) errno=%d (ENODEV: FAT write path, not unmounted)",
               FATTEST_DIR, errno);
        }
      else
        {
          LOGE("mkdir(%s) errno=%d", FATTEST_DIR, errno);
        }

      return -1;
    }

  fd = open(FATTEST_PROBE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      if (errno == EROFS || errno == EACCES)
        {
          fattest_ro_error(errno);
        }
      else
        {
          LOGE("probe open errno=%d", errno);
        }

      return -1;
    }

  n = write(fd, &one, 1);
  fsync(fd);
  close(fd);
  fattest_rm(FATTEST_PROBE);
  if (n != 1)
    {
      if (errno == EROFS || errno == EACCES)
        {
          fattest_ro_error(errno);
        }
      else
        {
          LOGE("probe write n=%zd errno=%d", n, errno);
        }

      return -1;
    }

  return 0;
}

static int fattest_prepare(void)
{
  return fattest_require_rw();
}

static int fattest_xfer(FAR const char *path, size_t total, size_t iosz,
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
      LOGE("open(%s) %s errno=%d%s", path, do_write ? "write" : "read",
           errno, errno == EROFS ? " (FAT read-only)" : "");
      free(buf);
      return 1;
    }

  t0 = fattest_now_us();
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
          fattest_fill(buf, want, seed);
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

      if (!do_write && fattest_check(buf, want, seed) < 0)
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
  us = fattest_now_us() - t0;
  fattest_rate(do_write ? "write+fsync" : "read+verify", off, us);
  free(buf);
  return fail;
}

static int fattest_seq(size_t kib)
{
  size_t total = kib * 1024;
  int fail;

  LOGI("---- seq %zu KiB iosz=%u ----", kib, FATTEST_CLUS);
  fattest_rm(FATTEST_SEQ);
  fail = fattest_xfer(FATTEST_SEQ, total, FATTEST_CLUS, true);
  if (fail == 0)
    {
      fail = fattest_xfer(FATTEST_SEQ, total, FATTEST_CLUS, false);
    }

  fattest_rm(FATTEST_SEQ);
  return fail;
}

static int fattest_chunk(size_t kib)
{
  size_t total = kib * 1024;
  int fail;

  LOGI("---- chunk %zu KiB iosz=%u (MTP-like) ----", kib, FATTEST_PAGE);
  fattest_rm(FATTEST_CHUNK);
  fail = fattest_xfer(FATTEST_CHUNK, total, FATTEST_PAGE, true);
  if (fail == 0)
    {
      fail = fattest_xfer(FATTEST_CHUNK, total, FATTEST_PAGE, false);
    }

  fattest_rm(FATTEST_CHUNK);
  return fail;
}

static void fattest_usage(void)
{
  printf("Usage: test fat [seq [KiB]|chunk [KiB]|all]\n"
         "  Needs /mnt/fat mounted read-write (factory). Product is MS_RDONLY\n"
         "  and test fat errors until remounted rw.\n"
         "  seq    sequential write/read/verify, 32 KiB I/O (default %u KiB)\n"
         "  chunk  4 KiB writes + fsync, then read (default %u KiB)\n"
         "  all    seq then chunk (default)\n"
         "  Files: %s  (deleted after the run)\n",
         FATTEST_SEQ_KIB, FATTEST_CHK_KIB, FATTEST_DIR);
}

int test_fat_main(int argc, FAR char *argv[])
{
  enum
    {
      MODE_ALL = 0,
      MODE_SEQ,
      MODE_CHUNK
    } mode = MODE_ALL;
  size_t seq_kib = FATTEST_SEQ_KIB;
  size_t chk_kib = FATTEST_CHK_KIB;
  int fail = 0;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "help") == 0)
        {
          fattest_usage();
          return EXIT_SUCCESS;
        }

      if (strcmp(argv[i], "seq") == 0)
        {
          mode = MODE_SEQ;
          if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            {
              seq_kib = (size_t)strtoul(argv[++i], NULL, 0);
            }
        }
      else if (strcmp(argv[i], "chunk") == 0)
        {
          mode = MODE_CHUNK;
          if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            {
              chk_kib = (size_t)strtoul(argv[++i], NULL, 0);
            }
        }
      else if (strcmp(argv[i], "all") == 0)
        {
          mode = MODE_ALL;
        }
      else if (argv[i][0] >= '0' && argv[i][0] <= '9' && mode == MODE_ALL)
        {
          seq_kib = (size_t)strtoul(argv[i], NULL, 0);
        }
      else
        {
          LOGE("unknown arg '%s'", argv[i]);
          fattest_usage();
          return EXIT_FAILURE;
        }
    }

  if (seq_kib == 0)
    {
      seq_kib = FATTEST_SEQ_KIB;
    }

  if (chk_kib == 0)
    {
      chk_kib = FATTEST_CHK_KIB;
    }

  g_fat_nres = 0;
  fattest_stamp("begin");
  if (fattest_prepare() < 0)
    {
      fattest_stamp("end");
      return EXIT_FAILURE;
    }

  if (mode == MODE_ALL || mode == MODE_SEQ)
    {
      fail += fattest_seq(seq_kib);
    }

  if (mode == MODE_ALL || mode == MODE_CHUNK)
    {
      fail += fattest_chunk(chk_kib);
    }

  fattest_result();
  if (fail == 0)
    {
      LOGI("pass");
    }
  else
    {
      LOGI("FAIL %d", fail);
    }
  fattest_stamp("end");
  return fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#else /* !CONFIG_FS_FAT */

int test_fat_main(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);
  printf("test fat: requires CONFIG_FS_FAT\n");
  return EXIT_FAILURE;
}

#endif /* CONFIG_FS_FAT */
