/**
 * @file ble_sensor.h
 * @brief 独立 HR / CSC / CPS 外设的 BLE Central。
 *
 * 扫描、连接和 GATT 客户端跑在 ble_companion 线程上（不要在 zblue
 * 回调里做）。连接按广播地址，绝不按设备名。
 *
 * 双角色：手机走另一条连接上的 Companion GATT Server 0xFF10。
 * @ref ble_sensor_owns_addr 让 GATTS 忽略传感器对端，避免 Companion
 * FS/MTU 被覆盖（默认 ATT 23 曾被当成 MTU 20，文件传输会坏）。
 *
 * 禁止 discover-all：Android 模拟器会超过 GATT_CLIENT_ELEMENT_MAX，
 * SAL 过去会直接 HCI 断开。先发现 BAS 再发现槽位 UUID；板卡 SAL keep_db
 * 追加属性表。电量有 Notify 则订 CCC，否则周期 Read。
 * Central 不得强推 7.5 ms / 1 s / PHY / DLE。
 *
 * @see docs/ble/ble_sensor.md
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_BLE_SENSOR_H
#define MYVENDOR_BLE_SENSOR_H

#include "bluetooth.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 HR/CSC/CPS GATT 客户端槽位（此时还不扫描）。
 * @param ins companion_start() 得到的蓝牙实例。
 * @return 成功返回 0；@p ins 为 NULL 时返回负 errno。
 */
int  ble_sensor_start(bt_instance_t *ins);

/**
 * @brief 停止观察者扫描，断开 GATT 客户端，清空槽位。
 */
void ble_sensor_stop(void);

/**
 * @brief companion 线程泵：扫描超时、发现、订阅、通知过期。
 * @param now_ms CLOCK_MONOTONIC 毫秒。
 *
 * 禁止从 GATT/HCI 回调里调用。先发现 BAS 缓存电量 handle，再槽位 UUID，
 * 再只写测量 CCC；电量周期 Read。
 */
void ble_sensor_pump(uint32_t now_ms);

/**
 * @brief 反馈手机链路与手机广播 hold 状态，驱动 ADV / observer 时间片。
 *
 * 只从 companion 线程调用。手机已连接：无手机 ADV，sensor 侧仍按
 * 4 s 静默 + 0.8 s 短扫间隔找表。手机未连接且有待重连 sensor：与可发现
 * 广播互斥轮换（先 15 s 只广播）。
 * @param connected 手机 Companion GATT 链路是否已建立。
 * @param adv_held companion 是否已消费“停手机广播”请求。
 */
void ble_sensor_set_phone_state(bool connected, bool adv_held);

/**
 * @brief 允许或禁止 GATT 连接（NSH `test sensor scan` 只扫描不连）。
 * @param enable `connect <idx>` / connect-on 后为 true；开始扫描时为 false。
 */
void ble_sensor_set_connect(bool enable);

/**
 * @brief 扫描时打印原始广播 hex（调试）。
 * @param enable true 则打印每条 ADV。
 */
void ble_sensor_set_adv_debug(bool enable);

/**
 * @brief 启动限时观察者扫描，填满 NSH 表格。
 * @param timeout_ms 0 表示一直扫到 @ref ble_sensor_scan_stop。
 */
void ble_sensor_scan_arm(uint32_t timeout_ms);

/** 尽快开一轮短扫（掉线 / 界面点连接；手机优先 15 s 内则排队到其后）。 */
void ble_sensor_scan_burst(void);

/**
 * @brief 建链或已绑定待回连时为 true（此时不得再开发现扫描）。
 */
bool ble_sensor_connect_busy(void);
void ble_sensor_reconnect_backoff_reset(void);

/**
 * @brief 只停观察者；已连上的 GATT 保持。
 */
void ble_sensor_scan_stop(void);

/**
 * @brief 按 BLE 地址连接扫描表第 @p idx_1based 行。
 * @param idx_1based 最近一次扫描表的 1-based 下标。
 * @return 成功返回 0；下标越界返回 -1。
 */
int  ble_sensor_connect_index(uint8_t idx_1based);

