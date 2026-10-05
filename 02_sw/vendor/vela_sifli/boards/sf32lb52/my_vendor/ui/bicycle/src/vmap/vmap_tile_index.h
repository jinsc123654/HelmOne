/**
 * @file vmap_tile_index.h
 * @brief 打包 map.idx 查找（C）。
 */

#ifndef VMAP_TILE_INDEX_H
#define VMAP_TILE_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t pack_id;
    uint32_t offset;
    uint32_t size;
} vmap_tile_ref_t;

typedef struct {
    uint16_t pack_id;
    uint32_t offset;
    uint32_t size;
} vmap_graph_shard_ref_t;

typedef struct vmap_tile_index vmap_tile_index_t;

/**
 * @brief 创建索引句柄。
 * @return 0 成功，负 errno 失败。
 */
vmap_tile_index_t * vmap_tile_index_create(void);
/**
 * @brief 销毁索引句柄。
 * @param idx 索引。
 */
void vmap_tile_index_destroy(vmap_tile_index_t * idx);
/**
 * @brief 从目录加载 map.idx。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_tile_index_load(vmap_tile_index_t * idx, const char * tile_dir, int zoom);
/**
 * @brief 卸载索引。
 * @return 0 成功，负 errno 失败。
 */
void vmap_tile_index_unload(vmap_tile_index_t * idx);
bool vmap_tile_index_find(const vmap_tile_index_t * idx, int zoom, int tile_x,
    int tile_y, vmap_tile_ref_t * out);
/**
 * @brief 索引是否已加载。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_tile_index_is_loaded(const vmap_tile_index_t * idx);
bool vmap_tile_index_graph(const vmap_tile_index_t * idx,
    const vmap_graph_shard_ref_t ** out_shards, uint16_t * out_count,
    uint32_t * out_total);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_TILE_INDEX_H */
