/**
 * @file startup_page.c
 * @brief 开机 splash：黑屏播完开机音 → 白屏后再滚入事项。
 *
 * 事项与关机同款「左边 · 右边」：
 *   骑行记录 · 恢复中 / 已恢复|已检查|无记录
 *   系统字体 · 加载中 / 已加载|未找到
 *   地图加载 · 加载中 → 文件中 / 已加载
 *
 * 每项：滚入 → 干活 → 出结果（停点）→ 短停 → 滚出。地图事项只准备
 * catalog/region 文件；首帧在 LiveMap 淡入完成后开始。细节见 doc/splash.md。
 */

#include "startup_page.h"

#include "bicycle_page_anima.h"
#include "bicycle_page_ids.h"
#include "bicycle_ride_gpx.h"
#include "helm_font.h"
#include "helm_palette.h"
#include "helm_splash.h"
#include "live_map/map_page.h"
#include "lv_pm_anima.h"
#include "lv_pm_core.h"
#include "lv_pm_port.h"
#include "myvendor_sound.h"
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
void board_ble_companion_start_after_ui(void);
#endif
#include "myvendor_system_font.h"
#include "myvendor_watchdog.h"
#include "myvendor_mtp.h"
#include "myvendor_gnss.h"

#include <string.h>

#define STARTUP_READY_HOLD_MS          300
#define STARTUP_MAX_WAIT_MS            12000
#define STARTUP_FADE_MS                400
#define STARTUP_POLL_MS                50
#define STARTUP_FONT_CHUNKS            4
#define STARTUP_BG_MS                  360
#define STARTUP_BG_STEPS               HELM_SPLASH_BG_STEPS
#define STARTUP_SLIDE_MS               HELM_SPLASH_SLIDE_MS
#define STARTUP_HOLD_MS                480u

/** 开机事项：GPX 恢复 → 字体 → 地图文件。工厂固件跳过字体。 */
enum {
    STARTUP_CHORE_SALVAGE = 0,
    STARTUP_CHORE_FONT,
    STARTUP_CHORE_MAP,
    STARTUP_CHORE_COUNT
};

/** 单项时序：滚入 → 干活 → 看结果 → 滚出。 */
enum {
    STARTUP_STEP_SLIDE_IN = 0,
    STARTUP_STEP_RUN,
    STARTUP_STEP_HOLD,
    STARTUP_STEP_SLIDE_OUT
};

/**
 * @brief 开机 splash 页状态。
 */
typedef struct {
    lv_obj_t * root;           /**< lv_pm 页面根。 */
    lv_obj_t * cont;           /**< 淡出目标（与 root 相同）。 */
    helm_splash_t splash;      /**< 共用 logo / 事项行 / 进度条。 */
    lv_timer_t * poll_timer;   /**< 50 ms 启动事项状态机。 */
    uint32_t appear_tick;      /**< will_appear 时刻，用于总超时。 */
    uint32_t ready_tick;       /**< 末项滚出后的短 hold。 */
    uint32_t chore_t0;         /**< 当前 step 起始。 */
    int16_t bg_applied;        /**< 已应用的背景灰阶；-1 未设。 */
    uint8_t bar_goal;          /**< 进度条目标，避免重复开动画。 */
    uint8_t chore;             /**< `STARTUP_CHORE_*`。 */
    uint8_t step;              /**< `STARTUP_STEP_*`。 */
    char chore_key[16];        /**< 当前事项左侧文案。 */
    bool exiting;              /**< 已开始 fade/pop。 */
    bool map_opened;           /**< 已 `boot_begin_load`。 */
    bool bg_started;           /**< 黑→白结束，chrome 已露出。 */
    bool sound_wait;           /**< 黑屏等开机提示音播完。 */
} startup_page_t;

static startup_page_t * s_startup;
static lv_style_t s_boot_bg;
static bool s_boot_bg_inited;

