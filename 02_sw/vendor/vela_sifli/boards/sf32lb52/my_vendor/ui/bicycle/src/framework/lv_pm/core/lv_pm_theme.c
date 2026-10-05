/**
 * @file lv_pm_theme.c
 * @brief lv_pm 页面管理 — theme。
 */

#include <string.h>

#include "../include/lv_pm_theme.h"
#include "../include/lv_pm_bar.h"
#include "../include/lv_pm_overlay.h"
#include "../include/lv_pm_core.h"
#include "../themes/lv_pm_themes.h"

static lv_pm_theme_id_t g_theme_id = LV_PM_THEME_ID_INVALID;
static lv_pm_theme_notify_cb g_theme_notify;
static void * g_theme_notify_ud;
/** @brief 待办的"页面级主题应用"（见 `lv_pm_theme_apply_all()` 的注释）。 */
static bool s_pages_pending;

/**
 * @brief lv_pm foreach registered page。
 */
void lv_pm_foreach_registered_page(void (*fn)(lv_pm_page_t page, void * user_data),
                                   void * user_data);

static const lv_pm_theme_def_t * theme_def_for_id(lv_pm_theme_id_t id)
{
    if ((unsigned)id >= lv_pm_builtin_theme_count) {
        return NULL;
    }

    return lv_pm_builtin_themes[id];
}

/**
 * @brief lv_pm theme count。
 * @return 请求的值。
 */
unsigned lv_pm_theme_count(void)
{
    return lv_pm_builtin_theme_count;
}

/**
 * @brief lv_pm theme name。
 */
const char * lv_pm_theme_name(lv_pm_theme_id_t id)
{
    const lv_pm_theme_def_t * def = theme_def_for_id(id);

    if (def == NULL || def->name == NULL) {
        return "";
    }

    return def->name;
}

/**
 * @brief lv_pm theme find。
 */
lv_pm_theme_id_t lv_pm_theme_find(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return LV_PM_THEME_ID_INVALID;
    }

    for (unsigned i = 0; i < lv_pm_builtin_theme_count; i++) {
        if (lv_pm_builtin_themes[i]->name != NULL &&
            strcmp(lv_pm_builtin_themes[i]->name, name) == 0) {
            return (lv_pm_theme_id_t)i;
        }
    }

    return LV_PM_THEME_ID_INVALID;
}

/**
 * @brief lv_pm theme current。
 */
const lv_pm_theme_def_t * lv_pm_theme_current(void)
{
    return theme_def_for_id(g_theme_id);
}

static void theme_invoke_page(lv_pm_page_t page, void * user_data)
{
    lv_pm_theme_id_t id = (lv_pm_theme_id_t)(uintptr_t)user_data;

    if (page == NULL || page->theme_changed_cb == NULL || page->page == NULL) {
        return;
    }

    page->theme_changed_cb(page, id, page->theme_changed_ud);
}

static void theme_apply_shell_visitor(lv_pm_page_t page, void * user_data)
{
    LV_UNUSED(user_data);

    if (page != NULL && page->page != NULL) {
        lv_pm_theme_apply_page_shell(page);
    }
}

/**
 * @brief lv_pm theme apply page shell。
 */
