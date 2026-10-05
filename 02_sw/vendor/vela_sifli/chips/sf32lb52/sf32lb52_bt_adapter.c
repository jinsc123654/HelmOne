/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb52_bt_adapter.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

/**
 * @file sf32lb52_bt_adapter.c
 * @brief HCPU↔LCPU 的蓝牙控制器适配层（HCI 走 IPC 环 + wqueue 收包）。
 *
 * 板级补充（my_vendor 侧）：
 *   - `hci tx` / `hci rx` 是**命令级追踪**（配对看"哪条命令没有回条"），
 *     2026-09-25 起整族降为 DEBUG —— 常态不出，抬运行期掩码的 DEBUG 位（`ctl log`）
 *     即可复现；其中 `evt=0x13`（Number Of Completed Packets，~10 ms 一次的周期性
 *     流控回执）连 DEBUG 也不打；
 *   - 探针计数（`sf32lb52_bt_note_host_rx()` 等）只累加、不打日志，
 *     输出统一由 diag 线程负责（见 `myvendor_diag.c`）。
 */

#include <nuttx/config.h>
#include <fcntl.h>

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <nuttx/cache.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include "bf0_hal.h"
#include "circular_buf.h"
#include "ipc_hw.h"
#include "ipc_queue.h"
#include "mem_map.h"
#include "sf32lb52_bt_adapter.h"
#include "sf32lb52_bd_addr.h"

#define SF32LB52_BT_QID          0
/* Fixed HCPU SRAM top: CH2 0x2007FC00 + CH1 0x2007FE00 (2 x 512 B).
 * Hardware mailbox, not a malloc buffer. Must stay outside Umem
 * (SRAM_HEAP_END in sifli_allocateheap.c). BSP once mapped kumm to
 * SRAM_END; HCI TX into this ring corrupted the SRAM heap (not OOM).
 */
#define SF32LB52_BT_TX_BUF_SIZE  HCPU2LCPU_MB_CH1_BUF_SIZE
#define SF32LB52_BT_TX_BUF_ADDR  HCPU2LCPU_MB_CH1_BUF_START_ADDR
#define SF32LB52_BT_TX_BUF_ALIAS HCPU_ADDR_2_LCPU_ADDR(HCPU2LCPU_MB_CH1_BUF_START_ADDR)
#define SF32LB52_BT_RX_BUF_LEGACY \
  LCPU_ADDR_2_HCPU_ADDR(LCPU2HCPU_MB_CH1_BUF_START_ADDR)
#define SF32LB52_BT_RX_BUF_REV_B \
  LCPU_ADDR_2_HCPU_ADDR(LCPU2HCPU_MB_CH1_BUF_REV_B_START_ADDR)
#define SF32LB52_BT_RX_BUF_SIZE  LCPU2HCPU_MB_CH1_BUF_SIZE
#define SF32LB52_BT_RING_DATA_SIZE \
  ((SF32LB52_BT_RX_BUF_SIZE - sizeof(struct circular_buf)) & ~3UL)
#define SF32LB52_BT_NVDS_BUF_START 0x2040FE00
#define SF32LB52_BT_NVDS_BUF_SIZE  512
#define SF32LB52_BT_NVDS_PATTERN   0x4e564453
#define SF32LB52_BT_NVDS_TAG_BD    0x01
#define SF32LB52_BT_BD_ADDR_LEN    6
#define SF32LB52_BT_TRACE          0
#define SF32LB52_BT_H4_CMD         0x01
#define SF32LB52_BT_RX_FAIL_LOG_MS 1000u
#define SF32LB52_BT_RX_FAIL_GAP_MS 500u
#define SF32LB52_BT_RX_ENOMEM_BACKOFF_US 3000u

/** 上电后等控制器起来的**上限**（毫秒）；到点照常继续，不判失败。
 *
 *  8 s 不是拍的：SDK 2.4 自家 Zephyr 通路等栈就绪用的就是这个量级
 *  （`sf_port/zbt_hci.c` 里 `sifli_sem_take_ex(5000)`，而
 *  `SDK/2.4/CHANGELOG.md:478` 记着供应商把它上调到了 8 s）。
 *  见 `sf32lb52_bt_controller_enable()` 里的说明 —— 那里的循环一旦看到
 *  LCPU 往 RX 环写过东西就提前退出，所以正常路径**不会**真的等满 8 s。 */
#define SF32LB52_BT_READY_MAX_MS  8000u
#ifdef CONFIG_MYVENDOR_BLE_LOG
#  define BLE_LOG(fmt, ...) syslog(LOG_INFO, "ble: " fmt "\n", ##__VA_ARGS__)
#else
#  define BLE_LOG(...) ((void)0)
#endif

typedef enum
{
  SF32LB52_BT_STATUS_IDLE = 0,
  SF32LB52_BT_STATUS_INITED,
  SF32LB52_BT_STATUS_ENABLED,
} sf32lb52_bt_status_t;

struct sf32lb52_bt_env_s
{
  ipc_queue_handle_t ipc_port;
  uint8_t data_buf[SF32LB52_BT_RX_BUF_SIZE];
  sf32lb52_bt_rx_callback_t notify_host;
  struct work_s rx_work;
  bool queue_open;
  bool wake_held;
  volatile bool rx_work_pending;
  bool rx_worker_running;
  uint32_t rx_read_idx_mirror;
  uint32_t rx_count;
};

static struct sf32lb52_bt_env_s g_sf32lb52_bt_env;
static sf32lb52_bt_status_t g_sf32lb52_bt_status = SF32LB52_BT_STATUS_IDLE;
static volatile uint32_t g_rx_fail_first_ms;
static volatile uint32_t g_rx_fail_last_ms;
static volatile uint32_t g_rx_fail_n;
static uint32_t g_rx_fail_log_ms;
static volatile bool g_hci_skip_sync;
static volatile uint32_t g_hci_last_rx_ms;
static volatile uint32_t g_hci_last_tx_ms;
static volatile uint32_t g_hci_last_ok_tx_ms;
static volatile uint16_t g_hci_last_tx_op;
static volatile uint8_t g_hci_last_rx_evt;

/** 已发到 LCPU、还没等到事件（Command Complete / Status）的操作码，0 = 没有。
 *
 *  2026-09-18：`cmd_stalled()` 原来拿时间戳比大小（`last_rx < last_ok_tx`），
 *  但"发送成功"是 `wait_tx_idle()` 用 `usleep(1000)` 轮询确认 LCPU 取走字节
 *  后才盖章的 —— 控制器回 Command Complete 往往比这个盖章**早几毫秒**，
 *  于是 `last_rx < last_ok_tx` 对一条**已经被回答**的命令也成立，检测器把
 *  健康空闲当"控制器哑了"，4 s 后 diag 把适配器整个拆掉：实机就是
 *  22.6 s 一轮的拆装循环（广播能起、扫描起不来、手机永远连不上）。
 *  改成按操作码配对：只有"某条命令真的没回条"才算静默，空闲不算。
 */
static volatile uint16_t g_hci_pend_op;
static volatile uint32_t g_hci_pend_ms;
#ifdef CONFIG_MYVENDOR_BLE_LOG
static uint32_t g_send_skip_log_ms;
#endif

void sf32lb52_bt_hci_skip_sync_set(bool on)
{
  if (g_hci_skip_sync != on)
    {
      BLE_LOG("skip_sync %d -> %d", g_hci_skip_sync ? 1 : 0, on ? 1 : 0);
    }

  g_hci_skip_sync = on;
}

bool sf32lb52_bt_hci_skip_sync(void)
{
  return g_hci_skip_sync;
}
static uintptr_t g_sf32lb52_bt_rx_buf_addr;
static ipc_hw_q_handle_t g_sf32lb52_bt_tx_hw =
{
  .ch_id = SF32LB52_BT_QID / IPC_HW_QUEUE_NUM,
  .q_idx = SF32LB52_BT_QID % IPC_HW_QUEUE_NUM,
};

struct sf32lb52_bt_nvds_mem_init_s
{
  uint32_t pattern;
  uint16_t used_mem;
  uint16_t writting;
};

static const uint8_t g_sf32lb52_bt_nvds_default_rc10k[] =
{
  0x0d, 0x02, 0x64, 0x19, 0x12, 0x01, 0x01, 0x2f,
  0x04, 0x20, 0x00, 0x00, 0x00, 0x01, 0x06, 0x12,
  0x34, 0x56, 0x78, 0xab, 0xcd, 0x15, 0x01, 0x01
};

static const uint8_t g_sf32lb52_bt_nvds_default_lxt32k[] =
{
  0x2f, 0x04, 0x20, 0x00, 0x00, 0x00, 0x01, 0x06,
  0x12, 0x34, 0x56, 0x78, 0xab, 0xcd, 0x15, 0x01,
  0x01
};

extern uint8_t lcpu_power_on(void);
extern uint8_t lcpu_power_off(void);
extern void ipc_queue_data_ind(uint32_t user_data);

static uint32_t sf32lb52_bt_now_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

void sf32lb52_bt_hci_rx_stall_clear(void)
{
  g_rx_fail_first_ms = 0;
  g_rx_fail_last_ms = 0;
  g_rx_fail_n = 0;

  /* 适配器重建/显式清障时，"待完成"也要归零：否则上一代的残留会让新的
   * 一轮刚起来就带着一个假的待完成命令。 */
  g_hci_pend_op = 0;
  g_hci_pend_ms = 0;
}

bool sf32lb52_bt_hci_rx_stalled(uint32_t stall_ms)
{
  uint32_t first = g_rx_fail_first_ms;
  uint32_t last = g_rx_fail_last_ms;
  uint32_t now;

  if (first == 0 || stall_ms == 0)
    {
      return false;
    }

  now = sf32lb52_bt_now_ms();
  if ((int32_t)(now - last) > (int32_t)SF32LB52_BT_RX_FAIL_GAP_MS)
    {
      return false;
    }

  return (uint32_t)(now - first) >= stall_ms;
}

bool sf32lb52_bt_hci_cmd_stalled(uint32_t stall_ms)
{
  uint32_t pend_age;
  uint16_t pend;
  uint32_t now;

  if (stall_ms == 0 || g_hci_skip_sync || !g_sf32lb52_bt_env.queue_open)
    {
      return false;
    }

  /** 没有任何"已发出、还没回"的命令 → 主机只是没事可发（广播已经在跑、
   *  没人要扫描、也没有链路），**不是**控制器哑。原实现只看时间戳，
   *  这种健康空闲会一路走到 4 s 判死 → 拆适配器（见 g_hci_pend_op 注释）。
   */
  pend = g_hci_pend_op;
  if (pend == 0)
    {
      return false;
    }

  now = sf32lb52_bt_now_ms();
  pend_age = (uint32_t)(now - g_hci_pend_ms);
  return pend_age >= stall_ms;
}

static uintptr_t sf32lb52_bt_rx_buf_addr(void);
static size_t sf32lb52_bt_ring_data_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                        uint32_t buffer_size);
static size_t sf32lb52_bt_tx_pending(struct circular_buf *tx_ring,
                                     uint32_t *rd_ptr,
                                     uint32_t *wr_ptr);

