/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb52_bth4.c
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

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <debug.h>

#include <nuttx/wireless/bluetooth/bt_hci.h>
#include <nuttx/serial/uart_bth4.h>
#include <nuttx/wireless/bluetooth/bt_driver.h>
#include <nuttx/wireless/bluetooth/bt_uart.h>

#include "sf32lb52_bt_adapter.h"
#include "myvendor_mono.h"

#define SF32LB52_BT_H4_RX_BUFSIZE 2048
#define SF32LB52_BT_TRACE         0
#define SF32LB52_HCI_SCO_HDR_SIZE 3

#ifndef BT_HCI_OP_READ_SUPPORTED_COMMANDS
#  define BT_HCI_OP_READ_SUPPORTED_COMMANDS BT_OP(BT_OGF_INFO, 0x0002)
#endif

#ifndef BT_HCI_OP_READ_LOCAL_EXT_FEATURES
#  define BT_HCI_OP_READ_LOCAL_EXT_FEATURES BT_OP(BT_OGF_INFO, 0x0004)
#endif

#ifndef BT_HCI_OP_READ_BUFFER_SIZE
#  define BT_HCI_OP_READ_BUFFER_SIZE        BT_OP(BT_OGF_INFO, 0x0005)
#endif

#ifndef BT_HCI_OP_WRITE_DEFAULT_LINK_POLICY_SETTINGS
#  define BT_HCI_OP_WRITE_DEFAULT_LINK_POLICY_SETTINGS BT_OP(BT_OGF_LINK_POLICY, 0x000f)
#endif

#ifndef BT_HCI_OP_SET_EVENT_MASK
#  define BT_HCI_OP_SET_EVENT_MASK          BT_OP(BT_OGF_BASEBAND, 0x0001)
#endif

#ifndef BT_HCI_OP_WRITE_LOCAL_NAME
#  define BT_HCI_OP_WRITE_LOCAL_NAME        BT_OP(BT_OGF_BASEBAND, 0x0013)
#endif

#ifndef BT_HCI_OP_WRITE_PAGE_TIMEOUT
#  define BT_HCI_OP_WRITE_PAGE_TIMEOUT      BT_OP(BT_OGF_BASEBAND, 0x0018)
#endif

#ifndef BT_HCI_OP_WRITE_SCAN_ENABLE
#  define BT_HCI_OP_WRITE_SCAN_ENABLE       BT_OP(BT_OGF_BASEBAND, 0x001a)
#endif

#ifndef BT_HCI_OP_WRITE_PAGE_SCAN_ACTIVITY
#  define BT_HCI_OP_WRITE_PAGE_SCAN_ACTIVITY BT_OP(BT_OGF_BASEBAND, 0x001c)
#endif

#ifndef BT_HCI_OP_WRITE_INQUIRY_SCAN_ACTIVITY
#  define BT_HCI_OP_WRITE_INQUIRY_SCAN_ACTIVITY BT_OP(BT_OGF_BASEBAND, 0x001e)
#endif

#ifndef BT_HCI_OP_WRITE_CLASS_OF_DEVICE
#  define BT_HCI_OP_WRITE_CLASS_OF_DEVICE   BT_OP(BT_OGF_BASEBAND, 0x0024)
#endif

#ifndef BT_HCI_OP_WRITE_INQUIRY_SCAN_TYPE
#  define BT_HCI_OP_WRITE_INQUIRY_SCAN_TYPE BT_OP(BT_OGF_BASEBAND, 0x0043)
#endif

#ifndef BT_HCI_OP_WRITE_EXTENDED_INQUIRY_RESPONSE
#  define BT_HCI_OP_WRITE_EXTENDED_INQUIRY_RESPONSE BT_OP(BT_OGF_BASEBAND, 0x0052)
#endif

#ifndef BT_HCI_OP_WRITE_INQUIRY_MODE
#  define BT_HCI_OP_WRITE_INQUIRY_MODE      BT_OP(BT_OGF_BASEBAND, 0x0045)
#endif

#ifndef BT_HCI_OP_WRITE_PAGE_SCAN_TYPE
#  define BT_HCI_OP_WRITE_PAGE_SCAN_TYPE    BT_OP(BT_OGF_BASEBAND, 0x0047)
#endif

#ifndef BT_HCI_OP_WRITE_SSP_MODE
#  define BT_HCI_OP_WRITE_SSP_MODE          BT_OP(BT_OGF_BASEBAND, 0x0056)
#endif

#ifndef BT_HCI_OP_SET_EVENT_MASK_PAGE_2
#  define BT_HCI_OP_SET_EVENT_MASK_PAGE_2   BT_OP(BT_OGF_BASEBAND, 0x0063)
#endif

#ifndef BT_HCI_OP_WRITE_SC_HOST_SUPP
#  define BT_HCI_OP_WRITE_SC_HOST_SUPP      BT_OP(BT_OGF_BASEBAND, 0x007a)
#endif

#ifndef BT_HCI_OP_ACCEPT_CONN_REQ
#  define BT_HCI_OP_ACCEPT_CONN_REQ         BT_OP(BT_OGF_LINK_CTRL, 0x0009)
#endif

#ifndef BT_HCI_OP_LINK_KEY_REPLY
#  define BT_HCI_OP_LINK_KEY_REPLY          BT_OP(BT_OGF_LINK_CTRL, 0x000b)
#endif

#ifndef BT_HCI_OP_LINK_KEY_NEG_REPLY
#  define BT_HCI_OP_LINK_KEY_NEG_REPLY      BT_OP(BT_OGF_LINK_CTRL, 0x000c)
#endif

#ifndef BT_HCI_OP_AUTH_REQUESTED
#  define BT_HCI_OP_AUTH_REQUESTED          BT_OP(BT_OGF_LINK_CTRL, 0x0011)
#endif

#ifndef BT_HCI_OP_SET_CONN_ENCRYPT
#  define BT_HCI_OP_SET_CONN_ENCRYPT        BT_OP(BT_OGF_LINK_CTRL, 0x0013)
#endif

#ifndef BT_HCI_OP_IO_CAPABILITY_REPLY
#  define BT_HCI_OP_IO_CAPABILITY_REPLY     BT_OP(BT_OGF_LINK_CTRL, 0x002b)
#endif

