/**
 * @file myvendor_bicycle_ctl.c
 * @brief 自行车 UI 控制中间件实现：互斥保护的环形命令队列 + UI 状态快照。
 *
 * 生产者（NSH、传感器线程等）通过 myvendor_bicycle_ctl_post() 投递命令；
 * bicycle_ui（LVGL 线程）通过 myvendor_bicycle_ctl_take() 消费并执行。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#if defined(CONFIG_MYVENDOR_BICYCLE) && CONFIG_MYVENDOR_BICYCLE

#include "myvendor_bicycle_ctl.h"
#include "board_malloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#ifndef MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH
#define MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH 8
#endif

#define MYVENDOR_BICYCLE_CTL_STATE_MAX 16

/** 地图样式名表（与 lv_pm 主题名对应）。 */
static const char * const g_map_style_names[] = {
    "classic",
    "outdoor",
    /* 用户界面上「显示」那一行走的就是这一档（UI 现在是两档：classic ↔ night）。
     * 缺了它 ⇒ `bicycle_nsh style night` 会被校验拒掉，控制台测不到用户实际那套。 */
    "night",
};

/** 支持的 locale 名表。 */
static const char * const g_locale_names[] = {
    "zh_CN",
    "en",
};

/** 全局控制块：UI 存活标志、队列与状态槽。命令队列在 BoardPSRAM。 */
struct myvendor_bicycle_ctl_s {
    atomic_bool ui_alive;
    pthread_mutex_t lock;
    myvendor_bicycle_ctl_msg_t *queue;
    unsigned q_head;
    unsigned q_tail;
    unsigned q_count;
    char state[MYVENDOR_BICYCLE_CTL_STATE_COUNT][MYVENDOR_BICYCLE_CTL_STATE_MAX];
};

/** 单例控制块实例。 */
static struct myvendor_bicycle_ctl_s g_bicycle_ctl = {
    .ui_alive = ATOMIC_VAR_INIT(false),
};

/**
 * @brief 惰性初始化互斥锁（首次 post/take 前调用）。
 */
static void myvendor_bicycle_ctl_init_once(void)
{
    static bool inited;

    if (inited) {
        return;
    }

    pthread_mutex_init(&g_bicycle_ctl.lock, NULL);
    g_bicycle_ctl.queue = board_malloc_psram(
        sizeof(myvendor_bicycle_ctl_msg_t) * MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH);
    if (g_bicycle_ctl.queue != NULL) {
        memset(g_bicycle_ctl.queue,
               0,
               sizeof(myvendor_bicycle_ctl_msg_t) * MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH);
    }

    inited = true;
}

/**
 * @brief 确保命令队列已从 BoardPSRAM 申请。
 * @return 队列指针；失败 NULL。
 */
static myvendor_bicycle_ctl_msg_t *myvendor_bicycle_ctl_queue(void)
{
    if (g_bicycle_ctl.queue == NULL) {
        g_bicycle_ctl.queue = board_malloc_psram(
            sizeof(myvendor_bicycle_ctl_msg_t) * MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH);
        if (g_bicycle_ctl.queue != NULL) {
            memset(g_bicycle_ctl.queue,
                   0,
                   sizeof(myvendor_bicycle_ctl_msg_t) *
                       MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH);
        }
    }

    return g_bicycle_ctl.queue;
}

/**
 * @brief 将操作码转为调试用英文字符串。
 * @param op 操作码。
 * @return 名称字符串；未知 op 返回 "none"。
 */
const char * myvendor_bicycle_ctl_op_name(myvendor_bicycle_ctl_op_t op)
{
    switch (op) {
    case MYVENDOR_BICYCLE_CTL_OP_STYLE_SET:
        return "style_set";
    case MYVENDOR_BICYCLE_CTL_OP_STYLE_CYCLE:
        return "style_cycle";
    case MYVENDOR_BICYCLE_CTL_OP_UI_REDRAW:
        return "ui_redraw";
    case MYVENDOR_BICYCLE_CTL_OP_LOCALE_SET:
        return "locale_set";
    case MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW:
        return "notify_show";
    case MYVENDOR_BICYCLE_CTL_OP_BOTTOM_SHOW:
        return "bottom_show";
    case MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN:
        return "nav_plan";
    case MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN_SIM:
        return "nav_plan_sim";
    case MYVENDOR_BICYCLE_CTL_OP_NAV_TRIP:
        return "nav_trip";
    case MYVENDOR_BICYCLE_CTL_OP_NAV_STOP:
        return "nav_stop";
    case MYVENDOR_BICYCLE_CTL_OP_NAV_SIM_SPEED:
        return "nav_sim_speed";
    case MYVENDOR_BICYCLE_CTL_OP_MAP_ZOOM:
        return "map_zoom";
    case MYVENDOR_BICYCLE_CTL_OP_RIDE_START:
        return "ride_start";
    case MYVENDOR_BICYCLE_CTL_OP_RIDE_STOP:
        return "ride_stop";
    case MYVENDOR_BICYCLE_CTL_OP_SENSOR_SET:
        return "sensor_set";
    case MYVENDOR_BICYCLE_CTL_OP_GNSS_SIM:
        return "gnss_sim";
    case MYVENDOR_BICYCLE_CTL_OP_IDLE_ENTER:
        return "idle_enter";
    case MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE:
        return "idle_wake";
    case MYVENDOR_BICYCLE_CTL_OP_IDLE_HOUR:
        return "idle_hour";
    case MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY:
        return "idle_stay";
    case MYVENDOR_BICYCLE_CTL_OP_DEMO:
        return "demo";
    default:
        return "none";
    }
}

