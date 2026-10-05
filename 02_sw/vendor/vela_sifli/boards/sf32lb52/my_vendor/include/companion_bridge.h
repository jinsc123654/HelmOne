/**
 * @file companion_bridge.h
 * @brief ble_companion（BT 线程）与 NSH / 自行车之间的共享状态。
 *
 * 扁平 NuttX：一份固件，板级全局对各任务可见。NSH `test sensor`
 * 投递命令，companion 线程取出后做扫描/连接/GATT。传感器遥测 8 s
 * 过期（与 notify 过期重连相同）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_COMPANION_BRIDGE_H
#define MYVENDOR_COMPANION_BRIDGE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "companion_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 最新 HR / CSC / CPS 采样，供 NSH `test sensor read`。
 *
 * 各字段仅在最近一次写入未超过 8 s 时有效。
 */
struct companion_sensor_telem
{
  bool     hr_valid;       /**< 心率采样仍新鲜。 */
  bool     cadence_valid;  /**< CSC 踏频采样仍新鲜。 */
  bool     power_valid;    /**< CPS 功率采样仍新鲜。 */
  uint16_t hr_bpm;         /**< 每分钟心跳。 */
  uint16_t cadence_rpm;    /**< CSC 曲柄 RPM。 */
  uint16_t power_w;        /**< CPS 瞬时功率（瓦）。 */
};

/**
 * @brief 保存一次手机 GNSS 定位（0xFF14）。
 * @param fix 已解包的定位。
 * @param now_ms CLOCK_MONOTONIC 毫秒。
 */
void companion_bridge_gnss_set(const struct companion_gnss_fix *fix,
                               uint32_t now_ms);

/**
 * @brief 若未过期则拷贝最近一次手机 GNSS（含搜星帧）。
 * @param out 输出。
 * @return 拷贝到未过期帧则为 true。
 */
bool companion_bridge_gnss_get(struct companion_gnss_fix *out);

/**
 * @brief 同 [companion_bridge_gnss_get]，但新鲜度窗口由调用方指定。
 * @param out 输出手机 GNSS。
 * @param max_age_ms 允许的最大年龄（毫秒）。
 * @return 有足够新的手机 GNSS 时返回 true。
 *
 * 辅助数据（灌给模组的粗位置）比「显示当前定位」宽松得多：一分钟前的手机位置
 * 仍然可用，而显示用的 5 秒窗口对辅助太紧。 */
bool companion_bridge_gnss_get_within(struct companion_gnss_fix *out,
                                      uint32_t max_age_ms);

/**
 * @brief 最近一次手机 GNSS 的接收时刻（CLOCK_MONOTONIC 毫秒）；0 表示无。
 *
 * @return 接收时刻，供 myvendor_sys_gnss_t::stamp_ms 使用。
 */
uint32_t companion_bridge_gnss_stamp_ms(void);

/**
 * @brief 丢掉已存的手机 GNSS（链路断开）。
 */
void companion_bridge_gnss_clear(void);

/**
 * @brief 发布 GATT 客户端的心率采样。
 * @param bpm 每分钟心跳。
 * @param now_ms CLOCK_MONOTONIC 毫秒。
 */
void companion_bridge_sensor_set_hr(uint16_t bpm, uint32_t now_ms);

/**
 * @brief 发布 CSC 踏频采样。
 * @param rpm 曲柄 RPM。
 * @param now_ms CLOCK_MONOTONIC 毫秒。
 */
void companion_bridge_sensor_set_cadence(uint16_t rpm, uint32_t now_ms);

/**
 * @brief 发布骑行功率采样。
 * @param watts 瞬时功率。
 * @param now_ms CLOCK_MONOTONIC 毫秒。
 */
void companion_bridge_sensor_set_power(uint16_t watts, uint32_t now_ms);

/**
 * @brief 快照传感器遥测（过期字段会被清掉）。
 * @param out 输出。
 */
void companion_bridge_sensor_get(struct companion_sensor_telem *out);

