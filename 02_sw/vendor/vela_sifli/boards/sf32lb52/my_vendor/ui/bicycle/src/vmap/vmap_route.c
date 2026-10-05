/**
 * @file vmap_route.c
 * @brief vmap route 模块。
 */

#include "vmap_route.h"
#include "vmap_style.h"   /* 夜间（outdoor）路线改亮绿 ⇒ 需要当前地图风格 */

#include "vmap_alloc.h"
#include "vmap_route_log.h"
#include "vmap_config.h"
#include "vmap_geo.h"
#include "vmap_render.h"
#include "vmap_view.h"
#include "vmap_format.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef VMAP_ROUTE_YIELD_US
#  define VMAP_ROUTE_YIELD_US 4000
#endif

static atomic_bool g_plan_abort;
/* 最近一次规划的吸附距离（米）。给 worker 判断"这张图是不是这对点该用的图"：
 * 骑行者换格后 UI 可能还拿着上一格的图，吸附被迫拉远（实测 dest=800m），算出来的
 * 路线会出发就掉头。 */
static double g_last_snap_src_m;
static double g_last_snap_dst_m;
static atomic_uint g_plan_steps;
static atomic_uint g_plan_progress_ticks;

/*
 * 最近段搜索的段号提示（性能）。
 *
 * 实测 `[vperf] cyc_nav` = 26~35 ms/帧，而那是**每帧**在全部航点上重跑一遍
 * 双精度"点到线段"搜索（约 0.45 ms/点：69 点 ≈ 31 ms，点更多按线性涨）。
 * 稳态下骑行者就在上一次分割点附近，所以缓存段号、只搜它前后各
 * VMAP_ROUTE_SEG_WINDOW 段。
 *
 * **自校验**：窗口内最近距离若大于 VMAP_ROUTE_SEG_SNAP_M，说明人不在这个窗口
 * 附近（重规划 / 换路 / 跳到别处），立刻退回全量搜索；几何变了（点数不同）
 * 也退回全量。所以退不到"用错段"上 —— 最坏情况就是退化成原来的行为。
 * 同一份几何同时只有一个 route 在用，故文件级静态即可。
 */
#define VMAP_ROUTE_SEG_WINDOW  24u
#define VMAP_ROUTE_SEG_SNAP_M  40.0

static uint32_t s_paint_seg_hint;
static uint32_t s_paint_hint_pts;

/**
 * @brief 规划线程让出 CPU（约 4 ms），给 UI 刷新和 idle/IWDT 心跳。
 */
void vmap_route_plan_yield(void)
{
    atomic_fetch_add_explicit(&g_plan_steps, 1u, memory_order_relaxed);
    atomic_store_explicit(&g_plan_progress_ticks, (unsigned)clock_systime_ticks(),
        memory_order_relaxed);
    usleep(VMAP_ROUTE_YIELD_US);
}

/** @brief 规划步进计数（见 vmap_route.h）。 */
uint32_t vmap_route_plan_steps(void)
{
    return atomic_load_explicit(&g_plan_steps, memory_order_relaxed);
}

/** @brief 最近一次规划的起/终点吸附距离（米，见 vmap_route.h）。 */
void vmap_route_last_snap_m(double * src_m, double * dst_m)
{
    if (src_m) {
        *src_m = g_last_snap_src_m;
    }
    if (dst_m) {
        *dst_m = g_last_snap_dst_m;
    }
}

/** @brief 最近一次规划步进的时刻（ticks）—— 判"规划器还在动"（见 vmap_route.h）。 */
uint32_t vmap_route_plan_progress_ticks(void)
{
    return atomic_load_explicit(&g_plan_progress_ticks, memory_order_relaxed);
}

void vmap_route_plan_abort(void)
{
    atomic_store_explicit(&g_plan_abort, true, memory_order_release);
}

void vmap_route_plan_abort_clear(void)
{
    atomic_store_explicit(&g_plan_abort, false, memory_order_release);
}

bool vmap_route_plan_aborted(void)
{
    return atomic_load_explicit(&g_plan_abort, memory_order_acquire);
}

/**
 * @brief vmap route set progress pos。
 */
/* vmap_route_set_origin(): 定义见下面 struct vmap_route 之后。 */

void vmap_route_set_progress_pos(vmap_route_t * route, double lon, double lat);

struct vmap_route {
    vmap_route_graph_t * graph;
    vmap_view_t * view;
    vmap_route_pt_t pts[VMAP_ROUTE_MAX_PTS];
    uint32_t pt_count;
    double total_m;
    vmap_route_maneuver_t maneuvers[VMAP_ROUTE_MAX_MANEUVERS];
    uint32_t maneuver_count;
    bool active;
    bool progress_valid;
    /** 是否曾经给过进度位置：重规划换几何后要用它把红色段重新算出来。 */
    bool progress_pos_seen;
    bool origin_valid;       /* 导航起点标记（绿点）是否有效 */
    double origin_lon;
    double origin_lat;
    uint32_t progress_seg;
    double anchor_lon;
    double anchor_lat;
    double progress_lon;
    double progress_lat;
    vmap_route_pt_t history_pts[VMAP_ROUTE_MAX_PTS];
    uint32_t history_count;
    bool review;
    /* 多站行程的剩余航点（只画点，不参与几何）。见 set_trip_pins()。 */
    vmap_route_waypoint_t trip_pins[VMAP_ROUTE_TRIP_MAX_WAYPOINTS];
    uint32_t trip_pin_n;
    uint32_t trip_pin_cur;
};
/* 设置/清除"导航起点"标记（绿点）。nav 起航时调一次，重规划不要动它 —— 否则
 * 绿点会跟着骑行者跑、被箭头压住（用户现场：看不到起点绿点）。 */
void vmap_route_set_origin(vmap_route_t * route, bool valid, double lon,
    double lat)
{
    if (!route) {
        return;
    }

    route->origin_valid = valid;
    route->origin_lon = lon;
    route->origin_lat = lat;
}



typedef struct {
    uint32_t * dist;
} dijkstra_ctx_t;

/*
 * Planner scratch — kumm PSRAM (malloc); kept across jobs when graph size unchanged.
 */
typedef struct {
    uint32_t * dist;
    uint8_t * closed;
    uint32_t * heap;
    uint32_t * prev;
    uint32_t node_cap;
    uint32_t heap_cap;
} route_plan_scratch_t;

static route_plan_scratch_t g_plan_scratch;
static dijkstra_ctx_t g_dij;

static void route_plan_scratch_release(void)
{
    vmap_free(g_plan_scratch.dist);
    vmap_free(g_plan_scratch.closed);
    vmap_free(g_plan_scratch.heap);
    vmap_free(g_plan_scratch.prev);
    memset(&g_plan_scratch, 0, sizeof(g_plan_scratch));
}

static bool route_plan_scratch_ensure(uint32_t n, uint32_t ecount)
{
    const uint32_t heap_cap = n + (ecount / 4u) + 64u;

    if (g_plan_scratch.node_cap >= n && g_plan_scratch.heap_cap >= heap_cap
        && g_plan_scratch.dist != NULL && g_plan_scratch.closed != NULL
        && g_plan_scratch.heap != NULL && g_plan_scratch.prev != NULL) {
        return true;
    }

    route_plan_scratch_release();

    g_plan_scratch.dist = (uint32_t *)vmap_malloc((size_t)n * sizeof(uint32_t));
    g_plan_scratch.closed = (uint8_t *)vmap_malloc((size_t)n);
    g_plan_scratch.heap = (uint32_t *)vmap_malloc((size_t)heap_cap * sizeof(uint32_t));
    g_plan_scratch.prev = (uint32_t *)vmap_malloc((size_t)n * sizeof(uint32_t));
    if (!g_plan_scratch.dist || !g_plan_scratch.closed || !g_plan_scratch.heap
        || !g_plan_scratch.prev) {
        VMAP_NAV_ERR("route scratch alloc failed n=%u heap=%u", n, heap_cap);
        route_plan_scratch_release();
        return false;
    }

    g_plan_scratch.node_cap = n;
    g_plan_scratch.heap_cap = heap_cap;
    VMAP_NAV_OUT("scratch psram nodes=%u heap=%u", n, heap_cap);
    return true;
}

static uint32_t dijkstra_key(uint32_t u)
{
    if (!g_dij.dist || u >= g_plan_scratch.node_cap) {
        return UINT32_MAX;
    }
    return g_dij.dist[u];
}

static void dijkstra_heap_swap(uint32_t * a, uint32_t * b)
{
    const uint32_t t = *a;

    *a = *b;
    *b = t;
}

static void dijkstra_heap_sift_up(uint32_t * heap, uint32_t idx)
{
    while (idx > 0) {
        const uint32_t parent = (idx - 1u) / 2u;
        const uint32_t k_idx = dijkstra_key(heap[idx]);
        const uint32_t k_parent = dijkstra_key(heap[parent]);

        if (k_idx >= k_parent) {
            break;
        }
        dijkstra_heap_swap(&heap[idx], &heap[parent]);
        idx = parent;
    }
}

static void dijkstra_heap_sift_down(uint32_t * heap, uint32_t heap_sz, uint32_t idx)
{
    for (;;) {
        uint32_t smallest = idx;
        const uint32_t left = idx * 2u + 1u;
        const uint32_t right = left + 1u;

        if (left < heap_sz && dijkstra_key(heap[left]) < dijkstra_key(heap[smallest])) {
            smallest = left;
        }
        if (right < heap_sz && dijkstra_key(heap[right]) < dijkstra_key(heap[smallest])) {
            smallest = right;
        }
        if (smallest == idx) {
            break;
        }
        dijkstra_heap_swap(&heap[idx], &heap[smallest]);
        idx = smallest;
    }
}

static bool dijkstra_heap_push(uint32_t * heap, uint32_t cap, uint32_t * heap_sz,
    uint32_t node)
{
    if (node >= g_plan_scratch.node_cap || *heap_sz >= cap) {
        return false;
    }
    heap[*heap_sz] = node;
    dijkstra_heap_sift_up(heap, *heap_sz);
    (*heap_sz)++;
    return true;
}

static uint32_t dijkstra_heap_pop(uint32_t * heap, uint32_t * heap_sz)
{
    const uint32_t root = heap[0];

    (*heap_sz)--;
    if (*heap_sz > 0) {
        heap[0] = heap[*heap_sz];
        dijkstra_heap_sift_down(heap, *heap_sz, 0);
    }
    return root;
}

