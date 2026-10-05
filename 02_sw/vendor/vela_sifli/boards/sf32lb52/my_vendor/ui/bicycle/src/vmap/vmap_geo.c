/**
 * @file vmap_geo.c
 * @brief vmap geo 模块。
 */

#include "vmap_geo.h"

#include <math.h>

/**
 * @brief vmap geo haversine m。
 */
double vmap_geo_haversine_m(double lon1, double lat1, double lon2, double lat2)
{
    const double r = 6371000.0;
    const double dlat = (lat2 - lat1) * M_PI / 180.0;
    const double dlon = (lon2 - lon1) * M_PI / 180.0;
    const double lat1r = lat1 * M_PI / 180.0;
    const double lat2r = lat2 * M_PI / 180.0;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0)
        + cos(lat1r) * cos(lat2r) * sin(dlon / 2.0) * sin(dlon / 2.0);

    return 2.0 * r * asin(fmin(1.0, sqrt(a)));
}

/**
 * @brief vmap geo bearing deg。
 */
float vmap_geo_bearing_deg(double lon1, double lat1, double lon2, double lat2)
{
    const double lat1r = lat1 * M_PI / 180.0;
    const double lat2r = lat2 * M_PI / 180.0;
    const double dlon = (lon2 - lon1) * M_PI / 180.0;
    const double y = sin(dlon) * cos(lat2r);
    const double x = cos(lat1r) * sin(lat2r)
        - sin(lat1r) * cos(lat2r) * cos(dlon);
    float brg = (float)(atan2(y, x) * 180.0 / M_PI);

    if (brg < 0.0f) {
        brg += 360.0f;
    }
    return brg;
}

/**
 * @brief vmap geo angle delta deg。
 */
float vmap_geo_angle_delta_deg(float from_deg, float to_deg)
{
    float d = to_deg - from_deg;

    while (d > 180.0f) {
        d -= 360.0f;
    }
    while (d < -180.0f) {
        d += 360.0f;
    }
    return d;
}

/**
 * @brief vmap geo point to seg m。
 */
double vmap_geo_point_to_seg_m(double px, double py,
    double ax, double ay, double bx, double by, double * t_out)
{
    const double seg_m = vmap_geo_haversine_m(ax, ay, bx, by);
    float brg = vmap_geo_bearing_deg(ax, ay, bx, by);
    float brg_p = vmap_geo_bearing_deg(ax, ay, px, py);
    const double ap_m = vmap_geo_haversine_m(ax, ay, px, py);
    double t = 0.0;

    if (seg_m < 0.5) {
        if (t_out) {
            *t_out = 0.0;
        }
        return ap_m;
    }

    {
        const double along = ap_m * cos((brg_p - brg) * M_PI / 180.0);

        t = along / seg_m;
        if (t < 0.0) {
            t = 0.0;
        } else if (t > 1.0) {
            t = 1.0;
        }
    }

    if (t_out) {
        *t_out = t;
    }

    {
        const double mid_lon = ax + (bx - ax) * t;
        const double mid_lat = ay + (by - ay) * t;

        return vmap_geo_haversine_m(px, py, mid_lon, mid_lat);
    }
}

/**
 * @brief vmap geo project polyline m。
 */
double vmap_geo_project_polyline_m(const double * lons, const double * lats,
    uint32_t n, double lon, double lat, uint32_t * seg_index, double * t_on_seg)
{
    double best_d = 1e18;
    uint32_t best_i = 0;
    double best_t = 0.0;

    if (!lons || !lats || n < 2) {
        if (seg_index) {
            *seg_index = 0;
        }
        if (t_on_seg) {
            *t_on_seg = 0.0;
        }
        return 1e18;
    }

    for (uint32_t i = 0; i + 1 < n; i++) {
        double t = 0.0;
        const double d = vmap_geo_point_to_seg_m(lon, lat,
            lons[i], lats[i], lons[i + 1], lats[i + 1], &t);

        if (d < best_d) {
            best_d = d;
            best_i = i;
            best_t = t;
        }
    }

    if (seg_index) {
        *seg_index = best_i;
    }
    if (t_on_seg) {
        *t_on_seg = best_t;
    }
    return best_d;
}
