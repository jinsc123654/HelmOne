/**
 * @file vmap_route_nav.c
 * @brief vmap route_nav 模块。
 */

#include "vmap_route_nav.h"

#include "vmap_route_log.h"

#include "vmap_config.h"
#include "vmap_geo.h"
#include <nuttx/clock.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VMAP_NAV_MATCH_BACK_SEGS     8u
#define VMAP_NAV_MATCH_AHEAD_SEGS   64u
#define VMAP_NAV_MATCH_ACQUIRE_MAX 192u
#define VMAP_NAV_MATCH_ACQUIRE_HALF 96u
#define VMAP_NAV_MATCH_BACK_TOL_M   40.0
#define VMAP_NAV_MATCH_AHEAD_TOL_M 200.0
#define VMAP_NAV_MATCH_MAX_PENALTY 100.0
#define VMAP_NAV_MATCH_PROGRESS_W    2.0

struct vmap_route_nav {
    vmap_route_t * route;
    vmap_route_nav_state_t state;
    uint32_t maneuver_hint;
    uint32_t track_seg_hint;
    double track_seg_start_m;
    bool track_hint_valid;
    uint32_t last_passed_maneuver;
    double off_route_threshold_m;
    double opposite_accum_m;
    double junction_off_accum_m;
    double prev_lon;
    double prev_lat;
    bool has_prev;
    bool need_reroute;
    vmap_route_reroute_reason_t reroute_reason;
    /** 上次提出重规划的时刻（ticks）与"抑制中"日志的限频时刻 —— 见
     *  VMAP_NAV_REROUTE_MIN_INTERVAL_MS：偏航判定在稀几何下会每帧成立。 */
    uint32_t last_reroute_tick;
    uint32_t reroute_suppress_log_tick;
};
/* 偏航重规划节流：允许就记录时刻并返回 true；否则按秒限频打日志并返回 false。 */
static bool nav_reroute_cooldown_ok(vmap_route_nav_t * nav)
{
    const uint32_t now = (uint32_t)clock_systime_ticks();

    if (nav->last_reroute_tick != 0u
        && (uint32_t)(now - nav->last_reroute_tick)
            < (uint32_t)MSEC2TICK(VMAP_NAV_REROUTE_MIN_INTERVAL_MS)) {
        if (nav->reroute_suppress_log_tick == 0u
            || (uint32_t)(now - nav->reroute_suppress_log_tick)
                >= (uint32_t)MSEC2TICK(1000u)) {
            nav->reroute_suppress_log_tick = now;
            VMAP_NAV_WARN("off-route reroute suppressed (<%u ms since last)",
                (unsigned)VMAP_NAV_REROUTE_MIN_INTERVAL_MS);
        }
        return false;
    }

    nav->last_reroute_tick = now;
    return true;
}


static void nav_reset_track_match(vmap_route_nav_t * nav)
{
    if (!nav) {
        return;
    }

    nav->track_seg_hint = 0;
    nav->track_seg_start_m = 0.0;
    nav->track_hint_valid = false;
}

static void nav_reset_deviation(vmap_route_nav_t * nav)
{
    if (!nav) {
        return;
    }
    nav->opposite_accum_m = 0.0;
    nav->junction_off_accum_m = 0.0;
    nav->last_passed_maneuver = UINT32_MAX;
    nav->need_reroute = false;
    nav->reroute_reason = VMAP_ROUTE_REROUTE_NONE;
    nav->has_prev = false;
}

static void nav_update_passed_maneuvers(vmap_route_nav_t * nav, double along_m)
{
    const vmap_route_maneuver_t * mans;
    uint32_t n;
    uint32_t passed = UINT32_MAX;

    if (!nav || !nav->route) {
        return;
    }

    mans = vmap_route_maneuvers(nav->route);
    n = vmap_route_maneuver_count(nav->route);
    if (!mans || n == 0) {
        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        const vmap_maneuver_kind_t kind = mans[i].kind;

        if (kind == VMAP_MANEUVER_START || kind == VMAP_MANEUVER_ARRIVE) {
            continue;
        }
        if (along_m >= mans[i].along_m + 15.0) {
            passed = i;
        }
    }
    nav->last_passed_maneuver = passed;
}

static bool nav_passed_junction(const vmap_route_nav_t * nav)
{
    const vmap_route_maneuver_t * mans;

    if (!nav || nav->last_passed_maneuver == UINT32_MAX || !nav->route) {
        return false;
    }

    mans = vmap_route_maneuvers(nav->route);
    if (!mans) {
        return false;
    }
    return mans[nav->last_passed_maneuver].kind != VMAP_MANEUVER_START;
}