static uint32_t route_drop_shared_src(uint32_t * src, uint32_t n_src,
    const uint32_t * dst, uint32_t n_dst)
{
    uint32_t w = 0;
    uint32_t i;
    uint32_t j;

    for (i = 0; i < n_src; i++) {
        bool shared = false;

        for (j = 0; j < n_dst; j++) {
            if (src[i] == dst[j]) {
                shared = true;
                break;
            }
        }
        if (!shared) {
            src[w++] = src[i];
        }
    }
    return w;
}

static bool route_search_targets(const vmap_route_graph_t * graph,
    const uint32_t * sources, uint32_t n_src,
    const uint32_t * targets, uint32_t n_dst,
    uint32_t * prev_out, uint32_t * out_src, uint32_t * out_dst,
    uint32_t * reachable_out)
{
    const uint32_t n = vmap_route_graph_node_count(graph);
    const uint32_t ecount = vmap_route_graph_edge_count(graph);
    uint32_t * dist = g_plan_scratch.dist;
    uint8_t * closed = g_plan_scratch.closed;
    uint32_t * heap = g_plan_scratch.heap;
    const uint32_t heap_cap = g_plan_scratch.heap_cap;
    uint32_t heap_sz = 0;
    uint32_t heap_peak = 0;
    bool ok = false;
    uint32_t reachable = 0;

    if (!graph || n == 0 || !sources || n_src == 0 || !targets || n_dst == 0
        || !prev_out || !out_src || !out_dst || !dist || !closed || !heap) {
        return false;
    }

    g_dij.dist = dist;

    for (uint32_t i = 0; i < n; i++) {
        dist[i] = UINT32_MAX;
        prev_out[i] = UINT32_MAX;
        closed[i] = 0;
    }

    for (uint32_t si = 0; si < n_src; si++) {
        const uint32_t s = sources[si];

        if (s >= n) {
            continue;
        }
        if (dist[s] == UINT32_MAX) {
            dist[s] = 0;
            prev_out[s] = UINT32_MAX;
            (void)dijkstra_heap_push(heap, heap_cap, &heap_sz, s);
            if (heap_sz > heap_peak) {
                heap_peak = heap_sz;
            }
        }
    }

    while (heap_sz > 0) {
        const uint32_t u = dijkstra_heap_pop(heap, &heap_sz);
        uint32_t adj_cnt;
        const uint32_t * adj;

        if (u >= n || closed[u]) {
            continue;
        }
        closed[u] = 1;
        reachable++;
        if ((reachable & 31u) == 0u) {
            vmap_route_plan_yield();
            if (vmap_route_plan_aborted()) {
                ok = false;
                goto done;
            }
        }

        for (uint32_t ti = 0; ti < n_dst; ti++) {
            if (targets[ti] < n && u == targets[ti]) {
                uint32_t cur = u;

                *out_dst = u;
                while (cur != UINT32_MAX && cur < n
                    && prev_out[cur] != UINT32_MAX) {
                    cur = prev_out[cur];
                }
                *out_src = cur;
                ok = true;
                goto done;
            }
        }

        if (dist[u] == UINT32_MAX) {
            continue;
        }

        adj = vmap_route_graph_adj_edges(graph, u, &adj_cnt);
        if (!adj || adj_cnt == 0) {
            continue;
        }

        for (uint32_t ai = 0; ai < adj_cnt; ai++) {
            const uint32_t ei = adj[ai];
            vmap_graph_edge_t e;
            uint32_t v;
            uint32_t w;
            uint32_t nd;

            if (ei >= ecount) {
                continue;
            }
            if (!vmap_route_graph_edge(graph, ei, &e)) {
                continue;
            }
            v = vmap_route_graph_edge_other(&e, u);
            w = vmap_route_graph_edge_cost(&e);
            if (v >= n || w == UINT32_MAX || closed[v]) {
                continue;
            }
            if (w > UINT32_MAX - dist[u]) {
                continue;
            }
            nd = dist[u] + w;
            if (nd < dist[v]) {
                dist[v] = nd;
                prev_out[v] = u;
                (void)dijkstra_heap_push(heap, heap_cap, &heap_sz, v);
                if (heap_sz > heap_peak) {
                    heap_peak = heap_sz;
                }
            }
        }
    }

done:
    if (ok && *out_dst < n) {
        VMAP_NAV_OUT("Dijkstra ok expanded=%lu g_cost=%u src_node=%u dst_node=%u heap_peak=%u/%u",
            (unsigned long)reachable, dist[*out_dst], *out_src, *out_dst,
            heap_peak, heap_cap);
    } else {
        VMAP_NAV_OUT("Dijkstra fail expanded=%lu/%u nodes heap_peak=%u/%u",
            (unsigned long)reachable, n, heap_peak, heap_cap);
    }
    if (reachable_out) {
        *reachable_out = reachable;
    }
    return ok;
}

static bool route_find_edge(const vmap_route_graph_t * graph, uint32_t from,
    uint32_t to, vmap_graph_edge_t * out_edge)
{
    uint32_t adj_cnt;
    const uint32_t * adj;
    const uint32_t ecount = vmap_route_graph_edge_count(graph);

    if (!graph || !out_edge || from >= vmap_route_graph_node_count(graph)) {
        return false;
    }

    adj = vmap_route_graph_adj_edges(graph, from, &adj_cnt);
    if (!adj) {
        return false;
    }

    for (uint32_t ai = 0; ai < adj_cnt; ai++) {
        const uint32_t ei = adj[ai];
        vmap_graph_edge_t e;

        if (ei >= ecount) {
            continue;
        }
        if (!vmap_route_graph_edge(graph, ei, &e)) {
            continue;
        }
        if (vmap_route_graph_edge_other(&e, from) == to) {
            *out_edge = e;
            return true;
        }
    }
    return false;
}

static void route_log_graph_node(const vmap_route_graph_t * graph,
    const char * tag, uint32_t node)
{
    double lon = 0.0;
    double lat = 0.0;

    if (!graph || node >= vmap_route_graph_node_count(graph)) {
        return;
    }
    vmap_route_graph_node_lonlat(graph, node, &lon, &lat);
    VMAP_NAV_LOG("%s node=%u (%.7f,%.7f) routable=%d",
        tag, node, lon, lat,
        vmap_route_graph_node_is_routable(graph, node) ? 1 : 0);
}

static void route_log_snap_cands(const char * end_label,
    const vmap_route_graph_t * graph, const uint32_t * cands, uint32_t n,
    double radius_m)
{
    VMAP_NAV_LOG("snap %s: pool=%.0fm cands=%u", end_label, radius_m, n);
    for (uint32_t i = 0; i < n && i < VMAP_ROUTE_SNAP_K; i++) {
        char tag[24];

        snprintf(tag, sizeof(tag), "  %s[%u]", end_label, (unsigned)i);
        route_log_graph_node(graph, tag, cands[i]);
    }
}

static void route_log_maneuvers(const vmap_route_maneuver_t * maneuvers,
    uint32_t count)
{
    VMAP_NAV_OUT("maneuvers: %u steps", count);
    for (uint32_t i = 0; i < count; i++) {
        const vmap_route_maneuver_t * m = &maneuvers[i];
        const char * road = (m->road_name && m->road_name[0] != '\0')
            ? m->road_name : "-";

        VMAP_NAV_OUT("  man[%u] %s along=%.0fm turn=%.0f road=%s",
            i, vmap_maneuver_kind_label(m->kind), m->along_m, (double)m->turn_deg,
            road);
        if ((i & 3u) == 3u) {
            vmap_route_plan_yield();
        }
    }
}

static uint32_t route_graph_out_degree(const vmap_route_graph_t * graph,
    uint32_t node)
{
    uint32_t adj_cnt;

    if (!graph) {
        return 0;
    }
    (void)vmap_route_graph_adj_edges(graph, node, &adj_cnt);
    return adj_cnt;
}

static vmap_maneuver_kind_t route_classify_turn(float turn_deg)
{
    const float a = fabsf(turn_deg);

    if (a >= 150.0f) {
        return VMAP_MANEUVER_UTURN;
    }
    if (a >= 100.0f) {
        return turn_deg < 0.0f ? VMAP_MANEUVER_SHARP_LEFT : VMAP_MANEUVER_SHARP_RIGHT;
    }
    if (a >= 35.0f) {
        return turn_deg < 0.0f ? VMAP_MANEUVER_LEFT : VMAP_MANEUVER_RIGHT;
    }
    if (a >= 15.0f) {
        return turn_deg < 0.0f ? VMAP_MANEUVER_SLIGHT_LEFT
                               : VMAP_MANEUVER_SLIGHT_RIGHT;
    }
    return VMAP_MANEUVER_STRAIGHT;
}

static void route_maneuver_set_plan_flags(vmap_route_maneuver_t * m,
    vmap_maneuver_kind_t kind, uint32_t out_degree,
    const char * in_name, const char * out_name)
{
    m->flags = 0;
    if (kind == VMAP_MANEUVER_START || kind == VMAP_MANEUVER_ARRIVE) {
        return;
    }
    if (out_degree >= 3u) {
        m->flags |= VMAP_MANEUVER_F_PLAN_JUNCTION;
        return;
    }
    if (in_name && out_name && in_name[0] != '\0' && out_name[0] != '\0'
        && strcmp(in_name, out_name) != 0) {
        m->flags |= VMAP_MANEUVER_F_PLAN_JUNCTION;
    }
}

static void route_maneuver_put(vmap_route_maneuver_t * m,
    vmap_maneuver_kind_t kind, uint32_t node, double along_m, float turn_deg,
    const char * road_name, uint32_t out_degree, const char * in_name,
    const char * out_name)
{
    m->kind = kind;
    m->node = node;
    m->along_m = along_m;
    m->turn_deg = turn_deg;
    m->road_name = road_name;
    route_maneuver_set_plan_flags(m, kind, out_degree, in_name, out_name);
}

static bool route_should_emit_maneuver(float turn_deg, uint32_t out_degree)
{
    const float a = fabsf(turn_deg);

    if (a >= VMAP_ROUTE_MANEUVER_DEG) {
        return true;
    }
    if (out_degree >= 3u && a >= VMAP_ROUTE_MANEUVER_JUNCTION_DEG) {
        return true;
    }
    return false;
}

