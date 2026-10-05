/**
 * @file lv_port_buttons.c
 * @brief lv_port_buttons 模块。
 */

#include "lv_port_buttons.h"

#include <nuttx/config.h>

#if defined(__NuttX__) && defined(CONFIG_INPUT_BUTTONS)

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/input/buttons.h>

#include "lvgl/src/widgets/label/lv_label.h"
#include "lvgl/src/widgets/msgbox/lv_msgbox.h"
#include "helm_idle.h"
#include "helm_pwr.h"
#include "myvendor_sound.h"
#include "sf32lb_dvfs.h"

#define LV_PORT_BTN_PATH      "/dev/buttons"
#define LV_PORT_BTN_CLICK     (1u << 0) /* KEY2 PA33 */
#define LV_PORT_BTN_SCROLL    (1u << 1) /* KEY1 PA30 */
#define LV_PORT_BTN_PWR       (1u << 2) /* PWR  PA34，按下为高 */
#define LV_PORT_SCROLL_STEP       48
#define LV_PORT_POLL_MS           20
#define LV_PORT_DOUBLE_CLICK_MS   350
#define LV_PORT_LONG_PRESS_MS     600
#define LV_PORT_PWR_LONG_MS       2000
#define LV_PORT_FOCUS_MAX         8

#define BTN_LOG(fmt, ...) syslog(LOG_NOTICE, "btn: " fmt, ##__VA_ARGS__)

typedef struct {
    int fd;
    btn_buttonset_t last;
    lv_display_t *disp;
    lv_indev_t *pointer;
    lv_obj_t *focus_items[LV_PORT_FOCUS_MAX];
    lv_obj_t *focused;
    uint32_t focus_count;
    int focus_index;
    bool focus_ready;
    bool had_msgbox;
    lv_port_page_btn_cb_t page_scroll_cb;
    void *page_scroll_ud;
    /** KEY1 双击回调（注册了才启用双击：单击要延后一个双击窗口，见 lv_port_scroll_down） */
    lv_port_page_btn_cb_t page_scroll_double_cb;
    void *page_scroll_double_ud;
    lv_port_page_btn_cb_t page_confirm_cb;
    void *page_confirm_ud;
    lv_port_page_click_cb_t page_click_cb;
    void *page_click_ud;
    lv_port_page_longpress_cb_t page_longpress_cb;
    void *page_longpress_ud;
    lv_port_page_longpress_cb_t page_longpress2_cb;
    void *page_longpress2_ud;
    lv_port_page_longpress_cb_t page_longpress2_up_cb;
    void *page_longpress2_up_ud;
    lv_timer_t *pending_single_click;
    uint32_t last_click_release_tick;
    lv_timer_t *pending_scroll_single;
    uint32_t last_scroll_release_tick;
    uint32_t scroll_press_tick;
    bool scroll_long_fired;
    uint32_t click_press_tick;
    bool click_long_fired;
    uint32_t pwr_press_tick;
    bool pwr_long_fired;
    bool idle_swallow;
} lv_port_btn_ctx_t;

static lv_port_btn_ctx_t s_ctx;
#if MYVENDOR_LVGL_STALL_LOG
static uint32_t s_btn_last_poll_tick;
#endif

static btn_buttonset_t lv_port_btn_read(void)
{
    btn_buttonset_t set = 0;
    ssize_t n;

    if (s_ctx.fd < 0) {
        return 0;
    }

    n = read(s_ctx.fd, &set, sizeof(set));
    if (n != (ssize_t)sizeof(set)) {
        return s_ctx.last;
    }

    return set;
}

static lv_obj_t *lv_port_find_msgbox_in_tree(lv_obj_t *root)
{
    uint32_t i;
    uint32_t cnt;
    lv_obj_t *found;

    if (root == NULL || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }

    if (lv_obj_check_type(root, &lv_msgbox_class)) {
        return root;
    }

    cnt = lv_obj_get_child_count(root);
    for (i = 0; i < cnt; i++) {
        found = lv_port_find_msgbox_in_tree(lv_obj_get_child(root, i));
        if (found != NULL) {
            return found;
        }
    }

    return NULL;
}

