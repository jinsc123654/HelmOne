/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include "ipc_queue.h"
#include "ipc_hw.h"

void ipc_hw_trigger_interrupt(ipc_hw_q_handle_t *hw_q_handle)
{
    ipc_hw_ch_t *ch;

    SF_ASSERT(hw_q_handle->ch_id < IPC_HW_CH_NUM);
    SF_ASSERT(hw_q_handle->q_idx < IPC_HW_QUEUE_NUM);
    ch = &ipc_hw_obj.ch[hw_q_handle->ch_id];

    __HAL_MAILBOX_TRIGGER_CHANNEL_IT(&ch->cfg.tx.handle, hw_q_handle->q_idx);
}

int32_t ipc_hw_check_interrupt(ipc_hw_q_handle_t *hw_q_handle)
{
    ipc_hw_ch_t *ch;

    SF_ASSERT(hw_q_handle->ch_id < IPC_HW_CH_NUM);
    SF_ASSERT(hw_q_handle->q_idx < IPC_HW_QUEUE_NUM);
    ch = &ipc_hw_obj.ch[hw_q_handle->ch_id];

    return __HAL_MAILBOX_CHECK_CHANNEL_IT(&ch->cfg.tx.handle,
                                          hw_q_handle->q_idx);
}

int32_t ipc_hw_enable_interrupt(ipc_hw_q_handle_t *hw_q_handle, uint8_t qid,
                                uint32_t user_data)
{
    uint8_t ch_id;
    int32_t result = -1;
    uint8_t q_idx;
    ipc_hw_ch_t *ch;
    ipc_ch_cfg_t *ch_cfg;

    SF_ASSERT(hw_q_handle);

    ch_id = qid / IPC_HW_QUEUE_NUM;
    if (ch_id >= IPC_HW_CH_NUM)
    {
        return result;
    }

    ch = &ipc_hw_obj.ch[ch_id];
    ch_cfg = &ch->cfg;
    q_idx = qid - ch_id * IPC_HW_QUEUE_NUM;
    ch->data.user_data[q_idx] = user_data;
    ch->data.act_bitmap |= (1UL << q_idx);
    hw_q_handle->ch_id = ch_id;
    hw_q_handle->q_idx = q_idx;

    os_interrupt_start(ch_cfg->rx.irqn, 3, 0);

    /* Unmask H2L (HCPU->LCPU) so LCPU's mailbox IRQ fires when HCPU writes
     * CxITR.  Per SiFli-SDK reference, each side unmasks its own TX mailbox
     * (the remote side's receive path) at open time.  The original vendor
     * copy incorrectly used rx.handle here, leaving H2L.CxIER=0 and LCPU
     * deaf to all H2L triggers (CxISR would be set but CxMISR stayed 0).
     */
    __HAL_MAILBOX_UNMASK_CHANNEL_IT(&ch_cfg->tx.handle, q_idx);

    /* Also unmask L2H (LCPU->HCPU) so HCPU receives LCPU notifications.
     * In the SiFli-SDK reference design LCPU unmasks L2H from its own side,
     * but we do it here too as a safety net for LCPU firmwares that skip it.
     */
    __HAL_MAILBOX_UNMASK_CHANNEL_IT(&ch_cfg->rx.handle, q_idx);

    result = 0;
    return result;
}

