/**
 * @file lv_pm_i18n_def.h
 * @brief lv_pm 页面管理 — i18n_def。
 */

#ifndef LV_PM_I18N_DEF_H
#define LV_PM_I18N_DEF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef uint8_t lv_pm_locale_id_t;

/** @brief 页面钩子：locale 变更时刷新标签。 */
typedef void (*lv_pm_page_i18n_cb)(void * pm_page, lv_pm_locale_id_t locale_id,
                                   void * user_data);

/** @brief 可选全局通知（NSH 状态、持久化等）。 */
typedef void (*lv_pm_i18n_notify_cb)(lv_pm_locale_id_t locale_id, void * user_data);

typedef struct {
    const char * name;
    /** @brief 按 lv_pm_i18n_key_t 索引；长度 LV_PM_I18N_KEY_COUNT。NULL 项回退 locale。 */
    const char * const * strings;
    void * user_data;
} lv_pm_locale_def_t;

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_I18N_DEF_H */
