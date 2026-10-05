/**
 * @file vmap_alloc.h
 * @brief vmap 专用 PSRAM 分配器（画布、瓦片缓存、临时缓冲）。
 */

#ifndef VMAP_ALLOC_H
#define VMAP_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 vmap PSRAM 池分配。
 * @param size 字节数。
 */
void * vmap_malloc(size_t size);
/**
 * @brief 释放 vmap_malloc 分配的内存。
 * @param ptr 指针；NULL 安全。
 */
void   vmap_free(void * ptr);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_ALLOC_H */
