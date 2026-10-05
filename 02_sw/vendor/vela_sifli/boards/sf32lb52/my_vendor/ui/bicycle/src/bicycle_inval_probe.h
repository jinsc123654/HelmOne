/**
 * @file bicycle_inval_probe.h
 * @brief 自行车 UI — inval_probe。
 */

#ifndef BICYCLE_INVAL_PROBE_H
#define BICYCLE_INVAL_PROBE_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 自行车 inval probe init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_inval_probe_init(lv_display_t * disp);

/** @brief 为下一次 lv_obj_invalidate 打标签（如 "vmap_render"）。 */
void bicycle_inval_probe_tag(const char * tag);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_INVAL_PROBE_H */
