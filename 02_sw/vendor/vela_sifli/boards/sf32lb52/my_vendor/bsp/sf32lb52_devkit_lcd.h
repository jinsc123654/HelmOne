/**
 * @file sf32lb52_devkit_lcd.h
 * @brief SF32LB52 DevKit LCD 板 GPIO 数量与 bringup 对外 API。
 *
 * 定义 /dev/gpio 示例引脚及晚初始化、按键、BLE 异步启动声明。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __VENDOR_SIFLI_BOARDS_SF32LB52_SF32LB52_DEVKIT_LCD_SF32LB52_DEVKIT_LCD_H
#define __VENDOR_SIFLI_BOARDS_SF32LB52_SF32LB52_DEVKIT_LCD_SF32LB52_DEVKIT_LCD_H

#include <stdbool.h>

/** @name NuttX GPIO 示例引脚数量 */
/** @{ */
#define BOARD_NGPIOIN     1  /**< 输入引脚数。 */
#define BOARD_NGPIOOUT    1  /**< 输出引脚数。 */
#define BOARD_NGPIOINT    1  /**< 中断输入引脚数。 */
/** @} */

#define GPIO_IN1          (GET_PIN_2(hwp_gpio1, 34))  /**< 示例输入 PA34。 */
#define GPIO_OUT1         (GET_PIN_2(hwp_gpio1, 21))  /**< 示例输出 PA21。 */
#define GPIO_INT1         (GET_PIN_2(hwp_gpio1, 34))  /**< 示例中断 PA34。 */

/**
 * @brief 执行 g_bringup[] 板级晚初始化步骤表。
 * @return 0 成功；负值为 abort_on_fail 步骤失败码。
 */
int sf32lb52_devkit_lcd_bringup(void);

#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
/** @brief 异步启动 ble_companion 任务。 */
void board_start_ble_companion_async(void);

/** @brief 开机 splash / 工厂页起来后再开 BLE（约 1.5 s）。 */
void board_ble_companion_start_after_ui(void);

/**
 * @brief 停掉当前 ble_companion 再 task_spawn（同一块静态栈）。
 * @return 0 成功；-EBUSY 正在 spawn；负值为其它错误。
 */
int board_restart_ble_companion(void);

/** @brief ble_companion 任务是否还在。 */
bool board_ble_companion_pid_alive(void);

/** @brief 正在 spawn / restart，diag 不要重入。 */
bool board_ble_companion_busy(void);
#endif

#ifdef CONFIG_INPUT_BUTTONS
/**
 * @brief 注册按键 lower-half 到 devname。
 * @param devname 字符设备路径（如 "/dev/buttons"）。
 * @return 0 成功，负值为错误码。
 */
int sf32lb52_button_initialize(const char *devname);
#endif

#ifdef CONFIG_DEV_GPIO
/**
 * @brief 初始化并注册板载 GPIO 到 NuttX。
 * @return 0 成功。
 */
int sifli_gpio_initialize(void);
#endif

#endif /* __VENDOR_SIFLI_BOARDS_SF32LB52_SF32LB52_DEVKIT_LCD_SF32LB52_DEVKIT_LCD_H */
