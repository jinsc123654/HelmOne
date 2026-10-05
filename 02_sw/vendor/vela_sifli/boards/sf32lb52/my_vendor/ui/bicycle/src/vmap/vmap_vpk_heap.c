/**
 * @file vmap_vpk_heap.c
 * @brief First-fit PSRAM heap for whole cell vpk files (LRU unused eviction).
 */

#include "vmap_vpk_heap.h"

#include "vmap_config.h"
#include "board_malloc.h"
#include <myvendor_mtp_lfs.h>
#include "myvendor_watchdog.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#ifndef CONFIG_MYVENDOR_VMAP_VPK_HEAP_KB
#  define CONFIG_MYVENDOR_VMAP_VPK_HEAP_KB 1024
#endif

#define VPK_HEAP_BYTES ((size_t)CONFIG_MYVENDOR_VMAP_VPK_HEAP_KB * 1024u)
#define VPK_HEAP_ALIGN 8u
#define VPK_HEAP_MAX_BLOCKS 24
#define VPK_HEAP_MIN_SPLIT 32u
/* Match SD CMD18 max (8 x 512 B). FAT direct-I/O of a 32 KiB cluster into
 * PSRAM was aborting mid-file (stat size ok, fread short). Bounce in .bss. */
#define VPK_HEAP_IO_CHUNK  4096u
#define VPK_HEAP_IO_TRIES  3

static uint8_t g_vpk_bounce[VPK_HEAP_IO_CHUNK] __attribute__((aligned(32)));

static bool io_read_fd(int fd, void * dst, uint32_t len)
{
    uint8_t * p = (uint8_t *)dst;
    uint32_t got = 0;

    while (got < len) {
        uint32_t chunk = len - got;
        ssize_t n;

        if (chunk > VPK_HEAP_IO_CHUNK) {
            chunk = VPK_HEAP_IO_CHUNK;
        }
        n = read(fd, g_vpk_bounce, (size_t)chunk);
        if (n <= 0) {
            return false;
        }
        memcpy(p + got, g_vpk_bounce, (size_t)n);
        got += (uint32_t)n;
        myvendor_watchdog_busy_pump();
    }
    return true;
}

typedef struct {
    vmap_cell_id_t id;
    uint32_t off;
    uint32_t size;
    uint32_t file_sz;
    uint32_t lru;
    int refs;
} vpk_block_t;

static uint8_t * g_heap;
static size_t g_heap_bytes;
static vpk_block_t g_blk[VPK_HEAP_MAX_BLOCKS];
static uint32_t g_n;
static uint32_t g_clock;
static bool g_ready;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void init_unlocked(void)
{
    if (g_ready) {
        return;
    }
    g_heap_bytes = VPK_HEAP_BYTES;
    if (g_heap_bytes < 64u * 1024u) {
        return;
    }
    g_heap = (uint8_t *)board_malloc_psram(g_heap_bytes);
    if (!g_heap) {
        syslog(LOG_ERR, "[vmap] vpk heap alloc %u KiB failed\n",
            (unsigned)(g_heap_bytes / 1024u));
        return;
    }
    g_n = 1;
    g_blk[0].id = VMAP_CELL_NONE;
    g_blk[0].off = 0;
    g_blk[0].size = (uint32_t)g_heap_bytes;
    g_blk[0].file_sz = 0;
    g_blk[0].lru = 0;
    g_blk[0].refs = 0;
    g_clock = 0;
    g_ready = true;
    syslog(LOG_NOTICE, "[vmap] vpk heap %u KiB psram\n",
        (unsigned)(g_heap_bytes / 1024u));
}

static uint32_t align_up(uint32_t n)
{
    return (n + (VPK_HEAP_ALIGN - 1u)) & ~(VPK_HEAP_ALIGN - 1u);
}

