/**
 * @file helm_idle.c
 * @brief 加速度判断静止：满 10 分钟关背光和 GNSS，BLE 不关。
 *
 * 半反屏无背光仍可见。休眠用全屏黑白电子钟：上为「静止」+ 时长，
 * 下为当前时刻 + 日期。时分秒各用独立标签，避免非等宽数字左右跳。
 * 静止满 1 小时后进入关机动画并断电。
 * 按键或拿起关闭并唤醒。静止与关机期间不弹 App 下发的通知横幅。
 * 进静止后 HCPU 由 DVFS ondemand 落到 72/96；唤醒不再钉 240。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "helm_idle.h"

#include "helm_font.h"
#include "helm_palette.h"
#include "helm_pwr.h"
#include "helm_widget.h"
#include "lv_pm_overlay.h"
#include "myvendor_devctl.h"
#include "myvendor_gnss.h"
#include "myvendor_board_sensor.h"
#include "myvendor_identity.h"
#include "myvendor_sound.h"
#include "myvendor_sys.h"
#include "sf32lb_dvfs.h"

#include "lvgl/lvgl.h"

#include <string.h>
#include <syslog.h>
#include <time.h>

#define HELM_IDLE_STILL_MS  (10u * 60u * 1000u)
#define HELM_IDLE_OFF_MS    (60u * 60u * 1000u)
#define HELM_IDLE_BOOT_MS   15000u
/** 静止期心跳周期：睡眠覆盖层会暂停主 UI 定时器，这条要自己跑。 */
#define HELM_IDLE_TICK_MS   250u
/** 相对低通的加速度残差阈值（m/s²）。 */
#define HELM_IDLE_ACCEL_TH  0.45f
/** 陀螺模阈值（rad/s），桌上微振不够、拿起/骑行会超过。 */
#define HELM_IDLE_GYRO_TH   0.20f

static const char *const s_wday[] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六"
};

typedef struct {
    lv_obj_t *d[6];
    lv_obj_t *colon[2];
    uint8_t ndigit;
} helm_idle_hms_t;

static bool s_sleep;
static bool s_boot_armed;
static bool s_stay;
static uint32_t s_boot_t0;
static uint32_t s_still_t0;
static uint32_t s_enter_t0;
static lv_obj_t *s_mask;
static lv_obj_t *s_date_lab;
static helm_idle_hms_t s_still;
static helm_idle_hms_t s_clock;

static void helm_idle_labs_clear(void)
{
    s_mask = NULL;
    s_date_lab = NULL;
    memset(&s_still, 0, sizeof(s_still));
    memset(&s_clock, 0, sizeof(s_clock));
}

static void helm_idle_mask_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_mask) {
        helm_idle_labs_clear();
    }
}

static void helm_idle_overlay_hide(void)
{
    if (s_mask == NULL) {
        return;
    }

    {
        lv_obj_t *mask = s_mask;

        helm_idle_labs_clear();
        lv_obj_delete(mask);
    }
}

static void helm_idle_label_set(lv_obj_t *lab, const char *txt)
{
    const char *cur;

    if (lab == NULL || txt == NULL) {
        return;
    }

    cur = lv_label_get_text(lab);
    if (cur != NULL && strcmp(cur, txt) == 0) {
        return;
    }

    lv_label_set_text(lab, txt);
}

static lv_coord_t helm_idle_glyph_w(const lv_font_t *font, uint32_t letter,
                                   lv_coord_t min_w)
{
    lv_coord_t z;

    z = (lv_coord_t)lv_font_get_glyph_width(font, letter, letter);
    if (z < min_w) {
        z = min_w;
    }

    return z;
}

/*
 * 静止屏是**全屏黑白电子钟**：半透半反屏在背光关掉后靠环境光看，所以底用固定白、
 * 字必须用**固定黑** —— 不能跟主题走。夜间调色板会把 INK 翻成浅色，配白底就是
 * "白字白底"，日期/时间/「静止」三样全看不见（用户 2026-09-27 报的
 * "静止状态的日期 时间 还有静止提示 都消失了"）。
 */
#define HELM_IDLE_INK   0x000000u

static lv_obj_t *helm_idle_cell(lv_obj_t *row, const lv_font_t *font,
                               lv_coord_t w, const char *txt)
{
    lv_obj_t *lab = helm_label(row, font, HELM_IDLE_INK, txt);

    lv_obj_set_width(lab, w);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    return lab;
}

