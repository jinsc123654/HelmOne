/**
 * @file vmap_route_portal.c
 * @brief vmap route_portal 模块。
 */

#include "vmap_route_portal.h"

#include "vmap_config.h"
#include "vmap_format.h"
#include "vmap_grid.h"
#include "vmap_route_log.h"
#include "vmap_alloc.h"
#include "vmap_vpk_heap.h"
#include <myvendor_mtp_lfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    vmap_cell_id_t a;
    vmap_cell_id_t b;
    uint32_t first;
    uint32_t num;
} portal_pair_t;

typedef struct {
    bool loaded;
    uint8_t version;
    char map_dir[64];
    char path[80];

    vmap_route_portal_rec_t * recs;
    uint32_t count;

    portal_pair_t * pairs;
    uint32_t pair_count;
    uint32_t rec_base;
    uint32_t pair_off;
    uint32_t pair_size;
    uint32_t rec_size;

    vmap_cell_id_t cache_a;
    vmap_cell_id_t cache_b;
    vmap_route_portal_rec_t * cache_recs;
    uint32_t cache_num;
} portal_db_t;

static portal_db_t g_portals;

static inline uint16_t rd_u16(const uint8_t * p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t * p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void decode_rec(const uint8_t * rec, vmap_route_portal_rec_t * out, uint8_t ver)
{
    if (ver >= VMAP_ROUTE_PORT_VERSION3) {
        out->region_a = rd_u32(rec + 0);
        out->region_b = rd_u32(rec + 4);
        out->node_a = rd_u32(rec + 8);
        out->node_b = rd_u32(rec + 12);
        memcpy(&out->lon, rec + 16, sizeof(double));
        memcpy(&out->lat, rec + 24, sizeof(double));
        return;
    }
    out->region_a = rd_u16(rec + 0);
    out->region_b = rd_u16(rec + 2);
    out->node_a = rd_u32(rec + 4);
    out->node_b = rd_u32(rec + 8);
    memcpy(&out->lon, rec + 12, sizeof(double));
    memcpy(&out->lat, rec + 20, sizeof(double));
}

/**
 * @brief vmap route portal unload。
 * @return 0 成功，负 errno 失败。
 */
void vmap_route_portal_unload(void)
{
    vmap_free(g_portals.recs);
    vmap_free(g_portals.pairs);
    vmap_free(g_portals.cache_recs);
    memset(&g_portals, 0, sizeof(g_portals));
}

/* v1: read the whole record array into RAM. fp positioned right after the
 * 10-byte header. */
static bool load_v1(FILE * fp, uint32_t n)
{
    vmap_route_portal_rec_t * recs;

    if (n == 0 || n > 1000000u) {
        return false;
    }
    recs = (vmap_route_portal_rec_t *)vmap_malloc(n * sizeof(*recs));
    if (!recs) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t rec[VMAP_ROUTE_PORT_REC_SIZE];

        if (fread(rec, 1, sizeof(rec), fp) != sizeof(rec)) {
            vmap_free(recs);
            return false;
        }
        decode_rec(rec, &recs[i], VMAP_ROUTE_PORT_VERSION);
    }
    g_portals.recs = recs;
    g_portals.count = n;
    g_portals.version = VMAP_ROUTE_PORT_VERSION;
    VMAP_NAV_LOG("portal: v1 loaded %u records", n);
    return true;
}

/* v2: read only the pair index into RAM; records stay on disk. fp positioned
 * right after the 10-byte common header (needs 4 more bytes for pair_count). */
static bool load_v2(FILE * fp, uint32_t rec_count)
{
    uint8_t pc[4];
    uint32_t pair_count;
    portal_pair_t * pairs;

    if (fread(pc, 1, sizeof(pc), fp) != sizeof(pc)) {
        return false;
    }
    pair_count = rd_u32(pc);
    if (pair_count == 0 || pair_count > 1000000u) {
        return false;
    }
    pairs = (portal_pair_t *)vmap_malloc(pair_count * sizeof(*pairs));
    if (!pairs) {
        return false;
    }
    for (uint32_t i = 0; i < pair_count; i++) {
        uint8_t e[VMAP_ROUTE_PORT_PAIR_SIZE];

        if (fread(e, 1, sizeof(e), fp) != sizeof(e)) {
            vmap_free(pairs);
            return false;
        }
        pairs[i].a = rd_u16(e + 0);
        pairs[i].b = rd_u16(e + 2);
        pairs[i].first = rd_u32(e + 4);
        pairs[i].num = rd_u32(e + 8);
    }
    g_portals.pairs = pairs;
    g_portals.pair_count = pair_count;
    g_portals.pair_off = VMAP_ROUTE_PORT_V2_HDR_SIZE;
    g_portals.pair_size = VMAP_ROUTE_PORT_PAIR_SIZE;
    g_portals.rec_size = VMAP_ROUTE_PORT_REC_SIZE;
    g_portals.rec_base = VMAP_ROUTE_PORT_V2_HDR_SIZE
        + pair_count * VMAP_ROUTE_PORT_PAIR_SIZE;
    g_portals.count = rec_count;
    g_portals.version = VMAP_ROUTE_PORT_VERSION2;
    g_portals.cache_a = VMAP_CELL_NONE;
    g_portals.cache_b = VMAP_CELL_NONE;
    VMAP_NAV_LOG("portal: v2 index %u pairs, %u records (on demand)",
        pair_count, rec_count);
    return true;
}

