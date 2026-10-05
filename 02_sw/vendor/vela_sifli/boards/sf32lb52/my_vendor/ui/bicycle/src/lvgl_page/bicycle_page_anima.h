/**
 * @file bicycle_page_anima.h
 * @brief 自行车 UI — page_anima。
 */

#ifndef BICYCLE_PAGE_ANIMA_H
#define BICYCLE_PAGE_ANIMA_H

#include "bicycle_page_ids.h"
#include "lv_pm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 从中央表应用转场、缓存与 chrome 标志。 */
void bicycle_page_anima_apply(lv_pm_page_t page, bicycle_pm_id_t id);

/** @brief 仅启动：启动页下 LiveMap 壳层（无进入动画）。 */
void bicycle_page_anima_apply_silent(lv_pm_page_t page);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_PAGE_ANIMA_H */
