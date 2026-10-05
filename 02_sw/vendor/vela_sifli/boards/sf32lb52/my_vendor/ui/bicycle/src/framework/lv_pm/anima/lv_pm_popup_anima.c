/**
 * @file lv_pm_popup_anima.c
 * @brief lv_pm 页面管理 — popup_anima。
 */

#include "../include/lv_pm_anima.h"
#include "../include/lv_pm_disp.h"
/* 基础测试 */
static void translateX_anima_cb(void* var, int32_t v)
{
    lv_obj_set_x(var, v);
}

static void translateY_anima_cb(void* var, int32_t v)
{
    lv_obj_set_y(var, v);
}
/**
----------------------------------------------------------------------------------------------------------
  popup animation弹入
----------------------------------------------------------------------------------------------------------
*/

static void _pm_popup_appear(lv_pm_anima_data* anima_data, lv_anim_t* appear_anima)
{
    int32_t start_ops1 = 0;
    int32_t end_ops1 = 0;
    int32_t start_ops2 = 0;
    int32_t end_ops2 = 0;
    lv_anim_exec_xcb_t anima_cb = NULL;

    lv_pm_open_options_t* open_options = &(anima_data->pm_page->open_options);
    switch (open_options->direction)
    {
    case 0:
    {
        start_ops1 = -lv_pm_ver_res();
        end_ops1 = 0;
        start_ops2 = lv_pm_ver_res() / 2;
        end_ops2 = 0;
        anima_cb = translateY_anima_cb;
    }
    break;
    case 1:
    {
        start_ops1 = lv_pm_ver_res();
        end_ops1 = 0;
        start_ops2 = lv_pm_ver_res() / 2;
        end_ops2 = 0;
        anima_cb = translateY_anima_cb;
    }
    break;
    case 2:
    {
        start_ops1 = -lv_pm_hor_res();
        end_ops1 = 0;
        start_ops2 = -start_ops1 / 2;
        end_ops2 = 0;
        anima_cb = translateX_anima_cb;
    }
    break;
    case 3:
    {
        start_ops1 = lv_pm_hor_res();
        end_ops1 = 0;
        start_ops2 = -start_ops1 / 2;
        end_ops2 = 0;
        anima_cb = translateX_anima_cb;
    }
    break;
    default:
        break;
    }

    lv_anim_init(appear_anima);
    lv_anim_set_user_data(appear_anima, (void*)anima_data);
    lv_anim_set_var(appear_anima, anima_data->pm_page->page);

    if (anima_data->pm_page->flag.is_back)
    {
        lv_anim_set_values(appear_anima, start_ops2, end_ops2);
        //lv_obj_set_style_radius(anima_data->pm_page->page, 0, LV_STATE_DEFAULT);
    }
    else
    {
        lv_obj_move_foreground(anima_data->pm_page->page);/*打开时新界面从前方覆盖 否则动画可能会由于界面先后创建的问题导致异常*/
        lv_anim_set_values(appear_anima, start_ops1, end_ops1);
        //lv_obj_set_style_radius(anima_data->pm_page->page, 10, LV_STATE_DEFAULT);
    }

    lv_anim_set_path_cb(appear_anima, lv_anim_path_ease_in_out);
    lv_anim_set_time(appear_anima, open_options->time);
    lv_anim_set_repeat_count(appear_anima, 1);
    lv_anim_set_exec_cb(appear_anima, anima_cb);
    lv_anim_set_ready_cb(appear_anima, get_lv_pm_deleted_cb());
    lv_anim_start(appear_anima);
}

static void _pm_popup_dis_appear(lv_pm_anima_data* anima_data, lv_anim_t* dis_appear_anima)
{
    int32_t start_ops = 0;
    int32_t end_ops = 0;
    int32_t start_ops2 = 0;
    int32_t end_ops2 = 0;
    lv_anim_exec_xcb_t anima_cb = NULL;

    lv_pm_open_options_t* open_options = &(anima_data->pm_page->open_options);
    switch (open_options->direction)
    {
    case 0:
    {
        start_ops = 0;
        end_ops = -lv_pm_ver_res();
        start_ops2 = 0;
        end_ops2 = lv_pm_ver_res() / 2;
        anima_cb = translateY_anima_cb;
    }
    break;
    case 1:
    {
        start_ops = 0;
        end_ops = lv_pm_ver_res();
        start_ops2 = 0;
        end_ops2 = lv_pm_ver_res() / 2;
        anima_cb = translateY_anima_cb;
    }
    break;
    case 2:
    {
        start_ops = 0;
        end_ops = -lv_pm_hor_res();
        start_ops2 = 0;
        end_ops2 = -end_ops / 2;
        anima_cb = translateX_anima_cb;
    }
    break;
    case 3:
    {
        start_ops = 0;
        end_ops = lv_pm_hor_res();
        start_ops2 = 0;
        end_ops2 = -end_ops / 2;
        anima_cb = translateX_anima_cb;
    }
    break;
    default:
        break;
    }

    lv_anim_init(dis_appear_anima);
    lv_anim_set_user_data(dis_appear_anima, (void*)anima_data);
    lv_anim_set_var(dis_appear_anima, anima_data->pm_page->page);

    if (anima_data->pm_page->flag.is_back)
    {
        lv_obj_move_foreground(anima_data->pm_page->page);/*退出时旧界面从前方覆盖 否则动画可能会由于界面先后创建的问题导致异常*/
        lv_anim_set_values(dis_appear_anima, start_ops, end_ops);
        //lv_obj_set_style_radius(anima_data->pm_page->page, 0, LV_STATE_DEFAULT);
    }
    else {
        lv_anim_set_values(dis_appear_anima, start_ops2, end_ops2);
        //lv_obj_set_style_radius(anima_data->pm_page->page, 10, LV_STATE_DEFAULT);
    }

    lv_anim_set_time(dis_appear_anima, open_options->time);
    lv_anim_set_repeat_count(dis_appear_anima, 1);
    lv_anim_set_exec_cb(dis_appear_anima, anima_cb);
    lv_anim_set_ready_cb(dis_appear_anima, get_lv_pm_deleted_cb());
    lv_anim_set_path_cb(dis_appear_anima, lv_anim_path_ease_in_out);
    lv_anim_start(dis_appear_anima);
}

const lv_pm_anima_t lv_pm_popup_anima =
{
    _pm_popup_appear,
    _pm_popup_dis_appear
};