#ifndef BT_HCI_OP_USER_CONFIRM_REPLY
#  define BT_HCI_OP_USER_CONFIRM_REPLY      BT_OP(BT_OGF_LINK_CTRL, 0x002c)
#endif

#ifndef BT_HCI_OP_USER_CONFIRM_NEG_REPLY
#  define BT_HCI_OP_USER_CONFIRM_NEG_REPLY  BT_OP(BT_OGF_LINK_CTRL, 0x002d)
#endif

#ifndef BT_HCI_OP_LE_SET_EVENT_MASK
#  define BT_HCI_OP_LE_SET_EVENT_MASK       BT_OP(BT_OGF_LE, 0x0001)
#endif

#ifndef BT_HCI_OP_LE_CONN_UPDATE
#  define BT_HCI_OP_LE_CONN_UPDATE          BT_OP(BT_OGF_LE, 0x0013)
#endif

#ifndef BT_HCI_OP_LE_READ_SUPP_STATES
#  define BT_HCI_OP_LE_READ_SUPP_STATES     BT_OP(BT_OGF_LE, 0x001c)
#endif

#ifndef BT_HCI_OP_LE_READ_LOCAL_FEATURES
#  define BT_HCI_OP_LE_READ_LOCAL_FEATURES  BT_OP(BT_OGF_LE, 0x0003)
#endif

#ifndef BT_HCI_OP_LE_WRITE_DEFAULT_DATA_LEN
#  define BT_HCI_OP_LE_WRITE_DEFAULT_DATA_LEN BT_OP(BT_OGF_LE, 0x0024)
#endif

#ifndef BT_HCI_OP_LE_READ_RL_SIZE
#  define BT_HCI_OP_LE_READ_RL_SIZE         BT_OP(BT_OGF_LE, 0x002a)
#endif

#ifndef BT_HCI_OP_LE_SET_RPA_TIMEOUT
#  define BT_HCI_OP_LE_SET_RPA_TIMEOUT      BT_OP(BT_OGF_LE, 0x002e)
#endif

#ifndef BT_HCI_OP_LE_READ_MAX_DATA_LEN
#  define BT_HCI_OP_LE_READ_MAX_DATA_LEN    BT_OP(BT_OGF_LE, 0x002f)
#endif

#ifndef BT_HCI_OP_LE_READ_MAX_ADV_DATA_LEN
#  define BT_HCI_OP_LE_READ_MAX_ADV_DATA_LEN BT_OP(BT_OGF_LE, 0x003a)
#endif

#ifndef BT_HCI_OP_LE_SET_HOST_FEATURE
#  define BT_HCI_OP_LE_SET_HOST_FEATURE     BT_OP(BT_OGF_LE, 0x0074)
#endif

#define SF32LB52_HCI_STATUS_SUCCESS          0x00
#define SF32LB52_HCI_READ_COMMANDS_RPLEN     65
#define SF32LB52_HCI_RAND_RPLEN              9
#define SF32LB52_HCI_MAX_CMD_COMPLETE_RPLEN  SF32LB52_HCI_READ_COMMANDS_RPLEN

/** 控制器解析列表（RAL）容量：**必须与 LCPU 侧配置一致**。
 *
 *  来源：`sf32lb52_lcpu_boot.c` 开机下发的
 *  `act_cfg.ble_max_ral = 3`（`HAL_LCPU_CONFIG_BT_ACT_CFG`），
 *  与厂商 SDK 里 `peripheral` / `pair` / `file_transfer` 例程的
 *  `act_cfg.ble_max_ral = 3` 是同一个值（Kconfig `MAX_BLE_RAL` 默认 3）。
 *
 *  ⚠ 以前这里报 0（bring-up 期的桩）。zblue 的 `bt_id_add()` 开头就是
 *  `if (!hdev->le.rl_size || ...) { 只记账; return; }` —— 报 0 等于告诉协议栈
 *  "控制器不会解析"，于是**每个已绑定设备的 IRK 都从不写给控制器**、
 *  `LE Set Address Resolution Enable` 也从不打开，手机每次回连的轮换 RPA
 *  都原样上报给应用 ⇒ 记录/准入永远认不出它、只能靠配对窗口续命。
 */
#define SF32LB52_HCI_RL_SIZE                 3

struct sf32lb52_bt_priv_s
{
  struct bt_driver_s drv;
  uint8_t rxbuf[SF32LB52_BT_H4_RX_BUFSIZE];
  size_t rxlen;
  bool drop_rx_until_tx;
  uint8_t classic_name_left;
  uint8_t classic_cod_left;
};

/* ===== 诊断插桩 ==========================================================
 * 只被 syslog 读取，不参与任何判断。目的：把"蓝牙为什么异常"变成可读的
 * 事实——协商到了什么参数、每条连接实际收发多少、下发给栈时有没有丢包。
 */
static uint32_t g_diag_conn_ms;          /* 本连接的 LE Connection Complete 时刻 */
static uint16_t g_diag_conn_hdl;
static uint16_t g_diag_conn_n;           /* 本开机累计连接数 */
static uint32_t g_diag_acl_in_pkts;
static uint32_t g_diag_acl_in_bytes;
static uint32_t g_diag_acl_out_pkts;
static uint32_t g_diag_acl_out_bytes;
static uint32_t g_diag_evt_rx;
static uint32_t g_diag_fwd_fail;         /* bt_netdev_receive 失败（-ENOMEM 等）*/
static uint32_t g_diag_conn_upd;         /* LE Connection Update Complete 次数 */
static uint32_t g_diag_rx_ms;            /* 最近一次从 LCPU 收到 HCI 包的时刻 */

/****************************************************************************
 * Name: myvendor_bth4_activity
 *
 * Description:
 *   报"距最近一次从 LCPU 收到 HCI 包"的毫秒数，给上层做**链路新鲜度**判据。
 *
 *   为什么这个数最能说明问题：手机连着且已订阅时，companion 每
 *   `COMPANION_STATUS_HEARTBEAT_SEC` 发一次 status notify，控制器收到后必须回
 *   `Number Of Completed Packets`（HCI 事件，走本驱动的收包路径）。所以这个
 *   年龄一直涨 = 控制器/LCPU 不再说话 —— 哪怕 `Disconnection Complete` 被丢掉、
 *   上层那个 `g_ctx.connected` 永远不清，也能据此判死（在案缺陷 #2）。
 *   与 App 行为无关，所以不会把"手机闲着"误判成断链。
 *
 * Input Parameters:
 *   rx_age_ms - 距最近一次收包的毫秒数（可 NULL）
 *   pkts      - 累计收包数（可 NULL，仅诊断）
 *
 * Returned Value:
 *   true = 本开机至少收到过一次包（rx_age_ms 有意义）；false = 还没收到过。
 *
 ****************************************************************************/