static uint32_t route_build_maneuvers(vmap_route_graph_t * graph,
    const uint32_t * path_nodes, uint32_t path_len,
    double from_lon, double from_lat,
    vmap_route_maneuver_t * maneuvers, uint32_t cap)
{
    uint32_t count = 0;
    double along = 0.0;

    if (!graph || !path_nodes || path_len < 2 || !maneuvers || cap == 0) {
        return 0;
    }

    if (cap > 0) {
        route_maneuver_put(&maneuvers[count], VMAP_MANEUVER_START, path_nodes[0],
            0.0, 0.0f, NULL, 0, NULL, NULL);
        if (path_len >= 2) {
            vmap_graph_edge_t e;

            if (route_find_edge(graph, path_nodes[0], path_nodes[1], &e)) {
                maneuvers[count].road_name =
                    vmap_route_graph_edge_name(graph, &e);
            }
        }
        count++;
    }

    /* Turn at the first junction (path start node) using approach from plan origin. */
    if (path_len >= 2 && count < cap) {
        const uint32_t n0 = path_nodes[0];
        const uint32_t n1 = path_nodes[1];
        double lon0;
        double lat0;
        double lon1;
        double lat1;
        float in_brg;
        float out_brg;
        float turn_deg;
        vmap_graph_edge_t out_edge;
        const char * out_name = NULL;
        uint32_t out_degree;
        vmap_maneuver_kind_t kind;

        vmap_route_graph_node_lonlat(graph, n0, &lon0, &lat0);
        vmap_route_graph_node_lonlat(graph, n1, &lon1, &lat1);
        in_brg = vmap_geo_bearing_deg(from_lon, from_lat, lon0, lat0);
        out_brg = vmap_geo_bearing_deg(lon0, lat0, lon1, lat1);
        turn_deg = vmap_geo_angle_delta_deg(in_brg, out_brg);
        out_degree = route_graph_out_degree(graph, n0);

        if (route_find_edge(graph, n0, n1, &out_edge)) {
            out_name = vmap_route_graph_edge_name(graph, &out_edge);
        }

        if (route_should_emit_maneuver(turn_deg, out_degree)) {
            kind = route_classify_turn(turn_deg);
            if (kind != VMAP_MANEUVER_STRAIGHT || out_degree >= 3u) {
                route_maneuver_put(&maneuvers[count], kind, n0, 0.0, turn_deg,
                    out_name, out_degree, NULL, out_name);
                count++;
            }
        } else if (out_degree >= 3u) {
            route_maneuver_put(&maneuvers[count], VMAP_MANEUVER_STRAIGHT, n0,
                0.0, turn_deg, out_name, out_degree, NULL, out_name);
            count++;
        }
    }

    for (uint32_t i = 1; i + 1 < path_len && count < cap; i++) {
        const uint32_t prev = path_nodes[i - 1];
        const uint32_t cur = path_nodes[i];
        const uint32_t next = path_nodes[i + 1];
        double lon0;
        double lat0;
        double lon1;
        double lat1;
        double lon2;
        double lat2;
        float in_brg;
        float out_brg;
        float turn_deg;
        vmap_graph_edge_t out_edge;
        const char * out_name = NULL;
        const char * in_name = NULL;
        vmap_graph_edge_t in_edge;
        uint32_t out_degree;
        vmap_maneuver_kind_t kind;

        vmap_route_graph_node_lonlat(graph, prev, &lon0, &lat0);
        vmap_route_graph_node_lonlat(graph, cur, &lon1, &lat1);
        along += vmap_geo_haversine_m(lon0, lat0, lon1, lat1);

        vmap_route_graph_node_lonlat(graph, next, &lon2, &lat2);
        in_brg = vmap_geo_bearing_deg(lon0, lat0, lon1, lat1);
        out_brg = vmap_geo_bearing_deg(lon1, lat1, lon2, lat2);
        turn_deg = vmap_geo_angle_delta_deg(in_brg, out_brg);
        out_degree = route_graph_out_degree(graph, cur);

        if (route_find_edge(graph, cur, next, &out_edge)) {
            out_name = vmap_route_graph_edge_name(graph, &out_edge);
        }
        if (route_find_edge(graph, prev, cur, &in_edge)) {
            in_name = vmap_route_graph_edge_name(graph, &in_edge);
        }

        if (!route_should_emit_maneuver(turn_deg, out_degree)) {
            if (in_name && out_name && in_name != out_name
                && strcmp(in_name, out_name) != 0
                && fabsf(turn_deg) >= VMAP_ROUTE_MANEUVER_JUNCTION_DEG) {
                /* name change at a bend */
            } else {
                continue;
            }
        }

        kind = route_classify_turn(turn_deg);
        if (kind == VMAP_MANEUVER_STRAIGHT && out_degree < 3u) {
            continue;
        }

        route_maneuver_put(&maneuvers[count], kind, cur, along, turn_deg,
            out_name, out_degree, in_name, out_name);
        count++;
    }

    if (count < cap && path_len >= 2) {
        double lon_a;
        double lat_a;
        double lon_b;
        double lat_b;

        vmap_route_graph_node_lonlat(graph, path_nodes[path_len - 2], &lon_a,
            &lat_a);
        vmap_route_graph_node_lonlat(graph, path_nodes[path_len - 1], &lon_b,
            &lat_b);
        along += vmap_geo_haversine_m(lon_a, lat_a, lon_b, lat_b);

        route_maneuver_put(&maneuvers[count], VMAP_MANEUVER_ARRIVE,
            path_nodes[path_len - 1], along, 0.0f, NULL, 0, NULL, NULL);
        count++;
    }

    return count;
}

static bool route_build_result(vmap_route_graph_t * graph, uint32_t src,
    uint32_t dst, uint32_t * prev, double from_lon, double from_lat,
    vmap_route_pt_t * pts, uint32_t * pt_count,
    double * total_m, vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count)
{
    uint32_t * path_nodes;
    uint32_t path_len = 0;
    uint32_t cur = dst;
    bool ok;

    if (!graph || !prev || !pts || !pt_count || !total_m) {
        return false;
    }

    *pt_count = 0;
    *total_m = 0.0;
    if (maneuver_count) {
        *maneuver_count = 0;
    }

    path_nodes = (uint32_t *)vmap_malloc((size_t)VMAP_ROUTE_MAX_PTS * sizeof(*path_nodes));
    if (!path_nodes) {
        return false;
    }

    while (cur != UINT32_MAX && path_len < VMAP_ROUTE_MAX_PTS) {
        path_nodes[path_len++] = cur;
        if (cur == src) {
            break;
        }
        if (cur >= vmap_route_graph_node_count(graph)) {
            break;
        }
        cur = prev[cur];
    }

    if (path_len < 2) {
        VMAP_NAV_WARN("compute: trivial path src=%u dst=%u len=%u",
            src, dst, path_len);
        vmap_free(path_nodes);
        return false;
    }

    for (uint32_t i = path_len; i > 0; i--) {
        const uint32_t idx = path_nodes[i - 1];

        vmap_route_graph_node_lonlat(graph, idx, &pts[*pt_count].lon,
            &pts[*pt_count].lat);
        pts[*pt_count].ele_m = vmap_route_graph_node_ele_m(graph, idx);
        (*pt_count)++;
    }

    for (uint32_t i = 1; i < *pt_count; i++) {
        *total_m += vmap_geo_haversine_m(pts[i - 1].lon, pts[i - 1].lat,
            pts[i].lon, pts[i].lat);
    }

    if (maneuvers && maneuver_cap > 0 && maneuver_count) {
        uint32_t * ordered;
        uint32_t n_man;

        ordered = (uint32_t *)vmap_malloc((size_t)path_len * sizeof(*ordered));
        if (ordered) {
            for (uint32_t i = 0; i < path_len; i++) {
                ordered[i] = path_nodes[path_len - 1u - i];
            }
            n_man = route_build_maneuvers(graph, ordered, path_len,
                from_lon, from_lat, maneuvers, maneuver_cap);
            *maneuver_count = n_man;
            route_log_maneuvers(maneuvers, n_man);
            vmap_free(ordered);
        }
    }

    VMAP_NAV_OUT("path: graph_nodes=%u polyline_pts=%u length=%.0fm",
        path_len, *pt_count, *total_m);
    route_log_graph_node(graph, "path_start", path_nodes[path_len - 1u]);
    route_log_graph_node(graph, "path_end", path_nodes[0]);

    ok = *pt_count >= 2;
    vmap_free(path_nodes);
    return ok;
}

/**
 * @brief vmap maneuver kind label。
 */
const char * vmap_maneuver_kind_label(vmap_maneuver_kind_t kind)
{
    switch (kind) {
    case VMAP_MANEUVER_START:       return "出发";
    case VMAP_MANEUVER_STRAIGHT:    return "直行";
    case VMAP_MANEUVER_SLIGHT_LEFT: return "稍向左";
    case VMAP_MANEUVER_LEFT:        return "左转";
    case VMAP_MANEUVER_SHARP_LEFT:  return "急左转";
    case VMAP_MANEUVER_SLIGHT_RIGHT:return "稍向右";
    case VMAP_MANEUVER_RIGHT:       return "右转";
    case VMAP_MANEUVER_SHARP_RIGHT: return "急右转";
    case VMAP_MANEUVER_UTURN:       return "掉头";
    case VMAP_MANEUVER_ARRIVE:      return "到达";
    default:                        return "";
    }
}

/**
 * @brief vmap route create。
 * @return 0 成功，负 errno 失败。
 */
vmap_route_t * vmap_route_create(vmap_route_graph_t * graph, vmap_view_t * view)
{
    vmap_route_t * route = (vmap_route_t *)vmap_malloc(sizeof(*route));

    if (!route) {
        return NULL;
    }
    memset(route, 0, sizeof(*route));
    route->graph = graph;
    route->view = view;
    return route;
}

/**
 * @brief vmap route destroy。
 */
void vmap_route_destroy(vmap_route_t * route)
{
    vmap_free(route);
}

/**
 * @brief vmap route release plan scratch。
 */
void vmap_route_release_plan_scratch(void)
{
    route_plan_scratch_release();
}

/**
 * @brief vmap route bind view。
 */
void vmap_route_bind_view(vmap_route_t * route, vmap_view_t * view)
{
    if (route) {
        route->view = view;
    }
}

/**
 * @brief vmap route set graph。
 */
