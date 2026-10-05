/**
 * @file lv_pm_port.c
 * @brief lv_pm 页面管理 — port。
 */

#include "../include/lv_pm_port.h"
#include "../include/lv_pm_anima.h"

static lv_pm_page_t pm_page_of(void * page)
{
    return lv_pm_get_pm_page(page);
}

/**
 * @brief lv_pm set open。
 * @return 0 成功，负 errno 失败。
 */
int8_t lv_pm_set_open(void * page, lv_pm_lifecycle fun)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->open_cb = fun;
    return 0;
}

/**
 * @brief lv_pm set will appear。
 */
int8_t lv_pm_set_will_appear(void * page, lv_pm_lifecycle fun)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->will_appear_cb = fun;
    return 0;
}

/**
 * @brief lv_pm set dis appear。
 * @return 请求的值。
 */
int8_t lv_pm_set_dis_appear(void * page, lv_pm_lifecycle fun)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->dis_appear_cb = fun;
    return 0;
}

/**
 * @brief lv_pm set will disappear。
 */
int8_t lv_pm_set_will_disappear(void * page, lv_pm_lifecycle fun)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->will_disappear_cb = fun;
    return 0;
}

/**
 * @brief lv_pm set dis disappear。
 * @return 请求的值。
 */
int8_t lv_pm_set_dis_disappear(void * page, lv_pm_lifecycle fun)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->dis_disappear_cb = fun;
    return 0;
}

/**
 * @brief lv_pm set close。
 */
int8_t lv_pm_set_close(void * page, lv_pm_lifecycle fun)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->close_cb = fun;
    return 0;
}

/**
 * @brief lv_pm set open options。
 * @return 0 成功，负 errno 失败。
 */
int8_t lv_pm_set_open_options(void * page, const void * lv_pm_anima_cb,
                              lv_pm_target target, uint32_t direction, uint32_t time)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    if (lv_pm_anima_cb != NULL) {
        const lv_pm_anima_t * anima = (const lv_pm_anima_t *)lv_pm_anima_cb;

        if (anima->lv_pm_appear == NULL || anima->lv_pm_dis_appear == NULL) {
            LV_LOG_WARN("[lv_pm_port] anima missing appear/dis_appear pair, use no_anima");
            pm->open_options.lv_pm_anima_cb = (void *)&lv_pm_no_anima;
        }
        else {
            pm->open_options.lv_pm_anima_cb = lv_pm_anima_cb;
        }
    }
    else {
        pm->open_options.lv_pm_anima_cb = (void *)&lv_pm_no_anima;
    }

    pm->open_options.target = target;
    pm->open_options.direction = direction;
    pm->open_options.time = time;
    return 0;
}

/**
 * @brief lv_pm add focus obj。
 */
int8_t lv_pm_add_focus_obj(void * page, lv_obj_t * obj)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL || obj == NULL) {
        return -1;
    }

    if (pm->group == NULL) {
        return -1;
    }

    lv_group_add_obj(pm->group, obj);
    return 0;
}

/**
 * @brief lv_pm set focus obj。
 */
int8_t lv_pm_set_focus_obj(void * page, lv_obj_t * obj)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->focus_obj = obj;
    return 0;
}

/**
 * @brief lv_pm set top bar。
 */
int8_t lv_pm_set_top_bar(void * page, bool enable)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->flag.top_bar_en = enable ? 1u : 0u;
    return 0;
}

/**
 * @brief lv_pm set back bar。
 */
int8_t lv_pm_set_back_bar(void * page, bool enable)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->flag.back_bar_en = enable ? 1u : 0u;
    return 0;
}

/**
 * @brief lv_pm set cache enable。
 */
int8_t lv_pm_set_cache_enable(void * page, bool enable)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return -1;
    }

    pm->flag.cache_enable = enable ? 1u : 0u;
    return 0;
}

/**
 * @brief page_timer_create 接口。
 */
void page_timer_create(void * page, lv_timer_cb_t timer_xcb, uint32_t period, void * user_data)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL || timer_xcb == NULL) {
        return;
    }

    if (pm->page_timer != NULL) {
        lv_timer_del(pm->page_timer);
        pm->page_timer = NULL;
    }

    pm->page_timer = lv_timer_create(timer_xcb, period, user_data);
    if (pm->page_timer != NULL) {
        lv_timer_pause(pm->page_timer);
    }
}

/**
 * @brief page_timer_del 接口。
 */
void page_timer_del(void * page)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL || pm->page_timer == NULL) {
        return;
    }

    lv_timer_del(pm->page_timer);
    pm->page_timer = NULL;
}

/**
 * @brief page_timer_backend 接口。
 */
void page_timer_backend(void * page, bool en)
{
    lv_pm_page_t pm = pm_page_of(page);

    if (pm == NULL) {
        return;
    }

    pm->flag.timer_backend_run = en ? 1u : 0u;
}
