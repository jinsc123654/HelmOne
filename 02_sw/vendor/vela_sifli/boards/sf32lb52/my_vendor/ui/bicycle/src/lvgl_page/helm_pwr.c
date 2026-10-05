/**
 * @file helm_pwr.c
 * @brief PA34 开机键：亮度步进与关机确认。低电提示与关机也在这里。
 *
 * 关机：与开机同一套白底 logo splash。事项滚入再滚出后，整屏慢慢刷黑断电。
 * 半透半反屏关背光仍能看见残影，必须把帧刷成全黑。
 * 进入关机界面后不再响应按键。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "helm_pwr.h"

#include "bicycle_ride_gpx.h"
#include "bicycle_runtime.h"
#include "drv_io.h"
#include "helm_font.h"
#include "helm_idle.h"
#include "helm_menu.h"
#include "helm_palette.h"
#include "helm_splash.h"
#include "helm_widget.h"
#include "lv_pm_overlay.h"
#include "myvendor_devctl.h"
#include "myvendor_gnss.h"
#include "myvendor_identity.h"
#include "myvendor_mtp.h"
#include "myvendor_sound.h"
#include "myvendor_sys.h"
#include "sf32lb_dvfs.h"

#include "lvgl/lvgl.h"

#include <string.h>
#include <syslog.h>

static lv_obj_t *s_mask;
static lv_obj_t *s_off;
static lv_obj_t *s_off_black;
static helm_splash_t s_off_sp;
static char s_off_key[16];
static lv_timer_t *s_off_tmr;
static uint8_t s_off_step;
static uint8_t s_off_chore;
static uint32_t s_off_t0;
static bool s_offing;
static bool s_bat_armed;
static bool s_bat_warned;
static uint8_t s_bat_off_hits;
static uint32_t s_bat_t0;
static lv_timer_t *s_bl_commit_tmr;
static lv_timer_t *s_bl_ramp_tmr;
static uint8_t s_bl_ramp_from;
static uint8_t s_bl_ramp_to;
static uint32_t s_bl_ramp_t0;

#define HELM_PWR_BL_COMMIT_MS 5000u
#define HELM_PWR_BL_RAMP_MS   220u
#define HELM_PWR_BL_RAMP_TICK_MS 16u
#define HELM_PWR_BAT_WARN_PCT    10
#define HELM_PWR_BAT_REARM_PCT   15
#define HELM_PWR_BAT_OFF_PCT      2
#define HELM_PWR_BAT_OFF_MV     (MYVENDOR_SYS_BAT_MV_EMPTY + 50)
#define HELM_PWR_BAT_GRACE_MS  8000u
#define HELM_PWR_BAT_OFF_HITS     3
#define HELM_PWR_OFF_ENTER_MS    400u
#define HELM_PWR_OFF_SLIDE_MS    HELM_SPLASH_SLIDE_MS
#define HELM_PWR_OFF_HOLD_MS     640u
#define HELM_PWR_OFF_BYE_HOLD_MS 80u
#define HELM_PWR_OFF_BLACK_MS    800u
#define HELM_PWR_OFF_BYE_MS      (HELM_PWR_OFF_BYE_HOLD_MS + HELM_PWR_OFF_BLACK_MS)
#define HELM_PWR_OFF_TICK_MS      40u

enum {
    HELM_PWR_OFF_ENTER = 0,
    HELM_PWR_OFF_SLIDE_IN,
    HELM_PWR_OFF_RUN,
    HELM_PWR_OFF_HOLD,
    HELM_PWR_OFF_SLIDE_OUT,
    HELM_PWR_OFF_BYE,
    HELM_PWR_OFF_CUT
};

static void helm_pwr_bl_ramp_stop(void)
{
    if (s_bl_ramp_tmr != NULL) {
        lv_timer_delete(s_bl_ramp_tmr);
        s_bl_ramp_tmr = NULL;
    }
}

static void helm_pwr_bl_ramp_cb(lv_timer_t *t)
{
    uint32_t e;
    int from;
    int to;
    int v;

    LV_UNUSED(t);
    e = lv_tick_elaps(s_bl_ramp_t0);
    from = (int)s_bl_ramp_from;
    to = (int)s_bl_ramp_to;
    if (e >= HELM_PWR_BL_RAMP_MS) {
        (void)BSP_LCD_BL_PwmPct(s_bl_ramp_to);
        s_bl_ramp_tmr = NULL;
        lv_timer_delete(t);
        return;
    }

    v = from + (int)(((int32_t)(to - from) * (int32_t)e) /
                     (int32_t)HELM_PWR_BL_RAMP_MS);
    if (v < 0) {
        v = 0;
    } else if (v > 100) {
        v = 100;
    }

    (void)BSP_LCD_BL_PwmPct((uint8_t)v);
}

static void helm_pwr_bl_ramp_to(uint8_t pwm)
{
    uint8_t cur = BSP_LCD_BL_GetPct();
    uint8_t maxv = BSP_LCD_BL_GetMaxPct();

    if (maxv != 0 && pwm > maxv) {
        pwm = maxv;
    }

    s_bl_ramp_from = cur;
    s_bl_ramp_to = pwm;
    s_bl_ramp_t0 = lv_tick_get();
    if (cur == pwm) {
        helm_pwr_bl_ramp_stop();
        (void)BSP_LCD_BL_PwmPct(pwm);
        return;
    }

    if (s_bl_ramp_tmr != NULL) {
        lv_timer_reset(s_bl_ramp_tmr);
        return;
    }

    s_bl_ramp_tmr = lv_timer_create(helm_pwr_bl_ramp_cb,
                                    HELM_PWR_BL_RAMP_TICK_MS, NULL);
}

/** @brief 当前 PWM 线性过渡到 0，不改 persist。 */
void helm_pwr_bl_fade_off(void)
{
    helm_pwr_bl_ramp_to(0);
}

