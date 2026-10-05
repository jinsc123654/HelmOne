/**
 * @file vmap_route_corridor.c
 * @brief vmap route_corridor 模块。
 */

#include "vmap_route.h"

#include "vmap_config.h"
#include "vmap_geo.h"
#include "vmap_map_catalog.h"
#include "vmap_route_graph.h"
#include "vmap_route_log.h"
#include "vmap_route_portal.h"
#include "vmap_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
 * Region routing graphs are clipped from the full graph with a generous
 * overlap margin (pack_map.py GRAPH_CLIP_MARGIN_M).  A destination that sits
 * just across a region border is therefore already present, and well
 * connected, inside the FROM region's own graph.  When that is the case we
 * can plan the whole route in a single region instead of paying for the
 * expensive (and border-artifact-prone) corridor planner.
 *
 * This band MUST stay comfortably smaller than the packer clip margin so the
 * routable roads we rely on are backed by connected geometry rather than the
 * dangling stubs that appear at the very edge of the clip box.
 */
#define VMAP_ROUTE_REGION_REACH_M 300.0

static bool region_covers_point(const vmap_map_region_t * r, double lon,
    double lat, double margin_m)
{
    double clat;
    double dlat;
    double dlon;
    double coslat;

    if (!r) {
        return false;
    }
    clat = (r->south + r->north) * 0.5;
    coslat = cos(clat * (M_PI / 180.0));
    if (coslat < 0.1) {
        coslat = 0.1;
    }
    dlat = margin_m / 111320.0;
    dlon = margin_m / (111320.0 * coslat);
    return lon >= r->west - dlon && lon <= r->east + dlon
        && lat >= r->south - dlat && lat <= r->north + dlat;
}

typedef struct {
    vmap_route_pt_t * pts;
    vmap_route_maneuver_t * man;
} corridor_seg_scratch_t;

static bool corridor_seg_scratch_alloc(corridor_seg_scratch_t * scratch)
{
    if (!scratch) {
        return false;
    }
    scratch->pts = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(vmap_route_pt_t));
    scratch->man = (vmap_route_maneuver_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_MANEUVERS * sizeof(vmap_route_maneuver_t));
    if (!scratch->pts || !scratch->man) {
        vmap_free(scratch->pts);
        vmap_free(scratch->man);
        scratch->pts = NULL;
        scratch->man = NULL;
        return false;
    }
    return true;
}

static void corridor_seg_scratch_free(corridor_seg_scratch_t * scratch)
{
    if (!scratch) {
        return;
    }
    vmap_free(scratch->pts);
    vmap_free(scratch->man);
    scratch->pts = NULL;
    scratch->man = NULL;
}

typedef struct {
    double lon;
    double lat;
    double cross_m;
    vmap_cell_id_t rid_a;
    vmap_cell_id_t rid_b;
    uint32_t node_a;
    uint32_t node_b;
} corridor_portal_t;

#define CORRIDOR_PORTAL_NO_NODE UINT32_MAX

/**
 * @brief corridor_portal_init 接口。
 */
static void corridor_portal_init(corridor_portal_t * p)
{
    if (!p) {
        return;
    }
    p->lon = 0.0;
    p->lat = 0.0;
    p->cross_m = 0.0;
    p->rid_a = 0;
    p->rid_b = 0;
    p->node_a = CORRIDOR_PORTAL_NO_NODE;
    p->node_b = CORRIDOR_PORTAL_NO_NODE;
}

static bool corridor_portal_node_in_region(const corridor_portal_t * p,
    vmap_cell_id_t rid, uint32_t * node_out)
{
    if (!p || !node_out) {
        return false;
    }
    if (p->rid_a == rid && p->node_a != CORRIDOR_PORTAL_NO_NODE) {
        *node_out = p->node_a;
        return true;
    }
    if (p->rid_b == rid && p->node_b != CORRIDOR_PORTAL_NO_NODE) {
        *node_out = p->node_b;
        return true;
    }
    return false;
}

static void corridor_portal_sort(corridor_portal_t * portals, uint32_t n);

static void border_sample_points(const vmap_map_region_t * a,
    const vmap_map_region_t * b, double * lons, double * lats, uint32_t max,
    uint32_t * n_out)
{
    double lat_lo;
    double lat_hi;
    double lon_lo;
    double lon_hi;
    uint32_t i;

    if (!a || !b || !lons || !lats || !n_out || max == 0) {
        return;
    }

    lat_lo = a->south > b->south ? a->south : b->south;
    lat_hi = a->north < b->north ? a->north : b->north;
    lon_lo = a->west > b->west ? a->west : b->west;
    lon_hi = a->east < b->east ? a->east : b->east;

    *n_out = 0;
    for (i = 0; i < max; i++) {
        const double t = ((double)i + 0.5) / (double)max;

        if (b->grid_ix == a->grid_ix + 1 && b->grid_iy == a->grid_iy) {
            lons[i] = (a->east + b->west) * 0.5;
            lats[i] = lat_lo + (lat_hi - lat_lo) * t;
        } else if (b->grid_ix + 1 == a->grid_ix && b->grid_iy == a->grid_iy) {
            lons[i] = (a->west + b->east) * 0.5;
            lats[i] = lat_lo + (lat_hi - lat_lo) * t;
        } else if (b->grid_iy == a->grid_iy + 1 && b->grid_ix == a->grid_ix) {
            lats[i] = (a->north + b->south) * 0.5;
            lons[i] = lon_lo + (lon_hi - lon_lo) * t;
        } else if (b->grid_iy + 1 == a->grid_iy && b->grid_ix == a->grid_ix) {
            lats[i] = (a->south + b->north) * 0.5;
            lons[i] = lon_lo + (lon_hi - lon_lo) * t;
        } else {
            return;
        }
        (*n_out)++;
    }
}