/**
 * @brief 同上，但**不阻塞**：锁被 BLE 侧占着就立刻返回 false（@p out 未写）。
 *
 * UI 线程必须用这个版本 —— 否则 companion/FS 持锁时会把 LVGL 一起卡住，
 * 表现就是静止覆盖层心跳停摆、按键与拿起唤醒都不响应。
 * @return 取到快照返回 true；锁忙返回 false。
 */
bool companion_bridge_sensor_get_try(struct companion_sensor_telem *out);

/**
 * @brief 保存手机导航折线（0xFF19）。
 * @param lat_e7 纬度数组（×1e7）。
 * @param lon_e7 经度数组（×1e7）。
 * @param count 点数（上限 COMPANION_NAV_MAX_PTS）。
 */
void companion_bridge_nav_store(const int32_t *lat_e7, const int32_t *lon_e7,
                                uint8_t count);

/**
 * @brief 取出挂起的导航折线（一次性）。
 * @param lat_e7 输出纬度。
 * @param lon_e7 输出经度。
 * @param count 输出点数。
 * @return 拷贝到路线则为 true。
 */
bool companion_bridge_nav_take(int32_t *lat_e7, int32_t *lon_e7,
                               uint8_t *count);

/**
 * @brief 标记 ble_companion 主循环正在跑。
 * @param alive main() 在 tick 循环中则为 true。
 *
 * 置 false 时同时清心跳，避免线程已退出仍被看成活着。
 */
void companion_bridge_alive_set(bool alive);

/**
 * @brief companion 线程还在跑自己的代码（含启动等待）。
 *
 * 与 @ref companion_bridge_alive_get 不同：alive 表示 GATTS 已就绪，
 * 本函数只看心跳是否新鲜。堵在同步 HCI 里超过 stall 则返回 false。
 */
void companion_bridge_heartbeat(void);

/**
 * @brief 心跳仍新鲜则为 true。
 * @param stall_ms 超过该时长无心跳则视为线程死/卡死；0 则用 8 s。
 */
bool companion_bridge_thread_ok(uint32_t stall_ms);

/**
 * @brief 距上次 companion 心跳的毫秒数；从未心跳则为 UINT32_MAX。
 */
uint32_t companion_bridge_heartbeat_age_ms(void);

/**
 * @brief 主循环面包屑：diag 在 SIGTERM 前打印，用来抓心跳停在哪一段。
 */
void companion_bridge_phase_set(const char *phase);

/** @brief 最近一次 @ref companion_bridge_phase_set；从未设置则为 "-"。 */
const char *companion_bridge_phase_get(void);

/**
 * @brief 任务已确认退出后拆除残留 BT 实例。禁止对还在跑的线程调用。
 */
void ble_companion_reap_if_dead(void);

/**
 * @brief 诊断线程请求一次 adapter cycle（companion 线程取出后执行）。
 */
void companion_bridge_diag_cycle_post(void);

/**
 * @brief companion 取出一次诊断 cycle 请求。
 */
bool companion_bridge_diag_cycle_take(void);

/**
 * @brief 是否已有未消费的诊断 cycle 请求。
 */
bool companion_bridge_diag_cycle_pending(void);

/**
 * @brief companion 请求"重建 BLE 任务"（适配器 cycle 与 LCPU 复位都失败之后）。
 *
 * @details companion 自己不能调 `board_restart_ble_companion()` —— 那是给 diag
 *          线程用的函数，自己调等于自杀。所以这里只投递，由 diag 线程取走执行；
 *          companion 侧限频 10 分钟。
 */
void companion_bridge_diag_restart_post(void);

/** @brief diag 线程取走任务重建请求（一次性）。 */
bool companion_bridge_diag_restart_take(void);

/**
 * @brief 进程即将被 SIGTERM / respawn：拆除时不要再发同步 HCI。
 */
void companion_bridge_teardown_skip_hci_set(bool on);

/** @brief companion_stop / ble_sensor_stop 是否应跳过 HCI。 */
bool companion_bridge_teardown_skip_hci(void);

/**
 * @brief ble_companion 是否在主循环中。
 * @return true 表示 NSH 测试可以投递命令。
 */
bool companion_bridge_alive_get(void);

/**
 * @brief 标记手机 Companion GATT 已连接（非传感器对端）。
 */