/**
 * @brief UI 线程标记就绪/退出；退出时清空队列与状态。
 * @param ready true 表示 bicycle_ui 已启动；false 表示已退出。
 */
void myvendor_bicycle_ctl_ui_ready(bool ready)
{
    myvendor_bicycle_ctl_init_once();

    atomic_store_explicit(&g_bicycle_ctl.ui_alive, ready, memory_order_release);

    if (!ready) {
        pthread_mutex_lock(&g_bicycle_ctl.lock);
        g_bicycle_ctl.q_head = 0;
        g_bicycle_ctl.q_tail = 0;
        g_bicycle_ctl.q_count = 0;
        memset(g_bicycle_ctl.state, 0, sizeof(g_bicycle_ctl.state));
        pthread_mutex_unlock(&g_bicycle_ctl.lock);
    }
}

/**
 * @brief 查询 UI 线程是否标记为存活。
 * @return true 表示可接受 post。
 */
bool myvendor_bicycle_ctl_ui_alive(void)
{
    myvendor_bicycle_ctl_init_once();
    return atomic_load_explicit(&g_bicycle_ctl.ui_alive, memory_order_acquire);
}

/**
 * @brief 安全拷贝命令参数字符串。
 * @param dst 目标缓冲（MYVENDOR_BICYCLE_CTL_ARG_MAX）。
 * @param arg 源字符串；NULL 或空串写入空串。
 */
static void myvendor_bicycle_ctl_copy_arg(char * dst, const char * arg)
{
    if (arg == NULL || arg[0] == '\0') {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, arg, MYVENDOR_BICYCLE_CTL_ARG_MAX - 1u);
    dst[MYVENDOR_BICYCLE_CTL_ARG_MAX - 1u] = '\0';
}

/**
 * @brief 向 UI 命令队列追加一条消息。
 * @param op 操作码。
 * @param arg 参数字符串；可为 NULL。
 * @return 0 成功；-EINVAL 非法 op；-ENODEV UI 未就绪；-ENOMEM 队列未就绪；-ENOSPC 队列满。
 */
int myvendor_bicycle_ctl_post(myvendor_bicycle_ctl_op_t op, const char * arg)
{
    myvendor_bicycle_ctl_msg_t msg;

    myvendor_bicycle_ctl_init_once();

    if (op <= MYVENDOR_BICYCLE_CTL_OP_NONE ||
        op > MYVENDOR_BICYCLE_CTL_OP_DEMO) {
        return -EINVAL;
    }

    if (!myvendor_bicycle_ctl_ui_alive()) {
        return -ENODEV;
    }

    if (myvendor_bicycle_ctl_queue() == NULL) {
        return -ENOMEM;
    }

    memset(&msg, 0, sizeof(msg));
    msg.op = (uint8_t)op;
    myvendor_bicycle_ctl_copy_arg(msg.arg, arg);

    pthread_mutex_lock(&g_bicycle_ctl.lock);

    if (g_bicycle_ctl.q_count >= MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH) {
        pthread_mutex_unlock(&g_bicycle_ctl.lock);
        return -ENOSPC;
    }

    g_bicycle_ctl.queue[g_bicycle_ctl.q_tail] = msg;
    g_bicycle_ctl.q_tail =
        (g_bicycle_ctl.q_tail + 1u) % MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH;
    g_bicycle_ctl.q_count++;

    pthread_mutex_unlock(&g_bicycle_ctl.lock);
    return 0;
}

/**
 * @brief 队列中尚未取走的命令条数。
 * @return 待处理条数。
 */
unsigned myvendor_bicycle_ctl_pending_count(void)
{
    unsigned count;

    myvendor_bicycle_ctl_init_once();

    pthread_mutex_lock(&g_bicycle_ctl.lock);
    count = g_bicycle_ctl.q_count;
    pthread_mutex_unlock(&g_bicycle_ctl.lock);

    return count;
}

/**
 * @brief 队列是否非空。
 * @return true 有待处理命令。
 */
bool myvendor_bicycle_ctl_pending(void)
{
    return myvendor_bicycle_ctl_pending_count() > 0;
}

/**
 * @brief 弹出队首命令（LVGL 线程消费侧）。
 * @param[out] msg 输出消息。
 * @return true 成功取到；false 队列空或 msg 为 NULL。
 */
