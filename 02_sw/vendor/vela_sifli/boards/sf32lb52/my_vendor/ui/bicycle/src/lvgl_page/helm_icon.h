/**
 * @file helm_icon.h
 * @brief 线稿 24×24 描边图标（对齐 index.html sico / mico）。
 */

#ifndef HELM_ICON_H
#define HELM_ICON_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HELM_ICO_NONE = 0,
    HELM_ICO_NAV,
    HELM_ICO_BLE,
    HELM_ICO_USB,
    HELM_ICO_LIST,
    HELM_ICO_GEAR,
    HELM_ICO_SUN,
    HELM_ICO_EYE,
    HELM_ICO_UNIT,
    HELM_ICO_PAUSE,
    HELM_ICO_BELL,
    HELM_ICO_SOUND,
    HELM_ICO_CALL,
    HELM_ICO_INBOX,
    HELM_ICO_CHIP,
    HELM_ICO_GPX,
    HELM_ICO_STAR,
    HELM_ICO_REV,
    HELM_ICO_PLAY,
    HELM_ICO_TIME,
    HELM_ICO_DIST,
    HELM_ICO_AVG,
    HELM_ICO_MAX,
    HELM_ICO_CLIMB,
    HELM_ICO_ALT,
    HELM_ICO_SAVE,
    HELM_ICO_FLAG,
    HELM_ICO_PIN,
    HELM_ICO_SKIP,
    HELM_ICO_HR,
    HELM_ICO_CAD,
    HELM_ICO_BATT,
    HELM_ICO_BOLT,
    HELM_ICO_POWER,
    HELM_ICO_WARN,
    HELM_ICO_GPS,
    HELM_ICO_XMARK,
    HELM_ICO_CHECK,
    HELM_ICO_GO
} helm_ico_id_t;

lv_obj_t * helm_icon_create(lv_obj_t * parent, helm_ico_id_t id, lv_coord_t size);
void helm_icon_set(lv_obj_t * obj, helm_ico_id_t id);
void helm_icon_set_color(lv_obj_t * obj, lv_color_t color);

#ifdef __cplusplus
}
#endif

#endif /* HELM_ICON_H */
