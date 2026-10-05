/**
 * @file vmap_map_catalog.h
 * @brief Regional map: lon/lat → cell id → lonN/latN/x_y.vpk (no map.idx).
 */

#ifndef VMAP_MAP_CATALOG_H
#define VMAP_MAP_CATALOG_H

#include "vmap_format.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMAP_REG_NEIGHBOR_W  0x01u
#define VMAP_REG_NEIGHBOR_E  0x02u
#define VMAP_REG_NEIGHBOR_S  0x04u
#define VMAP_REG_NEIGHBOR_N  0x08u

typedef struct {
    vmap_cell_id_t id;
    uint16_t grid_ix;
    uint16_t grid_iy;
    uint16_t neighbors;
    double west;
    double south;
    double east;
    double north;
} vmap_map_region_t;

bool vmap_map_catalog_load(const char * map_dir);
void vmap_map_catalog_unload(void);
bool vmap_map_catalog_is_regional(void);
bool vmap_map_catalog_is_loaded(void);
uint16_t vmap_map_catalog_region_count(void);
vmap_cell_id_t vmap_map_catalog_active_region(void);

bool vmap_map_catalog_find_region(double lon, double lat, vmap_cell_id_t * out_id);
const vmap_map_region_t * vmap_map_catalog_region(vmap_cell_id_t region_id);

bool vmap_map_catalog_ensure_region(const char * map_dir, double lon, double lat,
    vmap_cell_id_t * out_region_id);
void vmap_map_catalog_set_active_region(vmap_cell_id_t region_id);

bool vmap_map_catalog_region_path(vmap_cell_id_t start_id, vmap_cell_id_t dest_id,
    vmap_cell_id_t * out_ids, uint32_t out_cap, uint32_t * out_len);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_MAP_CATALOG_H */
