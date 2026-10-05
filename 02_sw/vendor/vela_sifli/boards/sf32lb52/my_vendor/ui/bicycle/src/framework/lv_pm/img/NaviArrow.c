/**
 * @file NaviArrow.c
 * @brief 生成/位图资源数据。
 */

/*
 * NaviArrow — see NaviArrow.h
 */
#include "NaviArrow.h"

/* style table — add a new arrow style here (and in navi_arrow_style_t). */
static const lv_image_dsc_t *const s_navi_arrows[NAVI_ARROW_STYLE_COUNT] = {
    [NAVI_ARROW_LIGHT] = &img_src_navi_arrow_light,
    [NAVI_ARROW_DARK] = &img_src_navi_arrow_dark,
};

const lv_image_dsc_t *navi_arrow_get(navi_arrow_style_t style)
{
    if ((unsigned)style >= (unsigned)NAVI_ARROW_STYLE_COUNT) {
        style = NAVI_ARROW_LIGHT;
    }
    return s_navi_arrows[style];
}
