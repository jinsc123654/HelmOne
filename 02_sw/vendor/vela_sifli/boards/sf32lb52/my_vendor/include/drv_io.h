/**
 * @file drv_io.h
 * @brief 板级 BSP IO / 背光 / Flash / PSRAM 入口（SiFli HAL 约定）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DRV_IO_H__
#define __DRV_IO_H__

#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 MCU 支持包（HAL_PreInit 调用）。
 */
void HAL_MspInit(void);

/**
 * @brief 初始化引脚复用。
 */
void BSP_IO_Init(void);

/**
 * @brief 读 Flash1 分频。
 */
uint16_t BSP_GetFlash1DIV(void);
/** @brief 读 Flash2 分频。 */
uint16_t BSP_GetFlash2DIV(void);
/** @brief 读 Flash3 分频。 */
uint16_t BSP_GetFlash3DIV(void);
/** @brief 读 Flash4 分频。 */
uint16_t BSP_GetFlash4DIV(void);
/** @brief 读 Flash5 分频。 */
uint16_t BSP_GetFlash5DIV(void);
/** @brief 读外部 Flash 分频。 */
uint16_t BSP_GetFlashExtDiv(void);

/** @brief 写 Flash1 分频。 */
void BSP_SetFlash1DIV(uint16_t div);
/** @brief 写 Flash2 分频。 */
void BSP_SetFlash2DIV(uint16_t div);
/** @brief 写 Flash3 分频。 */
void BSP_SetFlash3DIV(uint16_t div);
/** @brief 写 Flash4 分频。 */
void BSP_SetFlash4DIV(uint16_t div);
/** @brief 写 Flash5 分频。 */
void BSP_SetFlash5DIV(uint16_t div);
/** @brief 写外部 Flash 分频。 */
void BSP_SetFlashExtDiv(uint16_t div);

/**
 * @brief 板级下电。
 * @param coreid 核 ID。
 * @param is_deep_sleep 是否深睡。
 */
void BSP_IO_Power_Down(int coreid, bool is_deep_sleep);

/**
 * @brief 板级上电。
 */
void BSP_Power_Up(bool is_deep_sleep);

/** @brief 客户板上电定制。 */
void BSP_PowerDownCustom(int coreid, bool is_deep_sleep);
/** @brief 客户板下电定制。 */
void BSP_PowerUpCustom(bool is_deep_sleep);

/**
 * @brief 默认引脚复用（见 bsp_pinmux.c）。
 */
void BSP_PIN_Init(void);

/** @brief LCD QSPI 引脚。 */
void BSP_PIN_LCD(void);
/** @brief LCD 软 SPI 引脚（若使用）。 */
void BSP_PIN_LCD_SOFTSPI(void);

/*
 * App 是否重新复位并初始化 NV3031A。
 * 不定义：沿用 2SFBL 已点亮的屏和背光（避免开机黑一下）。
 * 需要完整 init 时取消下面这行注释。
 */
/* #define NV3031A_APP_REINIT_PANEL */

/**
 * @brief 按上次 PrefPct / persist 打开或关闭背光。
 * @param on 非 0 打开。
 */
void BSP_LCD_BL_Set(uint8_t on);

/**
 * @brief 记住下次 BSP_LCD_BL_Set(1) 要用的百分比（不立刻改 PWM）。
 */
void BSP_LCD_BL_PrefPct(uint8_t brightness_pct);

/**
 * @brief 立刻设置背光 PWM 百分比（0 为关），并记住策略值。
 * @return 0 成功，负值为错误码。
 */
int BSP_LCD_BL_SetPct(uint8_t brightness_pct);

/**
 * @brief 只改当前 PWM 占空比，不改 Pref/persist 策略值。
 *
 * 亮度过渡用这个；关屏休眠仍靠 @ref BSP_LCD_BL_Set。
 * @return 0 成功，负值为错误码。
 */
int BSP_LCD_BL_PwmPct(uint8_t brightness_pct);

/**
 * @brief 当前 PWM 百分比。
 */
uint8_t BSP_LCD_BL_GetPct(void);

/**
 * @brief 已保存/Pref 的百分比。
 */
uint8_t BSP_LCD_BL_GetSavedPct(void);

