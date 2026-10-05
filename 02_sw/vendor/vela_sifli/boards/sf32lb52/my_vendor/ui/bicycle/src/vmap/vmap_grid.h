/**
 * @file vmap_grid.h
 * @brief Fixed national grid: lon/lat → (ix,iy) → lonN/latN/x_y.vpk
 */

#ifndef VMAP_GRID_H
#define VMAP_GRID_H

#include "vmap_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool vmap_grid_lonlat_to_cell(double lon, double lat,
    uint16_t * out_ix, uint16_t * out_iy);
void vmap_grid_cell_bbox(uint16_t ix, uint16_t iy,
    double * west, double * south, double * east, double * north);
void vmap_grid_vpk_path(char * buf, size_t sz, const char * dir,
    uint16_t ix, uint16_t iy);
bool vmap_grid_vpk_exists(const char * dir, uint16_t ix, uint16_t iy);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_GRID_H */
