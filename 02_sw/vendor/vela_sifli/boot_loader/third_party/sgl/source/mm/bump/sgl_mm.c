/* source/mm/bump/sgl_mm.c
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
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * WARNING: This memory allocator is not support free operation, if you free the memory, the memory will be corrupted.
 */

#include <stdint.h>
#include <sgl_mm.h>
#include <sgl_log.h>
#include <sgl_cfgfix.h>
#include <stdlib.h>
#include <string.h>


static sgl_mm_monitor_t mem = {
    .total_size = 0,
    .free_size = 0,
    .used_size = 0,
};

static void *bump_mem_start = NULL;
static size_t bump_mem_offset = 0;

/**
 * @brief  initialize memory pool
 * @param  mem_start  start address of memory pool
 * @param  len  length of memory pool
 */
void sgl_mm_init(void *mem_start, size_t len)
{
    bump_mem_start = mem_start;
    mem.total_size += len;
    mem.free_size = mem.total_size;
}


/**
 * @brief  add memory pool
 * @param  mem_start  start address of memory pool
 * @param  len  length of memory pool
 */
void sgl_mm_add_pool(void *mem_start, size_t len)
{
    SGL_UNUSED(mem_start);
    SGL_UNUSED(len);
    SGL_LOG_ERROR("sgl_mm_add_pool is not supported");
    SGL_ASSERT(0);
}


/**
 * @brief  memory alloc, the function is unsafe, you should ensure that 
 *         the requested size is smaller than the free size of memory
 * 
 * @param  size   request size of memory
 * 
 * @return point to request memory address
*/
void* sgl_malloc(size_t size)
{
    uint8_t *ptr = (uint8_t*)bump_mem_start;

    if (size == 0 || (bump_mem_offset + size) > mem.total_size) {
        return NULL;
    }

    ptr += bump_mem_offset;
    bump_mem_offset += size;
    mem.used_size += size;
    mem.free_size -= size;

    return (void*)ptr;
}


/**
 * @brief  memory realloc, the function is unsafe, you should ensure that 
 *         the requested size is smaller than the free size of memory
 * @param  p      the pointer of request size of memory
 * @param  size   request size of memory
 */
void* sgl_realloc(void *p, size_t size)
{
    SGL_UNUSED(p);
    SGL_UNUSED(size);
    SGL_LOG_ERROR("sgl_realloc is not supported");
    SGL_ASSERT(0);
    return NULL;
}


/**
 * @brief  memory free
 * 
 * @param  p  the pointer of request size of memory
 * 
 * @return none
*/
void sgl_free(void *p)
{
    SGL_UNUSED(p);
    SGL_LOG_ERROR("sgl_free is not supported");
    SGL_ASSERT(0);
}


sgl_mm_monitor_t sgl_mm_get_monitor(void)
{
    int integer = (mem.used_size * 100) / mem.total_size;
    int decimal = (mem.used_size * 10000) / mem.total_size - (integer * 100);
    mem.used_rate = integer << 8 | decimal;
    mem.free_size = mem.total_size - mem.used_size;

    return mem;
}