bool myvendor_bth4_activity(uint32_t *rx_age_ms, uint32_t *pkts)
{
  uint32_t last;

  last = g_diag_rx_ms;

  if (pkts != NULL)
    {
      *pkts = g_diag_evt_rx + g_diag_acl_in_pkts + g_diag_acl_out_pkts;
    }

  if (rx_age_ms != NULL)
    {
      *rx_age_ms = (last != 0u)
        ? myvendor_mono_elapsed_ms(myvendor_mono_ms(), last) : 0u;
    }

  return last != 0u;
}

static int sf32lb52_bt_open(struct bt_driver_s *drv);
static int sf32lb52_bt_send(struct bt_driver_s *drv,
                            enum bt_buf_type_e type,
                            void *data, size_t len);
static void sf32lb52_bt_close(struct bt_driver_s *drv);
static int sf32lb52_bt_recv_cb(uint8_t *data, uint16_t len);
static int sf32lb52_bt_ensure_controller_enabled(uint16_t opcode);

/* Do not call z_sys_init() here. Board bringup runs in the IDLE group;
 * k_thread_create() would start sysworkq as an IDLE pthread. libuv's
 * uv_default_loop() is task-TLS, so HCI ready → do_in_service_loop()
 * would uv_mutex_lock the wrong loop (assert at libuv thread.c:358).
 * SAL (ble_companion / bttool) calls z_sys_init() so the workqueue
 * inherits that process. */

