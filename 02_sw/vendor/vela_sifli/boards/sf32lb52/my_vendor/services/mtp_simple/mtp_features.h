/**
 * @file mtp_features.h
 * @brief mtp_simple 编译期特性裁剪；仅编辑本文件开关可选 MTP opcode（非 Kconfig）。
 *
 * 资源：SRAM-R 常驻 .bss；SRAM-S 单次 handler；PSRAM 为 mtp_psram 尾部 arena；
 * Flash 为 .text/.rodata。大缓冲与 walk 栈均在 PSRAM，与下方 flag 无关。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_FEATURES_H
#define MY_VENDOR_MTP_FEATURES_H

/* --- 诊断 -------------------------------------------------------- */

/* INFO syslog（mtp_info / myvendor_mtp: / 启动 arena 行）。0=关，调用点保留。
 * ERR/WARNING 恒开。调试上传时改 1。 */
#define MTP_FEAT_INFO                   0

/* Flash +0.5K；不影响协议。VERBOSE 时镜像 /dev/console。 */
#define MTP_FEAT_VERBOSE                0

/* --- 对象元数据 -------------------------------------------------- */

/* GetObjectPropList：文件名 + 大小（Ubuntu gvfs 多选拷贝需要）。
 * 不报 DateCreated/DateModified：LittleFS 没有可靠 POSIX mtime。 */
#define MTP_FEAT_OBJECT_PROPLIST        1

/* --- 读路径 ------------------------------------------------------ */

/* Windows 就地打开；共用 PSRAM g_io_buf。 */
#define MTP_FEAT_GET_PARTIAL_OBJECT     1

/* 空缩略图数据集，不解码图像。 */
#define MTP_FEAT_GET_THUMB              1

/* --- 写 / 文件系统（LittleFS） ----------------------------------- */

/* 资源管理器“新建文件夹”（Association + mkdir）。 */
#define MTP_FEAT_FOLDERS                1

/* 递归 DeleteObject 文件夹；与 COPY/FORMAT 共用 remove_tree。 */
#define MTP_FEAT_FOLDER_DELETE          1

/* MoveObject 拖放。 */
#define MTP_FEAT_MOVE_OBJECT            1

/* CopyObject 复制目录树。 */
#define MTP_FEAT_COPY_OBJECT            1

/* SetObjectPropValue 重命名。 */
#define MTP_FEAT_SET_OBJECT_PROP        1

/* FormatStore 清空存储路径。 */
#define MTP_FEAT_FORMAT_STORE           1

/* --- 主机兼容桩 -------------------------------------------------- */

/* SetDevicePropValue 处理器（默认拒绝写）。 */
#define MTP_FEAT_SET_DEVICE_PROP        0

/* 空 Get/SetObjectReferences。 */
#define MTP_FEAT_OBJECT_REFERENCES      1

/* InitiateCapture / TerminateCapture → OP_NOT_SUPPORTED。 */
#define MTP_FEAT_CAPTURE                1

/* DeviceInfo 与事件里多报 ObjectInfoChanged 等。 */
#define MTP_FEAT_EXTRA_EVENTS           1

/* 无数据阶段的 UpdateObject。 */
#define MTP_FEAT_UPDATE_OBJECT          1

/* --- Catalog / 浏览（LittleFS 多小文件） ------------------------- */

/*
 * 懒 catalog（推荐）：OpenSession 只列存储根；GetObjectHandles(parent) 才
 * opendir 一层。列出父目录会驱逐其子树缓存。避免 USB 连接时扫数百 map 瓦片。
 */
#define MTP_FEAT_LAZY_CATALOG           1

/*
 * 会话 catalog 缓存：文件夹 sync 一次后 RAM 服务，直到 catalog_dirty。
 */
#define MTP_FEAT_CATALOG_SESSION_CACHE  1

/* 遗留节流（MTP_FEAT_CATALOG_SESSION_CACHE 下未用）。 */
#define MTP_CATALOG_SYNC_THROTTLE_MS    3000

/* 本 LittleFS 上 statvfs 会 walk 所有块；GetStorageInfo 用 MTD 几何，勿重开。 */
#define MTP_STORAGE_STATVFS_THROTTLE_MS 5000

/* USB hangup 后 stale /dev/mtp/ep2 短暂忽略探测。 */
#define MTP_HOST_DISCONNECT_COOLDOWN_SEC  2