int32_t ipc_hw_enable_interrupt2(ipc_hw_q_handle_t *hw_q_handle, uint8_t qid,
                                 uint32_t user_data)
{
    uint8_t ch_id;
    int32_t result = -1;
    uint8_t q_idx;
    ipc_hw_ch_t *ch;
    ipc_ch_cfg_t *ch_cfg;

    SF_ASSERT(hw_q_handle);

    ch_id = qid / IPC_HW_QUEUE_NUM;
    if (ch_id >= IPC_HW_CH_NUM)
    {
        return result;
    }

    ch = &ipc_hw_obj.ch[ch_id];
    ch_cfg = &ch->cfg;
    q_idx = qid - ch_id * IPC_HW_QUEUE_NUM;
    ch->data.user_data[q_idx] = user_data;
    ch->data.act_bitmap |= (1UL << q_idx);
    hw_q_handle->ch_id = ch_id;
    hw_q_handle->q_idx = q_idx;

    os_interrupt_start(ch_cfg->rx.irqn, 3, 0);

#ifdef SOC_BF0_HCPU
    SF_ASSERT(ch_cfg->rx.core == CORE_ID_LCPU);
#ifdef SF32LB52X
    HAL_HPAON_WakeCore(ch_cfg->rx.core);
#endif
    __HAL_MAILBOX_UNMASK_CHANNEL_IT(&ch_cfg->rx.handle, q_idx);
#ifdef SF32LB52X
    HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
#endif
#elif defined(SOC_BF0_LCPU)
    SF_ASSERT(ch_cfg->rx.core == CORE_ID_HCPU);
    HAL_LPAON_WakeCore(ch_cfg->rx.core);
    __HAL_MAILBOX_UNMASK_CHANNEL_IT(&ch_cfg->rx.handle, q_idx);
    HAL_LPAON_CANCEL_HP_ACTIVE_REQUEST();
#else
#error "Invalid core"
#endif

    result = 0;
    return result;
}

int32_t ipc_hw_disable_interrupt(ipc_hw_q_handle_t *hw_q_handle)
{
    uint8_t ch_id;
    uint8_t q_idx;
    ipc_hw_ch_t *ch;
    ipc_ch_cfg_t *ch_cfg;

    SF_ASSERT(hw_q_handle);

    ch_id = hw_q_handle->ch_id;
    SF_ASSERT(ch_id < IPC_HW_CH_NUM);
    ch = &ipc_hw_obj.ch[ch_id];
    ch_cfg = &ch->cfg;
    q_idx = hw_q_handle->q_idx;
    SF_ASSERT(q_idx < IPC_HW_QUEUE_NUM);
    ch->data.act_bitmap &= ~(1UL << q_idx);

    if (ch->data.act_bitmap == 0)
    {
        os_interrupt_stop(ch_cfg->rx.irqn);
    }

    __HAL_MAILBOX_MASK_CHANNEL_IT(&ch_cfg->tx.handle, q_idx);
    return 0;
}

int32_t ipc_hw_disable_interrupt2(ipc_hw_q_handle_t *hw_q_handle)
{
    uint8_t ch_id;
    uint8_t q_idx;
    ipc_hw_ch_t *ch;
    ipc_ch_cfg_t *ch_cfg;

    SF_ASSERT(hw_q_handle);

    ch_id = hw_q_handle->ch_id;
    SF_ASSERT(ch_id < IPC_HW_CH_NUM);
    ch = &ipc_hw_obj.ch[ch_id];
    ch_cfg = &ch->cfg;
    q_idx = hw_q_handle->q_idx;
    SF_ASSERT(q_idx < IPC_HW_QUEUE_NUM);
    ch->data.act_bitmap &= ~(1UL << q_idx);

    if (ch->data.act_bitmap == 0)
    {
        os_interrupt_stop(ch_cfg->rx.irqn);
    }

#ifdef SOC_BF0_HCPU
    SF_ASSERT(ch_cfg->rx.core == CORE_ID_LCPU);
#ifdef SF32LB52X
    HAL_HPAON_WakeCore(ch_cfg->rx.core);
#endif
    __HAL_MAILBOX_MASK_CHANNEL_IT(&ch_cfg->rx.handle, q_idx);
#ifdef SF32LB52X
    HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
#endif
#elif defined(SOC_BF0_LCPU)
    SF_ASSERT(ch_cfg->rx.core == CORE_ID_HCPU);
    HAL_LPAON_WakeCore(ch_cfg->rx.core);
    __HAL_MAILBOX_MASK_CHANNEL_IT(&ch_cfg->rx.handle, q_idx);
    HAL_LPAON_CANCEL_HP_ACTIVE_REQUEST();
#else
#error "Invalid core"
#endif

    return 0;
}
