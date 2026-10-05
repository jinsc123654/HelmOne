/**
 * @file gpx_port.c
 * @brief GPX 平台抽象层实现。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_port.h"

#include "board_malloc.h"
#include "myvendor_watchdog.h"

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* 堆内存                                                                    */
/* ------------------------------------------------------------------------- */

/* 堆内存实现见 gpx_port.h */

void *gpx_port_malloc(size_t size)
{
  return board_malloc_psram(size);
}

void *gpx_port_calloc(size_t nmemb, size_t size)
{
  size_t total;
  void *ptr;

  if (nmemb != 0 && size > (size_t)-1 / nmemb)
    {
      return NULL;
    }

  total = nmemb * size;
  ptr   = board_malloc_psram(total);
  if (ptr != NULL)
    {
      memset(ptr, 0, total);
    }

  return ptr;
}

/**
 * @brief gpx_port_free 接口。
 */
void gpx_port_free(void *ptr)
{
  board_mem_free(ptr);
}

/* ------------------------------------------------------------------------- */
/* 环形队列                                                                  */
/* ------------------------------------------------------------------------- */

/* 环形队列实现见 gpx_port.h */

unsigned gpx_port_queue_depth_for_batch(unsigned batch_size)
{
  if (batch_size == 0)
    {
      return 0;
    }

  return (batch_size * 12 + 9) / 10;
}

/**
 * @brief gpx_port_queue_init 接口。
 * @return 0 成功，负 errno 失败。
 */
void gpx_port_queue_init(gpx_port_queue_t *queue, void *storage,
                         unsigned capacity, unsigned elem_size)
{
  if (queue == NULL)
    {
      return;
    }

  queue->storage = storage;
  queue->capacity = capacity;
  queue->elem_size = elem_size;
  queue->head = 0;
  queue->tail = 0;
}

/**
 * @brief gpx_port_queue_reset 接口。
 */
void gpx_port_queue_reset(gpx_port_queue_t *queue)
{
  if (queue == NULL)
    {
      return;
    }

  queue->head = 0;
  queue->tail = 0;
}

/**
 * @brief gpx_port_queue_count 接口。
 * @return 请求的值。
 */
unsigned gpx_port_queue_count(const gpx_port_queue_t *queue)
{
  if (queue == NULL || queue->capacity == 0)
    {
      return 0;
    }

  if (queue->tail >= queue->head)
    {
      return queue->tail - queue->head;
    }

  return queue->capacity - queue->head + queue->tail;
}

/**
 * @brief gpx_port_queue_free_slots 接口。
 */
unsigned gpx_port_queue_free_slots(const gpx_port_queue_t *queue)
{
  if (queue == NULL || queue->capacity == 0)
    {
      return 0;
    }

  return queue->capacity - gpx_port_queue_count(queue) - 1;
}

/**
 * @brief gpx_port_queue_empty 接口。
 */
bool gpx_port_queue_empty(const gpx_port_queue_t *queue)
{
  return gpx_port_queue_count(queue) == 0;
}

/**
 * @brief gpx_port_queue_full 接口。
 */
bool gpx_port_queue_full(const gpx_port_queue_t *queue)
{
  return gpx_port_queue_free_slots(queue) == 0;
}

/**
 * @brief 获取队列指定逻辑索引对应的 storage 槽指针。
 *
 * @param queue  队列
 * @param index  槽位索引（0 … capacity-1）
 * @return 槽字节指针
 */
static uint8_t *gpx_port_queue_slot(const gpx_port_queue_t *queue,
                                    unsigned index)
{
  return queue->storage + (size_t)index * (size_t)queue->elem_size;
}

/**
 * @brief gpx_port_queue_push 接口。
 */
int gpx_port_queue_push(gpx_port_queue_t *queue, const void *elem)
{
  unsigned next;

  if (queue == NULL || elem == NULL || queue->storage == NULL ||
      queue->capacity < 2 || queue->elem_size == 0)
    {
      return -EINVAL;
    }

  if (gpx_port_queue_full(queue))
    {
      return -ENOSPC;
    }

  memcpy(gpx_port_queue_slot(queue, queue->tail), elem, queue->elem_size);
  next = (queue->tail + 1) % queue->capacity;
  queue->tail = next;
  return 0;
}