static lv_obj_t *lv_port_get_active_msgbox(void)
{
    lv_obj_t *top;

    if (s_ctx.disp == NULL) {
        return NULL;
    }

    top = lv_display_get_layer_top(s_ctx.disp);
    return lv_port_find_msgbox_in_tree(top);
}

static bool lv_port_is_focus_candidate(lv_obj_t *obj)
{
    if (obj == NULL) {
        return false;
    }

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }

    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        return false;
    }

    if (lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        return false;
    }

    if (lv_obj_check_type(obj, &lv_label_class)) {
        return false;
    }

    return true;
}

static bool lv_port_has_focus_candidate_child(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t cnt;

    cnt = lv_obj_get_child_count(obj);
    for (i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        if (lv_port_is_focus_candidate(child) &&
            !lv_port_has_focus_candidate_child(child)) {
            return true;
        }

        if (lv_port_has_focus_candidate_child(child)) {
            return true;
        }
    }

    return false;
}

static bool lv_port_is_nav_button(lv_obj_t *obj)
{
    const char *page_id;

    if (!lv_port_is_focus_candidate(obj) || lv_port_has_focus_candidate_child(obj)) {
        return false;
    }

    page_id = (const char *)lv_obj_get_user_data(obj);
    if (page_id == NULL || page_id[0] == '\0') {
        return false;
    }

    /* DashboardView::btnCreate stores a page name string in user_data. */
    for (const char *p = page_id; *p != '\0'; p++) {
        if (!isalnum((unsigned char)*p)) {
            return false;
        }
    }

    return true;
}

static void lv_port_collect_nav_buttons(lv_obj_t *root, uint32_t *n)
{
    uint32_t i;
    uint32_t cnt;

    if (root == NULL || *n >= LV_PORT_FOCUS_MAX) {
        return;
    }

    if (lv_port_is_nav_button(root)) {
        s_ctx.focus_items[(*n)++] = root;
        return;
    }

    cnt = lv_obj_get_child_count(root);
    for (i = 0; i < cnt; i++) {
        lv_port_collect_nav_buttons(lv_obj_get_child(root, i), n);
    }
}

static bool lv_port_focus_still_valid(void)
{
    lv_obj_t *scr = lv_screen_active();
    uint32_t i;

    if (s_ctx.focused == NULL || scr == NULL) {
        return false;
    }

    for (i = 0; i < s_ctx.focus_count; i++) {
        if (s_ctx.focus_items[i] == s_ctx.focused) {
            return true;
        }
    }

    LV_UNUSED(scr);
    return false;
}

static void lv_port_focus_apply(int index)
{
    if (s_ctx.focused != NULL) {
        lv_obj_clear_state(s_ctx.focused, LV_STATE_FOCUSED);
    }

    if (s_ctx.focus_count == 0) {
        s_ctx.focused = NULL;
        s_ctx.focus_index = -1;
        s_ctx.focus_ready = false;
        return;
    }

    if (index < 0) {
        index = 0;
    }
    if ((uint32_t)index >= s_ctx.focus_count) {
        index = (int)s_ctx.focus_count - 1;
    }

    s_ctx.focus_index = index;
    s_ctx.focused = s_ctx.focus_items[index];
    lv_obj_add_state(s_ctx.focused, LV_STATE_FOCUSED);
    s_ctx.focus_ready = true;
}

