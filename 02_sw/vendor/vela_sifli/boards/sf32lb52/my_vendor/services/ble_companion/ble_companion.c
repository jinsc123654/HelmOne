/**
 * @file ble_companion.c
 * @brief BLE Companion 外设（服务 0xFF10）与双角色主机。
 *
 * 手机链路：广播 "<PRODUCT_NAME>-XXXX"（LE 地址末两字节），协议版本读、
 * GNSS Fix 写、GATT FS、通知、导航路线。不驱动自行车 UI。
 * NSH：`test sensor` / `test notif` / `test ctrl`。
 *
 * 双角色（docs/ble/ble_sensor.md）：
 * - 对手机是 Peripheral，对独立 HR/CSC/CPS 是 Central。
 * - GATTS 的 connect/MTU/disconnect/notify-complete 以及特征写/CCC
 *   必须忽略传感器地址。zblue att_mtu_updated 是全局的；HR ATT 23
 *   曾把 Companion FS MTU 设成 20。FS set_mtu 还要匹配手机 peer。
 * - 手机回连常先记 RPA、加密后 ATT/CCC 换身份地址：`companion_is_phone()`
 *   刷新 peer 并同步 FS，否则 `fs ccc/cmd DROPPED`（App 0 帧超时）。
 *   见 docs/ble/pairing_and_radio.md §1.3。
 * - 只在角色为 BT_CONN_ROLE_PERIPHERAL 时收紧 7.5 ms / PHY / DLE。
 *   在传感器（central）链路上强推会和 Android 打架（HCI 0x16）。
 * - zblue `disconnected` 转给 ble_sensor：GATTC 回调在满载时可能丢，
 *   传感器会假显示「已连接」而外设继续广播。
 * - 手机广播走框架 `bt_le_start_advertising`（SAL）。板卡
 *   `vela_override/bluetooth/sal_le_advertise_interface.c` 把 Ext Adv 换成
 *   legacy `bt_le_adv_start`，避免 SF32 Command Disallowed /
 *   `advertising started status=2`。
 *
 * 协议：include/companion_proto.h
 * 规格：docs/ble/companion_impl_plan.md
 *
 * NSH 用法：ble_companion 或 ble_companion & ；停止用 kill <pid>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bluetooth.h"
#include "bt_addr.h"
#include "bt_adapter.h"
#include "bt_device.h"
#include "bt_gatts.h"
#include "bt_le_advertiser.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>

#include "companion_proto.h"
#include "myvendor_identity.h"
#include "myvendor_sys.h"
#include "companion_fs.h"
#include "companion_bridge.h"
#include "companion_downlink.h"
#include "ble_sensor.h"
#include "ble_supervisor.h"
#include "myvendor_ble_log.h"
#include "myvendor_devctl.h"
#include "myvendor_gnss.h"
#include "myvendor_mono.h"
#include "transfer_backend.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOGI(fmt, ...) syslog(LOG_INFO, "ble_companion: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) syslog(LOG_ERR, "ble_companion: " fmt "\n", ##__VA_ARGS__)

/* 配对/准入那条线在 myvendor_bt_pair.c 里（同 target，头文件不共享，用 extern 声明）。
 * 语义见 myvendor_bt_pair.c：没配对就不接受连接；绑定后可连接无名 ADV。
 * **定向 ADV 已关闭**（2026-09-22 实测 SF32 status=2，不支持）。 */
extern bool myvendor_pair_admit(const bt_address_t *addr);
extern void myvendor_pair_note_bond(const bt_address_t *addr, bool bonded);
extern bool myvendor_pair_phone_link_up(void);
extern bool myvendor_pair_has_phone_record(void);
extern bool myvendor_pair_window_active(void);
extern bool myvendor_pair_window_take_restart(void);
extern void myvendor_pair_admit_poll(void);
extern void myvendor_pair_unbind_poll(void);
extern bool myvendor_pair_pending(void);
extern void myvendor_pair_settle_poll(void);
extern void myvendor_pair_peer_poll(void);
extern void myvendor_pair_window_open(unsigned secs);
extern void myvendor_pair_unbind_request(void);
extern void myvendor_pair_note_peer_if_secure(const bt_address_t *addr);

#ifdef CONFIG_MYVENDOR_BLE_COMPANION_DEBUG
#  define LOGD(fmt, ...) syslog(LOG_DEBUG, "ble_companion: " fmt "\n", ##__VA_ARGS__)
#else
#  define LOGD(fmt, ...)
#endif

/** 产品名来自 Kconfig；广播名为 "<PRODUCT_NAME>-XXXX"（LE 地址末两字节）。 */

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#ifdef CONFIG_MYVENDOR_BLE_COMPANION_ADV_INTERVAL_MS
#  define ADV_INTERVAL_MS CONFIG_MYVENDOR_BLE_COMPANION_ADV_INTERVAL_MS
#else
#  define ADV_INTERVAL_MS 200
#endif

/** 控制器按 0.625 ms 计广播间隔。 */

#define ADV_INTERVAL_UNITS  ((ADV_INTERVAL_MS) * 8 / 5)

/** 曾用 1280 ms「空闲档」给已绑定回连省空口。现场（2026-09-21）：
 *  手机未连时本来就会 ADV，再叠 sensor ADV↔scan 的 2 s 窗，1280 ms 间隔
 *  一窗只剩 1~2 个 ADV 事件 → App 直连经常卡几十秒到数分钟；关外设后
 *  连续 1280 ms 也不够 Android autoConnect。已绑定回连一律用快广播
 * （`ADV_INTERVAL_MS`）；省空口靠「手机连上后停 ADV」和 sensor 分时，
 *  不靠拉稀间隔。 */

#define HCI_DEV_PATH        "/dev/ttyHCI0"
#define HCI_WAIT_SECS       10
#define ADAPTER_WAIT_SECS   60
#define ADAPTER_CYCLE_WAIT_SECS 8

/** @brief "广播让位"（sensor 扫描/建链时暂停广播）**空转**多久就强制收回。
 *
 * 让位本身是必需的：这颗 LCPU 一个时刻只有一个射频角色，扫传感器就得停广播。
 * 但**让位必须有上限** —— 现场出现过 hold 一直没释放、广播永久停着，手机彻底
 * 看不到设备（"其他设备收不到 BLE 广播"）。
 *
 * **判据是"空转"，不是"时长"。** 早先这里取 20 s、按纯时长收，会误伤正常让位：
 * 同一版固件的健康会话里让位最长到过 768 s，而且每次都正常闭合
 * （`analysis/BLE_ADV_HOLD_LOCKOUT.md` §4）。所以收回条件是
 * **闩锁在 + 传感器一次都没真的用射频（`transient` 恒 0）连续这么久**：
 *   · 正常让位每拍都会被 `COMPANION_BLE_EV_TRANSIENT` 刷新 ⇒ 永不触发；
 *   · 2026-09-23 13:13~14:25 那次外设关机 72 min，`hold=0`（transient 恒 0）
 *     而闩锁恒 1 ⇒ 180 s 就到点收回，手机重新可见。
 *
 * 自证伪：若日志出现 `adv hold watchdog` 而同时段传感器**确实在扫描/建链**，
 * 说明 180 s 太短 ⇒ 调大本值，别去掉这道兜底。 */
#define COMPANION_ADV_HOLD_MAX_MS 180000u
#define ADAPTER_CYCLE_OFF_MS    8000u
/** skip-HCI respawn 后再 enable，GATTS 可能还没 startup。 */
#define GATTS_READY_RETRY       10u
#define GATTS_READY_SLICE_MS    200u
#define GATTS_RECYCLE_OFF_MS    1000u
/** uart_bth4 -ENOMEM 连续超过该时长才当 HCI 已卡住。 */
#define HCI_RX_STALL_MS         2500u

/** 主循环节拍。每拍重算状态；仅在变化、请求或心跳时发 notify。 */

#define TICK_MS             500

/** 属性 id，也是 bt_gatts_notify() 用的 handle。 */

enum
{
  COMP_SERVICE_ID = 1,
  COMP_PROTO_VER_ID,
  COMP_STATUS_ID,
  COMP_STATUS_CCC_ID,
  COMP_CONTROL_ID,
  COMP_GNSS_ID,
  COMP_FS_CMD_ID,
  COMP_FS_DATA_ID,
  COMP_FS_DATA_CCC_ID,
  COMP_NOTIF_ID,
  COMP_NAV_ID,
  COMP_DEVINFO_ID,
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct companion_ctx
{
  pthread_mutex_t lock;

  bool          connected;     /**< 手机 GATTS 已连接（不是传感器）。 */
  bt_address_t  peer;          /**< 手机 LE 地址。 */
  bool          ccc_enabled;   /**< App 已订阅 Device Status。 */
  uint32_t      mtu;           /**< 手机 ATT MTU；绝不能来自传感器。 */
  bool          adv_restart;   /**< 手机断开后主循环重启 ADV。 */

  struct companion_status  status;
  bool                     notify_req;   /**< 下一拍发送状态 notify。 */
  bool                     status_dirty;

  struct companion_gnss_fix last_fix;
  bool                      fix_valid;
  uint32_t                  last_gnss_ms;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct companion_ctx g_ctx;

static bt_instance_t   *g_ins;
static void            *g_adapter_cookie;
static gatts_handle_t   g_gatts_handle;
static bt_advertiser_t *g_adv;
static volatile bool    g_hold_le_adv;
/** 传感器"**要**射频"的显式让位请求（电平闩锁，由 request/release 边沿驱动）。
 *  drain 里 `g_hold_le_adv = transient` 是派生语义，而 transient 的定义是
 *  「正在扫描 / 正在建链」——**要等扫描真的起来才为真**。传感器的时间片
 *  状态机却是「先看到让位确认 (held) 才起扫」，两边互等：2026-09-18 实机就是
 *  卡在 ADV_STOP_WAIT 14 s（射频全空、HCI 一条命令不发、被 diag 拆适配器，
 *  22.6 s 一轮）。闩锁把"要"与"正在用"分开，环就断了。 */
static volatile bool    g_hold_req_latch;
/** @brief 判定侧读的本地镜像：现在该不该给 sensor 让出广播。
 *
 *  `transient` 那一路是**派生值** —— sensor 每拍上报"正在扫描/建链"
 *  （COMPANION_BLE_EV_TRANSIENT），不会泄漏；但本值是
 *  `transient || g_hold_req_latch`，**闩锁那一路是电平**：只要它的输入里混进
 *  一个常驻判据，广播就会被永久扣住、手机再也看不到设备。
 *  ⇒ 兜底不能省，见 `companion_adv_hold_watchdog()`。
 *
 *  （本注释 2026-09-23 更正：原文写的是"派生之后这类泄漏不可能发生"，
 *   `analysis/BLE_ADV_HOLD_LOCKOUT.md` 证伪 —— 闩锁混进常驻量后外设关机
 *  72 min 里广播全程停着。） */
static volatile bool    g_policy_adv_off;
static volatile bool    g_adv_active;

static volatile bool g_running = true;

/** 自动应答 Read 的 backing store。线上小端。 */

static uint8_t g_proto_ver_buf[2];
static uint8_t g_status_buf[COMPANION_STATUS_WIRE_LEN];
static uint8_t g_devinfo_buf[COMPANION_DEVINFO_LEN];
static bt_address_t g_classic_addr;

/** 广播载荷，启动时在已知 LE 地址后构建一次。 */

static uint8_t g_adv_data[7];
static uint8_t g_scan_rsp[31];
static uint8_t g_scan_rsp_len;

/** 最近一次 bt_gatts_notify() 错误，变化时只打一次日志。 */

static bt_status_t g_notify_err;
static char    g_dev_name[sizeof(CONFIG_MYVENDOR_PRODUCT_NAME) + 6];
static uint8_t g_phone_disc_reason;
/* 诊断：HCI 报的手机断开时刻。与 gatts_disconnect_callback 的到达时刻相减，
 * 用来判断事件是否丢失或延迟——上层"以为还连着"就是从这里看出来的。 */
static uint32_t g_hci_periph_disc_ms;

static void companion_stop_advertising(void);
static bool companion_phone_connected(void);
static bool adapter_is_usable(void);

/* 控制器活性读数（chips/sf32lb52/sf32lb52_bth4.c）：链路新鲜度看门狗用。
 * 板级目标能直接 include 芯片头，但这个函数的声明没进头部（芯片侧只有 .c），
 * 沿用本工程既有做法在调用点 extern。 */
extern bool myvendor_bth4_activity(uint32_t *rx_age_ms, uint32_t *pkts);
static void companion_apply_sensor_policy(bool on);
static void companion_supervisor_pump(uint32_t now_ms);
static int  companion_adapter_cycle(uint32_t now_ms);

bool sf32lb52_bt_hci_rx_stalled(uint32_t stall_ms);
void sf32lb52_bt_hci_rx_stall_clear(void);
bool sf32lb52_bt_hci_skip_sync(void);

/* zblue 侧 GATT 上下文池的哨兵（vela_override/zblue/patch_gatt_ccc_pool.py
 * 生成的那份 gatt.c 里定义）。只读两个哨兵字，不碰链表——链表是蓝牙栈自己的
 * 上下文在改，从别的线程走一遍会跟它抢。 */
void myvendor_gatt_ctx_checkpoint(void);

/* LCPU 控制器复位（chips/sf32lb52）：适配器 cycle 救不回来时的下一级。
 * 沿用本工程既有做法在调用点 extern（芯片侧没进头部）。 */
extern int sf32lb52_bt_controller_force_reset(void);

/****************************************************************************
 * Private Functions — time helpers
 ****************************************************************************/

/**
 * @brief CLOCK_MONOTONIC 毫秒。
 */
static uint32_t mono_ms(void)
{
  return myvendor_mono_ms();
}

/****************************************************************************
 * Private Functions — wire encoding
 ****************************************************************************/

/**
 * @brief 写入小端 uint16。
 */
static void put_le16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

/**
 * @brief 写入小端 uint32。
 */
static void put_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

/**
 * @brief 读出小端 uint32。
 */
static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief 读出小端 uint16。
 */
static uint16_t get_le16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/**
 * @brief 打包 Device Status（16 字节）加上 Classic BD_ADDR 尾。
 *
 * 显式打包而不 memcpy 结构体：这是协议边界，App 按字节解码。
 */
static void status_pack(const struct companion_status *st, uint8_t *out)
{
  put_le16(out + 0, st->flags);
  out[2] = st->battery_pct;
  out[3] = st->last_ctrl_op;
  put_le32(out + 4, st->storage_free_kb);
  put_le32(out + 8, st->uptime_sec);
  put_le32(out + 12, st->gnss_rx_count);
  memcpy(out + COMPANION_STATUS_LEN, g_classic_addr.addr,
         COMPANION_STATUS_ADDR_LEN);
}

/** @brief 固定长度字段写入字符串并 NUL 填充。 */
static void put_strpad(uint8_t *dst, size_t dst_len, const char *s)
{
  size_t n;

  memset(dst, 0, dst_len);
  if (s == NULL || dst_len == 0)
    {
      return;
    }

  n = strlen(s);
  if (n > dst_len)
    {
      n = dst_len;
    }

  memcpy(dst, s, n);
}

/** @brief 打包 0xFF1A Device Info 读响应。 */
static void devinfo_pack(uint8_t *out)
{
  out[0] = myvendor_boot_slot();
  put_strpad(out + 1, COMPANION_DEVINFO_SW_LEN, myvendor_sw_version());
  put_strpad(out + 1 + COMPANION_DEVINFO_SW_LEN, COMPANION_DEVINFO_HW_LEN,
             myvendor_hw_version());
  put_strpad(out + 1 + COMPANION_DEVINFO_SW_LEN + COMPANION_DEVINFO_HW_LEN,
             COMPANION_DEVINFO_NAME_LEN, myvendor_boot_fw_name());
  put_strpad(out + COMPANION_DEVINFO_LEN_V1, COMPANION_DEVINFO_BOOT_LEN,
             myvendor_boot_version());
}

/**
 * @brief 解包 24 字节的 0xFF14 GNSS Fix 写。
 */
static void gnss_fix_unpack(const uint8_t *in, struct companion_gnss_fix *fix)
{
  fix->lat_e7          = (int32_t)get_le32(in + 0);
  fix->lon_e7          = (int32_t)get_le32(in + 4);
  fix->alt_mm          = (int32_t)get_le32(in + 8);
  fix->speed_centi_kmh = get_le16(in + 12);
  fix->course_deg      = get_le16(in + 14);
  fix->utc_sec         = get_le32(in + 16);
  fix->fix_quality     = in[20];
  fix->satellites      = in[21];
  fix->hdop_x10        = in[22];
  fix->reserved        = in[23];
}

/****************************************************************************
 * Private Functions — status maintenance
 ****************************************************************************/

/**
 * @brief 更新状态标志（含 SENSOR_HR/CSC/CPS）。调用方须持锁。
 */
static void status_set_flags(uint16_t set, uint16_t clear)
{
  uint16_t before = g_ctx.status.flags;

  g_ctx.status.flags = (uint16_t)((before | set) & ~clear);
  if (g_ctx.status.flags != before)
    {
      g_ctx.status_dirty = true;
    }
}

/****************************************************************************
 * 私有函数 — GATT 写/CCC 回调
 *
 * 跑在蓝牙框架线程。只在锁内改 g_ctx 和置标志；notify 一律由主循环发出。
 * 传感器对端的写/CCC 一律吞掉（返回 length），避免误改手机会话。
 ****************************************************************************/

static bool companion_is_phone(const bt_address_t *addr);

/**
 * @brief Device Status CCC：App 订阅了 0xFF12 notify。
 */
static uint16_t status_ccc_changed(gatts_handle_t srv_handle,
                                   bt_address_t *addr, uint16_t attr_handle,
                                   const uint8_t *value, uint16_t length,
                                   uint16_t offset)
{
  bool enabled;

  (void)srv_handle;
  (void)attr_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  if (length < 1)
    {
      return 0;
    }

  enabled = (value[0] & GATT_CCC_NOTIFY) != 0;

  pthread_mutex_lock(&g_ctx.lock);
  g_ctx.ccc_enabled = enabled;

  /** 订阅后立刻发一份快照，避免 App 等到心跳。 */

  if (enabled)
    {
      g_ctx.notify_req = true;
    }

  pthread_mutex_unlock(&g_ctx.lock);

  /* 用 LOGI 而不是 LOGD：这是"App 到底订阅没有"的唯一现场证据，而 fs 那条
   * （`fs subscribe -> N`）早就是 LOGI —— 两条不同级别会让日志里只剩一半事实。
   * 症状"两边都显示已连接但没有数据"里，这条正是区分 **没连上** / **连上没订阅**
   * / **订阅了没数据** 的第一刀。 */
  LOGI("status subscribe -> %d", (int)enabled);
  return length;
}

/**
 * @brief Control 操作码名称，供 NSH `test ctrl`。
 */
static const char *ctrl_op_name(uint8_t op)
{
  switch (op)
    {
      case COMPANION_CTRL_NOP:
        return "NOP";
      case COMPANION_CTRL_START_RECORD:
        return "START_RECORD";
      case COMPANION_CTRL_STOP_RECORD:
        return "STOP_RECORD";
      case COMPANION_CTRL_PING:
        return "PING";
      case COMPANION_CTRL_TIME_SYNC:
        return "TIME_SYNC";
      case COMPANION_CTRL_FS_ABORT:
        return "FS_ABORT";
      case COMPANION_CTRL_SYNC_PREP:
        return "SYNC_PREP";
      case COMPANION_CTRL_NAV_STOP:
        return "NAV_STOP";
      case COMPANION_CTRL_PAIR_OPEN:
        return "PAIR_OPEN";
      case COMPANION_CTRL_PAIR_UNBIND:
        return "PAIR_UNBIND";
      default:
        return "?";
    }
}

/**
 * @brief 手机 TIME_SYNC：写 CLOCK_REALTIME（CONFIG_RTC 时同步硬件 RTC）并可选改时区。
 *
 * 不在 Control 锁内执行：settimeofday / persist 可能阻塞。
 */
static void companion_apply_time_sync(const uint8_t *value, uint16_t length)
{
  uint32_t utc;
  int16_t tz_min = 0;
  bool have_tz = false;
  struct timespec now;
  struct timeval tv;
  long long delta;

  if (value == NULL || length < COMPANION_CTRL_TIME_SYNC_MIN_LEN)
    {
      return;
    }

  utc = (uint32_t)value[1]
        | ((uint32_t)value[2] << 8)
        | ((uint32_t)value[3] << 16)
        | ((uint32_t)value[4] << 24);
  if (utc < COMPANION_TIME_MIN_UNIX || utc > COMPANION_TIME_MAX_UNIX)
    {
      LOGI("TIME_SYNC reject utc=%lu", (unsigned long)utc);
      return;
    }

  if (length >= COMPANION_CTRL_TIME_SYNC_LEN)
    {
      tz_min = (int16_t)((uint16_t)value[5] | ((uint16_t)value[6] << 8));
      if (tz_min >= MYVENDOR_DEVCTL_TZ_MIN && tz_min <= MYVENDOR_DEVCTL_TZ_MAX)
        {
          have_tz = true;
        }
    }

  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    {
      now.tv_sec = 0;
    }

  delta = (long long)now.tv_sec - (long long)utc;
  if (delta < 0)
    {
      delta = -delta;
    }

  if (now.tv_sec < (time_t)COMPANION_TIME_MIN_UNIX || delta >= 2)
    {
      tv.tv_sec = (time_t)utc;
      tv.tv_usec = 0;
      if (settimeofday(&tv, NULL) != 0)
        {
          syslog(LOG_WARNING, "ble_companion: TIME_SYNC settimeofday errno=%d\n",
                 errno);
        }
      else
        {
          LOGI("RTC sync utc=%lu tz=%d",
               (unsigned long)utc, have_tz ? (int)tz_min : (int)myvendor_devctl_tz_min_get());
        }
    }

  if (have_tz && tz_min != myvendor_devctl_tz_min_get())
    {
      (void)myvendor_devctl_tz_min_set(tz_min);
    }

  /* 时间辅助只有在「注入那一刻」才会进模组，而这里的准确时间正是模组需要的
   * 东西之一。所以：模组还没定位就立刻补一轮注入（顺带把文件里的位置辅助和
   * 星历一起送进去）。已经有定位就不打扰 —— 那时辅助无意义，白跑一轮 UART。 */
  {
    myvendor_sys_gnss_t fix;

    if (!(myvendor_gnss_get(&fix) && fix.valid))
      {
        myvendor_gnss_eph_reload();
        LOGI("TIME_SYNC -> eph reload (module has no fix yet)");
      }
  }
}

/**
 * @brief 处理 0xFF13 Control 写（ping、对时、录音、FS 中止、停导航）。
 */
static uint16_t control_on_write(gatts_handle_t srv_handle,
                                 bt_address_t *addr, uint16_t attr_handle,
                                 const uint8_t *value, uint16_t length,
                                 uint16_t offset)
{
  uint8_t op;
  uint8_t time_buf[COMPANION_CTRL_TIME_SYNC_LEN];
  uint16_t time_len = 0;

  (void)srv_handle;
  (void)attr_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  if (length < 1)
    {
      return 0;
    }

  op = value[0];

  pthread_mutex_lock(&g_ctx.lock);

  g_ctx.status.last_ctrl_op = op;

  switch (op)
    {
      case COMPANION_CTRL_NOP:
      case COMPANION_CTRL_PING:
        break;

      case COMPANION_CTRL_TIME_SYNC:
        if (length >= COMPANION_CTRL_TIME_SYNC_MIN_LEN)
          {
            time_len = length > COMPANION_CTRL_TIME_SYNC_LEN
                         ? COMPANION_CTRL_TIME_SYNC_LEN
                         : length;
            memcpy(time_buf, value, time_len);
          }
        break;

      case COMPANION_CTRL_START_RECORD:
      case COMPANION_CTRL_STOP_RECORD:
        /* 骑行会话由 MCU 判定并写入 0xFF12，不在此翻标志。 */
        break;

      case COMPANION_CTRL_FS_ABORT:
        companion_fs_abort();
        break;

      case COMPANION_CTRL_SYNC_PREP:
        break;

      case COMPANION_CTRL_NAV_STOP:
        break;

      /** app「传感器 / 配对」页：手动开配对窗口（手机自己发起的第一次配对就靠它）。
       *  载荷可选 `[secs:u16le]`，缺省 30 s。 */
      case COMPANION_CTRL_PAIR_OPEN:
        {
          unsigned secs = 30u;

          if (length >= COMPANION_CTRL_PAIR_OPEN_MIN_LEN + 2u)
            {
              secs = (unsigned)(value[1] | ((unsigned)value[2] << 8));
            }
          myvendor_pair_window_open(secs);
        }
        break;

      /** app 侧「解除绑定」：清密钥池 + 清手机记录（真解绑在 companion 线程里做）。 */
      case COMPANION_CTRL_PAIR_UNBIND:
        myvendor_pair_unbind_request();
        break;

      default:
        if (companion_bridge_test_ctrl_get())
          {
            LOGI("unknown control opcode 0x%02x", op);
          }
        break;
    }

  /** 每次 Control 写都用带 last_ctrl_op 的状态 notify 应答，作为 App 往返证明。 */

  g_ctx.notify_req = true;
  pthread_mutex_unlock(&g_ctx.lock);

  if (time_len != 0)
    {
      companion_apply_time_sync(time_buf, time_len);
    }

  if (companion_bridge_test_ctrl_get())
    {
      LOGI("ctrl %s (0x%02x) len=%u", ctrl_op_name(op), op, length);
    }

  return length;
}

/**
 * @brief 处理手机发来的 0xFF14 GNSS Fix 写。
 */
static uint16_t gnss_on_write(gatts_handle_t srv_handle, bt_address_t *addr,
                              uint16_t attr_handle, const uint8_t *value,
                              uint16_t length, uint16_t offset)
{
  struct companion_gnss_fix fix;
  uint32_t count;

  (void)srv_handle;
  (void)attr_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  if (length < COMPANION_GNSS_FIX_LEN)
    {
      LOGD("gnss write too short: %u", length);
      return length;
    }

  gnss_fix_unpack(value, &fix);
  companion_bridge_gnss_set(&fix, mono_ms());

  pthread_mutex_lock(&g_ctx.lock);
  g_ctx.last_fix     = fix;
  g_ctx.fix_valid    = fix.fix_quality != COMPANION_FIX_NONE;
  g_ctx.last_gnss_ms = mono_ms();
  count = ++g_ctx.status.gnss_rx_count;

  if (g_ctx.fix_valid)
    {
      status_set_flags(COMPANION_STATUS_GPS_PHONE, 0);
    }

  pthread_mutex_unlock(&g_ctx.lock);

  LOGD("gnss #%lu lat=%ld lon=%ld q=%u sats=%u",
       (unsigned long)count, (long)fix.lat_e7, (long)fix.lon_e7,
       fix.fix_quality, fix.satellites);
  (void)count;  /* only read by LOGD, which is compiled out by default */

  return length;
}

/**
 * @brief FS Data（0xFF16）的 Client Characteristic Configuration 写回调。
 *
 * @param[in] srv_handle   GATTS 服务句柄（未使用）。
 * @param[in] addr          写 CCC 的对端；须通过 @ref companion_is_phone。
 * @param[in] attr_handle   CCC 属性句柄（日志用，便于与驱动 `gatt: ccc write` 对齐）。
 * @param[in] value         CCC 值；bit0 = Notify。
 * @param[in] length        @p value 长度。
 * @param[in] offset        写偏移（未使用）。
 *
 * @return 已接受的字节数（与框架约定一致）。
 *
 * @note 若 @ref companion_is_phone 返回 false，打
 *       `fs ccc DROPPED (not phone) … addr=… peer=…`，App 仍可能以为 CCC 写成功。
 */
static uint16_t fs_ccc_changed(gatts_handle_t srv_handle, bt_address_t *addr,
                               uint16_t attr_handle, const uint8_t *value,
                               uint16_t length, uint16_t offset)
{
  (void)srv_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      char peer_str[BT_ADDR_STR_LENGTH];
      char addr_str[BT_ADDR_STR_LENGTH];

      bt_addr_ba2str(addr, addr_str);
      pthread_mutex_lock(&g_ctx.lock);
      if (g_ctx.connected)
        {
          bt_addr_ba2str(&g_ctx.peer, peer_str);
        }
      else
        {
          snprintf(peer_str, sizeof(peer_str), "(none)");
        }

      pthread_mutex_unlock(&g_ctx.lock);
      syslog(LOG_WARNING,
             "ble_companion: fs ccc DROPPED (not phone) h=0x%04x raw=%02x "
             "addr=%s peer=%s sensor=%d\n",
             (unsigned)attr_handle,
             (unsigned)(length > 0 ? value[0] : 0),
             addr_str, peer_str,
             ble_sensor_owns_addr(addr) ? 1 : 0);
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  if (length < 1)
    {
      return 0;
    }

  /* **把"哪一条 CCC、原值多少、判成什么"打全**（2026-09-20）。
   *
   * 现场：手机往 `h=0x001c` 反复写 `raw=0100`（值 1 = notify 开），而 FS 模块那边
   * 却一直报 `fs subscribe -> 0` —— 两端日志一比就是"客户端写了 1、设备判成 0"。
   * 但 `fs subscribe ->` 是 printf、没有时间戳，跟驱动侧的 `gatt: ccc write` 对不齐，
   * 没法确定是哪一次写把它打下去的（本回调**忽略了 attr_handle**，如果它也被
   * 别的特征的 CCC 写调用，一次 indications(值 2) 就会把 FS 的订阅判成关）。
   * 所以这里连 handle 和原始字节一起打，和驱动那行按 handle/raw 一一对上。 */
  syslog(LOG_INFO, "ble_companion: fs ccc h=0x%04x raw=%02x%02x -> %d\n",
         (unsigned)attr_handle,
         (unsigned)value[0],
         (unsigned)(length > 1 ? value[1] : 0),
         (value[0] & GATT_CCC_NOTIFY) != 0 ? 1 : 0);

  companion_fs_set_ccc((value[0] & GATT_CCC_NOTIFY) != 0);
  return length;
}

/**
 * @brief 接收 FS Command（0xFF15）写；实际 I/O 在 companion 线程泵出。
 *
 * @param[in] srv_handle   GATTS 服务句柄（未使用）。
 * @param[in] addr          写命令的对端；须通过 @ref companion_is_phone。
 * @param[in] attr_handle   属性句柄（未使用）。
 * @param[in] value         命令载荷。
 * @param[in] length        载荷长度。
 * @param[in] offset        写偏移（未使用）。
 *
 * @return 已接受的字节数；非手机对端时仍返回 @p length（帧已丢弃）。
 *
 * @see companion_fs_on_cmd()
 */
static uint16_t fs_cmd_on_write(gatts_handle_t srv_handle, bt_address_t *addr,
                                uint16_t attr_handle, const uint8_t *value,
                                uint16_t length, uint16_t offset)
{
  (void)srv_handle;
  (void)attr_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      char peer_str[BT_ADDR_STR_LENGTH];
      char addr_str[BT_ADDR_STR_LENGTH];

      bt_addr_ba2str(addr, addr_str);
      pthread_mutex_lock(&g_ctx.lock);
      if (g_ctx.connected)
        {
          bt_addr_ba2str(&g_ctx.peer, peer_str);
        }
      else
        {
          snprintf(peer_str, sizeof(peer_str), "(none)");
        }

      pthread_mutex_unlock(&g_ctx.lock);
      syslog(LOG_WARNING,
             "ble_companion: fs cmd DROPPED (not phone) len=%u cmd=0x%02x "
             "addr=%s peer=%s sensor=%d\n",
             (unsigned)length,
             (unsigned)(length > 0 ? value[0] : 0),
             addr_str, peer_str,
             ble_sensor_owns_addr(addr) ? 1 : 0);
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  return companion_fs_on_cmd(value, length);
}

/**
 * @brief 拷贝 0xFF16 FS Data 写。
 */
static uint16_t fs_data_on_write(gatts_handle_t srv_handle, bt_address_t *addr,
                                 uint16_t attr_handle, const uint8_t *value,
                                 uint16_t length, uint16_t offset)
{
  (void)srv_handle;
  (void)attr_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  return companion_fs_on_data(value, length);
}

/**
 * @brief 拷贝 0xFF17 Notification 写（可能分片）。
 */
static uint16_t notif_on_write(gatts_handle_t srv_handle, bt_address_t *addr,
                               uint16_t attr_handle, const uint8_t *value,
                               uint16_t length, uint16_t offset)
{
  (void)srv_handle;
  (void)attr_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  return companion_notif_on_write(value, length);
}

/**
 * @brief 拷贝 0xFF19 Nav Route 写（可能分片）。
 */
static uint16_t nav_on_write(gatts_handle_t srv_handle, bt_address_t *addr,
                             uint16_t attr_handle, const uint8_t *value,
                             uint16_t length, uint16_t offset)
{
  (void)srv_handle;
  (void)attr_handle;
  (void)offset;

  if (!companion_is_phone(addr))
    {
      return length;
    }

  /* 应用侧真的有控制帧进来 ⇒ 链路已加密（ENCRYPT 权限）⇒ 这就是那台已绑定手机，
   * 顺手把"手机记录"补上（记录空的话窗口一关准入就会拒它）。 */
  myvendor_pair_note_peer_if_secure(addr);

  return companion_nav_on_write(value, length);
}

/****************************************************************************
 * Private Data — attribute table
 ****************************************************************************/

static gatt_attr_db_t g_companion_attr_db[] =
{
  GATT_H_PRIMARY_SERVICE(BT_UUID_DECLARE_16(COMPANION_SERVICE_UUID16),
                         COMP_SERVICE_ID),

  /* 0xFF11 协议版本 — Read */

  GATT_H_CHARACTERISTIC_AUTO_RSP(
      BT_UUID_DECLARE_16(COMPANION_PROTO_VERSION_UUID16),
      GATT_PROP_READ, GATT_PERM_READ | GATT_PERM_ENCRYPT_REQUIRED,
      g_proto_ver_buf, sizeof(g_proto_ver_buf), COMP_PROTO_VER_ID),

  /* 0xFF12 设备状态 — Read + Notify（兼容旧 App）。
   * 新 App（2026-09）改轮询读，不再写 status CCC ⇒ 板端也发不出 status 心跳。
   * CCCD 仍保留：旧客户端订上才会走 notify 路径。
   */

  GATT_H_CHARACTERISTIC_AUTO_RSP(
      BT_UUID_DECLARE_16(COMPANION_DEVICE_STATUS_UUID16),
      GATT_PROP_READ | GATT_PROP_NOTIFY, GATT_PERM_READ | GATT_PERM_ENCRYPT_REQUIRED,
      g_status_buf, sizeof(g_status_buf), COMP_STATUS_ID),
  GATT_H_CCCD(GATT_PERM_READ | GATT_PERM_ENCRYPT_REQUIRED | GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED, status_ccc_changed,
              COMP_STATUS_CCC_ID),

  /* 0xFF13 控制 — Write */

  GATT_H_CHARACTERISTIC_USER_RSP(
      BT_UUID_DECLARE_16(COMPANION_CONTROL_UUID16),
      GATT_PROP_WRITE | GATT_PROP_WRITE_NR, GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED,
      NULL, control_on_write, COMP_CONTROL_ID),

  /* 0xFF14 GNSS Fix — Write + Write Without Response（App 以 1–5 Hz 用 WWR） */

  GATT_H_CHARACTERISTIC_USER_RSP(
      BT_UUID_DECLARE_16(COMPANION_GNSS_FIX_UUID16),
      GATT_PROP_WRITE | GATT_PROP_WRITE_NR, GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED,
      NULL, gnss_on_write, COMP_GNSS_ID),

  /* 0xFF15 FS 命令 — Write */

  GATT_H_CHARACTERISTIC_USER_RSP(
      BT_UUID_DECLARE_16(COMPANION_FS_COMMAND_UUID16),
      GATT_PROP_WRITE | GATT_PROP_WRITE_NR, GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED,
      NULL, fs_cmd_on_write, COMP_FS_CMD_ID),

  /* 0xFF16 FS 数据 — Write + Notify */

  GATT_H_CHARACTERISTIC_USER_RSP(
      BT_UUID_DECLARE_16(COMPANION_FS_DATA_UUID16),
      GATT_PROP_WRITE | GATT_PROP_WRITE_NR | GATT_PROP_NOTIFY, GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED,
      NULL, fs_data_on_write, COMP_FS_DATA_ID),
  GATT_H_CCCD(GATT_PERM_READ | GATT_PERM_ENCRYPT_REQUIRED | GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED, fs_ccc_changed,
              COMP_FS_DATA_CCC_ID),

  /* 0xFF17 通知 — Write（分片 TLV） */

  GATT_H_CHARACTERISTIC_USER_RSP(
      BT_UUID_DECLARE_16(COMPANION_NOTIFICATION_UUID16),
      GATT_PROP_WRITE | GATT_PROP_WRITE_NR, GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED,
      NULL, notif_on_write, COMP_NOTIF_ID),

  /* 0xFF19 导航路线 — Write（分片 lat/lon 点） */

  GATT_H_CHARACTERISTIC_USER_RSP(
      BT_UUID_DECLARE_16(COMPANION_NAV_ROUTE_UUID16),
      GATT_PROP_WRITE | GATT_PROP_WRITE_NR, GATT_PERM_WRITE | GATT_PERM_ENCRYPT_REQUIRED,
      NULL, nav_on_write, COMP_NAV_ID),

  /* 0xFF1A 设备信息 — Read（运行槽 / 软件 / 硬件 / 2SFBL 版本） */

  GATT_H_CHARACTERISTIC_AUTO_RSP(
      BT_UUID_DECLARE_16(COMPANION_DEVINFO_UUID16),
      GATT_PROP_READ, GATT_PERM_READ | GATT_PERM_ENCRYPT_REQUIRED,
      g_devinfo_buf, sizeof(g_devinfo_buf), COMP_DEVINFO_ID),
};

static gatt_srv_db_t g_companion_service_db =
{
  .attr_db  = g_companion_attr_db,
  .attr_num = sizeof(g_companion_attr_db) / sizeof(gatt_attr_db_t),
};

/****************************************************************************
 * Private Functions — adapter / GATT / advertiser callbacks
 ****************************************************************************/

/**
 * @brief 适配器状态回调（仅打日志）。
 */
static void on_adapter_state_changed(void *cookie, bt_adapter_state_t state)
{
  LOGD("adapter state -> %d", (int)state);
}

/**
 * @brief 配对显示/同意通知（服务层线程回调）。
 *
 * IO 能力钉成 NoInputNoOutput 之后（见 companion_le_io_cap_once()），唯一会来的
 * 是 `PAIR_TYPE_CONSENT` —— Just Works 那一步的"同意"。**必须回话**：SiFli SAL
 * 在 `bt_sal_le_set_io_capability()` 里顺带注册了 `pairing_confirm`，zblue 的
 * SMP 会停在 `SMP_FLAG_USER` 等这一拍（`bt_conn_auth_pairing_confirm()`），
 * 不回就一路等到 30 s 超时，对端看到的是"配对失败"。
 *
 * 板子是半透半反无触摸，没有可交互的配对界面，一律同意 —— 这也正是
 * NoInputNoOutput 的含义（"没有显示也没有键盘"，由用户侧决定信任）。
 */
static void on_pair_display(void *cookie, bt_address_t *addr,
                            bt_transport_t transport, bt_pair_type_t type,
                            uint32_t passkey)
{
  char addr_str[BT_ADDR_STR_LENGTH];
  bt_status_t st;

  (void)cookie;
  (void)passkey;

  if (transport != BT_TRANSPORT_BLE)
    {
      return;
    }

  /* 2026-09-23（审计 F9g）：同 gatts_connect_callback —— `addr` 先判空，
   * `bt_addr_ba2str()` 不解引用 NULL。 */
  if (addr == NULL)
    {
      LOGE("pair: display with null addr (type=%d), ignored", (int)type);
      return;
    }

  bt_addr_ba2str(addr, addr_str);

  if (type != PAIR_TYPE_CONSENT && type != PAIR_TYPE_PASSKEY_CONFIRMATION)
    {
      /* 到不了这里（NoInputNoOutput 不会走 PIN/数字比对），来了只记录不回应。 */
      LOGI("pair: display type=%d %s (no reply)", (int)type, addr_str);
      return;
    }

  /* 与 myvendor_bt_pair 的 pairing_confirm 一致：窗口外拒绝新配对。 */
  if (!myvendor_pair_window_active())
    {
      st = bt_device_set_pairing_confirmation(g_ins, addr, BT_TRANSPORT_BLE,
                                              false);
      LOGI("pair: consent %s -> reject (no window) ret=%d", addr_str, (int)st);
      return;
    }

  /* 名单已满且是新地址：拒绝（与 zblue pairing_confirm 同口径）。 */
  {
    extern bool myvendor_pair_reject_new_addr(const char *addr_str);

    if (myvendor_pair_reject_new_addr(addr_str))
      {
        st = bt_device_set_pairing_confirmation(g_ins, addr, BT_TRANSPORT_BLE,
                                                false);
        LOGI("pair: consent %s -> reject (slots full) ret=%d", addr_str,
             (int)st);
        return;
      }
  }

  st = bt_device_set_pairing_confirmation(g_ins, addr, BT_TRANSPORT_BLE, true);
  LOGI("pair: consent %s -> accept ret=%d", addr_str, (int)st);
}

/**
 * @brief 绑定状态变化：维护"已绑定手机"记录（`/mnt/kv/bt_phone.tsv`）。
 *
 * 这个文件给连接准入用（地址对得上才放行；窗口外无记录不广播）。
 * 注意 `BOND_STATE_NONE` 也会在**别的设备配对失败**时发出来，所以清记录必须
 * 核对地址（`myvendor_pair_note_bond(addr, false)` 内部会比对），否则一台陌生
 * 设备配对失败就会把手机那条记录抹掉、把手机挡在门外。
 */
static void on_bond_state_changed(void *cookie, bt_address_t *addr,
                                  bt_transport_t transport, bond_state_t state,
                                  bool is_ctkd)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  (void)cookie;
  (void)is_ctkd;

  if (transport != BT_TRANSPORT_BLE || addr == NULL)
    {
      return;
    }

  bt_addr_ba2str(addr, addr_str);
  LOGI("bond %s state=%d", addr_str, (int)state);

  if (state == BOND_STATE_BONDED)
    {
      myvendor_pair_note_bond(addr, true);
    }
  else if (state == BOND_STATE_NONE)
    {
      myvendor_pair_note_bond(addr, false);
    }
}

static const adapter_callbacks_t g_adapter_cbs =
{
  .on_adapter_state_changed = on_adapter_state_changed,
  .on_pair_display          = on_pair_display,
  .on_bond_state_changed    = on_bond_state_changed,
};

/**
 * @brief 判断 GATTS 回调对端是否为手机 Companion 链路。
 *
 * 用于过滤 HR/CSC/CPS 等传感器地址上的 connect / disconnect / MTU /
 * notify-complete / 特征写 / CCC，避免污染 `g_ctx.peer` 与 Companion FS。
 *
 * zblue 的 `att_mtu_updated` 是全局的：心率连接会把 ATT 23 报到 GATTS。
 * 若误把该 MTU 套到 Companion，FS 载荷会变成约 17 字节，文件传输失败。
 *
 * @param[in] addr  GATTS 回调给出的对端 LE 地址；可为 @c NULL。
 *
 * @retval true   视为手机链路，调用方可继续处理 FS/MTU/状态。
 * @retval false  传感器地址、@c NULL，或不应改动手机 peer 的其它情况。
 *
 * @note 连接回调尚未置位（`g_ctx.connected == false`）时，非传感器对端仍
 *       视为手机，以便首包 MTU/CCC 能进；FS 侧另需已 `companion_fs_on_connect`
 *       且地址与 FS peer 一致。
 *
 * @note **RPA / 身份地址（2026-09-21）**
 *       连接回调常记下可解析私有地址（RPA），加密后 ATT 写与 CCC 回调可能
 *       换成身份地址（或反过来）。严格 `memcmp(g_ctx.peer)` 会把
 *       `fs ccc` / `fs cmd` 全部标成 `DROPPED (not phone)`，App 侧表现为
 *       「CCCD=01 却 0 帧超时」。本产品单手机槽：已有手机链路且对端非传感器
 *       时，认作同一台手机，刷新 `g_ctx.peer` 并同步
 *       `companion_fs_on_connect()`（否则 `bt_gatts_notify` 仍打旧地址）。
 *       成功刷新时打日志：`phone peer refresh <old> -> <new> (RPA/identity)`。
 *
 * @warning 不得在持有 `g_ctx.lock` 时调用 `companion_fs_on_connect()`
 *          （该路径自带锁，会死锁）。
 *
 * @see companion_fs_on_connect(), ble_sensor_owns_addr()
 * @see docs/ble/pairing_and_radio.md §1.3
 */
static bool companion_is_phone(const bt_address_t *addr)
{
  bool yes = false;
  bool refresh = false;
  bt_address_t old_peer;
  bt_address_t new_peer;

  if (addr == NULL || ble_sensor_owns_addr(addr))
    {
      return false;
    }

  pthread_mutex_lock(&g_ctx.lock);
  if (!g_ctx.connected)
    {
      /* 连接回调尚未置位：非传感器对端仍视为手机（首包 MTU/CCC）。 */
      yes = true;
    }
  else if (memcmp(g_ctx.peer.addr, addr->addr, BT_ADDR_LENGTH) == 0)
    {
      yes = true;
    }
  else
    {
      /* 单手机槽：RPA ↔ 身份地址切换，刷新 peer。 */
      old_peer = g_ctx.peer;
      g_ctx.peer = *addr;
      new_peer = *addr;
      yes = true;
      refresh = true;
    }

  pthread_mutex_unlock(&g_ctx.lock);

  if (refresh)
    {
      char a1[BT_ADDR_STR_LENGTH];
      char a2[BT_ADDR_STR_LENGTH];

      bt_addr_ba2str(&old_peer, a1);
      bt_addr_ba2str(&new_peer, a2);
      syslog(LOG_INFO,
             "ble_companion: phone peer refresh %s -> %s (RPA/identity)\n",
             a1, a2);
      companion_fs_on_connect(&new_peer);
    }

  return yes;
}

/**
 * @brief GATTS 连接：忽略传感器地址；把 Companion FS 绑到手机。
 */
static void gatts_connect_callback(gatts_handle_t srv_handle,
                                   bt_address_t *addr)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  (void)srv_handle;

  /* 2026-09-23（审计 F9g）：**`addr` 必须显式判空**。
   *
   * 不能指望下面那句 `ble_sensor_owns_addr()` 兜住 —— 它对 NULL 返回
   * **false**（"不是传感器的地址"），于是 NULL 会一路走到 `addr->addr`／
   * `bt_addr_ba2str(addr, …)` 才炸。本文件同族的 `on_bond_state_changed()`
   * 就是 `|| addr == NULL` 一起判的，这里漏了。
   *
   * 本回调跑在 framework 的 service-loop 线程上，崩在这里等于整条 BT 服务
   * 线程没了（比"漏一次连接"严重得多），所以宁可不处理这一拍。 */
  if (addr == NULL)
    {
      LOGE("gatts connect callback: null addr, ignored");
      return;
    }

  if (ble_sensor_owns_addr(addr))
    {
      return;
    }

  /* **准入（2026-09-20；2026-09-22 抗旧手机占坑）**：没配对就不接受。
   * 只有"配对窗口内"或"本机记录的已绑定手机 / 短 grace 等 RPA 升密"才放行；
   * 其余在这里请求断开（异步，AUTH_FAIL）——被拒时**不要**往下走标记
   * connected，也**不要**停 ADV（停播在 admit 成功之后），否则旧手机 A
   * 占坑时 B 扫不到设备。详见 myvendor_bt_pair.c（短 grace + 拒连冷却）。 */
  if (!myvendor_pair_admit(addr))
    {
      return;
    }

  pthread_mutex_lock(&g_ctx.lock);
  if (g_ctx.connected &&
      memcmp(g_ctx.peer.addr, addr->addr, BT_ADDR_LENGTH) != 0)
    {
      pthread_mutex_unlock(&g_ctx.lock);
      return;
    }

  g_ctx.connected = true;
  g_ctx.peer      = *addr;
  pthread_mutex_unlock(&g_ctx.lock);

  companion_bridge_phone_set(true);
  /* 统一状态：手机链路（读侧只拿快照，不持内部锁）。 */
  companion_ble_state_set_phone(true);
  companion_stop_advertising();
  companion_fs_on_connect(addr);

  bt_addr_ba2str(addr, addr_str);
  LOGI("phone connected %s", addr_str);
}

/**
 * @brief zblue 连接：仅 Peripheral（手机）收紧 7.5 ms / PHY / DLE。
 *
 * 传感器（central）保持 BT_LE_CONN_PARAM_DEFAULT。7.5 ms / 1 s + PHY+DLE
 * 会和 Android 的 45 ms 更新打架（HCI 0x16），发现前就断链。
 */
static void le_connected(struct bt_conn *conn, uint8_t err)
{
  struct bt_conn_info info;

  if (err != 0 || bt_conn_get_info(conn, &info) != 0 ||
      info.type != BT_CONN_TYPE_LE)
    {
      return;
    }

  if (info.role == BT_CONN_ROLE_PERIPHERAL)
    {
      int ret;

      /* **控制器没就绪就别发这一族命令。**
       *
       * 连接刚建立时下发的 PM/参数命令（param update / PHY / data len）正是
       * 被 NOT_READY 控制器吞掉的那一族：命令发出去没有 Command Status，
       * `txop` 一直挂着 → diag 判 `cmdsilent` → 复位适配器 → 把刚连上的链路
       * 踢掉（现场：手机连上 4.4 s 内无任何 HCI 收发，`status=2`）。
       *
       * 跳过它们的代价≈0：实测即使请求成功，间隔最后也会回到 30 ms
       * （`conn_upd#2 itv=24(30.00 ms)`），所以**不发不吃亏**，而发了会喂出
       * 一串假 cmdsilent。 */
      if (!adapter_is_usable())
        {
          static uint32_t last_log_ms;

          if (last_log_ms == 0 ||
              myvendor_mono_elapsed_ms(mono_ms(), last_log_ms) >= 10000u)
            {
              last_log_ms = mono_ms();
              syslog(LOG_WARNING,
                     "ble_companion: PM tuning skipped, adapter not ready\n");
            }

          return;
        }

      /** 参数取**厂商 SDK 的"高性能档"原值**，不要再自己发明。
       *
       *  `ble_connection_manager.h` 里三档是
       *    高性能 12/12（15 ms）/ latency 0 / timeout 600（6 s）
       *    均衡   24~40（30~50 ms）/ 0 / 600
       *    低功耗 80~100（100~125 ms）/ 2 / 600
       *  并注明了"iOS 支持的最大监督超时是 6 s"。厂商的
       *  `file_transfer` 例程也是在传输期间强制切到高性能档。
       *
       *  以前这里要的是 `6/6/0/400`：**间隔 7.5 ms 是 BLE 规定的下限**，
       *  手机基本不会给（本文件自己的注释就记着"间隔最后也会回到 30 ms"），
       *  而 **4 s 监督超时比厂商的 6 s 短三分之一** —— 在一块"一张射频同时
       *  跑手机广播 + 传感器扫描"的板子上，缺的正是这两秒余量。
       *
       *  自证伪：若日志里 `le interval=` 长期停在 15 ms 且丢链变多，说明本档
       *  对这台手机偏激进 ⇒ 改投均衡档（24~40 / 0 / 600）。
       */
      ret = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(12, 12, 0, 600));
      LOGD("le interval 15 ms ret=%d", ret);
      (void)ret;

#if defined(CONFIG_BT_USER_PHY_UPDATE)
      ret = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
      LOGD("le phy 2M ret=%d", ret);
#endif

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
      ret = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
      LOGD("le dle max ret=%d", ret);
#endif
    }
}

/**
 * @brief HCI 断链：central 角色备份通知传感器健康监控。
 *
 * GATTS 仍走 @ref gatts_disconnect_callback。此处只补传感器，避免
 * 手机断链被处理两次。
 */
static void le_disconnected(struct bt_conn *conn, uint8_t reason)
{
  struct bt_conn_info info;
  bt_address_t addr;

  if (bt_conn_get_info(conn, &info) != 0 || info.type != BT_CONN_TYPE_LE)
    {
      return;
    }

  memset(&addr, 0, sizeof(addr));
  if (info.le.dst != NULL)
    {
      memcpy(addr.addr, info.le.dst->a.val, BT_ADDR_LENGTH);
    }
  else if (info.le.remote != NULL)
    {
      memcpy(addr.addr, info.le.remote->a.val, BT_ADDR_LENGTH);
    }

  LOGD("le disc role=%s reason=0x%02x",
       info.role == BT_CONN_ROLE_CENTRAL ? "central" : "peripheral",
       (unsigned)reason);

  /* 诊断：把 HCI 侧的断开与 GATTS 侧的状态并排打出来（syslog 级别，不受
   * LOGD 被编译掉的影响）。关键观察：peripheral 角色在这里只记 reason，
   * 不做任何清理，g_ctx.connected 只能由 gatts_disconnect_callback 清。
   * 若本行之后没有 "phone disconnected" 且这里 ctx_connected=1，说明上层
   * 卡在"以为还连着"——广播看门狗的 expected 恒为 false，不再恢复。
   *
   * ctx_* 描述的是**手机**那条链路（本模块在手机上做 peripheral），所以只在
   * peripheral 角色下打；central 角色（心率/速度传感器）只打对端地址 ——
   * 2026-09-17 那次心率带掉线（hdl 0x0003）就是被这行读成了"手机
   * ctx_connected=1 ccc=1 mtu=244"。 */
  {
    char peer[BT_ADDR_STR_LENGTH];

    bt_addr_ba2str(&addr, peer);

    if (info.role == BT_CONN_ROLE_PERIPHERAL)
      {
        syslog(LOG_INFO, "ble_companion: hci_disc role=peripheral reason=0x%02x peer=%s ctx_connected=%d ccc=%d mtu=%u adv_active=%d adv_restart=%d hold=%d fs_busy=%d\n",
               (unsigned)reason, peer,
               companion_phone_connected() ? 1 : 0,
               g_ctx.ccc_enabled ? 1 : 0, (unsigned)g_ctx.mtu,
               g_adv_active ? 1 : 0, g_ctx.adv_restart ? 1 : 0,
               g_hold_le_adv ? 1 : 0, companion_fs_busy() ? 1 : 0);
      }
    else
      {
        syslog(LOG_INFO, "ble_companion: hci_disc role=central reason=0x%02x peer=%s\n",
               (unsigned)reason, peer);
      }
  }

  if (info.role == BT_CONN_ROLE_PERIPHERAL)
    {
      g_phone_disc_reason = reason;
      g_hci_periph_disc_ms = myvendor_mono_ms();
    }

  if (info.role == BT_CONN_ROLE_CENTRAL)
    {
      ble_sensor_on_hci_disconnected(&addr, reason);
    }
}

/**
 * @brief 接受对端连接参数请求（打印间隔）。
 */
/**
 * @brief 这条链路如果已经加密、且不是传感器，就把地址交给配对模块记一笔。
 *
 * 只在**协议栈线程的回调里**做（有合法的 `conn` 指针、不遍历连接链表）—— 以前是
 * companion 线程每秒 `bt_conn_foreach()` 扫一遍，撞上协议栈正在增删的链表就 panic
 * （coredump n574：`pc=bt_conn_ref→stlex mmfar=0x124 pid=35 irq=1`）。落盘（/mnt/kv）
 * 也不在这里做，只置标志，`myvendor_pair_peer_poll()` 每拍取走。
 */
static void le_note_peer_if_secure(struct bt_conn *conn)
{
  extern bool         ble_sensor_owns_addr(const bt_address_t *addr);
  extern void         myvendor_pair_note_secure_peer(const bt_address_t *addr,
                                                     uint8_t addr_type);
  const bt_addr_le_t *dst;
  bt_address_t        a;

  if (conn == NULL || bt_conn_get_security(conn) < BT_SECURITY_L2)
    {
      return;
    }

  dst = bt_conn_get_dst(conn);
  if (dst == NULL)
    {
      return;
    }

  memset(&a, 0, sizeof(a));
  memcpy(a.addr, dst->a.val, BT_ADDR_LENGTH);
  if (ble_sensor_owns_addr(&a))
    {
      return;                     /* 本机做中央连出去的那几条是传感器，不是手机 */
    }

  myvendor_pair_note_secure_peer(&a, (uint8_t)dst->type);
}

/**
 * @brief 链路加密等级变了 —— 「配对完成」最可靠的信号。
 *
 * 手机连上只读不写时：bond 回调不落盘、控制帧路径也不会动，只有这条一定会来
 * （HCI Encrypt Change → `bt_conn_security_changed()` → 本回调）。
 */
static void le_security_changed(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err)
{
  if (err != BT_SECURITY_ERR_SUCCESS || level < BT_SECURITY_L2)
    {
      return;
    }
  LOGI("le secure lvl=%u", (unsigned)level);
  le_note_peer_if_secure(conn);   /* 兜底再判一次 level，顺便滤掉传感器 */
}

/**
 * @brief 对端请求调整连接参数：全部接受（只打日志）。
 *
 * @param conn  对端连接（协议栈线程回调，仅本函数内有效）。
 * @param param 请求的参数（间隔/延迟/超时）。
 * @return 恒为 true（同意）。
 */
static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
  (void)conn;
  LOGD("le param req %u-%u lat=%u to=%u",
       param->interval_min, param->interval_max,
       param->latency, param->timeout);
  return true;
}

/**
 * @brief 连接参数协商结果：打日志，并**兜底补一次手机记录**。
 *
 * 正常情况下"链路已加密"由 `le_security_changed()` 送达；协议栈万一没把 HCI
 * Encrypt Change 报上来，这条（手机连上后必做参数更新）仍会来，而那时链路通常已经
 * 加密 ⇒ 一样能把 `/mnt/kv/bt_phone.tsv` 补上。
 *
 * @param conn     对端连接（协议栈线程回调，仅本函数内有效）。
 * @param interval 协商后的间隔（1.25 ms 单位）。
 * @param latency  从机延迟。
 * @param timeout  监督超时（10 ms 单位）。
 */
