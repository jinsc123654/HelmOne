/**
 * @file vmap_track_arena.c
 * @brief vmap track_arena 模块。
 */

#include "vmap_track_arena.h"

#include "bicycle_config.h"
#include "board_malloc.h"
#include <nuttx/config.h>
#include <string.h>

#ifndef CONFIG_MYVENDOR_VMAP_TRACK_ARENA_KB
#  define CONFIG_MYVENDOR_VMAP_TRACK_ARENA_KB 1024
#endif

#define VMAP_TRACK_ARENA_BYTES_KCFG \
    ((size_t)CONFIG_MYVENDOR_VMAP_TRACK_ARENA_KB * 1024u)

static size_t track_arena_target_bytes(void)
{
    const size_t need = bicycle_config_track_arena_bytes();
    const size_t kcfg = VMAP_TRACK_ARENA_BYTES_KCFG;

    /* Kconfig is a hard ceiling when smaller than bicycle_config need (~130 km @ 512 KiB). */
    if (kcfg > 0 && kcfg < need) {
        return kcfg;
    }
    return need > kcfg ? need : kcfg;
}

static uint8_t * g_arena;
static size_t g_arena_bytes;
static size_t g_arena_off;

static size_t track_arena_align_up(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}

/**
 * @brief 初始化轨迹 arena。
 * @return 成功/有效则为 true。
 */
int vmap_track_arena_init(void)
{
    if (g_arena != NULL) {
        return 0;
    }

    g_arena_bytes = track_arena_target_bytes();
    if (g_arena_bytes < 4096u) {
        return -1;
    }

    g_arena = (uint8_t *)board_malloc_psram(g_arena_bytes);
    if (!g_arena) {
        g_arena_bytes = 0;
        return -1;
    }

    g_arena_off = 0;
    memset(g_arena, 0, g_arena_bytes);
    return 0;
}

/**
 * @brief 关闭轨迹 arena。
 */
void vmap_track_arena_shutdown(void)
{
    if (g_arena) {
        board_mem_free(g_arena);
        g_arena = NULL;
    }

    g_arena_bytes = 0;
    g_arena_off = 0;
}

/**
 * @brief 从轨迹 arena 分配。
 * @param size 字节数。
 */
void * vmap_track_arena_alloc(size_t size)
{
    size_t aligned;
    void * ptr;

    if (!g_arena || size == 0) {
        return NULL;
    }

    aligned = track_arena_align_up(size);
    if (g_arena_off + aligned > g_arena_bytes) {
        return NULL;
    }

    ptr = g_arena + g_arena_off;
    g_arena_off += aligned;
    return ptr;
}

/**
 * @brief 重置分配指针（不释放底层池）。
 */
void vmap_track_arena_reset_alloc(void)
{
    g_arena_off = 0;
}

/**
 * @brief arena 总字节。
 * @return 请求的值。
 */
size_t vmap_track_arena_total_bytes(void)
{
    return g_arena_bytes;
}

/**
 * @brief 已用字节。
 * @return 请求的值。
 */
size_t vmap_track_arena_used_bytes(void)
{
    return g_arena_off;
}

/**
 * @brief 剩余字节。
 * @return 请求的值。
 */
size_t vmap_track_arena_free_bytes(void)
{
    if (!g_arena || g_arena_off >= g_arena_bytes) {
        return 0;
    }

    return g_arena_bytes - g_arena_off;
}
