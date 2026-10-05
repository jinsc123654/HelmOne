/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __I2C_CONFIG_H__
#define __I2C_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2C1_CORE   CORE_ID_HCPU
#define I2C2_CORE   CORE_ID_HCPU
#define I2C3_CORE   CORE_ID_HCPU
#define I2C4_CORE   CORE_ID_HCPU
#define I2C5_CORE   CORE_ID_LCPU
#define I2C6_CORE   CORE_ID_LCPU
#define I2C7_CORE   CORE_ID_LCPU

#if defined(BSP_I2C1_USING_DMA)
#ifndef I2C1_TRX_DMA_CONFIG
#define I2C1_TRX_DMA_CONFIG                          \
    {                                                \
        .dma_irq_prio = I2C1_DMA_IRQ_PRIO,           \
        .Instance = I2C1_DMA_INSTANCE,               \
        .dma_irq = I2C1_DMA_IRQ,                     \
        .request = I2C1_DMA_REQUEST,                 \
    }
#endif
#endif

#if defined(BSP_I2C2_USING_DMA)
#ifndef I2C2_TRX_DMA_CONFIG
#define I2C2_TRX_DMA_CONFIG                          \
    {                                                \
        .dma_irq_prio = I2C2_DMA_IRQ_PRIO,           \
        .Instance = I2C2_DMA_INSTANCE,               \
        .dma_irq = I2C2_DMA_IRQ,                     \
        .request = I2C2_DMA_REQUEST,                 \
    }
#endif
#endif

#if defined(BSP_I2C3_USING_DMA)
#ifndef I2C3_TRX_DMA_CONFIG
#define I2C3_TRX_DMA_CONFIG                          \
    {                                                \
        .dma_irq_prio = I2C3_DMA_IRQ_PRIO,           \
        .Instance = I2C3_DMA_INSTANCE,               \
        .dma_irq = I2C3_DMA_IRQ,                     \
        .request = I2C3_DMA_REQUEST,                 \
    }
#endif
#endif

#if defined(BSP_I2C4_USING_DMA)
#ifndef I2C4_TRX_DMA_CONFIG
#define I2C4_TRX_DMA_CONFIG                          \
    {                                                \
        .dma_irq_prio = I2C4_DMA_IRQ_PRIO,           \
        .Instance = I2C4_DMA_INSTANCE,               \
        .dma_irq = I2C4_DMA_IRQ,                     \
        .request = I2C4_DMA_REQUEST,                 \
    }
#endif
#endif

#if defined(BSP_I2C5_USING_DMA)
#ifndef I2C5_TRX_DMA_CONFIG
#define I2C5_TRX_DMA_CONFIG                          \
    {                                                \
        .dma_irq_prio = I2C5_DMA_IRQ_PRIO,           \
        .Instance = I2C5_DMA_INSTANCE,               \
        .dma_irq = I2C5_DMA_IRQ,                     \
        .request = I2C5_DMA_REQUEST,                 \
    }
#endif
#endif

#if defined(BSP_I2C6_USING_DMA)
#ifndef I2C6_TRX_DMA_CONFIG
#define I2C6_TRX_DMA_CONFIG                          \
    {                                                \
        .dma_irq_prio = I2C6_DMA_IRQ_PRIO,           \
        .Instance = I2C6_DMA_INSTANCE,               \
        .dma_irq = I2C6_DMA_IRQ,                     \
        .request = I2C6_DMA_REQUEST,                 \
    }
#endif
#endif

#if defined(BSP_I2C7_USING_DMA)
#ifndef I2C7_TRX_DMA_CONFIG
#define I2C7_TRX_DMA_CONFIG                          \
    {                                                \
        .dma_irq_prio = I2C7_DMA_IRQ_PRIO            \
        .Instance = I2C7_DMA_INSTANCE,               \
        .dma_irq = I2C7_DMA_IRQ,                     \
        .request = I2C7_DMA_REQUEST,                 \
    }
#endif
#endif

#if defined(BSP_USING_I2C1)
#ifndef BF0_I2C1_CFG
#define BF0_I2C1_CFG                      \
    {                                     \
        .device_name = "i2c1",            \
        .Instance = I2C1,                 \
        .irq_type = I2C1_IRQn,            \
        .core     = I2C1_CORE,            \
    }
#endif
#endif

#if defined(BSP_USING_I2C2)
#ifndef BF0_I2C2_CFG
#define BF0_I2C2_CFG                      \
    {                                     \
        .device_name = "i2c2",            \
        .Instance = I2C2,                 \
        .irq_type = I2C2_IRQn,            \
        .core     = I2C2_CORE,            \
    }
#endif
#endif

#if defined(BSP_USING_I2C3)
#ifndef BF0_I2C3_CFG
#define BF0_I2C3_CFG                      \
    {                                     \
        .device_name = "i2c3",            \
        .Instance = I2C3,                 \
        .irq_type = I2C3_IRQn,            \
        .core     = I2C3_CORE,            \
    }
#endif
#endif

#if defined(BSP_USING_I2C4)
#ifndef BF0_I2C4_CFG
#define BF0_I2C4_CFG                      \
    {                                     \
        .device_name = "i2c4",            \
        .Instance = I2C4,                 \
        .irq_type = I2C4_IRQn,            \
        .core     = I2C4_CORE,            \
    }
#endif
#endif

#if defined(BSP_USING_I2C5)
#ifndef BF0_I2C5_CFG
#define BF0_I2C5_CFG                      \
    {                                     \
        .device_name = "i2c5",            \
        .Instance = I2C5,                 \
        .irq_type = I2C5_IRQn,            \
        .core     = I2C5_CORE,            \
    }
#endif
#endif

#if defined(BSP_USING_I2C6)
#ifndef BF0_I2C6_CFG
#define BF0_I2C6_CFG                      \
    {                                     \
        .device_name = "i2c6",            \
        .Instance = I2C6,                 \
        .irq_type = I2C6_IRQn,            \
        .core     = I2C6_CORE,            \
    }
#endif
#endif

#if defined(BSP_USING_I2C7)
#ifndef BF0_I2C7_CFG
#define BF0_I2C7_CFG                      \
    {                                     \
        .device_name = "i2c7",            \
        .Instance = I2C7,                 \
        .irq_type = I2C7_IRQn,            \
        .core     = I2C7_CORE,            \
    }
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* __TIM_CONFIG_H__ */

