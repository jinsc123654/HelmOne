/**
 * @file bicycle_c_debug.h
 * @brief 自行车 C 侧调试命令。
 */

#ifndef BICYCLE_C_DEBUG_H
#define BICYCLE_C_DEBUG_H

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vmap_track;
struct vmap_view;

#if defined(CONFIG_MYVENDOR_BICYCLE_C_DEBUG) && CONFIG_MYVENDOR_BICYCLE_C_DEBUG

/**
 * @brief 自行车 c debug init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_c_debug_init(void);
/**
 * @brief 自行车 c debug loop。
 */
void bicycle_c_debug_loop(uint32_t lv_idle_ms);
/**
 * @brief 自行车 c debug gpx tick。
 */
void bicycle_c_debug_gpx_tick(void);
/**
 * @brief 自行车 c debug track push。
 */
void bicycle_c_debug_track_push(bool accepted);
/**
 * @brief 自行车 c debug track refresh。
 */
void bicycle_c_debug_track_refresh(bool drew);
void bicycle_c_debug_track_state(uint16_t draw_n, uint16_t km_count,
    double lap_distance_m);
/**
 * @brief 自行车 c debug track lap。
 */
void bicycle_c_debug_track_lap(uint16_t lap);
/**
 * @brief 自行车 c debug map soft pan。
 */
void bicycle_c_debug_map_soft_pan(void);
/**
 * @brief 自行车 c debug map full render。
 */
void bicycle_c_debug_map_full_render(void);
void bicycle_c_debug_snap(const struct vmap_view * map,
    const struct vmap_track * track);

#else

static inline void bicycle_c_debug_init(void) {}
static inline void bicycle_c_debug_loop(uint32_t lv_idle_ms) { (void)lv_idle_ms; }
static inline void bicycle_c_debug_gpx_tick(void) {}
static inline void bicycle_c_debug_track_push(bool accepted) { (void)accepted; }
static inline void bicycle_c_debug_track_refresh(bool drew) { (void)drew; }
static inline void bicycle_c_debug_track_state(uint16_t draw_n, uint16_t km_count,
    double lap_distance_m)
{
    (void)draw_n;
    (void)km_count;
    (void)lap_distance_m;
}
static inline void bicycle_c_debug_track_lap(uint16_t lap) { (void)lap; }
static inline void bicycle_c_debug_map_soft_pan(void) {}
static inline void bicycle_c_debug_map_full_render(void) {}
static inline void bicycle_c_debug_snap(const struct vmap_view * map,
    const struct vmap_track * track)
{
    (void)map;
    (void)track;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_C_DEBUG_H */
