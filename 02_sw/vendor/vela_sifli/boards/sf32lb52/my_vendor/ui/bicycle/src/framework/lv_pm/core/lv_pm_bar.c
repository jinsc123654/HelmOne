/**
 * @file lv_pm_bar.c
 * @brief lv_pm 页面管理 — bar。
 */

#include <stdint.h>

#include "../include/lv_pm_bar.h"
#include "../include/lv_pm_config.h"
#include "../include/lv_pm_core.h"
#include "../include/lv_pm_theme_def.h"

#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* ======================== Color helpers (always available) ======================== */

static inline uint8_t pm_color_lightness(lv_color_t c)
{
    return (uint8_t)(0.299f * c.red + 0.587f * c.green + 0.114f * c.blue);
}

static inline uint8_t pm_clamp_u8(int val)
{
    if (val < 0) {
        return 0;
    }
    if (val > 255) {
        return 255;
    }
    return (uint8_t)val;
}

/**
 * @brief lv_color_derive_statusbar_auto 接口。
 */
lv_color_t lv_color_derive_statusbar_auto(lv_color_t bg_color)
{
    const int cold = (bg_color.blue >= bg_color.red) ? 1 : 0;
    int r = bg_color.red;
    int g = bg_color.green;
    int b = bg_color.blue;
    int dr = cold ? 6 : 8;
    int dg = 6;
    int db = cold ? 8 : 6;

    if (pm_color_lightness(bg_color) < 128) {
        r = pm_clamp_u8(r + dr);
        g = pm_clamp_u8(g + dg);
        b = pm_clamp_u8(b + db);
    }
    else {
        r = pm_clamp_u8(r - dr);
        g = pm_clamp_u8(g - dg);
        b = pm_clamp_u8(b - db);
    }

    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/**
 * @brief lv_color_derive_backbar_auto 接口。
 */
lv_color_t lv_color_derive_backbar_auto(lv_color_t bg)
{
    if (pm_color_lightness(bg) < 128) {
        return lv_color_make(230, 230, 230);
    }
    return lv_color_make(30, 30, 30);
}

#if LV_PM_USE_STA_BAR || LV_PM_USE_BACK_BAR

typedef struct {
    lv_obj_t * root;
    lv_coord_t hidden_y;
    lv_coord_t shown_y;
    uint32_t anim_ms;
} pm_bar_slide_t;

static void pm_bar_slide_exec(void * var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void pm_bar_slide_move(pm_bar_slide_t * slide, lv_coord_t target_y, bool animate)
{
    if (slide == NULL || slide->root == NULL) {
        return;
    }

    lv_obj_clear_flag(slide->root, LV_OBJ_FLAG_HIDDEN);

    if (animate && slide->anim_ms > 0u) {
        lv_anim_t anim;

        lv_anim_init(&anim);
        lv_anim_set_var(&anim, slide->root);
        lv_anim_set_values(&anim, lv_obj_get_y_aligned(slide->root), target_y);
        lv_anim_set_time(&anim, slide->anim_ms);
        lv_anim_set_path_cb(&anim, target_y == slide->shown_y ?
                                         lv_anim_path_ease_out : lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&anim, pm_bar_slide_exec);
        lv_anim_start(&anim);
        return;
    }

    lv_obj_set_y(slide->root, target_y);
}

static bool pm_bar_slide_is_hidden(const pm_bar_slide_t * slide)
{
    if (slide == NULL || slide->root == NULL) {
        return true;
    }
    return lv_obj_get_y_aligned(slide->root) == slide->hidden_y;
}

static lv_color_t pm_page_bg_color(lv_pm_page_t page)
{
    if (page == NULL || page->page == NULL) {
        return lv_color_black();
    }
    return lv_obj_get_style_bg_color(page->page, 0);
}

#endif /* LV_PM_USE_STA_BAR || LV_PM_USE_BACK_BAR */

void lv_pm_page_layout_below_bars(lv_pm_page_t page)
{
    lv_obj_t * obj;
    lv_obj_t * scr;
    lv_coord_t bar_h = 0;

    if (page == NULL || page->page == NULL) {
        return;
    }

    obj = page->page;
    scr = lv_obj_get_screen(obj);
    if (scr == NULL) {
        return;
    }

#if LV_PM_USE_STA_BAR
    if (page->flag.top_bar_en) {
        bar_h = lv_pm_status_bar_height();
    }
#endif
    lv_obj_set_size(obj, lv_obj_get_width(scr),
        lv_obj_get_height(scr) - bar_h);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, bar_h);
}

#if !LV_PM_USE_STA_BAR && !LV_PM_USE_BACK_BAR

/**
 * @brief lv_pm bars init。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_bars_init(lv_obj_t * parent)
{
    LV_UNUSED(parent);
}

/**
 * @brief lv_pm bar apply for page。
 */
void lv_pm_bar_apply_for_page(lv_pm_page_t page)
{
    LV_UNUSED(page);
}

#else

#if LV_PM_USE_STA_BAR

static lv_obj_t * s_status_main;
static lv_obj_t * s_status_chrome;
static lv_obj_t * s_status_panel;
static lv_obj_t * s_status_bg;
static lv_coord_t s_status_h;
static lv_group_t * s_status_group;
static lv_group_t * s_status_prev_group;
static pm_bar_slide_t s_status_slide;

static void status_bar_collapsed_style(void)
{
    if (s_status_chrome == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(s_status_chrome, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status_chrome, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_status_chrome, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_chrome, 0, 0);
    lv_obj_set_style_pad_all(s_status_chrome, 0, 0);
    lv_obj_set_style_radius(s_status_chrome, 0, 0);
}

/**
 * @brief lv_pm status bar apply theme。
 */
void lv_pm_status_bar_apply_theme(const lv_pm_theme_def_t * def)
{
    if (def == NULL) {
        return;
    }

    status_bar_collapsed_style();

    if (s_status_bg != NULL) {
        lv_obj_set_style_bg_color(s_status_bg, def->colors.status_bg, 0);
        lv_obj_set_style_bg_opa(s_status_bg, def->colors.status_bg_opa, 0);
    }
}

static void status_bar_opa_cb(void * var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void status_bar_scroll_cb(lv_event_t * event)
{
    lv_obj_t * scroller = lv_event_get_target(event);
    lv_obj_t * content = lv_event_get_user_data(event);
    lv_pm_page_t page = lv_pm_get_crr_page();
    const lv_coord_t scroll_y = LV_VER_RES - lv_obj_get_scroll_y(scroller);

    if (scroll_y == 0) {
        status_bar_collapsed_style();
        if (page != NULL && page->flag.top_bar_en == 0 &&
            !pm_bar_slide_is_hidden(&s_status_slide)) {
            pm_bar_slide_move(&s_status_slide, s_status_slide.hidden_y, true);
        }
        if (s_status_prev_group != NULL &&
            lv_group_get_default() == s_status_group) {
            lv_pm_set_group_default(s_status_prev_group);
            s_status_prev_group = NULL;
        }
        return;
    }

    if (scroll_y < LV_VER_RES) {
        const float span = (float)(LV_VER_RES - s_status_h);
        float ratio = (float)(scroll_y - s_status_h) / span * 4.0f;

        if (ratio < 0.0f) {
            ratio = 0.0f;
        }
        if (ratio > 1.0f) {
            ratio = 1.0f;
        }

        const lv_opa_t opa = (lv_opa_t)(255.0f - ratio * 255.0f);

        if (opa > 240) {
            lv_obj_clear_flag(content, LV_OBJ_FLAG_HIDDEN);
        }
        else if (opa < 15) {
            lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_opa(content, opa, 0);
        return;
    }

    s_status_prev_group = lv_group_get_default();
    lv_pm_set_group_default(s_status_group);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_HIDDEN);
#if LV_PM_USE_ANMI_STA
    {
        lv_anim_t anim;

        lv_anim_init(&anim);
        lv_anim_set_var(&anim, content);
        lv_anim_set_values(&anim, lv_obj_get_style_opa(content, 0), LV_OPA_COVER);
        lv_anim_set_time(&anim, 300);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, status_bar_opa_cb);
        lv_anim_start(&anim);
    }
#else
    lv_obj_set_style_opa(content, LV_OPA_COVER, 0);
#endif
}

static void status_bar_create(lv_obj_t * parent)
{
    lv_obj_t * ctrl;
    lv_obj_t * placeholder;

    s_status_h = LV_HOR_RES / 20;

    s_status_slide.root = lv_obj_create(parent);
    s_status_slide.hidden_y = -s_status_h;
    s_status_slide.shown_y = 0;
#if LV_PM_USE_ANMI_STA
    s_status_slide.anim_ms = 500;
#else
    s_status_slide.anim_ms = 0;
#endif

    s_status_main = s_status_slide.root;
    lv_obj_clear_flag(s_status_main, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_style_all(s_status_main);
    lv_obj_set_size(s_status_main, lv_pct(100), lv_pct(100));
    lv_obj_set_scroll_snap_y(s_status_main, LV_SCROLL_SNAP_CENTER);
    lv_obj_align(s_status_main, LV_ALIGN_CENTER, 0, 0);

    s_status_bg = lv_obj_create(s_status_main);
    lv_obj_set_scrollbar_mode(s_status_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(s_status_bg, LV_HOR_RES, LV_VER_RES + s_status_h * 2);
    lv_obj_set_y(s_status_bg, -s_status_h);
    lv_obj_set_style_bg_color(s_status_bg, lv_color_make(0x28, 0x2b, 0x30), 0);
    lv_obj_set_style_bg_opa(s_status_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_bg, 0, 0);
    lv_obj_set_style_pad_all(s_status_bg, 0, 0);

    ctrl = lv_obj_create(s_status_main);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_size(ctrl, lv_pct(100), LV_VER_RES);
    lv_obj_set_y(ctrl, 0);

    placeholder = lv_obj_create(s_status_main);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_TRANSP, 0);
    lv_obj_remove_style_all(placeholder);
    lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(placeholder, lv_pct(100), LV_VER_RES - 2 * s_status_h);
    lv_obj_set_y(placeholder, LV_VER_RES + s_status_h);
    lv_obj_scroll_to_view(placeholder, LV_ANIM_OFF);

    pm_bar_slide_move(&s_status_slide, s_status_slide.hidden_y, false);

    s_status_group = lv_group_create();
    lv_pm_set_group_default(s_status_group);

    s_status_panel = lv_obj_create(s_status_bg);
    lv_obj_clear_flag(s_status_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style_all(s_status_panel);
    lv_obj_set_size(s_status_panel, lv_pct(100), LV_VER_RES);
    lv_obj_set_style_bg_opa(s_status_panel, LV_OPA_TRANSP, 0);
    lv_obj_align(s_status_panel, LV_ALIGN_TOP_MID, 0, s_status_h);

    lv_obj_add_event_cb(s_status_main, status_bar_scroll_cb, LV_EVENT_SCROLL, s_status_panel);

    /* Collapsed strip: fixed on layer_top, above pull-down scroller — always black. */
    s_status_chrome = lv_obj_create(parent);
    lv_obj_clear_flag(s_status_chrome, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style_all(s_status_chrome);
    lv_obj_set_size(s_status_chrome, LV_HOR_RES, s_status_h);
    lv_obj_align(s_status_chrome, LV_ALIGN_TOP_MID, 0, 0);
    status_bar_collapsed_style();
    lv_obj_add_flag(s_status_chrome, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief lv_pm status bar main。
 */
lv_obj_t * lv_pm_status_bar_main(void) { return s_status_main; }
/**
 * @brief lv_pm status bar cont。
 */
lv_obj_t * lv_pm_status_bar_cont(void) { return s_status_chrome; }
/**
 * @brief lv_pm status bar panel。
 */
lv_obj_t * lv_pm_status_bar_panel(void) { return s_status_panel; }
/**
 * @brief lv_pm status bar bg。
 */
lv_obj_t * lv_pm_status_bar_bg(void) { return s_status_bg; }
/**
 * @brief lv_pm status bar height。
 */
lv_coord_t lv_pm_status_bar_height(void) { return s_status_h; }

/**
 * @brief lv_pm status bar set height。
 */
void lv_pm_status_bar_set_height(lv_coord_t height)
{
    lv_obj_t * placeholder;

    if (height <= 0 || s_status_main == NULL || s_status_chrome == NULL) {
        return;
    }

    s_status_h = height;
    s_status_slide.hidden_y = -s_status_h;
    lv_obj_set_height(s_status_chrome, s_status_h);

    if (s_status_bg != NULL) {
        lv_obj_set_height(s_status_bg, LV_VER_RES + s_status_h * 2);
        lv_obj_set_y(s_status_bg, -s_status_h);
    }

    if (pm_bar_slide_is_hidden(&s_status_slide)) {
        pm_bar_slide_move(&s_status_slide, s_status_slide.hidden_y, false);
    }

    /* main children: bg(0), ctrl(1), placeholder(2) */
    placeholder = lv_obj_get_child(s_status_main, 2);
    if (placeholder != NULL) {
        lv_obj_set_size(placeholder, lv_pct(100), LV_VER_RES - 2 * s_status_h);
        lv_obj_set_y(placeholder, LV_VER_RES + s_status_h);
    }

    lv_obj_update_layout(s_status_main);
}

static void status_bar_apply(lv_pm_page_t page, lv_color_t bg)
{
    LV_UNUSED(bg);

    status_bar_collapsed_style();

    if (page->flag.top_bar_en) {
        if (pm_bar_slide_is_hidden(&s_status_slide)) {
            pm_bar_slide_move(&s_status_slide, s_status_slide.shown_y, true);
        }
        if (s_status_chrome != NULL) {
            lv_obj_clear_flag(s_status_chrome, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_status_chrome);
        }
    }
    else {
        if (s_status_chrome != NULL) {
            lv_obj_add_flag(s_status_chrome, LV_OBJ_FLAG_HIDDEN);
        }
        if (!pm_bar_slide_is_hidden(&s_status_slide)) {
            pm_bar_slide_move(&s_status_slide, s_status_slide.hidden_y, true);
        }
    }
}

#else

static void status_bar_create(lv_obj_t * parent) { LV_UNUSED(parent); }
static void status_bar_apply(lv_pm_page_t page, lv_color_t bg)
{
    LV_UNUSED(page);
    LV_UNUSED(bg);
}

#endif /* LV_PM_USE_STA_BAR */

#if LV_PM_USE_BACK_BAR

#define BACK_BAR_WIDTH      (LV_HOR_RES / 3)
#define BACK_BAR_HEIGHT     (LV_VER_RES / 2)
#define BACK_BAR_HANDLE_H   20
#define BACK_BAR_PULL_CLOSE 10

static lv_obj_t * s_back_main;
static lv_obj_t * s_back_handle;
static pm_bar_slide_t s_back_slide;

static void back_bar_close_page(void)
{
    lv_pm_close_page_msg(NULL);
}

static void back_bar_scroll_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * scroller = lv_event_get_target(e);

    if (code == LV_EVENT_SCROLL) {
        const lv_coord_t y = lv_obj_get_scroll_y(scroller);

        if (y > BACK_BAR_PULL_CLOSE) {
            lv_obj_set_scroll_dir(scroller, LV_DIR_NONE);
        }
    }
    else if (code == LV_EVENT_SCROLL_END) {
        const lv_coord_t y = lv_obj_get_scroll_y(scroller);

        lv_obj_set_scroll_dir(scroller, LV_DIR_BOTTOM);
        if (y >= BACK_BAR_PULL_CLOSE) {
            back_bar_close_page();
        }
        else {
            lv_obj_scroll_to_y(scroller, 0, LV_ANIM_ON);
        }
    }
}

static void back_bar_pressed_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    back_bar_close_page();
}

static void back_bar_create(lv_obj_t * parent)
{
    lv_obj_t * bg;
    lv_obj_t * placeholder;
    static lv_style_t handle_style;

    s_back_slide.root = lv_obj_create(parent);
    s_back_slide.hidden_y = 40;
    s_back_slide.shown_y = 5;
#if LV_PM_USE_ANMI_BACK
    s_back_slide.anim_ms = 800;
#else
    s_back_slide.anim_ms = 0;
#endif

    s_back_main = s_back_slide.root;
    lv_obj_clear_flag(s_back_main, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_style_all(s_back_main);
    lv_obj_set_size(s_back_main, BACK_BAR_WIDTH, BACK_BAR_HEIGHT);
    lv_obj_set_scroll_snap_y(s_back_main, LV_SCROLL_SNAP_CENTER);
    lv_obj_align(s_back_main, LV_ALIGN_BOTTOM_MID, 0, 0);

    bg = lv_obj_create(s_back_main);
    lv_obj_set_style_bg_opa(bg, LV_OPA_TRANSP, 0);
    lv_obj_set_size(bg, BACK_BAR_WIDTH, BACK_BAR_HEIGHT + BACK_BAR_HANDLE_H * 2);
    lv_obj_set_y(bg, BACK_BAR_HEIGHT - BACK_BAR_HANDLE_H);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);

    placeholder = lv_obj_create(s_back_main);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_TRANSP, 0);
    lv_obj_remove_style_all(placeholder);
    lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(placeholder, lv_pct(100), BACK_BAR_HEIGHT - 2 * BACK_BAR_HANDLE_H);
    lv_obj_set_y(placeholder, BACK_BAR_HANDLE_H);

    lv_style_init(&handle_style);
    lv_style_set_bg_opa(&handle_style, LV_OPA_COVER);
    lv_style_set_bg_color(&handle_style, lv_color_white());
    lv_style_set_radius(&handle_style, 5);

    s_back_handle = lv_obj_create(bg);
    lv_obj_remove_style_all(s_back_handle);
    lv_obj_add_style(s_back_handle, &handle_style, LV_PART_MAIN);
    lv_obj_set_size(s_back_handle, 80, 3);
    lv_obj_align(s_back_handle, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_clear_flag(s_back_handle, LV_OBJ_FLAG_SCROLL_CHAIN);

    pm_bar_slide_move(&s_back_slide, s_back_slide.hidden_y, false);

    lv_obj_add_event_cb(s_back_main, back_bar_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_back_main, back_bar_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(s_back_handle, back_bar_pressed_cb, LV_EVENT_RELEASED, NULL);
}

/**
 * @brief lv_pm back bar main。
 */
lv_obj_t * lv_pm_back_bar_main(void) { return s_back_main; }
/**
 * @brief lv_pm back bar handle。
 */
lv_obj_t * lv_pm_back_bar_handle(void) { return s_back_handle; }

static void back_bar_apply(lv_pm_page_t page, lv_color_t bg)
{
    if (page->flag.back_bar_en) {
        if (pm_bar_slide_is_hidden(&s_back_slide)) {
            pm_bar_slide_move(&s_back_slide, s_back_slide.shown_y, true);
        }
        if (s_back_handle != NULL) {
            lv_obj_set_style_bg_color(s_back_handle, lv_color_derive_backbar_auto(bg), 0);
        }
    }
    else if (!pm_bar_slide_is_hidden(&s_back_slide)) {
        pm_bar_slide_move(&s_back_slide, s_back_slide.hidden_y, true);
    }
}

#else

static void back_bar_create(lv_obj_t * parent) { LV_UNUSED(parent); }
static void back_bar_apply(lv_pm_page_t page, lv_color_t bg)
{
    LV_UNUSED(page);
    LV_UNUSED(bg);
}

#endif /* LV_PM_USE_BACK_BAR */

void lv_pm_bars_init(lv_obj_t * parent)
{
#if LV_PM_USE_BACK_BAR
    back_bar_create(parent);
#endif
#if LV_PM_USE_STA_BAR
    status_bar_create(parent);
#endif
}

/**
 * @brief lv_pm bar apply for page。
 */
void lv_pm_bar_apply_for_page(lv_pm_page_t page)
{
    lv_color_t bg;

    if (page == NULL) {
        return;
    }

    bg = pm_page_bg_color(page);
#if LV_PM_USE_STA_BAR
    status_bar_apply(page, bg);
#endif
#if LV_PM_USE_BACK_BAR
    back_bar_apply(page, bg);
#endif
}

#endif /* any bar enabled */
