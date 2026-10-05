/**
 * @file vmap_alloc.c
 * @brief vmap alloc 模块。
 */

#include "vmap_alloc.h"

#include "board_malloc.h"

/**
 * @brief 从 vmap PSRAM 池分配。
 * @param size 字节数。
 */
void * vmap_malloc(size_t size)
{
    return board_malloc_psram(size);
}

/**
 * @brief 释放 vmap_malloc 分配的内存。
 * @param ptr 指针；NULL 安全。
 */
void vmap_free(void * ptr)
{
    board_free_psram(ptr);
}
