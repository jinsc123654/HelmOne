/**
 * @file board_malloc.c
 * @brief 板级 SRAM / PSRAM 显式内存分配。
 *
 * PSRAM 三块：Umem kumm、BoardPSRAM 独立堆、MTP bump（见 mtp_psram.c）。
 * board_malloc_psram() 供 gpx_port 等大块分配；不计入 NSH `free`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_malloc.h"
#include "board_psram_layout.h"

#include <nuttx/config.h>
#include <nuttx/fs/procfs.h>
#include <nuttx/mm/mm.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#if (CONFIG_MM_REGIONS > 1) || \
    (defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0)

#include "mm_heap/mm.h"

#endif

#if CONFIG_MM_REGIONS > 1

/**
 * Umem 按 region 统计（供 boardmem 显示 Umem SRAM / Umem PSRAM kumm）。
 *
 * NuttX mallinfo() 仅全堆合计；CONFIG_MM_REGIONS&gt;1 时需 walk 各 region。
 */

/** @brief mallinfo 单节点累加回调。 */
static void board_umem_mallinfo_node(FAR struct mm_allocnode_s *node,
                                     FAR void *arg)
{
  FAR struct mallinfo *info = arg;
  size_t nodesize = MM_SIZEOF_NODE(node);

  if (MM_NODE_IS_ALLOC(node))
    {
      info->aordblks++;
      info->uordblks += nodesize;
    }
  else
    {
      info->ordblks++;
      info->fordblks += nodesize;
      if (node->size > (size_t)info->mxordblk)
        {
          info->mxordblk = nodesize;
        }
    }
}

/**
 * @brief 查询 Umem 指定 region 的 mallinfo。
 * @param region 0=片内 SRAM，1=PSRAM kumm。
 * @param[out] info 输出；arena=mm_regionsize[region]。
 * @return true 成功，false 参数或 region 无效。
 */
bool board_umem_region_mallinfo(int region, struct mallinfo *info)
{
  FAR struct mm_heap_s *heap;
  FAR struct mm_allocnode_s *node;
  size_t nodesize;

  if (info == NULL || region < 0)
    {
      return false;
    }

  heap = g_mmheap;
  if (heap == NULL || region >= heap->mm_nregions)
    {
      return false;
    }

  memset(info, 0, sizeof(*info));
  mm_free_delaylist(heap);
  nxrmutex_lock(&heap->mm_lock);

  for (node = heap->mm_heapstart[region];
       node < heap->mm_heapend[region];
       node = (FAR struct mm_allocnode_s *)((FAR char *)node + nodesize))
    {
      nodesize = MM_SIZEOF_NODE(node);
      board_umem_mallinfo_node(node, info);
    }

  /* mm_foreach 对 heapend 哨兵节点也会调用一次 handler */
  board_umem_mallinfo_node(heap->mm_heapend[region], info);
  nxrmutex_unlock(&heap->mm_lock);

  info->arena   = heap->mm_regionsize[region];
  info->usmblks = info->uordblks;
  return true;
}

/**
 * @brief 返回 Umem region 尾部 probe_len 字节地址。
 * @return true 成功。
 */
bool board_umem_region_tail_addr(int region, size_t probe_len,
                                 uintptr_t *addr)
{
  FAR struct mm_heap_s *heap;
  size_t region_size;

  if (addr == NULL || probe_len == 0)
    {
      return false;
    }

  heap = g_mmheap;
  if (heap == NULL || region < 0 || region >= heap->mm_nregions)
    {
      return false;
    }

  region_size = heap->mm_regionsize[region];
  if (region_size < probe_len + 2 * MM_SIZEOF_ALLOCNODE)
    {
      return false;
    }

  *addr = (uintptr_t)heap->mm_heapend[region] - probe_len;
  return true;
}

#else

#include "mm_heap/mm.h"

/* 单 region 板型：region 0 复用全局 mallinfo() */

