/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "ipc_hw.h"
#include "circular_buf.h"

#ifndef __ROM_USED
#define __ROM_USED
#endif

static inline enum circular_buf_state circular_buf_status(struct circular_buf *cb,
                                                          uint32_t rd_ptr,
                                                          uint32_t wr_ptr)
{
    uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
    uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
    uint32_t rd_mirror = CB_GET_PTR_MIRROR(rd_ptr);
    uint32_t wr_mirror = CB_GET_PTR_MIRROR(wr_ptr);

    if (rd_idx == wr_idx)
    {
        return rd_mirror == wr_mirror ? CIRCULAR_BUF_EMPTY : CIRCULAR_BUF_FULL;
    }

    return CIRCULAR_BUF_HALFFULL;
}

__ROM_USED void circular_buf_init(struct circular_buf *cb, uint8_t *pool,
                                  int16_t size)
{
    SF_ASSERT(cb != NULL);
    SF_ASSERT(size > 0);

    cb->read_idx_mirror = 0;
    cb->write_idx_mirror = 0;
    cb->wr_buffer_ptr = pool;
    cb->rd_buffer_ptr = pool;
    cb->buffer_size = size & ~3UL;
}

__ROM_USED void circular_buf_wr_init(struct circular_buf *cb, uint8_t *pool,
                                     int16_t size)
{
    SF_ASSERT(cb != NULL);
    SF_ASSERT(size > 0);

    cb->read_idx_mirror = 0;
    cb->write_idx_mirror = 0;
    cb->wr_buffer_ptr = pool;
    cb->buffer_size = size & ~3UL;
}

__ROM_USED void circular_buf_rd_init(struct circular_buf *cb, uint8_t *pool,
                                     int16_t size)
{
    (void)size;
    cb->rd_buffer_ptr = pool;
}

__ROM_USED size_t circular_buf_put(struct circular_buf *cb, const uint8_t *ptr,
                                   uint16_t length)
{
    uint16_t size;
    uint32_t wr_mirror;
    uint32_t wr_idx;

    SF_ASSERT(cb != NULL);

    size = circular_buf_space_len(cb);
    if (size == 0)
    {
        return 0;
    }

    if (size < length)
    {
        length = size;
    }

    wr_idx = CB_GET_PTR_IDX(cb->write_idx_mirror);
    wr_mirror = CB_GET_PTR_MIRROR(cb->write_idx_mirror);
    if ((cb->buffer_size - wr_idx) > length)
    {
        memcpy(&cb->wr_buffer_ptr[wr_idx], ptr, length);
        wr_idx += length;
        cb->write_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, wr_mirror);
        return length;
    }

    memcpy(&cb->wr_buffer_ptr[wr_idx], &ptr[0], cb->buffer_size - wr_idx);
    memcpy(&cb->wr_buffer_ptr[0], &ptr[cb->buffer_size - wr_idx],
           length - (cb->buffer_size - wr_idx));

    wr_mirror = ~wr_mirror;
    wr_idx = length - (cb->buffer_size - wr_idx);
    cb->write_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, wr_mirror);

    return length;
}

__ROM_USED size_t circular_buf_put_force(struct circular_buf *cb,
                                         const uint8_t *ptr,
                                         uint16_t length)
{
    uint16_t space_length;
    uint32_t wr_mirror;
    uint32_t wr_idx;
    uint32_t rd_mirror;

    SF_ASSERT(cb != NULL);

    space_length = circular_buf_space_len(cb);
    if (length > cb->buffer_size)
    {
        ptr = &ptr[length - cb->buffer_size];
        length = cb->buffer_size;
    }

    wr_idx = CB_GET_PTR_IDX(cb->write_idx_mirror);
    wr_mirror = CB_GET_PTR_MIRROR(cb->write_idx_mirror);
    if ((cb->buffer_size - wr_idx) > length)
    {
        memcpy(&cb->wr_buffer_ptr[wr_idx], ptr, length);
        wr_idx += length;
        cb->write_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, wr_mirror);

        if (length > space_length)
        {
            rd_mirror = CB_GET_PTR_MIRROR(cb->read_idx_mirror);
            cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, rd_mirror);
        }

        return length;
    }

    memcpy(&cb->wr_buffer_ptr[wr_idx], &ptr[0], cb->buffer_size - wr_idx);
    memcpy(&cb->wr_buffer_ptr[0], &ptr[cb->buffer_size - wr_idx],
           length - (cb->buffer_size - wr_idx));

    wr_mirror = ~wr_mirror;
    wr_idx = length - (cb->buffer_size - wr_idx);
    cb->write_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, wr_mirror);

    if (length > space_length)
    {
        rd_mirror = CB_GET_PTR_MIRROR(cb->read_idx_mirror);
        rd_mirror = ~rd_mirror;
        cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, rd_mirror);
    }

    return length;
}

