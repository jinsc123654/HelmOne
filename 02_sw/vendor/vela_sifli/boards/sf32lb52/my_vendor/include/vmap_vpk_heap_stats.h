/**
 * @file vmap_vpk_heap_stats.h
 * @brief VPK cell heap usage for boardmem (lives inside BoardPSRAM).
 */

#ifndef VMAP_VPK_HEAP_STATS_H
#define VMAP_VPK_HEAP_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Query the dedicated cell-vpk PSRAM arena.
 *
 * @param arena_bytes     Total arena (CONFIG_MYVENDOR_VMAP_VPK_HEAP_KB).
 * @param occupied_bytes  Bytes held by resident vpk files (aligned blocks).
 * @param ncell           Number of resident cells.
 * @return true if the arena exists (allocated).
 */
bool vmap_vpk_heap_stats(size_t *arena_bytes, size_t *occupied_bytes,
    uint32_t *ncell);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_VPK_HEAP_STATS_H */
