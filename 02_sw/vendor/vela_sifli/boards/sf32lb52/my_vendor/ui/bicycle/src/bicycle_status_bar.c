/**
 * @file bicycle_status_bar.c
 * @brief 24px 黑顶栏：时钟、卫星、手机蓝牙、回连传感器、REC/NAV、电池。
 *
 * 不闪：进行中常亮白，正常绿，卫星信号差黄，无数据红。关的功能不画图标。
 */

#include "bicycle_status_bar.h"

#include "bicycle_runtime.h"
#include "bicycle_gpx_sim.h"
#include "eta9184.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_palette.h"
#include "helm_shell.h"
#include "lv_pm_bar.h"
#include "myvendor_devctl.h"
#include "myvendor_diag.h"   /* BLE 诊断异常 ⇒ 蓝牙图标红灯闪（用户 2026-09-27） */
#include "myvendor_gnss.h"   /* 星历下发 ⇒ 卫星图标黄灯闪 */
#include "myvendor_sys.h"

#include "lvgl/src/misc/lv_math.h"

#include <stdio.h>
#include <stdint.h>
#include <syslog.h>   /* 告警闪烁的打桩日志（定位完可删） */
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#ifndef BICYCLE_STATUS_BAR_POLL_MS
#define BICYCLE_STATUS_BAR_POLL_MS 1000u
#endif

/** 顶栏图标 16px；时钟/胶囊仍 14px，避免时间再次上移。 */
#define SB_ICO_H         16
#define SB_TEXT_H        14
#define SB_TIME_W        34
#define SB_TONE_HIDE     0u
#define SB_TONE_RED      1u
#define SB_TONE_YEL      2u
#define SB_TONE_GREEN    3u
#define SB_TONE_WAIT     4u
#define SB_BATT_BODY_W   20
#define SB_BATT_BODY_H   14
#define SB_BATT_FILL_MAX 16
#define SB_BATT_FILL_H   10
#define SB_BATT_WRAP_W   22
#define SB_BATT_WRAP_H   SB_ICO_H

static lv_obj_t * s_time_lbl;
static lv_obj_t * s_gps;
static lv_obj_t * s_ble;
static lv_obj_t * s_sns[MYVENDOR_SYS_SENSOR_KIND_N];
static lv_obj_t * s_pill_rec;
static lv_obj_t * s_pill_nav;
static lv_obj_t * s_pill_pause;
static lv_obj_t * s_batt_body;
static lv_obj_t * s_batt_nub;
static lv_obj_t * s_batt_fill;
static lv_obj_t * s_batt_bolt;
static lv_timer_t * s_timer;
static const lv_font_t * s_font;
static int s_battery_pct = -1;
static int s_last_gps = -1;
static int s_last_batt = -1;
static int s_last_chg = -1;
static int s_last_phone = -1;
static uint8_t s_last_sns[MYVENDOR_SYS_SENSOR_KIND_N];
static uint16_t s_last_time_hm = 0xFFFFu;
static bool s_last_rec;
static bool s_last_nav;
static bool s_last_pause;
/** @brief 上一拍是否处于两条"告警闪烁"窗口里（用于窗口结束时作废 tone 缓存、恢复原色）。 */
static bool s_ble_bad_was;
static bool s_gps_eph_was;
/** @brief 当前是否处于闪烁窗口（用来自动调快/恢复轮询周期，见 status_bar_alert_blink）。 */
static bool s_alert_fast;
/** @brief BLE 异常后红灯闪多久（ms）；期间只要有新异常上报，年龄就被刷新 ⇒ 自动续期。 */
#define SB_BLE_ALERT_MS   10000u

static const helm_ico_id_t s_sns_ico[MYVENDOR_SYS_SENSOR_KIND_N] = {
    HELM_ICO_HR,
    HELM_ICO_CAD,
    HELM_ICO_BOLT,
};

static void status_bar_style_label(lv_obj_t * obj)
{
    lv_obj_set_style_text_font(obj, s_font, 0);
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
}