const void *gpx_port_queue_peek(const gpx_port_queue_t *queue, unsigned index)
{
  unsigned count;
  unsigned slot;

  if (queue == NULL || queue->storage == NULL)
    {
      return NULL;
    }

  count = gpx_port_queue_count(queue);
  if (index >= count)
    {
      return NULL;
    }

  slot = (queue->head + index) % queue->capacity;
  return gpx_port_queue_slot(queue, slot);
}

/**
 * @brief gpx_port_queue_pop 接口。
 */
int gpx_port_queue_pop(gpx_port_queue_t *queue, void *elem)
{
  if (queue == NULL || gpx_port_queue_empty(queue))
    {
      return -EINVAL;
    }

  if (elem != NULL)
    {
      memcpy(elem, gpx_port_queue_slot(queue, queue->head),
             queue->elem_size);
    }

  queue->head = (queue->head + 1) % queue->capacity;
  return 0;
}

/**
 * @brief gpx_port_queue_advance 接口。
 */
int gpx_port_queue_advance(gpx_port_queue_t *queue, unsigned count)
{
  unsigned n;

  if (queue == NULL)
    {
      return -EINVAL;
    }

  n = gpx_port_queue_count(queue);
  if (count == 0 || count > n)
    {
      return -EINVAL;
    }

  queue->head = (queue->head + count) % queue->capacity;
  return 0;
}

/* ------------------------------------------------------------------------- */
/* 文件 I/O                                                                  */
/* ------------------------------------------------------------------------- */

static void gpx_port_file_clear_stream(gpx_port_file_t *file)
{
  file->rpos = 0;
  file->rlen = 0;
  file->wlen = 0;
  file->rcap = 0;
  file->wcap = 0;
  file->created = false;
  file->rbuf = NULL;
  file->wbuf = NULL;
  file->path[0] = '\0';
}

static void gpx_port_file_free_stream(gpx_port_file_t *file)
{
  gpx_port_free(file->rbuf);
  gpx_port_free(file->wbuf);
  gpx_port_file_clear_stream(file);
}

static uint8_t *gpx_port_file_alloc_buf(uint16_t *cap_out)
{
  uint8_t *p = gpx_port_malloc(GPX_PORT_STREAM_BUF);

  if (p != NULL)
    {
      *cap_out = (uint16_t)GPX_PORT_STREAM_BUF;
      return p;
    }

  p = gpx_port_malloc(GPX_PORT_STREAM_BUF_MIN);
  if (p != NULL)
    {
      *cap_out = (uint16_t)GPX_PORT_STREAM_BUF_MIN;
    }

  return p;
}

static uint8_t *gpx_port_file_alloc_write(uint32_t *cap_out)
{
  static const uint32_t sizes[] =
    {
      GPX_PORT_WRITE_BUF,
      GPX_PORT_WRITE_BUF / 2u,
      GPX_PORT_WRITE_BUF / 4u,
      GPX_PORT_STREAM_BUF,
      GPX_PORT_STREAM_BUF_MIN
    };
  unsigned i;

  for (i = 0; i < (unsigned)(sizeof(sizes) / sizeof(sizes[0])); i++)
    {
      uint8_t *p = gpx_port_malloc(sizes[i]);

      if (p != NULL)
        {
          *cap_out = sizes[i];
          return p;
        }
    }

  *cap_out = 0;
  return NULL;
}

static ssize_t gpx_port_sys_read(int fd, void *buf, size_t len)
{
  ssize_t n;

  do
    {
      n = read(fd, buf, len);
    }
  while (n < 0 && errno == EINTR);

  return n;
}

#ifndef GPX_PORT_IO_BOUNCE
#  define GPX_PORT_IO_BOUNCE  256
#endif

#ifndef GPX_PORT_FLUSH_TRY
#  define GPX_PORT_FLUSH_TRY  4
#endif