static lv_obj_t *helm_idle_hms_build(lv_obj_t *parent, helm_idle_hms_t *out,
                                    const lv_font_t *font, bool with_sec)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_coord_t dw;
    lv_coord_t cw;
    uint8_t i;

    dw = (lv_coord_t)(helm_idle_glyph_w(font, (uint32_t)'0', 10) + 2);
    cw = (lv_coord_t)(helm_idle_glyph_w(font, (uint32_t)':', 8) + 4);

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 1, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    out->ndigit = with_sec ? 6u : 4u;
    for (i = 0; i < out->ndigit; i++) {
        if (i == 2u || i == 4u) {
            uint8_t ci = (i == 2u) ? 0u : 1u;

            out->colon[ci] = helm_idle_cell(row, font, cw, ":");
        }

        out->d[i] = helm_idle_cell(row, font, dw, "0");
    }

    if (!with_sec) {
        out->colon[1] = NULL;
        out->d[4] = NULL;
        out->d[5] = NULL;
    }

    return row;
}

static void helm_idle_set_2(lv_obj_t *hi, lv_obj_t *lo, unsigned v)
{
    char b[2];

    b[1] = '\0';
    b[0] = (char)('0' + (v / 10u) % 10u);
    helm_idle_label_set(hi, b);
    b[0] = (char)('0' + (v % 10u));
    helm_idle_label_set(lo, b);
}

static void helm_idle_colon_opa(lv_obj_t *colon, bool on)
{
    if (colon == NULL) {
        return;
    }

    lv_obj_set_style_text_opa(colon, on ? LV_OPA_COVER : LV_OPA_30, 0);
}

static void helm_idle_hms_set(const helm_idle_hms_t *clk, unsigned hh,
                             unsigned mm, unsigned ss, bool blink_on)
{
    if (clk == NULL || clk->d[0] == NULL) {
        return;
    }

    helm_idle_set_2(clk->d[0], clk->d[1], hh);
    helm_idle_set_2(clk->d[2], clk->d[3], mm);
    helm_idle_colon_opa(clk->colon[0], blink_on);
    if (clk->ndigit >= 6u) {
        helm_idle_set_2(clk->d[4], clk->d[5], ss);
        helm_idle_colon_opa(clk->colon[1], true);
    }
}

static bool helm_idle_wall_now(struct tm *out)
{
    time_t now = time(NULL);
    time_t local;
    int16_t tz;

    if (out == NULL || now < 1704067200) {
        return false;
    }

    tz = myvendor_devctl_tz_min_get();
    local = now + (time_t)tz * 60;
    if (gmtime_r(&local, out) == NULL) {
        return false;
    }

    return true;
}

static void helm_idle_overlay_refresh(void)
{
    uint32_t elapsed;
    unsigned hh;
    unsigned mm;
    unsigned ss;
    struct tm tm_buf;
    char date[24];

    if (!s_sleep || s_still.d[0] == NULL || s_clock.d[0] == NULL) {
        return;
    }

    elapsed = lv_tick_elaps(s_enter_t0) / 1000u;
    if (elapsed > 99u * 3600u + 59u * 60u + 59u) {
        elapsed = 99u * 3600u + 59u * 60u + 59u;
    }

    hh = elapsed / 3600u;
    mm = (elapsed / 60u) % 60u;
    ss = elapsed % 60u;
    helm_idle_hms_set(&s_still, hh, mm, ss, true);

    if (helm_idle_wall_now(&tm_buf)) {
        helm_idle_hms_set(&s_clock, (unsigned)tm_buf.tm_hour,
                          (unsigned)tm_buf.tm_min, 0,
                          (tm_buf.tm_sec & 1) == 0);
        lv_snprintf(date, sizeof(date), "%d月%d日  %s",
                    tm_buf.tm_mon + 1, tm_buf.tm_mday,
                    s_wday[tm_buf.tm_wday % 7]);
        helm_idle_label_set(s_date_lab, date);
    } else {
        helm_idle_label_set(s_clock.d[0], "-");
        helm_idle_label_set(s_clock.d[1], "-");
        helm_idle_label_set(s_clock.d[2], "-");
        helm_idle_label_set(s_clock.d[3], "-");
        helm_idle_colon_opa(s_clock.colon[0], true);
        helm_idle_label_set(s_date_lab, "");
    }
}

