/**
 * @file vmap_tile.h
 * @brief VTIL 瓦片解析器 — 零拷贝要素迭代。
 */

#ifndef VMAP_TILE_H
#define VMAP_TILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t attr;
    uint8_t flags;
    uint16_t point_count;
    const uint16_t * coords;
    const char * name_text;
    uint8_t name_len;
} vmap_feature_t;

typedef struct {
    const uint8_t * ptr;
    const uint8_t * end;
    uint16_t remaining;
} vmap_feature_iter_t;

typedef struct {
    uint8_t kind;
    uint8_t priority;
    int32_t x;
    int32_t y;
    const char * text;
    uint8_t len;
} vmap_label_t;

typedef struct {
    const uint8_t * ptr;
    const uint8_t * end;
    uint16_t remaining;
} vmap_label_iter_t;

typedef struct {
    const uint8_t * buf;
    uint16_t extent;
    uint32_t tile_x;
    uint32_t tile_y;
    struct {
        uint8_t id;
        uint16_t feature_count;
        const uint8_t * data;
        uint32_t bytes;
    } layers[8];
    uint16_t num_layers;
} vmap_tile_t;

/**
 * @brief 零拷贝解析瓦片缓冲。
 * @param tile 输出瓦片。
 * @param buf 原始字节。
 * @param sz 长度。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_tile_parse_borrowed(vmap_tile_t * tile, const uint8_t * buf, size_t sz);
/**
 * @brief 重置瓦片结构。
 * @param tile 瓦片。
 */
void vmap_tile_reset(vmap_tile_t * tile);
/**
 * @brief 瓦片 extent（像素）。
 * @param tile 瓦片。
 */
uint16_t vmap_tile_extent(const vmap_tile_t * tile);

/**
 * @brief 创建图层要素迭代器。
 */
vmap_feature_iter_t vmap_tile_layer_iter(const vmap_tile_t * tile, uint8_t layer_id);
/**
 * @brief 创建标签迭代器。
 */
vmap_label_iter_t vmap_tile_label_iter(const vmap_tile_t * tile);
/**
 * @brief 迭代下一个要素。
 * @param it 迭代器。
 * @param out 输出要素。
 */
bool vmap_feature_iter_next(vmap_feature_iter_t * it, vmap_feature_t * out);
/**
 * @brief 迭代下一个标签。
 */
bool vmap_label_iter_next(vmap_label_iter_t * it, vmap_label_t * out);

static inline bool vmap_feature_is_closed(const vmap_feature_t * f)
{
    return (f->flags & 0x01) != 0;
}

static inline bool vmap_feature_has_name(const vmap_feature_t * f)
{
    return f && f->name_len > 0 && f->name_text != NULL;
}

static inline int32_t vmap_feature_x(const vmap_feature_t * f, uint16_t i)
{
    return (int32_t)f->coords[i * 2];
}

static inline int32_t vmap_feature_y(const vmap_feature_t * f, uint16_t i)
{
    return (int32_t)f->coords[i * 2 + 1];
}

#ifdef __cplusplus
}
#endif

#endif /* VMAP_TILE_H */
