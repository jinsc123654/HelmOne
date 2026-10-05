/**
 * @file vmap_map_catalog.c
 * @brief Grid catalog: derive cell from coordinates; probe lonN/latN/x_y.vpk.
 */

#include "vmap_map_catalog.h"

#include "vmap_alloc.h"
#include "vmap_config.h"
#include "vmap_format.h"
#include "vmap_grid.h"
#include <stdio.h>
#include <string.h>

#define SYNTH_SLOTS 8
#define GRID_BFS_CAP 4096

typedef struct {
    bool regional;
    bool loaded;
    char map_dir[64];
    vmap_cell_id_t active_region;
} catalog_t;

static catalog_t g_cat;
static vmap_map_region_t g_synth[SYNTH_SLOTS];
static uint8_t g_synth_n;
static uint16_t g_find_ix;
static uint16_t g_find_iy;
static bool g_find_cached;
static bool g_find_exists;

static void find_cache_reset(void)
{
    g_find_cached = false;
    g_find_exists = false;
}

static void fill_region(vmap_map_region_t * r, uint16_t ix, uint16_t iy)
{
    r->id = VMAP_CELL_ID(ix, iy);
    r->grid_ix = ix;
    r->grid_iy = iy;
    r->neighbors = 0;
    vmap_grid_cell_bbox(ix, iy, &r->west, &r->south, &r->east, &r->north);
    if (g_cat.map_dir[0] != '\0') {
        if (ix > 0 && vmap_grid_vpk_exists(g_cat.map_dir, (uint16_t)(ix - 1), iy)) {
            r->neighbors |= VMAP_REG_NEIGHBOR_W;
        }
        if (ix < 0xffffu && vmap_grid_vpk_exists(g_cat.map_dir, (uint16_t)(ix + 1), iy)) {
            r->neighbors |= VMAP_REG_NEIGHBOR_E;
        }
        if (iy > 0 && vmap_grid_vpk_exists(g_cat.map_dir, ix, (uint16_t)(iy - 1))) {
            r->neighbors |= VMAP_REG_NEIGHBOR_S;
        }
        if (iy < 0xffffu && vmap_grid_vpk_exists(g_cat.map_dir, ix, (uint16_t)(iy + 1))) {
            r->neighbors |= VMAP_REG_NEIGHBOR_N;
        }
    }
}

static bool map_idx_is_vidx(const char * map_dir)
{
    char path[72];
    FILE * fp;
    uint8_t mag[4];
    bool vidx = false;

    if (!map_dir) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s", map_dir, VMAP_MAP_INDEX_FILE);
    fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    if (fread(mag, 1, 4, fp) == 4
        && mag[0] == VMAP_MAP_INDEX_MAGIC0 && mag[1] == VMAP_MAP_INDEX_MAGIC1
        && mag[2] == VMAP_MAP_INDEX_MAGIC2 && mag[3] == VMAP_MAP_INDEX_MAGIC3) {
        vidx = true;
    }
    fclose(fp);
    return vidx;
}

void vmap_map_catalog_unload(void)
{
    memset(&g_cat, 0, sizeof(g_cat));
    g_cat.active_region = VMAP_CELL_NONE;
    g_synth_n = 0;
    find_cache_reset();
}

bool vmap_map_catalog_load(const char * map_dir)
{
    if (!map_dir) {
        return false;
    }

    if (g_cat.loaded && g_cat.regional && strcmp(g_cat.map_dir, map_dir) == 0) {
        return true;
    }

    vmap_map_catalog_unload();

    /* Legacy size-sharded VIDX packs still use map.idx; grid mode otherwise. */
    if (map_idx_is_vidx(map_dir)) {
        return false;
    }

    strncpy(g_cat.map_dir, map_dir, sizeof(g_cat.map_dir) - 1);
    g_cat.map_dir[sizeof(g_cat.map_dir) - 1] = '\0';
    g_cat.regional = true;
    g_cat.loaded = true;
    g_cat.active_region = VMAP_CELL_NONE;
    return true;
}

bool vmap_map_catalog_is_regional(void)
{
    return g_cat.loaded && g_cat.regional;
}

bool vmap_map_catalog_is_loaded(void)
{
    return g_cat.loaded;
}

uint16_t vmap_map_catalog_region_count(void)
{
    return 0;
}

vmap_cell_id_t vmap_map_catalog_active_region(void)
{
    return g_cat.active_region;
}

bool vmap_map_catalog_find_region(double lon, double lat, vmap_cell_id_t * out_id)
{
    uint16_t ix;
    uint16_t iy;

    if (!g_cat.regional) {
        return false;
    }
    if (!vmap_grid_lonlat_to_cell(lon, lat, &ix, &iy)) {
        return false;
    }
    if (!g_find_cached || ix != g_find_ix || iy != g_find_iy) {
        g_find_ix = ix;
        g_find_iy = iy;
        g_find_cached = true;
        g_find_exists = (g_cat.map_dir[0] == '\0')
            || vmap_grid_vpk_exists(g_cat.map_dir, ix, iy);
    }
    if (!g_find_exists) {
        return false;
    }
    if (out_id) {
        *out_id = VMAP_CELL_ID(ix, iy);
    }
    return true;
}