/** @brief 过路口后：nav_off_route_m 处 90°，至 2 倍处线性降至 0°。 */
static float nav_junction_angle_limit_deg(double off_m, double threshold)
{
    const double t1 = threshold;
    const double t2 = threshold * 2.0;

    if (off_m >= t2) {
        return 0.0f;
    }
    if (off_m <= t1) {
        return 90.0f;
    }
    return 90.0f * (float)((t2 - off_m) / (t2 - t1));
}

static void nav_update_next_maneuver(vmap_route_nav_t * nav, double along_m,
    double remain_m)
{
    const vmap_route_maneuver_t * mans;
    uint32_t n;
    uint32_t idx;

    nav->state.maneuver_idx = UINT32_MAX;
    nav->state.next_kind = VMAP_MANEUVER_NONE;
    nav->state.dist_to_maneuver_m = 0.0;
    nav->state.turn_rel_deg = 0.0f;
    nav->state.next_road = NULL;

    if (!nav || !nav->route) {
        return;
    }

    mans = vmap_route_maneuvers(nav->route);
    n = vmap_route_maneuver_count(nav->route);
    if (!mans || n == 0) {
        return;
    }

    idx = nav->maneuver_hint;
    while (idx < n && mans[idx].along_m < along_m - 20.0) {
        idx++;
    }
    nav->maneuver_hint = idx;

    for (; idx < n; idx++) {
        const vmap_maneuver_kind_t kind = mans[idx].kind;
        double dist;

        if (kind == VMAP_MANEUVER_START) {
            continue;
        }

        if (kind == VMAP_MANEUVER_ARRIVE) {
            if (remain_m <= VMAP_ROUTE_ARRIVE_M) {
                nav->state.maneuver_idx = idx;
                nav->state.next_kind = kind;
                nav->state.dist_to_maneuver_m = remain_m;
            }
            return;
        }

        dist = mans[idx].along_m - along_m;
        if (dist < -15.0) {
            continue;
        }

        nav->state.maneuver_idx = idx;
        nav->state.next_kind = kind;
        nav->state.dist_to_maneuver_m = dist > 0.0 ? dist : 0.0;
        nav->state.turn_rel_deg = mans[idx].turn_deg;
        nav->state.next_road = mans[idx].road_name;
        return;
    }
}

static void nav_update_deviation(vmap_route_nav_t * nav, double lon, double lat,
    float course_deg, float speed_kph, double off_m, uint32_t seg)
{
    const vmap_route_pt_t * pts;
    uint32_t n;
    double step_m = 0.0;
    float move_brg = course_deg;
    float route_brg;
    float delta;
    const double threshold = nav->off_route_threshold_m > 0.0
        ? nav->off_route_threshold_m : 50.0;

    if (!nav || !nav->route) {
        return;
    }

    pts = vmap_route_points(nav->route);
    n = vmap_route_point_count(nav->route);
    if (!pts || seg + 1u >= n) {
        return;
    }

    route_brg = vmap_geo_bearing_deg(pts[seg].lon, pts[seg].lat,
        pts[seg + 1u].lon, pts[seg + 1u].lat);

    if (nav->has_prev) {
        step_m = vmap_geo_haversine_m(nav->prev_lon, nav->prev_lat, lon, lat);
        if (step_m >= 3.0) {
            move_brg = vmap_geo_bearing_deg(nav->prev_lon, nav->prev_lat, lon, lat);
        }
    }

    delta = vmap_geo_angle_delta_deg(route_brg, move_brg);

    if (nav_passed_junction(nav) && off_m >= threshold * 2.0
        && nav_reroute_cooldown_ok(nav)) {
        nav->need_reroute = true;
        nav->reroute_reason = VMAP_ROUTE_REROUTE_JUNCTION_FAR;
        VMAP_NAV_WARN("off-route reroute: junction off=%.0fm >= 2x threshold %.0fm",
            off_m, threshold);
    } else if (speed_kph >= 3.0f && step_m >= 1.0) {
        /* 逆行只在**贴着路线**时才有意义：off_m 已经很大时，`delta` 比的是骑行者
         * 与"最近那条路线段"的方位 —— 那条段属于**另一条路**，夹角毫无意义。
         * 实测现场：骑行者离路线 289 m、正沿 金陵路 朝终点骑，却一路被判"逆行"。
         * 偏航（不在路线上）交给下面的 junction-far / accum 规则处理。 */
        if (fabsf(delta) > 90.0f && off_m <= threshold) {
            nav->opposite_accum_m += step_m;
        } else {
            nav->opposite_accum_m = 0.0;
        }

        if (nav_passed_junction(nav) && off_m > VMAP_ROUTE_OFF_M) {
            const float angle_limit = nav_junction_angle_limit_deg(off_m, threshold);

            if (fabsf(delta) > angle_limit) {
                nav->junction_off_accum_m += step_m;
            }
        } else if (off_m <= VMAP_ROUTE_OFF_M * 0.5) {
            nav->junction_off_accum_m = 0.0;
        }
    }

    nav->prev_lon = lon;
    nav->prev_lat = lat;
    nav->has_prev = true;

    if (!nav->need_reroute
        && (nav->opposite_accum_m >= threshold
            || nav->junction_off_accum_m >= threshold)
        && nav_reroute_cooldown_ok(nav)) {
        nav->need_reroute = true;
        if (nav->opposite_accum_m >= threshold) {
            nav->reroute_reason = VMAP_ROUTE_REROUTE_OPPOSITE;
        } else {
            nav->reroute_reason = VMAP_ROUTE_REROUTE_JUNCTION_ACCUM;
        }
        VMAP_NAV_WARN("off-route reroute opposite=%.0fm junction=%.0fm off=%.0fm "
            "angle=%.0f limit=%.0f reason=%d",
            nav->opposite_accum_m, nav->junction_off_accum_m, off_m,
            (double)fabsf(delta),
            (double)nav_junction_angle_limit_deg(off_m, threshold),
            (int)nav->reroute_reason);
    }
}