/** RX 抽取链的"哪一环断了"计数器与判据（板级，2026-09-24）。
 *
 *  现场（那份 241 s 的 stall）：`rd` 与 `wr` **双双冻结**、`pend` 逼近满、`rxf=0`；
 *  按本文件自己的判据"`rd` 不动 ⇒ 主机侧抽取链断了"，而`wr` 后来也停是因为
 *  **环满了 LCPU 没窗口可写** ⇒ 上层看到的 `cmdsilent`（"控制器静默"）其实是**结果**。
 *
 *  这条链有四个环节，必须分开计数才能在日志里直接点名：
 *    邮箱中断 → `sf32lb52_bt_rx_ind()` → `work_queue(HPWORK, rx_work)` → worker 取走
 *      · `ind` 不涨（且 NVIC 那一位置着）⇒ **中断没来**；
 *      · `ind` 涨、`drain` 不涨       ⇒ **回调来了但 worker 没跑**（work/队列问题）；
 *      · 两者都涨、`pend` 不清        ⇒ 取走了但上层没收（`notify_host` 侧）。
 *  判到"断层"就**直接打印**，并做一次自愈（重开中断位 / 重排 worker / 再唤一次 LCPU）。
 *
 *  **打印归口**：唯一的调用点是 diag 线程的周期（`myvendor_diag.c`）——
 *  中断侧只计数，其它路径也不打印，这样"计数器打印"只有 diag 一个出口。
 */
#define SF32LB52_BT_RX_CHAIN_STALL_MS 1500u

static volatile uint32_t g_rx_ind_n;      /**< IPC 回调（邮箱中断路径）次数。 */
static volatile uint32_t g_rx_drain_n;    /**< worker 真正取走数据的次数。 */
static volatile uint32_t g_rx_rearm_n;    /**< 自愈动作次数。 */
static volatile uint32_t g_rx_stuck_n;    /**< "worker 挂着却无进展"的抢回次数。 */
static pid_t g_rx_worker_tid;             /**< HPWORK 线程 tid（worker 入口记下）。 */
static uint32_t g_rx_dump_ms;             /**< worker 栈转储限频（10 s）。 */
static uint32_t g_rx_dump_n;              /**< 已转储张数（一次卡死最多 3 张）。 */
static uint32_t g_rx_kick_ms;             /**< wqueue kick 限频（1 s）。 */
static uint32_t g_rx_kick_n;              /**< kick 次数（自愈是否真的动过手）。 */
static uint32_t g_rx_drain_last_ms;       /**< 最近一次真正 drain 的时刻（判进展用）。 */
static volatile uint32_t g_rx_queue_fail_n; /**< work_queue 失败次数（中断里只计数）。 */
static volatile uint32_t g_rx_preopen_n;  /**< 队列未开时丢掉的回调次数。 */
static uint32_t g_rx_chain_ok_ms;         /**< 最近一次"链路有动静"的时刻。 */
static uint32_t g_rx_chain_log_ms;        /**< 告警限频。 */
static uint32_t g_rx_qfail_seen;          /**< 上一次已上报的 work_queue 失败数。 */
static uint32_t g_rx_qfail_log_ms;        /**< 异常直报的限频。 */

void sf32lb52_bt_hci_dump_stall(void)
{
  uint32_t now = sf32lb52_bt_now_ms();
  uint32_t rx = g_hci_last_rx_ms;
  uint32_t tx = g_hci_last_tx_ms;

  syslog(LOG_ERR,
         "sf32lb52 bt stall skip=%d open=%d status=%d rxn=%lu "
         "txop=0x%04x rxevt=0x%02x rxage=%lu txage=%lu rxf=%lu "
         "pendop=0x%04x pendage=%lu\n",
         g_hci_skip_sync ? 1 : 0,
         g_sf32lb52_bt_env.queue_open ? 1 : 0,
         (int)g_sf32lb52_bt_status,
         (unsigned long)g_sf32lb52_bt_env.rx_count,
         (unsigned)g_hci_last_tx_op,
         (unsigned)g_hci_last_rx_evt,
         (unsigned long)(rx == 0 ? 0xfffffffful : (unsigned long)(now - rx)),
         (unsigned long)(tx == 0 ? 0xfffffffful : (unsigned long)(now - tx)),
         (unsigned long)g_rx_fail_n,
         (unsigned)g_hci_pend_op,
         (unsigned long)(g_hci_pend_op == 0
                             ? 0ul
                             : (unsigned long)(now - g_hci_pend_ms)));

  /** LCPU→host 环的指针：这是区分「LCPU 哑了」和「主机没在抽」的唯一判据。
   *
   *  - `wr` 一直在走、`rd` 不动 → LCPU 在写，是主机侧 RX 抽取链断了
   *    （`rx_worker_running` 闩死 / HPWORK 被占）；
   *  - `wr` 不动 → LCPU 根本没往环里写，控制器/IPC 侧的静默；
   *  - `pend` 是环里积压的字节数，配合 `rxage` 看。
   *
   *  `oktxage` 是"最后一条**成功发出**的 HCI 命令"到现在的毫秒数（与
   *  `txage` 不同：后者在尝试发送时就盖章，失败也算）。
   */
  {
    struct circular_buf *rx_ring =
        (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

    if (rx_ring != NULL)
      {
        uint32_t wr = rx_ring->write_idx_mirror;
        uint32_t rd = rx_ring->read_idx_mirror;

        syslog(LOG_ERR,
               "sf32lb52 bt ring wr=0x%08lx rd=0x%08lx pend=%lu size=%d "
               "oktxage=%lu\n",
               (unsigned long)wr,
               (unsigned long)rd,
               (unsigned long)sf32lb52_bt_ring_data_len(rd, wr,
                                                        rx_ring->buffer_size),
               (int)rx_ring->buffer_size,
               (unsigned long)(g_hci_last_ok_tx_ms == 0
                                   ? 0xfffffffful
                                   : (unsigned long)(now -
                                                     g_hci_last_ok_tx_ms)));
      }
  }

  /** 反方向的判据：TX 环里还有没有我们的字节没被 LCPU 取走。
   *
   *  - `pend==0` → LCPU 把命令读走了，只是不回事件（命令接口/固件的静默）；
   *  - `pend>0`  → LCPU 连 TX 环都不抽了（IPC 或 LCPU 整体停摆）。
   *  这两者要走的恢复动作不同，所以必须分开看。
   */
  {
    struct circular_buf *tx_ring =
        (struct circular_buf *)SF32LB52_BT_TX_BUF_ADDR;
    uint32_t t_rd = 0;
    uint32_t t_wr = 0;
    size_t t_pend;

    t_pend = sf32lb52_bt_tx_pending(tx_ring, &t_rd, &t_wr);
    syslog(LOG_ERR, "sf32lb52 bt txring wr=0x%08lx rd=0x%08lx pend=%lu\n",
           (unsigned long)t_wr, (unsigned long)t_rd, (unsigned long)t_pend);
  }
}

static void sf32lb52_bt_note_host_rx(int ret)
{
  uint32_t now;

  if (ret >= 0)
    {
      g_hci_last_rx_ms = sf32lb52_bt_now_ms();
      sf32lb52_bt_hci_rx_stall_clear();
      return;
    }

  now = sf32lb52_bt_now_ms();
  if (g_rx_fail_first_ms == 0)
    {
      g_rx_fail_first_ms = now;
      BLE_LOG("hci rx fail begin ret=%d skip=%d",
              ret, g_hci_skip_sync ? 1 : 0);
    }

  g_rx_fail_last_ms = now;
  g_rx_fail_n++;
  if (g_rx_fail_log_ms == 0 ||
      (uint32_t)(now - g_rx_fail_log_ms) >= SF32LB52_BT_RX_FAIL_LOG_MS)
    {
      g_rx_fail_log_ms = now;
      syslog(LOG_ERR,
             "sf32lb52 bt rx callback: %d (n=%lu stall=%lu ms skip=%d)\n",
             ret,
             (unsigned long)g_rx_fail_n,
             (unsigned long)(now - g_rx_fail_first_ms),
             g_hci_skip_sync ? 1 : 0);
#ifdef CONFIG_MYVENDOR_BLE_LOG
      BLE_LOG("hci rx stall n=%lu first=%lu last=%lu status=%d open=%d",
              (unsigned long)g_rx_fail_n,
              (unsigned long)g_rx_fail_first_ms,
              (unsigned long)g_rx_fail_last_ms,
              (int)g_sf32lb52_bt_status,
              g_sf32lb52_bt_env.queue_open ? 1 : 0);
#endif
    }
}

static uintptr_t sf32lb52_bt_rx_buf_addr(void)
{
  uint8_t rev_id;

  if (g_sf32lb52_bt_rx_buf_addr != 0)
    {
      return g_sf32lb52_bt_rx_buf_addr;
    }

  rev_id = __HAL_SYSCFG_GET_REVID();
  if (rev_id >= HAL_CHIP_REV_ID_A4)
    {
      g_sf32lb52_bt_rx_buf_addr = SF32LB52_BT_RX_BUF_REV_B;
    }
  else
    {
      g_sf32lb52_bt_rx_buf_addr = SF32LB52_BT_RX_BUF_LEGACY;
    }

  syslog(LOG_INFO, "sf32lb52 bt rx ring addr=0x%08lx rev=%u\n",
         (unsigned long)g_sf32lb52_bt_rx_buf_addr, rev_id);
  return g_sf32lb52_bt_rx_buf_addr;
}

/* ---- NVDS 落盘（2026-09-19）------------------------------------------------
 * 这块 512 B（0x2040FE00）是**控制器共享的非易失信箱**：BD_ADDR、射频校准，
 * 以及（如果它承载的话）配对/绑定记录。它固定在 SRAM 地址上（控制器按地址读写），
 * 所以我们**不搬地址**，只在外面套一层 KV：
 *   开机：先按住默认值 memset，再尝试用 /mnt/kv/bt_nvds.bin 覆盖（pattern 校验通过才用）；
 *   变化：写入点主动存一次 + 低频摘要巡检（控制器自己写的也能被逮到）。
 * 只在**内容真的变了**时写盘，空闲期几乎零写入。 */
#define SF32LB52_BT_NVDS_KV_PATH  "/mnt/kv/bt_nvds.bin"
#define SF32LB52_BT_NVDS_KV_MAGIC 0x564e5442u   /* "BTNV" */
#define SF32LB52_BT_NVDS_POLL_MS  5000u

struct sf32lb52_bt_nvds_kv_hdr_s
{
  uint32_t magic;
  uint32_t len;
  uint32_t crc;      /* 512 B 内容的 crc32（简单按字异或+移位，够用） */
  uint32_t pad;
};

static uint32_t g_nvds_last_crc;
static uint32_t g_nvds_poll_ms;

static uint32_t sf32lb52_bt_nvds_crc(const uint8_t *p, size_t n)
{
  uint32_t c = 0x811c9dc5u;
  size_t i;

  for (i = 0; i < n; i++)
    {
      c ^= p[i];
      c *= 16777619u;
    }
  return c;
}

static void sf32lb52_bt_nvds_kv_save(void)
{
  struct sf32lb52_bt_nvds_kv_hdr_s hdr;
  const uint8_t *blob = (const uint8_t *)SF32LB52_BT_NVDS_BUF_START;
  uint32_t crc = sf32lb52_bt_nvds_crc(blob, SF32LB52_BT_NVDS_BUF_SIZE);
  int fd;

  if (crc == g_nvds_last_crc)
    {
      return;                                    /* 没变就不写盘 */
    }

  hdr.magic = SF32LB52_BT_NVDS_KV_MAGIC;
  hdr.len   = SF32LB52_BT_NVDS_BUF_SIZE;
  hdr.crc   = crc;
  hdr.pad   = 0;

  fd = open(SF32LB52_BT_NVDS_KV_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      syslog(LOG_WARNING, "bt nvds: save open failed %d\n", errno);
      return;
    }

  if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)
      || write(fd, blob, SF32LB52_BT_NVDS_BUF_SIZE)
         != (ssize_t)SF32LB52_BT_NVDS_BUF_SIZE)
    {
      syslog(LOG_WARNING, "bt nvds: save write failed %d\n", errno);
    }
  else
    {
      g_nvds_last_crc = crc;
      syslog(LOG_NOTICE, "bt nvds: saved to kv (crc=%08lx)\n",
             (unsigned long)crc);
    }

  (void)close(fd);
}

