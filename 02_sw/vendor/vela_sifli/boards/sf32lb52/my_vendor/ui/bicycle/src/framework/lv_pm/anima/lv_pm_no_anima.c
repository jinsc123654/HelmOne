#include "../include/lv_pm_anima.h"
#include "../include/lv_pm_bar.h"

static void _pm_popup_no_anima_appear(lv_pm_anima_data* anima_data, lv_anim_t* appear_anima)
{
    lv_pm_open_options_t* open_options = &(anima_data->pm_page->open_options);
    lv_obj_t* page = anima_data->pm_page->page;
    lv_coord_t rest_y = 0;

    // 设置动画控制变量，确保动画数据有效
    lv_anim_init(appear_anima);
    lv_anim_set_user_data(appear_anima, (void*)anima_data);
    lv_anim_set_var(appear_anima, page);

#if LV_PM_USE_STA_BAR
    if (anima_data->pm_page->flag.top_bar_en) {
        rest_y = lv_pm_status_bar_height();
    }
#endif

    // 直接设置控件到最终位置，模拟动画完成状态
    switch (open_options->direction)
    {
    case 0:
    case 1:
        lv_obj_set_y(page, rest_y);
        break;
    case 2:
    case 3:
        lv_obj_set_x(page, 0);
        break;
    default:
        break;
    }

    lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page);

    // 触发动画完成回调
    lv_anim_ready_cb_t ready_cb = get_lv_pm_deleted_cb();
    if (ready_cb) ready_cb(appear_anima);
}

static void _pm_popup_no_anima_dis_appear(lv_pm_anima_data* anima_data, lv_anim_t* dis_appear_anima)
{
    lv_obj_t* page = anima_data->pm_page->page;

    lv_anim_init(dis_appear_anima);
    lv_anim_set_user_data(dis_appear_anima, (void*)anima_data);
    lv_anim_set_var(dis_appear_anima, page);

    // 直接隐藏页面，模拟动画完成状态
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    // 触发动画完成回调
    lv_anim_ready_cb_t ready_cb = get_lv_pm_deleted_cb();
    if (ready_cb) ready_cb(dis_appear_anima);
}

const lv_pm_anima_t lv_pm_no_anima =
{
    _pm_popup_no_anima_appear,
    _pm_popup_no_anima_dis_appear
};