/**
 * @brief vmap route nav create。
 * @return 0 成功，负 errno 失败。
 */
vmap_route_nav_t * vmap_route_nav_create(vmap_route_t * route)
{
    vmap_route_nav_t * nav = (vmap_route_nav_t *)calloc(1, sizeof(*nav));

    if (!nav) {
        return NULL;
    }
    nav->route = route;
    nav->maneuver_hint = 0;
    nav_reset_track_match(nav);
    nav->last_passed_maneuver = UINT32_MAX;
    nav->off_route_threshold_m = 50.0;
    return nav;
}

/**
 * @brief vmap route nav destroy。
 */
void vmap_route_nav_destroy(vmap_route_nav_t * nav)
{
    free(nav);
}

/**
 * @brief vmap route nav set off route threshold。
 */
void vmap_route_nav_set_off_route_threshold(vmap_route_nav_t * nav, double meters)
{
    if (!nav) {
        return;
    }
    nav->off_route_threshold_m = meters > 0.0 ? meters : 50.0;
}

/**
 * @brief vmap route nav plan to。
 */
bool vmap_route_nav_plan_to(vmap_route_nav_t * nav,
    double from_lon, double from_lat, double to_lon, double to_lat)
{
    if (!nav || !nav->route) {
        return false;
    }
    memset(&nav->state, 0, sizeof(nav->state));
    nav->maneuver_hint = 0;
    nav_reset_track_match(nav);
    nav_reset_deviation(nav);
    if (!vmap_route_plan(nav->route, from_lon, from_lat, to_lon, to_lat)) {
        return false;
    }
    nav->state.active = true;
    nav->state.remain_m = vmap_route_total_length_m(nav->route);
    nav->state.maneuver_idx = UINT32_MAX;
    return true;
}

/**
 * @brief vmap route nav apply。
 */
void vmap_route_nav_apply(vmap_route_nav_t * nav, const vmap_route_pt_t * pts,
    uint32_t pt_count, double total_m,
    const vmap_route_maneuver_t * maneuvers, uint32_t maneuver_count)
{
    if (!nav || !nav->route) {
        return;
    }

    memset(&nav->state, 0, sizeof(nav->state));
    nav->maneuver_hint = 0;
    nav_reset_track_match(nav);
    nav_reset_deviation(nav);
    vmap_route_apply(nav->route, pts, pt_count, total_m, maneuvers,
        maneuver_count);
    if (!vmap_route_is_active(nav->route)) {
        return;
    }
    nav->state.active = true;
    nav->state.remain_m = total_m;
    nav->state.maneuver_idx = UINT32_MAX;
    VMAP_NAV_LOG("nav_apply: pts=%u man=%u len=%.0fm",
        pt_count, maneuver_count, total_m);
}

/**
 * @brief vmap route nav stop。
 */
