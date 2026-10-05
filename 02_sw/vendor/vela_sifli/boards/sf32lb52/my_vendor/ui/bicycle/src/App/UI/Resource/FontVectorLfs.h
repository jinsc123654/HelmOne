/**
 * @file FontVectorLfs.h
 * @brief 生成/位图资源数据。
 */

/*
 * Load subset TTF via board system font (PSRAM, liblvgl).
 */

#ifndef BICYCLE_FONT_VECTOR_LFS_H
#define BICYCLE_FONT_VECTOR_LFS_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_font_t *font_vector_lfs_create(int32_t size);
void font_vector_lfs_destroy(lv_font_t *font);
bool font_vector_lfs_preload(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_FONT_VECTOR_LFS_H */
