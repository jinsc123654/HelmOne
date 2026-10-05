/**
 * @file lv_pm_theme_def.h
 * @brief lv_pm 页面管理 — theme_def。
 */

#ifndef LV_PM_THEME_DEF_H
#define LV_PM_THEME_DEF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"
#include <stdint.h>

typedef uint8_t lv_pm_theme_id_t;

/** @brief 页面私有控件钩子（LiveMap vmap 刷新等）。 */
typedef void (*lv_pm_page_theme_cb)(void * pm_page, lv_pm_theme_id_t theme_id,
                                    void * user_data);

/** @brief 每主题钩子：地图换色、应用存储等（在 page theme_changed_cb 之前）。 */
typedef void (*lv_pm_theme_activate_cb)(lv_pm_theme_id_t id, void * user_data);

typedef struct {
    lv_color_t page_bg;
    lv_color_t text_primary;
    lv_color_t text_secondary;
    lv_color_t accent;
    lv_color_t status_bg;
    lv_opa_t   status_bg_opa;
    lv_color_t panel_bg;
    lv_opa_t   panel_bg_opa;
    lv_color_t panel_border;
    lv_opa_t   panel_border_opa;
    lv_color_t rec;
    lv_color_t toast_bg;
    lv_opa_t   toast_bg_opa;
    lv_color_t toast_text;
    lv_color_t focus;
    lv_color_t focus_editing;
} lv_pm_theme_colors_t;

typedef struct {
    /** @brief NULL 表示页面壳层保留 LVGL 主题默认。 */
    const lv_font_t * body;
    const lv_font_t * title;
} lv_pm_theme_fonts_t;

typedef struct {
    const char * name;
    lv_pm_theme_colors_t colors;
    lv_pm_theme_fonts_t fonts;
    lv_pm_theme_activate_cb on_activate;
    void * user_data;
} lv_pm_theme_def_t;

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_THEME_DEF_H */