void vmap_route_set_graph(vmap_route_t * route, vmap_route_graph_t * graph)
{
    if (route) {
        route->graph = graph;
    }
}

/**
 * @brief vmap route plan。
 */
bool vmap_route_plan(vmap_route_t * route, double from_lon, double from_lat,
    double to_lon, double to_lat)
{
    vmap_route_pt_t * pts;
    vmap_route_maneuver_t maneuvers[VMAP_ROUTE_MAX_MANEUVERS];
    uint32_t n = 0;
    uint32_t n_man = 0;
    double total = 0.0;
    bool ok;

    if (!route || !route->graph) {
        return false;
    }

    pts = (vmap_route_pt_t *)vmap_malloc((size_t)VMAP_ROUTE_MAX_PTS * sizeof(*pts));
    if (!pts) {
        return false;
    }

    ok = vmap_route_compute(route->graph, from_lon, from_lat, to_lon, to_lat,
        pts, &n, &total, maneuvers, VMAP_ROUTE_MAX_MANEUVERS, &n_man);
    if (ok) {
        vmap_route_apply(route, pts, n, total, maneuvers, n_man);
    } else {
        vmap_route_clear(route);
    }
    vmap_free(pts);
    return ok;
}

/**
 * @brief vmap route compute pins。
 */
/* 把折线**末点**从"最近节点"拉到"请求点在该边上的投影"。
 *
 * 为什么需要：终点多半落在一段路的中间（显示瓦片里有这条路、有节点画出来的线），
 * 但路网图里节点只在端点/路口 —— 于是"取最近节点"会偏 100~200 m，屏幕上表现为
 * "到了却不判到达"（现场：偏航 43 m、终点 171 m 外）。投影到边上之后，路线末端
 * 就落在你点的位置附近（同一条路上，不会画出一条离开道路的直线）。
 * 只动末点、并按差值修正总长；投影不在段内（t 贴边）就不动。 */
static void route_pull_end_to_request(vmap_route_pt_t * pts, uint32_t n,
    double * total_m, double lon, double lat)
{
    double t = 0.0;
    double seg_old;
    double seg_new;
    vmap_route_pt_t last;

    if (!pts || !total_m || n < 2u) {
        return;
    }

    (void)vmap_geo_point_to_seg_m(lon, lat, pts[n - 2u].lon, pts[n - 2u].lat,
        pts[n - 1u].lon, pts[n - 1u].lat, &t);
    if (t <= 0.001 || t >= 0.999) {
        return;
    }

    seg_old = vmap_geo_haversine_m(pts[n - 2u].lon, pts[n - 2u].lat,
        pts[n - 1u].lon, pts[n - 1u].lat);
    last.lon = pts[n - 2u].lon + (pts[n - 1u].lon - pts[n - 2u].lon) * t;
    last.lat = pts[n - 2u].lat + (pts[n - 1u].lat - pts[n - 2u].lat) * t;
    seg_new = vmap_geo_haversine_m(pts[n - 2u].lon, pts[n - 2u].lat, last.lon,
        last.lat);
    if (seg_new < 1.0 || seg_new >= seg_old) {
        return;
    }

    pts[n - 1u] = last;
    *total_m += (seg_new - seg_old);
}

bool vmap_route_compute_pins(vmap_route_graph_t * graph,
    double from_lon, double from_lat, double to_lon, double to_lat,
    uint32_t pin_src, uint32_t pin_dst, vmap_route_pt_t * pts,
    uint32_t * pt_count, double * total_m, vmap_route_maneuver_t * maneuvers,
    uint32_t maneuver_cap, uint32_t * maneuver_count)
{
    uint32_t src_cands[VMAP_ROUTE_SNAP_K];
    uint32_t dst_cands[VMAP_ROUTE_SNAP_K];
    uint32_t n_src;
    uint32_t n_dst;
    double src_snap_m = 0.0;
    double dst_snap_m = 0.0;
    uint32_t src;
    uint32_t dst;
    uint32_t reachable = 0;
    uint32_t n_nodes;
    uint32_t n_edges;
    bool ok;

    if (!graph || !pts || !pt_count || !total_m) {
        VMAP_NAV_ERR("compute: bad args graph=%p pts=%p", (void *)graph, (void *)pts);
        return false;
    }

    *pt_count = 0;
    *total_m = 0.0;
    if (maneuver_count) {
        *maneuver_count = 0;
    }

    n_nodes = vmap_route_graph_node_count(graph);
    n_edges = vmap_route_graph_edge_count(graph);
    VMAP_NAV_OUT("compute: graph nodes=%u edges=%u", n_nodes, n_edges);
    {
        const double d = vmap_geo_haversine_m(from_lon, from_lat, to_lon, to_lat);

        if (d <= VMAP_ROUTE_ARRIVE_M) {
            VMAP_NAV_WARN("compute: already at dest d=%.0fm", d);
            return false;
        }
    }
    vmap_route_plan_yield();
    if (vmap_route_plan_aborted()) {
        return false;
    }
    if (!route_plan_scratch_ensure(n_nodes, n_edges)) {
        return false;
    }

    VMAP_NAV_OUT("compute: snap start (%.7f,%.7f)", from_lon, from_lat);
    if (pin_src != UINT32_MAX && pin_src < n_nodes
        && vmap_route_graph_node_is_routable(graph, pin_src)) {
        double slon;
        double slat;

        src_cands[0] = pin_src;
        n_src = 1;
        vmap_route_graph_node_lonlat(graph, pin_src, &slon, &slat);
        src_snap_m = vmap_geo_haversine_m(from_lon, from_lat, slon, slat);
        VMAP_NAV_LOG("compute: pin start node=%u", pin_src);
    } else {
        n_src = vmap_route_graph_snap_end(graph, from_lon, from_lat,
            src_cands, VMAP_ROUTE_SNAP_K, &src_snap_m);
    }

    VMAP_NAV_OUT("compute: snap dest (%.7f,%.7f)", to_lon, to_lat);
    if (pin_dst != UINT32_MAX && pin_dst < n_nodes
        && vmap_route_graph_node_is_routable(graph, pin_dst)) {
        double dlon;
        double dlat;

        dst_cands[0] = pin_dst;
        n_dst = 1;
        vmap_route_graph_node_lonlat(graph, pin_dst, &dlon, &dlat);
        dst_snap_m = vmap_geo_haversine_m(to_lon, to_lat, dlon, dlat);
        VMAP_NAV_LOG("compute: pin dest node=%u", pin_dst);
    } else {
        n_dst = vmap_route_graph_snap_end(graph, to_lon, to_lat,
            dst_cands, VMAP_ROUTE_SNAP_K, &dst_snap_m);
    }

    if (n_src == 0 || n_dst == 0) {
        double from_near = vmap_route_graph_nearest_distance_m(graph, from_lon,
            from_lat, NULL);
        double to_near = vmap_route_graph_nearest_distance_m(graph, to_lon,
            to_lat, NULL);

        if (n_src == 0 && n_dst == 0) {
            VMAP_NAV_WARN("compute: both ends off-road "
                "from=%.6f,%.6f nearest=%.0fm to=%.6f,%.6f nearest=%.0fm",
                from_lon, from_lat, from_near, to_lon, to_lat, to_near);
        } else if (n_src == 0) {
            VMAP_NAV_WARN("compute: start off-road "
                "from=%.6f,%.6f nearest=%.0fm (dest ok n_dst=%lu)",
                from_lon, from_lat, from_near, (unsigned long)n_dst);
        } else {
            VMAP_NAV_WARN("compute: dest off-road "
                "to=%.6f,%.6f nearest=%.0fm (start ok n_src=%lu)",
                to_lon, to_lat, to_near, (unsigned long)n_src);
        }
        route_plan_scratch_release();
        return false;
    }

    route_log_snap_cands("start", graph, src_cands, n_src, src_snap_m);
    route_log_snap_cands("dest", graph, dst_cands, n_dst, dst_snap_m);
    vmap_route_plan_yield();

    n_src = route_drop_shared_src(src_cands, n_src, dst_cands, n_dst);
    if (n_src == 0) {
        VMAP_NAV_WARN("compute: start/dest snap overlap d=%.0fm",
            vmap_geo_haversine_m(from_lon, from_lat, to_lon, to_lat));
        route_plan_scratch_release();
        return false;
    }

    g_last_snap_src_m = src_snap_m;
    g_last_snap_dst_m = dst_snap_m;

    if (src_snap_m > VMAP_ROUTE_SNAP_M || dst_snap_m > VMAP_ROUTE_SNAP_M) {
        VMAP_NAV_OUT("compute: snap expanded start=%.0fm dest=%.0fm",
            src_snap_m, dst_snap_m);
    }

    VMAP_NAV_OUT("compute: Dijkstra seeds src=%u dst_lead=%u",
        src_cands[0], dst_cands[0]);
    ok = route_search_targets(graph, src_cands, n_src, dst_cands, n_dst,
        g_plan_scratch.prev, &src, &dst, &reachable);
    if (ok) {
        ok = route_build_result(graph, src, dst, g_plan_scratch.prev,
            from_lon, from_lat, pts, pt_count, total_m, maneuvers, maneuver_cap,
            maneuver_count);

        /* 折线首尾补上**请求的起终点**：snap 只吸到最近的"图节点"，而这张图的节点很稀
         * （边可长达 500 m+），于是路线会在离终点最多半条边长的地方就断掉 —— 屏幕上
         * 就是"轨迹线缺一段/离路"（实测终点偏 167 m）。这里在首尾各接一小段到真实
         * 起终点，让线走到该走的位置；与节点重合（<1 m）时不加。 */
        if (ok && pts && pt_count && *pt_count >= 2u) {
            if (src_snap_m > 1.0 && src_snap_m <= VMAP_ROUTE_SNAP_MAX_M
                && *pt_count + 1u <= VMAP_ROUTE_MAX_PTS) {
                uint32_t k;

                for (k = *pt_count; k > 0u; k--) {
                    pts[k] = pts[k - 1u];
                }
                pts[0].lon = from_lon;
                pts[0].lat = from_lat;
                pts[0].ele_m = pts[1].ele_m;
                (*pt_count)++;
                *total_m += src_snap_m;
            }
            if (dst_snap_m > 1.0 && dst_snap_m <= VMAP_ROUTE_SNAP_MAX_M
                && *pt_count + 1u <= VMAP_ROUTE_MAX_PTS) {
                pts[*pt_count].lon = to_lon;
                pts[*pt_count].lat = to_lat;
                pts[*pt_count].ele_m = pts[*pt_count - 1u].ele_m;
                (*pt_count)++;
                *total_m += dst_snap_m;
            }
        }
        if (ok) {
            /* 末点投到请求点所在的边上（见 route_pull_end_to_request） */
            route_pull_end_to_request(pts, *pt_count, total_m, to_lon, to_lat);
            VMAP_NAV_OUT("compute: ok pts=%u man=%u len=%.0fm",
                *pt_count,
                maneuver_count ? *maneuver_count : 0u,
                *total_m);
        } else {
            VMAP_NAV_WARN("compute: build failed src=%u dst=%u", src, dst);
            ok = false;
        }
    } else {
        double slon = 0.0;
        double slat = 0.0;
        double dlon = 0.0;
        double dlat = 0.0;

        vmap_route_graph_node_lonlat(graph, src_cands[0], &slon, &slat);
        vmap_route_graph_node_lonlat(graph, dst_cands[0], &dlon, &dlat);
        VMAP_NAV_WARN("compute: no path lead_src=%u(%.6f,%.6f) routable=%d "
            "lead_dst=%u(%.6f,%.6f) reachable=%lu/%u",
            src_cands[0], slon, slat,
            vmap_route_graph_node_is_routable(graph, src_cands[0]) ? 1 : 0,
            dst_cands[0], dlon, dlat,
            (unsigned long)reachable, vmap_route_graph_node_count(graph));
    }
    return ok;
}

