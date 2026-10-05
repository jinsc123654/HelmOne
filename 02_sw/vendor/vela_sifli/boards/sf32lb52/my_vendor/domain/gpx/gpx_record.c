/**
 * @file gpx_record.c
 * @brief 句柄化 GPX 录制：每句柄一线程；data 队列 + cmd 队列线程间通信。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_record.h"

#include <nuttx/config.h>

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_malloc.h"
#include "gpx_ext.h"
#include "gpx_port.h"
#include "gpx_writer.h"

#ifndef CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE
#  define CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE 32
#endif

#ifndef CONFIG_MYVENDOR_GPX_RECORD_STACKSIZE
#  define CONFIG_MYVENDOR_GPX_RECORD_STACKSIZE 8192
#endif

#define GPX_RECORD_STACK_ALIGN 16u
static void *g_gpx_record_stack_raw;
static void *g_gpx_record_stack;
static bool g_gpx_record_stack_busy;

static bool gpx_record_stack_alloc(void)
{
  uintptr_t aligned;

  if (g_gpx_record_stack != NULL)
    {
      return true;
    }

  g_gpx_record_stack_raw = board_malloc_psram(
      CONFIG_MYVENDOR_GPX_RECORD_STACKSIZE + GPX_RECORD_STACK_ALIGN - 1u);
  if (g_gpx_record_stack_raw == NULL ||
      !board_ptr_in_psram_pool(g_gpx_record_stack_raw))
    {
      board_mem_free(g_gpx_record_stack_raw);
      g_gpx_record_stack_raw = NULL;
      return false;
    }

  aligned = ((uintptr_t)g_gpx_record_stack_raw + GPX_RECORD_STACK_ALIGN - 1u) &
      ~((uintptr_t)GPX_RECORD_STACK_ALIGN - 1u);
  g_gpx_record_stack = (void *)aligned;
  return true;
}

static void gpx_record_stack_free(void)
{
  board_free_psram(g_gpx_record_stack_raw);
  g_gpx_record_stack_raw = NULL;
  g_gpx_record_stack = NULL;
}

#ifndef GPX_RECORD_CMD_QUEUE_DEPTH
#  define GPX_RECORD_CMD_QUEUE_DEPTH  16
#endif

/** cmd 队列消息类型。 */
typedef enum gpx_record_cmd_id
{
  GPX_RECORD_CMD_START = 0,    /* 打开 GPX，开始会话 */
  GPX_RECORD_CMD_PAUSE,        /* 暂停刷盘 */
  GPX_RECORD_CMD_RESUME,       /* 恢复刷盘 */
  GPX_RECORD_CMD_STOP,         /* 尾批 + close，线程存活 */
  GPX_RECORD_CMD_THREAD_EXIT,  /* 空闲退出线程（stop 后回收） */
  GPX_RECORD_CMD_RELEASE,      /* stop + 线程退出 + destroy 句柄 */
} gpx_record_cmd_id_t;

/** 经 cmd 队列传递给工作线程的控制消息。 */
typedef struct gpx_record_cmd_msg
{
  gpx_record_cmd_id_t id;
  char filepath[256];           /* 输出 GPX 路径（START） */
  char meta_creator[32];
  char meta_name[64];
  char meta_desc[64];
  char track_name[128];
  char track_desc[64];
  bool append;                  /* START：打开已有文件追加，不写头 */
} gpx_record_cmd_msg_t;

/** 录制器句柄内部状态（对外不透明）。 */
struct gpx_record
{
  bool thread_alive;            /* create 后 true，线程退出后置 false */
  bool thread_uses_static_stack; /* 占用单例 BoardPSRAM 外部栈 */
  bool recording;               /* 文件已 open，可稳定 push */
  bool start_pending;           /* start 已投递，fopen 尚未完成 */
  bool paused;                  /* pause 置 true，resume 清零 */
  bool stop_drain;              /* stop/release 时尝试刷完 data */
  bool release_pending;         /* release 命令已投递，线程即将退出 */
  bool thread_exit_pending;     /* thread_exit：stop 后回收线程 */

  unsigned batch_size;          /* 批写 N */
  unsigned data_depth;          /* data 环形容量 */
  size_t fmt_buf_size;          /* 批写 XML 缓冲字节数 */

  gpx_port_thread_t thread;
  gpx_port_mutex_t lock;
  gpx_port_sem_t sem_wake;

  gpx_port_queue_t data_q;      /* 轨迹点队列 */
  gpx_point_t *data_storage;

  gpx_port_queue_t cmd_q;       /* 控制命令队列 */
  gpx_record_cmd_msg_t *cmd_storage;

  gpx_writer_t writer;
  gpx_record_stats_t stats;

