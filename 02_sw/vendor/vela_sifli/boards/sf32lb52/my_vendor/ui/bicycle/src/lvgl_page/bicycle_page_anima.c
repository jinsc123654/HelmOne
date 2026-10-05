/**
 * @file bicycle_page_anima.c
 * @brief Central page transition / cache configuration for lv_pm.
 */
#include "bicycle_page_anima.h"

#include "lv_pm_anima.h"
#include "lv_pm_gpu_anima.h"
#include "lv_pm_port.h"

typedef struct {
    const void * anima_cb;
    uint32_t direction;
    uint32_t time_ms;
    bool cache_enable;
    bool top_bar_en;
    bool back_bar_en;
} bicycle_page_anima_row_t;

/*
 * Page transition table (edit here only).
 *
 * LiveMap / Menu: fade (opacity only). Do NOT use
 * lv_pm_gpu_rotate_slide_anima — full-page rotate+slide is too heavy on
 * this panel (jank with map canvas underneath).
 * Overlay pages may use GPU zoom. Boot still calls apply_silent() so the
 * empty shell does not animate under splash; map_will_appear restores this.
 */
static const bicycle_page_anima_row_t s_page_anima[BICYCLE_PM_ID_COUNT] = {
    [BICYCLE_PM_ID_MAP] = {
        .anima_cb = &lv_pm_fade_anima,
        .direction = 0,
        .time_ms = 220,
        .cache_enable = true,
        .top_bar_en = true,
        .back_bar_en = false,
    },
    [BICYCLE_PM_ID_USB_TRANSFER] = {
        .anima_cb = &lv_pm_gpu_zoom_anima,
        .direction = LV_PM_GPU_DIR_CENTER,
        .time_ms = 280,
        .cache_enable = false,
        .top_bar_en = false,
        .back_bar_en = false,
    },
    [BICYCLE_PM_ID_STARTUP] = {
        .anima_cb = &lv_pm_no_anima,
        .direction = 0,
        .time_ms = 0,
        .cache_enable = false,
        .top_bar_en = false,
        .back_bar_en = false,
    },
    [BICYCLE_PM_ID_MENU] = {
        /* ⚠ **两个方向都由本行决定**（用户 2026-09-26 晚："主界面进入菜单和菜单返回
         * 主界面，不加动画了"）：
         *   进 = 菜单 ENTER（`lv_pm_open_page_msg`）+ 主界面 EXIT 用的是"打开页的
         *        options"（`lv_pm_core.c` 里 `&open_page->open_options`）⇒ 也是本行；
         *   回 = 菜单 EXIT（`lv_pm_close_page_msg` 只让当前页播退出动画，底下的主界面
         *        直接露出来）⇒ 还是本行。
         * 所以这里换 `lv_pm_no_anima` + 0ms 就两个方向都瞬变，而且**不牵动主界面自己
         * 的过渡**（开机揭场、从 USB/工厂页回来仍走 `[BICYCLE_PM_ID_MAP]` 那行的淡入）。 */
        .anima_cb = &lv_pm_no_anima,
        .direction = 0,
        .time_ms = 0,
        .cache_enable = false,
        .top_bar_en = true,
        .back_bar_en = false,
    },
    [BICYCLE_PM_ID_FACTORY] = {
        .anima_cb = &lv_pm_gpu_zoom_anima,
        .direction = LV_PM_GPU_DIR_CENTER,
        .time_ms = 250,
        .cache_enable = false,
        .top_bar_en = false,
        .back_bar_en = false,
    },
};

/**
 * @brief 自行车 page anima apply。
 */
void bicycle_page_anima_apply(lv_pm_page_t page, bicycle_pm_id_t id)
{
    const bicycle_page_anima_row_t * row;

    if (page == NULL || id >= BICYCLE_PM_ID_COUNT) {
        return;
    }

    row = &s_page_anima[id];
    lv_pm_set_open_options(page, row->anima_cb, LV_PM_TARGET_NEW,
                           row->direction, row->time_ms);
    lv_pm_set_cache_enable(page, row->cache_enable);
    lv_pm_set_top_bar(page, row->top_bar_en);
    lv_pm_set_back_bar(page, row->back_bar_en);
}

/**
 * @brief 自行车 page anima apply silent。
 */
void bicycle_page_anima_apply_silent(lv_pm_page_t page)
{
    if (page == NULL) {
        return;
    }

    lv_pm_set_open_options(page, &lv_pm_no_anima, LV_PM_TARGET_NEW, 0, 0);
}
