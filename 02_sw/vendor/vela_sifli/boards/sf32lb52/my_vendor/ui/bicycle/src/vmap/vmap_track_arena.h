/**
 * @file vmap_track_arena.h
 * @brief 固定 BoardPSRAM arena：内存轨迹点（显示叠加层）。
 */

#ifndef VMAP_TRACK_ARENA_H
#define VMAP_TRACK_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化轨迹 arena。
 * @return 成功/有效则为 true。
 */
int  vmap_track_arena_init(void);
/**
 * @brief 关闭轨迹 arena。
 */
void vmap_track_arena_shutdown(void);

/**
 * @brief 从轨迹 arena 分配。
 * @param size 字节数。
 */
void * vmap_track_arena_alloc(size_t size);
/**
 * @brief 重置分配指针（不释放底层池）。
 */
void   vmap_track_arena_reset_alloc(void);

/**
 * @brief arena 总字节。
 * @return 请求的值。
 */
size_t vmap_track_arena_total_bytes(void);
/**
 * @brief 已用字节。
 * @return 请求的值。
 */
size_t vmap_track_arena_used_bytes(void);
/**
 * @brief 剩余字节。
 * @return 请求的值。
 */
size_t vmap_track_arena_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_TRACK_ARENA_H */