void vmap_route_nav_stop(vmap_route_nav_t * nav)
{
    if (!nav) {
        return;
    }
    VMAP_NAV_LOG("nav_stop");
    vmap_route_clear(nav->route);
    memset(&nav->state, 0, sizeof(nav->state));
    nav->maneuver_hint = 0;
    nav_reset_track_match(nav);
    nav_reset_deviation(nav);
}

/**
 * @brief vmap route nav reset track。
 */
void vmap_route_nav_reset_track(vmap_route_nav_t * nav)
{
    if (!nav) {
        return;
    }
    nav->maneuver_hint = 0;
    nav_reset_track_match(nav);
    nav->state.maneuver_idx = UINT32_MAX;
}

/**
 * @brief vmap route nav take reroute。
 */
vmap_route_reroute_reason_t vmap_route_nav_take_reroute(vmap_route_nav_t * nav)
{
    vmap_route_reroute_reason_t reason;

    if (!nav || !nav->need_reroute) {
        return VMAP_ROUTE_REROUTE_NONE;
    }
    reason = nav->reroute_reason;
    if (reason == VMAP_ROUTE_REROUTE_NONE) {
        reason = VMAP_ROUTE_REROUTE_JUNCTION_ACCUM;
    }
    nav->need_reroute = false;
    nav->reroute_reason = VMAP_ROUTE_REROUTE_NONE;
    return reason;
}

/**
 * @brief vmap route reroute reason brief。
 */
const char * vmap_route_reroute_reason_brief(vmap_route_reroute_reason_t reason)
{
    switch (reason) {
    case VMAP_ROUTE_REROUTE_OPPOSITE:
        return "逆行偏航";
    case VMAP_ROUTE_REROUTE_JUNCTION_ACCUM:
        return "路口偏航";
    case VMAP_ROUTE_REROUTE_JUNCTION_FAR:
        return "严重偏航";
    default:
        return "偏航";
    }
}

/**
 * @brief vmap route nav update。
 */