/** @brief 当前 PWM 线性过渡到策略亮度。 */
void helm_pwr_bl_fade_on(void)
{
    helm_pwr_bl_ramp_to(BSP_LCD_BL_GetSavedPct());
}

static void helm_pwr_bl_commit_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_bl_commit_tmr = NULL;
    myvendor_devctl_bl_commit();
}

static void helm_pwr_bl_defer_commit(void)
{
    if (s_bl_commit_tmr != NULL) {
        lv_timer_reset(s_bl_commit_tmr);
        return;
    }

    s_bl_commit_tmr = lv_timer_create(helm_pwr_bl_commit_cb,
                                      HELM_PWR_BL_COMMIT_MS, NULL);
    if (s_bl_commit_tmr != NULL) {
        lv_timer_set_repeat_count(s_bl_commit_tmr, 1);
    }
}

static void helm_pwr_bl_commit_now(void)
{
    if (s_bl_commit_tmr != NULL) {
        lv_timer_delete(s_bl_commit_tmr);
        s_bl_commit_tmr = NULL;
    }

    myvendor_devctl_bl_commit();
}

static uint8_t helm_pwr_pwm_to_ui(uint8_t pwm)
{
    uint8_t maxv = myvendor_devctl_bl_max();
    uint8_t ui;

    if (maxv == 0) {
        maxv = 80;
    }

    ui = (uint8_t)(((unsigned)pwm * 100u) / (unsigned)maxv);
    if (ui > 100) {
        ui = 100;
    }

    return (uint8_t)(ui / HELM_PWR_BL_UI_STEP * HELM_PWR_BL_UI_STEP);
}

/** @brief 当前策略背光换成 UI 百分比。 */
uint8_t helm_pwr_bl_ui(void)
{
    return helm_pwr_pwm_to_ui(myvendor_devctl_bl_get());
}

/** @brief UI 百分比换成要下发的 PWM。 */
uint8_t helm_pwr_ui_to_pwm(uint8_t ui)
{
    uint8_t maxv = myvendor_devctl_bl_max();

    if (maxv == 0) {
        maxv = 80;
    }

    if (ui > 100) {
        ui = 100;
    }

    return (uint8_t)(((unsigned)ui * (unsigned)maxv + 50u) / 100u);
}