/* ---- NVDS dump（`ctl bt nvds`）------------------------------------------------
 * 直接把 512 B 打出来（16 B/行，hex + ASCII）并报 KV 状态和 CRC。
 * 不做 tag 解析：这块是控制器私有的信箱，布局没有公开契约 —— 用"配对前后各 dump
 * 一次、对比 diff"来判断绑定/IRK 是否在其中，比按猜测的布局解析可靠。 */
int sf32lb52_bt_nvds_dump(void)
{
  const uint8_t *blob = (const uint8_t *)SF32LB52_BT_NVDS_BUF_START;
  unsigned off;

  printf("bt nvds: buf=%p size=%u crc=%08lx kv=%s\n",
         (const void *)blob, (unsigned)SF32LB52_BT_NVDS_BUF_SIZE,
         (unsigned long)sf32lb52_bt_nvds_crc(blob, SF32LB52_BT_NVDS_BUF_SIZE),
         access(SF32LB52_BT_NVDS_KV_PATH, F_OK) == 0 ? "present" : "absent");

  for (off = 0; off < SF32LB52_BT_NVDS_BUF_SIZE; off += 16u)
    {
      char asc[17];
      unsigned i;

      for (i = 0; i < 16u; i++)
        {
          const unsigned char c = blob[off + i];

          asc[i] = (c >= 0x20u && c < 0x7fu) ? (char)c : '.';
        }
      asc[16] = '\0';

      printf("  %03x  %02x%02x%02x%02x %02x%02x%02x%02x "
             "%02x%02x%02x%02x %02x%02x%02x%02x  %s\n",
             off,
             blob[off + 0], blob[off + 1], blob[off + 2], blob[off + 3],
             blob[off + 4], blob[off + 5], blob[off + 6], blob[off + 7],
             blob[off + 8], blob[off + 9], blob[off + 10], blob[off + 11],
             blob[off + 12], blob[off + 13], blob[off + 14], blob[off + 15],
             asc);
    }

  return 0;
}

/* 让 dump 之前先把 KV 与内存对齐（若控制器刚改过，顺手落盘）。 */
void sf32lb52_bt_nvds_sync(void)
{
  sf32lb52_bt_nvds_kv_save();
}

static void sf32lb52_bt_nvds_kv_load(void)
{
  struct sf32lb52_bt_nvds_kv_hdr_s hdr;
  uint8_t buf[SF32LB52_BT_NVDS_BUF_SIZE];
  int fd;
  uint32_t pat;

  fd = open(SF32LB52_BT_NVDS_KV_PATH, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_NOTICE, "bt nvds: no kv file, keep defaults\n");
      g_nvds_last_crc = sf32lb52_bt_nvds_crc(
          (const uint8_t *)SF32LB52_BT_NVDS_BUF_START,
          SF32LB52_BT_NVDS_BUF_SIZE);
      return;
    }

  if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)
      || hdr.magic != SF32LB52_BT_NVDS_KV_MAGIC
      || hdr.len != SF32LB52_BT_NVDS_BUF_SIZE
      || read(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
    {
      syslog(LOG_WARNING, "bt nvds: kv header/short read, keep defaults\n");
      (void)close(fd);
      g_nvds_last_crc = sf32lb52_bt_nvds_crc(
          (const uint8_t *)SF32LB52_BT_NVDS_BUF_START,
          SF32LB52_BT_NVDS_BUF_SIZE);
      return;
    }

  (void)close(fd);

  pat = *(const uint32_t *)buf;
  if (pat != SF32LB52_BT_NVDS_PATTERN
      || sf32lb52_bt_nvds_crc(buf, sizeof(buf)) != hdr.crc)
    {
      syslog(LOG_WARNING, "bt nvds: kv pattern/crc bad (pat=%08lx), defaults\n",
             (unsigned long)pat);
      g_nvds_last_crc = sf32lb52_bt_nvds_crc(
          (const uint8_t *)SF32LB52_BT_NVDS_BUF_START,
          SF32LB52_BT_NVDS_BUF_SIZE);
      return;
    }

  memcpy((void *)SF32LB52_BT_NVDS_BUF_START, buf, sizeof(buf));
  up_clean_dcache((uintptr_t)SF32LB52_BT_NVDS_BUF_START,
                  (uintptr_t)SF32LB52_BT_NVDS_BUF_START + sizeof(buf));
  g_nvds_last_crc = hdr.crc;
  syslog(LOG_NOTICE, "bt nvds: restored from kv (crc=%08lx)\n",
         (unsigned long)hdr.crc);
}

/** 低频巡检：控制器自己改了这块（配对/解绑）也能被逮到并落盘。 */
static void sf32lb52_bt_nvds_kv_poll(uint32_t now_ms)
{
  if (g_nvds_poll_ms != 0u
      && (int32_t)(now_ms - g_nvds_poll_ms) < (int32_t)SF32LB52_BT_NVDS_POLL_MS)
    {
      return;
    }

  g_nvds_poll_ms = now_ms ? now_ms : 1u;
  sf32lb52_bt_nvds_kv_save();
}

static void sf32lb52_bt_clean_nvds_shared(void)
{
  up_clean_dcache((uintptr_t)SF32LB52_BT_NVDS_BUF_START,
                  (uintptr_t)SF32LB52_BT_NVDS_BUF_START +
                  SF32LB52_BT_NVDS_BUF_SIZE);
}

static int sf32lb52_bt_bd_addr_from_uid(uint8_t addr[SF32LB52_BT_BD_ADDR_LEN])
{
  return sf32lb52_bd_addr_from_efuse(addr);
}

static void sf32lb52_bt_nvds_set_bd_addr(uint8_t *blob, size_t len,
                                         const uint8_t *addr)
{
  size_t i = 0;

  while (i + 2 <= len)
    {
      uint8_t tag = blob[i];
      uint8_t tlen = blob[i + 1];

      if (tag == 0xff || i + 2 + tlen > len)
        {
          break;
        }

      if (tag == SF32LB52_BT_NVDS_TAG_BD &&
          tlen == SF32LB52_BT_BD_ADDR_LEN)
        {
          memcpy(&blob[i + 2], addr, SF32LB52_BT_BD_ADDR_LEN);
          return;
        }

      i += 2 + tlen;
    }

  syslog(LOG_ERR, "sf32lb52 bt NVDS has no BD_ADDR tag\n");

  /** 2026-09-23（审计 F9j）：这里原来调 `sf32lb52_bt_nvds_kv_save()`，
   *  想表达"BD_ADDR 是我们自己写的，立刻落盘"—— 但它是错的：
   *
   *  - 本函数只改**本地的 blob 副本**，共享缓冲（SHARED）此刻还是**上一代的
   *    残留**；
   *  - 而 `kv_save()` 读的是 SHARED ⇒ 它把**陈旧的**内容写进 KV，并把
   *    `g_nvds_last_crc` 盖章成那个陈旧值；
   *  - 紧接着 `sf32lb52_bt_nvds_kv_load()` 又把这个刚写进去的陈旧文件
   *    **读回来**，于是"从 KV 恢复"变成了恢复一个寂寞。
   *
   *  落盘挪到真正写进 SHARED 之后（见 prepare_stack_nvds 末尾），
   *  语义才对：内容确实变了才写。 */
}

static bool sf32lb52_bt_rx_ring_valid(struct circular_buf *rx_ring)
{
  uint32_t rd_ptr = rx_ring->read_idx_mirror;
  uint32_t wr_ptr = rx_ring->write_idx_mirror;
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  long buf_size = rx_ring->buffer_size;

  return buf_size == (long)SF32LB52_BT_RING_DATA_SIZE &&
         rd_idx <= SF32LB52_BT_RING_DATA_SIZE &&
         wr_idx <= SF32LB52_BT_RING_DATA_SIZE;
}

static bool sf32lb52_bt_rx_ring_ready(const char *tag)
{
  struct circular_buf *rx_ring = (struct circular_buf *)sf32lb52_bt_rx_buf_addr();
  uint32_t rd_ptr = rx_ring->read_idx_mirror;
  uint32_t wr_ptr = rx_ring->write_idx_mirror;
  long buf_size = rx_ring->buffer_size;

  if (!sf32lb52_bt_rx_ring_valid(rx_ring))
    {
      syslog(LOG_WARNING,
             "%s rx ring invalid: buf=%ld expected=%lu rd=%08lx wr=%08lx\n",
             tag,
             buf_size,
             (unsigned long)SF32LB52_BT_RING_DATA_SIZE,
             (unsigned long)rd_ptr,
             (unsigned long)wr_ptr);
      return false;
    }

  return true;
}

static int sf32lb52_bt_wait_rx_ring_ready(void)
{
  struct circular_buf *rx_ring =
      (struct circular_buf *)sf32lb52_bt_rx_buf_addr();
  int i;

  for (i = 0; i < 1000; i++)
    {
      up_invalidate_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                           (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                           SF32LB52_BT_RX_BUF_SIZE);

      if (sf32lb52_bt_rx_ring_valid(rx_ring))
        {
          return OK;
        }

      usleep(1000);
    }

  sf32lb52_bt_rx_ring_ready("sf32lb52 wait");
  return -ETIMEDOUT;
}

