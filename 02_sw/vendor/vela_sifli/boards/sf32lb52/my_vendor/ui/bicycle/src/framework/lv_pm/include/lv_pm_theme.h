/**
 * @file lv_pm_theme.h
 * @brief lv_pm 页面管理 — theme。
 */

#ifndef LV_PM_THEME_H
#define LV_PM_THEME_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_pm_theme_def.h"

struct lv_pm_page_def;
typedef struct lv_pm_page_def * lv_pm_page_t;

#define LV_PM_THEME_ID_INVALID ((lv_pm_theme_id_t)0xFFu)

/** @brief 可选全局通知（遗留/额外监听者）。 */
typedef void (*lv_pm_theme_notify_cb)(lv_pm_theme_id_t theme_id, void * user_data);

/**
 * @brief lv_pm page theme init。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_page_theme_init(lv_pm_page_t pm_page);

/**
 * @brief lv_pm set theme changed。
 */
int8_t lv_pm_set_theme_changed(void * page, lv_pm_page_theme_cb cb, void * user_data);
/**
 * @brief lv_pm set theme notify cb。
 */
void lv_pm_set_theme_notify_cb(lv_pm_theme_notify_cb cb, void * user_data);

/**
 * @brief lv_pm theme count。
 * @return 请求的值。
 */
unsigned lv_pm_theme_count(void);
/**
 * @brief lv_pm theme name。
 */
const char * lv_pm_theme_name(lv_pm_theme_id_t id);
/**
 * @brief lv_pm theme find。
 */
lv_pm_theme_id_t lv_pm_theme_find(const char * name);
/**
 * @brief lv_pm theme current。
 */
const lv_pm_theme_def_t * lv_pm_theme_current(void);

/**
 * @brief lv_pm theme get。
 * @return 请求的值。
 */
lv_pm_theme_id_t lv_pm_theme_get(void);
/**
 * @brief lv_pm theme set。
 */
bool lv_pm_theme_set(lv_pm_theme_id_t id);
/**
 * @brief lv_pm theme set by name。
 */
bool lv_pm_theme_set_by_name(const char * name);
/**
 * @brief lv_pm theme set next。
 */
bool lv_pm_theme_set_next(void);

/** @brief 将当前预设应用到 pm 页面壳层（背景/默认字体/焦点）。 */
void lv_pm_theme_apply_page_shell(lv_pm_page_t page);
/**
 * @brief lv_pm theme apply。
 */
void lv_pm_theme_apply(void * pm_page);
/**
 * @brief lv_pm theme apply all。
 */
void lv_pm_theme_apply_all(void);

/** @brief 将当前预设推入 LVGL lv_theme_t（apply_cb + lv_display_set_theme）。 */
void lv_pm_theme_lvgl_bind(const lv_pm_theme_def_t * def);

/** @brief 循环顶补跑"整树样式重算"（1.2~1.5 s 的活，不能挡在换主题的同步路径里）。
 *  @details 详见 `lv_pm_theme_lvgl_bind()` 的注释；UI 主循环每圈调一次，空操作无开销。 */
void lv_pm_theme_poll_styles(void);

/** @brief 只补跑"整树样式重算"那一步（由 `lv_pm_theme_poll_styles()` 调用）。 */
void lv_pm_theme_report_styles_now(void);

/** @brief 一次性叠加 pm 页面壳层样式（add_style，不 remove_style_all）。 */
void lv_pm_theme_lvgl_attach_page(lv_obj_t * page);
/**
 * @brief lv_pm theme lvgl attach focus。
 */
void lv_pm_theme_lvgl_attach_focus(lv_obj_t * obj);

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_THEME_H */