/**
 * @brief vmap route compute。
 */
bool vmap_route_compute(vmap_route_graph_t * graph,
    double from_lon, double from_lat, double to_lon, double to_lat,
    vmap_route_pt_t * pts, uint32_t * pt_count, double * total_m,
    vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count)
{
    return vmap_route_compute_pins(graph, from_lon, from_lat, to_lon, to_lat,
        UINT32_MAX, UINT32_MAX, pts, pt_count, total_m, maneuvers,
        maneuver_cap, maneuver_count);
}

/**
 * @brief vmap route apply。
 */
void vmap_route_apply(vmap_route_t * route, const vmap_route_pt_t * pts,
    uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count)
{
    if (!route) {
        return;
    }

    route->active = false;
    route->pt_count = 0;
    route->total_m = 0.0;
    route->maneuver_count = 0;

    if (!pts || pt_count < 2 || pt_count > VMAP_ROUTE_MAX_PTS) {
        VMAP_NAV_WARN("apply: rejected pts=%u", pt_count);
        return;
    }

    memcpy(route->pts, pts, (size_t)pt_count * sizeof(route->pts[0]));
    route->pt_count = pt_count;
    route->total_m = total_m;
    route->active = true;

    if (maneuvers && maneuver_count > 0) {
        if (maneuver_count > VMAP_ROUTE_MAX_MANEUVERS) {
            maneuver_count = VMAP_ROUTE_MAX_MANEUVERS;
        }
        memcpy(route->maneuvers, maneuvers,
            (size_t)maneuver_count * sizeof(route->maneuvers[0]));
        route->maneuver_count = maneuver_count;
    }

    VMAP_NAV_LOG("apply: active pts=%u man=%u len=%.0fm",
        pt_count, route->maneuver_count, total_m);
    route->progress_valid = false;
    if (route->progress_pos_seen) {
        /* 重规划换了几何：立刻用上一次的位置把"已骑段"重新标出来，
         * 否则 progress 一直是 invalid，绘制退化成全蓝（用户看到的就是这个）。 */
        vmap_route_set_progress_pos(route, route->progress_lon,
            route->progress_lat);
    }
    route->review = false;
}

/**
 * @brief vmap route splice forward。
 */
bool vmap_route_splice_forward(vmap_route_t * route, double lon, double lat,
    const vmap_route_pt_t * new_pts, uint32_t new_n, double new_total_m,
    const vmap_route_maneuver_t * new_man, uint32_t new_man_n)
{
    vmap_route_pt_t * merged;
    vmap_route_maneuver_t * merged_man;
    uint32_t merged_n = 0;
    uint32_t merged_man_n = 0;
    uint32_t start_i = 0;
    uint32_t i;
    double kept_m = 0.0;

    if (!route || !new_pts || new_n < 2 || new_n > VMAP_ROUTE_MAX_PTS) {
        return false;
    }

    /* Rare reroute path: these two arrays are ~50 KiB and must not live on
     * bicycle_ui's SRAM stack. vmap_malloc is the BoardPSRAM allocator. */
    merged = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(*merged));
    merged_man = (vmap_route_maneuver_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_MANEUVERS * sizeof(*merged_man));
    if (merged == NULL || merged_man == NULL) {
        vmap_free(merged);
        vmap_free(merged_man);
        return false;
    }

    vmap_route_set_progress_pos(route, lon, lat);

    for (i = 0; i <= route->progress_seg && i < route->pt_count; i++) {
        merged[merged_n++] = route->pts[i];
    }
    if (merged_n < VMAP_ROUTE_MAX_PTS) {
        merged[merged_n].lon = route->progress_lon;
        merged[merged_n].lat = route->progress_lat;
        merged[merged_n].ele_m = (int16_t)VGRF_ELE_UNKNOWN;
        merged_n++;
    }

    if (merged_n > 0 && new_n > 0) {
        const double d = vmap_geo_haversine_m(merged[merged_n - 1u].lon,
            merged[merged_n - 1u].lat, new_pts[0].lon, new_pts[0].lat);

        if (d < VMAP_ROUTE_STITCH_DEDUP_M) {
            start_i = 1;
        }
    }

    for (i = start_i; i < new_n && merged_n < VMAP_ROUTE_MAX_PTS; i++) {
        merged[merged_n++] = new_pts[i];
    }

    if (merged_n < 2) {
        vmap_free(merged);
        vmap_free(merged_man);
        return false;
    }

    for (i = 1; i < merged_n; i++) {
        kept_m += vmap_geo_haversine_m(merged[i - 1u].lon, merged[i - 1u].lat,
            merged[i].lon, merged[i].lat);
    }

    if (new_man && new_man_n > 0) {
        for (i = 0; i < new_man_n && merged_man_n < VMAP_ROUTE_MAX_MANEUVERS; i++) {
            if (new_man[i].kind == VMAP_MANEUVER_START) {
                continue;
            }
            merged_man[merged_man_n] = new_man[i];
            merged_man[merged_man_n].along_m += kept_m - new_total_m;
            merged_man_n++;
        }
    }

    vmap_route_apply(route, merged, merged_n, kept_m, merged_man, merged_man_n);
    vmap_route_set_progress_pos(route, lon, lat);
    vmap_free(merged);
    vmap_free(merged_man);
    return true;
}

/**
 * @brief vmap route set progress pos。
 */
void vmap_route_set_progress_match(vmap_route_t * route, double lon, double lat,
    uint32_t seg, double seg_t)
{
    if (!route || !vmap_route_is_active(route)
        || seg + 1u >= route->pt_count) {
        return;
    }

    if (seg_t < 0.0) {
        seg_t = 0.0;
    } else if (seg_t > 1.0) {
        seg_t = 1.0;
    }

    route->anchor_lon = lon;
    route->anchor_lat = lat;
    route->progress_seg = seg;
    route->progress_lon = route->pts[seg].lon
        + (route->pts[seg + 1u].lon - route->pts[seg].lon) * seg_t;
    route->progress_lat = route->pts[seg].lat
        + (route->pts[seg + 1u].lat - route->pts[seg].lat) * seg_t;
    route->progress_valid = true;
}

void vmap_route_set_progress_pos(vmap_route_t * route, double lon, double lat)
{
    const vmap_route_pt_t * pts;
    uint32_t n;
    uint32_t start;
    uint32_t best_seg = 0;
    double best_t = 0.0;
    double best_d = 1e18;

    if (!route || !vmap_route_is_active(route)) {
        return;
    }

    route->progress_pos_seen = true;

    pts = route->pts;
    n = route->pt_count;
    start = route->progress_valid && route->progress_seg > 1u
        ? route->progress_seg - 1u : 0u;

    for (uint32_t i = start; i + 1u < n; i++) {
        double t = 0.0;
        const double d = vmap_geo_point_to_seg_m(lon, lat,
            pts[i].lon, pts[i].lat, pts[i + 1u].lon, pts[i + 1u].lat, &t);

        if (d < best_d) {
            best_d = d;
            best_seg = i;
            best_t = t;
        }
    }

    vmap_route_set_progress_match(route, lon, lat, best_seg, best_t);
}

/**
 * @brief vmap route effective along m。
 */
double vmap_route_effective_along_m(const vmap_route_t * route,
    double polyline_along_m)
{
    double connector_m;

    if (!route || !route->progress_valid) {
        return polyline_along_m;
    }

    connector_m = vmap_geo_haversine_m(route->anchor_lon, route->anchor_lat,
        route->progress_lon, route->progress_lat);
    if (connector_m > 2.0 && polyline_along_m < connector_m + 5.0) {
        return polyline_along_m - connector_m;
    }
    return polyline_along_m;
}

static bool route_px_keep(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    bool force)
{
    const int32_t dx = x1 - x0;
    const int32_t dy = y1 - y0;
    const int32_t min2 = VMAP_ROUTE_DRAW_MIN_PX * VMAP_ROUTE_DRAW_MIN_PX;

    if (force) {
        return (dx != 0) || (dy != 0);
    }
    return (dx * dx + dy * dy) >= min2;
}

static void route_stroke_px(vmap_canvas_t * canvas, const vmap_point_t * pts,
    int32_t n, uint16_t rgb565)
{
    if (!canvas || !pts || n < 2) {
        return;
    }
    vmap_render_stroke_ribbon(canvas, pts, n, VMAP_ROUTE_LINE_W, rgb565);
}

