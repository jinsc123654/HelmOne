/**
 * @file lv_pm_overlay.c
 * @brief lv_pm 页面管理 — overlay。
 */

#include "../include/lv_pm_overlay.h"
#include "../include/lv_pm_bar.h"
#include "../include/lv_pm_disp.h"

#include "helm_font.h"
#include "helm_icon.h"   /* 内置图标：系统通知的左侧图标，与 App 通知的 PNG 同一个槽 */

#include <unistd.h>

#ifndef LV_PM_NOTIFY_ANIM_MS
#define LV_PM_NOTIFY_ANIM_MS 320
#endif
#ifndef LV_PM_NOTIFY_DEFAULT_MS
#define LV_PM_NOTIFY_DEFAULT_MS 3200
#endif
#ifndef LV_PM_BOTTOM_DEFAULT_MS
#define LV_PM_BOTTOM_DEFAULT_MS 2600
#endif
#ifndef LV_PM_BOTTOM_Y_OFS
#define LV_PM_BOTTOM_Y_OFS (lv_pm_ver_res() / 5)
#endif
#ifndef LV_PM_OVERLAY_FONT_PX
#define LV_PM_OVERLAY_FONT_PX 14
#endif
#ifndef LV_PM_BOTTOM_CARD_MIN_H
#define LV_PM_BOTTOM_CARD_MIN_H 44
#endif
#ifndef LV_PM_OVERLAY_BG_OPA
#define LV_PM_OVERLAY_BG_OPA 200
#endif
#ifndef LV_PM_NOTIFY_CARD_MARGIN
#define LV_PM_NOTIFY_CARD_MARGIN 16
#endif
#ifndef LV_PM_BOTTOM_MAX_W
#define LV_PM_BOTTOM_MAX_W (lv_pm_hor_res() - 32)
#endif
#ifndef LV_PM_NOTIFY_GAP_BELOW_BAR
#define LV_PM_NOTIFY_GAP_BELOW_BAR 2
#endif
#ifndef LV_PM_NOTIFY_ICON_PX
#define LV_PM_NOTIFY_ICON_PX 32
#endif

#if LV_PM_USE_OVERLAY

typedef struct {
    lv_obj_t * card;
    lv_obj_t * icon;
    lv_obj_t * text_col;
    lv_obj_t * title;
    lv_obj_t * body;
    lv_timer_t * timer;
    lv_coord_t shown_y;
    lv_coord_t hidden_y;
    bool showing;
} pm_notify_t;

typedef struct {
    lv_obj_t * card;
    lv_obj_t * label;
    lv_timer_t * timer;
    bool showing;
} pm_bottom_t;

static pm_notify_t s_notify;
static pm_bottom_t s_bottom;

static void overlay_anim_y(void * var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static lv_font_t * overlay_font(void)
{
    /* 量产：任意通知走系统 TTF；工厂：只用 4bpp C 字模。 */
    return (lv_font_t *)helm_font_sys(LV_PM_OVERLAY_FONT_PX, helm_font_title());
}

static void overlay_style_label_fixed(lv_obj_t * lbl, lv_font_t * font)
{
    if (font != NULL) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
}

static void overlay_style_label_fit(lv_obj_t * lbl, lv_font_t * font)
{
    if (font != NULL) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, LV_PM_BOTTOM_MAX_W - 20, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
}

static lv_coord_t overlay_notify_card_w(void)
{
    return lv_pm_hor_res() - LV_PM_NOTIFY_CARD_MARGIN;
}

static void overlay_notify_layout_card(void)
{
    if (s_notify.card == NULL) {
        return;
    }

    lv_obj_set_width(s_notify.card, overlay_notify_card_w());
    lv_obj_add_flag(s_notify.card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_translate_x(s_notify.card, 0, 0);
    lv_obj_set_style_transform_pivot_x(s_notify.card, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_notify.card, 0, 0);
    lv_obj_update_layout(s_notify.card);
    lv_obj_align(s_notify.card, LV_ALIGN_TOP_MID, 0, 0);
}

static void overlay_style_notify_card(lv_obj_t * card)
{
    lv_font_t * font = overlay_font();

    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, overlay_notify_card_w());
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_PM_OVERLAY_BG_OPA, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 8, 0);
    if (font != NULL) {
        lv_obj_set_style_text_font(card, font, 0);
    }
}