  char *fmt_buf;

  char filepath[256];
  gpx_meta_t meta;              /* 指针指向下方内部字符串缓冲 */
  char meta_creator[32];
  char meta_name[64];
  char meta_desc[64];
  char track_name[128];
  char track_desc[64];
};

/**
 * @brief 安全拷贝 C 字符串到定长缓冲区。
 *
 * @param dst   目标缓冲区
 * @param size  目标容量（字节）
 * @param src   源字符串；NULL 时写入空串
 */
static void gpx_strcopy(char *dst, size_t size, const char *src)
{
  if (size == 0)
    {
      return;
    }

  if (src == NULL)
    {
      dst[0] = '\0';
      return;
    }

  snprintf(dst, size, "%s", src);
}

/**
 * @brief 解析并校验录制器创建配置（batch / queue depth）。
 *
 * @param cfg         输入配置；可为 NULL
 * @param batch_size  输出批写大小 N
 * @param data_depth  输出 data 队列深度
 * @return 0 成功，-EINVAL 参数非法
 */
static int gpx_record_resolve_cfg(const gpx_record_cfg_t *cfg,
                                  unsigned *batch_size,
                                  unsigned *data_depth)
{
  unsigned batch;
  unsigned depth;

  if (cfg != NULL && cfg->batch_size > 0)
    {
      batch = cfg->batch_size;
    }
  else
    {
      batch = CONFIG_MYVENDOR_GPX_DEFAULT_BATCH_SIZE;
    }

  if (cfg != NULL && cfg->queue_depth > 0)
    {
      depth = cfg->queue_depth;
    }
  else
    {
      depth = gpx_port_queue_depth_for_batch(batch);
    }

  if (depth < batch || depth < 2)
    {
      return -EINVAL;
    }

  *batch_size = batch;
  *data_depth = depth;
  return 0;
}

/**
 * @brief 从 start 命令消息填充 rec 内部 meta 字符串与指针。
 *
 * @param rec  录制器句柄
 * @param cmd  start 命令
 */
static void gpx_record_meta_from_cmd(gpx_record_t *rec,
                                     const gpx_record_cmd_msg_t *cmd)
{
  gpx_strcopy(rec->meta_creator, sizeof(rec->meta_creator), cmd->meta_creator);
  gpx_strcopy(rec->meta_name, sizeof(rec->meta_name), cmd->meta_name);
  gpx_strcopy(rec->meta_desc, sizeof(rec->meta_desc), cmd->meta_desc);
  gpx_strcopy(rec->track_name, sizeof(rec->track_name), cmd->track_name);
  gpx_strcopy(rec->track_desc, sizeof(rec->track_desc), cmd->track_desc);

  rec->meta.creator = rec->meta_creator;
  rec->meta.meta_name = rec->meta_name;
  rec->meta.meta_desc = rec->meta_desc;
  rec->meta.track_name = rec->track_name;
  rec->meta.track_desc = rec->track_desc;
}

/**
 * @brief 构造 GPX_RECORD_CMD_START 命令消息。
 *
 * @param cmd       输出命令
 * @param filepath  输出 GPX 路径
 * @param meta      元数据；NULL 用默认
 */
static void gpx_record_fill_start_cmd(gpx_record_cmd_msg_t *cmd,
                                      const char *filepath,
                                      const gpx_meta_t *meta,
                                      bool append)
{
  const char *creator = CONFIG_MYVENDOR_PRODUCT_NAME;
  const char *meta_name = CONFIG_MYVENDOR_PRODUCT_NAME;
  const char *meta_desc = "";
  const char *track_name = filepath;
  const char *track_desc = "";

  memset(cmd, 0, sizeof(*cmd));
  cmd->id = GPX_RECORD_CMD_START;
  cmd->append = append;
  gpx_strcopy(cmd->filepath, sizeof(cmd->filepath), filepath);

  if (meta != NULL)
    {
      if (meta->creator != NULL)
        {
          creator = meta->creator;
        }

      if (meta->meta_name != NULL)
        {
          meta_name = meta->meta_name;
        }

      if (meta->meta_desc != NULL)
        {
          meta_desc = meta->meta_desc;
        }

      if (meta->track_name != NULL)
        {
          track_name = meta->track_name;
        }

      if (meta->track_desc != NULL)
        {
          track_desc = meta->track_desc;
        }
    }

  gpx_strcopy(cmd->meta_creator, sizeof(cmd->meta_creator), creator);
  gpx_strcopy(cmd->meta_name, sizeof(cmd->meta_name), meta_name);
  gpx_strcopy(cmd->meta_desc, sizeof(cmd->meta_desc), meta_desc);
  gpx_strcopy(cmd->track_name, sizeof(cmd->track_name), track_name);
  gpx_strcopy(cmd->track_desc, sizeof(cmd->track_desc), track_desc);
}