/** @brief 亮度 0→20→…→100→0，PWM 线性过渡。 */
uint8_t helm_pwr_bl_cycle(void)
{
    uint8_t ui = helm_pwr_bl_ui();

    if (ui >= 100u) {
        ui = 0u;
    } else {
        ui = (uint8_t)(ui + HELM_PWR_BL_UI_STEP);
        if (ui > 100u) {
            ui = 100u;
        }
    }

    (void)myvendor_devctl_bl_apply(helm_pwr_ui_to_pwm(ui));
    helm_pwr_bl_ramp_to(myvendor_devctl_bl_get());
    helm_pwr_bl_defer_commit();
    return ui;
}

static void helm_pwr_mask_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_mask) {
        s_mask = NULL;
    }
}

static void helm_pwr_off_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_off) {
        s_off = NULL;
        s_off_black = NULL;
        helm_splash_clear(&s_off_sp);
    }
}

/** @brief 关机动画进行中。 */
bool helm_pwr_is_offing(void)
{
    return s_offing;
}

static void helm_pwr_opa(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static uint32_t helm_pwr_off_bar_ms(void)
{
    return HELM_PWR_OFF_ENTER_MS +
           (3u * (HELM_PWR_OFF_SLIDE_MS + HELM_PWR_OFF_HOLD_MS +
                  HELM_PWR_OFF_SLIDE_MS)) +
           HELM_PWR_OFF_BYE_MS;
}

/** @brief 关机事项行：`s_off_key  ·  val`。busy 时跟省略点。 */
static void helm_pwr_row_set(const char *val, bool busy)
{
    helm_splash_set_row(&s_off_sp, s_off_key, val, busy);
}

static const char *helm_pwr_eph_txt(myvendor_gnss_off_t r)
{
    switch (r) {
    case MYVENDOR_GNSS_OFF_DUMPED:
        return "已保存";
    case MYVENDOR_GNSS_OFF_SKIP_IDLE:
        return "定位休眠";
    case MYVENDOR_GNSS_OFF_SKIP_FACTORY:
        return "工厂模式";
    case MYVENDOR_GNSS_OFF_SKIP_AUTO:
        return "同步已关";
    case MYVENDOR_GNSS_OFF_SKIP_NOFIX:
        return "无定位";
    case MYVENDOR_GNSS_OFF_SKIP_NODATA:
        return "无星历数据";
    case MYVENDOR_GNSS_OFF_TIMEOUT:
        return "超时";
    default:
        return "已跳过";
    }
}

static void helm_pwr_do_gpx(void)
{
    int ret;

    if (!bicycle_ride_gpx_active()) {
        helm_pwr_row_set("无记录", false);
        return;
    }

    bicycle_runtime_save_last_pos();
    ret = bicycle_ride_gpx_commit(NULL, 0);
    bicycle_ride_gpx_wait_idle();
    helm_pwr_row_set(ret == 0 ? "已保存" : "未完成", false);
}

static void helm_pwr_do_eph(void)
{
    myvendor_gnss_off_t r;

    r = myvendor_gnss_prepare_poweroff();
    helm_pwr_row_set(helm_pwr_eph_txt(r), false);
}

static void helm_pwr_do_mtp(void)
{
    myvendor_mtp_prepare_poweroff();
    helm_pwr_row_set("已关闭", false);
}

static void helm_pwr_chore_arm(unsigned i)
{
    static const char *const keys[3] = {"骑行记录", "卫星星历", "文件存储"};
    static const char *const busy[3] = {"保存中", "保存中", "关闭中"};

    if (i >= 3u) {
        return;
    }

    lv_snprintf(s_off_key, sizeof(s_off_key), "%s", keys[i]);
    helm_pwr_row_set(busy[i], true);
}

static void helm_pwr_chore_run(unsigned i)
{
    switch (i) {
    case 0:
        helm_pwr_do_gpx();
        break;
    case 1:
        helm_pwr_do_eph();
        break;
    case 2:
        helm_pwr_do_mtp();
        break;
    default:
        break;
    }
}

static void helm_pwr_slide_in(unsigned i)
{
    helm_pwr_chore_arm(i);
    helm_splash_slide_in(&s_off_sp);
}

static void helm_pwr_slide_out(void)
{
    helm_splash_slide_out(&s_off_sp);
}

static void helm_pwr_paint_black(void)
{
    helm_splash_hide_brand(&s_off_sp);

    if (s_off_black != NULL) {
        lv_obj_set_style_opa(s_off_black, LV_OPA_COVER, 0);
    }

    if (s_off != NULL) {
        lv_obj_set_style_bg_color(s_off, lv_color_black(), 0);
        lv_obj_set_style_opa(s_off, LV_OPA_COVER, 0);
    }

    lv_refr_now(NULL);
}

static void helm_pwr_bye_begin(void)
{
    lv_anim_t a;

    s_off_sp.busy = false;
    lv_refr_now(NULL);

    if (s_off_black == NULL) {
        return;
    }

    lv_anim_delete(s_off_black, helm_pwr_opa);
    lv_obj_set_style_opa(s_off_black, LV_OPA_TRANSP, 0);
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_off_black);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_delay(&a, HELM_PWR_OFF_BYE_HOLD_MS);
    lv_anim_set_time(&a, HELM_PWR_OFF_BLACK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, helm_pwr_opa);
    lv_anim_start(&a);
}

