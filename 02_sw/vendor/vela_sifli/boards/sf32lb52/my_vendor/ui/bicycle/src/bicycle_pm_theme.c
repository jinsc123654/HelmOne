/**
 * @file bicycle_pm_theme.c
 * @brief 自行车 UI — pm_theme。
 */

#include "lv_pm_theme_def.h"

#include "vmap/vmap_style.h"

static vmap_style_id_t pm_theme_to_vmap(lv_pm_theme_id_t id)
{
    /* ⚠ 判据必须跟**调色板**同源（`helm_palette_follow_theme()`：classic=日光、其余=夜间）：
     * 原来只把 id==1（outdoor）当深色 ⇒ **night(2) 落到 classic（浅色地图）** ——
     * 深色界面配浅色地图，观感割裂。现在"不是 classic 就是深色"，三个主题一条规则。
     * （注：vmap 的 outdoor 枚举值是 2，跟 pm 的 id 不是一回事。） */
    if (id != 0u) {
        return VMAP_STYLE_OUTDOOR;
    }

    return VMAP_STYLE_CLASSIC;
}

/**
 * @brief 自行车 pm theme on activate。
 */
void bicycle_pm_theme_on_activate(lv_pm_theme_id_t id, void * user_data)
{
    (void)user_data;
    vmap_style_set_id(pm_theme_to_vmap(id));
}