/**
 * @brief 释放 data 队列队首起 count 个点的扩展 payload（须已加锁）。
 *
 * @param rec    录制器句柄
 * @param count  点数
 */
static void gpx_record_free_data_ext_locked(gpx_record_t *rec, unsigned count)
{
  unsigned i;

  for (i = 0; i < count; i++)
    {
      gpx_point_t *point = (gpx_point_t *)gpx_port_queue_peek(&rec->data_q, i);

      if (point != NULL)
        {
          gpx_ext_point_free_ext(point);
        }
    }
}

/**
 * @brief 清空 data 队列并释放其中所有扩展 payload（须已加锁）。
 *
 * @param rec  录制器句柄
 */
static void gpx_record_drain_data_ext_locked(gpx_record_t *rec)
{
  unsigned count = gpx_port_queue_count(&rec->data_q);

  if (count > 0)
    {
      gpx_record_free_data_ext_locked(rec, count);
      gpx_port_queue_reset(&rec->data_q);
    }
}

/**
 * @brief 将队首 count 个点格式化为 XML 写入 fmt_buf（须已加锁）。
 *
 * @param rec      录制器句柄
 * @param count    批大小
 * @param out_len  输出格式化字节数
 * @return 0 成功，负 errno 失败
 */
static int gpx_record_format_batch_locked(gpx_record_t *rec, unsigned count,
                                          size_t *out_len)
{
  size_t off = 0;
  unsigned i;

  if (count == 0 || count > rec->batch_size || rec->fmt_buf == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < count; i++)
    {
      const gpx_point_t *point;
      int n;

      point = gpx_port_queue_peek(&rec->data_q, i);
      if (point == NULL)
        {
          return -EINVAL;
        }

      n = gpx_writer_format_trkpt(point, rec->fmt_buf + off,
                                  rec->fmt_buf_size - off);
      if (n < 0)
        {
          return n;
        }

      off += (size_t)n;
    }

  *out_len = off;
  return 0;
}

/**
 * @brief 格式化并写盘一批点，随后 advance 队列并更新统计（须已加锁）。
 *
 * @param rec    录制器句柄
 * @param count  本批点数
 * @return 0 成功，负 errno 失败
 */
static int gpx_record_flush_batch_locked(gpx_record_t *rec, unsigned count)
{
  size_t len;
  int ret;

  ret = gpx_record_format_batch_locked(rec, count, &len);
  if (ret != 0)
    {
      return ret;
    }

  /* 锁外 memcpy 进 128KiB。骑行中不 write()：地图跟车若再碰 SD，会 LFS_CORRUPT。 */
  gpx_port_mutex_unlock(&rec->lock);
  ret = gpx_writer_write_bytes(&rec->writer, rec->fmt_buf, len);

  gpx_port_mutex_lock(&rec->lock);
  if (ret != 0)
    {
      return ret;
    }

  gpx_record_free_data_ext_locked(rec, count);
  ret = gpx_port_queue_advance(&rec->data_q, count);
  if (ret != 0)
    {
      return ret;
    }

  rec->stats.points_written += count;
  rec->stats.batches_written++;
  return 0;
}

/**
 * @brief 刷写 data 队列中所有待写点（尾批可小于 N）（须已加锁）。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
static int gpx_record_flush_pending_locked(gpx_record_t *rec)
{
  unsigned count;
  int ret = 0;

  count = gpx_port_queue_count(&rec->data_q);
  while (count > 0)
    {
      unsigned batch = rec->batch_size;

      if (count < rec->batch_size)
        {
          batch = count;
        }

      ret = gpx_record_flush_batch_locked(rec, batch);
      if (ret != 0)
        {
          printf("gpx_record: tail flush failed (%d)\n", ret);
          return ret;
        }

      count = gpx_port_queue_count(&rec->data_q);
    }

  return 0;
}

/**
 * @brief 结束当前录制会话：刷剩余点并 close 文件（须已加锁）。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
static int gpx_record_close_session_locked(gpx_record_t *rec)
{
  int ret = 0;
  unsigned i;

  if (!rec->recording)
    {
      rec->stop_drain = false;
      return 0;
    }

  for (i = 0; i < 4u; i++)
    {
      ret = gpx_record_flush_pending_locked(rec);
      if (ret == 0)
        {
          break;
        }

      gpx_port_mutex_unlock(&rec->lock);
      gpx_port_sleep_ms(50);
      gpx_port_mutex_lock(&rec->lock);
    }

  /* close 可能一直刷 128KiB 缓存；锁外做，UI 才不会跟盘卡住。 */
  gpx_port_mutex_unlock(&rec->lock);
  {
    int close_ret = gpx_writer_close(&rec->writer);

    if (ret == 0)
      {
        ret = close_ret;
      }
  }

  gpx_port_mutex_lock(&rec->lock);
  rec->recording = false;
  rec->paused = false;
  rec->stop_drain = false;
  return ret;
}

