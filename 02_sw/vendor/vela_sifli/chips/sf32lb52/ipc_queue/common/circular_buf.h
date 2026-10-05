/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CIRCULAR_BUF_H__
#define CIRCULAR_BUF_H__

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SF_ASSERT
#define SF_ASSERT assert
#endif

#define CB_PTR_MIRROR_OFFSET (0)
#define CB_PTR_MIRROR_MASK (0xFFFF)
#define CB_PTR_IDX_OFFSET (16)
#define CB_PTR_IDX_MASK (0xFFFF)

#define CB_MAKE_PTR_IDX_MIRROR(idx, mirror) \
    (((uint32_t)(idx) << CB_PTR_IDX_OFFSET) | ((mirror) & CB_PTR_MIRROR_MASK))
#define CB_GET_PTR_IDX(ptr_idx_mirror) \
    (((ptr_idx_mirror) >> CB_PTR_IDX_OFFSET) & CB_PTR_IDX_MASK)
#define CB_GET_PTR_MIRROR(ptr_idx_mirror) \
    ((ptr_idx_mirror) & CB_PTR_MIRROR_MASK)

struct circular_buf
{
    uint8_t *rd_buffer_ptr;
    uint8_t *wr_buffer_ptr;
    uint32_t read_idx_mirror;
    uint32_t write_idx_mirror;
    int16_t buffer_size;
};

enum circular_buf_state
{
    CIRCULAR_BUF_EMPTY,
    CIRCULAR_BUF_FULL,
    CIRCULAR_BUF_HALFFULL,
};

void circular_buf_init(struct circular_buf *cb, uint8_t *pool, int16_t size);
void circular_buf_wr_init(struct circular_buf *cb, uint8_t *pool, int16_t size);
void circular_buf_rd_init(struct circular_buf *cb, uint8_t *pool, int16_t size);
void circular_buf_reset(struct circular_buf *cb);
size_t circular_buf_put(struct circular_buf *cb, const uint8_t *ptr,
                        uint16_t length);
size_t circular_buf_put_force(struct circular_buf *cb, const uint8_t *ptr,
                              uint16_t length);
size_t circular_buf_putchar(struct circular_buf *cb, uint8_t ch);
size_t circular_buf_putchar_force(struct circular_buf *cb, uint8_t ch);
size_t circular_buf_get(struct circular_buf *cb, uint8_t *ptr, uint16_t length);
size_t circular_buf_get_and_update_len(struct circular_buf *cb, uint8_t *ptr,
                                       uint16_t length, size_t *remaining_len);
size_t circular_buf_getchar(struct circular_buf *cb, uint8_t *ch);
size_t circular_buf_data_len(struct circular_buf *cb);

static inline uint16_t circular_buf_get_size(struct circular_buf *cb)
{
    SF_ASSERT(cb != NULL);
    return cb->buffer_size;
}

#define circular_buf_space_len(cb) ((cb)->buffer_size - circular_buf_data_len(cb))

#ifdef __cplusplus
}
#endif

#endif