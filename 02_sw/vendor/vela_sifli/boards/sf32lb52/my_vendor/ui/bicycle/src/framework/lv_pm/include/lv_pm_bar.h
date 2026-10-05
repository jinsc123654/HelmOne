/**
 * @file lv_pm_bar.h
 * @brief lv_pm 页面管理 — bar。
 */

#ifndef LV_PM_BAR_H
#define LV_PM_BAR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_pm_config.h"
#include "lv_pm_theme_def.h"
#include "lvgl/lvgl.h"

struct lv_pm_page_def;
typedef struct lv_pm_page_def * lv_pm_page_t;

/**
 * @brief lv_pm bars init。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_bars_init(lv_obj_t * parent);
/**
 * @brief lv_pm bar apply for page。
 */
void lv_pm_bar_apply_for_page(lv_pm_page_t page);

/** @brief 有状态栏的页面：高度减去栏高，整体下移到栏下。 */
void lv_pm_page_layout_below_bars(lv_pm_page_t page);

/**
 * @brief lv_color_derive_statusbar_auto 接口。
 */
lv_color_t lv_color_derive_statusbar_auto(lv_color_t bg_color);
/**
 * @brief lv_color_derive_backbar_auto 接口。
 */
lv_color_t lv_color_derive_backbar_auto(lv_color_t bg);

#if LV_PM_USE_STA_BAR

/**
 * @brief lv_pm status bar main。
 */
lv_obj_t * lv_pm_status_bar_main(void);
/**
 * @brief lv_pm status bar cont。
 */
lv_obj_t * lv_pm_status_bar_cont(void);
/** @brief 展开的下拉面板（主题化）；页面特有扩展（圈数/录制）。 */
lv_obj_t * lv_pm_status_bar_panel(void);
/**
 * @brief lv_pm status bar bg。
 */
lv_obj_t * lv_pm_status_bar_bg(void);
/**
 * @brief lv_pm status bar set height。
 */
void lv_pm_status_bar_set_height(lv_coord_t height);
/**
 * @brief lv_pm status bar height。
 */
lv_coord_t lv_pm_status_bar_height(void);
/** @brief 折叠条：黑色；展开下拉背景：主题 status_bg。 */
void lv_pm_status_bar_apply_theme(const lv_pm_theme_def_t * def);

#define get_status_bar_main()  lv_pm_status_bar_main()
#define get_status_bar_cont()  lv_pm_status_bar_cont()
#define get_status_bar_bg()    lv_pm_status_bar_bg()

#else

static inline lv_obj_t * lv_pm_status_bar_main(void) { return NULL; }
static inline lv_obj_t * lv_pm_status_bar_cont(void) { return NULL; }
static inline lv_obj_t * lv_pm_status_bar_bg(void) { return NULL; }
/**
 * @brief lv_pm status bar set height。
 */
static inline void lv_pm_status_bar_set_height(lv_coord_t height) { LV_UNUSED(height); }
static inline lv_coord_t lv_pm_status_bar_height(void) { return 0; }
static inline void lv_pm_status_bar_apply_theme(const lv_pm_theme_def_t * def)
{
    LV_UNUSED(def);
}

#endif /* LV_PM_USE_STA_BAR */

#if LV_PM_USE_BACK_BAR

/**
 * @brief lv_pm back bar main。
 */
lv_obj_t * lv_pm_back_bar_main(void);
/**
 * @brief lv_pm back bar handle。
 */
lv_obj_t * lv_pm_back_bar_handle(void);

#else

static inline lv_obj_t * lv_pm_back_bar_main(void) { return NULL; }
static inline lv_obj_t * lv_pm_back_bar_handle(void) { return NULL; }

#endif /* LV_PM_USE_BACK_BAR */

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_BAR_H */