static int find_id(vmap_cell_id_t id)
{
    uint32_t i;

    if (id == VMAP_CELL_NONE) {
        return -1;
    }
    for (i = 0; i < g_n; i++) {
        if (g_blk[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static void coalesce(void)
{
    uint32_t i = 0;

    while (i + 1u < g_n) {
        if (g_blk[i].id == VMAP_CELL_NONE
                && g_blk[i + 1u].id == VMAP_CELL_NONE) {
            g_blk[i].size += g_blk[i + 1u].size;
            memmove(&g_blk[i + 1u], &g_blk[i + 2u],
                (size_t)(g_n - i - 2u) * sizeof(g_blk[0]));
            g_n--;
        } else {
            i++;
        }
    }
}

static bool evict_one(void)
{
    int best = -1;
    uint32_t i;

    for (i = 0; i < g_n; i++) {
        if (g_blk[i].id == VMAP_CELL_NONE || g_blk[i].refs > 0) {
            continue;
        }
        if (best < 0 || g_blk[i].lru < g_blk[best].lru) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return false;
    }
    g_blk[best].id = VMAP_CELL_NONE;
    g_blk[best].refs = 0;
    g_blk[best].file_sz = 0;
    g_blk[best].lru = 0;
    coalesce();
    return true;
}

static int alloc_need(uint32_t need)
{
    uint32_t i;

    for (i = 0; i < g_n; i++) {
        uint32_t leftover;

        if (g_blk[i].id != VMAP_CELL_NONE || g_blk[i].size < need) {
            continue;
        }
        leftover = g_blk[i].size - need;
        if (leftover >= VPK_HEAP_MIN_SPLIT && g_n < VPK_HEAP_MAX_BLOCKS) {
            memmove(&g_blk[i + 2u], &g_blk[i + 1u],
                (size_t)(g_n - i - 1u) * sizeof(g_blk[0]));
            g_n++;
            g_blk[i + 1u].id = VMAP_CELL_NONE;
            g_blk[i + 1u].off = g_blk[i].off + need;
            g_blk[i + 1u].size = leftover;
            g_blk[i + 1u].file_sz = 0;
            g_blk[i + 1u].lru = 0;
            g_blk[i + 1u].refs = 0;
            g_blk[i].size = need;
        }
        return (int)i;
    }
    return -1;
}

static bool file_copy(const char * path, uint32_t off, void * dst, uint32_t len)
{
    int fd;
    bool ok;

    if (!path || !dst || len == 0) {
        return false;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    ok = lseek(fd, (off_t)off, SEEK_SET) == (off_t)off
        && io_read_fd(fd, dst, len);
    close(fd);
    return ok;
}

static bool load_into(int idx, const char * path, uint32_t file_sz)
{
    int try;
    int fd;
    bool ok = false;

    for (try = 0; try < VPK_HEAP_IO_TRIES; try++) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            syslog(LOG_WARNING, "[vmap] vpk open fail %s errno=%d",
                path, errno);
            continue;
        }
        ok = io_read_fd(fd, g_heap + g_blk[idx].off, file_sz);
        if (!ok) {
            syslog(LOG_WARNING, "[vmap] vpk read fail %s errno=%d try=%d",
                path, errno, try);
        }
        close(fd);
        if (ok) {
            return true;
        }
    }
    return false;
}

static bool ensure_locked(vmap_cell_id_t id, const char * path, int * out_idx)
{
    struct stat st;
    uint32_t file_sz;
    uint32_t need;
    int idx;

    idx = find_id(id);
    if (idx >= 0) {
        g_blk[idx].lru = ++g_clock;
        *out_idx = idx;
        return true;
    }
    if (!path || myvendor_mtp_lfs_quiesce()) {
        return false;
    }
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return false;
    }
    file_sz = (uint32_t)st.st_size;
    need = align_up(file_sz);
    if (need > g_heap_bytes || need == 0) {
        syslog(LOG_WARNING, "[vmap] vpk too big %u KiB (heap %u)",
            (unsigned)((file_sz + 1023u) / 1024u),
            (unsigned)(g_heap_bytes / 1024u));
        return false;
    }
    idx = alloc_need(need);
    while (idx < 0) {
        if (!evict_one()) {
            return false;
        }
        idx = alloc_need(need);
    }
    if (!load_into(idx, path, file_sz)) {
        syslog(LOG_WARNING, "[vmap] vpk load fail cell=%u sz=%u errno=%d",
            (unsigned)id, (unsigned)file_sz, errno);
        g_blk[idx].id = VMAP_CELL_NONE;
        g_blk[idx].refs = 0;
        g_blk[idx].file_sz = 0;
        coalesce();
        return false;
    }
    g_blk[idx].id = id;
    g_blk[idx].file_sz = file_sz;
    g_blk[idx].refs = 0;
    g_blk[idx].lru = ++g_clock;
    *out_idx = idx;
    syslog(LOG_NOTICE, "[vmap] vpk ram %u KiB cell=%u",
        (unsigned)((file_sz + 1023u) / 1024u), (unsigned)id);
    return true;
}

void vmap_vpk_heap_init(void)
{
    pthread_mutex_lock(&g_lock);
    init_unlocked();
    pthread_mutex_unlock(&g_lock);
}

void vmap_vpk_heap_clear(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_ready || !g_heap) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_n = 1;
    g_blk[0].id = VMAP_CELL_NONE;
    g_blk[0].off = 0;
    g_blk[0].size = (uint32_t)g_heap_bytes;
    g_blk[0].file_sz = 0;
    g_blk[0].lru = 0;
    g_blk[0].refs = 0;
    g_clock = 0;
    pthread_mutex_unlock(&g_lock);
}

bool vmap_vpk_heap_pin(vmap_cell_id_t id, const char * path,
    const uint8_t ** data, uint32_t * size)
{
    int idx;

    if (!data || !size || id == VMAP_CELL_NONE) {
        return false;
    }
    pthread_mutex_lock(&g_lock);
    init_unlocked();
    if (!g_ready || !ensure_locked(id, path, &idx)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    g_blk[idx].refs++;
    *data = g_heap + g_blk[idx].off;
    *size = g_blk[idx].file_sz;
    pthread_mutex_unlock(&g_lock);
    return true;
}

void vmap_vpk_heap_unpin(vmap_cell_id_t id)
{
    int idx;

    pthread_mutex_lock(&g_lock);
    idx = find_id(id);
    if (idx >= 0 && g_blk[idx].refs > 0) {
        g_blk[idx].refs--;
    }
    pthread_mutex_unlock(&g_lock);
}

void vmap_vpk_heap_touch(vmap_cell_id_t id)
{
    int idx;

    pthread_mutex_lock(&g_lock);
    idx = find_id(id);
    if (idx >= 0) {
        g_blk[idx].lru = ++g_clock;
    }
    pthread_mutex_unlock(&g_lock);
}

bool vmap_vpk_heap_has(vmap_cell_id_t id)
{
    bool ok;

    pthread_mutex_lock(&g_lock);
    init_unlocked();
    ok = g_ready && find_id(id) >= 0;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool vmap_vpk_heap_stats(size_t *arena_bytes, size_t *occupied_bytes,
    uint32_t *ncell)
{
    size_t occupied = 0;
    uint32_t cells = 0;
    uint32_t i;

    pthread_mutex_lock(&g_lock);
    if (!g_ready || !g_heap) {
        pthread_mutex_unlock(&g_lock);
        if (arena_bytes) {
            *arena_bytes = VPK_HEAP_BYTES;
        }
        if (occupied_bytes) {
            *occupied_bytes = 0;
        }
        if (ncell) {
            *ncell = 0;
        }
        return true;
    }
    for (i = 0; i < g_n; i++) {
        if (g_blk[i].id == VMAP_CELL_NONE) {
            continue;
        }
        occupied += g_blk[i].size;
        cells++;
    }
    if (arena_bytes) {
        *arena_bytes = g_heap_bytes;
    }
    if (occupied_bytes) {
        *occupied_bytes = occupied;
    }
    if (ncell) {
        *ncell = cells;
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool vmap_vpk_heap_copy(vmap_cell_id_t id, const char * path,
    uint32_t file_off, void * dst, uint32_t len)
{
    int idx;

    if (!dst || len == 0) {
        return false;
    }
    if (id == VMAP_CELL_NONE) {
        return file_copy(path, file_off, dst, len);
    }
    pthread_mutex_lock(&g_lock);
    init_unlocked();
    if (g_ready && ensure_locked(id, path, &idx)
            && file_off + len <= g_blk[idx].file_sz) {
        memcpy(dst, g_heap + g_blk[idx].off + file_off, len);
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    pthread_mutex_unlock(&g_lock);
    return false;
}
