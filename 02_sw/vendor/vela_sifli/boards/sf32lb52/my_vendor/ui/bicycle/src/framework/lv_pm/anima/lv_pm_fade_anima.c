#include "../include/lv_pm_anima.h"

static void fade_exec(void * var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void _pm_fade_appear(lv_pm_anima_data * anima_data, lv_anim_t * appear_anima)
{
    lv_obj_t * page = anima_data->pm_page->page;
    lv_pm_open_options_t * open_options = &(anima_data->pm_page->open_options);

    lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);

    lv_anim_init(appear_anima);
    lv_anim_set_user_data(appear_anima, (void *)anima_data);
    lv_anim_set_var(appear_anima, page);
    lv_anim_set_values(appear_anima, 0, 255);
    lv_anim_set_time(appear_anima, open_options->time);
    lv_anim_set_repeat_count(appear_anima, 1);
    lv_anim_set_exec_cb(appear_anima, fade_exec);
    lv_anim_set_ready_cb(appear_anima, get_lv_pm_deleted_cb());
    lv_anim_set_path_cb(appear_anima, lv_anim_path_ease_out);
    lv_anim_start(appear_anima);
}

static void _pm_fade_dis_appear(lv_pm_anima_data * anima_data, lv_anim_t * dis_appear_anima)
{
    lv_obj_t * page = anima_data->pm_page->page;
    lv_pm_open_options_t * open_options = &(anima_data->pm_page->open_options);

    lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);

    lv_anim_init(dis_appear_anima);
    lv_anim_set_user_data(dis_appear_anima, (void *)anima_data);
    lv_anim_set_var(dis_appear_anima, page);
    lv_anim_set_values(dis_appear_anima, 255, 0);
    lv_anim_set_time(dis_appear_anima, open_options->time);
    lv_anim_set_repeat_count(dis_appear_anima, 1);
    lv_anim_set_exec_cb(dis_appear_anima, fade_exec);
    lv_anim_set_ready_cb(dis_appear_anima, get_lv_pm_deleted_cb());
    lv_anim_set_path_cb(dis_appear_anima, lv_anim_path_ease_in);
    lv_anim_start(dis_appear_anima);
}

const lv_pm_anima_t lv_pm_fade_anima =
{
    _pm_fade_appear,
    _pm_fade_dis_appear
};