/**
 * @brief 单 region 板型 Umem mallinfo。
 * @return true 且 region==0 时填充 *info。
 */
bool board_umem_region_mallinfo(int region, struct mallinfo *info)
{
  if (info == NULL || region != 0)
    {
      return false;
    }

  *info = mallinfo();
  return true;
}

bool board_umem_region_tail_addr(int region, size_t probe_len,
                                 uintptr_t *addr)
{
  FAR struct mm_heap_s *heap;
  size_t region_size;

  if (addr == NULL || probe_len == 0 || region != 0)
    {
      return false;
    }

  heap = g_mmheap;
  if (heap == NULL || heap->mm_nregions < 1)
    {
      return false;
    }

  region_size = heap->mm_regionsize[0];
  if (region_size < probe_len + 2 * MM_SIZEOF_ALLOCNODE)
    {
      return false;
    }

  *addr = (uintptr_t)heap->mm_heapend[0] - probe_len;
  return true;
}

#endif

/****************************************************************************
 * BoardPSRAM 独立堆（board_psram_pool_base() 上 mm_initialize）
 ****************************************************************************/

static pthread_mutex_t g_board_psram_init_lock = PTHREAD_MUTEX_INITIALIZER; /**< BoardPSRAM 堆 init 锁。 */
static struct mm_heap_s *g_board_psram_heap;   /**< BoardPSRAM mm 堆指针。 */
static bool g_board_psram_heap_ready;          /**< 堆是否已成功 mm_initialize。 */
/** ⚠ 一次性诊断：MPU 的 RBAR/RLAR/MAIR 只打一遍（2026-09-27）。 */
static bool g_mpu_dumped;

/**
 * @brief BoardPSRAM 池尾部地址（堆未就绪时用 layout 推算）。
 * @return true 成功。
 */
bool board_psram_pool_tail_addr(size_t probe_len, uintptr_t *addr)
{
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (addr == NULL || probe_len == 0)
    {
      return false;
    }

  board_psram_heap_init();

  if (g_board_psram_heap_ready && g_board_psram_heap != NULL)
    {
      if (g_board_psram_heap->mm_regionsize[0] <
          probe_len + 2 * MM_SIZEOF_ALLOCNODE)
        {
          return false;
        }

      *addr = (uintptr_t)g_board_psram_heap->mm_heapend[0] - probe_len;
      return true;
    }

  if (BOARD_PSRAM_POOL_BYTES < probe_len)
    {
      return false;
    }

  *addr = board_psram_pool_base() + BOARD_PSRAM_POOL_BYTES - probe_len;
  return true;

#else

  (void)probe_len;
  (void)addr;
  return false;

#endif
}

/**
 * @brief 初始化 BoardPSRAM 独立堆（幂等）。
 * @return 0 成功，-ENOMEM/-ENOTSUP 失败。
 */
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

/** @brief 检测 BoardPSRAM 是否与 Umem region 重叠。 */
static bool board_psram_overlaps_umem(uintptr_t pool, size_t bytes)
{
  FAR struct mm_heap_s *heap = g_mmheap;
  uintptr_t pool_end = pool + bytes;
  int nregions;
  int r;

  if (heap == NULL || bytes == 0)
    {
      return false;
    }

#if CONFIG_MM_REGIONS > 1
  nregions = heap->mm_nregions;
#else
  nregions = 1;
#endif

  for (r = 0; r < nregions; r++)
    {
      uintptr_t lo = (uintptr_t)heap->mm_heapstart[r];
      uintptr_t hi = (uintptr_t)heap->mm_heapend[r];

      syslog(LOG_INFO, "board_malloc: umem r%d %p-%p\n",
             r, (void *)lo, (void *)hi);

      if (pool < hi && pool_end > lo)
        {
          return true;
        }
    }

  return false;
}

#endif

