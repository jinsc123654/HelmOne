/**
 * @file myvendor_usbdev.h
 * @brief USB 控制器开关（PHY + USBC 时钟 + MTP gadget）。
 *
 * `ctl mtp` 的执行器：off 卸 gadget 并 `arm_usbuninitialize()`（关 PHY/时钟）；
 * on 再 `arm_usbinitialize()` 并绑定 MTP 类。不启停 BLE FS，也不拆
 * `mtp_simple` worker。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_USBDEV_H
#define MYVENDOR_USBDEV_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 打开或关闭 USB 设备控制器。
 * @param on true 上电并绑定 MTP gadget；false 解绑并下电。
 * @return 0 成功，-ENODEV gadget 绑定失败，-ENOTSUP 未编 USB 设备控制器。
 */
int myvendor_usbdev_set(bool on);

/**
 * @brief USB 控制器是否处于上电/可枚举状态。
 */
bool myvendor_usbdev_get(void);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_USBDEV_H */
