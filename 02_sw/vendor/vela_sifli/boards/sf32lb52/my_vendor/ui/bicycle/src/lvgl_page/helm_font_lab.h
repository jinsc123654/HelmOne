/**
 * @file helm_font_lab.h
 * @brief 设置 → 字体试验：多套位图，KEY1/KEY2 切换。
 */

#ifndef HELM_FONT_LAB_H
#define HELM_FONT_LAB_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * helm_font_lab_build(lv_obj_t * parent);
void helm_font_lab_refresh(void);
void helm_font_lab_next(void);
void helm_font_lab_prev(void);

#ifdef __cplusplus
}
#endif

#endif /* HELM_FONT_LAB_H */