static uint16_t sf32lb52_bt_get_le16(const uint8_t *data){
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void sf32lb52_bt_put_le16(uint8_t *data, uint16_t value)
{
  data[0] = value & 0xff;
  data[1] = value >> 8;
}

static bool sf32lb52_bt_is_classic_opcode(uint16_t opcode)
{
  switch (opcode)
    {
      case BT_HCI_OP_SET_EVENT_MASK:
      case BT_HCI_OP_SET_EVENT_MASK_PAGE_2:
      case BT_HCI_OP_WRITE_LOCAL_NAME:
      case BT_HCI_OP_WRITE_SCAN_ENABLE:
      case BT_HCI_OP_WRITE_PAGE_SCAN_ACTIVITY:
      case BT_HCI_OP_WRITE_INQUIRY_SCAN_ACTIVITY:
      case BT_HCI_OP_WRITE_PAGE_TIMEOUT:
      case BT_HCI_OP_WRITE_CLASS_OF_DEVICE:
      case BT_HCI_OP_WRITE_INQUIRY_SCAN_TYPE:
      case BT_HCI_OP_WRITE_EXTENDED_INQUIRY_RESPONSE:
      case BT_HCI_OP_WRITE_INQUIRY_MODE:
      case BT_HCI_OP_WRITE_PAGE_SCAN_TYPE:
      case BT_HCI_OP_WRITE_SSP_MODE:
      case BT_HCI_OP_WRITE_SC_HOST_SUPP:
      case BT_HCI_OP_WRITE_DEFAULT_LINK_POLICY_SETTINGS:
      case BT_HCI_OP_READ_LOCAL_EXT_FEATURES:
      case BT_HCI_OP_READ_BUFFER_SIZE:
      case BT_HCI_OP_ACCEPT_CONN_REQ:
      case BT_HCI_OP_LINK_KEY_REPLY:
      case BT_HCI_OP_LINK_KEY_NEG_REPLY:
      case BT_HCI_OP_AUTH_REQUESTED:
      case BT_HCI_OP_SET_CONN_ENCRYPT:
      case BT_HCI_OP_IO_CAPABILITY_REPLY:
      case BT_HCI_OP_USER_CONFIRM_REPLY:
      case BT_HCI_OP_USER_CONFIRM_NEG_REPLY:
        return true;

      default:
        return false;
    }
}

static ssize_t sf32lb52_bt_h4_packet_len(const uint8_t *data, size_t len)
{
  size_t hdr_len;
  uint16_t payload_len;
  size_t total;

  if (len < H4_HEADER_SIZE)
    {
      return 0;
    }

  switch (data[0])
    {
      case H4_EVT:
        hdr_len = H4_HEADER_SIZE + sizeof(struct bt_hci_evt_hdr_s);
        if (len < hdr_len)
          {
            return 0;
          }

        payload_len = data[H4_HEADER_SIZE + 1];
        break;

      case H4_ACL:
        hdr_len = H4_HEADER_SIZE + sizeof(struct bt_hci_acl_hdr_s);
        if (len < hdr_len)
          {
            return 0;
          }

        payload_len = sf32lb52_bt_get_le16(&data[H4_HEADER_SIZE + 2]);
        break;

      case H4_SCO:
        hdr_len = H4_HEADER_SIZE + SF32LB52_HCI_SCO_HDR_SIZE;
        if (len < hdr_len)
          {
            return 0;
          }

        payload_len = data[H4_HEADER_SIZE + 2];
        break;

      case H4_ISO:
        hdr_len = H4_HEADER_SIZE + sizeof(struct bt_hci_iso_hdr_s);
        if (len < hdr_len)
          {
            return 0;
          }

        payload_len = sf32lb52_bt_get_le16(&data[H4_HEADER_SIZE + 2]) & 0x3fff;
        break;

      default:
        return -EINVAL;
    }

  total = hdr_len + payload_len;
  if (total > SF32LB52_BT_H4_RX_BUFSIZE)
    {
      return -EINVAL;
    }

  return (ssize_t)total;
}

static void sf32lb52_bt_normalize_event(uint8_t *data, size_t len)
{
  uint16_t opcode;

  if (len < H4_HEADER_SIZE + sizeof(struct bt_hci_evt_hdr_s) + 4 ||
      data[0] != H4_EVT)
    {
      return;
    }

  if (data[1] == BT_HCI_EVT_CMD_COMPLETE && data[2] >= 4)
    {
      opcode = sf32lb52_bt_get_le16(&data[4]);
      if (sf32lb52_bt_is_classic_opcode(opcode))
        {
          syslog(LOG_INFO,
                 "sf32lb52 bth4 cc opcode=0x%04x status=%02x evtlen=%u\n",
                 opcode,
                 len > 6 ? data[6] : 0xff,
                 data[2]);
        }

      if (opcode == BT_HCI_OP_RESET && data[6] != SF32LB52_HCI_STATUS_SUCCESS)
        {
          data[6] = SF32LB52_HCI_STATUS_SUCCESS;
        }

      if (opcode == BT_HCI_OP_READ_BUFFER_SIZE && len >= 14)
        {
          syslog(LOG_INFO,
                 "sf32lb52 bth4 acl buf len=%u num=%u\n",
                 sf32lb52_bt_get_le16(&data[7]),
                 sf32lb52_bt_get_le16(&data[10]));
        }
    }
  else if (data[1] == BT_HCI_EVT_CMD_STATUS && data[2] >= 4)
    {
      opcode = sf32lb52_bt_get_le16(&data[5]);
      if (sf32lb52_bt_is_classic_opcode(opcode))
        {
          syslog(LOG_INFO,
                 "sf32lb52 bth4 cs opcode=0x%04x status=%02x\n",
                 opcode, data[3]);
        }

      if (opcode == BT_HCI_OP_RESET && data[3] != SF32LB52_HCI_STATUS_SUCCESS)
        {
          data[3] = SF32LB52_HCI_STATUS_SUCCESS;
        }
    }
  else if (data[1] == BT_HCI_EVT_LE_META_EVENT && len > 3 &&
           (data[3] == BT_HCI_EVT_LE_ADVERTISING_REPORT ||
            data[3] == BT_HCI_EVT_LE_EXT_ADVERTISING_REPORT ||
            data[3] == BT_HCI_EVT_LE_PER_ADVERTISING_REPORT))
    {
      /* 观察者扫描会刷爆控制台；ble_sensor 每 2 s 打一行摘要。 */
    }
  else if (data[1] == BT_HCI_EVT_DISCONN_COMPLETE && data[2] >= 4)
    {
      /* reason 在 payload[3]：0x13 对端断开，0x16 本机主机，0x08 超时。 */
      syslog(LOG_INFO,
             "sf32lb52 bth4 disconn hdl=0x%02x%02x status=%02x reason=0x%02x\n",
             len > 5 ? data[5] : 0xff,
             len > 4 ? data[4] : 0xff,
             data[3],
             len > 6 ? data[6] : 0xff);
      /* 诊断：断开时把这条连接的完整画像打出来。0x13 是对端主动断开，
       * 配合下面的收发量能区分"对端重试"与"链路故障"。 */
      syslog(LOG_INFO,
             "sf32lb52 bth4 sess hdl=0x%04x conn#%u dur=%u ms "
             "acl_in=%lu/%luB acl_out=%lu/%luB evt_rx=%lu fwd_fail=%lu "
             "conn_upd=%lu\n",
             (unsigned)g_diag_conn_hdl, (unsigned)g_diag_conn_n,
             g_diag_conn_ms != 0
               ? (unsigned)myvendor_mono_elapsed_ms(myvendor_mono_ms(),
                                                   g_diag_conn_ms)
               : 0u,
             (unsigned long)g_diag_acl_in_pkts,
             (unsigned long)g_diag_acl_in_bytes,
             (unsigned long)g_diag_acl_out_pkts,
             (unsigned long)g_diag_acl_out_bytes,
             (unsigned long)g_diag_evt_rx,
             (unsigned long)g_diag_fwd_fail,
             (unsigned long)g_diag_conn_upd);
      g_diag_conn_ms = 0;
    }
  else if (data[1] == BT_HCI_EVT_LE_META_EVENT && data[2] >= 1 && len > 3)
    {
      /* 诊断：把 LE Meta 子事件解码成可读一行。原来只打 subevent 和三字节，
       * 看不到协商到的 interval/latency/timeout，也无法判断
       * Connection Update 为什么反复发生。
       *
       * 偏移约定：data[0]=H4 类型，data[1]=事件码，data[2]=plen，
       * data[3+n] = payload[n]。故 data[3] 是 subevent。
       * 子事件编号与 payload 长度见 Bluetooth Core Vol 4 Part E 7.7.65。
       */
      const uint8_t sub = data[3];

      switch (sub)
        {
          case 0x01: /* LE Connection Complete, plen 19 */
            if (data[2] >= 19)
              {
                g_diag_conn_n++;
                g_diag_conn_hdl = sf32lb52_bt_get_le16(&data[5]);
                g_diag_acl_in_pkts = 0;
                g_diag_acl_in_bytes = 0;
                g_diag_acl_out_pkts = 0;
                g_diag_acl_out_bytes = 0;
                g_diag_evt_rx = 0;
                g_diag_fwd_fail = 0;
                g_diag_conn_upd = 0;
                g_diag_conn_ms = myvendor_mono_ms();
                syslog(LOG_INFO,
                       "sf32lb52 bth4 conn st=%02x hdl=0x%04x role=%u "
                       "atype=%u peer=%02x:%02x:%02x:%02x:%02x:%02x "
                       "itv=%u lat=%u to=%u(%u ms)\n",
                       data[4], (unsigned)g_diag_conn_hdl, data[7], data[8],
                       data[14], data[13], data[12], data[11], data[10], data[9],
                       (unsigned)sf32lb52_bt_get_le16(&data[15]),
                       (unsigned)sf32lb52_bt_get_le16(&data[17]),
                       (unsigned)sf32lb52_bt_get_le16(&data[19]),
                       (unsigned)(sf32lb52_bt_get_le16(&data[19]) * 10u));
              }
            break;

          case 0x03: /* LE Connection Update Complete, plen 10 */
            if (data[2] >= 10)
              {
                uint16_t itv = sf32lb52_bt_get_le16(&data[7]);

                g_diag_conn_upd++;
                syslog(LOG_INFO,
                       "sf32lb52 bth4 conn_upd#%lu st=%02x hdl=0x%04x "
                       "itv=%u(%u.%02u ms) lat=%u to=%u(%u ms)\n",
                       (unsigned long)g_diag_conn_upd, data[4],
                       (unsigned)sf32lb52_bt_get_le16(&data[5]),
                       (unsigned)itv,
                       (unsigned)((itv * 125u) / 100u),
                       (unsigned)((itv * 125u) % 100u),
                       (unsigned)sf32lb52_bt_get_le16(&data[9]),
                       (unsigned)sf32lb52_bt_get_le16(&data[11]),
                       (unsigned)(sf32lb52_bt_get_le16(&data[11]) * 10u));
              }
            break;

          case 0x04: /* LE Read Remote Features Complete, plen 12 */
            if (data[2] >= 12)
              {
                syslog(LOG_INFO,
                       "sf32lb52 bth4 rmt_feat st=%02x hdl=0x%04x "
                       "feat=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                       data[4], (unsigned)sf32lb52_bt_get_le16(&data[5]),
                       data[14], data[13], data[12], data[11],
                       data[10], data[9], data[8], data[7]);
              }
            break;

          case 0x07: /* LE Data Length Change, plen 11 */
            if (data[2] >= 11)
              {
                syslog(LOG_INFO,
                       "sf32lb52 bth4 dlen hdl=0x%04x tx=%u/%u rx=%u/%u\n",
                       (unsigned)sf32lb52_bt_get_le16(&data[3]),
                       (unsigned)sf32lb52_bt_get_le16(&data[5]),
                       (unsigned)sf32lb52_bt_get_le16(&data[7]),
                       (unsigned)sf32lb52_bt_get_le16(&data[9]),
                       (unsigned)sf32lb52_bt_get_le16(&data[11]));
              }
            break;

          default:
            /* 保留原来的兜底行，避免丢失信息。 */
            syslog(LOG_INFO,
                   "sf32lb52 bth4 evt=0x%02x le_sub=0x%02x len=%u "
                   "p1=%02x p2=%02x\n",
                   data[1], sub, data[2],
                   len > 4 ? data[4] : 0xff,
                   len > 5 ? data[5] : 0xff);
            break;
        }
    }
  else if (data[1] != BT_HCI_EVT_NUM_COMPLETED_PACKETS)
    {
      syslog(LOG_INFO,
             "sf32lb52 bth4 evt=0x%02x len=%u p0=%02x p1=%02x p2=%02x\n",
             data[1], data[2],
             len > 3 ? data[3] : 0xff,
             len > 4 ? data[4] : 0xff,
             len > 5 ? data[5] : 0xff);
    }
}

static int sf32lb52_bt_forward_packet(struct sf32lb52_bt_priv_s *priv,
                                      uint8_t *data, size_t len)
{
  enum bt_buf_type_e type;
  int ret;

  if (len <= H4_HEADER_SIZE)
    {
      return -EINVAL;
    }

  switch (data[0])
    {
      case H4_EVT:
        sf32lb52_bt_normalize_event(data, len);
        g_diag_evt_rx++;
        type = BT_EVT;
        break;

      case H4_ACL:
        g_diag_acl_in_pkts++;
        g_diag_acl_in_bytes += (uint32_t)len;
        type = BT_ACL_IN;
        break;

      case H4_ISO:
        type = BT_ISO_IN;
        break;

      case H4_SCO:
        /* NuttX bt_buf has no SCO type.  Consume the packet so the H4
         * stream stays aligned; page scan does not need voice slots.
         */
        syslog(LOG_INFO, "sf32lb52 bth4 drop sco len=%lu\n",
               (unsigned long)len);
        return OK;

      default:
        return -EINVAL;
    }

  ret = bt_netdev_receive(&priv->drv,
                          type,
                          (void *)&data[H4_HEADER_SIZE],
                          len - H4_HEADER_SIZE);
  if (ret < 0)
    {
      /* 诊断：这里是 HCI 包被永久丢弃的唯一出口。丢一个 ACL 分片会让
       * L2CAP 重组永不完结，丢一个断连事件会让上层一直以为链路还在。
       * 计数 + 类型是判断"上层看到的蓝牙异常是否源于此处"的关键。 */
      g_diag_fwd_fail++;
      syslog(LOG_ERR,
             "sf32lb52 bth4 fwd fail n=%lu type=0x%02x len=%lu ret=%d "
             "(pkt dropped)\n",
             (unsigned long)g_diag_fwd_fail, data[0],
             (unsigned long)len, ret);
      wlerr("Failed to receive HCI packet: %d\n", ret);
    }

  return ret;
}

static int sf32lb52_bt_synth_cmd_complete(struct sf32lb52_bt_priv_s *priv,
                                          uint16_t opcode,
                                          const uint8_t *return_params,
                                          size_t return_len)
{
  uint8_t event[H4_HEADER_SIZE + sizeof(struct bt_hci_evt_hdr_s) + 3 +
                SF32LB52_HCI_MAX_CMD_COMPLETE_RPLEN];
  size_t payload_len;

  if (return_len > SF32LB52_HCI_MAX_CMD_COMPLETE_RPLEN)
    {
      return -E2BIG;
    }

  payload_len = 3 + return_len;
  event[0] = H4_EVT;
  event[1] = BT_HCI_EVT_CMD_COMPLETE;
  event[2] = payload_len;
  event[3] = 1;
  sf32lb52_bt_put_le16(&event[4], opcode);
  if (return_len > 0)
    {
      memcpy(&event[6], return_params, return_len);
    }

  return sf32lb52_bt_forward_packet(priv, event,
                                    H4_HEADER_SIZE +
                                    sizeof(struct bt_hci_evt_hdr_s) +
                                    payload_len);
}

static int sf32lb52_bt_synth_status_complete(struct sf32lb52_bt_priv_s *priv,
                                             uint16_t opcode)
{
  const uint8_t status = SF32LB52_HCI_STATUS_SUCCESS;

  return sf32lb52_bt_synth_cmd_complete(priv, opcode, &status,
                                        sizeof(status));
}

static bool sf32lb52_bt_emulate_cmd(struct sf32lb52_bt_priv_s *priv,
                                    uint16_t opcode, int *ret)
{
  uint8_t params[SF32LB52_HCI_MAX_CMD_COMPLETE_RPLEN];

  switch (opcode)
    {
      case BT_HCI_OP_READ_SUPPORTED_COMMANDS:
        memset(params, 0, sizeof(params));
        params[0] = SF32LB52_HCI_STATUS_SUCCESS;
        /* (octet, bit) 索引与 zblue 的 `BT_CMD_TEST(supported_commands, o, b)`
         * 调用点一一对应 —— 全树只有这两处：
         *
         *   (27,7) = HCI_LE_Rand：`hci_core.c:554`（`bt_rand()` 的入口检查）。
         *            本函数**自己桩了** `LE_Rand`，所以必须报支持，否则
         *            `bt_rand()` 直接 -ENOTSUP。
         *
         *   (39,2) = HCI_LE_Set_Privacy_Mode：`id.c:838`
         *            （`le_set_privacy_mode()` 的唯一调用点）。**这一位以前是 0**
         *            ⇒ 该函数开头 `if (!BT_CMD_TEST(...)) { return 0; }` 直接返回，
         *            **`LE Set Privacy Mode` 从不发给控制器** ⇒ 控制器一直停在复位
         *            默认的 network privacy mode。
         *
         * 补上它的理由：`bt_id_add()` 在把对端 IRK 写进解析列表之后会调
         * `le_set_privacy_mode(…, DEVICE)`，厂商 `connection_manager` 的
         * `update_resolving_list()` 同样用 `priv_mode = 1`（device）。
         * 现场（2026-09-23 17:0x）解析列表已写入且解析已使能
         * （日志可见 `op=0x2027` / `op=0x202d`），但连接完成事件仍报 RPA
         * （`conn … atype=1 peer=5e:…`）—— 这是与厂商参考剩下的最后一处差异。
         *
         * 安全性：`(39,2)` 全树只有 `le_set_privacy_mode()` 一个消费者，与
         * `(27,7)` 不相干。LCPU 若不支持该命令，`bt_id_add()` 只走
         * `LOG_ERR` + `goto done`，解析列表条目**已经在**、解析使能照旧执行。
         */
        params[1 + 27] = 1 << 7;
        params[1 + 39] = 1 << 2;
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params,
                                              SF32LB52_HCI_READ_COMMANDS_RPLEN);
        return true;

      case BT_HCI_OP_LE_RAND:
        params[0] = SF32LB52_HCI_STATUS_SUCCESS;
        arc4random_buf(&params[1], SF32LB52_HCI_RAND_RPLEN - 1);
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params,
                                              SF32LB52_HCI_RAND_RPLEN);
        return true;

      /* Keep EIR stubbed and later name/CoD stubbed: extra 251-byte
       * writes after br_init smash the HCPU heap.
       * READ_BUFFER_SIZE / READ_LOCAL_EXT_FEATURES are forwarded.
       * LE_READ_BUFFER_SIZE is stubbed: forwarding LCPU's reply
       * made zblue assert on sysworkq during init.
       */

      case BT_HCI_OP_LE_READ_BUFFER_SIZE:
        params[0] = SF32LB52_HCI_STATUS_SUCCESS;
        sf32lb52_bt_put_le16(&params[1], 0x00fb);
        params[3] = 6;
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params, 4);
        return true;

      case BT_HCI_OP_LE_READ_LOCAL_FEATURES:
        memset(params, 0, 9);
        params[0] = SF32LB52_HCI_STATUS_SUCCESS;
        /* enc, conn-param, ext-reject, slave-features, ping, DLE, **LL Privacy**
         *
         * ⚠ bit6（LL Privacy，`BT_LE_FEAT_BIT_PRIVACY = 6`）以前是 0，与
         * `LE Read Resolving List Size` 报 0 是**同一处 bring-up 期的保守桩**，
         * 而且这两条必须一起改、只改一条没用：
         *
         *   zblue 的 `le_init()` 里整段隐私初始化（含读 RAL 容量）被
         *   `if (BT_FEAT_LE_PRIVACY(hdev->le.features))` 包着。
         *   bit6 为 0 ⇒ `LE Read Resolving List Size` **根本不会发出去** ⇒
         *   `hdev->le.rl_size` 永远是 0 ⇒ `bt_id_add()` 走"主机侧解析"早退 ⇒
         *   已绑定设备的 IRK 从不写进控制器解析列表 ⇒ 手机每次回连上报的都是
         *   轮换 RPA（现场：`bt_keys kv: … -> RAL rl_size=0`）。
         *
         * 控制器确实支持：板上给 LCPU 配了 `ble_max_ral = 3`
         * （`sf32lb52_lcpu_boot.c`），厂商 `example/ble/*` 也是同一个值。
         *
         * 打开它只多三件事，都已核过是安全的：
         *   · `LE Set RPA Timeout` —— 本函数已有桩（回 success，不转给 LCPU）；
         *   · `LE Read Resolving List Size` —— 本函数回 3（目标）；
         *   · 事件掩码里的 `ENH_CONN_COMPLETE` —— 但 `LE Set Event Mask` 也被
         *     本函数吞掉了，控制器照旧发 legacy 连接完成事件，而 zblue 是按
         *     **实际收到的子事件**分发的（`le_meta_event()`），两种都认 ⇒
         *     连接事件格式不变，`conn st=… peer=…` 那行诊断照旧。 */
        params[1] = 0x7f;
        /* LE 2M PHY */
        params[2] = 0x01;
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params, 9);
        return true;

      case BT_HCI_OP_LE_READ_MAX_DATA_LEN:
        params[0] = SF32LB52_HCI_STATUS_SUCCESS;
        sf32lb52_bt_put_le16(&params[1], 0x00fb);
        sf32lb52_bt_put_le16(&params[3], 0x0148);
        sf32lb52_bt_put_le16(&params[5], 0x00fb);
        sf32lb52_bt_put_le16(&params[7], 0x0148);
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params, 9);
        return true;

      case BT_HCI_OP_LE_READ_MAX_ADV_DATA_LEN:
        params[0] = SF32LB52_HCI_STATUS_SUCCESS;
        sf32lb52_bt_put_le16(&params[1], 31);
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params, 3);
        return true;

      case BT_HCI_OP_LE_READ_SUPP_STATES:
        memset(params, 0, 9);
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params, 9);
        return true;

      case BT_HCI_OP_LE_READ_RL_SIZE:
        /* 报真实容量（见 SF32LB52_HCI_RL_SIZE 注释）。回报里它就一个字节，
         * 之后 zblue 会依次发 LE Add Device To Resolving List（每把已存 IRK
         * 一条）、LE Set Privacy Mode(=device)、LE Set Address Resolution
         * Enable —— 这三条本函数不拦，原样转给 LCPU。
         *
         * 自证伪：若 LCPU 其实不支持，日志会成片出现
         *   `Failed to add IRK to controller` / `Failed to set privacy mode`
         * 且手机侧地址仍是轮换 RPA（`ctl pair phone` 里 type=1 每次都在变）
         * ⇒ 那时说明容量该报 0，把本行改回去。
         */
        params[0] = SF32LB52_HCI_STATUS_SUCCESS;
        params[1] = SF32LB52_HCI_RL_SIZE;
        *ret = sf32lb52_bt_synth_cmd_complete(priv, opcode, params, 2);
        return true;

      case BT_HCI_OP_LE_WRITE_LE_HOST_SUPP:
      case BT_HCI_OP_LE_SET_HOST_FEATURE:
      case BT_HCI_OP_LE_SET_EVENT_MASK:
      case BT_HCI_OP_LE_SET_RPA_TIMEOUT:
      case BT_HCI_OP_WRITE_EXTENDED_INQUIRY_RESPONSE:
      case BT_HCI_OP_HOST_BUFFER_SIZE:
      case BT_HCI_OP_SET_CTL_TO_HOST_FLOW:
        *ret = sf32lb52_bt_synth_status_complete(priv, opcode);
        return true;

      case BT_HCI_OP_WRITE_LOCAL_NAME:
        if (priv->classic_name_left == 0)
          {
            *ret = sf32lb52_bt_synth_status_complete(priv, opcode);
            return true;
          }

        priv->classic_name_left--;
        return false;

      case BT_HCI_OP_WRITE_CLASS_OF_DEVICE:
        if (priv->classic_cod_left == 0)
          {
            *ret = sf32lb52_bt_synth_status_complete(priv, opcode);
            return true;
          }

        priv->classic_cod_left--;
        return false;

      default:
        return false;
    }
}

