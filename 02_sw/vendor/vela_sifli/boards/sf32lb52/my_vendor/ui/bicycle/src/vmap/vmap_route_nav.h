/**
 * @file vmap_route_nav.h
 * @brief vmap route_nav 模块。
 */

#ifndef VMAP_ROUTE_NAV_H
#define VMAP_ROUTE_NAV_H

#include "vmap_config.h"
#include "vmap_route.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    /** 到达锁存：一旦进入到达区就不再撤销（骑过终点后 remain_m 会重新变大，
     *  不锁存就会出现"骑过终点→报逆行→重规划→起点终点吸到同一节点→规划失败"
     *  的死循环）。apply 新路线时清零。 */
    bool arrived;
    double off_route_m;
    double remain_m;
    double along_m;
    double dist_to_maneuver_m;
    float turn_rel_deg;
    float eta_s;
    uint32_t maneuver_idx;
    vmap_maneuver_kind_t next_kind;
    const char * next_road;
} vmap_route_nav_state_t;

typedef enum {
    VMAP_ROUTE_REROUTE_NONE = 0,
    /** @brief 沿规划方向反骑过久。 */
    VMAP_ROUTE_REROUTE_OPPOSITE,
    /** @brief 过路口后偏航（累计）。 */
    VMAP_ROUTE_REROUTE_JUNCTION_ACCUM,
    /** @brief 过路口后远离路径（>= 2 倍阈值）。 */
    VMAP_ROUTE_REROUTE_JUNCTION_FAR,
} vmap_route_reroute_reason_t;

typedef struct vmap_route_nav vmap_route_nav_t;

/**
 * @brief vmap route nav create。
 * @return 0 成功，负 errno 失败。
 */
vmap_route_nav_t * vmap_route_nav_create(vmap_route_t * route);
/**
 * @brief vmap route nav destroy。
 */
void vmap_route_nav_destroy(vmap_route_nav_t * nav);

bool vmap_route_nav_plan_to(vmap_route_nav_t * nav,
    double from_lon, double from_lat, double to_lon, double to_lat);
void vmap_route_nav_apply(vmap_route_nav_t * nav, const vmap_route_pt_t * pts,
    uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count);
/**
 * @brief vmap route nav stop。
 */
void vmap_route_nav_stop(vmap_route_nav_t * nav);
/**
 * @brief vmap route nav reset track。
 */
void vmap_route_nav_reset_track(vmap_route_nav_t * nav);

/**
 * @brief vmap route nav set off route threshold。
 */
void vmap_route_nav_set_off_route_threshold(vmap_route_nav_t * nav, double meters);
/** @brief 锁存的偏航原因；非待处理时为 VMAP_ROUTE_REROUTE_NONE，读取后清除。 */
vmap_route_reroute_reason_t vmap_route_nav_take_reroute(vmap_route_nav_t * nav);
/**
 * @brief vmap route reroute reason brief。
 */
const char * vmap_route_reroute_reason_brief(vmap_route_reroute_reason_t reason);

void vmap_route_nav_update(vmap_route_nav_t * nav, double lon, double lat,
    float course_deg, float speed_kph);
/**
 * @brief vmap route nav state。
 */
const vmap_route_nav_state_t * vmap_route_nav_state(const vmap_route_nav_t * nav);

/** @brief 剩余距离在到达区内时为 true。 */
static inline bool vmap_route_nav_is_arrived(const vmap_route_nav_state_t * st)
{
    return st && st->active
        && (st->arrived || st->remain_m <= VMAP_ROUTE_ARRIVE_M);
}

/** @brief 到达后骑手仍在移动时结束导航会话（清除路线叠加层）。 */
static inline bool vmap_route_nav_should_auto_stop(const vmap_route_nav_state_t * st,
    float speed_kph)
{
    return vmap_route_nav_is_arrived(st)
        && speed_kph > VMAP_ROUTE_END_MIN_SPEED_KPH;
}

#ifdef __cplusplus
}
#endif

#endif /* VMAP_ROUTE_NAV_H */