static bool region_border_overlap(const vmap_map_region_t * a,
    const vmap_map_region_t * b, double margin_m, double * west, double * south,
    double * east, double * north)
{
    double lat_ref;
    double dlat;
    double dlon;
    double coslat;
    double aw;
    double ae;
    double as_;
    double an;
    double bw;
    double be;
    double bs;
    double bn;

    if (!a || !b || !west || !south || !east || !north) {
        return false;
    }

    lat_ref = ((a->south + a->north) + (b->south + b->north)) * 0.25;
    dlat = margin_m / 111320.0;
    coslat = cos(lat_ref * (M_PI / 180.0));
    if (coslat < 0.1) {
        coslat = 0.1;
    }
    dlon = margin_m / (111320.0 * coslat);

    aw = a->west - dlon;
    ae = a->east + dlon;
    as_ = a->south - dlat;
    an = a->north + dlat;
    bw = b->west - dlon;
    be = b->east + dlon;
    bs = b->south - dlat;
    bn = b->north + dlat;

    *west = aw > bw ? aw : bw;
    *south = as_ > bs ? as_ : bs;
    *east = ae < be ? ae : be;
    *north = an < bn ? an : bn;
    if (*west < *east && *south < *north) {
        return true;
    }

    /*
     * Adjacent VREG cells can share an edge with zero-width raw intersection.
     * Widen to a stripe (same rule as portal_match.bbox_overlap).
     */
    if (fabs(ae - bw) < 1e-6 || fabs(be - aw) < 1e-6) {
        const double shared_lon = fabs(ae - bw) < 1e-6 ? ae : aw;
        const double lat_lo = as_ > bs ? as_ : bs;
        const double lat_hi = an < bn ? an : bn;

        if (lat_lo < lat_hi) {
            *west = shared_lon - dlon;
            *east = shared_lon + dlon;
            *south = lat_lo;
            *north = lat_hi;
            return true;
        }
    }

    if (fabs(an - bs) < 1e-6 || fabs(bn - as_) < 1e-6) {
        const double shared_lat = fabs(an - bs) < 1e-6 ? an : as_;
        const double lon_lo = aw > bw ? aw : bw;
        const double lon_hi = ae < be ? ae : be;

        if (lon_lo < lon_hi) {
            *west = lon_lo;
            *east = lon_hi;
            *south = shared_lat - dlat;
            *north = shared_lat + dlat;
            return true;
        }
    }

    return false;
}

static bool portal_point_in_overlap(double lon, double lat, double west,
    double south, double east, double north)
{
    return lon >= west && lon <= east && lat >= south && lat <= north;
}

static bool portal_snap_both(vmap_route_graph_t * ga, vmap_route_graph_t * gb,
    vmap_cell_id_t rid_a, vmap_cell_id_t rid_b, double lon, double lat,
    corridor_portal_t * out, double match_m)
{
    uint32_t nodes_a[VMAP_ROUTE_SNAP_K];
    uint32_t nodes_b[VMAP_ROUTE_SNAP_K];
    uint32_t na;
    uint32_t nb;
    uint32_t ia;
    uint32_t ib;
    uint32_t best_ia = 0;
    uint32_t best_ib = 0;
    double best_cross = match_m + 1.0;
    double alon;
    double alat;
    double blon;
    double blat;
    bool have = false;

    if (!ga || !gb || !out) {
        return false;
    }

    na = vmap_route_graph_snap_end(ga, lon, lat, nodes_a, VMAP_ROUTE_SNAP_K, NULL);
    nb = vmap_route_graph_snap_end(gb, lon, lat, nodes_b, VMAP_ROUTE_SNAP_K, NULL);
    if (na == 0 || nb == 0) {
        return false;
    }

    for (ia = 0; ia < na; ia++) {
        vmap_route_graph_node_lonlat(ga, nodes_a[ia], &alon, &alat);
        for (ib = 0; ib < nb; ib++) {
            double cross;

            vmap_route_graph_node_lonlat(gb, nodes_b[ib], &blon, &blat);
            cross = vmap_geo_haversine_m(alon, alat, blon, blat);
            if (cross <= match_m && cross < best_cross) {
                best_cross = cross;
                best_ia = nodes_a[ia];
                best_ib = nodes_b[ib];
                out->lon = (alon + blon) * 0.5;
                out->lat = (alat + blat) * 0.5;
                out->cross_m = cross;
                have = true;
            }
        }
    }

    if (have) {
        out->rid_a = rid_a;
        out->rid_b = rid_b;
        out->node_a = best_ia;
        out->node_b = best_ib;
    }

    return have;
}

static uint32_t portal_collect_border_scan(vmap_route_graph_t * ga,
    vmap_route_graph_t * gb, vmap_cell_id_t rid_a, vmap_cell_id_t rid_b,
    const vmap_map_region_t * ra, const vmap_map_region_t * rb,
    corridor_portal_t * out, uint32_t out_max, double match_m)
{
    double west;
    double south;
    double east;
    double north;
    uint32_t found = 0;
    uint32_t i;
    uint32_t j;

    if (!ga || !gb || !ra || !rb || !out || out_max == 0) {
        return 0;
    }

    if (!region_border_overlap(ra, rb, VMAP_ROUTE_PORTAL_BORDER_M, &west, &south,
            &east, &north)) {
        return 0;
    }

    {
        const uint32_t ga_n = vmap_route_graph_node_count(ga);
        const uint32_t gb_n = vmap_route_graph_node_count(gb);

        for (i = 0; i < ga_n && found < out_max; i++) {
            double alon;
            double alat;

            vmap_route_graph_node_lonlat(ga, i, &alon, &alat);
            if (!portal_point_in_overlap(alon, alat, west, south, east, north)) {
                continue;
            }

            for (j = 0; j < gb_n && found < out_max; j++) {
                double blon;
                double blat;
                double cross;

                vmap_route_graph_node_lonlat(gb, j, &blon, &blat);
                if (!portal_point_in_overlap(blon, blat, west, south, east, north)) {
                    continue;
                }

                cross = vmap_geo_haversine_m(alon, alat, blon, blat);
                if (cross > match_m) {
                    continue;
                }

                out[found].lon = (alon + blon) * 0.5;
                out[found].lat = (alat + blat) * 0.5;
                out[found].cross_m = cross;
                out[found].rid_a = rid_a;
                out[found].rid_b = rid_b;
                out[found].node_a = i;
                out[found].node_b = j;
                found++;
            }
        }
    }

    if (found > 1) {
        corridor_portal_sort(out, found);
    }
    return found;
}

