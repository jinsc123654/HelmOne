/**
 * @file vmap_route.h
 * @brief vmap route 模块。
 */

#ifndef VMAP_ROUTE_H
#define VMAP_ROUTE_H

#include "vmap_render.h"
#include "vmap_route_graph.h"
/* 只为 vmap_route_waypoint_t（多站行程航点）—— 行程叠层挂在路线对象上，
 * 定义见文件末尾 vmap_route_set_trip_pins()。 */
#include "vmap_route_trip.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vmap_view;

typedef struct vmap_route vmap_route_t;

typedef struct {
    double lon;
    double lat;
    int16_t ele_m;
} vmap_route_pt_t;

typedef struct {
    const char * map_dir;
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;
    const vmap_cell_id_t * region_path;
    uint32_t region_path_len;
    uint32_t region_begin;
    uint32_t max_regions;
} vmap_route_rolling_req_t;

typedef enum {
    VMAP_ROUTE_JOB_STANDARD = 0,
    VMAP_ROUTE_JOB_ROLLING,
} vmap_route_job_kind_t;

typedef enum {
    VMAP_MANEUVER_NONE = 0,
    VMAP_MANEUVER_START,
    VMAP_MANEUVER_STRAIGHT,
    VMAP_MANEUVER_SLIGHT_LEFT,
    VMAP_MANEUVER_LEFT,
    VMAP_MANEUVER_SHARP_LEFT,
    VMAP_MANEUVER_SLIGHT_RIGHT,
    VMAP_MANEUVER_RIGHT,
    VMAP_MANEUVER_SHARP_RIGHT,
    VMAP_MANEUVER_UTURN,
    VMAP_MANEUVER_ARRIVE,
} vmap_maneuver_kind_t;

/** @brief 仅在规划完成弹窗中计数（真实路口/街道变化，非同路弯折）。 */
#define VMAP_MANEUVER_F_PLAN_JUNCTION 0x01u

typedef struct {
    vmap_maneuver_kind_t kind;
    uint32_t node;
    double along_m;
    float turn_deg;
    const char * road_name;
    uint8_t flags;
} vmap_route_maneuver_t;

/**
 * @brief vmap route create。
 * @return 0 成功，负 errno 失败。
 */
vmap_route_t * vmap_route_create(vmap_route_graph_t * graph, struct vmap_view * view);
/**
 * @brief vmap route destroy。
 */
void vmap_route_destroy(vmap_route_t * route);
/**
 * @brief vmap route bind view。
 */
void vmap_route_bind_view(vmap_route_t * route, struct vmap_view * view);
/**
 * @brief vmap route set graph。
 */
void vmap_route_set_graph(vmap_route_t * route, vmap_route_graph_t * graph);

bool vmap_route_plan(vmap_route_t * route, double from_lon, double from_lat,
    double to_lon, double to_lat);
/** @brief 可在非 UI 线程调用：计算折线而不触碰 vmap_route_t。 */
bool vmap_route_compute(vmap_route_graph_t * graph,
    double from_lon, double from_lat, double to_lon, double to_lat,
    vmap_route_pt_t * pts, uint32_t * pt_count, double * total_m,
    vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count);
/** @brief 同 vmap_route_compute，但固定 src/dst 图节点（UINT32_MAX = 吸附）。 */
bool vmap_route_compute_pins(vmap_route_graph_t * graph,
    double from_lon, double from_lat, double to_lon, double to_lat,
    uint32_t pin_src, uint32_t pin_dst, vmap_route_pt_t * pts,
    uint32_t * pt_count, double * total_m, vmap_route_maneuver_t * maneuvers,
    uint32_t maneuver_cap, uint32_t * maneuver_count);
/** @brief 起终点落在不同 VREG 区域时为 true（需走廊规划器）。 */
bool vmap_route_corridor_needed(const char * map_dir, double from_lon,
    double from_lat, double to_lon, double to_lat);
/** @brief 跨 VPK 分段规划；同区域委托 vmap_route_compute。 */
bool vmap_route_compute_corridor(const char * map_dir, double from_lon,
    double from_lat, double to_lon, double to_lat, vmap_route_pt_t * pts,
    uint32_t * pt_count, double * total_m, vmap_route_maneuver_t * maneuvers,
    uint32_t maneuver_cap, uint32_t * maneuver_count);
/** @brief 沿预计算区域路径滚动走廊细化（path 为 NULL 时用 BFS）。 */
bool vmap_route_compute_rolling(const vmap_route_rolling_req_t * req,
    vmap_route_pt_t * pts, uint32_t * pt_count, double * total_m,
    vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count, uint32_t * out_regions_planned);
void vmap_route_apply(vmap_route_t * route, const vmap_route_pt_t * pts,
    uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count);
/** @brief 从骑手投影位置起替换路线折线。 */
bool vmap_route_splice_forward(vmap_route_t * route, double lon, double lat,
    const vmap_route_pt_t * new_pts, uint32_t new_n, double new_total_m,
    const vmap_route_maneuver_t * new_man, uint32_t new_man_n);
/**
 * @brief vmap route clear。
 */
void vmap_route_clear(vmap_route_t * route);
/**
 * @brief vmap route is active。
 * @return 请求的值。
 */
bool vmap_route_is_active(const vmap_route_t * route);

/**
 * @brief vmap route point count。
 * @return 请求的值。
 */
uint32_t vmap_route_point_count(const vmap_route_t * route);
/**
 * @brief vmap route points。
 */
const vmap_route_pt_t * vmap_route_points(const vmap_route_t * route);
/**
 * @brief vmap route total length m。
 */