static void helm_pwr_off_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    helm_splash_pump(&s_off_sp);

    switch (s_off_step) {
    case HELM_PWR_OFF_ENTER:
        if (lv_tick_elaps(s_off_t0) < HELM_PWR_OFF_ENTER_MS) {
            return;
        }

        s_off_chore = 0;
        s_off_step = HELM_PWR_OFF_SLIDE_IN;
        s_off_t0 = lv_tick_get();
        helm_pwr_slide_in(s_off_chore);
        return;

    case HELM_PWR_OFF_SLIDE_IN:
        if (lv_tick_elaps(s_off_t0) < HELM_PWR_OFF_SLIDE_MS) {
            return;
        }

        s_off_step = HELM_PWR_OFF_RUN;
        return;

    case HELM_PWR_OFF_RUN:
        helm_pwr_chore_run(s_off_chore);
        s_off_t0 = lv_tick_get();
        s_off_step = HELM_PWR_OFF_HOLD;
        return;

    case HELM_PWR_OFF_HOLD:
        if (lv_tick_elaps(s_off_t0) < HELM_PWR_OFF_HOLD_MS) {
            return;
        }

        s_off_step = HELM_PWR_OFF_SLIDE_OUT;
        s_off_t0 = lv_tick_get();
        helm_pwr_slide_out();
        return;

    case HELM_PWR_OFF_SLIDE_OUT:
        if (lv_tick_elaps(s_off_t0) < HELM_PWR_OFF_SLIDE_MS) {
            return;
        }

        if (s_off_chore < 2u) {
            s_off_chore++;
            s_off_step = HELM_PWR_OFF_SLIDE_IN;
            s_off_t0 = lv_tick_get();
            helm_pwr_slide_in(s_off_chore);
            return;
        }

        s_off_step = HELM_PWR_OFF_BYE;
        s_off_t0 = lv_tick_get();
        helm_pwr_bye_begin();
        return;

    case HELM_PWR_OFF_BYE:
        if (lv_tick_elaps(s_off_t0) < HELM_PWR_OFF_BYE_MS) {
            return;
        }

        s_off_step = HELM_PWR_OFF_CUT;
        helm_pwr_paint_black();
        if (s_off_tmr != NULL) {
            lv_timer_delete(s_off_tmr);
            s_off_tmr = NULL;
        }

        myvendor_devctl_power_cut();
        return;

    default:
        return;
    }
}

