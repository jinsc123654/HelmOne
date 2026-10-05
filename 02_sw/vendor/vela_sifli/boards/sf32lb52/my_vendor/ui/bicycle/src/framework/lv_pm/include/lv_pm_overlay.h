/**
 * @file lv_pm_overlay.h
 * @brief lv_pm 页面管理 — overlay。
 */

#ifndef LV_PM_OVERLAY_H
#define LV_PM_OVERLAY_H

#include "lv_pm_config.h"
#include "lv_pm_theme_def.h"

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef LV_PM_NOTIFY_HOLD
#define LV_PM_NOTIFY_HOLD UINT32_MAX
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if LV_PM_USE_OVERLAY

/**
 * @brief lv_pm overlay init。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_overlay_init(lv_obj_t * parent);
/**
 * @brief lv_pm overlay apply theme。
 */
void lv_pm_overlay_apply_theme(const lv_pm_theme_def_t * def);

/** @brief 手机式顶部横幅：从状态栏下方滑入并自动消失；title 可为 NULL。 */
bool lv_pm_notify_show(const char * title, const char * text, uint32_t duration_ms);
/** @brief 同 lv_pm_notify_show，可选 POSIX 图标路径（PNG）；icon_path 可为 NULL。 */
bool lv_pm_notify_show_ex(const char * title, const char * text,
                          const char * icon_path, uint32_t duration_ms);
/**
 * @brief 同 lv_pm_notify_show，但左侧画一个**内置图标**（系统通知用）。
 *
 * @details App 通知的图标是手机下发的 PNG（走 `_ex` 的 icon_path）；系统自己发的通知
 *          （亮度、低电、骑行恢复…）没有 PNG，就用本接口的内置图标 —— 两者占**同一个
 *          槽**（左侧 32px + 文字左对齐），观感一致。
 * @param ico `helm_ico_id_t` 取值（0 = 无图标），见 lvgl_page/helm_icon.h。
 */
bool lv_pm_notify_show_icon(const char * title, const char * text, int ico,
                            uint32_t duration_ms);
/** @brief 同 lv_pm_notify_show 且 LV_PM_NOTIFY_HOLD — 须 lv_pm_notify_dismiss() 关闭。 */
static inline bool lv_pm_notify_show_hold(const char * title, const char * text)
{
/**
 * @brief lv_pm notify show。
 */
    return lv_pm_notify_show(title, text, LV_PM_NOTIFY_HOLD);
}
/**
 * @brief lv_pm notify dismiss。
 */
void lv_pm_notify_dismiss(void);
/** @brief 顶栏横幅是否正在显示（含自动关闭倒计时）。 */
bool lv_pm_notify_is_showing(void);

/** @brief Android 式底部系统 toast：从底边滑入。 */
bool lv_pm_bottom_show(const char * text, uint32_t duration_ms);
/**
 * @brief lv_pm bottom dismiss。
 */
void lv_pm_bottom_dismiss(void);

#else

/**
 * @brief lv_pm overlay init。
 * @return 0 成功，负 errno 失败。
 */
static inline void lv_pm_overlay_init(lv_obj_t * parent) { LV_UNUSED(parent); }
static inline void lv_pm_overlay_apply_theme(const lv_pm_theme_def_t * def) { LV_UNUSED(def); }
static inline bool lv_pm_notify_show(const char * title, const char * text, uint32_t duration_ms)
{
    LV_UNUSED(title);
    LV_UNUSED(text);
    LV_UNUSED(duration_ms);
    return false;
}
static inline bool lv_pm_notify_show_ex(const char * title, const char * text,
                                        const char * icon_path, uint32_t duration_ms)
{
    LV_UNUSED(title);
    LV_UNUSED(text);
    LV_UNUSED(icon_path);
    LV_UNUSED(duration_ms);
    return false;
}
static inline void lv_pm_notify_dismiss(void) {}
static inline bool lv_pm_notify_is_showing(void) { return false; }
static inline bool lv_pm_bottom_show(const char * text, uint32_t duration_ms)
{
    LV_UNUSED(text);
    LV_UNUSED(duration_ms);
    return false;
}
static inline void lv_pm_bottom_dismiss(void) {}

#endif /* LV_PM_USE_OVERLAY */

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_OVERLAY_H */