static void status_bar_show(lv_obj_t * obj, bool on)
{
    if (obj == NULL) {
        return;
    }

    if (on) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void status_bar_set_tone(lv_obj_t * ico, uint8_t tone)
{
    if (ico == NULL) {
        return;
    }

    if (tone == SB_TONE_HIDE) {
        status_bar_show(ico, false);
        return;
    }

    status_bar_show(ico, true);
    if (tone == SB_TONE_WAIT) {
        helm_icon_set_color(ico, lv_color_white());
    } else if (tone == SB_TONE_RED) {
        helm_icon_set_color(ico, helm_color(HELM_COLOR_GPS_DEAD));
    } else if (tone == SB_TONE_YEL) {
        helm_icon_set_color(ico, helm_color(HELM_COLOR_YEL));
    } else {
        helm_icon_set_color(ico, helm_color(HELM_COLOR_GPS));
    }
}

static void status_bar_set_gps(const bicycle_runtime_t * rt)
{
    uint8_t tone;
    bool alive = rt && rt->gnss_alive;
    bool valid = rt && rt->gnss_valid;
    uint8_t q = rt ? rt->gnss_fix_quality : 0;
    uint8_t sats = rt ? rt->gnss_satellites : 0;
    uint8_t h = rt ? rt->gnss_hdop_x10 : 0;

    if (!alive) {
        tone = SB_TONE_RED;
    } else if (!valid) {
        tone = SB_TONE_WAIT;
    } else if (q <= 1 || sats < 5 || h > 20) {
        tone = SB_TONE_YEL;
    } else {
        tone = SB_TONE_GREEN;
    }

    if ((int)tone == s_last_gps) {
        return;
    }

    s_last_gps = (int)tone;
    status_bar_set_tone(s_gps, tone);
}

static void status_bar_set_phone(void)
{
    uint8_t tone;

    if (!myvendor_devctl_radio_get()) {
        tone = SB_TONE_HIDE;
    } else if (myvendor_sys_phone_ble_connected()) {
        tone = SB_TONE_GREEN;
    } else {
        tone = SB_TONE_WAIT;
    }

    if (s_ble == NULL || (int)tone == s_last_phone) {
        return;
    }

    s_last_phone = (int)tone;
    status_bar_set_tone(s_ble, tone);
}

static void status_bar_set_sensors(void)
{
    myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
    myvendor_sys_sensor_ui_t ui;
    size_t n = 0;
    size_t i;
    uint8_t k;
    bool want[MYVENDOR_SYS_SENSOR_KIND_N];
    bool sensor_on = myvendor_devctl_sensor_get();

    for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++) {
        want[k] = false;
    }

    if (sensor_on &&
        myvendor_devctl_sensor_recs_get(recs, MYVENDOR_DEVCTL_SENSOR_REC_MAX,
                                        &n) == 0) {
        for (i = 0; i < n; i++) {
            if (recs[i].autorc && recs[i].kind < MYVENDOR_SYS_SENSOR_KIND_N) {
                want[recs[i].kind] = true;
            }
        }
    }

    myvendor_sys_sensor_ui_get(&ui);

    for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++) {
        bool ready = (ui.slot[k].link == MYVENDOR_SYS_SENSOR_LINK_READY);
        uint8_t tone;

        if (!sensor_on || !want[k]) {
            tone = SB_TONE_HIDE;
        } else if (ready) {
            tone = SB_TONE_GREEN;
        } else {
            tone = SB_TONE_WAIT;
        }

        if (s_sns[k] == NULL || tone == s_last_sns[k]) {
            continue;
        }

        s_last_sns[k] = tone;
        status_bar_set_tone(s_sns[k], tone);
    }
}

static void status_bar_set_batt_chrome(bool charging)
{
    lv_color_t c = charging ? helm_color(HELM_COLOR_WARN) : lv_color_white();

    if (s_batt_body != NULL) {
        lv_obj_set_style_border_color(s_batt_body, c, 0);
    }

    if (s_batt_nub != NULL) {
        lv_obj_set_style_bg_color(s_batt_nub, c, 0);
    }

    if (s_batt_bolt != NULL) {
        helm_icon_set_color(s_batt_bolt, lv_color_white());
        status_bar_show(s_batt_bolt, charging);
    }
}