static void lv_port_focus_sync(void)
{
    lv_obj_t *scr;
    lv_obj_t *prev = s_ctx.focused;
    uint32_t n = 0;
    uint32_t i;
    int keep_index = -1;

    scr = lv_screen_active();
    if (scr == NULL) {
        s_ctx.focus_count = 0;
        s_ctx.focus_ready = false;
        s_ctx.focused = NULL;
        return;
    }

    lv_port_collect_nav_buttons(scr, &n);
    s_ctx.focus_count = n;

    if (prev != NULL) {
        for (i = 0; i < n; i++) {
            if (s_ctx.focus_items[i] == prev) {
                keep_index = (int)i;
                break;
            }
        }
    }

    if (n == 0) {
        if (prev != NULL) {
            lv_obj_clear_state(prev, LV_STATE_FOCUSED);
        }
        s_ctx.focused = NULL;
        s_ctx.focus_index = -1;
        s_ctx.focus_ready = false;
        return;
    }

    if (keep_index < 0) {
        keep_index = 0;
    }

    lv_port_focus_apply(keep_index);
}

static void lv_port_focus_prepare(void)
{
    if (!s_ctx.focus_ready || !lv_port_focus_still_valid()) {
        lv_port_focus_sync();
    }
}

static void lv_port_focus_next(void)
{
    int next;

    lv_port_focus_prepare();
    if (s_ctx.focus_count == 0) {
        return;
    }

    next = s_ctx.focus_index + 1;
    if ((uint32_t)next >= s_ctx.focus_count) {
        next = 0;
    }

    lv_port_focus_apply(next);
    LV_LOG_INFO("btn: focus next");
}

static lv_obj_t *lv_port_msgbox_confirm_button(lv_obj_t *mbox)
{
    lv_obj_t *footer;
    uint32_t cnt;

    if (mbox == NULL) {
        return NULL;
    }

    footer = lv_msgbox_get_footer(mbox);
    if (footer == NULL) {
        return NULL;
    }

    cnt = lv_obj_get_child_count(footer);
    if (cnt == 0) {
        return NULL;
    }

    return lv_obj_get_child(footer, cnt - 1);
}

static void lv_port_send_click(lv_obj_t *obj)
{
    if (obj == NULL || !lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        return;
    }

    lv_obj_send_event(obj, LV_EVENT_PRESSED, s_ctx.pointer);
    lv_obj_send_event(obj, LV_EVENT_RELEASED, s_ctx.pointer);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, s_ctx.pointer);
}

static void lv_port_cancel_pending_single_click(void)
{
    if (s_ctx.pending_single_click != NULL) {
        lv_timer_delete(s_ctx.pending_single_click);
        s_ctx.pending_single_click = NULL;
    }
}

static void lv_port_pending_single_click_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    s_ctx.pending_single_click = NULL;
    if (s_ctx.page_click_cb != NULL) {
        s_ctx.page_click_cb(1, s_ctx.page_click_ud);
        LV_LOG_INFO("btn: page single click");
    }
}

static void lv_port_click_confirm_page(void)
{
    uint32_t now = lv_tick_get();

    if (s_ctx.last_click_release_tick != 0 &&
        lv_tick_elaps(s_ctx.last_click_release_tick) < LV_PORT_DOUBLE_CLICK_MS) {
        lv_port_cancel_pending_single_click();
        s_ctx.page_click_cb(2, s_ctx.page_click_ud);
        s_ctx.last_click_release_tick = 0;
        LV_LOG_INFO("btn: page double click");
        return;
    }

    s_ctx.last_click_release_tick = now;
    lv_port_cancel_pending_single_click();
    s_ctx.pending_single_click = lv_timer_create(
        lv_port_pending_single_click_cb, LV_PORT_DOUBLE_CLICK_MS, NULL);
    lv_timer_set_repeat_count(s_ctx.pending_single_click, 1);
}

