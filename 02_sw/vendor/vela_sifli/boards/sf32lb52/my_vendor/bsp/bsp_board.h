/**
 * @file bsp_board.h
 * @brief BSP 板级时钟与堆边界声明。
 *
 * 提供 HCPU 预初始化、Flash/MPI 时钟、HCLK 240MHz 提频及 HEAP_BEGIN/HEAP_END。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __BSP_BOARD_H__
#define __BSP_BOARD_H__

#include "sfconfig.h"
#include "drv_io.h"
#include "bf0_hal.h"
#ifdef PMIC_CTRL_ENABLE
    #include "pmic_controller.h"
#endif /* PMIC_CTRL_ENABLE */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __CC_ARM
extern int Image$$RW_IRAM1$$ZI$$Limit;
#define HEAP_BEGIN      ((void *)&Image$$RW_IRAM1$$ZI$$Limit) /**< 链接脚本 BSS 结束，堆起始。 */
#elif __ICCARM__
#pragma section="CSTACK"
#define HEAP_BEGIN      (__segment_end("CSTACK")) /**< IAR 堆起始。 */
#elif defined (__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)
extern int Image$$RW_IRAM1$$ZI$$Limit;
#define HEAP_BEGIN      ((void *)&Image$$RW_IRAM1$$ZI$$Limit) /**< ARMClang 堆起始。 */
#elif defined ( __GNUC__ )
extern int __bss_end;
#define HEAP_BEGIN      ((void *)&__bss_end) /**< GCC 堆起始（BSS 之后）。 */
#endif

#ifdef SOC_BF0_HCPU
#ifndef HEAP_END
#define HEAP_END       (HCPU_RAM_DATA_START_ADDR + HCPU_RAM_DATA_SIZE) /**< HCPU 数据 RAM 末尾。 */
#endif
#else
#define HEAP_END       (LCPU_RAM_DATA_START_ADDR + LCPU_RAM_DATA_SIZE) /**< LCPU 数据 RAM 末尾。 */
#endif

/** @brief 弱符号：应用可覆盖的系统时钟配置。 */
void SystemClock_Config(void);

/** @brief HCPU/LCPU 上电早期时钟与低功耗域初始化。 */
void BSP_Board_PreInit(void);

/** @brief 使能 SDK Flash 时钟（DLL2、MPI 分频、PSRAM/NOR 路由）。 */
void BSP_Board_EnableSdkFlashClocks(void);

/** @brief 使能 NAND MPI2 时钟（FLASH2 走 SYSCLK，不碰 PSRAM 的 DLL2）。 */
void BSP_Board_EnableNandMpiClocks(void);

/** @brief 打印 HCPU / MPI 时钟诊断（NAND XIP 调试用）。 */
void BSP_Board_PrintClocks(void);

/** @brief 将 HCPU HCLK 提至 240 MHz（PSRAM XIP 稳定后调用）。 */
void BSP_Board_EnableHclk240(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_H__ */
