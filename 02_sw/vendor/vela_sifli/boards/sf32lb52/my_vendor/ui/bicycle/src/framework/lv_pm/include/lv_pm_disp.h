/**
 * @file lv_pm_disp.h
 * @brief lv_pm 显示/分辨率辅助。
 */

#ifndef LV_PM_DISP_H
#define LV_PM_DISP_H

#include "lvgl/lvgl.h"

static inline lv_coord_t lv_pm_hor_res(void)
{
#if defined(PAGE_HOR_RES)
    return (lv_coord_t)PAGE_HOR_RES;
#else
    lv_display_t * disp = lv_display_get_default();

    return disp ? lv_display_get_horizontal_resolution(disp) : 240;
#endif
}

static inline lv_coord_t lv_pm_ver_res(void)
{
#if defined(PAGE_VER_RES)
    return (lv_coord_t)PAGE_VER_RES;
#else
    lv_display_t * disp = lv_display_get_default();

    return disp ? lv_display_get_vertical_resolution(disp) : 320;
#endif
}

#endif /* LV_PM_DISP_H */
