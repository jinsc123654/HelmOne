/**
 * @file vmap_tile_cache.c
 * @brief vmap tile_cache 模块。
 */

#include "vmap_tile_cache.h"

#include "vmap_map_catalog.h"
#include "vmap_grid.h"
#include "vmap_tile_index.h"
#include "vmap_alloc.h"
#include "vmap_cache_arena.h"
#include "vmap_config.h"
#include "vmap_format.h"
#include "vmap_vpk_heap.h"
#include "vmap_style.h"   /* 风格闸门：切主题要让"已烘焙的瓦片位图"过期 */
#include "lvgl/lvgl.h"
#include <myvendor_mtp_lfs.h>
#include "myvendor_watchdog.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Number of region packs kept open simultaneously. A single on-screen view can
 * straddle several grid cells (especially when zoomed out or when the current
 * position sits near a cell boundary), so tiles must be servable from more than
 * just the active region. */
#define VMAP_REG_SLOTS 4

struct regional_entry {
    uint8_t z;
    uint16_t x;
    uint16_t y;
    uint32_t offset;
    uint32_t size;
};

/* One resident region pack: its tile table plus an open file handle. */
struct reg_slot {
    bool used;
    vmap_cell_id_t region_id;
    struct regional_entry * entries;
    uint16_t entry_count;
    uint32_t graph_off;
    uint32_t graph_sz;
    FILE * fp;
    const uint8_t * ram;
    uint32_t ram_sz;
    uint32_t lru;
};

struct vmap_tile_cache {
    char tile_dir[64];
    int zoom;
    bool packed;
    uint16_t city_id;
    vmap_tile_index_t * index;
    FILE * pack_fp;
    int pack_zoom;
    uint16_t pack_id;
    char pack_path[64];
    uint8_t * staging;
    size_t staging_cap;
    bool has_reg_pack;
    vmap_cell_id_t region_id;
    uint32_t region_graph_off;
    uint32_t region_graph_sz;
    struct reg_slot slots[VMAP_REG_SLOTS];
    uint32_t lru_clock;
    /** 位图是按哪份 vmap 风格烘焙的（见 vmap_tile_cache_get 的风格闸门）。 */
    uint8_t style_id;
};

