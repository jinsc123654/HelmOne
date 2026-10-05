/**
 * @file helm_pwr.h
 * @brief 开机键 PA34：任意界面单击调亮度，长按 2s 关机确认。低电提示/关机。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HELM_PWR_H
#define HELM_PWR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 菜单/按键共用的 UI 亮度步进（0 / 20 / … / 100）。 */
#define HELM_PWR_BL_UI_STEP 20u

/** @brief 当前策略背光换成 UI 百分比（0 / 20 / … / 100）。 */
uint8_t helm_pwr_bl_ui(void);

/** @brief UI 百分比换成要下发的 PWM。 */
uint8_t helm_pwr_ui_to_pwm(uint8_t ui);

/**
 * @brief 亮度 0→20→…→100→0，PWM 线性过渡；5 秒内不再点才写 persist。
 * @return 新的 UI 百分比。
 */
uint8_t helm_pwr_bl_cycle(void);

/** @brief 当前 PWM 线性过渡到 0，不改 persist 策略。 */
void helm_pwr_bl_fade_off(void);

/** @brief 当前 PWM 线性过渡到策略亮度。 */
void helm_pwr_bl_fade_on(void);

/** @brief PWR 单击：循环亮度并弹出百分比。确认框打开时当取消。 */
void helm_pwr_on_click(void);

/** @brief PWR 长按 2s：弹出关机确认（任意界面）。 */
void helm_pwr_on_long(void);

/** @brief 关机确认框是否在。KEY1/KEY2 应先走这里。 */
bool helm_pwr_dialog_open(void);

/** @brief KEY1：关掉确认框。 */
void helm_pwr_dialog_cancel(void);

/** @brief KEY2：确认断电。 */
void helm_pwr_dialog_confirm(void);

/** @brief 菜单「关机」：直接进入关机动画后断电（已是一次确认）。 */
void helm_pwr_exec(void);

/**
 * @brief 关机动画（标题可换：关机 / 电量耗尽 / 静止超时）。
 * 进入后不再响应按键。与开机同一套白底 logo splash，事项滚入再滚出，
 * 进度条走完后整屏慢慢刷黑再断电（黑屏不再画 logo / 文字）。
 * 时序与文案见 `doc/splash.md`。
 */
void helm_pwr_exec_as(const char *title);

/** @brief 关机动画进行中。按键应全部吞掉。 */
bool helm_pwr_is_offing(void);

/**
 * @brief 低电策略：约 10% 提示一次；约 3.05 V / ≤2% 存 GPX 后关机。
 * 充电与工厂固件不触发。UI 定时器里调用。
 */
void helm_pwr_bat_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* HELM_PWR_H */
