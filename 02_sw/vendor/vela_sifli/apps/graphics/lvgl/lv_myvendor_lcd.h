/**
 * @file lv_myvendor_lcd.h
 */

#ifndef LV_MYVENDOR_LCD_H
#define LV_MYVENDOR_LCD_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LV_USE_MYVENDOR_LCD_DISP
#ifdef CONFIG_MYVENDOR_LCD_DISP
#define LV_USE_MYVENDOR_LCD_DISP CONFIG_MYVENDOR_LCD_DISP
#else
#define LV_USE_MYVENDOR_LCD_DISP 0
#endif
#endif

#if LV_USE_MYVENDOR_LCD_DISP

lv_display_t * lv_myvendor_lcd_create(const char * lcd_path);

#endif

#ifdef __cplusplus
}
#endif

#endif /* LV_MYVENDOR_LCD_H */
