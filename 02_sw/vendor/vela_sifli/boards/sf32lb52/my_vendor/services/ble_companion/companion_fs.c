/**
 * @file companion_fs.c
 * @brief Companion GATT 文件会话（0xFF15 / 0xFF16）。
 *
 * 文件 I/O 跑在 companion 线程。GATT 回调只拷贝并 kick。
 * 工厂固件拒绝全部 FS 命令（EACCES）：拷文件只走 USB MTP。
 * @ref companion_fs_set_mtu 仅用于已 connect 的手机地址：传感器 ATT 23
 * 不得砸 FS。漏判 GATTS 回调时仍靠 peer 匹配挡住。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "companion_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/crc32.h>

#include "board_malloc.h"
#include "companion_bridge.h"
#include "companion_proto.h"
#include "companion_rle.h"
#include "myvendor_gnss.h"
#include "myvendor_identity.h"
#include "myvendor_mono.h"
#include "myvendor_watchdog.h"
#include "transfer_backend.h"

/**
 * @brief 本模块日志出口（官方规则：服务层统一走 `syslog()`，不用 `printf`）。
 *
 * @details 前缀固定 `ble_companion: `（与 downlink 共用一个标签，便于整条 FS 通道一起
 *          grep）；正常会话/结束用 INFO，真故障才 ERROR。
 */
#define LOGI(fmt, ...) syslog(LOG_INFO, "ble_companion: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) syslog(LOG_ERR, "ble_companion: " fmt "\n", ##__VA_ARGS__)

#define FS_STAGING_MAX          320
#define FS_DATA_SLOTS           512
#define FS_DATA_DRAIN_MAX       24
#define FS_SLOT_MAX             256
#define FS_WBUF_SIZE            (256 * 1024)
#define FS_RBUF_SIZE            (16 * 1024)
#define FS_FLUSH_CHUNK          (4 * 1024)
#define FS_SYNC_BYTES           (128 * 1024)
#define FS_ENQ_WAIT_US          2000
#define FS_ENQ_WAIT_MAX         8
#define FS_NOTIFY_GAP_MS        0
#define FS_NOTIFY_INFLIGHT_MAX  6
#define FS_CREDIT_TIMEOUT_MS    40
/** 事务开了却这么久**一点进展都没有** = 卡死，收掉（见 companion_fs_pump 头部）。 */
#define FS_SESSION_STALL_MS     3000u
/** 写入 / 落盘中的事务用这个上限：上传期间本来就没有通知，落盘又可能很慢。 */
#define FS_SESSION_STALL_IO_MS  30000u
/** `ccc` 丢失后的宽限期：等 app 补订，超时才收掉事务（见 pump 头部）。 */
#define FS_CCC_GRACE_MS         5000u
#define FS_NEED_Q               4
#define FS_NEED_NAME_MAX        COMPANION_NOTIF_ICON_NAME_MAX
#define FS_NEED_RETRY_MS        15000
#define FS_ICON_PNG_MIN         32
#define FS_ICON_PNG_MAX         (64 * 1024)

enum fs_state
{
  FS_IDLE = 0,
  FS_REPLY,          /* one-shot reply (stat / mkdir / …) already packed   */
  FS_LIST,
  FS_READ,
  FS_WRITE,
};

struct fs_cmd_req
{
  bool     pending;
  uint8_t  buf[FS_STAGING_MAX];
  uint16_t len;
};

struct fs_data_slot
{
  uint16_t len;
  uint8_t  buf[FS_SLOT_MAX];
};

struct fs_need_slot
{
  char     name[FS_NEED_NAME_MAX + 1];
  uint32_t last_ms;
  bool     pending;
};

struct fs_ctx
{
  pthread_mutex_t lock;

  gatts_handle_t handle;
  uint16_t       data_attr;
  bt_address_t   peer;
  bool           connected;
  bool           ccc;
  uint32_t       mtu;
  uint8_t        inflight;       /* outstanding FS notifies                */
  bool           lfs_held;

  enum fs_state  state;
  uint8_t        cmd;
  uint8_t        tid;
  uint16_t       seq;

  char           path[XFER_PATH_MAX];
  char           tmp_path[XFER_PATH_MAX];
  uint16_t       list_cursor;
  uint32_t       read_off;
  uint32_t       read_len;       /* remaining bytes requested              */
  int            wfd;
  uint32_t       w_expected;
  uint32_t       w_got;
  uint32_t       w_flushed;      /* 本会话已落到 *.part 的字节 */
  uint32_t       w_crc;
  uint16_t       rx_seq;         /* next expected WRITE_DATA seq           */
  int            w_fail;         /* sticky error; CLOSE reports this       */
  bool           w_rle;
  bool           r_rle;
  struct companion_rle_dec rle_dec;
  struct companion_rle_enc rle_enc;
  uint8_t       *wbuf;           /* PSRAM coalesce buffer                  */
  uint32_t       wbuf_cap;
  uint32_t       wbuf_len;
  int            rfd;            /* kept open across READ notify pages     */
  uint32_t       rfd_pos;
  uint8_t       *rbuf;           /* PSRAM read-ahead                       */
  uint32_t       rbuf_cap;
  uint32_t       rbuf_off;
  uint32_t       rbuf_len;

  uint8_t        tx[FS_STAGING_MAX];
  uint16_t       tx_len;
  bool           tx_last;
  bool           tx_err;
  uint8_t        tx_status;

  struct fs_cmd_req   cmd_req;
  struct fs_data_slot *data_q;   /* PSRAM ring; fallback SRAM below        */
  uint16_t            data_q_cap;
  uint16_t            data_q_rd;
  uint16_t            data_q_n;
  bool                abort_req;
  bool                commit_pending; /* CLOSE 已回复，尚未落盘 */
  bool                disk_hold;      /* CLOSE 抽环时只进 RAM，不写盘 */
  uint32_t            send_ms;
  struct fs_need_slot need[FS_NEED_Q];
};

static struct fs_data_slot g_data_q_sram[8];

static struct fs_ctx g_fs =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .wfd  = -1,
  .rfd  = -1,
};

static sem_t g_fs_wake;
static bool  g_fs_wake_ready;

/* ===== 诊断插桩 ==========================================================
 * 只被 syslog 读取，不参与任何判断。目的：量化"发出去的通知有多少其实
 * 没到达对端"——这是手机侧报序号/CRC 断裂并主动断开（reason=0x13）的
 * 最可能来源，而 bt_gatts_notify() 的返回值无法反映它。
 */
static uint32_t g_diag_notify_sent;      /* 计入 seq 的通知数 */
static uint32_t g_diag_notify_fail;      /* bt_gatts_notify 明确失败 */
static uint32_t g_diag_credit_timeout;   /* 信用超时被强行归零的次数 */
static uint32_t g_diag_credit_lost;      /* 归零时被"吞掉"的 in-flight 页数 */
static uint32_t g_diag_notify_done;      /* 收到的 notify-complete 数 */

/** @brief 分配 FS 读写/RLE 缓冲（PSRAM/board_malloc）。 */
static void fs_buffers_init(void)
{
  if (g_fs.data_q == NULL)
    {
      g_fs.data_q = board_malloc_psram(sizeof(struct fs_data_slot) *
                                       (size_t)FS_DATA_SLOTS);
      if (g_fs.data_q != NULL)
        {
          g_fs.data_q_cap = FS_DATA_SLOTS;
          LOGI("fs rx ring %u slots in PSRAM", (unsigned)FS_DATA_SLOTS);
        }
      else
        {
          g_fs.data_q     = g_data_q_sram;
          g_fs.data_q_cap = (uint16_t)(sizeof(g_data_q_sram) /
                                       sizeof(g_data_q_sram[0]));
          LOGE("fs rx ring PSRAM alloc failed, SRAM fallback %u",
               (unsigned)g_fs.data_q_cap);
        }
    }

  if (g_fs.wbuf == NULL)
    {
      g_fs.wbuf = board_malloc_psram(FS_WBUF_SIZE);
      if (g_fs.wbuf != NULL)
        {
          g_fs.wbuf_cap = FS_WBUF_SIZE;
          LOGI("fs write cache %u KiB in PSRAM",
               (unsigned)(FS_WBUF_SIZE / 1024));
        }
      else
        {
          LOGE("fs write cache PSRAM alloc failed, write-through");
        }
    }

  if (g_fs.rbuf == NULL)
    {
      g_fs.rbuf = board_malloc_psram(FS_RBUF_SIZE);
      if (g_fs.rbuf != NULL)
        {
          g_fs.rbuf_cap = FS_RBUF_SIZE;
          LOGI("fs read cache %u KiB in PSRAM",
               (unsigned)(FS_RBUF_SIZE / 1024));
        }
      else
        {
          LOGE("fs read cache PSRAM alloc failed, uncached reads");
        }
    }
}

/** @brief 关闭跨 notify 保持的读 fd。 */
static void read_fd_close(void)
{
  if (g_fs.rfd >= 0)
    {
      xfer_read_close(g_fs.rfd);
      g_fs.rfd = -1;
    }

  g_fs.rfd_pos  = 0;
  g_fs.rbuf_len = 0;
}

/** @brief 从读缓冲或 LFS 取数据（含 RLE 解码）。 */
static int rbuf_get(uint32_t off, uint8_t *dst, uint16_t want, uint16_t *got)
{
  uint16_t copied = 0;
  int      ret;

  *got = 0;
  if (want == 0)
    {
      return 0;
    }

  if (g_fs.rfd < 0)
    {
      return xfer_read(g_fs.path, off, dst, want, got);
    }

  while (copied < want)
    {
      uint16_t n;
      uint16_t loaded = 0;

      if (g_fs.rbuf != NULL && g_fs.rbuf_len > 0 &&
          off >= g_fs.rbuf_off &&
          off < g_fs.rbuf_off + g_fs.rbuf_len)
        {
          uint32_t idx = off - g_fs.rbuf_off;
          uint32_t have = g_fs.rbuf_len - idx;

          n = (uint16_t)have;
          if (n > (uint16_t)(want - copied))
            {
              n = (uint16_t)(want - copied);
            }

          memcpy(dst + copied, g_fs.rbuf + idx, n);
          copied = (uint16_t)(copied + n);
          off += n;
          continue;
        }

      if (g_fs.rfd_pos != off)
        {
          ret = xfer_read_seek(g_fs.rfd, off);
          if (ret != 0)
            {
              return ret;
            }

          g_fs.rfd_pos = off;
        }

      if (g_fs.rbuf != NULL && g_fs.rbuf_cap > 0)
        {
          uint16_t load_n = (uint16_t)g_fs.rbuf_cap;

          ret = xfer_read_data(g_fs.rfd, g_fs.rbuf, load_n, &loaded);
          if (ret != 0)
            {
              return ret;
            }

          g_fs.rbuf_off = off;
          g_fs.rbuf_len = loaded;
          g_fs.rfd_pos  = off + loaded;
          if (loaded == 0)
            {
              break;
            }

          continue;
        }

      n = (uint16_t)(want - copied);
      ret = xfer_read_data(g_fs.rfd, dst + copied, n, &loaded);
      if (ret != 0)
        {
          return ret;
        }

      g_fs.rfd_pos += loaded;
      copied = (uint16_t)(copied + loaded);
      off += loaded;
      if (loaded == 0)
        {
          break;
        }
    }

  *got = copied;
  return 0;
}

