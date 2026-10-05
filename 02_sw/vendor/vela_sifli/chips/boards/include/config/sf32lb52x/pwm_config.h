/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PWM_CONFIG_H__
#define __PWM_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWM2_CORE CORE_ID_HCPU
#define PWM3_CORE CORE_ID_HCPU
#define PWM4_CORE CORE_ID_LCPU
#define PWM5_CORE CORE_ID_LCPU
#define PWM6_CORE CORE_ID_LCPU

#ifdef BSP_USING_PWM2
#define PWM2_CONFIG                             \
{                                           \
   .tim_handle.Instance     = GPTIM1,       \
   .tim_handle.core         = PWM2_CORE,    \
   .name                    = "pwm2",       \
   .channel                 = 0             \
}
#endif /* BSP_USING_PWM2 */

#ifdef BSP_PWM2_UPDATE_USING_DMA
#define PWM2_UPDATE_DMA_CONFIG                             \
    {                                           \
       .dma_handle.Init.Priority = PWM2_UPDATE_DMA_IRQ_PRIO,           \
       .dma_handle.Instance = PWM2_UPDATE_DMA_INSTANCE,               \
       .dma_handle.Init.Request = GPTIM1_UPDATE_DMA_REQUEST,                 \
       .dma_irq = PWM2_UPDATE_DMA_IRQ,                     \
       .dma_handle_index = GPT_DMA_ID_UPDATE,                \
       .dma_handle.Init.PeriphDataAlignment    = PWM2_UPDATE_DMA_PDATAALIGN,         \
       .dma_handle.Init.MemDataAlignment   = PWM2_UPDATE_DMA_MDATAALIGN           \
    }
#endif /* BSP_PWM2_UPDATE_USING_DMA */

#ifdef BSP_PWM2_CC1_USING_DMA
#define PWM2_CC1_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM2_CC1_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM2_CC1_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM2_CC1_DMA_REQUEST,                 \
   .dma_irq = PWM2_CC1_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC1,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM2_CC1_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM2_CC1_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM2_CC1_USING_DMA */

#ifdef BSP_PWM2_CC2_USING_DMA
#define PWM2_CC2_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM2_CC2_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM2_CC2_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM2_CC2_DMA_REQUEST,                 \
   .dma_irq = PWM2_CC2_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC2,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM2_CC2_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM2_CC2_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM2_CC2_USING_DMA */
#ifdef BSP_PWM2_CC3_USING_DMA
#define PWM2_CC3_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM2_CC3_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM2_CC3_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM2_CC3_DMA_REQUEST,                 \
   .dma_irq = PWM2_CC3_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC3,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM2_CC3_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM2_CC3_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM2_CC3_USING_DMA */
#ifdef BSP_PWM2_CC4_USING_DMA
#define PWM2_CC4_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM2_CC4_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM2_CC4_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM2_CC4_DMA_REQUEST,                 \
   .dma_irq = PWM2_CC4_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC4,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM2_CC4_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM2_CC4_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM2_CC4_USING_DMA */

#ifdef BSP_USING_PWM3
#define PWM3_CONFIG                             \
{                                           \
   .tim_handle.Instance     = GPTIM2,         \
   .tim_handle.core         = PWM3_CORE,    \
   .name                    = "pwm3",       \
   .channel                 = 0             \
}
#endif /* BSP_USING_PWM3 */

#ifdef BSP_PWM3_UPDATE_USING_DMA
#define PWM3_UPDATE_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM3_UPDATE_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM3_UPDATE_DMA_INSTANCE,               \
   .dma_handle.Init.Request = GPTIM2_UPDATE_DMA_REQUEST,                 \
   .dma_irq = PWM3_UPDATE_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_UPDATE,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM3_UPDATE_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM3_UPDATE_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM3_UPDATE_USING_DMA */

#ifdef BSP_PWM3_CC1_USING_DMA
#define PWM3_CC1_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM3_CC1_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM3_CC1_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM3_CC1_DMA_REQUEST,                 \
   .dma_irq = PWM3_CC1_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC1,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM3_CC1_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM3_CC1_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM3_CC1_USING_DMA */

#ifdef BSP_PWM3_CC2_USING_DMA
#define PWM3_CC2_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM3_CC2_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM3_CC2_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM3_CC2_DMA_REQUEST,                 \
   .dma_irq = PWM3_CC2_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC2,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM3_CC2_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM3_CC2_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM3_CC2_USING_DMA */
