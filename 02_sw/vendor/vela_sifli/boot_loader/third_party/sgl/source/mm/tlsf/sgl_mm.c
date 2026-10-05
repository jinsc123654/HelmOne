/* source/mm/tlsf/sgl_mm.c
 *
 * MIT License
 *
 * Copyright(c) 2023-present All contributors of SGL
 * Document reference link: https://sgl-docs.readthedocs.io
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "tlsf.h"
#include <stdint.h>
#include <sgl_mm.h>
#include <sgl_log.h>
#include <sgl_cfgfix.h>

static tlsf_t mem_tlsf;
static size_t g_total_pool_size = 0;

typedef struct {
    size_t free_bytes;
} tlsf_walk_ctx_t;

static void tlsf_walk_callback(void* ptr, size_t size, int used, void* user)
{
    SGL_UNUSED(ptr);
    tlsf_walk_ctx_t* ctx = (tlsf_walk_ctx_t*)user;
    if (!used) {
        ctx->free_bytes += size;
    }
}

/**
 * @brief  initialize memory pool
 */
void sgl_mm_init(void *mem_start, size_t len)
{
    mem_tlsf = tlsf_create_with_pool(mem_start, len);
    g_total_pool_size = len;
}

/**
 * @brief  add memory pool
 */
void sgl_mm_add_pool(void *mem_start, size_t len)
{
    tlsf_add_pool(mem_tlsf, mem_start, len);
    g_total_pool_size += len;
}

void* sgl_malloc(size_t size)
{
    void *ret = tlsf_malloc(mem_tlsf, size);
    if(ret == NULL) {
        SGL_LOG_ERROR("out of memory");
    }
    return ret;
}

void* sgl_realloc(void *p, size_t size)
{
    void *ret = tlsf_realloc(mem_tlsf, p, size);
    if(ret == NULL) {
        SGL_LOG_ERROR("out of memory");
    }
    return ret;
}

void sgl_free(void *p)
{
    tlsf_free(mem_tlsf, p);
}

sgl_mm_monitor_t sgl_mm_get_monitor(void)
{
    static sgl_mm_monitor_t monitor;
    tlsf_walk_ctx_t ctx = {0};
    int integer, decimal;

    if (mem_tlsf == NULL || g_total_pool_size == 0) {
        return monitor;
    }

    tlsf_walk_pool(tlsf_get_pool(mem_tlsf), tlsf_walk_callback, &ctx);

    monitor.total_size = g_total_pool_size;
    monitor.free_size = ctx.free_bytes;
    monitor.used_size = g_total_pool_size - ctx.free_bytes;

    integer = (monitor.used_size * 100) / monitor.total_size;
    decimal = ((monitor.used_size * 10000) / monitor.total_size) % 100;
    monitor.used_rate = (integer << 8) | (decimal & 0xFF);

    return monitor;
}