static void lv_port_click_confirm(void)
{
    lv_obj_t *mbox;
    lv_obj_t *btn;

    if (helm_pwr_dialog_open()) {
        helm_pwr_dialog_confirm();
        BTN_LOG("KEY2 short -> poweroff confirm");
        return;
    }

    mbox = lv_port_get_active_msgbox();
    if (mbox != NULL) {
        btn = lv_port_msgbox_confirm_button(mbox);
        if (btn != NULL) {
            lv_port_send_click(btn);
            BTN_LOG("KEY2 short -> msgbox ok");
        }
        return;
    }

    if (s_ctx.page_click_cb != NULL) {
        lv_port_click_confirm_page();
        return;
    }

    lv_port_focus_prepare();
    if (s_ctx.page_confirm_cb != NULL) {
        uint32_t t0 = lv_tick_get();

        s_ctx.page_confirm_cb(s_ctx.page_confirm_ud);
        BTN_LOG("KEY2 short -> page confirm %ums", (unsigned)lv_tick_elaps(t0));
        return;
    }

    BTN_LOG("KEY2 short, no confirm cb");

    if (s_ctx.focused != NULL) {
        lv_port_send_click(s_ctx.focused);
        LV_LOG_INFO("btn: nav click");
    }
}

/* 前向声明：实现在 lv_port_scroll_down() 之后（它俩要回调它，避免再来一次声明）。 */
static void lv_port_cancel_pending_scroll_single(void);
static void lv_port_pending_scroll_single_cb(lv_timer_t *timer);
/** @brief KEY1 短按的**真正动作**（不含双击判定）—— 延后执行也走它，避免重入判定。 */
static void lv_port_scroll_do(void)
{
    if (s_ctx.page_scroll_cb != NULL) {
        uint32_t t0 = lv_tick_get();

        s_ctx.page_scroll_cb(s_ctx.page_scroll_ud);
        BTN_LOG("KEY1 short -> page scroll %ums", (unsigned)lv_tick_elaps(t0));
        return;
    }

    BTN_LOG("KEY1 short, no scroll cb");
    lv_port_focus_next();
}

static void lv_port_scroll_down(void)
{
    lv_obj_t *mbox;

    if (helm_pwr_dialog_open()) {
        helm_pwr_dialog_cancel();
        BTN_LOG("KEY1 short -> poweroff cancel");
        return;
    }

    mbox = lv_port_get_active_msgbox();
    if (mbox != NULL) {
        lv_msgbox_close(mbox);
        BTN_LOG("KEY1 short -> msgbox close");
        return;
    }

    /*
     * KEY1 双击（只有页面注册了 double 回调才启用，目前是菜单主页）：**向上一个聚焦**。
     * ⚠ 单击必须**延后一个双击窗口**再执行 —— 否则"第一次单击已经走了一步 + 双击再走回去"
     * 正好抵消，双击看上去什么也没发生（KEY2 的单击/双击也是这么处理的，见
     * lv_port_click_confirm_page()）。没有注册 double 回调的页面保持原样：立即执行、零延迟。
     */
    if (s_ctx.page_scroll_double_cb != NULL) {
        const uint32_t now = lv_tick_get();

        if (s_ctx.last_scroll_release_tick != 0 &&
            lv_tick_elaps(s_ctx.last_scroll_release_tick) < LV_PORT_DOUBLE_CLICK_MS) {
            lv_port_cancel_pending_scroll_single();
            s_ctx.last_scroll_release_tick = 0;
            s_ctx.page_scroll_double_cb(s_ctx.page_scroll_double_ud);
            BTN_LOG("KEY1 double -> page prev");
            return;
        }

        s_ctx.last_scroll_release_tick = now;
        lv_port_cancel_pending_scroll_single();
        s_ctx.pending_scroll_single = lv_timer_create(
            lv_port_pending_scroll_single_cb, LV_PORT_DOUBLE_CLICK_MS, NULL);
        lv_timer_set_repeat_count(s_ctx.pending_scroll_single, 1);
        return;
    }

    lv_port_scroll_do();
}

static void lv_port_cancel_pending_scroll_single(void)
{
    if (s_ctx.pending_scroll_single != NULL) {
        lv_timer_delete(s_ctx.pending_scroll_single);
        s_ctx.pending_scroll_single = NULL;
    }
}