static struct sf32lb52_bt_priv_s g_sf32lb52_bt_priv =
{
  .drv =
    {
      .head_reserve = H4_HEADER_SIZE,
      .open         = sf32lb52_bt_open,
      .send         = sf32lb52_bt_send,
      .close        = sf32lb52_bt_close,
    },
};

static int sf32lb52_bt_recv_cb(uint8_t *data, uint16_t len)
{
  struct sf32lb52_bt_priv_s *priv = &g_sf32lb52_bt_priv;
  ssize_t packet_len;
  int ret;

  if (data == NULL || len == 0)
    {
      return -EINVAL;
    }

  /* 活性时间戳：**控制器还在往上报**的直接证据（见 myvendor_bth4_activity）。
   *
   * 记在这里而不是下面各 type 分支里：EVT（含 Number Of Completed Packets）
   * 和 ACL 都算；`drop_rx_until_tx` 丢包期间**照样要记** —— 丢包是主机的策略，
   * 不代表控制器没说话。 */
  g_diag_rx_ms = myvendor_mono_ms();

  if (priv->drop_rx_until_tx)
    {
      return OK;
    }

  if (len > sizeof(priv->rxbuf))
    {
      syslog(LOG_ERR, "sf32lb52 bth4 rx chunk too big: %u\n", len);
      priv->rxlen = 0;
      return -ENOBUFS;
    }

  if (priv->rxlen + len > sizeof(priv->rxbuf))
    {
      syslog(LOG_ERR,
             "sf32lb52 bth4 rx overflow: pending=%lu incoming=%u first=%02x\n",
             (unsigned long)priv->rxlen,
             len,
             priv->rxlen > 0 ? priv->rxbuf[0] : 0);
      priv->rxlen = 0;
    }

  memcpy(&priv->rxbuf[priv->rxlen], data, len);
  priv->rxlen += len;

#if SF32LB52_BT_TRACE
  syslog(LOG_INFO,
         "sf32lb52 bth4 recv: len=%u pending=%lu first=%02x\n",
         len, (unsigned long)priv->rxlen, priv->rxbuf[0]);
#endif

  ret = OK;

  while (priv->rxlen > 0)
    {
      packet_len = sf32lb52_bt_h4_packet_len(priv->rxbuf, priv->rxlen);
      if (packet_len == 0)
        {
          break;
        }

      if (packet_len < 0)
        {
          syslog(LOG_WARNING,
                 "sf32lb52 bth4 drop invalid h4 type=%02x pending=%lu\n",
                 priv->rxbuf[0],
                 (unsigned long)priv->rxlen);
          /* Byte-resync of a corrupt HCI stream has previously smashed
           * the HCPU heap.  Drop the pending buffer instead.
           */
          priv->rxlen = 0;
          ret = -EINVAL;
          continue;
        }

      if ((size_t)packet_len > priv->rxlen)
        {
          break;
        }

      ret = sf32lb52_bt_forward_packet(priv, priv->rxbuf,
                   (size_t)packet_len);

      priv->rxlen -= (size_t)packet_len;
      if (priv->rxlen > 0)
        {
          memmove(priv->rxbuf,
                  &priv->rxbuf[packet_len],
                  priv->rxlen);
        }
    }

  return ret;
}