static void status_bar_refresh(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    char buf[8];
    bool rec;
    bool nav;
    lv_color_t batt_c;

    bicycle_status_bar_set_battery_pct(myvendor_sys_battery_percent());

    {
        bool charging = (myvendor_sys_power_state() == MYVENDOR_SYS_PWR_CHARGING);

        if (charging != (s_last_chg == 1)) {
            s_last_chg = charging ? 1 : 0;
            status_bar_set_batt_chrome(charging);
            s_last_batt = -1;
        }
    }

    status_bar_set_gps(rt);
    status_bar_set_phone();
    status_bar_set_sensors();

    if (s_time_lbl) {
        time_t now = time(NULL);
        int hour = -1;
        int min = -1;

        if (now >= 1704067200) {
            if (!myvendor_devctl_tz_hms((long)now, &hour, &min)) {
                hour = -1;
                min = -1;
            }
        }

        if (hour < 0) {
            myvendor_sys_gnss_t cfix;
            bool got = myvendor_sys_onboard_gnss_get(&cfix);

            if (!got) {
                got = myvendor_sys_phone_gnss_get(&cfix);
            }

            if (got && cfix.utc_sec >= 1704067200u) {
                time_t utc = (time_t)cfix.utc_sec;
                struct timeval tv;

                tv.tv_sec = utc;
                tv.tv_usec = 0;
                (void)settimeofday(&tv, NULL);
                if (!myvendor_devctl_tz_hms((long)utc, &hour, &min)) {
                    hour = -1;
                    min = -1;
                }
            }
        }

        if (hour < 0) {
            (void)bicycle_gpx_sim_get_wall_hms(&hour, &min);
        }

        if (hour < 0) {
            hour = 0;
            min = 0;
        }

        {
            const uint16_t hm = (uint16_t)(hour * 60 + min);

            if (hm != s_last_time_hm) {
                lv_snprintf(buf, sizeof(buf), "%02d:%02d", hour, min);
                lv_label_set_text(s_time_lbl, buf);
                s_last_time_hm = hm;
            }
        }
    }

    rec = rt && rt->recording;
    nav = map_page_nav_active(live_map_page_instance());

    if (s_pill_rec && rec != s_last_rec) {
        s_last_rec = rec;
        status_bar_show(s_pill_rec, rec);
    }

    if (s_pill_nav && nav != s_last_nav) {
        s_last_nav = nav;
        status_bar_show(s_pill_nav, nav);
    }

    {
        bool paused = helm_shell_paused();

        if (s_pill_pause && paused != s_last_pause) {
            s_last_pause = paused;
            status_bar_show(s_pill_pause, paused);
        }
    }


    if (s_batt_fill) {
        if (s_battery_pct >= 0) {
            if (s_battery_pct != s_last_batt) {
                lv_coord_t w = (lv_coord_t)lv_map(s_battery_pct, 0, 100, 0,
                                                  SB_BATT_FILL_MAX);

                if (s_battery_pct > 0 && w < 2) {
                    w = 2;
                }

                lv_obj_set_width(s_batt_fill, w);
                if (s_battery_pct <= 15) {
                    batt_c = helm_color(HELM_COLOR_GPS_DEAD);
                } else if (s_battery_pct <= 35) {
                    batt_c = helm_color(HELM_COLOR_BATT_MID);
                } else {
                    batt_c = helm_color(HELM_COLOR_GPS);
                }

                lv_obj_set_style_bg_color(s_batt_fill, batt_c, 0);
                s_last_batt = s_battery_pct;
            }
        } else if (s_last_batt != -2) {
            lv_obj_set_width(s_batt_fill, 0);
            s_last_batt = -2;
        }
    }
}

/**
 * @brief 两条"告警闪烁"（用户 2026-09-27 加）。
 *
 * @details ① diag 判 **BLE 异常** ⇒ 蓝牙图标**红灯闪**（窗口 `SB_BLE_ALERT_MS`）；
 *          ② **星历下发中** ⇒ 卫星图标**黄灯闪**（判据在 gnss 侧：最近 3 s 真的写出过帧）。
 *
 * @note 只在本拍**盖**一次（相位由 `lv_tick_get()` 取 500 ms 一档 ⇒ 1 s 一个"透↔实"周期）：
 *       **亮相 = 目标色 + 不透明，暗相 = `OPA_TRANSP`（全透明）**。⚠ 刻意**不用** `status_bar_show()`
 *       —— 它加的是 `LV_OBJ_FLAG_HIDDEN`，而隐藏对象会被 flex 布局跳过 ⇒ 旁边的图标/时钟会跟着
 *       左右跳（用户 2026-09-27 明确要求："不是消失，而是透明度设置为全透明"）。透明只影响绘制。
 *       窗口**结束**时先把 opa 复位为 `COVER`、再作废 tone 缓存 ⇒ 下一拍正常取色恢复原样。
 */
