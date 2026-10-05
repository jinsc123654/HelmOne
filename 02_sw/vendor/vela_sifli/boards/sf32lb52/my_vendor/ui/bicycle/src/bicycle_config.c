/**
 * @file bicycle_config.c
 * @brief 自行车 UI — config。
 */

#include "bicycle_config.h"

#include "helm_palette.h"
#include "lv_pm_theme.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_style.h"
#include <string.h>

static bicycle_config_t g_cfg;
static bool g_cfg_ready;

/**
 * @brief 自行车 config clamp track limits。
 */
static void bicycle_config_clamp_track_limits(void)
{
    if (g_cfg.max_session_km == 0
            || g_cfg.max_session_km > BICYCLE_MAX_SESSION_KM) {
        g_cfg.max_session_km = BICYCLE_MAX_SESSION_KM;
    }
    if (g_cfg.km_marker_max == 0
            || g_cfg.km_marker_max > g_cfg.max_session_km) {
        g_cfg.km_marker_max = g_cfg.max_session_km;
    }
    if (g_cfg.track_draw_cap == 0
            || g_cfg.track_draw_cap > BICYCLE_TRACK_DRAW_CAP) {
        g_cfg.track_draw_cap = (uint16_t)BICYCLE_TRACK_DRAW_CAP;
    }
    if (g_cfg.track_point_cap == 0
            || g_cfg.track_point_cap > BICYCLE_TRACK_POINT_CAP) {
        g_cfg.track_point_cap = BICYCLE_TRACK_POINT_CAP;
    }
    if (g_cfg.track_point_min_dist_m < 1.0) {
        g_cfg.track_point_min_dist_m = BICYCLE_TRACK_POINT_MIN_DIST_M;
    }
}

/**
 * @brief 自行车 config reset defaults。
 */
void bicycle_config_reset_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.lap.min_distance_m = VMAP_LAP_MIN_DISTANCE_M;
    g_cfg.lap.close_radius_m = VMAP_LAP_CLOSE_RADIUS_M;
    g_cfg.lap.max_course_diff_deg = VMAP_LAP_MAX_COURSE_DIFF_DEG;
    g_cfg.km_marker_interval_m = 1000.0;
    g_cfg.max_session_km = BICYCLE_MAX_SESSION_KM;
    g_cfg.km_marker_max = BICYCLE_KM_MARKER_MAX;
    g_cfg.track_point_cap = BICYCLE_TRACK_POINT_CAP;
    g_cfg.track_draw_cap = (uint16_t)BICYCLE_TRACK_DRAW_CAP;
    g_cfg.track_point_min_dist_m = BICYCLE_TRACK_POINT_MIN_DIST_M;
    g_cfg.nav_off_route_m = 50.0;
    g_cfg.record_on_start = false;
    g_cfg.gnss_update_ms = VMAP_GNSS_UPDATE_MS;
    g_cfg.gnss_sim_speed_kph = VMAP_GNSS_SIM_SPEED_KPH;
    g_cfg.nav_sim_speed_kph = VMAP_NAV_SIM_SPEED_KPH;
    g_cfg.gpx_path = VMAP_GPX_PATH;
    g_cfg.map_style = VMAP_STYLE_CLASSIC;
    g_cfg_ready = true;
}

/**
 * @brief 自行车 config ensure init。
 * @return 0 成功，负 errno 失败。
 */
static void bicycle_config_ensure_init(void)
{
    if (!g_cfg_ready) {
        bicycle_config_reset_defaults();
    }
}

/**
 * @brief 自行车 config。
 */
const bicycle_config_t * bicycle_config(void)
{
    bicycle_config_ensure_init();
    return &g_cfg;
}

/**
 * @brief 自行车 config get。
 * @return 请求的值。
 */
void bicycle_config_get(bicycle_config_t * out)
{
    if (!out) {
        return;
    }
    bicycle_config_ensure_init();
    *out = g_cfg;
}

/**
 * @brief 自行车 config set。
 */
