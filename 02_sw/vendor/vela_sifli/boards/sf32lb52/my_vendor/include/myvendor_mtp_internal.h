/**
 * @file myvendor_mtp_internal.h
 * @brief 仅供 mtp_simple worker 使用的钩子，应用代码不要包含。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_INTERNAL_H
#define MY_VENDOR_MTP_INTERNAL_H

#include "myvendor_mtp_lfs.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief worker 更新链路状态（映射到对外 plug）。
 */
void myvendor_mtp_lfs_set_link(myvendor_mtp_link_state_t state);

/**
 * @brief worker 更新 MTP session 开/关。
 */
void myvendor_mtp_lfs_set_session(bool active);

/**
 * @brief worker 等待 owner 的 transfer_begin()。
 * @return false 应放弃等待（shutdown / 断开）。
 */
bool myvendor_mtp_worker_wait_begin(void);

/**
 * @brief poll 循环是否应退出（owner 结束传输或 shutdown）。
 */
bool myvendor_mtp_worker_poll_abort(void);

/**
 * @brief 标记协议 poll 是否 ACTIVE。
 */
void myvendor_mtp_worker_set_active(bool active);

/**
 * @brief deinit 是否已要求 worker 离开 wait/poll。
 */
bool myvendor_mtp_worker_should_exit(void);

/**
 * @brief worker 退出前拆掉 attach。
 */
void myvendor_mtp_worker_detach(void);

/**
 * @brief ACTIVE session 中 I/O 失败：丢掉 ACTIVE、关 session、plug OFF。
 */
void myvendor_mtp_worker_link_lost(void);

/**
 * @brief 是否有未处理的 link-lost。
 */
bool myvendor_mtp_worker_link_lost_pending(void);

/**
 * @brief 清 link-lost 标志。
 */
void myvendor_mtp_worker_clear_link_lost(void);

/**
 * @brief mtp_simple_main 调用一次；拒绝重复 worker。
 * @return 0 成功，负值为已有实例等错误。
 */
int myvendor_mtp_worker_attach(void);

/**
 * @brief 开始一项 UI 可见活动。
 */
void myvendor_mtp_activity_start(myvendor_mtp_op_t op, const char *target,
                                 uint64_t total_bytes);

/**
 * @brief 设置活动详情（如重命名目的名）。
 */
void myvendor_mtp_activity_set_detail(const char *detail);

/**
 * @brief 更新已完成字节。
 */
void myvendor_mtp_activity_progress(uint64_t done_bytes);

/**
 * @brief 结束当前活动（保留最后一帧给 UI 短暂显示）。
 */
void myvendor_mtp_activity_finish(void);

/**
 * @brief 立刻清空活动。
 */
void myvendor_mtp_activity_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_MTP_INTERNAL_H */