static void status_bar_alert_blink(void)
{
    const bool on = ((lv_tick_get() / 500u) & 1u) != 0u;
    const bool ble_bad = myvendor_diag_ble_unhealthy_age_ms() < SB_BLE_ALERT_MS;
    const bool eph_on = myvendor_gnss_eph_active();

    /*
     * ⚠ **轮询周期必须跟着闪**：默认 1 s 轮询 × 500 ms 相位 = **别名**（每次采样都落在同一
     * 相位 ⇒ 相位被冻住、一个颜色到底，用户 2026-09-27 实测"一直是白色"就是这样来的）。
     * 闪烁期间把轮询缩到 250 ms，窗口结束再恢复 1 s。
     */
    if ((ble_bad || eph_on) != s_alert_fast) {
        s_alert_fast = (ble_bad || eph_on);
        if (s_timer != NULL) {
            lv_timer_set_period(s_timer, s_alert_fast ? 250u : BICYCLE_STATUS_BAR_POLL_MS);
        }
    }

    if (s_ble != NULL) {
        if (ble_bad) {
            if (on) {
                helm_icon_set_color(s_ble, helm_color(HELM_COLOR_GPS_DEAD));
                lv_obj_set_style_opa(s_ble, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_opa(s_ble, LV_OPA_TRANSP, 0);
            }
        } else if (s_ble_bad_was) {
            lv_obj_set_style_opa(s_ble, LV_OPA_COVER, 0);
            s_last_phone = -1;   /* 作废缓存 ⇒ 下一拍回到正常色 */
        }
        s_ble_bad_was = ble_bad;
    }

    if (s_gps != NULL) {
        if (eph_on) {
            if (on) {
                helm_icon_set_color(s_gps, helm_color(HELM_COLOR_YEL));
                lv_obj_set_style_opa(s_gps, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_opa(s_gps, LV_OPA_TRANSP, 0);
            }
        } else if (s_gps_eph_was) {
            lv_obj_set_style_opa(s_gps, LV_OPA_COVER, 0);
            s_last_gps = -1;
        }
        s_gps_eph_was = eph_on;
    }
}

static void status_bar_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    status_bar_refresh();
    status_bar_alert_blink();   /* 盖在正常取色之上；窗口结束由它自己恢复 */
}

void bicycle_status_bar_set_battery_pct(int pct)
{
    if (pct < 0 || pct > 100) {
        s_battery_pct = -1;
        s_last_batt = -1;
        return;
    }

    if (pct != s_battery_pct) {
        s_battery_pct = pct;
        s_last_batt = -1;
    }
}

static lv_obj_t * make_pill(lv_obj_t * parent, const char * txt, uint32_t bg,
                           uint32_t fg)
{
    lv_obj_t * pill = lv_obj_create(parent);
    lv_obj_t * lbl;

    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, SB_TEXT_H);
    lv_obj_set_style_bg_color(pill, helm_color(bg), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 4, 0);
    lv_obj_set_style_pad_hor(pill, 3, 0);
    lv_obj_set_style_pad_ver(pill, 1, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);

    lbl = lv_label_create(pill);
    lv_label_set_text_static(lbl, txt);
    lv_obj_set_style_text_font(lbl, s_font, 0);
    lv_obj_set_style_text_color(lbl, helm_color(fg), 0);
    lv_obj_center(lbl);
    return pill;
}

static lv_obj_t * make_glyph_ico(lv_obj_t * parent, helm_ico_id_t id)
{
    lv_obj_t * ico = helm_icon_create(parent, id, SB_ICO_H);

    helm_icon_set_color(ico, helm_color(HELM_COLOR_GPS_DEAD));
    return ico;
}

