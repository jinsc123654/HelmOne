/**
 * @file companion_downlink.h
 * @brief Notification 0xFF17 与 Nav Route 0xFF19 的 GATT 写处理。
 *
 * 分片在 companion 路径上重组。不驱动自行车 UI；NSH `test notif` /
 * `test ctrl` 负责打印到达的数据。缺图标时设备发 FS_NEED，由 App 上传。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_COMPANION_DOWNLINK_H
#define MYVENDOR_COMPANION_DOWNLINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 处理一次 0xFF17 写（可能只是一个分片）。
 * @param value ATT 值。
 * @param length 字节数。
 * @return 已接受的字节数。
 */
uint16_t companion_notif_on_write(const uint8_t *value, uint16_t length);

/**
 * @brief 处理一次 0xFF19 写（可能只是一个分片）。
 * @param value ATT 值。
 * @param length 字节数。
 * @return 已接受的字节数。
 */
uint16_t companion_nav_on_write(const uint8_t *value, uint16_t length);

/**
 * @brief 丢弃进行中的通知/导航分片（手机断开）。
 */
void companion_downlink_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_COMPANION_DOWNLINK_H */