void lv_pm_theme_apply_page_shell(lv_pm_page_t page)
{
    const lv_pm_theme_def_t * def;

    if (page == NULL || page->page == NULL) {
        return;
    }

    def = lv_pm_theme_current();
    if (def == NULL) {
        return;
    }

    /* 带状态栏时不要铺满全屏，否则 appear 后主题会把标题顶到电池栏下面。 */
    lv_pm_page_layout_below_bars(page);
    lv_obj_set_style_outline_width(page->page, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(page->page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(page->page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(page->page, 0, LV_STATE_DEFAULT);

    lv_pm_theme_lvgl_attach_page(page->page);

    if (page->group != NULL) {
        lv_obj_t * focus = lv_group_get_focused(page->group);

        if (focus != NULL) {
            lv_pm_theme_lvgl_attach_focus(focus);
        }
    }
}

/**
 * @brief lv_pm page theme init。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_page_theme_init(lv_pm_page_t pm_page)
{
    lv_obj_t * scr;

    if (pm_page == NULL || pm_page->page == NULL) {
        return;
    }

    scr = lv_obj_get_screen(pm_page->page);

    lv_obj_set_style_outline_width(pm_page->page, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(pm_page->page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(pm_page->page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(pm_page->page, 0, LV_STATE_DEFAULT);

    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(scr, LV_OPA_0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_0, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);

    if (g_theme_id != LV_PM_THEME_ID_INVALID) {
        lv_pm_theme_apply_page_shell(pm_page);
    }
}

/**
 * @brief lv_pm set theme changed。
 */
int8_t lv_pm_set_theme_changed(void * page, lv_pm_page_theme_cb cb, void * user_data)
{
    lv_pm_page_t pm_page = lv_pm_get_pm_page(page);

    if (pm_page == NULL) {
        return -1;
    }

    pm_page->theme_changed_cb = cb;
    pm_page->theme_changed_ud = user_data;
    return 0;
}

/**
 * @brief lv_pm set theme notify cb。
 */
void lv_pm_set_theme_notify_cb(lv_pm_theme_notify_cb cb, void * user_data)
{
    g_theme_notify = cb;
    g_theme_notify_ud = user_data;
}

/**
 * @brief lv_pm theme get。
 * @return 请求的值。
 */
lv_pm_theme_id_t lv_pm_theme_get(void)
{
    return g_theme_id;
}

/**
 * @brief lv_pm theme apply。
 */
void lv_pm_theme_apply(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    if (page == NULL || g_theme_id == LV_PM_THEME_ID_INVALID) {
        return;
    }

    lv_pm_theme_apply_page_shell(page);
    theme_invoke_page(page, (void *)(uintptr_t)g_theme_id);
}

/**
 * @brief lv_pm theme apply all。
 */
void lv_pm_theme_apply_all(void)
{
    const lv_pm_theme_def_t * def;

    if (g_theme_id == LV_PM_THEME_ID_INVALID) {
        return;
    }

    def = lv_pm_theme_current();
    if (def != NULL) {
        lv_pm_theme_lvgl_bind(def);
        lv_pm_status_bar_apply_theme(def);
        lv_pm_overlay_apply_theme(def);
    }

    if (def != NULL && def->on_activate != NULL) {
        def->on_activate(g_theme_id, def->user_data);
    }

    if (g_theme_notify != NULL) {
        g_theme_notify(g_theme_id, g_theme_notify_ud);
    }

    /* ⚠ **页面级重活不在这里做**（2026-09-27，用户现场"点一下像要点两次"）：
     * 这两趟 foreach 会逐页 `lv_pm_page_layout_below_bars()` + 重贴页面壳样式 +
     * 跑页面自己的 theme_changed_cb ⇒ 实测 0.4~2.0 秒（同一动作方差极大，见
     * `[theme] click #N … cycle=…ms`）。它们跟"调色板换没换"无关，却排在同一个同步
     * 路径里 ⇒ 用户点一下要等它们跑完**屏幕上才动**。
     * ⇒ 这里只置待办，由 UI 主循环在循环顶补跑（同 lv_pm_theme_lvgl_bind 的整树重算）。
     *   可见顺序变成：先换色（快），页面壳/状态栏晚一帧 —— 最终取的是同一个主题色，
     *   所以肉眼无感。 */
    s_pages_pending = true;
}

/** @brief 循环顶补跑待办的主题工作：页面级应用 + 整树样式重算。
 *  @note UI 主循环每圈调一次；无待办时空操作。顺序：先页面（会设样式），
 *        再整树重算（顺带把改动过的区域标脏）。 */
void lv_pm_theme_poll_styles(void)
{
    if (s_pages_pending) {
        s_pages_pending = false;
        lv_pm_foreach_registered_page(theme_apply_shell_visitor, NULL);
        lv_pm_foreach_registered_page(theme_invoke_page,
                                      (void *)(uintptr_t)g_theme_id);
    }

    lv_pm_theme_report_styles_now();
}

/**
 * @brief lv_pm theme set。
 */
bool lv_pm_theme_set(lv_pm_theme_id_t id)
{
    if (theme_def_for_id(id) == NULL) {
        return false;
    }

    if (id == g_theme_id) {
        lv_pm_theme_apply_all();
        return true;
    }

    g_theme_id = id;
    lv_pm_theme_apply_all();
    return true;
}

/**
 * @brief lv_pm theme set by name。
 */
bool lv_pm_theme_set_by_name(const char * name)
{
    lv_pm_theme_id_t id = lv_pm_theme_find(name);

    if (id == LV_PM_THEME_ID_INVALID) {
        return false;
    }

    return lv_pm_theme_set(id);
}

/**
 * @brief lv_pm theme set next。
 */
bool lv_pm_theme_set_next(void)
{
    lv_pm_theme_id_t next;

    if (lv_pm_builtin_theme_count == 0u) {
        return false;
    }

    if (g_theme_id == LV_PM_THEME_ID_INVALID) {
        return lv_pm_theme_set(0);
    }

    next = (lv_pm_theme_id_t)(((unsigned)g_theme_id + 1u) % lv_pm_builtin_theme_count);
    return lv_pm_theme_set(next);
}