/*
 * LittleFS/SD 的 user buffer 经 SRAM bounce。PSRAM wbuf 只作合并/重试，
 * 内核 write 始终拷自 bounce，避免 PSRAM 指针进 VFS。
 * *wrote 为已进内核的字节；失败时调用方只丢掉这一截，避免整段重写重复。
 */
static int gpx_port_sys_write_all(int fd, const void *data, size_t len,
                                  size_t *wrote)
{
  const uint8_t *p = data;
  uint8_t bounce[GPX_PORT_IO_BOUNCE];
  size_t done = 0;

  while (len > 0)
    {
      size_t chunk = (len < sizeof(bounce)) ? len : sizeof(bounce);
      size_t off = 0;

      memcpy(bounce, p, chunk);
      while (off < chunk)
        {
          ssize_t n;

          do
            {
              n = write(fd, bounce + off, chunk - off);
            }
          while (n < 0 && errno == EINTR);

          if (n < 0)
            {
              if (wrote != NULL)
                {
                  *wrote = done + off;
                }

              return -errno;
            }

          if (n == 0)
            {
              if (wrote != NULL)
                {
                  *wrote = done + off;
                }

              return -EIO;
            }

          off += (size_t)n;
        }

      p += chunk;
      len -= chunk;
      done += chunk;
      myvendor_watchdog_hw_pet();
    }

  if (wrote != NULL)
    {
      *wrote = done;
    }

  return 0;
}

static void gpx_port_file_consume_w(gpx_port_file_t *file, uint32_t n)
{
  if (file == NULL || n == 0 || file->wbuf == NULL)
    {
      return;
    }

  if (n >= file->wlen)
    {
      file->wlen = 0;
      return;
    }

  memmove(file->wbuf, file->wbuf + n, (size_t)(file->wlen - n));
  file->wlen -= n;
}

static int gpx_port_file_flush_once(gpx_port_file_t *file)
{
  size_t wrote = 0;
  int ret;

  if (file == NULL || file->wbuf == NULL || file->wlen == 0)
    {
      return 0;
    }

  ret = gpx_port_sys_write_all(file->fd, file->wbuf, file->wlen, &wrote);
  gpx_port_file_consume_w(file, (uint32_t)wrote);
  return ret;
}

static int gpx_port_file_flush_try(gpx_port_file_t *file)
{
  int ret = 0;
  unsigned i;

  if (file == NULL || file->wbuf == NULL || file->wlen == 0)
    {
      return 0;
    }

  for (i = 0; i < GPX_PORT_FLUSH_TRY; i++)
    {
      ret = gpx_port_file_flush_once(file);
      if (ret == 0)
        {
          return 0;
        }

      myvendor_watchdog_hw_pet();
      if (i + 1u < GPX_PORT_FLUSH_TRY)
        {
          gpx_port_sleep_ms(20);
        }
    }

  return ret;
}

/* 缓存满或关文件：没关机就一直刷，不把已攒的点扔掉。 */
static int gpx_port_file_flush_wait(gpx_port_file_t *file)
{
  unsigned n = 0;

  if (file == NULL || file->wbuf == NULL || file->wlen == 0)
    {
      return 0;
    }

  for (;;)
    {
      int ret = gpx_port_file_flush_once(file);

      if (ret == 0)
        {
          return 0;
        }

      n++;
      if ((n % 64u) == 1u)
        {
          printf("gpx: flush wait %u (%d) keep %lu\n", n, ret,
                 (unsigned long)file->wlen);
        }

      myvendor_watchdog_hw_pet();
      gpx_port_sleep_ms(50);
    }
}

/* 开录时占住空文件，后续 dump 走 O_APPEND，停表不必再创 inode。 */
#ifndef GPX_PORT_PLANT_TRY
#  define GPX_PORT_PLANT_TRY  40
#endif

static int gpx_port_file_plant_empty(const char *path)
{
  unsigned n = 0;

  if (path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  for (;;)
    {
      int fd;

      myvendor_watchdog_hw_pet();
      fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd >= 0)
        {
          (void)fsync(fd);
          (void)close(fd);
          return 0;
        }

      n++;
      if (n >= GPX_PORT_PLANT_TRY)
        {
          return -errno;
        }

      gpx_port_sleep_ms(50);
    }
}

