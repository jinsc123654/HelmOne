/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/include/sf32lb_adc.h
 *
 * SF32LB52x GPADC. QFN68 pin 20 VBATS is internally wired to software
 * channel 7 (datasheet CH8 / BAT). It is not a GPIO: do not HAL_PIN_Set.
 * CH7 + ANAU EN_VBAT_MON samples the on-chip battery divider; ANAU EN_BG
 * must be set while GPADC runs (datasheet). `am_data` is pack millivolts:
 * factory two-point converts raw to tap mV, then eFuse vbat_mv/vbat_reg
 * undoes the divider (0.5 @ 3.3 V AVDD or 0.3 @ 1.8 V). No eFuse: ×2.01.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_SIFLI_SF32LB52_INCLUDE_SF32LB_ADC_H
#define __VENDOR_SIFLI_SF32LB52_INCLUDE_SF32LB_ADC_H

#include <stdint.h>

#define ADC_CHAN_0             0
#define ADC_CHAN_1             1
#define ADC_CHAN_2             2
#define ADC_CHAN_3             3
#define ADC_CHAN_4             4
#define ADC_CHAN_5             5
#define ADC_CHAN_6             6
#define ADC_CHAN_7             7

/** Internal VBATS / BAT monitor (not PA20). */
#define ADC_CHAN_VBAT          ADC_CHAN_7

/**
 * @brief Init GPADC and register NuttX ADC character device @p devpath.
 *
 * Default channel is VBATS (CH7). `ioctl(ANIOC_TRIGGER)` then `read()` of
 * `struct adc_msg_s` returns pack millivolts in `am_data`.
 * Busy-waits ~300 ms once for analog settle. Do not trigger from IRQ/wdog.
 *
 * @return 0 or a negative errno.
 */
int sf32lb_adc_init(const char *devpath);

/**
 * @brief HCLK 切换**前**调用：排空在途转换（持有驱动锁）。
 *
 * @details GPADC 的转换计数时钟就是 PCLK1（= HCLK >> PDIV1，本芯片没有专属
 *          分频器），而采样宽度是按 PCLK1 算出来的。切频瞬间若正好有转换在跑，
 *          结果不可信；拿到驱动锁即说明没有转换在途，等价于 SD 的"停时钟"。
 *          锁由 sf32lb_adc_reclock() 释放，两者必须成对调用。
 *          未初始化时是空操作。
 *
 * @return 0。
 */
int sf32lb_adc_clk_hold(void);

/**
 * @brief HCLK 切换**后**调用：把 ADC 的采样时序写回标准值并读回校验。
 *
 * @details 采样宽度是**固定的标准值**（`ADC_STD_CONV_WIDTH` / `ADC_STD_SAMP_WIDTH` /
 *          `ADC_STD_DATA_DLY`，≈ ATE 的 76/74/2），**不随 HCLK 缩放** —— 官方 HAL
 *          例程 `adc_battery` 就是这样，从不调 `HAL_ADC_SetFreq()`。
 *          反面教材是本驱动原先的做法：调 `HAL_ADC_SetFreq()`，而它在 52x 的
 *          `GPADC_CALIB_FLOW_VERSION == 3` 分支里**忽略 freq 实参**、按调用那一刻的
 *          PCLK1 把宽度放大（HCLK=240 时算到 250+，贴着 `CONV_WIDTH` 那个 **8 位**
 *          字段的上限）。宽度与实际时钟脱节后，电池读数会随 DVFS 档位在
 *          4080~4571 mV 之间跳，而电池真实电压并未变化。
 *
 *          本函数因此做两件事：把已知正确的那组宽度写回去（万一被别处改坏能纠正），
 *          再从**寄存器**读回校验（`Init.*` 软件副本对硬件截断是瞎的）。
 *
 * @return 0 成功；-EIO 读回校验不通过。
 */
int sf32lb_adc_reclock(void);

#endif /* __VENDOR_SIFLI_SF32LB52_INCLUDE_SF32LB_ADC_H */