void companion_bridge_phone_set(bool on);

/**
 * @brief 手机 Companion GATT 是否已连接。
 *
 * 心跳超过 8 s 未刷新时返回 false，避免 companion 已死后 UI 仍显示已连接。
 */
bool companion_bridge_phone_get(void);

/**
 * @brief 自行车 UI 上报骑行会话 / 骑行中（供 0xFF12）。
 * @param session 已开始活动（含暂停）。
 * @param moving  正在骑行（未暂停且速度/踏频过阈值）。
 */
void companion_bridge_ride_set(bool session, bool moving);

/**
 * @brief 读取最近一次骑行会话标志。
 */
void companion_bridge_ride_get(bool *session, bool *moving);

/** 没有挂起的 NSH 传感器命令。 */
#define COMPANION_TEST_SENSOR_NONE        0u
/** 限时观察者扫描，然后打表（不 GATT 连接）。 */
#define COMPANION_TEST_SENSOR_SCAN        1u
/** 按 BLE 地址连接扫描表某一行。 */
#define COMPANION_TEST_SENSOR_CONNECT     2u
/** 断开 GATT；观察者可以继续跑。 */
#define COMPANION_TEST_SENSOR_DISCONNECT  3u
/** 停扫描并断开。 */
#define COMPANION_TEST_SENSOR_STOP        4u
/** 再打表 / 已连接行；不改 sensor_mode。 */
#define COMPANION_TEST_SENSOR_DUMP        5u
/** 只停观察者；GATT 保持。 */
#define COMPANION_TEST_SENSOR_SCAN_STOP   6u
/** 断开某一个 HR/CSC/CPS 槽位，其它槽位保持。 */
#define COMPANION_TEST_SENSOR_DISCONNECT_KIND  7u
/** 按已保存地址绑定槽位并回连（不自动连扫描表里的其它设备）。 */
#define COMPANION_TEST_SENSOR_CONNECT_ADDR     8u
/** 清 want_link，不断开当前 GATT。 */
#define COMPANION_TEST_SENSOR_CLEAR_WANT       9u

/* 配对（2026-09-20）：设备 UI 的「蓝牙设备」页 / `ctl pair …` 用；都在 companion 线程执行。
 * 参考 COMPANION_TEST_SENSOR_* 的写法：`_post()` 投命令，值用 `set_pair_secs()`。 */
#define COMPANION_TEST_PAIR_OPEN          10u  /* 开配对窗口（秒数见 set_pair_secs） */
#define COMPANION_TEST_PAIR_UNBIND        11u  /* 解除绑定（清密钥 + 清手机记录 + 关窗） */
/* `ctl pair list` 的快照请求：**必须**由 companion 线程去遍历连接 —— 任何非协议栈线程
 * 直接 `bt_conn_foreach()` 都会踩到被协议栈改到一半的链表（2026-09-20 现场：
 * `bt_conn_ref` 的 stlex 打在野指针上，MemManage panic；pid=ctl 与 pid=ble_companion
 * 都出现过）。填好的快照走 `companion_bridge_pair_report_set()`。 */
#define COMPANION_TEST_PAIR_LIST          12u
#define COMPANION_TEST_PAIR_CONNECT       13u  /* 设备侧发起配对（挑非传感器那条） */

/** `test sensor scan` 默认观察窗口。 */
#define COMPANION_TEST_SENSOR_SCAN_MS_DEFAULT  30000u
#define COMPANION_TEST_PAIR_OPEN_SECS_DEFAULT  30u

#define COMPANION_SENSOR_KIND_HR   0u
#define COMPANION_SENSOR_KIND_CSC  1u
#define COMPANION_SENSOR_KIND_CPS  2u
#define COMPANION_SENSOR_KIND_N    3u
/** CONNECT 的「不指定类型」：由服务侧取 ADV 里第一个匹配的（NSH / ctl 用）。 */
#define COMPANION_SENSOR_KIND_AUTO 0xffu
#define COMPANION_SENSOR_NAME_MAX  24u
#define COMPANION_SENSOR_FOUND_MAX 16u
#define COMPANION_SENSOR_ADDR_LEN  6u