#ifdef BSP_PWM3_CC3_USING_DMA
#define PWM3_CC3_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM3_CC3_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM3_CC3_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM3_CC3_DMA_REQUEST,                 \
   .dma_irq = PWM3_CC3_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC3,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM3_CC3_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM3_CC3_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM3_CC3_USING_DMA */
#ifdef BSP_PWM3_CC4_USING_DMA
#define PWM3_CC4_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM3_CC4_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM3_CC4_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM3_CC4_DMA_REQUEST,                 \
   .dma_irq = PWM3_CC4_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC4,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM3_CC4_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM3_CC4_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM3_CC4_USING_DMA */

#ifdef BSP_USING_PWM4
#define PWM4_CONFIG                             \
{                                           \
   .tim_handle.Instance     = GPTIM3,         \
   .tim_handle.core         = PWM4_CORE,    \
   .name                    = "pwm4",       \
   .channel                 = 0             \
}
#endif /* BSP_USING_PWM4 */

#ifdef BSP_PWM4_UPDATE_USING_DMA
#define PWM4_UPDATE_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM4_UPDATE_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM4_UPDATE_DMA_INSTANCE,               \
   .dma_handle.Init.Request = GPTIM3_UPDATE_DMA_REQUEST,                 \
   .dma_irq = PWM4_UPDATE_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_UPDATE,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM4_UPDATE_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM4_UPDATE_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM4_UPDATE_USING_DMA */

#ifdef BSP_PWM4_CC1_USING_DMA
#define PWM4_CC1_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM4_CC1_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM4_CC1_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM4_CC1_DMA_REQUEST,                 \
   .dma_irq = PWM4_CC1_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC1,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM4_CC1_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM4_CC1_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM4_CC1_USING_DMA */

#ifdef BSP_PWM4_CC2_USING_DMA
#define PWM4_CC2_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM4_CC2_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM4_CC2_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM4_CC2_DMA_REQUEST,                 \
   .dma_irq = PWM4_CC2_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC2,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM4_CC2_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM4_CC2_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM4_CC2_USING_DMA */
#ifdef BSP_PWM4_CC3_USING_DMA
#define PWM4_CC3_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM4_CC3_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM4_CC3_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM4_CC3_DMA_REQUEST,                 \
   .dma_irq = PWM4_CC3_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC3,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM4_CC3_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM4_CC3_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM4_CC3_USING_DMA */
#ifdef BSP_PWM4_CC4_USING_DMA
#define PWM4_CC4_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM4_CC4_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM4_CC4_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM4_CC4_DMA_REQUEST,                 \
   .dma_irq = PWM4_CC4_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC4,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM4_CC4_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM4_CC4_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM4_CC4_USING_DMA */

#ifdef BSP_USING_PWM5
#define PWM5_CONFIG                             \
{                                           \
   .tim_handle.Instance     = GPTIM4,         \
   .tim_handle.core         = PWM5_CORE,    \
   .name                    = "pwm5",       \
   .channel                 = 0             \
}
#endif /* BSP_USING_PWM5 */

#ifdef BSP_PWM5_UPDATE_USING_DMA
#define PWM5_UPDATE_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM5_UPDATE_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM5_UPDATE_DMA_INSTANCE,               \
   .dma_handle.Init.Request = GPTIM4_UPDATE_DMA_REQUEST,                 \
   .dma_irq = PWM5_UPDATE_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_UPDATE,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM5_UPDATE_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM5_UPDATE_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM5_UPDATE_USING_DMA */

#ifdef BSP_PWM5_CC1_USING_DMA
#define PWM5_CC1_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM5_CC1_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM5_CC1_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM5_CC1_DMA_REQUEST,                 \
   .dma_irq = PWM5_CC1_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC1,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM5_CC1_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM5_CC1_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM5_CC1_USING_DMA */

#ifdef BSP_PWM5_CC2_USING_DMA
#define PWM5_CC2_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM5_CC2_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM5_CC2_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM5_CC2_DMA_REQUEST,                 \
   .dma_irq = PWM5_CC2_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC2,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM5_CC2_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM5_CC2_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM5_CC2_USING_DMA */
#ifdef BSP_PWM5_CC3_USING_DMA
#define PWM5_CC3_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM5_CC3_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM5_CC3_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM5_CC3_DMA_REQUEST,                 \
   .dma_irq = PWM5_CC3_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC3,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM5_CC3_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM5_CC3_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM5_CC3_USING_DMA */
