/**
 * @file companion_proto.h
 * @brief 共享的 BLE Companion 协议（服务 0xFF10）。
 *
 * 固件 ble_companion 与 Flutter App 的权威定义；
 * lib/ble/companion_proto.dart 必须与本文件镜像。
 *
 * 拓扑：一特征一功能（不分路复用）。多字节字段一律小端。
 *
 * 手机链路走本 GATT Server。独立 HR/CSC/CPS 外设走另一套 GATT
 * 客户端，见 docs/ble/ble_sensor.md。
 *
 * 规格：docs/ble/companion_impl_plan.md
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_COMPANION_PROTO_H
#define MYVENDOR_COMPANION_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Service / characteristic UUIDs (16-bit)                                    */
/* -------------------------------------------------------------------------- */

#define COMPANION_SERVICE_UUID16        0xFF10u
#define COMPANION_PROTO_VERSION_UUID16  0xFF11u  /* Read                      */
#define COMPANION_DEVICE_STATUS_UUID16  0xFF12u  /* Read, Notify              */
#define COMPANION_CONTROL_UUID16        0xFF13u  /* Write                     */
#define COMPANION_GNSS_FIX_UUID16       0xFF14u  /* Write, Write NR           */
#define COMPANION_FS_COMMAND_UUID16     0xFF15u  /* Write            (P1)     */
#define COMPANION_FS_DATA_UUID16        0xFF16u  /* Write, Notify    (P1)     */
#define COMPANION_NOTIFICATION_UUID16   0xFF17u  /* Write            (P1)     */
#define COMPANION_TRACK_POINT_UUID16    0xFF18u  /* Notify           (P2)     */
#define COMPANION_NAV_ROUTE_UUID16      0xFF19u  /* Write           (P2)     */
#define COMPANION_DEVINFO_UUID16        0xFF1Au  /* Read            (slot / sw / hw / boot) */

/* 0x0003: Device Status 16 bytes, CTRL_PING, path-addressed FS.
 * 0x0004: WRITE_OPEN gained a flags byte + 8-byte resume reply;
 *         disconnect keeps *.part so uploads can continue.
 * 0x0005: Dual-mode Classic SPP file manager.  Device Status grew a
 *         6-byte Classic BD_ADDR trailer (wire 22 bytes).  GATT
 *         0xFF15/0xFF16 is unchanged (silent small-file).  Folder-style
 *         list/upload/download uses the length-prefixed SPP stream.
 * 0x0006: Optional RLE/LZ payload on GATT FS READ/WRITE for text
 *         (txt/gpx/xml/…).  Size and CRC stay uncompressed.  READ
 *         gained a flags byte before the path.
 * 0x0007: Notification 0xFF17, phone nav route 0xFF19, sensor
 *         status bits for HR/CSC/CPS.  App icons travel over BLE FS
 *         to notif_icons/<package>.png.
 * 0x0008: TAG_ICON is a filename only (not a path).  Each side stores
 *         icons in its own notif_icons/ directory.  App does not push
 *         icons by default.  If the device is missing the file it
 *         notifies App on 0xFF16 with FS_NEED (filename payload);
 *         App then WRITE_OPEN to notif_icons/<filename>.  Incomplete
 *         writes stay in *.part until size+CRC32 match; a committed
 *         icon must look like a PNG or it is deleted and FS_NEED repeats.
 *         MD5 refresh is App-local only.
 * 0x0008 also: 0xFF1A Device Info (Read) — boot slot, software version,
 *         hardware version, optional /fw basename, 2SFBL version.
 *         V1 wire 97 bytes; V2 appends 16-byte boot_ver (113).  Protocol
 *         number stays 0x0008 so an older App can still talk; extra bytes
 *         are ignored.  Missing 0xFF1A is treated as unknown slot.
 * 0x0008 also: CTRL TIME_SYNC (0x04) — App writes UTC + tz on connect
 *         and every hour.  Old firmware ignores unknown opcodes.
 * App reads 0xFF11 and refuses to talk on mismatch.
 */

#define COMPANION_PROTO_VERSION         0x0008u

/* Advertised local name is CONFIG_MYVENDOR_PRODUCT_NAME + '-' + last two
 * bytes of the LE address (uppercase hex), e.g. "Helm One-A3F2".
 * App filters on the service UUID, not the name.
 */

#define COMPANION_ADV_NAME_PREFIX       "Helm One-"

/* -------------------------------------------------------------------------- */
/* Device Status (0xFF12, Read + Notify) — 16 bytes                           */
/* -------------------------------------------------------------------------- */