void bicycle_config_set(const bicycle_config_t * cfg)
{
    if (!cfg) {
        return;
    }

    bicycle_config_ensure_init();
    g_cfg = *cfg;
    if (g_cfg.km_marker_interval_m <= 0.0) {
        g_cfg.km_marker_interval_m = 1000.0;
    }
    if (g_cfg.gnss_update_ms == 0) {
        g_cfg.gnss_update_ms = VMAP_GNSS_UPDATE_MS;
    }
    if (g_cfg.gpx_path == NULL) {
        g_cfg.gpx_path = VMAP_GPX_PATH;
    }
    if (g_cfg.nav_off_route_m < 10.0) {
        g_cfg.nav_off_route_m = 50.0;
    }
    bicycle_config_clamp_track_limits();
}

/**
 * @brief 自行车 config apply。
 */
void bicycle_config_apply(void)
{
    bicycle_config_ensure_init();

    /* 主题名已经落盘（`/mnt/kv/ui_theme`）⇒ **让它说了算**，这里不再覆盖。
     * 原有的 map_style 只有 classic/outdoor 两支，而"夜间"是主题才表达得出的取值：
     * 开机再拿它去 set 一次，用户选的夜间重启后就没了（实测：选 outdoor 重启变
     * classic，而且回调里会把 KV 改写成 classic）。没有 KV（首次开机/被清）时
     * 仍按原样用地图样式兜底。 */
    if (helm_theme_kv_present()) {
        return;
    }

    if (g_cfg.map_style == VMAP_STYLE_OUTDOOR) {
        lv_pm_theme_set_by_name("outdoor");
    } else {
        lv_pm_theme_set_by_name("classic");
    }
}

/**
 * @brief 自行车 config max session km。
 */
uint16_t bicycle_config_max_session_km(void)
{
    bicycle_config_ensure_init();
    return g_cfg.max_session_km;
}

/**
 * @brief 自行车 config km marker max。
 */
uint16_t bicycle_config_km_marker_max(void)
{
    bicycle_config_ensure_init();
    return g_cfg.km_marker_max;
}

/**
 * @brief 自行车 config track point cap。
 */
uint16_t bicycle_config_track_point_cap(void)
{
    bicycle_config_ensure_init();
    return g_cfg.track_point_cap;
}

/**
 * @brief 自行车 config track draw cap。
 */
uint16_t bicycle_config_track_draw_cap(void)
{
    bicycle_config_ensure_init();
    return g_cfg.track_draw_cap;
}

/**
 * @brief 自行车 config track point min dist m。
 */
double bicycle_config_track_point_min_dist_m(void)
{
    bicycle_config_ensure_init();
    return g_cfg.track_point_min_dist_m;
}

/**
 * @brief 自行车 config nav off route m。
 */
double bicycle_config_nav_off_route_m(void)
{
    bicycle_config_ensure_init();
    return g_cfg.nav_off_route_m;
}

static float bicycle_config_clamp_nav_sim_speed(float kph)
{
    if (kph < 5.0f) {
        return 5.0f;
    }
    if (kph > 360.0f) {
        return 360.0f;
    }
    return kph;
}

/**
 * @brief 自行车 config nav sim speed kph。
 */
float bicycle_config_nav_sim_speed_kph(void)
{
    bicycle_config_ensure_init();
    return g_cfg.nav_sim_speed_kph > 0.0f
        ? g_cfg.nav_sim_speed_kph : VMAP_NAV_SIM_SPEED_KPH;
}

/**
 * @brief 自行车 config set nav sim speed kph。
 */
void bicycle_config_set_nav_sim_speed_kph(float kph)
{
    bicycle_config_ensure_init();
    g_cfg.nav_sim_speed_kph = bicycle_config_clamp_nav_sim_speed(kph);
}

/**
 * @brief 自行车 config track arena bytes。
 * @return 请求的值。
 */
size_t bicycle_config_track_arena_bytes(void)
{
    bicycle_config_ensure_init();
    return (size_t)g_cfg.track_draw_cap * (size_t)BICYCLE_TRACK_DRAW_PT_BYTES
        + (size_t)g_cfg.track_point_cap * (size_t)BICYCLE_TRACK_PT_BYTES
        + (size_t)BICYCLE_TRACK_ARENA_SLACK_BYTES;
}