/**
 * @brief 打开 GPX 文件并开始录制会话（须已加锁）。
 *
 * @param rec  录制器句柄
 * @param cmd  start 命令
 * @return 0 成功，-EBUSY 已在录，其他负 errno 失败
 */
static int gpx_record_open_session_locked(gpx_record_t *rec,
                                          const gpx_record_cmd_msg_t *cmd)
{
  int ret;

  if (rec->recording)
    {
      rec->start_pending = false;
      return -EBUSY;
    }

  gpx_strcopy(rec->filepath, sizeof(rec->filepath), cmd->filepath);
  gpx_record_meta_from_cmd(rec, cmd);
  memset(&rec->stats, 0, sizeof(rec->stats));
  /* 勿 reset data_q：start_pending 期间 UI 可能已 push 了前几个点。 */

  gpx_port_mutex_unlock(&rec->lock);
  if (cmd->append)
    {
      ret = gpx_writer_open_append(&rec->writer, rec->filepath);
    }
  else
    {
      ret = gpx_writer_open(&rec->writer, rec->filepath, &rec->meta);
    }
  gpx_port_mutex_lock(&rec->lock);

  rec->start_pending = false;
  if (ret != 0)
    {
      gpx_record_drain_data_ext_locked(rec);
      gpx_port_queue_reset(&rec->data_q);
      return ret;
    }

  rec->recording = true;
  rec->paused = false;
  rec->stop_drain = false;
  return 0;
}

/**
 * @brief 处理单条 cmd 消息（须已加锁）。
 *
 * @param rec  录制器句柄
 * @param cmd  命令
 * @return 0 成功，负 errno 失败
 */
static int gpx_record_process_cmd_locked(gpx_record_t *rec,
                                         const gpx_record_cmd_msg_t *cmd)
{
  switch (cmd->id)
    {
    case GPX_RECORD_CMD_START:
      return gpx_record_open_session_locked(rec, cmd);

    case GPX_RECORD_CMD_PAUSE:
      if (!rec->recording)
        {
          return -EINVAL;
        }

      rec->paused = true;
      return 0;

    case GPX_RECORD_CMD_RESUME:
      if (!rec->recording)
        {
          return -EINVAL;
        }

      rec->paused = false;
      return 0;

    case GPX_RECORD_CMD_STOP:
      rec->start_pending = false;
      rec->stop_drain = true;
      return gpx_record_close_session_locked(rec);

    case GPX_RECORD_CMD_THREAD_EXIT:
      rec->thread_exit_pending = true;
      return 0;

    case GPX_RECORD_CMD_RELEASE:
      rec->start_pending = false;
      rec->release_pending = true;
      rec->stop_drain = true;
      return gpx_record_close_session_locked(rec);

    default:
      return -EINVAL;
    }
}

/**
 * @brief 排空 cmd 队列并逐条处理（须已加锁）。
 *
 * @param rec  录制器句柄
 */
static void gpx_record_process_cmds_locked(gpx_record_t *rec)
{
  gpx_record_cmd_msg_t cmd;

  while (gpx_port_queue_pop(&rec->cmd_q, &cmd) == 0)
    {
      int ret = gpx_record_process_cmd_locked(rec, &cmd);

      if (ret != 0 && cmd.id == GPX_RECORD_CMD_START)
        {
          printf("gpx_record: start failed (%d)\n", ret);
        }
    }
}

/**
 * @brief 若 data 队列满 N 则循环批写（须已加锁；暂停时不刷）。
 *
 * @param rec  录制器句柄
 */
static void gpx_record_flush_if_ready_locked(gpx_record_t *rec)
{
  unsigned count;

  if (!rec->recording || rec->paused)
    {
      return;
    }

  count = gpx_port_queue_count(&rec->data_q);
  while (count >= rec->batch_size)
    {
      int fret = gpx_record_flush_batch_locked(rec, rec->batch_size);

      if (fret != 0)
        {
          printf("gpx_record: flush failed (%d)\n", fret);
          break;
        }

      count = gpx_port_queue_count(&rec->data_q);
    }
}

/**
 * @brief 唤醒录制工作线程（sem_post）。
 *
 * @param rec  录制器句柄
 */
static void gpx_record_wake(gpx_record_t *rec)
{
  gpx_port_sem_post(&rec->sem_wake);
}