/** 码表已开始一次骑行活动（含自动/手动暂停）。由 MCU 上报，非 App CTRL。 */
#define COMPANION_STATUS_RECORDING      (1u << 0)
#define COMPANION_STATUS_GPS_PHONE      (1u << 1)
#define COMPANION_STATUS_GPS_INTERNAL   (1u << 2)
#define COMPANION_STATUS_STORAGE_FULL   (1u << 3)
#define COMPANION_STATUS_FS_BUSY        (1u << 4)
#define COMPANION_STATUS_USB_MTP_BUSY   (1u << 5)
/** 码表判定正在骑行（活动已开始且未暂停）。由 MCU 速度/踏频判断，非 App CTRL。 */
#define COMPANION_STATUS_MOVING         (1u << 6)
/** GATT 客户端已连上心率 0x180D（不是手机）。 */
#define COMPANION_STATUS_SENSOR_HR      (1u << 8)
/** GATT 客户端已连上 CSC 0x1816。 */
#define COMPANION_STATUS_SENSOR_CSC     (1u << 9)
/** GATT 客户端已连上骑行功率 0x1818。 */
#define COMPANION_STATUS_SENSOR_CPS     (1u << 10)

#define COMPANION_BATTERY_UNKNOWN       0xFFu

/* statfs() on the LittleFS transfer volume walks every metadata block, which
 * takes minutes on an SD card holding a full map set.  The firmware therefore
 * reports "unknown" instead of stalling; the App must not render 0 as "disk
 * full".
 */

#define COMPANION_STORAGE_FREE_UNKNOWN  0u

/* Phone GNSS：多久没有 0xFF14 写入就算过期（GPS_PHONE 状态位清掉，板子屏幕
 * 不再拿它兜底）。
 *
 * 2026-09-18 从 5 s 放宽到 600 s：按用户的决定，手机位置**只在用得到的时候随
 * 下发数据带一次**（给显示/兜底），不再 1–5 Hz 连续推送，也不再用于辅助定位 ——
 * 5 s 的窗口会让那份位置一送到就已经过期。10 分钟既能覆盖"带一次管一阵"的用法，
 * 又能在 App 真的断了之后自己回到"无手机定位"。 */
#define COMPANION_GNSS_STALE_SEC        600

/* Status heartbeat interval when nothing else changed. */

#define COMPANION_STATUS_HEARTBEAT_SEC  5

/** @brief "连着但手机不说话"的判据（ms）：连着 + 已订阅，却这么久没有 0xFF14 写入。
 *
 *  手机唯一周期性的写入是它自己那份定位（1–5 Hz），所以超过这个时长没有任何写入
 *  就说明手机侧已经不在这个连接上说话了（App 挂了/被挂起/链路死了），而两边 UI
 *  都还显示"已连接"——**丢 `Disconnection Complete` 的缺陷也是这个形状**。
 *  取 30 s：远大于 App 的推送周期（含它自己降频的情形），只报真静默。
 *  目前只用于打一行日志（见 ble_companion.c 的探针），暂不触发动作。 */
#define COMPANION_PHONE_SILENT_MS      30000u

/** @brief 链路活性看门狗阈值（ms）："距最近一次从控制器收到 HCI 包"的年龄。
 *
 *  判据与 App 行为无关：连着且已订阅时我们每 `COMPANION_STATUS_HEARTBEAT_SEC`
 *  (5 s) 发一次 status notify，控制器收到后**必须**回 `Number Of Completed
 *  Packets`（HCI 事件，走 RX 方向）。所以这个年龄超过阈值 = 控制器/LCPU 不再
 *  说话 = 这条链其实已经死了 —— 哪怕 `Disconnection Complete` 被丢掉、上层那个
 *  `g_ctx.connected` 永远不清（在案缺陷 #2），也能据此判死并强制拆链重广播。
 *  取 20 s：4 个心跳周期的余量，够区分"手机闲着"（心跳照发照回）与真死。
 *  见 ble_companion.c 的 companion_link_watchdog() / myvendor_bth4_activity()。 */
#define COMPANION_LINK_STALE_MS        20000u

/** @brief 状态通知连续失败后，降频探测的周期（ms）。
 *
 *  失败通常意味着链路已经不送货了，而 `bt_gatts_notify()` 每次都会借走一个
 *  PDU（要等拆链才归还）。失败后立刻放弃心跳频率、改成这个周期试一次，
 *  可以马上止住"继续借 buffer 把池子抽干"。 */