static bool load_v3(FILE * fp, uint32_t rec_count)
{
    uint8_t pc[4];
    uint32_t pair_count;

    if (fread(pc, 1, sizeof(pc), fp) != sizeof(pc)) {
        return false;
    }
    pair_count = rd_u32(pc);
    if (pair_count == 0 || pair_count > 1000000u) {
        return false;
    }
    g_portals.pairs = NULL;
    g_portals.pair_count = pair_count;
    g_portals.pair_off = VMAP_ROUTE_PORT_V3_HDR_SIZE;
    g_portals.pair_size = VMAP_ROUTE_PORT_PAIR_SIZE_V3;
    g_portals.rec_size = VMAP_ROUTE_PORT_REC_SIZE_V3;
    g_portals.rec_base = VMAP_ROUTE_PORT_V3_HDR_SIZE
        + pair_count * VMAP_ROUTE_PORT_PAIR_SIZE_V3;
    g_portals.count = rec_count;
    g_portals.version = VMAP_ROUTE_PORT_VERSION3;
    g_portals.cache_a = VMAP_CELL_NONE;
    g_portals.cache_b = VMAP_CELL_NONE;
    VMAP_NAV_LOG("portal: v3 index %u pairs, %u records (on disk)",
        pair_count, rec_count);
    (void)fp;
    return true;
}