/**
 * @brief 向 cmd 队列投递无参控制命令并唤醒线程。
 *
 * @param rec  录制器句柄
 * @param id   命令 ID
 * @return 0 成功，-EINVAL / -ENOSPC 失败
 */
static int gpx_record_post_cmd(gpx_record_t *rec, gpx_record_cmd_id_t id)
{
  gpx_record_cmd_msg_t cmd;

  if (rec == NULL || !rec->thread_alive)
    {
      return -EINVAL;
    }

  memset(&cmd, 0, sizeof(cmd));
  cmd.id = id;

  gpx_port_mutex_lock(&rec->lock);
  if (gpx_port_queue_push(&rec->cmd_q, &cmd) != 0)
    {
      gpx_port_mutex_unlock(&rec->lock);
      return -ENOSPC;
    }

  gpx_port_mutex_unlock(&rec->lock);
  gpx_record_wake(rec);
  return 0;
}

/**
 * @brief 录制工作线程：处理 cmd、批写 data、等待 sem 唤醒。
 *
 * @param arg  gpx_record_t 指针
 * @return NULL
 */
static void *gpx_record_thread(void *arg)
{
  gpx_record_t *rec = arg;

  for (;;)
    {
      gpx_port_mutex_lock(&rec->lock);
      gpx_record_process_cmds_locked(rec);
      gpx_record_flush_if_ready_locked(rec);

      if (rec->release_pending || rec->thread_exit_pending)
        {
          gpx_port_mutex_unlock(&rec->lock);
          break;
        }

      if (rec->stop_drain && gpx_port_queue_count(&rec->data_q) > 0 &&
          rec->recording && !rec->paused)
        {
          /* stop/release 后若仍有 data，继续刷 */
          gpx_record_flush_if_ready_locked(rec);
        }

      gpx_port_mutex_unlock(&rec->lock);

      if (rec->release_pending || rec->thread_exit_pending)
        {
          break;
        }

      if (gpx_port_sem_wait(&rec->sem_wake) != 0)
        {
          break;
        }
    }

  gpx_port_mutex_lock(&rec->lock);
  gpx_record_drain_data_ext_locked(rec);
  gpx_port_queue_reset(&rec->cmd_q);
  rec->thread_alive = false;
  gpx_port_mutex_unlock(&rec->lock);

  return NULL;
}

/**
 * @brief 启动录制工作线程（尚未运行时）。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
static int gpx_record_thread_spawn(gpx_record_t *rec)
{
  int ret;
  void *stack = NULL;
  unsigned stack_size = CONFIG_MYVENDOR_GPX_RECORD_STACKSIZE;

  if (rec == NULL)
    {
      return -EINVAL;
    }

  if (rec->thread_alive)
    {
      return 0;
    }

  rec->thread_exit_pending = false;
  rec->release_pending = false;
  rec->thread_uses_static_stack = false;

  if (!g_gpx_record_stack_busy)
    {
      if (!gpx_record_stack_alloc())
        {
          return -ENOMEM;
        }

      g_gpx_record_stack_busy = true;
      rec->thread_uses_static_stack = true;
      stack = g_gpx_record_stack;
    }

  /* 低于 bicycle_ui(100)，fopen/fsync 时不抢 UI。 */
  ret = gpx_port_thread_create(&rec->thread, gpx_record_thread, rec,
                               stack_size, 80, "gpx_record", stack);
  if (ret != 0)
    {
      if (rec->thread_uses_static_stack)
        {
          rec->thread_uses_static_stack = false;
          g_gpx_record_stack_busy = false;
          gpx_record_stack_free();
        }

      return ret;
    }

  rec->thread_alive = true;
  return 0;
}

/**
 * @brief 请求工作线程退出并 join（stop 后回收；句柄仍有效可再次 start）。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
static int __attribute__((unused))
gpx_record_thread_shutdown(gpx_record_t *rec)
{
  int ret;

  if (rec == NULL)
    {
      return -EINVAL;
    }

  if (!rec->thread_alive)
    {
      return 0;
    }

  ret = gpx_record_post_cmd(rec, GPX_RECORD_CMD_THREAD_EXIT);
  if (ret != 0)
    {
      return ret;
    }

  gpx_record_wake(rec);
  gpx_port_thread_join(&rec->thread);
  rec->thread_alive = false;
  rec->thread_exit_pending = false;
  if (rec->thread_uses_static_stack)
    {
      rec->thread_uses_static_stack = false;
      g_gpx_record_stack_busy = false;
      gpx_record_stack_free();
    }

  return 0;
}

/**
 * @brief 按 cfg 分配 data/cmd 队列 storage 与 fmt_buf。
 *
 * @param rec  录制器句柄
 * @return 0 成功，-ENOMEM 失败
 */
