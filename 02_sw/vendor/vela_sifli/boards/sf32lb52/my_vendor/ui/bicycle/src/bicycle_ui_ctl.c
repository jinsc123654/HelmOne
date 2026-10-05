/**
 * @file bicycle_ui_ctl.c
 * @brief 自行车 UI — ui_ctl。
 */

#include "bicycle_ui_ctl.h"

#include "helm_idle.h"
#include "helm_icon.h"   /* HELM_ICO_BELL/INBOX：通知横幅左侧图标 */
#include "helm_pwr.h"
#include "lv_pm_i18n.h"
#include "lv_pm_overlay.h"
#include "map_page.h"
#include "myvendor_bicycle_ctl.h"
#include "myvendor_identity.h"
#include "myvendor_sys.h"
#include "bicycle_runtime.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_style.h"
#include "bicycle_config.h"
#include "bicycle_demo.h"
#include "bicycle_gpx_sim.h"
#if VMAP_ROUTE_ENABLE
#include "vmap/vmap_route_trip.h"
#include "vmap/vmap_route_log.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
#include "myvendor_lcd_disp.h"
#endif

#ifndef BICYCLE_PHONE_NOTIF_MS
#define BICYCLE_PHONE_NOTIF_MS 3000u
#endif

#ifndef BICYCLE_UI_CTL_STYLE_REFR_SUPPRESS_MS
#define BICYCLE_UI_CTL_STYLE_REFR_SUPPRESS_MS 5000u
#endif

/**
 * @brief 自行车 ui ctl sync state。
 */
void bicycle_ui_ctl_sync_state(void)
{
    const char * style = vmap_style_name(vmap_style_get_id());
    const char * locale = lv_pm_i18n_locale_name(lv_pm_i18n_get());
    map_page_t * page = live_map_page_instance();

    myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_MAP_STYLE, style);
    myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_UI_LOCALE, locale);
    if (page) {
        map_page_sync_zoom_state(page);
    }
}

/**
 * @brief 自行车 ui ctl ready。
 */
void bicycle_ui_ctl_ready(void)
{
    myvendor_bicycle_ctl_ui_ready(true);
}

/**
 * @brief 自行车 ui ctl shutdown。
 */
void bicycle_ui_ctl_shutdown(void)
{
    myvendor_bicycle_ctl_ui_ready(false);
}

static void bicycle_ui_ctl_before_bulk_refr(void)
{
#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
    myvendor_lcd_disp_suppress_recover_ms(BICYCLE_UI_CTL_STYLE_REFR_SUPPRESS_MS);
#endif
}

static void bicycle_ui_ctl_handle_style_set(map_page_t * page, const char * name)
{
    if (page == NULL || name == NULL || name[0] == '\0') {
        return;
    }

    bicycle_ui_ctl_before_bulk_refr();

    if (map_page_set_style_by_name(page, name)) {
        bicycle_ui_ctl_sync_state();
    }
}

static void bicycle_ui_ctl_handle_locale_set(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }

    if (lv_pm_i18n_set_by_name(name)) {
        myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_UI_LOCALE, name);
    }
}

