/**
 * @file myvendor_epic_blit.h
 * @brief EPIC 2D 搬运（拷贝 / 纯色填充）的板级封装，给应用层用。
 *
 * 全树只有一份 EPIC 句柄（LVGL 侧 `lv_sifli_epic_cfg.c`），这里复用它、
 * 并把芯片语义收敛在 `services/myvendor_epic_blit.c` 里 —— 应用侧（如
 * bicycle 的 `vmap_blit.c`）不引任何芯片层头文件，只认 RGB565 + 矩形。
 *
 * **同步语义**：这几个函数返回时硬件已经停（走的是 polling 版 HAL），
 * 调用方不需要额外等待，也**必须**在同一个线程里连续调用（渲染路径 = UI 线程）。
 *
 * **重叠不安全**：EPIC 内部是"边读边写"的流水，同一块缓冲内源/目的矩形
 * 有交叠时，某些方向的读写顺序会自己写坏自己。是否可用由调用方自检决定，
 * 见 `boards/.../vmap/vmap_blit.c` 的 `vmap_blit_probe()`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_EPIC_BLIT_H
#define MYVENDOR_EPIC_BLIT_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief EPIC 是否可用（LVGL 侧已初始化硬件）。 */
bool myvendor_epic_ready(void);

/**
 * @brief 硬件是否**还在忙**（EPIC 的 IA_BUSY 位）。
 *
 * 同步路径返回后这里应当是 false；如果观察到 true，说明"返回即完成"这个
 * 前提不成立（作业还在写内存），搬迁结果和后续读都不能信。
 */
bool myvendor_epic_busy(void);

/**
 * @brief 把 `src` 的 (sx,sy,w,h) 拷到 `dst` 的 (dx,dy)。RGB565，行跨度为字节数。
 *
 * 源与目的可以是同一块缓冲（允许，但**重叠时结果是否可靠要调用方自己保证**）。
 * 越界、尺寸超限、硬件没初始化 —— 一律返回 false 且**什么都不做**。
 *
 * @return true = 硬件已执行完（返回时已停）；false = 没执行，调用方走 CPU。
 */
bool myvendor_epic_copy(uint16_t * dst_buf, uint32_t dst_stride, int32_t dx,
  int32_t dy, const uint16_t * src_buf, uint32_t src_stride, int32_t sx,
  int32_t sy, int32_t w, int32_t h);

/**
 * @brief 用 RGB565 纯色填充 `dst` 的 (x,y,w,h)（不透明，不混合底图）。
 * @return true = 硬件已执行完；false = 没执行，调用方走 CPU。
 */
bool myvendor_epic_fill(uint16_t * dst_buf, uint32_t dst_stride, int32_t x,
  int32_t y, int32_t w, int32_t h, uint16_t color565);

/** @brief 等到硬件完成（同步路径下是记账；见文件头）。 */
void myvendor_epic_wait(void);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_EPIC_BLIT_H */
