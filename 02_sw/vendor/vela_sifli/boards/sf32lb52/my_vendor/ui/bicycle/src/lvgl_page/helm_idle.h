/**
 * @file helm_idle.h
 * @brief 加速度静止超时：关背光与 GNSS，BLE 保持。
 *
 * 半反屏无背光仍可见。静止超时会盖上全屏黑白电子钟
 *（上：静止 + 时长，下：当前时刻 + 日期）；按键或拿起关闭并唤醒。
 * 静止满 1 小时后进入关机动画并断电。
 * 静止与关机期间不弹 App 下发的通知横幅。
 * 进静止后 HCPU 由 DVFS ondemand 落到 72/96；唤醒不再钉 240。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HELM_IDLE_H
#define HELM_IDLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 主界面 500 ms 节拍里调用。工厂固件为空操作。
 */
void helm_idle_tick(void);

/**
 * @brief 当前是否已关背光并请求 GNSS 下电。
 */
bool helm_idle_is_sleeping(void);

/**
 * @brief 亮屏并恢复 GNSS。按键唤醒时调用，本次按键应被吞掉。
 */
void helm_idle_wake(void);

/**
 * @brief 立刻进入静止休眠（等同 10 分钟计时耗尽）。按键或拿起均可唤醒。
 */
void helm_idle_force_enter(void);

/**
 * @brief 立刻耗尽静止 1 小时计时并关机（调试）。与真实超时同一条关机动画。
 */
void helm_idle_force_hour(void);

/**
 * @brief 本次开机禁止自动进静止（不写 persist，重启失效）。
 * 已在静止则唤醒。`ctl idle on` / `hour` 仍可手动触发。
 */
void helm_idle_stay(bool stay);

/**
 * @brief 只拿掉静止遮盖，不唤醒 GNSS。关机动画进场时调用。
 */
void helm_idle_dismiss_cover(void);

#ifdef __cplusplus
}
#endif

#endif /* HELM_IDLE_H */