/**
 * @brief 同上，但指定绑哪个类型槽位。
 *
 * @details
 * 菜单是分类型页的，列表也按该类型过滤 —— 点哪页就该连哪页。不指定的话
 * （@p kind >= SENSOR_KIND_COUNT）会取 ADV 里第一个匹配的类型，一台同时
 * 广播 HR/CSC/CPS 的设备就会被绑到「靠前」的那个槽位上去。
 *
 * @param idx_1based 最近一次扫描表的 1-based 下标。
 * @param kind 0 HR / 1 CSC / 2 CPS；>= SENSOR_KIND_COUNT 为「不指定」。
 * @return 成功返回 0；下标越界、对端 ADV 里没有该服务、或该地址已被别的
 *         类型占用（一个地址只占一个槽位）返回 -1。
 */
int  ble_sensor_connect_index_kind(uint8_t idx_1based, uint8_t kind);

/**
 * @brief 按已保存地址绑定一个类型槽位并回连（不打开 g_allow_connect）。
 * @param addr 6 字节 LE 地址。
 * @param addr_type LE 地址类型。
 * @param kind 0 HR / 1 CSC / 2 CPS。
 * @param name 可为空。
 * @param drop_addr 取消另一台设备的 want_link；全 0 则忽略。
 * @param drop_kind drop_addr 对应槽位。
 */
int  ble_sensor_connect_addr(const uint8_t *addr, uint8_t addr_type,
                             uint8_t kind, const char *name,
                             const uint8_t *drop_addr, uint8_t drop_kind);

/**
 * @brief 清指定地址槽位的 want_link，不断开当前 GATT。
 */
int  ble_sensor_clear_want(const uint8_t *addr, uint8_t kind);

/**
 * @brief 再打一遍扫描表（idx / kind / rssi / addr / name）。
 */
void ble_sensor_print_table(void);

/**
 * @brief 断开全部传感器 GATT，并清 want_link（不再自动重连）。
 */
void ble_sensor_disconnect_all(void);

/**
 * @brief 断开一个类型槽位并清 want_link（不再自动重连该槽）。
 * @param kind 0 HR / 1 CSC / 2 CPS。
 * @return 成功返回 0；kind 越界返回 -1。
 */
int ble_sensor_disconnect_kind(uint8_t kind);

/**
 * @brief 打印槽位状态，供 NSH `test sensor` dump。
 */
void ble_sensor_dump(void);

/**
 * @brief @ref ble_sensor_start 是否已执行。
 * @return 槽位已创建则为 true。
 */
bool ble_sensor_is_started(void);

/**
 * @brief @p addr 是否是 HR/CSC/CPS 槽位（为重连保存的地址）。
 * @param addr GATTS/HCI 里的 LE 地址。
 * @return true 表示 Companion 不得把该对端当手机。
 *
 * zblue `att_mtu_updated` 是全局的。HR 连接会把 ATT 23 报到 GATT Server
 * 回调；若把该 MTU 套到 Companion FS，文件传输会坏。
 */
bool ble_sensor_owns_addr(const bt_address_t *addr);

/**
 * @brief HCI 断链备份（companion 的 zblue `disconnected` 回调）。
 *
 * GATTC `on_disconnected` 在双角色抢射频时可能丢。只置标志，由
 * @ref ble_sensor_pump 在 companion 线程回收槽位。
 * @param addr 对端 LE 地址（identity / 连接时地址）。
 * @param reason HCI 原因码（0x13 对端 / 0x08 监督超时 / 0x16 本机）。
 */
void ble_sensor_on_hci_disconnected(const bt_address_t *addr, uint8_t reason);

/**
 * @brief 已 @c SENSOR_READY 的槽位掩码：bit0 HR，bit1 CSC，bit2 CPS。
 *
 * CSC/CPS 停车无测量 Notify 仍保持 GATT，此掩码仍置位（Device Status
 * 「已连接」不等于「采样仍新鲜」）。
 */
uint8_t ble_sensor_ready_mask(void);

/**
 * @brief 取出一次“停 LE 广播”的挂起请求（观察者开启时）。
 * @return 每次 arm 为 true 一次，直到 companion 调用 @ref companion_hold_le_adv。
 */
bool ble_sensor_take_hold_request(void);

/**
 * @brief 取出一次“恢复 LE 广播”的挂起请求（观察者关闭时）。
 * @return 每次 scan stop 为 true 一次。
 */
bool ble_sensor_take_hold_release(void);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_BLE_SENSOR_H */
