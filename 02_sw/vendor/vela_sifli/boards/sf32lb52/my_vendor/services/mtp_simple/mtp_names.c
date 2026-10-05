/**
 * @file mtp_names.c
 * @brief PC 可见 MTP/PTP 名称表与存储路径。
 *
 * Model/卷标与 BLE 相同：CONFIG_MYVENDOR_PRODUCT_NAME + eFuse MAC 后缀（myvendor_identity）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp_names.h"

#include "myvendor_identity.h"

#include <nuttx/config.h>

#include <string.h>

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#ifndef CONFIG_MYVENDOR_PRODUCT_VERSION
#  define CONFIG_MYVENDOR_PRODUCT_VERSION "1.4.2"
#endif

#ifndef CONFIG_MYVENDOR_MANUFACTURER
#  define CONFIG_MYVENDOR_MANUFACTURER "Helm"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_MTP_EXTENSION
#  define CONFIG_MYVENDOR_MTP_SIMPLE_MTP_EXTENSION "microsoft.com: 1.0"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_SERIAL
#  define CONFIG_MYVENDOR_MTP_SIMPLE_SERIAL "00000001"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_DESC
#  define CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_DESC "LittleFS"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_VOLUME_LABEL
#  define CONFIG_MYVENDOR_MTP_SIMPLE_VOLUME_LABEL "LFS"
#endif

#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH
#  define CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH "/mnt/lfs/mtp"
#endif

/**
 * Windows PerceivedDeviceType 十六进制对照：
 * 0x0 通用，0x1 相机，0x2 媒体播放器，0x3 手机（默认），0x4 摄像机，0x5 PDA，0x6 录音机。
 */
#ifndef CONFIG_MYVENDOR_MTP_SIMPLE_PERCEIVED_TYPE
#  define CONFIG_MYVENDOR_MTP_SIMPLE_PERCEIVED_TYPE 0x0
#endif

char g_mtp_names[MTP_NAME_COUNT][MTP_NAME_MAX] =
{
  [MTP_NAME_MTP_EXTENSION] = CONFIG_MYVENDOR_MTP_SIMPLE_MTP_EXTENSION,
  [MTP_NAME_MANUFACTURER]  = CONFIG_MYVENDOR_MANUFACTURER,
  [MTP_NAME_MODEL]         = CONFIG_MYVENDOR_PRODUCT_NAME,
  [MTP_NAME_VERSION]       = CONFIG_MYVENDOR_PRODUCT_VERSION,
  [MTP_NAME_SERIAL]        = CONFIG_MYVENDOR_MTP_SIMPLE_SERIAL,
  [MTP_NAME_STORAGE_DESC]  = CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_DESC,
  [MTP_NAME_VOLUME_LABEL]  = CONFIG_MYVENDOR_MTP_SIMPLE_VOLUME_LABEL,
};

/** @brief 用 myvendor_identity 刷新 model/serial/卷标。 */
void mtp_names_apply_identity(void)
{
  const char *id = myvendor_identity_name();
  const char *serial = myvendor_identity_serial();

  strncpy(g_mtp_names[MTP_NAME_MODEL], id, MTP_NAME_MAX - 1);
  g_mtp_names[MTP_NAME_MODEL][MTP_NAME_MAX - 1] = '\0';

  if (serial[0] != '\0')
    {
      strncpy(g_mtp_names[MTP_NAME_SERIAL], serial, MTP_NAME_MAX - 1);
      g_mtp_names[MTP_NAME_SERIAL][MTP_NAME_MAX - 1] = '\0';
    }

  if (strcmp(g_mtp_names[MTP_NAME_STORAGE_DESC],
             CONFIG_MYVENDOR_PRODUCT_NAME) == 0)
    {
      strncpy(g_mtp_names[MTP_NAME_STORAGE_DESC], id, MTP_NAME_MAX - 1);
      g_mtp_names[MTP_NAME_STORAGE_DESC][MTP_NAME_MAX - 1] = '\0';
    }

  if (strcmp(g_mtp_names[MTP_NAME_VOLUME_LABEL],
             CONFIG_MYVENDOR_PRODUCT_NAME) == 0)
    {
      strncpy(g_mtp_names[MTP_NAME_VOLUME_LABEL], id, MTP_NAME_MAX - 1);
      g_mtp_names[MTP_NAME_VOLUME_LABEL][MTP_NAME_MAX - 1] = '\0';
    }
}

/* Edit literals here to override Kconfig without changing defconfig. */
const char g_mtp_storage_path[MTP_STORAGE_PATH_MAX] =
  CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH;

const uint32_t g_mtp_perceived_type =
  CONFIG_MYVENDOR_MTP_SIMPLE_PERCEIVED_TYPE;