static void route_append_geo(const vmap_view_t * view, vmap_point_t * out,
    int32_t * n, int32_t cap, double lon, double lat, bool force)
{
    int32_t x;
    int32_t y;
    int32_t cw;
    int32_t ch;
    int64_t max_jump2;

    if (!view || !out || !n || cap < 2) {
        return;
    }
    if (!vmap_view_geo_to_canvas_raw(view, lon, lat, &x, &y)) {
        return;
    }
    if (!vmap_view_canvas_point_in_pad(view, x, y, 64)) {
        return;
    }

    if (*n > 0) {
        const int32_t px = out[*n - 1].x;
        const int32_t py = out[*n - 1].y;

        if (!route_px_keep(px, py, x, y, force || (*n + 1 >= cap))) {
            return;
        }

        vmap_view_get_buffer_size(view, &cw, &ch);
        max_jump2 = (int64_t)(cw > ch ? cw : ch);
        max_jump2 *= max_jump2;
        {
            const int64_t dx = (int64_t)x - (int64_t)px;
            const int64_t dy = (int64_t)y - (int64_t)py;

            if (dx * dx + dy * dy > max_jump2) {
                return;
            }
        }
    }

    if (*n >= cap) {
        out[cap - 1].x = x;
        out[cap - 1].y = y;
        return;
    }
    out[*n].x = x;
    out[*n].y = y;
    (*n)++;
}

/* 运行方向标的间距与形状（屏幕像素；见 route_paint_geo_range 里的调用）。
 * 定位用**点序号**（每 N 个点一个）而不是累积屏幕距离 —— 后者依赖绘制范围起点，
 * 切页回来起点微动就会落在差几像素的位置，旧墨迹没人擦、看起来"每切一次多一个"
 * （用户 2026-09-25）。点序号与起点无关，重绘像素完全一致，天然幂等。
 * 点距约 13.6 m ⇒ 每 18 点约 245 m，折算到当前缩放约 48 px，观感与之前一致。 */
#define VMAP_ROUTE_CHEVRON_EVERY   18
#define VMAP_ROUTE_CHEVRON_ARM_PX  8
#define VMAP_ROUTE_CHEVRON_DEG     44.0

/** @brief 沿屏幕方向画一个黑色 "^"：两臂各偏 ±35°、长 5 px、线宽 2。 */
/**
 * @brief 方向指示（"^" 瓦片）的颜色：日光=**黑**（一直是这个），夜间=**橙**。
 *
 * @note 用户 2026-09-27 定：黑的 ^ 在夜间那份深色地图上完全看不见 ⇒ 夜间改橙 #FF6E12
 *       （RGB565 0xFB62）；日光保持原来的黑（浅色底上黑最清楚，也不必和蓝色路线较劲）。
 *       判据与路线取色同源（地图风格，outdoor = 夜间）。
 * @return RGB565。
 */
static uint16_t route_chevron_rgb(void)
{
    return (vmap_style_get_id() == VMAP_STYLE_OUTDOOR) ? (uint16_t)0xFB62u
                                                      : (uint16_t)0x0000u;
}

static void route_draw_chevron(vmap_canvas_t * canvas, int32_t x, int32_t y,
    double ang_rad)
{
    const double half = VMAP_ROUTE_CHEVRON_DEG * (3.14159265358979323846 / 180.0);
    int s;

    for (s = -1; s <= 1; s += 2) {
        const double a = ang_rad + 3.14159265358979323846 + (double)s * half;
        vmap_point_t arm[2];

        arm[0].x = x;
        arm[0].y = y;
        arm[1].x = (int32_t)((double)x + cos(a) * VMAP_ROUTE_CHEVRON_ARM_PX + 0.5);
        arm[1].y = (int32_t)((double)y + sin(a) * VMAP_ROUTE_CHEVRON_ARM_PX + 0.5);
        /* 方向指示（"^"）：日光黑、夜间橙 —— 见 route_chevron_rgb() 的注释。 */
        vmap_render_stroke_ribbon(canvas, arm, 2, 2, route_chevron_rgb());
    }
}

/**
 * @brief 导航线（未走过部分）的颜色：日光=蓝，夜间（深色地图）=**亮绿**。
 *
 * @note 用户 2026-09-27：夜间那份地图的水面也是蓝的，蓝色路线和水面糊在一起 ⇒ 夜间改亮绿。
 *       判据用**地图风格**（outdoor = 夜间深色地图，见 bicycle_pm_theme.c 的映射），
 *       与其它取色同源；日光仍走 `VMAP_ROUTE_LINE_RGB565`。
 * @return RGB565。
 */
static uint16_t route_line_rgb(void)
{
    /* #00E676（Material 亮绿）⇒ RGB565：r=0<<11 | g=(230>>2)<<5 | b=(118>>3) = 0x072E */
    return (vmap_style_get_id() == VMAP_STYLE_OUTDOOR) ? (uint16_t)0x072Eu
                                                      : VMAP_ROUTE_LINE_RGB565;
}

/* —— 路线/箭头的擦除记账 ——
 * 覆盖层是烘进常驻画布的：瓦片重贴会盖掉线（该补画），但**几何跳变**（GPX 窗口续载、
 * 跟车重锚、容器尺寸变化）会让旧位置的墨迹没人擦，留下残影（用户 2026-09-25 实测：
 * "每进一次地图就多烘一份"）。做法与路名标注一致：记下这一帧画过的屏幕框与分割点，
 * 下一次发现分割点**跳变**就把旧框并进脏区，让瓦片在那儿重贴一遍。
 * 只在跳变时发生（不是每拍），所以代价可控。 */
static lv_area_t s_route_box;
static int32_t   s_route_split_x;
static int32_t   s_route_split_y;
static bool      s_route_box_valid;

static void route_note_box(lv_area_t * box, int32_t x1, int32_t y1, int32_t x2,
    int32_t y2)
{
    if (x1 > x2) {
        const int32_t t = x1;

        x1 = x2;
        x2 = t;
    }
    if (y1 > y2) {
        const int32_t t = y1;

        y1 = y2;
        y2 = t;
    }
    box->x1 = (box->x1 < x1) ? box->x1 : x1;
    box->y1 = (box->y1 < y1) ? box->y1 : y1;
    box->x2 = (box->x2 > x2) ? box->x2 : x2;
    box->y2 = (box->y2 > y2) ? box->y2 : y2;
}

static void route_paint_geo_range(const vmap_view_t * view, vmap_canvas_t * canvas,
    const vmap_route_pt_t * pts, uint32_t begin, uint32_t end,
    double extra_lon, double extra_lat, bool extra_first, uint16_t rgb565)
{
    vmap_point_t px[256];
    int32_t n = 0;
    uint32_t i;

    if (!view || !canvas || !pts || end < 2u || begin + 1u > end) {
        return;
    }

    lv_area_t new_box = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
    const bool chevrons = extra_first;

    if (extra_first) {
        route_append_geo(view, px, &n, (int32_t)(sizeof(px) / sizeof(px[0])),
            extra_lon, extra_lat, true);
    }

    for (i = begin; i < end; i++) {
        const bool last = (i + 1u == end);
        const int32_t before = n;

        route_append_geo(view, px, &n, (int32_t)(sizeof(px) / sizeof(px[0])),
            pts[i].lon, pts[i].lat, last);

        if (n > before && n >= 2) {
            const int32_t k = n - 1;
            const double dx = (double)(px[k].x - px[k - 1].x);
            const double dy = (double)(px[k].y - px[k - 1].y);
            const double len = sqrt(dx * dx + dy * dy);

            /* 方向箭头与蓝线在**同一次绘制**里烘焙（用户 2026-09-25），位置按点序号取，
             * 与绘制范围起点无关 ⇒ 重绘幂等，切页回来不会再冒出一个箭头。 */
            if (chevrons && (i % VMAP_ROUTE_CHEVRON_EVERY) == 0u && len >= 1.0) {
                route_draw_chevron(canvas, px[k].x, px[k].y, atan2(dy, dx));
            }
            route_note_box(&new_box, px[k - 1].x, px[k - 1].y, px[k].x, px[k].y);
        }
    }

    route_stroke_px(canvas, px, n, rgb565);

    if (extra_first && n >= 2) {
        /* 分割点跳变判定：以路线起点（骑手位置投影）为准，阈值取半个屏宽 ——
         * 小于它的移动都属于跟车滚动，滚动脏条已经覆盖。 */
        const int32_t sx = px[0].x;
        const int32_t sy = px[0].y;
        int32_t jx = sx - s_route_split_x;
        int32_t jy = sy - s_route_split_y;

        if (jx < 0) {
            jx = -jx;
        }
        if (jy < 0) {
            jy = -jy;
        }
        if (s_route_box_valid && (jx > 120 || jy > 120)) {
            vmap_view_invalidate_canvas_area(view, s_route_box.x1, s_route_box.y1,
                s_route_box.x2, s_route_box.y2);
        }
        if (new_box.x2 >= new_box.x1) {
            s_route_box = new_box;
            s_route_box_valid = true;
        }
        s_route_split_x = sx;
        s_route_split_y = sy;
    }
}

static void route_history_append_pt(vmap_route_t * route, double lon, double lat)
{
    vmap_route_pt_t * dst;

    if (!route || route->history_count >= VMAP_ROUTE_MAX_PTS) {
        return;
    }
    if (route->history_count > 0u) {
        const vmap_route_pt_t * last = &route->history_pts[route->history_count - 1u];

        if (vmap_geo_haversine_m(last->lon, last->lat, lon, lat) < 2.0) {
            return;
        }
    }
    dst = &route->history_pts[route->history_count++];
    dst->lon = lon;
    dst->lat = lat;
    dst->ele_m = (int16_t)VGRF_ELE_UNKNOWN;
}

/**
 * @brief vmap route capture ridden history。
 */
void vmap_route_capture_ridden_history(vmap_route_t * route, double along_m,
    double lon, double lat)
{
    const vmap_route_pt_t * pts;
    uint32_t n;
    double acc = 0.0;
    uint32_t i;

    if (!route || !vmap_route_is_active(route) || along_m <= 0.0) {
        return;
    }

    if (route->progress_valid) {
        lon = route->progress_lon;
        lat = route->progress_lat;
    }

    pts = route->pts;
    n = route->pt_count;
    if (n < 2u) {
        route_history_append_pt(route, lon, lat);
        return;
    }

    if (route->history_count == 0u) {
        route_history_append_pt(route, pts[0].lon, pts[0].lat);
    }

    for (i = 0; i + 1u < n && acc < along_m - 0.5; i++) {
        const double seg_m = vmap_geo_haversine_m(pts[i].lon, pts[i].lat,
            pts[i + 1u].lon, pts[i + 1u].lat);

        if (acc + seg_m <= along_m + 0.5) {
            route_history_append_pt(route, pts[i + 1u].lon, pts[i + 1u].lat);
            acc += seg_m;
        } else {
            break;
        }
    }
    route_history_append_pt(route, lon, lat);
    VMAP_NAV_LOG("ridden history pts=%lu along=%.0fm",
        (unsigned long)route->history_count, along_m);
}