static int sf32lb52_bt_send(struct bt_driver_s *drv,
                            enum bt_buf_type_e type,
                            void *data, size_t len)
{
  struct sf32lb52_bt_priv_s *priv = &g_sf32lb52_bt_priv;
  uint8_t *hdr = (uint8_t *)data - drv->head_reserve;
  uint16_t opcode;
  int ret;

  switch (type)
    {
      case BT_CMD:
        *hdr = H4_CMD;
        break;
      case BT_ACL_OUT:
        *hdr = H4_ACL;
        g_diag_acl_out_pkts++;
        g_diag_acl_out_bytes += (uint32_t)len;
        break;
      case BT_ISO_OUT:
        *hdr = H4_ISO;
        break;
      default:
        return -EINVAL;
    }

  if (type == BT_CMD && len >= sizeof(struct bt_hci_cmd_hdr_s))
    {
      opcode = sf32lb52_bt_get_le16(data);
      if (sf32lb52_bt_emulate_cmd(priv, opcode, &ret))
        {
          return ret < 0 ? ret : len;
        }

      /* LCPU BR/EDR Secure Connections (P-256) has been unreliable on
       * this H4 path.  Force legacy SSP (P-192 Just Works) so the
       * Settings pairing dialog can finish.  Host still sees CC success.
       */

      if (opcode == BT_HCI_OP_WRITE_SC_HOST_SUPP &&
          len > sizeof(struct bt_hci_cmd_hdr_s))
        {
          uint8_t *sc =
            &((uint8_t *)data)[sizeof(struct bt_hci_cmd_hdr_s)];

          if (*sc != 0)
            {
              syslog(LOG_INFO,
                     "sf32lb52 bth4 disable SC host supp (was %02x)\n",
                     *sc);
              *sc = 0;
            }
        }

      /* zblue only sets ENCRYPT_CHANGE in the mask when LE encryption
       * is advertised.  We stub LE features to zero, so Classic
       * ENCRYPT_CHANGE (bit 7) and KEY_REFRESH (bit 47) never reach
       * the host — RFCOMM then never leaves L1.
       */

      if (opcode == BT_HCI_OP_SET_EVENT_MASK &&
          len >= sizeof(struct bt_hci_cmd_hdr_s) + 8)
        {
          uint8_t *mask =
            &((uint8_t *)data)[sizeof(struct bt_hci_cmd_hdr_s)];

          if ((mask[0] & 0x80) == 0 || (mask[5] & 0x80) == 0)
            {
              syslog(LOG_INFO,
                     "sf32lb52 bth4 enable ENCRYPT_CHANGE in event mask\n");
              mask[0] |= 0x80;
              mask[5] |= 0x80;
            }
        }

      if (sf32lb52_bt_is_classic_opcode(opcode))
        {
          syslog(LOG_INFO,
                 "sf32lb52 bth4 fwd classic opcode=0x%04x plen=%lu param=%02x\n",
                 opcode,
                 (unsigned long)len,
                 len > sizeof(struct bt_hci_cmd_hdr_s)
                   ? ((const uint8_t *)data)[sizeof(struct bt_hci_cmd_hdr_s)]
                   : 0);
        }

      if (opcode == BT_HCI_OP_LE_CONN_UPDATE &&
          len >= sizeof(struct bt_hci_cmd_hdr_s) + 14)
        {
          uint8_t *p = &((uint8_t *)data)[sizeof(struct bt_hci_cmd_hdr_s)];

          /* Unit is 0.625 ms.  0/0 makes some controllers use a one-PDU
           * event; 5–15 ms fits several 251-byte 2M packets.
           */

          if (sf32lb52_bt_get_le16(&p[10]) < 8 ||
              sf32lb52_bt_get_le16(&p[12]) < 24)
            {
              sf32lb52_bt_put_le16(&p[10], 8);
              sf32lb52_bt_put_le16(&p[12], 24);
              syslog(LOG_INFO, "sf32lb52 bth4 conn update ce_len 8-24\n");
            }
        }

      ret = sf32lb52_bt_ensure_controller_enabled(opcode);
      if (ret < 0)
        {
          return ret;
        }
    }
  else
    {
      ret = sf32lb52_bt_ensure_controller_enabled(0);
      if (ret < 0)
        {
          return ret;
        }
    }

  priv->drop_rx_until_tx = false;

  if (SF32LB52_BT_TRACE && type == BT_ACL_OUT)
    {
      syslog(LOG_INFO,
             "sf32lb52 bth4 tx: type=%u len=%lu h4=%02x acl=%02x %02x %02x %02x\n",
             (unsigned int)type,
             (unsigned long)(len + drv->head_reserve),
             hdr[0],
             len >= 1 ? hdr[1] : 0,
             len >= 2 ? hdr[2] : 0,
             len >= 3 ? hdr[3] : 0,
             len >= 4 ? hdr[4] : 0);
    }

  ret = sf32lb52_host_send_packet(hdr, len + drv->head_reserve);
  if (ret < 0)
    {
      return ret;
    }

  return len;
}