static uint32_t corridor_portal_collect(const char * map_dir, vmap_cell_id_t rid_a,
    vmap_cell_id_t rid_b, corridor_portal_t * out, uint32_t out_max)
{
    vmap_route_portal_rec_t packed[VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX];
    uint32_t packed_n;
    uint32_t i;

    if (!map_dir || !out || out_max == 0) {
        return 0;
    }

    (void)vmap_route_portal_load(map_dir);
    packed_n = vmap_route_portal_query(rid_a, rid_b, packed,
        VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX);
    if (packed_n > 0) {
        for (i = 0; i < packed_n && i < out_max; i++) {
            corridor_portal_init(&out[i]);
            out[i].lon = packed[i].lon;
            out[i].lat = packed[i].lat;
            out[i].cross_m = 0.0;
            out[i].rid_a = packed[i].region_a;
            out[i].rid_b = packed[i].region_b;
            out[i].node_a = packed[i].node_a;
            out[i].node_b = packed[i].node_b;
        }
        VMAP_NAV_LOG("corridor: r%u->r%u packed portals=%u",
            (unsigned)rid_a, (unsigned)rid_b, packed_n);
        return packed_n < out_max ? packed_n : out_max;
    }

    {
        const vmap_map_region_t * ra;
        const vmap_map_region_t * rb;
        vmap_route_graph_t * ga = NULL;
        vmap_route_graph_t * gb = NULL;
        double sample_lon[VMAP_ROUTE_BORDER_PORTAL_SAMPLES];
        double sample_lat[VMAP_ROUTE_BORDER_PORTAL_SAMPLES];
        uint32_t sample_n = 0;
        uint32_t found = 0;

        ra = vmap_map_catalog_region(rid_a);
        rb = vmap_map_catalog_region(rid_b);
        if (!ra || !rb) {
            return 0;
        }

        border_sample_points(ra, rb, sample_lon, sample_lat,
            VMAP_ROUTE_BORDER_PORTAL_SAMPLES, &sample_n);
        if (sample_n == 0) {
            VMAP_NAV_WARN("corridor: border sample fail r%u->r%u",
                (unsigned)rid_a, (unsigned)rid_b);
            return 0;
        }

        ga = vmap_route_graph_acquire_region(map_dir, rid_a);
        gb = vmap_route_graph_acquire_region(map_dir, rid_b);
        vmap_route_plan_yield();
        if (vmap_route_plan_aborted()) {
            vmap_route_graph_release_region(ga);
            vmap_route_graph_release_region(gb);
            return 0;
        }
        if (!ga || !gb) {
            VMAP_NAV_WARN("corridor: portal graph load fail r%u/r%u",
                (unsigned)rid_a, (unsigned)rid_b);
            vmap_route_graph_release_region(ga);
            vmap_route_graph_release_region(gb);
            return 0;
        }

        for (i = 0; i < sample_n && found < out_max; i++) {
            corridor_portal_t cand;

            corridor_portal_init(&cand);
            if (portal_snap_both(ga, gb, rid_a, rid_b, sample_lon[i],
                    sample_lat[i], &cand, VMAP_ROUTE_PORTAL_MATCH_M)) {
                out[found++] = cand;
            }
        }

        if (found == 0) {
            for (i = 0; i < sample_n && found < out_max; i++) {
                corridor_portal_t cand;

                corridor_portal_init(&cand);
                if (portal_snap_both(ga, gb, rid_a, rid_b, sample_lon[i],
                        sample_lat[i], &cand,
                        VMAP_ROUTE_PORTAL_FALLBACK_MATCH_M)) {
                    out[found++] = cand;
                }
            }
            if (found > 0) {
                VMAP_NAV_LOG("corridor: r%u->r%u loose snap portals=%u",
                    (unsigned)rid_a, (unsigned)rid_b, found);
            }
        }

        if (found == 0) {
            found = portal_collect_border_scan(ga, gb, rid_a, rid_b, ra, rb,
                out, out_max, VMAP_ROUTE_PORTAL_MATCH_M);
            if (found == 0) {
                found = portal_collect_border_scan(ga, gb, rid_a, rid_b, ra, rb,
                    out, out_max, VMAP_ROUTE_PORTAL_FALLBACK_MATCH_M);
            }
            if (found > 0) {
                VMAP_NAV_LOG("corridor: r%u->r%u border-scan portals=%u",
                    (unsigned)rid_a, (unsigned)rid_b, found);
            }
        }

        vmap_route_graph_release_region(ga);
        vmap_route_graph_release_region(gb);

        VMAP_NAV_LOG("corridor: r%u->r%u snap portals=%u/%u",
            (unsigned)rid_a, (unsigned)rid_b, found, sample_n);
        return found;
    }
}

static void corridor_portal_sort(corridor_portal_t * portals, uint32_t n)
{
    uint32_t i;
    uint32_t j;

    for (i = 1; i < n; i++) {
        corridor_portal_t key = portals[i];

        j = i;
        while (j > 0 && portals[j - 1].cross_m > key.cross_m) {
            portals[j] = portals[j - 1];
            j--;
        }
        portals[j] = key;
    }
}

