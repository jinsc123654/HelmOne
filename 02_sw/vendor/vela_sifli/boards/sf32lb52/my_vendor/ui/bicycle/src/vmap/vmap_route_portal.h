/**
 * @file vmap_route_portal.h
 * @brief vmap route_portal 模块。
 */

#ifndef VMAP_ROUTE_PORTAL_H
#define VMAP_ROUTE_PORTAL_H

#include "vmap_format.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    vmap_cell_id_t region_a;
    vmap_cell_id_t region_b;
    uint32_t node_a;
    uint32_t node_b;
    double lon;
    double lat;
} vmap_route_portal_rec_t;

/**
 * @brief vmap route portal load。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_portal_load(const char * map_dir);
/**
 * @brief vmap route portal unload。
 * @return 0 成功，负 errno 失败。
 */
void vmap_route_portal_unload(void);
/**
 * @brief vmap route portal loaded。
 * @return 0 成功，负 errno 失败。
 */
bool vmap_route_portal_loaded(void);

/** @brief rid_a 与 rid_b 之间的门户（双向）。 */
uint32_t vmap_route_portal_query(vmap_cell_id_t rid_a, vmap_cell_id_t rid_b,
    vmap_route_portal_rec_t * out, uint32_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_ROUTE_PORTAL_H */
