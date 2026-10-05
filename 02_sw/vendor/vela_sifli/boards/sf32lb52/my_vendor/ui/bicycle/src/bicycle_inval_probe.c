/**
 * @file bicycle_inval_probe.c
 * @brief 自行车 UI — inval_probe。
 */

#include "bicycle_inval_probe.h"

#include <nuttx/config.h>
#include <syslog.h>

#include "src/widgets/canvas/lv_canvas.h"
#include "src/widgets/label/lv_label.h"

#ifndef BICYCLE_INVAL_LOG_RATE_MS
#define BICYCLE_INVAL_LOG_RATE_MS 1000u
#endif

#ifndef BICYCLE_INVAL_LOG_BURST
#define BICYCLE_INVAL_LOG_BURST 12u
#endif

#ifndef BICYCLE_INVAL_SUMMARY_MS
#define BICYCLE_INVAL_SUMMARY_MS 5000u
#endif

static const char * s_tag;
static uint32_t s_win_start;
static uint32_t s_win_logged;

static uint32_t s_obj_inval_total;
static uint32_t s_area_inval_total;
static uint32_t s_refr_req_total;
static uint32_t s_refr_start_total;
static uint32_t s_flush_total;

static const char * inval_obj_kind(const lv_obj_t * obj)
{
    if (obj == NULL) {
        return "?";
    }
    if (lv_obj_check_type(obj, &lv_canvas_class)) {
        return "canvas";
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        return "label";
    }
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        return "obj";
    }
    return "widget";
}

static bool inval_should_log_detail(void)
{
    uint32_t now = lv_tick_get();

    if (s_win_start == 0 || lv_tick_elaps(s_win_start) >= BICYCLE_INVAL_LOG_RATE_MS) {
        s_win_start = now;
        s_win_logged = 0;
    }

    if (s_win_logged < BICYCLE_INVAL_LOG_BURST) {
        s_win_logged++;
        return true;
    }

    return false;
}

/**
 * @brief 自行车 lv inval probe。
 */
void bicycle_lv_inval_probe(const lv_obj_t * obj, const lv_area_t * area)
{
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    const char * tag = s_tag ? s_tag : "-";

    s_tag = NULL;
    s_obj_inval_total++;

    if (!inval_should_log_detail()) {
        return;
    }

    const char * kind = inval_obj_kind(obj);
    LV_LOG_WARN("[inval] obj %s obj=%p parent=%p area=(%d,%d)-(%d,%d) tag=%s ra=0x%x",
        kind, (void *)obj,
        obj ? (void *)lv_obj_get_parent(obj) : NULL,
        area ? (int)area->x1 : 0, area ? (int)area->y1 : 0,
        area ? (int)area->x2 : 0, area ? (int)area->y2 : 0,
        tag, (unsigned)ra);
    syslog(LOG_WARNING,
        "[inval] obj %s obj=%p area=(%d,%d)-(%d,%d) tag=%s ra=0x%x",
        kind, (void *)obj,
        area ? (int)area->x1 : 0, area ? (int)area->y1 : 0,
        area ? (int)area->x2 : 0, area ? (int)area->y2 : 0,
        tag, (unsigned)ra);
}

/**
 * @brief 自行车 inval probe tag。
 */
void bicycle_inval_probe_tag(const char * tag)
{
    s_tag = tag;
}

static void bicycle_inval_disp_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
    case LV_EVENT_INVALIDATE_AREA: {
        lv_area_t * area = lv_event_get_invalidated_area(e);
        s_area_inval_total++;
        if (!inval_should_log_detail()) {
            break;
        }
        if (area == NULL) {
            break;
        }
        LV_LOG_WARN("[inval] display area=(%d,%d)-(%d,%d) size=%dx%d",
            (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2,
            (int)(area->x2 - area->x1 + 1), (int)(area->y2 - area->y1 + 1));
        syslog(LOG_WARNING,
            "[inval] display area=(%d,%d)-(%d,%d) size=%dx%d",
            (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2,
            (int)(area->x2 - area->x1 + 1), (int)(area->y2 - area->y1 + 1));
        break;
    }
    case LV_EVENT_REFR_REQUEST:
        s_refr_req_total++;
        break;
    case LV_EVENT_REFR_START:
        s_refr_start_total++;
        break;
    case LV_EVENT_FLUSH_START:
        s_flush_total++;
        if (!inval_should_log_detail()) {
            break;
        }
        LV_LOG_WARN("[inval] FLUSH_START (LCD push) total=%u",
            (unsigned)s_flush_total);
        syslog(LOG_WARNING, "[inval] FLUSH_START (LCD push) total=%u",
            (unsigned)s_flush_total);
        break;
    default:
        break;
    }
}

static void bicycle_inval_summary_timer(lv_timer_t * tmr)
{
    LV_UNUSED(tmr);

    LV_LOG_WARN(
        "[inval] summary 5s: obj=%u area=%u refr_req=%u refr_start=%u flush=%u",
        (unsigned)s_obj_inval_total, (unsigned)s_area_inval_total,
        (unsigned)s_refr_req_total, (unsigned)s_refr_start_total,
        (unsigned)s_flush_total);
    syslog(LOG_WARNING,
        "[inval] summary 5s: obj=%u area=%u refr_req=%u refr_start=%u flush=%u",
        (unsigned)s_obj_inval_total, (unsigned)s_area_inval_total,
        (unsigned)s_refr_req_total, (unsigned)s_refr_start_total,
        (unsigned)s_flush_total);

    s_obj_inval_total = 0;
    s_area_inval_total = 0;
    s_refr_req_total = 0;
    s_refr_start_total = 0;
    s_flush_total = 0;
    s_win_logged = 0;
}

/**
 * @brief 自行车 inval probe init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_inval_probe_init(lv_display_t * disp)
{
    lv_display_add_event_cb(disp, bicycle_inval_disp_event, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, bicycle_inval_disp_event, LV_EVENT_REFR_REQUEST, NULL);
    lv_display_add_event_cb(disp, bicycle_inval_disp_event, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, bicycle_inval_disp_event, LV_EVENT_FLUSH_START, NULL);
    lv_timer_create(bicycle_inval_summary_timer, BICYCLE_INVAL_SUMMARY_MS, NULL);
    LV_LOG_INFO("[inval] probe on — watch [inval] lines in log");
    syslog(LOG_NOTICE, "[inval] probe on");
}
