/**
 * @file ble_sensor.c
 * @brief 独立 HR（0x180D）、CSC（0x1816）、CPS（0x1818）的 BLE Central。
 *
 * 一次观察者扫描填表；`test sensor connect <idx>` 按地址绑定（绝不按名）。
 * 扫描/连接/GATT 跑在 companion 线程上。
 *
 * 双角色规则（docs/ble/ble_sensor.md）：
 * - 禁止 discover-all：Android 模拟器会超过 ELEMENT_MAX=40，SAL 过去会在
 *   任何 svc 回调之前直接 HCI 断开。
 * - 先发现 BAS 0x180F，缓存 0x2A19，再发现槽位 UUID。板卡 SAL
 *   keep_db 让第二次发现追加表。有 Notify 则订电量 CCC，否则 READY 后周期 Read。
 * - Central 保持 BT_LE_CONN_PARAM_DEFAULT。强推 7.5 ms / 1 s / PHY / DLE
 *   会和 Android 的 45 ms 更新打架，断链（HCI 0x16）。
 * - GATTS 的 connect/MTU/notify 必须忽略传感器地址：zblue att_mtu_updated
 *   是全局的，曾把 Companion FS 砸成 MTU 20。
 * - 搜表 LOW_LATENCY；重连 BALANCED/LOW_POWER，建链飞行中停观察者。
 * - HR 测量 Notify 8 s 视为关机重连；CSC/CPS 停车只标 idle，保持 GATT。
 *   停车后低占空比盯该地址：模拟器主动断开常不发 0x13，再广播则拆僵死链路。
 * - 健康监控：HCI 断链备份、订阅失败不得标 READY、已连接却见可连接 ADV
 *   则拆僵死 ACL、断链看门狗并重建 GATTC。手机+传感器双角色时 GATTC
 *   回调可能丢，UI 会假显示「已连接」而外设仍在广播。
 *
 * 禁止 statfs()。BT 使能后不要写 GAP Device Name。SF32 观察者走框架
 * `bt_le_start_scan_settings`；板卡 SAL 强制 legacy GAP（扩展扫描会
 * Command Disallowed）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "advertiser_data.h"
#include "ble_sensor.h"
#include "ble_supervisor.h"
#include "bluetooth.h"
#include "bt_adapter.h"
#include "bt_addr.h"
#include "bt_gatt_defs.h"
#include "bt_gattc.h"
#include "bt_le_scan.h"
#include "bt_uuid.h"
#include "companion_bridge.h"
/* 静止状态（息屏 + GNSS 已停）是射频状态机的一个输入，见 sensor_radio_eval()。 */
#include "myvendor_gnss.h"
#include "myvendor_ble_log.h"
#include "myvendor_devctl.h"
#include "myvendor_mono.h"

bool sf32lb52_bt_hci_skip_sync(void);

#ifndef CONFIG_BLUETOOTH_BLE_SCAN
#  error "ble_companion needs CONFIG_BLUETOOTH_BLE_SCAN=y (in defconfig). cmake_out/.config is stale: cmake --build cmake_out/my_vendor_nsh --target resetconfig"
#endif

/**
 * @brief 本模块日志出口（官方规则：服务层统一走 `syslog()`，不用 `printf`、不走 BT_LOG）。
 *
 * @details 前缀固定 `ble_sensor: `，一条事件一行；等级分档 = 例行/正常结束 INFO、
 *          可恢复异常 WARNING（例如 `[radio] heal:` 这类自愈）、真故障 ERROR。
 *          `/mnt/kv/diag` 归档只收 WARNING 及以上——所以"能自愈的事"不该占 ERROR。
 */
#define LOGI(fmt, ...) syslog(LOG_INFO, "ble_sensor: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) syslog(LOG_WARNING, "ble_sensor: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) syslog(LOG_ERR, "ble_sensor: " fmt "\n", ##__VA_ARGS__)

#ifdef CONFIG_MYVENDOR_BLE_COMPANION_DEBUG
#  define LOGD(fmt, ...) syslog(LOG_DEBUG, "ble_sensor: " fmt "\n", ##__VA_ARGS__)
#else
#  define LOGD(fmt, ...)
#endif

#define UUID_HRS                0x180Du
#define UUID_CSCS               0x1816u
#define UUID_CPS                0x1818u
#define UUID_BAS                0x180Fu
#define UUID_HR_MEAS            0x2A37u
#define UUID_CSC_MEAS           0x2A5Bu
#define UUID_CP_MEAS            0x2A63u
#define UUID_BAT_LEVEL          0x2A19u
#define UUID_BODY_SENSOR_LOC    0x2A38u

#define SENSOR_KIND_HR          0
#define SENSOR_KIND_CSC         1
#define SENSOR_KIND_CPS         2
#define SENSOR_KIND_COUNT       3
#define SENSOR_FEAT_HR          (1u << 0)
#define SENSOR_FEAT_CSC         (1u << 1)
#define SENSOR_FEAT_CPS         (1u << 2)
#define SENSOR_FEAT_BAT         (1u << 3)
#define SENSOR_FEAT_LOC         (1u << 4)
#define SENSOR_POLL_MS          2000u

#define SENSOR_RSSI_MIN         (-90)
#define SENSOR_DWELL_MS         2000u
#define SENSOR_CONNECT_MS       15000u
/** ADV_STOP_WAIT 里等 companion 确认让位的最长时间；超时照样起扫。 */
#define SENSOR_ADV_STOP_ACK_MS  1500u
/** LE 连接后等待 features / 自动 PHY 稳定，再做 UUID 发现。 */
#define SENSOR_DISCOVER_DELAY_MS     500u
/** 本机 HCI 0x16 / 断开后，重连前的冷却。 */
#define SENSOR_RECONNECT_MS          800u
/** 关机 / notify 丢失；不是 1 s 的 HCI 监督超时。HR 才拆链重连。 */
#define SENSOR_NOTIFY_STALE_MS       8000u
/** CSC/CPS 停车后 ATT 探活周期；失败则当对端已关。 */
#define SENSOR_PROBE_MS              2000u
#define SENSOR_PROBE_TIMEOUT_MS      4000u
/** 已有其它传感器时，重连冷却加长，降低 0x3e。 */
#define SENSOR_RECONNECT_BUSY_MS     1500u
/** HCI 0x3e（建链失败）后再连，冷却更长。 */
#define SENSOR_RECONNECT_3E_MS       3000u
#define HCI_ERR_CONN_FAIL_TO_ESTAB   0x3eu
/** 本机 disconnect 后等 GATTC/HCI 回调的看门狗。 */
#define SENSOR_HEALTH_DISC_MS        4000u
/** 已连接却收到可连接 ADV 的确认次数（防单包误报）。 */
#define SENSOR_HEALTH_ADV_HITS       2u
#define SENSOR_HEALTH_LOG_MS         10000u
#define SENSOR_SUB_RETRY_MAX         3u
/* **射频策略（2026-09-22 / 2026-09-23）** —— SF32 广播/扫描互斥，分时：
 *   1) BLE 稳定 / 手机刚掉线后：**15 s 只广播**（接 App），不扫 sensor；
 *   2) 之后进入稳态：**4 s 广播（或手机已连时的静默）+ 0.8 s 快扫**；
 *      连失败扫描窗按 1/2/4… 拉长，上限 8 s；
 *   3) 扫到已绑地址且 RSSI 尚可 → **停广播再连**（只覆盖建链过程，无 30 s 专连窗）；
 *   4) 各 want_link 槽均已连上 → 停快扫；掉线后再进稳态间隔扫；
 *   5) 手机已连：同样间隔扫（静默↔短扫），不再连续扫；
 *   6) 自愈：deadline 丢失 / STOP_WAIT 超时 / 长时间无扫 / SCAN 无 observer
 *      → kick 重进短扫（限频）；**建链/发现 busy 时禁止踢扫**（留给 ATT）；
 *   7) 配对窗关闭后 **4 s 冷静** 再扫传感器；重连 observer 重启 ≥500 ms。 */
#define SENSOR_PHONE_FIRST_MS        15000u
#define SENSOR_STEADY_ADV_MS         4000u
#define SENSOR_STEADY_SCAN_MS         800u
/** 重连需要扫却长时间未真正进入扫窗 → 强制 kick。 */
#define SENSOR_RADIO_HEAL_NO_SCAN_MS  10000u
/** 两次 heal kick 最小间隔，避免刷屏。 */
#define SENSOR_RADIO_HEAL_MIN_GAP_MS   5000u
/** ADV_STOP_WAIT 绝对上限（hold 回执丢失时仍强制开扫）。 */
#define SENSOR_RADIO_STOP_WAIT_MAX_MS  2500u
/** SCAN_WINDOW 无 observer 超过此时长则补起扫。 */
#define SENSOR_RADIO_SCAN_REARM_MS     1500u
/** 配对窗关闭后冷静期：先别扫/建传感器，把射频还给手机收尾。 */
#define SENSOR_PAIR_COOL_MS            4000u
/** 重连 observer 重启最小间隔，压住 30 ms 刷屏风暴。 */
#define SENSOR_SCAN_RESTART_MIN_MS      500u
/** 0 = 搜表 LOW_LATENCY，1 = 重连低占空比（与 g_scan_duty 一致）。 */
#define SCAN_DUTY_DISCOVERY  0
#define SCAN_DUTY_RECONNECT  1
/** 可见门槛（列表/认地址）；专连/建链用更高的「尚可」。 */
#define SENSOR_CONNECT_RSSI_MIN       (-75)

/** **临时关掉 RSSI 信号门槛**（用户 2026-09-23 要求）。
 *
 *  下面这行在 = 两道门槛都失效：
 *    · `SENSOR_RSSI_MIN`(-90)        —— 本来"连 -90 都不到的广播直接不处理"；
 *    · `SENSOR_CONNECT_RSSI_MIN`(-75) —— 本来"弱信号只更新 rssi、等更近再连"。
 *  失效后：**只要收到该地址的广播就认可建链**，弱信号也照连。
 *
 *  为什么两道一起关：现场（固件 `202609231915`）那台小米心率带
 *  `DF:F6:04:F3:7B:AC` 的 rssi 在 **-80 ~ -90** 之间漂、正好骑在 -90 上；
 *  只关 -75 的话，掉到 -91 的那些广播仍会被就地丢掉，还是连不上。
 *
 *  ⚠ 这是**临时调试开关**，不是修好：弱信号下心率数据本身就不可靠，连上之后
 *  RSSI 更差时会自己断、然后重连。要恢复原设计，**把下面这行注释掉**即可
 *  —— 两道门槛的判据都还在代码里（`SENSOR_RSSI_ACCEPT` /
 *  `SENSOR_RSSI_CONNECTABLE`），不用改回来。 */
#define SENSOR_RSSI_GATE_OFF 1

#ifdef SENSOR_RSSI_GATE_OFF
/** "这包广播值不值得处理"（原：`rssi >= -90`）。 */
#  define SENSOR_RSSI_ACCEPT(rssi)       (true)
/** "这个信号可以建链了"（原：`rssi >= -75`）。 */
#  define SENSOR_RSSI_CONNECTABLE(rssi)  (true)
#else
#  define SENSOR_RSSI_ACCEPT(rssi)       ((rssi) >= SENSOR_RSSI_MIN)
#  define SENSOR_RSSI_CONNECTABLE(rssi)  ((rssi) >= SENSOR_CONNECT_RSSI_MIN)
#endif

/* **连接失败退避**：连上后清零；超时按 1/2/4/8 s 拉长 scan 窗。 */
static uint32_t s_connect_fails;

/** 空扫退避（稳态短扫仍空时略拉长 ADV，封顶 4 s）。 */
static uint32_t s_reconnect_idle_cycles;
#define SENSOR_RECONNECT_IDLE_MAX_MS  4000u
#define SENSOR_RECONNECT_BACKOFF_MAX_MS  60000u
#define SENSOR_RECONNECT_BACKOFF_STEP    4u

/** 手机优先窗截止（mono）；0 = 未武装。 */
static uint32_t s_phone_first_until_ms;
/** phone-first 结束后立刻开一轮短扫（掉线 burst 排在优先窗之后）。 */
static bool     s_kick_scan_after_phone_first;
/** 上一拍是否在配对窗口内（上升沿：拆传感器链，射频全给手机 ADV）。 */
static bool     s_pair_win_prev;
/** 配对窗关闭后冷静截止（mono）；0 = 无。 */
static uint32_t s_pair_cool_until_ms;
/** 最近一次成功 `bt_le_start_scan` 的时刻（限频）。 */
static uint32_t s_scan_start_ms;
#define SENSOR_NAME_MAX         24
#define ADV_HEX_MAX             62
#define ADV_DEBUG_HEARTBEAT_MS  2000u
#define SENSOR_FOUND_MAX        16
#define SENSOR_SEEN_MAX         64

/**
 * @brief 每个槽位的 GATT 客户端状态机。
 */
enum sensor_state
{
  SENSOR_IDLE = 0,
  SENSOR_CONNECTING,
  SENSOR_DISCOVERING,
  SENSOR_SUBSCRIBING,
  SENSOR_READY,
  SENSOR_DISCONNECTING,
};

/**
 * @brief 手机未连接时，手机 ADV 与 sensor 重连 observer 的射频相位。
 */
enum sensor_radio_phase
{
  SENSOR_RADIO_IDLE = 0,
  SENSOR_RADIO_ADV_WINDOW,
  SENSOR_RADIO_ADV_STOP_WAIT,
  SENSOR_RADIO_SCAN_WINDOW,
};

/**
 * @brief 一台独立外设（HR 或 CSC 或 CPS，不是三合一）。
 *
 * 断开后仍保留 @p addr，重连只按已保存地址。
 * disconnect_all / off 会清 @p want_link（之后不再自动重连）。
 */
struct sensor_slot
{
  uint8_t         kind;
  enum sensor_state state;
  bt_address_t    addr;
  uint8_t         addr_type;
  int8_t          rssi;
  char            name[SENSOR_NAME_MAX];
  uint32_t        seen_ms;
  uint32_t        state_ms;
  gattc_handle_t  gatt;
  uint16_t        feat;
  uint16_t        hr_h;
  uint16_t        csc_h;
  uint16_t        cps_h;
  uint16_t        bat_h;
  uint16_t        loc_h;
  uint8_t         sub_step;      /**< 0 测量 CCC，1 电量 CCC（若有 Notify），然后读位置。 */
  uint8_t         disc_step;     /**< 0 BAS 缓存 0x2A19，1 槽位 UUID，然后订阅。 */
  uint32_t        poll_ms;
  uint16_t        last_bpm;
  uint16_t        last_energy;
  uint16_t        last_csc_rpm;
  uint16_t        last_cps_w;
  bool            meas_idle;     /**< CSC/CPS 停车：无测量 Notify，GATT 仍保持。 */
  bool            drop_link;     /**< 已连上却又看到可连接 ADV：僵死链路。 */
  bool            rearm_adv;     /**< 拆僵死链前已见 ADV，断开后立刻再连。 */
  bool            probe_sent;    /**< 停车 ATT 探活已发出。 */
  uint32_t        probe_ms;
  uint32_t        bat_ms;        /**< 最近电量 notify/read；证明链路仍活。 */
  int8_t          last_contact;
  int8_t          last_loc;
  uint8_t         last_bat;
  bool            energy_ok;
  bool            bat_ok;
  bool            bat_notify;    /**< 电量已订 CCC；否则 READY 后周期 Read。 */
  bool            gatt_ok;
  bool            op_sent;
  bool            skip_dwell;
  bool            want_link;     /**< 自动重连，直到 disconnect_all / off。 */
  bool            adv_seen;      /**< ADV 里又看到已保存地址（重连）。 */
  bool            meas_sub_ok;   /**< 测量 CCC 已订上；否则不得标 READY。 */
  uint8_t         sub_fail;      /**< 订阅失败次数，超过则拆链。 */
  bool            hci_gone;      /**< HCI 已断，GATTC 回调可能还没到。 */
  uint8_t         hci_reason;    /**< 最近一次 HCI 断链原因。 */
  uint8_t         adv_live;      /**< 声称已连时收到的可连接 ADV 次数。 */
  uint32_t        retry_ms;
  uint32_t        notify_ms;     /**< 最近一次测量 notify；0=从未收到。 */

  bool            csc_have;      /**< CSC 曲柄字段已有上一次采样。 */
  uint16_t        crank_revs;
  uint16_t        crank_time;
};

/**
 * @brief NSH 扫描表的一行。连接按此地址，绝不按名。
 */
struct sensor_found
{
  bt_address_t addr;
  uint8_t      addr_type;
  int8_t       rssi;
  uint16_t     svc_mask;
  uint32_t     last_ms;
  char         name[SENSOR_NAME_MAX];
};

static bt_instance_t *g_sensor_ins;
static bt_scanner_t *g_scanner;
static struct sensor_slot g_slots[SENSOR_KIND_COUNT];
/** 重连观察者的空扫退避（0 = 未退避）。见 `sensor_want_reconnect_observer()`。 */
static uint32_t g_reconnect_gap_ms;

static uint32_t g_reconnect_next_ms;
static struct sensor_found g_found[SENSOR_FOUND_MAX];
static uint8_t g_found_n;
static bt_address_t g_seen[SENSOR_SEEN_MAX];
static uint8_t g_seen_n;

/* 射频窗武装函数在 g_radio_phase / sensor_now_ms 定义之后。 */

/**
 * @brief 保护槽位/扫描表/地址的互斥量。
 *
 * @details
 * 本文件有两个线程会碰这些结构：
 *   - **BLE service-loop 线程**：GATTC 回调（gattc_on_*）与扫描回调
 *     （on_scan_result）在这里跑；
 *   - **companion 线程**：ble_sensor_pump / sensor_health_pump / 所有
 *     ble_sensor_* 公开 API / sensor_ui_publish。
 * 以前两边都裸读裸写，最直接的后果是 `hci_gone` 的"置位-读清"不原子，
 * 断连事件会被 pump 的清位吃掉，槽位卡在 READY 而链路已断。
 *
 * 加锁约定（重要，否则会自死锁）：
 *   - 非递归互斥量。回调已由下面的 *_locked 包装持锁，**回调内部调用的
 *     辅助函数一律不许再自己加锁**，它们假定调用者持锁。
 *   - **绝不在持锁状态下调用 SAL/BLE**（bt_gattc_* / bt_le_*）。凡是必须查
 *     SAL 的（如 slot_cache_bat_handle），先解锁、查完再重新加锁。
 *   - pump 侧（companion 线程）写字段一律走 @ref SENSOR_STATE_WRITE，
 *     锁内只放字段赋值，出锁后再跑状态机和 SAL。2026-09-18 之前 pump 侧
 *     是裸写，锁只盖了回调一侧，等于没有互斥。
 *   - 判定用的读仍在锁外：字段是单字，写在锁内保证原子与互见；状态机只在
 *     pump 这一侧推进，回调只负责记录事件。
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/** @brief pump 侧写 slot 状态字段的唯一入口（与回调侧共用 g_lock）。
 *
 *  2026-09-18 之前：回调侧（gattc_locked_*）整段持锁写字段，pump 侧
 *  （ble_sensor_pump，companion 线程）14 处字段赋值全裸写 —— 锁只盖了一侧，
 *  实际不构成互斥。现在两侧都过这把锁：
 *    - 宏里**只许放字段赋值**，禁止 SAL（bt_*）与日志：syslog 会走串口，
 *      在锁里打日志等于让 BLE service-loop 线程等串口；
 *    - 一次只包一组相关字段，临界区保持在"几条赋值"的量级；
 *    - 判定用的读（`if (slot->op_sent)` 之类）仍在锁外：字段是单字，写在锁内
 *      保证原子与互见，状态机本身只在 pump 这一侧推进。
 */
#define SENSOR_STATE_WRITE(...)                 \
  do                                            \
    {                                           \
      pthread_mutex_lock(&g_lock);              \
      __VA_ARGS__;                              \
      pthread_mutex_unlock(&g_lock);            \
    }                                           \
  while (0)
static bool g_started;
static bool g_allow_connect;
static bool g_want_scan;
static uint8_t g_scan_duty;      /**< 0 搜表 LOW_LATENCY，1 重连低占空比。 */
static uint32_t g_scan_until_ms;
static bool g_adv_debug;
static uint32_t g_adv_reports;
static uint32_t g_adv_debug_ms;
static bool g_hold_release_pend;
static bool g_hold_request_pend;
static bool g_adv_hold_want;     /**< 上次已请求的停广播状态。 */
static bool g_phone_connected;   /**< companion 线程反馈的手机链路状态。 */
static bool g_phone_adv_held;    /**< companion 已消费停广播请求。 */
static uint32_t g_radio_dbg_ms;  /**< [radio] tick 快照的限频时间戳。 */
static enum sensor_radio_phase g_radio_phase;
static uint32_t g_radio_deadline_ms;
static uint32_t g_radio_phase_ms;       /**< 进入当前 phase 的 mono。 */
static uint32_t g_radio_last_scan_ms;   /**< 最近一次真正开扫窗的 mono；0=从未。 */
static uint32_t g_radio_reconnect_since_ms; /**< 本轮 reconnect 需求起始；0=不需要。 */
static uint32_t g_radio_heal_ms;        /**< 上次 heal kick 时间。 */
static uint32_t g_linked_print_ms;
static uint32_t g_health_log_ms;
static uint16_t g_health_zombie_n;
static uint16_t g_health_hci_n;
static uint16_t g_health_disc_to_n;
static uint16_t g_health_recreate_n;
static uint16_t g_health_radio_heal_n;

