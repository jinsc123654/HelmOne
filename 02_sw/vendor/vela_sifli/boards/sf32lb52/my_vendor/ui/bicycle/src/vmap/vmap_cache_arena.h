/**
 * @file vmap_cache_arena.h
 * @brief 固定 PSRAM arena：按 (cityId,z,x,y) 缓存已解析瓦片。
 */

#ifndef VMAP_CACHE_ARENA_H
#define VMAP_CACHE_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMAP_CACHE_FLAG_USED    0x01
#define VMAP_CACHE_FLAG_MISSING 0x02

typedef struct vmap_cache_blob {
    const uint8_t * data;
    uint32_t size;
    uint8_t flags;
} vmap_cache_blob_t;

/**
 * @brief 初始化瓦片缓存 arena。
 * @return 成功/有效则为 true。
 */
int  vmap_cache_arena_init(void);
/**
 * @brief 重置缓存 arena。
 */
void vmap_cache_arena_reset(void);

/**
 * @brief 当前城市 ID。
 * @return 请求的值。
 */
uint16_t vmap_cache_city_id(void);
/**
 * @brief 设置当前城市 ID。
 * @param city_id 城市 ID。
 * @return 请求的值。
 */
void     vmap_cache_set_city_id(uint16_t city_id);

/**
 * @brief 清空指定城市缓存。
 */
void vmap_cache_flush_city(uint16_t city_id);
/**
 * @brief 使单瓦片缓存项失效。
 * @return true 成功/有效，false 失败/无效。
 */
void vmap_cache_invalidate(uint16_t city_id, int z, int x, int y);

int vmap_cache_lookup(uint16_t city_id, int z, int x, int y,
    vmap_cache_blob_t * out);

int vmap_cache_insert(uint16_t city_id, int z, int x, int y,
    const uint8_t * data, uint32_t size);

/**
 * @brief 标记瓦片缺失（负缓存）。
 * @return true 成功/有效，false 失败/无效。
 */
int vmap_cache_insert_missing(uint16_t city_id, int z, int x, int y);

void vmap_cache_stats(size_t * used_bytes, size_t * blob_bytes,
    uint32_t * entry_count);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_CACHE_ARENA_H */