#ifdef BSP_PWM5_CC4_USING_DMA
#define PWM5_CC4_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM5_CC4_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM5_CC4_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM5_CC4_DMA_REQUEST,                 \
   .dma_irq = PWM5_CC4_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC4,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM5_CC4_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM5_CC4_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM5_CC4_USING_DMA */

#ifdef BSP_USING_PWM6
#define PWM6_CONFIG                             \
{                                           \
   .tim_handle.Instance     = GPTIM5,         \
   .tim_handle.core         = PWM6_CORE,    \
   .name                    = "pwm6",       \
   .channel                 = 0             \
}
#endif /* BSP_USING_PWM5 */

#ifdef BSP_PWM6_UPDATE_USING_DMA
#define PWM6_UPDATE_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM6_UPDATE_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM6_UPDATE_DMA_INSTANCE,               \
   .dma_handle.Init.Request = GPTIM5_UPDATE_DMA_REQUEST,                 \
   .dma_irq = PWM6_UPDATE_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_UPDATE,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM6_UPDATE_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM6_UPDATE_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM6_UPDATE_USING_DMA */

#ifdef BSP_PWM6_CC1_USING_DMA
#define PWM6_CC1_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM6_CC1_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM6_CC1_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM6_CC1_DMA_REQUEST,                 \
   .dma_irq = PWM6_CC1_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC1,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM6_CC1_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM6_CC1_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM6_CC1_USING_DMA */

#ifdef BSP_PWM6_CC2_USING_DMA
#define PWM6_CC2_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM6_CC2_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM6_CC2_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM6_CC2_DMA_REQUEST,                 \
   .dma_irq = PWM6_CC2_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC2,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM6_CC2_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM6_CC2_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM6_CC2_USING_DMA */
#ifdef BSP_PWM6_CC3_USING_DMA
#define PWM6_CC3_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM6_CC3_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM6_CC3_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM6_CC3_DMA_REQUEST,                 \
   .dma_irq = PWM6_CC3_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC3,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM6_CC3_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM6_CC3_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM6_CC3_USING_DMA */
#ifdef BSP_PWM6_CC4_USING_DMA
#define PWM6_CC4_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWM6_CC4_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWM6_CC4_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWM6_CC4_DMA_REQUEST,                 \
   .dma_irq = PWM6_CC4_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC4,                \
   .dma_handle.Init.PeriphDataAlignment    = PWM6_CC4_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWM6_CC4_DMA_MDATAALIGN           \
}
#endif /* BSP_PWM6_CC4_USING_DMA */

#ifdef BSP_USING_PWMA1
#define PWMA1_CONFIG                             \
{                                           \
   .tim_handle.Instance     = (GPT_TypeDef *)ATIM1,         \
   .tim_handle.core         = ATIM1_CORE,    \
   .name                    = "pwma1",       \
   .channel                 = 0             \
}
#endif /* BSP_USING_PWM_A1 */

#ifdef BSP_PWMA1_UPDATE_USING_DMA
#define PWMA1_UPDATE_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA1_UPDATE_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA1_UPDATE_DMA_INSTANCE,               \
   .dma_handle.Init.Request = ATIM1_UPDATE_DMA_REQUEST,                 \
   .dma_irq = PWMA1_UPDATE_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_UPDATE,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA1_UPDATE_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA1_UPDATE_DMA_MDATAALIGN           \
}
#endif /* BSP_PWMA1_UPDATE_USING_DMA */

#ifdef BSP_PWMA1_CC1_USING_DMA
#define PWMA1_CC1_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA1_CC1_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA1_CC1_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA1_CC1_DMA_REQUEST,                 \
   .dma_irq = PWMA1_CC1_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC1,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA1_CC1_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA1_CC1_DMA_MDATAALIGN            \
}
#endif /* BSP_PWMA1_CC1_USING_DMA */
#ifdef BSP_PWMA1_CC2_USING_DMA
#define PWMA1_CC2_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA1_CC2_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA1_CC2_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA1_CC2_DMA_REQUEST,                 \
   .dma_irq = PWMA1_CC2_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC2,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA1_CC2_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA1_CC2_DMA_MDATAALIGN
}
#endif /* BSP_PWMA1_CC2_USING_DMA */
#ifdef BSP_PWMA1_CC3_USING_DMA
#define PWMA1_CC3_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA1_CC3_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA1_CC3_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA1_CC3_DMA_REQUEST,                 \
   .dma_irq = PWMA1_CC3_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC3,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA1_CC3_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA1_CC3_DMA_MDATAALIGN           \
}
#endif /* BSP_PWMA1_CC3_USING_DMA */
#ifdef BSP_PWMA1_CC4_USING_DMA
#define PWMA1_CC4_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA1_CC4_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA1_CC4_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA1_CC4_DMA_REQUEST,                 \
   .dma_irq = PWMA1_CC4_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC4,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA1_CC4_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA1_CC4_DMA_MDATAALIGN
}
#endif /* BSP_PWMA1_CC4_USING_DMA */