/**
 * @brief 硬件夹紧后的最大百分比。
 */
uint8_t BSP_LCD_BL_GetMaxPct(void);

/** @brief LCD D/C 线。 */
void BSP_LCD_DC_Set(uint8_t is_cmd);
/** @brief LCD 复位脚，1 为高。 */
void BSP_LCD_Reset(uint8_t high1_low0);
/** @brief 打开屏电源（如 PA10 下拉供电）。 */
void BSP_LCD_PowerUp(void);
/** @brief 关闭屏电源。 */
void BSP_LCD_PowerDown(void);

/**
 * @brief 按地址取 Flash 句柄。
 */
void *BSP_Flash_get_handle(uint32_t addr);
/** @brief 按控制器 ID 取 Flash 句柄。 */
void *BSP_Flash_get_handle_by_id(uint8_t id);

int BSP_Flash_read_id(uint32_t addr);

int BSP_Nor_read(uint32_t addr, uint8_t *buf, int size);
int BSP_Nor_erase(uint32_t addr, uint32_t size);
int BSP_Nor_write(uint32_t addr, const uint8_t *buf, uint32_t size);

void BSP_Flash_var_init(void);
int BSP_Flash_hw1_init(void);
int BSP_Flash_hw2_init(void);
int BSP_Flash_hw2_init_with_no_dtr(void);
int BSP_Flash_hw3_init(void);
int BSP_Flash_hw4_init(void);
int BSP_Flash_hw5_init(void);
uint32_t flash_get_freq(int clk_module, uint16_t clk_div, uint8_t hcpu);
void BSP_Board_EnableNandMpiClocks(void);
void BSP_Board_PrintClocks(void);
/** @brief 把 HCLK 拉到 240 MHz（一次性；运行时 72/96/144/240 走 `sf32lb_dvfs`）。 */
void BSP_Board_EnableHclk240(void);
/** @brief SysTick 随 HCLK 重配。 */
void sifli_systick_reclock(void);

int BSP_Flash_Init(void);

/** @brief SD 卡电源。 */
void BSP_SD_PowerUp(void);

/**
 * @brief 写 GPIO。
 * @param pin 引脚号。
 * @param val 电平。
 * @param is_porta 是否 GPIOA。
 */
void BSP_GPIO_Set(int pin, int val, int is_porta);

#ifdef BSP_USING_PSRAM

    /**
     * @brief PSRAM 控制器硬件初始化。
     * @return 0 成功。
     */
    int bsp_psramc_init(void);

    /**
     * @brief 读 PSRAM 时钟频率。
     * @param addr PSRAM 基址。
     */
    uint32_t bsp_psram_get_clk(uint32_t addr);

    /**
     * @brief 更新 PSRAM 刷新率。
     */
    int bsp_psram_update_refresh_rate(char *name, uint32_t value);

    /**
     * @brief PSRAM 进低功耗。
     */
    int bsp_psram_enter_low_power(char *name);

    /**
     * @brief PSRAM 深掉电。
     */
    int bsp_psram_deep_power_down(char *name);

    /**
     * @brief PSRAM 退出低功耗。
     */
    int bsp_psram_exit_low_power(char *name);

    /**
     * @brief 设置部分阵列自刷新。
     * @param deno 刷新分母，须为 2^n；大于 16 则整片不刷新。
     */
    int bsp_psram_set_pasr(char *name, uint8_t top, uint8_t deno);

    /**
     * @brief PSRAM 自动校准延迟。
     */
    int bsp_psram_auto_calib(char *name, uint8_t *sck, uint8_t *dqs);

    /**
     * @brief 等待 PSRAM 硬件空闲。
     */
    void bsp_psram_wait_idle(char *name);

#else

    #define bsp_psramc_init() -1
    #define bsp_psram_get_clk() 0
    #define bsp_psram_update_refresh_rate(name,value) -1
    #define bsp_psram_enter_low_power(name) -1
    #define bsp_psram_deep_power_down(name) -1
    #define bsp_psram_exit_low_power(name) -1
    #define bsp_psram_set_pasr(name,top,deno) -1
    #define bsp_psram_auto_calib(name,sck,dqs) -1
    #define bsp_psram_wait_idle(name)

#endif

#ifdef __cplusplus
}
#endif
#endif