bool myvendor_bicycle_ctl_take(myvendor_bicycle_ctl_msg_t * msg)
{
    bool have = false;

    myvendor_bicycle_ctl_init_once();

    if (msg == NULL || myvendor_bicycle_ctl_queue() == NULL) {
        return false;
    }

    pthread_mutex_lock(&g_bicycle_ctl.lock);

    if (g_bicycle_ctl.q_count > 0) {
        *msg = g_bicycle_ctl.queue[g_bicycle_ctl.q_head];
        g_bicycle_ctl.q_head =
            (g_bicycle_ctl.q_head + 1u) % MYVENDOR_BICYCLE_CTL_QUEUE_DEPTH;
        g_bicycle_ctl.q_count--;
        have = true;
    }

    pthread_mutex_unlock(&g_bicycle_ctl.lock);
    return have;
}

/**
 * @brief UI 线程写入只读状态槽。
 * @param id 状态槽 ID。
 * @param value 字符串；NULL 或空串清空该槽。
 */
void myvendor_bicycle_ctl_state_set(myvendor_bicycle_ctl_state_id_t id,
                                    const char * value)
{
    myvendor_bicycle_ctl_init_once();

    if (id >= MYVENDOR_BICYCLE_CTL_STATE_COUNT) {
        return;
    }

    pthread_mutex_lock(&g_bicycle_ctl.lock);

    if (value == NULL || value[0] == '\0') {
        g_bicycle_ctl.state[id][0] = '\0';
    } else {
        strncpy(g_bicycle_ctl.state[id], value,
                MYVENDOR_BICYCLE_CTL_STATE_MAX - 1u);
        g_bicycle_ctl.state[id][MYVENDOR_BICYCLE_CTL_STATE_MAX - 1u] = '\0';
    }

    pthread_mutex_unlock(&g_bicycle_ctl.lock);
}

/**
 * @brief 任意任务读取状态槽（UI 须存活）。
 * @param id 状态槽 ID。
 * @param[out] out 输出缓冲；可为 NULL（仅检查是否有值）。
 * @param out_sz 缓冲字节数。
 * @return 槽内非空则为 true。
 */
bool myvendor_bicycle_ctl_state_get(myvendor_bicycle_ctl_state_id_t id,
                                    char * out, size_t out_sz)
{
    bool ok;

    myvendor_bicycle_ctl_init_once();

    if (id >= MYVENDOR_BICYCLE_CTL_STATE_COUNT || !myvendor_bicycle_ctl_ui_alive()) {
        return false;
    }

    pthread_mutex_lock(&g_bicycle_ctl.lock);

    if (out != NULL && out_sz > 0) {
        strncpy(out, g_bicycle_ctl.state[id], out_sz - 1u);
        out[out_sz - 1u] = '\0';
    }

    ok = g_bicycle_ctl.state[id][0] != '\0';
    pthread_mutex_unlock(&g_bicycle_ctl.lock);

    return ok;
}

/**
 * @brief 地图样式名表长度。
 * @return 条目数。
 */
unsigned myvendor_bicycle_ctl_style_name_count(void)
{
    return (unsigned)(sizeof(g_map_style_names) / sizeof(g_map_style_names[0]));
}

/**
 * @brief 按下标取地图样式名。
 * @param index 下标。
 * @return 名称；越界返回空串。
 */
const char * myvendor_bicycle_ctl_style_name(unsigned index)
{
    if (index >= myvendor_bicycle_ctl_style_name_count()) {
        return "";
    }

    return g_map_style_names[index];
}

/**
 * @brief 检查样式名是否在支持列表中。
 * @param name 样式名。
 * @return true 有效。
 */
bool myvendor_bicycle_ctl_style_name_valid(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (unsigned i = 0; i < myvendor_bicycle_ctl_style_name_count(); i++) {
        if (strcmp(g_map_style_names[i], name) == 0) {
            return true;
        }
    }

    return false;
}

/**
 * @brief locale 名表长度。
 * @return 条目数。
 */
unsigned myvendor_bicycle_ctl_locale_name_count(void)
{
    return (unsigned)(sizeof(g_locale_names) / sizeof(g_locale_names[0]));
}

/**
 * @brief 按下标取 locale 名。
 * @param index 下标。
 * @return 名称；越界返回空串。
 */
const char * myvendor_bicycle_ctl_locale_name(unsigned index)
{
    if (index >= myvendor_bicycle_ctl_locale_name_count()) {
        return "";
    }

    return g_locale_names[index];
}

/**
 * @brief 检查 locale 名是否在支持列表中。
 * @param name locale 名。
 * @return true 有效。
 */
bool myvendor_bicycle_ctl_locale_name_valid(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (unsigned i = 0; i < myvendor_bicycle_ctl_locale_name_count(); i++) {
        if (strcmp(g_locale_names[i], name) == 0) {
            return true;
        }
    }

    return false;
}

#endif /* CONFIG_MYVENDOR_BICYCLE */
