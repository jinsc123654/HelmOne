/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "ipc_hw.h"

__WEAK void ipc_queue_data_ind_rom(uint32_t user_data)
{
    ipc_queue_data_ind(user_data);
}

static void hcpu2lcpu_notification_callback(MAILBOX_HandleTypeDef *hmailbox,
                                            uint8_t q_idx);

static MAILBOX_HandleTypeDef h2l_rx_handle = {
    .Instance = H2L_MAILBOX,
    .NotificationCallback = hcpu2lcpu_notification_callback,
};

__ROM_USED ipc_hw_t ipc_hw_obj = {
    .ch = {
        [0] = {
            .cfg = {
                .rx = {
                    .handle.Instance = H2L_MAILBOX,
                    .handle.NotificationCallback = hcpu2lcpu_notification_callback,
                    .core = CORE_ID_HCPU,
                    .irqn = HCPU2LCPU_IRQn,
                },
                .tx = {
                    .handle.Instance = L2H_MAILBOX,
                    .core = CORE_ID_LCPU,
                    .irqn = LCPU2HCPU_IRQn,
                },
            },
        },
    },
};

static void hcpu2lcpu_notification_callback(MAILBOX_HandleTypeDef *hmailbox,
                                            uint8_t q_idx)
{
    (void)hmailbox;

    SF_ASSERT(q_idx < IPC_HW_QUEUE_NUM);
    if (ipc_hw_obj.ch[0].data.act_bitmap & (1UL << q_idx))
    {
        if (q_idx == 0)
        {
            ipc_queue_data_ind_rom(ipc_hw_obj.ch[0].data.user_data[q_idx]);
        }
        else
        {
            ipc_queue_data_ind(ipc_hw_obj.ch[0].data.user_data[q_idx]);
        }
    }
}

int HCPU2LCPU_IRQHandler(int irq, FAR void *context, FAR void *arg)
{
    (void)irq;
    (void)context;
    (void)arg;

    os_interrupt_enter();
    HAL_MAILBOX_IRQHandler(&h2l_rx_handle);
    os_interrupt_exit();
    return 0;
}