static void le_param_updated(struct bt_conn *conn, uint16_t interval,
                             uint16_t latency, uint16_t timeout)
{
  LOGD("le interval=%u (%.2f ms) latency=%u timeout=%u",
       interval, (double)interval * 1.25, latency, timeout);

  /* 兜底：协议栈若没把 Encrypt Change 报到 `security_changed`，这条仍会来
   * （手机连上后必做连接参数更新），此时链路通常已经加密 → 一样能把记录补上。 */
  le_note_peer_if_secure(conn);
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
/**
 * @brief 打印 PHY 更新（手机 2M 路径）。
 */
static void le_phy_updated(struct bt_conn *conn,
                           struct bt_conn_le_phy_info *param)
{
  (void)conn;
  LOGD("le phy tx=%u rx=%u", param->tx_phy, param->rx_phy);
}
#endif

static struct bt_conn_cb g_le_conn_cb =
{
  .connected         = le_connected,
  .disconnected      = le_disconnected,
  .security_changed  = le_security_changed,
  .le_param_req      = le_param_req,
  .le_param_updated  = le_param_updated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
  .le_phy_updated  = le_phy_updated,
#endif
};

/**
 * @brief 手机链路没了之后的统一收尾（GATTS 回调 / 状态对账两条路共用）。
 *
 * @param addr 对端地址（只用于打日志）。
 * @param why  触发原因（"gatts" 或 "link-gone"）。
 */
static void companion_phone_gone(const bt_address_t *addr, const char *why)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  pthread_mutex_lock(&g_ctx.lock);
  g_ctx.connected   = false;
  g_ctx.ccc_enabled = false;
  g_ctx.mtu         = 0;
  g_ctx.fix_valid   = false;
  g_ctx.adv_restart = true;

  /** 手机 GNSS 随链路一起消失。 */

  status_set_flags(0, COMPANION_STATUS_GPS_PHONE);
  pthread_mutex_unlock(&g_ctx.lock);

  companion_bridge_phone_set(false);
  /* 统一状态：手机链路没了（真断开 / adapter cycle 丢掉 / 停机）。 */
  companion_ble_state_set_phone(false);
  companion_fs_on_disconnect();
  companion_downlink_reset();
  companion_bridge_gnss_clear();

  bt_addr_ba2str(addr, addr_str);
  if (strcmp(why, "gatts") == 0)
    {
      LOGI("phone disconnected %s reason=0x%02x",
           addr_str, (unsigned)g_phone_disc_reason);
    }
  else
    {
      LOGI("phone link gone %s (%s)", addr_str, why);
    }

  /* 诊断：HCI 早已报过断开的场景（peripheral 角色）在这里会显示一个非 0
   * 的 hci_first_ms，说明 GATTS 事件是后到的；若这一行始终不出现，就说明
   * gatts_disconnect_callback 根本没被调用——那正是 connected 卡死的成因。
   * （2026-09-20：那条路补了 `companion_reconcile_phone()` 对账，见主循环。） */
  syslog(LOG_INFO,
         "ble_companion: gatts_disc %s reason=0x%02x hci_first_ms=%u "
         "ctx_connected_after=0 why=%s\n",
         addr_str, (unsigned)g_phone_disc_reason,
         g_hci_periph_disc_ms != 0
           ? (unsigned)myvendor_mono_elapsed_ms(myvendor_mono_ms(),
                                                g_hci_periph_disc_ms)
           : 0u,
         why);
  g_hci_periph_disc_ms = 0;
}

/**
 * @brief 手机 GATTS 断开：丢掉 FS/GNSS/下行；忽略传感器对端。
 */
static void gatts_disconnect_callback(gatts_handle_t srv_handle,
                                      bt_address_t *addr)
{
  (void)srv_handle;
  if (!companion_is_phone(addr))
    {
      return;
    }

  companion_phone_gone(addr, "gatts");
}

/**
 * @brief GATTS 属性表已注册。
 */
static void gatts_attr_table_added(gatts_handle_t srv_handle,
                                   gatt_status_t status, uint16_t attr_handle)
{
  LOGD("attr table added handle=0x%04x status=%d", attr_handle, (int)status);
}

/**
 * @brief 仅对手机对端把 ATT MTU 应用到 Companion FS。
 *
 * 传感器对端到不了 companion_fs_set_mtu()。载荷已经是 ATT_MTU-3。
 */
static void gatts_mtu_changed(gatts_handle_t srv_handle, bt_address_t *addr,
                              uint32_t mtu)
{
  (void)srv_handle;
  if (!companion_is_phone(addr))
    {
      return;
    }

  pthread_mutex_lock(&g_ctx.lock);
  g_ctx.mtu = mtu;
  pthread_mutex_unlock(&g_ctx.lock);

  companion_fs_set_mtu(addr, mtu);

  LOGD("MTU -> %lu (payload %lu)", (unsigned long)mtu,
       (unsigned long)(mtu > 3 ? mtu - 3 : 0));
}

/**
 * @brief 仅处理手机的 FS notify 完成（额度 / inflight）。
 */
static void gatts_notify_complete(gatts_handle_t srv_handle,
                                  bt_address_t *addr, gatt_status_t status,
                                  uint16_t attr_handle)
{
  (void)srv_handle;
  if (!companion_is_phone(addr))
    {
      return;
    }

  companion_fs_on_notify_complete(attr_handle, status);
}

static const gatts_callbacks_t g_gatts_cbs =
{
  .size                 = sizeof(gatts_callbacks_t),
  .on_connected         = gatts_connect_callback,
  .on_disconnected      = gatts_disconnect_callback,
  .on_attr_table_added  = gatts_attr_table_added,
  .on_notify_complete   = gatts_notify_complete,
  .on_mtu_changed       = gatts_mtu_changed,
};

/****************************************************************************
 * Private Functions — startup
 ****************************************************************************/

/**
 * @brief 等待 /dev/ttyHCI0（HCPU zblue + LCPU H4）。
 */
static int wait_hci_device(int timeout_secs)
{
  int i;

  for (i = 0; i < timeout_secs * 10; i++)
    {
      if (!g_running)
        {
          return -EINTR;
        }

      if (access(HCI_DEV_PATH, F_OK) == 0)
        {
          return 0;
        }

      if ((i % 10) == 0)
        {
          companion_bridge_heartbeat();
        }

      usleep(100 * 1000);
    }

  LOGE("%s missing — enable CONFIG_UART_BTH4 and rebuild", HCI_DEV_PATH);
  return -ENOENT;
}

/**
 * @brief 适配器已有可用 LE 地址。
 */
static bool adapter_is_usable(void)
{
  bt_adapter_state_t st = bt_adapter_get_state(g_ins);

  /** 双模板落到 STATE_ON。纯 BLE（无 BREDR）停在 BLE_ON，到不了 Classic ON。 */

  return st == BT_ADAPTER_STATE_ON || st == BT_ADAPTER_STATE_BLE_ON;
}

/**
 * @brief 适配器此刻是否"不可用 / 正在被我们自己恢复"（diag 用，见头文件说明）。
 */
bool companion_ble_recovering(void)
{
  struct ble_supervisor_stats st;

  if (!adapter_is_usable())
    {
      return true;
    }

  if (ble_supervisor_cycle_requested())
    {
      return true;
    }

  memset(&st, 0, sizeof(st));
  ble_supervisor_get_stats(&st);

  return st.recovering || st.cycle_pending;
}

/**
 * @brief 等到适配器变为 ON。
 */
static int wait_adapter_ready(int timeout_secs)
{
  int i;

  for (i = 0; i < timeout_secs * 10; i++)
    {
      if (!g_running)
        {
          return -EINTR;
        }

      if (adapter_is_usable())
        {
          return 0;
        }

      if ((i % 10) == 0)
        {
          companion_bridge_heartbeat();
        }

      usleep(100 * 1000);
    }

  LOGE("adapter not ready, state=%d (expect ON or BLE_ON)",
       (int)bt_adapter_get_state(g_ins));
  return -ETIMEDOUT;
}

/**
 * @brief 构建 ADV + 扫描响应（Helm One-XXXX + 0xFF10 UUID）。
 *
 * 广播服务 UUID，让 App 按 0xFF10 过滤而不是按名字（用户可改名）。
 * 名字放扫描响应：两者塞不进一个 31 字节 legacy PDU。
 * 名字与 USB/MTP 共用 myvendor_identity_name()（产品名 + MAC 后两字节）。
 */
static void build_adv_payload(const bt_address_t *le_addr)
{
  const char *id = myvendor_identity_name();
  size_t name_len;

  (void)le_addr;
  snprintf(g_dev_name, sizeof(g_dev_name), "%s", id);

  g_adv_data[0] = 0x02;             /**< Flags AD 长度 */
  g_adv_data[1] = 0x01;             /**< AD 类型：Flags */
  g_adv_data[2] = 0x06;             /**< LE General + 不支持 BR/EDR */
  g_adv_data[3] = 0x03;             /**< UUID16 列表 AD 长度 */
  g_adv_data[4] = 0x03;             /**< AD 类型：完整 16 位 UUID 列表 */
  put_le16(&g_adv_data[5], COMPANION_SERVICE_UUID16);

  name_len = strlen(g_dev_name);
  if (name_len > sizeof(g_scan_rsp) - 2)
    {
      name_len = sizeof(g_scan_rsp) - 2;
    }

  g_scan_rsp[0] = (uint8_t)(name_len + 1);
  g_scan_rsp[1] = 0x09;             /**< AD 类型：完整本地名 */
  memcpy(&g_scan_rsp[2], g_dev_name, name_len);
  g_scan_rsp_len = (uint8_t)(name_len + 2);
}

/**
 * @brief 框架广播已启动 / 失败的回调（status=0 才算真正在播）。
 *
 * 失败分支现在会打 `adv start failed status=<码> id=<id>`：以前只有编译掉的 LOGD，
 * 现场只剩 supervisor 的 `request adapter cycle reason=adv`，分不清是"射频被共用
 * 吞掉（status=2）"还是"参数被拒"（2026-09-20 加的诊断）。
 *
 * @param adv    广播对象（失败时用于清 `g_adv`）。
 * @param adv_id 广播 id。
 * @param status 框架给的启动结果，0 = `BT_ADV_STATUS_SUCCESS`。
 */
static void on_advertising_start(bt_advertiser_t *adv, uint8_t adv_id,
                                 uint8_t status)
{
  LOGD("advertising started id=%u status=%u name=%s",
       adv_id, status, g_dev_name);
  if (status == BT_ADV_STATUS_SUCCESS)
    {
      /* **广播"起来"要可见**：以前成功只有 LOGD（编译掉），于是"心率计连着时
       * 广播没了"这件事在日志里看不出来（现场 2026-09-18 16:08）。 */
      if (!g_adv_active)
        {
          syslog(LOG_INFO, "ble_companion: adv active\n");
        }

      g_adv_active = true;
      return;
    }

  g_adv_active = false;
  if (g_adv == adv)
    {
      g_adv = NULL;
    }

  /* **失败带状态码**：以前这里只有开头那句 LOGD（编译掉），现场只剩
   * supervisor 的 `request adapter cycle reason=adv` —— 分不清是"射频被吞
   * （status=2，有链路时的固有代价）"还是"参数被拒/地址类型不对"。 */
  syslog(LOG_WARNING,
         "ble_companion: adv start failed status=%u id=%u\n",
         (unsigned)status, (unsigned)adv_id);

  /* **有链路在时，"广播起不来"不算故障。**
   *
   * 手机或心率连着时广播失败多半是"连接与广播共用射频"的固有代价（LCPU 会吞掉
   * `Set_Adv_Enable`，`status=2`），而 cycle 是**整栈复位**：它会把这条正在工作的
   * 链路踢掉。现场（2026-09-18 15:52）：手机每次连上后约 42 秒被 cycle 踢一次
   * （`hci_disc reason=0x00`），用户的感受就是"彻底连不上"。
   * 所以只有**什么都没连**时才上报故障 —— 那时 cycle 没有任何代价。 */
  /* 判据是"**确实有链路**"（READY），不是"正在扫描/建链" ——
   * 后者为真时链路其实还没建立，放行等于把恢复路径一起堵掉
   * （2026-09-18：手机和心率计同时连不上，三条路全被堵）。 */
  if (!companion_phone_connected() && ble_sensor_ready_mask() == 0u)
    {
      ble_supervisor_report(BLE_SUPERVISOR_FAULT_ADV, mono_ms());
    }
  else
    {
      static uint32_t last_log_ms;

      if (last_log_ms == 0 ||
          myvendor_mono_elapsed_ms(mono_ms(), last_log_ms) >= 30000u)
        {
          last_log_ms = mono_ms();
          syslog(LOG_WARNING,
                 "ble_companion: adv start failed while a link is up "
                 "(not counted as a fault)\n");
        }
    }

  pthread_mutex_lock(&g_ctx.lock);
  if (!g_ctx.connected && !g_hold_le_adv && !g_policy_adv_off)
    {
      g_ctx.adv_restart = true;
    }

  pthread_mutex_unlock(&g_ctx.lock);
}

/**
 * @brief 框架广播已停；句柄随后被 advertising.c 释放。
 */
static void on_advertising_stopped(bt_advertiser_t *adv, uint8_t adv_id)
{

  if (g_adv_active)
    {
      syslog(LOG_INFO, "ble_companion: adv down\n");
    }

  LOGD("advertising stopped id=%u", adv_id);
  g_adv_active = false;
  if (g_adv == adv)
    {
      g_adv = NULL;
    }
}

static const advertiser_callback_t g_adv_cbs =
{
  sizeof(g_adv_cbs),
  on_advertising_start,
  on_advertising_stopped,
};

/**
 * @brief 停框架广播（观察者占用、手机已连、关机）。
 */
static void companion_stop_advertising(void)
{
  if (g_adv == NULL || g_ins == NULL)
    {
      g_adv = NULL;
      return;
    }

  bt_le_stop_advertising(g_ins, g_adv);
  g_adv = NULL;
  g_adv_active = false;
}

/**
 * @brief 配对窗口刚打开：让广播按新档位**立刻重来**（可发现快广播）。
 *
 * 由 `ctl pair open [秒]`（myvendor_bt_pair.c 里 extern 调）和窗口过期后的下一拍用。
 * 只开窗口不重播是不够的：`companion_start_advertising()` 在 `g_adv != NULL`
 * 时会直接早退，档位永远不切（2026-09-20 现场：`ctl pair open 30`
 * 之后看不到任何广播变化）。
 */
void companion_adv_restart_for_pair_window(void)
{
  pthread_mutex_lock(&g_ctx.lock);
  g_ctx.adv_restart = true;
  pthread_mutex_unlock(&g_ctx.lock);

  companion_stop_advertising();
  LOGI("pair window -> adv restart");
}

/**
 * @brief 配对窗口开着才允许 LE 新绑定；窗口外只保留已绑定回连（LTK）。
 *
 * 旧注释"不要调 set_bondable_le(false)"已过时：Companion GATT 现已 ENCRYPT，
 * 窗口外若仍 bondable，任意扫到的中心都能发起 Just Works（2026-09-21）。
 */
static void companion_sync_bondable(void)
{
  bool on = myvendor_pair_window_active();
  bt_status_t st;

  if (g_ins == NULL)
    {
      return;
    }

  st = bt_device_set_bondable_le(g_ins, on);
  LOGI("bondable_le=%d ret=%d", on ? 1 : 0, (int)st);
}

/**
 * @brief 把 SAL 的 LE IO 能力钉成 NoInputNoOutput（每台设备做一次，成功后不再重试）。
 *
 * **配对码/PIN 那一档不在 zblue 的 auth 回调里**：`bt_conn_auth_cb_register()`
 * 写的是 `hdev->bt_auth`（经典那条），而 SMP 协商读的是 `hdev->le_auth`
 * （smp.c 的 `get_io_capa()` ← `latch_auth_cb()`）—— 所以在那儿去掉
 * `passkey_display` 没用（2026-09-20 现场：去掉后手机照样提配对码）。正式入口在 SiFli SAL：
 * `bt_adapter_set_le_io_capability()` → `adapter_set_le_io_capability()`
 * → `bt_sal_le_set_io_capability()`（sal_adapter_le_interface.c 里按能力给
 * `g_conn_auth_cbs` 选回调）。本机既没有屏幕也没有键盘，声明 NoInputNoOutput
 * 之后协商固定走 Just Works。
 *
 * 副作用已接住：SAL 那条路顺带给 `pairing_confirm` 注册了回调（Just Works 的
 * "同意"要 app 回话），由 `on_pair_display()` 自动同意。
 *
 * 放在 `companion_start_advertising()` 里是因为：它在每次 adapter enable/cycle
 * 之后都会跑到，`g_ins` 在作用域内，而且此时 SAL 已经注册完自己的回调 —— 谁后
 * 注册谁生效（`le_auth` 全局，重复注册是 -EALREADY，所以只做一次）。
 */
static void companion_le_io_cap_once(void)
{
  static bool done;
  static uint8_t tries;
  bt_status_t st;

  if (done || g_ins == NULL || tries >= 8u)
    {
      return;
    }

  tries++;
  st = bt_adapter_set_le_io_capability(g_ins, BT_IO_CAPABILITY_NOINPUTNOOUTPUT);
  if (st == BT_STATUS_SUCCESS)
    {
      done = true;
    }

  LOGI("le io cap: NoInputOutput ret=%d (try %u)", (int)st, (unsigned)tries);
}

/**
 * @brief 把落盘的配对密钥（含 IRK/LTK）塞回 zblue 的 key_pool。
 *
 * 必须在"开广播之前 + 普通线程里"做：密钥池在 zblue 的 `bt_keys.c`，恢复过程要读
 * `/mnt/kv/bt_keys.bin`（open/read/close，还会拿 FS 互斥、吃 ~1 KB 栈）。
 * **不要放进 zblue 的 `bt_keys_find*()` 里做** —— 那条路会被
 * `bt_lookup_id_addr()` 从连接/扫描事件处理里调用，而本板的事件处理跑在 BT 收包
 * 的**中断上下文**：实测每次手机连上必崩（`arm_memfault.c:136 irq=1`）。
 * 放在这里正好早于任何对端能连上我们（连上必须先看到我们的广播）。
 *
 * 幂等（zblue 侧有 loaded 标记，adapter cycle 清池后会重新置位），限次重试兜住
 * "调得比 /mnt/kv 挂载早"的情况。
 */
static void companion_restore_keys(void)
{
  extern void myvendor_keys_kv_load_all(void);
  extern void myvendor_pair_prune_against_keys(void);

  myvendor_keys_kv_load_all();

  /** **密钥池刚恢复完、还没开广播 ⇒ 这是唯一能安全遍历密钥池的窗口。**
   *
   *  用它修剪 `bt_phone.tsv` 里混进去的陈旧 RPA（配对那次连接建立时密钥池里
   *  还没有 IRK，只能记下轮换 RPA；它永远匹配不上、只白占名额）。
   *  详情与约束见 `myvendor_bt_pair.c` 的 `myvendor_pair_prune_against_keys()`。
   *
   *  必须**紧跟** `myvendor_keys_kv_load_all()`：池还是空的时候这个函数自己
   *  会跳过（不清表），所以这里不用额外判。 */
  myvendor_pair_prune_against_keys();
}

/**
 * @brief 启动 LE 广播（观察者占用或已有句柄时除外）。
 *
 * 走 `bt_le_start_advertising`。SF32 的 Ext Adv 由板卡 SAL override
 * 换成 legacy HCI；成功时回调 `advertising started id=… status=0`。
 */