static void helm_pwr_off_show(const char *title)
{
    lv_obj_t *top;
    lv_anim_t a;

    LV_UNUSED(title);

    top = lv_layer_top();
    if (top == NULL) {
        helm_pwr_do_gpx();
        helm_pwr_do_eph();
        helm_pwr_do_mtp();
        myvendor_devctl_power_cut();
        return;
    }

    s_off = lv_obj_create(top);
    lv_obj_remove_style_all(s_off);
    lv_obj_set_size(s_off, PAGE_HOR_RES, PAGE_VER_RES);
    lv_obj_align(s_off, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(s_off, 0, 0);
    lv_obj_add_flag(s_off, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_off, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_off, helm_pwr_off_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_opa(s_off, LV_OPA_TRANSP, 0);

    helm_splash_build(&s_off_sp, s_off);
    /* 底色跟主题（用户 2026-09-26 晚："开关机界面也[要]受主题变化影响"）：
     * 日光仍是白底黑字；夜间走黑底白字 —— 否则关机会先闪一整屏白，再接黑遮罩淡入。
     * `set_gray` 传的是**背景**灰阶，文字/进度条自动取反（`ink = 255 - g`）。
     * 这里每次弹出都重建 ⇒ 直接读当前调色板即可。 */
    helm_splash_set_gray(&s_off_sp, helm_pal_night() ? 0u : 255u);
    s_off_key[0] = '\0';
    helm_splash_bar_set(&s_off_sp, 0);
    helm_splash_bar_linear(&s_off_sp, 0, 100, helm_pwr_off_bar_ms());

    s_off_black = lv_obj_create(s_off);
    lv_obj_remove_style_all(s_off_black);
    lv_obj_set_size(s_off_black, PAGE_HOR_RES, PAGE_VER_RES);
    lv_obj_align(s_off_black, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_off_black, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_off_black, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_off_black, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_off_black, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_off_black,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_off_black);

    lv_obj_move_foreground(s_off);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s_off);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 360);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, helm_pwr_opa);
    lv_anim_start(&a);

    s_off_step = HELM_PWR_OFF_ENTER;
    s_off_chore = 0;
    s_off_t0 = lv_tick_get();
    if (s_off_tmr != NULL) {
        lv_timer_delete(s_off_tmr);
    }

    s_off_tmr = lv_timer_create(helm_pwr_off_tick, HELM_PWR_OFF_TICK_MS, NULL);
    lv_refr_now(NULL);
}

/** @brief 关机动画（标题可换）后断电。进入后不再响应按键。 */
void helm_pwr_exec_as(const char *title)
{
    if (s_offing) {
        return;
    }

    s_offing = true;
    sf32lb_dvfs_hold(SF32LB_DVFS_HOLD_OFFING);
    helm_pwr_bl_ramp_stop();
    helm_pwr_bl_commit_now();
    lv_pm_notify_dismiss();
    helm_idle_dismiss_cover();
    if (s_mask != NULL) {
        lv_obj_t *mask = s_mask;

        s_mask = NULL;
        lv_obj_delete(mask);
    }

    helm_pwr_bl_fade_on();
    myvendor_sound_ok();
    helm_pwr_off_show(title ? title : "关机");
}

/** @brief 菜单「关机」：直接进入关机 splash。 */
void helm_pwr_exec(void)
{
    helm_pwr_exec_as("关机");
}

/** @brief KEY1：关掉关机确认框。 */
void helm_pwr_dialog_cancel(void)
{
    if (s_offing || s_mask == NULL) {
        return;
    }

    {
        lv_obj_t *mask = s_mask;

        s_mask = NULL;
        lv_obj_delete(mask);
    }
    myvendor_sound_back();
}

/** @brief KEY2：确认断电。 */
void helm_pwr_dialog_confirm(void)
{
    if (s_offing) {
        return;
    }

    helm_obj_press(s_mask);
    helm_pwr_exec_as("关机");
}

/** @brief 关机确认框是否在。 */
bool helm_pwr_dialog_open(void)
{
    return s_mask != NULL;
}

