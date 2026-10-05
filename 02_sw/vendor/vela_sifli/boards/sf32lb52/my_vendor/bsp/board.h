/**
 * @file board.h
 * @brief NuttX 板级入口头文件，聚合 sfconfig 与 bsp_board。
 *
 * RT-Thread 板型额外包含 drv_common.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __BOARD_H__
#define __BOARD_H__

#include "sfconfig.h"
#include "bf0_hal.h"
#include "bsp_board.h"
#ifdef BSP_USING_RTTHREAD
    #include "drv_common.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif


#ifdef __cplusplus
}
#endif

#endif /* __BOARD_H__ */
