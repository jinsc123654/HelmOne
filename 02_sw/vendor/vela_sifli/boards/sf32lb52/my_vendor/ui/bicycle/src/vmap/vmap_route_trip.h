/**
 * @file vmap_route_trip.h
 * @brief 多途经点行程：当前坐标不算，一次规划下一点与下下一点。
 *
 * 「当前坐标不算」还有一层含义：站在某站 `VMAP_ROUTE_ARRIVE_M` 内就等于那一站
 * 已经过掉（见 `vmap_route_trip_skip_reached()`）。选点或换站时都要跳过去，
 * 否则会拿一段 0 长度的 leg 去问规划器，它只会回一句 `already at dest`。
 */

#ifndef VMAP_ROUTE_TRIP_H
#define VMAP_ROUTE_TRIP_H

#include "vmap_config.h"
#include "vmap_format.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double lon;
    double lat;
} vmap_route_waypoint_t;

typedef struct {
    vmap_route_waypoint_t wps[VMAP_ROUTE_TRIP_MAX_WAYPOINTS];
    uint32_t wp_count;
    uint32_t leg_idx;
    vmap_cell_id_t region_path[VMAP_ROUTE_REGION_PATH_MAX];
    uint32_t region_path_len;
    uint32_t region_refined;
    bool active;
} vmap_route_trip_t;

/**
 * @brief 重置行程状态。
 * @param trip 行程。
 */
void vmap_route_trip_reset(vmap_route_trip_t * trip);
bool vmap_route_trip_set_waypoints(vmap_route_trip_t * trip,
    const vmap_route_waypoint_t * wps, uint32_t count);
bool vmap_route_trip_begin_leg(const char * map_dir, vmap_route_trip_t * trip,
    double from_lon, double from_lat);
bool vmap_route_trip_leg_target(const vmap_route_trip_t * trip,
    double * lon, double * lat);
bool vmap_route_trip_waypoint_at(const vmap_route_trip_t * trip, uint32_t idx,
    double * lon, double * lat);
bool vmap_route_trip_fill_path(const char * map_dir, double from_lon,
    double from_lat, double to_lon, double to_lat, vmap_cell_id_t * path,
    uint32_t cap, uint32_t * out_len);
/**
 * @brief 进入下一 leg。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_trip_advance_leg(vmap_route_trip_t * trip);

/**
 * @brief 从 @p from 起，第一个「人还没到」的站下标。
 *
 * 判据是 `VMAP_ROUTE_ARRIVE_M`。站就在脚下时给它规划一条 0 长度的 leg 毫无
 * 意义 —— `vmap_route_compute()` 会直接以 `already at dest` 拒绝，调用方拿到
 * 的只是「规划失败」，看不出真正原因是"你已经在这儿了"。
 *
 * @return 首个不在 (lon,lat) `VMAP_ROUTE_ARRIVE_M` 内的站下标；全都到了返回
 *         @p count（因此 `>= count` 即"这段行程没什么可导的"）。
 */
uint32_t vmap_route_trip_first_unreached(const vmap_route_waypoint_t * wps,
    uint32_t from, uint32_t count, double lon, double lat);

/**
 * @brief 把 leg 推到第一个「还没到」的站（吃掉已经在脚下的前置站）。
 *
 * 用在两处：`set_waypoints()` 之后、`begin_leg()` 之前（选点时就站在某站上，
 * 见 helm_menu 的选点提交），以及行程中途 `advance_leg()` 之后（两个站选得
 * 太近，新 leg 的目标同样在脚下）。
 *
 * 为了让 `leg_idx < wp_count` 这条不变量始终成立，全部站都到过时 leg_idx 停在
 * **最后一站**（此时 `vmap_route_trip_is_complete()` 仍为 false，但调用方的
 * 「还有下一站吗」判据 `leg+1 < total` 为 false，收尾逻辑会正常接管），并由
 * 返回值告知调用方。
 *
 * @return 还有没到过的站为 true；剩下的站全在脚下为 false。
 */
bool vmap_route_trip_skip_reached(vmap_route_trip_t * trip, double lon,
    double lat);

/**
 * @brief 跳到指定途经点（其后路点保留，向前规划）。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_trip_set_leg(vmap_route_trip_t * trip, uint32_t idx);
/**
 * @brief 全部 leg 是否完成。
 * @return 请求的值。
 */
bool vmap_route_trip_is_complete(const vmap_route_trip_t * trip);
/**
 * @brief 当前 leg 下标。
 */
uint32_t vmap_route_trip_leg_index(const vmap_route_trip_t * trip);
/**
 * @brief 途经点总数。
 * @return 请求的值。
 */
uint32_t vmap_route_trip_waypoint_count(const vmap_route_trip_t * trip);
/**
 * @brief 是否需要滚动细化。
 * @param remain_m 剩余距离（米）。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_trip_needs_refine(const vmap_route_trip_t * trip, double remain_m);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_ROUTE_TRIP_H */
