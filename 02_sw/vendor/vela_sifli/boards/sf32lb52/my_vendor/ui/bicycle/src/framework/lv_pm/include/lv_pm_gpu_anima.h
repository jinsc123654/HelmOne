/**
 * @file lv_pm_gpu_anima.h
 * @brief lv_pm 页面管理 — gpu_anima。
 */

#ifndef LV_PM_GPU_ANIMA_H
#define LV_PM_GPU_ANIMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_pm_anima.h"

/**
 * GPU-friendly page transitions (lv_obj transform → EPIC layer blend).
 *
 * direction (open_options.direction):
 *   LV_PM_GPU_DIR_* — see each animation's comment.
 */

#define LV_PM_GPU_DIR_CENTER     0
#define LV_PM_GPU_DIR_FROM_TOP   1
#define LV_PM_GPU_DIR_FROM_RIGHT 2
#define LV_PM_GPU_DIR_FROM_LEFT  3

/** @brief 缩放 80%→100% + 淡入（Startup / 通用 push）。 */
extern const lv_pm_anima_t lv_pm_gpu_zoom_anima;

/** @brief Do not use: rotate+slide is too heavy (jank on SF32LB52 + map). */
extern const lv_pm_anima_t lv_pm_gpu_rotate_slide_anima;

/** @brief 水平翻转 scale_x 12%→100% + 淡入 — 模态/详情。 */
extern const lv_pm_anima_t lv_pm_gpu_flip_anima;

/** @brief 景深视差：进入 88%→100% + 淡入；离开 100%→92%。 */
extern const lv_pm_anima_t lv_pm_gpu_depth_anima;

/** @brief 转场后清除 transform 样式（可选）。 */
void lv_pm_gpu_page_reset_transform(lv_obj_t * page);

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_GPU_ANIMA_H */