static int companion_start_advertising(void)
{
  ble_adv_params_t params;

  companion_le_io_cap_once();
  companion_restore_keys();

  if (g_hold_le_adv || g_policy_adv_off)
    {
      return 0;
    }

  if (g_adv != NULL)
    {
      return 0;
    }

  if (g_ins == NULL)
    {
      return -1;
    }

  /* **适配器不可用就别发 Set_Adv_Enable。**
   *
   * 对还没就绪（或刚复位）的控制器发它：① 被吞掉（`status=2`，4.4 s 无任何 HCI
   * 收发）；② 堆进 hci_cmd_pool（只有 4 个缓冲，现场见过 `net_buf: pool … empty
   * 2000 ms`）。保留调用方的 `adv_restart`，下一拍再试；日志限频 10 s ——
   * 起不来时它一秒一次，只是刷屏。 */
  if (!adapter_is_usable())
    {
      static uint32_t last_defer_log;

      if (last_defer_log == 0 ||
          myvendor_mono_elapsed_ms(mono_ms(), last_defer_log) >= 10000u)
        {
          last_defer_log = mono_ms();
          syslog(LOG_WARNING,
                 "ble_companion: adv start deferred: adapter not ready\n");
        }

      return -1;
    }

  /* **不要再在开广播前先停扫描。** 原实现是 2026-09-18 一次未验证的试验改动（注释里自己就写着"安全可删"）。
   *
   * 现场：cycle 之后 sensor 侧刚启动扫描，这里一句 scan_stop 就让扫描当场结束 ——
   * `[scan] done seen=0` 出现在 `connect on` 之后 26 ms。扫描需要 1~3 s 才能发现
   * 心率计，被这么打断就永远扫不到；而实测规律是"启用后约 4 s 内没有链路，控制器
   * 就进 NOT_READY"，于是成环：无链路 -> 控制器死 -> cycle -> 扫描刚起就停 -> 仍无链路。
   *
   * 射频交替本来有自己的分时（SENSOR_RADIO_ADV_WINDOW / SCAN_WINDOW，见
   * sensor_reconnect_radio_pump），本函数不该抢在它前面把对方掐掉：需要广播而扫描
   * 正占射频时，让分时窗口到点自然交接；adv-enable 万一被吞，cmdsilent 那两个门控
   * 现在会容忍它并等下一拍。
   */
  memset(&params, 0, sizeof(params));
  params.adv_type        = BT_LE_LEGACY_ADV_IND;
  params.peer_addr_type  = BT_LE_ADDR_TYPE_PUBLIC;
  params.own_addr_type   = BT_LE_ADDR_TYPE_PUBLIC;
  params.interval        = ADV_INTERVAL_UNITS;
  params.tx_power        = 0;
  params.channel_map     = BT_LE_ADV_CHANNEL_DEFAULT;
  params.filter_policy   = BT_LE_ADV_FILTER_WHITE_LIST_FOR_NONE;

  /* **广播三档（2026-09-22：定向 ADV 关闭）**：
   *   ① 配对窗口内 → 可发现 + 可连接 + 带本地名（快广播）+ bondable。
   *   ② 有"已绑定手机"记录 + 窗口外 → 可连接 + 快广播 + **不带本地名**
   *      （已绑定 app 用 UUID/直连回连）。bondable=false。
   *   ③ 没记录 + 窗口外 → **完全不广播**。
   *
   * **定向 ADV（ADV_DIRECT_IND + DIR_ADDR_RPA）2026-09-22 实测不支持**：
   * SF32 上稳定 `adv start failed status=2`（框架 1 s 超时），且不会回落成功；
   * 已从产品路径拿掉。旧手机占坑靠 `myvendor_pair_admit` 短 grace + 拒连冷却。
   * 函数 `myvendor_pair_directed_peer()` 仍保留备查，勿再挂上。
   */
  {
    uint8_t rsp_len = g_scan_rsp_len;

    if (myvendor_pair_window_active())
      {
        LOGI("adv discoverable (pair window; %u ms)", (unsigned)ADV_INTERVAL_MS);
      }
    else if (myvendor_pair_has_phone_record())
      {
        rsp_len = 0;
        LOGI("adv connectable unnamed (paired; %u ms)",
             (unsigned)ADV_INTERVAL_MS);
      }
    else
      {
        static uint32_t last_quiet_log_ms;

        if (last_quiet_log_ms == 0 ||
            myvendor_mono_elapsed_ms(mono_ms(), last_quiet_log_ms) >= 10000u)
          {
            last_quiet_log_ms = mono_ms();
            LOGI("adv off (unpaired, no window; ctl pair open 恢复广播)");
          }

        return 0;
      }

  companion_bridge_phase_set("hci_adv");
  g_adv = bt_le_start_advertising(g_ins, &params,
                                  g_adv_data, sizeof(g_adv_data),
                                  g_scan_rsp, rsp_len,
                                  (advertiser_callback_t *)&g_adv_cbs);
  }
  if (g_adv == NULL)
    {
#ifdef CONFIG_MYVENDOR_BLE_LOG
      static uint32_t last_adv_fail_ms;
      uint32_t now = mono_ms();

      if (last_adv_fail_ms == 0 ||
          myvendor_mono_elapsed_ms(now, last_adv_fail_ms) >= 2000u)
        {
          last_adv_fail_ms = now;
          BLE_LOG("start_adv failed skip=%d hold=%d radio_off=%d",
                  sf32lb52_bt_hci_skip_sync() ? 1 : 0,
                  g_hold_le_adv ? 1 : 0,
                  g_policy_adv_off ? 1 : 0);
        }
#endif
      /* 双角色时间片里 start 失败是常态，由 12 s ADV stall 再升级。 */
      return -1;
    }

  pthread_mutex_lock(&g_ctx.lock);
  g_ctx.adv_restart = false;
  pthread_mutex_unlock(&g_ctx.lock);
  LOGD("advertising requested name=%s", g_dev_name);
  return 0;
}

/* ── 板内 BLE 状态：一份状态、一把锁 ─────────────────────────────────────
 *
 * 在这之前 BLE 的状态散在几处、各带自己的锁：本模块的 `g_ctx.lock`（手机链路）、
 * `ble_sensor` 的 `g_lock`（外设链路）、supervisor（故障计数/cycle）、以及 adv/scan
 * 的让位握手；外面（diag / UI / GNSS / app）还各拿各的锁往里读 —— 这就是"锁乱发"：
 * 谁也说不出某一刻"BLE 整体是什么状态"，锁序也不在一个地方。
 *
 * 从这一版起：**状态只有这一份、锁只有这一把**，判定逻辑留在本模块内部。外部只做
 * 两件事：
 *   1. **上报事件**：手机连/断、sensor 连/断（`companion_ble_state_set_*`）；
 *   2. **读快照**：`companion_ble_state_snapshot()` 拿一份拷贝，全程不持有 BLE 锁
 *      —— 与本工程"共享读一律走快照"的既有约定一致。
 *
 * 下一步（未做）：把"要不要广播/扫描、要不要恢复"的判定从 supervisor / ble_sensor
 * 收进这里；本版先把状态与锁统一，行为与之前一致。
 */
static pthread_mutex_t g_ble_state_lock = PTHREAD_MUTEX_INITIALIZER;

/* owner 自己写的手机链路状态（外部只投递事件、只读快照）。 */
void companion_ble_state_set_phone(bool linked);
static companion_ble_state_t g_ble_state;

void companion_ble_state_set_phone(bool linked)
{
  uint32_t now;

  pthread_mutex_lock(&g_ble_state_lock);
  if (g_ble_state.phone_linked != linked)
    {
      now = myvendor_mono_ms();
      g_ble_state.phone_linked   = linked;
      g_ble_state.phone_since_ms = linked ? (now != 0u ? now : 1u) : 0u;
      g_ble_state.gen++;
    }

  pthread_mutex_unlock(&g_ble_state_lock);
}

/** @brief 纯判定：从状态字段算出射频用途（快照与查询共用）。
 *
 * 方案：① 广播与扫描互斥；② 手机没连 + 有 sensor 想连 → 交替；③ 手机已连 →
 * 不需要广播（sensor 侧仍间隔扫）；④ 手机没连 + 没有 sensor 想连 → 只广播。
 * 「节奏」（交替多快、不在场时退避多久、静止时是否找）不在这里 —— 那是执行侧
 * （ble_sensor）的事，本函数只回答"此刻该干什么"。
 */
static companion_ble_radio_state_t radio_state_from(const companion_ble_state_t *st)
{
  if (st->recovering)
    {
      return COMPANION_RADIO_RECOVERING;
    }

  if (st->phone_linked)
    {
      return COMPANION_RADIO_SCAN_ONLY;
    }

  return st->sensor_want ? COMPANION_RADIO_ALTERNATE : COMPANION_RADIO_ADV_ONLY;
}

/** @brief 本线程内部的裁决与状态日志（外部只读快照的 `radio_state` 字段）。 */
static companion_ble_radio_state_t companion_ble_radio_state(void)
{
  static companion_ble_radio_state_t last;
  companion_ble_state_t st;
  companion_ble_radio_state_t r;

  companion_ble_state_snapshot(&st);
  r = radio_state_from(&st);

  if (r != last)
    {
      syslog(LOG_INFO, "ble_companion: [radio] state %u -> %u (phone=%d want=%d seen=%d adv=%d hold=%d gen=%u)\n",
             (unsigned)last, (unsigned)r, st.phone_linked ? 1 : 0,
             st.sensor_want ? 1 : 0, st.sensor_seen ? 1 : 0,
             st.adv_active ? 1 : 0, st.transient ? 1 : 0, (unsigned)st.gen);
      last = r;
    }

  return r;
}

/** 合流信箱：状态事件是"电平"语义，所以每类只保留最新值；sensor 链路按槽位。
 * 投递方短临界区、不阻塞（拿不到锁就丢并计数，下一拍会再报）。 */
static pthread_mutex_t g_ble_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_ev_linked[COMPANION_BLE_SENSOR_N];
static bool g_ev_want;
static bool g_ev_seen;
static bool g_ev_transient;
static uint32_t g_ble_ev_dropped;

void companion_ble_post(companion_ble_ev_type_t type, int a, bool b, bool c)
{
  if (pthread_mutex_trylock(&g_ble_ev_lock) != 0)
    {
      /* 丢一次是良性的：事件是电平语义，下一拍会再报同一个值。但静默丢弃会让
       * "状态怎么没跟上"变成谜团，所以第一次丢时留一行。 */
      if (g_ble_ev_dropped++ == 0u)
        {
          syslog(LOG_WARNING, "ble_companion: ble event box busy, first event dropped\n");
        }

      return;
    }

  switch (type)
    {
      case COMPANION_BLE_EV_SENSOR_LINK:
        if (a >= 0 && a < COMPANION_BLE_SENSOR_N)
          {
            g_ev_linked[a] = b;
          }
        break;

      case COMPANION_BLE_EV_SENSOR_WANT:
        g_ev_want = b;
        g_ev_seen = c;
        break;

      case COMPANION_BLE_EV_TRANSIENT:
        g_ev_transient = b;
        break;

      default:
        break;
    }

  pthread_mutex_unlock(&g_ble_ev_lock);
}

/**
 * @brief 消费信箱：**状态机跑在 owner 线程内**的入口（pump 顶部调用）。
 *
 * 外部只投递，这里才改状态；裁决随后由快照发布（`companion_ble_state_snapshot()`
 * 的 `radio_state` 字段），别的线程只读快照。
 */
static void companion_ble_drain(void)
{
  bool linked[COMPANION_BLE_SENSOR_N];
  bool want;
  bool seen;
  bool transient;
  bool recovering;
  int  k;

  /* recovering 不用外部投递：它是 supervisor 自己的判定（适配器可用性、
   * 故障计数、排队中的 cycle），在这里问一次即可。少了这一步，RECOVERING
   * 永远到不了判定侧 —— 恢复期间照旧去扫、去广播，只会把恢复拖长。
   * 注意在拿状态锁**之前**问：锁序保持"外部锁 → 状态锁"单向，不嵌套。 */
  recovering = companion_ble_recovering();

  pthread_mutex_lock(&g_ble_ev_lock);
  for (k = 0; k < COMPANION_BLE_SENSOR_N; k++)
    {
      linked[k] = g_ev_linked[k];
    }

  want      = g_ev_want;
  seen      = g_ev_seen;
  transient = g_ev_transient;
  pthread_mutex_unlock(&g_ble_ev_lock);

  pthread_mutex_lock(&g_ble_state_lock);
  for (k = 0; k < COMPANION_BLE_SENSOR_N; k++)
    {
      if (g_ble_state.sensor_linked[k] != linked[k])
        {
          g_ble_state.sensor_linked[k] = linked[k];
          g_ble_state.gen++;
        }
    }

  if (g_ble_state.sensor_want != want || g_ble_state.sensor_seen != seen)
    {
      g_ble_state.sensor_want = want;
      g_ble_state.sensor_seen = seen;
      g_ble_state.gen++;
    }

  if (g_ble_state.transient != transient)
    {
      g_ble_state.transient = transient;
      g_ble_state.gen++;
    }

  if (g_ble_state.recovering != recovering)
    {
      g_ble_state.recovering = recovering;
      g_ble_state.gen++;
    }

  /* 判定侧读的本地镜像：避免在持有 g_ctx.lock 时去拿状态锁。
   * 闩锁（显式请求）与 transient（正在用）取并：只信 transient 会和
   * 传感器状态机互等，见 g_hold_req_latch 注释。 */
  g_hold_le_adv = transient || g_hold_req_latch;
  pthread_mutex_unlock(&g_ble_state_lock);
}

void companion_ble_state_snapshot(companion_ble_state_t *out)
{
  if (out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_ble_state_lock);
  *out = g_ble_state;
  out->radio_state = (uint8_t)radio_state_from(&g_ble_state);
  pthread_mutex_unlock(&g_ble_state_lock);
}

/**
 * @brief 手机断开后若未被传感器扫描占用则重启 ADV。
 */
static void companion_ensure_advertising(void)
{
  bool need = false;

  pthread_mutex_lock(&g_ctx.lock);
  /* 需要广播 = 统一状态机说此刻该广播（ADV_ONLY / ALTERNATE），再带上本模块的
   * 策略闸与瞬时让位。 */
  {
    companion_ble_radio_state_t rs = companion_ble_radio_state();

    need = g_ctx.adv_restart && !g_ctx.connected && !g_hold_le_adv &&
           !g_policy_adv_off &&
           (rs == COMPANION_RADIO_ADV_ONLY || rs == COMPANION_RADIO_ALTERNATE);
  }
  pthread_mutex_unlock(&g_ctx.lock);

  if (!need)
    {
      /* **"广播没起"必须能说原因。** 现场（2026-09-18 16:08）：心率计连着、
       * 广播没了，日志里什么都看不出来。让位（`g_hold_le_adv`）是最常见的那
       * 一条 —— 它现在是**派生**的：sensor 侧正在扫描/建链才为真。 */
      if (g_ctx.adv_restart && !g_ctx.connected && !g_policy_adv_off &&
          g_hold_le_adv)
        {
          static uint32_t last_held_log_ms;

          if (last_held_log_ms == 0 ||
              myvendor_mono_elapsed_ms(mono_ms(), last_held_log_ms) >= 10000u)
            {
              last_held_log_ms = mono_ms();
              syslog(LOG_WARNING,
                     "ble_companion: adv held by sensor "
                     "(no adv while the sensor uses the radio)\n");
            }
        }

      return;
    }

  LOGD("restarting advertising after disconnect");
  if (companion_start_advertising() != 0)
    {
      pthread_mutex_lock(&g_ctx.lock);
      g_ctx.adv_restart = true;
      pthread_mutex_unlock(&g_ctx.lock);
    }
}

static bool companion_phone_connected(void)
{
  bool connected;

  pthread_mutex_lock(&g_ctx.lock);
  connected = g_ctx.connected;
  pthread_mutex_unlock(&g_ctx.lock);
  return connected;
}

/**
 * @brief 同类故障达阈值、或 HCI RX 持续 -ENOMEM 后，disable/enable adapter。
 *
 * 普通 ADV/SCAN 故障在手机已连接时跳过，保留 pending 待断开后重试。
 * HCI 卡住时连接状态机也是僵尸，必须照样 cycle。
 */
static int companion_adapter_cycle(uint32_t now_ms)
{
  bt_status_t ret;
  bool sensor_on;
  bool radio_on;
  bt_address_t peer;

  /** **先走统一收尾，不能只清 `g_ctx` 那三个字段。**
   *
   *  2026-09-23 修：原来这里只清 `connected/ccc_enabled/mtu`，其余
   *  （`companion_ble_state_set_phone(false)` / `companion_bridge_phone_set(false)` /
   *  `companion_fs_on_disconnect()` / `companion_downlink_reset()` /
   *  `companion_bridge_gnss_clear()` / 清 `COMPANION_STATUS_GPS_PHONE` 位）
   *  全靠 `bt_adapter_disable()` 过程中的 GATTS 断开回调去兜。
   *
   *  问题是 cycle 恰恰常在**栈已经不回话**时才被发起（HCI 静默 / 收不到
   *  Disconnection Complete）—— 那种时候回调不会来，于是：
   *
   *    `g_ble_state.phone_linked` 粘住 true
   *      → `radio_state_from()` 返回 SCAN_ONLY
   *      → `companion_ensure_advertising()` 里 `need` 恒为假
   *      → **设备再也不广播、手机连不回来**（只能 `ctl radio on` 或重启）。
   *
   *  而本来为「断开回调丢失」准备的那条对账（主循环
   *  `linked = !g_ctx.connected || myvendor_pair_phone_link_up()`）
   *  **在这个场景下恰好被关掉**：cycle 已经把 `connected` 清成 false，
   *  `linked` 就恒为真 ⇒ 对账永不触发。也就是说这条路原先没有任何兜底。
   *  现场抓到过一次：`[radio] state 1 -> 0 (phone=1 …)`，此后整个开机
   *  再没有 `[radio] state 0 -> …` 的转换。
   *
   *  直接复用 `companion_phone_gone()`：它本来就是「几类收尾共用一个漏斗」，
   *  cycle 是又一个调用方。重复调用是幂等的（断开回调后到时会再走一次，
   *  顺带还能从日志里看出"回调到底有没有来"）。
   */
  pthread_mutex_lock(&g_ctx.lock);
  peer = g_ctx.peer;
  pthread_mutex_unlock(&g_ctx.lock);
  companion_phone_gone(&peer, "adapter-cycle");

  sf32lb52_bt_hci_rx_stall_clear();

  ble_supervisor_cycle_begin(now_ms);

  sensor_on = ble_sensor_is_started() || companion_bridge_policy_sensor();
  radio_on  = !g_policy_adv_off;
  BLE_LOG("adapter cycle begin skip=%d sensor=%d radio=%d",
          sf32lb52_bt_hci_skip_sync() ? 1 : 0,
          sensor_on ? 1 : 0, radio_on ? 1 : 0);

  companion_bridge_phase_set("sensor_stop");
  ble_sensor_stop();
  companion_bridge_phase_set("hci_adv_stop");
  companion_stop_advertising();

  companion_bridge_phase_set("adapter_disable");
  ret = bt_adapter_disable(g_ins);
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("adapter disable failed: %d", (int)ret);
      ble_supervisor_report(BLE_SUPERVISOR_FAULT_ADAPTER, now_ms);
      ble_supervisor_cycle_complete(false, now_ms);
      return -1;
    }

  {
    uint32_t left = ADAPTER_CYCLE_OFF_MS;

    while (left > 0 && g_running)
      {
        uint32_t slice = left > 200u ? 200u : left;

        companion_bridge_heartbeat();
        usleep(slice * 1000u);
        left -= slice;
      }

    if (!g_running)
      {
        ble_supervisor_cycle_complete(false, now_ms);
        return -EINTR;
      }
  }

  LOGI("adapter off done, enable after %u ms",
       (unsigned)ADAPTER_CYCLE_OFF_MS);

  ret = bt_adapter_enable(g_ins);
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("adapter enable failed: %d", (int)ret);
      ble_supervisor_report(BLE_SUPERVISOR_FAULT_ADAPTER, now_ms);
      ble_supervisor_cycle_complete(false, now_ms);
      return -1;
    }

  if (wait_adapter_ready(ADAPTER_CYCLE_WAIT_SECS) != 0)
    {
      ble_supervisor_report(BLE_SUPERVISOR_FAULT_ADAPTER, now_ms);
      ble_supervisor_cycle_complete(false, now_ms);
      return -1;
    }

  (void)bt_conn_cb_register(&g_le_conn_cb);

  if (radio_on && !companion_phone_connected())
    {
      pthread_mutex_lock(&g_ctx.lock);
      g_ctx.adv_restart = true;
      pthread_mutex_unlock(&g_ctx.lock);
    }

  if (sensor_on)
    {
      companion_apply_sensor_policy(true);
    }

  ble_supervisor_cycle_complete(true, now_ms);
  LOGI("adapter cycle done sensor=%d radio=%d",
       (int)sensor_on, (int)radio_on);
  return 0;
}