/**
 * @brief vmap route portal load。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_portal_load(const char * map_dir)
{
    char path[80];
    FILE * fp;
    uint8_t hdr[VMAP_ROUTE_PORT_HDR_SIZE];
    uint32_t n;
    bool ok;

    if (!map_dir) {
        return false;
    }

    if (g_portals.loaded && strcmp(g_portals.map_dir, map_dir) == 0) {
        return true;
    }

    vmap_route_portal_unload();

    if (myvendor_mtp_lfs_quiesce()) {
        return false;
    }

    strncpy(g_portals.map_dir, map_dir, sizeof(g_portals.map_dir) - 1);
    g_portals.map_dir[sizeof(g_portals.map_dir) - 1] = '\0';
    g_portals.loaded = true;

    snprintf(path, sizeof(path), "%s/%s", map_dir, VMAP_ROUTE_PORT_FILE);
    fp = fopen(path, "rb");
    if (!fp) {
        VMAP_NAV_LOG("portal: no %s (per-cell VPOR)", VMAP_ROUTE_PORT_FILE);
        return true;
    }

    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)
            || !(hdr[0] == VMAP_ROUTE_PORT_MAGIC0
                && hdr[1] == VMAP_ROUTE_PORT_MAGIC1
                && hdr[2] == VMAP_ROUTE_PORT_MAGIC2
                && hdr[3] == VMAP_ROUTE_PORT_MAGIC3)) {
        fclose(fp);
        VMAP_NAV_LOG("portal: ignore %s, using per-cell VPOR", VMAP_ROUTE_PORT_FILE);
        return true;
    }

    n = rd_u32(hdr + 6);
    if (hdr[4] == VMAP_ROUTE_PORT_VERSION3) {
        ok = load_v3(fp, n);
    } else if (hdr[4] == VMAP_ROUTE_PORT_VERSION2) {
        ok = load_v2(fp, n);
    } else if (hdr[4] == VMAP_ROUTE_PORT_VERSION) {
        ok = load_v1(fp, n);
    } else {
        ok = false;
    }
    fclose(fp);
    if (!ok) {
        VMAP_NAV_LOG("portal: bad %s, using per-cell VPOR", VMAP_ROUTE_PORT_FILE);
        return true;
    }

    strncpy(g_portals.path, path, sizeof(g_portals.path) - 1);
    g_portals.path[sizeof(g_portals.path) - 1] = '\0';
    return true;
}

/**
 * @brief vmap route portal loaded。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_portal_loaded(void)
{
    return g_portals.loaded;
}

static bool read_pair_at(FILE * fp, uint32_t i, portal_pair_t * out)
{
    uint8_t e[16];
    uint32_t off;

    if (!fp || !out || i >= g_portals.pair_count) {
        return false;
    }
    off = g_portals.pair_off + i * g_portals.pair_size;
    if (fseek(fp, (long)off, SEEK_SET) != 0) {
        return false;
    }
    if (fread(e, 1, g_portals.pair_size, fp) != g_portals.pair_size) {
        return false;
    }
    if (g_portals.version >= VMAP_ROUTE_PORT_VERSION3) {
        out->a = rd_u32(e + 0);
        out->b = rd_u32(e + 4);
        out->first = rd_u32(e + 8);
        out->num = rd_u32(e + 12);
    } else {
        out->a = rd_u16(e + 0);
        out->b = rd_u16(e + 2);
        out->first = rd_u32(e + 4);
        out->num = rd_u32(e + 8);
    }
    return true;
}

static bool find_pair(vmap_cell_id_t lo, vmap_cell_id_t hi, portal_pair_t * out)
{
    uint32_t left = 0;
    uint32_t right = g_portals.pair_count;
    FILE * fp = NULL;
    bool ok = false;

    if (g_portals.pairs) {
        while (left < right) {
            uint32_t mid = left + (right - left) / 2u;
            const portal_pair_t * p = &g_portals.pairs[mid];

            if (p->a < lo || (p->a == lo && p->b < hi)) {
                left = mid + 1u;
            } else if (p->a > lo || (p->a == lo && p->b > hi)) {
                right = mid;
            } else {
                *out = *p;
                return true;
            }
        }
        return false;
    }

    fp = fopen(g_portals.path, "rb");
    if (!fp) {
        return false;
    }
    while (left < right) {
        uint32_t mid = left + (right - left) / 2u;
        portal_pair_t p;

        if (!read_pair_at(fp, mid, &p)) {
            break;
        }
        if (p.a < lo || (p.a == lo && p.b < hi)) {
            left = mid + 1u;
        } else if (p.a > lo || (p.a == lo && p.b > hi)) {
            right = mid;
        } else {
            *out = p;
            ok = true;
            break;
        }
    }
    fclose(fp);
    return ok;
}

static bool cache_pair(vmap_cell_id_t lo, vmap_cell_id_t hi)
{
    portal_pair_t p;
    FILE * fp;
    vmap_route_portal_rec_t * recs;
    uint32_t rec_sz = g_portals.rec_size ? g_portals.rec_size : VMAP_ROUTE_PORT_REC_SIZE;

    if (g_portals.cache_recs && g_portals.cache_a == lo && g_portals.cache_b == hi) {
        return true;
    }

    if (!find_pair(lo, hi, &p) || p.num == 0) {
        return false;
    }

    if (myvendor_mtp_lfs_quiesce()) {
        return false;
    }

    fp = fopen(g_portals.path, "rb");
    if (!fp) {
        return false;
    }
    if (fseek(fp, (long)(g_portals.rec_base + p.first * rec_sz), SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    recs = (vmap_route_portal_rec_t *)vmap_malloc(p.num * sizeof(*recs));
    if (!recs) {
        fclose(fp);
        return false;
    }
    for (uint32_t i = 0; i < p.num; i++) {
        uint8_t rec[32];

        if (fread(rec, 1, rec_sz, fp) != rec_sz) {
            vmap_free(recs);
            fclose(fp);
            return false;
        }
        decode_rec(rec, &recs[i], g_portals.version);
    }
    fclose(fp);

    vmap_free(g_portals.cache_recs);
    g_portals.cache_recs = recs;
    g_portals.cache_num = p.num;
    g_portals.cache_a = lo;
    g_portals.cache_b = hi;
    return true;
}

static uint32_t parse_vpor_ram(const uint8_t * ram, uint32_t ram_sz,
    vmap_cell_id_t file_rid, vmap_cell_id_t want_nbr,
    vmap_route_portal_rec_t * out, uint32_t out_max)
{
    const uint8_t * rhdr;
    const uint8_t * phdr;
    uint32_t graph_off;
    uint32_t graph_sz;
    uint32_t n_nbr;
    uint32_t i;
    uint32_t vpor_off;
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t rec_base;
    uint32_t found = 0;
    bool have = false;

    if (!ram || ram_sz < VMAP_VPK_HDR_SIZE + VMAP_REG_PACK_INDEX_HDR_SIZE) {
        return 0;
    }
    rhdr = ram + VMAP_VPK_HDR_SIZE;
    if (rhdr[0] != VMAP_REG_PACK_INDEX_MAGIC0
            || rhdr[1] != VMAP_REG_PACK_INDEX_MAGIC1
            || rhdr[2] != VMAP_REG_PACK_INDEX_MAGIC2
            || rhdr[3] != VMAP_REG_PACK_INDEX_MAGIC3) {
        return 0;
    }
    graph_off = rd_u32(rhdr + 12);
    graph_sz = rd_u32(rhdr + 16);
    if (graph_sz == 0) {
        return 0;
    }
    vpor_off = VMAP_VPK_HDR_SIZE + graph_off + graph_sz;
    if (vpor_off + VMAP_VPOR_HDR_SIZE > ram_sz) {
        return 0;
    }
    phdr = ram + vpor_off;
    if (phdr[0] != VMAP_VPOR_MAGIC0 || phdr[1] != VMAP_VPOR_MAGIC1
            || phdr[2] != VMAP_VPOR_MAGIC2 || phdr[3] != VMAP_VPOR_MAGIC3
            || phdr[4] != VMAP_VPOR_VERSION) {
        return 0;
    }
    n_nbr = rd_u16(phdr + 6);
    if (n_nbr == 0 || n_nbr > VMAP_VPOR_NBR_MAX
            || vpor_off + VMAP_VPOR_HDR_SIZE + n_nbr * VMAP_VPOR_NBR_SIZE > ram_sz) {
        return 0;
    }
    for (i = 0; i < n_nbr; i++) {
        const uint8_t * e = ram + vpor_off + VMAP_VPOR_HDR_SIZE
            + i * VMAP_VPOR_NBR_SIZE;

        if (rd_u32(e + 0) == want_nbr) {
            first = rd_u16(e + 4);
            count = rd_u16(e + 6);
            have = true;
            break;
        }
    }
    if (!have || count == 0) {
        return 0;
    }
    rec_base = vpor_off + VMAP_VPOR_HDR_SIZE + n_nbr * VMAP_VPOR_NBR_SIZE
        + first * VMAP_VPOR_REC_SIZE;
    if (count > out_max) {
        count = out_max;
    }
    if (rec_base + count * VMAP_VPOR_REC_SIZE > ram_sz) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const uint8_t * rec = ram + rec_base + i * VMAP_VPOR_REC_SIZE;

        out[found].region_a = file_rid;
        out[found].region_b = want_nbr;
        out[found].node_a = rd_u32(rec + 0);
        out[found].node_b = rd_u32(rec + 4);
        memcpy(&out[found].lon, rec + 8, sizeof(double));
        memcpy(&out[found].lat, rec + 16, sizeof(double));
        found++;
    }
    return found;
}

static uint32_t read_vpk_portals(const char * map_dir, vmap_cell_id_t file_rid,
    vmap_cell_id_t want_nbr, vmap_route_portal_rec_t * out, uint32_t out_max)
{
    char path[VMAP_GRID_PATH_MAX];
    const uint8_t * ram = NULL;
    uint32_t ram_sz = 0;
    uint32_t found;

    if (!map_dir || file_rid == VMAP_CELL_NONE || want_nbr == VMAP_CELL_NONE
            || !out || out_max == 0) {
        return 0;
    }

    vmap_grid_vpk_path(path, sizeof(path), map_dir, VMAP_CELL_IX(file_rid),
        VMAP_CELL_IY(file_rid));
    if (vmap_vpk_heap_pin(file_rid, path, &ram, &ram_sz)) {
        found = parse_vpor_ram(ram, ram_sz, file_rid, want_nbr, out, out_max);
        vmap_vpk_heap_unpin(file_rid);
        return found;
    }
    if (myvendor_mtp_lfs_quiesce()) {
        return 0;
    }

    {
        uint8_t rhdr[VMAP_REG_PACK_INDEX_HDR_SIZE];
        uint8_t phdr[VMAP_VPOR_HDR_SIZE];
        uint8_t e[VMAP_VPOR_NBR_SIZE];
        FILE * fp;
        uint32_t graph_off;
        uint32_t graph_sz;
        uint32_t n_nbr;
        uint32_t i;
        uint32_t vpor_off;
        uint32_t first = 0;
        uint32_t count = 0;
        uint32_t rec_base;
        bool have = false;

        fp = fopen(path, "rb");
        if (!fp) {
            return 0;
        }
        if (fseek(fp, (long)VMAP_VPK_HDR_SIZE, SEEK_SET) != 0
                || fread(rhdr, 1, sizeof(rhdr), fp) != sizeof(rhdr)
                || rhdr[0] != VMAP_REG_PACK_INDEX_MAGIC0
                || rhdr[1] != VMAP_REG_PACK_INDEX_MAGIC1
                || rhdr[2] != VMAP_REG_PACK_INDEX_MAGIC2
                || rhdr[3] != VMAP_REG_PACK_INDEX_MAGIC3) {
            fclose(fp);
            return 0;
        }
        graph_off = rd_u32(rhdr + 12);
        graph_sz = rd_u32(rhdr + 16);
        if (graph_sz == 0) {
            fclose(fp);
            return 0;
        }
        vpor_off = VMAP_VPK_HDR_SIZE + graph_off + graph_sz;
        if (fseek(fp, (long)vpor_off, SEEK_SET) != 0
                || fread(phdr, 1, sizeof(phdr), fp) != sizeof(phdr)
                || phdr[0] != VMAP_VPOR_MAGIC0 || phdr[1] != VMAP_VPOR_MAGIC1
                || phdr[2] != VMAP_VPOR_MAGIC2 || phdr[3] != VMAP_VPOR_MAGIC3
                || phdr[4] != VMAP_VPOR_VERSION) {
            fclose(fp);
            return 0;
        }
        n_nbr = rd_u16(phdr + 6);
        if (n_nbr == 0 || n_nbr > VMAP_VPOR_NBR_MAX) {
            fclose(fp);
            return 0;
        }
        for (i = 0; i < n_nbr; i++) {
            if (fread(e, 1, sizeof(e), fp) != sizeof(e)) {
                fclose(fp);
                return 0;
            }
            if (rd_u32(e + 0) == want_nbr) {
                first = rd_u16(e + 4);
                count = rd_u16(e + 6);
                have = true;
                break;
            }
        }
        if (!have || count == 0) {
            fclose(fp);
            return 0;
        }
        rec_base = vpor_off + VMAP_VPOR_HDR_SIZE + n_nbr * VMAP_VPOR_NBR_SIZE
            + first * VMAP_VPOR_REC_SIZE;
        if (fseek(fp, (long)rec_base, SEEK_SET) != 0) {
            fclose(fp);
            return 0;
        }
        if (count > out_max) {
            count = out_max;
        }
        found = 0;
        for (i = 0; i < count; i++) {
            uint8_t rec[VMAP_VPOR_REC_SIZE];

            if (fread(rec, 1, sizeof(rec), fp) != sizeof(rec)) {
                break;
            }
            out[found].region_a = file_rid;
            out[found].region_b = want_nbr;
            out[found].node_a = rd_u32(rec + 0);
            out[found].node_b = rd_u32(rec + 4);
            memcpy(&out[found].lon, rec + 8, sizeof(double));
            memcpy(&out[found].lat, rec + 16, sizeof(double));
            found++;
        }
        fclose(fp);
        return found;
    }
}

uint32_t vmap_route_portal_query(vmap_cell_id_t rid_a, vmap_cell_id_t rid_b,
    vmap_route_portal_rec_t * out, uint32_t out_max)
{
    uint32_t found = 0;
    uint32_t i;

    if (!out || out_max == 0 || !g_portals.loaded) {
        return 0;
    }

    found = read_vpk_portals(g_portals.map_dir, rid_a, rid_b, out, out_max);
    if (found == 0) {
        found = read_vpk_portals(g_portals.map_dir, rid_b, rid_a, out, out_max);
        for (i = 0; i < found; i++) {
            vmap_cell_id_t ra = out[i].region_a;
            uint32_t na = out[i].node_a;

            out[i].region_a = out[i].region_b;
            out[i].region_b = ra;
            out[i].node_a = out[i].node_b;
            out[i].node_b = na;
        }
    }
    if (found > 0) {
        return found;
    }

    if (g_portals.version >= VMAP_ROUTE_PORT_VERSION2) {
        vmap_cell_id_t lo = rid_a <= rid_b ? rid_a : rid_b;
        vmap_cell_id_t hi = rid_a <= rid_b ? rid_b : rid_a;

        if (!cache_pair(lo, hi)) {
            return 0;
        }
        for (i = 0; i < g_portals.cache_num && found < out_max; i++) {
            out[found++] = g_portals.cache_recs[i];
        }
        return found;
    }

    if (!g_portals.recs) {
        return 0;
    }
    for (i = 0; i < g_portals.count && found < out_max; i++) {
        const vmap_route_portal_rec_t * r = &g_portals.recs[i];

        if ((r->region_a == rid_a && r->region_b == rid_b)
            || (r->region_a == rid_b && r->region_b == rid_a)) {
            out[found++] = *r;
        }
    }
    return found;
}
