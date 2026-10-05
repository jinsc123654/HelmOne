/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef IPC_QUEUE_H
#define IPC_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef BSP_USING_PC_SIMULATOR
#include "ipc_hw_port.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_QUEUE_INVALID_HANDLE (0)

typedef int32_t ipc_queue_handle_t;
typedef int32_t (*ipc_queue_rx_ind_t)(ipc_queue_handle_t handle, size_t size);

typedef struct
{
    uint8_t qid;
    uint32_t rx_buf_addr;
    uint32_t tx_buf_addr;
    uint32_t tx_buf_addr_alias;
    uint32_t tx_buf_size;
    ipc_queue_rx_ind_t rx_ind;
    uint32_t user_data;
} ipc_queue_cfg_t;

ipc_queue_handle_t ipc_queue_init(ipc_queue_cfg_t *q_cfg);
int32_t ipc_queue_get_user_data(ipc_queue_handle_t handle, uint32_t *user_data);
int32_t ipc_queue_set_user_data(ipc_queue_handle_t handle, uint32_t user_data);
int32_t ipc_queue_open(ipc_queue_handle_t handle);
int32_t ipc_queue_open2(ipc_queue_handle_t handle);
bool ipc_queue_is_open(ipc_queue_handle_t handle);
int32_t ipc_queue_close(ipc_queue_handle_t handle);
int32_t ipc_queue_close2(ipc_queue_handle_t handle);
int32_t ipc_queue_deinit(ipc_queue_handle_t handle);
size_t ipc_queue_read(ipc_queue_handle_t handle, void *buffer, size_t size);
size_t ipc_queue_write(ipc_queue_handle_t handle, const void *buffer, size_t size,
                       uint32_t timeout);
bool ipc_queue_check_idle(void);
bool ipc_queue_check_idle_rom(void);
void ipc_queue_restore_all(void);
void ipc_queue_restore_all_rom(void);
size_t ipc_queue_get_rx_size(ipc_queue_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif
