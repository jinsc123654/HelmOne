/**
 * @file lv_pm_core.h
 * @brief lv_pm 页面管理核心 API（创建/打开/关闭/跳转）。
 */

#ifndef LV_PM_CORE_H
#define LV_PM_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_pm_config.h"
#include "lv_pm_theme_def.h"
#include "lv_pm_i18n_def.h"
#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#if LVGL_VERSION_MAJOR == 8
#define lv_pm_malloc lv_mem_alloc
#define lv_pm_free   lv_mem_free
#else
#define lv_pm_malloc lv_malloc
#define lv_pm_free   lv_free
#endif

/** @brief 不透明页面 ID — 应用定义枚举值（见 bicycle_page_ids.h）。 */
typedef uint8_t lv_pm_id;

typedef void (*lv_pm_lifecycle)(void * pm_page);

typedef enum {
    LV_PM_TARGET_NEW = 0,
    LV_PM_TARGET_SELF = 1,
    LV_PM_TARGET_RESET = 2
} lv_pm_target;

typedef struct {
    const void * lv_pm_anima_cb;
    lv_pm_target target;
    uint32_t direction;
    uint32_t time;
} lv_pm_open_options_t;

struct lv_pm_page_def {
    lv_obj_t * page;
    lv_pm_id id;
    const char * name;
    lv_group_t * group;
    lv_obj_t * focus_obj;
    lv_timer_t * page_timer;
    lv_pm_lifecycle open_cb;
    lv_pm_lifecycle will_appear_cb;
    lv_pm_lifecycle dis_appear_cb;
    lv_pm_lifecycle will_disappear_cb;
    lv_pm_lifecycle dis_disappear_cb;
    lv_pm_lifecycle close_cb;
    lv_pm_open_options_t open_options;
    struct lv_pm_flag {
        uint32_t is_open : 1;
        uint32_t is_back : 1;
        uint32_t top_bar_en : 1;
        uint32_t back_bar_en : 1;
        uint32_t timer_backend_run : 1;
        uint32_t cache_enable : 1;
    } flag;
    lv_pm_page_theme_cb theme_changed_cb;
    void * theme_changed_ud;
    lv_pm_page_i18n_cb i18n_changed_cb;
    void * i18n_changed_ud;
    /**
     * Navigation payload pointer for the current open/close/jump.
     * - Owned by caller; lv_pm never lv_pm_free() this.
     * - Valid from will_appear/open through close_cb on that transition.
     * - Cleared by the framework after close_cb (or on page delete).
     * - Do not pass stack addresses; use NULL (LV_PM_MSG_NONE) when unused.
     */
    void * msg_data;
    void * user_data;
};

typedef struct lv_pm_page_def * lv_pm_page_t;

/** @brief 无导航载荷（应用中优先于裸 NULL）。 */
#define LV_PM_MSG_NONE ((void *)0)

/**
 * @brief lv_pm init。
 * @return 0 成功，负 errno 失败。
 */
uint8_t lv_pm_init(int page_num);

/**
 * @brief lv_pm create page。
 * @return 0 成功，负 errno 失败。
 */
lv_pm_page_t lv_pm_create_page(lv_pm_id id, const char * name);
/**
 * @brief lv_pm get pm page。
 */
lv_pm_page_t lv_pm_get_pm_page(void * lv_pm_page);
/**
 * @brief lv_pm get crr page。
 */
lv_pm_page_t lv_pm_get_crr_page(void);

void lv_pm_foreach_registered_page(void (*fn)(lv_pm_page_t page, void * user_data),
                                   void * user_data);

/**
 * @brief lv_pm delete page。
 */
void lv_pm_delete_page(lv_pm_id id);
/**
 * @brief lv_pm delete deep page。
 */
void lv_pm_delete_deep_page(lv_pm_id id);

/**
 * @brief lv_pm open page msg。
 * @return 0 成功，负 errno 失败。
 */
int lv_pm_open_page_msg(lv_pm_id id, void * msg_data);
/**
 * @brief lv_pm close page msg。
 */
int lv_pm_close_page_msg(void * msg_data);
/**
 * @brief lv_pm close to page msg。
 */
int lv_pm_close_to_page_msg(lv_pm_id target_id, void * msg_data);
/**
 * @brief lv_pm jump to page msg。
 */
int lv_pm_jump_to_page_msg(lv_pm_id target_id, void * msg_data);
/**
 * @brief lv_pm jump to page msg no animation。
 */
int lv_pm_jump_to_page_msg_no_animation(lv_pm_id target_id, void * msg_data);

/**
 * @brief lv_pm set group default。
 */
void lv_pm_set_group_default(lv_group_t * set_group);
/**
 * @brief lv_pm create group。
 * @return 0 成功，负 errno 失败。
 */
lv_group_t * lv_pm_create_group(void);

/**
 * @brief lv_pm nav busy。
 */
bool lv_pm_nav_busy(void);
/**
 * @brief lv_pm nav anim begin。
 */
void lv_pm_nav_anim_begin(void);
/**
 * @brief lv_pm nav anim end。
 */
void lv_pm_nav_anim_end(void);
/** @brief 结束挂起转场，以便立即执行新的 open/close。 */
void lv_pm_nav_drain(void);

typedef enum {
    LV_PM_NAV_EVT_APPEAR_DONE = 0,
    LV_PM_NAV_EVT_DISAPPEAR_DONE,
} lv_pm_nav_evt_t;

typedef void (*lv_pm_nav_notify_cb)(lv_pm_page_t page, lv_pm_nav_evt_t evt, void * user_data);

/**
 * @brief lv_pm set nav notify cb。
 */
void lv_pm_set_nav_notify_cb(lv_pm_nav_notify_cb cb, void * user_data);

/** @brief 仅在生命周期回调中读取 msg_data（close 后可能为 NULL）。 */
void * lv_pm_page_get_msg(void * pm_page);

/**
 * @brief printf_all_page 接口。
 */
void printf_all_page(void);

#include "lv_pm_bar.h"

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_CORE_H */
