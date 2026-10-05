/**
 * @file lv_pm_themes.c
 * @brief lv_pm 页面管理 — themes。
 */

#include "lv_pm_themes.h"

/** @brief 夜间主题（设计稿 zcode/analysis/helm_menu_redesign/preview/dark）。
 *  @note 只在本注册表里用，所以 static；取值就是设计稿 dark 那套：纸黑底、
 *        #1C1C1E 卡片、纸白正文、#8E8E93 次级、橙强调。activate 钩子沿用工程
 *        现成的那个（地图换色 / 应用存储等）。 */
extern void bicycle_pm_theme_on_activate(lv_pm_theme_id_t id, void * user_data);

static const lv_pm_theme_def_t s_theme_night = {
    .name = "night",
    .colors = {
        .page_bg          = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .text_primary     = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .text_secondary   = LV_COLOR_MAKE(0x8E, 0x8E, 0x93),
        .accent           = LV_COLOR_MAKE(0xFF, 0x5C, 0x33),
        .status_bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .status_bg_opa    = 255,
        .panel_bg         = LV_COLOR_MAKE(0x1C, 0x1C, 0x1E),
        .panel_bg_opa     = 255,
        .panel_border     = LV_COLOR_MAKE(0x2C, 0x2C, 0x2E),
        .panel_border_opa = 255,
        .rec              = LV_COLOR_MAKE(0xC6, 0x00, 0x00),
        .toast_bg         = LV_COLOR_MAKE(0x1C, 0x1C, 0x1E),
        .toast_bg_opa     = 255,
        .toast_text       = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .focus            = LV_COLOR_MAKE(0x1D, 0x4E, 0xD8),
        .focus_editing    = LV_COLOR_MAKE(0x1D, 0x4E, 0xD8),
    },
    .on_activate = bicycle_pm_theme_on_activate,
};

const lv_pm_theme_def_t * const lv_pm_builtin_themes[] = {
    &lv_pm_theme_classic,
    &lv_pm_theme_outdoor,
    &s_theme_night,
};

const unsigned lv_pm_builtin_theme_count =
    (unsigned)(sizeof(lv_pm_builtin_themes) / sizeof(lv_pm_builtin_themes[0]));
