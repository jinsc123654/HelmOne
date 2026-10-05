/**
 * @file bicycle_c_debug.c
 * @brief 自行车 UI — c_debug。
 */

#include "bicycle_c_debug.h"

#include "vmap/vmap_track.h"
#include "vmap/vmap_view.h"
#include "vmap/vmap_config.h"

#include <nuttx/config.h>
#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP \
    && defined(CONFIG_MYVENDOR_LCD_DISP_DEBUG) && CONFIG_MYVENDOR_LCD_DISP_DEBUG
#include "myvendor_lcd_disp.h"
#endif

#include <syslog.h>
#include <time.h>

#include "lvgl/lvgl.h"

#ifndef BICYCLE_C_DEBUG_PERIOD_MS
#define BICYCLE_C_DEBUG_PERIOD_MS 5000u
#endif

static struct timespec s_win_ts;
static bool s_win_valid;

static uint32_t s_loops;
static uint32_t s_idle_min;
static uint32_t s_gpx_ticks;
static uint32_t s_track_push;
static uint32_t s_track_push_skip;
static uint32_t s_track_draw;
static uint32_t s_track_skip;
static uint32_t s_map_pan;
static uint32_t s_map_render;
static uint32_t s_lap_events;

static uint16_t s_pts;
static uint16_t s_draw_n;
static uint16_t s_km;
static uint16_t s_lap;
static uint32_t s_map_rev;
static uint32_t s_render_rev;
#if VMAP_TRACK_ENABLE
static bool s_track_rec;
static uint32_t s_lap_dist_m;
static uint32_t s_session_m;
static uint16_t s_pts_cap;
#endif

static uint64_t debug_ts_delta_ns(const struct timespec * a, const struct timespec * b)
{
    int64_t sec = (int64_t)b->tv_sec - (int64_t)a->tv_sec;
    int64_t nsec = (int64_t)b->tv_nsec - (int64_t)a->tv_nsec;

    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }

    return (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
}

static uint16_t debug_count_lv_timers(void)
{
    uint16_t n = 0;
    lv_timer_t * t = lv_timer_get_next(NULL);

    while (t) {
        n++;
        t = lv_timer_get_next(t);
    }

    return n;
}

static void debug_reset_window(const struct timespec * now)
{
    s_win_ts = *now;
    s_win_valid = true;
    s_loops = 0;
    s_idle_min = UINT32_MAX;
    s_gpx_ticks = 0;
    s_track_push = 0;
    s_track_push_skip = 0;
    s_track_draw = 0;
    s_track_skip = 0;
    s_map_pan = 0;
    s_map_render = 0;
    s_lap_events = 0;
}

static void debug_emit(const struct timespec * now)
{
    uint64_t win_ns = debug_ts_delta_ns(&s_win_ts, now);
    uint32_t win_ms = (uint32_t)(win_ns / 1000000ULL);
    uint32_t idle_min = (s_idle_min == UINT32_MAX) ? 0 : s_idle_min;
    uint32_t loop_hz = win_ms ? (uint32_t)((uint64_t)s_loops * 1000ULL / win_ms) : 0;

    syslog(LOG_NOTICE,
        "[bicycle] t=%llu.%06llu win=%u.%03us loop=%uHz idle_min=%ums "
        "gpx=%u push=%u skip=%u track_draw=%u track_skip=%u "
        "map_pan=%u map_render=%u lap_evt=%u lv_timers=%u "
        "pts=%u draw=%u km=%u lap=%u"
#if VMAP_TRACK_ENABLE
        " cap=%u rec=%u sess_m=%u lap_m=%u"
#endif
        " map_rev=%u render_rev=%u",
        (unsigned long long)now->tv_sec,
        (unsigned long long)(now->tv_nsec / 1000LL),
        (unsigned)(win_ms / 1000u), (unsigned)(win_ms % 1000u),
        (unsigned)loop_hz, (unsigned)idle_min,
        (unsigned)s_gpx_ticks,
        (unsigned)s_track_push, (unsigned)s_track_push_skip,
        (unsigned)s_track_draw, (unsigned)s_track_skip,
        (unsigned)s_map_pan, (unsigned)s_map_render,
        (unsigned)s_lap_events, (unsigned)debug_count_lv_timers(),
        (unsigned)s_pts, (unsigned)s_draw_n, (unsigned)s_km,
        (unsigned)s_lap
#if VMAP_TRACK_ENABLE
        , (unsigned)s_pts_cap, (unsigned)s_track_rec, (unsigned)s_session_m,
        (unsigned)s_lap_dist_m
#endif
        , (unsigned)s_map_rev, (unsigned)s_render_rev);

    LV_LOG_INFO(
        "[bicycle] loop=%uHz idle_min=%ums gpx=%u push=%u draw=%u skip=%u "
        "map_pan=%u render=%u pts=%u lap=%u"
#if VMAP_TRACK_ENABLE
        " rec=%u sess_m=%u lap_m=%u"
#endif
        " timers=%u",
        (unsigned)loop_hz, (unsigned)idle_min,
        (unsigned)s_gpx_ticks, (unsigned)s_track_push,
        (unsigned)s_track_draw, (unsigned)s_track_skip,
        (unsigned)s_map_pan, (unsigned)s_map_render,
        (unsigned)s_pts, (unsigned)s_lap
#if VMAP_TRACK_ENABLE
        , (unsigned)s_track_rec, (unsigned)s_session_m, (unsigned)s_lap_dist_m
#endif
        , (unsigned)debug_count_lv_timers());

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP \
    && defined(CONFIG_MYVENDOR_LCD_DISP_DEBUG) && CONFIG_MYVENDOR_LCD_DISP_DEBUG
    {
        myvendor_lcd_disp_stats_t lcd;

        myvendor_lcd_disp_stats_snapshot(&lcd);
        syslog(LOG_NOTICE,
            "[lcd_flush] submit_ok=%u busy=%u err=%u worker=%u ready=%u "
            "wait=%u wait_max=%ums stall=%u timeout=%u force=%u recover=%u "
            "putarea_max=%ums putarea_slow=%u putarea_fail=%u busy_now=%u",
            (unsigned)lcd.submit_ok, (unsigned)lcd.submit_busy,
            (unsigned)lcd.submit_err,
            (unsigned)lcd.worker_done, (unsigned)lcd.flush_ready,
            (unsigned)lcd.wait_calls, (unsigned)lcd.wait_ms_max,
            (unsigned)lcd.wait_stall,
            (unsigned)lcd.wait_timeout, (unsigned)lcd.flush_ready_force,
            (unsigned)lcd.recover,
            (unsigned)lcd.putarea_ms_max, (unsigned)lcd.putarea_slow,
            (unsigned)lcd.putarea_fail,
            (unsigned)myvendor_lcd_disp_flush_busy());
        myvendor_lcd_disp_stats_reset_window();
    }
#endif
}