static void lv_port_pending_scroll_single_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    s_ctx.pending_scroll_single = NULL;
    /* ⚠ 必须调**动作**而不是再次 lv_port_scroll_down()：后者会再走一遍双击判定 ⇒
     * `last_scroll_release_tick` 刚被自己刷新过 ⇒ 永远判成"还在窗口内" ⇒ 无限延后、
     * 单击永远不执行（用户 2026-09-27 实测："只能触发双击了，无论间隔多久"）。 */
    lv_port_scroll_do();
    LV_LOG_INFO("btn: KEY1 single (after double window)");
}

static void lv_port_scroll_hold_update(void)
{
    if (s_ctx.page_longpress_cb == NULL) {
        return;
    }

    if (helm_pwr_dialog_open() || lv_port_get_active_msgbox() != NULL) {
        return;
    }

    if (s_ctx.scroll_long_fired) {
        return;
    }

    if (lv_tick_elaps(s_ctx.scroll_press_tick) >= LV_PORT_LONG_PRESS_MS) {
        s_ctx.scroll_long_fired = true;
        s_ctx.page_longpress_cb(s_ctx.page_longpress_ud);
        BTN_LOG("KEY1 long");
    }
}

static void lv_port_click_hold_update(void)
{
    if (s_ctx.page_longpress2_cb == NULL) {
        return;
    }

    if (helm_pwr_dialog_open() || lv_port_get_active_msgbox() != NULL) {
        return;
    }

    if (s_ctx.click_long_fired) {
        return;
    }

    if (lv_tick_elaps(s_ctx.click_press_tick) >= LV_PORT_LONG_PRESS_MS) {
        s_ctx.click_long_fired = true;
        s_ctx.page_longpress2_cb(s_ctx.page_longpress2_ud);
        BTN_LOG("KEY2 long");
    }
}

static void lv_port_pwr_hold_update(void)
{
    if (s_ctx.pwr_long_fired) {
        return;
    }

    if (lv_tick_elaps(s_ctx.pwr_press_tick) >= LV_PORT_PWR_LONG_MS) {
        s_ctx.pwr_long_fired = true;
        helm_pwr_on_long();
        BTN_LOG("PWR long 2s");
    }
}

