/**
 * @file myvendor_ble_log.h
 * @brief BLE 诊断日志。CONFIG_MYVENDOR_BLE_LOG 打开后走 syslog INFO。
 *
 * 与 CONFIG_MYVENDOR_BLE_COMPANION_DEBUG 分开：后者是 1–5 Hz 的
 * Control / GNSS Fix 写路径，本宏只覆盖 skip_sync、HCI RX stall、
 * companion 心跳、adapter cycle、diag 不健康原因。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_BLE_LOG_H
#define MYVENDOR_BLE_LOG_H

#include <syslog.h>

#ifdef CONFIG_MYVENDOR_BLE_LOG
#  define BLE_LOG(fmt, ...) \
     syslog(LOG_INFO, "ble: " fmt "\n", ##__VA_ARGS__)
#else
#  define BLE_LOG(...) ((void)0)
#endif

#endif