static void sf32lb52_bt_prepare_stack_nvds(void)
{
  struct sf32lb52_bt_nvds_mem_init_s *nvds;
  const uint8_t *defaults;
  size_t defaults_len;
  uint8_t blob[sizeof(g_sf32lb52_bt_nvds_default_rc10k)];
  uint8_t mac[SF32LB52_BT_BD_ADDR_LEN];

  if (HAL_LXT_DISABLED())
    {
      defaults = g_sf32lb52_bt_nvds_default_rc10k;
      defaults_len = sizeof(g_sf32lb52_bt_nvds_default_rc10k);
    }
  else
    {
      defaults = g_sf32lb52_bt_nvds_default_lxt32k;
      defaults_len = sizeof(g_sf32lb52_bt_nvds_default_lxt32k);
    }

  memcpy(blob, defaults, defaults_len);
  if (sf32lb52_bt_bd_addr_from_uid(mac) == OK)
    {
      sf32lb52_bt_nvds_set_bd_addr(blob, defaults_len, mac);
    }
  else
    {
      syslog(LOG_WARNING,
             "sf32lb52 bt MAC: eFuse UID unusable, keeping NVDS default\n");
    }

  HAL_HPAON_WakeCore(CORE_ID_LCPU);

  nvds = (struct sf32lb52_bt_nvds_mem_init_s *)SF32LB52_BT_NVDS_BUF_START;
  memset((void *)SF32LB52_BT_NVDS_BUF_START, 0, SF32LB52_BT_NVDS_BUF_SIZE);
  sf32lb52_bt_nvds_kv_load();
  nvds->pattern = SF32LB52_BT_NVDS_PATTERN;
  nvds->used_mem = defaults_len;
  nvds->writting = 0;
  memcpy((void *)(nvds + 1), blob, defaults_len);
  sf32lb52_bt_clean_nvds_shared();

  /** BD_ADDR（来自 eFuse）是我们刚写进 SHARED 的 ⇒ 立刻落一次盘。
   *
   *  2026-09-23（审计 F9j）：这一步原来挂在 `set_bd_addr()` 里、跑在
   *  `kv_load()` **之前**，读到的还是上一代的 SHARED ⇒ 把陈旧内容写进 KV，
   *  随后 `kv_load()` 又把它读回来，"恢复"等于没恢复。挪到这里之后，
   *  判据回到 `kv_save()` 自己的语义：**内容真的变了才写**（它先比
   *  `g_nvds_last_crc`，而那个值是上面 `kv_load()` 刚盖的）。 */
  sf32lb52_bt_nvds_kv_save();

  HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
}

static size_t sf32lb52_bt_ring_data_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                        uint32_t buffer_size)
{
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  uint32_t rd_mirror = CB_GET_PTR_MIRROR(rd_ptr);
  uint32_t wr_mirror = CB_GET_PTR_MIRROR(wr_ptr);

  if (rd_idx == wr_idx)
    {
      return rd_mirror == wr_mirror ? 0 : buffer_size;
    }

  if (wr_idx > rd_idx)
    {
      return wr_idx - rd_idx;
    }

  return buffer_size - (rd_idx - wr_idx);
}

static size_t sf32lb52_bt_ring_space_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                         uint32_t buffer_size)
{
  return buffer_size - sf32lb52_bt_ring_data_len(rd_ptr, wr_ptr,
                                                buffer_size);
}

static uint32_t sf32lb52_bt_ring_advance(uint32_t ptr, size_t len,
                                         uint32_t buffer_size)
{
  uint32_t idx = CB_GET_PTR_IDX(ptr);
  uint32_t mirror = CB_GET_PTR_MIRROR(ptr);

  idx += len;
  if (idx >= buffer_size)
    {
      idx -= buffer_size;
      mirror = ~mirror;
    }

  return CB_MAKE_PTR_IDX_MIRROR(idx, mirror);
}

static size_t sf32lb52_bt_ring_copy(uint8_t *dst,
                                    const struct circular_buf *rx_ring,
                                    uint32_t rd_ptr,
                                    size_t len)
{
  const uint8_t *pool = (const uint8_t *)(rx_ring + 1);
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  size_t tail;

  if (len == 0)
    {
      return 0;
    }

  tail = rx_ring->buffer_size - rd_idx;
  if (tail >= len)
    {
      memcpy(dst, &pool[rd_idx], len);
      return len;
    }

  memcpy(dst, &pool[rd_idx], tail);
  memcpy(&dst[tail], pool, len - tail);
  return len;
}

static size_t sf32lb52_bt_ring_write(struct circular_buf *tx_ring,
                                     const uint8_t *src, size_t len)
{
  uint8_t *pool = (uint8_t *)(tx_ring + 1);
  uint32_t wr_ptr = tx_ring->write_idx_mirror;
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  size_t space;
  size_t tail;

  space = sf32lb52_bt_ring_space_len(tx_ring->read_idx_mirror,
                                     wr_ptr,
                                     tx_ring->buffer_size);
  if (space == 0)
    {
      return 0;
    }

  if (len > space)
    {
      len = space;
    }

  tail = tx_ring->buffer_size - wr_idx;
  if (tail >= len)
    {
      memcpy(&pool[wr_idx], src, len);
      up_clean_dcache((uintptr_t)&pool[wr_idx],
                      (uintptr_t)&pool[wr_idx] + len);
    }
  else
    {
      memcpy(&pool[wr_idx], src, tail);
      memcpy(pool, &src[tail], len - tail);
      up_clean_dcache((uintptr_t)&pool[wr_idx],
                      (uintptr_t)&pool[wr_idx] + tail);
      up_clean_dcache((uintptr_t)pool,
                      (uintptr_t)pool + len - tail);
    }

  tx_ring->write_idx_mirror = sf32lb52_bt_ring_advance(wr_ptr, len,
                                                       tx_ring->buffer_size);
  up_clean_dcache((uintptr_t)tx_ring,
                  (uintptr_t)tx_ring + sizeof(*tx_ring));
  __DSB();

  return len;
}

static size_t sf32lb52_bt_tx_pending(struct circular_buf *tx_ring,
                                     uint32_t *rd_ptr,
                                     uint32_t *wr_ptr)
{
  uint32_t rd;
  uint32_t wr;

  up_invalidate_dcache((uintptr_t)SF32LB52_BT_TX_BUF_ADDR,
                       (uintptr_t)SF32LB52_BT_TX_BUF_ADDR +
                       sizeof(*tx_ring));

  rd = tx_ring->read_idx_mirror;
  wr = tx_ring->write_idx_mirror;

  if (rd_ptr != NULL)
    {
      *rd_ptr = rd;
    }

  if (wr_ptr != NULL)
    {
      *wr_ptr = wr;
    }

  return sf32lb52_bt_ring_data_len(rd, wr, tx_ring->buffer_size);
}

static void sf32lb52_bt_trigger_tx(void)
{
  __DSB();
  ipc_hw_trigger_interrupt(&g_sf32lb52_bt_tx_hw);
}

static int sf32lb52_bt_wait_tx_idle(struct circular_buf *tx_ring)
{
  uint32_t start_time = HAL_GetTick();
  uint32_t tick_count = 0;
  uint32_t rd_ptr = 0;
  uint32_t wr_ptr = 0;

  while (sf32lb52_bt_tx_pending(tx_ring, &rd_ptr, &wr_ptr) > 0)
    {
      sf32lb52_bt_trigger_tx();

      if (HAL_GetTick() != start_time)
        {
          tick_count++;
          start_time = HAL_GetTick();
        }

      if (tick_count >= 100)
        {
          syslog(LOG_ERR,
                 "sf32lb52 bt tx busy: rd=%08lx wr=%08lx\n",
                 (unsigned long)rd_ptr,
                 (unsigned long)wr_ptr);
          return -ETIMEDOUT;
        }

      usleep(1000);
    }

  return OK;
}

#if SF32LB52_BT_TRACE
static void sf32lb52_bt_log_tx_state(struct circular_buf *tx_ring,
                                     const char *tag,
                                     uint32_t target_wr)
{
  uint32_t rd_ptr = 0;
  uint32_t wr_ptr = 0;
  size_t pending;

  pending = sf32lb52_bt_tx_pending(tx_ring, &rd_ptr, &wr_ptr);
  syslog(LOG_INFO,
         "sf32lb52 bt tx %s: target=%08lx rd=%08lx wr=%08lx pending=%lu\n",
         tag,
         (unsigned long)target_wr,
         (unsigned long)rd_ptr,
         (unsigned long)wr_ptr,
         (unsigned long)pending);
}
#endif

static size_t sf32lb52_bt_tx_chunk_len(const uint8_t *data,
                                       size_t len,
                                       size_t offset)
{
  size_t remaining = len - offset;

  if (offset == 0 && remaining > 1)
    {
      return 1;
    }

  return remaining;
}

