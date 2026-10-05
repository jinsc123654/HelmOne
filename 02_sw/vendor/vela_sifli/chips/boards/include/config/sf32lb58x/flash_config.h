/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FLASH_CONFIG_H__
#define __FLASH_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BSP_USING_MPI

#ifdef BSP_ENABLE_QSPI1
#if ((BSP_QSPI1_MODE == SPI_MODE_NOR)||(BSP_QSPI1_MODE == SPI_MODE_NAND))

#ifndef FLASH1_CONFIG
#define FLASH1_CONFIG                                  \
    {                                                  \
        .Instance = FLASH1,                            \
        .line = 2,                                     \
        .base = FLASH1_BASE_ADDR,                       \
        .msize = BSP_QSPI1_MEM_SIZE,                   \
        .SpiMode = BSP_QSPI1_MODE,                     \
    }

#endif  /* FLASH_CONFIG1 */

#ifdef BSP_QSPI1_USING_DMA
#ifndef FLASH1_DMA_CONFIG
#define FLASH1_DMA_CONFIG                              \
    {                                                  \
        .dma_irq_prio = FLASH1_DMA_IRQ_PRIO,           \
        .Instance = FLASH1_DMA_INSTANCE,               \
        .dma_irq = FLASH1_DMA_IRQ,                     \
        .request = FLASH1_DMA_REQUEST,                 \
    }

#endif  /* FLASH1_DMA_CONFIG */
#endif  /* BSP_QSPI1_USING_DMA */

#endif /*#if ((BSP_QSPI1_MODE == SPI_MODE_NOR)||(BSP_QSPI1_MODE == SPI_MODE_NAND))*/
#endif  /* BSP_ENABLE_QSPI1 */

#ifdef BSP_ENABLE_QSPI2
#if ((BSP_QSPI2_MODE == SPI_MODE_NOR)||(BSP_QSPI2_MODE == SPI_MODE_NAND))

#ifndef FLASH2_CONFIG
#define FLASH2_CONFIG                                  \
    {                                                  \
        .Instance = FLASH2,                            \
        .line = 2,                                     \
        .base = FLASH2_BASE_ADDR,                      \
        .msize = BSP_QSPI2_MEM_SIZE,                   \
        .SpiMode = BSP_QSPI2_MODE,                     \
    }

#endif  /* FLASH_CONFIG2 */

#ifdef BSP_QSPI2_USING_DMA
#ifndef FLASH2_DMA_CONFIG
#define FLASH2_DMA_CONFIG                              \
    {                                                  \
        .dma_irq_prio = FLASH2_DMA_IRQ_PRIO,           \
        .Instance = FLASH2_DMA_INSTANCE,               \
        .dma_irq = FLASH2_DMA_IRQ,                     \
        .request = FLASH2_DMA_REQUEST,                 \
    }

#endif  /* FLASH2_DMA_CONFIG */
#endif  /* BSP_QSPI2_USING_DMA */

#endif /*#if ((BSP_QSPI2_MODE == SPI_MODE_NOR)||(BSP_QSPI2_MODE == SPI_MODE_NAND))*/
#endif  /* BSP_ENABLE_QSPI2 */

#ifdef BSP_ENABLE_QSPI3
#if ((BSP_QSPI3_MODE == SPI_MODE_NOR)||(BSP_QSPI3_MODE == SPI_MODE_NAND))

#ifndef FLASH3_CONFIG
#define FLASH3_CONFIG                                  \
    {                                                  \
        .Instance = FLASH3,                            \
        .line = 2,                                     \
        .base = FLASH3_BASE_ADDR,                      \
        .msize = BSP_QSPI3_MEM_SIZE,                   \
        .SpiMode = BSP_QSPI3_MODE,                     \
    }

#endif  /* FLASH_CONFIG3 */

#ifdef BSP_QSPI3_USING_DMA
#ifndef FLASH3_DMA_CONFIG
#define FLASH3_DMA_CONFIG                              \
    {                                                  \
        .dma_irq_prio = FLASH3_DMA_IRQ_PRIO,           \
        .Instance = FLASH3_DMA_INSTANCE,               \
        .dma_irq = FLASH3_DMA_IRQ,                     \
        .request = FLASH3_DMA_REQUEST,                 \
    }

#endif  /* FLASH3_DMA_CONFIG */
#endif  /* BSP_QSPI3_USING_DMA */

#endif /*#if ((BSP_QSPI3_MODE == SPI_MODE_NOR)||(BSP_QSPI3_MODE == SPI_MODE_NAND))*/

#endif  /* BSP_ENABLE_QSPI3 */

#ifdef BSP_ENABLE_QSPI4
#if ((BSP_QSPI4_MODE == SPI_MODE_NOR)||(BSP_QSPI4_MODE == SPI_MODE_NAND))

#ifndef FLASH4_CONFIG
#define FLASH4_CONFIG                                  \
    {                                                  \
        .Instance = FLASH4,                            \
        .line = 2,                                     \
        .base = FLASH4_BASE_ADDR,                      \
        .msize = BSP_QSPI4_MEM_SIZE,                   \
        .SpiMode = BSP_QSPI4_MODE,                     \
    }

#endif  /* FLASH_CONFIG4 */

#ifdef BSP_QSPI4_USING_DMA
#ifndef FLASH4_DMA_CONFIG
#define FLASH4_DMA_CONFIG                              \
    {                                                  \
        .dma_irq_prio = FLASH4_DMA_IRQ_PRIO,           \
        .Instance = FLASH4_DMA_INSTANCE,               \
        .dma_irq = FLASH4_DMA_IRQ,                     \
        .request = FLASH4_DMA_REQUEST,                 \
    }

#endif  /* FLASH4_DMA_CONFIG */
#endif  /* BSP_QSPI4_USING_DMA */

#endif /*#if ((BSP_QSPI4_MODE == SPI_MODE_NOR)||(BSP_QSPI3_MODE == SPI_MODE_NAND))*/

#endif  /* BSP_ENABLE_QSPI4 */

#ifdef BSP_ENABLE_QSPI5
#ifndef FLASH5_CONFIG
#define FLASH5_CONFIG                                  \
    {                                                  \
        .Instance = FLASH5,                            \
        .line = 2,                                     \
        .base = FLASH5_BASE_ADDR,                      \
        .msize = BSP_QSPI5_MEM_SIZE,                   \
        .SpiMode = BSP_QSPI5_MODE,                     \
    }
#endif  /* FLASH_CONFIG5 */
#endif //#ifdef BSP_ENABLE_QSPI5

#ifndef FLASH5_DMA_CONFIG
#define FLASH5_DMA_CONFIG                              \
    {                                                  \
        .dma_irq_prio = FLASH5_DMA_IRQ_PRIO,           \
        .Instance = FLASH5_DMA_INSTANCE,               \
        .dma_irq = FLASH5_DMA_IRQ,                     \
        .request = FLASH5_DMA_REQUEST,                 \
    }

#endif  /* FLASH4_DMA_CONFIG */

#endif  /* BSP_USING_QSPI */

#ifdef __cplusplus
}
#endif

#endif /*__FLASH_CONFIG_H__ */

