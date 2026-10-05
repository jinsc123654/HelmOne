/**
 * @file mtp_names.h
 * @brief PC 可见 PTP/MTP 字符串（GetDeviceInfo / GetStorageInfo / 根目录名）。
 *
 * Model/卷标为 CONFIG_MYVENDOR_PRODUCT_NAME + MAC 后缀（与 BLE 一致）。
 * USB iProduct 在 myvendor_identity.c 于 usbdev_mtp_initialize() 前写入。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_NAMES_H
#define MY_VENDOR_MTP_NAMES_H

#include <stddef.h>
#include <stdint.h>

#define MTP_NAME_MAX       64
#define MTP_STORAGE_PATH_MAX 128

/** @brief GetDeviceInfo / GetStorageInfo 用的名称槽。 */
enum mtp_name_id
{
  MTP_NAME_MTP_EXTENSION = 0, /**< DeviceInfo 扩展串。 */
  MTP_NAME_MANUFACTURER,
  MTP_NAME_MODEL,             /**< 型号 + catalog 根文件夹名。 */
  MTP_NAME_VERSION,
  MTP_NAME_SERIAL,
  MTP_NAME_STORAGE_DESC,      /**< GetStorageInfo 描述。 */
  MTP_NAME_VOLUME_LABEL,      /**< GetStorageInfo 卷标。 */
  MTP_NAME_COUNT
};

extern char g_mtp_names[MTP_NAME_COUNT][MTP_NAME_MAX];

/** 主存储挂载路径与 Windows 感知设备类型。 */
extern const char    g_mtp_storage_path[MTP_STORAGE_PATH_MAX];
extern const uint32_t g_mtp_perceived_type;

/** @brief 用 myvendor_identity 刷新 model/serial/卷标。 */
void mtp_names_apply_identity(void);

/** @brief 主存储挂载路径。 */
static inline const char *mtp_storage_path(void)
{
  return g_mtp_storage_path;
}

/** @brief 按 id 取名称字符串。 */
static inline const char *mtp_name(enum mtp_name_id id)
{
  if ((unsigned)id >= MTP_NAME_COUNT)
    {
      return "";
    }

  return g_mtp_names[id];
}

#endif /* MY_VENDOR_MTP_NAMES_H */
