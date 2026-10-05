/**
 * @file board_mtp.c
 * @brief 按需 MTP：USB 控制器与 MTP 类驱动由 mtp_responder 应用持有（LAZY 模式）。
 *
 * CONFIG_MTP_RESPONDER_LAZY 下，board bringup 不初始化 USB；mtp_responder 启动时
 * 才 arm_usbinitialize + usbdev_mtp_initialize，退出时释放。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#ifdef CONFIG_MTP_RESPONDER_LAZY

#include "mtp_board.h"
#include "mtp_simple/mtp_features.h"

#include <errno.h>
#include <stdbool.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/usb/mtp.h>

extern void arm_usbinitialize(void);
extern void arm_usbuninitialize(void);
extern void board_usb_lazy_enable(bool enable);

static void *g_mtp_usb_handle;
static pid_t g_mtp_pid;
static bool g_mtp_usb_hw_up;
static bool g_mtp_usb_drv_up;

/**
 * @brief MTP USB 驱动与 responder 是否都在运行。
 */
bool board_mtp_is_running(void)
{
  return g_mtp_usb_drv_up && g_mtp_pid > 0;
}

/** @brief 主机断开：卸载 MTP 类驱动。 */
void board_mtp_host_disconnect(void)
{
  if (g_mtp_usb_drv_up && g_mtp_usb_handle != NULL)
    {
      usbdev_mtp_uninitialize(g_mtp_usb_handle);
      g_mtp_usb_handle = NULL;
      g_mtp_usb_drv_up = false;
    }
}

/** @brief 断开 MTP 并关闭 USB 硬件。 */
void board_mtp_host_reset(void)
{
  board_mtp_host_disconnect();

  if (g_mtp_usb_hw_up)
    {
      arm_usbuninitialize();
      board_usb_lazy_enable(false);
      g_mtp_usb_hw_up = false;
    }
}

/**
 * @brief mtp_responder 启动：拉起 USB 与 MTP 驱动。
 * @param pid responder 进程 pid。
 */
void board_mtp_app_running(pid_t pid)
{
  if (g_mtp_usb_drv_up)
    {
      g_mtp_pid = pid;
      return;
    }

  if (!g_mtp_usb_hw_up)
    {
      board_usb_lazy_enable(true);
      arm_usbinitialize();
      g_mtp_usb_hw_up = true;
    }

  g_mtp_usb_handle = usbdev_mtp_initialize();
  if (g_mtp_usb_handle == NULL)
    {
      syslog(LOG_ERR, "board_mtp: usbdev_mtp_initialize failed\n");
      board_mtp_host_reset();
      return;
    }

  g_mtp_usb_drv_up = true;
  g_mtp_pid = pid;
#if MTP_FEAT_INFO
  syslog(LOG_INFO, "board_mtp: USB MTP up (pid=%ld)\n", (long)pid);
#endif
}

/** @brief mtp_responder 退出：释放 USB/MTP 资源。 */
void board_mtp_app_exited(void)
{
  pid_t pid = g_mtp_pid;

  board_mtp_host_reset();
  g_mtp_pid = 0;

  if (pid > 0)
    {
#if MTP_FEAT_INFO
      syslog(LOG_INFO, "board_mtp: resources released (pid=%ld)\n",
             (long)pid);
#endif
    }
}

#endif /* CONFIG_MTP_RESPONDER_LAZY */