static int gpx_record_alloc_io(gpx_record_t *rec)
{
  size_t fmt_size;

  rec->data_storage = gpx_port_calloc(rec->data_depth, sizeof(gpx_point_t));
  rec->cmd_storage = gpx_port_calloc(GPX_RECORD_CMD_QUEUE_DEPTH,
                            sizeof(gpx_record_cmd_msg_t));
  fmt_size = (size_t)rec->batch_size * gpx_writer_trkpt_fmt_max();
  if (rec->batch_size != 0 &&
      fmt_size / rec->batch_size != gpx_writer_trkpt_fmt_max())
    {
      rec->fmt_buf = NULL;
    }
  else
    {
      rec->fmt_buf = gpx_port_malloc(fmt_size);
    }

  if (rec->data_storage == NULL || rec->cmd_storage == NULL ||
      rec->fmt_buf == NULL)
    {
      gpx_port_free(rec->data_storage);
      gpx_port_free(rec->cmd_storage);
      gpx_port_free(rec->fmt_buf);
      rec->data_storage = NULL;
      rec->cmd_storage = NULL;
      rec->fmt_buf = NULL;
      return -ENOMEM;
    }

  rec->fmt_buf_size = fmt_size;
  gpx_port_queue_init(&rec->data_q, rec->data_storage, rec->data_depth,
                 sizeof(gpx_point_t));
  gpx_port_queue_init(&rec->cmd_q, rec->cmd_storage, GPX_RECORD_CMD_QUEUE_DEPTH,
                 sizeof(gpx_record_cmd_msg_t));
  return 0;
}

/**
 * @brief 释放 data/cmd 队列与 fmt_buf（须已加锁或线程已停）。
 *
 * @param rec  录制器句柄
 */
static void gpx_record_free_io(gpx_record_t *rec)
{
  if (rec == NULL)
    {
      return;
    }

  gpx_record_drain_data_ext_locked(rec);
  gpx_port_free(rec->data_storage);
  gpx_port_free(rec->cmd_storage);
  gpx_port_free(rec->fmt_buf);
  rec->data_storage = NULL;
  rec->cmd_storage = NULL;
  rec->fmt_buf = NULL;
  rec->fmt_buf_size = 0;
}

/**
 * @brief 创建录制器句柄（不启动线程；首次 start 时才拉起 gpx_record 线程）。
 *
 * @param cfg   批写 / data 队列配置；可为 NULL
 * @param out   输出句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_create(const gpx_record_cfg_t *cfg, gpx_record_t **out)
{
  gpx_record_t *rec;
  unsigned batch_size;
  unsigned data_depth;
  int ret;

  if (out == NULL)
    {
      return -EINVAL;
    }

  *out = NULL;
  ret = gpx_record_resolve_cfg(cfg, &batch_size, &data_depth);
  if (ret != 0)
    {
      return ret;
    }

  rec = gpx_port_calloc(1, sizeof(*rec));
  if (rec == NULL)
    {
      return -ENOMEM;
    }

  rec->batch_size = batch_size;
  rec->data_depth = data_depth;
  rec->writer.file.fd = -1;
  rec->thread_alive = false;

  if (gpx_port_mutex_init(&rec->lock) != 0)
    {
      gpx_port_free(rec);
      return -errno;
    }

  if (gpx_port_sem_init(&rec->sem_wake, 0) != 0)
    {
      gpx_port_mutex_destroy(&rec->lock);
      gpx_port_free(rec);
      return -errno;
    }

  ret = gpx_record_alloc_io(rec);
  if (ret != 0)
    {
      gpx_port_sem_destroy(&rec->sem_wake);
      gpx_port_mutex_destroy(&rec->lock);
      gpx_port_free(rec);
      return ret;
    }

  *out = rec;
  return 0;
}

/**
 * @brief 预拉工作线程，避免首次 start 在调用线程上 pthread_create。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_prepare(gpx_record_t *rec)
{
  return gpx_record_thread_spawn(rec);
}

/**
 * @brief 投递 start：工作线程异步打开 GPX。调用立即返回。
 *
 * @param rec       录制器句柄
 * @param filepath  输出路径
 * @param meta      元数据；可为 NULL
 * @return 0 已投递，-EBUSY 已在录制，其他负 errno 失败
 */