/** @brief 初始化 BoardPSRAM 独立堆（幂等，见文件头说明）。 */
int board_psram_heap_init(void)
{
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  pthread_mutex_lock(&g_board_psram_init_lock);
  if (!g_board_psram_heap_ready)
    {
      uintptr_t pool = board_psram_pool_base();

      syslog(LOG_INFO, "board_malloc: BoardPSRAM %p +%u KiB\n",
             (void *)pool, (unsigned)(BOARD_PSRAM_POOL_BYTES / 1024u));

      if (board_psram_overlaps_umem(pool, BOARD_PSRAM_POOL_BYTES))
        {
          syslog(LOG_ERR,
                 "board_malloc: BoardPSRAM overlaps Umem, skip "
                 "(BLE/UI will fall back to malloc)\n");
        }
      else
        {
          /* ⚠ 2026-09-27：池子里要**挖掉画布 arena** 那块（见 `board_psram_layout.h`
           * 的 `BOARD_PSRAM_CANVAS_ARENA_*`）：它是给 vmap 常驻画布专用的、1 MiB 对齐的
           * 1 MiB，板级 MPU 只给它开 write-back（解决"每次像素 store 都打到 PSRAM"）。
           * `mm_heap` 支持多 region ⇒ 把池子拆成 arena 之前/之后两块加进去：
           * 不用改别的区布局，也保证堆**永远不会**把 arena 分出去。 */
          const size_t lo = board_psram_pool_lo_bytes();
          const size_t hi = board_psram_pool_hi_bytes();

          g_board_psram_heap = mm_initialize("BoardPSRAM", (FAR void *)pool, lo);
          if (g_board_psram_heap != NULL && hi > 0)
            {
              mm_addregion(g_board_psram_heap,
                (FAR void *)(pool + lo + BOARD_PSRAM_CANVAS_ARENA_BYTES), hi);
            }

          g_board_psram_heap_ready = (g_board_psram_heap != NULL);

          syslog(LOG_INFO,
                 "board_malloc: canvas arena %p +%u KiB (heap %u+%u KiB)\n",
                 (void *)board_psram_canvas_arena_base(),
                 (unsigned)(BOARD_PSRAM_CANVAS_ARENA_BYTES / 1024u),
                 (unsigned)(lo / 1024u), (unsigned)(hi / 1024u));

#if defined(CONFIG_FS_PROCFS) && !defined(CONFIG_FS_PROCFS_EXCLUDE_MEMINFO)
          /* NSH `free` walks every procfs heap.  Keep BoardPSRAM out of
           * that list so a PSRAM-side glitch cannot assert nsh_main.
           * Use `boardmem` for this pool. */

          if (g_board_psram_heap != NULL &&
              g_board_psram_heap->mm_procfs != NULL)
            {
              procfs_unregister_meminfo(g_board_psram_heap->mm_procfs);
              g_board_psram_heap->mm_procfs = NULL;
            }
#endif
        }
    }

  pthread_mutex_unlock(&g_board_psram_init_lock);
  return g_board_psram_heap_ready ? 0 : -ENOMEM;

#else

  return -ENOTSUP;

#endif
}

/** @brief BoardPSRAM 堆是否已就绪。 */
bool board_psram_heap_ready(void)
{
  return g_board_psram_heap_ready;
}

/** @brief 返回 BoardPSRAM 堆 mallinfo。 */
struct mallinfo board_psram_mallinfo(void)
{
  struct mallinfo info;

  memset(&info, 0, sizeof(info));
  if (board_psram_heap_init() == 0 && g_board_psram_heap != NULL)
    {
      info = mm_mallinfo(g_board_psram_heap);
    }

  return info;
}

/****************************************************************************
 * 板级分配 / 释放 API
 ****************************************************************************/

/** @brief 从 Umem SRAM（malloc）分配。 */
void *board_malloc_sram(size_t size)
{
  if (size == 0)
    {
      return NULL;
    }

  return malloc(size);
}