static struct
{
  bool         valid;
  bt_address_t addr;
  uint8_t      addr_type;
  char         name[SENSOR_NAME_MAX];
} g_pending_bind[SENSOR_KIND_COUNT];

static void sensor_ui_publish(void);
static void slot_handle_disconnected(struct sensor_slot *slot);
static void slot_begin_disconnect(struct sensor_slot *slot, uint32_t now_ms);
static int  slot_gattc_recreate(struct sensor_slot *slot);
static uint8_t sensor_ready_count(void);
static void slot_bind_addr(struct sensor_slot *slot, const bt_address_t *addr,
                           uint8_t addr_type, int8_t rssi, const char *name,
                           bool seen);

/**
 * @brief CLOCK_MONOTONIC 毫秒，用于驻留 / 过期 / 轮询。
 * @return 毫秒。
 */
static uint32_t sensor_now_ms(void)
{
  return myvendor_mono_ms();
}

static uint32_t sensor_age_ms(uint32_t now, uint32_t then)
{
  return myvendor_mono_elapsed_ms(now, then);
}

/** @brief 武装「先给手机 15 s ADV」；结束后直接进稳态间隔扫。 */
static void sensor_radio_arm_phone_first(uint32_t now_ms)
{
  if (g_phone_connected)
    {
      /* 手机已在线：跳过优先窗，进稳态间隔扫。 */
      s_phone_first_until_ms = 0;
      s_kick_scan_after_phone_first = false;
      s_reconnect_idle_cycles = 0;
      s_connect_fails = 0;
      g_reconnect_gap_ms = 0;
      g_reconnect_next_ms = 0;
      g_radio_phase = SENSOR_RADIO_IDLE;
      g_radio_phase_ms = now_ms;
      g_radio_deadline_ms = 0;
      LOGI("[radio] skip phone-first (phone up) -> steady interval scan");
      return;
    }

  s_phone_first_until_ms = now_ms + SENSOR_PHONE_FIRST_MS;
  s_kick_scan_after_phone_first = false;
  s_reconnect_idle_cycles = 0;
  if (g_radio_phase != SENSOR_RADIO_IDLE)
    {
      g_radio_phase = SENSOR_RADIO_IDLE;
      g_radio_phase_ms = now_ms;
      g_radio_deadline_ms = 0;
    }

  LOGI("[radio] phone-first ADV %u ms (then steady ADV/scan)",
       (unsigned)SENSOR_PHONE_FIRST_MS);
}

/** @brief 立刻切到短扫（停 ADV 再扫）；用于掉线 / 扫到可连设备。 */
static void sensor_radio_kick_scan(uint32_t now_ms, const char *why)
{
  s_reconnect_idle_cycles = 0;
  /* heal 踢扫不清 0x3e 退避计数，否则每 5 s 自愈把退避清零，建链更挤。 */
  if (why == NULL || strncmp(why, "heal-", 5) != 0)
    {
      s_connect_fails = 0;
    }

  g_reconnect_gap_ms = 0;
  g_reconnect_next_ms = 0;
  g_want_scan = true;
  g_scan_duty = SCAN_DUTY_RECONNECT;

  /* 相位交给 pump：手机已连无 ADV 可停，ADV_STOP_WAIT 立刻到期进 SCAN。 */
  g_radio_phase = SENSOR_RADIO_ADV_STOP_WAIT;
  g_radio_phase_ms = now_ms;
  g_radio_deadline_ms = g_phone_connected ? now_ms : (now_ms + SENSOR_ADV_STOP_ACK_MS);

  LOGI("[radio] kick scan (%s)", why != NULL ? why : "-");
}

static void sensor_radio_enter_phase(enum sensor_radio_phase phase,
                                    uint32_t now_ms)
{
  if (g_radio_phase != phase)
    {
      g_radio_phase = phase;
      g_radio_phase_ms = now_ms;
    }
  else
    {
      g_radio_phase = phase;
    }
}

static void sensor_radio_note_scan_window(uint32_t now_ms)
{
  g_radio_last_scan_ms = now_ms;
  sensor_radio_enter_phase(SENSOR_RADIO_SCAN_WINDOW, now_ms);
}

/**
 * @brief 射频时间片自愈：相位被外部清掉 / hold 丢回执 / 长期无扫时强制 kick。
 *
 * @details
 * 实机：`ble_sensor_set_phone_state()` 曾每拍把 phase 打回 IDLE，kick 起的
 * 扫窗立刻消失，CPS 掉线后 busy=1 却 scanner=0。边沿修复后仍保留本看门狗
 * 作兜底（其它路径误清 phase / ADV_STOP 卡死同理）。
 */
static void sensor_radio_heal_watchdog(uint32_t now_ms, bool reconnect,
                                      bool connect_busy)
{
  uint32_t since_scan;
  uint32_t since_need;
  bool can_heal;
  const char *why = NULL;

  if (!reconnect)
    {
      g_radio_reconnect_since_ms = 0;
      return;
    }

  if (g_radio_reconnect_since_ms == 0)
    {
      g_radio_reconnect_since_ms = now_ms;
    }

  can_heal = (g_radio_heal_ms == 0) ||
             (sensor_age_ms(now_ms, g_radio_heal_ms) >=
              SENSOR_RADIO_HEAL_MIN_GAP_MS);

  /* 1) 有窗相位却丢了 deadline → 直接 kick。 */
  if ((g_radio_phase == SENSOR_RADIO_ADV_WINDOW ||
       g_radio_phase == SENSOR_RADIO_SCAN_WINDOW) &&
      g_radio_deadline_ms == 0)
    {
      why = "heal-deadline";
    }

  /* 2) ADV_STOP_WAIT 超过绝对上限（hold 回执丢失）。 */
  if (why == NULL &&
      g_radio_phase == SENSOR_RADIO_ADV_STOP_WAIT &&
      g_radio_phase_ms != 0 &&
      sensor_age_ms(now_ms, g_radio_phase_ms) >= SENSOR_RADIO_STOP_WAIT_MAX_MS)
    {
      why = "heal-stop-wait";
    }

  /* 3) 已在扫窗却长期无 observer → kick 重进 ADV_STOP→SCAN。 */
  if (why == NULL &&
      g_radio_phase == SENSOR_RADIO_SCAN_WINDOW &&
      g_scanner == NULL &&
      g_radio_phase_ms != 0 &&
      sensor_age_ms(now_ms, g_radio_phase_ms) >= SENSOR_RADIO_SCAN_REARM_MS)
    {
      why = "heal-scan-rearm";
    }

  /* 4) 需要重连却长时间没真正扫过（相位被清 / quiet 死循环）。
   *    busy（槽位要连却未 READY）时阈值收紧到 6 s。 */
  if (why == NULL && g_scanner == NULL &&
      g_radio_phase != SENSOR_RADIO_SCAN_WINDOW &&
      g_radio_phase != SENSOR_RADIO_ADV_STOP_WAIT)
    {
      since_need = sensor_age_ms(now_ms, g_radio_reconnect_since_ms);
      since_scan = (g_radio_last_scan_ms == 0) ?
                   since_need :
                   sensor_age_ms(now_ms, g_radio_last_scan_ms);

      if (since_need >= SENSOR_RADIO_HEAL_NO_SCAN_MS &&
          since_scan >= SENSOR_RADIO_HEAL_NO_SCAN_MS)
        {
          why = "heal-noscan";
        }
    }

  /* 5) ADV deadline 被异常刷到过远（超过一个安静窗），当作漂移。 */
  if (why == NULL && connect_busy && g_scanner == NULL &&
      g_radio_phase == SENSOR_RADIO_ADV_WINDOW &&
      g_radio_deadline_ms != 0 &&
      (int32_t)(g_radio_deadline_ms - now_ms) >
      (int32_t)(SENSOR_STEADY_ADV_MS + 1000u))
    {
      why = "heal-deadline-drift";
    }

  if (why == NULL || !can_heal)
    {
      return;
    }

  /* 正在建链 / GATT 发现时禁止踢扫类自愈：ATT 与 LE features 需要安静射频。
   * 现场（2026-09-23）：heal-scan-rearm 在 discover 超时前每 5 s 踢扫 →
   * scanner 空转 + 0x3e / discover timeout 螺旋。只保留 ADV_STOP_WAIT 卡死救援。 */
  if (connect_busy && strcmp(why, "heal-stop-wait") != 0)
    {
      return;
    }

  /* 2026-09-25（口径：可恢复异常→WARN）：这条是"发现卡住并**当场踢活**"的叙事
   * （why ∈ heal-noscan / heal-deadline / heal-scan-rearm / heal-deadline-drift），
   * 踢完就恢复 ⇒ 降为 WARNING，别占 `[ ERROR]` 这一档。 */
  LOGW("[radio] heal: %s phase=%d busy=%d scanner=%d since_need=%u since_scan=%u",
       why, (int)g_radio_phase, connect_busy ? 1 : 0,
       g_scanner != NULL ? 1 : 0,
       (unsigned)sensor_age_ms(now_ms, g_radio_reconnect_since_ms),
       (unsigned)(g_radio_last_scan_ms != 0 ?
                  sensor_age_ms(now_ms, g_radio_last_scan_ms) : 0));
  sensor_radio_kick_scan(now_ms, why);
  g_radio_heal_ms = now_ms;
  g_health_radio_heal_n++;
}

static bool sensor_radio_in_phone_first(uint32_t now_ms)
{
  return s_phone_first_until_ms != 0u &&
         (int32_t)(s_phone_first_until_ms - now_ms) > 0;
}

/** @brief 手动/掉线：尽快开一轮短扫（手机优先窗内则排队到其后）。 */
void ble_sensor_scan_burst(void)
{
  uint32_t now = sensor_now_ms();

  if (!g_started)
    {
      return;
    }

  if (sensor_radio_in_phone_first(now))
    {
      s_kick_scan_after_phone_first = true;
      LOGI("[radio] kick-scan queued after phone-first");
      return;
    }

  sensor_radio_kick_scan(now, "burst");
}

/**
 * @brief 日志用的短槽位名（HR / CSC / CPS）。
 * @param kind SENSOR_KIND_*。
 * @return 静态字符串。
 */
static const char *kind_name(uint8_t kind)
{
  switch (kind)
    {
      case SENSOR_KIND_HR:
        return "HR";
      case SENSOR_KIND_CSC:
        return "CSC";
      default:
        return "CPS";
    }
}

/**
 * @brief 槽位的主服务 UUID（0x180D / 0x1816 / 0x1818）。
 * @param kind SENSOR_KIND_*。
 * @return 16 位 UUID。
 */
static uint16_t kind_svc_uuid(uint8_t kind)
{
  switch (kind)
    {
      case SENSOR_KIND_HR:
        return UUID_HRS;
      case SENSOR_KIND_CSC:
        return UUID_CSCS;
      default:
        return UUID_CPS;
    }
}

/**
 * @brief 槽位的测量特征 UUID。
 * @param kind SENSOR_KIND_*。
 * @return 16 位 UUID。
 */
static uint16_t kind_meas_uuid(uint8_t kind)
{
  switch (kind)
    {
      case SENSOR_KIND_HR:
        return UUID_HR_MEAS;
      case SENSOR_KIND_CSC:
        return UUID_CSC_MEAS;
      default:
        return UUID_CP_MEAS;
    }
}

/**
 * @brief 槽位主服务对应的 SENSOR_FEAT_* 位。
 */
static uint16_t kind_feat_bit(uint8_t kind)
{
  switch (kind)
    {
      case SENSOR_KIND_HR:
        return SENSOR_FEAT_HR;
      case SENSOR_KIND_CSC:
        return SENSOR_FEAT_CSC;
      default:
        return SENSOR_FEAT_CPS;
    }
}

static void print_linked_table(void);
static void slot_finish_ready(struct sensor_slot *slot);

/**
 * @brief 比较两个 6 字节 LE 地址。
 */
static bool addr_eq(const bt_address_t *a, const bt_address_t *b)
{
  return memcmp(a->addr, b->addr, BT_ADDR_LENGTH) == 0;
}

/**
 * @brief @p a 全 0 则为 true（槽位未绑定）。
 */
static bool addr_empty(const bt_address_t *a)
{
  static const uint8_t z[BT_ADDR_LENGTH];

  return memcmp(a->addr, z, BT_ADDR_LENGTH) == 0;
}

static void slot_bind_addr(struct sensor_slot *slot, const bt_address_t *addr,
                           uint8_t addr_type, int8_t rssi, const char *name,
                           bool seen)
{
  uint32_t now = sensor_now_ms();

  /* 多字段整体更新必须原子：ble_sensor_owns_addr() 在 BT 线程做 6 字节
   * memcmp，读到写了一半的地址时既不等于旧值也不等于新值 —— 手机可能被
   * 判成传感器（反之亦然），文件传输状态就此套到传感器链路上。
   * 调用方都是 companion 线程（不在持锁回调内），所以这里自己加锁安全。 */
  pthread_mutex_lock(&g_lock);
  slot->addr       = *addr;
  slot->addr_type  = addr_type;
  slot->rssi       = rssi;
  slot->seen_ms    = now;
  slot->state_ms   = 0;
  slot->retry_ms   = 0;
  slot->skip_dwell = seen;
  slot->want_link  = true;
  slot->adv_seen   = seen;
  slot->state      = SENSOR_IDLE;
  slot->op_sent    = false;
  memset(slot->name, 0, sizeof(slot->name));
  if (name != NULL && name[0] != '\0')
    {
      size_t n = strlen(name);

      if (n >= SENSOR_NAME_MAX)
        {
          n = SENSOR_NAME_MAX - 1u;
        }

      memcpy(slot->name, name, n);
    }

  pthread_mutex_unlock(&g_lock);
}

/**
 * @brief 按已保存地址找槽位（HCI 备份路径；GATTC 句柄可能对不上）。
 */
static struct sensor_slot *slot_by_addr(const bt_address_t *addr)
{
  uint8_t i;

  if (addr == NULL)
    {
      return NULL;
    }

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (!addr_empty(&g_slots[i].addr) && addr_eq(&g_slots[i].addr, addr))
        {
          return &g_slots[i];
        }
    }

  return NULL;
}

/**
 * @brief 用户已绑定该槽位（connect idx），掉线后应自动重连。
 *
 * 不依赖 @c g_allow_connect：`test sensor scan` 只扫不连新设备，
 * 不得把已绑定槽位的重连关掉。
 */
static bool slot_keep_link(const struct sensor_slot *slot)
{
  return slot->want_link && !addr_empty(&slot->addr);
}

/**
 * @brief 回到 IDLE，但保留 @p addr 以便重连。
 * @param slot 槽位。
 * @param now_ms 未使用（与其它槽位辅助函数签名对齐）。
 */
static void slot_idle_keep_addr(struct sensor_slot *slot, uint32_t now_ms)
{
  (void)now_ms;
  slot->state       = SENSOR_IDLE;
  slot->feat        = 0;
  slot->hr_h        = 0;
  slot->csc_h       = 0;
  slot->cps_h       = 0;
  slot->bat_h       = 0;
  slot->loc_h       = 0;
  slot->sub_step    = 0;
  slot->disc_step   = 0;
  slot->csc_have    = false;
  slot->op_sent     = false;
  slot->energy_ok   = false;
  slot->bat_ok      = false;
  slot->bat_notify  = false;
  slot->last_bat    = 0;
  slot->last_bpm    = 0;
  slot->last_csc_rpm = 0;
  slot->last_cps_w  = 0;
  slot->meas_idle   = false;
  slot->drop_link   = false;
  slot->rearm_adv   = false;
  slot->probe_sent  = false;
  slot->probe_ms    = 0;
  slot->bat_ms      = 0;
  slot->last_contact = -1;
  slot->last_loc    = -1;
  slot->skip_dwell  = true;
  slot->adv_seen    = false;
  slot->retry_ms    = 0;
  slot->notify_ms   = 0;
  slot->meas_sub_ok = false;
  slot->sub_fail    = 0;
  slot->hci_gone    = false;
  slot->hci_reason  = 0;
  slot->adv_live    = 0;

  /* 槽位刚被重置/新存（用户刚配了一个表）：空扫退避必须清零，否则那个新设备
   * 最长要等 60 s 才开始找。 */
  g_reconnect_gap_ms  = 0;
  g_reconnect_next_ms = 0;
}

/**
 * @brief GATTC 或 HCI 备份确认断链后回收槽位（可重入：已 IDLE 则忽略）。
 */
static void slot_handle_disconnected(struct sensor_slot *slot)
{
  char a[BT_ADDR_STR_LENGTH];
  bool keep;
  bool early;
  bool rearm;
  uint8_t reason;

  if (slot == NULL || slot->state == SENSOR_IDLE)
    {
      return;
    }

  bt_addr_ba2str(&slot->addr, a);
  keep   = slot_keep_link(slot);
  early  = (slot->state == SENSOR_CONNECTING ||
            slot->state == SENSOR_DISCOVERING);
  rearm  = slot->rearm_adv;
  reason = slot->hci_reason;

  if (keep)
    {
      if (early)
        {
          LOGI("%s connect failed %s, scanning", kind_name(slot->kind), a);
        }
      else
        {
          ble_sensor_scan_burst();   /* 有设备掉线：回到全量 */

          LOGI("%s disconnected %s, scanning to reconnect",
               kind_name(slot->kind), a);
        }
    }
  else
    {
      LOGI("%s disconnected %s", kind_name(slot->kind), a);
    }

  slot_idle_keep_addr(slot, sensor_now_ms());

  if (!keep)
    {
      uint8_t k = slot->kind;

      slot->want_link = false;
      /* addr/name 清零与 ble_sensor_owns_addr 的 memcmp 互斥（同 slot_bind_addr）。
       * 注意只包住这两行：后面的 slot_bind_addr 自己会加锁，不能嵌套。 */
      pthread_mutex_lock(&g_lock);
      memset(&slot->addr, 0, sizeof(slot->addr));
      memset(slot->name, 0, sizeof(slot->name));
      pthread_mutex_unlock(&g_lock);
      if (g_pending_bind[k].valid)
        {
          slot_bind_addr(slot, &g_pending_bind[k].addr,
                         g_pending_bind[k].addr_type, 0,
                         g_pending_bind[k].name, false);
          g_pending_bind[k].valid = false;
        }

      sensor_ui_publish();
      return;
    }

  if (rearm)
    {
      slot->adv_seen   = true;
      slot->skip_dwell = true;
      LOGI("%s already saw advertiser, connect now", kind_name(slot->kind));
    }

  {
    uint32_t cool = SENSOR_RECONNECT_MS;

    if (reason == HCI_ERR_CONN_FAIL_TO_ESTAB)
      {
        cool = SENSOR_RECONNECT_3E_MS;
      }
    else if (!rearm && (early || sensor_ready_count() >= 1))
      {
        cool = SENSOR_RECONNECT_BUSY_MS;
      }

    slot->retry_ms = sensor_now_ms() + cool;
  }
  sensor_ui_publish();
}

/**
 * @brief 在 companion 线程发起拆链；成功则进 DISCONNECTING 等回调。
 *
 * UI 立刻离开「已连接」。回调丢失由健康看门狗重建 GATTC。
 */
static void slot_begin_disconnect(struct sensor_slot *slot, uint32_t now_ms)
{
  if (slot == NULL || slot->state == SENSOR_IDLE ||
      slot->state == SENSOR_DISCONNECTING)
    {
      return;
    }

  slot->state     = SENSOR_DISCONNECTING;
  slot->state_ms  = now_ms;
  slot->op_sent   = false;
  slot->drop_link = false;
  LOGI("%s disconnecting", kind_name(slot->kind));
  sensor_ui_publish();

  if (slot->gatt == NULL ||
      bt_gattc_disconnect(slot->gatt) != BT_STATUS_SUCCESS)
    {
      slot_handle_disconnected(slot);
    }
}

/**
 * @brief 把 SENSOR_FEAT_* 格式化成 HR+BAT（没有则 "-"）。
 */
static void mask_str(uint16_t mask, char *buf, size_t n)
{
  size_t off = 0;

  buf[0] = '\0';
  if (n == 0)
    {
      return;
    }

  if ((mask & SENSOR_FEAT_HR) != 0)
    {
      off += (size_t)snprintf(buf + off, n - off, "%sHR", off ? "+" : "");
    }

  if ((mask & SENSOR_FEAT_CSC) != 0 && off < n)
    {
      off += (size_t)snprintf(buf + off, n - off, "%sCSC", off ? "+" : "");
    }

  if ((mask & SENSOR_FEAT_CPS) != 0 && off < n)
    {
      off += (size_t)snprintf(buf + off, n - off, "%sCPS", off ? "+" : "");
    }

  if ((mask & SENSOR_FEAT_BAT) != 0 && off < n)
    {
      (void)snprintf(buf + off, n - off, "%sBAT", off ? "+" : "");
    }

  if (buf[0] == '\0')
    {
      (void)snprintf(buf, n, "-");
    }
}

/**
 * @brief 按 BLE 地址插入或合并扫描行。
 * @param result 观察者报告。
 * @param svc_mask AD 里的 SENSOR_FEAT_HR / CSC / CPS 位。
 * @param name 完整或缩短的本地名（可为空）。
 */
