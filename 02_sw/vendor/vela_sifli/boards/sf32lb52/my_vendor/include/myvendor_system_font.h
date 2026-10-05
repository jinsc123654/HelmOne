/**
 * @file myvendor_system_font.h
 * @brief 板级系统矢量字体（TTF 预载 PSRAM，FreeType 光栅）。
 *
 * 开启 MYVENDOR_SYSTEM_VECTOR_FONT 时链进 liblvgl。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_SYSTEM_FONT_H
#define MYVENDOR_SYSTEM_FONT_H

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把 TTF 读进 PSRAM（lv_init 之前也可）。幂等，会阻塞到读完。
 * @return 成功则为 true。
 */
bool myvendor_system_font_preload(void);

/**
 * @brief 分片把 TTF 读进 PSRAM，便于启动页边显示边加载。
 * @return true 已结束（成功或失败，查 preloaded()）；false 还需再调。
 */
bool myvendor_system_font_preload_pump(void);

/**
 * @brief lv_init() 之后创建缓存的 lv_font_t。幂等。
 * @return 成功则为 true。
 */
bool myvendor_system_font_lv_bind(void);

/**
 * @brief 按像素大小取缓存字体。
 * @param px 字号。
 * @return 句柄；不可用则为 NULL。
 */
lv_font_t *myvendor_system_font_get(int32_t px);

/**
 * @brief 释放一次引用（缓存字体直到 reset 才真正丢掉）。
 */
void myvendor_system_font_put(lv_font_t *font);

/**
 * @brief TTF 字节是否已在 PSRAM。
 */
bool myvendor_system_font_preloaded(void);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_SYSTEM_FONT_H */
