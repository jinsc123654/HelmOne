/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb52_bt_adapter.h
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

#ifndef __VENDOR_SIFLI_CHIPS_SF32LB52_SF32LB52_BT_ADAPTER_H
#define __VENDOR_SIFLI_CHIPS_SF32LB52_SF32LB52_BT_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

typedef int (*sf32lb52_bt_rx_callback_t)(uint8_t *data, uint16_t len);

int sf32lb52_bt_controller_init(void);
int sf32lb52_bt_controller_deinit(void);
int sf32lb52_bt_controller_enable(void);
int sf32lb52_bt_controller_disable(void);
int sf32lb52_hci_register_callback(sf32lb52_bt_rx_callback_t callback);
int sf32lb52_host_send_packet(const uint8_t *data, uint16_t len);

/**
 * @brief uart_bth4 收包失败（常见 -ENOMEM）是否已持续 stall_ms。
 *
 * H4 RX 线程卡住时 /dev/ttyHCI0 不再读，LCPU 仍在灌包。
 */
bool sf32lb52_bt_hci_rx_stalled(uint32_t stall_ms);

/** @brief 清掉当前 HCI RX 失败窗口（adapter cycle 开始时调用）。 */
void sf32lb52_bt_hci_rx_stall_clear(void);

/**
 * @brief 不等待 HPWORK 永久结束：关 LCPU 再开。companion 卡住时由诊断线程调用。
 */
int sf32lb52_bt_controller_force_reset(void);

/**
 * @brief 命令发出去了、stall_ms 内一个事件都没回来。
 *
 * 与 @ref sf32lb52_bt_hci_rx_stalled 互补：那条只看**收包失败**（回调返回
 * <0），控制器彻底静默时一次都不失败，所以看不见。这条看的是"发了没应"——
 * 用**成功发出**的最后一条 HCI 命令（@c g_hci_last_ok_tx_ms）与最后一次
 * 收到事件的时刻比较。
 *
 * 空闲安全：没有命令要发时（没连接、没扫描）永远不成立，不会误报。
 * skip_sync 置位期间（正在恢复 LCPU）也不判。
 */
bool sf32lb52_bt_hci_cmd_stalled(uint32_t stall_ms);

/**
 * @brief 控制器正在硬复位或已判死：SAL / companion 不要再发同步 HCI。
 *
 * zblue 默认对 Command Complete 10 s 超时 assert。复位窗口里再
 * bt_le_adv_stop 会把 libuv worker 干掉，之后扫描/广播都起不来。
 *
 * 已置位时 @ref sf32lb52_bt_controller_force_reset 成功也不会清掉，
 * 避免 companion 仍堵在不可中断的 k_sem_take 时又去发同步 HCI。
 * 由 board_restart_ble_companion 在新线程起来后再清。
 */
void sf32lb52_bt_hci_skip_sync_set(bool on);

/** @brief 当前是否应跳过同步 HCI。 */
bool sf32lb52_bt_hci_skip_sync(void);

/**
 * @brief 心跳停时打一条 HCI 快照（最后命令、RX 年龄、skip）。
 *
 * 不依赖 CONFIG_MYVENDOR_BLE_LOG。diag / companion restart 在杀线程前调用。
 */
void sf32lb52_bt_hci_dump_stall(void);

/**
 * @brief 报"RX 抽取链"的计数器，并在判到断层时自愈（**只由 diag 线程调用**）。
 *
 * @param force false = 只在判到断层时打印（diag 每拍调，保证恢复与开关无关）；
 *              true  = 无条件打一行计数器（diag 的周期报表，见 `ctl cnt`）。
 *
 * 中断侧（`sf32lb52_bt_rx_ind()`）只累加计数，打印与自愈统一在这里 ——
 * 于是"计数器打印"只有 diag 一个出口。详见 .c 里的说明。
 */
void sf32lb52_bt_rx_chain_report(bool force);

#endif