/* open → write wbuf → fsync → close。成功才把 created 置位。 */
static int gpx_port_file_commit(gpx_port_file_t *file, bool wait)
{
  unsigned n = 0;

  if (file == NULL || file->wbuf == NULL || file->wlen == 0)
    {
      return 0;
    }

  if (file->path[0] == '\0')
    {
      return -EINVAL;
    }

  for (;;)
    {
      int flags = O_WRONLY | O_CREAT | (file->created ? O_APPEND : O_TRUNC);
      int fd;
      int ret;
      uint32_t before;

      fd = open(file->path, flags, 0666);
      if (fd < 0)
        {
          ret = -errno;
          if (!wait)
            {
              return ret;
            }

          n++;
          if ((n % 64u) == 1u)
            {
              printf("gpx: open wait %u (%d) keep %lu\n", n, ret,
                     (unsigned long)file->wlen);
            }

          myvendor_watchdog_hw_pet();
          gpx_port_sleep_ms(50);
          continue;
        }

      file->fd = fd;
      before = file->wlen;
      ret = wait ? gpx_port_file_flush_wait(file)
                 : gpx_port_file_flush_try(file);
      if (file->wlen < before)
        {
          file->created = true;
        }

      if (ret == 0)
        {
          unsigned fs = 0;

          while (fsync(fd) != 0)
            {
              int err = -errno;

              if (!wait)
                {
                  (void)close(fd);
                  file->fd = -1;
                  return err;
                }

              fs++;
              if ((fs % 64u) == 1u)
                {
                  printf("gpx: fsync wait %u (%d%s)\n", fs, err,
                         err == -EFAULT ? " lfs-corrupt" : "");
                }

              myvendor_watchdog_hw_pet();
              gpx_port_sleep_ms(50);
            }

          (void)close(fd);
          file->fd = -1;
          printf("gpx: dump %lu then close\n", (unsigned long)before);
          return 0;
        }

      (void)close(fd);
      file->fd = -1;
      if (!wait)
        {
          return ret;
        }

      n++;
      if ((n % 64u) == 1u)
        {
          printf("gpx: dump wait %u (%d) keep %lu\n", n, ret,
                 (unsigned long)file->wlen);
        }

      myvendor_watchdog_hw_pet();
      gpx_port_sleep_ms(50);
    }
}

int gpx_port_file_open_read(gpx_port_file_t *file, const char *path)
{
  uint8_t *rbuf;
  uint16_t rcap = 0;

  if (file == NULL || path == NULL)
    {
      return -EINVAL;
    }

  gpx_port_file_clear_stream(file);
  file->fd = -1;

  rbuf = gpx_port_file_alloc_buf(&rcap);
  if (rbuf == NULL)
    {
      return -ENOMEM;
    }

  file->fd = open(path, O_RDONLY);
  if (file->fd < 0)
    {
      int err = -errno;

      gpx_port_free(rbuf);
      return err;
    }

  file->rbuf = rbuf;
  file->rcap = rcap;
  return 0;
}