void bicycle_status_bar_init(void)
{
    lv_obj_t * cont = lv_pm_status_bar_cont();
    lv_obj_t * flags;
    lv_obj_t * icos;
    lv_obj_t * batt_wrap;
    uint8_t k;

    if (cont == NULL || s_time_lbl != NULL) {
        return;
    }

    /* 仍是 12px 苹方位图（观感不变），只是链尾多了系统 TTF 保底 ——
     * 状态栏里也可能出现外来串（传感器名/通知角标），别让缺字画成方块。 */
    s_font = helm_font_status();

    (void)eta9184_start();

    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(cont, 4, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cont, 2, 0);

    s_time_lbl = lv_label_create(cont);
    lv_label_set_text(s_time_lbl, "00:00");
    lv_obj_set_size(s_time_lbl, SB_TIME_W, SB_TEXT_H);
    lv_obj_set_style_pad_top(s_time_lbl, 2, 0);
    lv_obj_set_style_text_align(s_time_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_time_lbl, LV_LABEL_LONG_CLIP);
    status_bar_style_label(s_time_lbl);

    icos = lv_obj_create(cont);
    lv_obj_remove_style_all(icos);
    lv_obj_set_height(icos, SB_ICO_H);
    lv_obj_set_width(icos, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icos, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icos, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(icos, 2, 0);
    lv_obj_clear_flag(icos, LV_OBJ_FLAG_CLICKABLE);

    s_gps = make_glyph_ico(icos, HELM_ICO_GPS);
    s_ble = make_glyph_ico(icos, HELM_ICO_BLE);

    for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++) {
        s_sns[k] = make_glyph_ico(icos, s_sns_ico[k]);
        lv_obj_add_flag(s_sns[k], LV_OBJ_FLAG_HIDDEN);
        s_last_sns[k] = 0xFFu;
    }

    flags = lv_obj_create(cont);
    lv_obj_remove_style_all(flags);
    lv_obj_set_height(flags, SB_ICO_H);
    lv_obj_set_flex_grow(flags, 1);
    lv_obj_set_flex_flow(flags, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flags, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(flags, 2, 0);
    lv_obj_set_style_min_width(flags, 0, 0);

    s_pill_rec = make_pill(flags, "REC", HELM_COLOR_HR, HELM_COLOR_PAPER);
    s_pill_nav = make_pill(flags, "NAV", HELM_COLOR_NAV, HELM_COLOR_PAPER);
    s_pill_pause = make_pill(flags, "PAUSE", HELM_COLOR_PAUSE_PILL, HELM_COLOR_INK);

    batt_wrap = lv_obj_create(cont);
    lv_obj_remove_style_all(batt_wrap);
    lv_obj_set_size(batt_wrap, SB_BATT_WRAP_W, SB_BATT_WRAP_H);
    lv_obj_clear_flag(batt_wrap, LV_OBJ_FLAG_CLICKABLE);

    s_batt_body = lv_obj_create(batt_wrap);
    lv_obj_remove_style_all(s_batt_body);
    lv_obj_set_size(s_batt_body, SB_BATT_BODY_W, SB_BATT_BODY_H);
    lv_obj_align(s_batt_body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(s_batt_body, 1, 0);
    lv_obj_set_style_border_color(s_batt_body, lv_color_white(), 0);
    lv_obj_set_style_radius(s_batt_body, 2, 0);
    lv_obj_set_style_pad_all(s_batt_body, 1, 0);

    s_batt_fill = lv_obj_create(s_batt_body);
    lv_obj_remove_style_all(s_batt_fill);
    lv_obj_set_size(s_batt_fill, 0, SB_BATT_FILL_H);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_batt_fill, helm_color(HELM_COLOR_GPS), 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);

    s_batt_nub = lv_obj_create(batt_wrap);
    lv_obj_remove_style_all(s_batt_nub);
    lv_obj_set_size(s_batt_nub, 2, 7);
    lv_obj_align(s_batt_nub, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_batt_nub, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_batt_nub, 1, 0);

    s_batt_bolt = helm_icon_create(batt_wrap, HELM_ICO_BOLT, SB_ICO_H);
    helm_icon_set_color(s_batt_bolt, lv_color_white());
    lv_obj_align(s_batt_bolt, LV_ALIGN_LEFT_MID, 3, 0);
    lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);

    s_last_rec = true;
    s_last_nav = true;
    s_last_pause = true;
    s_last_chg = -1;
    s_last_phone = -1;
    status_bar_refresh();
    s_timer = lv_timer_create(status_bar_timer_cb, BICYCLE_STATUS_BAR_POLL_MS, NULL);
}

void bicycle_status_bar_refresh(void)
{
    s_last_time_hm = 0xFFFFu;
    status_bar_refresh();
}
