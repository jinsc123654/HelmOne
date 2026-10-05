/**
 * @file map_page.c
 * @brief LiveMap：retained 画布、开机文件预载、Helm 翻页隐藏、导航/REC。
 *
 * 开机：空壳进栈，splash 底下 `boot_prepare` 只加载文件；淡入完成后才泵
 * 首帧，canvas 隐藏到 `render_finish`。还没进过地图页时禁止 GNSS
 * `set_center` rebase。
 * 约定见 `doc/splash.md` 与 `docs/map/LIVEMAP.md` §4.4。
 */

#include "map_page.h"

#include "bicycle_page_anima.h"
#include "helm_palette.h"
#include "helm_shell.h"
#include "myvendor_watchdog.h"
#include "bicycle_config.h"
#include "bicycle_gpx_sim.h"
#include "bicycle_runtime.h"
#include "bicycle_ride_gpx.h"
#include "bicycle_env.h"
#include "bicycle_c_debug.h"
#include "bicycle_page_ids.h"
#include "lv_pm_theme.h"
#include "lv_pm_port.h"
#include "lv_pm_bar.h"
#include "lv_pm_overlay.h"
#include "myvendor_devctl.h"
#include "myvendor_sound.h"
#include "gpx_decode.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_style.h"
#if VMAP_TRACK_ENABLE
#include "vmap/vmap_track.h"
#endif
#if VMAP_ROUTE_ENABLE
#include "vmap/vmap_route.h"
#include "vmap/vmap_route_graph.h"
#include "vmap/vmap_route_nav.h"
#include "vmap/vmap_route_worker.h"
#include "vmap/vmap_route_log.h"
#include "vmap/vmap_route_trip.h"
#include "vmap/vmap_map_catalog.h"
#include "vmap/vmap_grid.h"
#include "vmap/vmap_tile_cache.h"
#include "vmap/vmap_view.h"
#include "vmap/vmap_geo.h"
#include "vmap/vmap_format.h"
#include "vmap/vmap_alloc.h"
#include <myvendor_mtp_lfs.h>
#include "myvendor_watchdog.h"
#endif
#include "myvendor_bicycle_ctl.h"
#include "myvendor_sys.h"
#include "myvendor_system_font.h"
#include "NaviArrow.h"
#include "Vendor/Board/lv_port/lv_port_buttons.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <syslog.h>
#include <unistd.h>

#if !LV_USE_OBSERVER
#error "bicycle map UI requires LV_USE_OBSERVER (lv_subject bindings)"
#endif

#define MAP_PAGE_FOLLOW_UPDATE_MS  200UL
/** 非地图页仍低频维护大画布；只在越过 canvas margin 后触发瓦片 pump。 */
#define MAP_PAGE_HIDDEN_FOLLOW_MS 1000UL
#define MAP_PAGE_RENDER_PUMP_MS    20u
#define MAP_PAGE_TRIP_NAME_LEN      24u
/** 隐藏地图时 DEM 全表最近邻太贵，1s 一次够给气压计定标。 */
#define MAP_PAGE_DEM_MS            1000u
/** 刚翻进地图：先露出上一帧画布，300ms 后再跟车重绘。 */
#define MAP_PAGE_QUIET_MS          300u
/** 位移小于此则复用上次 DEM 节点，避免每拍 haversine 扫全图。 */
#define MAP_PAGE_DEM_CACHE_M       25.0

struct map_page {
    lv_obj_t * root;
#if VMAP_TRACK_ENABLE
    lv_obj_t * lap_lbl;
    lv_obj_t * rec_lbl;
#endif
    lv_obj_t * map_area;
    vmap_view_t * map;
#if VMAP_TRACK_ENABLE
    vmap_track_t * track;
#endif
    lv_obj_t * panel;
    lv_obj_t * speed_lbl;
    lv_obj_t * heading_lbl;
    lv_obj_t * sensor_lbl;
#if VMAP_TRACK_ENABLE
    lv_obj_t * dist_lbl;
#endif
    lv_timer_t * gpx_timer;
    lv_timer_t * render_timer;
    lv_pm_theme_id_t map_theme_id;
    bool covered;            /**< splash / 菜单盖住：停 render_timer。 */
    bool map_ui_visible;     /**< Helm 当前是否显示地图区。 */
    bool map_ui_seen;        /**< 开机后是否进过地图页；未进过禁止隐藏跟车 rebase。 */
    bool map_refresh_pending;
    uint32_t map_last_follow_ms;
    uint32_t map_quiet_until;
    uint32_t dem_last_ms;
#if VMAP_ROUTE_ENABLE
    vmap_route_t * route;
    vmap_route_nav_t * nav;
    lv_obj_t * nav_lbl;
    lv_obj_t * plan_hint;
    lv_timer_t * plan_hint_timer;
    char plan_hint_base[32];
    uint8_t plan_hint_phase;
    bool nav_planned;
    bool nav_user_set;
    bool nav_planning;
    bool nav_replanning;
    vmap_route_reroute_reason_t nav_reroute_reason;
    double nav_plan_from_lon;
    double nav_plan_from_lat;
    double nav_plan_to_lon;
    double nav_plan_to_lat;
    uint32_t nav_maneuver_announced;
    uint32_t nav_last_update_ms;
    vmap_route_trip_t nav_trip;
    char nav_trip_names[VMAP_ROUTE_TRIP_MAX_WAYPOINTS][MAP_PAGE_TRIP_NAME_LEN];
    uint8_t nav_trip_name_count;
    bool nav_trip_active;
    bool nav_top_only;
    bool nav_refine_extend;
    bool nav_trip_silent;
    bool nav_trip_ahead_planning;
    bool nav_trip_ahead_ready;
    uint32_t nav_trip_ahead_dest_idx;
    vmap_cell_id_t nav_trip_ahead_path[VMAP_ROUTE_REGION_PATH_MAX];
    uint32_t nav_trip_ahead_path_len;
    vmap_route_pt_t * nav_trip_ahead_pts;
    uint32_t nav_trip_ahead_n;
    double nav_trip_ahead_m;
    vmap_route_maneuver_t nav_trip_ahead_mans[VMAP_ROUTE_MAX_MANEUVERS];
    uint32_t nav_trip_ahead_man_n;
    uint32_t nav_roll_begin;
    bool nav_route_sim;
    uint32_t nav_last_reroute_ms;
    double nav_last_reroute_lon;
    double nav_last_reroute_lat;
    uint32_t nav_off_since_ms;
    bool nav_arrive_hold;
    /** 到点后 5 s 清屏定时器是否已挂（见 map_page_nav_end_clear_cb）。 */
    bool nav_end_clear_armed;
    bool nav_gpx_course;
    bool nav_gpx_rejoining;
    bool nav_mtp_held;
    uint32_t nav_gpx_off_since_ms;
    bool nav_gpx_off_warned;
    vmap_route_pt_t * nav_gpx_suffix;
    uint32_t nav_gpx_suffix_count;
    vmap_route_pt_t * nav_gpx_pts;
    uint32_t nav_gpx_n;
    double nav_gpx_total_m;
    double nav_gpx_true_m;
    double nav_gpx_win_start_m;
    double nav_gpx_win_end_m;
    bool nav_gpx_dense;
    bool nav_gpx_sparse;
    bool nav_gpx_reverse;
    bool nav_review;
    char nav_title[24];
#endif
};

static map_page_t * s_live_map;
static lv_pm_page_t s_live_map_page;
static bool s_boot_deferred;
static bool s_boot_center_ready;
static bool s_boot_files_ready;
#if VMAP_ROUTE_ENABLE
static vmap_route_graph_t * s_route_graph;
static vmap_cell_id_t s_graph_region = VMAP_CELL_NONE;
static uint32_t s_dem_node = UINT32_MAX;
static double s_dem_lon;
static double s_dem_lat;

static void map_page_dem_cache_clear(void)
{
    s_dem_node = UINT32_MAX;
}

#define MAP_PAGE_GPX_OFF_CONFIRM_MS       3000u
#define MAP_PAGE_GPX_REROUTE_INTERVAL_MS 15000u
/** @brief 返航时对向车道 + GNSS 误差的横向走廊（双向 8 车道约 30–70 m）。 */
#define MAP_PAGE_GPX_RETURN_OFF_M         100.0

static bool map_page_nav_gpx_rejoin_begin(map_page_t * page,
    const vmap_route_nav_state_t * st, double lon, double lat);
static double map_page_gpx_nearest_along(const vmap_route_pt_t * pts, uint32_t n,
    double lon, double lat, double * dist_m);
static double map_page_gpx_off_threshold(const map_page_t * page);
static void map_page_gpx_course_clear(map_page_t * page);
static bool map_page_gpx_tick_window(map_page_t * page,
    const vmap_route_nav_state_t * st, double lon, double lat);
static double map_page_nav_remain_display_m(const map_page_t * page,
    const vmap_route_nav_state_t * st);
static void map_page_trip_ahead_clear(map_page_t * page);
static bool map_page_trip_ahead_kick(map_page_t * page);
static bool map_page_trip_apply_ahead(map_page_t * page, double lon, double lat);

static void map_page_gpx_rejoin_reset(map_page_t * page)
{
    if (!page) {
        return;
    }
    vmap_free(page->nav_gpx_suffix);
    page->nav_gpx_suffix = NULL;
    page->nav_gpx_suffix_count = 0;
    page->nav_gpx_rejoining = false;
    page->nav_gpx_off_since_ms = 0u;
    page->nav_gpx_off_warned = false;
}

static void map_page_gpx_course_clear(map_page_t * page)
{
    if (!page) {
        return;
    }
    vmap_free(page->nav_gpx_pts);
    page->nav_gpx_pts = NULL;
    page->nav_gpx_n = 0u;
    page->nav_gpx_total_m = 0.0;
    page->nav_gpx_true_m = 0.0;
    page->nav_gpx_win_start_m = 0.0;
    page->nav_gpx_win_end_m = 0.0;
    page->nav_gpx_dense = false;
    page->nav_gpx_sparse = false;
    page->nav_gpx_reverse = false;
}

static double map_page_gpx_off_threshold(const map_page_t * page)
{
    double base = bicycle_config_nav_off_route_m() > 0.0
        ? bicycle_config_nav_off_route_m() : 50.0;

    if (page && page->nav_gpx_reverse && base < MAP_PAGE_GPX_RETURN_OFF_M) {
        base = MAP_PAGE_GPX_RETURN_OFF_M;
    }
    return base;
}

static double map_page_gpx_window_max_off_m(const map_page_t * page)
{
    if (page && page->nav_gpx_reverse) {
        return map_page_gpx_off_threshold(page);
    }
    return 30.0;
}

static double map_page_nav_remain_display_m(const map_page_t * page,
    const vmap_route_nav_state_t * st)
{
    double r;
    double poly_m;

    if (!page || !st) {
        return 0.0;
    }
    r = st->remain_m;
    poly_m = page->nav_gpx_total_m;
    if (page->nav_gpx_pts && page->nav_gpx_n >= 2u && poly_m > 0.0) {
        if (page->nav_gpx_dense) {
            r = poly_m - (page->nav_gpx_win_start_m + st->along_m);
        } else if (page->nav_gpx_sparse) {
            r = (poly_m - page->nav_gpx_win_end_m) + st->remain_m;
        }
        if (r < 0.0) {
            r = 0.0;
        } else if (r > poly_m) {
            r = poly_m;
        }
        if (page->nav_gpx_true_m > poly_m && poly_m > 1.0) {
            r = page->nav_gpx_true_m * (r / poly_m);
        }
    }
    return r;
}

/**
 * @brief 把行程剩余航点交给路线叠层（地图上画成小圆点）。
 *
 * 放在导航几何刷新路径上：那里正好是 canvas 要重盖的时刻，点位和线不会错拍。
 * 但被菜单盖住时 `map_page_nav_refresh_map()` 会早退，所以行程起步时也直接叫一次
 * （用户就是从菜单里点的导航）。
 */
static void map_page_trip_pins_sync(map_page_t * page);
void map_page_nav_stop(map_page_t * page);

/* 导航结束（到点）5 秒后：所有提示退出 + 地图刷掉导航线与点位。
 * 用一次性 LVGL timer 挂在 UI 线程上，不需要额外的 tick 钩子。 */
static void map_page_nav_end_clear_cb(lv_timer_t * t)
{
    map_page_t * page = (map_page_t *)lv_timer_get_user_data(t);

    if (page == NULL) {
        return;
    }

    page->nav_arrive_hold = false;
    page->nav_end_clear_armed = false;
    /* **结束导航状态**：调用工程里标准收尾（="导航结束"dock 走的那一套）：
     * worker_cancel / nav_planning / nav_replanning / 提示隐藏 / nav_stop /
     * 路网释放 / 行程复位。然后清掉路线几何，轨迹与起终点标记一起消失。 */
    map_page_nav_stop(page);
    if (page->route) {
        vmap_route_clear(page->route);
    }
    (void)map_page_trip_pins_sync(page);
}

/* 到点那一刻调一次：5 s 后清屏（只挂一次）。 */
static void map_page_nav_end_clear_schedule(map_page_t * page)
{
    if (page == NULL || page->nav_end_clear_armed) {
        return;
    }

    page->nav_end_clear_armed = true;
    {
        lv_timer_t * t = lv_timer_create(map_page_nav_end_clear_cb, 5000u, page);

        if (t != NULL) {
            lv_timer_set_repeat_count(t, 1);
        }
    }
}

static void map_page_trip_pins_sync(map_page_t * page)
{
    if (!page || !page->route) {
        return;
    }

    if (!page->nav_trip_active) {
        vmap_route_set_trip_pins(page->route, NULL, 0u, 0u);
        return;
    }

    vmap_route_set_trip_pins(page->route, page->nav_trip.wps,
        page->nav_trip.wp_count,
        vmap_route_trip_leg_index(&page->nav_trip));
}

static void map_page_trip_ahead_clear(map_page_t * page)
{
    uint32_t i;

    if (!page) {
        return;
    }
    vmap_free(page->nav_trip_ahead_pts);
    page->nav_trip_ahead_pts = NULL;
    page->nav_trip_ahead_n = 0u;
    page->nav_trip_ahead_m = 0.0;
    page->nav_trip_ahead_man_n = 0u;
    page->nav_trip_ahead_path_len = 0u;
    page->nav_trip_ahead_dest_idx = 0u;
    page->nav_trip_ahead_ready = false;
    page->nav_trip_ahead_planning = false;
    page->nav_trip_silent = false;
    for (i = 0u; i < VMAP_ROUTE_MAX_MANEUVERS; i++) {
        page->nav_trip_ahead_mans[i].road_name = NULL;
    }
}
#endif

