/**
 * @file vmap_tile_system.c
 * @brief vmap tile_system 模块。
 */

#include "vmap_tile_system.h"

#include <math.h>

static const double k_min_lat = -85.05112878;
static const double k_max_lat = 85.05112878;
static const double k_pi = 3.14159265358979323846;

static double clipd(double n, double min_v, double max_v)
{
    if (n < min_v) {
        return min_v;
    }
    if (n > max_v) {
        return max_v;
    }
    return n;
}

/**
 * @brief 指定 zoom 的地图像素边长。
 * @param level 缩放级别。
 * @return 请求的值。
 */
uint32_t vmap_map_size(int level)
{
    return (uint32_t)256u << (unsigned)level;
}

/**
 * @brief 经纬度转像素坐标（分数，不四舍五入）。
 */
void vmap_latlong_to_pixel_d(double lat, double lon, int zoom,
    double * pixel_x, double * pixel_y)
{
    lat = clipd(lat, k_min_lat, k_max_lat);
    lon = clipd(lon, -180.0, 180.0);

    double x = (lon + 180.0) / 360.0;
    double sin_lat = sin(lat * k_pi / 180.0);
    double y = 0.5 - log((1.0 + sin_lat) / (1.0 - sin_lat)) / (4.0 * k_pi);

    uint32_t map_size = vmap_map_size(zoom);
    double max_px = (double)map_size - 1.0;

    if (pixel_x) {
        *pixel_x = clipd(x * (double)map_size, 0.0, max_px);
    }
    if (pixel_y) {
        *pixel_y = clipd(y * (double)map_size, 0.0, max_px);
    }
}

/**
 * @brief 经纬度转像素坐标。
 */
void vmap_latlong_to_pixel(double lat, double lon, int zoom,
    int * pixel_x, int * pixel_y)
{
    double px = 0.0;
    double py = 0.0;
    uint32_t map_size = vmap_map_size(zoom);
    double max_px = (double)map_size - 1.0;

    vmap_latlong_to_pixel_d(lat, lon, zoom, &px, &py);
    if (pixel_x) {
        *pixel_x = (int)clipd(px + 0.5, 0.0, max_px);
    }
    if (pixel_y) {
        *pixel_y = (int)clipd(py + 0.5, 0.0, max_px);
    }
}