static void overlay_style_bottom_card(lv_obj_t * card)
{
    lv_font_t * font = overlay_font();

    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_SIZE_CONTENT);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, LV_PM_BOTTOM_CARD_MIN_H, 0);
    lv_obj_set_style_max_width(card, LV_PM_BOTTOM_MAX_W, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_PM_OVERLAY_BG_OPA, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (font != NULL) {
        lv_obj_set_style_text_font(card, font, 0);
    }
}

static void overlay_apply_chrome(void)
{
    lv_font_t * font = overlay_font();

    if (s_notify.card != NULL) {
        overlay_style_notify_card(s_notify.card);
    }
    if (s_notify.title != NULL) {
        overlay_style_label_fixed(s_notify.title, font);
    }
    if (s_notify.body != NULL) {
        overlay_style_label_fixed(s_notify.body, font);
    }
    if (s_bottom.card != NULL) {
        overlay_style_bottom_card(s_bottom.card);
    }
    if (s_bottom.label != NULL) {
        overlay_style_label_fit(s_bottom.label, font);
    }
}

static void overlay_run_slide(lv_obj_t * card, lv_coord_t from_y, lv_coord_t to_y,
                              uint32_t time_ms, lv_anim_ready_cb_t ready_cb)
{
    lv_anim_t a;

    lv_anim_delete(card, overlay_anim_y);
    lv_obj_set_style_translate_y(card, from_y, 0);
    lv_obj_set_style_opa(card, LV_OPA_COVER, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_exec_cb(&a, overlay_anim_y);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (ready_cb != NULL) {
        lv_anim_set_ready_cb(&a, ready_cb);
    }
    lv_anim_start(&a);
}

static void overlay_raise_status_chrome(void)
{
#if LV_PM_USE_STA_BAR
    lv_obj_t * chrome = lv_pm_status_bar_cont();

    if (chrome != NULL && !lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(chrome);
    }
#endif
}

static void overlay_bring_top(lv_obj_t * card)
{
    if (card != NULL) {
        lv_obj_move_foreground(card);
    }
}

static void notify_hide_ready(lv_anim_t * a)
{
    lv_obj_t * card = (lv_obj_t *)a->var;

    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(card, s_notify.hidden_y, 0);
    lv_obj_set_style_opa(card, LV_OPA_COVER, 0);
    s_notify.showing = false;
}

static void bottom_hide_now(void)
{
    if (s_bottom.card == NULL) {
        return;
    }

    lv_anim_delete(s_bottom.card, overlay_anim_y);
    lv_obj_set_style_translate_y(s_bottom.card, 0, 0);
    lv_obj_set_style_opa(s_bottom.card, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_bottom.card, LV_OBJ_FLAG_HIDDEN);
    s_bottom.showing = false;
}

static void notify_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    lv_pm_notify_dismiss();
}

static void bottom_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    lv_pm_bottom_dismiss();
}

static void notify_stop_timer(void)
{
    if (s_notify.timer != NULL) {
        lv_timer_pause(s_notify.timer);
    }
}

static void bottom_stop_timer(void)
{
    if (s_bottom.timer != NULL) {
        lv_timer_pause(s_bottom.timer);
    }
}

static void notify_update_geometry(void)
{
    const lv_coord_t bar_h = lv_pm_status_bar_height();

    s_notify.shown_y = bar_h + LV_PM_NOTIFY_GAP_BELOW_BAR;
    s_notify.hidden_y = -(lv_coord_t)(lv_pm_ver_res() / 2);
}

/**
 * @brief lv_pm overlay apply theme。
 */
void lv_pm_overlay_apply_theme(const lv_pm_theme_def_t * def)
{
    LV_UNUSED(def);
    overlay_apply_chrome();
}