static void found_upsert(const ble_scan_result_t *result, uint16_t svc_mask,
                         const char *name)
{
  uint8_t i;
  uint8_t weakest = 0;

  if (result == NULL || svc_mask == 0)
    {
      return;
    }

  for (i = 0; i < g_found_n; i++)
    {
      if (addr_eq(&g_found[i].addr, &result->addr))
        {
          g_found[i].svc_mask |= svc_mask;
          g_found[i].addr_type = result->addr_type;
          g_found[i].last_ms   = sensor_now_ms();
          if (result->rssi > g_found[i].rssi)
            {
              g_found[i].rssi = result->rssi;
            }

          if (name != NULL && name[0] != '\0')
            {
              memcpy(g_found[i].name, name, SENSOR_NAME_MAX);
            }

          return;
        }

      if (g_found[i].rssi < g_found[weakest].rssi)
        {
          weakest = i;
        }
    }

  if (g_found_n < SENSOR_FOUND_MAX)
    {
      i = g_found_n++;
    }
  else if (result->rssi > g_found[weakest].rssi)
    {
      i = weakest;
    }
  else
    {
      return;
    }

  g_found[i].addr      = result->addr;
  g_found[i].addr_type = result->addr_type;
  g_found[i].rssi      = result->rssi;
  g_found[i].svc_mask  = svc_mask;
  g_found[i].last_ms   = sensor_now_ms();
  memset(g_found[i].name, 0, sizeof(g_found[i].name));
  if (name != NULL && name[0] != '\0')
    {
      memcpy(g_found[i].name, name, SENSOR_NAME_MAX);
    }
}

/**
 * @brief 记下 ADV 地址，供重连扫描的 seen=N 计数。
 */
static void seen_note(const bt_address_t *addr)
{
  uint8_t i;

  if (addr == NULL)
    {
      return;
    }

  for (i = 0; i < g_seen_n; i++)
    {
      if (addr_eq(&g_seen[i], addr))
        {
          return;
        }
    }

  if (g_seen_n < SENSOR_SEEN_MAX)
    {
      g_seen[g_seen_n++] = *addr;
    }
}

/**
 * @brief 取出 16 位 UUID；不是 UUID16 则返回 0。
 */
static uint16_t uuid16_of(const bt_uuid_t *uuid)
{
  if (uuid == NULL)
    {
      return 0;
    }

  if (uuid->type == BT_UUID16_TYPE)
    {
      return uuid->val.u16;
    }

  return 0;
}

/**
 * @brief 体感位置名称；未读/未知则为 "NA"。
 */
static const char *loc_name(int8_t loc)
{
  switch (loc)
    {
      case 0:
        return "other";
      case 1:
        return "chest";
      case 2:
        return "wrist";
      case 3:
        return "finger";
      case 4:
        return "hand";
      case 5:
        return "earlobe";
      case 6:
        return "foot";
      default:
        return "-";
    }
}

/**
 * @brief 槽位完成发现+订阅（SENSOR_READY）则为 true。
 */
static bool slot_linked(const struct sensor_slot *slot)
{
  return slot->state == SENSOR_READY;
}

/**
 * @brief 已 READY 的传感器数量（不含正在拆链的本槽）。
 */
static uint8_t sensor_ready_count(void)
{
  uint8_t i;
  uint8_t n = 0;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (slot_linked(&g_slots[i]))
        {
          n++;
        }
    }

  return n;
}

/**
 * @brief 是否已有 HCI 建链或发现未完成（再飞一条 create_conn 易 0x3e）。
 */
static bool sensor_hci_connecting(void)
{
  uint8_t i;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (g_slots[i].state > SENSOR_IDLE &&
          g_slots[i].state < SENSOR_READY)
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief 可连接广播（对端认为自己空闲，可被再连）。
 */
static bool adv_is_connectable(uint8_t adv_type)
{
  return adv_type != BT_LE_ADV_NONCONN_IND &&
         adv_type != BT_LE_LEGACY_ADV_NONCONN_IND &&
         adv_type != BT_LE_EXT_ADV_NONCONN_IND &&
         adv_type != BT_LE_SCAN_RSP &&
         adv_type != BT_LE_LEGACY_SCAN_RSP &&
         adv_type != BT_LE_EXT_SCAN_RSP;
}

/**
 * @brief GATT 仍活：测量 Notify 或电量 Notify/Read 在监督窗口内。
 *
 * CSC/CPS 停车会停发测量，但电量 CCC 仍在。不能因此一直开重连观察者
 *（日志里会反复 `[scan] observer on`，还和已连的 HR 抢射频，易 0x3e）。
 */
static bool slot_link_alive(const struct sensor_slot *s, uint32_t now_ms)
{
  if (s->state != SENSOR_READY)
    {
      return false;
    }

  if (s->notify_ms != 0 &&
      sensor_age_ms(now_ms, s->notify_ms) < SENSOR_NOTIFY_STALE_MS)
    {
      return true;
    }

  if (s->bat_ms != 0 &&
      sensor_age_ms(now_ms, s->bat_ms) < SENSOR_NOTIFY_STALE_MS)
    {
      return true;
    }

  return false;
}

/**
 * @brief 射频用途由**统一状态机**裁决（`companion_ble_radio_state()`）。
 *
 * 以前这里的 `sensor_radio_eval()` 自己算状态；现在状态与锁都在 ble_companion
 * 内部（手机 + 各 sensor + 恢复 + 射频），本文件只做两件事：
 *   1. **上报**：谁连上了（`sensor_ui_publish()`）、有谁想连 / 在不在附近；
 *   2. **执行 + 节奏**：按裁决起停扫描与它的交替节奏（见下）。
 */
/** @brief 有 kept 槽位还没连上（"想连"）—— 上报给统一状态机。 */
static bool sensor_want_link(void)
{
  uint8_t i;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      const struct sensor_slot *s = &g_slots[i];

      if (slot_keep_link(s) && s->state == SENSOR_IDLE)
        {
          return true;
        }
    }

  return false;
}

/** @brief "想连"的槽位里有没有谁露过面（= 在附近）—— 节奏用。 */
static bool sensor_wanted_seen(void)
{
  uint8_t i;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      const struct sensor_slot *s = &g_slots[i];

      if (slot_keep_link(s) && s->state == SENSOR_IDLE && s->adv_seen)
        {
          return true;
        }
    }

  return false;
}

static companion_ble_radio_state_t sensor_radio_state(uint32_t now_ms)
{
  companion_ble_state_t st;

  (void)now_ms;

  /* 只读快照，不跨线程调判定（裁决在 companion 线程内算好、随快照发布）。 */
  companion_ble_state_snapshot(&st);
  return (companion_ble_radio_state_t)st.radio_state;
}

static bool sensor_radio_scan_allowed(uint32_t now_ms)
{
  switch (sensor_radio_state(now_ms))
    {
      case COMPANION_RADIO_RECOVERING:
      case COMPANION_RADIO_ADV_ONLY:
        return false;

      case COMPANION_RADIO_SCAN_ONLY:
        return true;

      case COMPANION_RADIO_ALTERNATE:
      default:
        break;
    }

  /* 交替态的**速率**按「在不在场」分档（现场 2026-09-18 09:5x：两张已保存的表
   * 不在场，却每秒抢 1 s 射频，广播只剩 2 s 一档 → 手机很难发现板子）。 */
  if (sensor_wanted_seen())
    {
      g_reconnect_gap_ms  = 0;
      g_reconnect_next_ms = 0;
      return true;
    }

  /* 一个都没露面；板子还处于静止（息屏、GNSS 已停）时**不找** —— 射频全给广播，
   * 有人来（手机）就能进；真需要连表时，动一下唤醒之后再找。 */
  if (myvendor_gnss_idle_get())
    {
      g_reconnect_next_ms = now_ms + SENSOR_RECONNECT_BACKOFF_MAX_MS;
      return false;
    }

  return g_reconnect_gap_ms == 0 || g_reconnect_next_ms == 0 ||
         (int32_t)(now_ms - g_reconnect_next_ms) >= 0;
}


/**
 * @brief 需要重连观察者：只为 IDLE 槽位等 ADV。
 *
 * READY 的 CSC 停车不得因此开扫描：电量常和测量一起更新，停车后两者
 * 一起过期，会误判链路死、抢已连 HR 的射频，触发本机 0x16 再 0x3e。
 * 僵死 ACL 靠 ATT 探活；真掉线走 HCI 备份后再扫。
 *
 * 现在它只是状态机的一个视图（见 `sensor_radio_scan_allowed()`），保留名字
 * 是为了不动既有调用点。
 */
static bool sensor_want_reconnect_observer(uint32_t now_ms)
{
  extern bool myvendor_pair_window_active(void);

  /* 配对窗口：射频全给可发现广播。 */
  if (myvendor_pair_window_active())
    {
      return false;
    }

  /* 配对窗刚关的冷静期：先别报 want_reconnect，避免别处立刻起扫。 */
  if (s_pair_cool_until_ms != 0u &&
      (int32_t)(s_pair_cool_until_ms - now_ms) > 0)
    {
      return false;
    }

  if (!sensor_want_link())
    {
      s_kick_scan_after_phone_first = false;
      if (!sensor_radio_in_phone_first(now_ms))
        {
          s_phone_first_until_ms = 0;
        }

      return false;
    }

  /* BLE 稳定后的 15 s：只广播接 App（手机已连则跳过）。 */
  if (!g_phone_connected && sensor_radio_in_phone_first(now_ms))
    {
      return false;
    }

  /* 优先窗刚结束且有排队的 kick：立刻短扫一轮，然后走稳态。 */
  if (s_kick_scan_after_phone_first)
    {
      s_kick_scan_after_phone_first = false;
      if (s_phone_first_until_ms != 0u)
        {
          s_phone_first_until_ms = 0;
        }

      sensor_radio_kick_scan(now_ms, "after-phone-first");
    }

  /* 专连窗已取消；建链忙时仍允许扫（见 pump 延长 SCAN）。 */
  if (ble_sensor_connect_busy())
    {
      return true;
    }

  return sensor_radio_scan_allowed(now_ms);
}

/**
 * @brief 紧凑轮询行：HR bpm loc=… bat=… contact=… energy=…（缺项打 NA）。
 */
static void print_linked_table(void)
{
  uint8_t i;
  char meas[16];
  char loc[16];
  char bat[12];
  char extra[48];

  /** 每个已连接槽位一行。缺 loc/bat/contact/energy 打 NA。 */

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      struct sensor_slot *s = &g_slots[i];

      if (!slot_linked(s))
        {
          continue;
        }

      extra[0] = '\0';
      if (i == SENSOR_KIND_HR)
        {
          char energy[16];

          if (s->last_bpm != 0)
            {
              (void)snprintf(meas, sizeof(meas), "%u bpm",
                             (unsigned)s->last_bpm);
            }
          else
            {
              (void)snprintf(meas, sizeof(meas), "NA");
            }

          if (s->energy_ok)
            {
              (void)snprintf(energy, sizeof(energy), "%u kJ",
                             (unsigned)s->last_energy);
            }
          else
            {
              (void)snprintf(energy, sizeof(energy), "NA");
            }

          (void)snprintf(extra, sizeof(extra), " contact=%s energy=%s",
                         s->last_contact < 0 ? "NA" :
                         (s->last_contact ? "yes" : "no"),
                         energy);
        }
      else if (i == SENSOR_KIND_CSC)
        {
          if (s->meas_idle)
            {
              (void)snprintf(meas, sizeof(meas), "idle");
            }
          else if (s->last_csc_rpm != 0)
            {
              (void)snprintf(meas, sizeof(meas), "%u rpm",
                             (unsigned)s->last_csc_rpm);
            }
          else
            {
              (void)snprintf(meas, sizeof(meas), "NA");
            }
        }
      else
        {
          if (s->meas_idle)
            {
              (void)snprintf(meas, sizeof(meas), "idle");
            }
          else if (s->last_cps_w != 0)
            {
              (void)snprintf(meas, sizeof(meas), "%u W",
                             (unsigned)s->last_cps_w);
            }
          else
            {
              (void)snprintf(meas, sizeof(meas), "NA");
            }
        }

      if (s->last_loc >= 0)
        {
          (void)snprintf(loc, sizeof(loc), " loc=%s", loc_name(s->last_loc));
        }
      else
        {
          (void)snprintf(loc, sizeof(loc), " loc=NA");
        }

      if (s->bat_ok)
        {
          (void)snprintf(bat, sizeof(bat), " bat=%u%%",
                         (unsigned)s->last_bat);
        }
      else
        {
          (void)snprintf(bat, sizeof(bat), " bat=NA");
        }

      LOGI("%s %s%s%s%s", kind_name(i), meas, loc, bat, extra);
    }
}

/**
 * @brief 订阅完成后标 SENSOR_READY，并打印 feat=HR+BAT。
 */
static void slot_finish_ready(struct sensor_slot *slot)
{
  char feats[24];

  mask_str(slot->feat, feats, sizeof(feats));
  slot->state       = SENSOR_READY;
  slot->poll_ms     = sensor_now_ms();
  slot->adv_live    = 0;
  g_linked_print_ms = slot->poll_ms;
  LOGD("%s ready feat=%s", kind_name(slot->kind), feats);
  {
    /* 返回值的唯一消费者就是这里：保存失败（/mnt/kv 没挂、IO 错）以前被
     * `(void)` 吞掉，表现就是「连过但开机不回连」，且没有任何线索。 */
    int rc = myvendor_devctl_sensor_remember(slot->kind, slot->addr.addr,
                                             slot->addr_type, slot->name);

    if (rc != 0)
      {
        LOGE("%s record save failed %d (will not autorc)", kind_name(slot->kind),
             rc);
      }
  }

  sensor_ui_publish();
}

/**
 * @brief 解析心率测量（0x2A37）；没有能量则保持 NA。
 */
