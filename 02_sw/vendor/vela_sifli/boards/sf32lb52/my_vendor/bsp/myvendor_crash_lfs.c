/**
 * @file myvendor_crash_lfs.c
 * @brief 崩溃路径专用 LittleFS（与 boot / NuttX KV 同几何）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#if defined(CONFIG_MYVENDOR_COREDUMP) && CONFIG_MYVENDOR_COREDUMP

#include "myvendor_crash_lfs.h"

#include "lfs_util.c"
#include "lfs.c"

#endif
