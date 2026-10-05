/**
 * @file bicycle_ts_overlay.h
 * @brief 自行车触摸屏调试叠加层。
 */

#ifndef BICYCLE_TS_OVERLAY_H
#define BICYCLE_TS_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create label on lv_layer_top() and refresh via lv_timer. */
void bicycle_ts_overlay_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_TS_OVERLAY_H */