__ROM_USED size_t circular_buf_get(struct circular_buf *cb, uint8_t *ptr,
                                   uint16_t length)
{
    size_t size;
    uint32_t rd_mirror;
    uint32_t rd_idx;

    SF_ASSERT(cb != NULL);

    size = circular_buf_data_len(cb);
    if (size == 0)
    {
        return 0;
    }

    if (size < length)
    {
        length = size;
    }

    rd_idx = CB_GET_PTR_IDX(cb->read_idx_mirror);
    rd_mirror = CB_GET_PTR_MIRROR(cb->read_idx_mirror);
    if (cb->buffer_size - rd_idx > length)
    {
        memcpy(ptr, &cb->rd_buffer_ptr[rd_idx], length);
        rd_idx += length;
        cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(rd_idx, rd_mirror);
        return length;
    }

    memcpy(&ptr[0], &cb->rd_buffer_ptr[rd_idx], cb->buffer_size - rd_idx);
    memcpy(&ptr[cb->buffer_size - rd_idx], &cb->rd_buffer_ptr[0],
           length - (cb->buffer_size - rd_idx));

    rd_mirror = ~rd_mirror;
    rd_idx = length - (cb->buffer_size - rd_idx);
    cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(rd_idx, rd_mirror);

    return length;
}

__ROM_USED size_t circular_buf_get_and_update_len(struct circular_buf *cb,
                                                  uint8_t *ptr,
                                                  uint16_t length,
                                                  size_t *remaining_len)
{
    size_t size;
    uint32_t mask;
    uint32_t rd_mirror;
    uint32_t rd_idx;

    SF_ASSERT(cb != NULL);

    size = circular_buf_data_len(cb);
    if (size == 0)
    {
        return 0;
    }

    if (size < length)
    {
        length = size;
    }

    rd_idx = CB_GET_PTR_IDX(cb->read_idx_mirror);
    rd_mirror = CB_GET_PTR_MIRROR(cb->read_idx_mirror);
    if (cb->buffer_size - rd_idx > length)
    {
        memcpy(ptr, &cb->rd_buffer_ptr[rd_idx], length);

        mask = os_interrupt_disable();
        if (remaining_len)
        {
            *remaining_len = circular_buf_data_len(cb) - length;
        }
        rd_idx += length;
        cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(rd_idx, rd_mirror);
        os_interrupt_enable(mask);

        return length;
    }

    memcpy(&ptr[0], &cb->rd_buffer_ptr[rd_idx], cb->buffer_size - rd_idx);
    memcpy(&ptr[cb->buffer_size - rd_idx], &cb->rd_buffer_ptr[0],
           length - (cb->buffer_size - rd_idx));

    mask = os_interrupt_disable();
    if (remaining_len)
    {
        *remaining_len = circular_buf_data_len(cb) - length;
    }
    rd_mirror = ~rd_mirror;
    rd_idx = length - (cb->buffer_size - rd_idx);
    cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(rd_idx, rd_mirror);
    os_interrupt_enable(mask);

    return length;
}