static void startup_poll_cb(lv_timer_t * tmr);
static void startup_arm_chore(startup_page_t * ui, unsigned chore);
static void startup_stop_bg_anim(startup_page_t * ui);
static void startup_start_bg_anim(startup_page_t * ui);

static void startup_lock_black_style(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    if (!s_boot_bg_inited) {
        lv_style_init(&s_boot_bg);
        lv_style_set_bg_color(&s_boot_bg, lv_color_black());
        lv_style_set_bg_opa(&s_boot_bg, LV_OPA_COVER);
        s_boot_bg_inited = true;
    }

    lv_obj_remove_style(obj, &s_boot_bg, 0);
    lv_obj_add_style(obj, &s_boot_bg, 0);
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

static void startup_bar_to(startup_page_t * ui, uint8_t to, uint32_t ms)
{
    uint8_t from;

    if (ui == NULL) {
        return;
    }

    from = ui->splash.bar_pct;
    helm_splash_bar_linear(&ui->splash, from, to, ms);
}

static void startup_bar_goal(startup_page_t * ui, uint8_t to)
{
    if (ui == NULL || ui->bar_goal == to) {
        return;
    }

    ui->bar_goal = to;
    startup_bar_to(ui, to, 560);
}

static void startup_anim_opa(void * var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void startup_apply_bg(startup_page_t * ui, uint8_t g)
{
    if (!ui || !ui->root) {
        return;
    }
    if (ui->bg_applied == (int16_t)g) {
        return;
    }
    ui->bg_applied = (int16_t)g;

    if (g != 0 && s_boot_bg_inited) {
        lv_obj_remove_style(ui->root, &s_boot_bg, 0);
    }

    helm_splash_set_gray(&ui->splash, g);
}

static void startup_start_poll(startup_page_t * ui)
{
    if (!ui || ui->exiting || ui->poll_timer) {
        return;
    }
    ui->poll_timer = lv_timer_create(startup_poll_cb, STARTUP_POLL_MS, ui);
}

/** @brief splash 的**最终**底色灰阶（跟主题）。
 *  @details 用户 2026-09-26 晚："开关机界面也[要]受主题变化影响" ⇒ 日光是原来的
 *           白底黑字（255），夜间走黑底白字（0）。`helm_splash_set_gray()` 的约定：
 *           传进去的是**背景**灰阶，文字/进度条自动取反（`ink = 255 - g`）。
 *           logo 是固定位图（平均亮度 146，黑底上也看得见），不跟着换。 */
static uint8_t startup_theme_gray(void)
{
    return helm_pal_night() ? 0u : 255u;
}

static void startup_bg_exec(void * var, int32_t v)
{
    startup_page_t * ui = (startup_page_t *)var;
    uint8_t step;
    uint8_t g;

    if (!ui || ui->exiting) {
        return;
    }

    if (v < 0) {
        v = 0;
    }
    if (v > STARTUP_BG_STEPS) {
        v = STARTUP_BG_STEPS;
    }
    step = (uint8_t)v;
    /* 渐亮的**目标**跟主题：夜间目标是黑 ⇒ 这段动画等于不动（也就不会闪一下白）。 */
    g = (uint8_t)((step * (uint32_t)startup_theme_gray()) / STARTUP_BG_STEPS);
    startup_apply_bg(ui, g);
}

static void startup_bg_ready(lv_anim_t * a)
{
    startup_page_t * ui = a ? (startup_page_t *)a->var : NULL;

    if (!ui || ui->exiting) {
        return;
    }

    startup_apply_bg(ui, startup_theme_gray());
    ui->bg_started = true;
    ui->appear_tick = lv_tick_get();
    helm_splash_set_chrome_hidden(&ui->splash, false);
    helm_splash_bar_set(&ui->splash, 0);
    startup_arm_chore(ui, STARTUP_CHORE_SALVAGE);
    startup_start_poll(ui);
}

/** @brief 换主题后重铺 splash 底色（跟主题那件事的"变化"那一半）。
 *  @details 开机时主题是**先 KV 读回、后渐变**，但渐变可能在读回之前就结束
 *           （`startup_bg_ready` 先跑）⇒ 得有个"主题变了再铺一次"的入口。
 *           由 `helm_theme_poll_apply()` 在循环顶调用（安全点，同 ①②③）。 */
void startup_page_theme_refresh(void)
{
    startup_page_t * ui = s_startup;

    if (!ui || !ui->root || ui->exiting || !ui->bg_started) {
        return;                    /* 没在放 splash / 渐变还没走完：结束后自己会取新值 */
    }

    startup_apply_bg(ui, startup_theme_gray());
}

static void startup_play_show_anim(startup_page_t * ui)
{
    if (!ui || !ui->cont) {
        return;
    }

    /* First paint must be opaque: fade-in would leave a blank screen. */
    lv_obj_set_style_opa(ui->cont, LV_OPA_COVER, 0);
}

static void startup_start_bg_anim(startup_page_t * ui)
{
    lv_anim_t a;

    if (!ui || ui->exiting || ui->bg_started) {
        return;
    }

    startup_stop_bg_anim(ui);
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui);
    lv_anim_set_values(&a, 0, STARTUP_BG_STEPS);
    lv_anim_set_time(&a, STARTUP_BG_MS);
    lv_anim_set_delay(&a, 0);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, startup_bg_exec);
    lv_anim_set_ready_cb(&a, startup_bg_ready);
    lv_anim_start(&a);
}