void vmap_route_nav_update(vmap_route_nav_t * nav, double lon, double lat,
    float course_deg, float speed_kph)
{
    const vmap_route_pt_t * pts;
    uint32_t n;
    uint32_t seg = 0;
    double off_m;
    double along;
    double total;

    if (!nav || !vmap_route_is_active(nav->route)) {
        return;
    }

    pts = vmap_route_points(nav->route);
    n = vmap_route_point_count(nav->route);
    if (n < 2) {
        return;
    }

    {
        double best_d = 1e18;
        double best_score = 1e18;
        double best_along = 0.0;
        double best_seg_start_m = 0.0;
        double best_t = 0.0;
        const bool continuity = nav->track_hint_valid && nav->state.active;
        const double prev_along = nav->state.along_m;
        double acc;
        uint32_t hint;
        uint32_t start;
        uint32_t end;
        bool acquire_all;

        acquire_all = !nav->track_hint_valid;
        hint = nav->track_seg_hint;
        if (!acquire_all && hint + 1u >= n) {
            hint = n - 2u;
        }

        if (acquire_all) {
            uint32_t stride = 1u;
            uint32_t coarse = 0;
            double coarse_d = 1e18;
            double coarse_acc = 0.0;
            double run = 0.0;
            const uint32_t last_seg = n - 2u;

            if (last_seg + 1u > VMAP_NAV_MATCH_ACQUIRE_MAX) {
                stride = (last_seg + VMAP_NAV_MATCH_ACQUIRE_MAX)
                    / VMAP_NAV_MATCH_ACQUIRE_MAX;
            }
            for (uint32_t i = 0; i <= last_seg; i++) {
                if ((i % stride) == 0u || i == last_seg) {
                    double tloc = 0.0;
                    const double d = vmap_geo_point_to_seg_m(lon, lat,
                        pts[i].lon, pts[i].lat, pts[i + 1].lon, pts[i + 1].lat,
                        &tloc);

                    if (d < coarse_d) {
                        coarse_d = d;
                        coarse = i;
                        coarse_acc = run;
                    }
                }
                run += vmap_geo_haversine_m(pts[i].lon, pts[i].lat,
                    pts[i + 1].lon, pts[i + 1].lat);
            }
            hint = coarse;
            start = hint > VMAP_NAV_MATCH_ACQUIRE_HALF
                ? hint - VMAP_NAV_MATCH_ACQUIRE_HALF : 0u;
            end = hint + VMAP_NAV_MATCH_ACQUIRE_HALF;
            if (end > last_seg) {
                end = last_seg;
            }
            acc = coarse_acc;
        } else {
            start = hint > VMAP_NAV_MATCH_BACK_SEGS
                ? hint - VMAP_NAV_MATCH_BACK_SEGS : 0u;
            end = hint + VMAP_NAV_MATCH_AHEAD_SEGS;
            if (end + 1u >= n) {
                end = n - 2u;
            }
            acc = nav->track_seg_start_m;
        }

        for (uint32_t i = hint; i > start; i--) {
            acc -= vmap_geo_haversine_m(pts[i - 1u].lon, pts[i - 1u].lat,
                pts[i].lon, pts[i].lat);
        }

        for (uint32_t i = start; i <= end; i++) {
            double tloc = 0.0;
            const double d = vmap_geo_point_to_seg_m(lon, lat,
                pts[i].lon, pts[i].lat, pts[i + 1].lon, pts[i + 1].lat, &tloc);
            const double seg_m = vmap_geo_haversine_m(pts[i].lon, pts[i].lat,
                pts[i + 1].lon, pts[i + 1].lat);
            const double candidate_along = acc + seg_m * tloc;
            double score = d;

            if (continuity) {
                const double delta = candidate_along - prev_along;
                double excess = 0.0;

                if (delta < -VMAP_NAV_MATCH_BACK_TOL_M) {
                    excess = -delta - VMAP_NAV_MATCH_BACK_TOL_M;
                } else if (delta > VMAP_NAV_MATCH_AHEAD_TOL_M) {
                    excess = delta - VMAP_NAV_MATCH_AHEAD_TOL_M;
                }
                score += fmin(VMAP_NAV_MATCH_MAX_PENALTY,
                    excess * VMAP_NAV_MATCH_PROGRESS_W);
            }

            if (score < best_score) {
                best_score = score;
                best_d = d;
                seg = i;
                best_seg_start_m = acc;
                best_t = tloc;
                best_along = candidate_along;
            }
            acc += seg_m;
        }

        nav->track_seg_hint = seg;
        nav->track_seg_start_m = best_seg_start_m;
        nav->track_hint_valid = true;
        off_m = best_d;
        vmap_route_set_progress_match(nav->route, lon, lat, seg, best_t);
        along = vmap_route_effective_along_m(nav->route, best_along);
    }
    total = vmap_route_total_length_m(nav->route);

    nav->state.active = true;
    nav->state.off_route_m = off_m;
    nav->state.along_m = along;
    nav->state.remain_m = fmax(0.0, total - along);

    /* 每拍把骑行者位置同步给 route：绘制用它把"已走过的那段"刷灰。
     * **放在 nav 更新内部**，这样不管 UI 从哪条路径喂位置都生效
     * （之前挂在某一条 UI 路径上，实际走的不是那条 → 走过的线还是蓝的）。 */
    vmap_route_set_progress_pos(nav->route, lon, lat);

    if (nav->state.remain_m <= VMAP_ROUTE_ARRIVE_M) {
        nav->state.arrived = true;
    }

    /* 终点常常**不在路上**：它会被吸到最近的路网节点（实测偏 171 m），骑行轨迹从旁边
     * 过去（屏上"偏航 43 m"），于是沿路剩余距离永远 > 45 m，**永远不判到达、导航不收尾**
     * （用户现场：骑过终点还在导）。这里补一条：离**路线末点**足够近也算到达 —— 用直线
     * 距离，不看沿路剩余。半径取 VMAP_NAV_ARRIVE_NEAR_M 的 1/4（默认 75 m）。 */
    {
        const uint32_t rn = vmap_route_point_count(nav->route);
        const vmap_route_pt_t * rp = vmap_route_points(nav->route);

        if (rp != NULL && rn >= 2u
            && vmap_geo_haversine_m(lon, lat, rp[rn - 1u].lon,
                   rp[rn - 1u].lat) <= (VMAP_NAV_ARRIVE_NEAR_M * 0.25)) {
            nav->state.arrived = true;
        }
    }

    if (speed_kph > 3.0f) {
        nav->state.eta_s = (float)(nav->state.remain_m / ((double)speed_kph / 3.6));
    } else {
        nav->state.eta_s = 0.0f;
    }

    nav_update_passed_maneuvers(nav, along);
    nav_update_deviation(nav, lon, lat, course_deg, speed_kph, off_m, seg);
    nav_update_next_maneuver(nav, along, nav->state.remain_m);
}

/**
 * @brief vmap route nav state。
 */
const vmap_route_nav_state_t * vmap_route_nav_state(const vmap_route_nav_t * nav)
{
    return nav ? &nav->state : NULL;
}