static void bicycle_ui_ctl_handle_notify_show(const char * arg)
{
    char buf[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char * title;
    char * body;
    char * icon;

    if (arg == NULL || arg[0] == '\0') {
        lv_pm_notify_show_icon("Test", "NSH top notification", HELM_ICO_BELL, 0);
        return;
    }

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    title = buf;
    body = strchr(buf, '|');
    if (body == NULL) {
        lv_pm_notify_show_icon(NULL, buf, HELM_ICO_INBOX, 0);
        return;
    }

    *body++ = '\0';
    icon = strchr(body, '|');
    if (icon != NULL) {
        *icon++ = '\0';
    }

    if (title[0] == '\0') {
        title = NULL;
    }
    if (body[0] == '\0' && title != NULL) {
        body = title;
        title = NULL;
    }

    lv_pm_notify_show_ex(title, body,
        (icon != NULL && icon[0] != '\0') ? icon : NULL, 0);
}

static void bicycle_ui_ctl_handle_bottom_show(const char * arg)
{
    if (arg == NULL || arg[0] == '\0') {
        lv_pm_bottom_show("NSH bottom toast", 0);
        return;
    }

    lv_pm_bottom_show(arg, 0);
}

static void bicycle_ui_ctl_handle_map_zoom(map_page_t * page, const char * arg)
{
    if (page == NULL) {
        lv_pm_bottom_show("Zoom: no map page", 0);
        return;
    }

    if (!map_page_apply_zoom_cmd(page, arg)) {
        lv_pm_bottom_show("Zoom: bad arg", 0);
        return;
    }
}

#if VMAP_ROUTE_ENABLE
/**
 * @brief 自行车 ui ctl parse nav。
 */
static bool bicycle_ui_ctl_parse_nav(const char * arg, double * from_lon,
    double * from_lat, double * to_lon, double * to_lat)
{
    char buf[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char * tokens[4];
    char * save = NULL;
    char * p;
    int count = 0;

    if (!arg || arg[0] == '\0' || !from_lon || !from_lat || !to_lon || !to_lat) {
        return false;
    }

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (p = strtok_r(buf, ",", &save); p != NULL && count < 4;
         p = strtok_r(NULL, ",", &save)) {
        tokens[count++] = p;
    }

    if (count == 2) {
        bicycle_gnss_fix_t fix;

        if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
            VMAP_NAV_WARN("ui: no GNSS fix for nav start");
            lv_pm_bottom_show("Nav: no GNSS fix", 0);
            return false;
        }
        *from_lon = fix.longitude;
        *from_lat = fix.latitude;
        *to_lon = atof(tokens[0]);
        *to_lat = atof(tokens[1]);
        return true;
    }

    if (count == 4) {
        *from_lon = atof(tokens[0]);
        *from_lat = atof(tokens[1]);
        *to_lon = atof(tokens[2]);
        *to_lat = atof(tokens[3]);
        return true;
    }

    return false;
}

/**
 * @brief 自行车 ui ctl handle nav plan。
 */
static void bicycle_ui_ctl_handle_nav_plan(map_page_t * page, const char * arg)
{
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;

    if (!bicycle_ui_ctl_parse_nav(arg, &from_lon, &from_lat, &to_lon, &to_lat)) {
        VMAP_NAV_WARN("ui: bad coords \"%s\"", arg ? arg : "");
        lv_pm_bottom_show("Nav: bad coords", 0);
        return;
    }

    VMAP_NAV_LOG("ui: plan frozen start %.6f,%.6f -> %.6f,%.6f",
        from_lon, from_lat, to_lon, to_lat);
    if (page == NULL || !map_page_nav_plan(page, from_lon, from_lat, to_lon, to_lat)) {
        VMAP_NAV_WARN("ui: map_page_nav_plan failed page=%p", (void *)page);
        lv_pm_bottom_show("Nav plan failed", 0);
        return;
    }
}

/**
 * @brief 自行车 ui ctl handle nav plan sim。
 */
static void bicycle_ui_ctl_handle_nav_plan_sim(map_page_t * page, const char * arg)
{
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;

    if (!bicycle_ui_ctl_parse_nav(arg, &from_lon, &from_lat, &to_lon, &to_lat)) {
        VMAP_NAV_WARN("ui: bad sim coords \"%s\"", arg ? arg : "");
        lv_pm_bottom_show("Nav sim: bad coords", 0);
        return;
    }

    VMAP_NAV_LOG("ui: sim plan %.6f,%.6f -> %.6f,%.6f",
        from_lon, from_lat, to_lon, to_lat);
    if (page == NULL
        || !map_page_nav_plan_sim(page, from_lon, from_lat, to_lon, to_lat)) {
        VMAP_NAV_WARN("ui: map_page_nav_plan_sim failed page=%p", (void *)page);
        lv_pm_bottom_show("Nav sim plan failed", 0);
        return;
    }
}

static void bicycle_ui_ctl_handle_nav_stop(map_page_t * page)
{
    VMAP_NAV_LOG("ui: nav stop");
    map_page_nav_stop(page);
    lv_pm_bottom_show("Nav stopped", 0);
}

static void bicycle_ui_ctl_handle_nav_sim_speed(const char * arg)
{
    char msg[48];
    float kph;

    if (!arg || arg[0] == '\0') {
        snprintf(msg, sizeof(msg), "Nav sim speed %.0f km/h",
            bicycle_config_nav_sim_speed_kph());
        lv_pm_bottom_show(msg, 0);
        return;
    }

    kph = (float)atof(arg);
    if (kph <= 0.0f) {
        lv_pm_bottom_show("Nav sim speed: bad value", 0);
        return;
    }

    bicycle_config_set_nav_sim_speed_kph(kph);
    if (bicycle_route_sim_active()) {
        bicycle_route_sim_set_speed(bicycle_config_nav_sim_speed_kph());
    }
    snprintf(msg, sizeof(msg), "Nav sim speed %.0f km/h",
        bicycle_config_nav_sim_speed_kph());
    lv_pm_bottom_show(msg, 0);
}

static void bicycle_ui_ctl_handle_gnss_sim(map_page_t * page, const char * arg)
{
    char path[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char toast[80];
    const char * p;
    const char * rest;
    bool reverse = false;
    float kph = 0.0f;
    float skip_km = 0.0f;
    int ret;
    char * end = NULL;

    if (arg == NULL || arg[0] == '\0' || strcmp(arg, "stop") == 0) {
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
        lv_pm_bottom_show("GNSS 模拟已停止", 0);
        return;
    }

    p = arg;
    if (strncmp(p, "rev|", 4) == 0) {
        reverse = true;
        p += 4;
    } else if (strncmp(p, "fwd|", 4) == 0) {
        p += 4;
    }

    rest = strchr(p, '|');
    if (rest != NULL) {
        size_t n = (size_t)(rest - p);

        if (n >= sizeof(path)) {
            n = sizeof(path) - 1u;
        }
        memcpy(path, p, n);
        path[n] = '\0';
        kph = (float)strtod(rest + 1, &end);
        if (end == rest + 1) {
            kph = 0.0f;
        }
        if (end != NULL && *end == '|') {
            skip_km = (float)strtod(end + 1, NULL);
            if (skip_km > 0.0f) {
                skip_km = skip_km / 1000.0f;
            }
        }
    } else {
        lv_strlcpy(path, p, sizeof(path));
    }

    if (path[0] == '\0') {
        lv_pm_bottom_show("GPX 路径为空", 0);
        return;
    }

    ret = map_page_gnss_sim(page, path, reverse, kph, skip_km);
    if (ret != 0) {
        snprintf(toast, sizeof(toast), "GPX 打开失败 (%d)", ret);
        lv_pm_bottom_show(toast, 0);
        return;
    }

    {
        const char * base = strrchr(path, '/');
        double at_m = bicycle_gpx_sim_prefix_m();

        base = (base != NULL && base[1] != '\0') ? base + 1 : path;
        if (at_m >= 50.0) {
            snprintf(toast, sizeof(toast), "%s %.36s @%.1fkm",
                reverse ? "反向回放" : "顺序回放", base, at_m / 1000.0);
        } else {
            snprintf(toast, sizeof(toast), "%s %.48s",
                reverse ? "反向回放" : "顺序回放", base);
        }
        lv_pm_bottom_show(toast, 0);
    }
}

/**
 * @brief 自行车 ui ctl parse trip。
 */
static bool bicycle_ui_ctl_parse_trip(const char * arg,
    vmap_route_waypoint_t * out, uint32_t * out_count)
{
    char buf[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char * save = NULL;
    char * seg;
    uint32_t n = 0;

    if (!arg || !out || !out_count) {
        return false;
    }

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    seg = strtok_r(buf, ";", &save);
    while (seg != NULL && n < VMAP_ROUTE_TRIP_MAX_WAYPOINTS) {
        char * comma = strchr(seg, ',');

        if (!comma) {
            return false;
        }
        *comma = '\0';
        out[n].lon = atof(seg);
        out[n].lat = atof(comma + 1);
        n++;
        seg = strtok_r(NULL, ";", &save);
    }

    if (n == 0) {
        return false;
    }

    *out_count = n;
    return true;
}

static bool bicycle_ui_ctl_trip_from_bridge(vmap_route_waypoint_t * out,
    uint32_t * out_count)
{
    int32_t lat[MYVENDOR_SYS_NAV_MAX_PTS];
    int32_t lon[MYVENDOR_SYS_NAV_MAX_PTS];
    uint8_t n = 0;
    uint8_t i;

    if (!out || !out_count) {
        return false;
    }
    if (!myvendor_sys_nav_take(lat, lon, &n) || n == 0) {
        return false;
    }

    for (i = 0; i < n && i < VMAP_ROUTE_TRIP_MAX_WAYPOINTS; i++) {
        out[i].lat = (double)lat[i] / 10000000.0;
        out[i].lon = (double)lon[i] / 10000000.0;
    }
    *out_count = i;
    return i > 0;
}

static void bicycle_ui_ctl_handle_nav_trip(map_page_t * page, const char * arg)
{
    vmap_route_waypoint_t wps[VMAP_ROUTE_TRIP_MAX_WAYPOINTS];
    uint32_t count = 0;
    bool ok;

    if (arg == NULL || arg[0] == '\0') {
        ok = bicycle_ui_ctl_trip_from_bridge(wps, &count);
    } else {
        ok = bicycle_ui_ctl_parse_trip(arg, wps, &count);
    }

    if (!ok) {
        lv_pm_bottom_show("Nav trip: bad coords", 0);
        return;
    }

    if (page == NULL || !map_page_nav_trip_plan(page, wps, count)) {
        lv_pm_bottom_show("Nav trip failed", 0);
        return;
    }
}
#endif

static void bicycle_ui_ctl_handle_ride_start(map_page_t * page)
{
    map_page_set_recording(page, true);
#if VMAP_ROUTE_ENABLE
    {
        vmap_route_waypoint_t wps[VMAP_ROUTE_TRIP_MAX_WAYPOINTS];
        uint32_t count = 0;

        if (bicycle_ui_ctl_trip_from_bridge(wps, &count)) {
            if (page == NULL || !map_page_nav_trip_plan(page, wps, count)) {
                lv_pm_bottom_show("Ride nav failed", 0);
                return;
            }
        }
    }
#endif
    lv_pm_bottom_show("Ride started", 0);
}

static void bicycle_ui_ctl_handle_ride_stop(map_page_t * page)
{
    bicycle_runtime_save_last_pos();
    map_page_set_recording(page, false);
#if VMAP_ROUTE_ENABLE
    map_page_nav_stop(page);
#endif
    lv_pm_bottom_show("Ride stopped", 0);
}

static void bicycle_ui_ctl_handle_sensor_set(const char * arg)
{
    unsigned hr = 0;
    unsigned cad = 0;
    unsigned pwr = 0;
    int n;

    if (arg == NULL || arg[0] == '\0' || strcmp(arg, "off") == 0 ||
        strcmp(arg, "0") == 0) {
        bicycle_runtime_update_sensors(0, false, 0, false, 0, false);
        lv_pm_bottom_show("Sensor: off", 0);
        return;
    }

    n = sscanf(arg, "%u,%u,%u", &hr, &cad, &pwr);
    if (n < 1) {
        lv_pm_bottom_show("Sensor: bad arg", 0);
        return;
    }

    bicycle_runtime_update_sensors((uint16_t)hr, n >= 1 && hr > 0,
        (uint16_t)cad, n >= 2 && cad > 0, (uint16_t)pwr, n >= 3 && pwr > 0);
}

/**
 * @brief 演示模式开关（`bicycle_nsh demo` / 菜单那一行）。
 * @param arg "on"|"off"|"toggle"（空 = 开）。
 */
static void bicycle_ui_ctl_handle_demo(const char * arg)
{
    char msg[64];

    if (arg != NULL && strcmp(arg, "off") == 0) {
        bicycle_demo_stop();
        lv_pm_bottom_show("Demo: off", 0);
        return;
    }

    if (arg != NULL && strcmp(arg, "toggle") == 0) {
        bicycle_demo_toggle();
    } else if (!bicycle_demo_active()) {
        int rc = bicycle_demo_start();

        if (rc != BICYCLE_DEMO_OK) {
            lv_pm_bottom_show(bicycle_demo_error_text(rc), 0);
            return;
        }
    }

    snprintf(msg, sizeof(msg), "Demo: %s", bicycle_demo_status());
    lv_pm_bottom_show(msg, 0);
}

static void bicycle_ui_ctl_handle_msg(map_page_t * page,
                                      const myvendor_bicycle_ctl_msg_t * msg)
{
    if (msg == NULL) {
        return;
    }

    switch ((myvendor_bicycle_ctl_op_t)msg->op) {
    case MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW:
        bicycle_ui_ctl_handle_notify_show(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_BOTTOM_SHOW:
        bicycle_ui_ctl_handle_bottom_show(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_MAP_ZOOM:
        bicycle_ui_ctl_handle_map_zoom(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_RIDE_START:
        bicycle_ui_ctl_handle_ride_start(page);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_RIDE_STOP:
        bicycle_ui_ctl_handle_ride_stop(page);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_SENSOR_SET:
        bicycle_ui_ctl_handle_sensor_set(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_GNSS_SIM:
        bicycle_ui_ctl_handle_gnss_sim(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_DEMO:
        bicycle_ui_ctl_handle_demo(msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_ENTER:
        helm_idle_force_enter();
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE:
        helm_idle_wake();
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_HOUR:
        helm_idle_force_hour();
        return;

    case MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY:
        helm_idle_stay(msg->arg[0] != '0');
        return;

#if VMAP_ROUTE_ENABLE
    case MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN:
        bicycle_ui_ctl_handle_nav_plan(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN_SIM:
        bicycle_ui_ctl_handle_nav_plan_sim(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_TRIP:
        bicycle_ui_ctl_handle_nav_trip(page, msg->arg);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_STOP:
        bicycle_ui_ctl_handle_nav_stop(page);
        return;

    case MYVENDOR_BICYCLE_CTL_OP_NAV_SIM_SPEED:
        bicycle_ui_ctl_handle_nav_sim_speed(msg->arg);
        return;
#endif

    default:
        break;
    }

    if (page == NULL) {
        return;
    }

    switch ((myvendor_bicycle_ctl_op_t)msg->op) {
    case MYVENDOR_BICYCLE_CTL_OP_STYLE_SET:
        bicycle_ui_ctl_handle_style_set(page, msg->arg);
        break;

    case MYVENDOR_BICYCLE_CTL_OP_STYLE_CYCLE:
        bicycle_ui_ctl_before_bulk_refr();
        if (map_page_cycle_style(page)) {
            bicycle_ui_ctl_sync_state();
        }
        break;

    case MYVENDOR_BICYCLE_CTL_OP_UI_REDRAW:
        bicycle_ui_ctl_before_bulk_refr();
        map_page_redraw(page);
        break;

    case MYVENDOR_BICYCLE_CTL_OP_LOCALE_SET:
        bicycle_ui_ctl_handle_locale_set(msg->arg);
        break;

    default:
        break;
    }
}

/**
 * @brief 关机或陀螺仪静止休眠时不弹 App 横幅。
 * @details 0xFF17 已写入最近通知；这里只丢掉待弹队列，避免盖住时钟/关机页。
 */
static bool bicycle_ui_phone_banner_quiet(void)
{
    return helm_pwr_is_offing() || helm_pwr_dialog_open() ||
           helm_idle_is_sleeping();
}

/**
 * @brief 抽空 ctl 队列，并弹出下一条手机通知横幅。
 */
void bicycle_ui_ctl_poll(map_page_t * page)
{
    myvendor_bicycle_ctl_msg_t msg;
    myvendor_sys_notif_t ntf;

    while (myvendor_bicycle_ctl_take(&msg)) {
        bicycle_ui_ctl_handle_msg(page, &msg);
    }

    if (myvendor_is_factory() || bicycle_ui_phone_banner_quiet()) {
        while (myvendor_sys_notif_take(&ntf)) {
        }

        return;
    }

    if (lv_pm_notify_is_showing()) {
        return;
    }

    if (!myvendor_sys_notif_take(&ntf)) {
        return;
    }

    /* 防御（板级，2026-09-24）：**弹窗限速 1.5 s**。
     *
     * 现场：微信**通话**通知是常驻的、手机侧持续下发，UI 每抽到一条就重弹一次
     * `lv_pm_notify_show_ex()`（modal 反复 show/hide）⇒ 整块 UI 卡死。
     * bridge 侧已做"同键合并 + 弹窗限流"（`companion_bridge_notif_post()`），
     * 这里在 UI 侧再兜一道：太密的直接丢弃（该条仍在 inbox 列表里）。 */
    {
        static uint32_t s_last_show_ms;
        uint32_t t = (uint32_t)lv_tick_get();

        if (s_last_show_ms != 0u && (uint32_t)(t - s_last_show_ms) < 1500u) {
            return;
        }

        s_last_show_ms = t ? t : 1u;
    }

    lv_pm_notify_show_ex(ntf.title[0] != '\0' ? ntf.title : "通知",
                         ntf.body[0] != '\0' ? ntf.body : ntf.title,
                         ntf.icon[0] != '\0' ? ntf.icon : NULL,
                         BICYCLE_PHONE_NOTIF_MS);
}
