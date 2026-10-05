/**
 * @file mtp_scratch.h
 * @brief mtp_simple 用 PSRAM scratch 池（单线程，一次只跑一个 handler）。
 *
 * 槽位：path[0] 上传目标；path[1..3] 临时路径；name/cmd/data/utf16/walk 栈。
 * 递归深度由 MTP_WALK_MAX_DEPTH 限制，不再吃任务栈。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_SCRATCH_H
#define MY_VENDOR_MTP_SCRATCH_H

#include "mtp_simple.h"

#include <dirent.h>
#include <stdint.h>

#define MTP_SCRATCH_NPATH   4
#define MTP_SCRATCH_NNAME   2
#define MTP_CMD_BUF_SIZE    128
#define PTP_STR_MAX_CHARS   255

/** 迭代 remove/copy/rescan 的最大目录嵌套（PSRAM，非栈）。 */
#define MTP_WALK_MAX_DEPTH  24

/** @brief 目录遍历 walk 栈槽。 */
struct mtp_walk_slot
{
  char     path[MTP_MAX_PATH];
  char     path2[MTP_MAX_PATH];  /**< copy 目的或 scan 临时子路径 */
  uint32_t parent;               /**< catalog scan：父 handle */
  uint8_t  phase;                /**< remove/copy 后序阶段 */
  DIR     *dir;                  /**< catalog scan：挂起的 opendir */
};

/** @brief scratch 池布局。 */
struct mtp_scratch
{
  char      path[MTP_SCRATCH_NPATH][MTP_MAX_PATH];
  char      name[MTP_SCRATCH_NNAME][MTP_MAX_NAME];
  uint8_t   cmd[MTP_CMD_BUF_SIZE];
  uint8_t   data512[512];
  uint8_t   data256[256];
  uint8_t   data128[128];
  uint8_t   data64[64];
  uint16_t  utf16[PTP_STR_MAX_CHARS];
  struct mtp_walk_slot walk[MTP_WALK_MAX_DEPTH];
};

extern struct mtp_scratch *g_mtp_scratch;

#define MTP_UPLOAD_PATH       (g_mtp_scratch->path[0])
#define MTP_SCRATCH_PATH(n)   (g_mtp_scratch->path[(n)])
#define MTP_SCRATCH_NAME(n)   (g_mtp_scratch->name[(n)])
#define MTP_SCRATCH_CMD       (g_mtp_scratch->cmd)
#define MTP_SCRATCH_D512      (g_mtp_scratch->data512)
#define MTP_SCRATCH_D256      (g_mtp_scratch->data256)
#define MTP_SCRATCH_D128      (g_mtp_scratch->data128)
#define MTP_SCRATCH_D64       (g_mtp_scratch->data64)
#define MTP_SCRATCH_UTF16     (g_mtp_scratch->utf16)
#define MTP_WALK_STACK        (g_mtp_scratch->walk)

#endif /* MY_VENDOR_MTP_SCRATCH_H */
