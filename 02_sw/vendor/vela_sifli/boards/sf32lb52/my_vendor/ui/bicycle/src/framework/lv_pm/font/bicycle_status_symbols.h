/**
 * @file bicycle_status_symbols.h
 * @brief 状态栏 Font Awesome 符号映射。
 */

#ifndef BICYCLE_STATUS_SYMBOLS_H
#define BICYCLE_STATUS_SYMBOLS_H

#include "lvgl/lvgl.h"

/* Font Awesome 6 solid — same codepoints as X-TRACK StatusBarView. */
#define BICYCLE_SYMBOL_SATELLITE      "\xEF\x9E\xBF" /* U+F7BF */
#define BICYCLE_SYMBOL_BATTERY_EMPTY  "\xEF\x89\x84" /* U+F244 */

LV_FONT_DECLARE(font_awesome_15);

#endif /* BICYCLE_STATUS_SYMBOLS_H */