static void sf32lb52_bt_rx_worker(FAR void *arg)
{
  struct sf32lb52_bt_env_s *env = arg;

  /* 记下 HPWORK 线程自己的 tid：worker 就跑在它上面。卡死（`running=1 avl=0`、
   * drain 不涨）时 stall 报告要顺着这个 tid 把它的调用栈拍下来 —— 堵在哪一行
   * 只有栈能回答（同 `board_restart_ble_companion()` 里对 companion 的做法）。 */
  g_rx_worker_tid = nxsched_gettid();

  for (;;)
    {
      irqstate_t flags;
      int empty_retries = 0;

      for (;;)
        {
          size_t size;
          size_t read_len;
          int ret;
          struct circular_buf *rx_ring;
          uint32_t wr_ptr;

          flags = enter_critical_section();

          if (!env->queue_open || env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
            {
              env->rx_work_pending = false;
              env->rx_worker_running = false;
              leave_critical_section(flags);
              return;
            }

          leave_critical_section(flags);

          up_invalidate_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                               (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                               SF32LB52_BT_RX_BUF_SIZE);

          flags = enter_critical_section();

          if (!env->queue_open || env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
            {
              env->rx_work_pending = false;
              env->rx_worker_running = false;
              leave_critical_section(flags);
              return;
            }

          rx_ring = (struct circular_buf *)sf32lb52_bt_rx_buf_addr();
          wr_ptr = rx_ring->write_idx_mirror;
          size = sf32lb52_bt_ring_data_len(env->rx_read_idx_mirror,
                                          wr_ptr,
                                          rx_ring->buffer_size);
          if (size == 0)
            {
              uint32_t rd_ptr = rx_ring->read_idx_mirror;
              uint32_t local_rd = env->rx_read_idx_mirror;

              leave_critical_section(flags);

              if (empty_retries++ < 20)
                {
                  usleep(1000);
                  continue;
                }

          #if SF32LB52_BT_TRACE
              syslog(LOG_INFO,
                "sf32lb52 bt rx empty: local=%08lx rd=%08lx wr=%08lx\n",
                (unsigned long)local_rd,
                (unsigned long)rd_ptr,
                (unsigned long)wr_ptr);
          #endif
              break;
            }

          empty_retries = 0;

          if (size > sizeof(env->data_buf))
            {
              size = sizeof(env->data_buf);
            }

          read_len = sf32lb52_bt_ring_copy(env->data_buf, rx_ring,
                                           env->rx_read_idx_mirror, size);
          if (read_len == 0)
            {
              leave_critical_section(flags);
              break;
            }

          env->rx_read_idx_mirror = sf32lb52_bt_ring_advance(
              env->rx_read_idx_mirror, read_len, rx_ring->buffer_size);
          rx_ring->read_idx_mirror = env->rx_read_idx_mirror;
          __DSB();

          up_clean_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                          (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                          sizeof(*rx_ring));

          env->rx_count++;

          leave_critical_section(flags);

          /* 链路计数：worker 确实取走了数据（与 rx_ind 分开，用于区分哪一环断）。 */
          g_rx_drain_n++;
          g_rx_drain_last_ms = sf32lb52_bt_now_ms();
          g_rx_chain_ok_ms = sf32lb52_bt_now_ms();

        #if SF32LB52_BT_TRACE
             syslog(LOG_INFO,
               "sf32lb52 bt rx drain: len=%lu local=%08lx wr=%08lx\n",
               (unsigned long)read_len,
               (unsigned long)env->rx_read_idx_mirror,
               (unsigned long)wr_ptr);
        #endif

          if (env->notify_host == NULL)
            {
              continue;
            }

          if (read_len >= 2 && env->data_buf[0] == 0x04)
            {
              g_hci_last_rx_evt = env->data_buf[1];

              /* 待完成配对：Command Complete(0x0e) 与 Command Status(0x0f)
               * 都算"控制器活着并且理了这条命令"。参数布局不同：
               *   0x0e: [2]=plen [3]=ncmd [4..5]=opcode
               *   0x0f: [2]=plen [3]=status [4]=ncmd [5..6]=opcode
               */
              if (g_hci_pend_op != 0)
                {
                  if (g_hci_last_rx_evt == 0x0e && read_len >= 6 &&
                      ((uint16_t)env->data_buf[4] |
                       ((uint16_t)env->data_buf[5] << 8)) == g_hci_pend_op)
                    {
                      g_hci_pend_op = 0;
                    }
                  else if (g_hci_last_rx_evt == 0x0f && read_len >= 7 &&
                           ((uint16_t)env->data_buf[5] |
                            ((uint16_t)env->data_buf[6] << 8)) ==
                           g_hci_pend_op)
                    {
                      g_hci_pend_op = 0;
                    }
                }

              /* 事件级追踪：与 `hci tx` 配对，就能看出"哪条命令没有回条"。
               * 2026-09-25：整族 hci tx/rx 追踪降到 DEBUG（它们是配对排查用的测试日志，
               * 常态下没价值）。要看时把运行期掩码的 DEBUG 位打开（`ctl log`），
               * 不用重编；默认掩码里没有 DEBUG 位，所以常态一行都不出。 */
              if (g_hci_last_rx_evt == 0x0e && read_len >= 6)
                {
                  syslog(LOG_DEBUG,
                         "hci rx evt=0x0e ncmd=%u op=0x%04x len=%lu\n",
                         (unsigned)env->data_buf[3],
                         (unsigned)((uint16_t)env->data_buf[4] |
                                    ((uint16_t)env->data_buf[5] << 8)),
                         (unsigned long)read_len);
                }
              else if (g_hci_last_rx_evt != 0x13)
                {
                  /* 同族追踪 ⇒ DEBUG（见上）。0x13 = Number Of Completed Packets 更狠：
                   * 控制器每 ~10 ms 一次，是周期流控回执，**连打开 DEBUG 时也不打**
                   * （删掉本条件即可恢复）。 */
                  syslog(LOG_DEBUG, "hci rx evt=0x%02x len=%lu\n",
                         (unsigned)g_hci_last_rx_evt, (unsigned long)read_len);
                }
            }

          ret = env->notify_host(env->data_buf, read_len);
          sf32lb52_bt_note_host_rx(ret);
          if (ret < 0)
            {
              /* 不要在 HPWORK 上空转：会饿死 H4 RX / GNSS。 */
              usleep(SF32LB52_BT_RX_ENOMEM_BACKOFF_US);
              flags = enter_critical_section();
              if (!env->queue_open)
                {
                  env->rx_work_pending = false;
                  env->rx_worker_running = false;
                  leave_critical_section(flags);
                  return;
                }

              leave_critical_section(flags);
            }
        }

      flags = enter_critical_section();
      if (!env->rx_work_pending)
        {
          env->rx_worker_running = false;
          leave_critical_section(flags);
          break;
        }

      env->rx_work_pending = false;
      leave_critical_section(flags);
    }
}

static int32_t sf32lb52_bt_rx_ind(ipc_queue_handle_t handle, size_t size)
{
  struct sf32lb52_bt_env_s *env = &g_sf32lb52_bt_env;
  irqstate_t flags;
  bool queue_work;

  if (handle != env->ipc_port)
    {
      return -EINVAL;
    }

  if (!env->queue_open)
    {
      /* 中断上下文里**只计数、不打印**（队列还没开时丢掉的回调）。
       * 这个数会在外侧的 `rx chain stalled` 行里报出来。 */
      g_rx_preopen_n++;
      return OK;
    }

  flags = enter_critical_section();
  env->rx_work_pending = true;
  queue_work = !env->rx_worker_running && work_available(&env->rx_work);
  if (queue_work)
    {
      env->rx_worker_running = true;
    }
  leave_critical_section(flags);

  /* 链路计数：回调确实来过（IRQ/回调这一环的活证；配合 drain 判"断在哪一环"）。 */
  g_rx_ind_n++;
  g_rx_chain_ok_ms = sf32lb52_bt_now_ms();

#if SF32LB52_BT_TRACE
  /* trace 也遵守"中断里不打印"（本回调由邮箱中断路径进入）。 */
  if (!up_interrupt_context())
    {
      struct circular_buf *rx_ring =
          (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

      up_invalidate_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                           (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                           sizeof(*rx_ring));

      syslog(LOG_INFO,
             "sf32lb52 rx_ind: size=%lu local=%08lx rd=%08lx wr=%08lx\n",
             (unsigned long)size,
             (unsigned long)env->rx_read_idx_mirror,
             (unsigned long)rx_ring->read_idx_mirror,
             (unsigned long)rx_ring->write_idx_mirror);
    }
#endif

  if (queue_work)
    {
      int ret;

      ret = work_queue(HPWORK, &env->rx_work, sf32lb52_bt_rx_worker,
                       env, 0);
      if (ret < 0)
        {
          flags = enter_critical_section();
          env->rx_worker_running = false;
          leave_critical_section(flags);

          /* 中断上下文里只计数；失败详情由外侧的 `rx chain stalled` 行报出。 */
          g_rx_queue_fail_n++;
          return ret;
        }
    }

  return OK;
}

/** @brief 判"RX 抽取链断在哪一环"：直接打印 + 一次自愈尝试。
 *
 *  **只由 diag 线程调用**（`myvendor_diag.c` 的周期里）—— 中断侧只计数，
 *  这里是全工程唯一的打印出口；入口的 `up_interrupt_context()` 是保险，
 *  防止以后有人从别的上下文调进来时在中断里 syslog。
 *
 *  只在"环里有积压、且最近 SF32LB52_BT_RX_CHAIN_STALL_MS 内回调与 worker 都没动静"
 *  时才动作（限频 1 次/秒），所以正常运行时一次都不会触发。
 *
 *  `force=false`：异常判定 + 自愈（diag 每拍都调，保证恢复能力与开关无关）。
 *  `force=true` ：**无条件**打一行计数器（diag 的周期报表用，见 `ctl cnt`）。
 *
 *  自愈按三个环节各补一脚（都是幂等的）：
 *    ① NVIC 里 LCPU→HCPU 邮箱中断没开 → 重开（否则再怎么说话我们也收不到）；
 *    ② worker 没在跑且 work 不在队 → 重排一次；
 *    ③ 再唤一次 LCPU（`HAL_HPAON_WakeCore`）——LPSYS 总线桥没上电时，
 *       连"解蔽邮箱中断"这类写都会落空，这一脚就是给它补电的。
 */
void sf32lb52_bt_rx_chain_report(bool force)
{
  struct circular_buf *rx_ring;
  uint32_t now;
  size_t pend;
  uint32_t idle;
  bool nvic_off;
  bool stalled;

  now = sf32lb52_bt_now_ms();

  /** **中断上下文里绝不打印**（2026-09-24 定规）：本函数只从 diag 线程调用，
   *  这里再加一道保险 —— 万一以后有人从 ISR 调进来，也只会读几个寄存器就返回。 */
  if (up_interrupt_context())
    {
      return;
    }

  /* —— 异常直报（每拍跑、**与 `ctl cnt` 开关无关**；见 myvendor_diag.c 的约定）——
   *
   *  `work_queue()` 失败 = RX 抽取这一拍排不上队，是"链断"的最早信号；
   *  计数在中断侧累加（`g_rx_queue_fail_n`），这里发现增长就 `LOG_ERR` 直报，
   *  限频 1 s。用 ERROR 级：即使把 syslog 屏蔽位收紧到只留错误，它也看得见。 */
  if (g_rx_queue_fail_n != g_rx_qfail_seen)
    {
      uint32_t d = g_rx_queue_fail_n - g_rx_qfail_seen;

      g_rx_qfail_seen = g_rx_queue_fail_n;

      if (g_rx_qfail_log_ms == 0u ||
          (int32_t)(now - g_rx_qfail_log_ms) >= 1000)
        {
          g_rx_qfail_log_ms = now ? now : 1u;
          syslog(LOG_ERR,
                 "sf32lb52 bt rx chain: ERROR work_queue failed %lu time(s) "
                 "(total %lu) — RX 抽取可能停摆\n",
                 (unsigned long)d, (unsigned long)g_rx_queue_fail_n);
        }
    }

  rx_ring = (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

  /** ⚠ **队列没开就别看、更别动手**（2026-09-24 现场教训）。
   *
   *  开机头几秒 `queue_open == 0`：此刻 IPC 环的 `wr/rd` 镜像**还是残值**
   *  （实测 `pend=9597 rd=0x551a048d wr=0x7a97b8e2`），照它判会得到"环里有积压"
   *  的假象；更糟的是自愈会在 **LCPU 正在启动**的窗口里去 re-enable 邮箱中断、
   *  调 `HAL_HPAON_WakeCore()` —— 实测那几次之后 `HCI_RESET` 永不应答
   *  （`pendop=0x0c03`、`rxn=0`）⇒ BLE 整场起不来。
   *
   *  所以：**只有队列已开**且（`rx_ind` 至少来过一次或 `pend` 看起来是正常量级）
   *  才进入判定。队列没开就当作"链路尚未启用"，直接返回。 */
  if (!g_sf32lb52_bt_env.queue_open || rx_ring == NULL)
    {
      return;
    }

  up_invalidate_dcache((uintptr_t)rx_ring,
                       (uintptr_t)rx_ring + sizeof(*rx_ring));

  pend = sf32lb52_bt_ring_data_len(rx_ring->read_idx_mirror,
                                  rx_ring->write_idx_mirror,
                                  rx_ring->buffer_size);

  idle = now - g_rx_chain_ok_ms;
  stalled = ((int32_t)(now - g_rx_chain_ok_ms) >=
             (int32_t)SF32LB52_BT_RX_CHAIN_STALL_MS);

  if (pend == 0)
    {
      g_rx_chain_ok_ms = now;
    }

  if (!force)
    {
      if (pend == 0 || !stalled)
        {
          return;
        }

      if (g_rx_chain_log_ms != 0u &&
          (int32_t)(now - g_rx_chain_log_ms) < 1000)
        {
          return;
        }
    }

  g_rx_chain_log_ms = now ? now : 1u;

  nvic_off = ((NVIC->ISER[LCPU2HCPU_IRQn >> 5] & (1UL << (LCPU2HCPU_IRQn & 0x1F)))
              == 0u);

  /* —— 打印：`force` 是周期报表（正常也打），否则只在判到断层时打 —— */
  if (!force)
    {
      syslog(LOG_ERR,
             "sf32lb52 bt rx chain stalled: pend=%lu rd=0x%08lx wr=0x%08lx "
             "ind=%lu drain=%lu rearm=%lu qfail=%lu preopen=%lu "
             "pending=%d running=%d avl=%d irq_en=%d idle=%lu ms%s%s\n",
             (unsigned long)pend,
             (unsigned long)rx_ring->read_idx_mirror,
             (unsigned long)rx_ring->write_idx_mirror,
             (unsigned long)g_rx_ind_n, (unsigned long)g_rx_drain_n,
             (unsigned long)g_rx_rearm_n,
             (unsigned long)g_rx_queue_fail_n, (unsigned long)g_rx_preopen_n,
             g_sf32lb52_bt_env.rx_work_pending ? 1 : 0,
             g_sf32lb52_bt_env.rx_worker_running ? 1 : 0,
             work_available(&g_sf32lb52_bt_env.rx_work) ? 1 : 0,
             nvic_off ? 0 : 1,
             (unsigned long)idle,
             nvic_off ? " [邮箱中断没开 ⇒ 收不到]" : "",
             (g_rx_drain_n == 0u) ? " [drain 从没成功过 ⇒ worker 没跑起来]" : "");
    }
  else
    {
      syslog(LOG_INFO,
             "sf32lb52 bt rx chain cnt: pend=%lu ind=%lu drain=%lu rearm=%lu "
             "qfail=%lu preopen=%lu pending=%d running=%d avl=%d irq_en=%d "
             "rxage=%lu ms%s\n",
             (unsigned long)pend,
             (unsigned long)g_rx_ind_n, (unsigned long)g_rx_drain_n,
             (unsigned long)g_rx_rearm_n,
             (unsigned long)g_rx_queue_fail_n, (unsigned long)g_rx_preopen_n,
             g_sf32lb52_bt_env.rx_work_pending ? 1 : 0,
             g_sf32lb52_bt_env.rx_worker_running ? 1 : 0,
             work_available(&g_sf32lb52_bt_env.rx_work) ? 1 : 0,
             nvic_off ? 0 : 1,
             (unsigned long)(now - g_hci_last_rx_ms),
             nvic_off ? " [邮箱中断没开 ⇒ 收不到]" : "");
    }

  /* 自愈只在"真判到断层"时做（周期报表不触发动作）。 */
  if (!stalled)
    {
      return;
    }

  /* —— 卡死现场取证：把 HPWORK 线程的调用栈拍下来 ——
   *
   * `pending=1 running=1 avl=0` 只说明"work 项还挂在队列里、有人挂着"，到底是
   * 空转、还是堵在 `notify_host`（zblue 收包路径）里的某个信号量上，只有栈能答。
   * 限频 10 s、一次卡死最多 3 张：第一张给位置，第二张给"它真的没动"的对照，
   * 之后就不再刷屏（卡死是长事件，不缺样本）。 */
  if (g_rx_dump_n < 3u &&
      (g_rx_dump_ms == 0u || (int32_t)(now - g_rx_dump_ms) >= 10000))
    {
      g_rx_dump_ms = now ? now : 1u;
      g_rx_dump_n++;

      if (g_rx_worker_tid > 0)
        {
          syslog(LOG_ERR,
                 "sf32lb52 bt rx chain: worker stack dump tid=%d "
                 "pending=%d running=%d avl=%d (#%lu)\n",
                 (int)g_rx_worker_tid,
                 g_sf32lb52_bt_env.rx_work_pending ? 1 : 0,
                 g_sf32lb52_bt_env.rx_worker_running ? 1 : 0,
                 work_available(&g_sf32lb52_bt_env.rx_work) ? 1 : 0,
                 (unsigned long)g_rx_dump_n);
#ifdef CONFIG_SCHED_BACKTRACE
          /* 自己逐行打，不用 `sched_dumpstack()`：那个走 `_alert`(ALERT 级)，
           * 打出来没有 `[ ERROR]` 记号，只能靠落盘通道"续行继承上一行判定"
           * 这条规则搭车 —— 顺序一变就整段丢。**现场（不带电脑）取证要求
           * 每行自带级别记号**，所以这里每行都经 `syslog(LOG_ERR, …)`，
           * 格式与 `sched_dumpstack` 同量级（一帧一个 `%p`，每行 8 帧）。 */
          {
            int skip;

            for (skip = 0; skip < 4 * 8; skip += 8)
              {
                FAR void *frames[8];
                char line[8 * 11 + 1];
                size_t used = 0;
                int n;
                int i;

                n = sched_backtrace(g_rx_worker_tid, frames, 8, skip);
                if (n <= 0)
                  {
                    break;
                  }

                line[0] = '\0';
                for (i = 0; i < n; i++)
                  {
                    int w = snprintf(line + used, sizeof(line) - used,
                                     " %p", frames[i]);

                    if (w <= 0 || (size_t)w >= sizeof(line) - used)
                      {
                        break;
                      }

                    used += (size_t)w;
                  }

                syslog(LOG_ERR, "sf32lb52 bt rx chain stack[%d]:%s\n",
                       skip, line);

                if (n < 8)
                  {
                    break;
                  }
              }
          }
#endif
        }
      else
        {
          syslog(LOG_ERR,
                 "sf32lb52 bt rx chain: worker 一次都没跑起来（tid=0），无栈可拍\n");
        }
    }

  /* —— 自愈 ①：中断位 —— */
  if (nvic_off)
    {
      up_enable_irq(LCPU2HCPU_IRQn);
      g_rx_rearm_n++;
      syslog(LOG_ERR, "sf32lb52 bt rx chain: re-enabled LCPU2HCPU IRQ %d (#%lu)\n",
             (int)LCPU2HCPU_IRQn, (unsigned long)g_rx_rearm_n);
    }

  /* —— 自愈 ②：**无条件重排（kick）** ——
   *
   *  判据从"看标志"改成"**看进展**"（2026-09-24 现场教训），2026-09-27 又往前
   *  一步：**不再要求 `work_available()`**。
   *
   *  那天板上抓到的现场（栈解出 + `ps` 佐证）：worker 就是 pid1 `hpwork`、
   *  状态 `Waiting Semaphore`，栈是 `nxsched_switch ← nxsem_wait_slow ←
   *  nxsem_wait`；而我们的 work 项**还挂在队列里**（`pending=1 running=1
   *  avl=0`，`rearm` 恒 0）—— 形状是"活在那儿、没人叫醒 worker"（丢唤醒）。
   *  唤醒本来靠 wqueue 自己的定时器（`wd_start_abstick(&wqueue->timer, …,
   *  work_timer_expired, wqueue)` ⇒ `nxsem_post(&wqueue->sem)`），而那个定时器
   *  节点正是同一秒内核报"看门狗活动链表损坏"时被踩坏/裁掉的那批之一。
   *
   *  所以判据必须是"**有活却长时间没进展**"，而不是"项不在队里"：
   *  `work_queue()` 对已入队的项做的是 `work_remove()` + 重插 + **重新 arm
   *  队列定时器**（`work_queue_start()` 里的 `retimer` 分支）⇒ 这一脚既补唤醒、
   *  也顺手把丢掉的定时器装回去，两条路都通。限频 1 s。 */
  if ((int32_t)(now - g_rx_kick_ms) >= 1000)
    {
      int ret;

      g_rx_kick_ms = now ? now : 1u;
      g_rx_kick_n++;

      /* `stuck` = worker 记着"跑着"但 drain 很久没进展 —— 就是 2026-09-24 那族。 */
      if ((g_rx_drain_n == 0u) ||
          ((int32_t)(now - g_rx_drain_last_ms) >=
           (int32_t)(SF32LB52_BT_RX_CHAIN_STALL_MS * 2u)))
        {
          g_rx_stuck_n++;
        }

      /* 记账与"项已在队列里"一致：中断侧 rx_ind 见到 running=1 就不会再插一次。 */
      g_sf32lb52_bt_env.rx_work_pending = true;
      g_sf32lb52_bt_env.rx_worker_running = true;

      ret = work_queue(HPWORK, &g_sf32lb52_bt_env.rx_work,
                       sf32lb52_bt_rx_worker, &g_sf32lb52_bt_env, 0);

      syslog(LOG_ERR,
             "sf32lb52 bt rx chain: kick wqueue (avl=%d running=%d "
             "drain 无进展 %lu ms, ret=%d #%lu)\n",
             work_available(&g_sf32lb52_bt_env.rx_work) ? 1 : 0,
             g_sf32lb52_bt_env.rx_worker_running ? 1 : 0,
             (unsigned long)(now - g_rx_drain_last_ms), ret,
             (unsigned long)g_rx_kick_n);
    }

  /** —— 自愈 ③：**只在我们没按着 LCPU 醒着**时补一脚唤醒 ——
   *
   *  `HAL_HPAON_WakeCore()` 原本是无条件调的，但开机那一小段（LCPU 正在引导、
   *  `wake_held == false`）重复唤醒会干扰它起栈 —— 现场实测：连调 6 次之后
   *  `HCI_RESET` 永不应答（`pendop=0x0c03`）、BLE 整场起不来。
   *  正常运行时 `wake_held == true`，这一脚本来就不需要。 */
  if (!g_sf32lb52_bt_env.wake_held)
    {
      HAL_HPAON_WakeCore(CORE_ID_LCPU);
    }

  /* 记账：给自愈留一个复查窗口，避免 1 s 内反复刷。 */
  g_rx_chain_ok_ms = now;
}

static int sf32lb52_bt_mailbox_init(void)
{
  struct sf32lb52_bt_env_s *env = &g_sf32lb52_bt_env;
  ipc_queue_cfg_t q_cfg;

  memset(&q_cfg, 0, sizeof(q_cfg));
  q_cfg.qid = SF32LB52_BT_QID;
  q_cfg.tx_buf_size = SF32LB52_BT_TX_BUF_SIZE;
  q_cfg.tx_buf_addr = SF32LB52_BT_TX_BUF_ADDR;
  q_cfg.tx_buf_addr_alias = SF32LB52_BT_TX_BUF_ALIAS;
  q_cfg.rx_buf_addr = sf32lb52_bt_rx_buf_addr();
  q_cfg.rx_ind = sf32lb52_bt_rx_ind;

  env->ipc_port = ipc_queue_init(&q_cfg);
  if (env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
    {
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "sf32lb52 bt tx ring addr=0x%08lx size=%u\n",
         (unsigned long)SF32LB52_BT_TX_BUF_ADDR,
         (unsigned)SF32LB52_BT_TX_BUF_SIZE);
  return OK;
}

int sf32lb52_bt_controller_init(void)
{
  int ret;

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_IDLE)
    {
      return OK;
    }

  memset(&g_sf32lb52_bt_env, 0, sizeof(g_sf32lb52_bt_env));
  ret = sf32lb52_bt_mailbox_init();
  if (ret < 0)
    {
      return ret;
    }

  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_INITED;
  return OK;
}

int sf32lb52_bt_controller_deinit(void)
{
  int ret;

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_ENABLED)
    {
      ret = sf32lb52_bt_controller_disable();
      if (ret < 0)
        {
          return ret;
        }
    }

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_INITED)
    {
      return -EPERM;
    }

  g_sf32lb52_bt_env.queue_open = false;
  g_sf32lb52_bt_env.rx_work_pending = false;
  g_sf32lb52_bt_env.rx_worker_running = false;
  if (!work_available(&g_sf32lb52_bt_env.rx_work))
    {
      ret = work_cancel_sync(HPWORK, &g_sf32lb52_bt_env.rx_work);
      if (ret < 0 && ret != -ENOENT)
        {
          return ret;
        }
    }

  ret = ipc_queue_deinit(g_sf32lb52_bt_env.ipc_port);
  if (ret < 0)
    {
      return ret;
    }

  memset(&g_sf32lb52_bt_env, 0, sizeof(g_sf32lb52_bt_env));
  g_sf32lb52_bt_env.ipc_port = IPC_QUEUE_INVALID_HANDLE;
  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_IDLE;
  return OK;
}

int sf32lb52_hci_register_callback(sf32lb52_bt_rx_callback_t callback)
{
  if (callback == NULL)
    {
      return -EINVAL;
    }

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_IDLE)
    {
      return -EPERM;
    }

  g_sf32lb52_bt_env.notify_host = callback;
  return OK;
}

int sf32lb52_bt_controller_enable(void)
{
  int ret;

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_ENABLED)
    {
      return OK;
    }

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_INITED)
    {
      return -EPERM;
    }

  sf32lb52_bt_prepare_stack_nvds();
  HAL_LCPU_ASSERT_INFO_clear();

  ret = lcpu_power_on();
  if (ret != 0)
    {
      return -EIO;
    }

  /* The L2H_MAILBOX (0x40002000) is on the LPSYS APB bus.  Its CxIER
   * register is only writable when the LPSYS bus bridge is active.
   * Wake the LCPU first so the bus bridge is powered, THEN open the
   * IPC queue (which writes CxIER to unmask the mailbox RX interrupt).
   * The SDK reference ipc_hw_enable_interrupt2() has the same
   * HAL_HPAON_WakeCore() before the UNMASK call.
   */

  HAL_HPAON_WakeCore(CORE_ID_LCPU);
  g_sf32lb52_bt_env.wake_held = true;

  /** 上电后等控制器"起来"再开队列。
   *
   *  2026-09-23（审计 F3）：这一段原来是**光秃秃的 `usleep(500000)`** —— 一个
   *  固定睡眠，睡醒就往下走，**没有任何"控制器活了"的判据**。对照 SDK 2.4 的
   *  自家 Zephyr 通路（`middleware/bluetooth/zephyr_bt/sf_port/zbt_hci.c:4829-4842`）：
   *  它是 `bluetooth_init(); ble_power_on();` 之后**等一个"栈就绪"信号量**
   *  （有界超时，超时只记日志、照常继续），而且 `SDK/2.4/CHANGELOG.md:478` 记着
   *  SiFli 自己把这个超时**从 5 s 上调到 8 s**（"Increase stack ready wait time
   *  from 5s to 8s"）—— 说明"等 500 ms 就走"在供应商看来是不够的。
   *
   *  我们在这一层拿不到那个 data-svc 信号量（`BSP_USING_DATA_SVC` 是本树要单独
   *  确认的），所以这里先用**可观测的替代判据**：RX 环头部自洽（LCPU 已经把
   *  `buffer_size` 写好），并且尽可能看到 LCPU 真的往环里写过东西。
   *
   *  ⚠ 这是**先补观测、不冒进**的版本：500 ms 的下限原样保留（正常路径不加
   *  延时），只是把上限从 1 s 提到 `SF32LB52_BT_READY_MAX_MS`（8 s，对齐 SDK
   *  的上调后值），并把实测耗时打出来。有这行日志，"LCPU 到底多久才活"就有
   *  数据了，下一轮再按实数收紧上限 —— 不凭空猜一个更大的常量。
   */

  {
    uint32_t t0 = sf32lb52_bt_now_ms();
    uint32_t waited = 0;
    bool     saw_write = false;

    usleep(500000);

    while (waited < SF32LB52_BT_READY_MAX_MS)
      {
        struct circular_buf *ring =
            (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

        up_invalidate_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                             (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                             SF32LB52_BT_RX_BUF_SIZE);

        if (sf32lb52_bt_rx_ring_valid(ring))
          {
            /* LCPU 往环里写过东西 = 它的 BLE 固件已经在跑（上电后它会先写
             * 一串启动响应，见下面那段读指针对齐的注释）。 */
            if (ring->write_idx_mirror != 0u)
              {
                saw_write = true;
                break;
              }
          }

        usleep(20000);
        waited += 20u;
      }

    syslog(LOG_INFO,
           "sf32lb52 bt ready: settle=%lu ms ctrl_alive=%d (cap=%u ms)\n",
           (unsigned long)(sf32lb52_bt_now_ms() - t0),
           saw_write ? 1 : 0,
           (unsigned)SF32LB52_BT_READY_MAX_MS);
  }

  ret = ipc_queue_open(g_sf32lb52_bt_env.ipc_port);
  if (ret < 0)
    {
      HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
      g_sf32lb52_bt_env.wake_held = false;
      return ret;
    }

  /* Flush H2L TX ring to SRAM immediately after ipc_queue_open so
   * LCPU sees the reset indices (read_idx=write_idx=0) before it
   * tries to process any stale data from a previous session.
   */
  up_clean_dcache((uintptr_t)SF32LB52_BT_TX_BUF_ADDR,
                  (uintptr_t)SF32LB52_BT_TX_BUF_ADDR +
                  SF32LB52_BT_TX_BUF_SIZE);
  __DSB();

  /* After every lcpu_power_on(), LCPU resets its TX ring write pointer
   * (write_idx_mirror) to 0 and writes its own boot responses into the
   * ring.  HCPU's read pointer (read_idx_mirror) may still hold the
   * non-zero value from the previous session -- dirty in DCache or
   * stored in SRAM.  If we do not synchronise them:
   *
   *   circular_buf_data_len() = wrap(write_idx - read_idx)
   *
   * returns a large garbage value, and LCPU's fresh boot events
   * plus ring garbage all replay as "new" HCI responses.  These
   * ghost events are consumed by the host as replies to real commands,
   * so HCI_Reset (0x0c01) is never actually executed by the controller,
   * and bt_le_adv_start later gets "Command Disallowed" (0x07).
   *
   * Fix: flush DCache to write back any dirty read_idx and to load
   * LCPU's current write_idx from SRAM, then advance read_idx to
   * write_idx (discard ALL pending LCPU boot data), then clean the
   * DCache so LCPU sees the updated read pointer in SRAM.
   */

  {
    struct circular_buf *rx_ring =
        (struct circular_buf *)sf32lb52_bt_rx_buf_addr();

    ret = sf32lb52_bt_wait_rx_ring_ready();
    if (ret < 0)
      {
        syslog(LOG_ERR, "sf32lb52 rx ring not ready: %d\n", ret);
        ipc_queue_close(g_sf32lb52_bt_env.ipc_port);
        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
        g_sf32lb52_bt_env.wake_held = false;
        return ret;
      }

    rx_ring->read_idx_mirror = rx_ring->write_idx_mirror;
    g_sf32lb52_bt_env.rx_read_idx_mirror = rx_ring->write_idx_mirror;

    up_clean_dcache((uintptr_t)sf32lb52_bt_rx_buf_addr(),
                    (uintptr_t)sf32lb52_bt_rx_buf_addr() +
                    sizeof(*rx_ring));
    __DSB();
  }

  /* 原本这里是一段**算完就丢**的死代码（算 l2h / nvic_bit / nvic_en 再
   * `(void)` 掉，2026-09-23 审计记为 F9i）。既然值都读出来了就让它有用：
   * `ipc_queue_open()` 本该把 LCPU→HCPU 的邮箱中断在 NVIC 里放开；**没放开
   * 的话控制器再怎么说话我们也收不到**，而现象是"RX 环里明明有字节、主机
   * 一动不动"—— 这类静默最难查，正好用一行日志把它钉住。
   *
   * 只告警不判失败：中断没开仍可能靠轮询工作（本文件就有 tx 侧主动触发），
   * 不该因此在 enable 路径上把一个可用的适配器判死。 */
  {
    uint32_t nvic_bit = (1UL << (LCPU2HCPU_IRQn & 0x1F));
    uint32_t nvic_en  = NVIC->ISER[LCPU2HCPU_IRQn >> 5];

    if ((nvic_en & nvic_bit) == 0u)
      {
        syslog(LOG_WARNING,
               "sf32lb52 bt: LCPU2HCPU IRQ %d not enabled after ipc_queue_open "
               "(ISER=0x%08lx) - HCI RX depends on polling\n",
               (int)LCPU2HCPU_IRQn, (unsigned long)nvic_en);
      }
  }

  g_sf32lb52_bt_env.queue_open = true;
  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_ENABLED;

  return OK;
}

