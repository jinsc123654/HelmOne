/**
 * @file lv_pm_i18n.h
 * @brief lv_pm 页面管理 — i18n。
 */

#ifndef LV_PM_I18N_H
#define LV_PM_I18N_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_pm_i18n_def.h"
#include "lv_pm_i18n_keys.h"

#include "lvgl/lvgl.h"

struct lv_pm_page_def;
typedef struct lv_pm_page_def * lv_pm_page_t;

#define LV_PM_I18N_LOCALE_INVALID ((lv_pm_locale_id_t)0xFFu)

/** @brief 默认 locale 索引（zh_CN）。 */
#define LV_PM_I18N_DEFAULT_LOCALE ((lv_pm_locale_id_t)0u)

/**
 * @brief lv_pm set i18n changed。
 */
int8_t lv_pm_set_i18n_changed(void * page, lv_pm_page_i18n_cb cb, void * user_data);
/**
 * @brief lv_pm set i18n notify cb。
 */
void lv_pm_set_i18n_notify_cb(lv_pm_i18n_notify_cb cb, void * user_data);

/**
 * @brief lv_pm i18n locale count。
 * @return 请求的值。
 */
unsigned lv_pm_i18n_locale_count(void);
/**
 * @brief lv_pm i18n locale name。
 */
const char * lv_pm_i18n_locale_name(lv_pm_locale_id_t id);
/**
 * @brief lv_pm i18n locale find。
 */
lv_pm_locale_id_t lv_pm_i18n_locale_find(const char * name);

/**
 * @brief lv_pm i18n get。
 * @return 请求的值。
 */
lv_pm_locale_id_t lv_pm_i18n_get(void);
/**
 * @brief lv_pm i18n get text。
 */
const char * lv_pm_i18n_get_text(lv_pm_i18n_key_t key);
/**
 * @brief lv_pm i18n set。
 */
bool lv_pm_i18n_set(lv_pm_locale_id_t id);
/**
 * @brief lv_pm i18n set by name。
 */
bool lv_pm_i18n_set_by_name(const char * name);
/**
 * @brief lv_pm i18n set next。
 */
bool lv_pm_i18n_set_next(void);
/**
 * @brief lv_pm i18n apply all。
 */
void lv_pm_i18n_apply_all(void);

/** @brief lv_pm_i18n_get_text(key) 的简写。 */
static inline const char * lv_pm_tr(lv_pm_i18n_key_t key)
{
/**
 * @brief lv_pm i18n get text。
 */
    return lv_pm_i18n_get_text(key);
}

static inline void lv_pm_label_set_tr(lv_obj_t * label, lv_pm_i18n_key_t key)
{
    if (label != NULL) {
        lv_label_set_text(label, lv_pm_tr(key));
    }
}

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_I18N_H */