static int gpx_port_file_open_out(gpx_port_file_t *file, const char *path,
                                  bool plant)
{
  uint8_t *wbuf;
  uint32_t wcap = 0;

  if (file == NULL || path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  if (strlen(path) >= GPX_PORT_PATH_MAX)
    {
      return -ENAMETOOLONG;
    }

  if (!plant)
    {
      struct stat st;

      if (stat(path, &st) != 0)
        {
          return -errno;
        }
    }

  gpx_port_file_clear_stream(file);
  file->fd = -1;

  wbuf = gpx_port_file_alloc_write(&wcap);
  snprintf(file->path, sizeof(file->path), "%s", path);
  file->wbuf = wbuf;
  file->wcap = (wbuf != NULL) ? wcap : 0;
  file->created = false;

  if (plant)
    {
      int pret = gpx_port_file_plant_empty(path);

      if (pret != 0)
        {
          gpx_port_free(wbuf);
          file->wbuf = NULL;
          file->wcap = 0;
          file->path[0] = '\0';
          printf("gpx: empty %s failed %d\n", path, pret);
          return pret;
        }

      printf("gpx: empty %s\n", path);
    }

  file->created = true;

  if (wbuf == NULL)
    {
      file->fd = open(path, O_WRONLY | O_APPEND, 0666);
      if (file->fd < 0)
        {
          int err = -errno;

          file->path[0] = '\0';
          file->created = false;
          return err;
        }

      printf("gpx: write cache none, write-through\n");
      return 0;
    }

  printf("gpx: write cache %lu KiB, dump on full/close\n",
         (unsigned long)(wcap / 1024u));
  return 0;
}

int gpx_port_file_open_write(gpx_port_file_t *file, const char *path)
{
  return gpx_port_file_open_out(file, path, true);
}

int gpx_port_file_open_append(gpx_port_file_t *file, const char *path)
{
  return gpx_port_file_open_out(file, path, false);
}

int gpx_port_file_close(gpx_port_file_t *file)
{
  int ret = 0;

  if (file == NULL)
    {
      return 0;
    }

  if (file->wbuf != NULL && file->wlen > 0)
    {
      ret = gpx_port_file_commit(file, true);
    }
  else if (file->fd >= 0)
    {
      unsigned n = 0;

      for (;;)
        {
          if (fsync(file->fd) == 0)
            {
              break;
            }

          n++;
          if ((n % 64u) == 1u)
            {
              printf("gpx: fsync wait %u (%d%s)\n", n, -errno,
                     errno == EFAULT ? " lfs-corrupt" : "");
            }

          myvendor_watchdog_hw_pet();
          gpx_port_sleep_ms(50);
        }

      if (close(file->fd) != 0 && ret == 0)
        {
          ret = -errno;
        }

      file->fd = -1;
    }

  if (file->fd >= 0)
    {
      (void)close(file->fd);
      file->fd = -1;
    }

  gpx_port_file_free_stream(file);
  return ret;
}

int gpx_port_file_read_byte(gpx_port_file_t *file)
{
  if (file == NULL || file->fd < 0 || file->rbuf == NULL || file->rcap == 0)
    {
      return -1;
    }

  if (file->rpos >= file->rlen)
    {
      ssize_t n = gpx_port_sys_read(file->fd, file->rbuf, file->rcap);

      if (n <= 0)
        {
          file->rpos = 0;
          file->rlen = 0;
          return -1;
        }

      file->rpos = 0;
      file->rlen = (uint16_t)n;
    }

  return (int)file->rbuf[file->rpos++];
}

int gpx_port_file_size(gpx_port_file_t *file, uint32_t *size_out)
{
  struct stat st;

  if (file == NULL || file->fd < 0 || size_out == NULL)
    {
      return -EINVAL;
    }

  if (fstat(file->fd, &st) != 0)
    {
      return -errno;
    }

  if (st.st_size < 0)
    {
      return -EINVAL;
    }

  if ((uint64_t)st.st_size > (uint64_t)GPX_PORT_FILE_MAX)
    {
      return -EFBIG;
    }

  *size_out = (uint32_t)st.st_size;
  return 0;
}

int gpx_port_file_write(gpx_port_file_t *file, const void *data, size_t len)
{
  const uint8_t *p = data;
  int ret;

  if (file == NULL || data == NULL)
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return 0;
    }

  if (file->wbuf == NULL || file->wcap == 0)
    {
      if (file->fd < 0)
        {
          return -EINVAL;
        }

      return gpx_port_sys_write_all(file->fd, data, len, NULL);
    }

  while (len > 0)
    {
      uint32_t room = file->wcap - file->wlen;
      size_t n;

      if (room == 0)
        {
          ret = gpx_port_file_commit(file, true);
          if (ret != 0)
            {
              return ret;
            }

          room = file->wcap;
        }

      n = (len < (size_t)room) ? len : (size_t)room;
      memcpy(file->wbuf + file->wlen, p, n);
      file->wlen += (uint32_t)n;
      p += n;
      len -= n;
    }

  return 0;
}

/**
 * @brief gpx_port_file_write_str 接口。
 */
