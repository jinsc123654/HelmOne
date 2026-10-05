/**
 * @file bicycle_c_main.c
 * @brief 自行车 UI — c_main。
 */

#include <nuttx/config.h>

#include "bicycle_mtp_ui.h"
#include "bicycle_ride_gpx.h"
#include "bicycle_runtime.h"
#include "bicycle_ui_ctl.h"
#include "lvgl_page.h"
#include "lv_pm_theme.h"
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
#include "factory/factory_page.h"
#endif
#include "usb_transfer/usb_transfer_page.h"
#include "startup/startup_page.h"
#include "myvendor_bicycle_ctl.h"
#include "myvendor_gnss.h"
#include "myvendor_identity.h"
#include "myvendor_mtp.h"
#include "lvgl/lvgl.h"
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
#include "bicycle_inval_probe.h"
#endif
#if defined(CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT) && CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT
#include "bicycle_handler_stat.h"
#endif
#include "bicycle_c_debug.h"
#include "bicycle_demo.h"
#include "myvendor_lcd_disp.h"
#include "myvendor_watchdog.h"
#include "helm_palette.h"
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
void board_ble_companion_start_after_ui(void);
#endif
#include "sf32lb_dvfs.h"
#include "Vendor/Board/lv_port/lv_port_buttons.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <syslog.h>
#include <unistd.h>

#ifndef BICYCLE_LCD_PATH
#define BICYCLE_LCD_PATH "/dev/lcd0"
#endif

#ifndef BICYCLE_LCD_WAIT_MS
#define BICYCLE_LCD_WAIT_MS 30000
#endif

#ifndef BICYCLE_LOOP_SLEEP_MS
#define BICYCLE_LOOP_SLEEP_MS 10u
#endif

/* LVGL 日志的出口**不在这里**（2026-09-25 修正）：
 * `lv_nuttx_init()` 内部会 `lv_log_register_print_cb(syslog_print)`，把这里注册过的
 * 回调**覆盖掉** —— 所以 UI 层的日志实际由 LVGL 栈自己的回调落到 syslog
 * （`apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_entry.c` 的 `syslog_print()`，
 * 输出形如 `[LVGL] [User] ( ts, +dt) func: msg file:line`）。
 * 那边原来把 `LV_LOG_LEVEL_USER` 映射成 `LOG_CRIT`（整屏红色），已修成 `LOG_INFO`。
 * 所以这里**不再注册第二个回调**：一套机制、且 `transfer_ui` / `lvgl_bench` 同样受益。 */

static bool wait_for_lcddev(const char * path, int timeout_ms)
{
    const int step_ms = 100;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            close(fd);
            LV_LOG_INFO("lcd ready: %s (%d ms)", path, elapsed);
            return true;
        }
        usleep(step_ms * 1000);
        elapsed += step_ms;
    }

    LV_LOG_ERROR("lcd not ready: %s (waited %d ms, errno=%d)",
        path, timeout_ms, errno);
    return false;
}

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
static void bicycle_lcd_flush_done(void * user_data)
{
    myvendor_lcd_disp_stats_note_flush_ready();
    lv_display_flush_ready((lv_display_t *)user_data);
}
#endif

/**
 * @brief main 接口。
 */
