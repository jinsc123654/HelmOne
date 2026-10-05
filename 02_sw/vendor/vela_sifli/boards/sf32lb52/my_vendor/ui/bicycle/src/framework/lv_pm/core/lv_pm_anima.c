/**
 * @file lv_pm_anima.c
 * @brief lv_pm 页面管理 — anima。
 */

#include <stdlib.h>
#include "../include/lv_pm_anima.h"
#include "../include/lv_pm_gpu_anima.h"

static lv_anim_t appear_anima;
static lv_anim_t dis_appear_anima;

static lv_pm_anima_data s_appear_data = {.is_free = true};
static lv_pm_anima_data s_disappear_data = {.is_free = true};

static void anima_invoke_finish(lv_pm_anima_data * data)
{
    lv_pm_anima_complete_cb cb;
    lv_pm_page_t page;
    lv_pm_open_options_t * opts;

    if (data == NULL || data->is_free) {
        return;
    }

    cb = data->cb;
    page = data->pm_page;
    opts = data->open_options;
    data->cb = NULL;
    data->is_free = true;

    if (cb != NULL) {
        cb(page, opts);
    }

    LV_LOG_INFO("动画结束回调 ID -> %d, back=%d",
           page ? page->id : -1,
           page ? page->flag.is_back : 0);
    lv_pm_nav_anim_end();
}

static void anima_stop_lvgl(lv_pm_anima_data * data)
{
    lv_obj_t * page;

    if (data == NULL || data->pm_page == NULL) {
        return;
    }

    page = data->pm_page->page;
    if (page == NULL) {
        return;
    }

    lv_anim_delete(page, NULL);
    lv_pm_gpu_page_reset_transform(page);
}

static void anima_channel_preempt(lv_pm_anima_data * data)
{
    if (data->is_free) {
        return;
    }

    anima_stop_lvgl(data);
    anima_invoke_finish(data);
}

static void anima_deleted_cb(lv_anim_t * anim)
{
    lv_pm_anima_data * cb_data = (lv_pm_anima_data *)anim->user_data;

    if (cb_data == NULL) {
        lv_pm_nav_anim_end();
        return;
    }

    anima_invoke_finish(cb_data);
}

static const lv_pm_anima_t * anima_resolve(lv_pm_page_t pm_page)
{
    const lv_pm_anima_t * anima;

    if (pm_page == NULL) {
        return NULL;
    }

    anima = (const lv_pm_anima_t *)pm_page->open_options.lv_pm_anima_cb;
    if (anima == NULL) {
        return NULL;
    }

    if (anima->lv_pm_appear == NULL || anima->lv_pm_dis_appear == NULL) {
        LV_LOG_WARN("[lv_pm_anima] page %d: incomplete appear/dis_appear pair, use no_anima",
                    pm_page->id);
        return &lv_pm_no_anima;
    }

    return anima;
}

/**
 * @brief get_lv_pm_deleted_cb 接口。
 */
lv_anim_deleted_cb_t get_lv_pm_deleted_cb(void)
{
    return anima_deleted_cb;
}

/**
 * @brief lv_pm anima drain all。
 */
void lv_pm_anima_drain_all(void)
{
    anima_channel_preempt(&s_disappear_data);
    anima_channel_preempt(&s_appear_data);
}

/**
 * @brief lv_pm anima play。
 */
void lv_pm_anima_play(lv_pm_page_t pm_page, lv_pm_anima_phase_t phase,
                      lv_pm_open_options_t * route_opts, lv_pm_anima_complete_cb cb)
{
    const lv_pm_anima_t * anima;
    lv_pm_anima_data * channel;
    lv_anim_t * lv_anim;

    if (pm_page == NULL) {
        return;
    }

    anima = anima_resolve(pm_page);
    if (anima == NULL) {
        lv_pm_nav_anim_begin();
        if (cb != NULL) {
            cb(pm_page, route_opts);
        }
        lv_pm_nav_anim_end();
        return;
    }

    if (phase == LV_PM_ANIMA_ENTER) {
        channel = &s_appear_data;
        lv_anim = &appear_anima;
        anima_channel_preempt(channel);
    } else {
        channel = &s_disappear_data;
        lv_anim = &dis_appear_anima;
        anima_channel_preempt(channel);
    }

    channel->pm_page = pm_page;
    channel->open_options = route_opts;
    channel->cb = cb;
    channel->is_free = false;
    lv_pm_nav_anim_begin();

    if (phase == LV_PM_ANIMA_ENTER) {
        anima->lv_pm_appear(channel, lv_anim);
    } else {
        anima->lv_pm_dis_appear(channel, lv_anim);
    }
}

/**
 * @brief lv_pm anima open cb。
 * @return 0 成功，负 errno 失败。
 */
void lv_pm_anima_open_cb(lv_pm_page_t pm_page, lv_pm_open_options_t * route_opts,
                         lv_pm_anima_complete_cb cb)
{
    lv_pm_anima_play(pm_page, LV_PM_ANIMA_ENTER, route_opts, cb);
}

/**
 * @brief lv_pm anima back cb。
 */
void lv_pm_anima_back_cb(lv_pm_page_t pm_page, lv_pm_open_options_t * route_opts,
                         lv_pm_anima_complete_cb cb)
{
    lv_pm_anima_play(pm_page, LV_PM_ANIMA_EXIT, route_opts, cb);
}
