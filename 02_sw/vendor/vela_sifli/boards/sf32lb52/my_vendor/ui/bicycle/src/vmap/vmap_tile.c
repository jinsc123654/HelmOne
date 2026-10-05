/**
 * @file vmap_tile.c
 * @brief vmap tile 模块。
 */

#include "vmap_tile.h"

#include "vmap_format.h"
#include <stddef.h>
#include <string.h>

/**
 * @brief vmap tile ptr in range。
 */
static bool vmap_tile_ptr_in_range(const uint8_t * base, size_t sz,
    const uint8_t * p)
{
    if (!base || !p) {
        return false;
    }
    return p >= base && p < base + sz;
}

/**
 * @brief vmap tile ptr add。
 */
static bool vmap_tile_ptr_add(const uint8_t * base, size_t sz,
    const uint8_t * p, size_t add, const uint8_t ** out)
{
    size_t used;

    if (!out || !vmap_tile_ptr_in_range(base, sz, p)) {
        return false;
    }
    used = (size_t)(p - base);
    if (add > sz - used) {
        return false;
    }
    *out = p + add;
    return true;
}

static inline uint16_t rd_u16(const uint8_t * p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t * p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/**
 * @brief 重置瓦片结构。
 * @param tile 瓦片。
 */
void vmap_tile_reset(vmap_tile_t * tile)
{
    if (!tile) {
        return;
    }
    memset(tile, 0, sizeof(*tile));
    tile->extent = VMAP_EXTENT;
}

/**
 * @brief 零拷贝解析瓦片缓冲。
 * @param tile 输出瓦片。
 * @param buf 原始字节。
 * @param sz 长度。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_tile_parse_borrowed(vmap_tile_t * tile, const uint8_t * buf, size_t sz)
{
    vmap_tile_reset(tile);

    if (!tile || !buf || sz < 20) {
        return false;
    }

    if (!(buf[0] == VMAP_MAGIC0 && buf[1] == VMAP_MAGIC1
            && buf[2] == VMAP_MAGIC2 && buf[3] == VMAP_MAGIC3)) {
        return false;
    }
    /* v1 and v2 share the same header/layer layout. */
    if (buf[4] != 1 && buf[4] != VMAP_VERSION) {
        return false;
    }

    tile->buf = buf;
    tile->extent = rd_u16(buf + 6);
    if (tile->extent == 0) {
        tile->extent = VMAP_EXTENT;
    }
    tile->tile_x = rd_u32(buf + 8);
    tile->tile_y = rd_u32(buf + 12);
    uint16_t layer_count = rd_u16(buf + 16);

    const uint8_t * p = buf + 20;

    for (uint16_t i = 0; i < layer_count && tile->num_layers < 8; i++) {
        uint8_t id;
        uint16_t feature_count;
        uint32_t layer_bytes;
        const uint8_t * hdr;
        const uint8_t * data;
        const uint8_t * next;

        hdr = p;
        if (!vmap_tile_ptr_add(buf, sz, p, 8, &p)) {
            break;
        }
        id = hdr[0];
        feature_count = rd_u16(hdr + 2);
        layer_bytes = rd_u32(hdr + 4);
        data = p;
        if (!vmap_tile_ptr_add(buf, sz, data, layer_bytes, &next)) {
            break;
        }
        tile->layers[tile->num_layers].id = id;
        tile->layers[tile->num_layers].feature_count = feature_count;
        tile->layers[tile->num_layers].data = data;
        tile->layers[tile->num_layers].bytes = layer_bytes;
        tile->num_layers++;
        p = next;
    }

    return true;
}

/**
 * @brief 瓦片 extent（像素）。
 * @param tile 瓦片。
 */
uint16_t vmap_tile_extent(const vmap_tile_t * tile)
{
    return tile ? tile->extent : VMAP_EXTENT;
}

/**
 * @brief 创建图层要素迭代器。
 */
vmap_feature_iter_t vmap_tile_layer_iter(const vmap_tile_t * tile, uint8_t layer_id)
{
    vmap_feature_iter_t it = { 0 };
    if (!tile) {
        return it;
    }

    for (uint16_t i = 0; i < tile->num_layers; i++) {
        if (tile->layers[i].id == layer_id) {
            it.ptr = tile->layers[i].data;
            it.end = tile->layers[i].data + tile->layers[i].bytes;
            it.remaining = tile->layers[i].feature_count;
            break;
        }
    }
    return it;
}

/**
 * @brief 迭代下一个要素。
 * @param it 迭代器。
 * @param out 输出要素。
 */
bool vmap_feature_iter_next(vmap_feature_iter_t * it, vmap_feature_t * out)
{
    if (!it || !out || it->remaining == 0 || !it->ptr || it->ptr + 4 > it->end) {
        return false;
    }

    out->attr = it->ptr[0];
    out->flags = it->ptr[1];
    out->point_count = rd_u16(it->ptr + 2);
    const uint8_t * coords = it->ptr + 4;
    const uint8_t * nextp = coords + (size_t)out->point_count * 4;

    out->coords = (const uint16_t *)coords;
    out->name_text = NULL;
    out->name_len = 0;
    if (out->flags & VMAP_FLAG_NAMED) {
        if (nextp + 1 > it->end) {
            it->remaining = 0;
            return false;
        }
        out->name_len = nextp[0];
        out->name_text = (const char *)(nextp + 1);
        nextp += 1u + out->name_len;
    }

    if (nextp > it->end) {
        it->remaining = 0;
        return false;
    }

    it->ptr = nextp;
    it->remaining--;
    return true;
}

/**
 * @brief 创建标签迭代器。
 */
vmap_label_iter_t vmap_tile_label_iter(const vmap_tile_t * tile)
{
    vmap_label_iter_t it = { 0 };
    if (!tile) {
        return it;
    }

    for (uint16_t i = 0; i < tile->num_layers; i++) {
        if (tile->layers[i].id == VMAP_LAYER_LABEL) {
            it.ptr = tile->layers[i].data;
            it.end = tile->layers[i].data + tile->layers[i].bytes;
            it.remaining = tile->layers[i].feature_count;
            break;
        }
    }
    return it;
}

/**
 * @brief 迭代下一个标签。
 */
bool vmap_label_iter_next(vmap_label_iter_t * it, vmap_label_t * out)
{
    if (!it || !out || it->remaining == 0 || !it->ptr || it->ptr + 8 > it->end) {
        return false;
    }

    out->kind = it->ptr[0];
    out->priority = it->ptr[1];
    out->x = (int32_t)rd_u16(it->ptr + 2);
    out->y = (int32_t)rd_u16(it->ptr + 4);
    out->len = it->ptr[6];
    out->text = (const char *)(it->ptr + 8);

    const uint8_t * nextp = it->ptr + 8 + out->len;
    if (nextp > it->end) {
        it->remaining = 0;
        return false;
    }

    it->ptr = nextp;
    it->remaining--;
    return true;
}
