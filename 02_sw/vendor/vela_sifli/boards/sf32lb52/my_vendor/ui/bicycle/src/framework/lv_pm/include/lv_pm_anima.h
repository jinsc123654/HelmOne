/**
 * @file lv_pm_anima.h
 * @brief lv_pm 页面管理 — anima。
 */

#ifndef LVLG_PM_ANIMA_H
#define LVLG_PM_ANIMA_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
#include "../include/lv_pm_core.h"

typedef void (*lv_pm_anima_complete_cb)(lv_pm_page_t pm_page, lv_pm_open_options_t*);

typedef struct
{
    bool is_free; /* 资源是否被释放的标志 */
    lv_pm_page_t pm_page;
    lv_pm_open_options_t* open_options;
    lv_pm_anima_complete_cb cb;
} lv_pm_anima_data;

/** @brief 进入/退出成对：appear 与 dis_appear 互为逻辑逆操作。 */
typedef enum {
    LV_PM_ANIMA_ENTER = 0,
    LV_PM_ANIMA_EXIT,
} lv_pm_anima_phase_t;

typedef struct
{
    void (*lv_pm_appear)(lv_pm_anima_data *, lv_anim_t *);     /* enter */
    void (*lv_pm_dis_appear)(lv_pm_anima_data *, lv_anim_t *); /* exit (inverse of appear) */
} lv_pm_anima_t;

/**
 * @brief 播放页面已注册 lv_pm_anima_t 动画对的一个阶段。
 * @param route_opts 可选；仅转发给 complete_cb（如 push 时目标页 target）。
 */
void lv_pm_anima_play(lv_pm_page_t pm_page, lv_pm_anima_phase_t phase,
                      lv_pm_open_options_t * route_opts, lv_pm_anima_complete_cb cb);

/** @brief lv_pm_anima_play 的薄封装（ENTER / EXIT）。 */
void lv_pm_anima_open_cb(lv_pm_page_t pm_page, lv_pm_open_options_t * route_opts,
                         lv_pm_anima_complete_cb cb);
void lv_pm_anima_back_cb(lv_pm_page_t pm_page, lv_pm_open_options_t * route_opts,
                         lv_pm_anima_complete_cb cb);
/** @brief 同步结束进行中的 appear/disappear 段（生命周期 + 清理）。 */
void lv_pm_anima_drain_all(void);
/**
 * @brief lv_pm nav anim begin。
 */
void lv_pm_nav_anim_begin(void);
/**
 * @brief lv_pm nav anim end。
 */
void lv_pm_nav_anim_end(void);
/**
 * @brief lv_pm nav set busy。
 */
void lv_pm_nav_set_busy(bool busy);
/* 注册回调时会用到的参数 */
lv_anim_deleted_cb_t get_lv_pm_deleted_cb(void);



/* 基础已经支持的动画 */
extern const lv_pm_anima_t lv_pm_no_anima;
extern const lv_pm_anima_t lv_pm_slide_anima;
extern const lv_pm_anima_t lv_pm_popup_anima;
extern const lv_pm_anima_t lv_pm_fade_anima;
#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