static void map_page_apply_panel_chrome(map_page_t * page)
{
    lv_font_t * font;

    if (!page || !page->panel) {
        return;
    }

    lv_obj_set_style_bg_color(page->panel, helm_color(HELM_COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(page->panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page->panel, 0, 0);

    font = myvendor_system_font_get(16);
    if (font == NULL) {
        font = myvendor_system_font_get(14);
    }

    if (page->speed_lbl) {
        lv_obj_set_style_text_color(page->speed_lbl, helm_color(HELM_COLOR_NAV), 0);
        if (font) {
            lv_obj_set_style_text_font(page->speed_lbl, font, 0);
        }
    }
    if (page->heading_lbl) {
        lv_obj_set_style_text_color(page->heading_lbl, helm_color(HELM_COLOR_INK), 0);
        if (font) {
            lv_obj_set_style_text_font(page->heading_lbl, font, 0);
        }
    }
    if (page->sensor_lbl) {
        lv_obj_set_style_text_color(page->sensor_lbl, helm_color(HELM_COLOR_CAD), 0);
        if (font) {
            lv_obj_set_style_text_font(page->sensor_lbl, font, 0);
        }
    }
#if VMAP_TRACK_ENABLE
    if (page->dist_lbl) {
        lv_obj_set_style_text_color(page->dist_lbl, helm_color(HELM_COLOR_INK), 0);
        if (font) {
            lv_obj_set_style_text_font(page->dist_lbl, font, 0);
        }
    }
#endif
#if VMAP_ROUTE_ENABLE
    if (page->nav_lbl) {
        lv_obj_set_style_text_color(page->nav_lbl, helm_color(HELM_COLOR_NAV), 0);
        if (font) {
            lv_obj_set_style_text_font(page->nav_lbl, font, 0);
        }
    }
    if (page->plan_hint) {
        lv_obj_set_style_bg_color(page->plan_hint, helm_color(HELM_COLOR_NAV_FILL), 0);
        lv_obj_set_style_text_color(page->plan_hint, helm_color(HELM_COLOR_NAV), 0);
        lv_obj_set_style_border_width(page->plan_hint, 0, 0);
        if (font) {
            lv_obj_set_style_text_font(page->plan_hint, font, 0);
        }
    }
#endif
}

static void map_page_apply_style(map_page_t * page)
{
    const lv_pm_theme_def_t * def;
    const lv_pm_theme_colors_t * c;

    if (!page) {
        return;
    }

    def = lv_pm_theme_current();
    if (def == NULL) {
        return;
    }

    c = &def->colors;

    if (page->root) {
        lv_obj_set_style_bg_color(page->root, c->page_bg, 0);
    }
#if VMAP_TRACK_ENABLE
    if (page->lap_lbl) {
        lv_obj_set_style_text_color(page->lap_lbl, c->accent, 0);
    }
    if (page->rec_lbl) {
        lv_obj_set_style_text_color(page->rec_lbl, c->rec, 0);
    }
#endif
    map_page_apply_panel_chrome(page);
}

static void map_page_sync_arrow(map_page_t * page)
{
    bicycle_gnss_fix_t fix;
    double lon;
    double lat;
    float course = 0.0f;

    if (!page || !page->map) {
        return;
    }

    vmap_view_get_center(page->map, &lon, &lat);
    if (bicycle_runtime_poll_fix(&fix) && fix.valid) {
        lon = fix.longitude;
        lat = fix.latitude;
        course = fix.course_deg;
    }
    vmap_view_update_arrow(page->map, lon, lat, course);
    vmap_view_bring_arrow_to_front(page->map);
}

static void map_page_kick_render(map_page_t * page)
{
    if (page && page->map && page->render_timer
        && vmap_view_render_busy(page->map)) {
        lv_timer_resume(page->render_timer);
    }
}

/**
 * @brief 无 GNSS 也要把首帧画出来（开机上次坐标 / 天安门）。
 */
static void map_page_ensure_boot_paint(map_page_t * page)
{
    if (!page || !page->map || page->covered) {
        return;
    }

    if (vmap_view_render_busy(page->map)) {
        map_page_kick_render(page);
        return;
    }

    if (vmap_view_render_revision(page->map) > 0 && !page->map_refresh_pending) {
        return;
    }

    page->map_refresh_pending = false;
    if (vmap_view_render_revision(page->map) == 0) {
        vmap_view_render(page->map);
    } else {
        vmap_view_redraw_keep_pan(page->map);
    }
    map_page_kick_render(page);
}

static void map_page_stamp_route_overlay(map_page_t * page)
{
    if (!page || !page->map) {
        return;
    }
    /* 进度刷新：导航线再 REC。标签留给瓦片结束帧，避免把路名描胖。 */
    vmap_view_stamp_nav_then_track(page->map);
}

/*
 * Hidden map + REC: points stay in RAM. Returning always starts one fresh,
 * phased frame at the latest fix; never mix the stale pixel origin with new
 * route/track projections. Normal visible-page catchup may still use a small
 * restamp/tail update.
 */
static void map_page_catchup_visible(map_page_t * page)
{
    const bicycle_runtime_t * rt;

    if (!page || !page->map || page->covered || !page->map_ui_visible) {
        return;
    }

    if (vmap_view_render_revision(page->map) == 0) {
        page->map_refresh_pending = true;
        return;
    }
    if (vmap_view_render_busy(page->map)) {
        map_page_kick_render(page);
        return;
    }

    rt = bicycle_runtime_get();
    if (rt && rt->gnss_valid) {
        vmap_view_set_center(page->map, (double)rt->longitude,
            (double)rt->latitude);
        vmap_view_update_arrow(page->map, (double)rt->longitude,
            (double)rt->latitude, rt->course_deg);
        page->map_last_follow_ms = lv_tick_get();
        map_page_kick_render(page);
    }
    if (vmap_view_render_busy(page->map)) {
        return;
    }

#if VMAP_ROUTE_ENABLE
    if (page->route && vmap_route_is_active(page->route)) {
        map_page_stamp_route_overlay(page);
        return;
    }
#endif
#if VMAP_TRACK_ENABLE
    if (page->track) {
        vmap_track_refresh(page->track);
    }
#endif
}

static void map_page_on_pm_theme(void * pm_page, lv_pm_theme_id_t id, void * user_data)
{
    map_page_t * page;
    lv_pm_page_t p = lv_pm_get_pm_page(pm_page);
    navi_arrow_style_t arrow;

    (void)user_data;

    page = p ? (map_page_t *)p->user_data : s_live_map;
    if (!page || !page->map) {
        return;
    }

    map_page_apply_style(page);
    lv_pm_status_bar_apply_theme(lv_pm_theme_current());
    arrow = vmap_style_current()->ui_navi_arrow_dark ? NAVI_ARROW_DARK
                                                     : NAVI_ARROW_LIGHT;
    vmap_view_set_arrow_image(page->map, navi_arrow_get(arrow));
    map_page_sync_arrow(page);
    if (page->covered) {
        if (page->map_theme_id != LV_PM_THEME_ID_INVALID
            && page->map_theme_id != id) {
            page->map_refresh_pending = true;
        }
        page->map_theme_id = id;
        return;
    }

    /* 关菜单/回主界面会再 apply 一次当前主题，画布还在就不要整帧栅格化。
     * 正在 pump 时也不能开新帧，否则会冲掉 keep_pan 擦轨迹。 */
    if (vmap_view_render_busy(page->map)) {
        if (page->map_theme_id != LV_PM_THEME_ID_INVALID
            && page->map_theme_id != id) {
            page->map_refresh_pending = true;
        }
        page->map_theme_id = id;
        helm_shell_raise_chrome();
        return;
    }
    if (vmap_view_render_revision(page->map) > 0
        && (page->map_theme_id == LV_PM_THEME_ID_INVALID
            || page->map_theme_id == id)) {
        page->map_theme_id = id;
        helm_shell_raise_chrome();
        return;
    }

    page->map_theme_id = id;
    /* Startup 还没 warmup：这里开帧会清画布，随后又被 set_view 打断。 */
    if (map_page_boot_deferred()
        && vmap_view_render_revision(page->map) == 0) {
        page->map_refresh_pending = true;
        helm_shell_raise_chrome();
        return;
    }

    vmap_view_render(page->map);
    if (page->render_timer) {
        lv_timer_resume(page->render_timer);
    }
}

static void map_page_style_refresh(map_page_t * page)
{
    lv_pm_page_t pm = live_map_page_def();

    (void)page;

    if (pm != NULL) {
        lv_pm_theme_apply(pm);
    }
}

#if VMAP_TRACK_ENABLE
static void on_track_lap(vmap_track_t * track, uint16_t lap, void * user_data)
{
    char buf[40];

    (void)track;
    (void)user_data;

    snprintf(buf, sizeof(buf), "开始第 %u 圈", (unsigned)lap);
    lv_pm_notify_show("Lap", buf, 3000);
    myvendor_sound_ok();
    bicycle_c_debug_track_lap(lap);
}
#endif

#if VMAP_ROUTE_ENABLE
static void map_page_plan_hint_update(map_page_t * page)
{
    static const char spin[] = "|/-\\";
    char text[40];

    if (!page || !page->plan_hint || page->plan_hint_base[0] == '\0') {
        return;
    }

    lv_snprintf(text, sizeof(text), "%s %c", page->plan_hint_base,
        spin[page->plan_hint_phase & 3u]);
    page->plan_hint_phase++;
    lv_label_set_text(page->plan_hint, text);
    lv_obj_align(page->plan_hint, LV_ALIGN_CENTER, 0, 0);
}

static void map_page_plan_hint_timer_cb(lv_timer_t * timer)
{
    map_page_plan_hint_update((map_page_t *)lv_timer_get_user_data(timer));
}

static void map_page_plan_hint_hide(map_page_t * page)
{
    if (page != NULL) {
        if (page->plan_hint != NULL) {
            lv_obj_add_flag(page->plan_hint, LV_OBJ_FLAG_HIDDEN);
        }
        if (page->plan_hint_timer != NULL) {
            lv_timer_delete(page->plan_hint_timer);
            page->plan_hint_timer = NULL;
        }
        page->plan_hint_base[0] = '\0';
        page->plan_hint_phase = 0u;
    }

    lv_pm_notify_dismiss();
}

static void map_page_plan_hint_show(map_page_t * page, const char * text)
{
    lv_font_t * font;

    if (page == NULL || page->map_area == NULL || text == NULL) {
        return;
    }
    if (page->nav_top_only || page->nav_replanning) {
        return;
    }

    if (page->plan_hint == NULL) {
        page->plan_hint = lv_label_create(page->map_area);
        lv_obj_set_style_bg_color(page->plan_hint, helm_color(HELM_COLOR_NAV_FILL), 0);
        lv_obj_set_style_bg_opa(page->plan_hint, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(page->plan_hint, helm_color(HELM_COLOR_NAV), 0);
        lv_obj_set_style_pad_hor(page->plan_hint, 14, 0);
        lv_obj_set_style_pad_ver(page->plan_hint, 10, 0);
        lv_obj_set_style_radius(page->plan_hint, 6, 0);
        lv_obj_set_style_border_width(page->plan_hint, 0, 0);
        lv_obj_add_flag(page->plan_hint, LV_OBJ_FLAG_FLOATING);
        lv_obj_clear_flag(page->plan_hint, LV_OBJ_FLAG_CLICKABLE);
        font = myvendor_system_font_get(16);
        if (font == NULL) {
            font = myvendor_system_font_get(14);
        }

        if (font != NULL) {
            lv_obj_set_style_text_font(page->plan_hint, font, 0);
        }
    }

    lv_snprintf(page->plan_hint_base, sizeof(page->plan_hint_base), "%s", text);
    page->plan_hint_phase = 0u;
    map_page_plan_hint_update(page);
    lv_obj_clear_flag(page->plan_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page->plan_hint);
    lv_pm_notify_show_hold("导航", text);
    if (page->plan_hint_timer == NULL) {
        page->plan_hint_timer = lv_timer_create(map_page_plan_hint_timer_cb,
            250u, page);
    }
    lv_refr_now(NULL);
}

static void map_page_nav_mtp_release(map_page_t * page)
{
    if (page && page->nav_mtp_held) {
        (void)myvendor_devctl_mtp_nav_hold(false);
        page->nav_mtp_held = false;
    }
}

static bool map_page_nav_mtp_acquire(map_page_t * page)
{
    int ret;
    unsigned i;

    if (!page) {
        return false;
    }
    if (page->nav_mtp_held) {
        return true;
    }

    ret = myvendor_devctl_mtp_nav_hold(true);
    if (ret < 0 && ret != -ENOTSUP) {
        (void)myvendor_devctl_mtp_nav_hold(false);
        return false;
    }
    page->nav_mtp_held = true;
    if (!myvendor_mtp_lfs_quiesce()) {
        return true;
    }

    /* 只在**UI 真停在 MTP 传输页**时才说"正在关闭 MTP"：quiesce 为真还有一个与
     * MTP 无关的成因 —— 别人拿着有效的 LFS hold（手机 BLE 文件传输的
     * `myvendor_mtp_lfs_hold("companion")`、星历注入的 `("gnss")`，保鲜期 30 s）。
     * 以前不分缘由一律报"MTP"，骑行时手机一传文件就在地图上弹出这句话，
     * 看着像误触发（用户 2026-09-25 报："MCU 根本没开 MTP"）。*/
    if (myvendor_mtp_page_active()) {
        map_page_plan_hint_show(page, "正在关闭 MTP");
    }
    for (i = 0u; i < 50u; i++) {
        usleep(20000);
        if (!myvendor_mtp_lfs_quiesce()) {
            /* 让位成功要把提示收掉（调用方随后会显示"规划路线中"）。
             * 2026-09-25 修：这条提前 return 原来漏了 hide ⇒ 提示会一直挂在屏上，
             * 表现为"这句话关不掉"。 */
            map_page_plan_hint_hide(page);
            return true;
        }
        if ((i % 5u) == 4u) {
            map_page_plan_hint_update(page);
            lv_refr_now(NULL);
        }
    }

    VMAP_NAV_WARN("MTP did not quiesce for navigation");
    map_page_plan_hint_hide(page);
    map_page_nav_mtp_release(page);
    return false;
}

static bool map_page_ensure_route_graph(map_page_t * page)
{
    if (s_route_graph) {
        if (vmap_map_catalog_is_regional()) {
            vmap_cell_id_t rid = VMAP_CELL_NONE;
            vmap_tile_cache_t * tc = page ? vmap_view_tile_cache(page->map) : NULL;

            if (tc) {
                rid = vmap_tile_cache_active_region(tc);
            }
            if (rid != VMAP_CELL_NONE && rid == s_graph_region) {
                return true;
            }
        } else {
            return true;
        }
    }
    if (myvendor_mtp_lfs_quiesce()) {
        VMAP_NAV_WARN("graph load blocked: lfs quiesce");
        return false;
    }

    if (vmap_map_catalog_is_regional()) {
        vmap_cell_id_t rid = VMAP_CELL_NONE;
        vmap_tile_cache_t * tc = page ? vmap_view_tile_cache(page->map) : NULL;

        if (tc) {
            rid = vmap_tile_cache_active_region(tc);
        }
        if (rid == VMAP_CELL_NONE) {
            VMAP_NAV_WARN("graph load: no active region");
            return false;
        }
        if (s_route_graph) {
            vmap_route_graph_unload(s_route_graph);
            s_route_graph = NULL;
            map_page_dem_cache_clear();
        }
        if (!vmap_route_graph_load_from_region(&s_route_graph, VMAP_TILE_DIR,
                rid)) {
            return false;
        }
        s_graph_region = rid;
    } else if (!vmap_route_graph_load_from_map_dir(&s_route_graph, VMAP_TILE_DIR)) {
        return false;
    }

    if (page && page->route) {
        vmap_route_set_graph(page->route, s_route_graph);
    }
    VMAP_NAV_LOG("graph ready nodes=%u edges=%u region=%u",
        (unsigned)vmap_route_graph_node_count(s_route_graph),
        (unsigned)vmap_route_graph_edge_count(s_route_graph),
        (unsigned)s_graph_region);
    return true;
}

static void map_page_sync_route_region(map_page_t * page, double lon, double lat)
{
    vmap_cell_id_t rid;
    vmap_tile_cache_t * tc;
    uint16_t ix;
    uint16_t iy;

    if (!page || !page->map || !vmap_map_catalog_is_regional()) {
        return;
    }
    if (page->nav_gpx_course) {
        return;
    }
    if (!vmap_grid_lonlat_to_cell(lon, lat, &ix, &iy)) {
        return;
    }
    rid = VMAP_CELL_ID(ix, iy);
    if (rid == s_graph_region) {
        return;
    }
    if (myvendor_mtp_lfs_quiesce()) {
        return;
    }
    tc = vmap_view_tile_cache(page->map);
    if (!tc || vmap_tile_cache_active_region(tc) != rid) {
        /* Pack load is the render pump's job; do not fopen here on GNSS tick. */
        return;
    }
    rid = vmap_tile_cache_active_region(tc);
    if (rid == VMAP_CELL_NONE || rid == s_graph_region) {
        return;
    }
    if (s_route_graph) {
        vmap_route_graph_unload(s_route_graph);
        s_route_graph = NULL;
        s_graph_region = VMAP_CELL_NONE;
        map_page_dem_cache_clear();
    }
    if (page->route) {
        vmap_route_set_graph(page->route, NULL);
    }
    (void)map_page_ensure_route_graph(page);
}

static void map_page_release_route_graph(map_page_t * page)
{
    if (page && page->route) {
        vmap_route_set_graph(page->route, NULL);
        vmap_route_clear(page->route);
    }
    if (s_route_graph) {
        vmap_route_graph_unload(s_route_graph);
        s_route_graph = NULL;
    }
    s_graph_region = VMAP_CELL_NONE;
    map_page_dem_cache_clear();
    vmap_route_graph_region_cache_clear();
    vmap_route_release_plan_scratch();
}

static bool map_page_ensure_route_nav_ex(map_page_t * page, bool need_graph)
{
    if (!page || !page->map) {
        VMAP_NAV_ERR("ensure: no page/map");
        return false;
    }
    if (need_graph && !map_page_ensure_route_graph(page)) {
        return false;
    }
    if (!page->route) {
        page->route = vmap_route_create(need_graph ? s_route_graph : NULL,
            page->map);
        if (!page->route) {
            VMAP_NAV_ERR("ensure: route create failed");
            return false;
        }
        vmap_view_attach_route(page->map, page->route);
    } else if (need_graph && s_route_graph) {
        vmap_route_set_graph(page->route, s_route_graph);
    }
    if (!page->nav) {
        page->nav = vmap_route_nav_create(page->route);
        if (!page->nav) {
            VMAP_NAV_ERR("ensure: nav create failed");
            return false;
        }
        vmap_route_nav_set_off_route_threshold(page->nav,
            bicycle_config_nav_off_route_m());
    }
    return true;
}

static bool map_page_ensure_route_nav(map_page_t * page)
{
    return map_page_ensure_route_nav_ex(page, true);
}

static void map_page_nav_refresh_map(map_page_t * page, double center_lon,
    double center_lat)
{
    if (!page || !page->map || page->covered) {
        VMAP_NAV_LOG("refresh skipped covered=%d", page ? page->covered : -1);
        return;
    }
    if (page->route) {
        vmap_view_attach_route(page->map, page->route);
    }
    map_page_trip_pins_sync(page);
    if (!page->map_ui_visible) {
        page->map_refresh_pending = true;
        VMAP_NAV_LOG("refresh deferred hidden");
        return;
    }
    vmap_view_set_view(page->map, center_lon, center_lat,
        vmap_view_get_zoom(page->map));
    vmap_view_render(page->map);
    map_page_kick_render(page);
    {
        bicycle_gnss_fix_t fix;

        if (bicycle_runtime_poll_fix(&fix) && fix.valid) {
            vmap_view_update_arrow(page->map, fix.longitude, fix.latitude,
                fix.course_deg);
        } else {
            vmap_view_update_arrow(page->map, center_lon, center_lat, 0.0f);
        }
    }
    vmap_view_bring_arrow_to_front(page->map);
    helm_shell_raise_chrome();
    VMAP_NAV_LOG("map redraw route_active=%d pts=%lu center=%.6f,%.6f",
        page->route && vmap_route_is_active(page->route),
        (unsigned long)(page->route ? vmap_route_point_count(page->route) : 0u),
        center_lon, center_lat);
}

static void map_page_nav_announce_if_needed(map_page_t * page)
{
    const vmap_route_nav_state_t * st;

    if (!page || !page->nav) {
        return;
    }

    st = vmap_route_nav_state(page->nav);
    if (!st || !st->active || st->maneuver_idx == UINT32_MAX
        || st->maneuver_idx == page->nav_maneuver_announced
        || st->next_kind == VMAP_MANEUVER_NONE
        || st->next_kind == VMAP_MANEUVER_START
        || st->dist_to_maneuver_m > VMAP_ROUTE_MANEUVER_ANNOUNCE_M) {
        return;
    }

    myvendor_sound_ok();
    page->nav_maneuver_announced = st->maneuver_idx;
}

static void map_page_ensure_gpx_sim(map_page_t * page)
{
    (void)page;
}

static void map_page_nav_gnss_for_gps(map_page_t * page)
{
    if (bicycle_route_sim_active()) {
        bicycle_route_sim_stop();
    }
    page->nav_route_sim = false;
}

static void map_page_nav_gnss_for_sim(map_page_t * page)
{
    (void)page;
    if (bicycle_gpx_sim_active()) {
        bicycle_gpx_sim_stop();
    }
    page->nav_route_sim = true;
}

int map_page_gnss_sim(map_page_t * page, const char * path, bool reverse,
    float speed_kph, float skip_km)
{
    const bicycle_config_t * cfg = bicycle_config();
    double skip_m = 0.0;
    int ret;

    if (bicycle_route_sim_active()) {
        bicycle_route_sim_stop();
    }
    if (page) {
        page->nav_route_sim = false;
    }

    if (path == NULL || path[0] == '\0') {
        bicycle_gpx_sim_stop();
        return 0;
    }

    if (skip_km > 0.0f) {
        skip_m = (double)skip_km * 1000.0;
    }

    ret = bicycle_gpx_sim_start(path, speed_kph,
        cfg ? cfg->gnss_update_ms : VMAP_GNSS_UPDATE_MS, reverse, skip_m);
    if (ret != 0 || skip_m < 0.5 || page == NULL) {
        return ret;
    }

    helm_shell_ensure_ride();
#if VMAP_TRACK_ENABLE
    if (page->track) {
        uint32_t n = bicycle_gpx_sim_prefix_count();
        uint32_t i;

        if (vmap_track_point_count(page->track) > 0u) {
            vmap_track_clear(page->track);
        }
        vmap_track_set_recording(page->track, true);
        for (i = 0; i < n; i++) {
            float lon = 0.f;
            float lat = 0.f;
            float kph = 22.f;

            if (!bicycle_gpx_sim_prefix_point(i, &lon, &lat, &kph)) {
                continue;
            }
            (void)vmap_track_push(page->track, (double)lon, (double)lat, kph);
            if ((i & 63u) == 0u) {
                myvendor_watchdog_ui_beat();
            }
        }
    }
#endif
    bicycle_runtime_seed_session(bicycle_gpx_sim_prefix_m(),
        (uint64_t)bicycle_gpx_sim_prefix_ms());
    map_page_kick_render(page);
    helm_shell_refresh();
    return ret;
}

static void map_page_nav_route_sim_sync(map_page_t * page,
    const vmap_route_pt_t * pts, uint32_t pt_count, bool reload)
{
    const bicycle_config_t * cfg = bicycle_config();
    const bicycle_nav_pt_t * nav_pts = (const bicycle_nav_pt_t *)pts;

    if (!page->nav_route_sim || !pts || pt_count < 2) {
        return;
    }
    if (reload && bicycle_route_sim_active()) {
        bicycle_route_sim_reload(nav_pts, pt_count);
        return;
    }
    if (bicycle_route_sim_start(nav_pts, pt_count, bicycle_config_nav_sim_speed_kph(),
            cfg->gnss_update_ms) != 0) {
        VMAP_NAV_WARN("route sim start failed");
        page->nav_route_sim = false;
        map_page_ensure_gpx_sim(page);
    }
}

static void map_page_nav_plan_done(void * user, bool ok,
    vmap_route_job_kind_t kind, uint32_t regions_planned,
    const vmap_route_pt_t * pts, uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count)
{
    map_page_t * page = (map_page_t *)user;
    char msg[80];
    bicycle_gnss_fix_t fix;
    bool initial_plan;
    const bool route_sim_reload = page && page->nav_route_sim
        && (page->nav_replanning || page->nav_refine_extend
            || bicycle_route_sim_active());

    if (!page) {
        VMAP_NAV_WARN("plan_done: null page");
        return;
    }
    initial_plan = !page->nav_replanning && !page->nav_refine_extend
        && !page->nav_trip_ahead_planning;

    if (!page->nav_planning) {
        VMAP_NAV_LOG("plan_done: ignore cancelled");
        return;
    }

    if (page->nav_trip_ahead_planning) {
        uint32_t i;
        uint32_t n_man;
        const vmap_route_nav_state_t * st;

        page->nav_planning = false;
        page->nav_trip_ahead_planning = false;
        page->nav_trip_silent = false;
        if (!ok || !pts || pt_count < 2u) {
            VMAP_NAV_WARN("trip ahead plan failed");
            return;
        }
        if (pt_count > VMAP_ROUTE_MAX_PTS) {
            pt_count = VMAP_ROUTE_MAX_PTS;
        }
        vmap_free(page->nav_trip_ahead_pts);
        page->nav_trip_ahead_pts = (vmap_route_pt_t *)vmap_malloc(
            (size_t)pt_count * sizeof(*page->nav_trip_ahead_pts));
        if (!page->nav_trip_ahead_pts) {
            VMAP_NAV_WARN("trip ahead alloc failed");
            return;
        }
        memcpy(page->nav_trip_ahead_pts, pts,
            (size_t)pt_count * sizeof(*page->nav_trip_ahead_pts));
        page->nav_trip_ahead_n = pt_count;
        page->nav_trip_ahead_m = total_m;
        n_man = maneuver_count;
        if (n_man > VMAP_ROUTE_MAX_MANEUVERS) {
            n_man = VMAP_ROUTE_MAX_MANEUVERS;
        }
        memset(page->nav_trip_ahead_mans, 0, sizeof(page->nav_trip_ahead_mans));
        if (maneuvers && n_man > 0u) {
            memcpy(page->nav_trip_ahead_mans, maneuvers,
                (size_t)n_man * sizeof(page->nav_trip_ahead_mans[0]));
            for (i = 0u; i < n_man; i++) {
                page->nav_trip_ahead_mans[i].road_name = NULL;
            }
        }
        page->nav_trip_ahead_man_n = n_man;
        page->nav_trip_ahead_ready = true;
        VMAP_NAV_LOG("trip ahead ready dest=wp%u pts=%u len=%.0fm",
            (unsigned)page->nav_trip_ahead_dest_idx, (unsigned)pt_count,
            total_m);

        st = vmap_route_nav_state(page->nav);
        if (st && st->active && vmap_route_nav_is_arrived(st)
            && page->nav_trip_active) {
            const uint32_t leg = vmap_route_trip_leg_index(&page->nav_trip);
            const uint32_t total = vmap_route_trip_waypoint_count(&page->nav_trip);

            if (leg + 1u < total
                && page->nav_trip_ahead_dest_idx == leg + 1u) {
                vmap_route_trip_advance_leg(&page->nav_trip);
                (void)map_page_trip_apply_ahead(page, page->nav_plan_from_lon,
                    page->nav_plan_from_lat);
            }
        }
        return;
    }

    page->nav_planning = false;
    map_page_plan_hint_hide(page);

    if (!ok || !page->nav || !pts || pt_count < 2) {
        VMAP_NAV_WARN("plan_done: failed ok=%d nav=%p pts=%p count=%lu",
            ok ? 1 : 0, (void *)page->nav, (void *)pts, (unsigned long)pt_count);
        if (page->nav_trip_silent) {
            page->nav_trip_silent = false;
            page->nav_replanning = false;
            page->nav_refine_extend = false;
            /* 走到这里的 silent 提交只可能是 tick 里的「换站」那一笔
             * （rolling_submit 的调用点只有 rolling_begin 和 trip_ahead_kick，
             * 而 ahead 在上面就 return 了）。它没规划出来就要把 leg 退回上一站
             * 重试 —— 留着已经推进的 leg，到达判定下一拍立刻再次成立，会把
             * 后面所有站一路跳光。 */
            if (kind == VMAP_ROUTE_JOB_ROLLING && page->nav_trip_active) {
                const uint32_t leg = vmap_route_trip_leg_index(&page->nav_trip);

                if (leg > 0u) {
                    (void)vmap_route_trip_set_leg(&page->nav_trip, leg - 1u);
                }
            }
            return;
        }
        /* 规划失败但骑手**已经在终点跟前**（目的地吸在 171 m 外的那种：
         * 越重规划越"没路"，还一直被当成逆行）→ 按到达收尾，不要再报失败、
         * 更不要继续重规划。直线距离判定，不看能不能绕过去。 */
        if (!page->nav_trip_active
            && vmap_geo_haversine_m(page->nav_plan_from_lon,
                   page->nav_plan_from_lat, page->nav_plan_to_lon,
                   page->nav_plan_to_lat) <= VMAP_NAV_ARRIVE_NEAR_M) {
            double d_end = vmap_geo_haversine_m(page->nav_plan_from_lon,
                page->nav_plan_from_lat, page->nav_plan_to_lon,
                page->nav_plan_to_lat);

            VMAP_NAV_OUT("plan_done: plan failed but %.0fm from dest -> arrive", d_end);
            if (!page->nav_arrive_hold) {
                lv_pm_notify_show("导航", "终点到了", 4000);
                myvendor_sound_arrive();
            }
            page->nav_arrive_hold = true;
            page->nav_planned = false;
            page->nav_replanning = false;
            page->nav_reroute_reason = VMAP_ROUTE_REROUTE_NONE;
            page->nav_refine_extend = false;
            map_page_nav_mtp_release(page);
            return;
        }

        if (page->nav_replanning) {
            snprintf(msg, sizeof(msg), "%s，重规划失败",
                vmap_route_reroute_reason_brief(page->nav_reroute_reason));
        } else {
            snprintf(msg, sizeof(msg), "路线规划失败");
        }
        if (!page->nav_top_only) {
            lv_pm_notify_show("导航", msg, 4000);
            myvendor_sound_warn();
        }
        page->nav_planned = false;
        page->nav_replanning = false;
        page->nav_reroute_reason = VMAP_ROUTE_REROUTE_NONE;
        page->nav_refine_extend = false;
        if (page->nav_route_sim) {
            page->nav_route_sim = false;
            bicycle_route_sim_stop();
            map_page_ensure_gpx_sim(page);
        }
        if (page->nav) {
            vmap_route_nav_stop(page->nav);
        }
        map_page_nav_mtp_release(page);
        return;
    }

    /* 只要是**同一条导航的再规划**（偏航重规划 / 滚动细化），就走"拼接"而**不是整体替换**：
     * `vmap_route_splice_forward()` 只换骑手前方那一段、把骑过的那段几何保留下来，
     * 绘制时的进度分割才能把走过的部分涂灰 —— 不需要开 REC、也不需要历史线。
     * 以前只有 refine_extend 一种模式走 splice，其余（包括偏航重规划）全走 apply，
     * 几何一换"走过的部分"就没了，屏幕上永远是蓝的。 */
    /* 决策取证：这条重规划到底走 splice 还是 apply、带着哪些标志。
     * 骑一小段看这行就知道"是谁在不停替换几何"（之前 seg 一直是 1 就是这个原因）。 */
    VMAP_NAV_OUT("plan_done: route=%s kind=%d replan=%d refine=%d trip=%d active=%d",
        (page->route && (page->nav_replanning || page->nav_refine_extend))
            ? "splice" : "apply",
        (int)kind, page->nav_replanning ? 1 : 0, page->nav_refine_extend ? 1 : 0,
        page->nav_trip_active ? 1 : 0,
        (page->route && vmap_route_is_active(page->route)) ? 1 : 0);

    /* 判定规则改成**看导航是否已经在这条路线上跑**（`nav_planned` + route active）：
     * 只要这次不是"一次全新导航"（新导航会在 plan_begin 里把 nav_planned 置 false），
     * 就一律 **splice**（保留已走前缀），不再依赖 nav_replanning/refine_extend 这两个
     * 标志 —— 现场就是周期性 rolling 细化没带这两个标志、每拍都整体替换，于是已走段
     * 只有起点那一小截（用户看到"起点画了一截红就再没长过"）。 */
    if (page->route && page->nav_planned && vmap_route_is_active(page->route)
        && (kind == VMAP_ROUTE_JOB_ROLLING || kind == VMAP_ROUTE_JOB_STANDARD)) {
        double splice_lon = page->nav_plan_from_lon;
        double splice_lat = page->nav_plan_from_lat;

        if (bicycle_runtime_poll_fix(&fix) && fix.valid) {
            splice_lon = fix.longitude;
            splice_lat = fix.latitude;
        }
        if (!vmap_route_splice_forward(page->route, splice_lon, splice_lat,
                pts, pt_count, total_m, maneuvers, maneuver_count)) {
            VMAP_NAV_WARN("plan_done: splice failed");
            if (!page->nav_top_only) {
                lv_pm_notify_show("导航", "路线衔接失败", 4000);
            }
            myvendor_sound_warn();
            return;
        }
        pts = vmap_route_points(page->route);
        pt_count = vmap_route_point_count(page->route);
        total_m = vmap_route_total_length_m(page->route);
        maneuvers = vmap_route_maneuvers(page->route);
        maneuver_count = vmap_route_maneuver_count(page->route);
    } else if (page->route) {
        vmap_route_apply(page->route, pts, pt_count, total_m, maneuvers,
            maneuver_count);
    }

    vmap_route_nav_apply(page->nav, pts, pt_count, total_m, maneuvers,
        maneuver_count);
    page->nav_planned = true;
    page->nav_maneuver_announced = UINT32_MAX;

    if (kind == VMAP_ROUTE_JOB_ROLLING && page->nav_trip_active) {
        page->nav_trip.region_refined = page->nav_roll_begin + regions_planned;
        if (page->nav_trip.region_refined > page->nav_trip.region_path_len) {
            page->nav_trip.region_refined = page->nav_trip.region_path_len;
        }
    }

    if (bicycle_runtime_poll_fix(&fix) && fix.valid) {
        vmap_route_nav_update(page->nav, fix.longitude, fix.latitude,
            fix.course_deg, fix.speed_kph);
        /* 走过的路线画成红色：nav 每拍给 route 一个 progress 点，
         * 绘制时 [0..progress] 用 VMAP_ROUTE_RIDDEN_RGB565（红），其余保持蓝。 */
        if (page->route) {
            vmap_route_set_progress_pos(page->route, fix.longitude,
                fix.latitude);
        }
    } else {
        vmap_route_nav_update(page->nav, page->nav_plan_from_lon,
            page->nav_plan_from_lat, 0.0f, 0.0f);
    }
    map_page_nav_announce_if_needed(page);
    if (initial_plan && !page->nav_trip_silent) {
        helm_shell_on_nav_started();
    }
    {
        const uint32_t junction_n =
            vmap_route_maneuver_plan_junction_count(maneuvers, maneuver_count);

        if (page->nav_trip_active) {
            const uint32_t leg = vmap_route_trip_leg_index(&page->nav_trip);
            const uint32_t total = vmap_route_trip_waypoint_count(&page->nav_trip);

            if (page->nav_refine_extend) {
                snprintf(msg, sizeof(msg), "路线已延伸 · %.0f m", total_m);
            } else if (page->nav_replanning) {
                snprintf(msg, sizeof(msg), "路线已更新 · %.0f m · %u 路口", total_m,
                    (unsigned)junction_n);
            } else {
                snprintf(msg, sizeof(msg), "路书 %u/%u · %.0f m · %u 路口",
                    (unsigned)(leg + 1u), (unsigned)total, total_m,
                    (unsigned)junction_n);
            }
        } else if (page->nav_gpx_sparse && page->nav_gpx_total_m > 0.0) {
            const double course_m = page->nav_gpx_true_m > page->nav_gpx_total_m
                ? page->nav_gpx_true_m : page->nav_gpx_total_m;

            snprintf(msg, sizeof(msg), "GPX %.1f / %.1f km · %u 路口",
                page->nav_gpx_win_end_m / 1000.0,
                course_m / 1000.0, (unsigned)junction_n);
        } else if (page->nav_replanning) {
            snprintf(msg, sizeof(msg), "路线已更新 · %.0f m · %u 路口", total_m,
                (unsigned)junction_n);
        } else {
            snprintf(msg, sizeof(msg), "路线 %.0f m · %u 路口", total_m,
                (unsigned)junction_n);
        }
    }
    page->nav_replanning = false;
    page->nav_reroute_reason = VMAP_ROUTE_REROUTE_NONE;
    page->nav_refine_extend = false;
    if (!page->nav_top_only && !page->nav_trip_silent
        && !(page->nav_gpx_sparse && !initial_plan)) {
        lv_pm_notify_show("导航", msg, 4000);
    }
    page->nav_trip_silent = false;
    if (page->nav_trip_active) {
        if (initial_plan) {
            page->nav_top_only = false;
    /* 新导航开始：把上一次的"到达/收尾"状态清掉，否则结束 dock 与
     * 到达提示会挂在这次导航上不退。 */
    page->nav_arrive_hold = false;
        }
        (void)map_page_trip_ahead_kick(page);
    }
    if (page->nav_route_sim) {
        map_page_nav_route_sim_sync(page, pts, pt_count, route_sim_reload);
    }
    VMAP_NAV_LOG("plan_done: ok pts=%lu man=%u len=%.0fm roll=%u refined=%u sim=%d",
        (unsigned long)pt_count, (unsigned)maneuver_count, total_m,
        (unsigned)regions_planned,
        page->nav_trip_active ? (unsigned)page->nav_trip.region_refined : 0u,
        page->nav_route_sim ? 1 : 0);
    (void)map_page_ensure_route_graph(page);
    {
        /*
         * Center on the rider, not on pts[0]: the route start is snapped to the
         * nearest graph node, which can be 100m+ away from the true position
         * (sparse nodes / region borders).  Centering on the snapped node makes
         * the map visibly jump after every (re)plan.
         */
        double cx = page->nav_plan_from_lon;
        double cy = page->nav_plan_from_lat;

        if (bicycle_runtime_poll_fix(&fix) && fix.valid) {
            cx = fix.longitude;
            cy = fix.latitude;
        }
        map_page_nav_refresh_map(page, cx, cy);
    }
}

static bool map_page_nav_rolling_submit(map_page_t * page, double from_lon,
    double from_lat, double to_lon, double to_lat, const vmap_cell_id_t * path,
    uint32_t path_len, uint32_t region_refined, bool refine, bool silent,
    bool ahead)
{
    vmap_route_rolling_req_t req;

    if (!page || !page->nav_trip_active || !path || path_len == 0u) {
        return false;
    }
    if (vmap_route_worker_busy()) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.map_dir = VMAP_TILE_DIR;
    req.from_lon = from_lon;
    req.from_lat = from_lat;
    req.to_lon = to_lon;
    req.to_lat = to_lat;
    req.region_path = path;
    req.region_path_len = path_len;
    req.region_begin = refine && region_refined > 0u
        ? region_refined - 1u : region_refined;
    req.max_regions = VMAP_ROUTE_REFINE_BATCH;
    page->nav_roll_begin = req.region_begin;
    page->nav_planning = true;
    page->nav_trip_silent = silent || ahead;
    page->nav_trip_ahead_planning = ahead;
    if (ahead) {
        page->nav_refine_extend = false;
    } else {
        page->nav_refine_extend = refine;
        page->nav_plan_from_lon = from_lon;
        page->nav_plan_from_lat = from_lat;
        page->nav_plan_to_lon = to_lon;
        page->nav_plan_to_lat = to_lat;
        page->nav_maneuver_announced = UINT32_MAX;
        if (!silent) {
            map_page_plan_hint_show(page, refine ? "延伸路线中" : "规划路线中");
        }
    }

    if (!vmap_route_worker_submit_rolling(&req, map_page_nav_plan_done, page)) {
        page->nav_planning = false;
        page->nav_refine_extend = false;
        page->nav_trip_ahead_planning = false;
        page->nav_trip_silent = false;
        if (!silent && !ahead) {
            map_page_plan_hint_hide(page);
        }
        return false;
    }
    return true;
}

static bool map_page_nav_rolling_begin(map_page_t * page, double from_lon,
    double from_lat, bool refine, bool silent)
{
    double to_lon;
    double to_lat;

    if (!page || !page->nav_trip_active) {
        return false;
    }
    if (!vmap_route_trip_leg_target(&page->nav_trip, &to_lon, &to_lat)) {
        return false;
    }

    return map_page_nav_rolling_submit(page, from_lon, from_lat, to_lon, to_lat,
        page->nav_trip.region_path, page->nav_trip.region_path_len,
        page->nav_trip.region_refined, refine, silent, false);
}

static bool map_page_trip_ahead_kick(map_page_t * page)
{
    const uint32_t leg = page ? vmap_route_trip_leg_index(&page->nav_trip) : 0u;
    const uint32_t total = page
        ? vmap_route_trip_waypoint_count(&page->nav_trip) : 0u;
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;

    if (!page || !page->nav_trip_active || page->nav_planning
        || page->nav_trip_ahead_planning || page->nav_gpx_course) {
        return false;
    }
    if (leg + 1u >= total) {
        return false;
    }
    if (page->nav_trip_ahead_ready
        && page->nav_trip_ahead_dest_idx == leg + 1u) {
        return false;
    }
    if (!vmap_route_trip_waypoint_at(&page->nav_trip, leg, &from_lon, &from_lat)
        || !vmap_route_trip_waypoint_at(&page->nav_trip, leg + 1u, &to_lon,
            &to_lat)) {
        return false;
    }
    if (!vmap_route_trip_fill_path(VMAP_TILE_DIR, from_lon, from_lat, to_lon,
            to_lat, page->nav_trip_ahead_path, VMAP_ROUTE_REGION_PATH_MAX,
            &page->nav_trip_ahead_path_len)) {
        return false;
    }

    page->nav_trip_ahead_dest_idx = leg + 1u;
    page->nav_trip_ahead_ready = false;
    VMAP_NAV_LOG("trip ahead: wp%u -> wp%u silent", (unsigned)leg,
        (unsigned)(leg + 1u));
    return map_page_nav_rolling_submit(page, from_lon, from_lat, to_lon, to_lat,
        page->nav_trip_ahead_path, page->nav_trip_ahead_path_len, 0u,
        false, true, true);
}

static bool map_page_trip_apply_ahead(map_page_t * page, double lon, double lat)
{
    bicycle_gnss_fix_t fix;
    bool have_fix;

    if (!page || !page->nav || !page->nav_trip_ahead_ready
        || !page->nav_trip_ahead_pts || page->nav_trip_ahead_n < 2u) {
        return false;
    }

    vmap_route_nav_apply(page->nav, page->nav_trip_ahead_pts,
        page->nav_trip_ahead_n, page->nav_trip_ahead_m,
        page->nav_trip_ahead_mans, page->nav_trip_ahead_man_n);
    page->nav_planned = vmap_route_is_active(page->route);
    page->nav_maneuver_announced = UINT32_MAX;
    page->nav_plan_from_lon = page->nav_trip_ahead_pts[0].lon;
    page->nav_plan_from_lat = page->nav_trip_ahead_pts[0].lat;
    page->nav_plan_to_lon =
        page->nav_trip_ahead_pts[page->nav_trip_ahead_n - 1u].lon;
    page->nav_plan_to_lat =
        page->nav_trip_ahead_pts[page->nav_trip_ahead_n - 1u].lat;
    page->nav_trip.region_refined = 0u;
    (void)vmap_route_trip_begin_leg(VMAP_TILE_DIR, &page->nav_trip,
        page->nav_plan_from_lon, page->nav_plan_from_lat);
    map_page_trip_ahead_clear(page);
    have_fix = bicycle_runtime_poll_fix(&fix) && fix.valid;
    if (have_fix) {
        lon = fix.longitude;
        lat = fix.latitude;
    }
    if (page->nav_planned) {
        vmap_route_nav_update(page->nav, lon, lat,
            have_fix ? fix.course_deg : 0.0f, have_fix ? fix.speed_kph : 0.0f);
        map_page_nav_refresh_map(page, lon, lat);
    }
    VMAP_NAV_LOG("trip ahead applied dest=wp%u",
        (unsigned)vmap_route_trip_leg_index(&page->nav_trip));
    (void)map_page_trip_ahead_kick(page);
    return page->nav_planned;
}

static bool map_page_nav_plan_begin(map_page_t * page, double from_lon,
    double from_lat, double to_lon, double to_lat, bool user_set, bool reroute)
{
    bool mtp_was_held;

    if (!page) {
        VMAP_NAV_ERR("plan_begin: null page");
        return false;
    }

    if (page->nav_planning) {
        VMAP_NAV_WARN("plan_begin: already planning");
        return false;
    }
    mtp_was_held = page->nav_mtp_held;
    if (!map_page_nav_mtp_acquire(page)) {
        VMAP_NAV_ERR("plan_begin: MTP suspend failed");
        return false;
    }
    if (!map_page_ensure_route_nav(page)) {
        VMAP_NAV_ERR("plan_begin: route/nav not ready");
        if (!mtp_was_held) {
            map_page_plan_hint_hide(page);
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    if (myvendor_mtp_lfs_quiesce()) {
        VMAP_NAV_WARN("plan_begin: blocked lfs quiesce");
        if (!mtp_was_held) {
            map_page_plan_hint_hide(page);
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    vmap_route_worker_init();

    if (vmap_route_worker_busy()) {
        VMAP_NAV_WARN("plan_begin: worker busy");
        if (!mtp_was_held) {
            map_page_plan_hint_hide(page);
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    map_page_gpx_rejoin_reset(page);
    vmap_route_worker_cancel();
    if (page->nav) {
        vmap_route_nav_set_off_route_threshold(page->nav,
            page->nav_gpx_course ? map_page_gpx_off_threshold(page)
                                 : bicycle_config_nav_off_route_m());
    }
    if (!reroute) {
        vmap_route_nav_stop(page->nav);
        page->nav_top_only = false;
        page->nav_last_reroute_ms = 0u;
        page->nav_last_reroute_lon = 0.0;
        page->nav_last_reroute_lat = 0.0;
        page->nav_off_since_ms = 0u;
    }
    page->nav_planned = reroute ? page->nav_planned : false;
    page->nav_planning = true;
    page->nav_replanning = reroute;
    page->nav_user_set = user_set;
    page->nav_trip_active = false;
    map_page_trip_ahead_clear(page);
    page->nav_gpx_course = false;
    page->nav_plan_from_lon = from_lon;
    page->nav_plan_from_lat = from_lat;

    /* 起航（不是偏航重规划）时记下**导航起点**，一直画绿点 —— 重规划不改它，
     * 否则绿点跟着骑行者跑就被箭头压住了。 */
    if (!reroute && page->route) {
        vmap_route_set_origin(page->route, true, from_lon, from_lat);
    }
    page->nav_plan_to_lon = to_lon;
    page->nav_plan_to_lat = to_lat;
    page->nav_maneuver_announced = UINT32_MAX;

    {
        const bool corridor = vmap_route_corridor_needed(VMAP_TILE_DIR,
            from_lon, from_lat, to_lon, to_lat);

        if (!corridor) {
            map_page_plan_hint_show(page, "加载路网");
            if (!map_page_ensure_route_graph(page)) {
                page->nav_planning = false;
                page->nav_replanning = false;
                map_page_plan_hint_hide(page);
                if (!mtp_was_held) {
                    map_page_nav_mtp_release(page);
                }
                VMAP_NAV_ERR("plan_begin: route graph not ready");
                return false;
            }
        }

        map_page_plan_hint_show(page, reroute ? "重规划中" : "规划路线中");
        VMAP_NAV_OUT("%s 起点 lon=%.7f lat=%.7f 终点 lon=%.7f lat=%.7f reason=%s",
            reroute ? "偏航重规划" : "规划开始",
            from_lon, from_lat, to_lon, to_lat,
            reroute ? vmap_route_reroute_reason_brief(page->nav_reroute_reason)
                    : "-");

        if (!vmap_route_worker_submit(VMAP_TILE_DIR,
                corridor ? NULL : s_route_graph,
                from_lon, from_lat, to_lon, to_lat,
                map_page_nav_plan_done, page)) {
            page->nav_planning = false;
            page->nav_replanning = false;
            page->nav_reroute_reason = VMAP_ROUTE_REROUTE_NONE;
            map_page_plan_hint_hide(page);
            if (!mtp_was_held) {
                map_page_nav_mtp_release(page);
            }
            VMAP_NAV_ERR("plan_begin: worker submit failed");
            return false;
        }
    }
    return true;
}

static bool map_page_nav_reroute_begin(map_page_t * page, double from_lon,
    double from_lat)
{
    if (!page) {
        return false;
    }
    if (page->nav_trip_active) {
        page->nav_replanning = true;
        page->nav_trip.region_refined = 0;
        if (!vmap_route_trip_begin_leg(VMAP_TILE_DIR, &page->nav_trip,
                from_lon, from_lat)) {
            return false;
        }
        return map_page_nav_rolling_begin(page, from_lon, from_lat, false, false);
    }
    return map_page_nav_plan_begin(page, from_lon, from_lat,
        page->nav_plan_to_lon, page->nav_plan_to_lat, page->nav_user_set, true);
}

/*
 * Throttle reroutes so a rider stuck in a road-sparse spot (where every plan
 * snaps far from the true position) cannot spawn a replanning storm.  A large
 * jump from the previous reroute point is treated as a genuine deviation and
 * always allowed.
 */
#define MAP_PAGE_REROUTE_OFF_CONFIRM_MS  3000u
#define MAP_PAGE_REROUTE_MIN_INTERVAL_MS 15000u
#define MAP_PAGE_REROUTE_FORCE_MOVE_M    80.0

static bool map_page_reroute_allowed(map_page_t * page, double lon, double lat)
{
    if (!page) {
        return false;
    }
    if (page->nav_last_reroute_ms == 0u) {
        return true;
    }
    if (vmap_geo_haversine_m(page->nav_last_reroute_lon,
            page->nav_last_reroute_lat, lon, lat) >= MAP_PAGE_REROUTE_FORCE_MOVE_M) {
        return true;
    }
    return lv_tick_elaps(page->nav_last_reroute_ms)
        >= MAP_PAGE_REROUTE_MIN_INTERVAL_MS;
}
#endif

/* BLE sensor scan is NSH `test sensor`; only overlay live telem when the
 * snapshot has a fresh sample so NSH/UI values are not wiped by empty data.
 * 2026-09-20：逻辑提到 `bicycle_runtime_sync_companion_sensors()`，主循环每拍
 * 都会调（主骑行页以前看不到传感器值），这里保留一个薄壳避免改动调用点。 */
static void map_page_apply_companion_sensors(void)
{
    bicycle_runtime_sync_companion_sensors();
}

static void on_render_pump(lv_timer_t * timer)
{
    map_page_t * page = (map_page_t *)lv_timer_get_user_data(timer);

    if (!page || !page->map || page->covered) {
        return;
    }

    if (!vmap_view_render_busy(page->map)) {
        return;
    }

    if (vmap_view_render_pump(page->map, VMAP_RENDER_PUMP_MAX_TILES)) {
        map_page_sync_arrow(page);
        helm_shell_raise_chrome();
#if VMAP_TRACK_ENABLE
        if (page->track) {
            vmap_track_refresh(page->track);
        }
#endif
        if (page->render_timer) {
            lv_timer_pause(page->render_timer);
        }
    }
}

static void map_page_gpx_stall_log(uint32_t t0, bool vis0, bool cov0,
    bool pending0, bool fix_ok)
{
    LV_UNUSED(t0);
    LV_UNUSED(vis0);
    LV_UNUSED(cov0);
    LV_UNUSED(pending0);
    LV_UNUSED(fix_ok);
}

static void map_page_gpx_stall_part(const char * tag, uint32_t t0)
{
    LV_UNUSED(tag);
    LV_UNUSED(t0);
}

/* 定义在 map_page_nav_arrived() 上方；tick 里收尾判定也要用。 */
static bool map_page_trip_has_more(const map_page_t * page);

static void on_gpx_timer(lv_timer_t * timer)
{
    map_page_t * page = (map_page_t *)lv_timer_get_user_data(timer);
    bicycle_gnss_fix_t fix;
    bool have_fix;
    uint32_t t0;
    bool pending0;
    bool vis0;
    bool cov0;

    if (!page) {
        return;
    }

    t0 = lv_tick_get();
    pending0 = page->map_refresh_pending;
    vis0 = page->map_ui_visible;
    cov0 = page->covered;

    /* 导航横幅盖住地图顶部：把画布内容与箭头一起下移**半个横幅高**，让骑手落在
     * 「横幅以下那块可见区」的中心（用户 2026-09-25 定；实测箭头原本落在整块
     * 地图区的中心 132，而可见区中心是 161，差 29 = HELM_NAV_BAN_H/2）。
     *
     * **必须放在本函数所有早退之前**：无定位（骑手停在规划起点）、被别的页盖住、
     * 静音期这三种情况都会在下面 return，而那时箭头同样需要跟着横幅偏 —— 第一版
     * 放在函数尾部的正常跟车分支里，结果"有横幅但无 fix"时一点都不偏（用户实测）。
     * 每次都按横幅实际状态重算，谁隐藏横幅都不会留下陈旧偏置；偏置只作用于显示
     * 位置，不进跟车/rebase 的数学。 */
    vmap_view_set_y_bias(page->map,
        helm_shell_nav_ban_shown() ? (HELM_NAV_BAN_H / 2) : 0);

    have_fix = bicycle_runtime_poll_fix(&fix);
    bicycle_runtime_tick(lv_tick_get());
    bicycle_runtime_update_gnss(&fix);

#if VMAP_ROUTE_ENABLE
    if (have_fix && fix.valid) {
        if (page->dem_last_ms == 0u
            || lv_tick_elaps(page->dem_last_ms) >= MAP_PAGE_DEM_MS) {
            uint32_t t_dem = lv_tick_get();
            const int16_t e = map_page_sample_ele_m(page, fix.longitude,
                fix.latitude);

            page->dem_last_ms = lv_tick_get();
            if (page->dem_last_ms == 0u) {
                page->dem_last_ms = 1u;
            }
            bicycle_env_set_dem_m((float)e, e != (int16_t)VGRF_ELE_UNKNOWN);
            map_page_gpx_stall_part("dem", t_dem);
        }
    } else {
        bicycle_env_set_dem_m(0.0f, false);
    }
#else
    bicycle_env_set_dem_m(0.0f, false);
#endif
    bicycle_env_tick();
    map_page_apply_companion_sensors();
    bicycle_c_debug_gpx_tick();

    if (bicycle_runtime_get()->recording) {
        if (!bicycle_ride_gpx_active()) {
            (void)bicycle_ride_gpx_begin();
        }
        if (have_fix && fix.valid) {
            bicycle_ride_gpx_push_fix(&fix);
        }
    }

#if VMAP_TRACK_ENABLE
    /* 菜单盖住时仍把点记进 overlay，返回地图才不会缺一截 REC 线。 */
    if (page->track && vmap_track_is_recording(page->track) &&
        have_fix && fix.valid) {
        bicycle_c_debug_track_push(vmap_track_push(page->track, fix.longitude,
            fix.latitude, fix.speed_kph));
    }
#endif

    /* Covered by menu/MTP: keep GNSS/rec, skip nav compute and all drawing. */
    if (page->covered) {
        helm_shell_autopause_tick();
        map_page_gpx_stall_log(t0, vis0, cov0, pending0, have_fix && fix.valid);
        return;
    }

    if (!have_fix || !fix.valid) {
#if VMAP_ROUTE_ENABLE
        if (page->map && page->nav_planned) {
            vmap_view_update_arrow(page->map, page->nav_plan_from_lon,
                page->nav_plan_from_lat, 0.0f);
        }
#endif
        map_page_ensure_boot_paint(page);
        map_page_gpx_stall_log(t0, vis0, cov0, pending0, false);
        return;
    }

#if VMAP_ROUTE_ENABLE
    map_page_sync_route_region(page, fix.longitude, fix.latitude);
#endif

#if VMAP_ROUTE_ENABLE
    if (page->nav && page->nav_planned && !page->covered
        && !(page->nav_planning && !page->nav_trip_ahead_planning)
        && !myvendor_mtp_lfs_quiesce()) {
        const vmap_route_nav_state_t * st;
        const uint32_t now = lv_tick_get();

        if (page->nav_last_update_ms == 0
            || lv_tick_elaps(page->nav_last_update_ms) >= 500u) {
            uint32_t t_nav = lv_tick_get();

            page->nav_last_update_ms = now;
            vmap_route_nav_update(page->nav, fix.longitude, fix.latitude,
                fix.course_deg, fix.speed_kph);
            st = vmap_route_nav_state(page->nav);
            if (st && st->active) {
                bool gpx_window_busy = false;

                bicycle_runtime_update_nav(
                    map_page_nav_remain_display_m(page, st),
                    st->off_route_m, st->turn_rel_deg);
                map_page_nav_announce_if_needed(page);
                if (page->nav_gpx_pts && page->nav_gpx_n >= 2u) {
                    gpx_window_busy = map_page_gpx_tick_window(page, st,
                        fix.longitude, fix.latitude);
                    if (gpx_window_busy) {
                        st = vmap_route_nav_state(page->nav);
                        if (st && st->active) {
                            bicycle_runtime_update_nav(
                                map_page_nav_remain_display_m(page, st),
                                st->off_route_m, st->turn_rel_deg);
                        }
                    }
                }
                if (page->nav_gpx_course && !page->nav_planning
                    && !gpx_window_busy) {
                    const double threshold = map_page_gpx_off_threshold(page);
                    double off_m = st->off_route_m;

                    if (page->nav_gpx_pts && page->nav_gpx_n >= 2u
                        && off_m >= threshold * 0.5) {
                        double gpx_d = 1e300;

                        (void)map_page_gpx_nearest_along(page->nav_gpx_pts,
                            page->nav_gpx_n, fix.longitude, fix.latitude,
                            &gpx_d);
                        off_m = gpx_d;
                    }
                    bicycle_runtime_update_nav(
                        map_page_nav_remain_display_m(page, st),
                        off_m, st->turn_rel_deg);

                    if (off_m <= threshold * 0.6) {
                        page->nav_gpx_off_since_ms = 0u;
                        page->nav_gpx_off_warned = false;
                    } else if (off_m >= threshold) {
                        if (page->nav_gpx_off_since_ms == 0u) {
                            page->nav_gpx_off_since_ms = now;
                        } else if (lv_tick_elaps(page->nav_gpx_off_since_ms)
                            >= MAP_PAGE_GPX_OFF_CONFIRM_MS) {
                            bool retry_ok;

                            if (!page->nav_gpx_off_warned) {
                                myvendor_sound_warn();
                                page->nav_gpx_off_warned = true;
                            }

                            retry_ok = page->nav_last_reroute_ms == 0u
                                || lv_tick_elaps(page->nav_last_reroute_ms)
                                    >= MAP_PAGE_GPX_REROUTE_INTERVAL_MS
                                || vmap_geo_haversine_m(page->nav_last_reroute_lon,
                                    page->nav_last_reroute_lat, fix.longitude,
                                    fix.latitude) >= MAP_PAGE_REROUTE_FORCE_MOVE_M;
                            if (!page->nav_gpx_rejoining && retry_ok) {
                                page->nav_last_reroute_ms = now;
                                page->nav_last_reroute_lon = fix.longitude;
                                page->nav_last_reroute_lat = fix.latitude;
                                if (!map_page_nav_gpx_rejoin_begin(page, st,
                                        fix.longitude, fix.latitude)) {
                                    VMAP_NAV_WARN("GPX rejoin begin failed");
                                }
                            }
                        }
                    }
                }
                if (page->nav_planning && !page->nav_trip_ahead_planning) {
                    /* Active replan / GPX window submitted this tick. */
                } else if (vmap_route_nav_is_arrived(st) && page->nav_trip_active
                    && !page->nav_trip_ahead_planning) {
                    const uint32_t leg = vmap_route_trip_leg_index(&page->nav_trip);
                    const uint32_t total =
                        vmap_route_trip_waypoint_count(&page->nav_trip);

                    if (leg + 1u < total) {
                        bool applied = false;
                        char note[64];

                        /* worker 忙就先不要动 leg：推进了却交不出新路线的话，
                         * nav 会继续停在已经「到达」的旧路线上，下一拍到达
                         * 判定再次成立 → 又推进一站 …… 后面的站会被一路跳光，
                         * 表现就是第一站就收尾。 */
                        if (!vmap_route_worker_busy()) {
                            uint32_t next;

                            vmap_route_trip_advance_leg(&page->nav_trip);

                            /* 新 leg 的目标也可能已经在脚下（两站选得太近，比如
                             * 同一个路口）：再往后跳到第一个真正没到的站。
                             * 不跳的话规划器会以 "already at dest" 拒绝，
                             * 我们就退化成每拍推进→失败→退回的空转。 */
                            if (!vmap_route_trip_skip_reached(&page->nav_trip,
                                    fix.longitude, fix.latitude)) {
                                /* 剩下的站全在脚下：没有可规划的 leg 了，直接收尾。
                                 * 此时 leg_idx 已停在最后一站，`trip_has_more`
                                 * 为 false，结束 dock 会正常弹出；退回去只会空转。 */
                                page->nav_arrive_hold = true;
                        map_page_nav_end_clear_schedule(page);
                            map_page_nav_end_clear_schedule(page);
                            } else {
                                next = vmap_route_trip_leg_index(&page->nav_trip);

                                if (page->nav_trip_ahead_ready
                                    && page->nav_trip_ahead_dest_idx == next) {
                                    applied = map_page_trip_apply_ahead(page,
                                        fix.longitude, fix.latitude);
                                } else if (vmap_route_trip_begin_leg(VMAP_TILE_DIR,
                                        &page->nav_trip, fix.longitude,
                                        fix.latitude)) {
                                    applied = map_page_nav_rolling_begin(page,
                                        fix.longitude, fix.latitude, false, true);
                                }

                                if (!applied) {
                                    /* 交不出新一段就退回上一站，下一拍重试；
                                     * 宁可停在原地，也不要静默丢掉后面的站。 */
                                    (void)vmap_route_trip_set_leg(&page->nav_trip,
                                        leg);
                                }
                            }
                        }

                        if (applied) {
                            /* 「还剩几站」按**跳站之后**的 leg 算：前面可能一口气
                             * 吃掉了几个站，用推进前的 next 会多报。 */
                            const uint32_t now_leg =
                                vmap_route_trip_leg_index(&page->nav_trip);

                            /* 中间站要明说一句「还在走」+ 整趟进度：这一段的重新
                             * 规划是 silent 的，横幅会直接跳到下一站的转向与距离，
                             * 不给反馈的话骑手很容易以为导航在第一站就断了。 */
                            const uint32_t left = total > now_leg + 1u
                                ? total - (now_leg + 1u) : 0u;

                            myvendor_sound_arrive();
                            /* 到站提示要**说清是哪一站**：有名字就用名字
                             * （"XX点到了"），没名字退回序号。 */
                            if (leg < page->nav_trip_name_count
                                && page->nav_trip_names[leg][0] != '\0') {
                                snprintf(note, sizeof(note),
                                    "%s 点到了 · 还剩%u站",
                                    page->nav_trip_names[leg], (unsigned)left);
                            } else {
                                snprintf(note, sizeof(note),
                                    "第%u 点到了 · 还剩%u站",
                                    (unsigned)(leg + 1u), (unsigned)left);
                            }
                            lv_pm_notify_show("导航", note, 3000);
                        }
                    } else if (vmap_route_nav_should_auto_stop(st, fix.speed_kph)) {
                        if (!page->nav_arrive_hold) {
                            /* 终点：明确报一声再收尾（不然只有横幅变成"到达"，
                             * 骑手不知道导航已经结束）。 */
                            lv_pm_notify_show("导航", "终点到了", 4000);
                            myvendor_sound_arrive();
                        }
                        page->nav_arrive_hold = true;
                        map_page_nav_end_clear_schedule(page);
                    }
                } else if (vmap_route_nav_should_auto_stop(st, fix.speed_kph)) {
                    const bool gpx_more =
                        page->nav_gpx_pts && page->nav_gpx_n >= 2u
                        && page->nav_gpx_win_end_m + 30.0 < page->nav_gpx_total_m;

                    /* 这条分支在多站行程的**中间站**也会被命中：到站时若 ahead
                     * 预规划还在跑，上面的推进分支会被 !nav_trip_ahead_planning
                     * 挡住，于是掉到这里。此时置 hold 等于把「整趟结束」锁死，
                     * 行程再也推不动。后面还有站就不能收尾。 */
                    if (!gpx_more && !map_page_trip_has_more(page)) {
                        if (!page->nav_arrive_hold) {
                            lv_pm_notify_show("导航", "终点到了", 4000);
                            myvendor_sound_arrive();
                        }
                        page->nav_arrive_hold = true;
                        map_page_nav_end_clear_schedule(page);
                    }
                } else if (page->nav_trip_active
                    && vmap_route_trip_needs_refine(&page->nav_trip, st->remain_m)
                    && !page->nav_planning) {
                    (void)map_page_nav_rolling_begin(page, fix.longitude,
                        fix.latitude, true, false);
                } else if (!page->nav_planning && !page->nav_gpx_course) {
                    if (page->nav_trip_active
                        && map_page_trip_ahead_kick(page)) {
                        /* Next-next waypoint planned silently. */
                    } else {
                    const double threshold =
                        bicycle_config_nav_off_route_m() > 0.0
                            ? bicycle_config_nav_off_route_m() : 50.0;
                    vmap_route_reroute_reason_t reroute_reason =
                        vmap_route_nav_take_reroute(page->nav);
                    bool persistent_off = false;

                    if (st->off_route_m <= threshold * 0.6) {
                        page->nav_off_since_ms = 0u;
                    } else if (st->off_route_m >= threshold) {
                        if (page->nav_off_since_ms == 0u) {
                            page->nav_off_since_ms = now;
                        } else {
                            persistent_off =
                                lv_tick_elaps(page->nav_off_since_ms)
                                    >= MAP_PAGE_REROUTE_OFF_CONFIRM_MS;
                        }
                    }
                    if (reroute_reason == VMAP_ROUTE_REROUTE_NONE
                        && persistent_off
                        && map_page_reroute_allowed(page, fix.longitude,
                            fix.latitude)) {
                        reroute_reason = VMAP_ROUTE_REROUTE_JUNCTION_FAR;
                    }
                    if (reroute_reason != VMAP_ROUTE_REROUTE_NONE
                        && !map_page_reroute_allowed(page, fix.longitude,
                            fix.latitude)) {
                        VMAP_NAV_WARN("reroute throttled: %ums since last, keeping route",
                            (unsigned)lv_tick_elaps(page->nav_last_reroute_ms));
                    } else if (reroute_reason != VMAP_ROUTE_REROUTE_NONE) {
                        page->nav_reroute_reason = reroute_reason;
                        page->nav_off_since_ms = 0u;
                        page->nav_last_reroute_ms = now;
                        page->nav_last_reroute_lon = fix.longitude;
                        page->nav_last_reroute_lat = fix.latitude;
                        vmap_route_capture_ridden_history(page->route, st->along_m,
                            fix.longitude, fix.latitude);
                        if (!page->nav_top_only) {
                            lv_pm_bottom_show(
                                vmap_route_reroute_reason_brief(reroute_reason), 0);
                        }
                        if (!map_page_nav_reroute_begin(page, fix.longitude,
                                fix.latitude)) {
                            VMAP_NAV_WARN("reroute begin failed");
                        }
                    }
                    }
                }
            }
            map_page_gpx_stall_part("nav", t_nav);
        }
    }
#endif

    if (page->covered || !page->map) {
        map_page_gpx_stall_log(t0, vis0, cov0, pending0, have_fix && fix.valid);
        return;
    }

#if VMAP_ROUTE_ENABLE
    if (page->nav_review) {
        map_page_gpx_stall_log(t0, vis0, cov0, pending0, have_fix && fix.valid);
        return;
    }
#endif

    /*
     * Keep the retained overscan canvas tracking in the background while
     * another Helm page is visible.  This is cheap until movement crosses the
     * margin; only then does the existing 20 ms tile pump rotate cached tiles
     * into the exposed edge.  Returning to MAP therefore never implies reload.
     */
    if (!page->map_ui_visible) {
        /* 开机还没进过地图：保留清零且隐藏的 canvas，首帧留给实际进入。
         * 否则 GNSS set_center 会提前开半帧，用户立刻翻进来会看到噪点。 */
        if (page->map_ui_seen
            && (page->map_last_follow_ms == 0u
                || lv_tick_elaps(page->map_last_follow_ms)
                    >= MAP_PAGE_HIDDEN_FOLLOW_MS)) {
            page->map_last_follow_ms = lv_tick_get();
            vmap_view_set_center(page->map, fix.longitude, fix.latitude);
            map_page_kick_render(page);
        }
        map_page_gpx_stall_log(t0, vis0, cov0, pending0,
            have_fix && fix.valid);
        return;
    }

    vmap_view_update_arrow(page->map, fix.longitude, fix.latitude, fix.course_deg);
    if (page->map_quiet_until != 0u
        && lv_tick_elaps(page->map_quiet_until) < MAP_PAGE_QUIET_MS) {
        map_page_gpx_stall_log(t0, vis0, cov0, pending0, have_fix && fix.valid);
        return;
    }
    if (page->map_quiet_until != 0u) {
        page->map_quiet_until = 0u;
        map_page_catchup_visible(page);
        map_page_gpx_stall_log(t0, vis0, cov0, pending0,
            have_fix && fix.valid);
        return;
    }
    if (page->map_refresh_pending) {
        page->map_refresh_pending = false;
        page->map_last_follow_ms = lv_tick_get();
        if (vmap_view_render_revision(page->map) == 0) {
            vmap_view_set_view(page->map, fix.longitude, fix.latitude,
                vmap_view_get_zoom(page->map));
            vmap_view_render(page->map);
            map_page_kick_render(page);
        } else if (vmap_view_render_busy(page->map)) {
            map_page_kick_render(page);
        } else {
            vmap_view_set_center(page->map, fix.longitude, fix.latitude);
            if (!vmap_view_render_busy(page->map)) {
                vmap_view_redraw_keep_pan(page->map);
                map_page_kick_render(page);
            }
        }
    } else if (page->map_last_follow_ms == 0u
        || lv_tick_elaps(page->map_last_follow_ms) >= MAP_PAGE_FOLLOW_UPDATE_MS) {
        uint32_t t_follow = lv_tick_get();

        page->map_last_follow_ms = lv_tick_get();
        vmap_view_set_center(page->map, fix.longitude, fix.latitude);
        map_page_kick_render(page);
        map_page_gpx_stall_part("follow", t_follow);
    }
#if VMAP_TRACK_ENABLE
    if (page->track && !vmap_view_render_busy(page->map)) {
        uint32_t t_track = lv_tick_get();

        vmap_track_refresh(page->track);
        map_page_gpx_stall_part("track", t_track);
    }
#endif

    map_page_gpx_stall_log(t0, vis0, cov0, pending0, have_fix && fix.valid);
}

static void map_page_build_status_panel(map_page_t * page, lv_font_t * font16)
{
    LV_UNUSED(page);
    LV_UNUSED(font16);
    /* Helm subbar replaces the old Lap/REC overlay on lv_pm_status_bar_panel. */
}

/**
 * @brief 在 @p parent 上创建地图页。boot_deferred 时只建空壳。
 */
map_page_t * map_page_create(lv_obj_t * parent)
{
    const bicycle_config_t * cfg;
    map_page_t * page = (map_page_t *)calloc(1, sizeof(*page));

    if (!page) {
        return NULL;
    }

    /* boot_deferred 下从创建第一刻起就禁止任何主题/Helm 回调开帧。
     * boot_begin_load() 返回后再置 covered 已经太晚：helm_shell_attach()
     * 会在 map_page_create() 内触发可见性同步。 */
    page->covered = s_boot_deferred;

    bicycle_config_reset_defaults();
    bicycle_config_apply();
    bicycle_runtime_ui_init();
    bicycle_runtime_reset_session();
    bicycle_env_init();
    cfg = bicycle_config();

    page->root = lv_obj_create(parent);
    lv_obj_remove_style_all(page->root);
    lv_obj_set_size(page->root, PAGE_HOR_RES, HELM_PAGE_H);
    lv_obj_align(page->root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(page->root, helm_color(HELM_COLOR_SCR), 0);
    lv_obj_set_style_bg_opa(page->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);

    lv_font_t * font16 = myvendor_system_font_get(16);
    map_page_build_status_panel(page, font16);

    /* Map viewport: scrollable parent clips the 192px canvas overscan. */
    page->map_area = lv_obj_create(page->root);
    lv_obj_remove_style_all(page->map_area);
    /* 地图为常驻导航横幅让位：整体下移半个横幅高度并相应加高，多出的底部被数据条
     * 盖住看不见 —— 于是骑手箭头落在**可见区域**的中心（用户 2026-09-25 指出）。
     * 未导航（横幅隐藏）时地图会略偏下这半个横幅，属可接受的取舍。 */
    lv_obj_set_size(page->map_area, PAGE_HOR_RES,
                    HELM_MAP_BODY_H + HELM_NAV_BAN_SHIFT);
    lv_obj_align(page->map_area, LV_ALIGN_TOP_LEFT, 0,
                 HELM_SUBBAR_H + HELM_NAV_BAN_SHIFT);
    lv_obj_set_style_bg_opa(page->map_area, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(page->map_area, helm_color(HELM_COLOR_SCR), 0);
    lv_obj_set_style_radius(page->map_area, 1, 0);
    lv_obj_set_style_clip_corner(page->map_area, true, 0);
    lv_obj_remove_flag(page->map_area, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(page->map_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page->map_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(page->map_area, LV_DIR_NONE);

    page->map = vmap_view_create(page->map_area, VMAP_MAP_W,
        HELM_MAP_BODY_H + HELM_NAV_BAN_SHIFT,
        VMAP_CANVAS_MARGIN);
    if (!page->map) {
        map_page_destroy(page);
        return NULL;
    }

#if VMAP_TRACK_ENABLE
    page->track = vmap_track_create_on_view(page->map);
    if (!page->track) {
        map_page_destroy(page);
        return NULL;
    }
    if (font16) {
        vmap_track_set_font(page->track, font16);
    }
    vmap_track_set_lap_cb(page->track, on_track_lap, NULL);
#endif

#if VMAP_ROUTE_ENABLE
    VMAP_NAV_LOG("map create: nav deferred until first plan");
#endif

    /* Boot: skip ensure_region here (~880ms LFS). Startup 的地图文件事项
     * 单独完成 catalog/region 打开，但不在 splash 下解码或绘制瓦片。 */
    if (!s_boot_deferred) {
        double boot_lon;
        double boot_lat;

        vmap_boot_lonlat(&boot_lon, &boot_lat);
        vmap_view_set_view(page->map, boot_lon, boot_lat, VMAP_DEFAULT_ZOOM);
        s_boot_center_ready = true;
    } else {
        s_boot_center_ready = false;
    }
    vmap_view_set_scale(page->map, VMAP_DEFAULT_SCALE);
    map_page_sync_zoom_state(page);
    lv_obj_move_background(vmap_view_get_obj(page->map));

    vmap_view_bind_arrow_layer(page->map, page->map_area);
    vmap_view_set_follow_mode(page->map, true);
    {
        navi_arrow_style_t arrow = vmap_style_current()->ui_navi_arrow_dark
            ? NAVI_ARROW_DARK : NAVI_ARROW_LIGHT;

        vmap_view_set_arrow_image(page->map, navi_arrow_get(arrow));
    }

    lv_font_t * map_font = myvendor_system_font_get(16);
    if (map_font) {
        vmap_view_set_font(page->map, map_font);
    }

#if VMAP_TRACK_ENABLE
    vmap_track_set_recording(page->track, cfg->record_on_start);
#endif

    page->gpx_timer = lv_timer_create(on_gpx_timer, cfg->gnss_update_ms, page);
    if (s_boot_deferred && page->gpx_timer) {
        lv_timer_pause(page->gpx_timer);
    }

    page->render_timer = lv_timer_create(on_render_pump, MAP_PAGE_RENDER_PUMP_MS,
        page);
    if (page->render_timer) {
        lv_timer_pause(page->render_timer);
    }

    page->map_theme_id = lv_pm_theme_get();

    /* Bottom panel — bound to bicycle_runtime_t subjects */
    page->panel = lv_obj_create(page->root);
    lv_obj_remove_style_all(page->panel);
    lv_obj_set_size(page->panel, lv_pct(100), VMAP_PANEL_H);
    lv_obj_align(page->panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(page->panel, 0, 0);
    lv_obj_set_flex_flow(page->panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(page->panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(page->panel, 12, 0);
    lv_obj_set_style_pad_ver(page->panel, 8, 0);

    page->speed_lbl = lv_label_create(page->panel);
    lv_label_bind_text(page->speed_lbl, bicycle_runtime_subj_speed_kph(), "%3d km/h");

    page->heading_lbl = lv_label_create(page->panel);
    lv_label_bind_text(page->heading_lbl, bicycle_runtime_subj_course_deg(), "%3d deg");

    page->sensor_lbl = lv_label_create(page->panel);
    lv_label_bind_text(page->sensor_lbl, bicycle_runtime_subj_sensor(), "%s");

#if VMAP_TRACK_ENABLE
    page->dist_lbl = lv_label_create(page->panel);
    lv_label_bind_text(page->dist_lbl, bicycle_runtime_subj_lap_km(), "%s km");
#endif

#if VMAP_ROUTE_ENABLE
    page->nav_lbl = lv_label_create(page->panel);
    lv_label_bind_text(page->nav_lbl, bicycle_runtime_subj_nav_remain_m(), "N%dm");
#endif

    map_page_apply_panel_chrome(page);
    map_page_style_refresh(page);
    lv_obj_add_flag(page->panel, LV_OBJ_FLAG_HIDDEN);

    /* Canvas 已清零且保持 HIDDEN；页面淡入完成后才启动第一帧 pump。 */
    map_page_sync_arrow(page);

    helm_shell_attach(page);
    return page;
}

/**
 * @brief 循环主题并刷新 chrome。
 */
bool map_page_cycle_style(map_page_t * page)
{
    (void)page;
    return lv_pm_theme_set_next();
}

/**
 * @brief 当前主题名。
 */
const char * map_page_style_name(const map_page_t * page)
{
    (void)page;
    return lv_pm_theme_name(lv_pm_theme_get());
}

/**
 * @brief 按样式 id 切主题。
 */
bool map_page_set_style(map_page_t * page, vmap_style_id_t id)
{
    (void)page;

    if (id == VMAP_STYLE_OUTDOOR) {
        return lv_pm_theme_set_by_name("outdoor");
    }

    return lv_pm_theme_set_by_name("classic");
}

/**
 * @brief 按名字切主题。
 */
bool map_page_set_style_by_name(map_page_t * page, const char * name)
{
    (void)page;
    return lv_pm_theme_set_by_name(name);
}

/**
 * @brief 按当前主题重刷 chrome / 箭头。
 */
void map_page_redraw(map_page_t * page)
{
    map_page_style_refresh(page);
}

void map_page_set_arrow_visible(map_page_t * page, bool visible)
{
    if (page == NULL || page->map == NULL) {
        return;
    }

    vmap_view_set_arrow_visible(page->map, visible);
    if (visible) {
        map_page_kick_render(page);
    }
}

/**
 * @brief 开始或停止 REC。开录时创建 TRK_*.gpx。
 */
void map_page_set_recording(map_page_t * page, bool enable)
{
#if VMAP_TRACK_ENABLE
    if (page && page->track) {
        vmap_track_set_recording(page->track, enable);
        map_page_kick_render(page);
    }
#else
    (void)page;
#endif
    bicycle_runtime_set_recording(enable);
    if (enable) {
        if (bicycle_ride_gpx_begin() != 0) {
            lv_pm_notify_show("骑行", "GPX 打开失败", 2000);
        }
    } else {
        (void)bicycle_ride_gpx_commit(NULL, 0);
    }
}

void map_page_seed_track(map_page_t * page, const float * lon,
                         const float * lat, uint16_t n)
{
#if VMAP_TRACK_ENABLE
    uint16_t i;

    if (page == NULL || page->track == NULL || lon == NULL || lat == NULL ||
        n < 2u) {
        return;
    }

    for (i = 0; i < n; i++) {
        (void)vmap_track_push(page->track, (double)lon[i], (double)lat[i], 0.0f);
    }

    map_page_kick_render(page);
#else
    (void)page;
    (void)lon;
    (void)lat;
    (void)n;
#endif
}

void map_page_pause_ride(map_page_t * page)
{
#if VMAP_TRACK_ENABLE
    if (page && page->track) {
        vmap_track_pause(page->track);
        return;
    }
#endif
    (void)page;
    bicycle_runtime_set_recording(false);
}

void map_page_resume_ride(map_page_t * page)
{
#if VMAP_TRACK_ENABLE
    if (page && page->track) {
        vmap_track_resume(page->track);
        return;
    }
#endif
    (void)page;
    bicycle_runtime_set_recording(true);
}

void map_page_end_ride(map_page_t * page)
{
#if VMAP_TRACK_ENABLE
    if (page && page->track) {
        vmap_track_set_recording(page->track, false);
        vmap_track_clear(page->track);
        map_page_kick_render(page);
    }
#endif
    (void)page;
    (void)bicycle_ride_gpx_discard();
    bicycle_runtime_set_recording(false);
    bicycle_runtime_reset_session();
}

void map_page_continue_render(map_page_t * page)
{
    map_page_kick_render(page);
}

lv_obj_t * map_page_root(map_page_t * page)
{
    return page ? page->root : NULL;
}

/**
 * @brief 显示或隐藏地图区。第一次露出时记下 map_ui_seen。
 */
void map_page_set_map_ui_visible(map_page_t * page, bool visible)
{
    bool become;

    if (page == NULL) {
        return;
    }

    become = visible && !page->map_ui_visible;
    page->map_ui_visible = visible;
    if (become) {
        page->map_ui_seen = true;
    }

    if (page->map_area) {
        if (visible) {
            lv_obj_clear_flag(page->map_area, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(page->map_area, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (page->panel) {
        lv_obj_add_flag(page->panel, LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * 页面切换只改 LVGL 可见性，绝不把“进入 MAP”当成加载条件。
     * 隐藏期间 gpx_timer 已按低频率维护 retained canvas；回来先直接显示它，
     * 下一 GNSS 拍只做位置/叠层 catchup，位移未越 margin 就不会开瓦片帧。
     */
    if (become && page->map) {
        if (vmap_view_render_revision(page->map) == 0) {
            map_page_ensure_boot_paint(page);
        }
        page->map_quiet_until = lv_tick_get();
        if (page->map_quiet_until == 0u) {
            page->map_quiet_until = 1u;
        }
        lv_obj_invalidate(vmap_view_get_obj(page->map));
        vmap_view_bring_arrow_to_front(page->map);
    }
}

bool map_page_nav_active(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    const vmap_route_nav_state_t * st;

    if (page == NULL || page->nav == NULL || page->nav_review) {
        return false;
    }

    st = vmap_route_nav_state(page->nav);
    return st != NULL && st->active;
#else
    (void)page;
    return false;
#endif
}

bool map_page_nav_planning(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    return page != NULL && page->nav_planning;
#else
    (void)page;
    return false;
#endif
}

bool map_page_nav_replanning(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    return page != NULL && page->nav_planning && page->nav_replanning;
#else
    (void)page;
    return false;
#endif
}

bool map_page_review_active(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    return page != NULL && page->nav_review;
#else
    (void)page;
    return false;
#endif
}

/**
 * @brief 多站行程是否还有没跑的站。
 *
 * 判据是「当前 leg 是不是最后一站」，不是 vmap_route_trip_is_complete()：
 * 跑到最后一站时 leg_idx 停在 total-1（不会再 advance），complete 仍是 false。
 */
static bool map_page_trip_has_more(const map_page_t * page)
{
    uint32_t leg;
    uint32_t total;

    if (page == NULL || !page->nav_trip_active) {
        return false;
    }

    leg = vmap_route_trip_leg_index(&page->nav_trip);
    total = vmap_route_trip_waypoint_count(&page->nav_trip);

    return total > 0u && leg + 1u < total;
}

bool map_page_nav_arrived(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    const vmap_route_nav_state_t * st;

    if (page == NULL) {
        return false;
    }

    /* 多站行程（常用点 / 坐标点导航多选站点）里，`st->remain_m` 只算到
     * **当前这一站**，到中间站它同样归零 —— 于是这个函数原本会在第一站就
     * 返回 true。后果不只是横幅误显「已到达」：helm_shell 会据此弹出
     * 「导航结束」dock，而那个 dock 上**任意一个按键**都会走
     * helm_arrive_confirm() → map_page_nav_stop()，整趟行程在第一站就被停掉。
     *
     * 所以这里必须补一次行程维度的判断：后面还有站就一律不算「到达」。
     * 最后一站不拦，那时确实该弹结束 dock 由用户按键收尾。
     *
     * 注意这个判断要放在 `nav_arrive_hold` 之前：hold 有可能在行进中被误置
     * （见 tick 里 ahead 预规划挡下推进分支的那条路径），一旦被置位就会
     * 永久短路，后面的行程判定就没机会执行了。 */
    if (map_page_trip_has_more(page)) {
        return false;
    }

    if (page->nav_arrive_hold) {
        return true;
    }

    if (page->nav == NULL) {
        return false;
    }

    st = vmap_route_nav_state(page->nav);
    return st != NULL && st->active && vmap_route_nav_is_arrived(st);
#else
    (void)page;
    return false;
#endif
}

bool map_page_nav_turn_info(const map_page_t * page,
                            char * ico, size_t ico_n,
                            char * dist, size_t dist_n,
                            char * sub, size_t sub_n)
{
#if VMAP_ROUTE_ENABLE
    const vmap_route_nav_state_t * st;
    const char * arrow = "↑";
    const char * label;

    if (page == NULL || page->nav == NULL || ico == NULL || dist == NULL ||
        sub == NULL) {
        return false;
    }

    st = vmap_route_nav_state(page->nav);
    if (st == NULL || !st->active) {
        return false;
    }

    switch (st->next_kind) {
    case VMAP_MANEUVER_LEFT:
    case VMAP_MANEUVER_SLIGHT_LEFT:
    case VMAP_MANEUVER_SHARP_LEFT:
        arrow = "←";
        break;
    case VMAP_MANEUVER_RIGHT:
    case VMAP_MANEUVER_SLIGHT_RIGHT:
    case VMAP_MANEUVER_SHARP_RIGHT:
        arrow = "→";
        break;
    case VMAP_MANEUVER_UTURN:
        arrow = "U";
        break;
    case VMAP_MANEUVER_ARRIVE:
        arrow = "●";
        break;
    default:
        arrow = "↑";
        break;
    }

    label = (st->next_road && st->next_road[0] != '\0')
                ? st->next_road
                : vmap_maneuver_kind_label(st->next_kind);
    if (st->next_kind == VMAP_MANEUVER_NONE
        || st->maneuver_idx == UINT32_MAX) {
        arrow = "↑";
        label = "直行";
        lv_snprintf(dist, dist_n, "%d m",
            (int)(map_page_nav_remain_display_m(page, st) + 0.5));
    } else {
        lv_snprintf(dist, dist_n, "%d m", (int)(st->dist_to_maneuver_m + 0.5));
    }
    lv_snprintf(ico, ico_n, "%s", arrow);
    lv_snprintf(sub, sub_n, "%s", label ? label : "转向");
    return true;
#else
    (void)page;
    (void)ico;
    (void)ico_n;
    (void)dist;
    (void)dist_n;
    (void)sub;
    (void)sub_n;
    return false;
#endif
}

/**
 * @brief 多站行程进度：当前站序号（1 起）与总站数。
 * @return 是多站行程（总站数 >= 2）时为 true；单站导航没有「第几站」可言。
 */
bool map_page_nav_trip_progress(const map_page_t * page, uint32_t * idx,
    uint32_t * total)
{
#if VMAP_ROUTE_ENABLE
    uint32_t leg;
    uint32_t n;

    if (page == NULL || !page->nav_trip_active) {
        return false;
    }

    n = vmap_route_trip_waypoint_count(&page->nav_trip);
    if (n < 2u) {
        return false;
    }

    leg = vmap_route_trip_leg_index(&page->nav_trip);
    if (idx != NULL) {
        *idx = (leg < n) ? leg + 1u : n;
    }
    if (total != NULL) {
        *total = n;
    }
    return true;
#else
    (void)page;
    (void)idx;
    (void)total;
    return false;
#endif
}

bool map_page_nav_status_text(const map_page_t * page, char * buf, size_t n)
{
    if (buf == NULL || n == 0) {
        return false;
    }
    buf[0] = '\0';
#if VMAP_ROUTE_ENABLE
    if (page == NULL) {
        return false;
    }
    if (page->nav_trip_active) {
        const uint32_t leg = vmap_route_trip_leg_index(&page->nav_trip);
        uint32_t trip_idx = 0u;
        uint32_t trip_total = 0u;
        char prog[16] = "";

        /* 多站行程带上「1/3」：路线只画当前一段、剩余里程也只算到当前站，
         * 不加这个角标，中间站看起来就跟终点一样。
         *
         * 角标放**前面**：调用方的缓冲只有 24 字节（helm_shell 的 nav_st），
         * 站名又允许 23 字节，放后面会被长站名挤掉 —— 那就白加了。 */
        if (map_page_nav_trip_progress(page, &trip_idx, &trip_total)) {
            lv_snprintf(prog, sizeof(prog), "%u/%u · ", (unsigned)trip_idx,
                (unsigned)trip_total);
        }

        if (leg < page->nav_trip_name_count
            && page->nav_trip_names[leg][0] != '\0') {
            lv_snprintf(buf, n, "%s%s", prog, page->nav_trip_names[leg]);
        } else {
            lv_snprintf(buf, n, "%u/%u", (unsigned)(leg + 1u),
                (unsigned)vmap_route_trip_waypoint_count(&page->nav_trip));
        }
        return true;
    }
    if (page->nav_gpx_course && page->nav_title[0] != '\0') {
        lv_snprintf(buf, n, "%s", page->nav_title);
        return true;
    }
    if (map_page_nav_active(page)) {
        lv_snprintf(buf, n, "导航");
        return true;
    }
#else
    (void)page;
#endif
    return false;
}

double map_page_nav_off_warn_m(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    if (page && page->nav_gpx_course) {
        return map_page_gpx_off_threshold(page);
    }
#else
    (void)page;
#endif
    return 25.0;
}

double map_page_nav_turn_dist_m(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    const vmap_route_nav_state_t * st;

    if (page == NULL || page->nav == NULL) {
        return -1.0;
    }
    st = vmap_route_nav_state(page->nav);
    if (st == NULL || !st->active) {
        return -1.0;
    }
    /* 没有下一个转向点（纯轨迹跟随、或前方无路口）不是"临近路口"。 */
    if (st->next_kind == VMAP_MANEUVER_NONE || st->maneuver_idx == UINT32_MAX) {
        return -1.0;
    }
    return st->dist_to_maneuver_m;
#else
    (void)page;
    return -1.0;
#endif
}

double map_page_nav_remain_m(const map_page_t * page)
{
#if VMAP_ROUTE_ENABLE
    const vmap_route_nav_state_t * st;

    if (page == NULL || page->nav == NULL) {
        return -1.0;
    }
    st = vmap_route_nav_state(page->nav);
    if (st == NULL || !st->active) {
        return -1.0;
    }
    return map_page_nav_remain_display_m(page, st);
#else
    (void)page;
    return -1.0;
#endif
}

double map_page_nav_total_m(const map_page_t * page)
{
    if (page == NULL) {
        return -1.0;
    }
    /* GPX 跟线才存全长；路网规划出来的路线这里没有"全程"的概念。 */
    if (page->nav_gpx_total_m > 0.0) {
        return page->nav_gpx_total_m;
    }
    return -1.0;
}

double map_page_map_mpp(const map_page_t * page)
{
    const double k_mercator = 156543.03392; /* 赤道处 zoom 0 的米/像素（256 px 瓦片） */
    double lon = 0.0;
    double lat = 0.0;
    double scale;
    int zoom;

    if (page == NULL || page->map == NULL) {
        return 0.0;
    }
    zoom = vmap_view_get_zoom(page->map);
    scale = vmap_view_get_scale(page->map);
    if (zoom < 0 || scale <= 0.0) {
        return 0.0;
    }
    vmap_view_get_center(page->map, &lon, &lat);
    return k_mercator * cos(lat * M_PI / 180.0) / (double)(1u << zoom) / scale;
}

void map_page_set_banner_inset(map_page_t * page, int32_t px)
{
    static int32_t s_inset = -1;

    if (page == NULL || page->map_area == NULL) {
        return;
    }
    if (px < 0) {
        px = 0;
    }
    if (px > HELM_MAP_BODY_H / 2) {
        px = HELM_MAP_BODY_H / 2;
    }
    if (px == s_inset) {
        return;                      /* 幂等：几何只在值变化时动，避免每拍重排 */
    }
    s_inset = px;

    /* 可见窗口：容器上移半个预留高度、底部收掉同样多 —— 于是**视图中心**（跟车点）
     * 正好落在"横幅下沿 → 数据条"这段可见窗口的中心；容器顶部那半个横幅被横幅挡住，
     * 看不见。关键是**整体移动容器**（视图与箭头都在容器坐标系里），不能只挪视图。 */
    lv_obj_set_pos(page->map_area, 0, HELM_SUBBAR_H + px / 2);
    lv_obj_set_height(page->map_area, HELM_MAP_BODY_H - px / 2);

    /* 只动容器，**不动视图对象**：箭头图层就是 map_area 本身
     * （vmap_view_bind_arrow_layer(page->map, page->map_area)），箭头对象挂在它下面。
     * 把视图在容器里上移会让"画布内容"和"箭头坐标"差半个横幅 ——
     * 用户 2026-09-25 实测"指针和实际地图位置出现偏差"正是这个原因。 */

    /* 几何变了要重画一次：否则画布/覆盖层（含 GPX 细蓝线）会留下按旧尺寸烘的残图，
     * 表现为轨迹线缺段（用户 2026-09-25 同时报的那一条）。 */
    vmap_view_redraw_keep_pan(page->map);
}

#if VMAP_ROUTE_ENABLE
int16_t map_page_sample_ele_m(const map_page_t * page, double lon, double lat)
{
    uint32_t node = UINT32_MAX;
    double d;

    (void)page;
    if (!s_route_graph) {
        return (int16_t)VGRF_ELE_UNKNOWN;
    }

    if (s_dem_node != UINT32_MAX) {
        d = vmap_geo_haversine_m(lon, lat, s_dem_lon, s_dem_lat);
        if (d >= 0.0 && d < MAP_PAGE_DEM_CACHE_M) {
            return vmap_route_graph_node_ele_m(s_route_graph, s_dem_node);
        }
    }

    d = vmap_route_graph_nearest_distance_m(s_route_graph, lon, lat, &node);
    if (d < 0.0 || node == UINT32_MAX || d > VMAP_ROUTE_SNAP_M) {
        return (int16_t)VGRF_ELE_UNKNOWN;
    }
    s_dem_node = node;
    s_dem_lon = lon;
    s_dem_lat = lat;
    return vmap_route_graph_node_ele_m(s_route_graph, node);
}

int32_t map_page_nav_remain_gain_m(const map_page_t * page)
{
    const vmap_route_nav_state_t * st;

    if (page == NULL || page->route == NULL || page->nav == NULL) {
        return -1;
    }
    st = vmap_route_nav_state(page->nav);
    if (st == NULL || !st->active) {
        return -1;
    }
    return vmap_route_remain_gain_m(page->route, st->along_m);
}

uint32_t map_page_nav_profile_ele(const map_page_t * page, int16_t * out,
    uint32_t cap, uint32_t * now_idx)
{
    const vmap_route_nav_state_t * st;
    double along;
    double total;
    uint32_t n;

    if (now_idx) {
        *now_idx = 0;
    }
    if (page == NULL || page->route == NULL || page->nav == NULL || out == NULL) {
        return 0;
    }
    st = vmap_route_nav_state(page->nav);
    if (st == NULL || !st->active) {
        return 0;
    }

    along = st->along_m;
    total = vmap_route_total_length_m(page->route);
    if (along < 0.0) {
        along = 0.0;
    }
    if (along > total) {
        along = total;
    }
    n = vmap_route_profile_ele_span(page->route, 0.0, total, out, cap);
    if (now_idx && n >= 2u && total > 0.0) {
        double t = along / total;

        if (t < 0.0) {
            t = 0.0;
        } else if (t > 1.0) {
            t = 1.0;
        }
        *now_idx = (uint32_t)(t * (double)(n - 1u) + 0.5);
        if (*now_idx >= n) {
            *now_idx = n - 1u;
        }
    }
    return n;
}

bool map_page_nav_progress(const map_page_t * page, uint32_t * pts,
    uint32_t * total_cm, uint32_t * now_idx, uint32_t prof_n)
{
    const vmap_route_nav_state_t * st;
    double along;
    double total;
    uint32_t npts;

    if (pts) {
        *pts = 0;
    }
    if (total_cm) {
        *total_cm = 0;
    }
    if (now_idx) {
        *now_idx = 0;
    }
    if (page == NULL || page->route == NULL || page->nav == NULL) {
        return false;
    }
    st = vmap_route_nav_state(page->nav);
    if (st == NULL || !st->active) {
        return false;
    }

    npts = vmap_route_point_count(page->route);
    total = vmap_route_total_length_m(page->route);
    along = st->along_m;
    if (along < 0.0) {
        along = 0.0;
    }
    if (along > total) {
        along = total;
    }
    if (pts) {
        *pts = npts;
    }
    if (total_cm) {
        *total_cm = (uint32_t)(total * 100.0 + 0.5);
    }
    if (now_idx && prof_n >= 2u && total > 0.0) {
        double t = along / total;

        if (t < 0.0) {
            t = 0.0;
        } else if (t > 1.0) {
            t = 1.0;
        }
        *now_idx = (uint32_t)(t * (double)(prof_n - 1u) + 0.5);
        if (*now_idx >= prof_n) {
            *now_idx = prof_n - 1u;
        }
    }
    return npts >= 2u;
}
#endif

static void map_page_zoom_redraw(map_page_t * page)
{
    if (!page || !page->map || page->covered) {
        return;
    }

    vmap_view_render(page->map);
    map_page_kick_render(page);
    vmap_view_bring_arrow_to_front(page->map);
    helm_shell_raise_chrome();
#if VMAP_TRACK_ENABLE
    if (page->track) {
        vmap_track_refresh(page->track);
    }
#endif
}

/**
 * @brief 把当前 zoom/scale 写进 ctl 状态。
 */
void map_page_sync_zoom_state(map_page_t * page)
{
    char buf[48];

    if (!page || !page->map) {
        return;
    }

    snprintf(buf, sizeof(buf), "z=%d,s=%.2f",
        vmap_view_get_zoom(page->map), vmap_view_get_scale(page->map));
    myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_MAP_ZOOM, buf);
}

static bool map_page_zoom_set_tile(map_page_t * page, int zoom)
{
    double lon;
    double lat;

    if (zoom < VMAP_ZOOM_MIN) {
        zoom = VMAP_ZOOM_MIN;
    } else if (zoom > VMAP_ZOOM_MAX) {
        zoom = VMAP_ZOOM_MAX;
    }

    vmap_view_get_center(page->map, &lon, &lat);
    vmap_view_set_view(page->map, lon, lat, zoom);
    return true;
}

/**
 * @brief 在 LVGL 线程应用缩放 ctl：in/out/reset/z,N/s,F。
 */
bool map_page_apply_zoom_cmd(map_page_t * page, const char * arg)
{
    if (!page || !page->map || arg == NULL || arg[0] == '\0') {
        return false;
    }

    if (strcmp(arg, "in") == 0) {
        vmap_view_set_scale(page->map,
            vmap_view_get_scale(page->map) * VMAP_SCALE_STEP);
    } else if (strcmp(arg, "out") == 0) {
        vmap_view_set_scale(page->map,
            vmap_view_get_scale(page->map) / VMAP_SCALE_STEP);
    } else if (strcmp(arg, "reset") == 0) {
        double lon;
        double lat;

        vmap_view_get_center(page->map, &lon, &lat);
        vmap_view_set_view(page->map, lon, lat, VMAP_DEFAULT_ZOOM);
        vmap_view_set_scale(page->map, VMAP_DEFAULT_SCALE);
    } else if (strncmp(arg, "z,", 2) == 0) {
        const char * p = arg + 2;
        char * end = NULL;
        long z = strtol(p, &end, 10);

        if (end == p) {
            return false;
        }
        map_page_zoom_set_tile(page, (int)z);
    } else if (strncmp(arg, "s,", 2) == 0) {
        const char * p = arg + 2;
        char * end = NULL;
        double s = strtod(p, &end);

        if (end == p) {
            return false;
        }
        vmap_view_set_scale(page->map, s);
    } else if (strcmp(arg, "zin") == 0) {
        map_page_zoom_set_tile(page, vmap_view_get_zoom(page->map) + 1);
    } else if (strcmp(arg, "zout") == 0) {
        map_page_zoom_set_tile(page, vmap_view_get_zoom(page->map) - 1);
    } else {
        return false;
    }

    map_page_zoom_redraw(page);
    map_page_sync_zoom_state(page);
    return true;
}

#if VMAP_ROUTE_ENABLE
/**
 * @brief 规划单点路线。from 为 NAN 则用当前 GNSS。
 */
bool map_page_nav_plan(map_page_t * page, double from_lon, double from_lat,
    double to_lon, double to_lat)
{
    if (!page) {
        return false;
    }

    if (isnan(from_lon) || isnan(from_lat)) {
        bicycle_gnss_fix_t fix;

        if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
            VMAP_NAV_WARN("nav_plan: no valid GNSS fix for start");
            return false;
        }
        from_lon = fix.longitude;
        from_lat = fix.latitude;
    }

    map_page_nav_gnss_for_gps(page);
    map_page_gpx_course_clear(page);

    return map_page_nav_plan_begin(page, from_lon, from_lat, to_lon, to_lat, true,
        false);
}

/**
 * @brief 规划后按模拟速度沿折线驱动。
 */
bool map_page_nav_plan_sim(map_page_t * page, double from_lon, double from_lat,
    double to_lon, double to_lat)
{
    if (!page) {
        return false;
    }

    if (isnan(from_lon) || isnan(from_lat)) {
        bicycle_gnss_fix_t fix;

        if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
            VMAP_NAV_WARN("nav_plan_sim: no valid GNSS fix for start");
            return false;
        }
        from_lon = fix.longitude;
        from_lat = fix.latitude;
    }

    map_page_nav_gnss_for_sim(page);
    map_page_gpx_course_clear(page);

    return map_page_nav_plan_begin(page, from_lon, from_lat, to_lon, to_lat, true,
        false);
}

/**
 * @brief 多途经点规划：当前坐标不算，一次规划下一点与下下一点。
 */
bool map_page_nav_trip_plan(map_page_t * page,
    const vmap_route_waypoint_t * waypoints, uint32_t count)
{
    bicycle_gnss_fix_t fix;
    bool mtp_was_held;
    bool top_was_only;

    if (!page || !waypoints || count == 0) {
        return false;
    }
    memset(page->nav_trip_names, 0, sizeof(page->nav_trip_names));
    page->nav_trip_name_count = 0u;
    if (page->nav_planning) {
        return false;
    }
    top_was_only = page->nav_top_only;
    page->nav_top_only = true;
    mtp_was_held = page->nav_mtp_held;
    if (!map_page_nav_mtp_acquire(page)) {
        page->nav_top_only = top_was_only;
        return false;
    }
    if (!map_page_ensure_route_nav(page)) {
        page->nav_top_only = top_was_only;
        if (!mtp_was_held) {
            map_page_nav_mtp_release(page);
        }
        return false;
    }
    if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
        fix.longitude = (float)waypoints[0].lon;
        fix.latitude = (float)waypoints[0].lat;
        fix.valid = true;
        VMAP_NAV_WARN("nav_trip: no GNSS, start at first waypoint");
    }
    if (myvendor_mtp_lfs_quiesce()) {
        page->nav_top_only = top_was_only;
        if (!mtp_was_held) {
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    vmap_route_worker_init();
    if (vmap_route_worker_busy()) {
        page->nav_top_only = top_was_only;
        if (!mtp_was_held) {
            map_page_nav_mtp_release(page);
        }
        return false;
    }
    map_page_gpx_rejoin_reset(page);
    vmap_route_worker_cancel();
    vmap_route_nav_stop(page->nav);
    vmap_route_trip_reset(&page->nav_trip);
    map_page_gpx_course_clear(page);
    map_page_trip_ahead_clear(page);
    if (!vmap_route_trip_set_waypoints(&page->nav_trip, waypoints, count)) {
        page->nav_top_only = top_was_only;
        if (!mtp_was_held) {
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    /* 站在某站上＝那一站已经过掉（trip 的语义本来就是「当前坐标不算」）：
     * 把已经在脚下的前置站跳掉，从第一个真正没到的站开始规划。
     *
     * 不跳的话这段 leg 长度 ~0，`vmap_route_compute()` 会以 "already at dest"
     * 拒绝，用户看到的就是「已在终点附近」—— 哪怕后面还有好几站没去。
     * 返回 false 表示连最后一站也在脚下，那才是真的没什么可导的。
     *
     * 顺带把上面「无定位时把起点当成第 1 个站」那条兜底也接上了：那种情况下
     * `skip_reached()` 会跳过 wps[0]，leg 正好从第 2 个站起算，与那句
     * "start at first waypoint" 的注释一致（在此之前它实际会去规划一条
     * 起点＝终点的 0 长度 leg，必然失败）。 */
    if (!vmap_route_trip_skip_reached(&page->nav_trip, fix.longitude,
            fix.latitude)) {
        vmap_route_trip_reset(&page->nav_trip);
        page->nav_top_only = top_was_only;
        if (!mtp_was_held) {
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    page->nav_trip_active = true;
    page->nav_gpx_course = false;
    page->nav_user_set = true;
    page->nav_planned = false;
    page->nav_replanning = false;
    page->nav_reroute_reason = VMAP_ROUTE_REROUTE_NONE;
    page->nav_last_reroute_ms = 0u;
    page->nav_last_reroute_lon = 0.0;
    page->nav_last_reroute_lat = 0.0;
    page->nav_off_since_ms = 0u;
    /* 用户是从菜单里点的导航，此刻地图还盖在菜单下面，refresh_map 会早退；
     * 这里先把剩余航点挂上，菜单一收起就是对的。 */
    map_page_trip_pins_sync(page);

    /* 当前坐标不算：规划 GPS→当前站，并在完成后静默预规划下一站。
     * 起始站可能不是第 1 个（前面几站人已经站在上面了，见上面的 skip）。 */
    VMAP_NAV_LOG("nav_trip: %u waypoints, start leg %u/%u", (unsigned)count,
        (unsigned)(vmap_route_trip_leg_index(&page->nav_trip) + 1u),
        (unsigned)count);
    map_page_plan_hint_show(page, "加载路网");
    if (!vmap_route_trip_begin_leg(VMAP_TILE_DIR, &page->nav_trip,
            fix.longitude, fix.latitude)) {
        page->nav_trip_active = false;
        page->nav_top_only = top_was_only;
        vmap_route_trip_reset(&page->nav_trip);
        map_page_plan_hint_hide(page);
        if (!mtp_was_held) {
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    if (map_page_nav_rolling_begin(page, fix.longitude, fix.latitude, false,
            false)) {
        helm_shell_on_nav_started();
        return true;
    }

    map_page_plan_hint_hide(page);
    page->nav_top_only = top_was_only;
    if (!mtp_was_held) {
        map_page_nav_mtp_release(page);
    }
    return false;
}

void map_page_nav_trip_set_names(map_page_t * page,
    const char * const * names, uint32_t count)
{
    uint32_t i;

    if (!page) {
        return;
    }
    memset(page->nav_trip_names, 0, sizeof(page->nav_trip_names));
    page->nav_trip_name_count = 0u;
    if (!names) {
        return;
    }
    if (count > VMAP_ROUTE_TRIP_MAX_WAYPOINTS) {
        count = VMAP_ROUTE_TRIP_MAX_WAYPOINTS;
    }
    for (i = 0u; i < count; i++) {
        if (names[i] != NULL) {
            lv_snprintf(page->nav_trip_names[i],
                sizeof(page->nav_trip_names[i]), "%s", names[i]);
        }
    }
    page->nav_trip_name_count = (uint8_t)count;
}

/**
 * @brief 停止导航并清路线叠层。
 */
void map_page_nav_stop(map_page_t * page)
{
    const bool was_route_sim = page && page->nav_route_sim;

    if (!page) {
        VMAP_NAV_LOG("nav_stop: noop null page");
        return;
    }
    if (!page->nav) {
        VMAP_NAV_LOG("nav_stop: noop nav=%p", (void *)(page ? page->nav : NULL));
        map_page_gpx_course_clear(page);
        map_page_trip_ahead_clear(page);
        map_page_plan_hint_hide(page);
        map_page_nav_mtp_release(page);
        return;
    }

    VMAP_NAV_LOG("nav_stop");
    map_page_gpx_rejoin_reset(page);
    map_page_gpx_course_clear(page);
    map_page_trip_ahead_clear(page);
    page->nav_review = false;
    if (page->map) {
        vmap_view_set_arrow_visible(page->map, true);
    }
    vmap_route_worker_cancel();
    page->nav_planning = false;
    page->nav_replanning = false;
    page->nav_reroute_reason = VMAP_ROUTE_REROUTE_NONE;
    map_page_plan_hint_hide(page);
    vmap_route_nav_stop(page->nav);
    map_page_release_route_graph(page);
    vmap_route_trip_reset(&page->nav_trip);
    memset(page->nav_trip_names, 0, sizeof(page->nav_trip_names));
    page->nav_trip_name_count = 0u;
    page->nav_trip_active = false;
    page->nav_top_only = false;
    page->nav_gpx_course = false;
    page->nav_refine_extend = false;
    page->nav_planned = false;
    page->nav_user_set = false;
    page->nav_route_sim = false;
    page->nav_arrive_hold = false;
    page->nav_title[0] = '\0';
    page->nav_maneuver_announced = UINT32_MAX;
    page->nav_last_update_ms = 0;
    page->nav_last_reroute_ms = 0;
    page->nav_off_since_ms = 0u;
    if (was_route_sim) {
        bicycle_route_sim_stop();
    }
    map_page_nav_mtp_release(page);
    map_page_ensure_gpx_sim(page);
    bicycle_runtime_update_nav(0.0, 0.0, 0.0f);
    if (page->map) {
        /* Route is stamped into the canvas like the ride track. Clear the
         * polyline first (above), then restamp tiles so the blue ink goes. */
        vmap_view_redraw_keep_pan(page->map);
        map_page_kick_render(page);
        if (page->covered || !page->map_ui_visible) {
            page->map_refresh_pending = true;
        }
    }
    helm_shell_refresh();
}

/** @brief 相邻点平均间距不超过此值则沿 GPX 折线跟线，否则按窗口走两点路网规划。 */
#define MAP_PAGE_GPX_DENSE_AVG_M     200.0
#define MAP_PAGE_GPX_MIN_KEEP_M      1.0
/* 量航向的窗口。原来是 25 m —— 2026-09-25 用用户实测轨迹（20260529户外骑行.gpx，20.3 km，
 * 抽稀后 1493 点）离线复算：±25 m 只测出 2 个转向点，且把 9.1 km 处那个 40° 的**真实路口**
 * 漏掉了（该弯摊在 ~150 m 上，±25 m 窗口内只累积约 23°，卡在 VMAP_ROUTE_MANEUVER_DEG 之下）。
 * 实测窗口敏感性：±25 m ⇒ 2 个、±40 m ⇒ 5 个、±60 m ⇒ 8~11 个；用户 2026-09-25 选定 60 m，
 * 提示更勤（代价：河堤那种长半径缓弯也会被算成"微转"，图标与转向同级都是箭头，无碍）。
 * 窗口是**按距离**量的（见 map_page_gpx_turn_window），所以抽稀到 13.6 m 点距不影响判据。 */
#define MAP_PAGE_GPX_TURN_WINDOW_M   60.0
#define MAP_PAGE_GPX_TURN_MIN_GAP_M  40.0

static void map_page_gpx_store_pt(vmap_route_pt_t * dst, const gpx_point_t * src)
{
    dst->lon = (double)src->longitude;
    dst->lat = (double)src->latitude;
    if (src->has_altitude) {
        const float a = src->altitude;

        if (a > 32767.0f) {
            dst->ele_m = 32767;
        } else if (a < -32767.0f) {
            dst->ele_m = -32767;
        } else {
            dst->ele_m = (int16_t)lroundf(a);
        }
    } else {
        dst->ele_m = (int16_t)VGRF_ELE_UNKNOWN;
    }
}

static bool map_page_gpx_load(const char * path, vmap_route_pt_t * pts,
    uint32_t cap, uint32_t * kept_out, uint32_t * file_n_out, double * len_out,
    double * true_len_out)
{
    gpx_decode_t * dec = NULL;
    gpx_decode_cfg_t cfg;
    gpx_decode_stats_t st;
    gpx_point_t batch[16];
    unsigned n;
    int ret;
    uint32_t kept = 0;
    double len = 0.0;

    if (path == NULL || pts == NULL || cap < 2u || kept_out == NULL) {
        return false;
    }

    memset(&st, 0, sizeof(st));
    memset(batch, 0, sizeof(batch));
    gpx_decode_cfg_sparse(&cfg, cap);
    cfg.batch_max = 16;
    if (gpx_decode_open(path, &cfg, &dec) != 0) {
        return false;
    }

    for (;;) {
        unsigned i;

        memset(batch, 0, sizeof(batch));
        ret = gpx_decode_read(dec, batch, 16, &n);
        if (ret < 0) {
            break;
        }

        for (i = 0; i < n; i++) {
            if (batch[i].latitude == 0.0f && batch[i].longitude == 0.0f) {
                continue;
            }

            if (kept > 0u) {
                const double d = vmap_geo_haversine_m(pts[kept - 1u].lon,
                    pts[kept - 1u].lat, (double)batch[i].longitude,
                    (double)batch[i].latitude);

                if (d < MAP_PAGE_GPX_MIN_KEEP_M) {
                    map_page_gpx_store_pt(&pts[kept - 1u], &batch[i]);
                    continue;
                }

                if (d > 100000.0) {
                    continue;
                }

                len += d;
            }

            if (kept < cap) {
                map_page_gpx_store_pt(&pts[kept], &batch[i]);
                kept++;
            } else {
                map_page_gpx_store_pt(&pts[cap - 1u], &batch[i]);
            }
        }

        if (ret == 1) {
            break;
        }
    }

    (void)gpx_decode_get_stats(dec, &st);
    gpx_decode_close(&dec);
    if (kept < 2u) {
        return false;
    }

    *kept_out = kept;
    if (file_n_out) {
        *file_n_out = st.file_points > 0u ? st.file_points : kept;
    }
    if (len_out) {
        *len_out = len;
    }
    if (true_len_out) {
        *true_len_out = (st.length_m > len) ? st.length_m : len;
    }
    return true;
}

static double map_page_gpx_seg_m(const vmap_route_pt_t * a,
    const vmap_route_pt_t * b)
{
    return vmap_geo_haversine_m(a->lon, a->lat, b->lon, b->lat);
}

static uint32_t map_page_gpx_idx_at(const vmap_route_pt_t * pts, uint32_t n,
    double target_m, double * along_at_idx)
{
    double acc = 0.0;
    uint32_t i;

    if (!pts || n < 2u || target_m <= 0.0) {
        if (along_at_idx) {
            *along_at_idx = 0.0;
        }
        return 0u;
    }
    for (i = 0u; i + 1u < n; i++) {
        const double d = map_page_gpx_seg_m(&pts[i], &pts[i + 1u]);

        if (acc + d >= target_m) {
            if (along_at_idx) {
                *along_at_idx = acc;
            }
            return i;
        }
        acc += d;
    }
    if (along_at_idx) {
        *along_at_idx = acc;
    }
    return n >= 2u ? n - 2u : 0u;
}

static bool map_page_gpx_slice(const vmap_route_pt_t * pts, uint32_t n,
    double from_m, double window_m, uint32_t * i0, uint32_t * out_n,
    double * start_m, double * len_m)
{
    uint32_t a;
    uint32_t b;
    uint32_t i;
    double a_along;
    double acc;

    if (!pts || n < 2u || !i0 || !out_n || window_m <= 0.0) {
        return false;
    }
    if (from_m < 0.0) {
        from_m = 0.0;
    }
    a = map_page_gpx_idx_at(pts, n, from_m, &a_along);
    if (a + 1u >= n) {
        return false;
    }
    acc = 0.0;
    b = a + 1u;
    for (i = a; i + 1u < n; i++) {
        acc += map_page_gpx_seg_m(&pts[i], &pts[i + 1u]);
        b = i + 1u;
        if (acc >= window_m) {
            break;
        }
    }
    *i0 = a;
    *out_n = b - a + 1u;
    if (start_m) {
        *start_m = a_along;
    }
    if (len_m) {
        *len_m = acc;
    }
    return *out_n >= 2u && acc > 1.0;
}

static bool map_page_gpx_point_at(const vmap_route_pt_t * pts, uint32_t n,
    double along_m, double * lon, double * lat)
{
    double acc = 0.0;
    uint32_t i;

    if (!pts || n == 0u || !lon || !lat) {
        return false;
    }
    if (n == 1u || along_m <= 0.0) {
        *lon = pts[0].lon;
        *lat = pts[0].lat;
        return true;
    }
    for (i = 0u; i + 1u < n; i++) {
        const double d = map_page_gpx_seg_m(&pts[i], &pts[i + 1u]);

        if (acc + d >= along_m || i + 2u == n) {
            double t = d > 0.0 ? (along_m - acc) / d : 1.0;

            if (t < 0.0) {
                t = 0.0;
            } else if (t > 1.0) {
                t = 1.0;
            }
            *lon = pts[i].lon + (pts[i + 1u].lon - pts[i].lon) * t;
            *lat = pts[i].lat + (pts[i + 1u].lat - pts[i].lat) * t;
            return true;
        }
        acc += d;
    }
    *lon = pts[n - 1u].lon;
    *lat = pts[n - 1u].lat;
    return true;
}

static double map_page_gpx_nearest_along(const vmap_route_pt_t * pts, uint32_t n,
    double lon, double lat, double * dist_m)
{
    double acc = 0.0;
    double best_along = 0.0;
    double best_d = 1e300;
    uint32_t i;

    if (!pts || n == 0u) {
        if (dist_m) {
            *dist_m = 1e300;
        }
        return 0.0;
    }
    if (n == 1u) {
        best_d = vmap_geo_haversine_m(lon, lat, pts[0].lon, pts[0].lat);
        if (dist_m) {
            *dist_m = best_d;
        }
        return 0.0;
    }
    for (i = 0u; i + 1u < n; i++) {
        double t = 0.0;
        const double d = vmap_geo_point_to_seg_m(lon, lat, pts[i].lon,
            pts[i].lat, pts[i + 1u].lon, pts[i + 1u].lat, &t);
        const double seg_m = map_page_gpx_seg_m(&pts[i], &pts[i + 1u]);

        if (d < best_d) {
            best_d = d;
            best_along = acc + seg_m * t;
        }
        acc += seg_m;
    }
    if (dist_m) {
        *dist_m = best_d;
    }
    return best_along;
}

static bool map_page_gpx_join_fix_ok(const bicycle_gnss_fix_t * fix)
{
    if (fix == NULL || !fix->valid || fix->fix_quality < 2) {
        return false;
    }
    /* HDOP 0 = 未知；有值则须 ≤1.8（与 runtime MID 一致）。 */
    if (fix->hdop_x10 != 0u && fix->hdop_x10 > 18u) {
        return false;
    }
    return true;
}

static bool map_page_gpx_start_join(const map_page_t * page,
    const bicycle_gnss_fix_t * fix, double * from_m)
{
    double dist = 1e300;
    double along;

    if (from_m) {
        *from_m = 0.0;
    }
    if (!page || !page->nav_gpx_pts || page->nav_gpx_n < 2u
        || !map_page_gpx_join_fix_ok(fix)) {
        return false;
    }
    along = map_page_gpx_nearest_along(page->nav_gpx_pts, page->nav_gpx_n,
        (double)fix->longitude, (double)fix->latitude, &dist);
    if (dist > VMAP_NAV_GPX_START_JOIN_MAX_M) {
        VMAP_NAV_LOG("gpx start skip join dist=%.0f (need <=%.0f)",
            dist, VMAP_NAV_GPX_START_JOIN_MAX_M);
        return false;
    }
    if (along > VMAP_NAV_GPX_OVERLAP_M) {
        along -= VMAP_NAV_GPX_OVERLAP_M;
    } else {
        along = 0.0;
    }
    if (from_m) {
        *from_m = along;
    }
    return true;
}

static vmap_maneuver_kind_t map_page_gpx_turn_kind(float turn_deg)
{
    const float a = fabsf(turn_deg);

    if (a >= 150.0f) {
        return VMAP_MANEUVER_UTURN;
    }
    if (a >= 100.0f) {
        return turn_deg < 0.0f ? VMAP_MANEUVER_SHARP_LEFT
                               : VMAP_MANEUVER_SHARP_RIGHT;
    }
    if (a >= 35.0f) {
        return turn_deg < 0.0f ? VMAP_MANEUVER_LEFT : VMAP_MANEUVER_RIGHT;
    }
    if (a >= 15.0f) {
        return turn_deg < 0.0f ? VMAP_MANEUVER_SLIGHT_LEFT
                               : VMAP_MANEUVER_SLIGHT_RIGHT;
    }
    return VMAP_MANEUVER_STRAIGHT;
}

static bool map_page_gpx_turn_window(const vmap_route_pt_t * pts, uint32_t n,
    uint32_t at, uint32_t * prev, uint32_t * next)
{
    double back_m = 0.0;
    double ahead_m = 0.0;
    uint32_t p;
    uint32_t q;

    if (!pts || at == 0u || at + 1u >= n || !prev || !next) {
        return false;
    }

    p = at;
    while (p > 0u && back_m < MAP_PAGE_GPX_TURN_WINDOW_M) {
        back_m += vmap_geo_haversine_m(pts[p - 1u].lon, pts[p - 1u].lat,
            pts[p].lon, pts[p].lat);
        p--;
    }

    q = at;
    while (q + 1u < n && ahead_m < MAP_PAGE_GPX_TURN_WINDOW_M) {
        ahead_m += vmap_geo_haversine_m(pts[q].lon, pts[q].lat,
            pts[q + 1u].lon, pts[q + 1u].lat);
        q++;
    }

    if (back_m < MAP_PAGE_GPX_TURN_WINDOW_M
        || ahead_m < MAP_PAGE_GPX_TURN_WINDOW_M) {
        return false;
    }

    *prev = p;
    *next = q;
    return true;
}

static uint32_t map_page_gpx_build_maneuvers(const vmap_route_pt_t * pts,
    uint32_t n, double total_m, vmap_route_maneuver_t * mans, uint32_t cap)
{
    uint32_t n_man = 0;
    double along = 0.0;
    double pending_along = 0.0;
    float pending_turn = 0.0f;
    bool pending = false;
    uint32_t i;

    if (!pts || n < 2u || !mans || cap < 2u) {
        return 0;
    }

    memset(mans, 0, (size_t)cap * sizeof(*mans));
    mans[n_man].kind = VMAP_MANEUVER_START;
    n_man++;

    for (i = 1u; i + 1u < n && n_man + 1u < cap; i++) {
        const double d01 = vmap_geo_haversine_m(pts[i - 1u].lon, pts[i - 1u].lat,
            pts[i].lon, pts[i].lat);
        uint32_t prev;
        uint32_t next;
        float turn;

        along += d01;
        if (!map_page_gpx_turn_window(pts, n, i, &prev, &next)) {
            continue;
        }

        turn = vmap_geo_angle_delta_deg(
            vmap_geo_bearing_deg(pts[prev].lon, pts[prev].lat,
                pts[i].lon, pts[i].lat),
            vmap_geo_bearing_deg(pts[i].lon, pts[i].lat,
                pts[next].lon, pts[next].lat));
        if (fabsf(turn) < VMAP_ROUTE_MANEUVER_DEG) {
            continue;
        }

        /*
         * A recorded GPX usually contains points only a few metres apart.
         * Measure headings over distance windows, then keep the strongest
         * direction change in each junction-sized cluster.
         */
        if (pending && along - pending_along < MAP_PAGE_GPX_TURN_MIN_GAP_M) {
            if (fabsf(turn) > fabsf(pending_turn)) {
                pending_along = along;
                pending_turn = turn;
            }
            continue;
        }

        if (pending) {
            mans[n_man].kind = map_page_gpx_turn_kind(pending_turn);
            mans[n_man].along_m = pending_along;
            mans[n_man].turn_deg = pending_turn;
            n_man++;
            if (n_man + 1u >= cap) {
                pending = false;
                break;
            }
        }

        pending = true;
        pending_along = along;
        pending_turn = turn;
    }

    if (pending && n_man + 1u < cap) {
        mans[n_man].kind = map_page_gpx_turn_kind(pending_turn);
        mans[n_man].along_m = pending_along;
        mans[n_man].turn_deg = pending_turn;
        n_man++;
    }

    mans[n_man].kind = VMAP_MANEUVER_ARRIVE;
    mans[n_man].along_m = total_m;
    n_man++;
    return n_man;
}

static bool map_page_gpx_prepare_rejoin(map_page_t * page, double along_m)
{
    const vmap_route_pt_t * pts;
    uint32_t n;
    uint32_t i0;
    uint32_t cn;
    double target_m;
    double total_m;
    vmap_route_pt_t target;
    uint32_t suffix_n;

    if (!page) {
        return false;
    }

    if (page->nav_gpx_pts && page->nav_gpx_n >= 2u) {
        pts = page->nav_gpx_pts;
        n = page->nav_gpx_n;
        total_m = page->nav_gpx_total_m;
        target_m = along_m;
    } else if (page->route) {
        pts = vmap_route_points(page->route);
        n = vmap_route_point_count(page->route);
        total_m = vmap_route_total_length_m(page->route);
        target_m = along_m;
    } else {
        return false;
    }

    if (!pts || n < 2u) {
        return false;
    }

    if (target_m < 0.0) {
        target_m = 0.0;
    }
    if (target_m > total_m) {
        target_m = total_m;
    }
    if (total_m - target_m < 20.0) {
        return false;
    }
    target.ele_m = (int16_t)VGRF_ELE_UNKNOWN;
    if (!map_page_gpx_point_at(pts, n, target_m, &target.lon, &target.lat)) {
        return false;
    }
    if (!map_page_gpx_slice(pts, n, target_m, VMAP_NAV_GPX_WINDOW_M,
            &i0, &cn, NULL, NULL) || cn < 1u) {
        return false;
    }

    vmap_free(page->nav_gpx_suffix);
    page->nav_gpx_suffix = NULL;
    page->nav_gpx_suffix_count = 0u;

    if (vmap_geo_haversine_m(target.lon, target.lat, pts[i0].lon, pts[i0].lat)
        < MAP_PAGE_GPX_MIN_KEEP_M) {
        suffix_n = cn;
        page->nav_gpx_suffix = (vmap_route_pt_t *)vmap_malloc(
            (size_t)suffix_n * sizeof(*page->nav_gpx_suffix));
        if (!page->nav_gpx_suffix) {
            return false;
        }
        memcpy(page->nav_gpx_suffix, &pts[i0],
            (size_t)suffix_n * sizeof(*page->nav_gpx_suffix));
    } else {
        suffix_n = 1u + cn;
        page->nav_gpx_suffix = (vmap_route_pt_t *)vmap_malloc(
            (size_t)suffix_n * sizeof(*page->nav_gpx_suffix));
        if (!page->nav_gpx_suffix) {
            return false;
        }
        page->nav_gpx_suffix[0] = target;
        memcpy(&page->nav_gpx_suffix[1], &pts[i0],
            (size_t)cn * sizeof(*page->nav_gpx_suffix));
    }
    page->nav_gpx_suffix_count = suffix_n;
    return suffix_n > 0u;
}

static bool map_page_gpx_merge_rejoin(const vmap_route_pt_t * planned,
    uint32_t planned_n, const vmap_route_pt_t * suffix, uint32_t suffix_n,
    vmap_route_pt_t * merged, uint32_t * merged_n)
{
    uint32_t out_n = 0u;
    uint32_t suffix_i = 0u;

    if (!planned || planned_n < 2u || !suffix || suffix_n == 0u || !merged
        || !merged_n || planned_n > VMAP_ROUTE_MAX_PTS) {
        return false;
    }

    memcpy(merged, planned, (size_t)planned_n * sizeof(*merged));
    out_n = planned_n;
    if (vmap_geo_haversine_m(merged[out_n - 1u].lon, merged[out_n - 1u].lat,
            suffix[0].lon, suffix[0].lat) < VMAP_ROUTE_STITCH_DEDUP_M) {
        suffix_i = 1u;
    }

    if (suffix_n - suffix_i > VMAP_ROUTE_MAX_PTS - out_n) {
        const uint32_t available = VMAP_ROUTE_MAX_PTS - out_n;
        const uint32_t remain = suffix_n - suffix_i;
        uint32_t stride;
        uint32_t i;

        if (available < 2u) {
            return false;
        }
        stride = (remain - 1u + available - 2u) / (available - 1u);
        if (stride < 1u) {
            stride = 1u;
        }
        for (i = suffix_i; i + 1u < suffix_n
            && out_n + 1u < VMAP_ROUTE_MAX_PTS; i += stride) {
            merged[out_n++] = suffix[i];
        }
        merged[out_n++] = suffix[suffix_n - 1u];
    } else {
        memcpy(&merged[out_n], &suffix[suffix_i],
            (size_t)(suffix_n - suffix_i) * sizeof(*merged));
        out_n += suffix_n - suffix_i;
    }

    *merged_n = out_n;
    return out_n >= 2u;
}

static void map_page_nav_gpx_rejoin_done(void * user, bool ok,
    vmap_route_job_kind_t kind, uint32_t regions_planned,
    const vmap_route_pt_t * pts, uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count)
{
    map_page_t * page = (map_page_t *)user;
    vmap_route_pt_t * merged = NULL;
    vmap_route_maneuver_t mans[VMAP_ROUTE_MAX_MANEUVERS];
    uint32_t merged_n = 0u;
    uint32_t n_man;
    double merged_m = 0.0;
    bicycle_gnss_fix_t fix;

    (void)kind;
    (void)regions_planned;
    (void)total_m;
    (void)maneuvers;
    (void)maneuver_count;

    if (!page || !page->nav_gpx_rejoining) {
        return;
    }

    page->nav_planning = false;
    page->nav_replanning = false;
    map_page_plan_hint_hide(page);
    if (!ok || !pts || pt_count < 2u || !page->nav_gpx_suffix
        || page->nav_gpx_suffix_count == 0u) {
        lv_pm_notify_show("导航", "回归轨迹规划失败", 4000);
        myvendor_sound_warn();
        vmap_free(page->nav_gpx_suffix);
        page->nav_gpx_suffix = NULL;
        page->nav_gpx_suffix_count = 0u;
        page->nav_gpx_rejoining = false;
        return;
    }

    merged = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(*merged));
    if (!merged || !map_page_gpx_merge_rejoin(pts, pt_count,
            page->nav_gpx_suffix, page->nav_gpx_suffix_count, merged,
            &merged_n)) {
        vmap_free(merged);
        lv_pm_notify_show("导航", "回归轨迹内存不足", 4000);
        myvendor_sound_warn();
        vmap_free(page->nav_gpx_suffix);
        page->nav_gpx_suffix = NULL;
        page->nav_gpx_suffix_count = 0u;
        page->nav_gpx_rejoining = false;
        return;
    }

    for (uint32_t i = 1u; i < merged_n; i++) {
        merged_m += vmap_geo_haversine_m(merged[i - 1u].lon,
            merged[i - 1u].lat, merged[i].lon, merged[i].lat);
    }
    n_man = map_page_gpx_build_maneuvers(merged, merged_n, merged_m, mans,
        VMAP_ROUTE_MAX_MANEUVERS);
    vmap_route_nav_apply(page->nav, merged, merged_n, merged_m, mans, n_man);
    vmap_free(merged);

    vmap_free(page->nav_gpx_suffix);
    page->nav_gpx_suffix = NULL;
    page->nav_gpx_suffix_count = 0u;
    page->nav_gpx_rejoining = false;
    page->nav_gpx_off_since_ms = 0u;
    page->nav_gpx_off_warned = false;
    page->nav_gpx_course = true;
    page->nav_planned = vmap_route_is_active(page->route);
    page->nav_maneuver_announced = UINT32_MAX;

    if (bicycle_runtime_poll_fix(&fix) && fix.valid) {
        vmap_route_nav_update(page->nav, fix.longitude, fix.latitude,
            fix.course_deg, fix.speed_kph);
        map_page_nav_refresh_map(page, fix.longitude, fix.latitude);
    }
    lv_pm_notify_show("导航", "已规划路线返回 GPX", 4000);
    myvendor_sound_ok();
    VMAP_NAV_LOG("GPX rejoin ready pts=%u man=%u len=%.0fm",
        (unsigned)merged_n, (unsigned)n_man, merged_m);
}

static bool map_page_nav_gpx_rejoin_begin(map_page_t * page,
    const vmap_route_nav_state_t * st, double lon, double lat)
{
    bool corridor;
    double target_lon;
    double target_lat;

    if (!page || !page->route || !page->nav || !st || !st->active
        || page->nav_gpx_rejoining || page->nav_planning
        || myvendor_mtp_lfs_quiesce()) {
        return false;
    }

    {
        double join_m = page->nav_gpx_win_start_m + st->along_m;
        double dist = 1e300;

        if (page->nav_gpx_pts && page->nav_gpx_n >= 2u) {
            join_m = map_page_gpx_nearest_along(page->nav_gpx_pts,
                page->nav_gpx_n, lon, lat, &dist);
            (void)dist;
        }
        vmap_route_worker_init();
        if (vmap_route_worker_busy()
            || !map_page_gpx_prepare_rejoin(page, join_m)) {
            return false;
        }
    }

    target_lon = page->nav_gpx_suffix[0].lon;
    target_lat = page->nav_gpx_suffix[0].lat;
    corridor = vmap_route_corridor_needed(VMAP_TILE_DIR, lon, lat,
        target_lon, target_lat);
    if (!corridor && !map_page_ensure_route_graph(page)) {
        vmap_free(page->nav_gpx_suffix);
        page->nav_gpx_suffix = NULL;
        page->nav_gpx_suffix_count = 0u;
        return false;
    }

    page->nav_gpx_rejoining = true;
    page->nav_planning = true;
    page->nav_replanning = true;
    page->nav_plan_from_lon = lon;
    page->nav_plan_from_lat = lat;
    page->nav_plan_to_lon = target_lon;
    page->nav_plan_to_lat = target_lat;
    page->nav_maneuver_announced = UINT32_MAX;
    map_page_plan_hint_show(page, "规划路线返回 GPX");

    if (!vmap_route_worker_submit(VMAP_TILE_DIR,
            corridor ? NULL : s_route_graph, lon, lat, target_lon, target_lat,
            map_page_nav_gpx_rejoin_done, page)) {
        page->nav_gpx_rejoining = false;
        page->nav_planning = false;
        page->nav_replanning = false;
        map_page_plan_hint_hide(page);
        vmap_free(page->nav_gpx_suffix);
        page->nav_gpx_suffix = NULL;
        page->nav_gpx_suffix_count = 0u;
        return false;
    }

    VMAP_NAV_OUT("GPX 偏航回归 lon=%.7f lat=%.7f -> lon=%.7f lat=%.7f",
        lon, lat, target_lon, target_lat);
    return true;
}

static bool map_page_nav_gpx_follow(map_page_t * page, const vmap_route_pt_t * pts,
    uint32_t n, double total_m, bool first)
{
    vmap_route_maneuver_t mans[VMAP_ROUTE_MAX_MANEUVERS];
    uint32_t n_man;
    bicycle_gnss_fix_t fix;
    char msg[80];
    double cx;
    double cy;
    bool mtp_was_held;

    if (!page || !pts || n < 2u) {
        return false;
    }
    mtp_was_held = page->nav_mtp_held;
    if (first) {
        if (!map_page_nav_mtp_acquire(page)) {
            return false;
        }
        if (!map_page_ensure_route_nav_ex(page, false)) {
            if (!mtp_was_held) {
                map_page_nav_mtp_release(page);
            }
            return false;
        }
        vmap_route_worker_init();
        if (vmap_route_worker_busy()) {
            if (!mtp_was_held) {
                map_page_nav_mtp_release(page);
            }
            return false;
        }
        vmap_route_worker_cancel();
        map_page_gpx_rejoin_reset(page);
        if (page->nav) {
            vmap_route_nav_stop(page->nav);
            vmap_route_nav_set_off_route_threshold(page->nav,
                map_page_gpx_off_threshold(page));
        }
        vmap_route_trip_reset(&page->nav_trip);
        map_page_trip_ahead_clear(page);
        page->nav_trip_active = false;
        page->nav_top_only = false;
        page->nav_planning = false;
        page->nav_replanning = false;
        page->nav_refine_extend = false;
        page->nav_arrive_hold = false;
        page->nav_last_reroute_ms = 0u;
        page->nav_last_reroute_lon = 0.0;
        page->nav_last_reroute_lat = 0.0;
        page->nav_off_since_ms = 0u;
    } else if (!page->nav || !page->route || vmap_route_worker_busy()
        || page->nav_gpx_rejoining) {
        return false;
    }

    page->nav_gpx_course = true;
    page->nav_gpx_dense = true;
    page->nav_gpx_sparse = false;
    page->nav_user_set = true;

    n_man = map_page_gpx_build_maneuvers(pts, n, total_m, mans,
        VMAP_ROUTE_MAX_MANEUVERS);
    vmap_route_nav_apply(page->nav, pts, n, total_m, mans, n_man);
    if (!vmap_route_is_active(page->route)) {
        page->nav_gpx_course = false;
        if (first && !mtp_was_held) {
            map_page_nav_mtp_release(page);
        }
        return false;
    }

    page->nav_planned = true;
    page->nav_maneuver_announced = UINT32_MAX;
    page->nav_plan_from_lon = pts[0].lon;
    page->nav_plan_from_lat = pts[0].lat;
    page->nav_plan_to_lon = pts[n - 1u].lon;
    page->nav_plan_to_lat = pts[n - 1u].lat;
    cx = pts[0].lon;
    cy = pts[0].lat;
    if (bicycle_runtime_poll_fix(&fix) && fix.valid) {
        cx = fix.longitude;
        cy = fix.latitude;
        vmap_route_nav_update(page->nav, fix.longitude, fix.latitude,
            fix.course_deg, fix.speed_kph);
    } else {
        vmap_route_nav_update(page->nav, pts[0].lon, pts[0].lat, 0.0f, 0.0f);
    }

    map_page_nav_refresh_map(page, cx, cy);
    if (first) {
        snprintf(msg, sizeof(msg), "GPX %.1f / %.1f km",
            total_m / 1000.0,
            (page->nav_gpx_true_m > page->nav_gpx_total_m
                ? page->nav_gpx_true_m : page->nav_gpx_total_m) / 1000.0);
        lv_pm_notify_show("导航", msg, 4000);
        helm_shell_on_nav_started();
    }
    return true;
}

static bool map_page_gpx_load_window(map_page_t * page, double from_m, bool first)
{
    uint32_t i0;
    uint32_t cn;
    double start_m;
    double len_m;

    if (!page || !page->nav_gpx_pts || page->nav_gpx_n < 2u) {
        return false;
    }
    if (from_m < 0.0) {
        from_m = 0.0;
    }
    if (!map_page_gpx_slice(page->nav_gpx_pts, page->nav_gpx_n, from_m,
            VMAP_NAV_GPX_WINDOW_M, &i0, &cn, &start_m, &len_m)) {
        return false;
    }
    if (!first
        && fabs(start_m - page->nav_gpx_win_start_m) < 30.0
        && fabs(start_m + len_m - page->nav_gpx_win_end_m) < 30.0) {
        return false;
    }
    if (!map_page_nav_gpx_follow(page, &page->nav_gpx_pts[i0], cn, len_m, first)) {
        return false;
    }
    page->nav_gpx_win_start_m = start_m;
    page->nav_gpx_win_end_m = start_m + len_m;
    VMAP_NAV_LOG("gpx window%s start=%.0f end=%.0f / %.0f pts=%u",
        first ? "" : " reload", start_m, page->nav_gpx_win_end_m,
        page->nav_gpx_total_m, (unsigned)cn);
    return true;
}

static bool map_page_gpx_plan_window(map_page_t * page, double from_lon,
    double from_lat, double from_along_m, bool reroute)
{
    double dest_along;
    double to_lon;
    double to_lat;

    if (!page || !page->nav_gpx_pts || page->nav_gpx_n < 2u) {
        return false;
    }
    if (from_along_m < 0.0) {
        from_along_m = 0.0;
    }
    dest_along = from_along_m + VMAP_NAV_GPX_WINDOW_M;
    if (dest_along > page->nav_gpx_total_m) {
        dest_along = page->nav_gpx_total_m;
    }
    if (dest_along - from_along_m < 20.0) {
        return false;
    }
    if (fabs(dest_along - page->nav_gpx_win_end_m) < 50.0 && reroute) {
        return false;
    }
    if (!map_page_gpx_point_at(page->nav_gpx_pts, page->nav_gpx_n, dest_along,
            &to_lon, &to_lat)) {
        return false;
    }
    page->nav_gpx_win_start_m = from_along_m;
    page->nav_gpx_win_end_m = dest_along;
    page->nav_gpx_dense = false;
    page->nav_gpx_sparse = true;
    if (!map_page_nav_plan_begin(page, from_lon, from_lat, to_lon, to_lat, true,
            reroute)) {
        return false;
    }
    page->nav_gpx_sparse = true;
    VMAP_NAV_LOG("gpx sparse%s along=%.0f -> %.0f / %.0f",
        reroute ? " next" : "", from_along_m, dest_along, page->nav_gpx_total_m);
    return true;
}

static bool map_page_gpx_tick_window(map_page_t * page,
    const vmap_route_nav_state_t * st, double lon, double lat)
{
    double along;
    double dist = 0.0;
    bool arrived;
    bool need;

    if (!page || !st || !st->active || page->nav_planning
        || page->nav_gpx_rejoining || page->nav_review
        || !page->nav_gpx_pts || page->nav_gpx_n < 2u) {
        return false;
    }
    if (st->off_route_m > map_page_gpx_window_max_off_m(page)) {
        return false;
    }
    if (page->nav_gpx_win_end_m + 20.0 >= page->nav_gpx_total_m
        && st->remain_m > VMAP_ROUTE_ARRIVE_M) {
        return false;
    }

    arrived = vmap_route_nav_is_arrived(st);
    need = arrived || st->remain_m <= VMAP_NAV_GPX_RELOAD_M;
    if (!need) {
        return false;
    }
    if (page->nav_gpx_win_end_m + 20.0 >= page->nav_gpx_total_m && arrived) {
        return false;
    }

    along = map_page_gpx_nearest_along(page->nav_gpx_pts, page->nav_gpx_n,
        lon, lat, &dist);
    if (dist > VMAP_NAV_GPX_JOIN_MAX_M) {
        return false;
    }
    if (page->nav_gpx_dense) {
        double from_m = along;

        if (from_m > VMAP_NAV_GPX_OVERLAP_M) {
            from_m -= VMAP_NAV_GPX_OVERLAP_M;
        } else {
            from_m = 0.0;
        }
        return map_page_gpx_load_window(page, from_m, false);
    }
    if (page->nav_gpx_sparse) {
        return map_page_gpx_plan_window(page, lon, lat, along, true);
    }
    return false;
}

static void map_page_reverse_pts(vmap_route_pt_t * pts, uint32_t n)
{
    uint32_t i;
    vmap_route_pt_t tmp;

    if (pts == NULL || n < 2u) {
        return;
    }

    for (i = 0; i < n / 2u; i++) {
        tmp = pts[i];
        pts[i] = pts[n - 1u - i];
        pts[n - 1u - i] = tmp;
    }
}

/**
 * @brief 打开 GPX：一次只加载前方 N km，接近窗口末端再动态续载。
 */
bool map_page_nav_from_gpx(map_page_t * page, const char * path, bool reverse)
{
    vmap_route_pt_t * pts;
    uint32_t kept = 0;
    uint32_t file_n = 0;
    double len = 0.0;
    double true_len = 0.0;
    double avg_m;
    bicycle_gnss_fix_t fix;
    bool have_fix;
    bool join;
    double from_m;
    bool ok;

    if (!page || !path || path[0] == '\0') {
        return false;
    }

    if (bicycle_gpx_sim_active()) {
        bicycle_gpx_sim_stop();
    }
    map_page_nav_gnss_for_gps(page);
    page->nav_review = false;
    if (page->route) {
        vmap_route_set_review(page->route, false);
    }
    if (page->map) {
        vmap_view_set_arrow_visible(page->map, true);
    }

    {
        const char * base = path;
        const char * slash = strrchr(path, '/');
        char * dot;

        if (slash && slash[1] != '\0') {
            base = slash + 1;
        }
        lv_snprintf(page->nav_title, sizeof(page->nav_title), "%s", base);
        dot = strrchr(page->nav_title, '.');
        if (dot) {
            *dot = '\0';
        }
    }

    pts = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(*pts));
    if (!pts) {
        return false;
    }

    if (!map_page_gpx_load(path, pts, VMAP_ROUTE_MAX_PTS, &kept, &file_n, &len,
            &true_len)
        || kept < 2u) {
        VMAP_NAV_WARN("nav_gpx: no points in %s", path);
        vmap_free(pts);
        return false;
    }

    if (reverse) {
        map_page_reverse_pts(pts, kept);
    }

    map_page_gpx_course_clear(page);
    page->nav_gpx_pts = pts;
    page->nav_gpx_n = kept;
    page->nav_gpx_total_m = len;
    page->nav_gpx_true_m = true_len > len ? true_len : len;
    page->nav_gpx_reverse = reverse;

    have_fix = bicycle_runtime_poll_fix(&fix);
    join = map_page_gpx_start_join(page, have_fix ? &fix : NULL, &from_m);

    avg_m = (file_n > 1u) ? (len / (double)(file_n - 1u)) : len;
    if (avg_m <= MAP_PAGE_GPX_DENSE_AVG_M) {
        VMAP_NAV_LOG("nav_gpx: %s%s dense avg=%.0fm file=%u pts=%u len=%.0fm win=%.0f%s",
            path, reverse ? " rev" : "", avg_m, (unsigned)file_n,
            (unsigned)kept, len, VMAP_NAV_GPX_WINDOW_M,
            join ? " join" : "");
        ok = map_page_gpx_load_window(page, from_m, true);
        if (!ok) {
            map_page_gpx_course_clear(page);
        }
        return ok;
    }

    VMAP_NAV_LOG("nav_gpx: %s%s sparse avg=%.0fm file=%u pts=%u len=%.0fm win=%.0f%s",
        path, reverse ? " rev" : "", avg_m, (unsigned)file_n,
        (unsigned)kept, len, VMAP_NAV_GPX_WINDOW_M,
        join ? " join" : "");
    if (join) {
        ok = map_page_gpx_plan_window(page, (double)fix.longitude,
            (double)fix.latitude, from_m, false);
    } else {
        ok = map_page_gpx_plan_window(page, pts[0].lon, pts[0].lat, 0.0, false);
    }
    if (!ok) {
        map_page_gpx_course_clear(page);
    }
    return ok;
}

static void map_page_gpx_set_title(map_page_t * page, const char * path)
{
    const char * base = path;
    const char * slash;
    char * dot;

    if (!page || !path) {
        return;
    }

    slash = strrchr(path, '/');
    if (slash && slash[1] != '\0') {
        base = slash + 1;
    }
    lv_snprintf(page->nav_title, sizeof(page->nav_title), "%s", base);
    dot = strrchr(page->nav_title, '.');
    if (dot) {
        *dot = '\0';
    }
}

static void map_page_fit_pts(map_page_t * page, const vmap_route_pt_t * pts,
    uint32_t n)
{
    double min_lon;
    double max_lon;
    double min_lat;
    double max_lat;
    double cx;
    double cy;
    uint32_t i;
    int z;
    int32_t w = 0;
    int32_t h = 0;
    const int32_t pad = 20;

    if (!page || !page->map || !pts || n < 2u) {
        return;
    }

    min_lon = max_lon = pts[0].lon;
    min_lat = max_lat = pts[0].lat;
    for (i = 1u; i < n; i++) {
        if (pts[i].lon < min_lon) {
            min_lon = pts[i].lon;
        }
        if (pts[i].lon > max_lon) {
            max_lon = pts[i].lon;
        }
        if (pts[i].lat < min_lat) {
            min_lat = pts[i].lat;
        }
        if (pts[i].lat > max_lat) {
            max_lat = pts[i].lat;
        }
    }

    cx = (min_lon + max_lon) * 0.5;
    cy = (min_lat + max_lat) * 0.5;
    vmap_view_get_buffer_size(page->map, &w, &h);

    for (z = VMAP_ZOOM_MAX; z >= VMAP_ZOOM_MIN; z--) {
        int32_t x0;
        int32_t y0;
        int32_t x1;
        int32_t y1;

        vmap_view_set_view(page->map, cx, cy, z);
        (void)vmap_view_geo_to_viewport_raw(page->map, min_lon, min_lat, &x0, &y0);
        (void)vmap_view_geo_to_viewport_raw(page->map, max_lon, max_lat, &x1, &y1);
        if (x0 > x1) {
            int32_t t = x0;

            x0 = x1;
            x1 = t;
        }
        if (y0 > y1) {
            int32_t t = y0;

            y0 = y1;
            y1 = t;
        }
        if (x0 >= pad && y0 >= pad && x1 < w - pad && y1 < h - pad) {
            return;
        }
    }
}

/**
 * @brief 在地图上显示 GPX 轨迹（不导航、不模拟定位）。
 */
bool map_page_show_gpx(map_page_t * page, const char * path)
{
    vmap_route_pt_t * pts;
    uint32_t kept = 0;
    uint32_t file_n = 0;
    double len = 0.0;
    double true_len = 0.0;
    char msg[80];

    if (!page || !path || path[0] == '\0') {
        return false;
    }

    if (bicycle_gpx_sim_active()) {
        bicycle_gpx_sim_stop();
    }
    map_page_nav_gnss_for_gps(page);
    map_page_gpx_set_title(page, path);
    map_page_gpx_course_clear(page);

    pts = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(*pts));
    if (!pts) {
        return false;
    }

    if (!map_page_gpx_load(path, pts, VMAP_ROUTE_MAX_PTS, &kept, &file_n, &len,
            &true_len)
        || kept < 2u) {
        vmap_free(pts);
        return false;
    }
    (void)file_n;

    if (!map_page_ensure_route_nav_ex(page, false)) {
        vmap_free(pts);
        return false;
    }

    vmap_route_worker_init();
    if (vmap_route_worker_busy()) {
        vmap_free(pts);
        return false;
    }
    map_page_gpx_rejoin_reset(page);
    vmap_route_worker_cancel();
    if (page->nav) {
        vmap_route_nav_stop(page->nav);
    }
    vmap_route_trip_reset(&page->nav_trip);
    map_page_trip_ahead_clear(page);
    page->nav_trip_active = false;
    page->nav_top_only = false;
    page->nav_planning = false;
    page->nav_replanning = false;
    page->nav_refine_extend = false;
    page->nav_arrive_hold = false;
    page->nav_planned = false;
    page->nav_gpx_course = true;
    page->nav_user_set = true;
    page->nav_review = true;

    vmap_route_apply(page->route, pts, kept, len, NULL, 0);
    vmap_route_set_review(page->route, true);
    if (!vmap_route_is_active(page->route)) {
        page->nav_review = false;
        page->nav_gpx_course = false;
        vmap_free(pts);
        return false;
    }

    if (page->route) {
        vmap_view_attach_route(page->map, page->route);
    }
    map_page_fit_pts(page, pts, kept);
    vmap_view_set_arrow_visible(page->map, false);
    if (!page->covered) {
        vmap_view_render(page->map);
        map_page_kick_render(page);
    }
    helm_shell_raise_chrome();
    vmap_free(pts);

    snprintf(msg, sizeof(msg), "轨迹 · %.0f m",
        true_len > len ? true_len : len);
    lv_pm_notify_show("回放", msg, 2500);
    helm_shell_on_nav_started();
    return true;
}

static uint32_t map_page_trip_nearest_leg(const map_page_t * page,
    double lon, double lat)
{
    const uint32_t total = page
        ? vmap_route_trip_waypoint_count(&page->nav_trip) : 0u;
    uint32_t start = page ? vmap_route_trip_leg_index(&page->nav_trip) : 0u;
    uint32_t i;
    uint32_t best = start;
    double best_d = 1e300;
    double wlon;
    double wlat;

    if (!page || total == 0u) {
        return 0u;
    }
    if (start >= total) {
        start = 0u;
        best = 0u;
    }
    for (i = start; i < total; i++) {
        double d;

        if (!vmap_route_trip_waypoint_at(&page->nav_trip, i, &wlon, &wlat)) {
            continue;
        }
        d = vmap_geo_haversine_m(lon, lat, wlon, wlat);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    /* 已夹在最近点与下一点之间则取下一点，避免折回。 */
    if (best + 1u < total
        && vmap_route_trip_waypoint_at(&page->nav_trip, best, &wlon, &wlat)) {
        double nlon;
        double nlat;
        double d_cur;
        double d_next;
        double d_seg;

        if (vmap_route_trip_waypoint_at(&page->nav_trip, best + 1u, &nlon,
                &nlat)) {
            d_cur = vmap_geo_haversine_m(lon, lat, wlon, wlat);
            d_next = vmap_geo_haversine_m(lon, lat, nlon, nlat);
            d_seg = vmap_geo_haversine_m(wlon, wlat, nlon, nlat);
            if (d_seg > 1.0 && d_cur + d_next <= d_seg * 1.25 + 30.0) {
                best = best + 1u;
            }
        }
    }
    return best;
}

static bool map_page_nav_plan_nearest_gpx(map_page_t * page, double lon,
    double lat)
{
    double dist = 1e300;
    double along;
    double from_m;
    char msg[64];

    along = map_page_gpx_nearest_along(page->nav_gpx_pts, page->nav_gpx_n,
        lon, lat, &dist);
    if (page->nav_gpx_total_m - along < 20.0) {
        lv_pm_notify_show("就近规划", "已接近终点", 2000);
        return false;
    }

    from_m = along;
    if (from_m > VMAP_NAV_GPX_OVERLAP_M) {
        from_m -= VMAP_NAV_GPX_OVERLAP_M;
    } else {
        from_m = 0.0;
    }

    page->nav_arrive_hold = false;
    page->nav_gpx_win_start_m = -1e9;
    page->nav_gpx_win_end_m = -1e9;

    if (page->nav_gpx_sparse) {
        if (!map_page_gpx_plan_window(page, lon, lat, along, true)) {
            return false;
        }
        page->nav_gpx_sparse = true;
        lv_pm_notify_show("就近规划", "从最近点向后规划", 2000);
        VMAP_NAV_LOG("nearest gpx sparse along=%.0f dist=%.0f", along, dist);
        return true;
    }

    if (!map_page_gpx_load_window(page, from_m, false)
        && !map_page_gpx_load_window(page, from_m, true)) {
        return false;
    }
    {
        const vmap_route_nav_state_t * st =
            page->nav ? vmap_route_nav_state(page->nav) : NULL;
        const double off = map_page_gpx_off_threshold(page);

        if (st && st->active && dist > off
            && map_page_nav_gpx_rejoin_begin(page, st, lon, lat)) {
            lv_pm_notify_show("就近规划", "正在接回路书", 2000);
            VMAP_NAV_LOG("nearest gpx rejoin along=%.0f dist=%.0f", along, dist);
            return true;
        }
    }
    snprintf(msg, sizeof(msg), "GPX %.1f / %.1f km",
        along / 1000.0,
        (page->nav_gpx_true_m > page->nav_gpx_total_m
            ? page->nav_gpx_true_m : page->nav_gpx_total_m) / 1000.0);
    lv_pm_notify_show("就近规划", msg, 2500);
    VMAP_NAV_LOG("nearest gpx dense along=%.0f dist=%.0f", along, dist);
    return true;
}

static bool map_page_nav_plan_nearest_trip(map_page_t * page, double lon,
    double lat)
{
    const uint32_t total = vmap_route_trip_waypoint_count(&page->nav_trip);
    uint32_t best;
    double tlon;
    double tlat;
    double d;
    char msg[64];

    if (total == 0u) {
        return false;
    }
    best = map_page_trip_nearest_leg(page, lon, lat);
    if (!vmap_route_trip_waypoint_at(&page->nav_trip, best, &tlon, &tlat)) {
        return false;
    }
    d = vmap_geo_haversine_m(lon, lat, tlon, tlat);

    /* 从 best 起还有没有真正没到的站？**纯判，不动状态** —— 先改 leg 再判断的话，
     * 一旦决定不规划，leg 已经被挪走，正在跑的导航就对不上了。
     *
     * 「最近的那个站就在脚下」其实是常态：两站挨得近，或者刚过一站又点了就近
     * 规划。拿一个已经在脚下的站当 target，规划器只会回 "already at dest"，
     * 用户看到的是一次没有任何反馈的失败。后面的站全在脚下才是真的到头了。 */
    if (vmap_route_trip_first_unreached(page->nav_trip.wps, best, total,
            lon, lat) >= total) {
        lv_pm_notify_show("就近规划", "已接近终点", 2000);
        return false;
    }

    map_page_trip_ahead_clear(page);
    if (!vmap_route_trip_set_leg(&page->nav_trip, best)) {
        return false;
    }

    /* 把 leg 从 best 推到第一个真正没到的站。上面刚确认过存在这样的站，
     * 所以这里的返回值一定是 true。 */
    (void)vmap_route_trip_skip_reached(&page->nav_trip, lon, lat);

    if (!vmap_route_trip_begin_leg(VMAP_TILE_DIR, &page->nav_trip, lon, lat)) {
        return false;
    }
    page->nav_arrive_hold = false;
    if (!map_page_nav_rolling_begin(page, lon, lat, false, false)) {
        return false;
    }
    snprintf(msg, sizeof(msg), "途经 %u/%u",
        (unsigned)(vmap_route_trip_leg_index(&page->nav_trip) + 1u),
        (unsigned)total);
    lv_pm_notify_show("就近规划", msg, 2000);
    VMAP_NAV_LOG("nearest trip dest=wp%u/%u dist=%.0f",
        (unsigned)best, (unsigned)total, d);
    return true;
}

/**
 * @brief 就近规划：扫最近的 GPX 点或途经点，以该点为目标向后重新规划。
 */
bool map_page_nav_plan_nearest(map_page_t * page)
{
    bicycle_gnss_fix_t fix;

    if (!page) {
        return false;
    }
    if (page->nav_review) {
        return false;
    }
    if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
        lv_pm_notify_show("就近规划", "无定位", 2000);
        return false;
    }
    if (page->nav_planning || page->nav_gpx_rejoining
        || vmap_route_worker_busy()) {
        lv_pm_notify_show("就近规划", "规划中", 2000);
        return false;
    }

    if (page->nav_gpx_pts && page->nav_gpx_n >= 2u) {
        return map_page_nav_plan_nearest_gpx(page, fix.longitude, fix.latitude);
    }
    if (page->nav_trip_active) {
        return map_page_nav_plan_nearest_trip(page, fix.longitude, fix.latitude);
    }
    if (page->nav_planned) {
        if (!map_page_nav_reroute_begin(page, fix.longitude, fix.latitude)) {
            return false;
        }
        lv_pm_notify_show("就近规划", "从当前位置重算", 2000);
        return true;
    }
    lv_pm_notify_show("就近规划", "导航未启动", 2000);
    return false;
}
#endif /* VMAP_ROUTE_ENABLE */

#if !VMAP_ROUTE_ENABLE
bool map_page_nav_from_gpx(map_page_t * page, const char * path, bool reverse)
{
    (void)page;
    (void)path;
    (void)reverse;
    return false;
}

bool map_page_show_gpx(map_page_t * page, const char * path)
{
    (void)page;
    (void)path;
    return false;
}
#endif

/**
 * @brief 菜单/MTP 盖住：停 render_timer，不关瓦片句柄。
 */
void map_page_pause_for_cover(map_page_t * page)
{
    if (!page) {
        return;
    }

    page->covered = true;
    helm_shell_pause_for_cover();
    if (page->render_timer) {
        lv_timer_pause(page->render_timer);
    }
    /* 菜单盖住时不要关 .vpk：半帧 rebase 还在，关句柄再解算会画错当前瓦片。
     * MTP 另走 map_page_release_storage()。 */
    lv_port_buttons_clear_page_callbacks();
}

/**
 * @brief cover pop 后恢复渲染；已有画布只 invalidate。
 */
void map_page_resume_after_cover(map_page_t * page)
{
    uint32_t rev = 0;

    if (!page) {
        return;
    }

    rev = page->map ? vmap_view_render_revision(page->map) : 0u;

    page->covered = false;
    if (page->map && page->map_ui_visible) {
        lv_obj_t * canvas = vmap_view_get_obj(page->map);

        /* 保留大画布；下一 GNSS 拍按位移阈值 catchup，不因关闭 cover 重载。 */
        page->map_quiet_until = lv_tick_get();
        if (page->map_quiet_until == 0u) {
            page->map_quiet_until = 1u;
        }
        if (rev == 0 || page->map_refresh_pending) {
            map_page_ensure_boot_paint(page);
        } else if (canvas) {
            lv_obj_invalidate(canvas);
            vmap_view_bring_arrow_to_front(page->map);
        }
    }
    if (page->map && page->render_timer && vmap_view_render_busy(page->map)) {
        lv_timer_resume(page->render_timer);
    }
    helm_shell_resume_after_cover();
}

/**
 * @brief MTP 结束后重载瓦片并踢一帧。
 */
void map_page_reload_storage(map_page_t * page)
{
    if (!page || !page->map) {
        return;
    }

    vmap_view_reload_storage(page->map);
    map_page_kick_render(page);
}

/**
 * @brief MTP 静默：关 .vpk 句柄，保留画布与 RAM 缓存。
 */
void map_page_release_storage(map_page_t * page)
{
    if (!page || !page->map) {
        return;
    }

    vmap_view_release_storage(page->map);
}

/**
 * @brief Startup 进度页内准备地图文件，不解码或绘制瓦片。
 *
 * @details
 * 创建地图资源后设置启动中心，并打开该坐标对应的 region pack。Canvas 在
 * vmap_view_create() 中已经清零且保持 HIDDEN；首帧留给 LiveMap 出现完成回调。
 *
 * @param page LiveMap 实例；NULL 时先尝试完成 boot_begin_load。
 * @return 文件准备流程已经结束为 true；资源尚不可用时返回 false。
 */
bool map_page_boot_prepare(map_page_t * page)
{
    double boot_lon;
    double boot_lat;
    bool region_ok;

    if (!page) {
        if (!map_page_boot_begin_load()) {
            return false;
        }
        page = live_map_page_instance();
    }

    if (!page || !page->map) {
        return false;
    }

    if (s_boot_files_ready) {
        return true;
    }

    if (myvendor_mtp_lfs_quiesce()) {
        return false;
    }

    if (!s_boot_center_ready) {
        myvendor_watchdog_ui_beat();
        myvendor_watchdog_busy_pump();
        vmap_boot_lonlat(&boot_lon, &boot_lat);
        if (!vmap_boot_ll_ok(boot_lon, boot_lat)) {
            boot_lon = VMAP_DEFAULT_LON;
            boot_lat = VMAP_DEFAULT_LAT;
        }
        vmap_view_set_view(page->map, boot_lon, boot_lat, VMAP_DEFAULT_ZOOM);
        s_boot_center_ready = true;
        map_page_sync_zoom_state(page);
    }

    vmap_view_get_center(page->map, &boot_lon, &boot_lat);
    myvendor_watchdog_ui_beat();
    myvendor_watchdog_busy_pump();
    region_ok = vmap_view_ensure_region(page->map, boot_lon, boot_lat);
    myvendor_watchdog_ui_beat();
    if (!region_ok) {
        LV_LOG_WARN("map boot: region file unavailable, defer to first render");
    }

    /* 文件缺失不是可重试的启动阻塞条件；首帧会显示稳定背景并在实际进入
     * LiveMap 后按正常路径再次解析。 */
    s_boot_files_ready = true;
    LV_LOG_INFO("map boot: files prepared (render deferred)");
    return true;
}

/**
 * @brief 销毁地图页（关 GPX、卸瓦片、删 LVGL 对象）。
 */
void map_page_destroy(map_page_t * page)
{
    if (!page) {
        return;
    }

    (void)bicycle_ride_gpx_discard();
    if (page->gpx_timer) {
        lv_timer_delete(page->gpx_timer);
    }
    if (page->render_timer) {
        lv_timer_delete(page->render_timer);
    }
    bicycle_route_sim_stop();
    bicycle_gpx_sim_stop();
#if VMAP_ROUTE_ENABLE
    map_page_plan_hint_hide(page);
    map_page_nav_mtp_release(page);
    map_page_gpx_rejoin_reset(page);
    map_page_gpx_course_clear(page);
    map_page_trip_ahead_clear(page);
    if (page->nav) {
        vmap_route_nav_destroy(page->nav);
    }
    if (page->route) {
        vmap_view_attach_route(page->map, NULL);
        vmap_route_destroy(page->route);
    }
#endif
#if VMAP_TRACK_ENABLE
    if (page->track) {
        vmap_track_destroy(page->track);
    }
#endif
    if (page->map) {
        vmap_view_destroy(page->map);
    }
#if VMAP_TRACK_ENABLE
    if (page->lap_lbl) {
        lv_obj_delete(page->lap_lbl);
        page->lap_lbl = NULL;
    }
    if (page->rec_lbl) {
        lv_obj_delete(page->rec_lbl);
        page->rec_lbl = NULL;
    }
#endif
    if (page->root) {
        lv_obj_delete(page->root);
    }
    free(page);
}

/**
 * @brief 调试：把**真导航正在用**的那条路线折线打出来（`ctl nav dump`）。
 *
 * 用户报"轨迹线绘制有缺失"—— 先分清是**规划路线**点数太少/有断口，还是**已骑轨迹**
 * 的问题：这个 dump 给的是路线折线（`page->route`），可以直接和主机侧的道路几何比。
 *
 * @return 实际打出的点数。
 */
uint32_t map_page_debug_route_dump(void)
{
    const vmap_route_pt_t * pts;
    uint32_t n;
    uint32_t i;

    if (!s_live_map || !s_live_map->route) {
        printf("nav: no live route\n");
        return 0;
    }

    n = vmap_route_point_count(s_live_map->route);
    pts = vmap_route_points(s_live_map->route);
    printf("nav: ui route pts=%lu active=%d\n", (unsigned long)n,
        vmap_route_is_active(s_live_map->route) ? 1 : 0);
    for (i = 0; i < n && i < 64u; i++) {
        printf("nav pt ui %u %.7f %.7f\n", (unsigned)i, pts[i].lon, pts[i].lat);
    }
    return n;
}

/**
 * @brief 把当前画布 / 轨迹打到调试缓冲。
 */
void map_page_debug_snap(map_page_t * page)
{
    if (!page) {
        return;
    }

#if VMAP_TRACK_ENABLE
    bicycle_c_debug_snap(page->map, page->track);
#else
    bicycle_c_debug_snap(page->map, NULL);
#endif
}

/* ---------- lv_pm registration (AED-style page_create_*) ---------- */

/**
 * @brief 延迟 `map_page_create` 里的 vmap，先只进空壳。
 */
void map_page_set_boot_deferred(bool deferred)
{
    s_boot_deferred = deferred;
    if (deferred) {
        s_boot_center_ready = false;
        s_boot_files_ready = false;
    }
}

/**
 * @brief 仍处于 boot 空壳、尚未 begin_load 收尾。
 */
bool map_page_boot_deferred(void)
{
    return s_boot_deferred;
}

/**
 * @brief 在空壳 LiveMap 上创建 vmap；创建后 covered，首帧不在 Startup 绘制。
 */
bool map_page_boot_begin_load(void)
{
    lv_pm_page_t pm = s_live_map_page;

    if (!pm || !pm->page) {
        return false;
    }

    if (pm->user_data != NULL) {
        return true;
    }

    myvendor_watchdog_ui_beat();
    myvendor_watchdog_busy_pump();
    s_live_map = map_page_create(pm->page);
    if (!s_live_map) {
        LV_LOG_ERROR("map boot load: create failed");
        return false;
    }

    pm->user_data = s_live_map;
    /* splash 仍盖着：所有 LiveMap 定时器保持暂停，只准备文件资源。 */
    s_live_map->covered = true;

    s_boot_deferred = false;
    s_boot_files_ready = false;
    LV_LOG_INFO("map boot: resources created");
    return true;
}

static void map_will_disappear(void * pm_page)
{
    map_page_t * map = live_map_page_instance();

    (void)pm_page;

    if (map) {
        map_page_pause_for_cover(map);
    }
}

static void map_did_disappear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    /* Fade leaves opa=0 but not HIDDEN; child invalidate still dirties the LCD. */
    if (page && page->page) {
        lv_obj_add_flag(page->page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void map_will_appear(void * pm_page)
{
    map_page_t * map = live_map_page_instance();
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    if (page && page->page) {
        lv_obj_clear_flag(page->page, LV_OBJ_FLAG_HIDDEN);
    }

    /* Boot used apply_silent(); restore fade so menu/MTP covers animate. */
    if (!s_boot_deferred && page != NULL) {
        bicycle_page_anima_apply(page, BICYCLE_PM_ID_MAP);
    }

    if (map) {
        helm_shell_bind_keys();
    }
}

/**
 * @brief LiveMap 淡入完成后恢复定时器并启动首帧渲染。
 *
 * @details
 * 文件资源已在 Startup 阶段准备；把同步 region I/O 和瓦片 pump 排除在
 * 220 ms 淡入之外。Canvas 继续保持 HIDDEN，直到 render_finish() 完整收帧。
 */
static void map_did_appear(void * pm_page)
{
    map_page_t * map = live_map_page_instance();

    (void)pm_page;

    if (!map) {
        return;
    }

    if (map->gpx_timer) {
        lv_timer_resume(map->gpx_timer);
    }
    map_page_resume_after_cover(map);
}

static void map_page_on_load(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    if (page == NULL || page->page == NULL) {
        return;
    }

    if (page->user_data != NULL) {
        s_live_map = (map_page_t *)page->user_data;
        return;
    }

    if (s_boot_deferred) {
        lv_obj_set_style_bg_color(page->page, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(page->page, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(page->page, 0, 0);
        s_live_map = NULL;
        return;
    }

    s_live_map = map_page_create(page->page);
    page->user_data = s_live_map;
}

static void map_page_on_unload(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    if (page == NULL) {
        return;
    }

    if (page->flag.cache_enable) {
        return;
    }

    if (page->user_data != NULL) {
        map_page_destroy((map_page_t *)page->user_data);
        page->user_data = NULL;
        s_live_map = NULL;
    }
}

/**
 * @brief 向 lv_pm 注册 LiveMap。
 */
void live_map_page_register(void)
{
    lv_pm_page_t page = lv_pm_create_page((lv_pm_id)BICYCLE_PM_ID_MAP, "LiveMap");

    if (page == NULL) {
        LV_LOG_ERROR("live_map: register failed");
        return;
    }

    s_live_map_page = page;
    lv_pm_set_open(page, map_page_on_load);
    lv_pm_set_will_appear(page, map_will_appear);
    lv_pm_set_dis_appear(page, map_did_appear);
    lv_pm_set_will_disappear(page, map_will_disappear);
    lv_pm_set_dis_disappear(page, map_did_disappear);
    lv_pm_set_close(page, map_page_on_unload);
    lv_pm_set_theme_changed(page, map_page_on_pm_theme, NULL);
    bicycle_page_anima_apply(page, BICYCLE_PM_ID_MAP);
}

/**
 * @brief 当前 LiveMap 实例。
 */
map_page_t * live_map_page_instance(void)
{
    return s_live_map;
}

/**
 * @brief 已注册的 LiveMap 页描述符。
 */
lv_pm_page_t live_map_page_def(void)
{
    return s_live_map_page;
}