static bool path_is_ota_fw(const char *path);

/** @brief 让出 SD/LFS，避免 CLOSE 尾部把 UI 线程饿死。 */
static void wbuf_yield(void)
{
  /** 主循环的心跳也在这里续上 —— "在写盘" 不是 "线程停摆"。
   *
   *  why：CLOSE 已应答后的延迟补写（`fs_deferred_commit()`）跑在 companion
   *  线程上，它调的 `wbuf_flush()` 会**同步**排空整个写缓存；而 `wbuf_append()`
   *  缓存满时的那两个排空循环同样在一个调用里连写很多块。一个满 cache
   *  （`FS_WBUF_SIZE` = 256 KiB）按 `FS_FLUSH_CHUNK`(4 KiB) 一块块写，实测
   *  每块 ~290 ms（`usleep(2000)` 只占 2 ms，其余是 LittleFS 在 SD 上的
   *  写），整段 18~19 s。
   *
   *  心跳只在 `ble_companion.c` 主循环顶上喂一次，于是这一整段里
   *  `companion_bridge_heartbeat_age_ms()` 会一路涨过 diag 的
   *  `DIAG_BLE_THREAD_MS`(15 s)，判成"线程停摆"→ SIGKILL 掉一个正在写盘的
   *  线程，再往后是 LCPU force reset / adapter not ready / respawn 失败 ——
   *  现场那次 388 KB 的 GPX 导入就是这么把自己写死的。
   *
   *  这里续心跳不会削弱 diag 的本意：`diag_ble_restart()` 要抓的是"堵在
   *  不可中断 HCI 等待上"，那种情况下这个循环根本不在跑，也就没人喂心跳。
   *  watchdog 不必在这里补 —— SD 的 PIO 忙等里 `sd1_spin_yield()` 已经每
   *  1 ms 喂过一次（见 chips/sf32lb52/sf32lb_sdio.c）。
   */
  companion_bridge_heartbeat();
  usleep(2000);
}

/** @brief 边收边写。CLOSE 抽环阶段禁止写盘，否则 App 等应答时第一次
 * lfs_alloc 会卡数秒。OTA 也曾攒满 256KiB 再写，BLE 环先满丢包变成 seq gap。 */
static bool wbuf_stream_disk(void)
{
  return !g_fs.disk_hold;
}

/** @brief 写出写缓冲一个 chunk。 */
static int wbuf_flush_chunk(void)
{
  uint32_t n;
  int      ret;

  if (g_fs.wbuf_len == 0)
    {
      return 0;
    }

  if (g_fs.wfd < 0)
    {
      g_fs.wbuf_len = 0;
      return EINVAL;
    }

  n = g_fs.wbuf_len;
  if (n > FS_FLUSH_CHUNK)
    {
      n = FS_FLUSH_CHUNK;
    }

  ret = xfer_write_data(g_fs.wfd, g_fs.wbuf, n);
  if (ret != 0)
    {
      LOGI("fs write flush failed errno=%d n=%lu got=%lu",
           ret, (unsigned long)n, (unsigned long)g_fs.w_got);
      return ret;
    }

  {
    uint32_t before = g_fs.w_flushed;

    g_fs.w_flushed += n;
    if (before == 0u ||
        (before / (32u * 1024u)) != (g_fs.w_flushed / (32u * 1024u)))
      {
        LOGI("fs write flushed %lu/%lu",
             (unsigned long)g_fs.w_flushed,
             (unsigned long)g_fs.w_expected);
      }

    /* OTA 在 /mnt/kv：不定期 fsync 的话，复位后 LittleFS 会回到
     * WRITE_OPEN 时的 size，刚写的一两兆续传就没了。 */
    if (path_is_ota_fw(g_fs.path) &&
        (before / FS_SYNC_BYTES) != (g_fs.w_flushed / FS_SYNC_BYTES))
      {
        myvendor_watchdog_work_beat();
        myvendor_watchdog_hw_pet();
        (void)fsync(g_fs.wfd);
        myvendor_watchdog_hw_pet();
        LOGI("fs write synced %lu/%lu",
             (unsigned long)g_fs.w_flushed,
             (unsigned long)g_fs.w_expected);
      }
  }

  if (n < g_fs.wbuf_len)
    {
      memmove(g_fs.wbuf, g_fs.wbuf + n, g_fs.wbuf_len - n);
    }

  g_fs.wbuf_len -= n;
  return 0;
}

static int wbuf_flush(void)
{
  int ret;

  while (g_fs.wbuf_len > 0)
    {
      ret = wbuf_flush_chunk();
      if (ret != 0)
        {
          return ret;
        }

      if (g_fs.wbuf_len > 0)
        {
          wbuf_yield();
        }
    }

  return 0;
}

/** @brief 追加明文到写缓冲（必要时 flush）。 */
static int wbuf_append(const uint8_t *p, uint16_t n)
{
  int ret;

  if (n == 0)
    {
      return 0;
    }

  if (g_fs.wbuf == NULL || g_fs.wbuf_cap == 0)
    {
      return xfer_write_data(g_fs.wfd, p, n);
    }

  while (g_fs.wbuf_len + n > g_fs.wbuf_cap)
    {
      ret = wbuf_flush_chunk();
      if (ret != 0)
        {
          return ret;
        }

      wbuf_yield();
    }

  if (n > g_fs.wbuf_cap)
    {
      return xfer_write_data(g_fs.wfd, p, n);
    }

  memcpy(g_fs.wbuf + g_fs.wbuf_len, p, n);
  g_fs.wbuf_len += n;

  /* 满 4KiB 就写。否则 BLE 先把整文件堆进 RAM，环满丢包。 */
  if (wbuf_stream_disk())
    {
      while (g_fs.wbuf_len >= FS_FLUSH_CHUNK)
        {
          ret = wbuf_flush_chunk();
          if (ret != 0)
            {
              return ret;
            }

          wbuf_yield();
        }
    }

  return 0;
}

/**
 * @brief 唤醒 companion 线程（READ 等待时不要睡满 500 ms）。
 */
void companion_fs_kick(void)
{
  if (g_fs_wake_ready)
    {
      (void)sem_post(&g_fs_wake);
    }
}

/**
 * @brief 睡到 kick 或超时。
 * @param timeout_ms 最长等待。
 */
void companion_fs_wait(uint32_t timeout_ms)
{
  struct timespec ts;

  if (!g_fs_wake_ready)
    {
      usleep(timeout_ms * 1000);
      return;
    }

  while (sem_trywait(&g_fs_wake) == 0)
    {
    }

  if (timeout_ms == 0)
    {
      return;
    }

  /* 2026-09-23（审计 F9c）：**改用 sem_clockwait + CLOCK_MONOTONIC。**
   *
   * 原来读 CLOCK_REALTIME 再喂给 `sem_timedwait()` —— 而本树的
   * `sem_timedwait()` 是**写死**的实时钟：
   *   nuttx/libs/libc/semaphore/sem_timedwait.c:71-73
   *       return sem_clockwait(sem, CLOCK_REALTIME, abstime);
   * 于是这个"等一段时间"会被 `settimeofday()` 直接拖动 —— 而**同一个
   * companion 线程**就会校时：`companion_apply_time_sync()`（ble_companion.c，
   * 手机下发 TIME_SYNC 时调）。校时到真实时间通常是一次**大跳**，
   * 跳完之后这里算出的绝对时间早就在过去 ⇒ `sem_timedwait()` 立刻返回 ⇒
   * 等待被无声打断（本该等 N ms 的地方变成忙转）。
   *
   * 修法不是"换成 CLOCK_MONOTONIC 再喂 sem_timedwait"（那样更糟：单调基准
   * 比实时基准小约 1.79e9 秒，算出来的绝对时间远在过去，等待直接变 no-op），
   * 而是换成能把时钟**一起传下去**的接口。本树的
   * `sem_clockwait()`（nuttx/libs/libc/semaphore/sem_clockwait.c:76）就是把
   * clockid 透传给 `nxsem_clockwait()`，后者对非 CLOCK_REALTIME 走
   * `wd_start_abstime()`（单调基准）—— 见
   * nuttx/sched/semaphore/sem_clockwait.c:66-80。
   */
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_sec += (time_t)(timeout_ms / 1000);
  ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L)
    {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
    }

  (void)sem_clockwait(&g_fs_wake, CLOCK_MONOTONIC, &ts);
  while (sem_trywait(&g_fs_wake) == 0)
    {
    }
}

/****************************************************************************
 * Wire helpers
 ****************************************************************************/

static uint32_t fs_now_ms(void)
{
  return myvendor_mono_ms();
}

/** @brief 写入小端 uint16。 */
static void put_le16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

/** @brief 写入小端 uint32。 */
static void put_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

/** @brief 读出小端 uint16。 */
static uint16_t get_le16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** @brief 读出小端 uint32。 */
static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** @brief errno 映射为协议 uint8 错误码。 */
static uint8_t errno_u8(int err)
{
  if (err < 0)
    {
      err = -err;
    }

  if (err <= 0)
    {
      return 0;
    }

  if (err > 255)
    {
      return 255;
    }

  return (uint8_t)err;
}

/** @brief 当前 ATT MTU 下 FS notify 载荷预算。 */
static uint16_t payload_budget(void)
{
  uint32_t att;

  att = (g_fs.mtu > 3) ? (g_fs.mtu - 3) : 20;
  if (att < COMPANION_FS_HDR_LEN + 1)
    {
      return 0;
    }

  att -= COMPANION_FS_HDR_LEN;
  if (att > sizeof(g_fs.tx) - COMPANION_FS_HDR_LEN)
    {
      att = sizeof(g_fs.tx) - COMPANION_FS_HDR_LEN;
    }

  return (uint16_t)att;
}

/** @brief 事务期间 hold LittleFS。 */
static void hold_lfs(void)
{
  if (!g_fs.lfs_held)
    {
      xfer_lfs_acquire();
      g_fs.lfs_held = true;
    }
}