static inline uint16_t rd_u16(const uint8_t * p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t * p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void slot_clear(struct reg_slot * s)
{
    if (!s) {
        return;
    }
    if (s->ram) {
        vmap_vpk_heap_unpin(s->region_id);
        s->ram = NULL;
    }
    s->ram_sz = 0;
    if (s->fp) {
        fclose(s->fp);
        s->fp = NULL;
    }
    if (s->entries) {
        vmap_free(s->entries);
        s->entries = NULL;
    }
    s->entry_count = 0;
    s->graph_off = 0;
    s->graph_sz = 0;
    s->region_id = VMAP_CELL_NONE;
    s->used = false;
    s->lru = 0;
}

static void regional_clear(vmap_tile_cache_t * cache)
{
    int i;

    if (!cache) {
        return;
    }
    for (i = 0; i < VMAP_REG_SLOTS; i++) {
        slot_clear(&cache->slots[i]);
    }
    cache->region_graph_off = 0;
    cache->region_graph_sz = 0;
    cache->region_id = VMAP_CELL_NONE;
    cache->has_reg_pack = false;
}

/* Close file handles for all resident region packs but keep their tile tables
 * so they can be reopened cheaply once flash access resumes (MTP quiesce). */
static void slots_close_fps(vmap_tile_cache_t * cache)
{
    int i;

    if (!cache) {
        return;
    }
    for (i = 0; i < VMAP_REG_SLOTS; i++) {
        if (cache->slots[i].fp) {
            fclose(cache->slots[i].fp);
            cache->slots[i].fp = NULL;
        }
    }
}

/* Web-Mercator tile center back to WGS84 lon/lat, matching the packer's
 * per-tile cell assignment so a tile always resolves to the region that
 * actually contains it. */
static void tile_center_lonlat(int z, int x, int y, double * lon, double * lat)
{
    double n = (double)(1u << (unsigned)z);
    double t = M_PI * (1.0 - 2.0 * ((double)y + 0.5) / n);

    *lon = ((double)x + 0.5) / n * 360.0 - 180.0;
    *lat = 180.0 / M_PI * atan(sinh(t));
}

static uint16_t cell_arena_key(vmap_cell_id_t id)
{
    if (id == VMAP_CELL_NONE) {
        return 0xffffu;
    }
    return (uint16_t)(id ^ (id >> 16));
}

/* Arena key: owning grid cell for this tile (tile-center rule, pack_map). */
static vmap_cell_id_t tile_cache_region(vmap_tile_cache_t * cache, int z, int x, int y)
{
    double lon;
    double lat;
    vmap_cell_id_t rid;

    if (!cache) {
        return VMAP_CELL_NONE;
    }
    if (!vmap_map_catalog_is_regional()) {
        return cache->city_id;
    }
    tile_center_lonlat(z, x, y, &lon, &lat);
    if (!vmap_map_catalog_find_region(lon, lat, &rid)) {
        return cache->region_id;
    }
    return rid;
}

static bool tile_blob_magic_ok(const uint8_t * data, uint32_t size)
{
    if (!data || size < 5) {
        return false;
    }
    if (!(data[0] == VMAP_MAGIC0 && data[1] == VMAP_MAGIC1
            && data[2] == VMAP_MAGIC2 && data[3] == VMAP_MAGIC3)) {
        return false;
    }
    return data[4] == 1 || data[4] == VMAP_VERSION;
}

static bool staging_ensure(vmap_tile_cache_t * cache, uint32_t need)
{
    uint8_t * p;
    size_t cap;

    if (!cache || need == 0 || need > VMAP_TILE_BYTES_MAX) {
        return false;
    }
    if (cache->staging && need <= cache->staging_cap) {
        return true;
    }
    cap = (size_t)need;
    if (cap < (size_t)VMAP_STAGING_CAP) {
        cap = (size_t)VMAP_STAGING_CAP;
    }
    p = (uint8_t *)vmap_malloc(cap);
    if (!p) {
        return false;
    }
    if (cache->staging) {
        vmap_free(cache->staging);
    }
    cache->staging = p;
    cache->staging_cap = cap;
    return true;
}

static void close_pack(vmap_tile_cache_t * cache)
{
    if (!cache) {
        return;
    }
    if (cache->pack_fp) {
        fclose(cache->pack_fp);
        cache->pack_fp = NULL;
    }
    cache->pack_zoom = -1;
    cache->pack_id = 0xffff;
    cache->pack_path[0] = '\0';
}

static bool open_pack_path(vmap_tile_cache_t * cache, const char * path,
    uint16_t pack_id)
{
    if (cache->pack_fp && cache->pack_id == pack_id
        && strcmp(cache->pack_path, path) == 0) {
        return true;
    }

    close_pack(cache);
    lv_strlcpy(cache->pack_path, path, sizeof(cache->pack_path));
    cache->pack_fp = fopen(path, "rb");
    if (!cache->pack_fp) {
        return false;
    }
    cache->pack_zoom = 0;
    cache->pack_id = pack_id;
    return true;
}

static bool open_pack(vmap_tile_cache_t * cache, uint16_t pack_id)
{
    char path[72];

    lv_snprintf(path, sizeof(path), "%s/p%03d.%s",
        cache->tile_dir, (int)pack_id, VMAP_SHARD_EXT);
    return open_pack_path(cache, path, pack_id);
}

static bool regional_find_in(const struct reg_slot * slot, int z, int x, int y,
    const struct regional_entry ** out)
{
    int lo;
    int hi;

    if (!slot || !slot->entries || slot->entry_count == 0) {
        return false;
    }

    lo = 0;
    hi = (int)slot->entry_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const struct regional_entry * e = &slot->entries[mid];

        if (e->z < (uint8_t)z
            || (e->z == (uint8_t)z && e->y < (uint16_t)y)
            || (e->z == (uint8_t)z && e->y == (uint16_t)y
                && e->x < (uint16_t)x)) {
            lo = mid + 1;
        } else if (e->z > (uint8_t)z
            || (e->z == (uint8_t)z && e->y > (uint16_t)y)
            || (e->z == (uint8_t)z && e->y == (uint16_t)y
                && e->x > (uint16_t)x)) {
            hi = mid - 1;
        } else {
            if (out) {
                *out = e;
            }
            return true;
        }
    }
    return false;
}

