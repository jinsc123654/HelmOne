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
        .base = FLASH_BASE_ADDR,                       \
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

#endif  /* BSP_USING_QSPI */

#ifdef __cplusplus
}
#endif

#endif /*__FLASH_CONFIG_H__ */

