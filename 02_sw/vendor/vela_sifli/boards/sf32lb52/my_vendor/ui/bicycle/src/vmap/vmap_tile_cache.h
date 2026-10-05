/**
 * @file vmap_tile_cache.h
 * @brief vmap tile_cache 模块。
 */

#ifndef VMAP_TILE_CACHE_H
#define VMAP_TILE_CACHE_H

#include "vmap_tile.h"
#include "vmap_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vmap_tile_cache vmap_tile_cache_t;

/**
 * @brief vmap tile cache create。
 * @return 0 成功，负 errno 失败。
 */
vmap_tile_cache_t * vmap_tile_cache_create(void);
/**
 * @brief vmap tile cache destroy。
 */
void vmap_tile_cache_destroy(vmap_tile_cache_t * cache);
/**
 * @brief vmap tile cache set dir。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_tile_cache_set_dir(vmap_tile_cache_t * cache, const char * dir);
/**
 * @brief vmap tile cache set city。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_tile_cache_set_city(vmap_tile_cache_t * cache, uint16_t city_id);
bool vmap_tile_cache_set_region(vmap_tile_cache_t * cache, vmap_cell_id_t region_id);
bool vmap_tile_cache_ensure_region(vmap_tile_cache_t * cache, double lon,
    double lat);
vmap_cell_id_t vmap_tile_cache_active_region(const vmap_tile_cache_t * cache);
bool vmap_tile_cache_region_graph(const vmap_tile_cache_t * cache,
    uint32_t * graph_off, uint32_t * graph_sz, char * vpk_path, size_t path_sz);
/**
 * @brief vmap tile cache set zoom。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_tile_cache_set_zoom(vmap_tile_cache_t * cache, int zoom);
bool vmap_tile_cache_get(vmap_tile_cache_t * cache, int z, int x, int y,
    vmap_tile_t * out_tile);
/** @brief 瓦片已在 RAM（含负缓存）则为 true，不读闪存。 */
bool vmap_tile_cache_cached(vmap_tile_cache_t * cache, int z, int x, int y);
/** @brief 若未缓存则从闪存装入 arena；已在 RAM 则立即返回。 */
bool vmap_tile_cache_warm(vmap_tile_cache_t * cache, int z, int x, int y);
/** @brief 关闭分片/索引文件句柄；保留 RAM 缓存。 */
void vmap_tile_cache_release_lfs(vmap_tile_cache_t * cache);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_TILE_CACHE_H */