#define COMPANION_NOTIFY_PROBE_MS       5000

/** @brief 连续失败多少次判定链路已死（≈ N × PROBE_MS 后拆链重连）。 */
#define COMPANION_NOTIFY_STARVE_N       3

/** @brief 判死拆链后的冷却（ms）：等断开→重连→CCC 重订阅跑完，避免抖动。 */
#define COMPANION_NOTIFY_CYCLE_COOLDOWN_MS  30000

/**
 * @brief Device Status 载荷（0xFF12），线上 16 字节小端。
 *
 * @p flags 的 bit8..10 表示独立的传感器 GATT 链路（HR/CSC/CPS）。
 */
struct companion_status
{
  uint16_t flags;            /**< COMPANION_STATUS_* */
  uint8_t  battery_pct;      /**< 0..100，未知为 COMPANION_BATTERY_UNKNOWN */
  uint8_t  last_ctrl_op;     /**< 最近一次处理的 Control 操作码回显 */
  uint32_t storage_free_kb;  /**< 空闲 KiB，或 COMPANION_STORAGE_FREE_UNKNOWN */
  uint32_t uptime_sec;
  uint32_t gnss_rx_count;    /**< 收到的 0xFF14 写次数 */
};

#define COMPANION_STATUS_LEN            16

/* 0x0005: 16-byte status + Classic BD_ADDR (same byte order as
 * bt_address_t.addr[], LSB first).  App formats it the same way as
 * bt_addr_ba2str (MSB printed first) and uses that for RFCOMM.  LE and
 * Classic share the chip public address from eFuse UID / NVDS.
 */

#define COMPANION_STATUS_ADDR_LEN       6
#define COMPANION_STATUS_WIRE_LEN       (COMPANION_STATUS_LEN + COMPANION_STATUS_ADDR_LEN)

/* -------------------------------------------------------------------------- */
/* Device Info (0xFF1A, Read) — 113 bytes, packed, NUL-padded strings         */
/*                                                                            */
/* slot matches persist.boot.running written by 2SFBL just before jump.       */
/* Software version is CONFIG_MYVENDOR_PRODUCT_VERSION of the running image.  */
/* Hardware version is CONFIG_MYVENDOR_HARDWARE_VERSION (board / BOM).        */
/* Boot version is persist.boot.version written by 2SFBL (BOOT_LOADER_VERSION).*/
/* V1 was 97 bytes (no boot_ver).  App accepts both.                          */
/* -------------------------------------------------------------------------- */

#define COMPANION_SLOT_UNKNOWN          0u
#define COMPANION_SLOT_FW               1u  /* /mnt/kv/fw/<file> */
#define COMPANION_SLOT_MAIN             2u  /* SD partition main */
#define COMPANION_SLOT_FACTORY          3u  /* SD partition factory */

#define COMPANION_DEVINFO_SW_LEN        32u
#define COMPANION_DEVINFO_HW_LEN        16u
#define COMPANION_DEVINFO_NAME_LEN      48u
#define COMPANION_DEVINFO_BOOT_LEN      16u
#define COMPANION_DEVINFO_LEN_V1        (1u + COMPANION_DEVINFO_SW_LEN + \
                                         COMPANION_DEVINFO_HW_LEN + \
                                         COMPANION_DEVINFO_NAME_LEN)
#define COMPANION_DEVINFO_LEN           (COMPANION_DEVINFO_LEN_V1 + \
                                         COMPANION_DEVINFO_BOOT_LEN)

/**
 * @brief Device Info 载荷（0xFF1A），线上 113 字节（旧固件 97）。
 *
 * 字符串字段 NUL 填充；App 读到 0 或字段末尾为止。
 */
struct companion_devinfo
{
  uint8_t slot;              /**< COMPANION_SLOT_* */
  char    sw_ver[COMPANION_DEVINFO_SW_LEN];
  char    hw_ver[COMPANION_DEVINFO_HW_LEN];
  char    fw_name[COMPANION_DEVINFO_NAME_LEN]; /**< slot==FW 时的 basename */
  char    boot_ver[COMPANION_DEVINFO_BOOT_LEN]; /**< 2SFBL BOOT_LOADER_VERSION */
};

/* -------------------------------------------------------------------------- */
/* Control (0xFF13, Write) — [opcode:1][param...]                             */
/* -------------------------------------------------------------------------- */