static void startup_use_sys_status_font(startup_page_t * ui)
{
#ifndef CONFIG_MYVENDOR_FACTORY_MODE
    if (ui) {
        helm_splash_set_tick_font(&ui->splash,
                                  helm_font_sys(14, helm_font_lab()));
    }
#else
    (void)ui;
#endif
}

static void startup_row(startup_page_t * ui, const char * val, bool busy)
{
    if (ui == NULL) {
        return;
    }

    helm_splash_set_row(&ui->splash, ui->chore_key, val, busy);
}

static unsigned startup_chore_next(unsigned chore)
{
    unsigned n = chore + 1u;

#ifdef CONFIG_MYVENDOR_FACTORY_MODE
    if (n == STARTUP_CHORE_FONT) {
        n++;
    }
#endif
    return n;
}

static uint8_t startup_chore_bar(unsigned chore)
{
    switch (chore) {
    case STARTUP_CHORE_SALVAGE:
        return 18;
    case STARTUP_CHORE_FONT:
        return 40;
    case STARTUP_CHORE_MAP:
        return 62;
    default:
        return 100;
    }
}

/**
 * @brief 滚入一项：左侧事项名、右侧进行中、slide_in。
 */
static void startup_arm_chore(startup_page_t * ui, unsigned chore)
{
    static const char *const keys[STARTUP_CHORE_COUNT] = {
        "骑行记录", "系统字体", "地图加载"
    };
    static const char *const busy[STARTUP_CHORE_COUNT] = {
        "恢复中", "加载中", "加载中"
    };

    if (ui == NULL || chore >= STARTUP_CHORE_COUNT) {
        return;
    }

    ui->chore = (uint8_t)chore;
    ui->step = STARTUP_STEP_SLIDE_IN;
    ui->chore_t0 = lv_tick_get();
    lv_snprintf(ui->chore_key, sizeof(ui->chore_key), "%s", keys[chore]);
    startup_bar_goal(ui, startup_chore_bar(chore));
    startup_row(ui, busy[chore], true);
    helm_splash_slide_in(&ui->splash);
}

static const char *startup_salvage_result(void)
{
    unsigned fixed = bicycle_ride_gpx_salvage_fixed();
    unsigned scanned = bicycle_ride_gpx_salvage_scanned();

    if (fixed > 0u) {
        return "已恢复";
    }

    if (scanned > 0u) {
        return "已检查";
    }

    return "无记录";
}

/**
 * @brief 推进当前事项；完成则写结果行并返回 true。
 * @return 本项已出结果为 true，还需再泵为 false。
 */
