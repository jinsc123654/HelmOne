/**
 * @file vmap_geo.h
 * @brief vmap geo 模块。
 */

#ifndef VMAP_GEO_H
#define VMAP_GEO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief vmap geo haversine m。
 */
double vmap_geo_haversine_m(double lon1, double lat1, double lon2, double lat2);
/**
 * @brief vmap geo bearing deg。
 */
float vmap_geo_bearing_deg(double lon1, double lat1, double lon2, double lat2);
/**
 * @brief vmap geo angle delta deg。
 */
float vmap_geo_angle_delta_deg(float from_deg, float to_deg);

/** @brief 点到线段 AB 的最短距离（米）。 */
double vmap_geo_point_to_seg_m(double px, double py,
    double ax, double ay, double bx, double by, double * t_out);

/**
 * Project point onto polyline; returns along-route distance to closest point (m)
 * and sets seg_index / t_on_seg.
 */
double vmap_geo_project_polyline_m(const double * lons, const double * lats,
    uint32_t n, double lon, double lat, uint32_t * seg_index, double * t_on_seg);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_GEO_H */