/**
 * @brief 链路新鲜度看门狗：连着、但**控制器已经不再回话**时强制拆链重广播。
 *
 * @details
 * 这条针对的是"两边 UI 都显示连着、实际已经死了"那个形状（在案缺陷 #2：
 * `Disconnection Complete` 被丢掉 → `g_ctx.connected` 粘滞为真 →
 * `ble_supervisor_watch_advertising()` 的 `expected = !phone_connected()` 恒为假
 * → 广播停滞看门狗永不触发 → 直到重启都不恢复）。
 *
 * **不看 `connected` 这个粘滞位，看"控制器还会不会说话"**：手机连着时，
 * 主循环会周期性 `bt_gatts_set_attr_value` 刷新 0xFF12，且 FS/控制写也会
 * 产生 HCI；另当旧 App 仍订了 status CCC 时还会每
 * `COMPANION_STATUS_HEARTBEAT_SEC`(5 s) 发一次 status notify。控制器收到后
 * 必须回 `Number Of Completed Packets`（HCI 事件，走 RX 方向）。所以
 * `myvendor_bth4_activity()` 给出的"距最近一次收包"年龄超过
 * `COMPANION_LINK_STALE_MS`，就说明控制器/LCPU 不再说话。
 *
 * **2026-09-21**：App 已改为轮询读 0xFF12、且 0xFF16 按需订 —— 不能再要求
 * `ccc_enabled`（status CCC）才武装看门狗，否则新 App 永远不触发链路判死。
 *
 * 这个判据与 App 行为无关（手机闲着也会有 HCI 活动），所以不会误判"手机没在推定位"；
 * 与 `phone silent` 那个探针（看 0xFF14 写入）互补，后者只在"手机 App 侧不说话"
 * 时才有意义。
 *
 * 判死后的动作与 `gatts_disconnect_callback` 同源（那条路可能永远不来，所以这里
 * 自己走一遍，幂等）：
 *   1. `bt_gatts_disconnect()` 把 LL 真正断掉 —— 只清标志不拆链的话，作为外设
 *      我们**无法重新广播**（连着的从设备不能广播），恢复就无从谈起；
 *   2. 清 connected/ccc/mtu/fix_valid + `adv_restart = true`，主循环据此重广播；
 *   3. 手机 GNSS / 下行队列 / FS 会话一并收尾。
 *
 * @param now_ms 当前单调毫秒。
 */
static void companion_link_watchdog(uint32_t now_ms)
{
  uint32_t rx_age = 0;
  uint32_t pkts = 0;
  bt_address_t peer;
  bool armed;

  pthread_mutex_lock(&g_ctx.lock);
  armed = g_ctx.connected;
  peer  = g_ctx.peer;
  pthread_mutex_unlock(&g_ctx.lock);

  if (!armed)
    {
      return;
    }

  /* 还没收到过任何 HCI 包（刚开机）不判死：没有基线可言。 */
  if (!myvendor_bth4_activity(&rx_age, &pkts) ||
      rx_age < COMPANION_LINK_STALE_MS)
    {
      return;
    }

  LOGE("link stale %u ms: no HCI from controller (pkts=%u) — "
       "force disconnect + re-advertise",
       (unsigned)rx_age, (unsigned)pkts);

  if (g_gatts_handle != NULL)
    {
      bt_status_t dr = bt_gatts_disconnect(g_gatts_handle, &peer);

      if (dr != BT_STATUS_SUCCESS)
        {
          LOGE("bt_gatts_disconnect failed: %d (继续按判死处理)", (int)dr);
        }
    }

  pthread_mutex_lock(&g_ctx.lock);
  g_ctx.connected   = false;
  g_ctx.ccc_enabled = false;
  g_ctx.mtu         = 0;
  g_ctx.fix_valid   = false;
  g_ctx.adv_restart = true;
  status_set_flags(0, COMPANION_STATUS_GPS_PHONE);
  pthread_mutex_unlock(&g_ctx.lock);

  companion_bridge_phone_set(false);
  /* 统一状态：手机链路没了（真断开 / adapter cycle 丢掉 / 停机）。 */
  companion_ble_state_set_phone(false);
  companion_fs_on_disconnect();
  companion_downlink_reset();
  companion_bridge_gnss_clear();
}

/**
 * @brief 监控广播停滞、HCI RX 卡住与 adapter cycle 请求。
 */
/* 上一轮 adapter cycle 结束时控制器已经产生了多少个 HCI 包。
 *
 * 用来挡住"cycle 刚做完就再开一次"的自喂活锁：2026-09-18 现场里
 * `adapter cycle done` 之后 1 ms 就出现 `request adapter cycle reason=adapter
 * (immediate)`（因为 rx_stalled 仍为真），cycle 计数一路涨到 4，每一轮都把刚
 * 恢复的窗口踢掉。判据是**控制器活过的证据**：包计数必须往前走过一格，否则
 * 说明这个 cycle 根本没救活它 —— 这时候该等 supervisor 的失败重试 / diag 升级，
 * 而不是立刻再来一轮。 */
static uint32_t s_cycle_pkts;
static bool     s_cycle_life_wait;

/** @brief 记录"刚做完一次 cycle"，要求下一次 rxstall 之前先看到新包。 */
static void companion_cycle_note_done(void)
{
  uint32_t rx_age = 0;
  uint32_t pkts = 0;

  if (myvendor_bth4_activity(&rx_age, &pkts))
    {
      s_cycle_pkts = pkts;
      s_cycle_life_wait = true;
    }
}

/** @brief 自上次 cycle 之后控制器是否还没说过话（一个包都没有 = true）。 */
static bool companion_cycle_await_life(void)
{
  uint32_t rx_age = 0;
  uint32_t pkts = 0;

  if (!s_cycle_life_wait)
    {
      return false;
    }

  if (!myvendor_bth4_activity(&rx_age, &pkts) || pkts != s_cycle_pkts)
    {
      /* 有包了（或读数不可用）：这次 stall 是新的事实，放行。 */
      s_cycle_life_wait = false;
      return false;
    }

  return true;
}

/** @brief 适配器 cycle 连续失败 ⇒ 换级：复位 LCPU 控制器，再跑一次 cycle。
 *
 * 现场（2026-09-18）：控制器静默之后 adapter cycle 每次都
 * `adapter not ready, state=0`，60 s 一轮重试了 23 次、22 分钟 —— 手机和心率计
 * 一直断着，自己永远回不来。原因很简单：cycle 只会 disable/enable 适配器，
 * 而卡死的是**控制器**；那一档必须 `sf32lb52_bt_controller_force_reset()`，
 * 这也是 diag 路径一直在用、唯一真正把 LCPU 救回来过的手段。
 *
 * 限频 5 分钟一次：复位本身不能变成新的循环（n005 的教训是"在 cycle 飞行中
 * 再叠一次复位"会崩，所以这里由 supervisor 决定、本线程独占执行）。
 */
static void companion_controller_recover(uint32_t now_ms)
{
  static uint32_t last_ms;
  bt_status_t ret;

  if (last_ms != 0 &&
      myvendor_mono_elapsed_ms(now_ms, last_ms) < 300000u)
    {
      return;
    }

  last_ms = now_ms;
  LOGI("BLE recovery: forcing LCPU controller reset (adapter cycles failing)");
  sf32lb52_bt_hci_rx_stall_clear();
  (void)sf32lb52_bt_controller_force_reset();

  /* **不能**在这里走 companion_adapter_cycle()：那一档的第一步是
   * `bt_adapter_disable()`，而控制器刚复位、适配器本来就是关的 —— disable 会
   * 失败并立刻又记一次失败，永远走不到 enable。这里直接做 enable + 等就绪，
   * 后面几步照抄 cycle 成功之后的收尾。 */
  companion_bridge_phase_set("escalate_enable");
  ret = bt_adapter_enable(g_ins);
  if ((ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE) ||
      wait_adapter_ready(ADAPTER_CYCLE_WAIT_SECS) != 0)
    {
      static uint8_t  s_esc_fail_n;
      static uint32_t s_restart_ms;

      LOGE("BLE recovery: controller reset did not bring the adapter back "
           "(enable=%d)", (int)ret);

      /* 复位也救不回来 —— 坏的多半是**主机侧**传输（h4 RX 死了就没有重启路径），
       * 那只有重建任务能治。**不能让 companion 自己调**（自杀）：投给 diag 执行。
       * 两次复位失败才求助，整体限频 10 分钟。 */
      if (s_esc_fail_n < 0xffu)
        {
          s_esc_fail_n++;
        }

      /* 1->2 次：LCPU 复位都没救回来，说明坏的不是控制器而是主机侧传输，
       * 再复位一次只是浪费时间。重建有 10 分钟限频兜着。 */
      if (s_esc_fail_n >= 1u &&
          (s_restart_ms == 0 ||
           myvendor_mono_elapsed_ms(now_ms, s_restart_ms) >= 600000u))
        {
          s_restart_ms = now_ms;
          s_esc_fail_n = 0;
          LOGE("BLE recovery: escalate to task restart "
               "(reset+enable failed)");
          companion_bridge_diag_restart_post();
        }

      /* 阶梯不归零：下一轮 cycle 失败还会再来（5 分钟限频兜着）。 */
      return;
    }

  (void)bt_conn_cb_register(&g_le_conn_cb);

  if (!g_policy_adv_off && !companion_phone_connected())
    {
      pthread_mutex_lock(&g_ctx.lock);
      g_ctx.adv_restart = true;
      pthread_mutex_unlock(&g_ctx.lock);
    }

  companion_apply_sensor_policy(true);
  ble_supervisor_escalation_done(now_ms);
  LOGI("BLE recovery: controller reset + enable ok — re-advertising");
}

static void companion_supervisor_pump(uint32_t now_ms)
{
  enum ble_supervisor_fault reason;
  bool expected;
  bool active;
  bool skip_while_connected;

  /* 消息入：消费外部投递的 BLE 事件（状态与判定只在本线程改）。 */
  companion_ble_drain();

  /* GATT 上下文池哨兵：坏了就一行 ERROR（带原始值），落进 diag 环。
   *
   * 2026-09-18 的 panic 是那张回调表被写坏之后，被 adapter cycle 里的
   * bt_sal_gatt_client_disable() 走了一遍（mmfar=0xfee7fee7）。这里 30 s
   * 采一次，是为了把"谁写坏的"缩到一个窗口里：采样在下面的 cycle 之前，
   * 所以看到"canary broken"就说明坏在上一个窗口，配合 diag 环看当时在干什么。
   * 不带限频会把这行刷满整个环。 */
  {
    static uint32_t last_ctx_check_ms;

    if (last_ctx_check_ms == 0 ||
        myvendor_mono_elapsed_ms(now_ms, last_ctx_check_ms) >= 30000u)
      {
        last_ctx_check_ms = now_ms;
        myvendor_gatt_ctx_checkpoint();
      }
  }

  /* 先做链路新鲜度判定：它可能要"主动拆链"，而下面广播判定用的正是
   * `phone_connected()`。顺序反了会白等一个周期。 */
  companion_link_watchdog(now_ms);

  ble_supervisor_tick(now_ms);

  /* 两个触发源都只探一次：`companion_bridge_diag_cycle_take()` 是一次性消费者，
   * 探第二次必然为假。 */
  {
    bool rx_stalled = sf32lb52_bt_hci_rx_stalled(HCI_RX_STALL_MS);
    bool diag_asked = companion_bridge_diag_cycle_take();
    static uint32_t last_hold_log_ms;
    static uint32_t hold_since_ms;

    if (rx_stalled || diag_asked)
      {
#ifdef CONFIG_MYVENDOR_BLE_LOG
        static uint32_t last_rxstall_log_ms;

        if (rx_stalled &&
            (last_rxstall_log_ms == 0 ||
             myvendor_mono_elapsed_ms(now_ms, last_rxstall_log_ms) >= 2000u))
          {
            last_rxstall_log_ms = now_ms;
            BLE_LOG("supervisor rxstall skip=%d phone=%d hold=%d adv=%d",
                    sf32lb52_bt_hci_skip_sync() ? 1 : 0,
                    companion_phone_connected() ? 1 : 0,
                    g_hold_le_adv ? 1 : 0,
                    g_adv_active ? 1 : 0);
          }
#endif

        if (rx_stalled && !diag_asked && companion_cycle_await_life())
          {
            /* 刚做过 cycle 且控制器连一个包都没再产生：再开一轮只会把窗口踢掉。
             *
             * **但这个等待必须有界。** `ab4881b0` 那版这里是没有闸的（rxstall 直接
             * 开 cycle，所以恢复很快）；加闸是为了不吃掉刚恢复的窗口、也不再自喂
             * 活锁 —— 可代价是：控制器真哑时"复活包"永远不会来，这条路就再也不动，
             * 连升级阶梯都拿不到失败样本。所以封顶 60 s：到点照样开 cycle，失败就
             * 顺着阶梯往上（cycle → LCPU 复位 → 任务重建）。 */
            if (hold_since_ms == 0)
              {
                hold_since_ms = (now_ms != 0) ? now_ms : 1u;
              }

            if (myvendor_mono_elapsed_ms(now_ms, hold_since_ms) >= 60000u)
              {
                hold_since_ms = 0;
                syslog(LOG_WARNING,
                       "ble_companion: rxstall hold released after 60 s "
                       "(controller still silent) — cycling anyway\n");
                ble_supervisor_request_cycle(BLE_SUPERVISOR_FAULT_ADAPTER, now_ms);
              }
            else if (last_hold_log_ms == 0 ||
                     myvendor_mono_elapsed_ms(now_ms, last_hold_log_ms) >= 10000u)
              {
                last_hold_log_ms = now_ms;
                syslog(LOG_WARNING,
                       "ble_companion: rxstall held, controller silent since "
                       "last cycle (pkts=%lu) — no immediate re-cycle\n",
                       (unsigned long)s_cycle_pkts);
              }
          }
        else
          {
            hold_since_ms = 0;
            ble_supervisor_request_cycle(BLE_SUPERVISOR_FAULT_ADAPTER, now_ms);
          }
      }
  }

  /* 同上：有 sensor 链路在时，"广播没起来"不是异常（那是射频共用），
   * 别让它触发停滞看门狗。 */
  expected = !companion_phone_connected() && !g_hold_le_adv && !g_policy_adv_off &&
             ble_sensor_ready_mask() == 0u;
  active   = g_adv_active;
  ble_supervisor_watch_advertising(expected, active, now_ms);

  if (!ble_supervisor_cycle_requested())
    {
      return;
    }

  skip_while_connected =
    companion_phone_connected() &&
    ble_supervisor_pending_reason() != BLE_SUPERVISOR_FAULT_ADAPTER;
  if (skip_while_connected)
    {
      return;
    }

  /* 换级：cycle 那一档连续失败之后，复位控制器再试（见上面的注释）。 */
  if (ble_supervisor_take_escalation())
    {
      companion_controller_recover(now_ms);
      return;
    }

  if (ble_supervisor_take_adapter_cycle(&reason))
    {
      LOGI("adapter cycle requested reason=%s",
           ble_supervisor_fault_name(reason));
      (void)companion_adapter_cycle(now_ms);

      /* 成败都要记账：下一轮 rxstall 必须先看到控制器产生过新包，
       * 否则不许立刻再开一轮（见 companion_cycle_await_life）。 */
      companion_cycle_note_done();
    }
}

/**
 * @brief 传感器观察者运行时停/恢复手机广播。
 * @param hold 限时搜表或 HCI 建链为 true；重连观察者 / 已连 GATT 为 false。
 */
void companion_hold_le_adv(bool hold);   /* 定义在下面：看门狗要先调用它 */


void companion_hold_le_adv(bool hold)
{
  g_hold_req_latch = hold;

  if (g_hold_le_adv != hold)
    {
      /* 让位/收回是"广播停不停"的唯一开关，之前只有 LOGD（默认编译掉），
       * 实机上完全看不见 —— 排查 22.6 s 拆适配器循环时只能靠猜。 */
      syslog(LOG_INFO, "ble_companion: hold le adv %s\n",
             hold ? "on" : "off");
    }

  g_hold_le_adv = hold;
  /* 判定侧读本地镜像：本函数可能在持有 g_ctx.lock 时被调用，这里再去拿状态锁
   * 就是锁嵌套了。sensor 侧"正在扫描/建链"这一事实由它自己投递
   * （COMPANION_BLE_EV_TRANSIENT），每拍刷新。 */
  pthread_mutex_lock(&g_ctx.lock);
  if (hold)
    {
      g_ctx.adv_restart = false;
    }
  else if (!g_ctx.connected && !g_policy_adv_off)
    {
      g_ctx.adv_restart = true;
    }

  pthread_mutex_unlock(&g_ctx.lock);

  if (hold)
    {
      companion_stop_advertising();

      /** 停完立刻回执，不等下一拍 `ble_sensor_set_phone_state()`。
       *
       *  传感器侧的时间片状态机在 ADV_STOP_WAIT 里等这个确认才起扫
       *  （`g_phone_adv_held`）。实机日志：让它等下一拍泵（0.5~1.5 s）时，
       *  这段射频是**全空**的 —— 不广播、不扫描、HCI 一条命令不发，手机
       *  的可见窗口从 2 s/4.5 s 掉到 2 s/6 s。停广播的 HCI 这时已经回完，
       *  确认只是记账，没有理由拖。
       */
      ble_sensor_set_phone_state(companion_phone_connected(), true);
    }
  else
    {
      ble_sensor_set_phone_state(companion_phone_connected(), false);
    }
}

/**
 * @brief 让位闩锁的**空转兜底**：闩锁在、传感器却一直没真的用射频 ⇒ 收回。
 *
 * 为什么需要它：`g_hold_le_adv = transient || g_hold_req_latch`。`transient` 是
 * 派生值（每拍由传感器上报"正在扫描/建链"），本身不会泄漏；但**闩锁那一路是电平**，
 * 只要它的输入里混进一个常驻判据，广播就会被永久扣住（现场见
 * `analysis/BLE_ADV_HOLD_LOCKOUT.md`：外设关机 72 min，手机全程扫不到设备）。
 * 输入侧已经修过一轮，这条兜底是防"将来再混进别的常驻量"—— 让位是**借**射频，
 * 任何输入异常都不该让广播永久停。
 *
 * 只认"空转"：`transient` 为真 = 传感器此刻真的在用射频 = 正常让位，不碰。
 * 收回只清闩锁，**不动**传感器的 `g_adv_hold_want`（边沿记忆）——
 * 它下一拍从 `ble_sensor_set_phone_state()` 学到"广播已解除"，若之后真的需要用
 * 射频，`transient` 一路仍然能把让位顶起来。
 *
 * @param now_ms 当前单调毫秒。
 */
static void companion_adv_hold_watchdog(uint32_t now_ms)
{
  companion_ble_state_t st;
  static uint32_t idle_since_ms;

  if (!g_hold_req_latch)
    {
      idle_since_ms = 0u;
      return;
    }

  /* 只读快照（本工程约定：跨线程共享读一律走快照）。 */
  companion_ble_state_snapshot(&st);
  if (st.transient)
    {
      idle_since_ms = 0u;
      return;
    }

  if (idle_since_ms == 0u)
    {
      idle_since_ms = (now_ms != 0u) ? now_ms : 1u;
      return;
    }

  if (myvendor_mono_elapsed_ms(now_ms, idle_since_ms) <
      COMPANION_ADV_HOLD_MAX_MS)
    {
      return;
    }

  idle_since_ms = 0u;
  syslog(LOG_WARNING,
         "ble_companion: adv hold watchdog: latch idle %u s with no sensor "
         "radio activity — releasing (phone was invisible)\n",
         (unsigned)(COMPANION_ADV_HOLD_MAX_MS / 1000u));
  companion_hold_le_adv(false);
}