static bool startup_chore_pump(startup_page_t * ui)
{
    if (ui == NULL) {
        return true;
    }

    switch (ui->chore) {
    case STARTUP_CHORE_SALVAGE:
        if (!bicycle_ride_gpx_salvage_boot_pump()) {
            return false;
        }

        startup_row(ui, startup_salvage_result(), false);
        return true;

    case STARTUP_CHORE_FONT:
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
        startup_row(ui, "已跳过", false);
        return true;
#else
        {
            int n;

            for (n = 0; n < STARTUP_FONT_CHUNKS; n++) {
                if (myvendor_system_font_preload_pump()) {
                    (void)myvendor_system_font_lv_bind();
                    startup_use_sys_status_font(ui);
                    startup_row(ui,
                                myvendor_system_font_preloaded() ? "已加载" :
                                "未找到",
                                false);
                    LV_LOG_INFO("startup: font ready, loading map");
                    return true;
                }

                myvendor_watchdog_busy_pump();
            }
        }
        return false;
#endif

    case STARTUP_CHORE_MAP:
        if (!ui->map_opened) {
            if (!map_page_boot_begin_load()) {
                LV_LOG_ERROR("startup: map boot load failed");
                startup_row(ui, "失败", false);
                return true;
            }

            ui->map_opened = true;
            startup_bar_goal(ui, 85);
            startup_row(ui, "文件中", true);
            return false;
        }

        if (!map_page_boot_prepare(live_map_page_instance())) {
            return false;
        }

        startup_bar_goal(ui, 100);
        startup_row(ui, "已加载", false);
        LV_LOG_INFO("startup: files ready, map render deferred");
        return true;

    default:
        return true;
    }
}

static void startup_play_fade_out(startup_page_t * ui)
{
    lv_anim_t a;

    if (!ui || !ui->cont) {
        return;
    }

    lv_anim_init(&a);
    lv_anim_set_var(&a, ui->cont);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, STARTUP_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, startup_anim_opa);
    lv_anim_start(&a);
}

static void startup_finish_timer_cb(lv_timer_t * tmr)
{
    (void)tmr;
    startup_page_finish();
}

static void startup_stop_bg_anim(startup_page_t * ui)
{
    if (ui) {
        lv_anim_delete(ui, startup_bg_exec);
    }
}

static void startup_begin_exit(startup_page_t * ui)
{
    if (!ui || ui->exiting) {
        return;
    }

    ui->exiting = true;
    startup_stop_bg_anim(ui);

    if (ui->poll_timer) {
        lv_timer_del(ui->poll_timer);
        ui->poll_timer = NULL;
    }

    startup_play_fade_out(ui);

    lv_timer_t * finish = lv_timer_create(startup_finish_timer_cb, STARTUP_FADE_MS, NULL);
    lv_timer_set_repeat_count(finish, 1);
    bicycle_ride_gpx_salvage_scan();
}

