/**
 * @file myvendor_bicycle_ctl.h
 * @brief 自行车 UI 控制中间件：跨任务命令队列 + UI 状态快照。
 *
 * 生产者（NSH、工作线程等）只调 myvendor_bicycle_ctl_post()，不得碰 LVGL / vmap。
 * bicycle_ui（LVGL 线程）用 bicycle_ui_ctl_poll() 取队列再改样式 / 重绘。
 *
 * 扁平 NuttX：链进 nuttx 的任务都能看见板级全局量。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_BICYCLE_CTL_H
#define MYVENDOR_BICYCLE_CTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYVENDOR_BICYCLE_CTL_ARG_MAX 512  /**< 命令参数字符串上限（含 NUL）。 */

/**
 * @brief UI 命令操作码。新动作在此追加。
 */
typedef enum {
    MYVENDOR_BICYCLE_CTL_OP_NONE = 0,        /**< 空。 */
    MYVENDOR_BICYCLE_CTL_OP_STYLE_SET,       /**< arg：样式名，如 "nav"。 */
    MYVENDOR_BICYCLE_CTL_OP_STYLE_CYCLE,     /**< 忽略 arg。 */
    MYVENDOR_BICYCLE_CTL_OP_UI_REDRAW,       /**< 忽略 arg：全屏 chrome+地图刷新。 */
    MYVENDOR_BICYCLE_CTL_OP_LOCALE_SET,      /**< arg：locale，如 "en"。 */
    MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW,     /**< arg："title|body" 或 "title|body|iconpath"。 */
    MYVENDOR_BICYCLE_CTL_OP_BOTTOM_SHOW,     /**< arg：底部 toast 文本。 */
    MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN,        /**< arg："lon,lat" 或 "flon,flat,tlon,tlat"。 */
    MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN_SIM,    /**< 同 NAV_PLAN；按 nav_sim_speed 跑路线。 */
    MYVENDOR_BICYCLE_CTL_OP_NAV_TRIP,        /**< arg："lon,lat;lon,lat;..." 途经点。 */
    MYVENDOR_BICYCLE_CTL_OP_NAV_STOP,        /**< 忽略 arg。 */
    MYVENDOR_BICYCLE_CTL_OP_NAV_SIM_SPEED,   /**< arg：km/h 字符串，如 "120"。 */
    MYVENDOR_BICYCLE_CTL_OP_MAP_ZOOM,        /**< arg："in"|"out"|"reset"|"z,N"|"s,F"。 */
    MYVENDOR_BICYCLE_CTL_OP_RIDE_START,      /**< 开始记录；套用挂起导航。 */
    MYVENDOR_BICYCLE_CTL_OP_RIDE_STOP,       /**< 停止记录与导航。 */
    MYVENDOR_BICYCLE_CTL_OP_SENSOR_SET,      /**< arg："hr,cad,pwr" 或 "off"。 */
    MYVENDOR_BICYCLE_CTL_OP_GNSS_SIM,        /**< arg："stop" 或 "fwd|path[|kph][|skip_m]"。 */
    MYVENDOR_BICYCLE_CTL_OP_IDLE_ENTER,      /**< 立刻进静止休眠（等同 10 分钟计时耗尽）。 */
    MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE,       /**< 退出静止休眠。 */
    MYVENDOR_BICYCLE_CTL_OP_IDLE_HOUR,       /**< 立刻耗尽静止 1 小时并关机。 */
    MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY,       /**< arg："on" 本次开机禁止自动静止；"off" 恢复。 */
    MYVENDOR_BICYCLE_CTL_OP_DEMO,            /**< arg："on"|"off"|"toggle" 演示模式（四步循环）。 */
} myvendor_bicycle_ctl_op_t;

/**
 * @brief 队列里的一条命令。
 */
typedef struct {
    uint8_t op;  /**< #myvendor_bicycle_ctl_op_t。 */
    char arg[MYVENDOR_BICYCLE_CTL_ARG_MAX];  /**< 操作参数。 */
} myvendor_bicycle_ctl_msg_t;

/**
 * @brief bicycle_ui 发布的只读状态（任意任务可查）。
 */
typedef enum {
    MYVENDOR_BICYCLE_CTL_STATE_MAP_STYLE = 0,  /**< 当前地图样式名。 */
    MYVENDOR_BICYCLE_CTL_STATE_UI_LOCALE,      /**< 当前 locale。 */
    MYVENDOR_BICYCLE_CTL_STATE_MAP_ZOOM,       /**< 当前缩放描述。 */
    MYVENDOR_BICYCLE_CTL_STATE_DEMO,           /**< 演示模式："off" 或当前那一步的文案。 */
    MYVENDOR_BICYCLE_CTL_STATE_COUNT,          /**< 状态槽个数。 */
} myvendor_bicycle_ctl_state_id_t;

/**
 * @brief UI 线程标记自己是否已就绪（仅 bicycle_ui）。
 */
void myvendor_bicycle_ctl_ui_ready(bool ready);

/**
 * @brief UI 线程是否仍在跑。
 */
bool myvendor_bicycle_ctl_ui_alive(void);

/**
 * @brief 任意任务投递一条 UI 命令。
 * @param op 操作码。
 * @param arg 参数，可为 NULL。
 * @return 0 成功；-EINVAL 参数非法；-ENODEV UI 未就绪；-ENOSPC 队列满。
 */
int myvendor_bicycle_ctl_post(myvendor_bicycle_ctl_op_t op, const char * arg);

/**
 * @brief 队列是否非空（仅 LVGL 线程消费侧）。
 */
bool myvendor_bicycle_ctl_pending(void);

/**
 * @brief 队列中未取走的条数。
 */
unsigned myvendor_bicycle_ctl_pending_count(void);

/**
 * @brief 弹出一条。队列空则返回 false。
 * @param[out] msg 输出。
 */
bool myvendor_bicycle_ctl_take(myvendor_bicycle_ctl_msg_t * msg);

/**
 * @brief UI 线程写入状态槽。
 */
void myvendor_bicycle_ctl_state_set(myvendor_bicycle_ctl_state_id_t id,
                                    const char * value);

/**
 * @brief 任意任务读取状态槽。
 * @param id 槽。
 * @param[out] out 输出缓冲。
 * @param out_sz 缓冲字节。
 * @return 拷贝成功则为 true。
 */
bool myvendor_bicycle_ctl_state_get(myvendor_bicycle_ctl_state_id_t id,
                                    char * out, size_t out_sz);

/**
 * @brief 操作码英文名（NSH 打印）。
 */
const char * myvendor_bicycle_ctl_op_name(myvendor_bicycle_ctl_op_t op);

/**
 * @brief 样式名表长度（无 LVGL；真正换肤在 LVGL 线程）。
 */
unsigned myvendor_bicycle_ctl_style_name_count(void);

/**
 * @brief 按下标取样式名。
 */
const char * myvendor_bicycle_ctl_style_name(unsigned index);

/**
 * @brief 样式名是否在表中。
 */
bool myvendor_bicycle_ctl_style_name_valid(const char * name);

/**
 * @brief locale 名表长度（与 lv_pm_builtin_locales 保持一致）。
 */
unsigned myvendor_bicycle_ctl_locale_name_count(void);

/**
 * @brief 按下标取 locale 名。
 */
const char * myvendor_bicycle_ctl_locale_name(unsigned index);

/**
 * @brief locale 名是否在表中。
 */
bool myvendor_bicycle_ctl_locale_name_valid(const char * name);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_BICYCLE_CTL_H */
