/**
 * @file helm_widget.c
 * @brief 线稿控件：尺寸对齐 helm.css（mhead 32 / 行高 4.2 / 圆角 6）。
 */

#include "helm_widget.h"

#include "helm_font.h"
#include "helm_palette.h"
#include "lvgl/src/core/lv_obj_private.h"
#include "lvgl/src/draw/lv_draw_rect.h"

#include <stdint.h>
#include <string.h>

#define HELM_ENTER_MS    180
#define HELM_ENTER_PX    36
#define HELM_PRESS_MS    70
#define HELM_PRESS_PX    5
#define HELM_SQUASH_MS   80
#define HELM_SQUASH_H    (-8)
#define HELM_STAGGER_MS  200
#define HELM_STAGGER_PX  12
#define HELM_STAGGER_GAP 40
#define HELM_STAGGER_MAX 6
#define HELM_MSW_TAG     ((void *)(uintptr_t)0x48535731u)
/** @brief 次级文字标记（副标题/右值）：选中或深底时要翻成浅色，得先能认出它。 */
#define HELM_MLAB_TAG    ((void *)(uintptr_t)0x48534C31u)
#define HELM_MSW_W       34
#define HELM_MSW_H       18
#define HELM_MCUR_TAG    ((void *)(uintptr_t)0x484D4331u)
#define HELM_CUR_MS      16
#define HELM_CUR_SPEED   4
#define HELM_CUR_SQUEEZE 8

/**
 * @brief 图标按"域"取色：导航橙 / 连接青 / 数据·系统棕 / 危险红。
 * @details 原来一律 INK（黑），所有行的图标长得一样、扫读时没有区分度；
 *          用户 2026-09-26 定的现代化方案里这一条是"按域给色"。
 *          颜色只落在**形状**（图标）上，文字仍是墨色/次级灰，符合用色规则。
 */
static uint32_t helm_ico_tone(helm_ico_id_t ico)
{
    switch (ico) {
    case HELM_ICO_POWER:
    case HELM_ICO_XMARK:
        return HELM_COLOR_HR;
    case HELM_ICO_NAV:
    case HELM_ICO_PIN:
    case HELM_ICO_FLAG:
    case HELM_ICO_SKIP:
    case HELM_ICO_GO:
        return HELM_COLOR_NAV;
    case HELM_ICO_BLE:
    case HELM_ICO_USB:
    case HELM_ICO_BELL:
    case HELM_ICO_SOUND:
    case HELM_ICO_CALL:
    case HELM_ICO_INBOX:
        return HELM_COLOR_CLIMB;
    default:
        return HELM_COLOR_PWR;
    }
}

/** @brief 选中/深底上的行图标：本域色调的**浅色版**。
 *  @details 用户 2026-09-26：「可是 icon 是彩色的啊」—— 所以不上白，改成**同色相提亮**，
 *           保住"每项图标一色"。实测在选中蓝 0x1D4ED8 上：原色 1.12~1.83:1（等于看不见），
 *           浅色版 3.27~4.51:1（图形按 3:1 判，够；4.5:1 是文字的门槛）。 */
static uint32_t helm_ico_tone_sel(helm_ico_id_t ico)
{
    switch (ico) {
    case HELM_ICO_POWER:
    case HELM_ICO_XMARK:
        return HELM_COLOR_HR_FILL;
    case HELM_ICO_NAV:
    case HELM_ICO_PIN:
    case HELM_ICO_FLAG:
    case HELM_ICO_SKIP:
    case HELM_ICO_GO:
        return HELM_COLOR_NAV_FILL;
    case HELM_ICO_BLE:
    case HELM_ICO_USB:
    case HELM_ICO_BELL:
    case HELM_ICO_SOUND:
    case HELM_ICO_CALL:
    case HELM_ICO_INBOX:
        return HELM_COLOR_CLIMB_FILL;
    default:
        return HELM_COLOR_PWR_FILL;
    }
}

static void helm_mitem_apply_mix(lv_obj_t * row, uint8_t mix);

