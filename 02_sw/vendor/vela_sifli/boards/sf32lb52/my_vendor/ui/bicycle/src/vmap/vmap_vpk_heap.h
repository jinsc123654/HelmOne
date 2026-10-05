/**
 * @file vmap_vpk_heap.h
 * @brief Dedicated PSRAM heap that caches whole cell vpk files.
 *
 * Load once from flash (fopen/read/fclose); later tile / graph / portal reads
 * hit RAM. When a new file does not fit, the oldest unpinned file is dropped.
 */

#ifndef VMAP_VPK_HEAP_H
#define VMAP_VPK_HEAP_H

#include "vmap_format.h"
#include "vmap_vpk_heap_stats.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void vmap_vpk_heap_init(void);
void vmap_vpk_heap_clear(void);

/** @brief Pin file in heap (load from flash if needed). Pointer valid until unpin. */
bool vmap_vpk_heap_pin(vmap_cell_id_t id, const char * path,
    const uint8_t ** data, uint32_t * size);
void vmap_vpk_heap_unpin(vmap_cell_id_t id);
void vmap_vpk_heap_touch(vmap_cell_id_t id);
bool vmap_vpk_heap_has(vmap_cell_id_t id);

/** @brief Copy a byte range. Loads the vpk if it is not resident. */
bool vmap_vpk_heap_copy(vmap_cell_id_t id, const char * path,
    uint32_t file_off, void * dst, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_VPK_HEAP_H */
