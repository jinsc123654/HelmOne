/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __MAILBOX_CONFIG_H__
#define __MAILBOX_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

// Between LCPU and HCPU
#define MAILBOX_L2H_CH1_CONFIG                      \
    {                                            \
       .handle.Instance     = L2H_MAILBOX,       \
       .channel             = MAILBOX_CHANNEL_1, \
       .core                = CORE_ID_LCPU,      \
       .irqn                = LCPU2HCPU_IRQn,    \
       .name                = "mb_l2h1",         \
       .device.buffer_size  = LCPU2HCPU_MB_CH1_BUF_SIZE,              \
       .device.tx_buffer_addr = LCPU2HCPU_MB_CH1_BUF_START_ADDR,      \
       .device.rx_buffer_addr = LCPU2HCPU_MB_CH1_BUF_START_ADDR,      \
    }

#define MAILBOX_H2L_CH1_CONFIG                      \
    {                                            \
       .handle.Instance     = H2L_MAILBOX,       \
       .channel             = MAILBOX_CHANNEL_1, \
       .core                = CORE_ID_HCPU,      \
       .irqn                = HCPU2LCPU_IRQn,    \
       .name                = "mb_h2l1",        \
       .device.buffer_size  = HCPU2LCPU_MB_CH1_BUF_SIZE,              \
       .device.tx_buffer_addr = HCPU2LCPU_MB_CH1_BUF_START_ADDR,     \
       .device.rx_buffer_addr = HCPU_ADDR_2_LCPU_ADDR(HCPU2LCPU_MB_CH1_BUF_START_ADDR),     \
    }

#define MAILBOX_L2H_CH2_CONFIG                      \
    {                                            \
       .handle.Instance     = L2H_MAILBOX,       \
       .channel             = MAILBOX_CHANNEL_2, \
       .core                = CORE_ID_LCPU,      \
       .irqn                = LCPU2HCPU_IRQn,    \
       .name                = "mb_l2h2",         \
       .device.buffer_size  = LCPU2HCPU_MB_CH2_BUF_SIZE,              \
       .device.tx_buffer_addr = LCPU2HCPU_MB_CH2_BUF_START_ADDR,      \
       .device.rx_buffer_addr = LCPU2HCPU_MB_CH2_BUF_START_ADDR,      \
    }

#define MAILBOX_H2L_CH2_CONFIG                      \
    {                                            \
       .handle.Instance     = H2L_MAILBOX,       \
       .channel             = MAILBOX_CHANNEL_2, \
       .core                = CORE_ID_HCPU,      \
       .irqn                = HCPU2LCPU_IRQn,    \
       .name                = "mb_h2l2",        \
       .device.buffer_size  = HCPU2LCPU_MB_CH2_BUF_SIZE,              \
       .device.tx_buffer_addr = HCPU2LCPU_MB_CH2_BUF_START_ADDR,     \
       .device.rx_buffer_addr = HCPU_ADDR_2_LCPU_ADDR(HCPU2LCPU_MB_CH2_BUF_START_ADDR),     \
    }

#ifdef SOC_BF0_HCPU
#define BIDIR_MB_1_CONFIG                \
    {                                   \
        .name = "bdmb_hl1",            \
        .rx_dev_name = "mb_l2h1",       \
        .tx_dev_name = "mb_h2l1",       \
    }
#define BIDIR_MB_3_CONFIG                \
    {                                   \
        .name = "bdmb_hl2",            \
        .rx_dev_name = "mb_l2h2",       \
        .tx_dev_name = "mb_h2l2",       \
    }

#ifdef SOC_BF0_LCPU
#define BIDIR_MB_1_CONFIG                \
    {                                   \
        .name = "bdmb_lh1",            \
        .rx_dev_name = "mb_h2l1",       \
        .tx_dev_name = "mb_l2h1",       \
    }
#define BIDIR_MB_3_CONFIG                \
    {                                   \
        .name = "bdmb_lh2",            \
        .rx_dev_name = "mb_h2l2",       \
        .tx_dev_name = "mb_l2h2",       \
    }
#endif

#ifdef __cplusplus
}
#endif

#endif /* __MAILBOX_CONFIG_H__ */