#define COMPANION_SENSOR_LINK_IDLE       0u
#define COMPANION_SENSOR_LINK_CONNECTING 1u
#define COMPANION_SENSOR_LINK_READY      2u

/**
 * @brief 扫描表一行（给菜单）。
 */
struct companion_sensor_found
{
  uint8_t table_idx; /**< 1-based，CONNECT 用。 */
  uint8_t kind_mask; /**< bit0 HR / bit1 CSC / bit2 CPS。 */
  int8_t  rssi;
  bool    linked;    /**< 已占某个槽位。 */
  uint8_t addr_type;
  uint8_t addr[COMPANION_SENSOR_ADDR_LEN];
  char    name[COMPANION_SENSOR_NAME_MAX];
};

/**
 * @brief 一个类型槽位的连接态。
 */
struct companion_sensor_slot
{
  uint8_t link; /**< COMPANION_SENSOR_LINK_*。 */
  int8_t  bat_pct; /**< 0..100；未知为 -1。 */
  uint8_t addr_type;
  uint8_t addr[COMPANION_SENSOR_ADDR_LEN];
  char    name[COMPANION_SENSOR_NAME_MAX];
};

/**
 * @brief CONNECT_ADDR / CLEAR_WANT 载荷。
 */
struct companion_sensor_bind
{
  uint8_t addr[COMPANION_SENSOR_ADDR_LEN];
  uint8_t addr_type;
  uint8_t kind;
  char    name[COMPANION_SENSOR_NAME_MAX];
  uint8_t drop_addr[COMPANION_SENSOR_ADDR_LEN];
  uint8_t drop_kind;
};

/**
 * @brief 菜单用的扫描/连接快照（companion 线程写入）。
 */
struct companion_sensor_ui
{
  bool    scanning;  /**< 限时搜表观察者仍在跑。 */
  uint8_t found_n;
  struct companion_sensor_found found[COMPANION_SENSOR_FOUND_MAX];
  struct companion_sensor_slot  slot[COMPANION_SENSOR_KIND_N];
};

/**
 * @brief 发布扫描表与槽位状态（ble_sensor 调用）。
 */
void companion_bridge_sensor_ui_set(const struct companion_sensor_ui *ui);

/**
 * @brief 拷贝最新扫描/连接快照。
 */
void companion_bridge_sensor_ui_get(struct companion_sensor_ui *out);

/**
 * @brief 某个传感器槽位是否**已绑定**（有保存下来的对端地址）。
 * @param kind 0 HR / 1 CSC / 2 CPS；越界返回 false。
 * @return true = 有绑定记录。**连不连上都算 true**（停车断链、信号丢失不影响），
 *         只有"重新配网/删除绑定"才会翻回 false。
 * @note 比 @ref companion_bridge_sensor_ui_get 便宜得多：只取一个布尔，不整份
 *       快照（UI 每拍都要问一次，而 ui 快照里有整张扫描表）。
 */
bool companion_bridge_sensor_slot_bound(uint8_t kind);

#define COMPANION_NOTIF_INBOX_MAX      8u
#define COMPANION_NOTIF_QUEUE_MAX      4u
#define COMPANION_NOTIF_ICON_PATH_MAX  128u

/**
 * @brief 一条已接受的手机通知（横幅 / 最近通知）。
 */
struct companion_notif_item
{
  uint8_t type;  /**< COMPANION_NOTIF_TYPE_*。 */
  char    title[COMPANION_NOTIF_TITLE_MAX + 1];
  char    body[COMPANION_NOTIF_BODY_MAX + 1];
  char    icon[COMPANION_NOTIF_ICON_PATH_MAX];
};

/**
 * @brief 产品策略：是否弹出 / 收录手机通知。
 */
void companion_bridge_policy_set_notif(bool on);

/**
 * @brief 当前是否接受手机通知。
 */
bool companion_bridge_policy_notif(void);

