/**
 * @file vmap_tile_index.c
 * @brief vmap tile_index 模块。
 */

#include "vmap_tile_index.h"

#include "vmap_format.h"
#include "vmap_config.h"
#include "vmap_alloc.h"
#include <myvendor_mtp_lfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vmap_tile_index {
    struct entry {
        uint8_t z;
        uint16_t x;
        uint16_t y;
        uint16_t pack_id;
        uint32_t offset;
        uint32_t size;
    } * entries;
    uint16_t count;
    vmap_graph_shard_ref_t * graph_shards;
    uint16_t graph_count;
    uint32_t graph_total;
    bool loaded;
};

static inline uint16_t rd_u16(const uint8_t * p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t * p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/**
 * @brief 创建索引句柄。
 * @return 0 成功，负 errno 失败。
 */
vmap_tile_index_t * vmap_tile_index_create(void)
{
    return (vmap_tile_index_t *)calloc(1, sizeof(vmap_tile_index_t));
}

/**
 * @brief 销毁索引句柄。
 * @param idx 索引。
 */
void vmap_tile_index_destroy(vmap_tile_index_t * idx)
{
    if (!idx) {
        return;
    }
    vmap_tile_index_unload(idx);
    free(idx);
}

/**
 * @brief 卸载索引。
 * @return 0 成功，负 errno 失败。
 */
void vmap_tile_index_unload(vmap_tile_index_t * idx)
{
    if (!idx) {
        return;
    }
    if (idx->entries) {
        vmap_free(idx->entries);
        idx->entries = NULL;
    }
    if (idx->graph_shards) {
        vmap_free(idx->graph_shards);
        idx->graph_shards = NULL;
    }
    idx->count = 0;
    idx->graph_count = 0;
    idx->graph_total = 0;
    idx->loaded = false;
}

/**
 * @brief 从目录加载 map.idx。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_tile_index_load(vmap_tile_index_t * idx, const char * tile_dir, int zoom)
{
    (void)zoom;

    if (!idx || !tile_dir) {
        return false;
    }

    if (idx->loaded) {
        return true;
    }

    if (myvendor_mtp_lfs_quiesce()) {
        return idx->entries != NULL;
    }

    char path[72];
    snprintf(path, sizeof(path), "%s/%s", tile_dir, VMAP_MAP_INDEX_FILE);

    FILE * fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    uint8_t hdr[VMAP_MAP_INDEX_HDR_SIZE];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return false;
    }

    if (hdr[0] == VMAP_REG_INDEX_MAGIC0 && hdr[1] == VMAP_REG_INDEX_MAGIC1
        && hdr[2] == VMAP_REG_INDEX_MAGIC2 && hdr[3] == VMAP_REG_INDEX_MAGIC3) {
        fclose(fp);
        return false;
    }

    if (!(hdr[0] == VMAP_MAP_INDEX_MAGIC0 && hdr[1] == VMAP_MAP_INDEX_MAGIC1
            && hdr[2] == VMAP_MAP_INDEX_MAGIC2 && hdr[3] == VMAP_MAP_INDEX_MAGIC3)) {
        fclose(fp);
        return false;
    }

    if (hdr[4] != VMAP_MAP_INDEX_VERSION) {
        fclose(fp);
        return false;
    }

    const uint32_t tile_count = rd_u32(hdr + 12);
    const uint32_t graph_total = rd_u32(hdr + 16);
    const uint16_t graph_count = rd_u16(hdr + 20);

    if (tile_count == 0 || tile_count > 65535u) {
        fclose(fp);
        return false;
    }

    size_t bytes = (size_t)tile_count * sizeof(*idx->entries);
    idx->entries = (struct entry *)vmap_malloc(bytes);
    if (!idx->entries) {
        fclose(fp);
        return false;
    }

    for (uint32_t i = 0; i < tile_count; i++) {
        uint8_t rec[VMAP_MAP_INDEX_TILE_REC_SIZE];
        if (fread(rec, 1, sizeof(rec), fp) != sizeof(rec)) {
            vmap_free(idx->entries);
            idx->entries = NULL;
            fclose(fp);
            return false;
        }
        idx->entries[i].z = rec[0];
        idx->entries[i].x = rd_u16(rec + 2);
        idx->entries[i].y = rd_u16(rec + 4);
        idx->entries[i].pack_id = rd_u16(rec + 6);
        idx->entries[i].offset = rd_u32(rec + 8);
        idx->entries[i].size = rd_u16(rec + 12);
    }

    idx->count = (uint16_t)tile_count;
    idx->graph_total = graph_total;
    idx->graph_count = graph_count;

    if (graph_count > 0 && graph_total > 0) {
        idx->graph_shards = (vmap_graph_shard_ref_t *)vmap_malloc(
            (size_t)graph_count * sizeof(*idx->graph_shards));
        if (!idx->graph_shards) {
            vmap_free(idx->entries);
            idx->entries = NULL;
            fclose(fp);
            return false;
        }
        for (uint16_t i = 0; i < graph_count; i++) {
            uint8_t rec[VMAP_MAP_INDEX_GRAPH_REC_SIZE];
            if (fread(rec, 1, sizeof(rec), fp) != sizeof(rec)) {
                vmap_free(idx->graph_shards);
                idx->graph_shards = NULL;
                vmap_free(idx->entries);
                idx->entries = NULL;
                fclose(fp);
                return false;
            }
            idx->graph_shards[i].pack_id = rd_u16(rec + 0);
            idx->graph_shards[i].offset = rd_u32(rec + 2);
            idx->graph_shards[i].size = rd_u32(rec + 6);
        }
    }

    fclose(fp);
    idx->loaded = true;
    return true;
}

/**
 * @brief 索引是否已加载。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_tile_index_is_loaded(const vmap_tile_index_t * idx)
{
    return idx && idx->loaded && idx->entries != NULL;
}

/**
 * @brief 获取图分片表。
 */
bool vmap_tile_index_graph(const vmap_tile_index_t * idx,
    const vmap_graph_shard_ref_t ** out_shards, uint16_t * out_count,
    uint32_t * out_total)
{
    if (!idx || !idx->graph_shards || idx->graph_count == 0) {
        if (out_shards) {
            *out_shards = NULL;
        }
        if (out_count) {
            *out_count = 0;
        }
        if (out_total) {
            *out_total = 0;
        }
        return false;
    }
    if (out_shards) {
        *out_shards = idx->graph_shards;
    }
    if (out_count) {
        *out_count = idx->graph_count;
    }
    if (out_total) {
        *out_total = idx->graph_total;
    }
    return true;
}

/**
 * @brief 查找瓦片引用。
 * @param out 输出引用。
 * @return true 成功/有效，false 失败/无效。
 */
bool vmap_tile_index_find(const vmap_tile_index_t * idx, int zoom, int tile_x,
    int tile_y, vmap_tile_ref_t * out)
{
    if (!idx || !idx->entries || idx->count == 0) {
        return false;
    }

    int lo = 0;
    int hi = (int)idx->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const struct entry * e = &idx->entries[mid];
        if (e->z < (uint8_t)zoom
            || (e->z == (uint8_t)zoom && e->y < (uint16_t)tile_y)
            || (e->z == (uint8_t)zoom && e->y == (uint16_t)tile_y
                && e->x < (uint16_t)tile_x)) {
            lo = mid + 1;
        } else if (e->z > (uint8_t)zoom
            || (e->z == (uint8_t)zoom && e->y > (uint16_t)tile_y)
            || (e->z == (uint8_t)zoom && e->y == (uint16_t)tile_y
                && e->x > (uint16_t)tile_x)) {
            hi = mid - 1;
        } else {
            if (out) {
                out->pack_id = e->pack_id;
                out->offset = e->offset;
                out->size = e->size;
            }
            return true;
        }
    }

    return false;
}
