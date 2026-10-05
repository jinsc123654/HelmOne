/**
 * @file custom_mem_map.h
 * @brief 与 boot_loader/include 同内容的内存地图，供 NuttX 编译。
 *
 * 改 ptab 后请同步两份。通过 `USING_PARTITION_TABLE` 引入 ptab.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CUSTOM_MEM_MAP__
#define __CUSTOM_MEM_MAP__

#define USING_PARTITION_TABLE  /**< 使用分区表宏，而非手写地址。 */

#ifdef USING_PARTITION_TABLE
#  include "ptab.h"
#endif

#endif /* __CUSTOM_MEM_MAP__ */
