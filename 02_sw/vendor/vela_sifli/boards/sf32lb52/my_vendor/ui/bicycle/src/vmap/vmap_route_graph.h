/**
 * @file vmap_route_graph.h
 * @brief vmap route_graph 模块。
 */

#ifndef VMAP_ROUTE_GRAPH_H
#define VMAP_ROUTE_GRAPH_H

#include "vmap_format.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t from;
    uint32_t to;
    uint32_t length_cm;
    uint8_t road_class;
    uint8_t flags;
    uint32_t name_off; /* VGRF_NAME_NONE if unnamed */
} vmap_graph_edge_t;

typedef struct vmap_route_graph vmap_route_graph_t;

/**
 * @brief vmap route graph load。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_graph_load(vmap_route_graph_t ** out, const char * path);
/** @brief 从 <map_dir>/map.idx + 共享 p*.vpk 加载 VGRF（旧 VIDX）。 */
bool vmap_route_graph_load_from_map_dir(vmap_route_graph_t ** out, const char * map_dir);
/** @brief 从区域 rNNN.vpk 加载 VGRF。 */
bool vmap_route_graph_load_from_region(vmap_route_graph_t ** out,
    const char * map_dir, vmap_cell_id_t region_id);
/**
 * @brief vmap route graph unload。
 * @return 0 成功，负 errno 失败。
 */
void vmap_route_graph_unload(vmap_route_graph_t * g);

/**
 * Region-graph LRU cache (planner/worker thread only).
 *
 * acquire_region() returns a cached parsed graph or loads+caches one; the
 * returned graph MUST be returned with release_region() (never unload()).
 * Repeated acquires of the same region within a corridor plan and across
 * consecutive reroutes hit RAM instead of re-parsing the VGRF from flash.
 * region_cache_clear() frees idle (unreferenced) cached graphs; call it when
 * navigation stops or the map directory changes.
 */
vmap_route_graph_t * vmap_route_graph_acquire_region(const char * map_dir,
    vmap_cell_id_t region_id);
/**
 * @brief vmap route graph release region。
 */
void vmap_route_graph_release_region(vmap_route_graph_t * g);
/**
 * @brief vmap route graph region cache clear。
 */
void vmap_route_graph_region_cache_clear(void);

/**
 * @brief vmap route graph node count。
 * @return 请求的值。
 */
uint32_t vmap_route_graph_node_count(const vmap_route_graph_t * g);
/**
 * @brief vmap route graph edge count。
 * @return 请求的值。
 */
uint32_t vmap_route_graph_edge_count(const vmap_route_graph_t * g);
void vmap_route_graph_node_lonlat(const vmap_route_graph_t * g, uint32_t idx,
    double * lon, double * lat);
/** @brief True when the loaded VGRF has a trailing ELEV section. */
bool vmap_route_graph_has_elev(const vmap_route_graph_t * g);
/** @brief Node elevation in metres; INT16_MIN if unknown / no ELEV. */
int16_t vmap_route_graph_node_ele_m(const vmap_route_graph_t * g, uint32_t idx);
/** @brief Nearest-node elevation within snap radius; INT16_MIN if unknown. */
int16_t vmap_route_graph_sample_ele_m(const vmap_route_graph_t * g,
    double lon, double lat);
/**
 * @brief Decode edge i into out (v2 overlay or v3/v4 18-byte record).
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_route_graph_edge(const vmap_route_graph_t * g, uint32_t i,
    vmap_graph_edge_t * out);
/** @brief Neighbour of node along edge; UINT32_MAX if node is not an endpoint. */
uint32_t vmap_route_graph_edge_other(const vmap_graph_edge_t * e, uint32_t node);
const uint32_t * vmap_route_graph_adj_edges(const vmap_route_graph_t * g,
    uint32_t node, uint32_t * out_count);
/**
 * @brief vmap route graph strings。
 */
const char * vmap_route_graph_strings(const vmap_route_graph_t * g);
const char * vmap_route_graph_edge_name(const vmap_route_graph_t * g,
    const vmap_graph_edge_t * e);

/**
 * @brief vmap route graph edge cost。
 */
uint32_t vmap_route_graph_edge_cost(const vmap_graph_edge_t * e);
/** @brief 节点至少有一条可通行出边时为 true。 */
bool vmap_route_graph_node_is_routable(const vmap_route_graph_t * g,
    uint32_t node);
uint32_t vmap_route_graph_nearest_node(const vmap_route_graph_t * g,
    double lon, double lat, double max_m);
/** @brief max_m 内最多 max_k 个最近节点，按距离升序。 */
uint32_t vmap_route_graph_snap_candidates(const vmap_route_graph_t * g,
    double lon, double lat, double max_m,
    uint32_t * out_nodes, uint32_t max_k);
/** @brief 到最近图节点的距离（米）；无节点时节点 ID 为 UINT32_MAX。 */
double vmap_route_graph_nearest_distance_m(const vmap_route_graph_t * g,
    double lon, double lat, uint32_t * out_node);
/**
 * Snap with tiered radius expansion, then global-nearest fallback up to
 * VMAP_ROUTE_SNAP_MAX_M. Returns candidate count; *used_radius_m is the
 * tier that matched (or actual nearest distance for fallback).
 */
uint32_t vmap_route_graph_snap_end(const vmap_route_graph_t * g,
    double lon, double lat, uint32_t * out_nodes, uint32_t max_k,
    double * used_radius_m);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_ROUTE_GRAPH_H */
