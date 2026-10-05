/**
 * @file startup_page.h
 * @brief 开机 splash 页：与 2SFBL 同一套 logo，事项滚入后再露出 LiveMap。
 *
 * 由 `lvgl_page_open_boot_sequence()` 压在静默 LiveMap 空壳之上。
 * 事项与关机同款 `事项  ·  状态`，细节见 `doc/splash.md`。
 */

#ifndef STARTUP_PAGE_H
#define STARTUP_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 向 lv_pm 注册 Startup 页（`lvgl_page_init` 调用）。
 */
void startup_page_register(void);

/**
 * @brief 结束 splash：pop 本页，露出栈底已 cache 的 LiveMap。
 * @note 用 close/pop，不要 `open LiveMap + RESET`。
 */
void startup_page_finish(void);

/**
 * @brief 主题 apply 之后再锁回黑底，避免第一帧刷出米色。
 * @details `bicycle_c_main` 在打开 boot 序列后、允许 invalidation 前调用。
 */
void startup_page_sync_boot(void);

/**
 * @brief 换主题后重铺开机 splash 的底色（日=白底黑字 / 夜=黑底白字）。
 * @details 由 `helm_theme_poll_apply()` 在 UI 循环顶调用；没在放 splash 时是空操作。
 */
void startup_page_theme_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* STARTUP_PAGE_H */
