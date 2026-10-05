/**
 * @file sf32lb52_buttons.h
 * @brief 板级按键：注册 `/dev/buttons`，并提供 NSH 原始电平探测。
 *
 * bit0 KEY2 PA33（上拉按下接地）、bit1 KEY1 PA30、bit2 PWR PA34（下拉按下为高）。
 * PA34 硬件长按约 10s 复位，软件只处理单击调光 / 长按 2s 关机确认。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __MY_VENDOR_SF32LB52_BUTTONS_H
#define __MY_VENDOR_SF32LB52_BUTTONS_H

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#ifdef CONFIG_INPUT_BUTTONS

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 初始化按键 GPIO 并注册按钮字符设备。
 * @param devname 设备名，如 "/dev/buttons"。
 * @return 0 成功，负值为错误码。
 */
int sf32lb52_button_initialize(FAR const char *devname);

/**
 * @brief 读逻辑按下（1 = 按下），供 NSH `btnpeek`。
 * @param[out] click  KEY2（PA33），可为 NULL。
 * @param[out] scroll KEY1（PA30），可为 NULL。
 * @param[out] pwr    PWR（PA34），可为 NULL。
 */
void sf32lb52_button_debug_read(bool *click, bool *scroll, bool *pwr);

#endif /* CONFIG_INPUT_BUTTONS */

#endif /* __MY_VENDOR_SF32LB52_BUTTONS_H */