/**
 * @brief 通知过滤计数（板级，2026-09-24）：`rx` 收到总数 / `merged` 同键合并
 *        （常驻通知的更新）/ `throttled` 因弹窗限流而未弹 / `evicted` inbox 满丢条目。
 *
 * 见 .c 里 `companion_bridge_notif_post()` 的注释：设备端用 `type + title`
 * 近似"同一条通知"，常驻通知只更新内容、不重弹 modal。
 */
void companion_bridge_notif_stats(uint32_t *rx, uint32_t *merged,
                                  uint32_t *throttled, uint32_t *evicted);

/**
 * @brief 产品策略：仅收录来电。
 */
void companion_bridge_policy_set_calls_only(bool on);

/**
 * @brief 是否仅接受来电。
 */
bool companion_bridge_policy_calls_only(void);

/**
 * @brief 收录一条通知（inbox + 横幅队列）。
 */
void companion_bridge_notif_post(const struct companion_notif_item *item);

/**
 * @brief 取出下一条待弹横幅（一次性）。
 */
bool companion_bridge_notif_take(struct companion_notif_item *out);

/**
 * @brief 拷贝最近通知列表（新到旧）。
 * @param out 可为 NULL（只取条数）。
 * @param n 输出条数。
 * @param maxn out 容量。
 */
void companion_bridge_inbox_get(struct companion_notif_item *out, uint8_t *n,
                                uint8_t maxn);

/**
 * @brief 产品命令：向 companion 线程投递传感器操作（ctl 用）。
 *
 * 与 NSH `test sensor` 共用队列；探针请走 @ref companion_bridge_test_sensor_post。
 * @param cmd COMPANION_TEST_SENSOR_* 之一。
 */
void companion_bridge_sensor_cmd_post(uint8_t cmd);

void companion_bridge_sensor_cmd_set_scan_ms(uint32_t ms);
void companion_bridge_sensor_cmd_set_connect_idx(uint8_t idx);
void companion_bridge_sensor_cmd_set_kind(uint8_t kind);

/**
 * @brief 下一次 CONNECT 要绑哪个类型槽位（菜单按页面 kind 传）。
 *
 * 与 @ref companion_bridge_sensor_cmd_set_kind 分开：后者是 BIND /
 * DISCONNECT_KIND 共用的槽位，语义不同，混用会让 UI 点选粘到别的命令上。
 * 不设或设成 @ref COMPANION_SENSOR_KIND_AUTO 即「取第一种匹配」。
 */
void companion_bridge_sensor_cmd_set_connect_kind(uint8_t kind);

/**
 * @brief 取走 CONNECT 的类型（一次性，取完复位成 AUTO）。
 *
 * 必须是一次性的：否则 UI 点选留下的 kind 会一直粘着，后面 NSH
 * `test sensor connect` 也会跟着按那个类型连。
 * @return 0 HR / 1 CSC / 2 CPS / @ref COMPANION_SENSOR_KIND_AUTO。
 */
uint8_t companion_bridge_sensor_cmd_take_connect_kind(void);

void companion_bridge_sensor_cmd_set_bind(const struct companion_sensor_bind *bind);
void companion_bridge_sensor_cmd_get_bind(struct companion_sensor_bind *out);

/**
 * @brief 向 companion 线程投递一条 NSH 传感器命令（debug 探针）。
 * @param cmd COMPANION_TEST_SENSOR_* 之一。
 */
void companion_bridge_test_sensor_post(uint8_t cmd);

/**
 * @brief 设置下一次 SCAN 命令的观察时长。
 * @param ms 时长；0 表示用默认值。
 */
void companion_bridge_test_sensor_set_scan_ms(uint32_t ms);

/**
 * @brief NSH 最近一次投递的观察时长。
 * @return 毫秒。
 */
uint32_t companion_bridge_test_sensor_scan_ms(void);

/** @brief 配对窗口秒数（`COMPANION_TEST_PAIR_OPEN` 用）。 */
void companion_bridge_sensor_cmd_set_pair_secs(uint32_t secs);

/** @brief 取配对窗口秒数（companion 侧消费命令时读）。 */
uint32_t companion_bridge_test_pair_secs(void);

/** @brief PAIR_UNBIND 槽：0xff=全部，否则 0..2。 */
void companion_bridge_sensor_cmd_set_pair_unbind_slot(uint8_t slot);