int gpx_port_file_write_str(gpx_port_file_t *file, const char *str)
{
  size_t len;

  if (str == NULL)
    {
      return 0;
    }

  len = strlen(str);
  return gpx_port_file_write(file, str, len);
}

/**
 * @brief gpx_port_file_write_fmt 接口。
 */
int gpx_port_file_write_fmt(gpx_port_file_t *file, const char *fmt, ...)
{
  char buf[512];
  va_list ap;
  int n;

  if (file == NULL || fmt == NULL)
    {
      return -EINVAL;
    }

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n < 0 || (size_t)n >= sizeof(buf))
    {
      return -ENOMEM;
    }

  return gpx_port_file_write_str(file, buf);
}

/**
 * @brief gpx_port_file_flush 接口。
 */
int gpx_port_file_flush(gpx_port_file_t *file)
{
  int ret;
  static unsigned s_defer;

  if (file == NULL)
    {
      return -EINVAL;
    }

  if (file->wbuf == NULL || file->wlen == 0)
    {
      return 0;
    }

  ret = gpx_port_file_commit(file, false);
  if (ret != 0)
    {
      s_defer++;
      if ((s_defer % 8u) == 1u)
        {
          printf("gpx: dump defer %d%s keep %lu\n", ret,
                 (ret == -EFAULT) ? " lfs-corrupt" : "",
                 (unsigned long)file->wlen);
        }
    }

  return ret;
}

/**
 * @brief gpx_port_file_sync 接口。
 */
int gpx_port_file_sync(gpx_port_file_t *file)
{
  if (file == NULL)
    {
      return -EINVAL;
    }

  if (file->wbuf != NULL)
    {
      return gpx_port_file_commit(file, true);
    }

  if (file->fd < 0)
    {
      return -EINVAL;
    }

  if (fsync(file->fd) != 0)
    {
      return -errno;
    }

  myvendor_watchdog_hw_pet();
  return 0;
}

/**
 * @brief gpx_port_file_ensure_parent_dir 接口。
 */