static void parse_hr_meas(struct sensor_slot *slot, const uint8_t *p,
                         uint16_t len, uint32_t now_ms)
{
  uint8_t flags;
  uint16_t off = 1;
  uint16_t bpm;
  uint16_t energy = 0;
  bool energy_ok = false;
  int8_t contact = -1;

  if (len < 2)
    {
      return;
    }

  flags = p[0];
  if ((flags & 0x01) != 0)
    {
      if (len < 3)
        {
          return;
        }

      bpm = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
      off = 3;
    }
  else
    {
      bpm = p[1];
      off = 2;
    }

  if ((flags & 0x02) != 0)
    {
      contact = ((flags & 0x04) != 0) ? 1 : 0;
    }

  if ((flags & 0x08) != 0 && (uint16_t)(off + 2) <= len)
    {
      energy = (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
      energy_ok = true;
    }

  if (bpm == 0 || bpm > 250)
    {
      return;
    }

  if (slot != NULL)
    {
      slot->last_bpm     = bpm;
      slot->last_contact = contact;
      slot->last_energy  = energy;
      slot->energy_ok    = energy_ok;
    }

  companion_bridge_sensor_set_hr(bpm, now_ms);
}

/**
 * @brief 解析 CSC 测量（0x2A5B）曲柄增量，换成 RPM。
 */
static void parse_csc_meas(struct sensor_slot *slot, const uint8_t *p,
                           uint16_t len, uint32_t now_ms)
{
  uint8_t  flags;
  uint16_t off = 1;
  uint16_t crank_revs;
  uint16_t crank_time;

  if (len < 1 || slot == NULL)
    {
      return;
    }

  flags = p[0];
  if ((flags & 0x01) != 0)
    {
      off = (uint16_t)(off + 6);
    }

  if ((flags & 0x02) == 0)
    {
      return;
    }

  if ((uint16_t)(off + 4) > len)
    {
      return;
    }

  crank_revs = (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
  crank_time = (uint16_t)(p[off + 2] | ((uint16_t)p[off + 3] << 8));

  if (slot->csc_have)
    {
      uint16_t drev  = (uint16_t)(crank_revs - slot->crank_revs);
      uint16_t dtime = (uint16_t)(crank_time - slot->crank_time);

      if (drev > 0 && dtime > 0)
        {
          uint32_t rpm = ((uint32_t)drev * 1024u * 60u) / dtime;

          if (rpm > 0 && rpm < 300)
            {
              slot->last_csc_rpm = (uint16_t)rpm;
              slot->meas_idle    = false;
              companion_bridge_sensor_set_cadence((uint16_t)rpm, now_ms);
            }
        }
    }

  slot->crank_revs = crank_revs;
  slot->crank_time = crank_time;
  slot->csc_have   = true;
}

/**
 * @brief 解析骑行功率测量（0x2A63）瞬时瓦数。
 */
static void parse_cps_meas(struct sensor_slot *slot, const uint8_t *p,
                           uint16_t len, uint32_t now_ms)
{
  int16_t watts;

  if (len < 4)
    {
      return;
    }

  watts = (int16_t)(p[2] | ((uint16_t)p[3] << 8));
  if (watts < 0)
    {
      watts = 0;
    }

  if (watts > 3000)
    {
      return;
    }

  if (slot != NULL)
    {
      slot->last_cps_w = (uint16_t)watts;
      slot->meas_idle  = false;
    }

  companion_bridge_sensor_set_power((uint16_t)watts, now_ms);
}

/**
 * @brief 解析电池电量（0x2A19）；没有该特征则 bat=NA。
 */
static void parse_bat(struct sensor_slot *slot, const uint8_t *p, uint16_t len)
{
  if (p == NULL || len < 1)
    {
      return;
    }

  if (slot != NULL)
    {
      slot->last_bat = p[0];
      slot->bat_ok   = true;
      slot->bat_ms   = sensor_now_ms();
    }
}

/**
 * @brief 解析体感位置（0x2A38）；未读则 loc=NA。
 */
static void parse_loc(struct sensor_slot *slot, const uint8_t *p, uint16_t len)
{
  if (p == NULL || len < 1)
    {
      return;
    }

  if (slot != NULL)
    {
      slot->last_loc = (int8_t)p[0];
      slot->feat    |= SENSOR_FEAT_LOC;
    }
}

/**
 * @brief 按特征 UUID 分发 notify/read 值。
 */
static void apply_meas(struct sensor_slot *slot, uint16_t uuid,
                       const uint8_t *p, uint16_t len, uint32_t now_ms)
{
  if (uuid == UUID_HR_MEAS)
    {
      parse_hr_meas(slot, p, len, now_ms);
    }
  else if (uuid == UUID_CSC_MEAS)
    {
      parse_csc_meas(slot, p, len, now_ms);
    }
  else if (uuid == UUID_CP_MEAS)
    {
      parse_cps_meas(slot, p, len, now_ms);
    }
  else if (uuid == UUID_BAT_LEVEL)
    {
      parse_bat(slot, p, len);
    }
}

/**
 * @brief 遍历 AD 类型，取 16 位 UUID 和本地名。
 *
 * 连接绝不按名：是否入表由 UUID 掩码决定。
 */
static void walk_ad(const uint8_t *ad, uint8_t ad_len, uint32_t now_ms,
                    uint16_t *svc_mask, char *name, size_t namesz)
{
  uint8_t i = 0;

  *svc_mask = 0;
  if (namesz > 0)
    {
      name[0] = '\0';
    }

  while ((uint16_t)i + 1 < ad_len)
    {
      uint8_t elen = ad[i];
      uint8_t type;
      uint8_t dlen;
      const uint8_t *d;
      uint8_t k;

      if (elen == 0 || (uint16_t)i + 1 + elen > ad_len)
        {
          break;
        }

      type = ad[i + 1];
      d    = ad + i + 2;
      dlen = (uint8_t)(elen - 1);
      i    = (uint8_t)(i + 1 + elen);

      if (type == BT_AD_UUID16_SOME || type == BT_AD_UUID16_ALL)
        {
          for (k = 0; (uint16_t)k + 1 < dlen; k = (uint8_t)(k + 2))
            {
              uint16_t u = (uint16_t)(d[k] | ((uint16_t)d[k + 1] << 8));

              if (u == UUID_HRS)
                {
                  *svc_mask |= SENSOR_FEAT_HR;
                }
              else if (u == UUID_CSCS)
                {
                  *svc_mask |= SENSOR_FEAT_CSC;
                }
              else if (u == UUID_CPS)
                {
                  *svc_mask |= SENSOR_FEAT_CPS;
                }
              else if (u == UUID_BAS)
                {
                  *svc_mask |= SENSOR_FEAT_BAT;
                }
            }
        }
      else if (type == BT_AD_SERVICE_DATA16 && dlen >= 2)
        {
          uint16_t u = (uint16_t)(d[0] | ((uint16_t)d[1] << 8));

          if (u == UUID_HRS)
            {
              *svc_mask |= SENSOR_FEAT_HR;
              parse_hr_meas(NULL, d + 2, (uint16_t)(dlen - 2), now_ms);
            }
          else if (u == UUID_CSCS)
            {
              *svc_mask |= SENSOR_FEAT_CSC;
              parse_csc_meas(&g_slots[SENSOR_KIND_CSC], d + 2,
                             (uint16_t)(dlen - 2), now_ms);
            }
          else if (u == UUID_CPS)
            {
              *svc_mask |= SENSOR_FEAT_CPS;
              parse_cps_meas(NULL, d + 2, (uint16_t)(dlen - 2), now_ms);
            }
          else if (u == UUID_BAS)
            {
              *svc_mask |= SENSOR_FEAT_BAT;
            }
        }
      else if ((type == BT_AD_NAME_COMPLETE || type == BT_AD_NAME_SHORT) &&
               namesz > 1 && dlen > 0)
        {
          size_t n = dlen < namesz - 1 ? dlen : namesz - 1;

          memcpy(name, d, n);
          name[n] = '\0';
        }
    }
}

/**
 * @brief 把 GATTC 句柄映射回槽位。
 * @return 槽位，或 NULL。
 */
static struct sensor_slot *slot_by_gatt(gattc_handle_t h)
{
  uint8_t i;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (g_slots[i].gatt == h)
        {
          return &g_slots[i];
        }
    }

  return NULL;
}

/**
 * @brief GATTC 已连接：等 SENSOR_DISCOVER_DELAY_MS 再做 UUID 发现。
 *
 * 回调只拷状态；由 pump() 启动发现。禁止 discover-all。
 */
static void gattc_on_connected(gattc_handle_t h, bt_address_t *addr)
{
  struct sensor_slot *slot = slot_by_gatt(h);
  uint32_t now = sensor_now_ms();

  (void)addr;
  if (slot == NULL)
    {
      return;
    }

  slot->state         = SENSOR_DISCOVERING;
  slot->state_ms      = now;
  slot->op_sent       = false;
  slot->feat          = 0;
  slot->hr_h          = 0;
  slot->csc_h         = 0;
  slot->cps_h         = 0;
  slot->bat_h         = 0;
  slot->loc_h         = 0;
  slot->sub_step      = 0;
  slot->energy_ok     = false;
  slot->bat_ok        = false;
  slot->bat_notify    = false;
  slot->last_bat      = 0;
  slot->disc_step     = 0;
  slot->last_contact  = -1;
  slot->last_loc      = -1;
  slot->last_bpm      = 0;
  slot->last_csc_rpm  = 0;
  slot->last_cps_w    = 0;
  slot->meas_idle     = false;
  slot->drop_link     = false;
  slot->probe_sent    = false;
  slot->probe_ms      = 0;
  slot->bat_ms        = 0;
  slot->notify_ms     = 0;
  slot->meas_sub_ok   = false;
  slot->sub_fail      = 0;
  slot->hci_gone      = false;
  slot->hci_reason    = 0;
  slot->adv_live      = 0;
  {
    char addr_str[BT_ADDR_STR_LENGTH];

    bt_addr_ba2str(&slot->addr, addr_str);
    s_connect_fails = 0u;   /* 连上了：退避清零 */
    s_reconnect_idle_cycles = 0u;   /* 空轮回连退避也清零：有东西连上了 */
    LOGI("%s connected %s", kind_name(slot->kind), addr_str);
  }
}

/**
 * @brief GATTC 断开：want_link 则保留地址，否则槽位回到空闲。
 */
static void gattc_on_disconnected(gattc_handle_t h, bt_address_t *addr)
{
  struct sensor_slot *slot = slot_by_gatt(h);

  if (slot == NULL)
    {
      slot = slot_by_addr(addr);
    }

  if (slot == NULL)
    {
      return;
    }

  /** 与 zblue HCI 备份可能同时到达；只置事件，由 companion pump
   * 串行回收一次，避免回调线程重复修改槽位 / pending bind。
   */

  if (!slot->hci_gone)
    {
      slot->hci_reason = 0;
    }

  slot->hci_gone = true;
}

/**
 * @brief 在 BAS 仍留在 GATTC 表里时缓存 0x2A19 handle。
 *
 * 板卡 SAL keep_db 后第二次发现不会冲掉该 handle。
 */
static void slot_cache_bat_handle(struct sensor_slot *slot)
{
  bt_uuid_t        uuid;
  gatt_attr_desc_t desc;

  uuid = BT_UUID_DECLARE_16(UUID_BAT_LEVEL);
  memset(&desc, 0, sizeof(desc));
  if (bt_gattc_get_attribute_by_uuid(slot->gatt, 0x0001, 0xffff,
                                     &uuid, &desc) != BT_STATUS_SUCCESS)
    {
      pthread_mutex_lock(&g_lock);
      slot->bat_h      = 0;
      slot->bat_notify = false;
      slot->feat      &= (uint16_t)~SENSOR_FEAT_BAT;
      pthread_mutex_unlock(&g_lock);
      return;
    }

  /* 字段写在锁内；SAL 查询本身在锁外（见上）。 */
  pthread_mutex_lock(&g_lock);
  slot->bat_h  = desc.handle;
  slot->feat  |= SENSOR_FEAT_BAT;
  pthread_mutex_unlock(&g_lock);
  LOGD("%s bat handle 0x%04x%s", kind_name(slot->kind),
       (unsigned)slot->bat_h,
       (desc.properties & GATT_PROP_NOTIFY) ? " (notify)" : " (read)");
}

/**
 * @brief UUID 发现完成：先 BAS 缓存电量 handle，再槽位服务，再订测量和电量。
 *
 * 板卡 SAL 第二次发现追加表，BAS 的 0x2A19 仍可查找/订阅。
 */
static void gattc_on_discovered(gattc_handle_t h, gatt_status_t status,
                                bt_uuid_t *uuid, uint16_t start_handle,
                                uint16_t end_handle)
{
  struct sensor_slot *slot = slot_by_gatt(h);
  uint16_t u16;
  char feats[24];

  if (slot == NULL)
    {
      return;
    }

  if (status != GATT_STATUS_SUCCESS)
    {
      if (slot->disc_step == 0)
        {
          LOGI("%s battery skip status %d", kind_name(slot->kind),
               (int)status);
          slot->disc_step = 1;
          slot->state     = SENSOR_DISCOVERING;
          slot->op_sent   = false;
          slot->state_ms  = sensor_now_ms();
          return;
        }

      LOGE("%s discover status %d", kind_name(slot->kind), (int)status);
      slot->drop_link = true;
      return;
    }

  if (uuid != NULL && uuid->type != 0)
    {
      u16 = uuid16_of(uuid);
      if (u16 == UUID_HRS)
        {
          slot->feat |= SENSOR_FEAT_HR;
        }
      else if (u16 == UUID_CSCS)
        {
          slot->feat |= SENSOR_FEAT_CSC;
        }
      else if (u16 == UUID_CPS)
        {
          slot->feat |= SENSOR_FEAT_CPS;
        }

      LOGD("%s svc 0x%04x handles %04x-%04x", kind_name(slot->kind),
           (unsigned)u16, (unsigned)start_handle, (unsigned)end_handle);
      return;
    }

  mask_str(slot->feat, feats, sizeof(feats));
  LOGD("%s discover done feat=%s", kind_name(slot->kind), feats);

  if (slot->disc_step == 0)
    {
      /* gattc_locked_discovered 已持锁。slot_cache_bat_handle 内部要查 SAL
       * （bt_gattc_get_attribute_by_uuid），约定是绝不在持锁时碰 SAL —— 否则
       * 一旦该调用需要 service-loop 线程配合，而 service-loop 正等这把锁，
       * 就是死锁。这里显式让出锁，查完再拿回来。 */
      pthread_mutex_unlock(&g_lock);
      slot_cache_bat_handle(slot);
      pthread_mutex_lock(&g_lock);
      slot->disc_step = 1;
      slot->state     = SENSOR_DISCOVERING;
      slot->op_sent   = false;
      slot->state_ms  = sensor_now_ms();
      LOGD("%s discover 0x%04x", kind_name(slot->kind),
           (unsigned)kind_svc_uuid(slot->kind));
      return;
    }

  if ((slot->feat & kind_feat_bit(slot->kind)) == 0)
    {
      LOGE("%s discover missing 0x%04x", kind_name(slot->kind),
           (unsigned)kind_svc_uuid(slot->kind));
      slot->drop_link = true;
      return;
    }

  slot->state    = SENSOR_SUBSCRIBING;
  slot->state_ms = sensor_now_ms();
  slot->op_sent  = false;
  slot->sub_step = 0;
}

/**
 * @brief CCC 写完；pump 推进 sub_step（测量，然后电量若有 Notify）。
 */
static void gattc_on_subscribed(gattc_handle_t h, gatt_status_t status,
                                uint16_t attr_handle, bool enable)
{
  struct sensor_slot *slot = slot_by_gatt(h);

  if (slot == NULL)
    {
      return;
    }

  if (status == GATT_STATUS_SUCCESS && enable)
    {
      LOGD("%s notify on handle 0x%04x", kind_name(slot->kind),
           (unsigned)attr_handle);
      slot->sub_fail = 0;
      if (attr_handle == slot->hr_h || attr_handle == slot->csc_h ||
          attr_handle == slot->cps_h)
        {
          slot->meas_sub_ok = true;
          slot->sub_step    = 1;
        }
      else if (attr_handle == slot->bat_h)
        {
          slot->sub_step = 2;
        }
    }
  else
    {
      LOGE("%s subscribe status %d handle=0x%04x enable=%d",
           kind_name(slot->kind), (int)status, (unsigned)attr_handle,
           (int)enable);
      slot->sub_fail++;
      if (slot->sub_fail >= SENSOR_SUB_RETRY_MAX)
        {
          slot->drop_link = true;
        }
    }

  slot->op_sent = false;
}

/**
 * @brief 拷贝 notify 载荷，由 pump 解析。刷新 notify_ms（8 s 过期）。
 */
static void gattc_on_notified(gattc_handle_t h, uint16_t attr_handle,
                              uint8_t *value, uint16_t length)
{
  struct sensor_slot *slot = slot_by_gatt(h);
  uint16_t uuid = 0;

  if (slot == NULL || value == NULL)
    {
      return;
    }

  if (attr_handle == slot->hr_h)
    {
      uuid = UUID_HR_MEAS;
      slot->notify_ms = sensor_now_ms();
      slot->meas_idle = false;
      slot->adv_live  = 0;
    }
  else if (attr_handle == slot->csc_h)
    {
      uuid = UUID_CSC_MEAS;
      slot->notify_ms = sensor_now_ms();
      slot->meas_idle = false;
      slot->adv_live  = 0;
    }
  else if (attr_handle == slot->cps_h)
    {
      uuid = UUID_CP_MEAS;
      slot->notify_ms = sensor_now_ms();
      slot->meas_idle = false;
      slot->adv_live  = 0;
    }
  else if (attr_handle == slot->bat_h)
    {
      parse_bat(slot, value, length);
    }

  if (uuid != 0)
    {
      apply_meas(slot, uuid, value, length, sensor_now_ms());
    }
}

/**
 * @brief 体感位置（或其它）读完成。停车探活失败则拆链重连。
 */
static void gattc_on_read(gattc_handle_t h, gatt_status_t status,
                          uint16_t attr_handle, uint8_t *value, uint16_t length)
{
  struct sensor_slot *slot = slot_by_gatt(h);
  bool was_probe;

  if (slot == NULL)
    {
      return;
    }

  was_probe = slot->probe_sent &&
              (attr_handle == slot->bat_h || attr_handle == slot->csc_h ||
               attr_handle == slot->cps_h || attr_handle == slot->hr_h);
  if (was_probe)
    {
      slot->probe_sent = false;
    }

  if (status != GATT_STATUS_SUCCESS || value == NULL)
    {
      if (was_probe && slot_keep_link(slot) &&
          (slot->meas_idle || slot->kind == SENSOR_KIND_HR))
        {
          LOGI("%s idle ATT status %d, reconnect",
               kind_name(slot->kind), (int)status);
          slot->drop_link = true;
        }

      return;
    }

  if (was_probe && slot->kind == SENSOR_KIND_HR)
    {
      slot->notify_ms = sensor_now_ms();
    }

  if (attr_handle == slot->bat_h)
    {
      parse_bat(slot, value, length);
    }
  else if (attr_handle == slot->loc_h)
    {
      parse_loc(slot, value, length);
    }
}

/**
 * @brief 回调加锁包装：只覆盖回调自身的字段写入，不含任何 SAL 调用。
 *
 * @details
 * 七个回调的实体里都没有 bt_* 调用（唯一一个在 slot_cache_bat_handle 里，
 * 由 gattc_on_discovered 显式解锁后再调用），所以在这里整段持锁不会把 SAL
 * 拖进临界区，也不会与 pump 形成锁序问题。
 *
 * 为什么用包装而不是改每个函数体：注册点集中，改动面小，
 * 且"回调写入原子"这个不变量一眼可见。
 */
static void gattc_locked_connected(gattc_handle_t h, bt_address_t *addr)
{
  pthread_mutex_lock(&g_lock);
  gattc_on_connected(h, addr);
  pthread_mutex_unlock(&g_lock);
}

static void gattc_locked_disconnected(gattc_handle_t h, bt_address_t *addr)
{
  pthread_mutex_lock(&g_lock);
  gattc_on_disconnected(h, addr);
  pthread_mutex_unlock(&g_lock);
}

static void gattc_locked_discovered(gattc_handle_t h, gatt_status_t status,
                                    bt_uuid_t *uuid, uint16_t start_handle,
                                    uint16_t end_handle)
{
  pthread_mutex_lock(&g_lock);
  gattc_on_discovered(h, status, uuid, start_handle, end_handle);
  pthread_mutex_unlock(&g_lock);
}

static void gattc_locked_subscribed(gattc_handle_t h, gatt_status_t status,
                                    uint16_t attr_handle, bool enable)
{
  pthread_mutex_lock(&g_lock);
  gattc_on_subscribed(h, status, attr_handle, enable);
  pthread_mutex_unlock(&g_lock);
}

static void gattc_locked_notified(gattc_handle_t h, uint16_t attr_handle,
                                  uint8_t *value, uint16_t length)
{
  pthread_mutex_lock(&g_lock);
  gattc_on_notified(h, attr_handle, value, length);
  pthread_mutex_unlock(&g_lock);
}

static void gattc_locked_read(gattc_handle_t h, gatt_status_t status,
                              uint16_t attr_handle, uint8_t *value,
                              uint16_t length)
{
  pthread_mutex_lock(&g_lock);
  gattc_on_read(h, status, attr_handle, value, length);
  pthread_mutex_unlock(&g_lock);
}

static gattc_callbacks_t g_gattc_cbs =
{
  .size = sizeof(gattc_callbacks_t),
  .on_connected    = gattc_locked_connected,
  .on_disconnected = gattc_locked_disconnected,
  .on_discovered   = gattc_locked_discovered,
  .on_read         = gattc_locked_read,
  .on_subscribed   = gattc_locked_subscribed,
  .on_notified     = gattc_locked_notified,
};

/**
 * @brief 句柄卡死（断链回调丢失）时拆掉重建 GATTC。
 */
static int slot_gattc_recreate(struct sensor_slot *slot)
{
  bt_status_t ret;

  if (g_sensor_ins == NULL || slot == NULL)
    {
      return -1;
    }

  if (slot->gatt != NULL)
    {
      (void)bt_gattc_delete_connect(slot->gatt);
      slot->gatt = NULL;
    }

  slot->gatt_ok = false;
  g_gattc_cbs.size = sizeof(gattc_callbacks_t);
  ret = bt_gattc_create_connect(g_sensor_ins, &slot->gatt, &g_gattc_cbs);
  if (ret != BT_STATUS_SUCCESS || slot->gatt == NULL)
    {
      LOGE("gattc recreate %s failed: %d", kind_name(slot->kind), (int)ret);
      ble_supervisor_report(BLE_SUPERVISOR_FAULT_GATTC, sensor_now_ms());
      return -1;
    }

  slot->gatt_ok = true;
  g_health_recreate_n++;
  LOGI("%s GATTC recreated", kind_name(slot->kind));
  return 0;
}

/**
 * @brief 可选的原始 ADV hex 转储（调试）。
 */
static void adv_debug_print(const ble_scan_result_t *result,
                            uint16_t svc_mask, const char *name)
{
  char     addr[BT_ADDR_STR_LENGTH];
  char     hex[ADV_HEX_MAX * 2 + 4];
  char     match[16];
  uint8_t  n;
  uint8_t  i;
  size_t   off = 0;

  bt_addr_ba2str(&result->addr, addr);

  match[0] = '\0';
  if ((svc_mask & (1u << SENSOR_KIND_HR)) != 0)
    {
      strcat(match, "HR ");
    }

  if ((svc_mask & (1u << SENSOR_KIND_CSC)) != 0)
    {
      strcat(match, "CSC ");
    }

  if ((svc_mask & (1u << SENSOR_KIND_CPS)) != 0)
    {
      strcat(match, "CPS ");
    }

  if (match[0] == '\0')
    {
      strcpy(match, "-");
    }

  n = result->length;
  if (n > ADV_HEX_MAX)
    {
      n = ADV_HEX_MAX;
    }

  for (i = 0; i < n; i++)
    {
      hex[off++] = "0123456789ABCDEF"[(result->adv_data[i] >> 4) & 0x0f];
      hex[off++] = "0123456789ABCDEF"[result->adv_data[i] & 0x0f];
    }

  if (result->length > ADV_HEX_MAX)
    {
      hex[off++] = '.';
      hex[off++] = '.';
      hex[off++] = '.';
    }

  hex[off] = '\0';

  LOGI("[scan] ADV rssi=%d atype=%u evt=%u %s name=%s match=%s hex=%s",
       result->rssi, result->addr_type, result->adv_type, addr,
       (name != NULL && name[0] != '\0') ? name : "-", match,
       off > 0 ? hex : "-");
}

/**
 * @brief 观察者回调：拷贝 ADV，更新表格 / 重连 seen。
 *
 * 禁止在此回调里连接；GATT 由 pump() 负责。
 */
static void on_scan_result(bt_scanner_t *scanner, ble_scan_result_t *result)
{
  uint16_t svc_mask = 0;
  char     name[SENSOR_NAME_MAX];
  uint8_t  kind;
  uint32_t now;
  struct sensor_slot *slot;

  if (!g_started || scanner != g_scanner || result == NULL)
    {
      return;
    }

  now = sensor_now_ms();
  walk_ad(result->adv_data, result->length, now, &svc_mask,
          name, sizeof(name));

  g_adv_reports++;
  seen_note(&result->addr);
  if (g_adv_debug)
    {
      adv_debug_print(result, svc_mask, name);
    }

  if (svc_mask != 0 && SENSOR_RSSI_ACCEPT(result->rssi))
    {
      found_upsert(result, svc_mask, name);
    }

  /** 已绑定槽位按地址认人，不要求这包 ADV 里带服务 UUID。 */

  if (SENSOR_RSSI_ACCEPT(result->rssi))
    {
      for (kind = 0; kind < SENSOR_KIND_COUNT; kind++)
        {
          slot = &g_slots[kind];
          if (!slot->want_link || !addr_eq(&slot->addr, &result->addr))
            {
              continue;
            }

          slot->rssi      = result->rssi;
          slot->seen_ms   = now;
          slot->addr_type = result->addr_type;
          if (name[0] != '\0')
            {
              memcpy(slot->name, name, sizeof(slot->name));
            }

          if (slot->state == SENSOR_IDLE)
            {
              /* 尚可才认「可建链」；弱信号只更新 rssi，等更近再连。
               * `SENSOR_RSSI_GATE_OFF` 期间恒真 —— 见该开关的注释。 */
                  if (SENSOR_RSSI_CONNECTABLE(result->rssi))
                {
                  slot->adv_seen   = true;
                  slot->skip_dwell = true;
                  if (!sensor_radio_in_phone_first(now))
                    {
                      sensor_radio_kick_scan(now, "rssi-ok");
                    }
                }
            }
          else if ((slot->state == SENSOR_READY ||
                    slot->state == SENSOR_SUBSCRIBING) &&
                   adv_is_connectable(result->adv_type))
            {
              bool alive;

              alive = slot_link_alive(slot, now);
              if (!alive)
                {
                  slot->adv_live++;
                  if (slot->adv_live >= SENSOR_HEALTH_ADV_HITS)
                    {
                      slot->drop_link = true;
                      slot->rearm_adv = true;
                      g_health_zombie_n++;
                      LOGI("%s advertised while linked (zombie), reconnect",
                           kind_name(kind));
                    }
                }
            }
        }
    }

  /** 扫描回调到此为止。它只做两件事，**不再替任何槽位挑设备**：
   *
   *   1. 上面的 found_upsert() 维护 UI 的发现列表（`sys found` / 菜单扫描页）；
   *   2. 上面的 want_link 循环刷新已武装槽位的 rssi / adv_seen。
   *
   *  这里原先还有一段「保留每个 kind 里信号最强的广播者」的候选绑定：它会给
   *  `slot->addr` 填上一台用户**从没配对过**的设备，随后 `slot_try_connect()`
   *  借 `g_allow_connect`（sensor 策略开着时开机即为真，见
   *  `companion_apply_sensor_policy()`）把 `want_link` 一置，这台设备就成了
   *  「回连目标」。现场的两个毛病都出在这段：
   *
   *   - **删了还回连**：菜单里删掉记录后它照样回来 —— 回连依据根本不是
   *     /mnt/kv 里的记录，而是每次开机现挑的那个"最强广播者"（记录删了，
   *     KV 里没它，但 ADV 还在，于是又被挑中）。
   *   - **功率计连不上**：一台同时广播 CSC(0x1816)+CPS(0x1818) 的表会被
   *     CSC 槽位先挑走（HR > CSC > CPS 的取第一种匹配），功率计页再点它就
   *     只能被「一个地址一条 GATTC」挡掉。
   *
   *  现在的规则只有一条：**只有配对过的地址才回连**。武装由
   *  `companion_sensor_autorc_all()` 按 `/mnt/kv/bicycle_sensors.tsv` 里
   *  autorc 的记录做（→ `ble_sensor_connect_addr()`），记录删了就不再武装；
   *  用户点选走 `ble_sensor_connect_index_kind()`。两者都自己把
   *  `want_link` 置好，等上面的 want_link 循环把 `adv_seen` 打开即可建链。
   */
}

/**
 * @brief 允许连接时，每个槽位创建一个 GATTC。
 */
static void sensor_ensure_gattc(void)
{
  uint8_t     i;
  bt_status_t ret;

  if (g_sensor_ins == NULL)
    {
      return;
    }

  g_gattc_cbs.size = sizeof(gattc_callbacks_t);

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (g_slots[i].gatt_ok)
        {
          continue;
        }

      ret = bt_gattc_create_connect(g_sensor_ins, &g_slots[i].gatt,
                                    &g_gattc_cbs);
      if (ret != BT_STATUS_SUCCESS || g_slots[i].gatt == NULL)
        {
          LOGE("gattc create %s failed: %d", kind_name(i), (int)ret);
        }
      else
        {
          g_slots[i].gatt_ok = true;
        }
    }
}

