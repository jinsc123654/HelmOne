/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SPI_CONFIG_H__
#define __SPI_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI1_CORE   CORE_ID_HCPU
#define SPI2_CORE   CORE_ID_HCPU
#define SPI3_CORE   CORE_ID_LCPU
#define SPI4_CORE   CORE_ID_LCPU

#ifdef BSP_USING_SPI1
#ifndef SPI1_BUS_CONFIG
#define SPI1_BUS_CONFIG                                  \
    {                                                    \
        .Instance = SPI1,                                \
        .bus_name = "spi1",                              \
        .irq_type = SPI1_IRQn,                           \
        .core     = SPI1_CORE,                           \
    }

#endif  /* SPI1_BUS_CONFIG */
#endif  /* BSP_USING_SPI1 */

#ifdef BSP_SPI1_TX_USING_DMA
#ifndef SPI1_TX_DMA_CONFIG
#define SPI1_TX_DMA_CONFIG                              \
    {                                                   \
        .dma_irq_prio = SPI1_TX_DMA_IRQ_PRIO,           \
        .Instance = SPI1_TX_DMA_INSTANCE,               \
        .dma_irq = SPI1_TX_DMA_IRQ,                     \
        .request = SPI1_TX_DMA_REQUEST,                 \
    }

#endif  /* SPI1_TX_DMA_CONFIG */
#endif  /* BSP_SPI1_TX_USING_DMA */

#ifdef BSP_SPI1_RX_USING_DMA
#ifndef SPI1_RX_DMA_CONFIG
#define SPI1_RX_DMA_CONFIG                              \
    {                                                   \
        .dma_irq_prio = SPI1_RX_DMA_IRQ_PRIO,           \
        .Instance = SPI1_RX_DMA_INSTANCE,               \
        .dma_irq = SPI1_RX_DMA_IRQ,                     \
        .request = SPI1_RX_DMA_REQUEST,                 \
    }

#endif  /* SPI1_RX_DMA_CONFIG */
#endif  /* BSP_SPI1_RX_USING_DMA */

#ifdef BSP_USING_SPI2
#ifndef SPI2_BUS_CONFIG
#define SPI2_BUS_CONFIG                                  \
    {                                                    \
        .Instance = SPI2,                                \
        .bus_name = "spi2",                              \
        .irq_type = SPI2_IRQn,                           \
        .core     = SPI2_CORE,                           \
    }

#endif  /* SPI2_BUS_CONFIG */
#endif  /* BSP_USING_SPI2 */

#ifdef BSP_SPI2_TX_USING_DMA
#ifndef SPI2_TX_DMA_CONFIG
#define SPI2_TX_DMA_CONFIG                              \
    {                                                   \
        .dma_irq_prio = SPI2_TX_DMA_IRQ_PRIO,           \
        .Instance = SPI2_TX_DMA_INSTANCE,               \
        .dma_irq = SPI2_TX_DMA_IRQ,                     \
        .request = SPI2_TX_DMA_REQUEST,                 \
    }

#endif  /* SPI2_TX_DMA_CONFIG */
#endif  /* BSP_SPI2_TX_USING_DMA */

#ifdef BSP_SPI2_RX_USING_DMA
#ifndef SPI2_RX_DMA_CONFIG
#define SPI2_RX_DMA_CONFIG                              \
    {                                                   \
        .dma_irq_prio = SPI2_RX_DMA_IRQ_PRIO,           \
        .Instance = SPI2_RX_DMA_INSTANCE,               \
        .dma_irq = SPI2_RX_DMA_IRQ,                     \
        .request = SPI2_RX_DMA_REQUEST,                 \
    }

#endif  /* SPI2_RX_DMA_CONFIG */
#endif  /* BSP_SPI2_RX_USING_DMA */

#ifdef BSP_USING_SPI3
#ifndef SPI3_BUS_CONFIG
#define SPI3_BUS_CONFIG                                  \
    {                                                    \
        .Instance = SPI3,                                \
        .bus_name = "spi3",                              \
        .irq_type = SPI3_IRQn,                           \
        .core     = SPI3_CORE,                           \
    }

#endif  /* SPI3_BUS_CONFIG */
#endif  /* BSP_USING_SPI3 */

#ifdef BSP_SPI3_TX_USING_DMA
#ifndef SPI3_TX_DMA_CONFIG
#define SPI3_TX_DMA_CONFIG                              \
    {                                                   \
        .dma_irq_prio = SPI3_TX_DMA_IRQ_PRIO,           \
        .Instance = SPI3_TX_DMA_INSTANCE,               \
        .dma_irq = SPI3_TX_DMA_IRQ,                     \
        .request = SPI3_TX_DMA_REQUEST,                 \
    }

#endif  /* SPI3_TX_DMA_CONFIG */
#endif  /* BSP_SPI3_TX_USING_DMA */

#ifdef BSP_SPI3_RX_USING_DMA
#ifndef SPI3_RX_DMA_CONFIG
#define SPI3_RX_DMA_CONFIG                              \
    {                                                   \
        .dma_irq_prio = SPI3_RX_DMA_IRQ_PRIO,           \
        .Instance = SPI3_RX_DMA_INSTANCE,               \
        .dma_irq = SPI3_RX_DMA_IRQ,                     \
        .request = SPI3_RX_DMA_REQUEST,                 \
    }

#endif  /* SPI3_RX_DMA_CONFIG */
#endif  /* BSP_SPI3_RX_USING_DMA */

#ifdef BSP_USING_SPI4
#ifndef SPI4_BUS_CONFIG
#define SPI4_BUS_CONFIG                             \
    {                                               \
        .Instance = SPI4,                           \
        .bus_name = "spi4",                         \
        .irq_type = SPI4_IRQn,                           \
        .core     = SPI4_CORE,                           \
    }
#endif /* SPI4_BUS_CONFIG */
#endif /* BSP_USING_SPI4 */

#ifdef BSP_SPI4_TX_USING_DMA
#ifndef SPI4_TX_DMA_CONFIG
#define SPI4_TX_DMA_CONFIG                          \
    {                                               \
        .dma_irq_prio = SPI4_TX_DMA_IRQ_PRIO,       \
        .Instance = SPI4_TX_DMA_INSTANCE,           \
        .request = SPI4_TX_DMA_REQUEST,             \
        .dma_irq = SPI4_TX_DMA_IRQ,                 \
    }
#endif /* SPI4_TX_DMA_CONFIG */
#endif /* BSP_SPI4_TX_USING_DMA */

#ifdef BSP_SPI4_RX_USING_DMA
#ifndef SPI4_RX_DMA_CONFIG
#define SPI4_RX_DMA_CONFIG                          \
    {                                               \
        .dma_irq_prio = SPI4_RX_DMA_IRQ_PRIO,       \
        .Instance = SPI4_RX_DMA_INSTANCE,           \
        .request = SPI4_RX_DMA_REQUEST,             \
        .dma_irq = SPI4_RX_DMA_IRQ,                 \
    }
#endif /* SPI4_RX_DMA_CONFIG */
#endif /* BSP_SPI4_RX_USING_DMA */

#ifdef __cplusplus
}
#endif

#endif /*__SPI_CONFIG_H__ */