int gpx_port_file_ensure_parent_dir(const char *path)
{
  char parent[256];
  const char *slash;
  size_t len;

  if (path == NULL)
    {
      return -EINVAL;
    }

  slash = strrchr(path, '/');
  if (slash == NULL || slash == path)
    {
      return 0;
    }

  len = (size_t)(slash - path);
  if (len == 0 || len >= sizeof(parent))
    {
      return -ENAMETOOLONG;
    }

  memcpy(parent, path, len);
  parent[len] = '\0';

  if (mkdir(parent, 0755) != 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

/* ------------------------------------------------------------------------- */
/* 线程与同步（POSIX pthread + sem）                                         */
/* ------------------------------------------------------------------------- */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(pthread_mutex_t) <= GPX_PORT_MUTEX_STORAGE,
               "GPX_PORT_MUTEX_STORAGE too small");
_Static_assert(sizeof(sem_t) <= GPX_PORT_SEM_STORAGE,
               "GPX_PORT_SEM_STORAGE too small");
_Static_assert(sizeof(pthread_t) <= GPX_PORT_THREAD_STORAGE,
               "GPX_PORT_THREAD_STORAGE too small");
#endif

/** 从 opaque storage 取出底层 pthread_mutex_t。 */
static pthread_mutex_t *gpx_port_mutex_native(gpx_port_mutex_t *mutex)
{
  return (pthread_mutex_t *)(void *)mutex->storage;
}

/** 从 opaque storage 取出底层 sem_t。 */
static sem_t *gpx_port_sem_native(gpx_port_sem_t *sem)
{
  return (sem_t *)(void *)sem->storage;
}

/** 从 opaque storage 取出底层 pthread_t。 */
static pthread_t *gpx_port_thread_native(gpx_port_thread_t *thread)
{
  return (pthread_t *)(void *)thread->storage;
}

/* 线程与同步实现见 gpx_port.h */

int gpx_port_mutex_init(gpx_port_mutex_t *mutex)
{
  if (mutex == NULL)
    {
      return -EINVAL;
    }

  if (pthread_mutex_init(gpx_port_mutex_native(mutex), NULL) != 0)
    {
      return -errno;
    }

  return 0;
}

/**
 * @brief gpx_port_mutex_destroy 接口。
 */
void gpx_port_mutex_destroy(gpx_port_mutex_t *mutex)
{
  if (mutex == NULL)
    {
      return;
    }

  pthread_mutex_destroy(gpx_port_mutex_native(mutex));
}

/**
 * @brief gpx_port_mutex_lock 接口。
 */
void gpx_port_mutex_lock(gpx_port_mutex_t *mutex)
{
  if (mutex != NULL)
    {
      pthread_mutex_lock(gpx_port_mutex_native(mutex));
    }
}

/**
 * @brief gpx_port_mutex_unlock 接口。
 */
void gpx_port_mutex_unlock(gpx_port_mutex_t *mutex)
{
  if (mutex != NULL)
    {
      pthread_mutex_unlock(gpx_port_mutex_native(mutex));
    }
}

/**
 * @brief gpx_port_sem_init 接口。
 * @return 0 成功，负 errno 失败。
 */
int gpx_port_sem_init(gpx_port_sem_t *sem, unsigned initial)
{
  if (sem == NULL)
    {
      return -EINVAL;
    }

  if (sem_init(gpx_port_sem_native(sem), 0, initial) != 0)
    {
      return -errno;
    }

  return 0;
}

/**
 * @brief gpx_port_sem_destroy 接口。
 */
void gpx_port_sem_destroy(gpx_port_sem_t *sem)
{
  if (sem == NULL)
    {
      return;
    }

  sem_destroy(gpx_port_sem_native(sem));
}

/**
 * @brief gpx_port_sem_post 接口。
 */
void gpx_port_sem_post(gpx_port_sem_t *sem)
{
  if (sem != NULL)
    {
      sem_post(gpx_port_sem_native(sem));
    }
}

/**
 * @brief gpx_port_sem_wait 接口。
 */
int gpx_port_sem_wait(gpx_port_sem_t *sem)
{
  if (sem == NULL)
    {
      return -EINVAL;
    }

  if (sem_wait(gpx_port_sem_native(sem)) != 0)
    {
      return -errno;
    }

  return 0;
}

/**
 * @brief gpx_port_thread_create 接口。
 * @return 0 成功，负 errno 失败。
 */
int gpx_port_thread_create(gpx_port_thread_t *thread,
                           gpx_port_thread_entry_t entry, void *arg,
                           unsigned stack_size, unsigned priority,
                           const char *name, void *stack_addr)
{
  pthread_attr_t attr;
  struct sched_param sparam;
  int ret;

  if (thread == NULL || entry == NULL)
    {
      return -EINVAL;
    }

  pthread_attr_init(&attr);

  if (stack_addr != NULL && stack_size > 0)
    {
      ret = pthread_attr_setstack(&attr, stack_addr, stack_size);
      if (ret != 0)
        {
          pthread_attr_destroy(&attr);
          return -ret;
        }
    }
  else if (stack_size > 0)
    {
      pthread_attr_setstacksize(&attr, stack_size);
    }

  sparam.sched_priority = (int)priority;
  pthread_attr_setschedparam(&attr, &sparam);

  ret = pthread_create(gpx_port_thread_native(thread), &attr, entry, arg);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      return -ret;
    }

#ifndef CONFIG_DISABLE_PTHREAD
  if (name != NULL && name[0] != '\0')
    {
      pthread_setname_np(*gpx_port_thread_native(thread), name);
    }
#endif

  return 0;
}

/**
 * @brief gpx_port_thread_join 接口。
 */
int gpx_port_thread_join(gpx_port_thread_t *thread)
{
  if (thread == NULL)
    {
      return -EINVAL;
    }

  if (pthread_join(*gpx_port_thread_native(thread), NULL) != 0)
    {
      return -errno;
    }

  return 0;
}

/**
 * @brief gpx_port_sleep_ms 接口。
 */
void gpx_port_sleep_ms(unsigned ms)
{
  if (ms == 0)
    {
      return;
    }

  usleep((useconds_t)ms * 1000U);
}