/** @brief 释放 LFS hold。 */
static void release_lfs(void)
{
  if (g_fs.lfs_held)
    {
      xfer_lfs_release();
      g_fs.lfs_held = false;
    }
}

/** @brief CLOSE 已应答后把 RAM 缓冲写入 *.part 并 rename。 */
static void fs_deferred_commit(void);

/** @brief 重置 FS 会话状态。 */
static void session_reset(bool drop_part)
{
  if (g_fs.commit_pending)
    {
      g_fs.commit_pending = false;
      fs_deferred_commit();
    }
  else if (g_fs.wfd >= 0)
    {
      if (drop_part)
        {
          g_fs.wbuf_len = 0;
          xfer_write_abort(g_fs.wfd, g_fs.tmp_path);
        }
      else
        {
          (void)wbuf_flush();
          xfer_write_suspend(g_fs.wfd);
        }

      g_fs.wfd = -1;
    }
  else
    {
      g_fs.wbuf_len = 0;
    }

  read_fd_close();
  release_lfs();
  g_fs.state    = FS_IDLE;
  g_fs.tx_len   = 0;
  g_fs.seq      = 0;
  g_fs.rx_seq   = 0;
  g_fs.data_q_n = 0;
  g_fs.data_q_rd = 0;
  g_fs.tmp_path[0] = '\0';
  g_fs.inflight    = 0;
  g_fs.w_fail      = 0;
  g_fs.w_rle       = false;
  g_fs.r_rle       = false;
  g_fs.commit_pending = false;
  g_fs.disk_hold      = false;
  g_fs.abort_req      = false;
  companion_rle_dec_init(&g_fs.rle_dec);
  companion_rle_enc_init(&g_fs.rle_enc);
}

/** @brief 排队错误回复 notify。 */
static void queue_err(uint8_t cmd, uint8_t tid, int err)
{
  g_fs.cmd        = cmd;
  g_fs.tid        = tid;
  g_fs.seq        = 0;
  g_fs.tx_len     = 0;
  g_fs.tx_last    = true;
  g_fs.tx_err     = true;
  g_fs.tx_status  = errno_u8(err);
  g_fs.state      = FS_REPLY;
}

/** @brief 排队成功回复 notify。 */
static void queue_ok(uint8_t cmd, uint8_t tid, const uint8_t *payload,
                     uint16_t plen, bool last)
{
  if (plen > sizeof(g_fs.tx) - COMPANION_FS_HDR_LEN)
    {
      plen = (uint16_t)(sizeof(g_fs.tx) - COMPANION_FS_HDR_LEN);
    }

  if (payload != NULL && plen > 0)
    {
      memcpy(g_fs.tx, payload, plen);
    }

  g_fs.cmd       = cmd;
  g_fs.tid       = tid;
  g_fs.seq       = 0;
  g_fs.tx_len    = plen;
  g_fs.tx_last   = last;
  g_fs.tx_err    = false;
  g_fs.tx_status = 0;
}

/** @brief 校验 notif 图标文件名合法。 */
static bool icon_name_ok(const char *name)
{
  size_t n;
  size_t i;

  if (name == NULL || name[0] == '\0')
    {
      return false;
    }

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
      return false;
    }

  n = strlen(name);
  if (n == 0 || n > FS_NEED_NAME_MAX)
    {
      return false;
    }

  for (i = 0; i < n; i++)
    {
      char c = name[i];

      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
        {
          return false;
        }
    }

  return true;
}

/** @brief 路径是否在 notif_icons/ 下。 */
static bool path_is_notif_icon(const char *path)
{
  return path != NULL &&
         strstr(path, "/" COMPANION_NOTIF_ICON_DIR "/") != NULL;
}

/** @brief BLE 写入的 GNSS 星历（/eph 下 .ubx），写完后热加载。 */
static bool path_is_gnss_eph(const char *path)
{
  const char *name;
  size_t n;

  if (path == NULL || strstr(path, "/eph/") == NULL) {
    return false;
  }

  name = strrchr(path, '/');
  name = (name != NULL) ? name + 1 : path;
  n = strlen(name);
  if (n < 5) {
    return false;
  }

  return name[n - 4] == '.' &&
         (name[n - 3] == 'u' || name[n - 3] == 'U') &&
         (name[n - 2] == 'b' || name[n - 2] == 'B') &&
         (name[n - 1] == 'x' || name[n - 1] == 'X');
}

/** @brief OTA 固件槽：CLOSE 必须先落盘再应答。 */
static bool path_is_ota_fw(const char *path)
{
  return path != NULL &&
         (strstr(path, "/mnt/kv/fw") != NULL ||
          strstr(path, "/fw/") != NULL);
}

static bool icon_png_ok(const char *abs_path);

/** @brief CLOSE 已应答后把 RAM 缓冲写入 *.part 并 rename。 */
static void fs_deferred_commit(void)
{
  int ret;
  bool icon = path_is_notif_icon(g_fs.path);
  bool eph = path_is_gnss_eph(g_fs.path);
  uint32_t t0 = fs_now_ms();
  uint32_t left = g_fs.wbuf_len;

  ret = wbuf_flush();
  if (ret == 0)
    {
      ret = xfer_write_commit(g_fs.wfd, g_fs.tmp_path, g_fs.path);
    }

  g_fs.wfd = -1;
  if (ret == 0 && icon && !icon_png_ok(g_fs.path))
    {
      unlink(g_fs.path);
      ret = EIO;
      LOGI("fs write commit rejected icon %s", g_fs.path);
    }

  release_lfs();
  g_fs.state = FS_IDLE;
  if (ret != 0)
    {
      LOGI("fs write commit after-ack failed %s errno=%d", g_fs.path, ret);
      return;
    }

  LOGI("fs write commit %s %lu bytes leftover=%lu %ums",
       g_fs.path, (unsigned long)g_fs.w_expected,
       (unsigned long)left,
       (unsigned)myvendor_mono_elapsed_ms(fs_now_ms(), t0));
  if (eph)
    {
      myvendor_gnss_eph_reload();
      LOGI("fs eph reload %s", g_fs.path);
    }
}