/**
 * @brief lv_pm overlay init。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_overlay_init(lv_obj_t * parent)
{
    if (parent == NULL || s_notify.card != NULL) {
        return;
    }

    notify_update_geometry();

    s_notify.card = lv_obj_create(parent);
    overlay_style_notify_card(s_notify.card);
    overlay_notify_layout_card();
    lv_obj_set_style_translate_y(s_notify.card, s_notify.hidden_y, 0);
    lv_obj_add_flag(s_notify.card, LV_OBJ_FLAG_HIDDEN);

    s_notify.icon = lv_img_create(s_notify.card);
    lv_obj_set_size(s_notify.icon, LV_PM_NOTIFY_ICON_PX, LV_PM_NOTIFY_ICON_PX);
    lv_obj_add_flag(s_notify.icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_notify.icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_notify.text_col = lv_obj_create(s_notify.card);
    lv_obj_remove_style_all(s_notify.text_col);
    lv_obj_set_flex_grow(s_notify.text_col, 1);
    lv_obj_set_width(s_notify.text_col, LV_PCT(100));
    lv_obj_set_height(s_notify.text_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_notify.text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_notify.text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_notify.text_col, 2, 0);
    lv_obj_clear_flag(s_notify.text_col, LV_OBJ_FLAG_SCROLLABLE);

    s_notify.title = lv_label_create(s_notify.text_col);
    lv_label_set_text(s_notify.title, "");
    /* ⚠ 宽度必须撑满文字列：标签默认"跟内容一样宽"，那时 text_align=CENTER 等于没效果
     * （用户 2026-09-27 实测：文字贴在图标右边、右侧一片空）。撑满后 *_show_ex() 里的
     * 对齐（App 通知左对齐 / 系统通知居中）才真正生效。 */
    lv_obj_set_width(s_notify.title, LV_PCT(100));
    overlay_style_label_fixed(s_notify.title, overlay_font());

    s_notify.body = lv_label_create(s_notify.text_col);
    lv_label_set_text(s_notify.body, "");
    lv_obj_set_width(s_notify.body, LV_PCT(100));
    overlay_style_label_fixed(s_notify.body, overlay_font());

    s_notify.timer = lv_timer_create(notify_timer_cb, LV_PM_NOTIFY_DEFAULT_MS, NULL);
    lv_timer_pause(s_notify.timer);

    s_bottom.card = lv_obj_create(parent);
    overlay_style_bottom_card(s_bottom.card);
    lv_obj_align(s_bottom.card, LV_ALIGN_CENTER, 0, LV_PM_BOTTOM_Y_OFS);
    lv_obj_add_flag(s_bottom.card, LV_OBJ_FLAG_HIDDEN);

    s_bottom.label = lv_label_create(s_bottom.card);
    lv_label_set_text(s_bottom.label, "");
    overlay_style_label_fit(s_bottom.label, overlay_font());

    s_bottom.timer = lv_timer_create(bottom_timer_cb, LV_PM_BOTTOM_DEFAULT_MS, NULL);
    lv_timer_pause(s_bottom.timer);

    overlay_raise_status_chrome();
}

/** @brief 本次通知要画的内置图标（0 = 无）+ 它建出来的对象（每次 show 重建）。 */
static int s_notify_ico;
static lv_obj_t * s_notify_ico_obj;

/**
 * @brief 按标题猜一个内置图标 —— 兜底用，见 `_ex` 里的说明。
 *
 * @details 全树有 20+ 个 `lv_pm_notify_show()` 调用点（菜单里的蓝牙/清除/删除/骑行…），
 *          一个个补图标参数既啰嗦又容易漏；所以**在通知层按标题关键词兜底**，新加的通知
 *          自动就有图标。要求精确时仍可用 `lv_pm_notify_show_icon()` 显式指定（优先级更高）。
 * @param title 通知标题（可为 NULL）。
 * @return `helm_ico_id_t` 取值；认不出来时给 `HELM_ICO_BELL`。
 */
static int notify_ico_for_title(const char * title)
{
    if (title == NULL) {
        return HELM_ICO_NONE;
    }
    if (strstr(title, "蓝牙") != NULL) {
        return HELM_ICO_BLE;
    }
    if (strstr(title, "亮度") != NULL) {
        return HELM_ICO_SUN;
    }
    if (strstr(title, "电量") != NULL || strstr(title, "充电") != NULL) {
        return HELM_ICO_BATT;
    }
    if (strstr(title, "导航") != NULL || strstr(title, "返航") != NULL) {
        return HELM_ICO_NAV;
    }
    if (strstr(title, "骑行") != NULL || strstr(title, "回放") != NULL) {
        return HELM_ICO_GPX;
    }
    if (strstr(title, "删除") != NULL || strstr(title, "清除") != NULL) {
        return HELM_ICO_XMARK;
    }
    if (strstr(title, "保存") != NULL || strstr(title, "记录") != NULL) {
        return HELM_ICO_SAVE;
    }
    if (strstr(title, "USB") != NULL) {
        return HELM_ICO_USB;
    }
    if (strstr(title, "手机") != NULL || strstr(title, "App") != NULL) {
        return HELM_ICO_CALL;
    }
    if (strstr(title, "固件") != NULL) {
        return HELM_ICO_CHIP;
    }

    return HELM_ICO_BELL;   /* 认不出来也给一个：有图标比没图标好 */
}

