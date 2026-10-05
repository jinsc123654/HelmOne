/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef IPC_OS_PORT_H
#define IPC_OS_PORT_H

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "bf0_hal.h"
#include "sfconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define os_interrupt_disable() enter_critical_section()
#define os_interrupt_enable(mask) leave_critical_section(mask)

#define os_interrupt_enter()
#define os_interrupt_exit()

#ifdef SOC_BF0_HCPU
int LCPU2HCPU_IRQHandler(int irq, FAR void *context, FAR void *arg);
static inline void os_interrupt_start(IRQn_Type irq_number, uint32_t priority,
                                      uint32_t sub_priority)
{
    HAL_NVIC_SetPriority(irq_number, priority, sub_priority);
    irq_attach(NX_IRQ(irq_number), LCPU2HCPU_IRQHandler, NULL);
    up_enable_irq(NX_IRQ(irq_number));
}
#elif defined(SOC_BF0_LCPU)
int HCPU2LCPU_IRQHandler(int irq, FAR void *context, FAR void *arg);
static inline void os_interrupt_start(IRQn_Type irq_number, uint32_t priority,
                                      uint32_t sub_priority)
{
    HAL_NVIC_SetPriority(irq_number, priority, sub_priority);
    irq_attach(NX_IRQ(irq_number), HCPU2LCPU_IRQHandler, NULL);
    up_enable_irq(NX_IRQ(irq_number));
}
#else
#error "Invalid core"
#endif

static inline void os_interrupt_stop(IRQn_Type irq_number)
{
    up_disable_irq(NX_IRQ(irq_number));
}

#ifdef __cplusplus
}
#endif

#endif
