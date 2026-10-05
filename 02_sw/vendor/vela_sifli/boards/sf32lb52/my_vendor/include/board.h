/**
 * @file board.h
 * @brief NuttX 板级公开定义：按键编号等。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __VENDOR_SIFLI_BOARDS_SF32LB52_DEVKIT_LCD_INCLUDE_BOARD_H
#define __VENDOR_SIFLI_BOARDS_SF32LB52_DEVKIT_LCD_INCLUDE_BOARD_H

#include "custom_mem_map.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BUTTON_KEY2      0                 /**< 用户键 Key2（PA11）。 */
#define NUM_BUTTONS      1                 /**< 按键个数。 */
#define BUTTON_KEY2_BIT  (1 << BUTTON_KEY2) /**< Key2 位掩码。 */

#endif /* __VENDOR_SIFLI_BOARDS_SF32LB52_DEVKIT_LCD_INCLUDE_BOARD_H */