#define COMPANION_CTRL_NOP              0x00u
#define COMPANION_CTRL_START_RECORD     0x01u
#define COMPANION_CTRL_STOP_RECORD      0x02u
#define COMPANION_CTRL_PING             0x03u  /* reply with a Status notify  */
#define COMPANION_CTRL_TIME_SYNC        0x04u  /* [unix_sec:u32le][tz_min:i16le] */
#define COMPANION_CTRL_FS_ABORT         0x10u  /* P1                          */
#define COMPANION_CTRL_SYNC_PREP        0x11u  /* P1                          */
#define COMPANION_CTRL_NAV_STOP         0x22u  /* stop phone-fed navigation   */
#define COMPANION_CTRL_PAIR_OPEN        0x30u  /* [secs:u16le] 开配对窗口（缺省 30 s） */
#define COMPANION_CTRL_PAIR_UNBIND      0x31u  /* 解除与本机的绑定（清密钥 + 清手机记录） */

/* TIME_SYNC: opcode + UTC seconds; tz (local-UTC minutes) is optional. */
#define COMPANION_CTRL_TIME_SYNC_MIN_LEN  5u
/* PAIR_OPEN：完整帧 = op + u16le 秒；只有 op 时按 30 s（所以最小长度 1）。 */
#define COMPANION_CTRL_PAIR_OPEN_MIN_LEN  1u
#define COMPANION_CTRL_TIME_SYNC_LEN      7u
#define COMPANION_TIME_MIN_UNIX           1704067200u  /* 2024-01-01 UTC */
#define COMPANION_TIME_MAX_UNIX           4102444800u  /* 2100-01-01 UTC */

/* -------------------------------------------------------------------------- */
/* GNSS Fix (0xFF14, Write / Write Without Response) — 24 bytes               */
/*                                                                             */
/* 2026-09-18 起：**按需发送，只用于板子屏幕/快照的兜底显示**。                   */
/*                                                                             */
/* 注意别和"星历辅助"搞混：给模组用的位置辅助**不走这条特征** —— App 生成 .ubx   */
/* 时把 `MGA-INI-POS_LLH` 直接写进文件（eph_service.dart:_assistHead()），固件的  */
/* 注入路径原样转发，所以不依赖 0xFF14、也不需要固件再补一次。                    */
/* 1–5 Hz 连续推送 + 持续辅助定位（gnss_assist_tick 那条）已停用；过期窗口见      */
/* COMPANION_GNSS_STALE_SEC。特征保留注册以兼容旧 App。                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 手机 GNSS Fix（0xFF14），24 字节。
 *
 * @note **按需**（原"骑行时 1–5 Hz"）：只用于板子屏幕/快照的显示兜底，
 *       不参与位置辅助。见文件头的说明。
 */
struct companion_gnss_fix
{
  int32_t  lat_e7;           /**< 纬度 ×1e7，WGS84 */
  int32_t  lon_e7;           /**< 经度 ×1e7 */
  int32_t  alt_mm;           /**< 海拔，毫米 */
  uint16_t speed_centi_kmh;  /**< 0.01 km/h */
  uint16_t course_deg;       /**< 0..359 */
  uint32_t utc_sec;          /**< Unix 时间戳 */
  uint8_t  fix_quality;      /**< COMPANION_FIX_* */
  uint8_t  satellites;
  uint8_t  hdop_x10;
  uint8_t  reserved;
};

#define COMPANION_GNSS_FIX_LEN          24

#define COMPANION_FIX_NONE              0u
#define COMPANION_FIX_2D                1u
#define COMPANION_FIX_3D                2u

/* -------------------------------------------------------------------------- */
/* Track Point (0xFF18, Notify) — 24 bytes, P2                                */
/* Emitted only while the App session has sent CTRL_START_RECORD.             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 轨迹点 notify（0xFF18），24 字节。P2，仅在录音期间发出。
 */
struct companion_track_point
{
  int32_t  lat_e7;
  int32_t  lon_e7;
  int32_t  alt_mm;
  uint16_t speed_centi_kmh;
  uint16_t course_deg;
  uint32_t utc_sec;
  uint8_t  fix_quality;
  uint8_t  satellites;
  uint8_t  hdop_x10;
  uint8_t  flags;            /**< bit0 = 有效 */
};

#define COMPANION_TRACK_POINT_LEN       24

/* -------------------------------------------------------------------------- */
/* Notification (0xFF17, Write) — P1                                          */
/*                                                                            */
/* An Android notification (app name + title + body) routinely exceeds one    */
/* ATT write, so the payload carries its own 8-byte fragmentation header and   */
/* the reassembled payload is a TLV sequence.                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 0xFF17 / 0xFF19 的 8 字节分片头。
 */
