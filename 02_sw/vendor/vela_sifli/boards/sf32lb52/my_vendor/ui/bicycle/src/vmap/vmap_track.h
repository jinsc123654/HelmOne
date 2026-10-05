/**
 * @file vmap_track.h
 * @brief vmap track 模块。
 */

#ifndef VMAP_TRACK_H
#define VMAP_TRACK_H

#include "vmap_render.h"
#include "vmap_view.h"
#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VMAP_TRACK_DRAW_CAP
#  ifdef BICYCLE_TRACK_DRAW_CAP
#    define VMAP_TRACK_DRAW_CAP ((uint16_t)BICYCLE_TRACK_DRAW_CAP)
#  else
#    define VMAP_TRACK_DRAW_CAP 384
#  endif
#endif

#ifndef VMAP_KM_MARKER_CAP
#  ifdef BICYCLE_KM_MARKER_MAX
#    define VMAP_KM_MARKER_CAP ((uint16_t)BICYCLE_KM_MARKER_MAX)
#  else
#    define VMAP_KM_MARKER_CAP 150
#  endif
#endif

typedef struct vmap_track vmap_track_t;

typedef void (*vmap_track_lap_cb_t)(vmap_track_t * track, uint16_t lap,
    void * user_data);

/** @brief 圈闭合检测 — 全局默认，运行时可覆盖。 */
typedef struct {
    double min_distance_m;     /**< Min lap length before closure can fire. */
    double close_radius_m;     /**< Distance to lap start treated as closed. */
    double max_course_diff_deg; /**< |motion - lap_ref_course| (0=off). */
} vmap_track_lap_config_t;

/**
 * @brief vmap track color for speed。
 */
lv_color_t vmap_track_color_for_speed(float speed_kph);

/**
 * @brief vmap track lap config get。
 * @return 请求的值。
 */
void vmap_track_lap_config_get(vmap_track_lap_config_t * out);
/**
 * @brief vmap track lap config set。
 */
void vmap_track_lap_config_set(const vmap_track_lap_config_t * cfg);
/**
 * @brief vmap track lap config reset defaults。
 */
void vmap_track_lap_config_reset_defaults(void);

/**
 * @brief vmap track create on view。
 * @return 0 成功，负 errno 失败。
 */
vmap_track_t * vmap_track_create_on_view(vmap_view_t * view);
/**
 * @brief vmap view attach track。
 */
void vmap_view_attach_track(vmap_view_t * view, vmap_track_t * track);
/**
 * @brief vmap track destroy。
 */
void vmap_track_destroy(vmap_track_t * track);
/**
 * @brief vmap track bind view。
 */
void vmap_track_bind_view(vmap_track_t * track, vmap_view_t * view);
/**
 * @brief vmap track set font。
 */
void vmap_track_set_font(vmap_track_t * track, const lv_font_t * font);
void vmap_track_set_lap_cb(vmap_track_t * track, vmap_track_lap_cb_t cb,
    void * user_data);

/**
 * @brief vmap track set recording。
 */
void vmap_track_set_recording(vmap_track_t * track, bool enable);
/** @brief 暂停录点，不清空本次轨迹。 */
void vmap_track_pause(vmap_track_t * track);
/** @brief 继续录点，不清空本次轨迹。 */
void vmap_track_resume(vmap_track_t * track);
/**
 * @brief vmap track is recording。
 * @return 请求的值。
 */
bool vmap_track_is_recording(const vmap_track_t * track);
/**
 * @brief vmap track clear。
 */
void vmap_track_clear(vmap_track_t * track);
/**
 * @brief vmap track point count。
 * @return 请求的值。
 */
uint16_t vmap_track_point_count(const vmap_track_t * track);
/**
 * @brief vmap track point capacity。
 */
uint16_t vmap_track_point_capacity(const vmap_track_t * track);
/**
 * @brief vmap track lap count。
 * @return 请求的值。
 */
uint16_t vmap_track_lap_count(const vmap_track_t * track);
/**
 * @brief vmap track session distance m。
 */
double vmap_track_session_distance_m(const vmap_track_t * track);
/**
 * @brief vmap track lap distance m。
 */
double vmap_track_lap_distance_m(const vmap_track_t * track);
/**
 * @brief vmap track draw point count。
 * @return 请求的值。
 */
uint16_t vmap_track_draw_point_count(const vmap_track_t * track);
/**
 * @brief vmap track km marker count。
 * @return 请求的值。
 */
uint16_t vmap_track_km_marker_count(const vmap_track_t * track);

/**
 * @brief vmap track push。
 */
bool vmap_track_push(vmap_track_t * track, double lon, double lat, float speed_kph);
/**
 * @brief vmap track refresh。
 */
void vmap_track_refresh(vmap_track_t * track);
/**
 * @brief 隐藏期间积了很多未上色的点，或环形缓冲已转满。
 * @details 回来后不能在旧画布上增量画尾巴，否则跟车 rebase 会留下错位墨迹。
 */
bool vmap_track_needs_restamp(const vmap_track_t * track);
/**
 * @brief 把 REC 线、公里圆标、公里数字盖进画布（叠在标签之上，数字在最顶）。
 */
void vmap_track_paint_to_canvas(vmap_track_t * track, vmap_canvas_t * canvas);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_TRACK_H */
