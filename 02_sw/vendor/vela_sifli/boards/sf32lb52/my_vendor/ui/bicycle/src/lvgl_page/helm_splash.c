/**
 * @file helm_splash.c
 * @brief 开机进入 / 关机退出共用的 logo splash。
 *
 * 事项行滚入滚出、忙状态省略点、细进度条。API 见 helm_splash.h；
 * 开机/关机时序见 doc/splash.md。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "helm_splash.h"

#include "helm_font.h"
#include "helm_widget.h"
#include "img_src_boot_logo.h"

#include <string.h>

static void helm_splash_out_ready(lv_anim_t *a);

#define HELM_SPLASH_DOT_MS 400u

/** @brief 把 shown 画到事项行；busy 时跟省略点。 */
static void helm_splash_paint_label(helm_splash_t *s)
{
    char line[56];
    static const char *const dots[] = { "", ".", "..", "..." };
    unsigned n;

    if (s == NULL || s->lab == NULL) {
        return;
    }

    if (s->busy && s->shown[0] != '\0') {
        n = (lv_tick_get() / HELM_SPLASH_DOT_MS) % 4u;
        lv_snprintf(line, sizeof(line), "%s%s", s->shown, dots[n]);
        lv_label_set_text(s->lab, line);
    } else {
        lv_label_set_text(s->lab, s->shown);
    }

    lv_obj_align(s->lab, LV_ALIGN_CENTER, 0, 0);
}

static void helm_splash_ty(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void helm_splash_lab_opa(helm_splash_t *s, lv_opa_t o)
{
    if (s == NULL || s->lab == NULL) {
        return;
    }

    lv_obj_set_style_opa(s->lab, o, 0);
    lv_obj_set_style_text_opa(s->lab, o, 0);
}

static void helm_splash_fade(void *var, int32_t v)
{
    lv_obj_t *lab = (lv_obj_t *)var;
    helm_splash_t *s = lab ? (helm_splash_t *)lv_obj_get_user_data(lab) : NULL;

    helm_splash_lab_opa(s, (lv_opa_t)v);
}

static void helm_splash_bar_exec(void *var, int32_t v)
{
    helm_splash_bar_set((helm_splash_t *)var, (uint8_t)v);
}

/** @brief 事项行位移 + 透明度动画。out 结束后吃 pending。 */
static void helm_splash_slide_anim(helm_splash_t *s, int32_t y0, int32_t y1,
                                  int32_t o0, int32_t o1, bool out)
{
    lv_anim_t a;
    uint32_t ms;

    if (s == NULL || s->lab == NULL) {
        return;
    }

    ms = s->slide_ms == 0u ? HELM_SPLASH_SLIDE_MS : s->slide_ms;
    lv_anim_delete(s->lab, helm_splash_ty);
    lv_anim_delete(s->lab, helm_splash_fade);

    lv_obj_set_style_translate_y(s->lab, (lv_coord_t)y0, 0);
    helm_splash_lab_opa(s, (lv_opa_t)o0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s->lab);
    lv_anim_set_values(&a, y0, y1);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, helm_splash_ty);
    if (out) {
        lv_anim_set_ready_cb(&a, helm_splash_out_ready);
    }

    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s->lab);
    lv_anim_set_values(&a, o0, o1);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, helm_splash_fade);
    lv_anim_start(&a);
}

/** @brief 滚出结束：清 out_busy，有 pending 则滚入。 */
static void helm_splash_out_ready(lv_anim_t *a)
{
    helm_splash_t *s;
    lv_obj_t *lab;

    lab = a ? (lv_obj_t *)a->var : NULL;
    s = lab ? (helm_splash_t *)lv_obj_get_user_data(lab) : NULL;
    if (s == NULL) {
        return;
    }

    s->out_busy = false;
    if (s->pending[0] == '\0') {
        return;
    }

    helm_splash_set_line(s, s->pending, true);
    s->pending[0] = '\0';
    helm_splash_slide_in(s);
}