static void lv_port_btn_timer(lv_timer_t *timer)
{
    btn_buttonset_t set;
    lv_obj_t *mbox;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t now;
    uint32_t gap;
#endif

    LV_UNUSED(timer);

    if (s_ctx.fd < 0) {
        return;
    }

    if (helm_pwr_is_offing()) {
        s_ctx.last = lv_port_btn_read();
        return;
    }

#if MYVENDOR_LVGL_STALL_LOG
    now = lv_tick_get();
    gap = s_btn_last_poll_tick ? lv_tick_elaps(s_btn_last_poll_tick) : 0;
    s_btn_last_poll_tick = now;
    if (gap > 80u) {
        LVGL_STALL("btn timer gap %ums scroll_cb=%d confirm_cb=%d",
            (unsigned)gap, s_ctx.page_scroll_cb != NULL,
            s_ctx.page_confirm_cb != NULL);
    }
#endif

    set = lv_port_btn_read();
    mbox = lv_port_get_active_msgbox();

    /* CPU load is a 1-2 second history and cannot predict the first frame
     * after input.  Raise the short interactive floor on both edges so a
     * long press release also has a fresh window for its UI callback. */
    if (((set ^ s_ctx.last) &
         (LV_PORT_BTN_SCROLL | LV_PORT_BTN_CLICK | LV_PORT_BTN_PWR)) != 0) {
        sf32lb_dvfs_interact();
    }

    if (helm_idle_is_sleeping()) {
        if ((set & ~s_ctx.last) != 0) {
            helm_idle_wake();
            s_ctx.idle_swallow = true;
        }

        if (s_ctx.idle_swallow) {
            if (set == 0) {
                s_ctx.idle_swallow = false;
            }

            s_ctx.last = set;
            return;
        }
    } else {
        s_ctx.idle_swallow = false;
    }

    if (s_ctx.had_msgbox && mbox == NULL) {
        s_ctx.focus_ready = false;
    }
    s_ctx.had_msgbox = (mbox != NULL);

    if ((set & LV_PORT_BTN_SCROLL) && !(s_ctx.last & LV_PORT_BTN_SCROLL)) {
        s_ctx.scroll_press_tick = lv_tick_get();
        s_ctx.scroll_long_fired = false;
        myvendor_sound_key();
        BTN_LOG("KEY1 down set=0x%02x scroll_cb=%d", (unsigned)set,
            s_ctx.page_scroll_cb != NULL);
    }

    if ((set & LV_PORT_BTN_SCROLL) && (s_ctx.last & LV_PORT_BTN_SCROLL)) {
        lv_port_scroll_hold_update();
    }

    if (!(set & LV_PORT_BTN_SCROLL) && (s_ctx.last & LV_PORT_BTN_SCROLL)) {
        BTN_LOG("KEY1 up%s set=0x%02x held=%ums",
                s_ctx.scroll_long_fired ? " after-long" : "",
                (unsigned)set,
                (unsigned)lv_tick_elaps(s_ctx.scroll_press_tick));
        if (!s_ctx.scroll_long_fired) {
            lv_port_scroll_down();
        }
        s_ctx.scroll_long_fired = false;
    }

    if ((set & LV_PORT_BTN_CLICK) && !(s_ctx.last & LV_PORT_BTN_CLICK)) {
        s_ctx.click_press_tick = lv_tick_get();
        s_ctx.click_long_fired = false;
        myvendor_sound_ok();
        BTN_LOG("KEY2 down set=0x%02x confirm_cb=%d", (unsigned)set,
            s_ctx.page_confirm_cb != NULL);
    }

    if ((set & LV_PORT_BTN_CLICK) && (s_ctx.last & LV_PORT_BTN_CLICK)) {
        lv_port_click_hold_update();
    }

    if (!(set & LV_PORT_BTN_CLICK) && (s_ctx.last & LV_PORT_BTN_CLICK)) {
        /* 用户 2026-09-27 要求："打印我点下的模式" ⇒ 把按下时长与判定（短/长）一起打出来。 */
        BTN_LOG("KEY2 up%s set=0x%02x held=%ums",
                s_ctx.click_long_fired ? " after-long" : "",
                (unsigned)set,
                (unsigned)lv_tick_elaps(s_ctx.click_press_tick));
        if (s_ctx.click_long_fired) {
            if (s_ctx.page_longpress2_up_cb != NULL) {
                s_ctx.page_longpress2_up_cb(s_ctx.page_longpress2_up_ud);
            }
        } else {
            lv_port_click_confirm();
        }
        s_ctx.click_long_fired = false;
    }

    if ((set & LV_PORT_BTN_PWR) && !(s_ctx.last & LV_PORT_BTN_PWR)) {
        s_ctx.pwr_press_tick = lv_tick_get();
        s_ctx.pwr_long_fired = false;
        myvendor_sound_key();
        BTN_LOG("PWR down set=0x%02x", (unsigned)set);
    }

    if ((set & LV_PORT_BTN_PWR) && (s_ctx.last & LV_PORT_BTN_PWR)) {
        lv_port_pwr_hold_update();
    }

    if (!(set & LV_PORT_BTN_PWR) && (s_ctx.last & LV_PORT_BTN_PWR)) {
        BTN_LOG("PWR up%s set=0x%02x",
                s_ctx.pwr_long_fired ? " after-long" : "",
                (unsigned)set);
        if (!s_ctx.pwr_long_fired) {
            helm_pwr_on_click();
        }
        s_ctx.pwr_long_fired = false;
    }

    s_ctx.last = set;
}

/**
 * @brief lv_port_buttons_set_page_scroll_cb 接口。
 */
