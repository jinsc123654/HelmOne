/**
 * @file bicycle_ts_overlay.c
 * @brief 自行车 UI — ts_overlay。
 */

#include "bicycle_ts_overlay.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <time.h>

#ifndef BICYCLE_TS_OVERLAY_PERIOD_MS
#  define BICYCLE_TS_OVERLAY_PERIOD_MS 100u
#endif

static lv_obj_t* s_label;
static lv_timer_t* s_timer;

static void bicycle_ts_overlay_update(lv_timer_t* timer)
{
    (void)timer;
    if (!s_label) {
        return;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        lv_label_set_text(s_label, "[ ts err ]");
        return;
    }

    char buf[28];
    snprintf(buf, sizeof(buf), "[ %lu.%06lu]",
        (unsigned long)ts.tv_sec,
        (unsigned long)(ts.tv_nsec / 1000L));
    lv_label_set_text(s_label, buf);
}

/**
 * @brief 自行车 ts overlay init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_ts_overlay_init(void)
{
    if (s_label) {
        return;
    }

    lv_obj_t* top = lv_layer_top();
    s_label = lv_label_create(top);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_color(s_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_label, 4, 0);
    lv_obj_set_style_pad_ver(s_label, 2, 0);
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_obj_clear_flag(s_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_label);

    s_timer = lv_timer_create(bicycle_ts_overlay_update,
        BICYCLE_TS_OVERLAY_PERIOD_MS, NULL);
    bicycle_ts_overlay_update(s_timer);
}