static void resolve_region_vpk(char * buf, size_t sz,
    const char * dir, vmap_cell_id_t region_id)
{
    vmap_grid_vpk_path(buf, sz, dir, VMAP_CELL_IX(region_id),
        VMAP_CELL_IY(region_id));
}

static bool fill_entries(struct reg_slot * victim, const uint8_t * hdr,
    const uint8_t * recs, uint32_t recs_bytes)
{
    uint32_t tile_count;
    uint32_t rec_sz;
    uint32_t k;

    if (!(hdr[0] == VMAP_REG_PACK_INDEX_MAGIC0
            && hdr[1] == VMAP_REG_PACK_INDEX_MAGIC1
            && hdr[2] == VMAP_REG_PACK_INDEX_MAGIC2
            && hdr[3] == VMAP_REG_PACK_INDEX_MAGIC3)) {
        return false;
    }
    if (hdr[4] != VMAP_REG_PACK_INDEX_VERSION
            && hdr[4] != VMAP_REG_PACK_INDEX_VERSION2) {
        return false;
    }
    tile_count = rd_u32(hdr + 8);
    if (tile_count == 0 || tile_count > 4096u) {
        return false;
    }
    rec_sz = (hdr[4] == VMAP_REG_PACK_INDEX_VERSION2)
        ? (uint32_t)VMAP_REG_PACK_TILE_REC_SIZE_V2
        : (uint32_t)VMAP_REG_PACK_TILE_REC_SIZE;
    if (recs_bytes < tile_count * rec_sz) {
        return false;
    }
    victim->entries = (struct regional_entry *)vmap_malloc(
        (size_t)tile_count * sizeof(*victim->entries));
    if (!victim->entries) {
        return false;
    }
    for (k = 0; k < tile_count; k++) {
        const uint8_t * rec = recs + k * rec_sz;
        struct regional_entry * e = &victim->entries[k];

        e->z = rec[0];
        e->x = rd_u16(rec + 2);
        e->y = rd_u16(rec + 4);
        e->offset = rd_u32(rec + 8);
        e->size = (hdr[4] == VMAP_REG_PACK_INDEX_VERSION2)
            ? rd_u32(rec + 12) : rd_u16(rec + 12);
        if (e->size > VMAP_TILE_BYTES_MAX) {
            vmap_free(victim->entries);
            victim->entries = NULL;
            return false;
        }
    }
    victim->entry_count = (uint16_t)tile_count;
    victim->graph_off = rd_u32(hdr + 12);
    victim->graph_sz = rd_u32(hdr + 16);
    return true;
}

static bool slot_bind_ram(struct reg_slot * victim, vmap_cell_id_t region_id,
    const uint8_t * ram, uint32_t ram_sz)
{
    if (!ram || ram_sz < VMAP_VPK_HDR_SIZE + VMAP_REG_PACK_INDEX_HDR_SIZE) {
        return false;
    }
    if (!(ram[0] == VMAP_PACK_MAGIC0 && ram[1] == VMAP_PACK_MAGIC1
            && ram[2] == VMAP_PACK_MAGIC2 && ram[3] == VMAP_PACK_MAGIC3)) {
        return false;
    }
    if (!fill_entries(victim, ram + VMAP_VPK_HDR_SIZE,
            ram + VMAP_VPK_HDR_SIZE + VMAP_REG_PACK_INDEX_HDR_SIZE,
            ram_sz - VMAP_VPK_HDR_SIZE - VMAP_REG_PACK_INDEX_HDR_SIZE)) {
        return false;
    }
    victim->ram = NULL;
    victim->ram_sz = 0;
    victim->fp = NULL;
    victim->region_id = region_id;
    victim->used = true;
    return true;
}