/**
 * @brief vmap route clear history。
 */
void vmap_route_clear_history(vmap_route_t * route)
{
    if (!route) {
        return;
    }
    route->history_count = 0;
}

/**
 * @brief vmap route clear。
 */
void vmap_route_clear(vmap_route_t * route)
{
    if (!route) {
        return;
    }
    route->active = false;
    route->pt_count = 0;
    route->total_m = 0.0;
    route->maneuver_count = 0;
    route->progress_valid = false;
    route->history_count = 0;
    route->review = false;
    /* 航点是行程层挂上来的，路线清空（含 nav_stop）必须一起撤，否则停表后
     * 地图上会留着几个没人认领的点。 */
    route->trip_pin_n = 0u;
    route->trip_pin_cur = 0u;
}

/**
 * @brief vmap route is active。
 * @return 请求的值。
 */
bool vmap_route_is_active(const vmap_route_t * route)
{
    return route && route->active && route->pt_count >= 2;
}

/**
 * @brief vmap route point count。
 * @return 请求的值。
 */
uint32_t vmap_route_point_count(const vmap_route_t * route)
{
    return route ? route->pt_count : 0;
}

/**
 * @brief vmap route points。
 */
const vmap_route_pt_t * vmap_route_points(const vmap_route_t * route)
{
    return route ? route->pts : NULL;
}

/**
 * @brief vmap route total length m。
 */
double vmap_route_total_length_m(const vmap_route_t * route)
{
    return route ? route->total_m : 0.0;
}

static bool route_ele_known(int16_t e)
{
    return e != (int16_t)VGRF_ELE_UNKNOWN;
}

static int16_t route_ele_on_seg(const vmap_route_pt_t * a, const vmap_route_pt_t * b,
    double seg, double along_on_seg)
{
    if (!a || !b) {
        return (int16_t)VGRF_ELE_UNKNOWN;
    }
    if (!route_ele_known(a->ele_m) && !route_ele_known(b->ele_m)) {
        return (int16_t)VGRF_ELE_UNKNOWN;
    }
    if (!route_ele_known(a->ele_m)) {
        return b->ele_m;
    }
    if (!route_ele_known(b->ele_m) || seg < 0.5) {
        return a->ele_m;
    }
    {
        double t = along_on_seg / seg;

        if (t < 0.0) {
            t = 0.0;
        } else if (t > 1.0) {
            t = 1.0;
        }
        return (int16_t)lround((double)a->ele_m
            + t * ((double)b->ele_m - (double)a->ele_m));
    }
}

static bool route_ele_at_along(const vmap_route_pt_t * pts, uint32_t n,
    double along_m, int16_t * out)
{
    double acc = 0.0;
    uint32_t i;

    if (!pts || n < 2 || !out) {
        return false;
    }
    if (along_m <= 0.0) {
        if (!route_ele_known(pts[0].ele_m)) {
            return false;
        }
        *out = pts[0].ele_m;
        return true;
    }
    for (i = 0; i + 1u < n; i++) {
        const double seg = vmap_geo_haversine_m(pts[i].lon, pts[i].lat,
            pts[i + 1u].lon, pts[i + 1u].lat);

        if (acc + seg >= along_m || i + 2u >= n) {
            if (!route_ele_known(pts[i].ele_m) || !route_ele_known(pts[i + 1u].ele_m)) {
                if (route_ele_known(pts[i].ele_m)) {
                    *out = pts[i].ele_m;
                    return true;
                }
                if (route_ele_known(pts[i + 1u].ele_m)) {
                    *out = pts[i + 1u].ele_m;
                    return true;
                }
                return false;
            }
            if (seg < 0.5) {
                *out = pts[i].ele_m;
            } else {
                const double t = (along_m - acc) / seg;
                const double clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);

                *out = (int16_t)lround((double)pts[i].ele_m
                    + clamped * ((double)pts[i + 1u].ele_m - (double)pts[i].ele_m));
            }
            return true;
        }
        acc += seg;
    }
    if (route_ele_known(pts[n - 1u].ele_m)) {
        *out = pts[n - 1u].ele_m;
        return true;
    }
    return false;
}

int32_t vmap_route_remain_gain_m(const vmap_route_t * route, double along_m)
{
    const vmap_route_pt_t * pts;
    uint32_t n;
    uint32_t i;
    double acc = 0.0;
    int16_t prev = (int16_t)VGRF_ELE_UNKNOWN;
    int32_t gain = 0;
    bool any = false;
    bool in_remain = false;

    if (!route || !vmap_route_is_active(route)) {
        return -1;
    }
    n = route->pt_count;
    pts = route->pts;
    if (n < 2) {
        return -1;
    }
    if (along_m < 0.0) {
        along_m = 0.0;
    }

    for (i = 0; i + 1u < n; i++) {
        const double seg = vmap_geo_haversine_m(pts[i].lon, pts[i].lat,
            pts[i + 1u].lon, pts[i + 1u].lat);
        const double end = acc + seg;

        if (!in_remain && (end > along_m + 0.5 || i + 2u >= n)) {
            double t_m = along_m - acc;

            in_remain = true;
            prev = route_ele_on_seg(&pts[i], &pts[i + 1u], seg, t_m);
            if (route_ele_known(prev)) {
                any = true;
            }
        }
        if (in_remain && route_ele_known(pts[i + 1u].ele_m)) {
            if (route_ele_known(prev) && pts[i + 1u].ele_m > prev) {
                gain += (int32_t)pts[i + 1u].ele_m - (int32_t)prev;
            }
            prev = pts[i + 1u].ele_m;
            any = true;
        }
        acc = end;
    }
    return any ? gain : -1;
}

uint32_t vmap_route_profile_ele_span(const vmap_route_t * route,
    double from_m, double to_m, int16_t * out, uint32_t cap)
{
    uint32_t filled = 0;
    uint32_t k;
    double span;
    double total;

    if (!route || !out || cap == 0 || !vmap_route_is_active(route)) {
        return 0;
    }
    total = route->total_m;
    if (from_m < 0.0) {
        from_m = 0.0;
    }
    if (to_m > total) {
        to_m = total;
    }
    if (to_m < from_m) {
        to_m = from_m;
    }
    span = to_m - from_m;
    if (span < 1.0) {
        int16_t e;

        if (route_ele_at_along(route->pts, route->pt_count, from_m, &e)) {
            out[0] = e;
            return 1;
        }
        return 0;
    }

    /* One polyline walk (O(N)), not haversine-per-sample (O(N*cap)). */
    {
        const vmap_route_pt_t * pts = route->pts;
        const uint32_t npts = route->pt_count;
        uint32_t i;
        double acc = 0.0;
        int16_t last_e = (int16_t)VGRF_ELE_UNKNOWN;
        bool have_e = false;

        k = 0;
        for (i = 0; i + 1u < npts && k < cap; i++) {
            const double seg = vmap_geo_haversine_m(pts[i].lon, pts[i].lat,
                pts[i + 1u].lon, pts[i + 1u].lat);
            const double end = acc + seg;

            while (k < cap) {
                const double sample_m = (cap == 1u) ? from_m
                    : from_m + span * ((double)k / (double)(cap - 1u));
                int16_t e;

                if (sample_m > end + 1e-4 && i + 2u < npts) {
                    break;
                }
                e = route_ele_on_seg(&pts[i], &pts[i + 1u], seg, sample_m - acc);
                if (route_ele_known(e)) {
                    last_e = e;
                    have_e = true;
                } else if (have_e) {
                    e = last_e;
                }
                out[k++] = e;
            }
            acc = end;
        }
        while (k < cap) {
            out[k++] = have_e ? last_e : (int16_t)VGRF_ELE_UNKNOWN;
        }
        filled = k;
    }
    if (filled == cap) {
        uint32_t known = 0;

        for (k = 0; k < cap; k++) {
            if (route_ele_known(out[k])) {
                known++;
            }
        }
        if (known == 0) {
            return 0;
        }
    }
    return filled;
}

uint32_t vmap_route_profile_ele(const vmap_route_t * route, double along_m,
    int16_t * out, uint32_t cap)
{
    if (!route) {
        return 0;
    }
    if (along_m < 0.0) {
        along_m = 0.0;
    }
    return vmap_route_profile_ele_span(route, along_m, route->total_m, out, cap);
}

/**
 * @brief vmap route maneuver count。
 * @return 请求的值。
 */
uint32_t vmap_route_maneuver_count(const vmap_route_t * route)
{
    return route ? route->maneuver_count : 0;
}

/**
 * @brief vmap route maneuver plan junction count。
 * @return 请求的值。
 */
uint32_t vmap_route_maneuver_plan_junction_count(
    const vmap_route_maneuver_t * maneuvers, uint32_t count)
{
    uint32_t n = 0;

    if (!maneuvers) {
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (maneuvers[i].flags & VMAP_MANEUVER_F_PLAN_JUNCTION) {
            n++;
        }
    }
    return n;
}

/**
 * @brief vmap route plan junction count。
 * @return 请求的值。
 */
uint32_t vmap_route_plan_junction_count(const vmap_route_t * route)
{
    if (!route) {
        return 0;
    }
    return vmap_route_maneuver_plan_junction_count(route->maneuvers,
        route->maneuver_count);
}

/**
 * @brief vmap route maneuvers。
 */
const vmap_route_maneuver_t * vmap_route_maneuvers(const vmap_route_t * route)
{
    return route ? route->maneuvers : NULL;
}

/* 起/中/终三色实心点：240x320 的屏 + 半透半反面板上，实心点比"环+芯"清楚得多。
 * 起点绿、中间点蓝（导航色）、终点红。 */
#define VMAP_PIN_RGB_START  ((uint16_t)0x07E0u)
#define VMAP_PIN_RGB_VIA    ((uint16_t)0x045Fu)
#define VMAP_PIN_RGB_DEST   ((uint16_t)0xF800u)

static void route_paint_marker(const vmap_view_t * view, vmap_canvas_t * canvas,
    double lon, double lat, uint16_t ring, uint16_t fill, int32_t r_ring,
    int32_t r_fill)
{
    int32_t x;
    int32_t y;

    if (!vmap_view_geo_to_canvas_raw(view, lon, lat, &x, &y)) {
        return;
    }

    vmap_render_fill_disc(canvas, x, y, r_ring, ring);
    vmap_render_fill_disc(canvas, x, y, r_fill, fill);
}