/** @brief 校验 PNG 头与大小。 */
static bool icon_png_ok(const char *abs_path)
{
  static const uint8_t sig[8] =
    {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
  struct stat st;
  uint8_t     hdr[24];
  int         fd;
  ssize_t     n;

  if (abs_path == NULL || abs_path[0] == '\0')
    {
      return false;
    }

  if (stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
      return false;
    }

  if (st.st_size < FS_ICON_PNG_MIN || st.st_size > FS_ICON_PNG_MAX)
    {
      return false;
    }

  fd = open(abs_path, O_RDONLY);
  if (fd < 0)
    {
      return false;
    }

  n = read(fd, hdr, sizeof(hdr));
  close(fd);
  if (n < (ssize_t)sizeof(hdr))
    {
      return false;
    }

  if (memcmp(hdr, sig, sizeof(sig)) != 0)
    {
      return false;
    }

  return memcmp(hdr + 12, "IHDR", 4) == 0;
}

bool companion_fs_icon_usable(const char *abs_path)
{
  if (icon_png_ok(abs_path))
    {
      return true;
    }

  /* 半成品是 *.part，正式名上的坏文件会挡住以后的 FS_NEED。MTP/工厂不删。 */
  if (abs_path != NULL && abs_path[0] != '\0' &&
      access(abs_path, F_OK) == 0 && !xfer_usb_busy() &&
      !myvendor_is_factory())
    {
      if (unlink(abs_path) == 0)
        {
          LOGI("fs icon reject %s", abs_path);
        }
    }

  return false;
}

/** @brief 清除 FS_NEED 队列（持锁）。 */
static void need_clear_locked(void)
{
  int i;

  for (i = 0; i < FS_NEED_Q; i++)
    {
      g_fs.need[i].pending = false;
      g_fs.need[i].last_ms = 0;
      g_fs.need[i].name[0] = '\0';
    }
}

/** @brief 是否有挂起 FS_NEED（持锁）。 */
static bool need_any_pending_locked(void)
{
  int i;

  for (i = 0; i < FS_NEED_Q; i++)
    {
      if (g_fs.need[i].pending && g_fs.need[i].name[0] != '\0')
        {
          return true;
        }
    }

  return false;
}

/** @brief 尝试发送 FS_NEED notify（持锁）。 */
static bool try_queue_need_locked(void)
{
  int      i;
  uint16_t n;
  char     dir[XFER_PATH_MAX];
  int      ret;

  if (g_fs.state != FS_IDLE || g_fs.tx_len != 0 || g_fs.tx_err ||
      g_fs.tx_last || !g_fs.connected || !g_fs.ccc)
    {
      return false;
    }

  for (i = 0; i < FS_NEED_Q; i++)
    {
      if (!g_fs.need[i].pending || g_fs.need[i].name[0] == '\0')
        {
          continue;
        }

      n = (uint16_t)strlen(g_fs.need[i].name);
      ret = xfer_resolve(COMPANION_NOTIF_ICON_DIR,
                         strlen(COMPANION_NOTIF_ICON_DIR),
                         dir, sizeof(dir));
      if (ret == 0)
        {
          hold_lfs();
          ret = xfer_mkdir(dir);
          release_lfs();
          if (ret != 0 && ret != EEXIST)
            {
              LOGI("fs need mkdir %s failed %d", dir, ret);
            }
        }

      g_fs.state = FS_REPLY;
      queue_ok(COMPANION_FS_NEED, 0, (const uint8_t *)g_fs.need[i].name,
               n, true);
      g_fs.need[i].pending = false;
      g_fs.need[i].last_ms = fs_now_ms();
      LOGI("fs need %s", g_fs.need[i].name);
      return true;
    }

  return false;
}

/****************************************************************************
 * Command handlers (companion thread)
 ****************************************************************************/

static void handle_statfs(uint8_t tid)
{
  /* statfs() walks every LittleFS metadata block on this volume and can
   * take minutes.  Refuse rather than stall the radio.
   */

  queue_err(COMPANION_FS_STATFS, tid, EOPNOTSUPP);
}

static void handle_stat(uint8_t tid, const uint8_t *path, uint16_t plen)
{
  struct xfer_stat st;
  uint8_t          body[9];
  int              ret;

  ret = xfer_resolve((const char *)path, plen, g_fs.path, sizeof(g_fs.path));
  if (ret != 0)
    {
      queue_err(COMPANION_FS_STAT, tid, ret);
      return;
    }

  hold_lfs();
  ret = xfer_stat(g_fs.path, &st);
  release_lfs();
  if (ret != 0)
    {
      queue_err(COMPANION_FS_STAT, tid, ret);
      return;
    }

  body[0] = st.type;
  put_le32(body + 1, st.size);
  put_le32(body + 5, st.mtime);
  g_fs.state = FS_REPLY;
  queue_ok(COMPANION_FS_STAT, tid, body, sizeof(body), true);
}

/** @brief 开始 LIST 会话。 */
static void handle_list_begin(uint8_t tid, const uint8_t *payload,
                              uint16_t plen)
{
  int ret;

  if (plen < 2)
    {
      queue_err(COMPANION_FS_LIST, tid, EINVAL);
      return;
    }

  g_fs.list_cursor = get_le16(payload);
  ret = xfer_resolve((const char *)(payload + 2), (size_t)(plen - 2),
                     g_fs.path, sizeof(g_fs.path));
  if (ret != 0)
    {
      queue_err(COMPANION_FS_LIST, tid, ret);
      return;
    }

  g_fs.cmd  = COMPANION_FS_LIST;
  g_fs.tid  = tid;
  g_fs.seq  = 0;
  g_fs.state = FS_LIST;
  hold_lfs();
}

/** @brief 开始 READ 会话。 */
static void handle_read_begin(uint8_t tid, const uint8_t *payload,
                              uint16_t plen)
{
  uint8_t  flags;
  uint16_t poff;
  int      ret;
  char     prev[XFER_PATH_MAX];

  if (plen < 8)
    {
      queue_err(COMPANION_FS_READ, tid, EINVAL);
      return;
    }

  g_fs.read_off = get_le32(payload);
  g_fs.read_len = get_le32(payload + 4);
  flags = 0;
  poff  = 8;
  if (plen >= 9)
    {
      flags = payload[8];
      poff  = 9;
    }

  prev[0] = '\0';
  if (g_fs.rfd >= 0)
    {
      memcpy(prev, g_fs.path, sizeof(prev));
      prev[sizeof(prev) - 1] = '\0';
    }

  ret = xfer_resolve((const char *)(payload + poff), (size_t)(plen - poff),
                     g_fs.path, sizeof(g_fs.path));
  if (ret != 0)
    {
      queue_err(COMPANION_FS_READ, tid, ret);
      return;
    }

  hold_lfs();
  if (g_fs.rfd < 0 || strncmp(prev, g_fs.path, sizeof(prev)) != 0)
    {
      read_fd_close();
      ret = xfer_read_open(g_fs.path, &g_fs.rfd);
      if (ret != 0)
        {
          release_lfs();
          queue_err(COMPANION_FS_READ, tid, ret);
          return;
        }

      g_fs.rfd_pos  = 0;
      g_fs.rbuf_len = 0;
    }

  g_fs.cmd   = COMPANION_FS_READ;
  g_fs.tid   = tid;
  g_fs.seq   = 0;
  g_fs.r_rle = (flags & COMPANION_FS_READ_FLAG_RLE) != 0;
  companion_rle_enc_init(&g_fs.rle_enc);
  g_fs.state = FS_READ;
  LOGI("fs read %s off=%lu len=%lu rle=%d",
       g_fs.path, (unsigned long)g_fs.read_off, (unsigned long)g_fs.read_len,
       (int)g_fs.r_rle);
}

/** @brief 处理 WRITE OPEN。 */
static void handle_write_open(uint8_t tid, const uint8_t *payload,
                              uint16_t plen)
{
  uint8_t  flags;
  uint32_t off = 0;
  uint32_t crc = 0;
  uint8_t  reply[COMPANION_FS_WRITE_OPEN_REPLY_LEN];
  bool     resume;
  int      ret;

  if (plen < 5)
    {
      queue_err(COMPANION_FS_WRITE_OPEN, tid, EINVAL);
      return;
    }

  fs_buffers_init();
  session_reset(false);
  g_fs.w_expected = get_le32(payload);
  flags  = payload[4];
  resume = (flags & COMPANION_FS_WRITE_FLAG_RESUME) != 0;
  g_fs.w_rle = (flags & COMPANION_FS_WRITE_FLAG_RLE) != 0;
  companion_rle_dec_init(&g_fs.rle_dec);
  ret = xfer_resolve((const char *)(payload + 5), (size_t)(plen - 5),
                     g_fs.path, sizeof(g_fs.path));
  if (ret != 0)
    {
      queue_err(COMPANION_FS_WRITE_OPEN, tid, ret);
      return;
    }

  hold_lfs();
  ret = xfer_write_open(g_fs.path, g_fs.tmp_path, sizeof(g_fs.tmp_path),
                        &g_fs.wfd, resume, &off, &crc);
  if (ret != 0)
    {
      release_lfs();
      queue_err(COMPANION_FS_WRITE_OPEN, tid, ret);
      return;
    }

  if (off > g_fs.w_expected)
    {
      xfer_write_suspend(g_fs.wfd);
      g_fs.wfd = -1;
      release_lfs();
      queue_err(COMPANION_FS_WRITE_OPEN, tid, EINVAL);
      return;
    }

  g_fs.w_got  = off;
  g_fs.w_flushed = off;
  g_fs.w_crc  = crc;
  g_fs.rx_seq = 0;
  g_fs.w_fail = 0;
  g_fs.cmd    = COMPANION_FS_WRITE_OPEN;
  g_fs.tid    = tid;
  g_fs.seq    = 0;
  g_fs.state  = FS_WRITE;
  put_le32(reply, off);
  put_le32(reply + 4, crc);
  LOGI("fs write open %s resume=%d rle=%d off=%lu crc=%08lx mtu=%lu",
       g_fs.path, (int)resume, (int)g_fs.w_rle,
       (unsigned long)off, (unsigned long)crc, (unsigned long)g_fs.mtu);
  queue_ok(COMPANION_FS_WRITE_OPEN, tid, reply, sizeof(reply), true);
}

static void write_keep_part(void)
{
  if (g_fs.wfd >= 0)
    {
      /* CLOSE/seq 失败时 RAM 里可能还有未刷的 4KiB～256KiB。不 flush
       * 则下次续传只能接到旧 .part，App 会把已发送的尾巴再发一遍；
       * 更糟的是 CRC 按收到的算、盘上没有，关机后再续会 errno 5。 */
      (void)wbuf_flush();
      xfer_write_suspend(g_fs.wfd);
      g_fs.wfd = -1;
    }

  release_lfs();
}

static void write_drop_part(void)
{
  if (g_fs.wfd >= 0)
    {
      xfer_write_abort(g_fs.wfd, g_fs.tmp_path);
      g_fs.wfd = -1;
    }
  else if (g_fs.tmp_path[0] != '\0')
    {
      unlink(g_fs.tmp_path);
    }

  release_lfs();
}

static void handle_write_close(uint8_t tid, const uint8_t *payload,
                               uint16_t plen)
{
  uint32_t crc;
  int      ret;
  bool     icon = path_is_notif_icon(g_fs.path);

  /* Flush/seq failures keep FS_WRITE with w_fail set (and often wfd=-1).
   * Report that errno instead of a generic EIO after the session looks idle.
   */

  if (g_fs.w_fail != 0)
    {
      LOGI("fs write close aborted state=%d wfd=%d fail=%d",
           (int)g_fs.state, g_fs.wfd, g_fs.w_fail);
      if (icon)
        {
          write_drop_part();
        }
      else
        {
          write_keep_part();
        }

      g_fs.state = FS_IDLE;
      queue_err(COMPANION_FS_WRITE_CLOSE, tid, g_fs.w_fail);
      return;
    }

  if (g_fs.state != FS_WRITE || g_fs.wfd < 0)
    {
      LOGI("fs write close aborted state=%d wfd=%d fail=%d",
           (int)g_fs.state, g_fs.wfd, g_fs.w_fail);
      queue_err(COMPANION_FS_WRITE_CLOSE, tid, EIO);
      return;
    }

  if (plen < 4)
    {
      (void)wbuf_flush();
      if (icon)
        {
          write_drop_part();
        }
      else
        {
          xfer_write_suspend(g_fs.wfd);
          g_fs.wfd = -1;
          release_lfs();
        }

      g_fs.state = FS_IDLE;
      queue_err(COMPANION_FS_WRITE_CLOSE, tid, EINVAL);
      return;
    }

  crc = get_le32(payload);
  if (g_fs.w_rle && !companion_rle_dec_idle(&g_fs.rle_dec))
    {
      (void)wbuf_flush();
      if (icon)
        {
          write_drop_part();
        }
      else
        {
          xfer_write_suspend(g_fs.wfd);
          g_fs.wfd = -1;
          release_lfs();
        }

      g_fs.state = FS_IDLE;
      LOGI("fs write close rle truncated (%s)",
           icon ? "dropped .part" : "kept .part");
      queue_err(COMPANION_FS_WRITE_CLOSE, tid, EINVAL);
      return;
    }

  if (g_fs.w_got != g_fs.w_expected || crc != g_fs.w_crc)
    {
      (void)wbuf_flush();
      if (icon)
        {
          write_drop_part();
        }
      else
        {
          xfer_write_suspend(g_fs.wfd);
          g_fs.wfd = -1;
          release_lfs();
        }

      g_fs.state = FS_IDLE;
      LOGI("fs write close mismatch got=%lu expect=%lu crc=%08lx/%08lx (%s)",
           (unsigned long)g_fs.w_got, (unsigned long)g_fs.w_expected,
           (unsigned long)g_fs.w_crc, (unsigned long)crc,
           icon ? "dropped .part" : "kept .part");
      queue_err(COMPANION_FS_WRITE_CLOSE, tid, EIO);
      return;
    }

  if (path_is_ota_fw(g_fs.path))
    {
      ret = wbuf_flush();
      if (ret != 0)
        {
          xfer_write_suspend(g_fs.wfd);
          g_fs.wfd = -1;
          release_lfs();
          g_fs.state = FS_IDLE;
          queue_err(COMPANION_FS_WRITE_CLOSE, tid, ret);
          return;
        }

      ret = xfer_write_commit(g_fs.wfd, g_fs.tmp_path, g_fs.path);
      g_fs.wfd = -1;
      release_lfs();
      if (ret != 0)
        {
          g_fs.state = FS_IDLE;
          queue_err(COMPANION_FS_WRITE_CLOSE, tid, ret);
          return;
        }

      g_fs.state = FS_REPLY;
      queue_ok(COMPANION_FS_WRITE_CLOSE, tid, NULL, 0, true);
      LOGI("fs write commit %s %lu bytes",
           g_fs.path, (unsigned long)g_fs.w_expected);
      return;
    }

  /* GPX/图标等：先 CLOSE OK 让 App 结束，落盘在 notify 之后。
   * 否则 fsync/write 十几秒，手机进度条卡死。 */
  g_fs.commit_pending = true;
  queue_ok(COMPANION_FS_WRITE_CLOSE, tid, NULL, 0, true);
  LOGI("fs write close acked %s %lu bytes (flush deferred wbuf=%lu)",
       g_fs.path, (unsigned long)g_fs.w_expected,
       (unsigned long)g_fs.wbuf_len);
}

static void handle_simple_path(uint8_t cmd, uint8_t tid,
                               const uint8_t *path, uint16_t plen)
{
  int ret;

  ret = xfer_resolve((const char *)path, plen, g_fs.path, sizeof(g_fs.path));
  if (ret != 0)
    {
      queue_err(cmd, tid, ret);
      return;
    }

  hold_lfs();
  if (cmd == COMPANION_FS_DELETE)
    {
      ret = xfer_delete(g_fs.path);
    }
  else
    {
      ret = xfer_mkdir(g_fs.path);
    }

  release_lfs();
  if (ret != 0)
    {
      queue_err(cmd, tid, ret);
      return;
    }

  g_fs.state = FS_REPLY;
  queue_ok(cmd, tid, NULL, 0, true);
  LOGI("fs %s %s", cmd == COMPANION_FS_DELETE ? "delete" : "mkdir", g_fs.path);
}

/** @brief 处理 RENAME。 */
static void handle_rename(uint8_t tid, const uint8_t *payload, uint16_t plen)
{
  char oldp[XFER_PATH_MAX];
  char newp[XFER_PATH_MAX];
  uint8_t old_len;
  int     ret;

  if (plen < 1)
    {
      queue_err(COMPANION_FS_RENAME, tid, EINVAL);
      return;
    }

  old_len = payload[0];
  if ((uint16_t)(1 + old_len) > plen)
    {
      queue_err(COMPANION_FS_RENAME, tid, EINVAL);
      return;
    }

  ret = xfer_resolve((const char *)(payload + 1), old_len,
                     oldp, sizeof(oldp));
  if (ret != 0)
    {
      queue_err(COMPANION_FS_RENAME, tid, ret);
      return;
    }

  ret = xfer_resolve((const char *)(payload + 1 + old_len),
                     (size_t)(plen - 1 - old_len), newp, sizeof(newp));
  if (ret != 0)
    {
      queue_err(COMPANION_FS_RENAME, tid, ret);
      return;
    }

  hold_lfs();
  ret = xfer_rename(oldp, newp);
  release_lfs();
  if (ret != 0)
    {
      queue_err(COMPANION_FS_RENAME, tid, ret);
      return;
    }

  g_fs.state = FS_REPLY;
  queue_ok(COMPANION_FS_RENAME, tid, NULL, 0, true);
  LOGI("fs rename %s -> %s", oldp, newp);
}

static void dispatch_cmd(const uint8_t *buf, uint16_t len)
{
  uint8_t        cmd;
  uint8_t        tid;
  const uint8_t *payload;
  uint16_t       plen;

  if (len < COMPANION_FS_CMD_HDR_LEN)
    {
      return;
    }

  cmd     = buf[0];
  tid     = buf[1];
  payload = buf + COMPANION_FS_CMD_HDR_LEN;
  plen    = (uint16_t)(len - COMPANION_FS_CMD_HDR_LEN);

  if (myvendor_is_factory())
    {
      static bool logged;

      if (!logged)
        {
          logged = true;
          LOGI("fs disabled in factory; copy files over USB MTP");
        }

      queue_err(cmd, tid, EACCES);
      return;
    }

  if (xfer_usb_busy())
    {
      queue_err(cmd, tid, EBUSY);
      return;
    }

  /* A new command aborts an in-flight LIST/READ.  WRITE_CLOSE is the
   * exception: it finishes the WRITE session opened earlier.
   */

  if (g_fs.state != FS_IDLE && g_fs.state != FS_WRITE &&
      cmd != COMPANION_FS_WRITE_CLOSE)
    {
      session_reset(false);
    }

  /* Each command is its own seq space.  Forgetting this makes STAT/DELETE
   * after LIST start at seq=1; the App then rejects a perfectly good LAST.
   */

  g_fs.seq = 0;

  switch (cmd)
    {
      case COMPANION_FS_STATFS:
        handle_statfs(tid);
        break;

      case COMPANION_FS_LIST:
        handle_list_begin(tid, payload, plen);
        break;

      case COMPANION_FS_STAT:
        handle_stat(tid, payload, plen);
        break;

      case COMPANION_FS_READ:
        handle_read_begin(tid, payload, plen);
        break;

      case COMPANION_FS_WRITE_OPEN:
        handle_write_open(tid, payload, plen);
        break;

      case COMPANION_FS_WRITE_CLOSE:
        handle_write_close(tid, payload, plen);
        break;

      case COMPANION_FS_DELETE:
      case COMPANION_FS_MKDIR:
        handle_simple_path(cmd, tid, payload, plen);
        break;

      case COMPANION_FS_RENAME:
        handle_rename(tid, payload, plen);
        break;

      default:
        queue_err(cmd, tid, ENOSYS);
        break;
    }
}

static int rle_write_out(void *ctx, const uint8_t *p, uint16_t n)
{
  int ret;

  (void)ctx;
  if (n == 0)
    {
      return 0;
    }

  if (g_fs.w_got > g_fs.w_expected ||
      (uint32_t)n > g_fs.w_expected - g_fs.w_got)
    {
      return EINVAL;
    }

  ret = wbuf_append(p, n);
  if (ret != 0)
    {
      return ret;
    }

  g_fs.w_crc = crc32part(p, n, g_fs.w_crc);
  g_fs.w_got += n;
  return 0;
}

static void dispatch_data(const uint8_t *buf, uint16_t len)
{
  struct companion_fs_hdr hdr;
  const uint8_t          *payload;
  int                     ret;

  if (len < COMPANION_FS_HDR_LEN)
    {
      return;
    }

  hdr.cmd    = buf[0];
  hdr.tid    = buf[1];
  hdr.flags  = buf[2];
  hdr.status = buf[3];
  hdr.seq    = get_le16(buf + 4);
  hdr.len    = get_le16(buf + 6);
  if ((uint16_t)(COMPANION_FS_HDR_LEN + hdr.len) > len)
    {
      queue_err(COMPANION_FS_WRITE_DATA, hdr.tid, EINVAL);
      return;
    }

  payload = buf + COMPANION_FS_HDR_LEN;
  (void)hdr.flags;
  (void)hdr.status;

  if (myvendor_is_factory())
    {
      queue_err(hdr.cmd, hdr.tid, EACCES);
      return;
    }

  if (hdr.cmd != COMPANION_FS_WRITE_DATA)
    {
      queue_err(hdr.cmd, hdr.tid, EINVAL);
      return;
    }

  if (g_fs.state != FS_WRITE || g_fs.wfd < 0)
    {
      return;
    }

  if (g_fs.w_fail != 0)
    {
      return;
    }

  if (hdr.seq != g_fs.rx_seq)
    {
      LOGI("fs write seq gap got=%u expect=%u (kept .part)",
           (unsigned)hdr.seq, (unsigned)g_fs.rx_seq);
      g_fs.w_fail = EIO;
      /* 马上 notify，别等 App 把剩余两兆发完再 CLOSE 才知道失败。 */
      queue_err(COMPANION_FS_WRITE_DATA, hdr.tid, EIO);
      return;
    }

  if (g_fs.w_rle)
    {
      ret = companion_rle_dec_feed(&g_fs.rle_dec, payload, hdr.len,
                                   rle_write_out, NULL);
    }
  else
    {
      ret = wbuf_append(payload, hdr.len);
      if (ret == 0)
        {
          g_fs.w_crc = crc32part(payload, hdr.len, g_fs.w_crc);
          g_fs.w_got += hdr.len;
        }
    }

  if (ret != 0)
    {
      g_fs.w_fail = ret;
      LOGI("fs write data failed errno=%d seq=%u got=%lu rle=%d (kept .part)",
           g_fs.w_fail, (unsigned)hdr.seq, (unsigned long)g_fs.w_got,
           (int)g_fs.w_rle);
      write_keep_part();
      return;
    }

  g_fs.rx_seq++;
}

static void fill_list_page(void)
{
  uint16_t budget = payload_budget();
  bool     last   = false;
  int      n;

  if (budget == 0)
    {
      return;
    }

  n = xfer_list_pack(g_fs.path, &g_fs.list_cursor, g_fs.tx, budget, &last);
  if (n < 0)
    {
      release_lfs();
      queue_err(COMPANION_FS_LIST, g_fs.tid, n);
      return;
    }

  g_fs.tx_len    = (uint16_t)n;
  g_fs.tx_last   = last;
  g_fs.tx_err    = false;
  g_fs.tx_status = 0;
  if (last)
    {
      release_lfs();
      g_fs.state = FS_REPLY;
    }
}

static void fill_read_page(void)
{
  uint16_t budget = payload_budget();
  uint16_t want;
  uint16_t got = 0;
  int      ret;

  if (budget == 0)
    {
      return;
    }

  if (g_fs.r_rle)
    {
      g_fs.tx_len = 0;
      while (g_fs.tx_len < budget)
        {
          uint16_t room;
          uint16_t produced;
          bool     finish;
          uint8_t  tmp[COMPANION_RLE_LOOK];

          room = companion_rle_enc_room(&g_fs.rle_enc);
          if (room > 0 && g_fs.read_len > 0)
            {
              want = room;
              if (want > g_fs.read_len)
                {
                  want = (uint16_t)g_fs.read_len;
                }

              got = 0;
              ret = rbuf_get(g_fs.read_off, tmp, want, &got);
              if (ret != 0)
                {
                  release_lfs();
                  queue_err(COMPANION_FS_READ, g_fs.tid, ret);
                  return;
                }

              ret = companion_rle_enc_push(&g_fs.rle_enc, tmp, got);
              if (ret != 0)
                {
                  release_lfs();
                  queue_err(COMPANION_FS_READ, g_fs.tid, ret);
                  return;
                }

              g_fs.read_off += got;
              if (g_fs.read_len >= got)
                {
                  g_fs.read_len -= got;
                }
              else
                {
                  g_fs.read_len = 0;
                }

              if (got < want)
                {
                  g_fs.read_len = 0;
                }
            }

          finish = (g_fs.read_len == 0);
          produced = companion_rle_enc_pull(&g_fs.rle_enc,
                                            g_fs.tx + g_fs.tx_len,
                                            (uint16_t)(budget - g_fs.tx_len),
                                            finish);
          if (produced == 0)
            {
              break;
            }

          g_fs.tx_len = (uint16_t)(g_fs.tx_len + produced);
        }

      g_fs.tx_err    = false;
      g_fs.tx_status = 0;
      g_fs.tx_last   = (g_fs.read_len == 0 &&
                        companion_rle_enc_pending(&g_fs.rle_enc) == 0);
      if (g_fs.tx_last)
        {
          release_lfs();
          g_fs.state = FS_REPLY;
        }

      return;
    }

  want = budget;
  if (g_fs.read_len < want)
    {
      want = (uint16_t)g_fs.read_len;
    }

  ret = rbuf_get(g_fs.read_off, g_fs.tx, want, &got);
  if (ret != 0)
    {
      release_lfs();
      queue_err(COMPANION_FS_READ, g_fs.tid, ret);
      return;
    }

  g_fs.read_off += got;
  if (g_fs.read_len >= got)
    {
      g_fs.read_len -= got;
    }
  else
    {
      g_fs.read_len = 0;
    }

  g_fs.tx_len    = got;
  g_fs.tx_err    = false;
  g_fs.tx_status = 0;
  g_fs.tx_last   = (got < want) || (g_fs.read_len == 0);
  if (g_fs.tx_last)
    {
      release_lfs();
      g_fs.state = FS_REPLY;
    }
}

/**
 * @brief 组一帧 tx（调用方持有 g_fs.lock）：只读状态 + 拷帧，不发通知。
 *
 * @return true = 有帧可发，*flen 是总长；handle/peer/attr 也一并抄出来，
 *         好让调用方在**锁外**用它们发通知（不能让 `&g_fs.peer` 这种内部地址
 *         逃到锁外去）。
 */
static bool send_tx_build(uint8_t *frame, uint16_t *flen,
                          gatts_handle_t *handle, bt_address_t *peer,
                          uint16_t *attr)
{
  uint8_t flags = 0;

  if (g_fs.handle == NULL || !g_fs.connected || !g_fs.ccc)
    {
      return false;
    }

  if (g_fs.tx_last)
    {
      flags |= COMPANION_FS_FLAG_LAST;
    }

  if (g_fs.tx_err)
    {
      flags |= COMPANION_FS_FLAG_ERR;
    }

  frame[0] = g_fs.cmd;
  frame[1] = g_fs.tid;
  frame[2] = flags;
  frame[3] = g_fs.tx_status;
  put_le16(frame + 4, g_fs.seq);
  put_le16(frame + 6, g_fs.tx_len);
  if (g_fs.tx_len > 0)
    {
      memcpy(frame + COMPANION_FS_HDR_LEN, g_fs.tx, g_fs.tx_len);
    }

  *handle = g_fs.handle;
  *peer   = g_fs.peer;
  *attr   = g_fs.data_attr;
  *flen   = (uint16_t)(COMPANION_FS_HDR_LEN + g_fs.tx_len);
  return true;
}

/**
 * @brief 通知发完之后记账（调用方持有 g_fs.lock）。**不在这里打日志**：syslog
 *        会走串口，锁内打日志等于让协议栈线程等串口。失败信息由调用方解锁后打。
 */
static bool send_tx_finish(bool ok)
{
  if (!ok)
    {
      g_diag_notify_fail++;
      return false;
    }

  g_diag_notify_sent++;

  if (g_fs.inflight < 255)
    {
      g_fs.inflight++;
    }

  g_fs.send_ms = fs_now_ms();
  g_fs.seq++;
  g_fs.tx_len = 0;
  if (g_fs.tx_last && g_fs.state != FS_WRITE)
    {
      g_fs.state = FS_IDLE;
    }

  g_fs.tx_last = false;
  g_fs.tx_err  = false;
  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 把 FS 会话绑到 Companion GATTS 句柄。
 */
void companion_fs_bind(gatts_handle_t handle, uint16_t data_attr_id)
{
  pthread_mutex_lock(&g_fs.lock);
  g_fs.handle    = handle;
  g_fs.data_attr = data_attr_id;
  if (!g_fs_wake_ready)
    {
      sem_init(&g_fs_wake, 0, 0);
      g_fs_wake_ready = true;
    }

  fs_buffers_init();
  g_fs.abort_req = false;
  pthread_mutex_unlock(&g_fs.lock);
}

/**
 * @brief 手机已连接：记下对端并唤醒泵。
 */
void companion_fs_on_connect(const bt_address_t *peer)
{
  pthread_mutex_lock(&g_fs.lock);
  if (peer != NULL)
    {
      g_fs.peer = *peer;
    }

  g_fs.connected = true;
  g_fs.inflight  = 0;
  pthread_mutex_unlock(&g_fs.lock);
  companion_fs_kick();
}

/**
 * @brief 手机断开：关掉 CCC，保留 *.part 以便续传。
 */
void companion_fs_on_disconnect(void)
{
  /* 诊断：在会话结束时把通知对账打出来。sent - done 就是"已计入 seq 但从未
   * 确认到达"的页数；credit_lost 是信用超时强行归零时吞掉的页数。若手机在
   * 传输中途主动断开（reason=0x13），这两个数就是它断开的直接解释。 */
  LOGI("fs_disc state=%d seq=%u sent=%lu done=%lu "
       "fail=%lu credit_to=%lu credit_lost=%lu inflight=%u "
       "gap=%ldms mtu=%u ccc=%d",
       (int)g_fs.state, (unsigned)g_fs.seq,
       (unsigned long)g_diag_notify_sent,
       (unsigned long)g_diag_notify_done,
       (unsigned long)g_diag_notify_fail,
       (unsigned long)g_diag_credit_timeout,
       (unsigned long)g_diag_credit_lost,
       (unsigned)g_fs.inflight,
       (long)(g_fs.send_ms != 0
                ? (long)myvendor_mono_elapsed_ms(fs_now_ms(), g_fs.send_ms)
                : -1L),
       (unsigned)g_fs.mtu, g_fs.ccc ? 1 : 0);

  pthread_mutex_lock(&g_fs.lock);
  g_fs.connected = false;
  g_fs.ccc       = false;
  need_clear_locked();
  session_reset(false);
  pthread_mutex_unlock(&g_fs.lock);
}

/**
 * @brief 仅当 @p addr 是当前手机对端时记下 ATT MTU。
 * @param addr GATTS 对端。
 * @param mtu ATT MTU（载荷为 MTU-3）。
 *
 * 未连接或地址不符则忽略。不要对 HR/CSC/CPS 调用；即便误调也不会改 MTU。
 * 见 docs/ble/ble_sensor.md。
 */
void companion_fs_set_mtu(const bt_address_t *addr, uint32_t mtu)
{
  if (addr == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_fs.lock);
  if (!g_fs.connected ||
      memcmp(g_fs.peer.addr, addr->addr, BT_ADDR_LENGTH) != 0)
    {
      pthread_mutex_unlock(&g_fs.lock);
      return;
    }

  g_fs.mtu = mtu;
  pthread_mutex_unlock(&g_fs.lock);
}

/**
 * @brief FS Data notify（0xFF16）的 CCC。
 */
void companion_fs_set_ccc(bool enabled)
{
  bool was;
  int  state;

  pthread_mutex_lock(&g_fs.lock);
  was             = g_fs.ccc;
  state           = (int)g_fs.state;
  g_fs.ccc        = enabled;
  g_fs.inflight   = 0;

  /** **不要在 CCC=0 的瞬间立刻 `session_reset`**（2026-09-21）。
   *
   *  以前这里一关订阅就把开着的事务收掉，是为了避免 `FS_BUSY` 常亮。但 App
   *  侧的"3 s 无第一帧 → 立刻补订"会在**事务还挂着**时把 CCC 写成 0 再写回 1
   *  （`setNotifyValue` 重建订阅）；设备这边若立刻 abort，宽限期内回来的 CCC
   *  也救不回那条 reply —— App 看到的就是"就绪后文件传输全超时 / 无法通信"。
   *
   *  卡死兜底改由 `companion_fs_pump` 的 `FS_CCC_GRACE_MS` 宽限期负责：CCC
   *  在宽限内补上 ⇒ 事务接着发；一直没补 ⇒ 再收掉，busy 照样落回 IDLE。 */
  if (!enabled && state != FS_IDLE)
    {
      LOGI("fs ccc off while state=%d (grace, not abort)", state);
    }
  else if (enabled && !was && state != FS_IDLE)
    {
      LOGI("fs ccc on while state=%d (resume)", state);
    }

  pthread_mutex_unlock(&g_fs.lock);
  LOGI("fs subscribe -> %d (was=%d state=%d)", (int)enabled, was ? 1 : 0, state);

  /* 开关都 kick：开 ⇒ 把压着的 reply 立刻发出去；关 ⇒ 让宽限期计时跑起来。 */
  companion_fs_kick();
}

/* 事务"有进展"打点：定义在 pump 前面（它要用 g_fs_progress_ms），而 on_cmd/on_data
 * 在文件更前面就调它 —— 所以先声明。 */
static void fs_progress_ping(void);

/**
 * @brief 拷贝一次 0xFF15 命令写；真正 I/O 在 companion_fs_pump()。
 */
uint16_t companion_fs_on_cmd(const uint8_t *value, uint16_t length)
{
  uint8_t cmd;
  uint8_t tid;
  int     state;
  int     ccc;
  int     conn;
  unsigned qn;

  if (value == NULL || length == 0)
    {
      return 0;
    }

  if (length > sizeof(g_fs.cmd_req.buf))
    {
      length = sizeof(g_fs.cmd_req.buf);
    }

  cmd = value[0];
  tid = (length > 1) ? value[1] : 0;

  pthread_mutex_lock(&g_fs.lock);
  memcpy(g_fs.cmd_req.buf, value, length);
  g_fs.cmd_req.len     = length;
  g_fs.cmd_req.pending = true;
  fs_progress_ping();          /* 收到命令 = 事务有进展（看门狗别误判卡死） */
  state = (int)g_fs.state;
  ccc   = g_fs.ccc ? 1 : 0;
  conn  = g_fs.connected ? 1 : 0;
  qn    = (unsigned)g_fs.data_q_n;
  pthread_mutex_unlock(&g_fs.lock);
  /* 诊断放锁外：printf 走串口，别堵协议栈线程。 */
  LOGI("fs cmd in cmd=0x%02x tid=%u len=%u state=%d ccc=%d conn=%d q=%u",
       (unsigned)cmd, (unsigned)tid, (unsigned)length, state, ccc, conn, qn);
  companion_fs_kick();
  return length;
}

/**
 * @brief 拷贝一次 0xFF16 数据写；真正 I/O 在 companion_fs_pump()。
 */
uint16_t companion_fs_on_data(const uint8_t *value, uint16_t length)
{
  uint16_t wr;
  int      waits = 0;
  static unsigned s_drop_log;

  if (value == NULL || length == 0)
    {
      return 0;
    }

  if (length > FS_SLOT_MAX)
    {
      length = FS_SLOT_MAX;
    }

  pthread_mutex_lock(&g_fs.lock);
  if (g_fs.data_q == NULL || g_fs.data_q_cap == 0)
    {
      fs_buffers_init();
    }

  while (g_fs.data_q != NULL && g_fs.data_q_cap != 0 &&
         g_fs.data_q_n >= g_fs.data_q_cap && waits < FS_ENQ_WAIT_MAX)
    {
      pthread_mutex_unlock(&g_fs.lock);
      companion_fs_kick();
      usleep(FS_ENQ_WAIT_US);
      waits++;
      pthread_mutex_lock(&g_fs.lock);
    }

  if (g_fs.data_q == NULL || g_fs.data_q_cap == 0 ||
      g_fs.data_q_n >= g_fs.data_q_cap)
    {
      pthread_mutex_unlock(&g_fs.lock);
      if ((s_drop_log++ & 63) == 0)
        {
          LOGE("fs data queue full, drop %u (x%u)",
               (unsigned)length, s_drop_log);
        }

      return 0;
    }

  wr = (uint16_t)((g_fs.data_q_rd + g_fs.data_q_n) % g_fs.data_q_cap);
  memcpy(g_fs.data_q[wr].buf, value, length);
  g_fs.data_q[wr].len = length;
  g_fs.data_q_n++;
  fs_progress_ping();          /* 收到写数据 = 最强进展信号（上传期间没有通知） */
  pthread_mutex_unlock(&g_fs.lock);
  companion_fs_kick();
  return length;
}

/**
 * @brief 中止当前 FS 事务（Control 0x10）。
 */
void companion_fs_abort(void)
{
  pthread_mutex_lock(&g_fs.lock);
  g_fs.abort_req = true;
  pthread_mutex_unlock(&g_fs.lock);
  companion_fs_kick();
}

/**
 * @brief 0xFF16 notify 完成；释放 in-flight 额度。
 */
void companion_fs_on_notify_complete(uint16_t attr_id, gatt_status_t status)
{
  pthread_mutex_lock(&g_fs.lock);
  if (attr_id == g_fs.data_attr)
    {
      if (g_fs.inflight > 0)
        {
          g_fs.inflight--;
        }
      /* 诊断：完成事件计数。与 g_diag_notify_sent 的差值就是"已计入 seq
       * 但从未确认到达"的页数，也就是手机侧看到的缺口。 */
      g_diag_notify_done++;
      if (status != GATT_STATUS_SUCCESS)
        {
          LOGE("fs notify complete status=%d", (int)status);
        }
    }

  pthread_mutex_unlock(&g_fs.lock);
  companion_fs_kick();
}

/**
 * @brief 是否有 list/read/write 会话在进行。
 */
bool companion_fs_busy(void)
{
  bool busy;

  pthread_mutex_lock(&g_fs.lock);
  busy = (g_fs.state != FS_IDLE) || g_fs.cmd_req.pending ||
         g_fs.data_q_n != 0 || g_fs.abort_req || g_fs.commit_pending;
  pthread_mutex_unlock(&g_fs.lock);
  return busy;
}

/** 最近一次"事务有进展"的时刻（发出帧 / 收到命令 / 收到写数据）。见 pump 里的看门狗。 */
static uint32_t g_fs_progress_ms;
/** 宽限期内（`!ccc`）：置位后若 ccc 回来则恢复事务而不是收掉。 */
static bool     g_fs_grace;

/**
 * @brief 标记事务有进展（调用方需持 @c g_fs.lock）。
 *
 * 看门狗判的是**有没有进展**，不是"有没有发出通知"：上传 GPX 时设备一直在**收**
 * 数据（0xFF15 写）却几乎不发通知，按"发帧"计时会把整个上传判成卡死、
 * 在 100% 那一刻把事务收掉 —— 用户看到的就是"重复上传，最后 errno=5 存入失败"。
 */
static void fs_progress_ping(void)
{
  g_fs_progress_ms = fs_now_ms();
}

/**
 * @brief companion 线程上的 FS 工作函数。
 * @return 还有未完成工作则为 true。
 */
bool companion_fs_pump(void)
{
  int notify_fail_ret = 0;
  uint8_t  cmd_buf[FS_STAGING_MAX];
  uint16_t cmd_len = 0;
  uint8_t  data_buf[FS_SLOT_MAX];
  uint16_t data_len = 0;
  bool     abort = false;
  bool     busy;
  int      drained = 0;

  pthread_mutex_lock(&g_fs.lock);

  /** **事务卡死看门狗**（2026-09-20）。
   *
   *  现场：app 一直显示"码表 USB/MTP 占用中"、星历/文件操作全被 EBUSY 拒，
   *  而 `fs_disc` 是 `state=2 (FS_LIST) … ccc=1`（订阅还在、事务停着）。
   *
   *  成因：通知发送失败时 `send_tx_finish(false)` 直接 return，**不清
   *  `tx_last`/`tx_err`/`state`**；下一拍 `send_tx_build()` 撞上 `tx_last`
   *  就 false —— 于是这个事务**再也发不出一页、也永远回不到 IDLE**：
   *  `companion_fs_busy()` 恒真 ⇒ 状态帧 `FS_BUSY`(bit4) 常亮 ⇒ app 报占用，
   *  而 app 自己的命令又被"事务未收尾"refuse，两边互相等 ⇒ 只能靠取消订阅
   *  （ccc off 的收尾）或断链才解开，重连后又重演。
   *
   *  所以这里按时间兜底：进了事务却 `FS_SESSION_STALL_MS` 没有一次成功发送，
   *  就当作卡死收掉（与 ccc-off 同一收尾语义），让 app 能干净重来。
   *
   *  **2026-09-20 补**：没有通知通道（`ccc=0`，即 app 已取消订阅，或断链）而事务
   *  还开着 —— 那是**一页也发不出去**的局面，不必等 3 s，立刻收。
   *  这一档以前写成了"重置计时"，于是 `state=1 (FS_REPLY) + ccc=0` 这种停法
   *  永远逃过看门狗：命令在取消订阅之后才被处理、reply 组好了却没有通道发，
   *  就永远挂在 FS_REPLY ⇒ `companion_fs_busy()` 恒真 ⇒ 状态帧 FS_BUSY 常亮
   *  ⇒ app 报"码表 USB/MTP 占用中，请先拔掉数据线"（用户现场：线早拔了）。
   *  同一条链还握着 LFS：挂在 FS_REPLY 就永远不会走到 `release_lfs()`，
   *  星历注入于是被判 `mtp busy: hold=1 hold_by[companion+400s …]`。
   *  收掉这一个事务，两处一起解开。 */
  {
    uint32_t now_ms = fs_now_ms();

    if (g_fs.state == FS_IDLE)
      {
        g_fs_progress_ms = now_ms;     /* 空闲：计时跟着走 */
      }
    else if (!g_fs.connected)
      {
        /* 断链：没有通道、也不会有人补订，直接收。 */
        LOGE("fs session aborted (disconnected, state=%d)", (int)g_fs.state);
        g_fs_progress_ms = 0;
        session_reset(false);
      }
    else if (!g_fs.ccc)
      {
        /** **没有通知通道，但给一个宽限期。**
         *
         *  手机在"重连 / GATT 重建"时会把 0xFF16 的 CCC 写成 0，而 app 往往
         *  紧接着就发下一条命令（现场：`fs subscribe -> 0` 后立刻 `state=2
         *  ccc=0`）。这时 reply 已经组好、只是没有通道 —— **别急着收**：
         *  喂帧那条路只要 `ccc` 回来就能把它发出去（pump 里 `send_tx_build()`
         *  的门本来就是 ccc）。所以在这里停 `FS_CCC_GRACE_MS` 等补订：
         *  补上了 ⇒ 事务照常完成（app 侧无感）；一直没补 ⇒ 再收掉，
         *  保证不会像以前那样永久挂着。 */
        if (!g_fs_grace)
          {
            g_fs_grace       = true;
            g_fs_progress_ms = now_ms ? now_ms : 1u;
          }
        else if (myvendor_mono_elapsed_ms(now_ms, g_fs_progress_ms) >
                 FS_CCC_GRACE_MS)
          {
            LOGE("fs session aborted (no ccc %u ms, state=%d)",
                 (unsigned)myvendor_mono_elapsed_ms(now_ms, g_fs_progress_ms),
                 (int)g_fs.state);
            g_fs_grace       = false;
            g_fs_progress_ms = now_ms;
            session_reset(false);
          }
      }
    else if (g_fs_grace)
      {
        /* 补订回来了：事务接着走，计时从这一刻重新起算。 */
        LOGI("fs ccc restored, session resumes state=%d", (int)g_fs.state);
        g_fs_grace       = false;
        g_fs_progress_ms = now_ms;
      }
    else if (myvendor_mono_elapsed_ms(now_ms, g_fs_progress_ms) >
                 /* **写入/落盘中的事务另给一个上限**：GPX/日志上传时设备只收
                  * 0xFF15 数据、几乎不发通知，落盘 commit 也可能耗几秒（文件自述
                  * 单次 flush 可达 18 s）。按普通 3 s 判会把整个上传在 100% 那一刻
                  * 收掉 ⇒ app 看到 errno=5「存入失败」然后重传。所以这条路上放到
                  * `FS_SESSION_STALL_IO_MS`（30 s 完全没有进展才算真卡死）。 */
                 ((g_fs.wfd >= 0 || g_fs.commit_pending)
                      ? FS_SESSION_STALL_IO_MS
                      : FS_SESSION_STALL_MS))
      {
        LOGE("fs session aborted (stall %u ms, state=%d fail=%lu wfd=%d commit=%d)",
             (unsigned)myvendor_mono_elapsed_ms(now_ms, g_fs_progress_ms),
             (int)g_fs.state, (unsigned long)g_diag_notify_fail,
             g_fs.wfd, g_fs.commit_pending ? 1 : 0);
        g_fs_progress_ms = now_ms;
        session_reset(false);
      }
  }

  if (g_fs.abort_req)
    {
      if (g_fs.commit_pending)
        {
          g_fs.abort_req = false;
        }
      else
        {
          g_fs.abort_req = false;
          abort = true;
          g_fs.data_q_n = 0;
          g_fs.data_q_rd = 0;
        }
    }

  if (g_fs.cmd_req.pending)
    {
      cmd_len = g_fs.cmd_req.len;
      memcpy(cmd_buf, g_fs.cmd_req.buf, cmd_len);
      g_fs.cmd_req.pending = false;
    }

  pthread_mutex_unlock(&g_fs.lock);

  if (abort)
    {
      pthread_mutex_lock(&g_fs.lock);
      session_reset(true);
      pthread_mutex_unlock(&g_fs.lock);
      LOGI("fs abort");
    }

  /* WRITE_CLOSE lives on the command characteristic.  Drain queued DATA
   * first so the last packets are in the PSRAM cache (and CRC) before we
   * commit.  Other commands still run first so WRITE_OPEN beats leftover
   * DATA from a previous transfer.
   */

  if (cmd_len > 0 && cmd_buf[0] == COMPANION_FS_WRITE_CLOSE)
    {
      unsigned drained_close = 0;

      /* 先把环里的 DATA 解到 RAM 算 CRC，不要在应答前写 /mnt/lfs。
       * App 无响应写 1 秒把整文件塞进环，再立刻 CLOSE；若这里落盘，
       * 第一次 lfs_alloc（地图盘很满）会卡数秒，手机进度条和导入列表一起死。 */
      g_fs.disk_hold = true;
      while (true)
        {
          pthread_mutex_lock(&g_fs.lock);
          if (g_fs.data_q_n == 0 || g_fs.data_q == NULL ||
              g_fs.data_q_cap == 0)
            {
              pthread_mutex_unlock(&g_fs.lock);
              break;
            }

          data_len = g_fs.data_q[g_fs.data_q_rd].len;
          memcpy(data_buf, g_fs.data_q[g_fs.data_q_rd].buf, data_len);
          g_fs.data_q_rd =
            (uint16_t)((g_fs.data_q_rd + 1) % g_fs.data_q_cap);
          g_fs.data_q_n--;
          pthread_mutex_unlock(&g_fs.lock);
          dispatch_data(data_buf, data_len);
          drained_close++;
          if ((drained_close & 31u) == 0u)
            {
              wbuf_yield();
            }
        }

      g_fs.disk_hold = false;

      /* CLOSE 应答不要握着 g_fs.lock。落盘在 notify 之后。 */
      dispatch_cmd(cmd_buf, cmd_len);
    }
  else
    {
      if (cmd_len > 0)
        {
          pthread_mutex_lock(&g_fs.lock);
          dispatch_cmd(cmd_buf, cmd_len);
          pthread_mutex_unlock(&g_fs.lock);
        }

      while (drained < FS_DATA_DRAIN_MAX)
        {
          pthread_mutex_lock(&g_fs.lock);
          if (g_fs.data_q_n == 0 || g_fs.data_q == NULL ||
              g_fs.data_q_cap == 0)
            {
              pthread_mutex_unlock(&g_fs.lock);
              break;
            }

          data_len = g_fs.data_q[g_fs.data_q_rd].len;
          memcpy(data_buf, g_fs.data_q[g_fs.data_q_rd].buf, data_len);
          g_fs.data_q_rd =
            (uint16_t)((g_fs.data_q_rd + 1) % g_fs.data_q_cap);
          g_fs.data_q_n--;
          pthread_mutex_unlock(&g_fs.lock);
          dispatch_data(data_buf, data_len);
          drained++;
        }
    }

  pthread_mutex_lock(&g_fs.lock);

  if (g_fs.inflight > 0 && g_fs.send_ms != 0 &&
      myvendor_mono_elapsed_ms(fs_now_ms(), g_fs.send_ms) >
      FS_CREDIT_TIMEOUT_MS)
    {
      /* 诊断：信用超时 = 发出去的通知没收到 notify-complete，代码在这里
       * 强行把 inflight 归零继续发。bt_gatts_notify() 的返回值并不可信
       * （Vela SAL 丢掉了 bt_gatt_notify_cb 的结果），所以"已发未达"的页
       * 会照常计入 seq。这个计数直接对应手机侧看到的序号/CRC 断裂。 */
      g_diag_credit_timeout++;
      g_diag_credit_lost += (uint32_t)g_fs.inflight;
      g_fs.inflight = 0;
    }

  while (g_fs.connected && g_fs.ccc &&
         g_fs.inflight < FS_NOTIFY_INFLIGHT_MAX &&
         (g_fs.send_ms == 0 ||
          myvendor_mono_elapsed_ms(fs_now_ms(), g_fs.send_ms) >=
          FS_NOTIFY_GAP_MS))
    {
      if (g_fs.state == FS_IDLE && g_fs.tx_len == 0 && !g_fs.tx_last &&
          !g_fs.tx_err)
        {
          (void)try_queue_need_locked();
        }

      if (g_fs.state == FS_LIST && g_fs.tx_len == 0 && !g_fs.tx_err)
        {
          fill_list_page();
        }
      else if (g_fs.state == FS_READ && g_fs.tx_len == 0 && !g_fs.tx_err)
        {
          fill_read_page();
        }

      if (g_fs.state == FS_REPLY ||
          ((g_fs.state == FS_LIST || g_fs.state == FS_READ ||
            g_fs.state == FS_WRITE) &&
           (g_fs.tx_len > 0 || g_fs.tx_last || g_fs.tx_err)))
        {
          uint8_t      frame[FS_STAGING_MAX];
          uint16_t     flen = 0;
          gatts_handle_t n_handle;
          bt_address_t n_peer;
          uint16_t     n_attr;
          bt_status_t  n_ret;

          if (!send_tx_build(frame, &flen, &n_handle, &n_peer, &n_attr))
            {
              /* 组不出帧：多半是 ccc/conn 掉了。限频打，避免 pump 每拍刷屏。 */
              static uint32_t s_skip_log_ms;
              uint32_t now = fs_now_ms();

              if (s_skip_log_ms == 0 ||
                  myvendor_mono_elapsed_ms(now, s_skip_log_ms) > 1000u)
                {
                  s_skip_log_ms = now ? now : 1u;
                  LOGI("fs tx skip state=%d ccc=%d conn=%d handle=%d "
                       "tx_len=%u last=%d",
                       (int)g_fs.state, g_fs.ccc ? 1 : 0,
                       g_fs.connected ? 1 : 0,
                       g_fs.handle != NULL ? 1 : 0,
                       (unsigned)g_fs.tx_len, g_fs.tx_last ? 1 : 0);
                }

              break;
            }

          /* **通知在锁外发**：bt_gatts_notify() 要等协议栈线程收下这一包，而
           * 协议栈线程的 fs 回调（set_ccc / on_notify_complete / on_cmd …）
           * 要同一把 g_fs.lock —— 持锁发通知就是"自己等自己"。该文件自述
           * 单次 flush 可达 18 s，被堵的正是协议栈。
           * 结构：锁内取状态组帧 → 锁外 notify → 锁内记账。 */
          pthread_mutex_unlock(&g_fs.lock);
          n_ret = bt_gatts_notify(n_handle, &n_peer, n_attr, frame, flen);
          pthread_mutex_lock(&g_fs.lock);

          if (!send_tx_finish(n_ret == BT_STATUS_SUCCESS))
            {
              notify_fail_ret = (int)n_ret;
              break;
            }

          /* 成功发出一页：喂卡死看门狗（见函数开头的说明）。 */
          fs_progress_ping();
          /* 首帧 / 偶发对账：seq 从 0 起或每 32 页打一次。 */
          if ((g_diag_notify_sent & 31u) == 1u)
            {
              LOGI("fs notify ok cmd=0x%02x tid=%u seq=%u flen=%u inflight=%u",
                   (unsigned)g_fs.cmd, (unsigned)g_fs.tid,
                   (unsigned)(g_fs.seq > 0 ? g_fs.seq - 1 : 0),
                   (unsigned)flen, (unsigned)g_fs.inflight);
            }
        }
      else
        {
          break;
        }
    }

  busy = (g_fs.state != FS_IDLE) || g_fs.cmd_req.pending ||
         (g_fs.data_q_n > 0) || g_fs.commit_pending ||
         (need_any_pending_locked() && g_fs.connected && g_fs.ccc);
  {
    bool do_commit = g_fs.commit_pending;

    if (do_commit)
      {
        g_fs.commit_pending = false;
      }

    pthread_mutex_unlock(&g_fs.lock);
    if (do_commit)
      {
        fs_deferred_commit();
      }
  }

  /* 通知失败的日志放在这里（锁已放开）—— 见 send_tx_finish 的注释。 */
  if (notify_fail_ret != 0)
    {
      LOGE("fs notify failed: %d", notify_fail_ret);
    }

  return busy;
}

/**
 * @brief 请 App 把本地缓存的图标上传到设备的 notif_icons/。
 */
void companion_fs_need_file(const char *name)
{
  uint32_t now;
  int      i;
  int      free_i = -1;
  int      evict_i = -1;

  if (myvendor_is_factory() || !icon_name_ok(name))
    {
      return;
    }

  pthread_mutex_lock(&g_fs.lock);
  now = fs_now_ms();
  for (i = 0; i < FS_NEED_Q; i++)
    {
      if (g_fs.need[i].name[0] == '\0')
        {
          if (free_i < 0)
            {
              free_i = i;
            }

          continue;
        }

      if (strcmp(g_fs.need[i].name, name) == 0)
        {
          if (g_fs.need[i].pending)
            {
              pthread_mutex_unlock(&g_fs.lock);
              return;
            }

          if (g_fs.need[i].last_ms != 0 &&
              myvendor_mono_elapsed_ms(now, g_fs.need[i].last_ms) <
              FS_NEED_RETRY_MS)
            {
              pthread_mutex_unlock(&g_fs.lock);
              return;
            }

          g_fs.need[i].pending = true;
          pthread_mutex_unlock(&g_fs.lock);
          companion_fs_kick();
          return;
        }

      if (!g_fs.need[i].pending &&
          (evict_i < 0 ||
           g_fs.need[i].last_ms < g_fs.need[evict_i].last_ms))
        {
          evict_i = i;
        }
    }

  if (free_i < 0)
    {
      free_i = evict_i;
    }

  if (free_i < 0)
    {
      pthread_mutex_unlock(&g_fs.lock);
      return;
    }

  strncpy(g_fs.need[free_i].name, name, FS_NEED_NAME_MAX);
  g_fs.need[free_i].name[FS_NEED_NAME_MAX] = '\0';
  g_fs.need[free_i].pending = true;
  pthread_mutex_unlock(&g_fs.lock);
  companion_fs_kick();
}