/** @brief 取 PAIR_UNBIND 槽。 */
uint8_t companion_bridge_test_pair_unbind_slot(void);

/** @brief 配对/连接快照（companion 线程填，ctl 侧读）。 */
void companion_bridge_pair_report_set(const char *text);

/** @brief 读快照；[gen] 传入上次的代数，返回 1 表示有更新（并把新代数写回）。 */
int companion_bridge_pair_report_get(char *out, size_t len, uint32_t *gen);

/**
 * @brief CONNECT 用的扫描表 1-based 下标。
 * @param idx 最近一次打印的表格行号。
 */
void companion_bridge_test_sensor_set_connect_idx(uint8_t idx);

/**
 * @brief `test sensor connect` 最近投递的下标。
 * @return 1-based 下标。
 */
uint8_t companion_bridge_test_sensor_connect_idx(void);

/**
 * @brief DISCONNECT_KIND 用的槽位（HR=0 / CSC=1 / CPS=2）。
 */
void companion_bridge_test_sensor_set_kind(uint8_t kind);

/**
 * @brief 最近一次 DISCONNECT_KIND 的槽位。
 */
uint8_t companion_bridge_test_sensor_kind(void);

/**
 * @brief 取出一条挂起的 NSH 传感器命令（companion 线程）。
 * @param cmd 输出命令。
 * @return 消费到命令则为 true。
 */
bool companion_bridge_test_sensor_take(uint8_t *cmd);

/**
 * @brief 最近一次传感器模式（DUMP 不改这个值）。
 * @return COMPANION_TEST_SENSOR_* 。
 */
uint8_t companion_bridge_test_sensor_mode(void);

/**
 * @brief 是否在 NSH 打印每条 0xFF17 通知。
 * @param on true 则打印。
 */
void companion_bridge_test_notif_set(bool on);

/**
 * @brief 0xFF17 打印是否开启。
 * @return 开启则为 true。
 */
bool companion_bridge_test_notif_get(void);

/**
 * @brief 是否在 NSH 打印 0xFF13 / 0xFF19（不驱动自行车 UI）。
 * @param on true 则打印。
 */
void companion_bridge_test_ctrl_set(bool on);

/**
 * @brief 控制/导航打印是否开启。
 * @return 开启则为 true。
 */
bool companion_bridge_test_ctrl_get(void);

/**
 * @brief 产品策略：手机广播。@p notify 为 true 时 companion 线程下次泵会套用。
 */
void companion_bridge_policy_set_radio(bool on, bool notify);

/**
 * @brief 当前手机广播策略。
 */
bool companion_bridge_policy_radio(void);

/**
 * @brief companion 线程取出一次待套用的广播策略。
 * @param[out] on 新值。
 * @return 有待处理变更则为 true。
 */
bool companion_bridge_policy_radio_take(bool *on);

/**
 * @brief 产品策略：HR/CSC/CPS 中央设备。
 */
void companion_bridge_policy_set_sensor(bool on, bool notify);

/**
 * @brief 当前传感器 Central 策略。
 */
bool companion_bridge_policy_sensor(void);

/**
 * @brief companion 线程取出一次待套用的传感器策略。
 * @param[out] on 新值。
 * @return 有待处理变更则为 true。
 */
bool companion_bridge_policy_sensor_take(bool *on);

/**
 * @brief 适配器此刻是否"不可用 / 正在被我们自己恢复"。
 *
 * 给 diag 用来挡住**恢复中再恢复**。2026-09-18 n005 现场：adapter cycle 已经跑到
 * `adapter_disable`，diag 又判 `cmdsilent` → 叠加一次 LCPU 强制复位 →
 * `bt_disable_mc()` 撞上还没 init 完的 `hdev->conn_ctx`（NULL）→
 * `bt_conn_ref(&NULL->acl_conns[0])` = DACCVIOL `mmfar=0x124`（`bt_conn_foreach_mc`
 * 只挡了 `hdev == NULL`，没挡 `conn_ctx == NULL`）。
 *
 * 判据三条取或：适配器状态不是 ON/BLE_ON、supervisor 有排队中的 cycle、正在
 * recovering。恢复期间"命令没回应"是必然，不该再叠一次恢复。
 *
 * @return 处于上述任一状态则为 true。
 */