void lv_port_buttons_set_page_scroll_cb(lv_port_page_btn_cb_t cb, void *user_data)
{
    s_ctx.page_scroll_cb = cb;
    s_ctx.page_scroll_ud = user_data;
    LVGL_STALL("btn scroll_cb bound=%d", cb != NULL);
}

/**
 * @brief lv_port_buttons_set_page_scroll_double_cb 接口。
 */
void lv_port_buttons_set_page_scroll_double_cb(lv_port_page_btn_cb_t cb, void *user_data)
{
    s_ctx.page_scroll_double_cb = cb;
    s_ctx.page_scroll_double_ud = user_data;
    LVGL_STALL("btn scroll_double_cb bound=%d", cb != NULL);
}

/**
 * @brief lv_port_buttons_set_page_confirm_cb 接口。
 */
void lv_port_buttons_set_page_confirm_cb(lv_port_page_btn_cb_t cb, void *user_data)
{
    s_ctx.page_confirm_cb = cb;
    s_ctx.page_confirm_ud = user_data;
}

/**
 * @brief lv_port_buttons_set_page_click_cb 接口。
 */
void lv_port_buttons_set_page_click_cb(lv_port_page_click_cb_t cb, void *user_data)
{
    s_ctx.page_click_cb = cb;
    s_ctx.page_click_ud = user_data;
}

static void lv_port_buttons_consume_held_longpress(void)
{
    /* Same physical hold that opened a page must not fire the new page's
     * long-press (home KEY1 long = menu, menu KEY1 long = back). */
    if (s_ctx.last & LV_PORT_BTN_SCROLL) {
        s_ctx.scroll_long_fired = true;
    }

    if (s_ctx.last & LV_PORT_BTN_CLICK) {
        s_ctx.click_long_fired = true;
    }

    if (s_ctx.last & LV_PORT_BTN_PWR) {
        s_ctx.pwr_long_fired = true;
    }
}

/**
 * @brief KEY1 长按回调。同一指未松开时不把本次长当新页的长按。
 */
void lv_port_buttons_set_page_longpress_cb(lv_port_page_longpress_cb_t cb,
    void *user_data)
{
    s_ctx.page_longpress_cb = cb;
    s_ctx.page_longpress_ud = user_data;
    lv_port_buttons_consume_held_longpress();
}

void lv_port_buttons_set_page_longpress2_cb(lv_port_page_longpress_cb_t cb,
    void *user_data)
{
    s_ctx.page_longpress2_cb = cb;
    s_ctx.page_longpress2_ud = user_data;
    lv_port_buttons_consume_held_longpress();
}

void lv_port_buttons_set_page_longpress2_up_cb(lv_port_page_longpress_cb_t cb,
    void *user_data)
{
    s_ctx.page_longpress2_up_cb = cb;
    s_ctx.page_longpress2_up_ud = user_data;
}

/**
 * @brief lv_port_buttons_clear_page_callbacks 接口。
 */
void lv_port_buttons_clear_page_callbacks(void)
{
    LVGL_STALL("btn clear cbs");
    lv_port_cancel_pending_single_click();
    s_ctx.last_click_release_tick = 0;
    s_ctx.page_scroll_cb = NULL;
    s_ctx.page_scroll_ud = NULL;
    s_ctx.page_confirm_cb = NULL;
    s_ctx.page_confirm_ud = NULL;
    s_ctx.page_click_cb = NULL;
    s_ctx.page_click_ud = NULL;
    s_ctx.page_longpress_cb = NULL;
    s_ctx.page_longpress_ud = NULL;
    s_ctx.page_longpress2_cb = NULL;
    s_ctx.page_longpress2_ud = NULL;
    s_ctx.page_longpress2_up_cb = NULL;
    s_ctx.page_longpress2_up_ud = NULL;
    s_ctx.scroll_long_fired = (s_ctx.last & LV_PORT_BTN_SCROLL) != 0;
    s_ctx.click_long_fired = (s_ctx.last & LV_PORT_BTN_CLICK) != 0;
    s_ctx.pwr_long_fired = (s_ctx.last & LV_PORT_BTN_PWR) != 0;
}