/**
 * @brief 框架扫描启动结果。失败则丢掉已释放的句柄。
 */
static void on_scan_start_status(bt_scanner_t *scanner, uint8_t status)
{
  if (status != BT_SCAN_STATUS_SUCCESS)
    {
      LOGE("[scan] start status=%u", (unsigned)status);
      if (g_scanner == scanner)
        {
          g_scanner = NULL;
        }
    }
}

/**
 * @brief 框架扫描已停。
 */
static void on_scan_stopped(bt_scanner_t *scanner)
{
  (void)scanner;
}

/**
 * @brief 扫描结果回调加锁包装。
 *
 * @details
 * on_scan_result 会往 g_found[] / g_seen[] 写，而 ble_sensor_scan_arm() 会在
 * companion 线程清这两张表；以前两边都无锁，可能把回调刚写的一行连同计数一起
 * 清零，或者 count 与表内容不一致（后续按 idx 取到全零地址）。这里让回调写入
 * 与清表互斥。回调体内没有 SAL 调用（只是填表 + 置标志），所以整段持锁安全。
 */
static void on_scan_result_locked(bt_scanner_t *scanner, ble_scan_result_t *result)
{
  pthread_mutex_lock(&g_lock);
  on_scan_result(scanner, result);
  pthread_mutex_unlock(&g_lock);
}

static const scanner_callbacks_t g_scan_cbs =
{
  sizeof(g_scan_cbs),
  on_scan_result_locked,
  on_scan_start_status,
  on_scan_stopped,
};

/** @brief 适配器此刻是否可用（语义同 `ble_companion.c` 的 `adapter_is_usable`）。
 *
 *  2026-09-24 02:50 转储 n589 的护栏 —— 那一崩就在 `bt_le_scan_user_remove()`
 *  里对**已经坏掉的扫描上下文**做原子清位（`cfsr=0x82` DACCVIOL+MMARVALID、
 *  `mmfar=0x10`、`pc=stlex [r3]`），任务正是 `ble_companion`。
 *
 *  为什么需要它：上游 zblue 的护栏**不对称** ——
 *  `bt_le_scan_start()` 会先查 `BT_DEV_READY`，不 ready 就返回 `-EAGAIN`
 *  （现场那串 `bt_le_scan_start fail, err:-11` 就是它，**安全**）；
 *  而 `bt_le_scan_stop()` → `bt_le_scan_user_remove()` **不查**，
 *  直接 `atomic_clear_bit(hdev->scan_ctx->…->scan_flags, flag)` ——
 *  适配器已 down 时那条链是坏值 ⇒ MemManage ⇒ panic ⇒ 重启。
 *
 *  所以在我们这一侧：**适配器不可用时绝不把手柄交给协议栈**。
 *  此时那些手柄本身也已失效，丢掉即可（下面 `g_scanner = NULL` 照做）。 */
static bool sensor_adapter_usable(void)
{
  bt_adapter_state_t st;

  if (g_sensor_ins == NULL)
    {
      return false;
    }

  st = bt_adapter_get_state(g_sensor_ins);
  return st == BT_ADAPTER_STATE_ON || st == BT_ADAPTER_STATE_BLE_ON;
}

/**
 * @brief 只向栈发 stop；句柄随后由 scan_manager 释放。
 */
static void sensor_observer_halt(void)
{
  if (g_scanner != NULL && sensor_adapter_usable())
    {
      bt_le_stop_scan(g_sensor_ins, g_scanner);
    }
  else if (g_scanner != NULL)
    {
      /* 适配器不可用/正在恢复：**不下发**，只丢手柄。
       * 留一行日志，免得"扫描怎么没停"变成谜团（n589 就是没护栏才炸的）。 */
      LOGI("observer halt skipped, adapter not usable (handle dropped)");
    }

  g_scanner = NULL;
}

/**
 * @brief 启动框架观察者。搜表用 LOW_LATENCY；重连用 BALANCED / LOW_POWER。
 * @param duty @c SCAN_DUTY_DISCOVERY 或 @c SCAN_DUTY_RECONNECT。
 * @return 成功返回 0。
 */
static int sensor_start_scan(uint8_t duty)
{
  ble_scan_settings_t settings;
  uint8_t mode;

  if (duty == SCAN_DUTY_RECONNECT && sensor_hci_connecting())
    {
      return 0;
    }

  if (g_scanner != NULL && g_scan_duty == duty)
    {
      return 0;
    }

  /* 重连接扫限频：现场配对窗后 observer on 每 ~30 ms 刷一次，和建链抢射频。 */
  if (duty == SCAN_DUTY_RECONNECT &&
      s_scan_start_ms != 0 &&
      sensor_age_ms(sensor_now_ms(), s_scan_start_ms) < SENSOR_SCAN_RESTART_MIN_MS)
    {
      return 0;
    }

  if (g_scanner != NULL)
    {
      sensor_observer_halt();
    }

  if (g_sensor_ins == NULL)
    {
      LOGE("[scan] no bt instance");
      return -1;
    }

  if (duty == SCAN_DUTY_DISCOVERY)
    {
      mode = BT_SCAN_MODE_LOW_LATENCY;
    }
  else if (sensor_ready_count() >= 1)
    {
      mode = BT_SCAN_MODE_LOW_POWER;
    }
  else
    {
      mode = BT_SCAN_MODE_BALANCED;
    }

  memset(&settings, 0, sizeof(settings));
  settings.scan_mode     = mode;
  settings.legacy        = 1;
  settings.scan_type     = BT_LE_SCAN_TYPE_PASSIVE;
  settings.scan_phy      = BT_LE_1M_PHY;
  settings.policy.policy = BT_LE_SCAN_POLICY_ACCEPT_ALL;

  g_scanner = bt_le_start_scan_settings(g_sensor_ins, &settings, &g_scan_cbs);
  if (g_scanner == NULL)
    {
      LOGE("[scan] bt_le_start_scan_settings failed");
      ble_supervisor_report(BLE_SUPERVISOR_FAULT_SCAN, sensor_now_ms());
      return -1;
    }

  g_scan_duty = duty;
  s_scan_start_ms = sensor_now_ms();
  LOGI("[scan] observer on (mode=%u, %s)", (unsigned)mode,
       duty == SCAN_DUTY_RECONNECT ? "reconnect" : "discovery");
  return 0;
}

/**
 * @brief 只停观察者，不断 GATT。
 */
static void sensor_observer_stop(void)
{
  g_want_scan     = false;
  g_scan_until_ms = 0;
  sensor_observer_halt();
}

static bool sensor_pending_connect(void)
{
  uint8_t i;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      /** slot_try_connect() 对 want_link 槽位仍要求先看到该地址的 ADV。
       * skip_dwell 只跳过驻留时间，不能表示“已经可以建链”。断链回到
       * IDLE 时它会被置 true；若在这里据此 hold，手机断开后的每个
       * ADV_WINDOW 都会被永久压住。
       */
      if (g_slots[i].want_link && !addr_empty(&g_slots[i].addr) &&
          g_slots[i].state == SENSOR_IDLE && g_slots[i].adv_seen)
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief 只在限时搜表、即将建链或 HCI 建链时停手机广播。
 *
 * 否则踏频停车后的 reconnect observer 会一直 hold，手机扫不到码表。
 */
static void sensor_adv_hold_update(void)
{
  extern bool myvendor_pair_window_active(void);
  bool discovery;
  bool slot_scan;
  bool want;

  /* 配对窗口：射频只给可发现广播，禁止因传感器扫/建链停手机 ADV。 */
  if (myvendor_pair_window_active())
    {
      want = false;
    }
  else
    {
      discovery = g_want_scan && g_scan_duty == SCAN_DUTY_DISCOVERY;
      slot_scan = g_radio_phase == SENSOR_RADIO_ADV_STOP_WAIT ||
                  g_radio_phase == SENSOR_RADIO_SCAN_WINDOW;
      /** 这里**不能**用 `ble_sensor_connect_busy()`：它把"绑过地址但当前
       *  IDLE"也算 busy（`want_link && addr && state != READY`），那是**常驻**
       *  条件 —— 绑定的外设不在场时它恒真，让位闩锁就再也不回落，
       *  `g_hold_le_adv` 常 1 ⇒ `companion_start_advertising()` 一停不返
       *  ⇒ 手机永久扫不到设备。
       *
       *  现场 2026-09-23 13:13~14:25（`main-20260923-131318.dat`）：外设关机
       *  72 分钟，`[radio] tick` 恒为 `phone=0 held=1 want=1 reconnect=1 busy=1`、
       *  `hold le adv off`/`adv active` 各 0 次，手机侧每 30.5 分钟一轮
       *  `TimeoutException after 0:30:00`。同一版固件健康会话里让位最长 768 s
       *  且 32 次全闭合（见 analysis/BLE_ADV_HOLD_LOCKOUT.md）。
       *
       *  覆盖并不减少：正在建链由 `sensor_hci_connecting()` 管，"看见过它的广播、
       *  即将建链"由 `sensor_pending_connect()` 管（后者要求 `adv_seen`），
       *  扫描时片由上面的 `slot_scan` 管 —— 与本函数文档注释的三种情形一致。
       *
       *  自证伪：若改后出现"外设在眼前却被手机广播挤掉、建链成功率下降"，
       *  说明确实需要按 busy 扩让位 —— 那时应改成**有界**让位（上限几十秒），
       *  而不是恢复成无界闩锁。 */
      want = ((!g_phone_connected && discovery) || slot_scan ||
              sensor_hci_connecting() || sensor_pending_connect());
    }

  if (want == g_adv_hold_want)
    {
      return;
    }

  g_adv_hold_want = want;
  if (want)
    {
      g_hold_request_pend = true;
      g_hold_release_pend = false;
      LOGI("hold phone adv (discovery or connecting)");
    }
  else
    {
      g_hold_release_pend = true;
      g_hold_request_pend = false;
      LOGI("resume phone adv");
    }
}

/**
 * @brief 配对窗口上升沿：停扫、拆传感器链（保留 want_link）、强制放行手机 ADV。
 *
 * 现场：窗口已开仍 `adv held by sensor` + `busy=1`，整段 30 s 手机扫不到码表。
 */
static void sensor_pair_window_guard(uint32_t now_ms)
{
  extern bool myvendor_pair_window_active(void);
  bool win = myvendor_pair_window_active();
  uint8_t i;

  if (win && !s_pair_win_prev)
    {
      LOGI("[radio] pair window ON: sensors yield, ADV only");
      if (g_scanner != NULL)
        {
          sensor_observer_halt();
        }

      g_want_scan = false;
      g_scan_until_ms = 0;
      if (g_radio_phase != SENSOR_RADIO_IDLE)
        {
          sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
          g_radio_deadline_ms = 0;
        }

      for (i = 0; i < SENSOR_KIND_COUNT; i++)
        {
          struct sensor_slot *s = &g_slots[i];

          /* want_link 保留 → 窗口结束后自动回连；此处只拆当前占用射频的链路。 */
          if (s->gatt != NULL && s->state >= SENSOR_CONNECTING &&
              s->state != SENSOR_DISCONNECTING)
            {
              slot_begin_disconnect(s, now_ms);
            }
        }

      sensor_adv_hold_update();
    }
  else if (!win && s_pair_win_prev)
    {
      /* 冷静期：手机刚配对完仍在加密/服务发现，立刻扫传感器易 0x3e。 */
      s_pair_cool_until_ms = now_ms + SENSOR_PAIR_COOL_MS;
      if (g_scanner != NULL)
        {
          sensor_observer_halt();
        }

      g_want_scan = false;
      if (g_radio_phase != SENSOR_RADIO_IDLE)
        {
          sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
          g_radio_deadline_ms = 0;
        }

      LOGI("[radio] pair window OFF: cool %u ms then sensors reconnect",
           (unsigned)SENSOR_PAIR_COOL_MS);
    }

  s_pair_win_prev = win;
}

/**
 * @brief 丢弃上一次扫描的结果（清 found/seen 表）。
 *
 * @details
 * 语义：**每次开始扫描都必须从空表开始**。否则上一轮的设备会留在列表里，
 * 用户看到的是"这次没扫到，但列表里还有"——分不清是旧结果还是新结果，
 * 尤其是同一台设备在两轮里 RSSI 不同的情况。
 *
 * 原来只有用户起扫的 `ble_sensor_scan_arm()` 清了表，两处**后台重连观察**
 * （`sensor_reconnect_scan()` / `sensor_reconnect_radio_pump()`）直接
 * `g_want_scan = true` 就起扫，旧结果会跨轮残留。现在统一走这里。
 *
 * 调用者**不要**持 `g_lock`（本函数自己加；`scan_arm` 里持锁的那段用的是
 * 同样的字段，仍在它自己的临界区内完成）。
 */
static void sensor_found_reset(void)
{
  /* 清表必须与扫描回调的写互斥：否则可能把回调刚 upsert 的一行连同 g_found_n
   * 一起清掉，或留下 count 与表内容不一致（后续按 idx 取到全零地址）。 */
  pthread_mutex_lock(&g_lock);
  memset(g_found, 0, sizeof(g_found));
  memset(g_seen, 0, sizeof(g_seen));
  g_found_n = 0;
  g_seen_n  = 0;
  pthread_mutex_unlock(&g_lock);
}

/**
 * @brief 断开后低占空比观察，等到已保存地址再广播。
 */
static void sensor_reconnect_scan(void)
{
  if (sensor_hci_connecting())
    {
      return;
    }

  /* **不要在这里清表。**
   *
   * 后台重连观察与用户扫描共用同一张 g_found[]：在这里清会把用户那次
   * "点扫描"刚扫到的结果整表抹掉，现象就是"扫不出设备 / 列表瞬间空了"。
   * 清表只该发生在**用户起扫**那一刻（见 ble_sensor_scan_arm()）。 */
  g_want_scan     = true;
  g_scan_until_ms = 0;
  (void)sensor_start_scan(SCAN_DUTY_RECONNECT);
}

/**
 * @brief 手机优先窗 / 稳态 ADV(或静默)↔短扫。手机已连时同样走间隔扫。
 *
 * @return true 表示本拍的重连 observer 已由时间片状态机处理。
 */
static bool sensor_reconnect_radio_pump(uint32_t now_ms)
{
  bool reconnect;
  bool connect_busy;
  bool link_inflight;

  /* 手机优先窗内：保证不扫，射频全给 companion ADV。 */
  if (!g_phone_connected && sensor_radio_in_phone_first(now_ms))
    {
      if (g_scanner != NULL)
        {
          sensor_observer_halt();
        }

      if (g_radio_phase != SENSOR_RADIO_IDLE)
        {
          sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
          g_radio_deadline_ms = 0;
        }

      if (g_radio_dbg_ms == 0 ||
          sensor_age_ms(now_ms, g_radio_dbg_ms) >= 10000u)
        {
          g_radio_dbg_ms = now_ms;
          LOGI("[radio] tick phone-first left=%d ms",
               (int)(s_phone_first_until_ms - now_ms));
        }

      return true;
    }

  /* 配对窗刚关：冷静期内不扫不建链，避免和手机加密/服务发现抢射频。 */
  if (s_pair_cool_until_ms != 0u &&
      (int32_t)(s_pair_cool_until_ms - now_ms) > 0)
    {
      if (g_scanner != NULL)
        {
          sensor_observer_halt();
        }

      if (g_radio_phase != SENSOR_RADIO_IDLE)
        {
          sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
          g_radio_deadline_ms = 0;
        }

      if (g_radio_dbg_ms == 0 ||
          sensor_age_ms(now_ms, g_radio_dbg_ms) >= 2000u)
        {
          g_radio_dbg_ms = now_ms;
          LOGI("[radio] tick pair-cool left=%d ms",
               (int)(s_pair_cool_until_ms - now_ms));
        }

      return true;
    }

  if (s_pair_cool_until_ms != 0u)
    {
      s_pair_cool_until_ms = 0;
      LOGI("[radio] pair-cool done, sensors may reconnect");
    }

  reconnect = sensor_want_reconnect_observer(now_ms);
  connect_busy = ble_sensor_connect_busy();
  /* 只有真正飞在 CONNECTING…SUBSCRIBING 才静默射频。
   * ble_sensor_connect_busy() 在「want_link 但 IDLE」时也是 true ——
   * 若拿它挡扫，phone-first 结束后永远 scanner=0（现场 2026-09-23 01:20）。 */
  link_inflight = sensor_hci_connecting();

  /* 建链 / GATT 发现中：停扫、相位 IDLE，把射频留给 ATT（禁止补扫风暴）。 */
  if (link_inflight && reconnect)
    {
      if (g_scanner != NULL)
        {
          sensor_observer_halt();
        }

      if (g_radio_phase != SENSOR_RADIO_IDLE)
        {
          sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
          g_radio_deadline_ms = 0;
        }

      /* 仅允许 ADV_STOP 卡死救援；其它 heal 在 watchdog 内已按 inflight 过滤。 */
      sensor_radio_heal_watchdog(now_ms, reconnect, true);

      if (g_radio_dbg_ms == 0 ||
          sensor_age_ms(now_ms, g_radio_dbg_ms) >= 10000u)
        {
          g_radio_dbg_ms = now_ms;
          LOGI("[radio] tick phase=%d phone=%d held=%d want=%d scanner=%d "
               "reconnect=%d busy=%d inflight=1 deadline=%d heal=%u",
               (int)g_radio_phase, g_phone_connected ? 1 : 0,
               g_phone_adv_held ? 1 : 0,
               g_adv_hold_want ? 1 : 0, g_scanner != NULL ? 1 : 0,
               reconnect ? 1 : 0, connect_busy ? 1 : 0,
               g_radio_deadline_ms != 0 ? 1 : 0,
               (unsigned)g_health_radio_heal_n);
        }

      return true;
    }

  if (!reconnect)
    {
      g_radio_reconnect_since_ms = 0;
    }

  if (g_radio_dbg_ms == 0 ||
      sensor_age_ms(now_ms, g_radio_dbg_ms) >= 10000u)
    {
      g_radio_dbg_ms = now_ms;
      LOGI("[radio] tick phase=%d phone=%d held=%d want=%d scanner=%d "
           "reconnect=%d busy=%d deadline=%d heal=%u",
           (int)g_radio_phase, g_phone_connected ? 1 : 0,
           g_phone_adv_held ? 1 : 0,
           g_adv_hold_want ? 1 : 0, g_scanner != NULL ? 1 : 0,
           reconnect ? 1 : 0, connect_busy ? 1 : 0,
           g_radio_deadline_ms != 0 ? 1 : 0,
           (unsigned)g_health_radio_heal_n);

      /** **只加观测、不改行为**：把"有没有听到这个绑定地址 / 听到时多强"打出来。
       *
       *  建链闸门是 `slot_keep_link() && slot->adv_seen`，而 `adv_seen` 只在收到
       *  **该地址**的广播且 `rssi >= SENSOR_CONNECT_RSSI_MIN(-75)` 时打开；
       *  `seen_ms` 则是"只要匹配上该地址（rssi >= -90）就刷新"。两者以前都没有日志，
       *  现场只能看到 `empty=4` 与 companion 那句 `seen=0` 干瞪眼
       *  （2026-09-23 16:2x 用户："只有心率计在开机，但设备半天没连上"）。
       *
       *  判读：
       *    · `last=never` 一直不变 ⇒ **根本没匹配上这个地址**（记录里的地址不是它
       *      现在广播的 / 不在范围内 / rssi 连 -90 都不到）；
       *    · `last=` 在刷新但 `seen=0` ⇒ **听到了，只是没过 -75 门槛**（远近问题）；
       *    · `seen=1` 却不建链 ⇒ 闸门之外的问题（另查）。
       *
       *  自证伪：若这行自己不出现，说明没有已绑定槽位（`slot_keep_link` 全假）。 */
      {
        char line[192];
        size_t off = 0;
        uint8_t k;

        for (k = 0; k < SENSOR_KIND_COUNT; k++)
          {
            const struct sensor_slot *s = &g_slots[k];

            if (!slot_keep_link(s))
              {
                continue;
              }

            if (off < sizeof(line) - 48u)
              {
                if (s->seen_ms == 0u)
                  {
                    off += (size_t)snprintf(line + off, sizeof(line) - off,
                                            "%s%s st=%u seen=%u rssi=%d last=never",
                                            off != 0 ? " | " : "",
                                            kind_name(s->kind),
                                            (unsigned)s->state,
                                            s->adv_seen ? 1u : 0u, (int)s->rssi);
                  }
                else
                  {
                    uint32_t age = sensor_age_ms(now_ms, s->seen_ms);

                    off += (size_t)snprintf(line + off, sizeof(line) - off,
                                            "%s%s st=%u seen=%u rssi=%d last=%u.%us",
                                            off != 0 ? " | " : "",
                                            kind_name(s->kind),
                                            (unsigned)s->state,
                                            s->adv_seen ? 1u : 0u, (int)s->rssi,
                                            (unsigned)(age / 1000u),
                                            (unsigned)((age % 1000u) / 100u));
                  }
              }
          }

        if (off != 0)
          {
            /* 门槛状态如实打出来：`SENSOR_RSSI_GATE_OFF` 期间说 "无门槛"，
             * 否则会让人拿 -75 去解释一个根本不设门槛的现象。 */
#ifdef SENSOR_RSSI_GATE_OFF
            LOGI("[sensor] %s (建链需 seen=1；RSSI 门槛已关)", line);
#else
            LOGI("[sensor] %s (建链需 seen=1 且 rssi>=%d)",
                 line, (int)SENSOR_CONNECT_RSSI_MIN);
#endif
          }
      }
    }

  if (!reconnect ||
      (g_want_scan && g_scan_duty == SCAN_DUTY_DISCOVERY))
    {
      if (g_radio_phase != SENSOR_RADIO_IDLE)
        {
          sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
          g_radio_deadline_ms = 0;
        }

      return false;
    }

  /* 自愈在推进相位之前跑：卡住时 kick 进 ADV_STOP_WAIT，本拍 switch 接上。
   * heal 的 busy 用 link_inflight，勿用 want_link-IDLE。 */
  sensor_radio_heal_watchdog(now_ms, reconnect, link_inflight);

  g_want_scan = true;
  g_scan_duty = SCAN_DUTY_RECONNECT;

  switch (g_radio_phase)
    {
      case SENSOR_RADIO_IDLE:
        if (g_scanner != NULL)
          {
            sensor_observer_halt();
          }

        /* 手机已连：ADV_WINDOW = 静默间隔；未连：真广播窗。 */
        sensor_radio_enter_phase(SENSOR_RADIO_ADV_WINDOW, now_ms);
        g_radio_deadline_ms = now_ms + SENSOR_STEADY_ADV_MS;
        LOGI("[radio] steady %s window %u ms",
             g_phone_connected ? "quiet" : "ADV",
             (unsigned)SENSOR_STEADY_ADV_MS);
        break;

      case SENSOR_RADIO_ADV_WINDOW:
        if (g_scanner != NULL)
          {
            sensor_observer_halt();
          }

        /* deadline 丢失：当作立刻到期，避免永久安静。 */
        if (g_radio_deadline_ms == 0 || now_ms >= g_radio_deadline_ms)
          {
            if (g_phone_connected)
              {
                /* 无手机 ADV：直接开扫。 */
                (void)sensor_start_scan(SCAN_DUTY_RECONNECT);
                sensor_radio_note_scan_window(now_ms);
                g_radio_deadline_ms = now_ms + SENSOR_STEADY_SCAN_MS;
                if (s_connect_fails > 0u)
                  {
                    uint32_t stretch = SENSOR_STEADY_SCAN_MS << s_connect_fails;

                    if (stretch > 8000u)
                      {
                        stretch = 8000u;
                      }

                    g_radio_deadline_ms = now_ms + stretch;
                  }

                LOGI("[radio] phone-linked scan window");
              }
            else
              {
                sensor_radio_enter_phase(SENSOR_RADIO_ADV_STOP_WAIT, now_ms);
                g_radio_deadline_ms = now_ms + SENSOR_ADV_STOP_ACK_MS;
                LOGI("[radio] stop phone ADV for short scan");
              }
          }
        break;

      case SENSOR_RADIO_ADV_STOP_WAIT:
        if (g_phone_connected || g_phone_adv_held ||
            g_radio_deadline_ms == 0 ||
            now_ms >= g_radio_deadline_ms ||
            (g_radio_phase_ms != 0 &&
             sensor_age_ms(now_ms, g_radio_phase_ms) >=
             SENSOR_RADIO_STOP_WAIT_MAX_MS))
          {
            uint32_t scan_ms = SENSOR_STEADY_SCAN_MS;

            (void)sensor_start_scan(SCAN_DUTY_RECONNECT);
            sensor_radio_note_scan_window(now_ms);
            if (s_connect_fails > 0u)
              {
                uint32_t stretch = SENSOR_STEADY_SCAN_MS << s_connect_fails;

                if (stretch > 8000u)
                  {
                    stretch = 8000u;
                  }

                scan_ms = stretch;
              }

            g_radio_deadline_ms = now_ms + scan_ms;
            LOGI("[radio] scan window %u ms (held=%d phone=%d)",
                 (unsigned)scan_ms, g_phone_adv_held ? 1 : 0,
                 g_phone_connected ? 1 : 0);
          }
        break;

      case SENSOR_RADIO_SCAN_WINDOW:
        /* 建链中本应由上方 link_inflight 早退；若仍落到此，停扫勿补扫。 */
        if (link_inflight)
          {
            if (g_scanner != NULL)
              {
                sensor_observer_halt();
              }

            sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
            g_radio_deadline_ms = 0;
            break;
          }

        if (g_radio_deadline_ms == 0 || now_ms >= g_radio_deadline_ms)
          {
            uint32_t extra_ms = 0;
            uint32_t adv_ms = SENSOR_STEADY_ADV_MS;

            if (g_scanner != NULL)
              {
                sensor_observer_halt();
              }

            if (s_reconnect_idle_cycles < 4u)
              {
                s_reconnect_idle_cycles++;
              }

            if (s_reconnect_idle_cycles > 1u)
              {
                extra_ms = 1000u << (s_reconnect_idle_cycles - 2u);
                if (extra_ms > SENSOR_RECONNECT_IDLE_MAX_MS)
                  {
                    extra_ms = SENSOR_RECONNECT_IDLE_MAX_MS;
                  }
              }

            sensor_radio_enter_phase(SENSOR_RADIO_ADV_WINDOW, now_ms);
            g_radio_deadline_ms = now_ms + adv_ms + extra_ms;
            LOGI("[radio] resume steady %s %u ms (+%u idle, empty=%u)",
                 g_phone_connected ? "quiet" : "ADV",
                 (unsigned)adv_ms, (unsigned)extra_ms,
                 (unsigned)s_reconnect_idle_cycles);
          }
        break;

      default:
        sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now_ms);
        g_radio_deadline_ms = 0;
        break;
    }

  return true;
}

