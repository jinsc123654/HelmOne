/**
 * @file lvgl_page.h
 * @brief Bicycle LVGL 入口：注册页面、开机栈、主题/语言与 MTP 叠层。
 *
 * 开机走 `lvgl_page_open_boot_sequence()`（LiveMap 空壳 + Startup splash），
 * 不要单独 `open_startup` 再 RESET 到 LiveMap。细节见 `doc/splash.md`。
 */
#ifndef LVGL_PAGE_H
#define LVGL_PAGE_H

#include "bicycle_page_ids.h"
#include "live_map/map_page.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief lv_pm 初始化、注册全部页面、应用屏幕主题；显示就绪后调用。 */
void lvgl_page_init(void);

/**
 * @brief 只打开 Startup（调试用）。
 * @note 产品开机请用 `lvgl_page_open_boot_sequence()`。
 */
int lvgl_page_open_startup(void);

/**
 * @brief 先静默 push LiveMap 空壳，再 push Startup splash。
 * @details vmap / TTF 在白屏事项里分片加载；结束用 pop，不要 RESET。
 */
int lvgl_page_open_boot_sequence(void);

/** @brief 打开（或复用 cache 的）LiveMap。开机序列内部也会调用。 */
int lvgl_page_open_home(void);

/**
 * @brief 当前 LiveMap 实例。
 * @return 尚未创建则为 NULL。
 */
map_page_t * lvgl_page_map(void);

/**
 * @brief 栈顶页面 ID。
 * @return 无页面时为 `0xff`。
 */
bicycle_pm_id_t lvgl_page_current_id(void);
/** @brief 栈顶是否为 UsbTransfer。 */
bool lvgl_page_is_usb_transfer(void);
/** @brief 转场动画尚未结束。 */
bool lvgl_page_nav_busy(void);

/** @brief 当前主题名（classic / outdoor 等）。 */
const char * lvgl_page_theme_name(void);
/**
 * @brief 按名字切主题。须在 LVGL 线程调用。
 */
bool lvgl_page_theme_set_by_name(const char * name);
/** @brief 循环主题。 */
bool lvgl_page_theme_cycle(void);

/** @brief UI locale（zh_CN / en）；除非经 bicycle_ui_ctl，须在 LVGL 线程调用。 */
const char * lvgl_page_locale_name(void);
/**
 * @brief 按名字切 locale。
 */
bool lvgl_page_locale_set_by_name(const char * name);
/** @brief 循环 locale。 */
bool lvgl_page_locale_cycle(void);

/**
 * @brief push UsbTransfer overlay（MTP 传输页）。
 */
int lvgl_page_push_usb_transfer(void);
/**
 * @brief pop UsbTransfer，露出底下 LiveMap。
 */
int lvgl_page_pop_usb_transfer(void);

/**
 * @brief 标记应用已过开机 splash（事件总线 / 状态栏用）。
 */
void lvgl_page_set_app_started(bool started);
/** @brief splash 已结束、主界面可交互。 */
bool lvgl_page_app_started(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PAGE_H */