/* 门户候选按**顺路程度**重排：键 = "起点→门户 + 门户→终点"的大圆距离和，也就是走这个
 * 口的最短可能长度。打包数据（pack_map.py）里每边界的门户是按**匹配质量** cross_m 排的，
 * 那跟"好不好走"无关 —— 只取前几个就等于随机挑口。2026-09-19 实测：醉翁东路的跨界口就在
 * x3596_y1196.vpk 的 VPOR 里（118.3359,32.2453），却因为排在 64 条里的第 8 名开外而被
 * 挡在候选之外，系统于是绕 龙蟠大道 走了 3576 m —— 实际只要 1594 m。 */
static void corridor_portal_rank_by_detour(corridor_portal_t * portals, uint32_t n,
    double from_lon, double from_lat, double to_lon, double to_lat)
{
    double cost[VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX];
    uint32_t i;
    uint32_t j;

    if (!portals || n < 2u) {
        return;
    }

    if (n > VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX) {
        n = VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX;
    }

    for (i = 0; i < n; i++) {
        cost[i] = vmap_geo_haversine_m(from_lon, from_lat, portals[i].lon,
            portals[i].lat)
            + vmap_geo_haversine_m(portals[i].lon, portals[i].lat, to_lon, to_lat);
    }

    for (i = 1; i < n; i++) {
        corridor_portal_t key = portals[i];
        double kc = cost[i];

        j = i;
        while (j > 0 && cost[j - 1u] > kc) {
            portals[j] = portals[j - 1u];
            cost[j] = cost[j - 1u];
            j--;
        }
        portals[j] = key;
        cost[j] = kc;
    }
}

static bool corridor_append_segment(vmap_route_pt_t * out_pts, uint32_t * out_n,
    double * total_m, vmap_route_maneuver_t * out_man, uint32_t * out_man_n,
    uint32_t man_cap, const vmap_route_pt_t * seg_pts, uint32_t seg_n,
    double seg_total, const vmap_route_maneuver_t * seg_man, uint32_t seg_man_n,
    bool first_seg)
{
    uint32_t start_i = 0;
    uint32_t i;

    if (!out_pts || !out_n || !total_m || !seg_pts || seg_n < 2) {
        return false;
    }

    if (!first_seg && *out_n > 0) {
        const double d = vmap_geo_haversine_m(out_pts[*out_n - 1u].lon,
            out_pts[*out_n - 1u].lat, seg_pts[0].lon, seg_pts[0].lat);

        if (d < VMAP_ROUTE_STITCH_DEDUP_M) {
            start_i = 1;
        }
    }

    if (*out_n + (seg_n - start_i) > VMAP_ROUTE_MAX_PTS) {
        VMAP_NAV_WARN("corridor: stitch overflow pts=%u +%u",
            *out_n, seg_n - start_i);
        return false;
    }

    for (i = start_i; i < seg_n; i++) {
        out_pts[*out_n] = seg_pts[i];
        (*out_n)++;
    }
    *total_m += seg_total;

    if (out_man && out_man_n && seg_man && seg_man_n > 0) {
        for (i = 0; i < seg_man_n; i++) {
            if (*out_man_n >= man_cap) {
                break;
            }
            if (!first_seg && seg_man[i].kind == VMAP_MANEUVER_START) {
                continue;
            }
            out_man[*out_man_n] = seg_man[i];
            if (!first_seg) {
                out_man[*out_man_n].along_m += *total_m - seg_total;
            }
            (*out_man_n)++;
        }
    }

    return true;
}

static bool corridor_plan_segments(const char * map_dir,
    const vmap_cell_id_t * path, uint32_t path_len, const corridor_portal_t * portals,
    double from_lon, double from_lat, double to_lon, double to_lat,
    vmap_route_pt_t * pts, uint32_t * pt_count, double * total_m,
    vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count, corridor_seg_scratch_t * scratch)
{
    vmap_route_pt_t * seg_pts;
    vmap_route_maneuver_t * seg_man;
    uint32_t seg_n;
    uint32_t seg_man_n;
    double seg_total;
    uint32_t i;
    bool ok;

    if (!scratch || !scratch->pts || !scratch->man) {
        return false;
    }

    seg_pts = scratch->pts;
    seg_man = scratch->man;

    *pt_count = 0;
    *total_m = 0.0;
    if (maneuver_count) {
        *maneuver_count = 0;
    }

    for (i = 0; i < path_len; i++) {
        vmap_route_graph_t * graph = NULL;
        double seg_from_lon;
        double seg_from_lat;
        double seg_to_lon;
        double seg_to_lat;
        uint32_t pin_src = CORRIDOR_PORTAL_NO_NODE;
        uint32_t pin_dst = CORRIDOR_PORTAL_NO_NODE;

        graph = vmap_route_graph_acquire_region(map_dir, path[i]);
        if (!graph) {
            VMAP_NAV_WARN("corridor: segment load fail r%u",
                (unsigned)path[i]);
            return false;
        }
        vmap_route_plan_yield();
        if (vmap_route_plan_aborted()) {
            return false;
        }

        if (i == 0) {
            seg_from_lon = from_lon;
            seg_from_lat = from_lat;
        } else {
            seg_from_lon = portals[i - 1u].lon;
            seg_from_lat = portals[i - 1u].lat;
            (void)corridor_portal_node_in_region(&portals[i - 1u], path[i],
                &pin_src);
        }

        if (i + 1u == path_len) {
            seg_to_lon = to_lon;
            seg_to_lat = to_lat;
        } else {
            seg_to_lon = portals[i].lon;
            seg_to_lat = portals[i].lat;
            (void)corridor_portal_node_in_region(&portals[i], path[i], &pin_dst);
        }

        seg_n = 0;
        seg_man_n = 0;
        seg_total = 0.0;
        ok = vmap_route_compute_pins(graph, seg_from_lon, seg_from_lat,
            seg_to_lon, seg_to_lat, pin_src, pin_dst, seg_pts, &seg_n,
            &seg_total, seg_man, VMAP_ROUTE_MAX_MANEUVERS, &seg_man_n);
        if (!ok && (pin_src != CORRIDOR_PORTAL_NO_NODE
                || pin_dst != CORRIDOR_PORTAL_NO_NODE)) {
            ok = vmap_route_compute_pins(graph, seg_from_lon, seg_from_lat,
                seg_to_lon, seg_to_lat, CORRIDOR_PORTAL_NO_NODE,
                CORRIDOR_PORTAL_NO_NODE, seg_pts, &seg_n, &seg_total,
                seg_man, VMAP_ROUTE_MAX_MANEUVERS, &seg_man_n);
        }
        vmap_route_graph_release_region(graph);
        vmap_route_plan_yield();
        if (vmap_route_plan_aborted()) {
            return false;
        }

        if (!ok || seg_n < 2) {
            VMAP_NAV_WARN("corridor: segment %u fail r%u",
                (unsigned)i, (unsigned)path[i]);
            return false;
        }

        if (!corridor_append_segment(pts, pt_count, total_m, maneuvers,
                maneuver_count, maneuver_cap, seg_pts, seg_n, seg_total,
                seg_man, seg_man_n, i == 0)) {
            return false;
        }
    }

    return *pt_count >= 2;
}

