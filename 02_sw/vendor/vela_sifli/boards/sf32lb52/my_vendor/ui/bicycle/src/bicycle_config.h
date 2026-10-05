/**
 * @file bicycle_config.h
 * @brief 自行车 UI 配置（轨迹容量、PSRAM 规划、导航参数）。
 */

#ifndef BICYCLE_CONFIG_H
#define BICYCLE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Track / km-marker capacity (compile-time layout + PSRAM planning)
 *
 * Single session: cumulative km badges 1, 2, … (no upper stop).
 * Map stores BICYCLE_KM_MARKER_MAX slots; km 1…N use slot N-1, then km N+1
 * wraps and overwrites slot 0 like a ring buffer.
 * On-screen track polyline: current lap only (cleared on lap closure); pts[]
 * ring holds up to BICYCLE_MAX_SESSION_KM at BICYCLE_TRACK_POINT_MIN_DIST_M
 * spacing (~50k points @ 3 m).
 * PSRAM arena: draw decimation buffer + track points (+ slack).
 * ------------------------------------------------------------------------- */

#ifndef BICYCLE_MAX_SESSION_KM
#define BICYCLE_MAX_SESSION_KM             150u
#endif

#ifndef BICYCLE_TRACK_POINT_MIN_DIST_M
#define BICYCLE_TRACK_POINT_MIN_DIST_M     3.0
#endif

#ifndef BICYCLE_TRACK_DRAW_CAP
#define BICYCLE_TRACK_DRAW_CAP             384u
#endif

/** 地图上公里标记环槽上限（150 → 150 km 后从 slot 0 覆盖）。 */
#define BICYCLE_KM_MARKER_MAX              (BICYCLE_MAX_SESSION_KM)

/** pts[] 可表示的最大单圈屏幕距离（米）。 */
#define BICYCLE_MAX_LAP_DISPLAY_M \
    ((uint32_t)(BICYCLE_MAX_SESSION_KM) * 1000u)

/** 地图会话显示的轨迹点环容量。 */
#define BICYCLE_TRACK_POINT_CAP \
    ((uint16_t)(((BICYCLE_MAX_LAP_DISPLAY_M) \
        + (uint32_t)(BICYCLE_TRACK_POINT_MIN_DIST_M) - 1u) \
        / (uint32_t)(BICYCLE_TRACK_POINT_MIN_DIST_M)))

/** 结构体布局大小（arena 预算；须与 vmap_track.c 一致）。 */
#define BICYCLE_TRACK_PT_BYTES             12u
#define BICYCLE_TRACK_DRAW_PT_BYTES        12u
#define BICYCLE_TRACK_ARENA_SLACK_BYTES    4096u

/** 每公里轨迹环记录的 GNSS 点数（ceil(1000 / min_dist_m)）。 */
#define BICYCLE_TRACK_POINTS_PER_KM \
    ((uint32_t)(1000u + (uint32_t)(BICYCLE_TRACK_POINT_MIN_DIST_M) - 1u) \
        / (uint32_t)(BICYCLE_TRACK_POINT_MIN_DIST_M))

/** max_session_km 每增加 1 km 的 PSRAM arena 增量（仅 pts[]；draw cap 固定）。 */
#define BICYCLE_TRACK_PSRAM_BYTES_PER_KM \
    ((size_t)(BICYCLE_TRACK_POINTS_PER_KM) * (size_t)BICYCLE_TRACK_PT_BYTES)

#define BICYCLE_TRACK_ARENA_BYTES \
    ((size_t)(BICYCLE_TRACK_DRAW_CAP) * (size_t)BICYCLE_TRACK_DRAW_PT_BYTES \
     + (size_t)(BICYCLE_TRACK_POINT_CAP) * (size_t)BICYCLE_TRACK_PT_BYTES \
     + (size_t)BICYCLE_TRACK_ARENA_SLACK_BYTES)

#include "vmap/vmap_track.h"
#include "vmap/vmap_style.h"

typedef struct {
    vmap_track_lap_config_t lap;
    /** 地图轨迹上公里标记间距（米）。 */
    double km_marker_interval_m;
    /** 屏幕单圈/轨迹点预算（km，<= BICYCLE_MAX_SESSION_KM）。
     *  PSRAM 轨迹环：每 km +BICYCLE_TRACK_PSRAM_BYTES_PER_KM
     *  （默认 3 m 间距 → 334 pts/km × 12 B ≈ 4.0 KiB/km；150 km ≈ 586 KiB
     *  pts[]，另加固定 draw 抽稀 ~4.6 KiB + 4 KiB slack）。
     *  km 标记槽为 vmap_track 中独立固定环（非按 km 增长）。 */
    uint16_t max_session_km;
    /** 地图 km 标记环槽数（默认 BICYCLE_KM_MARKER_MAX）。 */
    uint16_t km_marker_max;
    /** 屏幕单圈轨迹点上限（<= BICYCLE_TRACK_POINT_CAP）。 */
    uint16_t track_point_cap;
    /** pts 超过 track_point_cap 时折线抽稀采样上限。 */
    uint16_t track_draw_cap;
    /** 记录 GNSS 点的最小间距（米）。 */
    double track_point_min_dist_m;
    /** 自动重路由偏航距离（米）：反骑过久或过路口后偏航。 */
    double nav_off_route_m;
    bool record_on_start;
    uint32_t gnss_update_ms;
    float gnss_sim_speed_kph;
    /** `nav sim` 规划成功后的沿路线速度（km/h）。 */
    float nav_sim_speed_kph;
    const char * gpx_path;
    vmap_style_id_t map_style;
} bicycle_config_t;

/**
 * @brief 自行车 config reset defaults。
 */
void bicycle_config_reset_defaults(void);
/**
 * @brief 自行车 config。
 */
const bicycle_config_t * bicycle_config(void);
/**
 * @brief 自行车 config get。
 * @return 请求的值。
 */
void bicycle_config_get(bicycle_config_t * out);
/**
 * @brief 自行车 config set。
 */
void bicycle_config_set(const bicycle_config_t * cfg);
/**
 * @brief 自行车 config apply。
 */
void bicycle_config_apply(void);

/**
 * @brief 自行车 config max session km。
 */
uint16_t bicycle_config_max_session_km(void);
/**
 * @brief 自行车 config km marker max。
 */
uint16_t bicycle_config_km_marker_max(void);
/**
 * @brief 自行车 config track point cap。
 */
uint16_t bicycle_config_track_point_cap(void);
/**
 * @brief 自行车 config track draw cap。
 */
uint16_t bicycle_config_track_draw_cap(void);
/**
 * @brief 自行车 config track point min dist m。
 */
double bicycle_config_track_point_min_dist_m(void);
/**
 * @brief 自行车 config nav off route m。
 */
double bicycle_config_nav_off_route_m(void);
/**
 * @brief 自行车 config nav sim speed kph。
 */
float bicycle_config_nav_sim_speed_kph(void);
/**
 * @brief 自行车 config set nav sim speed kph。
 */
void bicycle_config_set_nav_sim_speed_kph(float kph);
/** 当前配置所需的 BoardPSRAM bump arena 字节数。 */
size_t bicycle_config_track_arena_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_CONFIG_H */
