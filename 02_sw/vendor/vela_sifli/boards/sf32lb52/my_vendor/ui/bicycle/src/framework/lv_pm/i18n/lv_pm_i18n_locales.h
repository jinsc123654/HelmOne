/**
 * @file lv_pm_i18n_locales.h
 * @brief lv_pm 内置 locale 列表。
 */

#ifndef LV_PM_I18N_LOCALES_H
#define LV_PM_I18N_LOCALES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../include/lv_pm_i18n_def.h"

extern const lv_pm_locale_def_t lv_pm_locale_zh_cn;
extern const lv_pm_locale_def_t lv_pm_locale_en;

extern const lv_pm_locale_def_t * const lv_pm_builtin_locales[];
extern const unsigned lv_pm_builtin_locale_count;

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_I18N_LOCALES_H */
