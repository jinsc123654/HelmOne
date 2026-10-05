/**
 * @file lvgl_bench_main.c
 * @brief NSH 命令 lvgl_bench：LVGL 官方 benchmark 压测。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <syslog.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "demos/benchmark/lv_demo_benchmark.h"

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
#include "myvendor_lcd_disp.h"
#endif

#if LV_USE_PERF_MONITOR
#include "src/display/lv_display_private.h"
#include "src/debugging/sysmon/lv_sysmon.h"
#endif

#if LV_USE_MYVENDOR_LCD_DISP || LV_USE_NUTTX_LCD
#ifndef LVGL_BENCH_LCD_PATH
#define LVGL_BENCH_LCD_PATH "/dev/lcd0"
#endif
#ifndef LVGL_BENCH_LCD_WAIT_MS
#define LVGL_BENCH_LCD_WAIT_MS 30000
#endif
#else
#ifndef LVGL_BENCH_FBDEV_PATH
#define LVGL_BENCH_FBDEV_PATH "/dev/fb0"
#endif
#ifndef LVGL_BENCH_FBDEV_WAIT_MS
#define LVGL_BENCH_FBDEV_WAIT_MS 30000
#endif
#endif

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
/** @brief LCD flush 完成回调，通知 LVGL。 */
static void bench_lcd_flush_done(void * user_data)
{
    /* Do not lv_async_call from worker: wait_for_flushing() busy-spins. */
    lv_display_flush_ready((lv_display_t *)user_data);
}
#endif

/** @brief benchmark 结束回调，打印 CSV 摘要。 */
static void bench_on_end(const lv_demo_benchmark_summary_t * summary)
{
    /* CSV rows go to serial via LV_LOG (syslog); on-screen table needs no touch. */
    lv_demo_benchmark_summary_display(summary);
    LV_LOG_INFO("========== lvgl_bench finished ==========");
    LV_LOG_INFO("Search serial log for 'Benchmark Summary' CSV (Name, Avg. CPU, Avg. FPS, ...)");

#if LV_USE_PERF_MONITOR
    /* Summary screen is static; stop 300ms perf timer (no more sysmon spam). */
    lv_sysmon_performance_pause(lv_display_get_default());
#endif
}

#if LV_USE_PERF_MONITOR
/* Drop the display-layer sysmon label observer (LOG_MODE prints to serial).
 * Keep perf_sysmon_backend timer + subject for lv_demo_benchmark stats. */
/** @brief 隐藏屏上 sysmon 标签，保留后台计时。 */
static void bench_silence_display_sysmon(lv_display_t * disp)
{
    if (disp == NULL) {
        return;
    }

    lv_sysmon_hide_performance(disp);
    if (disp->perf_label != NULL) {
        lv_obj_delete(disp->perf_label);
        disp->perf_label = NULL;
    }
}
#endif

/** @brief 轮询等待显示设备就绪。 */
static bool wait_for_display(const char * path, int timeout_ms)
{
    const int step_ms = 100;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            close(fd);
            LV_LOG_INFO("display ready: %s (%d ms)", path, elapsed);
            return true;
        }

        usleep(step_ms * 1000);
        elapsed += step_ms;
    }

    LV_LOG_ERROR("display not ready: %s (waited %d ms, errno=%d)",
                 path, timeout_ms, errno);
    return false;
}

/**
 * @brief NSH 命令 lvgl_bench：LVGL 官方 benchmark 压测。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, char * argv[])
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;

    (void)argc;
    (void)argv;

    if (lv_is_initialized()) {
        LV_LOG_ERROR("LVGL already initialized");
        return 1;
    }

#if !LV_USE_DEMO_BENCHMARK
    LV_LOG_ERROR("Rebuild with CONFIG_LV_USE_DEMO_BENCHMARK=y");
    return 1;
#endif

    lv_init();

#if LV_USE_MYVENDOR_LCD_DISP || LV_USE_NUTTX_LCD
    if (!wait_for_display(LVGL_BENCH_LCD_PATH, LVGL_BENCH_LCD_WAIT_MS)) {
        return 1;
    }

    lv_nuttx_dsc_init(&info);
    info.fb_path = LVGL_BENCH_LCD_PATH;
#else
    if (!wait_for_display(LVGL_BENCH_FBDEV_PATH, LVGL_BENCH_FBDEV_WAIT_MS)) {
        return 1;
    }

    lv_nuttx_dsc_init(&info);
    info.fb_path = LVGL_BENCH_FBDEV_PATH;
#endif

    lv_nuttx_init(&info, &result);

    if (result.disp == NULL) {
        syslog(LOG_ERR, "lvgl_bench: lv_nuttx_init failed (disp=NULL)");
        LV_LOG_ERROR("lv_nuttx_init failed");
        return 1;
    }

#if defined(CONFIG_MYVENDOR_LCD_DISP) && CONFIG_MYVENDOR_LCD_DISP
    if (myvendor_lcd_disp_register_done_cb(bench_lcd_flush_done, result.disp) < 0) {
        syslog(LOG_ERR, "lvgl_bench: register_done_cb failed");
        LV_LOG_ERROR("myvendor_lcd_disp_register_done_cb failed");
        return 1;
    }
#endif

#if LV_USE_PERF_MONITOR
    bench_silence_display_sysmon(result.disp);
#endif

    syslog(LOG_INFO, "lvgl_bench: starting benchmark (EPIC=%d)",
           (int)LV_USE_SIFLI_EPIC);
    LV_LOG_INFO("Starting lv_demo_benchmark (EPIC=%d, PERF_MONITOR=%d)",
                (int)LV_USE_SIFLI_EPIC,
                (int)LV_USE_PERF_MONITOR);

    lv_demo_benchmark_set_end_cb(bench_on_end);
    lv_demo_benchmark();

    LV_LOG_INFO("Benchmark running (~90s). No touch needed; CSV prints to serial when done.");

    while (1) {
        uint32_t idle = lv_timer_handler();
        usleep(idle ? idle * 1000 : 1000);
    }

    /* not reached */
}