static bool corridor_plan_two_regions(const char * map_dir, vmap_cell_id_t rid_a,
    vmap_cell_id_t rid_b, double from_lon, double from_lat, double to_lon,
    double to_lat, vmap_route_pt_t * pts, uint32_t * pt_count, double * total_m,
    vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count)
{
    corridor_portal_t portals[VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX];
    corridor_seg_scratch_t seg_scratch;
    vmap_route_pt_t * trial_pts;
    vmap_route_pt_t * best_pts;
    vmap_route_maneuver_t * trial_man;
    vmap_route_maneuver_t * best_man;
    uint32_t portal_n;
    uint32_t best_n = 0;
    uint32_t best_man_n = 0;
    double best_total = 0.0;
    bool have_best = false;
    uint32_t i;
    vmap_cell_id_t path[2];

    memset(&seg_scratch, 0, sizeof(seg_scratch));
    trial_pts = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(vmap_route_pt_t));
    best_pts = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(vmap_route_pt_t));
    trial_man = (vmap_route_maneuver_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_MANEUVERS * sizeof(vmap_route_maneuver_t));
    best_man = (vmap_route_maneuver_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_MANEUVERS * sizeof(vmap_route_maneuver_t));
    if (!trial_pts || !best_pts || !trial_man || !best_man
        || !corridor_seg_scratch_alloc(&seg_scratch)) {
        VMAP_NAV_ERR("corridor: scratch alloc failed");
        vmap_free(trial_pts);
        vmap_free(best_pts);
        vmap_free(trial_man);
        vmap_free(best_man);
        corridor_seg_scratch_free(&seg_scratch);
        return false;
    }

    portal_n = corridor_portal_collect(map_dir, rid_a, rid_b, portals,
        VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX);
    if (portal_n == 0) {
        VMAP_NAV_WARN("corridor: no portals r%u->r%u",
            (unsigned)rid_a, (unsigned)rid_b);
        vmap_free(trial_pts);
        vmap_free(best_pts);
        vmap_free(trial_man);
        vmap_free(best_man);
        corridor_seg_scratch_free(&seg_scratch);
        return false;
    }

    path[0] = rid_a;
    path[1] = rid_b;

    corridor_portal_rank_by_detour(portals, portal_n, from_lon, from_lat, to_lon,
        to_lat);
    if (portal_n > VMAP_ROUTE_PORTAL_TRIAL_MAX) {
        portal_n = VMAP_ROUTE_PORTAL_TRIAL_MAX;
    }
    VMAP_NAV_LOG("corridor: r%u->r%u portals used=%u", (unsigned)rid_a,
        (unsigned)rid_b, portal_n);

    for (i = 0; i < portal_n; i++) {
        uint32_t trial_n = 0;
        uint32_t trial_man_n = 0;
        double trial_total = 0.0;

        if (!corridor_plan_segments(map_dir, path, 2, &portals[i], from_lon,
                from_lat, to_lon, to_lat, trial_pts, &trial_n, &trial_total,
                trial_man, maneuver_cap, &trial_man_n, &seg_scratch)) {
            continue;
        }

        if (!have_best || trial_total < best_total) {
            have_best = true;
            best_n = trial_n;
            best_total = trial_total;
            best_man_n = trial_man_n;
            memcpy(best_pts, trial_pts, (size_t)best_n * sizeof(best_pts[0]));
            if (maneuvers && best_man_n > 0) {
                memcpy(best_man, trial_man,
                    (size_t)best_man_n * sizeof(best_man[0]));
            }
            VMAP_NAV_LOG("corridor: portal pick #%u len=%.0fm at %.5f,%.5f",
                i, trial_total, portals[i].lon, portals[i].lat);
        }
        vmap_route_plan_yield();
        if (vmap_route_plan_aborted()) {
            break;
        }
    }

    vmap_free(trial_pts);
    vmap_free(trial_man);
    corridor_seg_scratch_free(&seg_scratch);

    if (!have_best) {
        VMAP_NAV_WARN("corridor: all portal trials failed r%u->r%u",
            (unsigned)rid_a, (unsigned)rid_b);
        vmap_free(best_pts);
        vmap_free(best_man);
        return false;
    }

    *pt_count = best_n;
    *total_m = best_total;
    memcpy(pts, best_pts, (size_t)best_n * sizeof(pts[0]));
    if (maneuvers && maneuver_count) {
        *maneuver_count = best_man_n;
        if (best_man_n > 0) {
            memcpy(maneuvers, best_man,
                (size_t)best_man_n * sizeof(maneuvers[0]));
        }
    }
    vmap_free(best_pts);
    vmap_free(best_man);
    return true;
}

