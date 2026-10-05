/**
 * @file myvendor_gpx.h
 * @brief 产品 GPX 目录：外部导入与本机记录，都在 MTP 用户卷下。
 *
 * PC 打开 USB 存储后看到：
 *
 *   import/   外部导入（USB / App 放入，码表导航读取）
 *   record/   本机记录（结束保存后写出，PC 拷走）
 *   navpts/   App 下发的坐标点记录（每条一个 .tsv）
 *
 * 绝对路径固定在 `/mnt/lfs/mtp`，与产品 `CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH`
 * 默认值一致。不要写到 `/mnt/lfs/Track`（那是旧演示目录，产品 MTP 不可见）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_GPX_H
#define MYVENDOR_GPX_H

#ifdef __cplusplus
extern "C" {
#endif

/** MTP 用户卷根（产品固件对主机只暴露这一层）。 */
#define MYVENDOR_GPX_MTP_ROOT     "/mnt/lfs/mtp"

/** 外部导入的 GPX。 */
#define MYVENDOR_GPX_IMPORT_DIR   MYVENDOR_GPX_MTP_ROOT "/import"

/** 本机记录的 GPX。 */
#define MYVENDOR_GPX_RECORD_DIR   MYVENDOR_GPX_MTP_ROOT "/record"

/** App 下发的坐标点记录（一条路线一个 TSV）。 */
#define MYVENDOR_NAVPTS_DIR       MYVENDOR_GPX_MTP_ROOT "/navpts"

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_GPX_H */