int main(int argc, char * argv[])
{
    (void)argc;
    (void)argv;

    map_page_t * page;
    bicycle_mtp_ui_t mtp_ui;

    if (lv_is_initialized()) {
        LV_LOG_ERROR("LVGL already initialized");
        return -1;
    }

    lv_init();

    if (!wait_for_lcddev(BICYCLE_LCD_PATH, BICYCLE_LCD_WAIT_MS)) {
        return 1;
    }

    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;
    lv_nuttx_dsc_init(&info);
    info.fb_path = BICYCLE_LCD_PATH;
    lv_nuttx_init(&info, &result);

    if (result.disp == NULL) {
        LV_LOG_ERROR("display init failed");
        return 1;
    }

    {
        lv_obj_t * scr = lv_display_get_screen_active(result.disp);
        lv_timer_t * refr;

        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        /* Keep 2SFBL pixels on the panel until splash is ready. */
        lv_display_enable_invalidation(result.disp, false);
        refr = lv_display_get_refr_timer(result.disp);
        if (refr) {
            lv_timer_pause(refr);
        }
    }

    lv_display_set_antialiasing(result.disp, true);

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
    if (myvendor_lcd_disp_register_done_cb(bicycle_lcd_flush_done, result.disp) < 0) {
        LV_LOG_ERROR("lcd flush callback failed");
        return 1;
    }
#endif

#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
    bicycle_inval_probe_init(result.disp);
#endif

#if defined(CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT) && CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT
    bicycle_handler_stat_init();
#endif
    bicycle_c_debug_init();

    lv_port_buttons_init();

    lvgl_page_init();

    if (lvgl_page_open_boot_sequence() != 0) {
        LV_LOG_ERROR("lvgl_page_open_boot_sequence failed");
        return 1;
    }

    if (!myvendor_is_factory()) {
        startup_page_sync_boot();
    }

    /* Splash / factory page use compiled-in helm fonts. Paint before TTF / MTP / vmap. */
    lv_display_enable_invalidation(result.disp, true);
    {
        lv_timer_t * refr = lv_display_get_refr_timer(result.disp);
        lv_obj_t * scr = lv_display_get_screen_active(result.disp);

        if (refr) {
            lv_timer_resume(refr);
        }
        lv_obj_invalidate(scr);
        lv_obj_update_layout(scr);
    }
    lv_refr_now(result.disp);

    page = lvgl_page_map();
    if (myvendor_is_factory()) {
        LV_LOG_INFO("bicycle: factory page (bitmap fonts, no TTF)");
    } else if (!page) {
        LV_LOG_INFO("bicycle: splash active, LiveMap loads under Startup");
    }

    myvendor_mtp_owner_reset();
    if (myvendor_mtp_init() != 0) {
        LV_LOG_WARN("MTP service init failed (MTP disabled?)");
    } else if (myvendor_mtp_transfer_begin() != 0) {
        LV_LOG_WARN("MTP transfer_begin deferred until host ENUM");
    }

    bicycle_mtp_ui_init(&mtp_ui);
    if (myvendor_is_factory()) {
        bicycle_ride_gpx_salvage_dir();
    }
    lvgl_page_set_app_started(true);

    LV_LOG_INFO(myvendor_is_factory()
                ? "bicycle started (factory page + MTP + BLE)"
                : "bicycle started (lvgl_page + vmap + MTP)");
    bicycle_ui_ctl_ready();
    bicycle_ui_ctl_sync_state();
    if (myvendor_is_factory()) {
        myvendor_gnss_ui_ready();
#ifdef CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART
        board_ble_companion_start_after_ui();
#endif
    }
    sf32lb_dvfs_hold(SF32LB_DVFS_HOLD_AWAKE);
    sf32lb_dvfs_release(SF32LB_DVFS_HOLD_BOOT);

#ifndef CONFIG_DISABLE_PTHREAD
    pthread_setname_np(pthread_self(), "bicycle_ui");
#endif

    while (1) {
        myvendor_watchdog_ui_beat();
#if defined(CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT) && CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT
        bicycle_handler_stat_begin();
#endif
#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
        myvendor_lcd_disp_poll_recover_ui();
#endif
        if (myvendor_is_factory()) {
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
            factory_page_poll_active();
#endif
        } else {
            bicycle_mtp_ui_run(&mtp_ui);
            if (lvgl_page_is_usb_transfer()) {
                usb_transfer_page_poll_active();
            }
        }
        page = lvgl_page_map();
        bicycle_ui_ctl_poll(page);
        /* 演示模式：四步顺序推进（GPX 导航 → 坐标点导航 → 全国跳点 → 换主题）。
         * 与 ctl 同一个上下文（循环顶、LVGL 刷新之外），未开时是空操作。 */
        bicycle_demo_poll();
        /* 传感器快照 → runtime：**主骑行页也要看得到 HR/踏频**（以前只有地图页
         * 的 render pump 在同步，停在主界面时值一直是 0）。 */
        bicycle_runtime_sync_companion_sensors();
        {
#if MYVENDOR_LVGL_STALL_LOG
            uint32_t t0 = lv_tick_get();
#endif
            uint32_t idle = lv_timer_handler();
#if MYVENDOR_LVGL_STALL_LOG
            uint32_t busy = lv_tick_elaps(t0);

            if (busy >= 50u) {
                LVGL_STALL("lv_timer_handler %ums idle_ret=%u",
                    (unsigned)busy, (unsigned)idle);
            }
#endif

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
            myvendor_lcd_disp_poll_recover_ui();
#endif

            /* 换主题的**重活**放这里 —— 在 `lv_timer_handler()` **之后**，也就是本帧
             * 已经画完之后（同样在"LVGL refresh 之外"，安全性与循环顶等价，见
             * myvendor_lcd_disp.c 的 recover 注释）。
             * 为什么不在循环顶：循环顶做的任何事都会**推迟本帧的刷新** ⇒ 用户点一下
             * 「主题」要等重活跑完屏幕才动（现场像"要点两次"）。挪到帧后，可见变化
             * 只等"按键回调里的行重画"，① 屏底 ② 页头 ③ 主界面 由这一步补上（晚一帧）。
             *   · helm_theme_poll_apply()：写 KV + ①② + 登记 ③ 重建
             *   · lv_pm_theme_poll_styles()：页面级主题应用 + LVGL 整树样式重算（1.2~1.5 s） */
            helm_theme_poll_apply();
            lv_pm_theme_poll_styles();
#if defined(CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT) && CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT
            bicycle_handler_stat_after_handler(idle, BICYCLE_LOOP_SLEEP_MS);
            bicycle_handler_stat_usleep_begin();
#endif
            bicycle_c_debug_loop(idle);
        }
        if (page && !lvgl_page_is_usb_transfer()) {
            map_page_debug_snap(page);
        }
        {
            uint32_t sleep_ms = BICYCLE_LOOP_SLEEP_MS;

            if (myvendor_bicycle_ctl_pending()) {
                sleep_ms = 0;
            }

            usleep((useconds_t)sleep_ms * 1000u);
        }
#if defined(CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT) && CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT
        bicycle_handler_stat_usleep_end();
#endif
    }

    myvendor_mtp_deinit();
    lv_nuttx_deinit(&result);
    lv_deinit();
    return 0;
}
