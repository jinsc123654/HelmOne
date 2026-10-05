/**
 * @file lv_pm_port.h
 * @brief lv_pm 页面管理 — port。
 */

#ifndef LV_PM_PORT_H
#define LV_PM_PORT_H
#ifdef __cplusplus
extern "C" {
#endif
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
#include <stdint.h>
#include "lv_pm_core.h"
#include "lv_pm_anima.h"
#include "lv_pm_theme.h"


// 设置页面打开时的回调函数
/**
 * @brief lv_pm set open。
 * @return 0 成功，负 errno 失败。
 */
int8_t lv_pm_set_open(void* page, lv_pm_lifecycle fun);

// 设置页面即将显示时的回调函数
/**
 * @brief lv_pm set will appear。
 */
int8_t lv_pm_set_will_appear(void* page, lv_pm_lifecycle fun);

// 设置页面已经完全显示时的回调函数
/**
 * @brief lv_pm set dis appear。
 * @return 请求的值。
 */
int8_t lv_pm_set_dis_appear(void* page, lv_pm_lifecycle fun);

// 设置页面即将隐藏时的回调函数
/**
 * @brief lv_pm set will disappear。
 */
int8_t lv_pm_set_will_disappear(void* page, lv_pm_lifecycle fun);

// 设置页面已经完全隐藏不可见时的回调函数
/**
 * @brief lv_pm set dis disappear。
 * @return 请求的值。
 */
int8_t lv_pm_set_dis_disappear(void* page, lv_pm_lifecycle fun);

// 设置页面关闭时的回调函数
/**
 * @brief lv_pm set close。
 */
int8_t lv_pm_set_close(void* page, lv_pm_lifecycle fun);

/* 设置界面的打开动画 */
int8_t lv_pm_set_open_options(void* page, const void* lv_pm_anima_cb, lv_pm_target target, uint32_t direction, uint32_t time);

// 添加一个可聚焦对象到页面
/**
 * @brief lv_pm add focus obj。
 */
int8_t lv_pm_add_focus_obj(void* page, lv_obj_t* obj);

// 设置页面打开后聚焦到指定对象
/**
 * @brief lv_pm set focus obj。
 */
int8_t lv_pm_set_focus_obj(void* page, lv_obj_t* obj);

// 设置是否启用顶部状态栏
/**
 * @brief lv_pm set top bar。
 */
int8_t lv_pm_set_top_bar(void* page, bool Enable);

// 设置是否启用返回栏
/**
 * @brief lv_pm set back bar。
 */
int8_t lv_pm_set_back_bar(void* page, bool Enable);

/** Pop/close 时保留 lv_obj 与 user_data（码表 LiveMap 常驻） */
int8_t lv_pm_set_cache_enable(void * page, bool enable);

/**
 * Read navigation payload in lifecycle callbacks (open/will_appear/close…).
 * Returns NULL after close_cb completes or when LV_PM_MSG_NONE was passed.
 */
void * lv_pm_page_get_msg(void * page);

// 创建页面定时器
/**
 * @brief page_timer_create 接口。
 */
void page_timer_create(void* page, lv_timer_cb_t timer_xcb, uint32_t period, void* user_data);

// 删除页面定时器
/**
 * @brief page_timer_del 接口。
 */
void page_timer_del(void* page);

// 设置页面定时器是否在后台运行
/**
 * @brief page_timer_backend 接口。
 */
void page_timer_backend(void* page, bool en);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