/** @brief 在 root 上创建 logo、品名、裁剪事项行和进度条。 */
void helm_splash_build(helm_splash_t *s, lv_obj_t *root)
{
    const lv_font_t *tf;
    lv_coord_t h;

    if (s == NULL || root == NULL) {
        return;
    }

    memset(s, 0, sizeof(*s));
    s->root = root;
    s->gray = 255;
    s->slide_ms = HELM_SPLASH_SLIDE_MS;

    s->logo = lv_img_create(root);
    lv_img_set_src(s->logo, &img_src_boot_logo);
    lv_obj_set_size(s->logo, BOOT_LOGO_W, BOOT_LOGO_H);
    lv_obj_set_pos(s->logo, BOOT_LOGO_X, BOOT_LOGO_Y);
    lv_obj_clear_flag(s->logo, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s->name = lv_label_create(root);
    lv_obj_set_style_text_font(s->name, &helm_ui_22, 0);
    lv_obj_set_style_text_color(s->name, lv_color_black(), 0);
    lv_obj_set_style_text_opa(s->name, LV_OPA_COVER, 0);
    lv_label_set_text(s->name, BOOT_PRODUCT_NAME);
    lv_obj_set_width(s->name, BOOT_NAME_W);
    lv_obj_set_style_text_align(s->name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s->name, BOOT_NAME_X, BOOT_NAME_Y);

    tf = helm_font_sys(14, helm_font_lab());
    h = (tf != NULL) ? (lv_coord_t)(tf->line_height + 4) : BOOT_TICK_H;
    if (h < 20) {
        h = 20;
    }

    if (h > BOOT_TICK_H) {
        h = BOOT_TICK_H;
    }

    s->stride = (uint16_t)h;
    s->clip = lv_obj_create(root);
    lv_obj_remove_style_all(s->clip);
    lv_obj_set_size(s->clip, BOOT_TICK_W, h);
    lv_obj_set_pos(s->clip, BOOT_TICK_X, BOOT_TICK_Y);
    lv_obj_set_style_bg_color(s->clip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s->clip, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(s->clip, true, 0);
    lv_obj_remove_flag(s->clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(s->clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(s->clip, LV_SCROLLBAR_MODE_OFF);

    s->lab = helm_label(s->clip, tf, 0x000000, "");
    lv_obj_set_user_data(s->lab, s);
    lv_obj_set_style_text_align(s->lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s->lab, LV_SIZE_CONTENT);
    lv_obj_align(s->lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_translate_y(s->lab, (lv_coord_t)s->stride, 0);
    helm_splash_lab_opa(s, LV_OPA_TRANSP);

    s->bar = lv_obj_create(root);
    lv_obj_remove_style_all(s->bar);
    lv_obj_set_size(s->bar, BOOT_BAR_W, BOOT_BAR_H);
    lv_obj_set_pos(s->bar, BOOT_BAR_X, BOOT_BAR_Y);
    lv_obj_set_style_bg_color(s->bar, lv_color_make(0xD0, 0xD0, 0xD0), 0);
    lv_obj_set_style_bg_opa(s->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s->bar, 2, 0);
    lv_obj_set_style_clip_corner(s->bar, true, 0);
    lv_obj_clear_flag(s->bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s->bar_fill = lv_obj_create(s->bar);
    lv_obj_remove_style_all(s->bar_fill);
    lv_obj_set_size(s->bar_fill, 0, BOOT_BAR_H);
    lv_obj_set_style_bg_color(s->bar_fill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s->bar_fill, LV_OPA_COVER, 0);
    lv_obj_align(s->bar_fill, LV_ALIGN_LEFT_MID, 0, 0);
}

/** @brief 背景与文字/进度条按灰阶反相。 */
void helm_splash_set_gray(helm_splash_t *s, uint8_t g)
{
    uint8_t ink;
    uint8_t track;

    if (s == NULL || s->root == NULL) {
        return;
    }

    s->gray = g;
    ink = (uint8_t)(255u - g);
    track = (uint8_t)(0x38u + ((uint16_t)g * (0xD0u - 0x38u)) / 255u);

    lv_obj_set_style_bg_color(s->root, lv_color_make(g, g, g), 0);
    lv_obj_set_style_bg_opa(s->root, LV_OPA_COVER, 0);

    if (s->clip != NULL) {
        lv_obj_set_style_bg_color(s->clip, lv_color_make(g, g, g), 0);
        lv_obj_set_style_bg_opa(s->clip, LV_OPA_COVER, 0);
    }

    if (s->name != NULL) {
        lv_obj_set_style_text_color(s->name, lv_color_make(ink, ink, ink), 0);
    }

    if (s->lab != NULL) {
        lv_obj_set_style_text_color(s->lab, lv_color_make(ink, ink, ink), 0);
    }

    if (s->bar != NULL) {
        lv_obj_set_style_bg_color(s->bar, lv_color_make(track, track, track), 0);
    }

    if (s->bar_fill != NULL) {
        lv_obj_set_style_bg_color(s->bar_fill, lv_color_make(ink, ink, ink), 0);
    }
}

/** @brief 按 HELM_SPLASH_BG_STEPS 把背景设到某一阶。 */
void helm_splash_set_step(helm_splash_t *s, uint8_t step)
{
    uint8_t g;

    if (step > HELM_SPLASH_BG_STEPS) {
        step = HELM_SPLASH_BG_STEPS;
    }

    g = (uint8_t)((step * 255u) / HELM_SPLASH_BG_STEPS);
    helm_splash_set_gray(s, g);
}

/** @brief 事项行改用指定字体（TTF 就绪后切系统字）。 */
void helm_splash_set_tick_font(helm_splash_t *s, const lv_font_t *font)
{
    if (s == NULL || s->lab == NULL || font == NULL) {
        return;
    }

    lv_obj_set_style_text_font(s->lab, font, 0);
}

/** @brief 隐藏或显示事项行裁剪窗与进度条。 */
void helm_splash_set_chrome_hidden(helm_splash_t *s, bool hide)
{
    if (s == NULL) {
        return;
    }

    if (s->clip != NULL) {
        if (hide) {
            lv_obj_add_flag(s->clip, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s->clip, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s->bar != NULL) {
        if (hide) {
            lv_obj_add_flag(s->bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s->bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** @brief 事项行与进度条整体不透明度。 */
void helm_splash_set_chrome_opa(helm_splash_t *s, lv_opa_t o)
{
    if (s == NULL) {
        return;
    }

    if (s->clip != NULL) {
        lv_obj_set_style_opa(s->clip, o, 0);
    }

    if (s->bar != NULL) {
        lv_obj_set_style_opa(s->bar, o, 0);
    }
}

/** @brief 关机刷黑前去掉 logo、品名和 chrome。 */
void helm_splash_hide_brand(helm_splash_t *s)
{
    if (s == NULL) {
        return;
    }

    if (s->logo != NULL) {
        lv_obj_add_flag(s->logo, LV_OBJ_FLAG_HIDDEN);
    }

    if (s->name != NULL) {
        lv_obj_add_flag(s->name, LV_OBJ_FLAG_HIDDEN);
    }

    helm_splash_set_chrome_hidden(s, true);
}

/** @brief 立刻改整行文字；busy 时 pump 追加省略点。 */
void helm_splash_set_line(helm_splash_t *s, const char *txt, bool busy)
{
    if (s == NULL || s->lab == NULL) {
        return;
    }

    s->busy = busy;
    lv_snprintf(s->shown, sizeof(s->shown), "%s", txt ? txt : "");
    helm_splash_paint_label(s);
}

/** @brief 立刻改整行文字，不加省略点。 */
void helm_splash_set_text(helm_splash_t *s, const char *txt)
{
    helm_splash_set_line(s, txt, false);
}

/** @brief 立刻改成 `key  ·  val`。 */
void helm_splash_set_row(helm_splash_t *s, const char *key, const char *val,
                         bool busy)
{
    char line[48];

    lv_snprintf(line, sizeof(line), "%s  ·  %s",
                (key != NULL && key[0] != '\0') ? key : "",
                val ? val : "");
    helm_splash_set_line(s, line, busy);
}

/** @brief 刷新进行中的省略点。 */
void helm_splash_pump(helm_splash_t *s)
{
    if (s == NULL || !s->busy || s->lab == NULL) {
        return;
    }

    helm_splash_paint_label(s);
}

/** @brief 当前行从裁剪窗下方滚入。 */
void helm_splash_slide_in(helm_splash_t *s)
{
    if (s == NULL) {
        return;
    }

    s->tick_on = true;
    s->out_busy = false;
    helm_splash_set_chrome_hidden(s, false);
    helm_splash_slide_anim(s, (int32_t)s->stride, 0, 0, 255, false);
}

/** @brief 当前行向上滚出。 */
void helm_splash_slide_out(helm_splash_t *s)
{
    if (s == NULL || s->lab == NULL) {
        return;
    }

    s->out_busy = true;
    helm_splash_slide_anim(s, 0, -((int32_t)s->stride), 255, 0, true);
}

/** @brief 切到新整行：已有一行则先滚出再滚入。 */
void helm_splash_show(helm_splash_t *s, const char *txt)
{
    if (s == NULL || txt == NULL) {
        return;
    }

    if (s->shown[0] != '\0' && strcmp(s->shown, txt) == 0 &&
        s->pending[0] == '\0') {
        return;
    }

    if (!s->tick_on && !s->out_busy) {
        helm_splash_set_line(s, txt, true);
        helm_splash_slide_in(s);
        return;
    }

    lv_snprintf(s->pending, sizeof(s->pending), "%s", txt);
    if (!s->out_busy) {
        helm_splash_slide_out(s);
    }
}

/** @brief 进度条立刻跳到 pct。 */
void helm_splash_bar_set(helm_splash_t *s, uint8_t pct)
{
    lv_coord_t w;

    if (s == NULL) {
        return;
    }

    if (pct > 100u) {
        pct = 100u;
    }

    s->bar_pct = pct;
    if (s->bar_fill == NULL) {
        return;
    }

    w = (lv_coord_t)(((int)BOOT_BAR_W * (int)pct) / 100);
    lv_obj_set_width(s->bar_fill, w);
}

/** @brief 进度条在 ms 内线性从 from 走到 to。 */
void helm_splash_bar_linear(helm_splash_t *s, uint8_t from, uint8_t to,
                           uint32_t ms)
{
    lv_anim_t a;

    if (s == NULL || s->bar == NULL) {
        return;
    }

    lv_anim_delete(s, helm_splash_bar_exec);
    helm_splash_bar_set(s, from);
    lv_anim_init(&a);
    lv_anim_set_var(&a, s);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms == 0u ? 1u : ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, helm_splash_bar_exec);
    lv_anim_start(&a);
}

/** @brief 停掉本 splash 上的动画并清零结构。 */
void helm_splash_clear(helm_splash_t *s)
{
    if (s == NULL) {
        return;
    }

    if (s->lab != NULL) {
        lv_anim_delete(s->lab, NULL);
    }

    lv_anim_delete(s, NULL);
    memset(s, 0, sizeof(*s));
}