/**
 * @brief 缓存 UUID 发现得到的特征句柄。
 */
static void slot_store_handle(struct sensor_slot *slot, uint16_t uuid,
                              uint16_t handle)
{
  if (uuid == UUID_HR_MEAS)
    {
      slot->hr_h = handle;
    }
  else if (uuid == UUID_CSC_MEAS)
    {
      slot->csc_h = handle;
    }
  else if (uuid == UUID_CP_MEAS)
    {
      slot->cps_h = handle;
    }
  else if (uuid == UUID_BAT_LEVEL)
    {
      slot->bat_h = handle;
    }
}

/**
 * @brief 句柄存在则对一个 UUID 做 CCC 订阅。
 * @return 已发出订阅（等回调）则为 true。
 */
static bool slot_subscribe_uuid(struct sensor_slot *slot, uint16_t u)
{
  bt_uuid_t        uuid;
  gatt_attr_desc_t desc;
  bt_status_t      ret;

  uuid = BT_UUID_DECLARE_16(u);
  memset(&desc, 0, sizeof(desc));
  ret = bt_gattc_get_attribute_by_uuid(slot->gatt, 0x0001, 0xffff,
                                       &uuid, &desc);
  if (ret != BT_STATUS_SUCCESS)
    {
      return false;
    }

  slot_store_handle(slot, u, desc.handle);

  ret = bt_gattc_subscribe(slot->gatt, desc.handle, GATT_CCC_NOTIFY);
  if (ret != BT_STATUS_SUCCESS)
    {
      LOGE("%s subscribe 0x%04x failed: %d", kind_name(slot->kind),
           (unsigned)u, (int)ret);
      return false;
    }

  slot->op_sent  = true;
  slot->state_ms = sensor_now_ms();
  return true;
}

/**
 * @brief 若 0x2A19 带 Notify 则订 CCC；否则留下 handle 做周期 Read。
 * @return 已发出订阅则为 true。
 */
static bool slot_try_subscribe_bat(struct sensor_slot *slot)
{
  bt_uuid_t        uuid;
  gatt_attr_desc_t desc;
  bt_status_t      ret;

  uuid = BT_UUID_DECLARE_16(UUID_BAT_LEVEL);
  memset(&desc, 0, sizeof(desc));
  ret = bt_gattc_get_attribute_by_uuid(slot->gatt, 0x0001, 0xffff,
                                       &uuid, &desc);
  if (ret != BT_STATUS_SUCCESS)
    {
      slot->bat_h      = 0;
      slot->bat_notify = false;
      slot->feat      &= (uint16_t)~SENSOR_FEAT_BAT;
      return false;
    }

  slot->bat_h  = desc.handle;
  slot->feat  |= SENSOR_FEAT_BAT;

  if ((desc.properties & GATT_PROP_NOTIFY) == 0)
    {
      slot->bat_notify = false;
      LOGI("%s bat handle 0x%04x (read, no notify)", kind_name(slot->kind),
           (unsigned)slot->bat_h);
      return false;
    }

  ret = bt_gattc_subscribe(slot->gatt, desc.handle, GATT_CCC_NOTIFY);
  if (ret != BT_STATUS_SUCCESS)
    {
      slot->bat_notify = false;
      LOGI("%s bat notify failed, fallback read 0x%04x",
           kind_name(slot->kind), (unsigned)slot->bat_h);
      return false;
    }

  slot->bat_notify = true;
  slot->op_sent    = true;
  slot->state_ms   = sensor_now_ms();
  return true;
}

/**
 * @brief 订阅后可选读取体感位置（没有则 loc=NA）。
 */
static void slot_read_loc(struct sensor_slot *slot)
{
  bt_uuid_t        uuid;
  gatt_attr_desc_t desc;

  uuid = BT_UUID_DECLARE_16(UUID_BODY_SENSOR_LOC);
  memset(&desc, 0, sizeof(desc));
  if (bt_gattc_get_attribute_by_uuid(slot->gatt, 0x0001, 0xffff,
                                     &uuid, &desc) == BT_STATUS_SUCCESS)
    {
      slot->loc_h = desc.handle;
      (void)bt_gattc_read(slot->gatt, slot->loc_h);
    }
}

/**
 * @brief 订阅测量 CCC，再订电量 Notify（没有则周期 Read），然后读位置。
 *
 * 测量 CCC 失败不得跳过，也不得标 READY（否则 UI 假显示已连接）。
 */
static void slot_subscribe_next(struct sensor_slot *slot)
{
  if (slot->sub_step == 0)
    {
      if (slot_subscribe_uuid(slot, kind_meas_uuid(slot->kind)))
        {
          return;
        }

      LOGE("%s meas CCC missing", kind_name(slot->kind));
      slot->drop_link = true;
      return;
    }

  if (slot->sub_step == 1)
    {
      if (slot_try_subscribe_bat(slot))
        {
          return;
        }

      slot->sub_step = 2;
    }

  if (!slot->meas_sub_ok)
    {
      LOGE("%s meas CCC not armed", kind_name(slot->kind));
      slot->drop_link = true;
      return;
    }

  slot_read_loc(slot);
  if (slot->bat_h != 0)
    {
      (void)bt_gattc_read(slot->gatt, slot->bat_h);
    }

  slot_finish_ready(slot);
}

/**
 * @brief 停车探活：ATT 失败或超时则拆僵死 ACL。
 *
 * 可连接 ADV 由扫描回调置 drop_link，健康监控统一拆链。
 */
static void slot_idle_link_check(struct sensor_slot *slot, uint32_t now_ms)
{
  uint16_t h;
  bt_status_t ret;

  if (slot->state != SENSOR_READY || !slot_keep_link(slot))
    {
      return;
    }

  if (!slot->meas_idle)
    {
      return;
    }

  if (slot->probe_sent)
    {
      if (sensor_age_ms(now_ms, slot->probe_ms) > SENSOR_PROBE_TIMEOUT_MS)
        {
          slot->probe_sent = false;
          LOGI("%s idle ATT timeout, reconnect", kind_name(slot->kind));
          slot->drop_link = true;
        }

      return;
    }

  if (slot->bat_ms != 0 &&
      sensor_age_ms(now_ms, slot->bat_ms) < SENSOR_NOTIFY_STALE_MS)
    {
      return;
    }

  if (slot->probe_ms != 0 &&
      sensor_age_ms(now_ms, slot->probe_ms) < SENSOR_PROBE_MS)
    {
      return;
    }

  h = slot->bat_h != 0 ? slot->bat_h :
      (slot->csc_h != 0 ? slot->csc_h :
       (slot->cps_h != 0 ? slot->cps_h : slot->hr_h));
  if (h == 0)
    {
      return;
    }

  ret = bt_gattc_read(slot->gatt, h);
  slot->probe_ms = now_ms;
  if (ret == BT_STATUS_SUCCESS)
    {
      slot->probe_sent = true;
    }
  else
    {
      LOGI("%s idle ATT probe fail %d, reconnect",
           kind_name(slot->kind), (int)ret);
      slot->drop_link = true;
    }
}

/**
 * @brief 驻留 / 重连冷却之后，按已保存地址连接该槽位。
 *
 * Central 角色使用 BT_LE_CONN_PARAM_DEFAULT。同时只飞一条 HCI 建链
 *（满载时再建易 0x3e）。冷却 800 ms；已有其它 READY 链路时 1500 ms。
 */
static void slot_try_connect(struct sensor_slot *slot, uint32_t now_ms)
{
  bt_status_t ret;
  extern bool myvendor_pair_window_active(void);

  if (myvendor_pair_window_active())
    {
      return;                     /* 配对窗：只广播，不建传感器链 */
    }

  if (!g_allow_connect && !slot->want_link)
    {
      return;
    }

  if (g_scan_until_ms != 0)
    {
      return;
    }

  if (addr_empty(&slot->addr) || slot->gatt == NULL)
    {
      return;
    }

  if (slot->want_link && !slot->adv_seen)
    {
      return;
    }

  if (slot->retry_ms != 0 && now_ms < slot->retry_ms)
    {
      return;
    }

  if (sensor_hci_connecting())
    {
      return;
    }

  if (slot->skip_dwell || slot->want_link)
    {
      slot->skip_dwell = false;
    }
  else
    {
      if (sensor_age_ms(now_ms, slot->seen_ms) > SENSOR_DWELL_MS * 4u)
        {
          return;
        }

      if (sensor_age_ms(now_ms, slot->seen_ms) < SENSOR_DWELL_MS &&
          slot->state_ms == 0)
        {
          slot->state_ms = slot->seen_ms;
          return;
        }

      if (slot->state_ms != 0 &&
          sensor_age_ms(now_ms, slot->state_ms) < SENSOR_DWELL_MS)
        {
          return;
        }
    }

  if (g_allow_connect)
    {
      slot->want_link = true;
    }

  ret = bt_gattc_connect(slot->gatt, &slot->addr, slot->addr_type);
  if (ret != BT_STATUS_SUCCESS)
    {
      slot->adv_seen = false;
      slot->retry_ms = now_ms + SENSOR_RECONNECT_MS;
      LOGE("%s connect failed: %d, scan again",
           kind_name(slot->kind), (int)ret);
      return;
    }

  slot->retry_ms = 0;
  slot->state    = SENSOR_CONNECTING;
  slot->state_ms = now_ms;
  {
    char addr[BT_ADDR_STR_LENGTH];

    bt_addr_ba2str(&slot->addr, addr);
    LOGI("connecting %s %s rssi=%d", kind_name(slot->kind), addr,
         (int)slot->rssi);
  }
}

/**
 * @brief 创建 HR/CSC/CPS 槽位（此时还不扫描）。
 * @param ins 蓝牙实例。
 * @return 成功返回 0；@p ins 为 NULL 返回 -1。
 */
int ble_sensor_start(bt_instance_t *ins)
{
  {
    extern void myvendor_bt_pair_autoinit(void);

    myvendor_bt_pair_autoinit();   /* 开机就把配对确认能力挂上 */
  }

  uint8_t i;

  if (g_started)
    {
      return 0;
    }

  if (ins == NULL)
    {
      LOGE("start failed: no bt instance");
      return -1;
    }

  g_sensor_ins = ins;
  memset(g_slots, 0, sizeof(g_slots));
  memset(g_pending_bind, 0, sizeof(g_pending_bind));
  g_phone_connected = false;
  g_phone_adv_held = false;
  g_radio_phase = SENSOR_RADIO_IDLE;
  g_radio_deadline_ms = 0;
  g_radio_phase_ms = 0;
  g_radio_last_scan_ms = 0;
  g_radio_reconnect_since_ms = 0;
  g_radio_heal_ms = 0;
  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      g_slots[i].kind = i;
    }

  g_started = true;
  return 0;
}

/**
 * @brief 停止观察者，断开 GATT 客户端，清空槽位。
 */
void ble_sensor_stop(void)
{
  uint8_t i;
  bool skip_hci;

  if (!g_started)
    {
      return;
    }

  skip_hci = companion_bridge_teardown_skip_hci() ||
             sf32lb52_bt_hci_skip_sync();
  if (!skip_hci)
    {
      if (g_scanner != NULL)
        {
          sensor_observer_halt();
        }

      for (i = 0; i < SENSOR_KIND_COUNT; i++)
        {
          if (g_slots[i].gatt != NULL)
            {
              if (g_slots[i].state >= SENSOR_CONNECTING)
                {
                  bt_gattc_disconnect(g_slots[i].gatt);
                }

              bt_gattc_delete_connect(g_slots[i].gatt);
              g_slots[i].gatt = NULL;
            }
        }
    }
  else
    {
      BLE_LOG("sensor_stop skipped HCI skip_sync=%d teardown=%d",
              sf32lb52_bt_hci_skip_sync() ? 1 : 0,
              companion_bridge_teardown_skip_hci() ? 1 : 0);
      g_scanner = NULL;
      memset(g_slots, 0, sizeof(g_slots));
    }

  g_started       = false;
  g_sensor_ins    = NULL;
  g_allow_connect = false;
  g_want_scan     = false;
  g_scan_until_ms = 0;
  g_adv_debug     = false;
  g_phone_connected = false;
  g_phone_adv_held = false;
  g_radio_phase = SENSOR_RADIO_IDLE;
  g_radio_deadline_ms = 0;
  g_radio_phase_ms = 0;
  g_radio_last_scan_ms = 0;
  g_radio_reconnect_since_ms = 0;
  g_radio_heal_ms = 0;
  s_phone_first_until_ms = 0;
  s_kick_scan_after_phone_first = false;
  s_pair_win_prev = false;
  s_pair_cool_until_ms = 0;
  s_scan_start_ms = 0;
  s_reconnect_idle_cycles = 0;
  g_reconnect_gap_ms = 0;
  g_reconnect_next_ms = 0;
  memset(g_pending_bind, 0, sizeof(g_pending_bind));
  sensor_adv_hold_update();
  sensor_ui_publish();
}

/**
 * @brief 观察者心跳：seen / match 计数，结束时打表。
 */
static void scan_progress(uint32_t left_sec, bool done)
{
  if (done)
    {
      LOGI("[scan] done  seen=%u%s  match=%u",
           (unsigned)g_seen_n,
           g_seen_n >= SENSOR_SEEN_MAX ? "+" : "",
           (unsigned)g_found_n);

      /* 本轮扫完，更新重连观察者的退避：
       *   扫到"要找的"（kept 且 IDLE 的槽位 adv_seen）→ 清退避，回到原节奏；
       *   什么都没扫到 → 退一层（1 s→4 s→15 s→60 s）。CSC/CPS 不在场时正是
       *   后者，而退避期间射频全给广播。 */
      {
        bool wanted_seen = false;
        uint8_t k;

        for (k = 0; k < SENSOR_KIND_COUNT; k++)
          {
            const struct sensor_slot *s = &g_slots[k];

            if (slot_keep_link(s) && s->state == SENSOR_IDLE && s->adv_seen)
              {
                wanted_seen = true;
                break;
              }
          }

        if (wanted_seen)
          {
            g_reconnect_gap_ms  = 0;
            g_reconnect_next_ms = 0;
          }
        else
          {
            uint32_t now = sensor_now_ms();

            if (g_reconnect_gap_ms == 0)
              {
                g_reconnect_gap_ms = SENSOR_STEADY_SCAN_MS *
                                     SENSOR_RECONNECT_BACKOFF_STEP;
              }
            else if (g_reconnect_gap_ms < SENSOR_RECONNECT_BACKOFF_MAX_MS)
              {
                g_reconnect_gap_ms *= SENSOR_RECONNECT_BACKOFF_STEP;

                if (g_reconnect_gap_ms > SENSOR_RECONNECT_BACKOFF_MAX_MS)
                  {
                    g_reconnect_gap_ms = SENSOR_RECONNECT_BACKOFF_MAX_MS;
                  }
              }

            g_reconnect_next_ms = now + g_reconnect_gap_ms;

            LOGI("[scan] nothing wanted in range, reconnect backoff %u ms",
                 (unsigned)g_reconnect_gap_ms);
          }
      }
    }
  else
    {
      LOGI("[scan] %lus left  seen=%u%s  match=%u",
           (unsigned long)left_sec, (unsigned)g_seen_n,
           g_seen_n >= SENSOR_SEEN_MAX ? "+" : "",
           (unsigned)g_found_n);
    }

  if (g_found_n > 0 || done)
    {
      ble_sensor_print_table();
    }
}

/**
 * @brief 把扫描表和槽位状态拷到 bridge，供菜单读取。
 */