/**
 * @brief 优先 BoardPSRAM 分配，失败或未就绪则回退 SRAM。
 * @return 指针或 NULL。
 */
void *board_malloc_psram(size_t size)
{
  void *ptr;

  if (size == 0)
    {
      return NULL;
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (board_psram_heap_init() != 0 || g_board_psram_heap == NULL)
    {
      return board_malloc_sram(size);
    }

  ptr = mm_malloc(g_board_psram_heap, size);
  if (ptr != NULL)
    {
      return ptr;
    }

#endif

  return board_malloc_sram(size);
}

/** @brief PSRAM 堆内 realloc，否则标准 realloc。 */
void *board_realloc_psram(void *ptr, size_t size)
{
  if (size == 0)
    {
      board_mem_free(ptr);
      return NULL;
    }

  if (ptr == NULL)
    {
      return board_malloc_psram(size);
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (board_psram_heap_init() == 0 && g_board_psram_heap != NULL &&
      mm_heapmember(g_board_psram_heap, ptr))
    {
      return mm_realloc(g_board_psram_heap, ptr, size);
    }

#endif

  return realloc(ptr, size);
}

/** @brief 释放 SRAM 指针（free）。 */
void board_free_sram(void *ptr)
{
  if (ptr != NULL)
    {
      free(ptr);
    }
}

/** @brief 释放 PSRAM 池指针，非池内则走 board_free_sram。 */
void board_free_psram(void *ptr)
{
  if (ptr == NULL)
    {
      return;
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (g_board_psram_heap_ready &&
      g_board_psram_heap != NULL &&
      mm_heapmember(g_board_psram_heap, ptr))
    {
      mm_free(g_board_psram_heap, ptr);
      return;
    }

#endif

  board_free_sram(ptr);
}

/** @brief 自动判别 PSRAM 池或 SRAM 并释放。 */
void board_mem_free(void *ptr)
{
  if (ptr == NULL)
    {
      return;
    }

#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (g_board_psram_heap_ready &&
      g_board_psram_heap != NULL &&
      mm_heapmember(g_board_psram_heap, ptr))
    {
      board_free_psram(ptr);
      return;
    }

#endif

  board_free_sram(ptr);
}

/** @brief 判断指针是否在 BoardPSRAM 堆内。 */
bool board_ptr_in_psram_pool(const void *ptr)
{
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  if (ptr != NULL && g_board_psram_heap_ready && g_board_psram_heap != NULL) {
    return mm_heapmember(g_board_psram_heap, ptr);
  }

#endif

  return false;
}

/** @brief 画布专属 arena（见头文件；板级 MPU 只给它开 write-back）。
 *
 *  @details 除了返回地址，这里还做**两条自检**（错了就返回 NULL，让上层回退慢路径）：
 *           ① arena 必须落在池子里、且 1 MiB 对齐（MPU region 的硬件要求）；
 *           ② 池子必须真的把它挖掉了（`board_psram_pool_lo/hi_bytes()` 拼起来
 *              等于池子减去 arena）—— 否则 heap 会把它分出去，两边打架。
 *           地址与 `mpu_config()` 用的是**同一组宏**，所以两边天然同源。 */
void *board_psram_canvas_arena(size_t *bytes)
{
#if defined(CONFIG_BSP_USING_PSRAM) && BOARD_PSRAM_POOL_KB > 0

  uintptr_t base = board_psram_canvas_arena_base();
  uintptr_t pool = board_psram_pool_base();
  size_t lo = board_psram_pool_lo_bytes();
  size_t hi = board_psram_pool_hi_bytes();

  if (bytes != NULL) {
    *bytes = BOARD_PSRAM_CANVAS_ARENA_BYTES;
  }

  /* 一次性诊断（2026-09-27）：把 MPU 的**实际寄存器值**打出来 —— 上面两条自检只看宏，
   * 看不出"硬件里到底是什么"。上板读回来的 RBAR/RLAR/MAIR 直接照出了 attribute index
   * 与 region 落位（顺带证明这台 MPU 有 12 个 region，TYPE=0x00000C00）。
   * 现状：MPU region 表**保持出厂那份没动** —— 试过两次给 arena 单加 region，两次都
   * 引导循环（机制未明），已回退。PSRAM 的缓存属性改走**全局**开关
   * `CONFIG_MYVENDOR_PSRAM_CACHE_WB`（产品默认已开 ⇒ write-back），不再动 region 表。
   * ⚠ **别拿 `[vperf] cache` 的 irange/drange 当"缓存覆不覆盖 PSRAM"的证据**：那是
   * **缓存性能计数器**（官方 `bf0_hal_cache.h` 原文 "Enable cache profiling"），只选给
   * 哪个 MPI 计数。核内 I/D 缓存是**开着**的（`mpu_config()` 尾部 + `sifli_start.c` 注释）。
   * 证据与两次实验见 `docs/psram_cache_wb_audit.md` + `zcode/analysis/MPU_WB_ATTEMPT.md`。
   * 别删这段：下次再想动 MPU / 缓存，先让这几个寄存器值说话。 */
  if (!g_mpu_dumped) {
    unsigned i;
    /* ARMv8-M MPU 寄存器（裸地址；本编译单元没有 CMSIS core 头，不引结构体）：
     * CTRL 0xE000ED94 / RNR 0xE000ED98 / RBAR 0xE000ED9C / RLAR 0xE000EDA0 /
     * MAIR0 0xE000EDC0 / MAIR1 0xE000EDC4。 */
    volatile uint32_t * const mpu_rnr  = (volatile uint32_t *)0xe000ed98u;
    volatile uint32_t * const mpu_rbar = (volatile uint32_t *)0xe000ed9cu;
    volatile uint32_t * const mpu_rlar = (volatile uint32_t *)0xe000eda0u;

    g_mpu_dumped = true;
    syslog(LOG_WARNING, "mpu: type=%08x ctrl=%08x mair0=%08x mair1=%08x",
           (unsigned)*(volatile uint32_t *)0xe000ed90u,
           (unsigned)*(volatile uint32_t *)0xe000ed94u,
           (unsigned)*(volatile uint32_t *)0xe000edc0u,
           (unsigned)*(volatile uint32_t *)0xe000edc4u);

    for (i = 0; i < 12u; i++) {
      *mpu_rnr = i;
      syslog(LOG_WARNING, "mpu: r%u rbar=%08x rlar=%08x", i,
             (unsigned)*mpu_rbar, (unsigned)*mpu_rlar);
    }
  }

  if (base < pool || base + BOARD_PSRAM_CANVAS_ARENA_BYTES > pool + BOARD_PSRAM_POOL_BYTES) {
    syslog(LOG_ERR, "board_malloc: canvas arena out of pool, disabled\n");
    return NULL;
  }

  if ((base & (BOARD_PSRAM_CANVAS_ARENA_ALIGN - 1u)) != 0u) {
    syslog(LOG_ERR, "board_malloc: canvas arena misaligned %p, disabled\n",
           (void *)base);
    return NULL;
  }

  if (lo + hi + BOARD_PSRAM_CANVAS_ARENA_BYTES != BOARD_PSRAM_POOL_BYTES) {
    syslog(LOG_ERR, "board_malloc: canvas arena not carved out (%u+%u+%u != %u)\n",
           (unsigned)lo, (unsigned)hi,
           (unsigned)BOARD_PSRAM_CANVAS_ARENA_BYTES,
           (unsigned)BOARD_PSRAM_POOL_BYTES);
    return NULL;
  }

  return (void *)base;

#else

  if (bytes != NULL) {
    *bytes = 0;
  }

  return NULL;

#endif
}