static int sf32lb52_bt_ensure_controller_enabled(uint16_t opcode)
{
  int ret;

  ret = sf32lb52_bt_controller_enable();
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "sf32lb52 bth4 controller enable failed before opcode=0x%04x: %d\n",
             opcode,
             ret);
    }

  return ret;
}

static int sf32lb52_bt_open(struct bt_driver_s *drv)
{
  int ret;

  (void)drv;

  g_sf32lb52_bt_priv.rxlen = 0;
  g_sf32lb52_bt_priv.drop_rx_until_tx = true;
  g_sf32lb52_bt_priv.classic_name_left = 1;
  g_sf32lb52_bt_priv.classic_cod_left = 1;

  ret = sf32lb52_bt_controller_init();
  if (ret < 0)
    {
      return ret;
    }

  ret = sf32lb52_hci_register_callback(sf32lb52_bt_recv_cb);
  if (ret < 0)
    {
      return ret;
    }

  return OK;
}

static void sf32lb52_bt_close(struct bt_driver_s *drv)
{
  int ret;

  (void)drv;

  g_sf32lb52_bt_priv.rxlen = 0;

  /* Full deinit (not just disable) so that the next open re-initialises
   * the IPC queue from a clean state.  Without deinit, controller_init
   * is skipped on re-open (status != IDLE), and LCPU's ring-buffer
   * read pointer (reset on power-on) diverges from HCPU's stale write
   * pointer, causing all HCI commands to be silently dropped. */
  ret = sf32lb52_bt_controller_deinit();
  if (ret < 0)
    {
      wlerr("Failed to deinit HCI controller: %d\n", ret);
    }
}

int sf32lb52_bt_initialize(void)
{
  int ret;

  ret = uart_bth4_register("/dev/ttyHCI0", &g_sf32lb52_bt_priv.drv);
  if (ret < 0 && ret != -EEXIST)
    {
      wlerr("Failed to register /dev/ttyHCI0: %d\n", ret);
      return ret;
    }

  return OK;
}