/**
 * @brief 同 lv_pm_notify_show，但左侧画一个**内置图标**（系统通知用）。
 *
 * @note 只设 `s_notify_ico`，真正的对象在 `lv_pm_notify_show_ex()` 里建（与 PNG 图标
 *       互斥、PNG 优先）；那边用完即把 `s_notify_ico` 清 0，所以**不会**被下一次
 *       普通 `lv_pm_notify_show()` 继承。
 */
bool lv_pm_notify_show_icon(const char * title, const char * text, int ico,
                            uint32_t duration_ms)
{
    s_notify_ico = ico;
    return lv_pm_notify_show_ex(title, text, NULL, duration_ms);
}

/**
 * @brief lv_pm notify show。
 */
bool lv_pm_notify_show(const char * title, const char * text, uint32_t duration_ms)
{
    return lv_pm_notify_show_ex(title, text, NULL, duration_ms);
}

/**
 * @brief lv_pm notify show ex。
 */
bool lv_pm_notify_show_ex(const char * title, const char * text,
                          const char * icon_path, uint32_t duration_ms)
{
    bool have_icon = false;
    bool icon_builtin = false;   /* 内置图标（系统通知）与 App 的 PNG 图标对齐策略不同 */

    if (s_notify.card == NULL || text == NULL) {
        return false;
    }

    notify_update_geometry();
    overlay_apply_chrome();

    if (icon_path != NULL && icon_path[0] != '\0' && s_notify.icon != NULL &&
        access(icon_path, R_OK) == 0) {
        lv_img_set_src(s_notify.icon, icon_path);
        lv_obj_clear_flag(s_notify.icon, LV_OBJ_FLAG_HIDDEN);
        have_icon = true;
    } else if (s_notify.icon != NULL) {
        lv_obj_add_flag(s_notify.icon, LV_OBJ_FLAG_HIDDEN);
    }

    /* 内置图标（系统通知）：PNG 优先；没显式指定就按标题兜底（覆盖全树 20+ 个调用点） */
    if (s_notify_ico == 0) {
        s_notify_ico = notify_ico_for_title(title);
    }
    if (s_notify_ico_obj != NULL) {
        lv_obj_del(s_notify_ico_obj);
        s_notify_ico_obj = NULL;
    }
    if (!have_icon && s_notify_ico > 0 && s_notify.card != NULL) {
        s_notify_ico_obj = helm_icon_create(s_notify.card, (helm_ico_id_t)s_notify_ico,
                                            LV_PM_NOTIFY_ICON_PX);
        if (s_notify_ico_obj != NULL) {
            /* ⚠ 内置图标**脱离布局**（FLOATING）自己贴左：这样文字列能占满整条横幅，
             * "居中"才是相对**整条通知**居中（用户 2026-09-27 明确要求）——若让图标留在
             * 布局里，文字只能在图标右边那块区域里居中，看着偏左。 */
            lv_obj_add_flag(s_notify_ico_obj, LV_OBJ_FLAG_FLOATING);
            lv_obj_align(s_notify_ico_obj, LV_ALIGN_LEFT_MID, 10, 0);
            have_icon = true;
            icon_builtin = true;
        }
    }
    s_notify_ico = 0;   /* 用完即清：普通 show() 不该继承上次的内置图标 */

    /*
     * 对齐（用户 2026-09-27）：**App 通知（手机下发的 PNG 图标）保持左对齐**（手机那边
     * 就是这么排的）；**系统通知的内置图标则让文字居中** —— 左边一个图标配左对齐文字
     * 看着突兀。没有图标时本来就居中。
     */
    {
        const lv_text_align_t al = (have_icon && !icon_builtin)
                                   ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER;

        if (s_notify.title != NULL) {
            lv_obj_set_style_text_align(s_notify.title, al, 0);
        }
        if (s_notify.body != NULL) {
            lv_obj_set_style_text_align(s_notify.body, al, 0);
        }

        /*
         * ⚠ 内置图标是 FLOATING（不占布局）⇒ 长文字居中时会钻到图标底下（用户 2026-09-27
         * 追问的正是这个）。这里给文字列加**对称**内边距，宽度 = 图标那一条（左 10 间距 +
         * 32 图标 + 8 余量）：既保证文字永不压到图标，又因为左右相等 ⇒ 居中依然是相对
         * **整条横幅**居中（不对称的话就会变成"图标右侧那块居中"，正是上一版被否掉的）。
         */
        if (s_notify.text_col != NULL) {
            lv_obj_set_style_pad_hor(s_notify.text_col,
                icon_builtin ? (10 + LV_PM_NOTIFY_ICON_PX + 8) : 0, 0);
        }
    }

    if (title != NULL && title[0] != '\0') {
        lv_label_set_text(s_notify.title, title);
        lv_obj_clear_flag(s_notify.title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_notify.title, "");
        lv_obj_add_flag(s_notify.title, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_notify.body, text);
    lv_obj_update_layout(s_notify.card);
    overlay_notify_layout_card();

    notify_stop_timer();
    lv_obj_clear_flag(s_notify.card, LV_OBJ_FLAG_HIDDEN);
    overlay_bring_top(s_notify.card);
    overlay_raise_status_chrome();

    overlay_run_slide(s_notify.card, s_notify.hidden_y, s_notify.shown_y,
                      LV_PM_NOTIFY_ANIM_MS, NULL);
    s_notify.showing = true;

    if (duration_ms == 0) {
        duration_ms = LV_PM_NOTIFY_DEFAULT_MS;
    }

    lv_timer_set_period(s_notify.timer, duration_ms);
    lv_timer_reset(s_notify.timer);
    lv_timer_resume(s_notify.timer);
    overlay_raise_status_chrome();
    return true;
}

/**
 * @brief lv_pm notify dismiss。
 */
void lv_pm_notify_dismiss(void)
{
    if (s_notify.card == NULL || !s_notify.showing) {
        return;
    }

    notify_stop_timer();
    s_notify.showing = false;
    overlay_run_slide(s_notify.card, lv_obj_get_style_translate_y(s_notify.card, 0),
                      s_notify.hidden_y, LV_PM_NOTIFY_ANIM_MS, notify_hide_ready);
}

bool lv_pm_notify_is_showing(void)
{
    return s_notify.showing;
}

/**
 * @brief lv_pm bottom show。
 */
bool lv_pm_bottom_show(const char * text, uint32_t duration_ms)
{
    if (s_bottom.card == NULL || text == NULL) {
        return false;
    }

    overlay_apply_chrome();
    lv_label_set_text(s_bottom.label, text);
    lv_obj_update_layout(s_bottom.card);
    lv_obj_align(s_bottom.card, LV_ALIGN_CENTER, 0, LV_PM_BOTTOM_Y_OFS);

    bottom_stop_timer();
    lv_anim_delete(s_bottom.card, overlay_anim_y);
    lv_obj_set_style_translate_y(s_bottom.card, 0, 0);
    lv_obj_set_style_opa(s_bottom.card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_bottom.card, LV_OBJ_FLAG_HIDDEN);
    overlay_bring_top(s_bottom.card);
    s_bottom.showing = true;

    if (duration_ms == 0) {
        duration_ms = LV_PM_BOTTOM_DEFAULT_MS;
    }

    lv_timer_set_period(s_bottom.timer, duration_ms);
    lv_timer_reset(s_bottom.timer);
    lv_timer_resume(s_bottom.timer);
    return true;
}

/**
 * @brief lv_pm bottom dismiss。
 */
void lv_pm_bottom_dismiss(void)
{
    if (s_bottom.card == NULL || !s_bottom.showing) {
        return;
    }

    bottom_stop_timer();
    bottom_hide_now();
}

#endif /* LV_PM_USE_OVERLAY */