double vmap_route_total_length_m(const vmap_route_t * route);
/** @brief Remaining ascent from along_m; -1 if the polyline has no elevation. */
int32_t vmap_route_remain_gain_m(const vmap_route_t * route, double along_m);
/** @brief Sample remaining-route elevations into out[0..cap). Returns filled count. */
uint32_t vmap_route_profile_ele(const vmap_route_t * route, double along_m,
    int16_t * out, uint32_t cap);
/** @brief Sample elevations along [from_m, to_m] into out[0..cap). */
uint32_t vmap_route_profile_ele_span(const vmap_route_t * route,
    double from_m, double to_m, int16_t * out, uint32_t cap);
/**
 * @brief vmap route maneuver count。
 * @return 请求的值。
 */
uint32_t vmap_route_maneuver_count(const vmap_route_t * route);
/**
 * @brief vmap route maneuvers。
 */
const vmap_route_maneuver_t * vmap_route_maneuvers(const vmap_route_t * route);
/** @brief 规划弹窗用的交叉口（排除同名道路弯折）。 */
uint32_t vmap_route_maneuver_plan_junction_count(
    const vmap_route_maneuver_t * maneuvers, uint32_t count);
/**
 * @brief vmap route plan junction count。
 * @return 请求的值。
 */
uint32_t vmap_route_plan_junction_count(const vmap_route_t * route);
/**
 * @brief vmap maneuver kind label。
 */
const char * vmap_maneuver_kind_label(vmap_maneuver_kind_t kind);

/** @brief 将路线叠加裁剪为从折线投影位置开始。 */
void vmap_route_set_progress_pos(vmap_route_t * route, double lon, double lat);
/** @brief 使用已匹配的线段更新路线进度，避免再次扫描整条路线。 */
void vmap_route_set_progress_match(vmap_route_t * route, double lon, double lat,
    uint32_t seg, double seg_t);

/** @brief GPS 在首图节点之前时沿距离调整的折线。 */
double vmap_route_effective_along_m(const vmap_route_t * route,
    double polyline_along_m);

/** @brief 把导航线盖进画布（底图之上、标签/REC 之下）。 */
void vmap_route_paint_to_canvas(vmap_route_t * route, vmap_canvas_t * canvas,
    const struct vmap_view * view);

/**
 * @brief 多站行程：把还没到的航点画成小圆点（与导航线同层）。
 *
 * 路线对象只装**当前一段**（`vmap_route_apply()` 整体替换），所以地图上那条线
 * 天生止于当前站 —— 骑手看不出后面还有几站。航点坐标是行程层的东西、不属于
 * 任何一段几何，挂在这里只是因为这一层已经拿得到 canvas 与 view，能跟着
 * canvas 像素一起平移/重盖，不必额外开一条叠层通道。
 *
 * @param wps     行程航点（lon/lat），`count` 内有效。
 * @param count   航点数；0 表示不画（清空）。
 * @param cur_leg 当前站下标；该下标及其之前的点已到过，不画。
 */
void vmap_route_set_trip_pins(vmap_route_t * route,
    const vmap_route_waypoint_t * wps, uint32_t count, uint32_t cur_leg);

/** @brief 轨迹回放：整段用已骑红色，并画起终点。 */
void vmap_route_set_review(vmap_route_t * route, bool review);

/** @brief 将当前路线已骑部分（0 .. along_m）追加到历史折线。 */
void vmap_route_capture_ridden_history(vmap_route_t * route, double along_m,
    double lon, double lat);
/**
 * @brief vmap route clear history。
 */
void vmap_route_clear_history(vmap_route_t * route);

/** @brief 释放 Dijkstra 规划器 scratch（kumm）；导航结束且图卸载时调用。 */
void vmap_route_release_plan_scratch(void);

/**
 * @brief 规划线程让出 CPU（约 4 ms），给 UI 刷新和 idle/IWDT 心跳。
 */
void vmap_route_plan_yield(void);

/** @brief 请求中止当前规划（单击停止）；worker / Dijkstra 在 yield 点退出。 */
void vmap_route_plan_abort(void);

/** @brief 提交新规划前清除中止标志。 */
void vmap_route_plan_abort_clear(void);

/** @brief 当前规划是否已被中止。 */
bool vmap_route_plan_aborted(void);

/**
 * @brief 规划步进计数：`vmap_route_plan_yield()` 每被调用一次 +1。
 *
 * 用来判断"规划器还在动" —— 提交出去的 job 若在超时窗口里这个数不变，就是原地卡住
 * （worker 线程没了，或算法卡在某一步）；见 `vmap_route_worker.c` 的提交前自愈。
 */
uint32_t vmap_route_plan_steps(void);

/** @brief 设置/清除导航起点标记（绿点）。valid=false 即清除。 */
void vmap_route_set_origin(vmap_route_t * route, bool valid, double lon,
    double lat);

/**
 * @brief 最近一次规划的起/终点吸附距离（米）。
 *
 * 用来判断"这张图是不是这对点该用的图"：换格后 UI 可能还拿着上一格的图，吸附会被迫
 * 拉远（>VMAP_ROUTE_SNAP_M），算出来的路线方向都可能反。worker 据此改走 corridor。
 */
void vmap_route_last_snap_m(double * src_m, double * dst_m);

/**
 * @brief 最近一次规划步进的时刻（ticks）。
 *
 * 与 `vmap_route_plan_steps()` 配对：worker 侧用"最后进度时刻"判卡死 ——
 * 规划器每 ~4 ms yield 一次，所以只要它还在动，这个时刻就一直在更新。
 */
uint32_t vmap_route_plan_progress_ticks(void);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_ROUTE_H */
