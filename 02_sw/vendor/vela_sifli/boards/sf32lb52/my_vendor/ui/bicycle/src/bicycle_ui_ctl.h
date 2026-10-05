/**
 * @file bicycle_ui_ctl.h
 * @brief 自行车 UI — ui_ctl。
 */

#ifndef BICYCLE_UI_CTL_H
#define BICYCLE_UI_CTL_H

#ifdef __cplusplus
extern "C" {
#endif

struct map_page;

/** @brief 标记 UI 就绪以接受跨任务 ctl 投递；bicycle_pm_open_map 后调用一次。 */
void bicycle_ui_ctl_ready(void);
/**
 * @brief 自行车 ui ctl shutdown。
 */
void bicycle_ui_ctl_shutdown(void);

/**
 * @brief 抽空 ctl 队列；关机/静止时丢掉待弹的 App 横幅。
 * LVGL 线程、`lv_timer_handler()` 之前调用。
 */
void bicycle_ui_ctl_poll(struct map_page * page);

/** @brief 将当前地图样式发布到 ctl 中间件（供 NSH style 列表）。 */
void bicycle_ui_ctl_sync_state(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_UI_CTL_H */
