/**
 * @file helm_font_lab.c
 * @brief 字体试验页：位图套数随 ftest_fonts.h，KEY1 下一套 / KEY2 上一套。
 *
 * 对照量产苹方特粗 2bpp；其余为已生成的米黑 / 普惠 / 思源 / Noto / 苹方常规。
 */

#include "helm_font_lab.h"

#include "fonts/ftest/ftest_fonts.h"
#include "helm_font.h"
#include "helm_palette.h"
#include "helm_widget.h"

typedef struct {
    const char * name;
    const char * meta;
    const char * note;
    const lv_font_t * lab;
    const lv_font_t * title;
    const lv_font_t * num;
} helm_ftest_t;

typedef struct {
    lv_obj_t * panel;
    lv_obj_t * idx;
    lv_obj_t * name;
    lv_obj_t * meta;
    lv_obj_t * t1;
    lv_obj_t * s1;
    lv_obj_t * t2;
    lv_obj_t * s2;
    lv_obj_t * num;
    lv_obj_t * unit;
    lv_obj_t * hint;
} helm_flab_ui_t;

static const helm_ftest_t s_set[] = {
    { "小米黑体中", "2bpp 强hint", "推荐 小屏中等",
      &ftest_mism2_12, &ftest_mism2_15, &ftest_mism2_32 },
    { "小米黑体中", "4bpp 强hint", "对照 更高灰度",
      &ftest_mism4_12, &ftest_mism4_15, &ftest_mism4_32 },
    { "普惠体中", "2bpp 强hint", "小字中等",
      &ftest_puhm2_12, &ftest_puhm2_15, &ftest_puhm2_32 },
    { "小米黑体常", "2bpp 强hint", "Regular",
      &ftest_misr2_12, &ftest_misr2_15, &ftest_misr2_32 },
    { "小米黑体次粗", "2bpp 强hint", "Demibold",
      &ftest_misd2_12, &ftest_misd2_15, &ftest_misd2_32 },
    { "普惠体常", "2bpp 强hint", "Regular",
      &ftest_puhr2_12, &ftest_puhr2_15, &ftest_puhr2_32 },
    { "谷歌黑体中", "2bpp 强hint", "Noto Medium",
      &ftest_notom2_12, &ftest_notom2_15, &ftest_notom2_32 },
    { "思源黑体常", "2bpp 强hint", "适合小字菜单",
      &ftest_shsr2_12, &ftest_shsr2_15, &ftest_shsr2_32 },
    { "苹方常规", "2bpp 强hint", "对照",
      &ftest_pfm2_12, &ftest_pfm2_15, &ftest_pfm2_32 },
    { "苹方特粗", "2bpp 强hint", "当前量产",
      &ftest_pfh2_12, &ftest_pfh2_15, &ftest_pfh2_32 },
};

#define HELM_FTEST_N  ((uint8_t)(sizeof(s_set) / sizeof(s_set[0])))

static helm_flab_ui_t s_ui;
static uint8_t s_idx;

static void helm_flab_set_font(lv_obj_t * obj, const lv_font_t * font)
{
    if (obj && font) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

static lv_obj_t * helm_flab_row(lv_obj_t * parent, bool sel, const char * title,
                               const char * sub, lv_obj_t ** tlab, lv_obj_t ** slab)
{
    lv_obj_t * row;
    lv_obj_t * body;
    uint32_t fg = HELM_COLOR_INK;

    row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 50);
    lv_obj_set_style_radius(row, HELM_RADIUS, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_set_style_bg_color(row,
        helm_color(sel ? HELM_COLOR_NAV : HELM_COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    body = row;
    *tlab = helm_label(body, helm_font_title(), fg, title);
    lv_label_set_long_mode(*tlab, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(*tlab, lv_pct(100));
    *slab = helm_label(body, helm_font_lab(), fg, sub);
    lv_label_set_long_mode(*slab, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(*slab, lv_pct(100));
    return row;
}

void helm_font_lab_refresh(void)
{
    const helm_ftest_t * v;
    char buf[40];

    if (s_ui.panel == NULL) {
        return;
    }

    if (s_idx >= HELM_FTEST_N) {
        s_idx = 0;
    }

    v = &s_set[s_idx];
    lv_snprintf(buf, sizeof(buf), "%u/%u  %s",
                (unsigned)(s_idx + 1u), (unsigned)HELM_FTEST_N,
                v->note ? v->note : "");
    lv_label_set_text(s_ui.idx, buf);
    lv_label_set_text(s_ui.name, v->name);
    lv_label_set_text(s_ui.meta, v->meta);

    helm_flab_set_font(s_ui.idx, v->lab);
    helm_flab_set_font(s_ui.name, v->title);
    helm_flab_set_font(s_ui.meta, v->lab);
    helm_flab_set_font(s_ui.t1, v->title);
    helm_flab_set_font(s_ui.s1, v->lab);
    helm_flab_set_font(s_ui.t2, v->title);
    helm_flab_set_font(s_ui.s2, v->lab);
    helm_flab_set_font(s_ui.num, v->num);
    helm_flab_set_font(s_ui.unit, v->lab);
    helm_flab_set_font(s_ui.hint, v->lab);
}

void helm_font_lab_next(void)
{
    s_idx = (uint8_t)((s_idx + 1u) % HELM_FTEST_N);
    helm_font_lab_refresh();
}

void helm_font_lab_prev(void)
{
    s_idx = (uint8_t)((s_idx + HELM_FTEST_N - 1u) % HELM_FTEST_N);
    helm_font_lab_refresh();
}

lv_obj_t * helm_font_lab_build(lv_obj_t * parent)
{
    lv_obj_t * numbox;

    s_ui.panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.panel);
    helm_grow_y(s_ui.panel);
    helm_style_scr(s_ui.panel);
    lv_obj_set_flex_flow(s_ui.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.panel, 6, 0);
    lv_obj_set_style_pad_row(s_ui.panel, 3, 0);
    lv_obj_add_flag(s_ui.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.panel, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.idx = helm_label(s_ui.panel, helm_font_lab(), HELM_COLOR_HAIR, "");
    s_ui.name = helm_label(s_ui.panel, helm_font_title(), HELM_COLOR_INK, "");
    s_ui.meta = helm_label(s_ui.panel, helm_font_lab(), HELM_COLOR_HAIR, "");

    helm_flab_row(s_ui.panel, true, "传感器", "0 已连接", &s_ui.t1, &s_ui.s1);
    helm_flab_row(s_ui.panel, false, "骑行记录", "暂无记录", &s_ui.t2, &s_ui.s2);

    numbox = lv_obj_create(s_ui.panel);
    lv_obj_remove_style_all(numbox);
    lv_obj_set_width(numbox, lv_pct(100));
    lv_obj_set_height(numbox, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(numbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(numbox, 4, 0);
    lv_obj_clear_flag(numbox, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.num = helm_label(numbox, helm_font_quad(), HELM_COLOR_INK, "17.1");
    s_ui.unit = helm_label(numbox, helm_font_lab(), HELM_COLOR_HAIR, "km/h");

    s_ui.hint = helm_label(s_ui.panel, helm_font_lab(), HELM_COLOR_HAIR, "");
    lv_obj_add_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);

    helm_font_lab_refresh();
    return s_ui.panel;
}