/** @brief 应用 radio 策略（ADV on/off）。 */
static void companion_apply_radio_policy(bool on)
{
  g_policy_adv_off = !on;
  if (on)
    {
      pthread_mutex_lock(&g_ctx.lock);
      if (!g_ctx.connected && !g_hold_le_adv)
        {
          g_ctx.adv_restart = true;
        }

      pthread_mutex_unlock(&g_ctx.lock);
      (void)companion_start_advertising();
      LOGI("phone radio on");
    }
  else
    {
      companion_stop_advertising();
      pthread_mutex_lock(&g_ctx.lock);
      g_ctx.adv_restart = false;
      pthread_mutex_unlock(&g_ctx.lock);
      LOGI("phone radio off");
    }
}

/** @brief 应用传感器 central 策略。 */
static void companion_sensor_autorc_all(void)
{
  myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
  size_t n = 0;
  size_t i;

  if (g_ins == NULL)
    {
      return;
    }

  if (ble_sensor_start(g_ins) != 0)
    {
      LOGE("sensor autorc start failed");
      return;
    }

  ble_sensor_set_connect(true);
  if (myvendor_devctl_sensor_recs_get(recs, MYVENDOR_DEVCTL_SENSOR_REC_MAX,
                                      &n) != 0)
    {
      return;
    }

  /* 板级改动（my_vendor，2026-09-24）：**每个 kind 只武装一个地址**。
   *
   * 记录可以有多条（例如现场放了两台心率计，两条都 autorc=1），但同一个 kind
   * 只有一个连接槽（`g_slots[SENSOR_KIND_COUNT]`），而 `ble_sensor_connect_addr()`
   * 在"同 kind 已是另一个地址"时会**主动拆掉当前链路去切换**
   * （日志 `switch to saved addr after disconnect`）⇒ 逐条武装的旧写法
   * 会让两台设备互相顶掉：实测一个会话里 28 次断连、0 次成功。
   *
   * 新口径（用户 2026-09-24 定）：**记录多个、同时只连一个** ——
   * 同一 kind 只取第一条 autorc 记录去连，其余留在 tsv 里当候选（被跳过时打一行日志，
   * 想换哪台用 `ctl sensor ...` 显式指定）。 */
  {
    uint32_t armed = 0;

    for (i = 0; i < n; i++)
      {
        uint32_t bit;

        if (!recs[i].autorc)
          {
            continue;
          }

        bit = 1u << (recs[i].kind & 31u);
        if ((armed & bit) != 0u)
          {
            char skip[BT_ADDR_STR_LENGTH];

            bt_addr_ba2str(&recs[i].addr, skip);
            LOGI("sensor autorc skip kind=%u %s (same kind already armed)",
                 (unsigned)recs[i].kind, skip);
            continue;
          }

        if (ble_sensor_connect_addr(recs[i].addr, recs[i].addr_type, recs[i].kind,
                                    recs[i].name, NULL, 0) == 0)
          {
            armed |= bit;
            LOGI("sensor autorc kind=%u", (unsigned)recs[i].kind);
          }
      }
  }
}

static void companion_apply_sensor_policy(bool on)
{
  if (g_ins == NULL)
    {
      return;
    }

  if (on)
    {
      /* 换了新局面（开机 / cycle 收尾 / devctl 重开）：把空扫退避清零，
       * 让第一轮扫描立刻开始、跑满窗口 —— 否则退避（可达 60 s）会把那 4 秒
       * 窗口整个跳过，控制器就会因为"没有链路"再次进 NOT_READY。 */
      ble_sensor_reconnect_backoff_reset();

      companion_sensor_autorc_all();
      LOGI("sensor central on");
    }
  else
    {
      ble_sensor_stop();
      /* 外设关：收回扫描让位，并**强制重开**手机 ADV。
       * hold 释放只置 adv_restart；若当前句柄还在（停在 ADV 窗）或刚
       * 从扫描窗出来，不 stop 一下会沿用旧节奏。关外设后射频应整段
       * 连续给手机（见 companion_start_advertising 快广播）。 */
      companion_hold_le_adv(false);
      companion_stop_advertising();
      pthread_mutex_lock(&g_ctx.lock);
      if (!g_ctx.connected && !g_policy_adv_off)
        {
          g_ctx.adv_restart = true;
        }

      pthread_mutex_unlock(&g_ctx.lock);
      LOGI("sensor central off");
    }
}

/** @brief 从 bridge 取 devctl 策略变更并应用。 */
static void companion_apply_devctl(void)
{
  bool on;

  if (companion_bridge_policy_radio_take(&on))
    {
      companion_apply_radio_policy(on);
    }

  if (companion_bridge_policy_sensor_take(&on))
    {
      companion_apply_sensor_policy(on);
    }
}

/**
 * @brief 注册 GATTS。skip-HCI 重启后 adapter 可能已 ON 但 profile 未 startup
 *（`BT_STATUS_NOT_READY` / 2）。先短等，再 disable/enable 一次把 GATTS 拉起来。
 */
static bt_status_t companion_register_gatts(void)
{
  bt_status_t ret = BT_STATUS_FAIL;
  unsigned i;

  for (i = 0; i < GATTS_READY_RETRY; i++)
    {
      companion_bridge_heartbeat();
      ret = bt_gatts_register_service(g_ins, &g_gatts_handle,
                                      (gatts_callbacks_t *)&g_gatts_cbs);
      if (ret == BT_STATUS_SUCCESS && g_gatts_handle != NULL)
        {
          return ret;
        }

      if (ret != BT_STATUS_NOT_READY)
        {
          return ret;
        }

      usleep(GATTS_READY_SLICE_MS * 1000u);
    }

  LOGI("GATTS not ready (%d), recycle adapter", (int)ret);
  ret = bt_adapter_disable(g_ins);
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("GATTS recycle disable failed: %d", (int)ret);
      return ret;
    }

  {
    uint32_t left = GATTS_RECYCLE_OFF_MS;

    while (left > 0 && g_running)
      {
        uint32_t slice = left > GATTS_READY_SLICE_MS ?
                         GATTS_READY_SLICE_MS : left;

        companion_bridge_heartbeat();
        usleep(slice * 1000u);
        left -= slice;
      }
  }

  ret = bt_adapter_enable(g_ins);
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("GATTS recycle enable failed: %d", (int)ret);
      return ret;
    }

  {
    /* 取证：把"等 adapter 等了多久"打出来。
     *
     * 这个数字直接决定 ADAPTER_CYCLE_WAIT_SECS(8 s) 够不够 —— LCPU 被强制
     * 复位之后 adapter 要多久才回到 ON/BLE_ON，以前完全不可见。异常1 就是
     * 这里超时 → companion_start() 返回 -1 → 任务退出 → diag 判"线程停摆"
     * → 再强制复位 LCPU → 又 8 s 不够，形成每轮一次的活锁。 */
    uint32_t t_wait = mono_ms();

    /* 等 adapter 就绪：**这不是终态，永不放弃**。
     *
     * 现场（异常1）：LCPU 被强制复位后 8 s 内没回到 BLE_ON → 这里返回
     * BT_STATUS_NOT_READY(2) → companion_start() 失败 → 任务退出 → diag 判
     * "线程停摆" → 再复位 LCPU → 又不够 8 s …… 每轮一次活锁，最后走到
     * skip respawn。而真实情况只是"控制器还得多等几秒"，等下去就好了。
     *
     * 改成 8 s 一段循环等待：`wait_adapter_ready()` 内部每 1 s 喂一次心跳，
     * 所以 diag 不会把我们判成"停摆"；只在 g_running 变假（关机/退出）时才返回。 */
    {
      unsigned seg = 0;

      while (wait_adapter_ready(ADAPTER_CYCLE_WAIT_SECS) != 0)
        {
          if (!g_running)
            {
              return BT_STATUS_NOT_READY;
            }

          seg++;
          LOGE("adapter still not ready after %u ms (waited %u s, segment %u) "
               "— keep waiting, register NOT skipped",
               (unsigned)myvendor_mono_elapsed_ms(mono_ms(), t_wait),
               (unsigned)(seg * (unsigned)ADAPTER_CYCLE_WAIT_SECS), seg);
          companion_bridge_heartbeat();
        }
    }

    LOGI("adapter ready in %u ms",
         (unsigned)myvendor_mono_elapsed_ms(mono_ms(), t_wait));
  }

  companion_bridge_heartbeat();
  return bt_gatts_register_service(g_ins, &g_gatts_handle,
                                   (gatts_callbacks_t *)&g_gatts_cbs);
}

/**
 * @brief 打开 BT，注册 GATTS 0xFF10，开始广播，创建传感器槽位。
 */
static int companion_start(void)
{
  bt_status_t      ret;
  bt_address_t     le_addr;
  ble_addr_type_t  le_addr_type;
  char             addr_str[BT_ADDR_STR_LENGTH];
  char             classic_str[BT_ADDR_STR_LENGTH];

  companion_bridge_teardown_skip_hci_set(false);
  companion_bridge_heartbeat();
  if (wait_hci_device(HCI_WAIT_SECS) != 0)
    {
      return -1;
    }

  companion_bridge_heartbeat();
  ret = bt_adapter_enable(g_ins);
  companion_bridge_heartbeat();
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("bt_adapter_enable failed: %d", (int)ret);
      return -1;
    }

  if (wait_adapter_ready(ADAPTER_WAIT_SECS) != 0)
    {
      return -1;
    }

  /** 配对窗口外不允许新 LE 绑定（已绑定手机仍可用 LTK 回连）。 */
  companion_sync_bondable();

  if (bt_adapter_get_le_address(g_ins, &le_addr, &le_addr_type) !=
      BT_STATUS_SUCCESS)
    {
      LOGE("bt_adapter_get_le_address failed");
      return -1;
    }

  bt_adapter_get_address(g_ins, &g_classic_addr);
  status_pack(&g_ctx.status, g_status_buf);
  bt_addr_ba2str(&le_addr, addr_str);
  bt_addr_ba2str(&g_classic_addr, classic_str);
  build_adv_payload(&le_addr);
  LOGI("LE address %s (status trailer %s unused)", addr_str, classic_str);

  (void)bt_conn_cb_register(&g_le_conn_cb);

  /** enable 之后再 set_name() 会发 251 字节 HCI 写，打爆 HCPU 堆。保持编译期本地名。 */

  ret = companion_register_gatts();
  if (ret != BT_STATUS_SUCCESS || g_gatts_handle == NULL)
    {
      /* NOT_READY 不是"注册失败"：companion_register_gatts() 在 adapter 没就绪时
       * **根本没调用注册函数**就返回了。现场日志把它报成
       * "bt_gatts_register_service failed: 2"，而 2 正是 BT_STATUS_NOT_READY
       * （bt_status.h: BT_STATUS_SUCCESS=0, FAIL=1, NOT_READY=2），
       * 这条文案误导过一轮排查，所以分开报。 */
      if (ret == BT_STATUS_NOT_READY)
        {
          LOGE("gatts not registered: adapter 未就绪（register 未被调用）");
        }
      else
        {
          LOGE("bt_gatts_register_service failed: %d", (int)ret);
        }

      return -1;
    }

  ret = bt_gatts_add_attr_table(g_gatts_handle, &g_companion_service_db);
  if (ret != BT_STATUS_SUCCESS)
    {
      LOGE("bt_gatts_add_attr_table failed: %d", (int)ret);
      return -1;
    }

  companion_fs_bind(g_gatts_handle, COMP_FS_DATA_ID);

  g_policy_adv_off = !companion_bridge_policy_radio();
  if (!g_policy_adv_off)
    {
      if (companion_start_advertising() != 0)
        {
          LOGE("LE adv start failed");
          return -1;
        }
    }
  else
    {
      LOGI("phone radio off (ctl persist.bt.radio)");
    }

  if (companion_bridge_policy_sensor())
    {
      companion_apply_sensor_policy(true);
    }
  else
    {
      LOGI("sensor radio off (ctl persist.bt.sensor)");
    }

  LOGI("ready — service 0x%04X, proto 0x%04X, GATT FS (UI off; use test sensor/notif/ctrl)",
       COMPANION_SERVICE_UUID16, COMPANION_PROTO_VERSION);
  companion_bridge_alive_set(true);
  return 0;
}

/**
 * @brief 拆除 GATTS、ADV、传感器客户端、BT 实例。
 */
static void companion_stop(void)
{
  bool skip_hci;

  companion_bridge_alive_set(false);
  companion_bridge_phone_set(false);
  /* 统一状态：手机链路没了（真断开 / adapter cycle 丢掉 / 停机）。 */
  companion_ble_state_set_phone(false);
  skip_hci = companion_bridge_teardown_skip_hci() ||
             sf32lb52_bt_hci_skip_sync();

  if (skip_hci)
    {
      BLE_LOG("stop skipped HCI skip_sync=%d teardown=%d",
              sf32lb52_bt_hci_skip_sync() ? 1 : 0,
              companion_bridge_teardown_skip_hci() ? 1 : 0);
      LOGI("stop skipped HCI (recovering)");
      ble_sensor_stop();
      companion_fs_abort();
      companion_fs_on_disconnect();
      companion_downlink_reset();
      g_adv = NULL;
      g_adv_active = false;
      g_gatts_handle = NULL;
      g_adapter_cookie = NULL;
      g_ins = NULL;
      return;
    }

  ble_sensor_stop();
  companion_fs_abort();
  companion_fs_on_disconnect();
  companion_downlink_reset();
  companion_stop_advertising();

  if (g_gatts_handle != NULL)
    {
      bt_gatts_remove_attr_table(g_gatts_handle, COMP_SERVICE_ID);
      bt_gatts_unregister_service(g_gatts_handle);
      g_gatts_handle = NULL;
    }

  if (g_ins != NULL)
    {
      bt_adapter_disable(g_ins);

      if (g_adapter_cookie != NULL)
        {
          bt_adapter_unregister_callback(g_ins, g_adapter_cookie);
          g_adapter_cookie = NULL;
        }

      bluetooth_delete_instance(g_ins);
      g_ins = NULL;
    }
}

/**
 * @brief 启动失败时的拆除。不要 bluetooth_delete_instance：
 * skip-HCI respawn 后 service loop 还在，delete 会和 uv worker 上的
 * bt_disable 抢 manager 链表（MemFault @ manager_cleanup）。
 */
static void companion_stop_after_failed_start(void)
{
  companion_bridge_teardown_skip_hci_set(true);
  companion_stop();
}

void ble_companion_reap_if_dead(void)
{
  if (g_ins != NULL || g_gatts_handle != NULL)
    {
      companion_stop();
    }

  g_running = false;
}

/****************************************************************************
 * Private Functions — main loop
 ****************************************************************************/

/**
 * @brief 在 companion 线程上消费一条 NSH `test sensor` 命令。
 *
 * 扫描/连接/GATT 绝不在 NSH 任务里跑。连接按表格 idx → 地址。
 */
static void companion_apply_sensor_test(bt_instance_t *ins)
{
  uint8_t cmd;

  if (!companion_bridge_test_sensor_take(&cmd))
    {
      return;
    }

  switch (cmd)
    {
      /** 设备 UI「蓝牙设备 → 手机蓝牙」页 / `ctl pair open`：开配对窗口（窗口内可发现可连接，
       *  射频从"不广播"切回来；见 myvendor_bt_pair.c 的三档说明）。 */
      case COMPANION_TEST_PAIR_OPEN:
        {
          extern void myvendor_pair_window_open(unsigned secs);

          myvendor_pair_window_open((unsigned)companion_bridge_test_pair_secs());
        }
        break;

      /** `ctl pair connect`：设备侧发起配对（挑连接必须在本线程做，理由同下）。 */
      case COMPANION_TEST_PAIR_CONNECT:
        {
          extern bool myvendor_pair_connect_auto(void);

          if (!myvendor_pair_connect_auto())
            {
              LOGE("pair: connect: no eligible LE connection");
            }
        }
        break;

      /** `ctl pair list`：在**本线程**遍历连接并填快照。
       *  不能在 ctl 线程做 —— 那边遍历会踩到被 BT 线程改到一半的链表
       *  （现场 `bt_conn_ref` 的 stlex 打野指针 → MemManage panic，pid=ctl）。 */
      case COMPANION_TEST_PAIR_LIST:
        {
          extern void myvendor_pair_report(char *buf, size_t cap);
          char buf[320];

          myvendor_pair_report(buf, sizeof(buf));
          companion_bridge_pair_report_set(buf);
        }
        break;

      /** 设备 UI「蓝牙设备 → 手机蓝牙」页：解除绑定（清密钥 + 清手机记录 + 关窗）。 */
      case COMPANION_TEST_PAIR_UNBIND:
        {
          extern void myvendor_pair_unbind_request(void);
          extern void myvendor_pair_unbind_request_slot(unsigned idx);
          uint8_t slot = companion_bridge_test_pair_unbind_slot();

          if (slot == 0xffu)
            {
              myvendor_pair_unbind_request();
            }
          else
            {
              myvendor_pair_unbind_request_slot((unsigned)slot);
            }
        }
        break;

      case COMPANION_TEST_SENSOR_SCAN:
        if (ble_sensor_connect_busy())
          {
            LOGI("sensor scan skipped, connecting");
            break;
          }

        ble_sensor_set_connect(false);
        ble_sensor_set_adv_debug(false);
        if (ble_sensor_start(ins) != 0)
          {
            LOGE("sensor scan start failed");
            break;
          }

        ble_sensor_scan_arm(companion_bridge_test_sensor_scan_ms());
        break;

      case COMPANION_TEST_SENSOR_CONNECT:
        {
          /* 先取走 kind：一次性，失败也不留在桥里粘给下一条命令。
           * AUTO（0xff）＝按 ADV 里第一个匹配的类型，NSH / ctl 走这条；
           * 菜单点选会带上当前页面的类型。 */
          uint8_t kind = companion_bridge_sensor_cmd_take_connect_kind();

          if (ble_sensor_start(ins) != 0)
            {
              LOGE("sensor connect start failed");
              break;
            }

          if (ble_sensor_connect_index_kind(
                  companion_bridge_test_sensor_connect_idx(), kind) != 0)
            {
              break;
            }
        }

        break;

      case COMPANION_TEST_SENSOR_CONNECT_ADDR:
        {
          struct companion_sensor_bind bind;

          companion_bridge_sensor_cmd_get_bind(&bind);
          if (ble_sensor_start(ins) != 0)
            {
              LOGE("sensor addr connect start failed");
              break;
            }

          if (ble_sensor_connect_addr(bind.addr, bind.addr_type, bind.kind,
                                      bind.name, bind.drop_addr,
                                      bind.drop_kind) != 0)
            {
              LOGE("sensor addr connect failed");
            }
        }
        break;

      case COMPANION_TEST_SENSOR_CLEAR_WANT:
        {
          struct companion_sensor_bind bind;

          companion_bridge_sensor_cmd_get_bind(&bind);
          (void)ble_sensor_clear_want(bind.addr, bind.kind);
        }
        break;

      case COMPANION_TEST_SENSOR_SCAN_STOP:
        ble_sensor_scan_stop();
        break;

      case COMPANION_TEST_SENSOR_DISCONNECT:
        ble_sensor_disconnect_all();
        break;

      case COMPANION_TEST_SENSOR_DISCONNECT_KIND:
        (void)ble_sensor_disconnect_kind(companion_bridge_test_sensor_kind());
        break;

      case COMPANION_TEST_SENSOR_STOP:
        ble_sensor_stop();
        companion_hold_le_adv(false);
        LOGI("sensor scan stopped");
        break;

      case COMPANION_TEST_SENSOR_DUMP:
        ble_sensor_dump();
        break;

      default:
        break;
    }
}

/**
 * @brief 主循环一拍：传感器泵、FS 泵、状态标志、心跳 notify。
 *
 * 文件 I/O 在 companion_fs_pump()；本任务优先级低于 CONFIG_BT_LONG_WQ_PRIO，
 * 避免 SD 等待饿死协议栈。禁止 statfs()。
 */
