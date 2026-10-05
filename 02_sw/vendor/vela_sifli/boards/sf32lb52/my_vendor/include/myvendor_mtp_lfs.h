/**
 * @file myvendor_mtp_lfs.h
 * @brief MTP / LFS 旧名。新代码请用 myvendor_mtp.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_MTP_LFS_H
#define MY_VENDOR_MTP_LFS_H

#include "myvendor_mtp.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef myvendor_mtp_plug_t myvendor_mtp_link_state_t;  /**< 旧链路枚举别名。 */

#define MYVENDOR_MTP_LINK_OFF   MYVENDOR_MTP_PLUG_OFF     /**< 未插。 */
#define MYVENDOR_MTP_LINK_ENUM  MYVENDOR_MTP_PLUG_ENUM    /**< 已枚举。 */
#define MYVENDOR_MTP_LINK_UP    MYVENDOR_MTP_PLUG_ACTIVE  /**< 协议 ACTIVE。 */

/**
 * @brief 当前 plug 状态。
 */
static inline myvendor_mtp_link_state_t myvendor_mtp_lfs_link_state(void)
{
  myvendor_mtp_status_t st;

  myvendor_mtp_get_status(&st);
  return (myvendor_mtp_link_state_t)st.plug;
}

/**
 * @brief MTP session 是否打开。
 */
static inline bool myvendor_mtp_lfs_session(void)
{
  myvendor_mtp_status_t st;

  myvendor_mtp_get_status(&st);
  return st.session_open;
}

/**
 * @brief 主机是否已选 MTP。
 */
static inline bool myvendor_mtp_lfs_is_mtp(void)
{
  return myvendor_mtp_host_is_mtp();
}

/**
 * @brief 协议 poll 是否正在跑。
 */
static inline bool myvendor_mtp_lfs_transfer_active(void)
{
  myvendor_mtp_status_t st;

  myvendor_mtp_get_status(&st);
  return st.transfer_active;
}

/**
 * @brief UI 是否应冻结 LFS（同 myvendor_mtp_lfs_quiesce）。
 */
static inline bool myvendor_mtp_lfs_ui_frozen(void)
{
  return myvendor_mtp_lfs_quiesce();
}

/**
 * @brief 旧接口：忽略 pid。
 */
static inline void myvendor_mtp_lfs_ui_register(pid_t pid)
{
  (void)pid;
}

/**
 * @brief 打状态日志。
 */
static inline void myvendor_mtp_lfs_log_status(void)
{
  myvendor_mtp_log_status();
}

/**
 * @brief 旧接口：true 则 transfer_begin，false 则 transfer_end。
 */
static inline void myvendor_mtp_lfs_set_usb_host(bool active)
{
  if (active)
    {
      myvendor_mtp_transfer_begin();
    }
  else
    {
      myvendor_mtp_transfer_end();
    }
}

/** worker 专用，声明在 myvendor_mtp_internal.h。 */
void myvendor_mtp_lfs_set_link(myvendor_mtp_link_state_t state);
void myvendor_mtp_lfs_set_session(bool active);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_MTP_LFS_H */