int sf32lb52_bt_controller_disable(void)
{
  int ret = OK;
  int tmpret;
  bool queue_open;

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_ENABLED)
    {
      return -EPERM;
    }

  queue_open = g_sf32lb52_bt_env.queue_open;
  g_sf32lb52_bt_env.queue_open = false;
  g_sf32lb52_bt_env.rx_work_pending = false;
  g_sf32lb52_bt_env.rx_worker_running = false;
  if (!work_available(&g_sf32lb52_bt_env.rx_work))
    {
      uint32_t t0 = sf32lb52_bt_now_ms();

      while (!work_available(&g_sf32lb52_bt_env.rx_work) &&
             (uint32_t)(sf32lb52_bt_now_ms() - t0) < 250u)
        {
          usleep(2000);
        }

      if (!work_available(&g_sf32lb52_bt_env.rx_work))
        {
          syslog(LOG_WARNING, "sf32lb52 bt rx worker still busy, async cancel\n");
          (void)work_cancel(HPWORK, &g_sf32lb52_bt_env.rx_work);
        }
    }

  if (queue_open)
    {
      tmpret = ipc_queue_close(g_sf32lb52_bt_env.ipc_port);
      if (tmpret < 0 && ret == OK)
        {
          ret = tmpret;
        }
    }

  if (g_sf32lb52_bt_env.wake_held)
    {
      HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
      g_sf32lb52_bt_env.wake_held = false;
    }

  tmpret = lcpu_power_off();
  if (tmpret != 0 && ret == OK)
    {
      ret = -EIO;
    }

  g_sf32lb52_bt_env.notify_host = NULL;
  g_sf32lb52_bt_status = SF32LB52_BT_STATUS_INITED;
  return ret;
}

