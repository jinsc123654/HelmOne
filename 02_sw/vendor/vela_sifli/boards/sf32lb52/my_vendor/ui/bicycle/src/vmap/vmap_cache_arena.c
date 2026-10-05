/**
 * @file vmap_cache_arena.c
 * @brief vmap cache_arena 模块。
 */

#include "vmap_cache_arena.h"

#include "vmap_format.h"
#include "board_malloc.h"
#include <nuttx/config.h>
#include <stdint.h>
#include <string.h>

#ifndef BICYCLE_STABILITY_PROBE
#  define BICYCLE_STABILITY_PROBE 0
#endif

#if BICYCLE_STABILITY_PROBE
#  include "App/Utils/StabilityProbe.h"
#else
#  define BICYCLE_STAB_WARN(...) ((void)0)
/**
 * @brief 自行车 stab arena evict cap。
 */
static inline void bicycle_stab_arena_evict_cap(void) { (void)0; }
/**
 * @brief 自行车 stab arena free drop。
 */
static inline void bicycle_stab_arena_free_drop(void) { (void)0; }
#endif

#ifndef CONFIG_MYVENDOR_VMAP_TILE_CACHE_KB
#  define CONFIG_MYVENDOR_VMAP_TILE_CACHE_KB 2048
#endif

#define VMAP_CACHE_TOTAL_BYTES \
    ((size_t)CONFIG_MYVENDOR_VMAP_TILE_CACHE_KB * 1024u)

#define VMAP_CACHE_HASH_BUCKETS 4096u
#define VMAP_CACHE_MAX_ENTRIES  2048u

struct meta_entry {
    uint16_t city_id;
    uint8_t z;
    uint8_t flags;
    uint16_t x;
    uint16_t y;
    uint32_t blob_len;
    uint32_t blob_off;
    uint32_t lru;
    uint32_t hash_next;
};

struct free_node {
    uint32_t off;
    uint32_t size;
    uint32_t next;
};

#define VMAP_CACHE_MAX_FREE 64

#define VMAP_CACHE_META_BYTES \
    (VMAP_CACHE_HASH_BUCKETS * sizeof(uint16_t) + \
     VMAP_CACHE_MAX_ENTRIES * sizeof(struct meta_entry))

static uint8_t * g_arena;
static size_t g_arena_bytes;
static uint16_t * g_hash;
static struct meta_entry * g_meta;
static uint8_t * g_blob_base;
static size_t g_blob_cap;
static size_t g_blob_high;
static uint32_t g_lru_tick;
static uint16_t g_city_id;

static struct free_node g_free[VMAP_CACHE_MAX_FREE];
static uint32_t g_free_head;

static uint32_t tile_hash(uint16_t city_id, uint8_t z, int x, int y)
{
    uint32_t h = (uint32_t)city_id * 2654435761u;
    h ^= (uint32_t)z * 2246822519u;
    h ^= (uint32_t)(uint16_t)x * 3266489917u;
    h ^= (uint32_t)(uint16_t)y * 668265263u;
    h ^= h >> 16;
    return h % VMAP_CACHE_HASH_BUCKETS;
}

static struct meta_entry * entry_at(uint32_t idx)
{
    return &g_meta[idx];
}

