/* source/mm/lwmem/sgl_mm.c
 *
 * MIT License
 *
 * Copyright(c) 2023-present All contributors of SGL
 * Document reference link: https://sgl-docs.readthedocs.io
 */

#include <stdint.h>
#include <sgl_mm.h>
#include "lwmem.h"
#include <sgl_log.h>
#include <sgl_cfgfix.h>

static size_t g_heap_total = 0;
extern lwmem_t lwmem_default;

#define LWMEM_BLOCK_ALLOC_MARK      ((void *)1U)
#define LWMEM_SIZE_ALLOC_FLAG       (((size_t)1) << (sizeof(size_t) * 8 - 1))

static size_t lwmem_get_real_free(void)
{
    size_t free = 0;
    lwmem_block_t *blk;

    if (g_heap_total == 0)
        return 0;

    blk = lwmem_default.start_block.next;
    while (blk != NULL && blk != lwmem_default.end_block) {
        if (!(blk->size & LWMEM_SIZE_ALLOC_FLAG)) {
            free += blk->size;
        }
        blk = blk->next;
    }
    return free;
}

void sgl_mm_init(void *mem_start, size_t len)
{
    lwmem_region_t regs[] = {
        {mem_start, len},
        {NULL, 0}
    };
    lwmem_assignmem(regs);
    g_heap_total = len;
}

void sgl_mm_add_pool(void *mem_start, size_t len)
{
    lwmem_region_t regs[] = {
        {mem_start, len},
        {NULL, 0}
    };
    lwmem_assignmem(regs);
    g_heap_total += len;
}

void* sgl_malloc(size_t size)
{
    void *p = lwmem_malloc(size);
    if (!p)
        SGL_LOG_ERROR("out of memory");
    return p;
}

void* sgl_realloc(void *p, size_t size)
{
    void *np = lwmem_realloc(p, size);
    if (!np)
        SGL_LOG_ERROR("out of memory");
    return np;
}

void sgl_free(void *p)
{
    if (p) {
        lwmem_free(p);
    }
}

sgl_mm_monitor_t sgl_mm_get_monitor(void)
{
    sgl_mm_monitor_t mon = {0};
    mon.total_size = g_heap_total;
    uint32_t total_rate = 0;
    int int_part, dec_part;

    size_t free_bytes = lwmem_get_real_free();
    mon.free_size = free_bytes;
    mon.used_size = g_heap_total - free_bytes;

    if (g_heap_total > 0) {
        total_rate = (uint32_t)mon.used_size * 10000U / (uint32_t)g_heap_total;
        int_part  = total_rate / 100;
        dec_part  = total_rate % 100;
        mon.used_rate = (int_part << 8) | (dec_part & 0xFF);
    }
    return mon;
}
