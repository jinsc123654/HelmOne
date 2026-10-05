/**
 * @file vmap_tile_system.h
 * @brief Web Mercator 瓦片数学（TileSystem.cpp 的 C 移植）。
 */

#ifndef VMAP_TILE_SYSTEM_H
#define VMAP_TILE_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 指定 zoom 的地图像素边长。
 * @param level 缩放级别。
 * @return 请求的值。
 */
uint32_t vmap_map_size(int level);
void vmap_latlong_to_pixel(double lat, double lon, int zoom,
    int * pixel_x, int * pixel_y);
/** @brief 分数墨卡托像素（不取整）。瓦片仍用整数版，避免底图错位。 */
void vmap_latlong_to_pixel_d(double lat, double lon, int zoom,
    double * pixel_x, double * pixel_y);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_TILE_SYSTEM_H */