static bool corridor_border_picks_inc(uint32_t * picks,
    const uint32_t * border_counts, uint32_t n_borders)
{
    int i;

    if (!picks || !border_counts || n_borders == 0) {
        return false;
    }

    for (i = (int)n_borders - 1; i >= 0; i--) {
        uint32_t cap = border_counts[i];

        if (cap > VMAP_ROUTE_CORRIDOR_PORTAL_TRIALS) {
            cap = VMAP_ROUTE_CORRIDOR_PORTAL_TRIALS;
        }
        picks[i]++;
        if (picks[i] < cap) {
            return true;
        }
        picks[i] = 0;
    }
    return false;
}

static bool corridor_plan_multi_regions(const char * map_dir,
    const vmap_cell_id_t * path, uint32_t path_len, double from_lon, double from_lat,
    double to_lon, double to_lat, vmap_route_pt_t * pts, uint32_t * pt_count,
    double * total_m, vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count)
{
    corridor_portal_t border_pools[VMAP_ROUTE_CORRIDOR_MAX_BORDERS]
        [VMAP_ROUTE_PORTAL_TRIAL_MAX];
    corridor_portal_t portals[VMAP_ROUTE_CORRIDOR_MAX_BORDERS];
    uint32_t border_counts[VMAP_ROUTE_CORRIDOR_MAX_BORDERS];
    uint32_t picks[VMAP_ROUTE_CORRIDOR_MAX_BORDERS];
    corridor_seg_scratch_t scratch;
    vmap_route_pt_t * trial_pts;
    vmap_route_pt_t * best_pts;
    vmap_route_maneuver_t * trial_man;
    vmap_route_maneuver_t * best_man;
    const uint32_t n_borders = path_len > 0 ? path_len - 1u : 0;
    uint32_t best_n = 0;
    uint32_t best_man_n = 0;
    double best_total = 0.0;
    bool have_best = false;
    uint32_t i;

    if (n_borders == 0 || n_borders > VMAP_ROUTE_CORRIDOR_MAX_BORDERS) {
        VMAP_NAV_WARN("corridor: bad border count %u", n_borders);
        return false;
    }

    memset(border_pools, 0, sizeof(border_pools));
    memset(border_counts, 0, sizeof(border_counts));
    memset(picks, 0, sizeof(picks));
    memset(&scratch, 0, sizeof(scratch));

    trial_pts = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(vmap_route_pt_t));
    best_pts = (vmap_route_pt_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_PTS * sizeof(vmap_route_pt_t));
    trial_man = (vmap_route_maneuver_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_MANEUVERS * sizeof(vmap_route_maneuver_t));
    best_man = (vmap_route_maneuver_t *)vmap_malloc(
        (size_t)VMAP_ROUTE_MAX_MANEUVERS * sizeof(vmap_route_maneuver_t));
    if (!trial_pts || !best_pts || !trial_man || !best_man
        || !corridor_seg_scratch_alloc(&scratch)) {
        VMAP_NAV_ERR("corridor: scratch alloc failed");
        vmap_free(trial_pts);
        vmap_free(best_pts);
        vmap_free(trial_man);
        vmap_free(best_man);
        corridor_seg_scratch_free(&scratch);
        return false;
    }

    for (i = 0; i < n_borders; i++) {
        border_counts[i] = corridor_portal_collect(map_dir, path[i], path[i + 1u],
            border_pools[i], VMAP_ROUTE_PORTAL_TRIAL_MAX);
        corridor_portal_rank_by_detour(border_pools[i], border_counts[i],
            from_lon, from_lat, to_lon, to_lat);
        if (border_counts[i] == 0) {
            VMAP_NAV_WARN("corridor: no portals r%u->r%u",
                (unsigned)path[i], (unsigned)path[i + 1u]);
            vmap_free(trial_pts);
            vmap_free(best_pts);
            vmap_free(trial_man);
            vmap_free(best_man);
            corridor_seg_scratch_free(&scratch);
            return false;
        }
        corridor_portal_sort(border_pools[i], border_counts[i]);
    }

    do {
        uint32_t trial_n = 0;
        uint32_t trial_man_n = 0;
        double trial_total = 0.0;

        for (i = 0; i < n_borders; i++) {
            portals[i] = border_pools[i][picks[i]];
        }

        if (!corridor_plan_segments(map_dir, path, path_len, portals, from_lon,
                from_lat, to_lon, to_lat, trial_pts, &trial_n, &trial_total,
                trial_man, maneuver_cap, &trial_man_n, &scratch)) {
            continue;
        }

        if (!have_best || trial_total < best_total) {
            have_best = true;
            best_n = trial_n;
            best_total = trial_total;
            best_man_n = trial_man_n;
            memcpy(best_pts, trial_pts, (size_t)best_n * sizeof(best_pts[0]));
            if (maneuvers && best_man_n > 0) {
                memcpy(best_man, trial_man,
                    (size_t)best_man_n * sizeof(best_man[0]));
            }
            VMAP_NAV_LOG("corridor: multi pick borders=%u len=%.0fm",
                n_borders, trial_total);
        }
    } while (corridor_border_picks_inc(picks, border_counts, n_borders));

    vmap_free(trial_pts);
    vmap_free(trial_man);
    corridor_seg_scratch_free(&scratch);

    if (!have_best) {
        VMAP_NAV_WARN("corridor: all multi portal trials failed len=%u",
            path_len);
        vmap_free(best_pts);
        vmap_free(best_man);
        return false;
    }

    *pt_count = best_n;
    *total_m = best_total;
    memcpy(pts, best_pts, (size_t)best_n * sizeof(pts[0]));
    if (maneuvers && maneuver_count) {
        *maneuver_count = best_man_n;
        if (best_man_n > 0) {
            memcpy(maneuvers, best_man,
                (size_t)best_man_n * sizeof(maneuvers[0]));
        }
    }
    vmap_free(best_pts);
    vmap_free(best_man);
    return true;
}