static int gpx_record_start_common(gpx_record_t *rec, const char *filepath,
                                   const gpx_meta_t *meta, bool append)
{
  gpx_record_cmd_msg_t cmd;
  int ret;

  if (rec == NULL || filepath == NULL)
    {
      return -EINVAL;
    }

  ret = gpx_record_thread_spawn(rec);
  if (ret != 0)
    {
      return ret;
    }

  gpx_record_fill_start_cmd(&cmd, filepath, meta, append);

  gpx_port_mutex_lock(&rec->lock);
  if (rec->start_pending)
    {
      gpx_port_mutex_unlock(&rec->lock);
      return -EBUSY;
    }

  /* 上一趟正在 close 时允许把下一趟 START 排在 STOP 后面。 */
  if (rec->recording && !rec->stop_drain)
    {
      gpx_port_mutex_unlock(&rec->lock);
      return -EBUSY;
    }

  if (gpx_port_queue_push(&rec->cmd_q, &cmd) != 0)
    {
      gpx_port_mutex_unlock(&rec->lock);
      return -ENOSPC;
    }

  rec->start_pending = true;
  gpx_port_mutex_unlock(&rec->lock);
  gpx_record_wake(rec);
  return 0;
}

int gpx_record_start(gpx_record_t *rec, const char *filepath,
                     const gpx_meta_t *meta)
{
  return gpx_record_start_common(rec, filepath, meta, false);
}

int gpx_record_start_append(gpx_record_t *rec, const char *filepath,
                            const gpx_meta_t *meta)
{
  return gpx_record_start_common(rec, filepath, meta, true);
}

/**
 * @brief 向 data 队列 push 轨迹点（扩展 payload 深拷贝）。
 *
 * @param rec    录制器句柄
 * @param point  输入点
 * @return 0 成功；-EAGAIN 未 start；-ENOSPC 队列满；其他负 errno
 */
int gpx_record_push(gpx_record_t *rec, const gpx_point_t *point)
{
  gpx_point_t queued;
  unsigned count;
  int ret;

  if (rec == NULL || point == NULL || !rec->thread_alive)
    {
      return -EINVAL;
    }

  gpx_port_mutex_lock(&rec->lock);

  if (!rec->recording && !rec->start_pending)
    {
      gpx_port_mutex_unlock(&rec->lock);
      return -EAGAIN;
    }

  if (rec->stop_drain)
    {
      rec->stats.points_dropped++;
      gpx_port_mutex_unlock(&rec->lock);
      return -EAGAIN;
    }

  queued = *point;
  memset(queued.ext_data, 0, sizeof(queued.ext_data));
  queued.ext_mask = 0;

  ret = gpx_ext_point_clone_ext(&queued, point);
  if (ret != 0)
    {
      /* 心率 payload 拷贝失败仍记下 lat/lon，不要把整点丢掉。 */
      queued.ext_mask = 0;
      memset(queued.ext_data, 0, sizeof(queued.ext_data));
    }

  ret = gpx_port_queue_push(&rec->data_q, &queued);
  if (ret != 0)
    {
      rec->stats.points_dropped++;
      gpx_ext_point_free_ext(&queued);
      gpx_port_mutex_unlock(&rec->lock);
      return ret;
    }

  count = gpx_port_queue_count(&rec->data_q);
  gpx_port_mutex_unlock(&rec->lock);

  if (count >= rec->batch_size)
    {
      gpx_record_wake(rec);
    }

  return 0;
}

/**
 * @brief 暂停刷写（data 仍可 push，恢复后继续写盘）。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_pause(gpx_record_t *rec)
{
  return gpx_record_post_cmd(rec, GPX_RECORD_CMD_PAUSE);
}

/**
 * @brief 恢复刷写。
 *
 * @param rec  录制器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_record_resume(gpx_record_t *rec)
{
  int ret;

  ret = gpx_record_post_cmd(rec, GPX_RECORD_CMD_RESUME);
  if (ret == 0)
    {
      gpx_record_wake(rec);
    }

  return ret;
}

/**
 * @brief 结束当前录制会话：投递 STOP，立即返回。工作线程刷尾批并 close。
 *
 * @param rec  录制器句柄
 * @return 0 已投递或已空闲，负 errno 失败
 */
int gpx_record_stop(gpx_record_t *rec)
{
  gpx_record_cmd_msg_t cmd;

  if (rec == NULL || !rec->thread_alive)
    {
      return -EINVAL;
    }

  memset(&cmd, 0, sizeof(cmd));
  cmd.id = GPX_RECORD_CMD_STOP;

  gpx_port_mutex_lock(&rec->lock);
  if (!rec->recording && !rec->start_pending)
    {
      rec->stop_drain = false;
      gpx_port_mutex_unlock(&rec->lock);
      return 0;
    }

  if (rec->stop_drain)
    {
      gpx_port_mutex_unlock(&rec->lock);
      return 0;
    }

  if (gpx_port_queue_push(&rec->cmd_q, &cmd) != 0)
    {
      gpx_port_mutex_unlock(&rec->lock);
      return -ENOSPC;
    }

  rec->stop_drain = true;
  gpx_port_mutex_unlock(&rec->lock);
  gpx_record_wake(rec);
  return 0;
}