static void sensor_ui_publish(void)
{
  /* 统一状态：把各 sensor 链路推进板内那份唯一状态（读侧只拿快照）。 */
  {
    uint8_t k;

    for (k = 0; k < SENSOR_KIND_COUNT; k++)
      {
        companion_ble_post(COMPANION_BLE_EV_SENSOR_LINK, (int)k,
                           slot_linked(&g_slots[k]), false);
      }

    companion_ble_post(COMPANION_BLE_EV_SENSOR_WANT, 0, sensor_want_link(),
                       sensor_wanted_seen());
  }

  struct companion_sensor_ui ui;
  uint8_t i;
  uint8_t k;
  uint8_t n;

  memset(&ui, 0, sizeof(ui));

  /* 锁内只做字段快照，出锁后再发布：ui 是本地副本，所以既保证读到的是
   * 同一时刻的一组槽位状态（回调那边现在也持锁写），又不会让 g_lock 与
   * bridge 的锁形成嵌套。UI/菜单消费的就是这份快照。 */
  pthread_mutex_lock(&g_lock);
  if (g_started)
    {
      ui.scanning = g_want_scan && g_scan_duty == SCAN_DUTY_DISCOVERY;
      n = g_found_n;
      if (n > COMPANION_SENSOR_FOUND_MAX)
        {
          n = COMPANION_SENSOR_FOUND_MAX;
        }

      ui.found_n = n;
      for (i = 0; i < n; i++)
        {
          ui.found[i].table_idx = (uint8_t)(i + 1u);
          ui.found[i].kind_mask = (uint8_t)(g_found[i].svc_mask & 0x07u);
          ui.found[i].rssi = g_found[i].rssi;
          ui.found[i].addr_type = g_found[i].addr_type;
          memcpy(ui.found[i].addr, g_found[i].addr.addr,
                 COMPANION_SENSOR_ADDR_LEN);
          memcpy(ui.found[i].name, g_found[i].name, COMPANION_SENSOR_NAME_MAX);
          ui.found[i].name[COMPANION_SENSOR_NAME_MAX - 1u] = '\0';
          for (k = 0; k < SENSOR_KIND_COUNT; k++)
            {
              if (!addr_empty(&g_slots[k].addr) &&
                  addr_eq(&g_slots[k].addr, &g_found[i].addr) &&
                  g_slots[k].state >= SENSOR_CONNECTING &&
                  g_slots[k].state <= SENSOR_READY)
                {
                  ui.found[i].linked = true;
                  break;
                }
            }
        }

      for (k = 0; k < SENSOR_KIND_COUNT; k++)
        {
          if (slot_linked(&g_slots[k]))
            {
              ui.slot[k].link = COMPANION_SENSOR_LINK_READY;
            }
          else if ((g_slots[k].state >= SENSOR_CONNECTING &&
                    g_slots[k].state <= SENSOR_READY) ||
                   (g_slots[k].state == SENSOR_DISCONNECTING &&
                    g_slots[k].want_link) ||
                   (g_slots[k].want_link && !addr_empty(&g_slots[k].addr) &&
                    g_slots[k].state != SENSOR_READY))
            {
              ui.slot[k].link = COMPANION_SENSOR_LINK_CONNECTING;
            }
          else
            {
              ui.slot[k].link = COMPANION_SENSOR_LINK_IDLE;
            }

          memcpy(ui.slot[k].name, g_slots[k].name, COMPANION_SENSOR_NAME_MAX);
          ui.slot[k].name[COMPANION_SENSOR_NAME_MAX - 1u] = '\0';
          ui.slot[k].bat_pct = -1;
          if (ui.slot[k].link == COMPANION_SENSOR_LINK_READY &&
              g_slots[k].bat_ok)
            {
              uint8_t pct = g_slots[k].last_bat;

              ui.slot[k].bat_pct = (int8_t)((pct > 100u) ? 100 : pct);
            }

          if (!addr_empty(&g_slots[k].addr))
            {
              memcpy(ui.slot[k].addr, g_slots[k].addr.addr,
                     COMPANION_SENSOR_ADDR_LEN);
              ui.slot[k].addr_type = g_slots[k].addr_type;
            }
        }
    }

  pthread_mutex_unlock(&g_lock);

  companion_bridge_sensor_ui_set(&ui);
}

static const char *state_name(enum sensor_state st)
{
  switch (st)
    {
      case SENSOR_CONNECTING:
        return "connecting";
      case SENSOR_DISCOVERING:
        return "discover";
      case SENSOR_SUBSCRIBING:
        return "subscribe";
      case SENSOR_READY:
        return "ready";
      case SENSOR_DISCONNECTING:
        return "disconnecting";
      default:
        return "idle";
    }
}

static void sensor_health_log(uint32_t now_ms)
{
  uint8_t i;
  bool any = false;

  if (!g_adv_debug)
    {
      return;
    }

  if (g_health_log_ms != 0 &&
      sensor_age_ms(now_ms, g_health_log_ms) < SENSOR_HEALTH_LOG_MS)
    {
      return;
    }

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (g_slots[i].want_link || g_slots[i].state != SENSOR_IDLE)
        {
          any = true;
          break;
        }
    }

  if (!any)
    {
      return;
    }

  g_health_log_ms = now_ms;
  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      const struct sensor_slot *s = &g_slots[i];
      uint32_t ntf = 0;

      if (!s->want_link && s->state == SENSOR_IDLE)
        {
          continue;
        }

      if (s->notify_ms != 0)
        {
          ntf = sensor_age_ms(now_ms, s->notify_ms);
        }

      LOGI("health %s %s ntf=%ums bat=%ums idle=%d alive=%d adv=%u want=%d",
           kind_name(i), state_name(s->state), (unsigned)ntf,
           (unsigned)(s->bat_ms != 0 ? sensor_age_ms(now_ms, s->bat_ms) : 0),
           (int)s->meas_idle, (int)slot_link_alive(s, now_ms),
           (unsigned)s->adv_live, (int)s->want_link);
    }

  LOGI("health stats zombie=%u hci=%u disc_to=%u gattc=%u radio_heal=%u obs=%d",
       (unsigned)g_health_zombie_n, (unsigned)g_health_hci_n,
       (unsigned)g_health_disc_to_n, (unsigned)g_health_recreate_n,
       (unsigned)g_health_radio_heal_n, (int)(g_scanner != NULL));
}

/**
 * @brief 健康监控：HCI 备份、僵死 ACL、断链看门狗。
 *
 * 不再对「测量 idle 但电量仍在」的 READY 链路做脉冲扫描：那会让
 * `[scan] observer on` 在心率已连时刷屏，并和建链抢射频（0x3e）。
 */
static void sensor_health_pump(uint32_t now_ms)
{
  /* 瞬时让位**派生**上报：正在扫描或正在建链 → 不要在此时抢广播。
   * 电平语义、每拍都报一次，所以不存在"谁忘了收回"那种闩锁泄漏。 */
  companion_ble_post(COMPANION_BLE_EV_TRANSIENT, 0,
                     sensor_hci_connecting() || g_scanner != NULL, false);
  uint8_t i;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      struct sensor_slot *slot = &g_slots[i];
      uint8_t  reason;
      uint8_t  state;
      bool     act;

      /* 锁内只做"读标志 + 清事件 + 快照判断依据"，出锁后再跑状态机与 HCI。
       *
       * hci_gone 的读-清必须与回调的置位互斥：以前是裸读裸写，回调刚置上的
       * 第二次断连会被这里的清位一并吃掉 —— 槽位就此卡在 READY 而链路已断，
       * 正好是这套 health pump 要防的故障。
       *
       * 三个判断依次做，每个都在锁内重读 state，保持与原来相同的分支顺序
       * （前一步的动作可能把 state 改成 IDLE，进而影响下一步是否该做）。
       */

      /* 1) HCI 断连事件（回调只置标志，真正回收在这里） */
      act = false;
      pthread_mutex_lock(&g_lock);
      if (slot->hci_gone)
        {
          slot->hci_gone = false;
          reason = slot->hci_reason;
          state  = slot->state;
          act    = true;
        }

      pthread_mutex_unlock(&g_lock);

      if (act && state != SENSOR_IDLE)
        {
          LOGI("%s HCI disc reason=0x%02x", kind_name(i), (unsigned)reason);
          g_health_hci_n++;
          slot_handle_disconnected(slot);
        }

      /* 2) 需要主动拆链 */
      pthread_mutex_lock(&g_lock);
      act = slot->drop_link &&
            slot->state > SENSOR_IDLE &&
            slot->state != SENSOR_DISCONNECTING;
      pthread_mutex_unlock(&g_lock);

      if (act)
        {
          slot_begin_disconnect(slot, now_ms);
        }

      /* 3) 拆链超时：重建 GATTC */
      pthread_mutex_lock(&g_lock);
      act = (slot->state == SENSOR_DISCONNECTING &&
             sensor_age_ms(now_ms, slot->state_ms) > SENSOR_HEALTH_DISC_MS);
      pthread_mutex_unlock(&g_lock);

      if (act)
        {
          LOGI("%s disconnect timeout, recreate GATTC", kind_name(i));
          g_health_disc_to_n++;
          (void)slot_gattc_recreate(slot);
          slot_handle_disconnected(slot);
        }
    }

  sensor_health_log(now_ms);
}

/**
 * @brief companion 线程泵：超时、UUID 发现、订阅、notify 过期。
 * @param now_ms CLOCK_MONOTONIC 毫秒。
 *
 * 连接后延迟 500 ms 再发现（LE features / 自动 PHY）。HR 测量 Notify
 * 过期 8 s 视为关机并重连。CSC/CPS 停车常停 Notify，只标 idle，保持 GATT。
 * 重连冷却 800 ms（已有其它链路时 1500 ms）。重连扫描用 BALANCED/LOW_POWER。
 * 健康监控先回收 HCI 丢事件 / 僵死 ACL，再跑观察者与状态机。
 */
void ble_sensor_pump(uint32_t now_ms)
{
  uint8_t i;
  struct sensor_slot *slot;
  bool radio_handled;
  bool pair_win;
  extern bool myvendor_pair_window_active(void);

  if (!g_started)
    {
      return;
    }

  sensor_pair_window_guard(now_ms);
  pair_win = myvendor_pair_window_active();

  if (!pair_win && g_want_scan && g_scan_until_ms != 0 &&
      now_ms >= g_scan_until_ms)
    {
      ble_sensor_scan_stop();
    }
  else if (!pair_win && g_want_scan && g_scan_until_ms != 0 &&
           (g_adv_debug_ms == 0 ||
            sensor_age_ms(now_ms, g_adv_debug_ms) >= ADV_DEBUG_HEARTBEAT_MS))
    {
      uint32_t left = 0;

      g_adv_debug_ms = now_ms;
      left = (now_ms < g_scan_until_ms) ?
             (g_scan_until_ms - now_ms) / 1000u : 0;
      scan_progress(left, false);
    }

  sensor_health_pump(now_ms);

  if (pair_win)
    {
      /* 配对窗：不跑 reconnect/扫；下面 switch 仍处理 DISCONNECTING 收尾。 */
      radio_handled = true;
      sensor_adv_hold_update();
    }
  else
    {
      radio_handled = sensor_reconnect_radio_pump(now_ms);
    }

  if (pair_win)
    {
      /* skip scan arming */
    }
  else if (sensor_hci_connecting())
    {
      if (g_scanner != NULL)
        {
          sensor_observer_halt();
        }
    }
  else if (g_want_scan && g_scan_duty == SCAN_DUTY_DISCOVERY)
    {
      if ((g_phone_connected || g_phone_adv_held) && g_scanner == NULL)
        {
          (void)sensor_start_scan(SCAN_DUTY_DISCOVERY);
        }
    }
  else if (radio_handled)
    {
      /** ADV / reconnect observer 已由 radio-slot 状态机处理。 */
    }

  else if (g_scan_until_ms == 0 && sensor_want_reconnect_observer(now_ms))
    {
      sensor_reconnect_scan();
    }
  else if (g_scan_until_ms == 0 && g_want_scan &&
           g_scan_duty == SCAN_DUTY_RECONNECT &&
           !sensor_want_reconnect_observer(now_ms))
    {
      sensor_observer_stop();
    }
  else if (g_want_scan && g_scanner == NULL)
    {
      (void)sensor_start_scan(g_scan_duty);
    }

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      slot = &g_slots[i];
      if (!slot->gatt_ok)
        {
          continue;
        }

      switch (slot->state)
        {
          case SENSOR_IDLE:
            if (slot_keep_link(slot) && slot->adv_seen &&
                !sensor_hci_connecting() &&
                (g_scanner != NULL || g_want_scan))
              {
                sensor_observer_stop();
                /* 退避窗口里这个分支每个 tick 都成立（retry_ms 的门在
                 * slot_try_connect() 内部），用 LOGI 就是按轮询周期刷屏：
                 * 2026-09-17 实机 0x3e 退避的 3 s 刷了 ~90 行。留在 LOGD
                 * （默认编译掉，CONFIG_MYVENDOR_BLE_COMPANION_DEBUG 打开才
                 * 有）；真正发起连接那一次由 slot_try_connect() 打
                 * "connecting <kind> <addr> rssi="。 */
                LOGD("%s saw advertiser, connecting",
                     kind_name(slot->kind));
              }

            slot_try_connect(slot, now_ms);
            break;

          case SENSOR_CONNECTING:
            if (sensor_age_ms(now_ms, slot->state_ms) > SENSOR_CONNECT_MS)
              {
                if (s_connect_fails < 3u) { s_connect_fails++; }   /* 退避：1→2→4→8s */
                LOGE("%s connect timeout", kind_name(slot->kind));
                slot_begin_disconnect(slot, now_ms);
              }
            break;

          case SENSOR_DISCOVERING:
            if (!slot->op_sent)
              {
                bt_uuid_t uuid;
                uint16_t su;

                /** 等待，避免 ATT 和 LE Read Remote Features 打架。
                 *  先 0x180F 缓存电量，再槽位 UUID（SAL keep_db 追加表）。
                 */

                if (slot->disc_step == 0 &&
                    sensor_age_ms(now_ms, slot->state_ms) <
                    SENSOR_DISCOVER_DELAY_MS)
                  {
                    break;
                  }

                su = (slot->disc_step == 0) ?
                     UUID_BAS : kind_svc_uuid(slot->kind);
                uuid = BT_UUID_DECLARE_16(su);
                if (bt_gattc_discover_service(slot->gatt, &uuid) !=
                    BT_STATUS_SUCCESS)
                  {
                    if (slot->disc_step == 0)
                      {
                        LOGI("%s battery discover start failed",
                             kind_name(slot->kind));
                        SENSOR_STATE_WRITE(slot->disc_step = 1; slot->op_sent = false; slot->state_ms = now_ms);
                        break;
                      }

                    LOGE("%s discover start failed", kind_name(slot->kind));
                    slot_begin_disconnect(slot, now_ms);
                  }
                else
                  {
                    SENSOR_STATE_WRITE(slot->op_sent = true; slot->state_ms = now_ms);
                  }
              }
            else if (sensor_age_ms(now_ms, slot->state_ms) >
                     SENSOR_CONNECT_MS)
              {
                if (slot->disc_step == 0)
                  {
                    LOGI("%s battery discover timeout",
                         kind_name(slot->kind));
                    SENSOR_STATE_WRITE(slot->disc_step = 1; slot->op_sent = false; slot->state_ms = now_ms);
                    break;
                  }

                LOGE("%s discover timeout", kind_name(slot->kind));
                slot_begin_disconnect(slot, now_ms);
              }
            break;

          case SENSOR_SUBSCRIBING:
            if (!slot->op_sent)
              {
                slot_subscribe_next(slot);
              }
            else if (sensor_age_ms(now_ms, slot->state_ms) >
                     SENSOR_CONNECT_MS)
              {
                LOGE("%s subscribe timeout", kind_name(slot->kind));
                slot_begin_disconnect(slot, now_ms);
              }
            break;

          case SENSOR_READY:
            if (slot->poll_ms == 0 ||
                sensor_age_ms(now_ms, slot->poll_ms) >= SENSOR_POLL_MS)
              {
                SENSOR_STATE_WRITE(slot->poll_ms = now_ms);
                if (slot->bat_h != 0 && !slot->bat_notify)
                  {
                    (void)bt_gattc_read(slot->gatt, slot->bat_h);
                  }
              }

            {
              uint32_t ntf_ref = slot->notify_ms != 0 ?
                                 slot->notify_ms : slot->poll_ms;

              if (slot_keep_link(slot) && ntf_ref != 0 &&
                  sensor_age_ms(now_ms, ntf_ref) > SENSOR_NOTIFY_STALE_MS)
                {
                  if (slot->kind == SENSOR_KIND_HR)
                    {
                      uint16_t h;

                      h = slot->bat_h != 0 ? slot->bat_h : slot->hr_h;
                      if (slot->probe_sent)
                        {
                          if (sensor_age_ms(now_ms, slot->probe_ms) >
                              SENSOR_PROBE_TIMEOUT_MS)
                            {
                              LOGI("%s notify stale, reconnect",
                                   kind_name(slot->kind));
                              slot_begin_disconnect(slot, now_ms);
                            }
                        }
                      else if (h != 0 &&
                               bt_gattc_read(slot->gatt, h) ==
                               BT_STATUS_SUCCESS)
                        {
                          SENSOR_STATE_WRITE(slot->probe_sent = true; slot->probe_ms = now_ms);
                          LOGI("%s notify stale, ATT probe",
                               kind_name(slot->kind));
                        }
                      else
                        {
                          LOGI("%s notify stale, reconnect",
                               kind_name(slot->kind));
                          slot_begin_disconnect(slot, now_ms);
                        }
                    }
                  else
                    {
                      SENSOR_STATE_WRITE(slot->meas_idle = true);
                    }
                }
            }

            slot_idle_link_check(slot, now_ms);
            break;

          case SENSOR_DISCONNECTING:
            break;

          default:
            break;
        }

      if (slot->drop_link &&
          slot->state > SENSOR_IDLE &&
          slot->state != SENSOR_DISCONNECTING)
        {
          slot_begin_disconnect(slot, now_ms);
        }
    }

  {
    bool any = false;

    for (i = 0; i < SENSOR_KIND_COUNT; i++)
      {
        if (slot_linked(&g_slots[i]))
          {
            any = true;
            break;
          }
      }

    if (g_adv_debug && any &&
        (g_linked_print_ms == 0 ||
         sensor_age_ms(now_ms, g_linked_print_ms) >= SENSOR_POLL_MS))
      {
        g_linked_print_ms = now_ms;
        print_linked_table();
      }
  }

  sensor_ui_publish();
  sensor_adv_hold_update();
}

/**
 * @brief 允许 GATT 连接（仅扫描阶段为 false）。
 */
void ble_sensor_set_connect(bool enable)
{
  g_allow_connect = enable;
  if (enable)
    {
      sensor_ensure_gattc();
    }

  LOGI("connect %s", enable ? "on" : "off");
}

/**
 * @brief 按 BLE 地址把槽位绑到扫描表第 @p idx 行（绝不按名）。
 * @param idx 表格 1-based 下标。
 * @return 成功返回 0；下标越界返回 -1。
 */
int ble_sensor_connect_index(uint8_t idx)
{
  ble_sensor_scan_burst();   /* 界面手动点连接：尽快短扫 */
  return ble_sensor_connect_index_kind(idx, SENSOR_KIND_COUNT);
}

/**
 * @brief 同上，但指定要绑哪个类型槽位。
 *
 * @details
 * 菜单是**分类型页**的（`s_scan_kind`），列表也按该类型过滤（`kind_mask & bit`）。
 * 以前不看页面、一律「取第一种匹配」，于是 HR+CSC 的组合带子在「踏频器」页被
 * 点中时绑的却是**心率**槽位 —— 点哪页连哪页才对。
 *
 * @param kind 0 HR / 1 CSC / 2 CPS；>= SENSOR_KIND_COUNT（如 0xff）表示
 *             「取 ADV 里第一个匹配的类型」（旧行为，NSH `test sensor`
 *             没有页面上下文，仍走这条）。
 * @return 成功返回 0；下标越界、对端没广播该服务、或该地址已被别的类型占用
 *         返回 -1。
 */