/**
 * @brief vmap route corridor needed。
 */
bool vmap_route_corridor_needed(const char * map_dir, double from_lon,
    double from_lat, double to_lon, double to_lat)
{
    vmap_cell_id_t start_rid;
    vmap_cell_id_t dest_rid;

    if (!map_dir || !vmap_map_catalog_load(map_dir)
        || !vmap_map_catalog_is_regional()) {
        return false;
    }
    if (!vmap_map_catalog_find_region(from_lon, from_lat, &start_rid)
        || !vmap_map_catalog_find_region(to_lon, to_lat, &dest_rid)) {
        return false;
    }
    if (start_rid == dest_rid) {
        return false;
    }
    /*
     * Cross-cell by grid, but the destination may still be inside the FROM
     * region's clipped graph (border overlap).  Plan single-region in that
     * case to avoid the slow corridor path and its border stub artifacts.
     */
    if (region_covers_point(vmap_map_catalog_region(start_rid), to_lon, to_lat,
            VMAP_ROUTE_REGION_REACH_M)) {
        VMAP_NAV_LOG("corridor: dest in r%u overlap (%.0fm band) -> single region",
            (unsigned)start_rid, VMAP_ROUTE_REGION_REACH_M);
        return false;
    }
    return true;
}

/**
 * @brief vmap route compute corridor。
 */
bool vmap_route_compute_corridor(const char * map_dir, double from_lon,
    double from_lat, double to_lon, double to_lat, vmap_route_pt_t * pts,
    uint32_t * pt_count, double * total_m, vmap_route_maneuver_t * maneuvers,
    uint32_t maneuver_cap, uint32_t * maneuver_count)
{
    vmap_cell_id_t start_rid;
    vmap_cell_id_t dest_rid;
    vmap_cell_id_t path[VMAP_ROUTE_REGION_PATH_MAX];
    uint32_t path_len = 0;
    vmap_route_graph_t * graph = NULL;
    bool ok;

    if (!map_dir || !pts || !pt_count || !total_m) {
        return false;
    }

    *pt_count = 0;
    *total_m = 0.0;
    if (maneuver_count) {
        *maneuver_count = 0;
    }

    if (!vmap_map_catalog_load(map_dir) || !vmap_map_catalog_is_regional()) {
        VMAP_NAV_ERR("corridor: not regional map_dir=%s", map_dir);
        return false;
    }

    if (!vmap_map_catalog_find_region(from_lon, from_lat, &start_rid)
        || !vmap_map_catalog_find_region(to_lon, to_lat, &dest_rid)) {
        VMAP_NAV_WARN("corridor: start/end outside catalog");
        return false;
    }

    if (start_rid == dest_rid) {
        graph = vmap_route_graph_acquire_region(map_dir, start_rid);
        if (!graph) {
            return false;
        }
        ok = vmap_route_compute(graph, from_lon, from_lat, to_lon, to_lat,
            pts, pt_count, total_m, maneuvers, maneuver_cap, maneuver_count);
        vmap_route_graph_release_region(graph);
        return ok;
    }

    if (!vmap_map_catalog_region_path(start_rid, dest_rid, path,
            VMAP_ROUTE_REGION_PATH_MAX, &path_len)) {
        VMAP_NAV_WARN("corridor: no region path r%u->r%u",
            (unsigned)start_rid, (unsigned)dest_rid);
        return false;
    }

    VMAP_NAV_OUT("corridor: path r%u->r%u len=%u regions",
        (unsigned)start_rid, (unsigned)dest_rid, path_len);
    for (uint32_t i = 0; i < path_len; i++) {
        VMAP_NAV_LOG("corridor:  [%u] r%u", i, (unsigned)path[i]);
    }

    if (path_len == 2) {
        ok = corridor_plan_two_regions(map_dir, path[0], path[1], from_lon,
            from_lat, to_lon, to_lat, pts, pt_count, total_m, maneuvers,
            maneuver_cap, maneuver_count);
    } else {
        ok = corridor_plan_multi_regions(map_dir, path, path_len, from_lon,
            from_lat, to_lon, to_lat, pts, pt_count, total_m, maneuvers,
            maneuver_cap, maneuver_count);
    }

     if (ok) {
        VMAP_NAV_OUT("corridor: ok pts=%u len=%.0fm",
            *pt_count, *total_m);
    }
    return ok;
}

