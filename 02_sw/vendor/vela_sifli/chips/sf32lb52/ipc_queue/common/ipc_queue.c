/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <ipc_os_port.h>

#include "bf0_hal.h"
#include "ipc_queue.h"
#include "ipc_hw.h"
#include "circular_buf.h"

#ifndef __ROM_USED
#define __ROM_USED
#endif

typedef struct
{
    bool active;
    uint32_t data_len;
    struct circular_buf *rx_ring_buffer;
    struct circular_buf *tx_ring_buffer;
    ipc_hw_q_handle_t hw_q_handle;
    ipc_queue_cfg_t cfg;
} ipc_queue_t;

#define IPC_LOGICAL_QUEUE_NUM (4)
#define IPC_INVALID_QUEUE_ID (UINT8_MAX)
#define IPC_QUEUE_HANDLE_OFFSET (10)
#define IPC_QUEUE_HANDLE_2_OFFSET(handle) ((handle) - IPC_QUEUE_HANDLE_OFFSET)
#define IPC_QUEUE_OFFSET_2_HANDLE(offset) ((offset) + IPC_QUEUE_HANDLE_OFFSET)

typedef struct
{
    bool init;
    ipc_queue_t queues[IPC_LOGICAL_QUEUE_NUM];
} ipc_ctx_t;

__ROM_USED ipc_ctx_t ipc_ctx;