static lv_obj_t *helm_idle_block(lv_obj_t *parent)
{
    lv_obj_t *blk = lv_obj_create(parent);

    lv_obj_remove_style_all(blk);
    lv_obj_set_size(blk, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(blk, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(blk, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(blk, 8, 0);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_SCROLLABLE);
    return blk;
}

static void helm_idle_overlay_show(void)
{
    lv_obj_t *top;
    lv_obj_t *still_blk;
    lv_obj_t *clock_blk;
    lv_obj_t *rule;
    const lv_font_t *cap;
    const lv_font_t *date_font;

    if (s_mask != NULL) {
        helm_idle_overlay_refresh();
        return;
    }

    top = lv_layer_top();
    if (top == NULL) {
        return;
    }

    cap = helm_font_sys(22, helm_font_val());
    date_font = helm_font_sys(15, helm_font_title());

    s_enter_t0 = lv_tick_get();
    s_mask = lv_obj_create(top);
    lv_obj_remove_style_all(s_mask);
    lv_obj_set_size(s_mask, PAGE_HOR_RES, PAGE_VER_RES);
    lv_obj_align(s_mask, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_mask, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_mask, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(s_mask, 28, 0);
    lv_obj_set_flex_flow(s_mask, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_mask, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_mask, helm_idle_mask_deleted, LV_EVENT_DELETE, NULL);

    still_blk = helm_idle_block(s_mask);
    {
        lv_obj_t *cap_lab = helm_label(still_blk, cap, HELM_IDLE_INK, "静止");

        lv_obj_set_style_text_align(cap_lab, LV_TEXT_ALIGN_CENTER, 0);
    }
    (void)helm_idle_hms_build(still_blk, &s_still, helm_font_quad(), true);

    rule = lv_obj_create(s_mask);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 64, 2);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);

    clock_blk = helm_idle_block(s_mask);
    lv_obj_set_style_pad_row(clock_blk, 10, 0);
    (void)helm_idle_hms_build(clock_blk, &s_clock, helm_font_speed(), false);
    s_date_lab = helm_label(clock_blk, date_font, HELM_IDLE_INK, "");
    lv_obj_set_style_text_align(s_date_lab, LV_TEXT_ALIGN_CENTER, 0);

    helm_idle_overlay_refresh();
    lv_obj_move_foreground(s_mask);
    lv_refr_now(NULL);
    myvendor_sound_prompt();
}

bool helm_idle_is_sleeping(void)
{
    return s_sleep;
}

static bool helm_idle_moving(void)
{
    myvendor_sys_vec3_t raw;
    myvendor_sys_vec3_t filt;
    myvendor_sys_vec3_t gyro;
    float dx;
    float dy;
    float dz;
    float mag2;
    const float ath2 = HELM_IDLE_ACCEL_TH * HELM_IDLE_ACCEL_TH;
    const float gth2 = HELM_IDLE_GYRO_TH * HELM_IDLE_GYRO_TH;

    myvendor_board_sensor_accel_get(&raw);
    myvendor_board_sensor_accel_filt_get(&filt);
    myvendor_board_sensor_gyro_get(&gyro);
    if (!raw.valid) {
        /* IMU 无效时不要睡，避免一直黑屏。 */
        return true;
    }

    dx = raw.x - filt.x;
    dy = raw.y - filt.y;
    dz = raw.z - filt.z;
    mag2 = dx * dx + dy * dy + dz * dz;
    if (mag2 > ath2) {
        return true;
    }

    if (gyro.valid) {
        mag2 = gyro.x * gyro.x + gyro.y * gyro.y + gyro.z * gyro.z;
        if (mag2 > gth2) {
            return true;
        }
    }

    return false;
}

void helm_idle_dismiss_cover(void)
{
    helm_idle_overlay_hide();
}

/**
 * @brief 静止期专用的心跳（2026-09-20 回归后重加）。
 *
 * `helm_idle_tick()` 原来只由 `helm_ui_cb`（helm_shell 的主 UI 定时器）驱动，
 * 而进入静止时覆盖层会 `helm_shell_pause_for_cover()` **把那个定时器暂停** ——
 * 于是睡着以后：拿起唤醒不响应、静止屏上的时间冻结、1 小时自动关机也永不触发。
 * 这里给睡眠期单独一个 250 ms 的 LVGL 定时器，只做一件事：喂 `helm_idle_tick()`。
 *
 * 线程契约：只在 UI/LVGL 线程创建与使用（`helm_idle_enter/wake` 都在 ctl 投递
 * 到 UI 线程的那条路上跑），与 `helm_idle.h` 的说明一致。
 */
static lv_timer_t *s_tick_timer;

static void helm_idle_tick_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    helm_idle_tick();          /* 不自删：暂停由 disarm 控制 */
}

