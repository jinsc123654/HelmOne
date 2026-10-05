/**
 * @file bsp_power.c
 * @brief 板级 GPIO 输出与 MPI2/SDIO 电源控制。
 *
 * BSP_GPIO_Set 供传感器、LCD、ETA9184 等直接控脚；
 * BSP_Power_Up/Down 管理深睡前后板级供电。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_board.h"

/**
 * @brief 将指定引脚配置为输出并写电平。
 * @param pin 引脚号（GPIO 组内索引）。
 * @param val 0 低，非 0 高。
 * @param is_porta 非 0 用 GPIO1（Port A），否则 GPIO2。
 */
void BSP_GPIO_Set(int pin, int val, int is_porta)
{
    GPIO_TypeDef *gpio = (is_porta) ? hwp_gpio1 : hwp_gpio2;
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Pin = pin;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(gpio, &GPIO_InitStruct);

    HAL_GPIO_WritePin(gpio, pin, (GPIO_PinState)val);
}

#define MPI2_POWER_PIN  (11) /**< MPI2 外设电源控制脚（Port A）。 */

/** @brief 弱符号：深睡前关闭自定义外设电源。 */
__WEAK void BSP_PowerDownCustom(int coreid, bool is_deep_sleep)
{
    BSP_GPIO_Set(MPI2_POWER_PIN, 0, 1);
}

/** @brief 弱符号：唤醒后打开自定义外设电源。 */
__WEAK void BSP_PowerUpCustom(bool is_deep_sleep)
{
    BSP_GPIO_Set(MPI2_POWER_PIN, 1, 1);
}

/** @brief 板级上电：打开外设电源，深睡唤醒时恢复 PA21。 */
void BSP_Power_Up(bool is_deep_sleep)
{
    BSP_PowerUpCustom(is_deep_sleep);
#ifdef BSP_USING_BOARD_SF32LB52_LCD_52J_SD
    if (is_deep_sleep)
    {
        HAL_PIN_Set(PAD_PA21, GPIO_A21, PIN_NOPULL, 1);
    }
#endif /* BSP_USING_BOARD_SF32LB52_LCD_52J_SD */
}

/** @brief 板级 IO 下电（深睡入口）。 */
void BSP_IO_Power_Down(int coreid, bool is_deep_sleep)
{
    BSP_PowerDownCustom(coreid, is_deep_sleep);
}

/** @brief SDIO 上电（RT-Thread SDIO 占位）。 */
void BSP_SDIO_Power_Up(void)
{
#ifdef RT_USING_SDIO
    /* TODO: Add SDIO power up */

#endif

}

/** @brief SDIO 下电（RT-Thread SDIO 占位）。 */
void BSP_SDIO_Power_Down(void)
{
#ifdef RT_USING_SDIO
    /* TODO: Add SDIO power down */
#endif
}
