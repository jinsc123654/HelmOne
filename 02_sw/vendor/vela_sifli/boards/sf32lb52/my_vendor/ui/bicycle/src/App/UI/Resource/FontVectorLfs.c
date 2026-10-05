/**
 * @file FontVectorLfs.c
 * @brief 生成/位图资源数据。
 */

/*
 * Vector font — thin wrapper around board system font (PSRAM + liblvgl).
 */

#include "FontVectorLfs.h"
#include "myvendor_system_font.h"

lv_font_t *font_vector_lfs_create(int32_t size)
{
    return myvendor_system_font_get(size);
}

void font_vector_lfs_destroy(lv_font_t *font)
{
    myvendor_system_font_put(font);
}

bool font_vector_lfs_preload(void)
{
    return myvendor_system_font_preload();
}
