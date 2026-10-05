/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SDIO_CONFIG_H__
#define __SDIO_CONFIG_H__

#include <rtconfig.h>
#include "bf0_hal.h"
#include "dma_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// FIX ME: With real HW setting.
#ifdef BSP_USING_SD_LINE
#ifdef SDMMC1_DMA_INSTANCE
#define SDIO_BUS_CONFIG                                  \
    {                                                    \
        .Instance = SDIO1,                               \
        .dma_rx.dma_irq_prio = SDMMC1_DMA_IRQ_PRIO,      \
        .dma_tx.dma_irq_prio = SDMMC1_DMA_IRQ_PRIO,      \
        .dma_rx.Instance = SDMMC1_DMA_INSTANCE,          \
        .dma_rx.request = SDMMC1_DMA_REQUEST,            \
        .dma_rx.dma_irq = SDMMC1_DMA_IRQ,                \
        .dma_tx.Instance = SDMMC1_DMA_INSTANCE,          \
        .dma_tx.request = SDMMC1_DMA_REQUEST,            \
        .dma_tx.dma_irq = SDMMC1_DMA_IRQ,                \
    }

#else
#define SDIO_BUS_CONFIG                                  \
    {                                                    \
        .Instance = SDIO1,                               \
        .dma_rx.dma_irq_prio = 0,                        \
        .dma_tx.dma_irq_prio = 0,                        \
        .dma_rx.Instance = 0,                            \
        .dma_rx.request = 0,                             \
        .dma_rx.dma_irq = 0,                             \
        .dma_tx.Instance = 0,                            \
        .dma_tx.request = 0,                             \
        .dma_tx.dma_irq = 0,                             \
    }

#endif  //SDMMC2_DMA_INSTANCE
#endif //BSP_USING_SD_LINE

#ifdef __cplusplus
}
#endif

#endif /*__SDIO_CONFIG_H__ */