/* Make a region pack resident in a slot (reusing it if already loaded, else
 * evicting the least-recently-used non-active slot). Returns the slot. */
static struct reg_slot * slot_load(vmap_tile_cache_t * cache, vmap_cell_id_t region_id)
{
    char path[VMAP_GRID_PATH_MAX];
    struct reg_slot * victim = NULL;
    const uint8_t * ram = NULL;
    uint32_t ram_sz = 0;
    int i;

    if (!cache || cache->tile_dir[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < VMAP_REG_SLOTS; i++) {
        struct reg_slot * s = &cache->slots[i];

        if (!s->used || s->region_id != region_id) {
            continue;
        }
        s->lru = ++cache->lru_clock;
        vmap_vpk_heap_touch(region_id);
        return s;
    }

    for (i = 0; i < VMAP_REG_SLOTS; i++) {
        struct reg_slot * s = &cache->slots[i];

        if (!s->used) {
            victim = s;
            break;
        }
        if (s->region_id == cache->region_id) {
            continue;
        }
        if (!victim || s->lru < victim->lru) {
            victim = s;
        }
    }
    if (!victim) {
        victim = &cache->slots[0];
    }
    slot_clear(victim);

    resolve_region_vpk(path, sizeof(path), cache->tile_dir, region_id);
    if (!vmap_vpk_heap_pin(region_id, path, &ram, &ram_sz)) {
        static vmap_cell_id_t last_fail = VMAP_CELL_NONE;

        if (last_fail != region_id) {
            last_fail = region_id;
            syslog(LOG_WARNING, "[vmap] vpk pin fail cell=%u",
                (unsigned)region_id);
        }
        return NULL;
    }
    if (!slot_bind_ram(victim, region_id, ram, ram_sz)) {
        vmap_vpk_heap_unpin(region_id);
        slot_clear(victim);
        return NULL;
    }
    /* 解析完索引就 unpin：文件仍留在 heap，refs=0 才能给邻格腾地方。 */
    vmap_vpk_heap_unpin(region_id);
    victim->lru = ++cache->lru_clock;
    return victim;
}

static bool load_regional_pack(vmap_tile_cache_t * cache, vmap_cell_id_t region_id)
{
    struct reg_slot * s;
    uint16_t arena_key;

    if (!cache || cache->tile_dir[0] == '\0' || region_id == VMAP_CELL_NONE) {
        return false;
    }

    s = slot_load(cache, region_id);
    if (!s) {
        return false;
    }

    cache->region_id = region_id;
    cache->region_graph_off = s->graph_off;
    cache->region_graph_sz = s->graph_sz;
    cache->has_reg_pack = true;
    cache->packed = true;

    arena_key = cell_arena_key(region_id);
    if (cache->city_id != arena_key) {
        cache->city_id = arena_key;
        vmap_cache_set_city_id(arena_key);
    }
    vmap_map_catalog_set_active_region(region_id);
    return true;
}

/**
 * @brief vmap tile cache create。
 * @return 0 成功，负 errno 失败。
 */
vmap_tile_cache_t * vmap_tile_cache_create(void)
{
    vmap_tile_cache_t * cache = (vmap_tile_cache_t *)calloc(1, sizeof(*cache));
    if (!cache) {
        return NULL;
    }

    cache->style_id = (uint8_t)vmap_style_get_id();
    cache->zoom = -1;
    cache->pack_zoom = -1;
    cache->pack_id = 0xffff;
    cache->region_id = VMAP_CELL_NONE;
    cache->staging_cap = (size_t)VMAP_STAGING_CAP;
    vmap_cache_arena_init();
    vmap_vpk_heap_init();
    cache->staging = (uint8_t *)vmap_malloc(cache->staging_cap);
    cache->index = vmap_tile_index_create();
    if (!cache->staging || !cache->index) {
        vmap_tile_cache_destroy(cache);
        return NULL;
    }
    return cache;
}

/**
 * @brief vmap tile cache destroy。
 */
void vmap_tile_cache_destroy(vmap_tile_cache_t * cache)
{
    if (!cache) {
        return;
    }
    close_pack(cache);
    regional_clear(cache);
    if (cache->staging) {
        vmap_free(cache->staging);
    }
    vmap_tile_index_destroy(cache->index);
    free(cache);
}

/**
 * @brief vmap tile cache set dir。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_tile_cache_set_dir(vmap_tile_cache_t * cache, const char * dir)
{
    if (!cache) {
        return;
    }
    if (dir) {
        lv_strlcpy(cache->tile_dir, dir, sizeof(cache->tile_dir));
    } else {
        cache->tile_dir[0] = '\0';
    }
    close_pack(cache);
    regional_clear(cache);
    vmap_vpk_heap_clear();
    vmap_map_catalog_unload();
    vmap_cache_arena_reset();
    cache->zoom = -1;
    cache->packed = false;
    vmap_tile_index_unload(cache->index);
    if (dir) {
        if (vmap_map_catalog_load(dir) && vmap_map_catalog_is_regional()) {
            cache->packed = true;
            LV_LOG_INFO("vmap: grid cells → lonN/latN/x_y.vpk dir=%s", dir);
        }
    }
}

/**
 * @brief vmap tile cache set city。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_tile_cache_set_city(vmap_tile_cache_t * cache, uint16_t city_id)
{
    if (!cache || cache->city_id == city_id) {
        return;
    }
    cache->city_id = city_id;
    vmap_cache_set_city_id(city_id);
}

/**
 * @brief vmap tile cache set region。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_tile_cache_set_region(vmap_tile_cache_t * cache, vmap_cell_id_t region_id)
{
    if (!cache) {
        return false;
    }
    if (vmap_map_catalog_is_regional()) {
        return load_regional_pack(cache, region_id);
    }
    vmap_tile_cache_set_city(cache, (uint16_t)region_id);
    return true;
}

bool vmap_tile_cache_ensure_region(vmap_tile_cache_t * cache, double lon,
    double lat)
{
    vmap_cell_id_t rid;

    if (!cache || !vmap_map_catalog_is_regional()) {
        return true;
    }
    if (!vmap_map_catalog_find_region(lon, lat, &rid)) {
        return false;
    }
    return load_regional_pack(cache, rid);
}

vmap_cell_id_t vmap_tile_cache_active_region(const vmap_tile_cache_t * cache)
{
    if (!cache || !cache->has_reg_pack) {
        return VMAP_CELL_NONE;
    }
    return cache->region_id;
}

/**
 * @brief vmap tile cache region graph。
 */
bool vmap_tile_cache_region_graph(const vmap_tile_cache_t * cache,
    uint32_t * graph_off, uint32_t * graph_sz, char * vpk_path, size_t path_sz)
{
    if (!cache || !cache->has_reg_pack || cache->region_graph_sz == 0) {
        return false;
    }
    if (graph_off) {
        *graph_off = cache->region_graph_off;
    }
    if (graph_sz) {
        *graph_sz = cache->region_graph_sz;
    }
    if (vpk_path && path_sz > 0) {
        resolve_region_vpk(vpk_path, path_sz, cache->tile_dir, cache->region_id);
    }
    return true;
}

/**
 * @brief vmap tile cache set zoom。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_tile_cache_set_zoom(vmap_tile_cache_t * cache, int zoom)
{
    if (!cache || zoom == cache->zoom || cache->tile_dir[0] == '\0') {
        return;
    }
    if (myvendor_mtp_lfs_quiesce()) {
        return;
    }

    cache->zoom = zoom;

    if (vmap_map_catalog_is_regional()) {
        cache->packed = true;
        return;
    }

    close_pack(cache);
    if (!vmap_tile_index_is_loaded(cache->index)) {
        cache->packed = vmap_tile_index_load(cache->index, cache->tile_dir, zoom);
    } else {
        cache->packed = true;
    }
}

/* 1=读到瓦片，0=包里没有，-1=瞬时 I/O（SD/句柄），下次再试，勿记 MISSING。 */
static int load_from_flash(vmap_tile_cache_t * cache, int z, int x, int y,
    uint32_t * out_size)
{
    const struct regional_entry * re = NULL;
    vmap_tile_ref_t ref;

    if (!cache || myvendor_mtp_lfs_quiesce()) {
        return -1;
    }

    /* Grid: lonN/latN/x_y.vpk. Look up the cell that owns this tile's
     * center (same rule as pack_map.py). Do not fall through to VIDX / .vt. */
    if (vmap_map_catalog_is_regional()) {
        struct reg_slot * slot = NULL;
        double lon;
        double lat;
        vmap_cell_id_t rid;

        if (cache->has_reg_pack) {
            slot = slot_load(cache, cache->region_id);
            if (slot && !regional_find_in(slot, z, x, y, &re)) {
                slot = NULL;
            }
        }
        if (!slot) {
            tile_center_lonlat(z, x, y, &lon, &lat);
            if (!vmap_map_catalog_find_region(lon, lat, &rid)) {
                return 0;
            }
            slot = slot_load(cache, rid);
            if (!slot) {
                return -1;
            }
            if (!regional_find_in(slot, z, x, y, &re)) {
                return 0;
            }
        }
        if (!re || !slot || !staging_ensure(cache, re->size)) {
            return -1;
        }
        {
            char path[VMAP_GRID_PATH_MAX];

            resolve_region_vpk(path, sizeof(path), cache->tile_dir,
                slot->region_id);
            if (!vmap_vpk_heap_copy(slot->region_id, path,
                    VMAP_VPK_HDR_SIZE + re->offset, cache->staging, re->size)) {
                return -1;
            }
        }
        if (out_size) {
            *out_size = re->size;
        }
        return 1;
    }

    if (cache->packed && vmap_tile_index_is_loaded(cache->index)) {
        if (!vmap_tile_index_find(cache->index, z, x, y, &ref)) {
            return 0;
        }
        if (!staging_ensure(cache, ref.size) || !open_pack(cache, ref.pack_id)) {
            return -1;
        }
        if (fseek(cache->pack_fp, (long)(VMAP_VPK_HDR_SIZE + ref.offset),
                SEEK_SET) != 0) {
            return -1;
        }
        if (fread(cache->staging, 1, ref.size, cache->pack_fp) != (size_t)ref.size) {
            return -1;
        }
        if (out_size) {
            *out_size = ref.size;
        }
        return 1;
    }

    char path[64];
    FILE * fp;
    long sz;

    lv_snprintf(path, sizeof(path), "%s/%d/%d/%d.vt", cache->tile_dir, z, x, y);
    fp = fopen(path, "rb");
    if (!fp) {
        return (errno == ENOENT) ? 0 : -1;
    }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 20 || (unsigned long)sz > (unsigned long)VMAP_TILE_BYTES_MAX) {
        fclose(fp);
        return 0;
    }
    if (!staging_ensure(cache, (uint32_t)sz)) {
        fclose(fp);
        return -1;
    }
    if (fread(cache->staging, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (out_size) {
        *out_size = (uint32_t)sz;
    }
    return 1;
}

/**
 * @brief vmap tile cache release lfs。
 */
void vmap_tile_cache_release_lfs(vmap_tile_cache_t * cache)
{
    if (!cache) {
        return;
    }
    close_pack(cache);
    slots_close_fps(cache);
}

static uint16_t tile_cache_rid(vmap_tile_cache_t * cache, int z, int x, int y)
{
    vmap_cell_id_t cell = tile_cache_region(cache, z, x, y);

    return vmap_map_catalog_is_regional() ? cell_arena_key(cell) : (uint16_t)cell;
}

bool vmap_tile_cache_cached(vmap_tile_cache_t * cache, int z, int x, int y)
{
    vmap_cache_blob_t blob;

    if (!cache || cache->tile_dir[0] == '\0') {
        return false;
    }

    if (z != cache->zoom) {
        vmap_tile_cache_set_zoom(cache, z);
    }

    return vmap_cache_lookup(tile_cache_rid(cache, z, x, y), z, x, y, &blob) != 0;
}

bool vmap_tile_cache_warm(vmap_tile_cache_t * cache, int z, int x, int y)
{
    vmap_tile_t tile;

    return vmap_tile_cache_get(cache, z, x, y, &tile);
}

/**
 * @brief vmap tile cache get。
 * @return 请求的值。
 */
bool vmap_tile_cache_get(vmap_tile_cache_t * cache, int z, int x, int y,
    vmap_tile_t * out_tile)
{
    vmap_cache_blob_t blob;
    vmap_cell_id_t cell;
    uint16_t rid;

    if (!cache || !out_tile || cache->tile_dir[0] == '\0' || !cache->staging) {
        return false;
    }

    /*
     * 风格闸门（2026-09-27）：缓存键里**没有**风格，位图是按烘焙时的风格存的 ⇒ 切主题
     * （vmap 风格）后必须整批作废，否则只有"恰好被重绘过"的瓦片换了色 —— 实测症状就是
     * 夜间主题下白色轨迹线压在浅色地图上（用户报的"REC 线没颜色 / 里程碑白圈白字"）。
     * 丢位图的配方与 load 路径一致：清槽 + 丢 arena；包/索引/目录都不动（那是换地图的事）。
     */
    if (cache->style_id != (uint8_t)vmap_style_get_id()) {
        cache->style_id = (uint8_t)vmap_style_get_id();
        regional_clear(cache);
        vmap_cache_arena_reset();
        syslog(LOG_NOTICE, "[vmap] style changed -> tile cache dropped (%s)\n",
               vmap_style_current()->name);
    }

    if (z != cache->zoom) {
        vmap_tile_cache_set_zoom(cache, z);
    }

    cell = tile_cache_region(cache, z, x, y);
    rid = vmap_map_catalog_is_regional() ? cell_arena_key(cell) : (uint16_t)cell;

    if (vmap_cache_lookup(rid, z, x, y, &blob)) {
        if (blob.flags & VMAP_CACHE_FLAG_MISSING) {
            return false;
        }
        if (!tile_blob_magic_ok(blob.data, blob.size)
            || !vmap_tile_parse_borrowed(out_tile, blob.data, blob.size)) {
            vmap_cache_invalidate(rid, z, x, y);
        } else {
            return true;
        }
    }

    uint32_t size = 0;
    int loaded = load_from_flash(cache, z, x, y, &size);

    if (loaded < 0) {
        return false;
    }
    if (loaded == 0) {
        vmap_cache_insert_missing(rid, z, x, y);
        return false;
    }
    myvendor_watchdog_busy_pump();

    if (!tile_blob_magic_ok(cache->staging, size)) {
        vmap_cache_insert_missing(rid, z, x, y);
        return false;
    }

    if (vmap_cache_insert(rid, z, x, y, cache->staging, size) != 0) {
        return vmap_tile_parse_borrowed(out_tile, cache->staging, size);
    }

    if (vmap_cache_lookup(rid, z, x, y, &blob)
        && !(blob.flags & VMAP_CACHE_FLAG_MISSING)
        && tile_blob_magic_ok(blob.data, blob.size)) {
        return vmap_tile_parse_borrowed(out_tile, blob.data, blob.size);
    }

    return false;
}
