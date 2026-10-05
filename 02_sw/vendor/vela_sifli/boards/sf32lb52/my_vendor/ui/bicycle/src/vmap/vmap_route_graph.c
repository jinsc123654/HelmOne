/**
 * @file vmap_route_graph.c
 * @brief vmap route_graph 模块。
 */

#include "vmap_route_graph.h"

#include "vmap_map_catalog.h"
#include "vmap_grid.h"
#include "vmap_route.h"
#include "vmap_route_log.h"
#include "vmap_config.h"
#include "vmap_format.h"
#include "vmap_geo.h"
#include "vmap_tile_index.h"
#include "vmap_alloc.h"
#include "vmap_vpk_heap.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vmap_route_graph {
    uint8_t * buf;
    size_t sz;
    uint32_t node_count;
    uint32_t edge_count;
    uint32_t adj_edge_count;
    const int32_t * nodes;
    const uint8_t * edge_raw;
    uint8_t edge_stride;
    const uint32_t * adj_off;
    const uint32_t * adj_edges;
    bool adj_owned;
    const char * strings;
    /* VGRF v2 SNAP grid (points into buf). */
    bool has_snap;
    int32_t snap_olon_e7;
    int32_t snap_olat_e7;
    uint32_t snap_cell_e7;
    uint16_t snap_ncol;
    uint16_t snap_nrow;
    const uint32_t * snap_cell_off;
    const uint32_t * snap_nodes;
    bool has_elev;
    const int16_t * ele_m;
};

#define VMAP_SNAP_SCRATCH_K 16

typedef struct {
    uint32_t node;
    double d;
} vmap_snap_item_t;

/* Route worker runs one planner job at a time; keep snap scratch off pthread stack. */
static struct {
    vmap_snap_item_t best[VMAP_SNAP_SCRATCH_K];
    uint32_t raw[VMAP_SNAP_SCRATCH_K];
} g_snap_scratch;