static void helm_idle_tick_timer_arm(void)
{
    if (s_tick_timer == NULL) {
        s_tick_timer = lv_timer_create(helm_idle_tick_timer_cb,
                                       HELM_IDLE_TICK_MS, NULL);
    } else {
        lv_timer_resume(s_tick_timer);
    }
}

static void helm_idle_tick_timer_disarm(void)
{
    if (s_tick_timer != NULL) {
        lv_timer_pause(s_tick_timer);
    }
}

void helm_idle_wake(void)
{
    s_still_t0 = lv_tick_get();
    if (helm_pwr_is_offing() || !s_sleep) {
        return;
    }

    helm_idle_tick_timer_disarm();
    sf32lb_dvfs_hold(SF32LB_DVFS_HOLD_AWAKE);
    s_sleep = false;
    helm_idle_overlay_hide();
    helm_pwr_bl_fade_on();
    myvendor_gnss_idle_sleep(false);
    syslog(LOG_INFO, "helm: idle wake (backlight+gnss, ble on)");
}

static void helm_idle_enter(void)
{
    if (s_sleep) {
        return;
    }

    s_sleep = true;
    helm_idle_tick_timer_arm();
    lv_pm_notify_dismiss();
    helm_idle_overlay_show();
    helm_pwr_bl_fade_off();
    myvendor_gnss_idle_sleep(true);
    sf32lb_dvfs_release(SF32LB_DVFS_HOLD_AWAKE);
    syslog(LOG_INFO, "helm: idle 10min still, backlight+gnss off (ble on)");
}

void helm_idle_force_enter(void)
{
    /* 调试：等同静止计时已满，进睡后仍可由按键或 IMU 唤醒。 */
    s_boot_armed = true;
    s_boot_t0 = lv_tick_get() - HELM_IDLE_BOOT_MS;
    s_still_t0 = lv_tick_get();
    if (s_sleep) {
        helm_idle_overlay_refresh();
        syslog(LOG_INFO, "helm: idle already on");
        return;
    }

    helm_idle_enter();
    syslog(LOG_INFO, "helm: idle enter (ctl, same as 10min still)");
}

void helm_idle_force_hour(void)
{
    s_boot_armed = true;
    s_boot_t0 = lv_tick_get() - HELM_IDLE_BOOT_MS;
    s_still_t0 = lv_tick_get();
    s_enter_t0 = lv_tick_get() - HELM_IDLE_OFF_MS;
    if (!s_sleep) {
        s_sleep = true;
        myvendor_gnss_idle_sleep(true);
    }

    syslog(LOG_INFO, "helm: idle hour exhausted (ctl)");
    helm_pwr_exec_as("静止超时");
}

void helm_idle_stay(bool stay)
{
    s_stay = stay;
    if (stay) {
        helm_idle_wake();
        syslog(LOG_INFO, "helm: idle stay (no auto still until reboot)");
    } else {
        s_still_t0 = lv_tick_get();
        syslog(LOG_INFO, "helm: idle auto restored");
    }
}

void helm_idle_tick(void)
{
    uint32_t now = lv_tick_get();

    if (myvendor_is_factory() || helm_pwr_is_offing()) {
        return;
    }

    if (s_sleep) {
        helm_idle_overlay_refresh();
        if (helm_pwr_dialog_open()) {
            helm_idle_wake();
            return;
        }

        if (helm_idle_moving()) {
            helm_idle_wake();
            return;
        }

        if (lv_tick_elaps(s_enter_t0) >= HELM_IDLE_OFF_MS &&
            myvendor_sys_power_state() != MYVENDOR_SYS_PWR_CHARGING) {
            syslog(LOG_INFO, "helm: idle 1h still, poweroff");
            helm_pwr_exec_as("静止超时");
        }

        return;
    }

    if (s_stay) {
        s_still_t0 = now;
        return;
    }

    if (!s_boot_armed) {
        s_boot_armed = true;
        s_boot_t0 = now;
        s_still_t0 = now;
        return;
    }

    if (lv_tick_elaps(s_boot_t0) < HELM_IDLE_BOOT_MS) {
        s_still_t0 = now;
        return;
    }

    if (helm_pwr_dialog_open()) {
        s_still_t0 = now;
        return;
    }

    if (helm_idle_moving()) {
        s_still_t0 = now;
        return;
    }

    if (lv_tick_elaps(s_still_t0) >= HELM_IDLE_STILL_MS) {
        helm_idle_enter();
    }
}
