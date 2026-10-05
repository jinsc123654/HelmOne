/**
 * @file lv_pm_theme_outdoor.c
 * @brief lv_pm 页面管理 — theme_outdoor。
 */

#include "lv_pm_themes.h"

/**
 * @brief 自行车 pm theme on activate。
 */
extern void bicycle_pm_theme_on_activate(lv_pm_theme_id_t id, void * user_data);

const lv_pm_theme_def_t lv_pm_theme_outdoor = {
    .name = "outdoor",
    .colors = {
        .page_bg          = LV_COLOR_MAKE(0x14, 0x14, 0x14),
        .text_primary     = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .text_secondary   = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .accent           = LV_COLOR_MAKE(0xFF, 0x8A, 0x28),
        .status_bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .status_bg_opa    = 255,
        .panel_bg         = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .panel_bg_opa     = 255,
        .panel_border     = LV_COLOR_MAKE(0x8C, 0x8E, 0x8C),
        .panel_border_opa = 255,
        .rec              = LV_COLOR_MAKE(0xC6, 0x00, 0x00),
        .toast_bg         = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .toast_bg_opa     = 255,
        .toast_text       = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .focus            = LV_COLOR_MAKE(0xFF, 0x8A, 0x28),
        .focus_editing    = LV_COLOR_MAKE(0xFF, 0x8A, 0x28),
    },
    .on_activate = bicycle_pm_theme_on_activate,
};
