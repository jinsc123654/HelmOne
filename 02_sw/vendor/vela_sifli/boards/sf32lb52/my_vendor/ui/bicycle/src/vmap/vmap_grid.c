/**
 * @file vmap_grid.c
 * @brief Coordinate → cell → filename (no map.idx).
 */

#include "vmap_grid.h"

#include "vmap_config.h"
#include "vmap_vpk_heap.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>

bool vmap_grid_lonlat_to_cell(double lon, double lat,
    uint16_t * out_ix, uint16_t * out_iy)
{
    double fx;
    double fy;
    int32_t ix;
    int32_t iy;

    if (!out_ix || !out_iy) {
        return false;
    }

    fx = (lon - VMAP_GRID_ORIGIN_LON) / VMAP_GRID_CELL_LON;
    fy = (lat - VMAP_GRID_ORIGIN_LAT) / VMAP_GRID_CELL_LAT;
    ix = (int32_t)floor(fx);
    iy = (int32_t)floor(fy);
    if (ix < 0 || iy < 0 || ix > 65535 || iy > 65535) {
        return false;
    }
    *out_ix = (uint16_t)ix;
    *out_iy = (uint16_t)iy;
    return true;
}

void vmap_grid_cell_bbox(uint16_t ix, uint16_t iy,
    double * west, double * south, double * east, double * north)
{
    double w = VMAP_GRID_ORIGIN_LON + (double)ix * VMAP_GRID_CELL_LON;
    double s = VMAP_GRID_ORIGIN_LAT + (double)iy * VMAP_GRID_CELL_LAT;

    if (west) {
        *west = w;
    }
    if (south) {
        *south = s;
    }
    if (east) {
        *east = w + VMAP_GRID_CELL_LON;
    }
    if (north) {
        *north = s + VMAP_GRID_CELL_LAT;
    }
}

void vmap_grid_vpk_path(char * buf, size_t sz, const char * dir,
    uint16_t ix, uint16_t iy)
{
    if (!buf || sz == 0) {
        return;
    }
    if (!dir || dir[0] == '\0') {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, sz, "%s/%s%u/%s%u/x%u_y%u.%s",
        dir,
        VMAP_GRID_BUCKET_LON, (unsigned)ix / VMAP_GRID_BUCKET,
        VMAP_GRID_BUCKET_LAT, (unsigned)iy / VMAP_GRID_BUCKET,
        (unsigned)ix, (unsigned)iy, VMAP_SHARD_EXT);
}

bool vmap_grid_vpk_exists(const char * dir, uint16_t ix, uint16_t iy)
{
    char path[VMAP_GRID_PATH_MAX];
    unsigned i;
    bool yes;
    static uint16_t s_ix[32];
    static uint16_t s_iy[32];
    static uint8_t s_yes[32];
    static uint8_t s_n;

    if (vmap_vpk_heap_has(VMAP_CELL_ID(ix, iy))) {
        return true;
    }

    for (i = 0; i < s_n; i++) {
        if (s_ix[i] == ix && s_iy[i] == iy) {
            return s_yes[i] != 0;
        }
    }

    vmap_grid_vpk_path(path, sizeof(path), dir, ix, iy);
    if (path[0] == '\0') {
        return false;
    }
    {
        struct stat st;

        yes = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
        if (!yes) {
            static bool s_logged;

            if (!s_logged) {
                s_logged = true;
                syslog(LOG_WARNING, "[vmap] missing %s errno=%d",
                    path, errno);
            }
        }
    }

    if (s_n < (unsigned)(sizeof(s_ix) / sizeof(s_ix[0]))) {
        s_ix[s_n] = ix;
        s_iy[s_n] = iy;
        s_yes[s_n] = yes ? 1u : 0u;
        s_n++;
    }
    return yes;
}
