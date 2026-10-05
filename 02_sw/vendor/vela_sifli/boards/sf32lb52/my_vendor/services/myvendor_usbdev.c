/**
 * @file myvendor_usbdev.c
 * @brief USB 控制器电源：arch 早初始化已 `arm_usbinitialize()`，此后由本文件接管。
 *
 * `ctl mtp off` 卸 MTP gadget，再 `arm_usbuninitialize()`：关 PHY、复位 USBC、
 * 停 USBC 时钟。不碰 BLE FS，也不拆 `mtp_simple` worker。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_usbdev.h"

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <syslog.h>

#ifdef CONFIG_USBMTP
#  include <nuttx/usb/mtp.h>
#endif

#if defined(CONFIG_USBDEV) || defined(CONFIG_SF32LB52_USBDEV)
extern void arm_usbinitialize(void);
extern void arm_usbuninitialize(void);

static bool g_hw_known;
static bool g_hw_up;
#ifdef CONFIG_USBMTP
static void *g_gadget;
#endif

/**
 * @brief 首次调用时认定 arch 已把 USBC 拉起来。
 */
static void assume_arch_usb(void)
{
  if (!g_hw_known)
    {
      g_hw_up = true;
      g_hw_known = true;
    }
}
#endif

int myvendor_usbdev_set(bool on)
{
#if defined(CONFIG_USBDEV) || defined(CONFIG_SF32LB52_USBDEV)
  assume_arch_usb();

  if (on)
    {
      if (!g_hw_up)
        {
          arm_usbinitialize();
          g_hw_up = true;
          syslog(LOG_INFO, "usbdev: controller on\n");
        }

#ifdef CONFIG_USBMTP
      if (g_gadget == NULL)
        {
          g_gadget = usbdev_mtp_initialize();
          if (g_gadget == NULL)
            {
              syslog(LOG_ERR, "usbdev: usbdev_mtp_initialize failed\n");
              return -ENODEV;
            }

          syslog(LOG_INFO, "usbdev: MTP gadget bound\n");
        }
#endif

      return 0;
    }

#ifdef CONFIG_USBMTP
  if (g_gadget != NULL)
    {
      usbdev_mtp_uninitialize(g_gadget);
      g_gadget = NULL;
      syslog(LOG_INFO, "usbdev: MTP gadget unbound\n");
    }
#endif

  if (g_hw_up)
    {
      arm_usbuninitialize();
      g_hw_up = false;
      syslog(LOG_INFO, "usbdev: controller off (PHY+USBC clk)\n");
    }

  return 0;
#else
  (void)on;
  return -ENOTSUP;
#endif
}

bool myvendor_usbdev_get(void)
{
#if defined(CONFIG_USBDEV) || defined(CONFIG_SF32LB52_USBDEV)
  assume_arch_usb();
  return g_hw_up;
#else
  return false;
#endif
}
