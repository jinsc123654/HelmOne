/**
 * @file mtp_psram.c
 * @brief mtp_simple 用 PSRAM 尾部 bump 分配器。
 *
 * 大缓冲（catalog、I/O、scratch）仅从本 arena 分配，生命周期等于 mtp_simple 进程。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp_psram.h"
#include "mtp_features.h"

#include <nuttx/config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static size_t g_mtp_psram_used;
static bool   g_mtp_psram_ready;

/**
 * @brief 初始化 PSRAM arena。
 * @return 0 成功。
 */
int mtp_psram_init(void)
{
#if defined(CONFIG_BSP_USING_PSRAM) && defined(CONFIG_MYVENDOR_MTP_SIMPLE)
  g_mtp_psram_used  = 0;
  g_mtp_psram_ready = true;
#if MTP_FEAT_INFO
  syslog(LOG_INFO, "mtp_psram: arena 0x%08x size %u bytes\n",
         (unsigned)MTP_PSRAM_ARENA_BASE, (unsigned)MTP_PSRAM_RESERVE_SIZE);
#endif
  return 0;
#else
  g_mtp_psram_ready = false;
  return 0;
#endif
}

/**
 * @brief 从 arena bump 分配（无 free）。
 * @return 指针或 NULL。
 */
void *mtp_psram_malloc(size_t size)
{
#if defined(CONFIG_BSP_USING_PSRAM) && defined(CONFIG_MYVENDOR_MTP_SIMPLE)
  size_t aligned;
  void *ptr;

  if (!g_mtp_psram_ready || size == 0)
    {
      return NULL;
    }

  aligned = (size + 3u) & ~((size_t)3);
  if (g_mtp_psram_used + aligned > MTP_PSRAM_RESERVE_SIZE)
    {
      syslog(LOG_ERR, "mtp_psram: out of arena (need %u, left %u)\n",
             (unsigned)aligned,
             (unsigned)(MTP_PSRAM_RESERVE_SIZE - g_mtp_psram_used));
      return NULL;
    }

  ptr = (void *)(uintptr_t)(MTP_PSRAM_ARENA_BASE + g_mtp_psram_used);
  g_mtp_psram_used += aligned;
  return ptr;
#else
  return malloc(size);
#endif
}

/** @brief calloc 风格 bump 分配。 */
void *mtp_psram_calloc(size_t n, size_t size)
{
  size_t total;
  void *p;

  if (n != 0 && size > (size_t)-1 / n)
    {
      return NULL;
    }

  total = n * size;
  p     = mtp_psram_malloc(total);
  if (p != NULL)
    {
      memset(p, 0, total);
    }

  return p;
}

/** @brief 已分配字节数（boardmem 统计）。 */
size_t mtp_psram_used_bytes(void)
{
#if defined(CONFIG_BSP_USING_PSRAM) && defined(CONFIG_MYVENDOR_MTP_SIMPLE)
  if (!g_mtp_psram_ready)
    {
      return 0;
    }

  return g_mtp_psram_used;
#else
  return 0;
#endif
}