static bool is_valid_handle(ipc_queue_handle_t handle)
{
    int32_t offset;

    if (!ipc_ctx.init)
    {
        return false;
    }

    if ((handle < IPC_QUEUE_HANDLE_OFFSET) ||
        ((handle - IPC_QUEUE_HANDLE_OFFSET) >= IPC_LOGICAL_QUEUE_NUM))
    {
        return false;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    if (ipc_ctx.queues[offset].cfg.qid == IPC_INVALID_QUEUE_ID)
    {
        return false;
    }

    return true;
}

__ROM_USED ipc_queue_handle_t ipc_queue_init(ipc_queue_cfg_t *q_cfg)
{
    uint32_t i;
    ipc_queue_handle_t handle = IPC_QUEUE_INVALID_HANDLE;
    int32_t offset;
    ipc_queue_t *ipc_queue;
    uint32_t mask;

    if (q_cfg == NULL)
    {
        return handle;
    }

    mask = os_interrupt_disable();

    if (!ipc_ctx.init)
    {
        for (i = 0; i < IPC_LOGICAL_QUEUE_NUM; i++)
        {
            ipc_ctx.queues[i].cfg.qid = IPC_INVALID_QUEUE_ID;
            ipc_ctx.queues[i].active = false;
        }
        ipc_ctx.init = true;
    }

    ipc_queue = &ipc_ctx.queues[0];
    offset = -1;
    for (i = 0; i < IPC_LOGICAL_QUEUE_NUM; i++)
    {
        if (q_cfg->qid == ipc_queue->cfg.qid)
        {
            handle = IPC_QUEUE_INVALID_HANDLE;
            break;
        }

        if ((handle == IPC_QUEUE_INVALID_HANDLE) &&
            (ipc_queue->cfg.qid == IPC_INVALID_QUEUE_ID))
        {
            offset = i;
            handle = IPC_QUEUE_OFFSET_2_HANDLE(offset);
        }

        ipc_queue++;
    }

    if ((i >= IPC_LOGICAL_QUEUE_NUM) && (offset >= 0))
    {
        memcpy(&ipc_ctx.queues[offset].cfg, q_cfg, sizeof(*q_cfg));
        ipc_ctx.queues[offset].data_len = 0;
        ipc_ctx.queues[offset].active = false;
    }

    os_interrupt_enable(mask);
    return handle;
}

__ROM_USED int32_t ipc_queue_get_user_data(ipc_queue_handle_t handle,
                                           uint32_t *user_data)
{
    int32_t offset;
    ipc_queue_t *queue;
    uint32_t mask;

    if ((user_data == NULL) || !is_valid_handle(handle))
    {
        return -1;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];

    mask = os_interrupt_disable();
    *user_data = queue->cfg.user_data;
    os_interrupt_enable(mask);

    return 0;
}

__ROM_USED int32_t ipc_queue_set_user_data(ipc_queue_handle_t handle,
                                           uint32_t user_data)
{
    int32_t offset;
    ipc_queue_t *queue;
    uint32_t mask;

    if ((user_data == 0) || !is_valid_handle(handle))
    {
        return -1;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];

    mask = os_interrupt_disable();
    queue->cfg.user_data = user_data;
    os_interrupt_enable(mask);

    return 0;
}

static int32_t ipc_queue_do_open(ipc_queue_handle_t handle,
                                 int32_t (*enable_interrupt)(
                                     ipc_hw_q_handle_t *hw_q_handle,
                                     uint8_t qid,
                                     uint32_t user_data))
{
    int32_t result = -1;
    uint8_t *pool;
    uint32_t mask;
    ipc_queue_t *queue;
    int32_t offset;

    if (!is_valid_handle(handle))
    {
        return result;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];

    mask = os_interrupt_disable();
    if (queue->active)
    {
        goto end;
    }

    queue->rx_ring_buffer = (struct circular_buf *)queue->cfg.rx_buf_addr;
    queue->tx_ring_buffer = (struct circular_buf *)queue->cfg.tx_buf_addr;

    if (queue->tx_ring_buffer)
    {
        SF_ASSERT(queue->cfg.tx_buf_size > sizeof(struct circular_buf));
        pool = (uint8_t *)(queue->tx_ring_buffer + 1);
        circular_buf_wr_init(queue->tx_ring_buffer, pool,
                             queue->cfg.tx_buf_size - sizeof(struct circular_buf));

        pool = (uint8_t *)((struct circular_buf *)queue->cfg.tx_buf_addr_alias + 1);
        circular_buf_rd_init(queue->tx_ring_buffer, pool,
                             queue->cfg.tx_buf_size - sizeof(struct circular_buf));
    }

    result = enable_interrupt(&queue->hw_q_handle, queue->cfg.qid,
                              (uint32_t)handle);
    if (result == 0)
    {
        queue->active = true;
    }

end:
    os_interrupt_enable(mask);
    return result;
}

__ROM_USED int32_t ipc_queue_open(ipc_queue_handle_t handle)
{
    return ipc_queue_do_open(handle, ipc_hw_enable_interrupt);
}

__ROM_USED int32_t ipc_queue_open2(ipc_queue_handle_t handle)
{
    return ipc_queue_do_open(handle, ipc_hw_enable_interrupt2);
}

__ROM_USED bool ipc_queue_is_open(ipc_queue_handle_t handle)
{
    int32_t offset;

    if (!is_valid_handle(handle))
    {
        return false;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    return ipc_ctx.queues[offset].active;
}

static int32_t ipc_queue_do_close(ipc_queue_handle_t handle,
                                  int32_t (*disable_interrupt)(
                                      ipc_hw_q_handle_t *hw_q_handle))
{
    int32_t offset;
    ipc_queue_t *queue;
    uint32_t mask;
    int32_t result;

    if (!is_valid_handle(handle))
    {
        return -1;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];
    if (!queue->active)
    {
        return -1;
    }

    mask = os_interrupt_disable();
    result = disable_interrupt(&queue->hw_q_handle);
    queue->active = false;
    queue->rx_ring_buffer = NULL;
    queue->tx_ring_buffer = NULL;
    os_interrupt_enable(mask);

    return result;
}

__ROM_USED int32_t ipc_queue_close(ipc_queue_handle_t handle)
{
    return ipc_queue_do_close(handle, ipc_hw_disable_interrupt);
}

__ROM_USED int32_t ipc_queue_close2(ipc_queue_handle_t handle)
{
    return ipc_queue_do_close(handle, ipc_hw_disable_interrupt2);
}

__ROM_USED int32_t ipc_queue_deinit(ipc_queue_handle_t handle)
{
    int32_t offset;
    ipc_queue_t *queue;

    if (!is_valid_handle(handle))
    {
        return -1;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];
    if (queue->active)
    {
        return -1;
    }

    queue->cfg.qid = IPC_INVALID_QUEUE_ID;
    return 0;
}

__ROM_USED size_t ipc_queue_read(ipc_queue_handle_t handle, void *buffer,
                                 size_t size)
{
    ipc_queue_t *queue;
    int32_t offset;

    if (!is_valid_handle(handle) || (buffer == NULL))
    {
        return 0;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];
    if (!queue->active || (queue->rx_ring_buffer == NULL) || (queue->data_len == 0))
    {
        return 0;
    }

    return circular_buf_get_and_update_len(queue->rx_ring_buffer, buffer, size,
                                           (size_t *)&queue->data_len);
}

__ROM_USED size_t ipc_queue_write(ipc_queue_handle_t handle, const void *buffer,
                                  size_t size, uint32_t timeout)
{
    size_t data_len;
    uint32_t start_time;
    uint32_t cnt;
    uint32_t total_len;
    ipc_queue_t *queue;
    int32_t offset;

    if (!is_valid_handle(handle))
    {
        return 0;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];
    if (!queue->active)
    {
        return 0;
    }

    if (buffer == NULL)
    {
        ipc_hw_trigger_interrupt(&queue->hw_q_handle);
        return 0;
    }

    if (queue->tx_ring_buffer == NULL)
    {
        return 0;
    }

    cnt = 0;
    start_time = HAL_GetTick();
    total_len = size;
    while (total_len > 0)
    {
        data_len = circular_buf_put(queue->tx_ring_buffer, buffer, total_len);
        SF_ASSERT(data_len <= total_len);
        total_len -= data_len;
        buffer = (const uint8_t *)buffer + data_len;

        if (data_len > 0)
        {
            ipc_hw_trigger_interrupt(&queue->hw_q_handle);
        }

        if (HAL_GetTick() != start_time)
        {
            cnt++;
            start_time = HAL_GetTick();
        }

        if (cnt >= timeout)
        {
            break;
        }
    }

    return size - total_len;
}

__ROM_USED bool ipc_queue_check_idle(void)
{
    bool is_idle = true;
    uint32_t i;
    ipc_queue_t *queue;
    uint32_t mask;

    if (!ipc_ctx.init)
    {
        return true;
    }

    mask = os_interrupt_disable();
    queue = &ipc_ctx.queues[0];
    for (i = 0; i < IPC_LOGICAL_QUEUE_NUM; i++)
    {
        if (queue->active && (ipc_hw_check_interrupt(&queue->hw_q_handle) != 0))
        {
            is_idle = false;
            break;
        }

        if (queue->active && queue->tx_ring_buffer &&
            (circular_buf_data_len(queue->tx_ring_buffer) > 0))
        {
            is_idle = false;
            break;
        }

        queue++;
    }
    os_interrupt_enable(mask);

    return is_idle;
}

__WEAK bool ipc_queue_check_idle_rom(void)
{
    return ipc_queue_check_idle();
}

__ROM_USED void ipc_queue_restore_all(void)
{
    ipc_ctx_t *ctx = &ipc_ctx;
    uint32_t i;
    int32_t res;

    if (!ctx->init)
    {
        return;
    }

    for (i = 0; i < IPC_LOGICAL_QUEUE_NUM; i++)
    {
        if (ctx->queues[i].active)
        {
            ctx->queues[i].active = false;
            res = ipc_queue_open(IPC_QUEUE_OFFSET_2_HANDLE(i));
            SF_ASSERT(res == 0);
        }
    }
}

__WEAK void ipc_queue_restore_all_rom(void)
{
    ipc_queue_restore_all();
}

__ROM_USED void ipc_queue_data_ind(uint32_t user_data)
{
    ipc_queue_handle_t handle = (ipc_queue_handle_t)user_data;
    ipc_queue_t *queue;
    int32_t offset;

    if (!is_valid_handle(handle))
    {
        return;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];
    if (!queue->active)
    {
        return;
    }

    queue->data_len = queue->rx_ring_buffer ?
        circular_buf_data_len(queue->rx_ring_buffer) : 0;
    queue->cfg.rx_ind(handle, queue->data_len);
}

__ROM_USED size_t ipc_queue_get_rx_size(ipc_queue_handle_t handle)
{
    ipc_queue_t *queue;
    int32_t offset;

    if (!is_valid_handle(handle))
    {
        return 0;
    }

    offset = IPC_QUEUE_HANDLE_2_OFFSET(handle);
    queue = &ipc_ctx.queues[offset];
    if (!queue->active || (queue->rx_ring_buffer == NULL))
    {
        return 0;
    }

    return queue->data_len;
}
