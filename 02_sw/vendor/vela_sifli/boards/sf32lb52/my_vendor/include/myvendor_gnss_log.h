/**
 * @file myvendor_gnss_log.h
 * @brief GNSS 诊断日志的两级开关。
 *
 * 两级分开，是因为这两类行的用途完全不同：
 *
 * - `CONFIG_MYVENDOR_GNSS_LOG`（INFO）：**事件行** —— UART 重开、VCC 上电/静默、
 *   静止下电/唤醒、poll 超时串、星历灌入进度、首帧 PVT 原文。它们只在状态变化
 *   时出现，行数少、信息密度高，平时开着不碍事。
 * - `CONFIG_MYVENDOR_GNSS_TRACE`（DEBUG）：**稳态逐帧行** —— 1 Hz `pub`、每帧
 *   `pvt`、5 s `link`/`link cpu`/`link extra`、每 10 次 poll 超时的 `poll_to run`。
 *   健康时每行都一样，几十秒就把监控环冲满、把现场挤掉；只在"要对着这些数字看
 *   趋势"时打开。**默认不参与构建**（宏展开成空），打开后按 DEBUG 级打印 ——
 *   diag 环只收 WARN 及以上，所以它们不会进 /mnt/kv/diag。
 *
 * 为什么不是"只把级别改成 DEBUG"：本 port 上 `syslog()` 走 libc → `vsyslog()`
 * → `nx_vsyslog()`，**绕过 `g_syslog_mask`**（只有内核侧 `nx_syslog()` 查它），
 * 所以 DEBUG 行照样会打出来。要真正安静下来必须编译期开关。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MYVENDOR_GNSS_LOG_H
#define MYVENDOR_GNSS_LOG_H

#include <syslog.h>

#ifdef CONFIG_MYVENDOR_GNSS_LOG
#  define GNSS_LOG(fmt, ...) \
     syslog(LOG_INFO, "gnss: " fmt "\n", ##__VA_ARGS__)
#else
#  define GNSS_LOG(...) ((void)0)
#endif

#ifdef CONFIG_MYVENDOR_GNSS_TRACE
#  define GNSS_TRACE(fmt, ...) \
     syslog(LOG_DEBUG, "gnss: " fmt "\n", ##__VA_ARGS__)
#  define GNSS_TRACE_ON 1
#else
#  define GNSS_TRACE(...) ((void)0)
#  define GNSS_TRACE_ON 0
#endif

#endif