int ble_sensor_connect_index_kind(uint8_t idx, uint8_t kind)
{
  extern bool myvendor_pair_window_active(void);
  struct sensor_found *found;
  struct sensor_slot *slot;
  uint8_t i;
  uint8_t target = SENSOR_KIND_COUNT;
  uint32_t now;
  char addr[BT_ADDR_STR_LENGTH];
  char kinds[16];
  bool bound = false;

  if (myvendor_pair_window_active())
    {
      LOGI("connect skip, pair window (ADV only)");
      return -1;
    }

  ble_sensor_scan_burst();   /* 界面手动点连接：尽快短扫 */

  if (idx == 0 || idx > g_found_n)
    {
      LOGE("connect idx=%u invalid (table has %u, run test sensor scan)",
           (unsigned)idx, (unsigned)g_found_n);
      return -1;
    }

  found = &g_found[idx - 1];
  now = sensor_now_ms();

  /** 先定目标 kind（绑定动作只针对它）。
   *
   *  显式指定就只认它：菜单点选传的是当前页面的类型，对端如果没广播这个服务
   *  就直接报错，不能悄悄换一个类型连。
   *  `kind >= SENSOR_KIND_COUNT` 是「取第一种匹配」的通配值（NSH 用）。 */
  if (kind >= SENSOR_KIND_COUNT)
    {
      for (i = 0; i < SENSOR_KIND_COUNT; i++)
        {
          if ((found->svc_mask & kind_feat_bit(i)) != 0)
            {
              target = i;
              break;
            }
        }
    }
  else if ((found->svc_mask & kind_feat_bit(kind)) != 0)
    {
      target = kind;
    }

  if (target >= SENSOR_KIND_COUNT)
    {
      LOGE("connect idx=%u: no %s service in ADV", (unsigned)idx,
           kind >= SENSOR_KIND_COUNT ? "sensor" : kind_name(kind));
      return -1;
    }

  g_want_scan          = false;
  g_scan_until_ms      = 0;
  if (g_scanner != NULL)
    {
      sensor_observer_halt();
    }

  /** 只清未绑定的扫描候选，避免按 idx 连接时把其它类型最强设备也连上。
   *  已 want_link 的槽位（含 IDLE 等重连）一律保留。
   */

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (g_slots[i].state < SENSOR_CONNECTING &&
          !g_slots[i].want_link)
        {
          g_slots[i].skip_dwell = false;
          g_slots[i].retry_ms   = 0;
          memset(&g_slots[i].addr, 0, sizeof(g_slots[i].addr));
        }
    }

  /** 不置 @c g_allow_connect。
   *
   *  那个标志的语义是「允许连接**新**设备」，dwell 逻辑（slot_try_connect）
   *  据此把每一种 kind 里信号最强的广播者都连掉；autorc 自动回连正是靠它。
   *  手工点选不需要它：本函数给选中槽位设的 want_link 就够了，
   *  slot_keep_link() 也是这个口径（显式写明「不依赖 g_allow_connect」）。
   *
   *  以前在这里置 true，于是它**整个会话**都留着：点完踏频器之后，附近任何
   *  广播 0x1818 的设备都会被当成「功率计」自动连上（菜单显示连接中），
   *  而 UI 侧只有 NSH 的 `test sensor scan` 会把它清掉。
   */
  sensor_ensure_gattc();

  mask_str(found->svc_mask, kinds, sizeof(kinds));
  bt_addr_ba2str(&found->addr, addr);
  LOGI("connect idx=%u %s %s rssi=%d kind=%s",
       (unsigned)idx, kinds, addr, (int)found->rssi, kind_name(target));

  /** 一个地址一个 GATTC：只绑上面定下来的那一个 kind。 */
  for (kind = 0; kind < SENSOR_KIND_COUNT; kind++)
    {
      if (kind != target)
        {
          continue;
        }

      if ((found->svc_mask & (1u << kind)) == 0)
        {
          continue;
        }

      slot = &g_slots[kind];

      /** 一地址一槽位：这台设备已经被别的类型占着就拒绝。
       *
       *  组合带子（同一台既有 HR 又有 CSC）一次只能当一个角色用 —— 两条
       *  GATTC 连同一对端，回调归属、健康监控（谁该拆链）都会互相打架。
       *  这里只拦「已经连着/正在连」的占用；没连上的候选上面已经被清掉了。 */
      for (i = 0; i < SENSOR_KIND_COUNT; i++)
        {
          if (i == kind)
            {
              continue;
            }

          if (!addr_empty(&g_slots[i].addr) &&
              addr_eq(&g_slots[i].addr, &found->addr) &&
              (g_slots[i].want_link ||
               g_slots[i].state >= SENSOR_CONNECTING))
            {
              LOGE("connect idx=%u: %s busy with another %s",
                   (unsigned)idx, addr, kind_name(i));
              return -1;
            }
        }

      if (slot->state >= SENSOR_CONNECTING)
        {
          if (addr_eq(&slot->addr, &found->addr))
            {
              slot->want_link  = true;
              slot->skip_dwell = true;
              slot->retry_ms   = 0;
              bound = true;
              if (slot->state == SENSOR_READY)
                {
                  LOGI("%s already linked", kind_name(kind));
                }
              else
                {
                  LOGI("%s already linking, keep", kind_name(kind));
                }

              break;
            }

          LOGE("%s already linking another device", kind_name(kind));
          continue;
        }

      slot->addr       = found->addr;
      slot->addr_type  = found->addr_type;
      slot->rssi       = found->rssi;
      slot->seen_ms    = now;
      slot->state_ms   = 0;
      slot->retry_ms   = 0;
      slot->skip_dwell = true;
      slot->want_link  = true;
      slot->adv_seen   = true;
      slot->state      = SENSOR_IDLE;
      slot->op_sent    = false;
      memset(slot->name, 0, sizeof(slot->name));
      if (found->name[0] != '\0')
        {
          memcpy(slot->name, found->name, SENSOR_NAME_MAX);
        }

      bound = true;
      break;
    }

  if (!bound)
    {
      LOGE("connect idx=%u no free slot", (unsigned)idx);
      return -1;
    }

  sensor_ui_publish();
  sensor_adv_hold_update();
  return 0;
}

static void slot_drop_want(uint8_t kind, const bt_address_t *addr)
{
  if (kind >= SENSOR_KIND_COUNT || addr == NULL || addr_empty(addr))
    {
      return;
    }

  g_pending_bind[kind].valid = false;

  /* 地址比对与清零要和 slot_bind_addr / slot_handle_disconnected 的写互斥。 */
  pthread_mutex_lock(&g_lock);
  if (addr_eq(&g_slots[kind].addr, addr))
    {
      g_slots[kind].want_link = false;

      /** 删除＝不再回连：**地址也要清掉**。
       *
       *  只清 want_link 挡不住复活：`slot_try_connect()` 里有
       *  `if (g_allow_connect) slot->want_link = true;`，而 sensor 策略开着时
       *  `g_allow_connect` 开机就是真，于是下一次 pump 又把 want_link 拉回来，
       *  等于没删（现场表现就是"删了，重启后它自己又连上了"）。
       *
       *  正在连（CONNECTING..）或已连（READY）的槽位**不动地址**：链路该由
       *  对端断开时走 `slot_handle_disconnected()` 收尾 —— 那边 `!keep`
       *  分支同样会清地址，之后没有记录、没有候选，自然不再重连。
       */
      if (g_slots[kind].state < SENSOR_CONNECTING)
        {
          memset(&g_slots[kind].addr, 0, sizeof(g_slots[kind].addr));
          memset(g_slots[kind].name, 0, sizeof(g_slots[kind].name));
        }
    }

  pthread_mutex_unlock(&g_lock);
}

int ble_sensor_clear_want(const uint8_t *mac, uint8_t kind)
{
  bt_address_t addr;

  if (kind >= SENSOR_KIND_COUNT || mac == NULL)
    {
      return -1;
    }

  /* 用户/`ctl sensor bind` 明确要连这一台 ⇒ 空轮回连退避清零，下一轮立即找它。 */
  s_reconnect_idle_cycles = 0;

  memcpy(addr.addr, mac, BT_ADDR_LENGTH);
  if (addr_empty(&addr))
    {
      return -1;
    }

  if (!g_started)
    {
      return 0;
    }

  slot_drop_want(kind, &addr);
  sensor_ui_publish();
  return 0;
}

/**
 * @brief 按已保存地址绑定槽位并回连。
 */
int ble_sensor_connect_addr(const uint8_t *mac, uint8_t addr_type, uint8_t kind,
                            const char *name, const uint8_t *drop_mac,
                            uint8_t drop_kind)
{
  extern bool myvendor_pair_window_active(void);
  bt_address_t addr;
  bt_address_t drop;
  struct sensor_slot *slot;
  uint8_t i;
  bool seen = false;
  int8_t rssi = 0;
  uint32_t now;

  if (myvendor_pair_window_active())
    {
      LOGI("connect_addr skip, pair window (ADV only)");
      return -1;
    }

  if (kind >= SENSOR_KIND_COUNT || mac == NULL)
    {
      return -1;
    }

  /* 用户/`ctl sensor bind` 明确要连这一台 ⇒ 空轮回连退避清零，下一轮立即找它。 */
  s_reconnect_idle_cycles = 0;

  memcpy(addr.addr, mac, BT_ADDR_LENGTH);
  if (addr_empty(&addr))
    {
      return -1;
    }

  if (!g_started)
    {
      return -1;
    }

  memset(&drop, 0, sizeof(drop));
  if (drop_mac != NULL)
    {
      memcpy(drop.addr, drop_mac, BT_ADDR_LENGTH);
    }

  if (!addr_empty(&drop) && (drop_kind != kind || !addr_eq(&drop, &addr)))
    {
      slot_drop_want(drop_kind, &drop);
    }

  now = sensor_now_ms();
  g_want_scan     = false;
  g_scan_until_ms = 0;
  if (g_scanner != NULL)
    {
      sensor_observer_halt();
    }

  sensor_ensure_gattc();
  slot = &g_slots[kind];

  for (i = 0; i < g_found_n; i++)
    {
      if (addr_eq(&g_found[i].addr, &addr))
        {
          seen = true;
          rssi = g_found[i].rssi;
          addr_type = g_found[i].addr_type;
          if ((name == NULL || name[0] == '\0') && g_found[i].name[0] != '\0')
            {
              name = g_found[i].name;
            }

          break;
        }
    }

  if (slot->state >= SENSOR_CONNECTING)
    {
      if (addr_eq(&slot->addr, &addr))
        {
          slot->want_link  = true;
          slot->skip_dwell = true;
          g_pending_bind[kind].valid = false;
          LOGI("%s keep %s", kind_name(kind),
               slot->state == SENSOR_READY ? "linked" : "linking");
          sensor_ui_publish();
          sensor_adv_hold_update();
          return 0;
        }

      memset(&g_pending_bind[kind], 0, sizeof(g_pending_bind[kind]));
      g_pending_bind[kind].valid = true;
      g_pending_bind[kind].addr = addr;
      g_pending_bind[kind].addr_type = addr_type;
      if (name != NULL && name[0] != '\0')
        {
          size_t n = strlen(name);

          if (n >= SENSOR_NAME_MAX)
            {
              n = SENSOR_NAME_MAX - 1u;
            }

          memcpy(g_pending_bind[kind].name, name, n);
        }

      slot->want_link = false;
      slot_begin_disconnect(slot, now);
      LOGI("%s switch to saved addr after disconnect", kind_name(kind));
      sensor_adv_hold_update();
      return 0;
    }

  g_pending_bind[kind].valid = false;
  slot_bind_addr(slot, &addr, addr_type, rssi, name, seen);
  {
    char a[BT_ADDR_STR_LENGTH];

    bt_addr_ba2str(&addr, a);
    LOGI("connect saved %s %s", kind_name(kind), a);
  }

  sensor_ui_publish();
  sensor_adv_hold_update();
  return 0;
}

/**
 * @brief 扫描时打印原始广播 hex。
 */
void ble_sensor_set_adv_debug(bool enable)
{
  g_adv_debug    = enable;
  g_adv_debug_ms = 0;
  if (enable)
    {
      g_adv_reports = 0;
      LOGI("[scan] debug on");
    }
}

/**
 * @brief 只停观察者；已连上的 GATT 保持。
 */
void ble_sensor_scan_stop(void)
{
  bool was_on = g_want_scan || g_scanner != NULL;

  g_want_scan     = false;
  g_scan_until_ms = 0;
  if (g_scanner != NULL)
    {
      sensor_observer_halt();
    }

  if (was_on)
    {
      scan_progress(0, true);
    }
  else
    {
      /* 这里是"重复停"的静默路径：广播重试时每拍都会走到，所以既不刷
       * 整张 sensors 表也不打 INFO（2026-09-18 现场：广播起不来时每秒
       * 4 行，60 多行把真正的错误淹了）。要看表用 `ble_sensor_dump()`。 */
      LOGD("[scan] already off");
    }

  sensor_adv_hold_update();
  sensor_ui_publish();
}

/** @brief 把"空扫退避"清零：重连立刻、并以完整窗口重试一次。
 *
 * @details 空扫会把退避推到 60 s（1 -> 4 -> 15 -> 60 s，见 g_reconnect_gap_ms），
 *          而退避期间**根本不扫描**。适配器 cycle 之后局面已经全变（控制器刚复位、
 *          链路全丢），旧退避留着就等于"4 秒窗口里没有扫描" —— 而实测规律是
 *          "启用后约 4 s 内没有链路，控制器就进 NOT_READY"，于是成环：
 *            无链路 -> 控制器死 -> cycle -> 退避还在 -> 仍无链路。
 *          在适配器策略重新应用（开机 / cycle 收尾 / devctl 重开）时清零，第一轮
 *          扫描就能立刻开始且跑满窗口，链路才有机会落在那 4 秒里。 */
void ble_sensor_reconnect_backoff_reset(void)
{
  uint8_t k;

  g_reconnect_gap_ms  = 0;
  g_reconnect_next_ms = 0;
  s_reconnect_idle_cycles = 0;

  for (k = 0; k < SENSOR_KIND_COUNT; k++)
    {
      g_slots[k].retry_ms = 0;
    }

  /* 开机 / cycle / 重开外设：先 15 s 给手机。 */
  sensor_radio_arm_phone_first(sensor_now_ms());
}

bool ble_sensor_connect_busy(void)
{
  uint8_t i;

  if (!g_started)
    {
      return false;
    }

  if (sensor_hci_connecting())
    {
      return true;
    }

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (g_slots[i].want_link && !addr_empty(&g_slots[i].addr) &&
          g_slots[i].state != SENSOR_READY)
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief 启动限时观察者；0 表示一直到 scan_stop。
 * @param timeout_ms 时长。
 */
void ble_sensor_scan_arm(uint32_t timeout_ms)
{
  extern bool myvendor_pair_window_active(void);

  if (myvendor_pair_window_active())
    {
      LOGI("[scan] skip, pair window (ADV only)");
      return;
    }

  ble_sensor_scan_burst();   /* 手动/被动触发：尽快短扫 */
  uint32_t now = sensor_now_ms();

  if (ble_sensor_connect_busy())
    {
      LOGI("[scan] skip, connecting");
      return;
    }

  /* 每次起扫都从空表开始（实现见 sensor_found_reset）。 */
  sensor_found_reset();

  g_adv_reports   = 0;
  g_adv_debug_ms  = now;
  g_want_scan     = true;
  g_scan_until_ms = (timeout_ms == 0) ? 0 : now + timeout_ms;
  g_scan_duty     = SCAN_DUTY_DISCOVERY;

  if (timeout_ms == 0)
    {
      LOGI("[scan] observer until off");
    }
  else
    {
      LOGI("[scan] %u s, 2s summary: seen devices / match sensors",
           (unsigned)(timeout_ms / 1000u));
    }

  sensor_ui_publish();
  sensor_adv_hold_update();
}

void ble_sensor_set_phone_state(bool connected, bool adv_held)
{
  bool was = g_phone_connected;

  g_phone_connected = connected;
  g_phone_adv_held = adv_held;

  /* 手机刚掉线：重新 15 s 只广播接 App，再进稳态间隔扫。 */
  if (was && !connected)
    {
      sensor_radio_arm_phone_first(sensor_now_ms());
    }
  else if (!was && connected)
    {
      /* 仅上升沿进间隔扫；主循环每拍都会调本函数，不能每拍清 phase，
       * 否则 quiet↔scan 永远卡在 IDLE（CPS 掉线后扫不起、busy 卡死）。 */
      uint32_t now = sensor_now_ms();

      s_phone_first_until_ms = 0;
      s_kick_scan_after_phone_first = false;
      s_reconnect_idle_cycles = 0;
      sensor_radio_enter_phase(SENSOR_RADIO_IDLE, now);
      g_radio_deadline_ms = 0;
    }
}

/**
 * @brief 打印 idx / kind / rssi / addr / name，供 `test sensor list`。
 */
void ble_sensor_print_table(void)
{
  uint8_t i;
  char addr[BT_ADDR_STR_LENGTH];
  char kinds[16];

  LOGI("---- sensors (%u) ----", (unsigned)g_found_n);
  LOGI("idx  kind      rssi  addr              name");
  if (g_found_n == 0)
    {
      LOGI("  (none)  HR 0x180D / CSC 0x1816 / CPS 0x1818");
      return;
    }

  for (i = 0; i < g_found_n; i++)
    {
      mask_str(g_found[i].svc_mask, kinds, sizeof(kinds));
      bt_addr_ba2str(&g_found[i].addr, addr);
      LOGI("%3u  %-8s  %4d  %s  %s",
           (unsigned)(i + 1), kinds, (int)g_found[i].rssi, addr,
           g_found[i].name[0] != '\0' ? g_found[i].name : "-");
    }
}

/**
 * @brief 断开全部传感器 GATT，并清 want_link（不再自动重连）。
 */
void ble_sensor_disconnect_all(void)
{
  uint8_t i;

  g_allow_connect = false;
  if (!g_started)
    {
      return;
    }

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      g_slots[i].want_link = false;
      g_slots[i].retry_ms  = 0;
      if (g_slots[i].gatt != NULL && g_slots[i].state >= SENSOR_CONNECTING &&
          g_slots[i].state != SENSOR_DISCONNECTING)
        {
          slot_begin_disconnect(&g_slots[i], sensor_now_ms());
        }
      else if (g_slots[i].state == SENSOR_IDLE)
        {
          memset(&g_slots[i].addr, 0, sizeof(g_slots[i].addr));
        }
    }

  LOGI("disconnect all");
  sensor_ui_publish();
}

/**
 * @brief 断开一个类型槽位。
 */
int ble_sensor_disconnect_kind(uint8_t kind)
{
  if (kind >= SENSOR_KIND_COUNT)
    {
      return -1;
    }

  if (!g_started)
    {
      return 0;
    }

  g_slots[kind].want_link = false;
  g_slots[kind].retry_ms  = 0;
  g_pending_bind[kind].valid = false;
  if (g_slots[kind].gatt != NULL && g_slots[kind].state >= SENSOR_CONNECTING &&
      g_slots[kind].state != SENSOR_DISCONNECTING)
    {
      slot_begin_disconnect(&g_slots[kind], sensor_now_ms());
    }
  else
    {
      memset(&g_slots[kind].addr, 0, sizeof(g_slots[kind].addr));
      g_slots[kind].state = SENSOR_IDLE;
      memset(g_slots[kind].name, 0, sizeof(g_slots[kind].name));
    }

  LOGI("disconnect %s", kind_name(kind));
  sensor_ui_publish();
  return 0;
}

/**
 * @brief 打印槽位状态，供 NSH `test sensor` dump/read。
 */
void ble_sensor_dump(void)
{
  uint8_t i;
  char addr[BT_ADDR_STR_LENGTH];
  const char *st;

  ble_sensor_print_table();
  print_linked_table();

  LOGI("started=%d connect=%d obs=%d",
       (int)g_started, (int)g_allow_connect,
       (int)(g_scanner != NULL));

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      st = state_name(g_slots[i].state);

      addr[0] = '\0';
      if (!addr_empty(&g_slots[i].addr))
        {
          bt_addr_ba2str(&g_slots[i].addr, addr);
        }

      LOGI("  %s %s rssi=%d name=%s addr=%s want=%d feat=0x%x ntf=%u adv=%u",
           kind_name(i), st, (int)g_slots[i].rssi,
           g_slots[i].name[0] != '\0' ? g_slots[i].name : "-",
           addr[0] != '\0' ? addr : "-",
           (int)g_slots[i].want_link, (unsigned)g_slots[i].feat,
           (unsigned)g_slots[i].notify_ms, (unsigned)g_slots[i].adv_live);
    }

  LOGI("health zombie=%u hci=%u disc_to=%u gattc=%u radio_heal=%u",
       (unsigned)g_health_zombie_n, (unsigned)g_health_hci_n,
       (unsigned)g_health_disc_to_n, (unsigned)g_health_recreate_n,
       (unsigned)g_health_radio_heal_n);
}

/**
 * @brief 槽位是否已创建。
 * @return start 之后为 true。
 */
bool ble_sensor_is_started(void)
{
  return g_started;
}

/**
 * @brief @p addr 是否是已保存的 HR/CSC/CPS 槽位（不是手机）。
 *
 * GATTS 不得把该对端的 ATT MTU 套到 Companion FS。
 */
bool ble_sensor_owns_addr(const bt_address_t *addr)
{
  uint8_t i;
  bool owned = false;

  if (addr == NULL)
    {
      return false;
    }

  /* 本函数从 ble_companion 的 GATTS 回调（service-loop 线程）经
   * companion_is_phone() 调用，而槽位地址由 companion 线程写。加锁保证
   * memcmp 看到的是完整地址（写侧见 slot_bind_addr / slot_handle_disconnected）。 */
  pthread_mutex_lock(&g_lock);
  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (!addr_empty(&g_slots[i].addr) && addr_eq(&g_slots[i].addr, addr))
        {
          owned = true;
          break;
        }
    }

  pthread_mutex_unlock(&g_lock);

  return owned;
}

/**
 * @brief HCI 断链备份：只置标志，由 pump 回收。
 */
void ble_sensor_on_hci_disconnected(const bt_address_t *addr, uint8_t reason)
{
  struct sensor_slot *slot;

  if (!g_started)
    {
      return;
    }

  slot = slot_by_addr(addr);
  if (slot == NULL || slot->state == SENSOR_IDLE)
    {
      return;
    }

  /* 本回调跑在 BLE service-loop 线程，pump 在 companion 线程读清这两个字段：
   * 置位与读清必须互斥（以前这里裸写，断连事件会被 pump 的清位吃掉）。 */
  SENSOR_STATE_WRITE(slot->hci_gone = true; slot->hci_reason = reason);
}

/**
 * @brief 已 READY 的槽位掩码（bit0 HR / bit1 CSC / bit2 CPS）。
 */
uint8_t ble_sensor_ready_mask(void)
{
  uint8_t i;
  uint8_t mask = 0;

  for (i = 0; i < SENSOR_KIND_COUNT; i++)
    {
      if (slot_linked(&g_slots[i]))
        {
          mask |= (uint8_t)(1u << i);
        }
    }

  return mask;
}

/**
 * @brief 取出挂起的“停 LE 广播”请求（观察者开启时）。
 */
bool ble_sensor_take_hold_request(void)
{
  bool pend = g_hold_request_pend;

  g_hold_request_pend = false;
  return pend;
}

/**
 * @brief 取出挂起的“恢复 LE 广播”请求（观察者关闭时）。
 */
bool ble_sensor_take_hold_release(void)
{
  bool pend = g_hold_release_pend;

  g_hold_release_pend = false;
  return pend;
}
