/**
 * @file helm_toolbox.h
 * @brief 菜单工具箱：指南针 / 水平仪 / 加速度计 / 高度计，以及系统状态条。
 */

#ifndef HELM_TOOLBOX_H
#define HELM_TOOLBOX_H

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HELM_TOOL_COMPASS = 0,
    HELM_TOOL_LEVEL,
    HELM_TOOL_GMETER,
    HELM_TOOL_ALTIMETER
} helm_tool_id_t;

lv_obj_t * helm_toolbox_build(lv_obj_t * parent);
void helm_toolbox_open(helm_tool_id_t id);
void helm_toolbox_hide(void);
void helm_toolbox_unload(void);
const char * helm_toolbox_title(void);
const char * helm_toolbox_title_for(helm_tool_id_t id);
/** @brief KEY1 左翻 / KEY2 右翻：高度计气压 ↔ 高度。其它工具忽略。 */
bool helm_toolbox_key1(void);
bool helm_toolbox_key2(void);

lv_obj_t * helm_sysstat_build(lv_obj_t * parent);
void helm_sysstat_show(void);
void helm_sysstat_hide(void);
void helm_sysstat_unload(void);

#ifdef __cplusplus
}
#endif

#endif /* HELM_TOOLBOX_H */