static void startup_poll_cb(lv_timer_t * tmr)
{
    startup_page_t * ui = (startup_page_t *)lv_timer_get_user_data(tmr);
    uint32_t elapsed;
    unsigned next;

    if (!ui || ui->exiting) {
        return;
    }

    helm_splash_pump(&ui->splash);

    elapsed = lv_tick_elaps(ui->appear_tick);

    /* Host already on USB: skip map file preparation (same LittleFS as MTP). */
    {
        myvendor_mtp_status_t mtp_st;

        memset(&mtp_st, 0, sizeof(mtp_st));
        (void)myvendor_mtp_get_status(&mtp_st);
        if (mtp_st.plug >= MYVENDOR_MTP_PLUG_ENUM) {
            startup_begin_exit(ui);
            return;
        }
    }

    /* 黑屏：开机音播完再黑→白。白屏后才跑 GPX/字体/地图。 */
    if (!ui->bg_started) {
        if (ui->sound_wait) {
            if (!myvendor_sound_idle()) {
                return;
            }

            ui->sound_wait = false;
            startup_start_bg_anim(ui);
        }

        return;
    }

    /* 白屏还在恢复 GPX 时不要因总超时直接进主界面。
     * 地图事项若被 GNSS/LFS 拖住：12 s 后进主界面，文件留给 LiveMap。 */
    if (elapsed >= STARTUP_MAX_WAIT_MS &&
        ui->chore != STARTUP_CHORE_SALVAGE) {
        if (ui->chore == STARTUP_CHORE_MAP) {
            LV_LOG_WARN("startup: map timeout, enter live map");
            startup_bar_goal(ui, 100);
            startup_row(ui, "已加载", false);
            startup_begin_exit(ui);
            return;
        }

        LV_LOG_WARN("startup: timeout, skip to map");
#ifndef CONFIG_MYVENDOR_FACTORY_MODE
        (void)myvendor_system_font_lv_bind();
        startup_use_sys_status_font(ui);
#endif
        if (!ui->map_opened) {
            (void)map_page_boot_begin_load();
            ui->map_opened = true;
        }

        lv_snprintf(ui->chore_key, sizeof(ui->chore_key), "%s", "地图加载");
        ui->chore = STARTUP_CHORE_MAP;
        ui->step = STARTUP_STEP_RUN;
        ui->chore_t0 = lv_tick_get();
        startup_bar_goal(ui, 62);
        startup_row(ui, "加载中", true);
        return;
    }

    switch (ui->step) {
    case STARTUP_STEP_SLIDE_IN:
        if (lv_tick_elaps(ui->chore_t0) < STARTUP_SLIDE_MS) {
            return;
        }

        ui->step = STARTUP_STEP_RUN;
        return;

    case STARTUP_STEP_RUN:
        if (!startup_chore_pump(ui)) {
            return;
        }

        ui->step = STARTUP_STEP_HOLD;
        ui->chore_t0 = lv_tick_get();
        return;

    case STARTUP_STEP_HOLD:
        if (lv_tick_elaps(ui->chore_t0) < STARTUP_HOLD_MS) {
            return;
        }

        ui->step = STARTUP_STEP_SLIDE_OUT;
        ui->chore_t0 = lv_tick_get();
        helm_splash_slide_out(&ui->splash);
        return;

    case STARTUP_STEP_SLIDE_OUT:
        if (lv_tick_elaps(ui->chore_t0) < STARTUP_SLIDE_MS) {
            return;
        }

        next = startup_chore_next(ui->chore);
        if (next < STARTUP_CHORE_COUNT) {
            startup_arm_chore(ui, next);
            return;
        }

        if (ui->ready_tick == 0u) {
            ui->ready_tick = lv_tick_get();
        }

        if (lv_tick_elaps(ui->ready_tick) < STARTUP_READY_HOLD_MS) {
            return;
        }

        startup_begin_exit(ui);
        return;

    default:
        return;
    }
}

static startup_page_t * startup_ui_create(lv_obj_t * root)
{
    startup_page_t * ui = lv_pm_malloc(sizeof(*ui));

    if (!ui) {
        return NULL;
    }

    memset(ui, 0, sizeof(*ui));
    ui->root = root;
    ui->cont = root;
    ui->bg_applied = -1;

    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_opa(root, LV_OPA_COVER, 0);

    helm_splash_build(&ui->splash, root);
    helm_splash_set_gray(&ui->splash, 0);
    helm_splash_set_chrome_hidden(&ui->splash, true);
    ui->bar_goal = 0xff;

    s_startup = ui;
    return ui;
}

static void startup_on_load(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    if (!page || !page->page) {
        return;
    }

    lv_obj_set_style_bg_color(page->page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page->page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page->page, 0, 0);
    startup_lock_black_style(page->page);

    if (page->user_data != NULL) {
        s_startup = (startup_page_t *)page->user_data;
        return;
    }

    page->user_data = startup_ui_create(page->page);
    s_startup = (startup_page_t *)page->user_data;
}

