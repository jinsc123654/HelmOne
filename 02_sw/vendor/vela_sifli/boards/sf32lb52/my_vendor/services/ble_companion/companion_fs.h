/**
 * @file companion_fs.h
 * @brief Companion GATT 文件会话（0xFF15 / 0xFF16）。
 *
 * 文件 I/O 跑在 ble_companion 线程（优先级低于 CONFIG_BT_LONG_WQ_PRIO）。
 * GATT 回调只拷贝请求并 kick 泵。
 *
 * 工厂固件（@ref myvendor_is_factory）拒绝全部 FS 命令，拷文件走 USB MTP。
 * @ref companion_fs_set_mtu 必须带手机地址，且对端须已
 * @ref companion_fs_on_connect。zblue att_mtu_updated 是全局的；漏判
 * 地址时传感器 ATT 23 不得把 FS 砸成载荷 17。见 docs/ble/ble_sensor.md。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_COMPANION_FS_H
#define MYVENDOR_COMPANION_FS_H

#include <stdbool.h>
#include <stdint.h>

#include "bt_gatts.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把 FS 会话绑到 Companion GATTS 句柄。
 * @param handle GATTS 服务句柄。
 * @param data_attr_id 0xFF16（notify/write 数据）的属性 id。
 */
void companion_fs_bind(gatts_handle_t handle, uint16_t data_attr_id);

/**
 * @brief 手机已连接：记下对端并唤醒泵。
 * @param peer 手机 LE 地址；未知则为 NULL。
 */
void companion_fs_on_connect(const bt_address_t *peer);

/**
 * @brief 手机断开：关掉 CCC，保留 *.part 以便续传。
 */
void companion_fs_on_disconnect(void);

/**
 * @brief 只记录本条手机链路的 ATT MTU（载荷为 MTU-3）。
 * @param addr GATTS 回调里的对端；须等于 @ref companion_fs_on_connect 的 peer。
 * @param mtu GATTS 给出的 ATT MTU。
 *
 * 未连接、地址不符或 @p addr 为 NULL 时忽略。传感器 ATT 23 不得砸 FS。
 */
void companion_fs_set_mtu(const bt_address_t *addr, uint32_t mtu);

/**
 * @brief FS Data notify（0xFF16）的 CCC。
 * @param enabled App 已订阅则为 true。
 */
void companion_fs_set_ccc(bool enabled);

/**
 * @brief 拷贝一次 0xFF15 命令写；真正 I/O 在 @ref companion_fs_pump。
 * @param value 命令字节。
 * @param length 字节数。
 * @return 已接受的字节数。
 */
uint16_t companion_fs_on_cmd(const uint8_t *value, uint16_t length);

/**
 * @brief 拷贝一次 0xFF16 数据写；真正 I/O 在 @ref companion_fs_pump。
 * @param value 数据字节。
 * @param length 字节数。
 * @return 已接受的字节数。
 */
uint16_t companion_fs_on_data(const uint8_t *value, uint16_t length);

/**
 * @brief 中止当前 FS 事务（Control 0x10）。
 */
void companion_fs_abort(void);

/**
 * @brief 0xFF16 notify 完成；释放 in-flight 额度。
 * @param attr_id 完成的属性。
 * @param status GATT 状态。
 */
void companion_fs_on_notify_complete(uint16_t attr_id, gatt_status_t status);

/**
 * @brief companion 线程上的 FS 工作函数。
 * @return 还有未完成工作则为 true。
 */
bool companion_fs_pump(void);

/**
 * @brief 请 App 把本地缓存的图标上传到设备的 notif_icons/。
 *
 * 线上只带文件名。设备在空闲且 0xFF16 已订阅时发 FS_NEED notify。
 * @param name 文件名（无路径），例如 com.tencent.mm.png。
 */
void companion_fs_need_file(const char *name);

/**
 * @brief 正式图标是否可显示（PNG 头 + 合理大小）。
 *
 * 半成品是 `*.part`，不走这条路径。若正式名已存在但损坏，会删掉以便再次
 * FS_NEED；MTP 占用 LittleFS 时只判定、不删除。
 * @param abs_path `/mnt/lfs/notif_icons/<file>.png`。
 * @return 可用则为 true。
 */
bool companion_fs_icon_usable(const char *abs_path);

/**
 * @brief 是否有 list/read/write 会话在进行。
 * @return 忙则为 true。
 */
bool companion_fs_busy(void);

/**
 * @brief 唤醒 companion 线程（READ 等待时不要睡满 500 ms）。
 */
void companion_fs_kick(void);

/**
 * @brief 睡到 kick 或超时。
 * @param timeout_ms 最长等待。
 */
void companion_fs_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_COMPANION_FS_H */
