/**
 * @file myvendor_system_font_stub.c
 * @brief 无矢量字体时的空实现（工厂固件 / 未开 SYSTEM_VECTOR_FONT）。
 *        不打开 TTF、不占 PSRAM。
 */

#include "myvendor_system_font.h"

bool myvendor_system_font_preload(void)
{
    return false;
}

bool myvendor_system_font_preload_pump(void)
{
    return true;
}

bool myvendor_system_font_lv_bind(void)
{
    return false;
}

lv_font_t *myvendor_system_font_get(int32_t px)
{
    (void)px;
    return NULL;
}

void myvendor_system_font_put(lv_font_t *font)
{
    (void)font;
}

bool myvendor_system_font_preloaded(void)
{
    return false;
}
