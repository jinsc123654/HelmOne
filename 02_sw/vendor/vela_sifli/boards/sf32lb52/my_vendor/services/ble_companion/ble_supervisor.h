/**
 * @file ble_supervisor.h
 * @brief BLE 明确故障计数与 adapter cycle 升级策略。
 *
 * 只在 ble_companion 主线程调用；本模块不直接调用 Bluetooth API。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_BLE_SUPERVISOR_H
#define MYVENDOR_BLE_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ble_supervisor_fault
{
  BLE_SUPERVISOR_FAULT_ADV = 0,
  BLE_SUPERVISOR_FAULT_SCAN,
  BLE_SUPERVISOR_FAULT_GATTC,
  BLE_SUPERVISOR_FAULT_ADAPTER,
  BLE_SUPERVISOR_FAULT_COUNT,
};

struct ble_supervisor_stats
{
  uint16_t adv_failures;
  uint16_t scan_failures;
  uint16_t gattc_recreates;
  uint16_t adapter_failures;
  uint16_t adapter_cycles;
  uint16_t adapter_cycle_failures;
  bool     recovering;
  bool     cycle_pending;
};

void ble_supervisor_init(uint32_t now_ms);
void ble_supervisor_report(enum ble_supervisor_fault fault, uint32_t now_ms);
void ble_supervisor_watch_advertising(bool expected, bool active,
                                      uint32_t now_ms);
void ble_supervisor_tick(uint32_t now_ms);
/**
 * @brief 立刻请求 adapter cycle（HCI RX 卡住等）。仍尊重 recovering / cooldown。
 */
void ble_supervisor_request_cycle(enum ble_supervisor_fault fault,
                                  uint32_t now_ms);
bool ble_supervisor_cycle_requested(void);
enum ble_supervisor_fault ble_supervisor_pending_reason(void);
bool ble_supervisor_take_adapter_cycle(enum ble_supervisor_fault *reason);
/**
 * @brief 取一次"该换更狠的一级"的请求（适配器 cycle 连续失败时置位）。
 *
 * cycle 只能 disable/enable 适配器；控制器本身卡死时那一级永远救不回来
 * （2026-09-18 现场：60 s 一轮重试了 23 次，全是 `adapter not ready, state=0`，
 * 手机和心率计一直断着）。置位后由 companion 线程执行 LCPU 复位，见
 * companion_controller_recover()。
 */
bool ble_supervisor_take_escalation(void);

/** @brief 升级动作做完了：阶梯归零，下一轮 cycle 重新计数。 */
void ble_supervisor_escalation_done(uint32_t now_ms);

void ble_supervisor_cycle_begin(uint32_t now_ms);
void ble_supervisor_cycle_complete(bool success, uint32_t now_ms);
void ble_supervisor_get_stats(struct ble_supervisor_stats *stats);
const char *ble_supervisor_fault_name(enum ble_supervisor_fault fault);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_BLE_SUPERVISOR_H */
