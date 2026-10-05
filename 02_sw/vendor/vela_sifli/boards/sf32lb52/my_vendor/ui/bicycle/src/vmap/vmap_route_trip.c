/**
 * @file vmap_route_trip.c
 * @brief vmap route_trip 模块。
 */

#include "vmap_route_trip.h"

#include "vmap_geo.h"
#include "vmap_map_catalog.h"
#include "vmap_route_log.h"
#include "vmap_route_portal.h"
#include <string.h>

/**
 * @brief 重置行程状态。
 * @param trip 行程。
 */
void vmap_route_trip_reset(vmap_route_trip_t * trip)
{
    if (trip) {
        memset(trip, 0, sizeof(*trip));
    }
}

/**
 * @brief 设置途经点列表。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_trip_set_waypoints(vmap_route_trip_t * trip,
    const vmap_route_waypoint_t * wps, uint32_t count)
{
    if (!trip || !wps || count == 0 || count > VMAP_ROUTE_TRIP_MAX_WAYPOINTS) {
        return false;
    }

    memcpy(trip->wps, wps, (size_t)count * sizeof(trip->wps[0]));
    trip->wp_count = count;
    trip->leg_idx = 0;
    trip->region_path_len = 0;
    trip->region_refined = 0;
    trip->active = true;
    return true;
}

/**
 * @brief 当前 leg 终点。
 */
bool vmap_route_trip_leg_target(const vmap_route_trip_t * trip,
    double * lon, double * lat)
{
    return vmap_route_trip_waypoint_at(trip, trip ? trip->leg_idx : 0u, lon, lat)
        && trip && trip->active;
}

bool vmap_route_trip_waypoint_at(const vmap_route_trip_t * trip, uint32_t idx,
    double * lon, double * lat)
{
    if (!trip || idx >= trip->wp_count || !lon || !lat) {
        return false;
    }

    *lon = trip->wps[idx].lon;
    *lat = trip->wps[idx].lat;
    return true;
}

bool vmap_route_trip_fill_path(const char * map_dir, double from_lon,
    double from_lat, double to_lon, double to_lat, vmap_cell_id_t * path,
    uint32_t cap, uint32_t * out_len)
{
    vmap_cell_id_t start_rid;
    vmap_cell_id_t dest_rid;

    if (!map_dir || !path || cap == 0u || !out_len) {
        return false;
    }

    if (!vmap_map_catalog_load(map_dir) || !vmap_map_catalog_is_regional()) {
        VMAP_NAV_WARN("trip: catalog not regional");
        return false;
    }

    (void)vmap_route_portal_load(map_dir);

    if (!vmap_map_catalog_find_region(from_lon, from_lat, &start_rid)
        || !vmap_map_catalog_find_region(to_lon, to_lat, &dest_rid)) {
        VMAP_NAV_WARN("trip: leg outside catalog");
        return false;
    }

    if (start_rid == dest_rid) {
        path[0] = start_rid;
        *out_len = 1u;
        return true;
    }

    if (!vmap_map_catalog_region_path(start_rid, dest_rid, path, cap, out_len)) {
        VMAP_NAV_WARN("trip: no region path r%u->r%u",
            (unsigned)start_rid, (unsigned)dest_rid);
        return false;
    }
    return true;
}

/**
 * @brief 开始当前 leg 规划。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_trip_begin_leg(const char * map_dir, vmap_route_trip_t * trip,
    double from_lon, double from_lat)
{
    double to_lon;
    double to_lat;

    if (!map_dir || !trip || !trip->active || trip->leg_idx >= trip->wp_count) {
        return false;
    }

    to_lon = trip->wps[trip->leg_idx].lon;
    to_lat = trip->wps[trip->leg_idx].lat;
    trip->region_path_len = 0;
    trip->region_refined = 0;

    if (!vmap_route_trip_fill_path(map_dir, from_lon, from_lat, to_lon, to_lat,
            trip->region_path, VMAP_ROUTE_REGION_PATH_MAX,
            &trip->region_path_len)) {
        return false;
    }

    VMAP_NAV_OUT("trip: two-point leg %u/%u region_path len=%u",
        (unsigned)(trip->leg_idx + 1u), (unsigned)trip->wp_count,
        trip->region_path_len);
    return true;
}

/**
 * @brief 进入下一 leg。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_trip_advance_leg(vmap_route_trip_t * trip)
{
    if (!trip || !trip->active) {
        return false;
    }

    trip->leg_idx++;
    trip->region_path_len = 0;
    trip->region_refined = 0;
    return trip->leg_idx < trip->wp_count;
}

uint32_t vmap_route_trip_first_unreached(const vmap_route_waypoint_t * wps,
    uint32_t from, uint32_t count, double lon, double lat)
{
    uint32_t i = from;

    if (wps == NULL) {
        return count;
    }

    while (i < count
        && vmap_geo_haversine_m(lon, lat, wps[i].lon, wps[i].lat)
            <= VMAP_ROUTE_ARRIVE_M) {
        i++;
    }

    return i;
}

bool vmap_route_trip_skip_reached(vmap_route_trip_t * trip, double lon,
    double lat)
{
    uint32_t i;

    if (!trip || !trip->active || trip->wp_count == 0u) {
        return false;
    }

    i = vmap_route_trip_first_unreached(trip->wps, trip->leg_idx,
        trip->wp_count, lon, lat);

    if (i >= trip->wp_count) {
        /* 剩下的站全在脚下：停在最后一站，leg_idx 不能越界（begin_leg /
         * leg_target 都要求 leg_idx < wp_count）。 */
        trip->leg_idx = trip->wp_count - 1u;
        trip->region_path_len = 0;
        trip->region_refined = 0;
        return false;
    }

    if (i != trip->leg_idx) {
        VMAP_NAV_OUT("trip: skip reached legs %u -> %u of %u",
            (unsigned)trip->leg_idx, (unsigned)i, (unsigned)trip->wp_count);
        trip->leg_idx = i;
        trip->region_path_len = 0;
        trip->region_refined = 0;
    }

    return true;
}

/**
 * @brief 跳到指定途经点（其后路点保留，向前规划）。
 */
bool vmap_route_trip_set_leg(vmap_route_trip_t * trip, uint32_t idx)
{
    if (!trip || !trip->active || idx >= trip->wp_count) {
        return false;
    }

    trip->leg_idx = idx;
    trip->region_path_len = 0;
    trip->region_refined = 0;
    return true;
}

/**
 * @brief 全部 leg 是否完成。
 * @return 请求的值。
 */
bool vmap_route_trip_is_complete(const vmap_route_trip_t * trip)
{
    return !trip || !trip->active || trip->leg_idx >= trip->wp_count;
}

/**
 * @brief 当前 leg 下标。
 */
uint32_t vmap_route_trip_leg_index(const vmap_route_trip_t * trip)
{
    return trip ? trip->leg_idx : 0;
}

/**
 * @brief 途经点总数。
 * @return 请求的值。
 */
uint32_t vmap_route_trip_waypoint_count(const vmap_route_trip_t * trip)
{
    return trip ? trip->wp_count : 0;
}

/**
 * @brief 是否需要滚动细化。
 * @param remain_m 剩余距离（米）。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_trip_needs_refine(const vmap_route_trip_t * trip, double remain_m)
{
    if (!trip || !trip->active || trip->region_path_len == 0) {
        return false;
    }

    if (trip->region_refined >= trip->region_path_len) {
        return false;
    }

    if (trip->region_refined == 0) {
        return true;
    }

    return remain_m <= VMAP_ROUTE_REFINE_REMAIN_M;
}
