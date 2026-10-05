/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef IPC_HW_H
#define IPC_HW_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "bf0_hal.h"
#include "ipc_hw_port.h"
#include "ipc_os_port.h"

#ifndef SF_ASSERT
#define SF_ASSERT assert
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    MAILBOX_HandleTypeDef handle;
    uint8_t core;
    IRQn_Type irqn;
} hwmailbox_ch_cfg_t;

typedef uint32_t (*ipc_hw_addr_conv_t)(uint32_t addr);

typedef struct
{
    ipc_hw_addr_conv_t addr_conv;
    hwmailbox_ch_cfg_t rx;
    hwmailbox_ch_cfg_t tx;
} ipc_ch_cfg_t;

#if IPC_HW_QUEUE_NUM > 32
#error "too large hw queue number"
#endif

typedef struct
{
    uint32_t act_bitmap;
    uint32_t user_data[IPC_HW_QUEUE_NUM];
} ipc_ch_dyn_data_t;

typedef struct
{
    ipc_ch_cfg_t cfg;
    ipc_ch_dyn_data_t data;
} ipc_hw_ch_t;

typedef struct
{
    ipc_hw_ch_t ch[IPC_HW_CH_NUM];
} ipc_hw_t;

typedef struct
{
    uint8_t ch_id;
    uint8_t q_idx;
} ipc_hw_q_handle_t;

extern ipc_hw_t ipc_hw_obj;

int32_t ipc_hw_enable_interrupt(ipc_hw_q_handle_t *hw_q_handle, uint8_t qid,
                                uint32_t user_data);
int32_t ipc_hw_enable_interrupt2(ipc_hw_q_handle_t *hw_q_handle, uint8_t qid,
                                 uint32_t user_data);
int32_t ipc_hw_disable_interrupt(ipc_hw_q_handle_t *hw_q_handle);
int32_t ipc_hw_disable_interrupt2(ipc_hw_q_handle_t *hw_q_handle);
void ipc_hw_trigger_interrupt(ipc_hw_q_handle_t *hw_q_handle);
int32_t ipc_hw_check_interrupt(ipc_hw_q_handle_t *hw_q_handle);
void ipc_queue_data_ind(uint32_t user_data);

#ifdef __cplusplus
}
#endif

#endif