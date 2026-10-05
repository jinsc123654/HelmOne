/**
 * @file myvendor_mtp.h
 * @brief MTP 后台 worker（mtp_simple）与自行车 UI 用的状态。
 *
 * 生命周期：
 * - 应用调用一次 myvendor_mtp_init() 与一次 myvendor_mtp_transfer_begin()。
 * - worker 自己探 USB、跑协议。
 * - UI 只切到 UsbTransfer 显示状态，不按插拔启停 worker。
 * - myvendor_mtp_lfs_quiesce() 为真时地图/存储不得碰 LittleFS。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_H
#define MY_VENDOR_MTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYVENDOR_MTP_EP_OUT_PATH "/dev/mtp/ep2"  /**< MTP OUT 端点字符设备。 */

/**
 * @brief 主机 USB / MTP 阶段（mtp_simple 探测与 poll）。
 */
typedef enum
{
  MYVENDOR_MTP_PLUG_OFF = 0,  /**< 未配置 MTP（未插或仅充电、无 ep2）。 */
  MYVENDOR_MTP_PLUG_ENUM,     /**< 主机已选 MTP，端点在，传输未开始。 */
  MYVENDOR_MTP_PLUG_ACTIVE,   /**< 协议 poll 中；LittleFS 由 mtp_simple 占用。 */
} myvendor_mtp_plug_t;

/**
 * @brief MTP 链路与 worker 快照。
 */
typedef struct
{
  myvendor_mtp_plug_t plug;  /**< 插拔/枚举阶段。 */
  bool session_open;         /**< MTP session 已打开。 */
  bool ep_present;           /**< `/dev/mtp/ep2` 存在。 */
  bool worker_running;       /**< 后台任务仍在。 */
  bool transfer_pending;     /**< 已 transfer_begin，尚未 ACTIVE。 */
  bool transfer_active;      /**< 协议 poll 正在跑（ACTIVE）。 */
  bool lfs_quiesce;          /**< true：自行车 / vmap 不得碰 LFS。 */
} myvendor_mtp_status_t;

#define MYVENDOR_MTP_ACTIVITY_NAME_LEN 48  /**< 活动目标/详情字符串长度。 */

/**
 * @brief 当前 MTP 操作类型（给 UI 进度条）。
 */
typedef enum
{
  MYVENDOR_MTP_OP_IDLE = 0,  /**< 空闲。 */
  MYVENDOR_MTP_OP_UPLOAD,    /**< 主机 → 设备（SendObject）。 */
  MYVENDOR_MTP_OP_DOWNLOAD,  /**< 设备 → 主机（GetObject）。 */
  MYVENDOR_MTP_OP_MKDIR,     /**< 建目录。 */
  MYVENDOR_MTP_OP_DELETE,    /**< 删除。 */
  MYVENDOR_MTP_OP_RENAME,    /**< 重命名。 */
  MYVENDOR_MTP_OP_MOVE,      /**< 移动。 */
  MYVENDOR_MTP_OP_COPY,      /**< 复制。 */
} myvendor_mtp_op_t;

/**
 * @brief 正在进行的 MTP 操作（文件名、字节进度）。
 */
typedef struct
{
  myvendor_mtp_op_t op;  /**< 操作类型。 */
  bool active;           /**< 是否有未结束的操作。 */
  char target[MYVENDOR_MTP_ACTIVITY_NAME_LEN];  /**< 主目标名。 */
  char detail[MYVENDOR_MTP_ACTIVITY_NAME_LEN];  /**< 如重命名目的名。 */
  uint64_t total_bytes;  /**< 总字节；未知则为 0。 */
  uint64_t done_bytes;   /**< 已完成字节。 */
  uint32_t seq;          /**< 每次新操作递增，UI 用来检测换文件。 */
} myvendor_mtp_activity_t;

/**
 * @brief 启动后台 mtp_simple worker（幂等）。
 * @return 0 成功，负值为错误码。
 */
int myvendor_mtp_init(void);

/**
 * @brief 请求 shutdown，等到 worker 退出后再允许再次 init。
 */
void myvendor_mtp_deinit(void);

/**
 * @brief 自行车（重新）启动时清 owner 侧 hold/传输标志。
 */
void myvendor_mtp_owner_reset(void);

/**
 * @brief 关机前放开传输/LFS（不停 worker）。
 */
void myvendor_mtp_prepare_poweroff(void);

/**
 * @brief 允许 worker 跑协议（init 后调用一次）。
 * @return 0 已接受；-EINVAL 主机还不到 ENUM+（worker 会重试）。
 */
