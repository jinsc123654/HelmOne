/**
 * @file lv_port_buttons.h
 * @brief LVGL 物理按键移植层（轮询 /dev/buttons）。
 *
 * KEY1=PA30、KEY2=PA33 走页面回调。PWR=PA34 全局：单击调亮度，
 * 长按 2s 关机确认。硬件长按约 10s 复位，软件不管。
 */

#ifndef LV_PORT_BUTTONS_H
#define LV_PORT_BUTTONS_H

#include "lvgl/lvgl.h"
#include <syslog.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 临时关掉 LVGL [stall] 刷屏；改成 1 再打开。按键 btn: 日志不受影响。 */
#ifndef MYVENDOR_LVGL_STALL_LOG
#  define MYVENDOR_LVGL_STALL_LOG 0
#endif
#if MYVENDOR_LVGL_STALL_LOG
#  define LVGL_STALL(fmt, ...) syslog(LOG_NOTICE, "[stall] " fmt, ##__VA_ARGS__)
#else
#  define LVGL_STALL(...) ((void)0)
#endif

/** @brief 注册按键轮询定时器；读取 /dev/buttons。 */
void lv_port_buttons_init(void);

typedef void (*lv_port_page_btn_cb_t)(void *user_data);

/** @brief KEY1（PA30）短按：主界面上一页 / 列表下一项。 */
void lv_port_buttons_set_page_scroll_cb(lv_port_page_btn_cb_t cb, void *user_data);

/**
 * @brief KEY1（PA30）**双击**回调：列表**向上**一个聚焦（用户 2026-09-27）。
 * @note 注册后该页的单击会**延后一个双击窗口**（350ms）再执行 —— 否则"单击下一个 +
 *       双击上一个"会互相抵消。离开该页时记得用 NULL 注销，免得把延迟带给别的页。
 */
void lv_port_buttons_set_page_scroll_double_cb(lv_port_page_btn_cb_t cb, void *user_data);

/** @brief KEY2（PA33）短按：主界面下一页 / 确定。 */
void lv_port_buttons_set_page_confirm_cb(lv_port_page_btn_cb_t cb, void *user_data);

/** @brief 可选遗留单击/双击钩子（LiveMap 未使用）。 */
typedef void (*lv_port_page_click_cb_t)(int clicks, void *user_data);

typedef void (*lv_port_page_longpress_cb_t)(void *user_data);

/**
 * @brief 设置页面单击/双击回调。
 * @param cb 回调；clicks 为 1 或 2。
 * @param user_data 用户数据。
 */
void lv_port_buttons_set_page_click_cb(lv_port_page_click_cb_t cb, void *user_data);

/**
 * @brief KEY1 长按（菜单 / 列表返回）。
 */
void lv_port_buttons_set_page_longpress_cb(lv_port_page_longpress_cb_t cb,
    void *user_data);

/**
 * @brief KEY2 长按（开始 / 暂停骑行）。
 */
void lv_port_buttons_set_page_longpress2_cb(lv_port_page_longpress_cb_t cb,
    void *user_data);

/**
 * @brief KEY2 长按后松开。未注册时松开不触发短按。
 */
void lv_port_buttons_set_page_longpress2_up_cb(lv_port_page_longpress_cb_t cb,
    void *user_data);

/**
 * @brief 清除全部页面级按键回调。
 */
void lv_port_buttons_clear_page_callbacks(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_BUTTONS_H */