int sf32lb52_bt_controller_force_reset(void)
{
  sf32lb52_bt_rx_callback_t cb;
  int ret = OK;
  int en = -EIO;
  bool keep_skip;
  bool tried_enable;

  cb = g_sf32lb52_bt_env.notify_host;

  /** skip_sync 只在"控制器确实还活着"时才继承。
   *
   *  板级修正（2026-09-24，现场 08:52）：
   *    `force reset` 原先无条件 `keep_skip = g_hci_skip_sync`，于是"BLE 死亡族
   *    已经把 skip 置成 1"再撞上"status 已降到 IDLE"这一路时：
   *      - `tried_enable = (status == INITED)` 为假 ⇒ **enable 根本没被调用**；
   *      - `if (!keep_skip)` 也为假 ⇒ **skip 被永久留下**。
   *    后果：`sf32lb52_host_send_packet()` 对一切 HCI 返回 -ENODEV ⇒ adapter 永远
   *    到不了 ON ⇒ respawn 的 companion 停在 `bluetooth_create_instance failed`，
   *    diag 记到 `ble restart … n=86`，**只有断电冷启能救**（热复位救不回来）。
   *
   *  skip 的持有者只可能是"一次成功的 init/enable"，IDLE 时没有任何持有者 ⇒
   *  这时保 skip 就是自锁。下面 `if (!keep_skip)` 会把它清掉，落到"发不出去"
   *  而不是"永远发不出去"（与那段注释的原意一致）。 */
  keep_skip = g_hci_skip_sync &&
              (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_IDLE);

  syslog(LOG_ERR, "sf32lb52 bt force reset (LCPU)\n");
  BLE_LOG("force_reset begin keep_skip=%d status=%d open=%d cb=%d",
          keep_skip ? 1 : 0,
          (int)g_sf32lb52_bt_status,
          g_sf32lb52_bt_env.queue_open ? 1 : 0,
          cb != NULL ? 1 : 0);
  sf32lb52_bt_hci_skip_sync_set(true);
  sf32lb52_bt_hci_rx_stall_clear();

  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_ENABLED)
    {
      ret = sf32lb52_bt_controller_disable();
    }

  usleep(20000);

  /** 板级修正（2026-09-24）：**IDLE 也要真的把它拉起来**。
   *
   *  原来只有 `status == INITED` 才 enable ⇒ IDLE（例如被 `…_deinit()` 降到 IDLE、
   *  或死亡族中途退出）时这一路什么都不做，只留下"enable skipped (keep skip)"，
   *  阶梯便再也回不去。`init()` 是幂等的，失败只是返回错误、下一轮再试，
   *  所以这里放心调 —— 这正是 `force reset` 这个名字该做的事。 */
  if (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_IDLE)
    {
      int ini = sf32lb52_bt_controller_init();

      BLE_LOG("force_reset: re-init from IDLE -> %d (status now %d)",
              ini, (int)g_sf32lb52_bt_status);

      if (ini < 0 && ret == OK)
        {
          ret = ini;
        }
    }

  /* 记下 enable 到底有没有被调用：下面"跳过 enable"那一栏靠它区分
   * 「enable 失败」和「根本没人可 enable」。 */
  tried_enable = (g_sf32lb52_bt_status == SF32LB52_BT_STATUS_INITED);

  if (tried_enable)
    {
      en = sf32lb52_bt_controller_enable();

      if (en < 0 && ret == OK)
        {
          ret = en;
        }
    }

  /** skip_sync 必须**无条件**还回去（除非调用方明确要求保持）。
   *
   *  旧写法是 `if (en >= 0 && !keep_skip)`，于是"enable 压根没被调用"
   *  这条路 —— `status` 既不是 ENABLED 也不是 INITED（例如已被
   *  `sf32lb52_bt_controller_deinit()` 降到 IDLE），`en` 一直是初值 -EIO ——
   *  就会把 skip 永远留着。
   *
   *  **skip=1 的后果是致命的**：`sf32lb52_host_send_packet()` 对一切 HCI
   *  返回 -ENODEV，上层 adapter 永远到不了 ON，之后 respawn 出来的
   *  companion 停在 `adapter not ready, state=0` /
   *  `bt_gatts_register_service failed: 2`，再也没有人去清它（判据就是
   *  "LCPU 复位成功也不要清 skip" 那条注释所依赖的前提 —— 那前提只在
   *  enable 真的跑过时成立）。
   *
   *  挡住误发的其实是 `host_send_packet()` 里的 `status != ENABLED ||
   *  !queue_open` 两道判据，不需要 skip 来兜底：这里清掉，复位失败也只是
   *  回到"发不出去"，不会是"永远发不出去"。
   */
  if (!keep_skip)
    {
      sf32lb52_bt_hci_skip_sync_set(false);
    }

  if (!tried_enable)
    {
      syslog(LOG_ERR,
             "sf32lb52 bt force reset: status=%d (init 未能带起来), enable skipped%s\n",
             (int)g_sf32lb52_bt_status,
             keep_skip ? " (keep skip)" : " (skip cleared)");
    }
  else if (en < 0)
    {
      syslog(LOG_ERR, "sf32lb52 bt force reset: enable failed %d\n", en);
    }

  if (cb != NULL)
    {
      g_sf32lb52_bt_env.notify_host = cb;
    }

  BLE_LOG("force_reset done keep_skip=%d tried_enable=%d enable=%d ret=%d "
          "skip=%d status=%d",
          keep_skip ? 1 : 0, tried_enable ? 1 : 0, en, ret,
          g_hci_skip_sync ? 1 : 0, (int)g_sf32lb52_bt_status);
  return ret;
}

