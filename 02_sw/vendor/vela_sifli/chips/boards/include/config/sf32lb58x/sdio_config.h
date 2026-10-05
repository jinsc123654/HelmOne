/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SDIO_CONFIG_H__
#define __SDIO_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

// FIX ME: With real HW setting.
#ifdef BSP_USING_SDIO
#define SDIO_BUS_CONFIG                      \
    {                                        \
        .Instance = SDIO,                    \
        .dma_rx.dma_irq_prio = 0,            \
        .dma_tx.dma_irq_prio = 0,            \
        .dma_rx.Instance = 0,                \
        .dma_rx.request = 0,                 \
        .dma_rx.dma_irq = 0,                 \
        .dma_tx.Instance = 0,                \
        .dma_tx.request = 0,                 \
        .dma_tx.dma_irq = 0,                 \
    }

#endif

#ifdef __cplusplus
}
#endif

#endif /*__SDIO_CONFIG_H__ */