static bool corridor_plan_path_batch(const char * map_dir,
    const vmap_cell_id_t * path, uint32_t path_len, uint32_t region_begin,
    uint32_t max_regions, double from_lon, double from_lat, double to_lon,
    double to_lat, vmap_route_pt_t * pts, uint32_t * pt_count, double * total_m,
    vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count, uint32_t * out_regions_planned)
{
    uint32_t remain;
    uint32_t sub_len;
    const vmap_cell_id_t * sub;
    double batch_to_lon;
    double batch_to_lat;

    if (!path || path_len == 0 || region_begin >= path_len || !out_regions_planned) {
        return false;
    }

    remain = path_len - region_begin;
    sub_len = max_regions > 0 ? max_regions : remain;
    if (sub_len > remain) {
        sub_len = remain;
    }
    if (sub_len == 0) {
        return false;
    }

    sub = path + region_begin;
    batch_to_lon = to_lon;
    batch_to_lat = to_lat;

    if (region_begin + sub_len < path_len) {
        corridor_portal_t pool[VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX];
        uint32_t pn;

        pn = corridor_portal_collect(map_dir, sub[sub_len - 1u],
            path[region_begin + sub_len], pool,
            VMAP_ROUTE_BORDER_PORTAL_QUERY_MAX);
        if (pn == 0) {
            return false;
        }
        corridor_portal_sort(pool, pn);
        corridor_portal_rank_by_detour(pool, pn, from_lon, from_lat, to_lon,
            to_lat);
        batch_to_lon = pool[0].lon;
        batch_to_lat = pool[0].lat;
    }

    *out_regions_planned = sub_len;

    if (sub_len == 1) {
        vmap_route_graph_t * graph;
        bool ok;

        graph = vmap_route_graph_acquire_region(map_dir, sub[0]);
        if (!graph) {
            return false;
        }
        ok = vmap_route_compute(graph, from_lon, from_lat, batch_to_lon, batch_to_lat,
            pts, pt_count, total_m, maneuvers, maneuver_cap, maneuver_count);
        vmap_route_graph_release_region(graph);
        return ok;
    }

    if (sub_len == 2) {
        return corridor_plan_two_regions(map_dir, sub[0], sub[1], from_lon,
            from_lat, batch_to_lon, batch_to_lat, pts, pt_count, total_m,
            maneuvers, maneuver_cap, maneuver_count);
    }

    return corridor_plan_multi_regions(map_dir, sub, sub_len, from_lon, from_lat,
        batch_to_lon, batch_to_lat, pts, pt_count, total_m, maneuvers,
        maneuver_cap, maneuver_count);
}

/**
 * @brief vmap route compute rolling。
 */
bool vmap_route_compute_rolling(const vmap_route_rolling_req_t * req,
    vmap_route_pt_t * pts, uint32_t * pt_count, double * total_m,
    vmap_route_maneuver_t * maneuvers, uint32_t maneuver_cap,
    uint32_t * maneuver_count, uint32_t * out_regions_planned)
{
    vmap_cell_id_t path[VMAP_ROUTE_REGION_PATH_MAX];
    uint32_t path_len = 0;
    uint32_t region_begin = 0;
    uint32_t max_regions = VMAP_ROUTE_REFINE_BATCH;
    vmap_cell_id_t start_rid;
    vmap_cell_id_t dest_rid;
    vmap_route_graph_t * graph = NULL;
    bool ok;

    if (!req || !req->map_dir || !pts || !pt_count || !total_m
        || !out_regions_planned) {
        return false;
    }

    *pt_count = 0;
    *total_m = 0.0;
    *out_regions_planned = 0;
    if (maneuver_count) {
        *maneuver_count = 0;
    }

    /* 取证（2026-09-19）：rolling 与直规对同一对点给出过 3576 m / 1986 m 两种长度，
     * 先得知道**两条规划器的输入是不是同一对起终点** —— 直规那边 vmap_route_plan()
     * 在吸附前会打 `compute: snap start/dest (raw)`，这里对齐打一份 rolling 的。 */
    VMAP_NAV_OUT("rolling: from (%.7f,%.7f) to (%.7f,%.7f) begin=%u max=%u path=%u",
        req->from_lon, req->from_lat, req->to_lon, req->to_lat,
        (unsigned)req->region_begin, (unsigned)req->max_regions,
        (unsigned)req->region_path_len);

    if (!vmap_map_catalog_load(req->map_dir)
        || !vmap_map_catalog_is_regional()) {
        return false;
    }

    (void)vmap_route_portal_load(req->map_dir);

    if (!vmap_map_catalog_find_region(req->from_lon, req->from_lat, &start_rid)
        || !vmap_map_catalog_find_region(req->to_lon, req->to_lat, &dest_rid)) {
        return false;
    }

    if (req->region_path && req->region_path_len > 0) {
        if (req->region_path_len > VMAP_ROUTE_REGION_PATH_MAX) {
            return false;
        }
        memcpy(path, req->region_path,
            (size_t)req->region_path_len * sizeof(path[0]));
        path_len = req->region_path_len;
        region_begin = req->region_begin;
        if (req->max_regions > 0) {
            max_regions = req->max_regions;
        }
    } else if (start_rid == dest_rid) {
        path[0] = start_rid;
        path_len = 1;
    } else if (!vmap_map_catalog_region_path(start_rid, dest_rid, path,
            VMAP_ROUTE_REGION_PATH_MAX, &path_len)) {
        return false;
    }

    if (path_len == 1) {
        graph = vmap_route_graph_acquire_region(req->map_dir, path[0]);
        if (!graph) {
            return false;
        }
        ok = vmap_route_compute(graph, req->from_lon, req->from_lat,
            req->to_lon, req->to_lat, pts, pt_count, total_m, maneuvers,
            maneuver_cap, maneuver_count);
        vmap_route_graph_release_region(graph);
        *out_regions_planned = ok ? 1u : 0u;
        return ok;
    }

    ok = corridor_plan_path_batch(req->map_dir, path, path_len, region_begin,
        max_regions, req->from_lon, req->from_lat, req->to_lon, req->to_lat,
        pts, pt_count, total_m, maneuvers, maneuver_cap, maneuver_count,
        out_regions_planned);
    if (ok) {
        VMAP_NAV_OUT("rolling: ok pts=%u len=%.0fm regions=%u begin=%u",
            *pt_count, *total_m, *out_regions_planned, region_begin);
    }
    return ok;
}