static void companion_tick(uint32_t start_ms, uint32_t *last_notify_ms)
{
  uint8_t      buf[COMPANION_STATUS_WIRE_LEN];
  bt_address_t peer;
  uint32_t     now = mono_ms();
  bool         send = false;
  bool         usb_busy = xfer_usb_busy();
  bool         fs_busy  = companion_fs_busy();
  bool         drop_phone_gnss = false;
  bool         ride_session = false;
  bool         ride_moving = false;
  uint8_t      ready;

  ready = ble_sensor_ready_mask();
  companion_bridge_ride_get(&ride_session, &ride_moving);

  pthread_mutex_lock(&g_ctx.lock);

  g_ctx.status.uptime_sec = myvendor_mono_elapsed_ms(now, start_ms) / 1000;

  {
    int bat = myvendor_sys_battery_percent_cached();
    uint8_t bat_wire = (bat < 0 || bat > 100) ?
                       COMPANION_BATTERY_UNKNOWN : (uint8_t)bat;

    if (g_ctx.status.battery_pct != bat_wire)
      {
        g_ctx.status.battery_pct = bat_wire;
        g_ctx.status_dirty = true;
      }
  }

  status_set_flags(ride_session ? COMPANION_STATUS_RECORDING : 0,
                   ride_session ? 0 : COMPANION_STATUS_RECORDING);
  status_set_flags(ride_moving ? COMPANION_STATUS_MOVING : 0,
                   ride_moving ? 0 : COMPANION_STATUS_MOVING);

  if (fs_busy)
    {
      status_set_flags(COMPANION_STATUS_FS_BUSY, 0);
    }
  else
    {
      status_set_flags(0, COMPANION_STATUS_FS_BUSY);
    }

  if (usb_busy)
    {
      status_set_flags(COMPANION_STATUS_USB_MTP_BUSY, 0);
    }
  else
    {
      status_set_flags(0, COMPANION_STATUS_USB_MTP_BUSY);
    }

  /** App 不再推送后清掉 GPS_PHONE（过期的手机定位）。 */

  if ((g_ctx.status.flags & COMPANION_STATUS_GPS_PHONE) != 0 &&
      myvendor_mono_elapsed_ms(now, g_ctx.last_gnss_ms) >=
      COMPANION_GNSS_STALE_SEC * 1000)
    {
      status_set_flags(0, COMPANION_STATUS_GPS_PHONE);
      g_ctx.fix_valid = false;
      drop_phone_gnss = true;
    }

  status_set_flags((ready & (1u << 0)) ? COMPANION_STATUS_SENSOR_HR : 0,
                   (ready & (1u << 0)) ? 0 : COMPANION_STATUS_SENSOR_HR);
  status_set_flags((ready & (1u << 1)) ? COMPANION_STATUS_SENSOR_CSC : 0,
                   (ready & (1u << 1)) ? 0 : COMPANION_STATUS_SENSOR_CSC);
  status_set_flags((ready & (1u << 2)) ? COMPANION_STATUS_SENSOR_CPS : 0,
                   (ready & (1u << 2)) ? 0 : COMPANION_STATUS_SENSOR_CPS);

  status_pack(&g_ctx.status, buf);

  /* "连着但手机不说话"探针已在 2026-09-18 移除。
   *
   * 它把"手机在不在说话"绑在 0xFF14 的 1–5 Hz 推送上了，而按用户的决定 App
   * 以后不做辅助定位、手机位置只在用得到的时候带一次 —— 这条判据就没有意义了
   * （会把正常的按需连接一律报成"静默"）。链路是否还活着改由
   * `companion_link_watchdog()` 用**控制器有没有回话**（HCI 活动）判定，与 App
   * 的数据节奏无关。 */

  if (g_ctx.connected && g_ctx.ccc_enabled)
    {
      send = g_ctx.notify_req || g_ctx.status_dirty ||
             (myvendor_mono_elapsed_ms(now, *last_notify_ms) >=
              COMPANION_STATUS_HEARTBEAT_SEC * 1000);
    }

  peer = g_ctx.peer;
  g_ctx.notify_req   = false;
  g_ctx.status_dirty = false;
  pthread_mutex_unlock(&g_ctx.lock);

  if (drop_phone_gnss)
    {
      companion_bridge_gnss_clear();
    }

  /** 可读值与 notify 保持一致，避免 0xFF12 普通读和通知对不上。 */

  if (g_gatts_handle != NULL)
    {
      bt_gatts_set_attr_value(g_gatts_handle, COMP_STATUS_ID, buf,
                              sizeof(buf));
    }

  /** FS Data notify 突发已经很压控制器。中间插状态心跳会让 Android 把 0xFF16 收成最后一片。 */

  if (send && !fs_busy)
    {
      /** 连续失败 = 链路已经不送货了（最典型的是缓冲区被占住：往死链路 notify，
       *  借走的 PDU 要等拆链才归还，池子被慢慢抽干 —— 现场 `myvendor net_buf:
       *  pool ... empty for 2000 ms, giving up` 每 5 s 一条就是这么来的）。
       *
       *  处理分两段，都不新造 API：
       *    1) **降频探测**：一旦失败，就不再按心跳频率打，改成每
       *       COMPANION_NOTIFY_PROBE_MS 试一次 —— 立刻止住"继续借 buffer"，
       *       同时保留探测能力（链路自己好了能恢复）；
       *    2) **如实断开**：连续 COMPANION_NOTIFY_STARVE_N 次仍失败，就交给既有
       *       的 adapter cycle 拆链重连（断开会让 stack 归还 PDU，池子随之恢复；
       *       `le_disconnected()` 回调负责把 connected 清掉，状态与手机侧一致）。
       *       带冷却，避免抖动。
       *
       *  不在这里直接清 g_ctx.connected：让真正的断开事件去清，避免状态分叉。
       */
      static uint8_t  s_fail_n;        /* 连续 notify 失败次数 */
      static uint32_t s_probe_ms;      /* 上次探测（含成功）的时刻 */
      static uint32_t s_cycle_ms;      /* 上次判死拆链的时刻（冷却基准） */
      bool probe = true;

      if (s_fail_n > 0 && s_probe_ms != 0 &&
          myvendor_mono_elapsed_ms(now, s_probe_ms) <
              COMPANION_NOTIFY_PROBE_MS)
        {
          probe = false;
        }

      if (probe)
        {
          bt_status_t ret;

          s_probe_ms = now;
          companion_bridge_phase_set("notify");
          ret = bt_gatts_notify(g_gatts_handle, &peer, COMP_STATUS_ID,
                                buf, sizeof(buf));
          if (ret == BT_STATUS_SUCCESS)
            {
              *last_notify_ms = now;
              g_notify_err = BT_STATUS_SUCCESS;
              s_fail_n = 0;
            }
          else
            {
              if (ret != g_notify_err)
                {
                  /** 状态特征从不 notify 在 App 看来像死机。仅在错误变化时打印，避免断链刷屏。 */

                  LOGE("status notify failed: %d", (int)ret);
                  g_notify_err = ret;
                }

              if (s_fail_n < 0xffu)
                {
                  s_fail_n++;
                }

              if (s_fail_n >= COMPANION_NOTIFY_STARVE_N &&
                  (s_cycle_ms == 0 ||
                   myvendor_mono_elapsed_ms(now, s_cycle_ms) >=
                       COMPANION_NOTIFY_CYCLE_COOLDOWN_MS))
                {
                  s_cycle_ms = now;
                  s_fail_n   = 0;
                  LOGE("status notify dead x%u (last=%d), adapter cycle",
                       (unsigned)COMPANION_NOTIFY_STARVE_N, (int)ret);
                  ble_supervisor_request_cycle(BLE_SUPERVISOR_FAULT_ADAPTER,
                                               now);
                }
            }
        }
    }
}

/**
 * @brief SIGTERM / SIGINT：退出主循环。
 */
static void signal_handler(int signo)
{
  (void)signo;
  g_running = false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief ble_companion 入口：一条线程上跑手机 GATTS + 传感器 GATTC。
 */
int main(int argc, FAR char *argv[])
{
  uint32_t start_ms;
  uint32_t last_notify_ms = 0;
  uint32_t last_tick_ms = 0;

  (void)argc;
  (void)argv;

  g_running = true;
  companion_bridge_phase_set("boot");
  companion_bridge_heartbeat();

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  memset(&g_ctx, 0, sizeof(g_ctx));
  pthread_mutex_init(&g_ctx.lock, NULL);
  g_ctx.status.battery_pct     = COMPANION_BATTERY_UNKNOWN;
  g_ctx.status.storage_free_kb = COMPANION_STORAGE_FREE_UNKNOWN;

  put_le16(g_proto_ver_buf, COMPANION_PROTO_VERSION);
  status_pack(&g_ctx.status, g_status_buf);
  devinfo_pack(g_devinfo_buf);
  LOGI("devinfo slot=%u sw=%s hw=%s fw=%s boot=%s",
       (unsigned)g_devinfo_buf[0],
       myvendor_sw_version(), myvendor_hw_version(),
       myvendor_boot_fw_name()[0] != '\0' ? myvendor_boot_fw_name() : "-",
       myvendor_boot_version()[0] != '\0' ? myvendor_boot_version() : "-");

  g_ins = bluetooth_create_instance();
  if (g_ins == NULL)
    {
      LOGE("bluetooth_create_instance failed");
      companion_bridge_alive_set(false);
      pthread_mutex_destroy(&g_ctx.lock);
      return EXIT_FAILURE;
    }

  g_adapter_cookie = bt_adapter_register_callback(g_ins, &g_adapter_cbs);
  if (g_adapter_cookie == NULL)
    {
      LOGE("bt_adapter_register_callback failed");
      companion_stop_after_failed_start();
      pthread_mutex_destroy(&g_ctx.lock);
      return EXIT_FAILURE;
    }

  if (companion_start() != 0)
    {
      companion_stop_after_failed_start();
      pthread_mutex_destroy(&g_ctx.lock);
      return EXIT_FAILURE;
    }

  ble_supervisor_init(mono_ms());

  start_ms = mono_ms();

  while (g_running)
    {
      bool     fs_active;
      bool     adv_wait;
      bool     connected;
      uint32_t now;
#ifdef CONFIG_MYVENDOR_BLE_LOG
      static uint32_t last_ble_log_ms;
      bool adv_restart;
#endif

      companion_bridge_phase_set("loop");
      companion_bridge_heartbeat();
      companion_bridge_phase_set("devctl");
      companion_apply_devctl();
      now = mono_ms();
      /* HCI 已卡住时后面 start_adv / sensor 会同步等 Command Complete，
       * 必须先 cycle，否则监督逻辑永远跑不到。 */
      companion_bridge_phase_set("supervisor");
      companion_supervisor_pump(now);
      companion_apply_sensor_test(g_ins);

      pthread_mutex_lock(&g_ctx.lock);
      connected = g_ctx.connected;
#ifdef CONFIG_MYVENDOR_BLE_LOG
      adv_restart = g_ctx.adv_restart;
#endif
      pthread_mutex_unlock(&g_ctx.lock);
      companion_bridge_phase_set("sensor");
      ble_sensor_set_phone_state(connected, g_hold_le_adv);
      ble_sensor_pump(mono_ms());
      if (ble_sensor_take_hold_request())
        {
          companion_hold_le_adv(true);
        }
      if (ble_sensor_take_hold_release())
        {
          companion_hold_le_adv(false);
        }

      /* 让位超时自愈：没人还回来的 hold 到点就收，否则广播永久停着、
       * 手机再也看不到设备。现在让位是派生上报的，漏配最多影响一拍。 */
      companion_adv_hold_watchdog(mono_ms());

      /** 先消费 scanner 的 hold/release，再按新相位启 ADV；板级 SAL
       * radio guard 再等待上一角色的 HCI stop 真正完成。
       */

      /* **`ctl pair open` 的重播请求**：那条命令跑在 NSH/ctl 任务里，不能直接调
       * 蓝牙框架（直接调 `bt_le_stop_advertising` 会撞 libuv thread.c:358 断言 panic），
       * 所以它只置标志，这里由 companion 线程来停/重播（2026-09-20）。 */
      if (myvendor_pair_window_take_restart())
        {
          companion_sync_bondable();
          companion_adv_restart_for_pair_window();
        }

      /** **控制器不应答时，这两条"配对动作"一律不做。**
       *
       *  它们都不是纯主机动作，会发**同步 HCI**：
       *    · `myvendor_pair_admit_poll()` → `bt_conn_disconnect()`（HCI_Disconnect）
       *    · `myvendor_pair_unbind_poll()` → `bt_unpair()` → `bt_keys_clear()`
       *      → `bt_id_del()` → `addr_res_enable()` → `LE Set Address Resolution
       *      Enable`（**解析列表真的写进控制器之后才有这一步**）
       *
       *  控制器静默时这些同步命令只能等超时或干脆等不到，而它们跑在
       *  **companion 主循环**上 ⇒ 心跳停 ⇒ diag 判 unhealthy ⇒ 三连杀；
       *  卡在不可中断等待里杀不掉 ⇒ 僵尸 ⇒ 只能断电。
       *
       *  2026-09-23 18:5x 现场就是这么卡的 —— pid 回溯一条链走完：
       *    `ble_companion_main → myvendor_pair_unbind_poll → bt_unpair →
       *     bt_keys_clear → bt_id_del → addr_res_enable → bt_hci_cmd_send_sync`
       *  （`id.c`，末帧停在 `k_msleep` 轮询上）。
       *
       *  门控只挡"此刻不做"：两个 poll 内部**不清请求标志**，下一拍适配器
       *  能用时照常执行 —— 用户侧顶多是"点了解绑晚几秒生效"。
       *
       *  自证伪：若日志出现 `pair: unbind deferred` 长时间不消失（适配器一直
       *  不可用），那是控制器本身没起来，属于另一个问题，别把门控当成病因。 */
      if (adapter_is_usable())
        {
          /* 未配对连接的宽限到期检查（配对窗口/宽限的配套，见 myvendor_bt_pair.c）。 */
          myvendor_pair_admit_poll();

          /* `ctl pair unbind` / 设备 UI 的"解除绑定"：真解绑只能在蓝牙自己的线程里做。 */
          myvendor_pair_unbind_poll();
        }
      else if (myvendor_pair_pending())
        {
          static uint32_t last_pair_defer_log_ms;

          if (last_pair_defer_log_ms == 0 ||
              myvendor_mono_elapsed_ms(mono_ms(), last_pair_defer_log_ms) >= 5000u)
            {
              last_pair_defer_log_ms = mono_ms();
              syslog(LOG_WARNING,
                     "pair: admit/unbind deferred: adapter not ready\n");
            }
        }

      /* **已加密对端对账**：链路加密就等于配对完成，立刻补手机记录（否则 UI 的
       * 「配对中 + 倒计时」会一直走到 0 也不显示"已配对"，见 myvendor_bt_pair.c）。 */
      myvendor_pair_peer_poll();

      /* **延迟落盘**：配对过程中对端会先以"本次连接的 RPA"报一次、约 0.7 s 后
       * 再以身份地址报一次；两次都只更新内存待写值（后到覆盖先到），等池稳定
       * 后由这里一次性落盘并按"在不在池里"把关 ⇒ 废 RPA 根本不进文件，
       * 名单也不会出现 3/3 的瞬时峰值（不会误触 FIFO 淘汰）。
       * 不放在上面 `adapter_is_usable()` 的门里 —— 它只遍历密钥池、不发 HCI。 */
      myvendor_pair_settle_poll();

      /* **配对窗口过期 + 没记录 ⇒ 停播**：开机的 60 s 窗口过后如果还没绑上，
       * 广播就该收掉（射频让给传感器扫描），等下次开窗再拉起来。 */
      {
        extern bool myvendor_pair_has_phone_record(void);

        if (g_adv_active && !myvendor_pair_window_active() &&
            !myvendor_pair_has_phone_record())
          {
            LOGI("pair window over, no record -> adv off (radio to sensor)");
            companion_stop_advertising();
          }
      }

      /* **状态对账（2026-09-20）**：链路已经没了、GATTS 的 disconnect 回调却没到，
       * `g_ctx.connected` 会一直为真 —— 现象就是"设备觉得还在连"：不再广播、手机
       * 也连不回来（用户现场：手机删配对 + 关 app，设备一直不复位）。这里拿 zblue
       * 真实的 LE 链路表对账，连续 1 s 对不上就自己收尾。 */
      {
        static uint32_t lost_since_ms;
        bool            linked;
        bool            stale = false;

        pthread_mutex_lock(&g_ctx.lock);
        linked = !g_ctx.connected || myvendor_pair_phone_link_up();
        pthread_mutex_unlock(&g_ctx.lock);

        if (linked)
          {
            lost_since_ms = 0;
          }
        else if (lost_since_ms == 0)
          {
            lost_since_ms = mono_ms();
          }
        else if (myvendor_mono_elapsed_ms(mono_ms(), lost_since_ms) >= 1000u)
          {
            lost_since_ms = 0;
            stale = true;
          }

        if (stale)
          {
            bt_address_t peer;

            pthread_mutex_lock(&g_ctx.lock);
            peer = g_ctx.peer;
            pthread_mutex_unlock(&g_ctx.lock);
            companion_phone_gone(&peer, "link-gone");
          }
      }

      companion_bridge_phase_set("adv");
      companion_ensure_advertising();
      now = mono_ms();
      companion_bridge_phase_set("supervisor");
      companion_supervisor_pump(now);
#ifdef CONFIG_MYVENDOR_BLE_LOG
      if (last_ble_log_ms == 0 ||
          myvendor_mono_elapsed_ms(now, last_ble_log_ms) >= 5000u)
        {
          last_ble_log_ms = now;
          BLE_LOG("loop skip=%d rxstall=%d alive=%d phone=%d "
                  "adv=%d hold=%d radio_off=%d sensor=%d restart=%d "
                  "policy_r=%d policy_s=%d",
                  sf32lb52_bt_hci_skip_sync() ? 1 : 0,
                  sf32lb52_bt_hci_rx_stalled(HCI_RX_STALL_MS) ? 1 : 0,
                  companion_bridge_alive_get() ? 1 : 0,
                  connected ? 1 : 0,
                  g_adv_active ? 1 : 0,
                  g_hold_le_adv ? 1 : 0,
                  g_policy_adv_off ? 1 : 0,
                  ble_sensor_is_started() ? 1 : 0,
                  adv_restart ? 1 : 0,
                  companion_bridge_policy_radio() ? 1 : 0,
                  companion_bridge_policy_sensor() ? 1 : 0);
        }
#endif
      companion_bridge_phase_set("fs");
      fs_active = companion_fs_pump();
      if (!fs_active ||
          myvendor_mono_elapsed_ms(now, last_tick_ms) >= TICK_MS)
        {
          companion_bridge_phase_set("tick");
          companion_tick(start_ms, &last_notify_ms);
          last_tick_ms = now;
        }

      pthread_mutex_lock(&g_ctx.lock);
      adv_wait = g_ctx.adv_restart && !g_ctx.connected && !g_hold_le_adv;
      connected = g_ctx.connected;
      pthread_mutex_unlock(&g_ctx.lock);

      /** GATT 写只置 pending；没有 kick 的话下一拍泵会睡满 500 ms，READ 大约 1 个 ATT 载荷/秒。 */

      companion_bridge_phase_set("wait");
      if (fs_active)
        {
          companion_fs_wait(1);
        }
      else if (adv_wait)
        {
          companion_fs_wait(2);
        }
      else if (connected || ble_sensor_is_started())
        {
          companion_fs_wait(20);
        }
      else
        {
          companion_fs_wait(TICK_MS);
        }
    }

  LOGI("shutting down...");
  companion_stop();
  pthread_mutex_destroy(&g_ctx.lock);
  LOGI("done");
  return EXIT_SUCCESS;
}