/**
 * @brief lv_port_buttons_init 接口。
 */
void lv_port_buttons_init(void)
{
    if (s_ctx.disp != NULL) {
        return;
    }

    s_ctx.fd = open(LV_PORT_BTN_PATH, O_RDONLY | O_NONBLOCK);
    if (s_ctx.fd < 0) {
        BTN_LOG("open %s failed errno=%d", LV_PORT_BTN_PATH, errno);
        LV_LOG_WARN("btn: open %s failed (%d)", LV_PORT_BTN_PATH, errno);
        return;
    }

    s_ctx.disp = lv_display_get_default();
    if (s_ctx.disp == NULL) {
        close(s_ctx.fd);
        s_ctx.fd = -1;
        return;
    }

    s_ctx.last = lv_port_btn_read();
    /* 开机时用户可能还按着 PWR，不能把这次当单击或 2s 关机。 */
    s_ctx.pwr_long_fired = (s_ctx.last & LV_PORT_BTN_PWR) != 0;
    s_ctx.pwr_press_tick = lv_tick_get();
    s_ctx.focus_index = -1;

    s_ctx.pointer = lv_indev_create();
    lv_indev_set_type(s_ctx.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_ctx.pointer, s_ctx.disp);
    lv_indev_enable(s_ctx.pointer, false);

    lv_timer_create(lv_port_btn_timer, LV_PORT_POLL_MS, NULL);

    BTN_LOG("ready fd=%d KEY1=PA30 KEY2=PA33 PWR=PA34 last=0x%02x",
            s_ctx.fd, (unsigned)s_ctx.last);
    LV_LOG_INFO("btn: KEY1=PA30 KEY2=PA33 PWR=PA34 (click=bl, long 2s=off)");
}

#else

/**
 * @brief lv_port_buttons_init 接口。
 */
void lv_port_buttons_init(void)
{
}

/**
 * @brief lv_port_buttons_set_page_scroll_cb 接口。
 */
void lv_port_buttons_set_page_scroll_cb(lv_port_page_btn_cb_t cb, void *user_data)
{
    LV_UNUSED(cb);
    LV_UNUSED(user_data);
}

/**
 * @brief lv_port_buttons_set_page_scroll_double_cb 接口。
 */
void lv_port_buttons_set_page_scroll_double_cb(lv_port_page_btn_cb_t cb, void *user_data)
{
    LV_UNUSED(cb);
    LV_UNUSED(user_data);
}

/**
 * @brief lv_port_buttons_set_page_confirm_cb 接口。
 */
void lv_port_buttons_set_page_confirm_cb(lv_port_page_btn_cb_t cb, void *user_data)
{
    LV_UNUSED(cb);
    LV_UNUSED(user_data);
}

/**
 * @brief lv_port_buttons_set_page_click_cb 接口。
 */
void lv_port_buttons_set_page_click_cb(lv_port_page_click_cb_t cb, void *user_data)
{
    LV_UNUSED(cb);
    LV_UNUSED(user_data);
}

/**
 * @brief lv_port_buttons_set_page_longpress_cb 接口。
 */
void lv_port_buttons_set_page_longpress_cb(lv_port_page_longpress_cb_t cb,
    void *user_data)
{
    LV_UNUSED(cb);
    LV_UNUSED(user_data);
}

void lv_port_buttons_set_page_longpress2_cb(lv_port_page_longpress_cb_t cb,
    void *user_data)
{
    LV_UNUSED(cb);
    LV_UNUSED(user_data);
}

void lv_port_buttons_set_page_longpress2_up_cb(lv_port_page_longpress_cb_t cb,
    void *user_data)
{
    LV_UNUSED(cb);
    LV_UNUSED(user_data);
}

/**
 * @brief lv_port_buttons_clear_page_callbacks 接口。
 */
void lv_port_buttons_clear_page_callbacks(void)
{
}

#endif