/**
 * @brief 自行车 c debug init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_c_debug_init(void)
{
    s_win_valid = false;
    syslog(LOG_NOTICE,
        "[bicycle] debug on — [bicycle]/[handler]/[inval]/[lcd_flush] every 5s");
}

/**
 * @brief 自行车 c debug loop。
 */
void bicycle_c_debug_loop(uint32_t lv_idle_ms)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!s_win_valid) {
        debug_reset_window(&now);
    }

    s_loops++;
    if (lv_idle_ms < s_idle_min) {
        s_idle_min = lv_idle_ms;
    }

    if (debug_ts_delta_ns(&s_win_ts, &now) >=
            (uint64_t)BICYCLE_C_DEBUG_PERIOD_MS * 1000000ULL) {
        debug_emit(&now);
        debug_reset_window(&now);
    }
}

/**
 * @brief 自行车 c debug gpx tick。
 */
void bicycle_c_debug_gpx_tick(void)
{
    s_gpx_ticks++;
}

/**
 * @brief 自行车 c debug track push。
 */
void bicycle_c_debug_track_push(bool accepted)
{
    if (accepted) {
        s_track_push++;
    } else {
        s_track_push_skip++;
    }
}

/**
 * @brief 自行车 c debug track refresh。
 */
void bicycle_c_debug_track_refresh(bool drew)
{
    if (drew) {
        s_track_draw++;
    } else {
        s_track_skip++;
    }
}

/**
 * @brief 自行车 c debug track state。
 */
void bicycle_c_debug_track_state(uint16_t draw_n, uint16_t km_count,
    double lap_distance_m)
{
    s_draw_n = draw_n;
    s_km = km_count;
#if VMAP_TRACK_ENABLE
    s_lap_dist_m = (uint32_t)lap_distance_m;
#else
    (void)lap_distance_m;
#endif
}

/**
 * @brief 自行车 c debug track lap。
 */
void bicycle_c_debug_track_lap(uint16_t lap)
{
    s_lap_events++;
    s_lap = lap;
}

/**
 * @brief 自行车 c debug map soft pan。
 */
void bicycle_c_debug_map_soft_pan(void)
{
    s_map_pan++;
}

/**
 * @brief 自行车 c debug map full render。
 */
void bicycle_c_debug_map_full_render(void)
{
    s_map_render++;
}

/**
 * @brief 自行车 c debug snap。
 */
void bicycle_c_debug_snap(const vmap_view_t * map, const vmap_track_t * track)
{
    if (map) {
        s_map_rev = vmap_view_map_revision(map);
        s_render_rev = vmap_view_render_revision(map);
    }

#if VMAP_TRACK_ENABLE
    if (track) {
        s_pts = vmap_track_point_count(track);
        s_draw_n = vmap_track_draw_point_count(track);
        s_km = vmap_track_km_marker_count(track);
        s_lap = vmap_track_lap_count(track);
        s_pts_cap = vmap_track_point_capacity(track);
        s_lap_dist_m = (uint32_t)vmap_track_lap_distance_m(track);
        s_session_m = (uint32_t)vmap_track_session_distance_m(track);
        s_track_rec = vmap_track_is_recording(track);
    } else {
        s_pts = 0;
        s_draw_n = 0;
        s_km = 0;
        s_lap = 0;
        s_pts_cap = 0;
        s_lap_dist_m = 0;
        s_session_m = 0;
        s_track_rec = false;
    }
#else
    (void)track;
#endif
}