lv_obj_t * helm_label(lv_obj_t * parent, const lv_font_t * font, uint32_t color,
                      const char * txt)
{
    lv_obj_t * l = lv_label_create(parent);

    if (font) {
        lv_obj_set_style_text_font(l, font, 0);
    }

    lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(l, helm_color(color), 0);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

void helm_style_paper(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, helm_color(HELM_COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void helm_style_scr(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, helm_color(HELM_COLOR_SCR), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

void helm_style_card(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, helm_color(HELM_COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, HELM_RADIUS, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void helm_text_sel(lv_obj_t * obj, bool sel, const lv_font_t * font)
{
    if (obj == NULL) {
        return;
    }

    LV_UNUSED(sel);
    lv_obj_set_style_text_color(obj, helm_color(HELM_COLOR_INK), 0);
    if (font) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

/** @brief 页头条底色。
 *  @details 日光是"黑条 + 米白字"：INK 当条底、PAPER 当字。夜间 INK 翻成**正文白**
 *           （黑底上的字）⇒ 照搬就会画出一条**白条**，与设计稿不符 —— `preview/dark`
 *           里根本没有标题条，标题就是写在页底上的正文色文字。所以夜间条底改用
 *           **屏底色**（条与页底同色），字用 INK。 */
static uint32_t helm_mhead_bar_hex(void)
{
    return helm_pal_night() ? HELM_COLOR_SCR : HELM_COLOR_INK;
}

/** @brief 页头标题文字色：日光 = 条底的米白反面，夜间 = 正文白。 */
static uint32_t helm_mhead_text_hex(void)
{
    return helm_pal_night() ? HELM_COLOR_INK : HELM_COLOR_PAPER;
}

lv_obj_t * helm_mhead_create(lv_obj_t * parent, const char * title)
{
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_t * lab;

    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, PAGE_HOR_RES, HELM_MHEAD_H);
    lv_obj_set_style_min_height(head, HELM_MHEAD_H, 0);
    lv_obj_set_style_max_height(head, HELM_MHEAD_H, 0);
    lv_obj_set_flex_grow(head, 0);
    lv_obj_set_style_bg_color(head, helm_color(helm_mhead_bar_hex()), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(head, 12, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lab = helm_label(head, helm_font_title(), helm_mhead_text_hex(),
                     title ? title : "");
    /* 像素宽：flex_grow + width 0 在父宽未解时标题是空白。 */
    lv_obj_set_width(lab, PAGE_HOR_RES - 24);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_user_data(head, lab);
    return head;
}

void helm_mhead_set(lv_obj_t * head, const char * title)
{
    lv_obj_t * lab;

    if (head == NULL) {
        return;
    }

    lab = (lv_obj_t *)lv_obj_get_user_data(head);
    if (lab) {
        lv_obj_set_style_text_font(lab, helm_font_title(), 0);
        lv_label_set_text(lab, title ? title : "");
    }
}

void helm_mhead_set_font(lv_obj_t * head, const lv_font_t * font)
{
    lv_obj_t * lab;

    if (head == NULL || font == NULL) {
        return;
    }

    lab = (lv_obj_t *)lv_obj_get_user_data(head);
    if (lab) {
        lv_obj_set_style_text_font(lab, font, 0);
    }
}

/** @brief 换主题后重设页头样式：把 `helm_mhead_create()` 设过的那几行**再跑一遍**。
 *  @details 颜色是建页时烘进样式的（`helm_color()` 取值就在 `set_style` 那一刻），
 *           `helm_mhead_set()` 只改文案不改色 ⇒ 明暗互换必须重跑这几行。
 *           **取色和 create 同源**（同一对 `helm_mhead_*_hex()`），免得两处跑偏。 */
void helm_mhead_reapply(lv_obj_t * head)
{
    lv_obj_t * lab;

    if (head == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(head, helm_color(helm_mhead_bar_hex()), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(head, 12, 0);

    lab = (lv_obj_t *)lv_obj_get_user_data(head);
    if (lab != NULL) {
        lv_obj_set_style_text_font(lab, helm_font_title(), 0);
        lv_obj_set_style_text_color(lab, helm_color(helm_mhead_text_hex()), 0);
    }
}

typedef struct {
    void * tag;
    lv_obj_t * list;
    lv_timer_t * tm;
    lv_area_t cur;
    lv_area_t trg;
    uint8_t sel;
    bool on;
    int16_t squeeze;
} helm_mcur_t;

static helm_mcur_t * helm_mcur_of(const lv_obj_t * list)
{
    helm_mcur_t * c;

    if (list == NULL) {
        return NULL;
    }

    /* lv_obj_get_user_data() 的形参没带 const（LVGL 的接口如此），这里只读。 */
    c = (helm_mcur_t *)lv_obj_get_user_data((lv_obj_t *)list);
    if (c == NULL || c->tag != HELM_MCUR_TAG) {
        return NULL;
    }

    return c;
}

static int32_t helm_unlinear(int32_t cur, int32_t tgt)
{
    int32_t d = tgt - cur;

    if (d == 0) {
        return cur;
    }

    if (d > -HELM_CUR_SPEED && d < HELM_CUR_SPEED) {
        return tgt;
    }

    return cur + d / HELM_CUR_SPEED;
}

static bool helm_mcur_visible(const lv_obj_t * list)
{
    const lv_obj_t * o;

    for (o = list; o != NULL; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }

        if (lv_obj_get_style_opa(o, LV_PART_MAIN) <= LV_OPA_MIN) {
            return false;
        }
    }

    return list != NULL;
}

static void helm_mcur_box(const helm_mcur_t * c, lv_area_t * out)
{
    *out = c->cur;
    if (c->squeeze > 0) {
        out->x1 += c->squeeze;
        out->x2 -= c->squeeze;
        if (out->x2 < out->x1) {
            out->x2 = out->x1;
        }
    }
}

static void helm_mcur_run(helm_mcur_t * c, bool run)
{
    if (c == NULL || c->tm == NULL) {
        return;
    }

    if (run) {
        lv_timer_resume(c->tm);
        lv_timer_reset(c->tm);
    } else {
        lv_timer_pause(c->tm);
    }
}

static void helm_mcur_inv_join(helm_mcur_t * c, const lv_area_t * a,
                               const lv_area_t * b)
{
    /* Two strips, not one tall union: OLED_UI 全屏 1bit 很便宜，
     * 这里 RGB 条带一合并就会把中间几行字一起栅格掉。 */
    lv_obj_invalidate_area(c->list, a);
    lv_obj_invalidate_area(c->list, b);
}

static void helm_mcur_draw(lv_event_t * e)
{
    helm_mcur_t * c = (helm_mcur_t *)lv_event_get_user_data(e);
    lv_layer_t * layer;
    lv_draw_rect_dsc_t rd;
    lv_area_t a;
    lv_opa_t opa;

    if (c == NULL || !c->on || c->list == NULL) {
        return;
    }

    opa = lv_obj_get_style_opa_recursive(c->list, LV_PART_MAIN);
    if (opa <= LV_OPA_MIN) {
        return;
    }

    layer = lv_event_get_layer(e);
    helm_mcur_box(c, &a);
    lv_draw_rect_dsc_init(&rd);
    /* 2026-09-26：选中态 = **中性抬升**（App 里根本没有"彩色光标"这回事，
     * 触屏靠手指；设备是键盘光标，所以做"底色浅一档"，不上任何彩色）。
     * 历史方案已否：实心橙块（压 22px 标题 ~3:1）、淡橙底 + 3px 橙条（太花）。 */
        rd.bg_color = helm_color(HELM_COLOR_MENU_LIFT);
    rd.bg_opa = opa;
    rd.radius = HELM_RADIUS - 2;
    lv_draw_rect(layer, &rd, &a);
}

static void helm_mcur_on_del(lv_event_t * e)
{
    helm_mcur_t * c = (helm_mcur_t *)lv_event_get_user_data(e);

    if (c == NULL) {
        return;
    }

    if (c->tm) {
        lv_timer_delete(c->tm);
        c->tm = NULL;
    }

    lv_free(c);
}

static void helm_mcur_tick(lv_timer_t * t)
{
    helm_mcur_t * c = (helm_mcur_t *)lv_timer_get_user_data(t);
    lv_obj_t * row;
    lv_area_t old;
    lv_area_t now;
    uint32_t n;

    if (c == NULL || c->list == NULL || !c->on) {
        helm_mcur_run(c, false);
        return;
    }

    if (!helm_mcur_visible(c->list)) {
        helm_mcur_run(c, false);
        return;
    }

    n = lv_obj_get_child_count(c->list);
    if (c->sel >= n) {
        lv_area_t gone;

        helm_mcur_box(c, &gone);
        c->on = false;
        lv_obj_invalidate_area(c->list, &gone);
        helm_mcur_run(c, false);
        return;
    }

    row = lv_obj_get_child(c->list, c->sel);
    if (row == NULL) {
        return;
    }

    lv_obj_get_coords(row, &c->trg);
    helm_mcur_box(c, &old);
    c->cur.x1 = helm_unlinear(c->cur.x1, c->trg.x1);
    c->cur.y1 = helm_unlinear(c->cur.y1, c->trg.y1);
    c->cur.x2 = helm_unlinear(c->cur.x2, c->trg.x2);
    c->cur.y2 = helm_unlinear(c->cur.y2, c->trg.y2);
    helm_mcur_box(c, &now);
    if (old.x1 != now.x1 || old.y1 != now.y1 ||
        old.x2 != now.x2 || old.y2 != now.y2) {
        helm_mcur_inv_join(c, &old, &now);
    } else if (!lv_obj_is_scrolling(c->list)) {
        helm_mcur_run(c, false);
    }
}

static helm_mcur_t * helm_mcur_ensure(lv_obj_t * list)
{
    helm_mcur_t * c;

    c = helm_mcur_of(list);
    if (c != NULL) {
        return c;
    }

    if (list == NULL) {
        return NULL;
    }

    c = (helm_mcur_t *)lv_malloc_zeroed(sizeof(*c));
    if (c == NULL) {
        return NULL;
    }

    c->tag = HELM_MCUR_TAG;
    c->list = list;
    c->tm = lv_timer_create(helm_mcur_tick, HELM_CUR_MS, c);
    lv_timer_pause(c->tm);
    lv_obj_set_user_data(list, c);
    lv_obj_add_event_cb(list, helm_mcur_draw, LV_EVENT_DRAW_MAIN_END, c);
    lv_obj_add_event_cb(list, helm_mcur_on_del, LV_EVENT_DELETE, c);
    return c;
}

static void helm_mcur_go(lv_obj_t * list, uint8_t idx, bool snap)
{
    helm_mcur_t * c;
    lv_obj_t * row;
    lv_area_t trg;
    lv_area_t old;

    c = helm_mcur_ensure(list);
    if (c == NULL || idx >= lv_obj_get_child_count(list)) {
        return;
    }

    helm_mcur_run(c, true);

    row = lv_obj_get_child(list, idx);
    if (row == NULL) {
        return;
    }

    lv_obj_update_layout(list);
    lv_obj_get_coords(row, &trg);
    c->sel = idx;
    c->trg = trg;
    if (!c->on || snap) {
        if (c->on) {
            helm_mcur_box(c, &old);
            c->cur = trg;
            c->squeeze = 0;
            helm_mcur_inv_join(c, &old, &c->cur);
        } else {
            c->cur = trg;
            c->squeeze = 0;
            lv_obj_invalidate_area(c->list, &c->cur);
        }
        c->on = true;
    } else {
        c->on = true;
    }
}

static void helm_mcur_sq_exec(void * var, int32_t v)
{
    helm_mcur_t * c = helm_mcur_of((lv_obj_t *)var);
    lv_area_t old;
    lv_area_t now;

    if (c == NULL || !c->on || !helm_mcur_visible(c->list)) {
        return;
    }

    helm_mcur_box(c, &old);
    c->squeeze = (int16_t)v;
    helm_mcur_box(c, &now);
    helm_mcur_inv_join(c, &old, &now);
}

static void helm_mcur_squeeze(lv_obj_t * list)
{
    helm_mcur_t * c;
    lv_anim_t a;

    c = helm_mcur_ensure(list);
    if (c == NULL || !c->on) {
        return;
    }

    lv_anim_delete(list, helm_mcur_sq_exec);
    c->squeeze = 0;
    lv_anim_init(&a);
    lv_anim_set_var(&a, list);
    lv_anim_set_values(&a, 0, HELM_CUR_SQUEEZE);
    lv_anim_set_time(&a, HELM_PRESS_MS);
    lv_anim_set_reverse_duration(&a, HELM_PRESS_MS + 20);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, helm_mcur_sq_exec);
    lv_anim_start(&a);
}

void helm_mlist_cursor_off(lv_obj_t * list)
{
    helm_mcur_t * c = helm_mcur_of(list);
    lv_area_t gone;

    if (c == NULL) {
        return;
    }

    lv_anim_delete(list, helm_mcur_sq_exec);
    helm_mcur_run(c, false);
    if (!c->on) {
        return;
    }

    helm_mcur_box(c, &gone);
    c->on = false;
    c->squeeze = 0;
    lv_obj_invalidate_area(c->list, &gone);
}

void helm_mlist_sel_snap(lv_obj_t * list, uint8_t idx)
{
    helm_mcur_go(list, idx, true);
}

lv_obj_t * helm_mlist_create(lv_obj_t * parent)
{
    lv_obj_t * list = lv_obj_create(parent);

    lv_obj_remove_style_all(list);
    helm_grow_y(list);
    helm_style_scr(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    helm_mcur_ensure(list);
    return list;
}

static uintptr_t helm_row_pack(uint8_t mix, uint8_t kind, bool has_sw, bool sw_on,
                               bool dark)
{
    uintptr_t u = (uintptr_t)mix | ((uintptr_t)kind << 8);

    if (has_sw) {
        u |= (1u << 10);
        if (sw_on) {
            u |= (1u << 11);
        }
    }

    if (dark) {
        u |= (1u << 12);
    }

    return u;
}

static bool helm_row_dark(lv_obj_t * row)
{
    return row != NULL &&
           (((uintptr_t)lv_obj_get_user_data(row) & (1u << 12)) != 0);
}

/** @brief 次级文字上色：浅底用 MUTE，深底（选中块 / 配色试验的整行底色）用浅色。
 *  @note 顺带打上 HELM_MLAB_TAG —— 光标动画到两端时要能把它找出来翻色。 */
static void helm_text_soft(lv_obj_t * obj, bool light, const lv_font_t * font)
{
    lv_color_t want;

    if (obj == NULL) {
        return;
    }

    lv_obj_set_user_data(obj, HELM_MLAB_TAG);
    want = helm_color(light ? HELM_COLOR_MENU_SEL_MUTED : HELM_COLOR_MENU_MUTED);
    /* 颜色没变就别 set_style：LVGL 不比较旧值，每设一次都会把该对象重刷一遍。
     * 光标每动一下要翻两行、每条 poll 刷新还会把整页再翻一遍 —— 全是白刷的
     * （用户 2026-09-26："感觉卡卡的"）。 */
    if (lv_color_eq(lv_obj_get_style_text_color(obj, 0), want)) {
        if (font && lv_obj_get_style_text_font(obj, 0) == font) {
            return;
        }

        if (font) {
            lv_obj_set_style_text_font(obj, font, 0);
        }

        return;
    }

    lv_obj_set_style_text_color(obj, want, 0);
    if (font) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

static void helm_mitem_soft_walk(lv_obj_t * obj, bool light)
{
    uint32_t i;
    uint32_t n;

    if (obj == NULL) {
        return;
    }

    n = lv_obj_get_child_count(obj);
    for (i = 0; i < n; i++) {
        lv_obj_t * c = lv_obj_get_child(obj, i);

        if (c == NULL) {
            continue;
        }

        if (lv_obj_get_user_data(c) == HELM_MLAB_TAG) {
            helm_text_soft(c, light, NULL);
        } else if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) {
            /* 行图标：亮底用本域色调，深底用它的**浅色版**（保住"每项图标一色"）。
             * 图标 id 存在 user_data 里（helm_icon_create），所以不另打标记，
             * 用 USER_1 这个用户标志认它，还原时照 id 重算色调；
             * helm_icon_set_color 自己会跳过同色（不然每次都要重画字形）。 */
            helm_ico_id_t lid = (helm_ico_id_t)(uintptr_t)lv_obj_get_user_data(c);

            helm_icon_set_color(c, helm_color(light
                ? helm_ico_tone_sel(lid) : helm_ico_tone(lid)));
        }

        helm_mitem_soft_walk(c, light);
    }
}

static void helm_row_store(lv_obj_t * row, uint8_t mix, uint8_t kind, bool has_sw,
                           bool sw_on, bool dark)
{
    lv_obj_set_user_data(row, (void *)helm_row_pack(mix, kind, has_sw, sw_on, dark));
}

static void helm_msw_restyle(lv_obj_t * sw, bool on, uint8_t mix)
{
    lv_obj_t * knob;
    lv_color_t ink = helm_color(HELM_COLOR_INK);
    lv_color_t paper = helm_color(HELM_COLOR_PAPER);
    lv_color_t nav = helm_color(HELM_COLOR_NAV);

    /* ⚠ 开关**不跟聚焦/选中反色**（用户 2026-09-26 晚："右侧的开关，在聚焦时也
     * 不需要反色吧，日光和夜间都可以保持原色显示"）。原来这里按 `mix` 把轨道/旋钮
     * 往 PAPER 混（↑聚焦↑浅），聚焦行上就成了一条浅色胶囊（现场截图：夜间那根粉白胶囊）。
     * 现在两个态都取**未聚焦**的满色，与 `helm_msw_make()` 同源：
     *   on  = 轨道 NAV、旋钮 PAPER；off = 描边 + 旋钮 INK。
     * 聚焦与否由整行底色（光标）表达，开关只表达**开/关**。
     * 注：这与本文件里"选中不再给文字反色"的那次是同一个决定（见
     * `helm_mitem_apply_mix()` 的注释）——这次把开关也补齐。 */
    LV_UNUSED(mix);

    if (sw == NULL) {
        return;
    }

    knob = lv_obj_get_child(sw, 0);
    if (on) {
        lv_obj_set_style_bg_color(sw, nav, 0);
        lv_obj_set_style_border_color(sw, nav, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        if (knob) {
            lv_obj_set_style_bg_color(knob, paper, 0);
            lv_obj_align(knob, LV_ALIGN_RIGHT_MID, -2, 0);
        }
    } else {
        lv_obj_set_style_border_color(sw, ink, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, 0);
        if (knob) {
            lv_obj_set_style_bg_color(knob, ink, 0);
            lv_obj_align(knob, LV_ALIGN_LEFT_MID, 2, 0);
        }
    }
}

static lv_obj_t * helm_row_find_sw(lv_obj_t * row)
{
    uint32_t i;
    uint32_t n;

    if (row == NULL) {
        return NULL;
    }

    n = lv_obj_get_child_count(row);
    for (i = 0; i < n; i++) {
        lv_obj_t * c = lv_obj_get_child(row, i);

        if (c && lv_obj_get_user_data(c) == HELM_MSW_TAG) {
            return c;
        }
    }

    return NULL;
}

static void helm_mitem_apply_mix(lv_obj_t * row, uint8_t mix)
{
    uintptr_t u;
    uint8_t kind;
    bool has_sw;
    bool sw_on;

    if (row == NULL) {
        return;
    }

    u = (uintptr_t)lv_obj_get_user_data(row);
    kind = (uint8_t)((u >> 8) & 0x3u);
    has_sw = (u & (1u << 10)) != 0;
    sw_on = (u & (1u << 11)) != 0;

    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    /* 次级文字翻色只在两端做：逐帧改会把整行标脏（原来连开关都只在端点动）。 */
    if (mix == 0 || mix == 255) {
        helm_mitem_soft_walk(row, mix == 255 || helm_row_dark(row));
    }

    /* ⚠ 不做"选中即反色"（2026-09-26 用户："选中后，可以不要吧字体反色设计吗"）：
     * 选中只换整行底色，文字保持 INK / MUTE —— 黑字压实心蓝（#2A6ED0）实测约
     * 4.2:1，够读；而且光标上下移动时文字不再闪白闪黑，观感稳。 */
    if (has_sw) {
        helm_msw_restyle(helm_row_find_sw(row), sw_on, mix);
    }

    helm_row_store(row, mix, kind, has_sw, sw_on, helm_row_dark(row));
}

static void helm_mitem_anim_mix(lv_obj_t * row, uint8_t to)
{
    helm_mitem_apply_mix(row, to);
}

/** @brief 造一个开关。
 *  @note **不看 `sel`**（"选中不反色"那条决定的落点，与 `helm_msw_restyle()` 同源）：
 *        建行时哪怕这行正被选中，开关也按它自己的态画 —— on = NAV 轨道 + PAPER 旋钮、
 *        off = INK 描边 + INK 旋钮。 */
static lv_obj_t * helm_msw_make(lv_obj_t * parent, bool on)
{
    lv_obj_t * sw = lv_obj_create(parent);
    lv_obj_t * knob;

    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, HELM_MSW_W, HELM_MSW_H);
    lv_obj_set_style_radius(sw, 9, 0);
    lv_obj_set_style_border_width(sw, 1, 0);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(sw, HELM_MSW_TAG);
    if (on) {
        lv_obj_set_style_bg_color(sw, helm_color(HELM_COLOR_NAV), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sw, helm_color(HELM_COLOR_NAV), 0);
    } else {
        lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(sw, helm_color(HELM_COLOR_INK), 0);
    }

    knob = lv_obj_create(sw);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, 12, 12);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    if (on) {
        lv_obj_set_style_bg_color(knob, helm_color(HELM_COLOR_PAPER), 0);
        lv_obj_align(knob, LV_ALIGN_RIGHT_MID, -2, 0);
    } else {
        lv_obj_set_style_bg_color(knob, helm_color(HELM_COLOR_INK), 0);
        lv_obj_align(knob, LV_ALIGN_LEFT_MID, 2, 0);
    }

    return sw;
}

static void helm_mbar_set(lv_obj_t * bar, const helm_item_t * spec)
{
    lv_obj_t * fill;
    uint32_t color;

    if (bar == NULL || spec == NULL) {
        return;
    }

    helm_pbar_set(bar, spec->progress);
    fill = (lv_obj_t *)lv_obj_get_user_data(bar);
    color = spec->progress_color ? spec->progress_color : HELM_COLOR_CAD;
    if (fill) {
        lv_obj_set_style_bg_color(fill, helm_color(color), 0);
    }
}

static lv_obj_t * helm_mbar_make(lv_obj_t * parent, const helm_item_t * spec)
{
    lv_obj_t * bar = helm_pbar_create(parent);

    lv_obj_set_height(bar, 5);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_margin_top(bar, 0, 0);
    helm_mbar_set(bar, spec);
    return bar;
}

/** @brief 给一棵子树里每个对象挂上绘制任务事件（打桩用，见 helm_ui_task_cb）。
 *  @details 行的标签/图标是**子对象**，事件只发给带标志的那个对象本身
 *           ⇒ 不逐对象挂，label/img/圆角填充这些分类永远是 0。 */
static void helm_probe_attach_tree(lv_obj_t * obj)
{
    uint32_t i;
    uint32_t n;

    if (obj == NULL) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(obj, helm_ui_task_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

    n = lv_obj_get_child_count(obj);
    for (i = 0; i < n; i++) {
        helm_probe_attach_tree(lv_obj_get_child(obj, i));
    }
}

void helm_mitem_list_built(lv_obj_t * list, int sel)
{
    lv_obj_t * row;
    uint32_t n;

    if (list == NULL || sel < 0) {
        return;
    }

    n = lv_obj_get_child_count(list);
    if ((uint32_t)sel >= n) {
        return;
    }

    row = lv_obj_get_child(list, (uint32_t)sel);
    if (row == NULL) {
        return;
    }

    /* 已经在视口里就别滚 —— `lv_obj_scroll_to_view()` 会强制整表重新布局，
     * 实测这一步单独就 50~250 ms（[vperf] ui slow menu 的 grp= 里就混着它）。
     * 判据取最省事的那种：选中行是第 0 行且列表已在顶端（进页/首页的常态），
     * 这时滚动本来就是空操作。要判"任意行是否可见"就得先算布局，反而更贵。 */
    if (sel != 0 || lv_obj_get_scroll_y(list) != 0) {
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }

    helm_mcur_go(list, (uint8_t)sel, true);
}

lv_obj_t * helm_mitem_create(lv_obj_t * list, const helm_item_t * spec)
{
    lv_obj_t * row;
    lv_obj_t * body;
    lv_obj_t * lab;
    lv_obj_t * ico;
    bool on;
    bool slim;
    lv_coord_t h;

    if (spec == NULL) {
        return NULL;
    }

    on = spec->sel;
    slim = (spec->kind == HELM_ITEM_SLIM);
    h = slim ? 36 : HELM_LIST_ROW_H;

    row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), h);
    lv_obj_set_style_radius(row, HELM_RADIUS, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* 2026-09-26 菜单现代化：行底改成 App 的深色卡（#1C1C1E），行间发丝线缩进到
     * 文字列（HELM_MENU_HAIR_INDENT）。样式只在这里设一次，刷新路径不动。 */
    lv_obj_set_style_bg_color(row,
        helm_color(spec->bg_set ? spec->bg_color : HELM_COLOR_MENU_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    helm_row_store(row, on ? 255 : 0, spec->kind,
                   spec->kind == HELM_ITEM_SW, spec->on, spec->bg_dark);

    if (spec->ico != HELM_ICO_NONE) {
        ico = helm_icon_create(row, spec->ico, 22);
        /* 选中块/深底上本域色调全糊（用户 2026-09-26："左侧 icon 看不清"）⇒ 翻白。 */
        lv_obj_add_flag(ico, LV_OBJ_FLAG_USER_1);
        helm_icon_set_color(ico, helm_color((on || spec->bg_dark)
            ? helm_ico_tone_sel(spec->ico) : helm_ico_tone(spec->ico)));
    }

    body = lv_obj_create(row);
    lv_obj_remove_style_all(body);
    helm_grow_x(body);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(body, 2, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lab = lv_label_create(body);
    lv_label_set_text(lab, spec->title ? spec->title : "");
    /* 字号层级对齐 App（18/15 在 390 宽上 ⇒ 15/12 在 240 宽上）：
     * 原来非 slim 行用 22px，在 240 宽上过大、"浮夸"的一半来自这里。 */
    /* 与 helm_mitem_refresh 同源：非 slim 行 15px。原来这里取 12px、
     * 刷新那条路取 15px ⇒ 同一行首帧 12px、重画一次变 15px。 */
    helm_text_sel(lab, on,
                  spec->title_font ? spec->title_font :
                  (slim ? helm_font_lab() : helm_font_title()));
    lv_obj_set_style_text_color(lab, helm_color(HELM_COLOR_MENU_FG), 0);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(lab, lv_pct(100));

    if (spec->sub && spec->sub[0] != '\0') {
        lv_obj_t * sub = lv_label_create(body);

        lv_label_set_text(sub, spec->sub);
        /* 走 helm_font_lab() 而不是裸 &helm_mism4_12：副标题里放的是蓝牙地址
         * 这类外来串，必须带上"苹方 → 系统 TTF"的回退链（见 helm_font.c）。 */
        helm_text_soft(sub, on || spec->bg_dark, helm_font_lab());
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(sub, lv_pct(100));
    }

    if (spec->progress_on) {
        helm_mbar_make(body, spec);
    }

    if (spec->kind == HELM_ITEM_SW) {
        helm_msw_make(row, spec->on);
    } else if (spec->kind == HELM_ITEM_VAL) {
        lv_obj_t * val = lv_label_create(row);

        lv_label_set_text(val, spec->value ? spec->value : "");
        helm_text_soft(val, on || spec->bg_dark, helm_font_lab());
    } else if (!slim) {
        if (spec->value && spec->value[0] != '\0') {
            lv_obj_t * val = lv_label_create(row);

            lv_label_set_text(val, spec->value);
            helm_text_soft(val, on || spec->bg_dark, helm_font_lab());
        }

        {
            lv_obj_t * go = helm_icon_create(row, HELM_ICO_GO, 10);

            helm_icon_set_color(go, helm_color(HELM_COLOR_INK));
        }
    }

    helm_probe_attach_tree(row);
    helm_mitem_apply_mix(row, on ? 255 : 0);
    /* ⚠ 这里**不要**滚动/起光标：建一行就 scroll_to_view 一次会强制整表重新布局，
     * 于是变成 O(n²) —— 实测 8 行 200 ms、15 行 115 ms（[vperf] ui slow menu 的
     * build=）。整页建完之后调一次 helm_mitem_list_built() 即可。 */

    return row;
}

bool helm_mitem_refresh(lv_obj_t * row, const helm_item_t * spec)
{
    uintptr_t u;
    uint8_t kind;
    bool has_sw;
    bool slim;
    bool want_ico;
    bool want_sub;
    bool want_val;
    bool want_bar;
    lv_obj_t * ico = NULL;
    lv_obj_t * body = NULL;
    lv_obj_t * sw = NULL;
    lv_obj_t * val = NULL;
    lv_obj_t * go = NULL;
    lv_obj_t * bar = NULL;
    lv_obj_t * title;
    lv_obj_t * sub;
    uint32_t i;
    uint32_t n;

    if (row == NULL || spec == NULL) {
        return false;
    }

    u = (uintptr_t)lv_obj_get_user_data(row);
    kind = (uint8_t)((u >> 8) & 0x3u);
    has_sw = (u & (1u << 10)) != 0;
    if (kind != spec->kind || has_sw != (spec->kind == HELM_ITEM_SW)) {
        return false;
    }

    /* 底色也归刷新路径管：不然重建后改了底色、就地刷新那条路会留着旧色
     * （创建与刷新两处必须同源，否则同一个 spec 两种观感）。 */
    lv_obj_set_style_bg_color(row,
        helm_color(spec->bg_set ? spec->bg_color : HELM_COLOR_MENU_CARD), 0);
    slim = (spec->kind == HELM_ITEM_SLIM);
    want_ico = (spec->ico != HELM_ICO_NONE);
    want_sub = (spec->sub != NULL && spec->sub[0] != '\0');
    want_bar = spec->progress_on;
    if (spec->kind == HELM_ITEM_VAL) {
        want_val = true;
    } else if (spec->kind == HELM_ITEM_GO && spec->value != NULL &&
               spec->value[0] != '\0') {
        want_val = true;
    } else {
        want_val = false;
    }

    n = lv_obj_get_child_count(row);
    for (i = 0; i < n; i++) {
        lv_obj_t * c = lv_obj_get_child(row, i);

        if (c == NULL) {
            continue;
        }

        if (lv_obj_get_user_data(c) == HELM_MSW_TAG) {
            sw = c;
            continue;
        }

        if (lv_obj_check_type(c, &lv_label_class)) {
            val = c;
            continue;
        }

        if (lv_obj_get_child_count(c) >= 1) {
            if (body == NULL) {
                body = c;
            }
            continue;
        }

        if (body == NULL) {
            ico = c;
        } else {
            go = c;
        }
    }

    if (body == NULL) {
        return false;
    }

    if (want_ico != (ico != NULL) ||
        (spec->kind == HELM_ITEM_SW) != (sw != NULL) ||
        want_val != (val != NULL)) {
        return false;
    }

    if (slim) {
        if (go != NULL || val != NULL) {
            return false;
        }
    } else if (spec->kind == HELM_ITEM_GO && go == NULL) {
        return false;
    }

    if (ico != NULL) {
        /* 就地刷新也要跟选中/深底走，否则翻色只发生在重建那一拍。 */
        helm_icon_set_color(ico, helm_color((spec->sel || spec->bg_dark)
            ? helm_ico_tone_sel(spec->ico) : helm_ico_tone(spec->ico)));
    }

    title = lv_obj_get_child(body, 0);
    sub = want_sub ? lv_obj_get_child(body, 1) : NULL;
    if (lv_obj_get_child_count(body) !=
        (uint32_t)(1u + (want_sub ? 1u : 0u) + (want_bar ? 1u : 0u))) {
        return false;
    }

    if (want_bar) {
        bar = lv_obj_get_child(body, want_sub ? 2 : 1);
    }

    if (title == NULL || !lv_obj_check_type(title, &lv_label_class)) {
        return false;
    }

    if (want_sub != (sub != NULL && lv_obj_check_type(sub, &lv_label_class))) {
        return false;
    }

    if (want_bar && (bar == NULL || lv_obj_check_type(bar, &lv_label_class))) {
        return false;
    }

    if (!spec->live) {
        lv_label_set_text(title, spec->title ? spec->title : "");
        helm_text_sel(title, spec->sel,
                      spec->title_font ? spec->title_font :
                      (slim ? helm_font_lab() : helm_font_title()));
        if (ico) {
            helm_icon_set(ico, spec->ico);
        }
    }

    if (sub) {
        const char * cur = lv_label_get_text(sub);
        const char * next = spec->sub ? spec->sub : "";

        helm_text_soft(sub, spec->sel || spec->bg_dark, NULL);

        if (cur == NULL || strcmp(cur, next) != 0) {
            lv_label_set_text(sub, next);
        }
    }

    if (val) {
        const char * cur = lv_label_get_text(val);
        const char * next = spec->value ? spec->value : "";

        helm_text_soft(val, spec->sel || spec->bg_dark, NULL);

        if (cur == NULL || strcmp(cur, next) != 0) {
            lv_label_set_text(val, next);
        }
    }

    if (bar) {
        helm_mbar_set(bar, spec);
    }

    if (!spec->live) {
        uint8_t mix = spec->sel ? 255 : 0;

        helm_row_store(row, mix, spec->kind, spec->kind == HELM_ITEM_SW,
                       spec->on, spec->bg_dark);
        helm_mitem_apply_mix(row, mix);
        if (spec->sel) {
            int32_t idx = lv_obj_get_index(row);
            lv_obj_t * parent = lv_obj_get_parent(row);

            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
            if (parent && idx >= 0) {
                helm_mcur_go(parent, (uint8_t)idx, true);
            }
        }
    }

    return true;
}

static bool helm_row_in_view(lv_obj_t * list, lv_obj_t * row)
{
    lv_area_t vis;
    lv_area_t ra;

    if (list == NULL || row == NULL) {
        return false;
    }

    lv_obj_update_layout(list);
    lv_obj_get_content_coords(list, &vis);
    lv_obj_get_coords(row, &ra);
    return ra.y1 >= vis.y1 && ra.y2 <= vis.y2;
}

void helm_mlist_move_sel(lv_obj_t * list, uint8_t from, uint8_t to)
{
    lv_obj_t * a;
    lv_obj_t * b;
    uint32_t n;

    if (list == NULL) {
        return;
    }

    n = lv_obj_get_child_count(list);
    if (from >= n || to >= n) {
        return;
    }

    a = lv_obj_get_child(list, from);
    b = lv_obj_get_child(list, to);
    if (from != to && a) {
        helm_mitem_anim_mix(a, 0);
    }

    if (b) {
        bool in_view = helm_row_in_view(list, b);

        helm_mitem_anim_mix(b, 255);
        helm_mcur_go(list, to, false);
        if (!in_view) {
            lv_obj_scroll_to_view(b, LV_ANIM_ON);
        }
    }
}

static void helm_enter_x(void * var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
}

static void helm_enter_opa(void * var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

void helm_obj_enter(lv_obj_t * obj, int dir)
{
    lv_anim_t ax;
    lv_anim_t ao;
    lv_coord_t from;

    if (obj == NULL || dir == 0) {
        return;
    }

    from = (dir > 0) ? HELM_ENTER_PX : -HELM_ENTER_PX;
    lv_anim_delete(obj, NULL);
    lv_obj_set_style_translate_x(obj, from, 0);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);

    lv_anim_init(&ax);
    lv_anim_set_var(&ax, obj);
    lv_anim_set_values(&ax, from, 0);
    lv_anim_set_time(&ax, HELM_ENTER_MS);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ax, helm_enter_x);
    lv_anim_start(&ax);

    lv_anim_init(&ao);
    lv_anim_set_var(&ao, obj);
    lv_anim_set_values(&ao, 0, 255);
    lv_anim_set_time(&ao, HELM_ENTER_MS);
    lv_anim_set_path_cb(&ao, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ao, helm_enter_opa);
    lv_anim_start(&ao);

    helm_obj_stagger_in(obj);
}

static void helm_set_ty(lv_obj_t * obj, int32_t v)
{
    lv_obj_t * parent;
    lv_area_t dirty;
    int32_t cur;
    int32_t dy;

    if (obj == NULL) {
        return;
    }

    cur = lv_obj_get_style_translate_y(obj, LV_PART_MAIN);
    if (cur == v) {
        return;
    }

    dy = v - cur;
    parent = lv_obj_get_parent(obj);
    lv_obj_get_coords(obj, &dirty);
    if (dy > 0) {
        dirty.y2 += dy;
    } else {
        dirty.y1 += dy;
    }
    /* Dirty the strip via the parent so vacated pixels (list bg / neighbor)
     * redraw, without lv_obj_set_style_translate_y marking the flex list. */
    if (parent != NULL) {
        lv_obj_invalidate_area(parent, &dirty);
    } else {
        lv_obj_invalidate(obj);
    }

    lv_obj_enable_style_refresh(false);
    lv_obj_set_style_translate_y(obj, v, 0);
    lv_obj_enable_style_refresh(true);

    obj->coords.y1 += dy;
    obj->coords.y2 += dy;
    lv_obj_move_children_by(obj, 0, dy, false);
}

static void helm_ty(void * var, int32_t v)
{
    helm_set_ty((lv_obj_t *)var, v);
}

static void helm_press_ready(lv_anim_t * a)
{
    lv_obj_t * obj = (lv_obj_t *)a->var;

    if (obj) {
        helm_set_ty(obj, 0);
    }
}

void helm_obj_press(lv_obj_t * obj)
{
    lv_anim_t a;

    if (obj == NULL) {
        return;
    }

    lv_anim_delete(obj, helm_ty);
    helm_set_ty(obj, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 0, HELM_PRESS_PX);
    lv_anim_set_time(&a, HELM_PRESS_MS);
    lv_anim_set_reverse_duration(&a, HELM_PRESS_MS + 20);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, helm_ty);
    lv_anim_set_ready_cb(&a, helm_press_ready);
    lv_anim_start(&a);
}

void helm_mlist_press_sel(lv_obj_t * list, uint8_t idx)
{
    if (list == NULL || idx >= lv_obj_get_child_count(list)) {
        return;
    }

    helm_mcur_go(list, idx, false);
    helm_mcur_squeeze(list);
}

static void helm_squash_exec(void * var, int32_t v)
{
    lv_obj_set_style_transform_height((lv_obj_t *)var, (HELM_SQUASH_H * v) / 256, 0);
}

static int32_t helm_squash_mix(const lv_obj_t * obj)
{
    int32_t h;

    if (obj == NULL) {
        return 0;
    }

    h = lv_obj_get_style_transform_height(obj, 0);
    return (h * 256) / HELM_SQUASH_H;
}

void helm_obj_squash_set(lv_obj_t * obj, bool down)
{
    if (obj == NULL) {
        return;
    }

    lv_anim_delete(obj, helm_squash_exec);
    helm_squash_exec(obj, down ? 256 : 0);
}

void helm_obj_squash(lv_obj_t * obj, bool down)
{
    lv_anim_t a;
    int32_t from;

    if (obj == NULL) {
        return;
    }

    from = helm_squash_mix(obj);
    if (from < 0) {
        from = 0;
    } else if (from > 256) {
        from = 256;
    }

    lv_anim_delete(obj, helm_squash_exec);
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, down ? 256 : 0);
    lv_anim_set_time(&a, HELM_SQUASH_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, helm_squash_exec);
    lv_anim_start(&a);
}

static void helm_squash_ready(lv_anim_t * a)
{
    if (a && a->var) {
        helm_squash_exec(a->var, 0);
    }
}

void helm_obj_squash_pulse(lv_obj_t * obj)
{
    lv_anim_t a;

    if (obj == NULL) {
        return;
    }

    lv_anim_delete(obj, helm_squash_exec);
    helm_squash_exec(obj, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_time(&a, HELM_SQUASH_MS);
    lv_anim_set_reverse_duration(&a, HELM_SQUASH_MS + 20);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, helm_squash_exec);
    lv_anim_set_ready_cb(&a, helm_squash_ready);
    lv_anim_start(&a);
}

void helm_obj_stagger_clear(lv_obj_t * parent)
{
    uint32_t i;
    uint32_t n;

    if (parent == NULL) {
        return;
    }

    n = lv_obj_get_child_count(parent);
    for (i = 0; i < n; i++) {
        lv_obj_t * c = lv_obj_get_child(parent, i);

        if (c == NULL || lv_obj_has_flag(c, LV_OBJ_FLAG_FLOATING)) {
            continue;
        }

        lv_anim_delete(c, helm_ty);
        helm_set_ty(c, 0);
        helm_obj_squash_set(c, false);
    }
}

void helm_obj_stagger_in(lv_obj_t * parent)
{
    uint32_t i;
    uint32_t n;
    uint32_t k = 0;
    helm_mcur_t * cur;

    if (parent == NULL) {
        return;
    }

    cur = helm_mcur_of(parent);
    if (cur && cur->on) {
        helm_mcur_run(cur, true);
    }

    n = lv_obj_get_child_count(parent);
    for (i = 0; i < n && k < HELM_STAGGER_MAX; i++) {
        lv_obj_t * c = lv_obj_get_child(parent, i);
        lv_anim_t a;

        if (c == NULL || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) ||
            lv_obj_has_flag(c, LV_OBJ_FLAG_FLOATING)) {
            continue;
        }

        lv_anim_delete(c, helm_ty);
        helm_set_ty(c, HELM_STAGGER_PX);

        lv_anim_init(&a);
        lv_anim_set_var(&a, c);
        lv_anim_set_values(&a, HELM_STAGGER_PX, 0);
        lv_anim_set_time(&a, HELM_STAGGER_MS);
        lv_anim_set_delay(&a, k * HELM_STAGGER_GAP);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, helm_ty);
        lv_anim_start(&a);
        k++;
    }
}

lv_obj_t * helm_softkeys_create(lv_obj_t * parent, uint32_t color)
{
    lv_obj_t * keys = lv_obj_create(parent);
    lv_obj_t * x;
    lv_obj_t * ok;
    lv_color_t c = helm_color(color);

    lv_obj_remove_style_all(keys);
    lv_obj_set_size(keys, lv_pct(100), HELM_DOCK_KEY_H);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(keys, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(keys, HELM_DOCK_KEY_MT, 0);
    lv_obj_clear_flag(keys, LV_OBJ_FLAG_SCROLLABLE);

    x = helm_icon_create(keys, HELM_ICO_XMARK, 14);
    helm_icon_set_color(x, c);
    ok = helm_icon_create(keys, HELM_ICO_CHECK, 14);
    helm_icon_set_color(ok, c);
    return keys;
}

lv_obj_t * helm_sum_cell(lv_obj_t * parent, helm_ico_id_t ico, uint32_t ico_color,
                         const char * lab, lv_obj_t ** val)
{
    lv_obj_t * cell = lv_obj_create(parent);
    lv_obj_t * col;
    lv_obj_t * icon;

    lv_obj_remove_style_all(cell);
    helm_grow_x(cell);
    lv_obj_set_height(cell, HELM_DOCK_CELL_H);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cell, HELM_DOCK_COL_GAP, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    icon = helm_icon_create(cell, ico, HELM_DOCK_ICO_SUM);
    helm_icon_set_color(icon, helm_color(ico_color));

    col = lv_obj_create(cell);
    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    helm_label(col, helm_font_lab(), HELM_COLOR_INK, lab);
    *val = helm_label(col, helm_font_title(), HELM_COLOR_INK, "--");
    return cell;
}

lv_obj_t * helm_sum_grid(lv_obj_t * parent)
{
    lv_obj_t * grid = lv_obj_create(parent);

    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(grid, HELM_DOCK_SUM_MT, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grid, HELM_DOCK_ROW_GAP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    return grid;
}

lv_obj_t * helm_sheet_hero(lv_obj_t * parent, helm_ico_id_t ico, bool compact)
{
    lv_obj_t * slot = lv_obj_create(parent);
    lv_obj_t * hero;
    lv_obj_t * icon;

    /* CSS margin 6 6 0. LVGL width 100% + margin clips the right border. */
    lv_obj_remove_style_all(slot);
    lv_obj_set_width(slot, lv_pct(100));
    lv_obj_set_height(slot, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(slot, 6, 0);
    lv_obj_set_style_pad_hor(slot, 6, 0);
    lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    hero = lv_obj_create(slot);
    lv_obj_remove_style_all(hero);
    lv_obj_set_width(hero, lv_pct(100));
    lv_obj_set_height(hero, LV_SIZE_CONTENT);
    helm_style_card(hero);
    lv_obj_set_style_pad_all(hero, compact ? 10 : 12, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    icon = helm_icon_create(hero, ico, compact ? 24 : 36);
    helm_icon_set_color(icon, helm_color(HELM_COLOR_NAV));
    return hero;
}

lv_obj_t * helm_kvbox_create(lv_obj_t * parent)
{
    lv_obj_t * slot = lv_obj_create(parent);
    lv_obj_t * box;

    lv_obj_remove_style_all(slot);
    helm_grow_y(slot);
    lv_obj_set_style_pad_all(slot, 6, 0);
    lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    box = lv_obj_create(slot);
    lv_obj_remove_style_all(box);
    helm_grow_y(box);
    helm_style_card(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

lv_obj_t * helm_kv_add(lv_obj_t * box, helm_ico_id_t ico, const char * k,
                       const char * v)
{
    lv_obj_t * row = lv_obj_create(box);
    lv_obj_t * icon;
    lv_obj_t * val;

    lv_obj_remove_style_all(row);
    helm_grow_y(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    icon = helm_icon_create(row, ico, 16);
    /* 设计稿 19/20 页的 KV 行图标是**次级灰**（原来是橙色，比右边的值还抢眼）。 */
    helm_icon_set_color(icon, helm_color(HELM_COLOR_MUTE));
    /* 键次级（MUTE 12px）、值主级（INK 15px）—— 与菜单行的层级一致。 */
    helm_label(row, helm_font_lab(), HELM_COLOR_MUTE, k);
    val = helm_label(row, helm_font_title(), HELM_COLOR_INK, v ? v : "");
    helm_grow_x(val);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_DOTS);
    return row;
}

void helm_kvbox_seal(lv_obj_t * box)
{
    uint32_t n;
    lv_obj_t * last;

    if (box == NULL) {
        return;
    }

    n = lv_obj_get_child_count(box);
    if (n == 0) {
        return;
    }

    last = lv_obj_get_child(box, (int32_t)(n - 1u));
    if (last) {
        lv_obj_set_style_border_width(last, 0, 0);
    }
}

lv_obj_t * helm_empty_create(lv_obj_t * parent, helm_ico_id_t ico,
                             const char * title, const char * sub)
{
    lv_obj_t * empty = lv_obj_create(parent);
    lv_obj_t * icon;

    /* CSS .empty { flex:1; margin:6px }. LVGL grow + margin overflows and
     * clips the bottom/right border — pad the slot, not the card. */
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_remove_style_all(empty);
    helm_grow_y(empty);
    helm_style_card(empty);
    lv_obj_set_style_pad_all(empty, 16, 0);
    lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(empty, 8, 0);
    lv_obj_clear_flag(empty, LV_OBJ_FLAG_SCROLLABLE);

    icon = helm_icon_create(empty, ico, 36);
    helm_icon_set_color(icon, helm_color(HELM_COLOR_NAV));
    helm_label(empty, helm_font_title(), HELM_COLOR_INK, title);
    if (sub) {
        helm_label(empty, helm_font_lab(), HELM_COLOR_INK, sub);
    }

    return empty;
}

void helm_empty_set(lv_obj_t * parent, helm_ico_id_t ico, const char * title,
                     const char * sub)
{
    lv_obj_t * card;
    lv_obj_t * icon;
    lv_obj_t * tlab;
    lv_obj_t * slab;
    bool want_sub;

    if (parent == NULL) {
        return;
    }

    want_sub = (sub != NULL);
    card = lv_obj_get_child(parent, 0);
    icon = (card != NULL) ? lv_obj_get_child(card, 0) : NULL;
    tlab = (card != NULL) ? lv_obj_get_child(card, 1) : NULL;
    slab = (card != NULL) ? lv_obj_get_child(card, 2) : NULL;
    if (lv_obj_get_child_count(parent) != 1 || card == NULL || icon == NULL ||
        tlab == NULL || !lv_obj_check_type(tlab, &lv_label_class) ||
        want_sub != (slab != NULL && lv_obj_check_type(slab, &lv_label_class))) {
        lv_obj_clean(parent);
        helm_empty_create(parent, ico, title, sub);
        return;
    }

    helm_icon_set(icon, ico);
    lv_label_set_text(tlab, title ? title : "");
    if (slab) {
        lv_label_set_text(slab, sub);
    }
}

lv_obj_t * helm_pbar_create(lv_obj_t * parent)
{
    lv_obj_t * wrap = lv_obj_create(parent);
    lv_obj_t * fill;

    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, lv_pct(100), 10);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_bg_color(wrap, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wrap, 5, 0);
    lv_obj_set_style_clip_corner(wrap, true, 0);
    lv_obj_set_style_margin_top(wrap, 10, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    fill = lv_obj_create(wrap);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, lv_pct(0), lv_pct(100));
    lv_obj_set_style_bg_color(fill, helm_color(HELM_COLOR_CAD), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_user_data(wrap, fill);
    return wrap;
}

void helm_pbar_set(lv_obj_t * bar, uint8_t pct)
{
    lv_obj_t * fill;

    if (bar == NULL) {
        return;
    }

    if (pct > 100) {
        pct = 100;
    }

    fill = (lv_obj_t *)lv_obj_get_user_data(bar);
    if (fill) {
        lv_obj_set_width(fill, lv_pct(pct));
    }
}

lv_obj_t * helm_mask_create(lv_obj_t * parent)
{
    lv_obj_t * mask = lv_obj_create(parent);

    lv_obj_remove_style_all(mask);
    /* 像素尺寸：flex 父上 FLOATING + pct(100) 会解成 0，弹窗/标题都不画。 */
    lv_obj_set_size(mask, PAGE_HOR_RES, HELM_PAGE_H);
    lv_obj_align(mask, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_set_flex_flow(mask, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mask, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    return mask;
}

lv_obj_t * helm_card_create(lv_obj_t * mask, helm_ico_id_t ico, const char * title,
                            bool alert)
{
    lv_obj_t * card = lv_obj_create(mask);
    lv_obj_t * head;
    lv_obj_t * icon;
    lv_obj_t * body;

    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 216, LV_SIZE_CONTENT);
    helm_style_card(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(head,
        helm_color(alert ? HELM_COLOR_HR : HELM_COLOR_INK), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(head, 6, 0);

    icon = helm_icon_create(head, ico, 18);
    helm_icon_set_color(icon, helm_color(HELM_COLOR_PAPER));
    helm_label(head, helm_font_lab(), HELM_COLOR_PAPER, title);

    body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_user_data(card, body);
    return card;
}

lv_obj_t * helm_dock_create(lv_obj_t * parent, bool ok)
{
    lv_obj_t * dock = lv_obj_create(parent);

    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, PAGE_HOR_RES - 2 * HELM_DOCK_INSET, LV_SIZE_CONTENT);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -HELM_DOCK_INSET);
    lv_obj_add_flag(dock, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    if (ok) {
        lv_obj_set_style_bg_color(dock, helm_color(HELM_COLOR_CAD_FILL), 0);
        lv_obj_set_style_border_color(dock, helm_color(HELM_COLOR_CAD), 0);
    } else {
        lv_obj_set_style_bg_color(dock, helm_color(HELM_COLOR_WARN), 0);
        lv_obj_set_style_border_color(dock, helm_color(HELM_COLOR_INK), 0);
    }

    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dock, HELM_DOCK_BORDER_W, 0);
    lv_obj_set_style_radius(dock, HELM_RADIUS, 0);
    lv_obj_set_style_pad_top(dock, 8, 0);
    lv_obj_set_style_pad_bottom(dock, 6, 0);
    lv_obj_set_style_pad_hor(dock, 10, 0);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_COLUMN);
    return dock;
}