/* 主机链路探测：poll 超时无 revents => 非 live host。 */
#define MTP_HOST_PROBE_MS                 500

/* bulk EAGAIN 重试切片（检测拔线）。 */
#define MTP_IO_POLL_MS                    500

/* DATA 阶段后可选 OUT ZLP 等待（64 字节对齐 payload）。 */
#define MTP_ZLP_DRAIN_MS                  250

/* ENUM 后等自行车切 UsbTransfer 再 LINK_UP freeze。 */
#define MTP_UI_HANDOFF_MS                 600

/* OpenSession 全树扫描（遗留；LFS 100+ 文件时慢）。 */
#define MTP_FEAT_FULL_CATALOG_SCAN      0

#if MTP_FEAT_LAZY_CATALOG && MTP_FEAT_FULL_CATALOG_SCAN
#  error "Pick either MTP_FEAT_LAZY_CATALOG or MTP_FEAT_FULL_CATALOG_SCAN"
#endif

/*
 * 可选：MTP 浏览隐藏某个顶层目录（0=全显示，如 map）。
 * ENABLE=1 且 DIR 为 basename 时隐藏应用资产树。
 */
#define MTP_CATALOG_SKIP_ENABLE         0
#define MTP_CATALOG_SKIP_DIR            "map"

/* ----- GetDeviceInfo opcode 列表推导（勿手改） ------------------- */

#if MTP_FEAT_GET_PARTIAL_OBJECT
#  define MTP_OP_GETPARTIALOBJECT       PTP_OPCODE_GETPARTIALOBJECT,
#else
#  define MTP_OP_GETPARTIALOBJECT
#endif

#if MTP_FEAT_GET_THUMB
#  define MTP_OP_GETTHUMB               PTP_OPCODE_GETTHUMB,
#else
#  define MTP_OP_GETTHUMB
#endif

#if MTP_FEAT_MOVE_OBJECT
#  define MTP_OP_MOVEOBJECT             PTP_OPCODE_MOVEOBJECT,
#else
#  define MTP_OP_MOVEOBJECT
#endif

#if MTP_FEAT_COPY_OBJECT
#  define MTP_OP_COPYOBJECT             PTP_OPCODE_COPYOBJECT,
#else
#  define MTP_OP_COPYOBJECT
#endif

#if MTP_FEAT_FORMAT_STORE
#  define MTP_OP_FORMATSTORE            PTP_OPCODE_FORMATSTORE,
#else
#  define MTP_OP_FORMATSTORE
#endif

#if MTP_FEAT_CAPTURE
#  define MTP_OP_CAPTURE                PTP_OPCODE_INITIATECAPTURE, \
                                        PTP_OPCODE_TERMINATECAPTURE,
#else
#  define MTP_OP_CAPTURE
#endif

#if MTP_FEAT_SET_DEVICE_PROP
#  define MTP_OP_SETDEVICEPROP          PTP_OPCODE_SETDEVICEPROPVALUE,
#else
#  define MTP_OP_SETDEVICEPROP
#endif

#if MTP_FEAT_OBJECT_PROPLIST
#  define MTP_OP_OBJPROPS               MTP_OPCODE_GETOBJECTPROPSUPPORTED, \
                                        MTP_OPCODE_GETOBJECTPROPDESC,     \
                                        MTP_OPCODE_GETOBJECTPROPVALUE,    \
                                        MTP_OPCODE_GETOBJECTPROPLIST,
#else
#  define MTP_OP_OBJPROPS
#endif

#if MTP_FEAT_SET_OBJECT_PROP
#  define MTP_OP_SETOBJPROP             MTP_OPCODE_SETOBJECTPROPVALUE,
#else
#  define MTP_OP_SETOBJPROP
#endif

#if MTP_FEAT_OBJECT_REFERENCES
#  define MTP_OP_OBJREFS                MTP_OPCODE_GETOBJECTREFERENCES, \
                                        MTP_OPCODE_SETOBJECTREFERENCES,
#else
#  define MTP_OP_OBJREFS
#endif

#if MTP_FEAT_UPDATE_OBJECT
#  define MTP_OP_UPDATEOBJECT           MTP_OPCODE_UPDATEOBJECT,
#else
#  define MTP_OP_UPDATEOBJECT
#endif

#endif /* MY_VENDOR_MTP_FEATURES_H */