struct companion_notif_hdr
{
  uint8_t  hdr_ver;          /**< COMPANION_NOTIF_HDR_VER */
  uint8_t  type;             /**< COMPANION_NOTIF_TYPE_* */
  uint8_t  flags;            /**< COMPANION_NOTIF_FLAG_* */
  uint8_t  msg_id;           /**< 同一条消息的各分片相同 */
  uint16_t total;            /**< 整条消息的载荷总字节数 */
  uint16_t off;              /**< 本分片在载荷中的字节偏移 */
};

#define COMPANION_NOTIF_HDR_LEN         8
#define COMPANION_NOTIF_HDR_VER         0x01u

#define COMPANION_NOTIF_FLAG_FIRST      (1u << 0)
#define COMPANION_NOTIF_FLAG_LAST       (1u << 1)

#define COMPANION_NOTIF_TYPE_GENERIC    0u
#define COMPANION_NOTIF_TYPE_CALL       1u
#define COMPANION_NOTIF_TYPE_SMS        2u
#define COMPANION_NOTIF_TYPE_APP        3u
#define COMPANION_NOTIF_TYPE_CALENDAR   4u

/* Reassembled payload: [tag:1][len:2][value...] repeated. */

#define COMPANION_NOTIF_TAG_APP_NAME    0x01u  /* UTF-8                       */
#define COMPANION_NOTIF_TAG_TITLE       0x02u  /* UTF-8                       */
#define COMPANION_NOTIF_TAG_BODY        0x03u  /* UTF-8                       */
#define COMPANION_NOTIF_TAG_TIMESTAMP   0x04u  /* uint32 Unix seconds         */
#define COMPANION_NOTIF_TAG_PACKAGE     0x05u  /* UTF-8 Android package name  */
#define COMPANION_NOTIF_TAG_KEY         0x06u  /* UTF-8 notification key      */
#define COMPANION_NOTIF_TAG_ICON        0x07u  /* UTF-8 filename, no path     */

/* Device drops messages larger than this. */

#define COMPANION_NOTIF_MAX_TOTAL       1024u

/* Each side keeps icons in its own notif_icons/ directory.  The wire
 * carries only the filename (e.g. com.tencent.mm.png), never a path.
 * PACKAGE_MAX / ICON_NAME_MAX are max character counts (not including NUL).
 */

#define COMPANION_NOTIF_ICON_DIR        "notif_icons"
#define COMPANION_NOTIF_ICON_NAME_MAX   80u
#define COMPANION_NOTIF_TITLE_MAX       48u
#define COMPANION_NOTIF_BODY_MAX        96u
#define COMPANION_NOTIF_PACKAGE_MAX     64u

/* -------------------------------------------------------------------------- */
/* Nav Route (0xFF19, Write) — P2                                             */
/*                                                                            */
/* Same 8-byte fragmentation header as 0xFF17.  Reassembled payload is        */
/* packed WGS84 points: [lat_e7:4 le][lon_e7:4 le] repeated.                  */
/* flags bit2 (COMPANION_NAV_FLAG_START) starts navigation when LAST arrives. */
/* -------------------------------------------------------------------------- */

#define COMPANION_NAV_FLAG_START        (1u << 2)
#define COMPANION_NAV_PT_LEN            8u
#define COMPANION_NAV_MAX_PTS           32u
#define COMPANION_NAV_MAX_TOTAL         (COMPANION_NAV_MAX_PTS * COMPANION_NAV_PT_LEN)

/* -------------------------------------------------------------------------- */
/* FS Command (0xFF15) / FS Data (0xFF16) — P1                                */
/*                                                                            */
/* Path addressed, not PTP object handles: BLE links drop often and a handle   */
/* table does not survive a reconnect.  File-manager root is /mnt/lfs.          */
/* OTA is a dedicated sandbox: relative "fw" / "fw/…" (also /mnt/kv/fw/…        */
/* and leftover /mnt/lfs/fw/…) maps to /mnt/kv/fw.  2SFBL loads those files     */
/* as /fw .bin files.  The LFS browser listing does not include the OTA slot.   */
/* -------------------------------------------------------------------------- */

#define COMPANION_OTA_DIR               "fw"
#define COMPANION_OTA_ROOT              "/mnt/kv/fw"
#define COMPANION_COREDUMP_DIR          "coredump"
#define COMPANION_COREDUMP_ROOT         "/mnt/kv/coredump"

