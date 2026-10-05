/**
 * @file myvendor_identity.h
 * @brief 运行时产品身份：品名 + 公有 BD_ADDR 末两字节（大写十六进制），与 BLE 扫描名相同。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_IDENTITY_H
#define MYVENDOR_IDENTITY_H

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** USB MTP iProduct 可写缓冲长度（含 NUL）。 */
#define USBMTP_PRODUCTSTR_MAX  32
/** USB MTP iSerialNumber 可写缓冲长度（含 NUL）。 */
#define USBMTP_SERIALSTR_MAX   16

#if defined(CONFIG_USBMTP) && !defined(CONFIG_USBMTP_COMPOSITE)
/** 覆盖 usbdev/mtp.c 的 iProduct（板级抽换）。 */
extern char g_usbmtp_productstr[USBMTP_PRODUCTSTR_MAX];
/** 覆盖 usbdev/mtp.c 的 iSerialNumber。 */
extern char g_usbmtp_serialstr[USBMTP_SERIALSTR_MAX];
#endif

/**
 * @brief 产品显示名，如 "Helm One-A3F2"。USB/BT 初始化前也可调，结果有缓存。
 */
const char *myvendor_identity_name(void);

/**
 * @brief 6 字节 MAC 的 12 位大写十六进制。
 *
 * HCI 线上 byte0 为 LSB；打印顺序 byte5…byte0，与常见 `AA:BB:…` 去冒号后一致。
 * eFuse MAC 不可用时为空串。
 */
const char *myvendor_identity_serial(void);

/**
 * @brief 2SFBL `persist.boot.running`：COMPANION_SLOT_*（fw / main / factory）。
 */
uint8_t     myvendor_boot_slot(void);

/**
 * @brief sticky `persist.boot.target`；缺文件为 #COMPANION_SLOT_UNKNOWN。
 */
uint8_t     myvendor_boot_target(void);

/**
 * @brief 写 `persist.boot.target` 为 fw 或 main。
 * @param slot #COMPANION_SLOT_FW 或 #COMPANION_SLOT_MAIN。
 * @return 0 成功，负 errno。
 */
int         myvendor_boot_target_set(uint8_t slot);

/**
 * @brief 当前镜像是否为工厂固件（CONFIG_MYVENDOR_FACTORY_MODE）。
 *
 * 与启动槽无关：产品镜像即使 KV 写成 factory 也不走工厂页。
 */
bool        myvendor_is_factory(void);

/**
 * @brief 槽为 fw 时的 basename；否则空串。
 */
const char *myvendor_boot_fw_name(void);

/**
 * @brief 软件版本字符串。
 */
const char *myvendor_sw_version(void);

/**
 * @brief 固件构建时间，本地时区，精确到秒（YYYY-MM-DD HH:MM:SS）。
 */
const char *myvendor_build_date(void);

/**
 * @brief 硬件版本字符串。
 */
const char *myvendor_hw_version(void);

/**
 * @brief 2SFBL `persist.boot.version`；旧 bootloader 则为空串。
 */
const char *myvendor_boot_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_IDENTITY_H */