int myvendor_mtp_transfer_begin(void);

/**
 * @brief 旧接口：只停 owner 侧 poll 请求；worker 仍自己探测。
 */
void myvendor_mtp_transfer_end(void);

/**
 * @brief 可选：UI 准备期间提前占用 LFS（计数 +1）。
 *
 * @param who 取用者名字（字符串字面量，如 "gnss" / "companion" / "test"）。
 *
 * @details
 * hold 是**计数式**的，本身没有 owner 字段：泄漏时只知道"有人没还"，
 * 不知道是谁。这里把名字记进一个 4 格的环形记录，
 * `myvendor_mtp_lfs_quiesce_why()` 与残留 hold 的告警会把它打出来 ——
 * 现场直接指名，不用再靠推。
 */
void myvendor_mtp_lfs_hold(const char *who);

/**
 * @brief 与 #myvendor_mtp_lfs_hold 配对（计数 -1）。
 *
 * @param who 取用者名字，与 hold 时一致（只用于诊断：多还 / 回收后晚到时指名）。
 *
 * @note 计数溢出/多还不会变成负数：`hold==0` 时只记一次日志。
 *       超过 30 s 还没归还、且没有会话/传输的 hold 会被
 *       `myvendor_mtp_lfs_quiesce()` **在源头回收**（并发一行
 *       `leaked lfs_hold reclaimed`，附最近取用记录），所以晚到的 release
 *       会走到这条路径上，不代表配对一定写错了。
 */
void myvendor_mtp_lfs_release(const char *who);

/**
 * @brief 自行车 / vmap 是否必须推迟 LittleFS I/O。
 */
bool myvendor_mtp_lfs_quiesce(void);

/**
 * @brief 设备 UI 是否停在 MTP 传输页 —— `myvendor_mtp_lfs_quiesce()` 为真的
 *        两个成因里"**确实与 MTP 有关**"的那一个。
 *
 * @details quiesce 为真有两个成因：① 本函数（UI 停在传输页）；② 别人拿着有效的
 *          LFS hold（`transfer_backend` 的手机 BLE 文件传输、GNSS 星历注入都是），
 *          后者与 MTP 毫无关系。调用方要**如实**描述"在等什么"时必须靠本函数区分
 *          —— 否则骑行时（手机在传文件）会把"等传输让位"误报成"正在关闭 MTP"。
 *
 * @return true 表示 UI 正停在传输页。
 */
bool myvendor_mtp_page_active(void);

/**
 * @brief 把 #myvendor_mtp_lfs_quiesce 为真的**具体成因**格式化出来。
 *
 * @details
 * quiesce 是四个条件或起来的：`g_transfer_active || g_lfs_hold > 0 ||
 * g_plug >= MYVENDOR_MTP_PLUG_ENUM || g_session`。只说 "mtp busy" 无法判断是
 * 真在传输、还是计数泄漏、还是"仅插着 USB 充电"就把 LFS 全冻住了 —— 现场
 * 就出现过"用户只是插着充电，星历注入却被判定 mtp busy 而跳过"。
 *
 * 供其它模块的诊断日志使用（不参与任何判定）。与 mtp_owner_info 不同，
 * 本函数不受 MTP_FEAT_INFO 影响 —— 那个宏在本仓没有定义，所有 mtp_owner_info
 * 都被编译成空，plug 跳变和 lfs_hold 计数因此从来没打印过。
 *
 * @param buf 输出缓冲。
 * @param n 缓冲长度。
 */
void myvendor_mtp_lfs_quiesce_why(char *buf, size_t n);

/**
 * @brief 复制链路快照。
 * @param[out] status 输出；NULL 则 -EINVAL。
 * @return 0 成功。
 */
int myvendor_mtp_get_status(myvendor_mtp_status_t *status);

/**
 * @brief 复制当前活动快照。
 * @param[out] activity 输出；NULL 则 -EINVAL。
 * @return 0 成功。
 */
int myvendor_mtp_get_activity(myvendor_mtp_activity_t *activity);

/**
 * @brief USB 主机是否在线（枚举或 ACTIVE）。
 */
bool myvendor_mtp_host_present(void);

/**
 * @brief 主机是否已选 MTP 配置。
 */
bool myvendor_mtp_host_is_mtp(void);

/** 见 myvendor_mtp.c：只有 ACTIVE（真在跑 MTP 会话）才算忙。 */
bool myvendor_mtp_session_active(void);

/**
 * @brief 把状态打到 syslog，供 NSH 诊断。
 */
void myvendor_mtp_log_status(void);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_MTP_H */