/* FS Command write: [cmd:1][tid:1][payload...] */

#define COMPANION_FS_CMD_HDR_LEN        2

#define COMPANION_FS_STATFS             0x01u  /* payload: none               */
#define COMPANION_FS_LIST               0x02u  /* [cursor:2][path...]         */
#define COMPANION_FS_STAT               0x03u  /* [path...]                   */
#define COMPANION_FS_READ               0x04u  /* [offset:4][len:4][flags:1][path...] */
#define COMPANION_FS_WRITE_OPEN         0x07u  /* [size:4][flags:1][path...]  */
#define COMPANION_FS_WRITE_DATA         0x08u  /* data arrives on FS Data     */
#define COMPANION_FS_WRITE_CLOSE        0x09u  /* [crc32:4] of the whole file */
#define COMPANION_FS_DELETE             0x0Au  /* [path...]                   */
#define COMPANION_FS_MKDIR              0x0Bu  /* [path...]                   */
#define COMPANION_FS_RENAME             0x0Cu  /* [old_len:1][old][new]       */
#define COMPANION_FS_NEED               0x0Du  /* device→app notify: [filename…] */

#define COMPANION_FS_WRITE_FLAG_RESUME  (1u << 0)
#define COMPANION_FS_WRITE_FLAG_RLE     (1u << 1)
#define COMPANION_FS_READ_FLAG_RLE      (1u << 0)

/* WRITE_OPEN success payload: [offset:4][crc32:4] of the existing *.part
 * prefix (both 0 when not resuming).
 */

#define COMPANION_FS_WRITE_OPEN_REPLY_LEN 8

/* FS Data frame header, shared by both directions. */

/**
 * @brief FS Data 帧头，双向共用。
 */
struct companion_fs_hdr
{
  uint8_t  cmd;              /**< 本帧所属命令的回显 */
  uint8_t  tid;              /**< 事务 id，App 分配，设备回显 */
  uint8_t  flags;            /**< COMPANION_FS_FLAG_* */
  uint8_t  status;           /**< 0 = OK，否则 errno */
  uint16_t seq;              /**< 事务内分片序号 */
  uint16_t len;              /**< 本分片载荷字节数 */
};

#define COMPANION_FS_HDR_LEN            8

#define COMPANION_FS_FLAG_LAST          (1u << 0)
#define COMPANION_FS_FLAG_ERR           (1u << 1)
#define COMPANION_FS_FLAG_ACK_REQ       (1u << 2)

/* FS_LIST entry encoding: [type:1][size:4][mtime:4][name_len:1][name...] */

#define COMPANION_FS_ENTRY_FILE         0u
#define COMPANION_FS_ENTRY_DIR          1u

/* FS_STAT reply payload: [type:1][size:4][mtime:4] (same as a LIST entry
 * without the name).
 */

#define COMPANION_FS_STAT_LEN           9

/* FS_STATFS is refused (status = EOPNOTSUPP).  statfs() on this LittleFS
 * volume walks every metadata block and can take minutes.
 *
 * FS_WRITE_CLOSE CRC32 is NuttX crc32part() with initial value 0
 * (polynomial 0xedb88320).
 */

/* -------------------------------------------------------------------------- */
/* Classic SPP stream (file manager).  GATT 0xFF15/16 stays for silent xfer.  */
/*                                                                            */
/* Request: [len:4 le][cmd:1][tid:1][payload…]                                */
/* Reply:   [len:4 le][cmd:1][tid:1][status:1][payload…]                      */
/* len is the number of bytes after the length field.                         */
/* cmd values reuse COMPANION_FS_* except HELLO (device-initiated).           */
/* -------------------------------------------------------------------------- */

#define COMPANION_SPP_HELLO_CMD         0x00u

/* Device → phone, first frame after RFCOMM comes up:
 * payload [proto:2 le][classic_bd_addr:6]
 */

#define COMPANION_SPP_HELLO_LEN         8

#define COMPANION_SPP_PAYLOAD_MAX       4096u
#define COMPANION_SPP_FRAME_MAX         (4u + 3u + COMPANION_SPP_PAYLOAD_MAX)

/* SPP READ request: [offset:4][want:2][path…]  (want ≤ PAYLOAD_MAX).
 * GATT READ still uses [offset:4][len:4][path…].
 */

#define COMPANION_SPP_READ_HDR_LEN      6

#define COMPANION_SPP_UUID16            0x1101u

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_COMPANION_PROTO_H */
