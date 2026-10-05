/**
 * @file lv_pm_theme_classic.c
 * @brief lv_pm 页面管理 — theme_classic。
 */

#include "lv_pm_themes.h"

/**
 * @brief 自行车 pm theme on activate。
 */
extern void bicycle_pm_theme_on_activate(lv_pm_theme_id_t id, void * user_data);

const lv_pm_theme_def_t lv_pm_theme_classic = {
    .name = "classic",
    .colors = {
        .page_bg          = LV_COLOR_MAKE(0xE4, 0xDC, 0xD0),
        .text_primary     = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .text_secondary   = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .accent           = LV_COLOR_MAKE(0xFF, 0x6E, 0x12),
        .status_bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .status_bg_opa    = 255,
        .panel_bg         = LV_COLOR_MAKE(0xFB, 0xF6, 0xEE),
        .panel_bg_opa     = 255,
        .panel_border     = LV_COLOR_MAKE(0xE4, 0xDC, 0xD0),
        .panel_border_opa = 255,
        .rec              = LV_COLOR_MAKE(0xC6, 0x00, 0x00),
        .toast_bg         = LV_COLOR_MAKE(0xFB, 0xF6, 0xEE),
        .toast_bg_opa     = 255,
        .toast_text       = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .focus            = LV_COLOR_MAKE(0xFF, 0x6E, 0x12),
        .focus_editing    = LV_COLOR_MAKE(0xFF, 0x6E, 0x12),
    },
    .on_activate = bicycle_pm_theme_on_activate,
};