static void startup_will_appear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    startup_page_t * ui = page ? (startup_page_t *)page->user_data : NULL;
    if (!ui || !page || !page->page) {
        return;
    }

    ui->exiting = false;
    ui->chore = STARTUP_CHORE_SALVAGE;
    ui->step = STARTUP_STEP_SLIDE_IN;
    ui->map_opened = false;
    ui->bg_started = false;
    ui->sound_wait = true;
    ui->bg_applied = -1;
    ui->bar_goal = 0xff;
    ui->appear_tick = lv_tick_get();
    ui->ready_tick = 0;
    ui->chore_t0 = 0;
    ui->chore_key[0] = '\0';
    ui->splash.shown[0] = '\0';
    ui->splash.pending[0] = '\0';
    ui->splash.tick_on = false;
    ui->splash.out_busy = false;

    lv_obj_move_foreground(page->page);
    lv_obj_clear_flag(page->page, LV_OBJ_FLAG_HIDDEN);
    helm_splash_set_chrome_hidden(&ui->splash, true);
    helm_splash_bar_set(&ui->splash, 0);
    startup_lock_black_style(page->page);
    startup_apply_bg(ui, 0);
    startup_play_show_anim(ui);
    myvendor_sound_boot();
    startup_stop_bg_anim(ui);

    if (ui->poll_timer) {
        lv_timer_del(ui->poll_timer);
        ui->poll_timer = NULL;
    }

    if (myvendor_sound_idle()) {
        ui->sound_wait = false;
        startup_start_bg_anim(ui);
        return;
    }

    startup_start_poll(ui);
}

static void startup_will_disappear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    startup_page_t * ui = page ? (startup_page_t *)page->user_data : NULL;

    if (!ui) {
        return;
    }

    startup_stop_bg_anim(ui);
    if (ui->poll_timer) {
        lv_timer_del(ui->poll_timer);
        ui->poll_timer = NULL;
    }
}

static void startup_on_unload(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    startup_page_t * ui;

    if (!page) {
        return;
    }

    ui = (startup_page_t *)page->user_data;
    if (!ui) {
        return;
    }

    startup_stop_bg_anim(ui);
    if (ui->poll_timer) {
        lv_timer_del(ui->poll_timer);
        ui->poll_timer = NULL;
    }

    helm_splash_clear(&ui->splash);
    lv_pm_free(ui);
    page->user_data = NULL;
    s_startup = NULL;
}

/**
 * @brief 向 lv_pm 注册 Startup splash 页。
 */
void startup_page_register(void)
{
    lv_pm_page_t page = lv_pm_create_page((lv_pm_id)BICYCLE_PM_ID_STARTUP, "Startup");

    if (page == NULL) {
        LV_LOG_ERROR("startup_page: register failed");
        return;
    }

    lv_pm_set_open(page, startup_on_load);
    lv_pm_set_will_appear(page, startup_will_appear);
    lv_pm_set_will_disappear(page, startup_will_disappear);
    lv_pm_set_close(page, startup_on_unload);
    bicycle_page_anima_apply(page, BICYCLE_PM_ID_STARTUP);
}

/**
 * @brief pop Startup，露出栈底 LiveMap。不要 RESET。
 */
void startup_page_finish(void)
{
    myvendor_gnss_ui_ready();
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
    board_ble_companion_start_after_ui();
#endif
    if (lv_pm_close_page_msg(NULL) != 0) {
        LV_LOG_ERROR("startup_page: pop failed");
    }
}

/**
 * @brief 主题 apply 之后锁回黑底，避免第一帧刷出米色。
 */
void startup_page_sync_boot(void)
{
    startup_page_t * ui = s_startup;

    if (!ui || !ui->root || ui->exiting) {
        return;
    }

    startup_lock_black_style(ui->root);
    ui->bg_applied = -1;
    startup_apply_bg(ui, 0);
    helm_splash_set_chrome_hidden(&ui->splash, true);
    helm_splash_bar_set(&ui->splash, 0);
}
