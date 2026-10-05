/**
 * @file mtp_board.h
 * @brief 按需 MTP 的板级钩子（CONFIG_MTP_RESPONDER_LAZY）。
 *
 * 未开启该 Kconfig 时提供空内联，调用方可无条件编译。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_BOARD_H
#define MY_VENDOR_MTP_BOARD_H

#include <nuttx/config.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef CONFIG_MTP_RESPONDER_LAZY

/**
 * @brief 允许或禁止 USB 控制器上电（lazy 模式）。
 */
void board_usb_lazy_enable(bool enable);

/**
 * @brief 主机侧硬复位：卸驱动并下电 USB。
 */
void board_mtp_host_reset(void);

/**
 * @brief 仅断开 MTP class，硬件可保持。
 */
void board_mtp_host_disconnect(void);

/**
 * @brief MTP 应用任务已起来。
 * @param pid worker pid。
 */
void board_mtp_app_running(pid_t pid);

/**
 * @brief MTP 应用任务已退出。
 */
void board_mtp_app_exited(void);

/**
 * @brief USB MTP 驱动与应用是否都在跑。
 */
bool board_mtp_is_running(void);

#else

static inline void board_usb_lazy_enable(bool enable)
{
  (void)enable;
}

static inline void board_mtp_host_reset(void) {}
static inline void board_mtp_host_disconnect(void) {}
static inline void board_mtp_app_running(pid_t pid) { (void)pid; }
static inline void board_mtp_app_exited(void) {}
static inline bool board_mtp_is_running(void) { return false; }

#endif

#endif /* MY_VENDOR_MTP_BOARD_H */
