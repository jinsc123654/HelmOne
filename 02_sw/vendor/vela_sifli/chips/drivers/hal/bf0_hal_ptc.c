/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bf0_hal.h"


#if defined(HAL_PTC_ENABLED) ||defined(_SIFLI_DOXYGEN_)

/**
* @brief  Initialize PTC
* @param  hptc Handle of PTC
* @retval HAL_OK if successful, otherwise error
*/
__HAL_ROM_USED HAL_StatusTypeDef HAL_PTC_Init(PTC_HandleTypeDef *hptc)
{
    uint32_t address;

    HAL_ASSERT(hptc);
    HAL_ASSERT(hptc->Init.Channel < HAL_PTC_MAXCHN);

    address = (uint32_t)(&(hptc->Instance->TCR1));
    address += hptc->Init.Channel * sizeof(PTC_ChnTypeDef);
    hptc->Chn = (PTC_ChnTypeDef *)address;
    hptc->Chn->TAR = hptc->Init.Address;
    hptc->Chn->TDR = hptc->Init.data;
    hptc->State = HAL_PTC_STATE_READY;
    return HAL_OK;
}

/**
* @brief  Enable/disable PTC
* @param  hptc Handle of PTC
* @retval HAL_OK if successful, otherwise error
*/
__HAL_ROM_USED HAL_StatusTypeDef HAL_PTC_Enable(PTC_HandleTypeDef *hptc, int enable)
{
    if (enable)
    {
        uint32_t cr;
        hptc->Instance->ICR |= ((1UL << hptc->Init.Channel) | PTC_ICR_CTEIF);
        hptc->Instance->IER |= ((1UL << hptc->Init.Channel) | ((1UL << hptc->Init.Channel) << PTC_IER_TEIE_Pos));
        cr = hptc->Init.Sel;
        cr |= (((uint32_t)(hptc->Init.Operation)) << PTC_TCR1_OP_Pos);
#ifdef SF32LB58X
        cr |= (((uint32_t)(hptc->Init.Tripol)) << PTC_TCR1_TRIGPOL_Pos);
#endif // SOC_SF32LB58X
        hptc->Chn->TCR = cr;
#ifdef SF32LB58X
        if (hptc->Init.Channel < 4 && hptc->Init.Delay != 0)
        {
            hptc->Chn->RCR &= ~PTC_RCR1_DLY;
            hptc->Chn->RCR |= hptc->Init.Delay << PTC_RCR1_DLY_Pos;
        }
#endif // SOC_SF32LB58X
        hptc->State = HAL_PTC_STATE_RUNNING;
    }
    else
    {
        hptc->Instance->IER &= ~((1UL << hptc->Init.Channel) | ((1UL << hptc->Init.Channel) << PTC_IER_TEIE_Pos));
        hptc->State = HAL_PTC_STATE_READY;
    }
    return HAL_OK;
}

/**
* @brief  Handle PTC interrupt
* @param  hptc Handle of PTC
* @retval None
*/
__HAL_ROM_USED void HAL_PTC_IRQHandler(PTC_HandleTypeDef *hptc)
{
    hptc->Instance->ICR |= (1 << hptc->Init.Channel);        // Clear PTC interrupt.
}


#endif