/**
 * @file lv_pm_themes.h
 * @brief lv_pm 页面管理 — themes。
 */

#ifndef LV_PM_THEMES_H
#define LV_PM_THEMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../include/lv_pm_theme_def.h"

extern const lv_pm_theme_def_t lv_pm_theme_classic;
extern const lv_pm_theme_def_t lv_pm_theme_outdoor;

/** @brief 内置预设表（顺序 = 主题 ID）。 */
extern const lv_pm_theme_def_t * const lv_pm_builtin_themes[];
extern const unsigned lv_pm_builtin_theme_count;

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_THEMES_H */