static int entry_alloc_index(void)
{
    for (uint32_t i = 0; i < VMAP_CACHE_MAX_ENTRIES; i++) {
        if ((g_meta[i].flags & VMAP_CACHE_FLAG_USED) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void hash_unlink(uint32_t bucket, uint32_t idx)
{
    uint32_t prev = 0;
    uint32_t cur = g_hash[bucket];

    while (cur != 0) {
        uint32_t i = cur - 1;
        if (i == idx) {
            uint32_t next = g_meta[i].hash_next;
            if (prev == 0) {
                g_hash[bucket] = next;
            } else {
                g_meta[prev - 1].hash_next = next;
            }
            g_meta[i].hash_next = 0;
            return;
        }
        prev = cur;
        cur = g_meta[i].hash_next;
    }
}

static void hash_link(uint32_t bucket, uint32_t idx)
{
    g_meta[idx].hash_next = g_hash[bucket];
    g_hash[bucket] = (uint16_t)(idx + 1);
}

static void free_unlink(uint32_t fi)
{
    uint32_t prev = 0;
    uint32_t cur = g_free_head;

    while (cur != 0) {
        if (cur - 1 == fi) {
            uint32_t next = g_free[fi].next;
            if (prev == 0) {
                g_free_head = next;
            } else {
                g_free[prev - 1].next = next;
            }
            g_free[fi].size = 0;
            g_free[fi].next = 0;
            return;
        }
        prev = cur;
        cur = g_free[prev - 1].next;
    }
}

static void free_push(uint32_t off, uint32_t size)
{
    int i;

    if (size < 8) {
        return;
    }

    for (i = 0; i < (int)VMAP_CACHE_MAX_FREE; i++) {
        if (g_free[i].size == 0) {
            continue;
        }

        if (g_free[i].off + g_free[i].size == off) {
            g_free[i].size += size;
            off = g_free[i].off;
            size = g_free[i].size;
            break;
        }

        if (off + size == g_free[i].off) {
            g_free[i].off = off;
            g_free[i].size += size;
            return;
        }
    }

    for (i = 0; i < (int)VMAP_CACHE_MAX_FREE; i++) {
        if (g_free[i].size == 0) {
            continue;
        }

        if (g_free[i].off + g_free[i].size == off) {
            g_free[i].size += size;
            return;
        }

        if (off + size == g_free[i].off) {
            g_free[i].off = off;
            g_free[i].size += size;
            return;
        }
    }

    for (i = 0; i < (int)VMAP_CACHE_MAX_FREE; i++) {
        if (g_free[i].size == 0) {
            g_free[i].off = off;
            g_free[i].size = size;
            g_free[i].next = g_free_head;
            g_free_head = (uint32_t)(i + 1);
            return;
        }
    }

    {
        int victim = -1;
        uint32_t victim_size = UINT32_MAX;

        for (i = 0; i < (int)VMAP_CACHE_MAX_FREE; i++) {
            if (g_free[i].size == 0) {
                continue;
            }
            if (g_free[i].size < victim_size) {
                victim_size = g_free[i].size;
                victim = i;
            }
        }

        if (victim >= 0 && size >= victim_size) {
            free_unlink((uint32_t)victim);
            g_free[victim].off = off;
            g_free[victim].size = size;
            g_free[victim].next = g_free_head;
            g_free_head = (uint32_t)(victim + 1);
            BICYCLE_STAB_WARN("vmap arena: free list full, replaced %uB with %uB",
                (unsigned)victim_size, (unsigned)size);
            bicycle_stab_arena_free_drop();
        } else if (victim >= 0) {
            BICYCLE_STAB_WARN("vmap arena: free list full, dropped %uB release",
                (unsigned)size);
            bicycle_stab_arena_free_drop();
        }
    }
}

static int blob_alloc(uint32_t need)
{
    need = (need + 3u) & ~3u;

    uint32_t prev = 0;
    uint32_t cur = g_free_head;
    while (cur != 0) {
        uint32_t fi = cur - 1;
        if (g_free[fi].size >= need) {
            uint32_t off = g_free[fi].off;
            uint32_t rem = g_free[fi].size - need;
            if (rem >= 8) {
                g_free[fi].off = off + need;
                g_free[fi].size = rem;
            } else {
                uint32_t next = g_free[fi].next;
                if (prev == 0) {
                    g_free_head = next;
                } else {
                    g_free[prev - 1].next = next;
                }
                g_free[fi].size = 0;
                g_free[fi].next = 0;
            }
            return (int)off;
        }
        prev = cur;
        cur = g_free[fi].next;
    }

    if (g_blob_high + need > g_blob_cap) {
        return -1;
    }

    {
        uint32_t off = (uint32_t)g_blob_high;
        g_blob_high += need;
        return (int)off;
    }
}

static void entry_release(uint32_t idx)
{
    struct meta_entry * e = entry_at(idx);
    if ((e->flags & VMAP_CACHE_FLAG_USED) == 0) {
        return;
    }

    {
        uint32_t bucket = tile_hash(e->city_id, e->z, (int)e->x, (int)e->y);
        hash_unlink(bucket, idx);
    }

    if (e->blob_len > 0) {
        free_push(e->blob_off, e->blob_len);
    }

    memset(e, 0, sizeof(*e));
}

static int entry_find_lru(void)
{
    int victim = -1;
    uint32_t best = 0xffffffffu;

    for (uint32_t i = 0; i < VMAP_CACHE_MAX_ENTRIES; i++) {
        if ((g_meta[i].flags & VMAP_CACHE_FLAG_USED) == 0) {
            continue;
        }
        if (g_meta[i].lru <= best) {
            best = g_meta[i].lru;
            victim = (int)i;
        }
    }

    return victim;
}

static struct meta_entry * entry_lookup(uint16_t city_id, int z, int x, int y)
{
    uint32_t bucket = tile_hash(city_id, (uint8_t)z, x, y);
    uint32_t cur = g_hash[bucket];

    while (cur != 0) {
        uint32_t i = cur - 1;
        struct meta_entry * e = entry_at(i);
        if (e->city_id == city_id && e->z == (uint8_t)z &&
            e->x == (uint16_t)x && e->y == (uint16_t)y) {
            return e;
        }
        cur = e->hash_next;
    }

    return NULL;
}

/**
 * @brief 初始化瓦片缓存 arena。
 * @return 成功/有效则为 true。
 */
int vmap_cache_arena_init(void)
{
    if (g_arena != NULL) {
        return 0;
    }

    g_arena_bytes = VMAP_CACHE_TOTAL_BYTES;
    if (g_arena_bytes <= VMAP_CACHE_META_BYTES + 4096u) {
        return -1;
    }

    g_arena = (uint8_t *)board_malloc_psram(g_arena_bytes);
    if (!g_arena) {
        return -1;
    }

    vmap_cache_arena_reset();
    return 0;
}

/**
 * @brief 重置缓存 arena。
 */
void vmap_cache_arena_reset(void)
{
    if (!g_arena) {
        return;
    }

    memset(g_arena, 0, g_arena_bytes);
    g_hash = (uint16_t *)g_arena;
    g_meta = (struct meta_entry *)(g_arena + VMAP_CACHE_HASH_BUCKETS * sizeof(uint16_t));
    g_blob_base = g_arena + VMAP_CACHE_META_BYTES;
    g_blob_cap = g_arena_bytes - VMAP_CACHE_META_BYTES;
    g_blob_high = 0;
    g_lru_tick = 0;
    g_free_head = 0;
    memset(g_free, 0, sizeof(g_free));
}

/**
 * @brief 当前城市 ID。
 * @return 请求的值。
 */
uint16_t vmap_cache_city_id(void)
{
    return g_city_id;
}

/**
 * @brief 设置当前城市 ID。
 * @param city_id 城市 ID。
 * @return 请求的值。
 */
void vmap_cache_set_city_id(uint16_t city_id)
{
    /* Active region marker only — tiles are keyed by owning region id so a
     * single frame can hold blobs from several neighbors at once. */
    g_city_id = city_id;
}

/**
 * @brief 使单瓦片缓存项失效。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_cache_invalidate(uint16_t city_id, int z, int x, int y)
{
    struct meta_entry * e;

    if (!g_arena) {
        return;
    }

    e = entry_lookup(city_id, z, x, y);
    if (e) {
        entry_release((uint32_t)(e - g_meta));
    }
}

/**
 * @brief 清空指定城市缓存。
 */
void vmap_cache_flush_city(uint16_t city_id)
{
    if (!g_arena) {
        return;
    }

    for (uint32_t i = 0; i < VMAP_CACHE_MAX_ENTRIES; i++) {
        if ((g_meta[i].flags & VMAP_CACHE_FLAG_USED) &&
            g_meta[i].city_id == city_id) {
            entry_release(i);
        }
    }
}

/**
 * @brief 查找缓存瓦片。
 * @param out 输出 blob。
 */
int vmap_cache_lookup(uint16_t city_id, int z, int x, int y,
    vmap_cache_blob_t * out)
{
    struct meta_entry * e;

    if (!g_arena || !out) {
        return 0;
    }

    e = entry_lookup(city_id, z, x, y);
    if (!e) {
        return 0;
    }

    e->lru = ++g_lru_tick;
    out->flags = e->flags;
    out->size = e->blob_len;
    if (e->blob_len > 0) {
        out->data = g_blob_base + e->blob_off;
    } else {
        out->data = NULL;
    }

    return 1;
}

static int entry_insert(uint16_t city_id, int z, int x, int y,
    const uint8_t * data, uint32_t size, int missing)
{
    struct meta_entry * existing;
    int idx;
    int meta_evict_attempts = 0;
    struct meta_entry * e;

    if (!g_arena) {
        if (vmap_cache_arena_init() != 0) {
            return -1;
        }
    }

    existing = entry_lookup(city_id, z, x, y);
    if (existing) {
        uint32_t eidx = (uint32_t)(existing - g_meta);
        entry_release(eidx);
    }

    idx = entry_alloc_index();
    while (idx < 0) {
        if (++meta_evict_attempts > (int)VMAP_CACHE_MAX_ENTRIES) {
            BICYCLE_STAB_WARN("vmap arena: meta slot evict cap");
            bicycle_stab_arena_evict_cap();
            return -1;
        }
        {
            int victim = entry_find_lru();
            if (victim < 0) {
                return -1;
            }
            entry_release((uint32_t)victim);
        }
        idx = entry_alloc_index();
    }

    e = entry_at((uint32_t)idx);
    e->city_id = city_id;
    e->z = (uint8_t)z;
    e->x = (uint16_t)x;
    e->y = (uint16_t)y;
    e->flags = VMAP_CACHE_FLAG_USED;
    e->lru = ++g_lru_tick;

    if (missing) {
        e->flags |= VMAP_CACHE_FLAG_MISSING;
        e->blob_len = 0;
        e->blob_off = 0;
    } else {
        uint32_t need = ((uint32_t)size + 3u) & ~3u;
        int off = -1;
        int evict_attempts = 0;

        while (off < 0) {
            off = blob_alloc(need);
            if (off >= 0) {
                break;
            }
            if (++evict_attempts > (int)VMAP_CACHE_MAX_ENTRIES) {
                BICYCLE_STAB_WARN(
                    "vmap arena: blob evict cap z=%d (%d,%d) need=%u high=%u cap=%u",
                    z, x, y, (unsigned)need, (unsigned)g_blob_high,
                    (unsigned)g_blob_cap);
                bicycle_stab_arena_evict_cap();
                memset(e, 0, sizeof(*e));
                return -1;
            }
            {
                int victim = entry_find_lru();
                if (victim < 0) {
                    memset(e, 0, sizeof(*e));
                    return -1;
                }
                entry_release((uint32_t)victim);
            }
        }

        memcpy(g_blob_base + off, data, size);
        e->blob_off = (uint32_t)off;
        e->blob_len = size;
    }

    hash_link(tile_hash(city_id, (uint8_t)z, x, y), (uint32_t)idx);
    return 0;
}

/**
 * @brief 插入缓存瓦片。
 * @return true 成功/有效，false 失败/无效。
 */
int vmap_cache_insert(uint16_t city_id, int z, int x, int y,
    const uint8_t * data, uint32_t size)
{
    if (!data || size < 20 || size > VMAP_TILE_BYTES_MAX) {
        return -1;
    }
    return entry_insert(city_id, z, x, y, data, size, 0);
}

/**
 * @brief 标记瓦片缺失（负缓存）。
 * @return true 成功/有效，false 失败/无效。
 */
int vmap_cache_insert_missing(uint16_t city_id, int z, int x, int y)
{
    return entry_insert(city_id, z, x, y, NULL, 0, 1);
}

/**
 * @brief 查询缓存统计。
 */
void vmap_cache_stats(size_t * used_bytes, size_t * blob_bytes,
    uint32_t * entry_count)
{
    size_t meta = VMAP_CACHE_META_BYTES;
    size_t blob_used = g_blob_high;
    uint32_t n = 0;

    if (g_arena) {
        for (uint32_t i = 0; i < VMAP_CACHE_MAX_ENTRIES; i++) {
            if (g_meta[i].flags & VMAP_CACHE_FLAG_USED) {
                n++;
            }
        }
    }

    if (used_bytes) {
        *used_bytes = meta + blob_used;
    }
    if (blob_bytes) {
        *blob_bytes = blob_used;
    }
    if (entry_count) {
        *entry_count = n;
    }
}
