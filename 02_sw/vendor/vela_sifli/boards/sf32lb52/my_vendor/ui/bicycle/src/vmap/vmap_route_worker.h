/**
 * @file vmap_route_worker.h
 * @brief vmap route_worker 模块。
 */

#ifndef VMAP_ROUTE_WORKER_H
#define VMAP_ROUTE_WORKER_H

#include "vmap_route.h"
#include "vmap_route_graph.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*vmap_route_worker_done_cb)(void * user, bool ok,
    vmap_route_job_kind_t kind, uint32_t regions_planned,
    const vmap_route_pt_t * pts, uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count);

/**
 * @brief vmap route worker init。
 * @return 0 成功，负 errno 失败。
 */
void vmap_route_worker_init(void);
/**
 * @brief vmap route worker shutdown。
 */
void vmap_route_worker_shutdown(void);

/**
 * @brief vmap route worker busy。
 */
bool vmap_route_worker_busy(void);
/**
 * @brief vmap route worker cancel。
 */
void vmap_route_worker_cancel(void);

/** @brief 线程体是否还活着（false ⇒ 下次提交会把它重生）。 */
bool vmap_route_worker_alive(void);
/** @brief 已经自愈重生过几次（每次漏一块 PSRAM 旧栈，上限 3）。 */
int vmap_route_worker_revives(void);
/** @brief 规划步进总数（每次 yield +1）—— 判断"还在动"。 */
uint32_t vmap_route_worker_steps(void);
/** @brief 故障注入：制造"有 job 挂着却没人取"的卡死现场，验证自愈（仅测试用）。 */
void vmap_route_worker_test_stall(void);

/** @brief 单次规划（同区域图或走廊）。 */
bool vmap_route_worker_submit(const char * map_dir, vmap_route_graph_t * graph,
    double from_lon, double from_lat, double to_lon, double to_lat,
    vmap_route_worker_done_cb cb, void * user);

/** @brief 沿 region_path 滚动细化（长途跨区）。 */
bool vmap_route_worker_submit_rolling(const vmap_route_rolling_req_t * req,
    vmap_route_worker_done_cb cb, void * user);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_ROUTE_WORKER_H */