/** @brief PWR 单击：循环亮度。确认框打开时当取消。 */
void helm_pwr_on_click(void)
{
    char buf[8];
    uint8_t ui;

    if (s_offing) {
        return;
    }

    if (s_mask != NULL) {
        helm_pwr_dialog_cancel();
        return;
    }

    ui = helm_pwr_bl_cycle();
    lv_snprintf(buf, sizeof(buf), "%u%%", (unsigned)ui);
    /* 系统通知也带图标（用户 2026-09-27：和 App 通知一样，左侧给一个图标）。 */
    lv_pm_notify_show_icon("亮度", buf, HELM_ICO_SUN, 1200);
    helm_menu_refresh();
}

/** @brief PWR 长按 2s：弹出关机确认。 */
void helm_pwr_on_long(void)
{
    lv_obj_t *top;
    lv_obj_t *card;
    lv_obj_t *body;
    const lv_font_t *f;

    if (s_offing || s_mask != NULL) {
        return;
    }

    top = lv_layer_top();
    if (top == NULL) {
        return;
    }

    f = helm_font_sys(15, helm_font_lab());
    lv_pm_notify_dismiss();
    s_mask = helm_mask_create(top);
    lv_obj_set_size(s_mask, PAGE_HOR_RES, PAGE_VER_RES);
    lv_obj_add_event_cb(s_mask, helm_pwr_mask_deleted, LV_EVENT_DELETE, NULL);
    card = helm_card_create(s_mask, HELM_ICO_POWER, "关机？", true);
    body = helm_card_body(card);
    {
        lv_obj_t *hint = helm_label(body, f, HELM_COLOR_INK, "保存后断电");

        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_ver(hint, 4, 0);
    }
    helm_softkeys_create(body, HELM_COLOR_INK);
    lv_obj_move_foreground(s_mask);
    helm_obj_stagger_in(s_mask);
    myvendor_sound_prompt();
}

/** @brief 低电策略：约 10% 提示；约 3.05 V 存 GPX 后关机。 */
void helm_pwr_bat_tick(void)
{
    int pct;
    int mv;
    uint8_t pwr;
    bool charging;
    bool empty;

    if (s_offing || myvendor_is_factory()) {
        return;
    }

    if (!s_bat_armed) {
        s_bat_armed = true;
        s_bat_t0 = lv_tick_get();
        return;
    }

    if (lv_tick_elaps(s_bat_t0) < HELM_PWR_BAT_GRACE_MS) {
        return;
    }

    pct = myvendor_sys_battery_percent();
    mv = myvendor_sys_battery_mv();
    pwr = myvendor_sys_power_state();
    charging = (pwr == MYVENDOR_SYS_PWR_CHARGING);
    if (pct < 0 || mv < 0) {
        s_bat_off_hits = 0;
        return;
    }

    if (charging) {
        s_bat_off_hits = 0;
        if (pct >= HELM_PWR_BAT_REARM_PCT) {
            s_bat_warned = false;
        }

        return;
    }

    if (pct >= HELM_PWR_BAT_REARM_PCT) {
        s_bat_warned = false;
        s_bat_off_hits = 0;
        return;
    }

    empty = (pct <= HELM_PWR_BAT_OFF_PCT) || (mv <= HELM_PWR_BAT_OFF_MV);
    if (empty) {
        s_bat_off_hits++;
        if (s_bat_off_hits < HELM_PWR_BAT_OFF_HITS) {
            return;
        }

        syslog(LOG_WARNING, "helm: low bat off pct=%d mv=%d", pct, mv);
        helm_pwr_exec_as("电量耗尽");
        return;
    }

    s_bat_off_hits = 0;
    if (s_bat_warned || pct > HELM_PWR_BAT_WARN_PCT) {
        return;
    }

    s_bat_warned = true;
    if (helm_idle_is_sleeping()) {
        helm_idle_wake();
    }

    myvendor_sound_warn();
    lv_pm_notify_show_icon("电量低", "请尽快充电", HELM_ICO_BATT, 4000);
    syslog(LOG_INFO, "helm: low bat warn pct=%d mv=%d", pct, mv);
}
