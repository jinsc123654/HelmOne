/* source/mm/other/sgl_mm.c
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
 * @file sgl_mm.c
 * @brief SGL Memory Management - Default (weak) implementations
 * 
 * @warning DO NOT MODIFY THIS FILE!
 * 
 * This file provides default weak implementations of the memory management
 * functions. These functions use the standard C library (malloc/free/realloc).
 * 
 * To customize memory management for your platform:
 * 1. Create your own implementation file in your project
 * 2. Define the same functions WITHOUT the weak attribute
 * 3. Your implementation will automatically override these defaults
 * 
 * Functions available for override:
 *   - void* sgl_mm_alloc(size_t size)
 *   - void  sgl_mm_free(void *ptr)
 *   - void* sgl_mm_realloc(void *ptr, size_t size)
 * 
 * Example (in your application code):
 * @code
 *   void sgl_mm_init(void)
 *   {
 *       my_custom_mm_init();
 *   }
 * 
 *   void* sgl_mm_alloc(size_t size)
 *   {
 *       return my_custom_allocator(size);
 *   }
 *   
 *   void sgl_mm_free(void *ptr)
 *   {
 *       my_custom_deallocator(ptr);
 *   }
 * 
 *   void sgl_mm_realloc(void *ptr, size_t size)
 *   {
 *       return my_custom_reallocator(ptr, size);
 *   }
 * 
 *   sgl_mm_monitor_t sgl_mm_get_monitor(void)
 *   {
 *       return sgl_mm_get_monitor();
 *   }
 * @endcode
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


/**
 * @brief  initialize memory pool
 * @param  mem_start  start address of memory pool
 * @param  len  length of memory pool
 */
void sgl_mm_init(void *mem_start, size_t len)
{
    mem.total_size += len;
}


/**
 * @brief  add memory pool
 * @param  mem_start  start address of memory pool
 * @param  len  length of memory pool
 */
void sgl_mm_add_pool(void *mem_start, size_t len)
{
    
    mem.total_size += len;
}


/**
 * @brief  memory alloc, the function is unsafe, you should ensure that 
 *         the requested size is smaller than the free size of memory
 * 
 * @param  size   request size of memory
 * 
 * @return point to request memory address
*/
sgl_weak_fn void* sgl_malloc(size_t size)
{
    void *p = malloc(size);
    memset(p, 0, size);
    return p;
}


/**
 * @brief  memory realloc, the function is unsafe, you should ensure that 
 *         the requested size is smaller than the free size of memory
 * @param  p      the pointer of request size of memory
 * @param  size   request size of memory
 */
sgl_weak_fn void* sgl_realloc(void *p, size_t size)
{
    return realloc(p, size);
}


/**
 * @brief  memory free
 * 
 * @param  p  the pointer of request size of memory
 * 
 * @return none
*/
sgl_weak_fn void sgl_free(void *p)
{
    //mem.used_size -= ;
    free(p);
}

/**
 * @brief  memory monitor info
 * @total_size: total size of memory
 * @free_size: free size of memory
 * @used_size: used size of memory
 * @used_rate: used rate of memory:
 *             |  8 bit  |  8 bit |          
 *             |   int   |   dec  |
 */
sgl_mm_monitor_t sgl_mm_get_monitor(void)
{
    int integer = (mem.used_size * 100) / mem.total_size;
    int decimal = (mem.used_size * 10000) / mem.total_size - (integer * 100);
    mem.used_rate = integer << 8 | decimal;
    mem.free_size = mem.total_size - mem.used_size;

    return mem;
}