/**
 * @brief 等到 fopen/close 不在进行（stop 后落盘完成）。录制中且未 stop 则立即返回。
 */
int gpx_record_wait_close(gpx_record_t *rec)
{
  if (rec == NULL)
    {
      return -EINVAL;
    }

  for (;;)
    {
      bool busy;

      gpx_port_mutex_lock(&rec->lock);
      busy = rec->start_pending || rec->stop_drain || rec->release_pending;
      gpx_port_mutex_unlock(&rec->lock);
      if (!busy)
        {
          return 0;
        }

      gpx_port_sleep_ms(20);
    }
}

/**
 * @brief 释放录制器：发送 release 命令，join 线程并 free 句柄。
 *
 * 若仍在录制，会先 stop 再退出。调用后 *rec 置 NULL。
 *
 * @param rec  句柄指针
 * @return 0 成功，负 errno 失败
 */
int gpx_record_release(gpx_record_t **rec)
{
  gpx_record_t *r;
  int ret;

  if (rec == NULL || *rec == NULL)
    {
      return -EINVAL;
    }

  r = *rec;
  if (!r->thread_alive)
    {
      if (r->thread_uses_static_stack)
        {
          (void)gpx_port_thread_join(&r->thread);
          r->thread_uses_static_stack = false;
          g_gpx_record_stack_busy = false;
          gpx_record_stack_free();
        }

      gpx_record_free_io(r);
      gpx_port_sem_destroy(&r->sem_wake);
      gpx_port_mutex_destroy(&r->lock);
      gpx_port_free(r);
      *rec = NULL;
      return 0;
    }

  ret = gpx_record_post_cmd(r, GPX_RECORD_CMD_RELEASE);
  if (ret != 0)
    {
      return ret;
    }

  gpx_record_wake(r);
  gpx_port_thread_join(&r->thread);
  if (r->thread_uses_static_stack)
    {
      r->thread_uses_static_stack = false;
      g_gpx_record_stack_busy = false;
      gpx_record_stack_free();
    }

  gpx_record_free_io(r);
  gpx_port_sem_destroy(&r->sem_wake);
  gpx_port_mutex_destroy(&r->lock);
  gpx_port_free(r);
  *rec = NULL;
  return 0;
}

/**
 * @brief 是否处于 start 后、stop 前的录制会话中。
 *
 * @param rec  录制器句柄
 * @return true 正在录制
 */
bool gpx_record_is_recording(gpx_record_t *rec)
{
  bool v;

  if (rec == NULL)
    {
      return false;
    }

  gpx_port_mutex_lock(&rec->lock);
  v = (rec->recording || rec->start_pending) && !rec->stop_drain;
  gpx_port_mutex_unlock(&rec->lock);
  return v;
}

bool gpx_record_is_closing(gpx_record_t *rec)
{
  bool v;

  if (rec == NULL)
    {
      return false;
    }

  gpx_port_mutex_lock(&rec->lock);
  v = rec->stop_drain;
  gpx_port_mutex_unlock(&rec->lock);
  return v;
}

/**
 * @brief 工作线程是否在运行（start 后、stop 前为 true）。
 *
 * @param rec  录制器句柄
 * @return true 线程存活
 */
bool gpx_record_is_alive(const gpx_record_t *rec)
{
  if (rec == NULL)
    {
      return false;
    }

  return rec->thread_alive;
}

/**
 * @brief 获取统计信息。
 *
 * @param rec    录制器句柄
 * @param stats  输出统计
 * @return 0 成功，-EINVAL 参数无效
 */
int gpx_record_get_stats(gpx_record_t *rec, gpx_record_stats_t *stats)
{
  if (rec == NULL || stats == NULL)
    {
      return -EINVAL;
    }

  gpx_port_mutex_lock(&rec->lock);
  *stats = rec->stats;
  gpx_port_mutex_unlock(&rec->lock);
  return 0;
}

/**
 * @brief 获取运行时参数与状态。
 *
 * @param rec      录制器句柄
 * @param runtime  输出 batch / queue / recording / paused
 * @return 0 成功，-EINVAL 参数无效
 */
int gpx_record_get_runtime(gpx_record_t *rec, gpx_record_runtime_t *runtime)
{
  if (rec == NULL || runtime == NULL)
    {
      return -EINVAL;
    }

  gpx_port_mutex_lock(&rec->lock);
  runtime->batch_size = rec->batch_size;
  runtime->queue_depth = rec->data_depth;
  runtime->recording = rec->recording || rec->start_pending;
  runtime->paused = rec->paused;
  gpx_port_mutex_unlock(&rec->lock);
  return 0;
}