__ROM_USED size_t circular_buf_putchar(struct circular_buf *cb, uint8_t ch)
{
    uint32_t wr_mirror;
    uint32_t wr_idx;

    SF_ASSERT(cb != NULL);

    if (!circular_buf_space_len(cb))
    {
        return 0;
    }

    wr_idx = CB_GET_PTR_IDX(cb->write_idx_mirror);
    wr_mirror = CB_GET_PTR_MIRROR(cb->write_idx_mirror);
    cb->wr_buffer_ptr[wr_idx] = ch;

    if ((int16_t)wr_idx == cb->buffer_size - 1)
    {
        wr_mirror = ~wr_mirror;
        wr_idx = 0;
    }
    else
    {
        wr_idx++;
    }

    cb->write_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, wr_mirror);
    return 1;
}

__ROM_USED size_t circular_buf_putchar_force(struct circular_buf *cb, uint8_t ch)
{
    enum circular_buf_state old_state;
    uint32_t wr_mirror;
    uint32_t wr_idx;
    uint32_t rd_mirror;

    SF_ASSERT(cb != NULL);

    old_state = circular_buf_status(cb, cb->read_idx_mirror,
                                    cb->write_idx_mirror);

    wr_idx = CB_GET_PTR_IDX(cb->write_idx_mirror);
    wr_mirror = CB_GET_PTR_MIRROR(cb->write_idx_mirror);
    cb->wr_buffer_ptr[wr_idx] = ch;

    if ((int16_t)wr_idx == cb->buffer_size - 1)
    {
        wr_mirror = ~wr_mirror;
        wr_idx = 0;
        if (old_state == CIRCULAR_BUF_FULL)
        {
            rd_mirror = CB_GET_PTR_MIRROR(cb->read_idx_mirror);
            rd_mirror = ~rd_mirror;
            cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, rd_mirror);
        }
    }
    else
    {
        wr_idx++;
        if (old_state == CIRCULAR_BUF_FULL)
        {
            rd_mirror = CB_GET_PTR_MIRROR(cb->read_idx_mirror);
            cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(wr_idx, rd_mirror);
        }
    }

    return 1;
}

__ROM_USED size_t circular_buf_getchar(struct circular_buf *cb, uint8_t *ch)
{
    uint32_t rd_mirror;
    uint32_t rd_idx;

    SF_ASSERT(cb != NULL);

    if (!circular_buf_data_len(cb))
    {
        return 0;
    }

    rd_idx = CB_GET_PTR_IDX(cb->read_idx_mirror);
    rd_mirror = CB_GET_PTR_MIRROR(cb->read_idx_mirror);
    *ch = cb->rd_buffer_ptr[rd_idx];

    if ((int16_t)rd_idx == cb->buffer_size - 1)
    {
        rd_mirror = ~rd_mirror;
        rd_idx = 0;
    }
    else
    {
        rd_idx++;
    }

    cb->read_idx_mirror = CB_MAKE_PTR_IDX_MIRROR(rd_idx, rd_mirror);
    return 1;
}

__ROM_USED size_t circular_buf_data_len(struct circular_buf *cb)
{
    uint32_t rd_idx;
    uint32_t wr_idx;
    uint32_t rd_ptr = cb->read_idx_mirror;
    uint32_t wr_ptr = cb->write_idx_mirror;

    switch (circular_buf_status(cb, rd_ptr, wr_ptr))
    {
        case CIRCULAR_BUF_EMPTY:
            return 0;

        case CIRCULAR_BUF_FULL:
            return cb->buffer_size;

        case CIRCULAR_BUF_HALFFULL:
        default:
            rd_idx = CB_GET_PTR_IDX(rd_ptr);
            wr_idx = CB_GET_PTR_IDX(wr_ptr);
            if (wr_idx > rd_idx)
            {
                return wr_idx - rd_idx;
            }

            return cb->buffer_size - (rd_idx - wr_idx);
    }
}

__ROM_USED void circular_buf_reset(struct circular_buf *cb)
{
    SF_ASSERT(cb != NULL);

    cb->read_idx_mirror = 0;
    cb->write_idx_mirror = 0;
}