#ifdef BSP_USING_PWMA2
#define PWMA2_CONFIG                             \
{                                           \
   .tim_handle.Instance     = (GPT_TypeDef *)ATIM2,         \
   .tim_handle.core         = ATIM2_CORE,    \
   .name                    = "pwma2",       \
   .channel                 = 0             \
}
#endif /* BSP_USING_PWM_A2 */

#ifdef BSP_PWMA2_UPDATE_USING_DMA
#define PWMA2_UPDATE_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA2_UPDATE_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA2_UPDATE_DMA_INSTANCE,               \
   .dma_handle.Init.Request = ATIM2_UPDATE_DMA_REQUEST,                 \
   .dma_irq = PWMA2_UPDATE_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_UPDATE,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA2_UPDATE_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA2_UPDATE_DMA_MDATAALIGN           \
}
#endif /* BSP_PWMA2_UPDATE_USING_DMA */

#ifdef BSP_PWMA2_CC1_USING_DMA
#define PWMA2_CC1_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA2_CC1_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA2_CC1_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA2_CC1_DMA_REQUEST,                 \
   .dma_irq = PWMA2_CC1_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC1,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA2_CC1_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA2_CC1_DMA_MDATAALIGN
}
#endif /* BSP_PWMA2_CC1_USING_DMA */
#ifdef BSP_PWMA2_CC2_USING_DMA
#define PWMA2_CC2_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA2_CC2_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA2_CC2_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA2_CC2_DMA_REQUEST,                 \
   .dma_irq = PWMA2_CC2_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC2,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA2_CC2_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA2_CC2_DMA_MDATAALIGN
}
#endif /* BSP_PWMA2_CC2_USING_DMA */
#ifdef BSP_PWMA2_CC3_USING_DMA
#define PWMA2_CC3_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA2_CC3_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA2_CC3_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA2_CC3_DMA_REQUEST,                 \
   .dma_irq = PWMA2_CC3_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC3,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA2_CC3_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA2_CC3_DMA_MDATAALIGN
}
#endif /* BSP_PWMA2_CC3_USING_DMA */
#ifdef BSP_PWMA2_CC4_USING_DMA
#define PWMA2_CC4_DMA_CONFIG                             \
{                                           \
   .dma_handle.Init.Priority = PWMA2_CC4_DMA_IRQ_PRIO,           \
   .dma_handle.Instance = PWMA2_CC4_DMA_INSTANCE,               \
   .dma_handle.Init.Request = PWMA2_CC4_DMA_REQUEST,                 \
   .dma_irq = PWMA2_CC4_DMA_IRQ,                     \
   .dma_handle_index = GPT_DMA_ID_CC4,                \
   .dma_handle.Init.PeriphDataAlignment    = PWMA2_CC4_DMA_PDATAALIGN,         \
   .dma_handle.Init.MemDataAlignment   = PWMA2_CC4_DMA_MDATAALIGN
}
#endif /* BSP_PWMA2_CC4_USING_DMA */

#ifdef BSP_USING_PWM_LPTIM1
#define PWM_LPTIM1_CONFIG                       \
{                                           \
   .tim_handle.Instance     = hwp_lptim1,   \
   .name                    = "pwmlp1",       \
}

#endif /* BSP_USING_PWM_LPTIM1 */

#ifdef BSP_USING_PWM_LPTIM2
#define PWM_LPTIM2_CONFIG                       \
{                                           \
   .tim_handle.Instance     = hwp_lptim2,   \
   .name                    = "pwmlp2",       \
}
#endif /* BSP_USING_PWM_LPTIM2 */

#ifdef BSP_USING_PWM_LPTIM3
#define PWM_LPTIM3_CONFIG                       \
{                                           \
   .tim_handle.Instance     = hwp_lptim3,   \
   .name                    = "pwmlp3",       \
}
#endif /* BSP_USING_PWM_LPTIM3 */

#ifdef __cplusplus
}
#endif

#endif /* __PWM_CONFIG_H__ */