int sf32lb52_host_send_packet(const uint8_t *data, uint16_t len)
{
  struct circular_buf *tx_ring =
      (struct circular_buf *)SF32LB52_BT_TX_BUF_ADDR;
  uint32_t start_time;
  uint32_t tick_count;
  size_t written;
  size_t remaining;
  size_t offset;
  size_t chunk;
  int ret;

  if (data == NULL || len == 0)
    {
      return -EINVAL;
    }

  if (g_sf32lb52_bt_status != SF32LB52_BT_STATUS_ENABLED ||
      !g_sf32lb52_bt_env.queue_open ||
      g_hci_skip_sync)
    {
#ifdef CONFIG_MYVENDOR_BLE_LOG
      uint32_t now = sf32lb52_bt_now_ms();

      if (g_send_skip_log_ms == 0 ||
          (uint32_t)(now - g_send_skip_log_ms) >= 1000u)
        {
          g_send_skip_log_ms = now;
          BLE_LOG("host_send skip status=%d open=%d skip_sync=%d len=%u",
                  (int)g_sf32lb52_bt_status,
                  g_sf32lb52_bt_env.queue_open ? 1 : 0,
                  g_hci_skip_sync ? 1 : 0,
                  (unsigned)len);
        }
#endif
      return -ENODEV;
    }

  if (data[0] == SF32LB52_BT_H4_CMD && len >= 3)
    {
      g_hci_last_tx_op = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
      g_hci_last_tx_ms = sf32lb52_bt_now_ms();

      /* 待完成必须在这里置位（**入环时刻**），不能等下面 wait_tx_idle 确认
       * 之后再置：那是 usleep(1000) 轮询 LCPU 取走字节，控制器回 Command
       * Complete 往往比它**早**（实机 2026-09-18：tx 18.132 / rx 18.140，
       * 而我们 18.14x 才盖章），于是"置位"发生在"清零"之后，配对永远配不上，
       * 一条已被回答的命令被当成"发了没人应"。 */
      if (g_hci_last_tx_op != 0)
        {
          g_hci_pend_op = g_hci_last_tx_op;
          g_hci_pend_ms = g_hci_last_tx_ms;
        }

      /* 命令级追踪：排查"发出去没人应"时，必须知道**是哪一条**命令、
       * 参数是什么（同一个 opcode 的 enable=0/1 行为完全不同）。
       * 2026-09-25：与 hci rx 同族 ⇒ 一并降到 DEBUG（常态不出，`ctl log` 打开掩码
       * 的 DEBUG 位即可复现这套配对追踪）。 */
      syslog(LOG_DEBUG, "hci tx op=0x%04x plen=%u p=%02x%02x%02x%02x len=%u\n",
             (unsigned)g_hci_last_tx_op,
             len >= 4 ? (unsigned)data[3] : 0u,
             len > 4 ? (unsigned)data[4] : 0u,
             len > 5 ? (unsigned)data[5] : 0u,
             len > 6 ? (unsigned)data[6] : 0u,
             len > 7 ? (unsigned)data[7] : 0u,
             (unsigned)len);
    }

  offset = 0;
  remaining = len;
  start_time = HAL_GetTick();
  tick_count = 0;

  ret = sf32lb52_bt_wait_tx_idle(tx_ring);
  if (ret < 0)
    {
      return ret;
    }

  while (remaining > 0)
    {
      irqstate_t flags;

      chunk = sf32lb52_bt_tx_chunk_len(data, len, offset);

      up_invalidate_dcache((uintptr_t)SF32LB52_BT_TX_BUF_ADDR,
                           (uintptr_t)SF32LB52_BT_TX_BUF_ADDR +
                           sizeof(*tx_ring));

      flags = enter_critical_section();
      written = sf32lb52_bt_ring_write(tx_ring, data + offset, chunk);
      leave_critical_section(flags);

      if (written == 0)
        {
          if (HAL_GetTick() != start_time)
            {
              tick_count++;
              start_time = HAL_GetTick();
            }

          if (tick_count >= 10)
            {
              syslog(LOG_ERR,
                     "sf32lb52 bt tx timeout: remaining=%lu\n",
                     (unsigned long)remaining);
              return -ETIMEDOUT;
            }

          continue;
        }

      offset += written;
      remaining -= written;

      __DSB();
      sf32lb52_bt_trigger_tx();
    }

#if SF32LB52_BT_TRACE
  sf32lb52_bt_log_tx_state(tx_ring, "queued", tx_ring->write_idx_mirror);
#endif

  if (data[0] == SF32LB52_BT_H4_CMD)
    {
      ret = sf32lb52_bt_wait_tx_idle(tx_ring);
      if (ret < 0)
        {
          return ret;
        }

      /* 命令确实进了环（wait_tx_idle 确认被 LCPU 取走）才盖章。
       * 只用于 `oktxage` 展示；"控制器是否哑"的判据是上面的 pendop 配对
       * （时间戳盖章会输给控制器的回包速度，见那里的注释）。 */
      g_hci_last_ok_tx_ms = sf32lb52_bt_now_ms();
    }

  return OK;
}