/**
 * @brief 多站行程：把还没到的航点画成小圆点。
 */
void vmap_route_set_trip_pins(vmap_route_t * route,
    const vmap_route_waypoint_t * wps, uint32_t count, uint32_t cur_leg)
{
    if (!route) {
        return;
    }

    route->trip_pin_n = 0u;
    route->trip_pin_cur = cur_leg;

    if (!wps || count == 0u) {
        return;
    }
    if (count > VMAP_ROUTE_TRIP_MAX_WAYPOINTS) {
        count = VMAP_ROUTE_TRIP_MAX_WAYPOINTS;
    }

    memcpy(route->trip_pins, wps, (size_t)count * sizeof(route->trip_pins[0]));
    route->trip_pin_n = count;
}

static void route_paint_trip_pins(const vmap_route_t * route,
    vmap_canvas_t * canvas, const vmap_view_t * view)
{
    const uint32_t last = route->trip_pin_n > 0u ? route->trip_pin_n - 1u : 0u;
    uint32_t i;

    /* 三类点要**一眼分得出来**（用户要求：起点 / 中间点 / 终点都做标记）：
     *   起点   = 绿环白芯（行程第一站，一直画，哪怕已经骑过去）
     *   中间点 = 白环导航色芯（小）
     *   终点   = 大一圈的导航色环白芯
     * 有名字的站点提示由 map_page 的到站通知负责（"XX 点到了"）。 */
    if (route->trip_pin_n >= 2u && route->trip_pin_cur > 0u) {
        route_paint_marker(view, canvas, route->trip_pins[0].lon,
            route->trip_pins[0].lat, VMAP_PIN_RGB_START, VMAP_PIN_RGB_START,
            5, 5);
    }

    for (i = route->trip_pin_cur; i < route->trip_pin_n; i++) {
        if (i == last) {
            route_paint_marker(view, canvas, route->trip_pins[i].lon,
                route->trip_pins[i].lat, VMAP_PIN_RGB_DEST, VMAP_PIN_RGB_DEST,
                6, 6);
        } else {
            route_paint_marker(view, canvas, route->trip_pins[i].lon,
                route->trip_pins[i].lat, VMAP_PIN_RGB_VIA, VMAP_PIN_RGB_VIA,
                4, 4);
        }
    }
}

/**
 * @brief 轨迹回放：整段用已骑红色，并画起终点。
 */
void vmap_route_set_review(vmap_route_t * route, bool review)
{
    if (route) {
        route->review = review;
    }
}

/**
 * @brief 把导航线盖进画布（底图之上、标签/REC 之下）。
 */
void vmap_route_paint_to_canvas(vmap_route_t * route, vmap_canvas_t * canvas,
    const vmap_view_t * view)
{
    const vmap_route_pt_t * pts;
    uint32_t n;

    if (!route || !canvas || !view) {
        return;
    }

    if (!vmap_route_is_active(route)) {
        /* 路线短暂 inactive（apply 清几何的那一瞬）也要保住点位 */
        /* 先把剩余航点点掉，而且**不看路线是否 active**：`vmap_route_apply()` 会先
         * 清零再填新几何，规划窗口里 route 会短暂 inactive —— 那时把后面几站擦掉
         * 再画回来就是闪一下。点位跟这一段几何无关，没理由跟着一起消失。
         * 顺序上先点位后线也没关系：要画的点都属于还没到的站，那里本来就没有线。 */
        if (route->trip_pin_n > route->trip_pin_cur) {
            route_paint_trip_pins(route, canvas, view);
        }
    
        /* 单点到点导航（没有行程航点）也要有起点/终点标记：直接用这条路线折线的
         * 首尾点 —— 用户的要求是"选点规划的起点/终点/中间点都在屏幕上看到"。 */
        if (route->origin_valid) {
        route_paint_marker(view, canvas, route->origin_lon, route->origin_lat,
            VMAP_PIN_RGB_START, VMAP_PIN_RGB_START, 5, 5);
    }

    if (route->trip_pin_n == 0u && route->pt_count >= 2u) {
            const uint32_t last = route->pt_count - 1u;
    
            route_paint_marker(view, canvas, route->pts[0].lon, route->pts[0].lat,
                VMAP_PIN_RGB_START, VMAP_PIN_RGB_START, 5, 5);
            route_paint_marker(view, canvas, route->pts[last].lon,
                route->pts[last].lat, VMAP_PIN_RGB_DEST, VMAP_PIN_RGB_DEST, 6, 6);
        }
        return;
    }

    pts = route->pts;
    n = route->pt_count;
    if (n < 2) {
        return;
    }

    if (route->review) {
        route_paint_geo_range(view, canvas, pts, 0, n, 0.0, 0.0, false,
            VMAP_ROUTE_RIDDEN_RGB565);
        route_paint_marker(view, canvas, pts[0].lon, pts[0].lat,
            (uint16_t)0xFFFFu, (uint16_t)0x07C0u, 6, 4);
        route_paint_marker(view, canvas, pts[n - 1u].lon, pts[n - 1u].lat,
            (uint16_t)0x0000u, (uint16_t)0xFFFFu, 6, 3);
        return;
    }

    /* (3) 箭头压过去的线刷灰：两段灰 —— ①已骑**历史**（跨重规划保留，
     * 见 vmap_route_capture_ridden_history，走过去的路线从几何里消失后靠它表示）
     * ②当前这条几何里"进度点之前"的那半段。灰线画在蓝线**下面**。 */
    if (route->history_count >= 2u) {
    }

    /* (2)(3) 只画"当前位置→终点"的蓝线，箭头压过去的那段刷灰。
     *
     * 分割点**在这里自己算**：拿保存的骑行者位置，在折线上找最近的那一段与其投影
     * （`vmap_geo_point_to_seg_m` 的 t）。不依赖 nav 更新路径是否调过
     * set_progress_pos —— 之前就是那个依赖导致"走过的线还是蓝的"。 */
    if (route->progress_pos_seen) {
        uint32_t seg = 0u;
        double best_t = 0.0;
        double best_d = 1e18;
        double plon = route->progress_lon;
        double plat = route->progress_lat;
        uint32_t lo = 0u;
        uint32_t hi = n - 2u;
        bool windowed = false;

        if (n == s_paint_hint_pts && s_paint_seg_hint <= n - 2u) {
            const uint32_t w = VMAP_ROUTE_SEG_WINDOW;

            lo = (s_paint_seg_hint > w) ? (s_paint_seg_hint - w) : 0u;
            hi = s_paint_seg_hint + w;
            if (hi > n - 2u) {
                hi = n - 2u;
            }
            windowed = true;
        }

        for (uint32_t i = lo; i <= hi; i++) {
            double t = 0.0;
            const double d = vmap_geo_point_to_seg_m(plon, plat,
                pts[i].lon, pts[i].lat, pts[i + 1u].lon, pts[i + 1u].lat, &t);

            if (d < best_d) {
                best_d = d;
                best_t = t;
                seg = i;
            }
        }

        if (windowed && best_d > VMAP_ROUTE_SEG_SNAP_M) {
            /* 窗口里没有可信的近段 ⇒ 全量重搜：换窗口 / 重规划 / 跳点都在这兜住，
             * 最坏退化成优化前的行为。 */
            for (uint32_t i = 0; i + 1u < n; i++) {
                double t = 0.0;
                const double d = vmap_geo_point_to_seg_m(plon, plat,
                    pts[i].lon, pts[i].lat, pts[i + 1u].lon, pts[i + 1u].lat, &t);

                if (d < best_d) {
                    best_d = d;
                    best_t = t;
                    seg = i;
                }
            }
        }

        s_paint_seg_hint = seg;
        s_paint_hint_pts = n;

        /* 诊断（限频 ~2 s）：把分割结果打出来，一眼看出是"没进度点"还是"分割算错" */
        {
            static uint32_t s_last_ms;
            const uint32_t now = (uint32_t)(lv_tick_get() & 0xFFFFFFFFu);

            if (now - s_last_ms > 2000u) {
                s_last_ms = now;
                VMAP_NAV_OUT("paint: seen=1 seg=%u/%u t=%.2f off=%.0fm hist=%u",
                    (unsigned)seg, (unsigned)n, best_t, best_d,
                    (unsigned)route->history_count);
            }
        }

        if (seg + 1u < n) {
            /* 投影点贴在本段中心附近时，把"本段"也整段刷灰，避免留下蓝色小尾巴 */
            if (best_t >= 0.5) {
            }
            route_paint_geo_range(view, canvas, pts, seg + 1u, n,
                plon, plat, true, route_line_rgb());
            goto paint_pins;   /* 别直接 return：点位在函数末尾画 */
        }
    }

    route_paint_geo_range(view, canvas, pts, 0, n, 0.0, 0.0, false,
        route_line_rgb());

paint_pins:
    /* 点位一律画在**最后**：线条在点下面（用户要求）。 */
    /* 先把剩余航点点掉，而且**不看路线是否 active**：`vmap_route_apply()` 会先
         * 清零再填新几何，规划窗口里 route 会短暂 inactive —— 那时把后面几站擦掉
         * 再画回来就是闪一下。点位跟这一段几何无关，没理由跟着一起消失。
         * 顺序上先点位后线也没关系：要画的点都属于还没到的站，那里本来就没有线。 */
        if (route->trip_pin_n > route->trip_pin_cur) {
            route_paint_trip_pins(route, canvas, view);
        }
    
        /* 单点到点导航（没有行程航点）也要有起点/终点标记：直接用这条路线折线的
         * 首尾点 —— 用户的要求是"选点规划的起点/终点/中间点都在屏幕上看到"。 */
        if (route->trip_pin_n == 0u && vmap_route_is_active(route)
            && route->pts && route->pt_count >= 2u) {
            const uint32_t last = route->pt_count - 1u;
    
            route_paint_marker(view, canvas, route->pts[0].lon, route->pts[0].lat,
                VMAP_PIN_RGB_START, VMAP_PIN_RGB_START, 5, 5);
            route_paint_marker(view, canvas, route->pts[last].lon,
                route->pts[last].lat, VMAP_PIN_RGB_DEST, VMAP_PIN_RGB_DEST, 6, 6);
        }
}