bool companion_ble_recovering(void);

/** 板内 BLE 链路数（HR / CSC / CPS，与 sensor kind 同序）。 */
#define COMPANION_BLE_SENSOR_N  3

/**
 * @brief 板内 BLE 状态快照（**只读拷贝**）。
 *
 * 状态与锁都收在 ble_companion 内部：外部只上报事件、只读这一份快照，
 * 不持有任何 BLE 内部锁。见 ble_companion.c 顶部的说明。
 */
typedef struct companion_ble_state
{
  bool     phone_linked;                          /**< 手机 GATT 链路在否。 */
  bool     sensor_linked[COMPANION_BLE_SENSOR_N]; /**< 各 sensor 链路在否。 */
  bool     adv_active;                            /**< 广播在否。 */
  bool     sensor_want;                           /**< 有 sensor 想连（未连上）。 */
  bool     sensor_seen;                           /**< 其中有人在附近（露过面）。 */
  bool     transient;                             /**< 瞬时让位中（用户扫描 / 建链）。 */
  uint8_t  radio_state;                           /**< 射频裁决（见下表，只读）。 */
  bool     recovering;                            /**< 适配器恢复中。 */
  uint32_t phone_since_ms;                        /**< 手机连上的时刻（0 = 未连）。 */
  uint32_t gen;                                   /**< 每次变更 +1。 */
} companion_ble_state_t;

/**
 * @brief 板内 BLE 事件（**消息入**）。
 *
 * 各子系统线程只**投递**事件，状态与判定由 companion 线程独占：事件都是"电平"
 * 语义（最新的值就是真相，不是边沿），所以用**合流信箱**而不是深队列 ——
 * 同一类型重复投递只保留最后一次，信箱满也不阻塞投递方（只计数）。
 */
typedef enum
  {
    COMPANION_BLE_EV_SENSOR_LINK = 0, /**< a = kind，b = linked。 */
    COMPANION_BLE_EV_SENSOR_WANT,     /**< b = want，c = seen。 */
    COMPANION_BLE_EV_TRANSIENT,       /**< b = 正在扫描 / 正在建链。 */
    COMPANION_BLE_EV_COUNT,
  } companion_ble_ev_type_t;

/**
 * @brief 投递一个 BLE 事件（短临界区、不阻塞、同类型覆盖）。
 *
 * @param type 事件类型。
 * @param a    类型相关（见枚举注释）。
 * @param b    类型相关。
 * @param c    类型相关。
 */
void companion_ble_post(companion_ble_ev_type_t type, int a, bool b, bool c);

/**
 * @brief 手机链路状态（companion 自身回调写入；别的子系统只读快照）。
 */
void companion_ble_state_set_phone(bool linked);

/**
 * @brief 读当前射频用途裁决（纯查询；节奏与动作由执行侧按这个裁决自己安排）。
 */

/**
 * @brief 读出当前 BLE 状态快照（内部一把锁，拷贝出去）。
 */
void companion_ble_state_snapshot(companion_ble_state_t *out);

/**
 * @brief 射频用途（唯一裁决点在 ble_companion 内部，`radio_state` 字段即此枚举）。
 *
 * 外部**只读快照**里的这个字段，不要跨线程调用判定：判定在 owner 线程内跑，
 * 动作由各子系统自己的线程执行（sensor 起停扫描、companion 起停广播）。
 */
typedef enum
  {
    COMPANION_RADIO_RECOVERING = 0, /**< 适配器在恢复：广播与扫描都不碰。 */
    COMPANION_RADIO_SCAN_ONLY,      /**< 手机已连：不需要广播。 */
    COMPANION_RADIO_ADV_ONLY,       /**< 手机没连、没有 sensor 想连：只广播。 */
    COMPANION_RADIO_ALTERNATE,      /**< 手机没连、有 sensor 想连：交替。 */
  } companion_ble_radio_state_t;

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_COMPANION_BRIDGE_H */