const vmap_map_region_t * vmap_map_catalog_region(vmap_cell_id_t region_id)
{
    uint8_t i;
    vmap_map_region_t * slot;

    if (!g_cat.regional || region_id == VMAP_CELL_NONE) {
        return NULL;
    }
    for (i = 0; i < g_synth_n && i < SYNTH_SLOTS; i++) {
        if (g_synth[i].id == region_id) {
            return &g_synth[i];
        }
    }
    if (g_synth_n < SYNTH_SLOTS) {
        slot = &g_synth[g_synth_n++];
    } else {
        static uint8_t clock;
        slot = &g_synth[clock % SYNTH_SLOTS];
        clock++;
    }
    fill_region(slot, VMAP_CELL_IX(region_id), VMAP_CELL_IY(region_id));
    return slot;
}

bool vmap_map_catalog_ensure_region(const char * map_dir, double lon, double lat,
    vmap_cell_id_t * out_region_id)
{
    vmap_cell_id_t rid;

    if (map_dir && !vmap_map_catalog_load(map_dir)) {
        return false;
    }
    if (!vmap_map_catalog_find_region(lon, lat, &rid)) {
        return false;
    }
    if (out_region_id) {
        *out_region_id = rid;
    }
    g_cat.active_region = rid;
    return true;
}

void vmap_map_catalog_set_active_region(vmap_cell_id_t region_id)
{
    g_cat.active_region = region_id;
}

static int bfs_find(const vmap_cell_id_t * q, uint32_t n, vmap_cell_id_t id)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (q[i] == id) {
            return (int)i;
        }
    }
    return -1;
}

bool vmap_map_catalog_region_path(vmap_cell_id_t start_id, vmap_cell_id_t dest_id,
    vmap_cell_id_t * out_ids, uint32_t out_cap, uint32_t * out_len)
{
    vmap_cell_id_t * queue = NULL;
    vmap_cell_id_t * parent = NULL;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t rev_len;
    vmap_cell_id_t rev[VMAP_ROUTE_REGION_PATH_MAX];
    uint32_t i;
    bool ok = false;
    static const int dx[4] = { -1, 1, 0, 0 };
    static const int dy[4] = { 0, 0, -1, 1 };

    if (!out_ids || !out_len || out_cap == 0 || !g_cat.regional) {
        return false;
    }

    if (start_id == dest_id) {
        out_ids[0] = start_id;
        *out_len = 1;
        return true;
    }

    queue = (vmap_cell_id_t *)vmap_malloc(sizeof(vmap_cell_id_t) * GRID_BFS_CAP);
    parent = (vmap_cell_id_t *)vmap_malloc(sizeof(vmap_cell_id_t) * GRID_BFS_CAP);
    if (!queue || !parent) {
        vmap_free(queue);
        vmap_free(parent);
        return false;
    }

    queue[tail] = start_id;
    parent[tail] = VMAP_CELL_NONE;
    tail++;

    while (head < tail) {
        vmap_cell_id_t cur = queue[head];
        uint16_t ix = VMAP_CELL_IX(cur);
        uint16_t iy = VMAP_CELL_IY(cur);

        if (cur == dest_id) {
            break;
        }
        head++;

        for (i = 0; i < 4u; i++) {
            int nix = (int)ix + dx[i];
            int niy = (int)iy + dy[i];
            vmap_cell_id_t nid;
            uint16_t uix;
            uint16_t uiy;

            if (nix < 0 || niy < 0 || nix > 65535 || niy > 65535) {
                continue;
            }
            uix = (uint16_t)nix;
            uiy = (uint16_t)niy;
            if (!vmap_grid_vpk_exists(g_cat.map_dir, uix, uiy)) {
                continue;
            }
            nid = VMAP_CELL_ID(uix, uiy);
            if (bfs_find(queue, tail, nid) >= 0) {
                continue;
            }
            if (tail >= GRID_BFS_CAP) {
                goto done;
            }
            queue[tail] = nid;
            parent[tail] = cur;
            tail++;
        }
    }

    if (bfs_find(queue, tail, dest_id) < 0) {
        goto done;
    }

    rev_len = 0;
    {
        vmap_cell_id_t c = dest_id;

        while (c != start_id) {
            int pi;

            if (rev_len >= VMAP_ROUTE_REGION_PATH_MAX) {
                goto done;
            }
            rev[rev_len++] = c;
            pi = bfs_find(queue, tail, c);
            if (pi < 0 || parent[pi] == VMAP_CELL_NONE) {
                goto done;
            }
            c = parent[pi];
        }
    }

    if (rev_len + 1u > out_cap || rev_len + 1u > VMAP_ROUTE_REGION_PATH_MAX) {
        goto done;
    }

    out_ids[0] = start_id;
    for (i = 0; i < rev_len; i++) {
        out_ids[i + 1u] = rev[rev_len - 1u - i];
    }
    *out_len = rev_len + 1u;
    ok = true;

done:
    vmap_free(queue);
    vmap_free(parent);
    return ok;
}