static inline uint16_t rd_u16(const uint8_t * p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t * p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint32_t road_weight(uint8_t cls)
{
    switch (cls) {
    case VMAP_ROAD_MOTORWAY:   return 0;
    case VMAP_ROAD_PRIMARY:    return 85;
    case VMAP_ROAD_SECONDARY:  return 90;
    case VMAP_ROAD_TERTIARY:   return 95;
    case VMAP_ROAD_RESIDENTIAL:return 110;
    case VMAP_ROAD_SERVICE:    return 135;
    case VMAP_ROAD_PATH:       return 120;
    default:                   return 110;
    }
}

/**
 * @brief vmap route graph edge cost。
 */
uint32_t vmap_route_graph_edge_cost(const vmap_graph_edge_t * e)
{
    uint64_t cost;

    if (!e) {
        return UINT32_MAX;
    }
    const uint32_t w = road_weight(e->road_class);

    if (w == 0 || (e->flags & VGRF_EDGE_NO_BIKE)) {
        return UINT32_MAX;
    }
    cost = ((uint64_t)e->length_cm * w) / 100u;
    /*
     * 同等级下优先有名称、路况更明确的道路，避免为了少量距离抄入
     * 无名支路。NO_BIKE 仍具有最高优先级，不会因道路等级而放行。
     */
    if (e->name_off == VGRF_NAME_NONE) {
        cost = (cost * 112u) / 100u;
    }
    return cost < UINT32_MAX ? (uint32_t)cost : UINT32_MAX - 1u;
}

/**
 * @brief vmap route graph node is routable。
 * @return 请求的值。
 */
bool vmap_route_graph_node_is_routable(const vmap_route_graph_t * g, uint32_t node)
{
    uint32_t cnt = 0;
    const uint32_t * adj;

    if (!g || node >= g->node_count) {
        return false;
    }

    adj = vmap_route_graph_adj_edges(g, node, &cnt);
    if (!adj) {
        return false;
    }
    for (uint32_t ai = 0; ai < cnt; ai++) {
        const uint32_t ei = adj[ai];

        vmap_graph_edge_t e;

        if (ei >= g->edge_count) {
            continue;
        }
        if (!vmap_route_graph_edge(g, ei, &e)) {
            continue;
        }
        if (vmap_route_graph_edge_cost(&e) != UINT32_MAX) {
            return true;
        }
    }
    return false;
}

static uint32_t snap_pick_routable(const vmap_route_graph_t * g,
    const uint32_t * raw, uint32_t raw_n,
    uint32_t * out_nodes, uint32_t max_k)
{
    uint32_t out_n = 0;

    for (uint32_t i = 0; i < raw_n && out_n < max_k; i++) {
        if (raw[i] >= g->node_count) {
            continue;
        }
        if (vmap_route_graph_node_is_routable(g, raw[i])) {
            out_nodes[out_n++] = raw[i];
        }
    }
    return out_n;
}

static bool route_graph_parse_snap(vmap_route_graph_t * g, size_t snap_off)
{
    const uint8_t * p;
    uint16_t snap_ver;
    uint16_t cell_m;

    if (!g || snap_off + VGRF_SNAP_HDR_SIZE > g->sz) {
        return false;
    }
    p = g->buf + snap_off;
    if (!(p[0] == VGRF_SNAP_MAGIC0 && p[1] == VGRF_SNAP_MAGIC1
            && p[2] == VGRF_SNAP_MAGIC2 && p[3] == VGRF_SNAP_MAGIC3)) {
        return false;
    }
    snap_ver = rd_u16(p + 4);
    cell_m = rd_u16(p + 6);
    if (snap_ver != VGRF_SNAP_VERSION) {
        return false;
    }
    g->snap_olon_e7 = (int32_t)rd_u32(p + 8);
    g->snap_olat_e7 = (int32_t)rd_u32(p + 12);
    g->snap_ncol = rd_u16(p + 16);
    g->snap_nrow = rd_u16(p + 18);
    if (g->snap_ncol == 0 || g->snap_nrow == 0) {
        return false;
    }
    g->snap_cell_e7 = (uint32_t)cell_m * 10000000u / 111320u;
    if (g->snap_cell_e7 < 1000u) {
        g->snap_cell_e7 = 1000u;
    }
    {
        const uint32_t ncells = (uint32_t)g->snap_ncol * (uint32_t)g->snap_nrow;
        const size_t need = (size_t)ncells + 1u;

        if (snap_off + VGRF_SNAP_HDR_SIZE + need * 4u > g->sz) {
            return false;
        }
        g->snap_cell_off = (const uint32_t *)(p + VGRF_SNAP_HDR_SIZE);
        {
            const uint32_t n_list_cells = (uint32_t)g->snap_ncol * (uint32_t)g->snap_nrow;
            const uint32_t n_list = g->snap_cell_off[n_list_cells];
            const size_t nodes_at = snap_off + VGRF_SNAP_HDR_SIZE
                + (size_t)(n_list_cells + 1u) * 4u;

            if (nodes_at + (size_t)n_list * 4u > g->sz) {
                return false;
            }
            g->snap_nodes = (const uint32_t *)(g->buf + nodes_at);
        }
        g->has_snap = true;
        VMAP_NAV_LOG("graph snap grid %ux%u cell_e7=%u",
            g->snap_ncol, g->snap_nrow, g->snap_cell_e7);
    }
    return true;
}

static size_t route_graph_snap_end_off(const vmap_route_graph_t * g, size_t snap_off)
{
    uint32_t ncells;
    uint32_t n_list;

    if (!g || !g->has_snap || !g->snap_cell_off) {
        return snap_off;
    }
    ncells = (uint32_t)g->snap_ncol * (uint32_t)g->snap_nrow;
    n_list = g->snap_cell_off[ncells];
    return snap_off + VGRF_SNAP_HDR_SIZE
        + (size_t)(ncells + 1u) * 4u
        + (size_t)n_list * 4u;
}

static bool route_graph_parse_elev(vmap_route_graph_t * g, size_t elev_off)
{
    const uint8_t * p;
    uint16_t ver;
    uint32_t count;

    if (!g || elev_off + VGRF_ELEV_HDR_SIZE > g->sz) {
        return false;
    }
    p = g->buf + elev_off;
    if (!(p[0] == VGRF_ELEV_MAGIC0 && p[1] == VGRF_ELEV_MAGIC1
            && p[2] == VGRF_ELEV_MAGIC2 && p[3] == VGRF_ELEV_MAGIC3)) {
        return false;
    }
    ver = rd_u16(p + 4);
    count = rd_u32(p + 8);
    if (ver != VGRF_ELEV_VERSION || count != g->node_count) {
        return false;
    }
    if (elev_off + VGRF_ELEV_HDR_SIZE + (size_t)count * 2u > g->sz) {
        return false;
    }
    g->ele_m = (const int16_t *)(p + VGRF_ELEV_HDR_SIZE);
    g->has_elev = true;
    VMAP_NAV_LOG("graph ELEV nodes=%u", (unsigned)count);
    return true;
}

static void route_graph_release_owned_adj(vmap_route_graph_t * g)
{
    if (!g || !g->adj_owned) {
        return;
    }
    vmap_free((void *)g->adj_off);
    vmap_free((void *)g->adj_edges);
    g->adj_off = NULL;
    g->adj_edges = NULL;
    g->adj_owned = false;
    g->adj_edge_count = 0;
}

static void route_graph_abort(vmap_route_graph_t * g)
{
    if (!g) {
        return;
    }
    route_graph_release_owned_adj(g);
    vmap_free(g);
}

static bool route_graph_build_undirected_adj(vmap_route_graph_t * g)
{
    const uint32_t n = g ? g->node_count : 0;
    const uint32_t ecount = g ? g->edge_count : 0;
    uint32_t * deg;
    uint32_t * off;
    uint32_t * list = NULL;
    uint32_t total = 0;
    uint32_t i;

    if (!g || n == 0) {
        return false;
    }

    deg = (uint32_t *)vmap_malloc((size_t)n * sizeof(uint32_t));
    if (!deg) {
        return false;
    }
    memset(deg, 0, (size_t)n * sizeof(uint32_t));

    for (i = 0; i < ecount; i++) {
        vmap_graph_edge_t e;

        if (!vmap_route_graph_edge(g, i, &e) || e.from >= n || e.to >= n) {
            vmap_free(deg);
            return false;
        }
        deg[e.from]++;
        if ((e.flags & VGRF_EDGE_ONEWAY) == 0u) {
            deg[e.to]++;
        }
    }
    for (i = 0; i < n; i++) {
        if (total > UINT32_MAX - deg[i]) {
            vmap_free(deg);
            return false;
        }
        total += deg[i];
    }

    off = (uint32_t *)vmap_malloc((size_t)(n + 1u) * sizeof(uint32_t));
    if (!off) {
        vmap_free(deg);
        return false;
    }
    off[0] = 0;
    for (i = 0; i < n; i++) {
        off[i + 1] = off[i] + deg[i];
        deg[i] = off[i];
    }

    if (total > 0u) {
        list = (uint32_t *)vmap_malloc((size_t)total * sizeof(uint32_t));
        if (!list) {
            vmap_free(deg);
            vmap_free(off);
            return false;
        }
    }
    for (i = 0; i < ecount; i++) {
        vmap_graph_edge_t e;

        if (!vmap_route_graph_edge(g, i, &e)) {
            vmap_free(deg);
            vmap_free(off);
            vmap_free(list);
            return false;
        }
        if (list) {
            list[deg[e.from]++] = i;
            if ((e.flags & VGRF_EDGE_ONEWAY) == 0u) {
                list[deg[e.to]++] = i;
            }
        }
    }
    vmap_free(deg);
    g->adj_off = off;
    g->adj_edges = list;
    g->adj_edge_count = total;
    g->adj_owned = true;
    return true;
}

static bool route_graph_parse_buffer(vmap_route_graph_t ** out, uint8_t * buf,
    size_t fsize)
{
    vmap_route_graph_t * g;
    const uint8_t * p;
    uint8_t ver;
    size_t hdr_sz;
    uint32_t snap_off = 0;
    uint32_t n;
    uint32_t ecount;
    size_t body_end;
    size_t edge_stride;

    if (!out || !buf || fsize < VGRF_HDR_SIZE_V1) {
        return false;
    }
    *out = NULL;

    if (!(buf[0] == VGRF_MAGIC0 && buf[1] == VGRF_MAGIC1
            && buf[2] == VGRF_MAGIC2 && buf[3] == VGRF_MAGIC3)) {
        return false;
    }
    ver = buf[4];
    if (ver < 1 || ver > VGRF_VERSION) {
        return false;
    }

    hdr_sz = (ver >= 2) ? (size_t)VGRF_HDR_SIZE_V2
                        : (size_t)VGRF_HDR_SIZE_V1;
    if (fsize < hdr_sz) {
        return false;
    }
    if (ver >= 2) {
        snap_off = rd_u32(buf + VGRF_HDR_SNAP_OFF);
    }

    n = rd_u32(buf + 8);
    ecount = rd_u32(buf + 12);
    body_end = (snap_off > hdr_sz && snap_off < fsize) ? snap_off : fsize;
    edge_stride = (ver >= 3)
        ? (size_t)VGRF_EDGE_SIZE_V3 : (size_t)VGRF_EDGE_SIZE_V2;

    p = buf + hdr_sz;
    if ((size_t)(p - buf) + (size_t)n * 8u
            + (size_t)ecount * edge_stride > body_end) {
        VMAP_NAV_ERR("graph parse: core hdr overflow");
        return false;
    }
    if (ver < 4 && (size_t)(p - buf) + (size_t)n * 8u
            + (size_t)ecount * edge_stride
            + (size_t)(n + 1u) * 4u > body_end) {
        VMAP_NAV_ERR("graph parse: core hdr overflow");
        return false;
    }

    g = (vmap_route_graph_t *)vmap_malloc(sizeof(*g));
    if (!g) {
        return false;
    }
    memset(g, 0, sizeof(*g));
    g->buf = buf;
    g->sz = fsize;
    g->node_count = n;
    g->edge_count = ecount;
    g->nodes = (const int32_t *)p;
    p += (size_t)n * 8u;
    g->edge_raw = p;
    g->edge_stride = (uint8_t)edge_stride;
    p += (size_t)ecount * edge_stride;

    if (ver >= 4) {
        g->strings = (const char *)p;
        if (n > 0u && !route_graph_build_undirected_adj(g)) {
            VMAP_NAV_ERR("graph parse: adj build fail");
            route_graph_abort(g);
            return false;
        }
    } else {
        g->adj_off = (const uint32_t *)p;
        p += (size_t)(n + 1u) * 4u;
        g->adj_edge_count = g->adj_off[n];
        if (g->adj_off[0] != 0u) {
            VMAP_NAV_ERR("graph parse: bad adj_off[0]");
            route_graph_abort(g);
            return false;
        }
        if ((size_t)(p - buf) + (size_t)g->adj_edge_count * 4u > body_end) {
            VMAP_NAV_ERR("graph parse: adj_edges overflow");
            route_graph_abort(g);
            return false;
        }
        g->adj_edges = (const uint32_t *)p;
        p += (size_t)g->adj_edge_count * 4u;
        g->strings = (const char *)p;

        for (uint32_t i = 0; i < n; i++) {
            if (g->adj_off[i] > g->adj_off[i + 1]
                || g->adj_off[i + 1] > g->adj_edge_count) {
                VMAP_NAV_ERR("graph parse: bad adj_off[%u]", i);
                route_graph_abort(g);
                return false;
            }
        }
        for (uint32_t ai = 0; ai < g->adj_edge_count; ai++) {
            if (g->adj_edges[ai] >= ecount) {
                VMAP_NAV_ERR("graph parse: bad adj edge %u", ai);
                route_graph_abort(g);
                return false;
            }
        }
    }

    if (snap_off > 0) {
        if (!route_graph_parse_snap(g, snap_off)) {
            VMAP_NAV_WARN("graph parse: snap section invalid, scan fallback");
        } else {
            const size_t elev_off = route_graph_snap_end_off(g, snap_off);

            if (!route_graph_parse_elev(g, elev_off)) {
                /* Optional; old packs have no ELEV. */
            }
        }
    }

    *out = g;
    VMAP_NAV_LOG("graph parse: v%u nodes=%u edges=%u adj=%u owned=%d bytes=%zu snap=%d elev=%d (psram)",
        ver, n, ecount, g->adj_edge_count, g->adj_owned ? 1 : 0, fsize,
        g->has_snap ? 1 : 0, g->has_elev ? 1 : 0);
    vmap_route_plan_yield();
    return true;
}

static bool route_read_vpk_chunk(const char * path, uint32_t offset, uint32_t size,
    uint8_t * dst)
{
    return vmap_vpk_heap_copy(VMAP_CELL_NONE, path,
        VMAP_VPK_HDR_SIZE + offset, dst, size);
}

static bool route_read_cell_chunk(vmap_cell_id_t region_id, const char * path,
    uint32_t offset, uint32_t size, uint8_t * dst)
{
    return vmap_vpk_heap_copy(region_id, path,
        VMAP_VPK_HDR_SIZE + offset, dst, size);
}

static bool route_graph_load_packed(vmap_route_graph_t ** out, const char * map_dir)
{
    char pack_path[96];
    vmap_tile_index_t * idx;
    const vmap_graph_shard_ref_t * shards;
    uint16_t count;
    uint32_t total;
    uint8_t * buf;
    size_t done = 0;

    if (!out || !map_dir) {
        return false;
    }
    *out = NULL;

    idx = vmap_tile_index_create();
    if (!idx) {
        return false;
    }
    if (!vmap_tile_index_load(idx, map_dir, 0)
        || !vmap_tile_index_graph(idx, &shards, &count, &total)) {
        vmap_tile_index_destroy(idx);
        return false;
    }

    buf = (uint8_t *)vmap_malloc(total);
    if (!buf) {
        vmap_tile_index_destroy(idx);
        return false;
    }

    for (uint16_t i = 0; i < count; i++) {
        if (done + shards[i].size > total) {
            vmap_free(buf);
            vmap_tile_index_destroy(idx);
            return false;
        }
        snprintf(pack_path, sizeof(pack_path), "%s/p%03u.%s",
            map_dir, (unsigned)shards[i].pack_id, VMAP_SHARD_EXT);
        if (!route_read_vpk_chunk(pack_path, shards[i].offset, shards[i].size,
                buf + done)) {
            vmap_free(buf);
            vmap_tile_index_destroy(idx);
            return false;
        }
        done += shards[i].size;
    }
    vmap_tile_index_destroy(idx);

    if (done != total) {
        vmap_free(buf);
        return false;
    }
    if (!route_graph_parse_buffer(out, buf, total)) {
        vmap_free(buf);
        return false;
    }
    return true;
}

/**
 * @brief vmap route graph load from map dir。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_graph_load_from_map_dir(vmap_route_graph_t ** out,
    const char * map_dir)
{
    if (!out || !map_dir) {
        VMAP_NAV_ERR("graph load: bad args");
        return false;
    }
    if (vmap_map_catalog_load(map_dir) && vmap_map_catalog_is_regional()) {
        VMAP_NAV_ERR("graph load: VREG map — use load_from_region");
        return false;
    }
    if (!route_graph_load_packed(out, map_dir)) {
        VMAP_NAV_ERR("graph load failed dir=%s", map_dir);
        return false;
    }
    return true;
}

static void route_resolve_region_vpk(char * buf, size_t sz,
    const char * dir, vmap_cell_id_t region_id)
{
    vmap_grid_vpk_path(buf, sz, dir, VMAP_CELL_IX(region_id),
        VMAP_CELL_IY(region_id));
}

static vmap_route_graph_t * region_graph_load_fresh(const char * map_dir,
    vmap_cell_id_t region_id)
{
    char path[VMAP_GRID_PATH_MAX];
    uint8_t hdr[VMAP_REG_PACK_INDEX_HDR_SIZE];
    uint32_t graph_off;
    uint32_t graph_sz;
    vmap_route_graph_t * g = NULL;
    uint8_t * buf;

    if (!map_dir) {
        return NULL;
    }

    route_resolve_region_vpk(path, sizeof(path), map_dir, region_id);
    if (!route_read_cell_chunk(region_id, path, 0, sizeof(hdr), hdr)) {
        VMAP_NAV_ERR("graph region open fail %s", path);
        return NULL;
    }

    if (!(hdr[0] == VMAP_REG_PACK_INDEX_MAGIC0
            && hdr[1] == VMAP_REG_PACK_INDEX_MAGIC1
            && hdr[2] == VMAP_REG_PACK_INDEX_MAGIC2
            && hdr[3] == VMAP_REG_PACK_INDEX_MAGIC3)) {
        VMAP_NAV_ERR("graph region bad RIDX %s", path);
        return NULL;
    }
    graph_off = rd_u32(hdr + 12);
    graph_sz = rd_u32(hdr + 16);
    if (graph_sz == 0) {
        VMAP_NAV_ERR("graph region %u has no graph", (unsigned)region_id);
        return NULL;
    }

    buf = (uint8_t *)vmap_malloc(graph_sz);
    if (!buf) {
        return NULL;
    }
    if (!route_read_cell_chunk(region_id, path, graph_off, graph_sz, buf)) {
        vmap_free(buf);
        return NULL;
    }
    if (!route_graph_parse_buffer(&g, buf, graph_sz)) {
        vmap_free(buf);
        return NULL;
    }
    VMAP_NAV_LOG("graph region %u loaded %u B", (unsigned)region_id,
        (unsigned)graph_sz);
    return g;
}

/**
 * @brief vmap route graph load from region。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_graph_load_from_region(vmap_route_graph_t ** out,
    const char * map_dir, vmap_cell_id_t region_id)
{
    if (!out || !map_dir) {
        return false;
    }
    *out = region_graph_load_fresh(map_dir, region_id);
    return *out != NULL;
}

/* ---- Region graph LRU cache -------------------------------------------
 * A single cross-region (corridor) plan re-loads the same region graphs many
 * times (portal snap + one segment plan per portal trial), and consecutive
 * off-route reroutes re-load them again. Parsing a ~150 KB VGRF from LittleFS
 * costs ~0.7s, so ~10 reloads made a single corridor replan take ~8s and the
 * rider drifted off-route before it finished — an endless reroute loop.
 *
 * This cache keeps a few parsed region graphs resident so repeated acquires hit
 * RAM. It is used only by the planner (worker thread) via acquire/release;
 * cache_clear() (UI thread, at nav stop / dir change) frees idle slots. A mutex
 * guards the small critical sections. */
#define VMAP_GRAPH_CACHE_SLOTS 3

struct graph_cache_slot {
    bool valid;
    vmap_cell_id_t region_id;
    int refs;
    uint32_t lru;
    vmap_route_graph_t * g;
};

static struct graph_cache_slot g_gcache[VMAP_GRAPH_CACHE_SLOTS];
static uint32_t g_gcache_clock;
static char g_gcache_dir[64];
static pthread_mutex_t g_gcache_lock = PTHREAD_MUTEX_INITIALIZER;

static void gcache_reset_dir_locked(const char * map_dir)
{
    int i;

    for (i = 0; i < VMAP_GRAPH_CACHE_SLOTS; i++) {
        if (g_gcache[i].valid && g_gcache[i].refs == 0) {
            vmap_route_graph_unload(g_gcache[i].g);
            memset(&g_gcache[i], 0, sizeof(g_gcache[i]));
        }
    }
    strncpy(g_gcache_dir, map_dir ? map_dir : "", sizeof(g_gcache_dir) - 1);
    g_gcache_dir[sizeof(g_gcache_dir) - 1] = '\0';
}

/**
 * @brief vmap route graph acquire region。
 */
vmap_route_graph_t * vmap_route_graph_acquire_region(const char * map_dir,
    vmap_cell_id_t region_id)
{
    vmap_route_graph_t * g = NULL;
    int i;
    int victim = -1;

    if (!map_dir) {
        return NULL;
    }

    pthread_mutex_lock(&g_gcache_lock);

    if (strncmp(g_gcache_dir, map_dir, sizeof(g_gcache_dir)) != 0) {
        gcache_reset_dir_locked(map_dir);
    }

    for (i = 0; i < VMAP_GRAPH_CACHE_SLOTS; i++) {
        if (g_gcache[i].valid && g_gcache[i].region_id == region_id) {
            g_gcache[i].refs++;
            g_gcache[i].lru = ++g_gcache_clock;
            g = g_gcache[i].g;
            pthread_mutex_unlock(&g_gcache_lock);
            return g;
        }
    }

    pthread_mutex_unlock(&g_gcache_lock);

    /* Miss: parse from flash outside the lock (slow), then insert. */
    g = region_graph_load_fresh(map_dir, region_id);
    if (!g) {
        return NULL;
    }

    pthread_mutex_lock(&g_gcache_lock);
    for (i = 0; i < VMAP_GRAPH_CACHE_SLOTS; i++) {
        if (!g_gcache[i].valid) {
            victim = i;
            break;
        }
        if (g_gcache[i].refs == 0
            && (victim < 0 || g_gcache[i].lru < g_gcache[victim].lru)) {
            victim = i;
        }
    }
    if (victim >= 0) {
        if (g_gcache[victim].valid) {
            vmap_route_graph_unload(g_gcache[victim].g);
        }
        g_gcache[victim].valid = true;
        g_gcache[victim].region_id = region_id;
        g_gcache[victim].refs = 1;
        g_gcache[victim].lru = ++g_gcache_clock;
        g_gcache[victim].g = g;
        pthread_mutex_unlock(&g_gcache_lock);
        return g;
    }
    /* All slots busy: hand back an uncached graph (released via unload). */
    pthread_mutex_unlock(&g_gcache_lock);
    return g;
}

/**
 * @brief vmap route graph release region。
 */
void vmap_route_graph_release_region(vmap_route_graph_t * g)
{
    int i;

    if (!g) {
        return;
    }

    pthread_mutex_lock(&g_gcache_lock);
    for (i = 0; i < VMAP_GRAPH_CACHE_SLOTS; i++) {
        if (g_gcache[i].valid && g_gcache[i].g == g) {
            if (g_gcache[i].refs > 0) {
                g_gcache[i].refs--;
            }
            pthread_mutex_unlock(&g_gcache_lock);
            return;
        }
    }
    pthread_mutex_unlock(&g_gcache_lock);

    /* Not cached (all-slots-busy fallback): free it. */
    vmap_route_graph_unload(g);
}

/**
 * @brief vmap route graph region cache clear。
 */
void vmap_route_graph_region_cache_clear(void)
{
    int i;

    pthread_mutex_lock(&g_gcache_lock);
    for (i = 0; i < VMAP_GRAPH_CACHE_SLOTS; i++) {
        if (g_gcache[i].valid && g_gcache[i].refs == 0) {
            vmap_route_graph_unload(g_gcache[i].g);
            memset(&g_gcache[i], 0, sizeof(g_gcache[i]));
        }
    }
    pthread_mutex_unlock(&g_gcache_lock);
}

/**
 * @brief vmap route graph load。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_graph_load(vmap_route_graph_t ** out, const char * path)
{
    FILE * fp;
    long fsize;
    uint8_t * buf;

    if (!out || !path) {
        return false;
    }
    *out = NULL;

    fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    fsize = ftell(fp);
    if (fsize < (long)VGRF_HDR_SIZE_V1) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    buf = (uint8_t *)vmap_malloc((size_t)fsize);
    if (!buf) {
        fclose(fp);
        return false;
    }
    if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        fclose(fp);
        vmap_free(buf);
        return false;
    }
    fclose(fp);

    if (!route_graph_parse_buffer(out, buf, (size_t)fsize)) {
        vmap_free(buf);
        return false;
    }
    return true;
}

/**
 * @brief vmap route graph unload。
 * @return 0 成功，负 errno 失败。
 */
void vmap_route_graph_unload(vmap_route_graph_t * g)
{
    if (!g) {
        return;
    }
    route_graph_release_owned_adj(g);
    vmap_free(g->buf);
    vmap_free(g);
}

/**
 * @brief vmap route graph node count。
 * @return 请求的值。
 */
uint32_t vmap_route_graph_node_count(const vmap_route_graph_t * g)
{
    return g ? g->node_count : 0;
}

/**
 * @brief vmap route graph edge count。
 * @return 请求的值。
 */
uint32_t vmap_route_graph_edge_count(const vmap_route_graph_t * g)
{
    return g ? g->edge_count : 0;
}

/**
 * @brief vmap route graph node lonlat。
 */
void vmap_route_graph_node_lonlat(const vmap_route_graph_t * g, uint32_t idx,
    double * lon, double * lat)
{
    if (!g || idx >= g->node_count || !lon || !lat) {
        return;
    }
    const int32_t * np = g->nodes + (size_t)idx * 2;

    *lon = np[0] / 1e7;
    *lat = np[1] / 1e7;
}

bool vmap_route_graph_has_elev(const vmap_route_graph_t * g)
{
    return g && g->has_elev && g->ele_m;
}

int16_t vmap_route_graph_node_ele_m(const vmap_route_graph_t * g, uint32_t idx)
{
    if (!vmap_route_graph_has_elev(g) || idx >= g->node_count) {
        return (int16_t)VGRF_ELE_UNKNOWN;
    }
    return g->ele_m[idx];
}

int16_t vmap_route_graph_sample_ele_m(const vmap_route_graph_t * g,
    double lon, double lat)
{
    uint32_t node = UINT32_MAX;
    double d;

    if (!vmap_route_graph_has_elev(g)) {
        return (int16_t)VGRF_ELE_UNKNOWN;
    }
    d = vmap_route_graph_nearest_distance_m(g, lon, lat, &node);
    if (d < 0.0 || node == UINT32_MAX || d > VMAP_ROUTE_SNAP_M) {
        return (int16_t)VGRF_ELE_UNKNOWN;
    }
    return vmap_route_graph_node_ele_m(g, node);
}

bool vmap_route_graph_edge(const vmap_route_graph_t * g, uint32_t i,
    vmap_graph_edge_t * out)
{
    const uint8_t * p;

    if (!g || !out || i >= g->edge_count || !g->edge_raw || g->edge_stride < 16) {
        return false;
    }
    p = g->edge_raw + (size_t)i * (size_t)g->edge_stride;
    out->from = rd_u32(p);
    out->to = rd_u32(p + 4);
    out->length_cm = rd_u32(p + 8);
    out->road_class = p[12];
    out->flags = p[13];
    if (g->edge_stride >= VGRF_EDGE_SIZE_V3) {
        out->name_off = rd_u32(p + 14);
    } else {
        uint16_t off = rd_u16(p + 14);

        out->name_off = (off == VGRF_NAME_NONE_V2) ? VGRF_NAME_NONE : off;
    }
    return true;
}

uint32_t vmap_route_graph_edge_other(const vmap_graph_edge_t * e, uint32_t node)
{
    if (!e) {
        return UINT32_MAX;
    }
    if (e->from == node) {
        return e->to;
    }
    if (e->to == node) {
        return e->from;
    }
    return UINT32_MAX;
}

/**
 * @brief vmap route graph adj edges。
 */
const uint32_t * vmap_route_graph_adj_edges(const vmap_route_graph_t * g,
    uint32_t node, uint32_t * out_count)
{
    if (!g || !g->adj_off || node >= g->node_count) {
        if (out_count) {
            *out_count = 0;
        }
        return NULL;
    }
    const uint32_t start = g->adj_off[node];
    const uint32_t end = g->adj_off[node + 1];
    const uint32_t cnt = (end >= start && start <= g->adj_edge_count
            && end <= g->adj_edge_count)
        ? (end - start) : 0u;

    if (out_count) {
        *out_count = cnt;
    }
    if (cnt == 0) {
        return NULL;
    }
    return g->adj_edges + start;
}

/**
 * @brief vmap route graph strings。
 */
const char * vmap_route_graph_strings(const vmap_route_graph_t * g)
{
    return g ? g->strings : NULL;
}

/**
 * @brief vmap route graph edge name。
 */
const char * vmap_route_graph_edge_name(const vmap_route_graph_t * g,
    const vmap_graph_edge_t * e)
{
    if (!g || !e || e->name_off == VGRF_NAME_NONE || !g->strings) {
        return NULL;
    }
    return g->strings + e->name_off;
}

/**
 * @brief vmap route graph nearest node。
 */
uint32_t vmap_route_graph_nearest_node(const vmap_route_graph_t * g,
    double lon, double lat, double max_m)
{
    uint32_t one = UINT32_MAX;

    if (!g) {
        return UINT32_MAX;
    }
    if (vmap_route_graph_snap_candidates(g, lon, lat, max_m, &one, 1) == 0) {
        return UINT32_MAX;
    }
    return one;
}

/**
 * @brief vmap route graph snap candidates。
 */
uint32_t vmap_route_graph_snap_candidates(const vmap_route_graph_t * g,
    double lon, double lat, double max_m,
    uint32_t * out_nodes, uint32_t max_k)
{
    vmap_snap_item_t * best = g_snap_scratch.best;
    uint32_t n = 0;
    uint32_t cap = VMAP_SNAP_SCRATCH_K;

    if (!g || !out_nodes || max_k == 0) {
        return 0;
    }
    if (max_k > cap) {
        max_k = cap;
    }

    if (g->has_snap && g->snap_cell_off && g->snap_nodes && g->snap_cell_e7 > 0) {
        const int32_t lon_e7 = (int32_t)(lon * 1e7);
        const int32_t lat_e7 = (int32_t)(lat * 1e7);
        const int32_t cx0 = (lon_e7 - g->snap_olon_e7) / (int32_t)g->snap_cell_e7;
        const int32_t cy0 = (lat_e7 - g->snap_olat_e7) / (int32_t)g->snap_cell_e7;
        const double cell_m = (double)g->snap_cell_e7 * 111320.0 / 1e7;
        int32_t ring = (int32_t)(max_m / cell_m) + 2;

        if (ring < 1) {
            ring = 1;
        } else if (ring > 48) {
            ring = 48;
        }

        for (int32_t dy = -ring; dy <= ring; dy++) {
            for (int32_t dx = -ring; dx <= ring; dx++) {
                const int32_t cx = cx0 + dx;
                const int32_t cy = cy0 + dy;
                uint32_t cell;
                uint32_t a;
                uint32_t b;

                if (cx < 0 || cy < 0 || cx >= (int32_t)g->snap_ncol
                    || cy >= (int32_t)g->snap_nrow) {
                    continue;
                }
                cell = (uint32_t)cy * (uint32_t)g->snap_ncol + (uint32_t)cx;
                a = g->snap_cell_off[cell];
                b = g->snap_cell_off[cell + 1u];
                for (uint32_t i = a; i < b; i++) {
                    const uint32_t node = g->snap_nodes[i];
                    double nlon;
                    double nlat;
                    double d;
                    uint32_t j;

                    if (node >= g->node_count) {
                        continue;
                    }
                    vmap_route_graph_node_lonlat(g, node, &nlon, &nlat);
                    d = vmap_geo_haversine_m(lon, lat, nlon, nlat);
                    if (d > max_m) {
                        continue;
                    }
                    for (j = 0; j < n && best[j].d <= d; j++) {
                    }
                    if (n < max_k) {
                        memmove(&best[j + 1], &best[j], (size_t)(n - j) * sizeof(best[0]));
                        best[j].node = node;
                        best[j].d = d;
                        n++;
                    } else if (j < max_k) {
                        memmove(&best[j + 1], &best[j],
                            (size_t)(max_k - j - 1) * sizeof(best[0]));
                        best[j].node = node;
                        best[j].d = d;
                    }
                }
            }
        }
    } else {
        for (uint32_t i = 0; i < g->node_count; i++) {
            double nlon;
            double nlat;
            double d;
            uint32_t j;

            vmap_route_graph_node_lonlat(g, i, &nlon, &nlat);
            d = vmap_geo_haversine_m(lon, lat, nlon, nlat);
            if (d > max_m) {
                continue;
            }
            for (j = 0; j < n && best[j].d <= d; j++) {
            }
            if (n < max_k) {
                memmove(&best[j + 1], &best[j], (size_t)(n - j) * sizeof(best[0]));
                best[j].node = i;
                best[j].d = d;
                n++;
            } else if (j < max_k) {
                memmove(&best[j + 1], &best[j], (size_t)(max_k - j - 1) * sizeof(best[0]));
                best[j].node = i;
                best[j].d = d;
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        out_nodes[i] = best[i].node;
    }
    return n;
}

/**
 * @brief vmap route graph nearest distance m。
 */
double vmap_route_graph_nearest_distance_m(const vmap_route_graph_t * g,
    double lon, double lat, uint32_t * out_node)
{
    uint32_t best = UINT32_MAX;
    double best_d = 0.0;
    bool have = false;

    if (!g) {
        if (out_node) {
            *out_node = UINT32_MAX;
        }
        return -1.0;
    }

    for (uint32_t i = 0; i < g->node_count; i++) {
        double nlon;
        double nlat;
        double d;

        vmap_route_graph_node_lonlat(g, i, &nlon, &nlat);
        d = vmap_geo_haversine_m(lon, lat, nlon, nlat);
        if (!have || d < best_d) {
            best_d = d;
            best = i;
            have = true;
        }
    }

    if (out_node) {
        *out_node = best;
    }
    return have ? best_d : -1.0;
}

/**
 * @brief vmap route graph snap end。
 */
uint32_t vmap_route_graph_snap_end(const vmap_route_graph_t * g,
    double lon, double lat, uint32_t * out_nodes, uint32_t max_k,
    double * used_radius_m)
{
    static const double pool_tiers[] = {
        VMAP_ROUTE_SNAP_M,
        VMAP_ROUTE_SNAP_POOL_M,
        1200.0,
        2500.0,
        VMAP_ROUTE_SNAP_MAX_M,
    };
    uint32_t * raw = g_snap_scratch.raw;
    uint32_t raw_n;
    uint32_t n;

    if (!g || !out_nodes || max_k == 0) {
        return 0;
    }

    for (size_t ti = 0; ti < sizeof(pool_tiers) / sizeof(pool_tiers[0]); ti++) {
        raw_n = vmap_route_graph_snap_candidates(g, lon, lat, pool_tiers[ti],
            raw, VMAP_SNAP_SCRATCH_K);

        /* 诊断（用户报"终点吸附到 800m 外/路线切弯"）：把这一档**看到**的最近几个
         * 候选和**选中**的那个都打出来 —— 一眼能看出近点是没进池、还是被判成不可走。 */
        if (raw_n > 0) {
            double d0 = 0.0;
            double d1 = 0.0;
            double nlon;
            double nlat;

            vmap_route_graph_node_lonlat(g, raw[0], &nlon, &nlat);
            d0 = vmap_geo_haversine_m(lon, lat, nlon, nlat);
            if (raw_n > 1u) {
                vmap_route_graph_node_lonlat(g, raw[1], &nlon, &nlat);
                d1 = vmap_geo_haversine_m(lon, lat, nlon, nlat);
            }
            VMAP_NAV_OUT("snap: tier=%.0fm raw=%u near=%.0fm@n%u %.0fm@n%u",
                pool_tiers[ti], raw_n, d0, raw[0], d1,
                raw_n > 1u ? raw[1] : 0u);
        }

        n = snap_pick_routable(g, raw, raw_n, out_nodes, max_k);
        if (n > 0) {
            if (raw_n > 0 && out_nodes[0] != raw[0]) {
                double nlon;
                double nlat;

                vmap_route_graph_node_lonlat(g, out_nodes[0], &nlon, &nlat);
                VMAP_NAV_OUT("snap: tier=%.0fm nearest n%u rejected -> pick n%u "
                    "at %.0fm", pool_tiers[ti], raw[0], out_nodes[0],
                    vmap_geo_haversine_m(lon, lat, nlon, nlat));
            }
            if (used_radius_m) {
                *used_radius_m = pool_tiers[ti];
            }
            if (ti > 0 || n < raw_n) {
                VMAP_NAV_LOG("snap: pool %.0fm routable=%lu/%lu at %.6f,%.6f",
                    pool_tiers[ti], (unsigned long)n, (unsigned long)raw_n,
                    lon, lat);
            }
            return n;
        }
        if (raw_n > 0) {
            VMAP_NAV_LOG("snap: pool %.0fm all %lu isolated, expand",
                pool_tiers[ti], (unsigned long)raw_n);
        }
    }

    {
        uint32_t nearest = UINT32_MAX;
        double nearest_d = VMAP_ROUTE_SNAP_MAX_M + 1.0;

        for (uint32_t i = 0; i < g->node_count; i++) {
            double nlon;
            double nlat;
            double d;

            if (!vmap_route_graph_node_is_routable(g, i)) {
                continue;
            }
            vmap_route_graph_node_lonlat(g, i, &nlon, &nlat);
            d = vmap_geo_haversine_m(lon, lat, nlon, nlat);
            if (d < nearest_d) {
                nearest_d = d;
                nearest = i;
            }
        }

        if (nearest != UINT32_MAX && nearest_d <= VMAP_ROUTE_SNAP_MAX_M) {
            out_nodes[0] = nearest;
            if (used_radius_m) {
                *used_radius_m = nearest_d;
            }
            VMAP_NAV_WARN("snap: global routable %.0fm at %.6f,%.6f",
                nearest_d, lon, lat);
            return 1;
        }
    }

    {
        uint32_t nearest = UINT32_MAX;
        const double dist_m = vmap_route_graph_nearest_distance_m(g, lon, lat,
            &nearest);

        if (nearest != UINT32_MAX && dist_m >= 0.0
            && dist_m <= VMAP_ROUTE_SNAP_MAX_M) {
            out_nodes[0] = nearest;
            if (used_radius_m) {
                *used_radius_m = dist_m;
            }
            VMAP_NAV_WARN("snap: dead-end nearest %.0fm at %.6f,%.6f",
                dist_m, lon, lat);
            return 1;
        }

        if (used_radius_m) {
            *used_radius_m = dist_m;
        }
        VMAP_NAV_WARN("snap: off-map at %.6f,%.6f nearest=%.0fm",
            lon, lat, dist_m);
        return 0;
    }
}
