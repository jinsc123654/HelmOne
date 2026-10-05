/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sifli_irq.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <arch/irq.h>
#include <arch/arm_m/nvicpri.h>

#include "nvic.h"
#include "ram_vectors.h"
#include "arm_internal.h"

#include "bf0_hal.h"

/* Get a 32-bit version of the default priority */    
#define DEFPRIORITY32 \
      (NVIC_SYSH_PRIORITY_DEFAULT << 24 | \
       NVIC_SYSH_PRIORITY_DEFAULT << 16 | \
       NVIC_SYSH_PRIORITY_DEFAULT << 8  | \
       NVIC_SYSH_PRIORITY_DEFAULT)

/* Given the address of a NVIC ENABLE register, this is the offset to
 * the corresponding CLEAR ENABLE register.
 */
#define NVIC_ENA_OFFSET    (0)
#define NVIC_CLRENA_OFFSET (NVIC_IRQ0_31_CLEAR - NVIC_IRQ0_31_ENABLE)


static inline void sifli_prioritize_syscall(int priority)
{
  uint32_t regval;

  /* SVCALL is system handler 11 */

  regval  = getreg32(NVIC_SYSH8_11_PRIORITY);
  regval &= ~NVIC_SYSH_PRIORITY_PR11_MASK;
  regval |= (priority << NVIC_SYSH_PRIORITY_PR11_SHIFT);
  putreg32(regval, NVIC_SYSH8_11_PRIORITY);
}

#ifdef CONFIG_DEBUG_FEATURES

static int sifli_nmi(int irq, void *context, void *arg)
{
  up_irq_save();
  arm_lowprintf("PANIC!!! NMI received\n");
  PANIC();
  return 0;
}

static int sifli_dbgmonitor(int irq, void *context, void *arg)
{
  up_irq_save();
  arm_lowprintf("PANIC!!! Debug Monitor received\n");
  PANIC();
  return 0;
}

static int sifli_reserved(int irq, void *context, void *arg)
{
  up_irq_save();
  arm_lowprintf("PANIC!!! Reserved interrupt\n");
  PANIC();
  return 0;
}
#endif

/****************************************************************************
 * Name: sifli_irqinfo
 *
 * Description:
 *   Given an IRQ number, provide the register and bit setting to enable or
 *   disable the irq.
 *
 ****************************************************************************/

static int sifli_irqinfo(int irq, uintptr_t *regaddr, uint32_t *bit,
                         uintptr_t offset)
{
    int n;

    /* Check IRQ range, return error instead of assert to avoid hanging */
    if (irq < (NonMaskableInt_IRQn+16) || irq >= NR_IRQS)
    {
        arm_lowprintf("sifli_irqinfo: ERROR - irq out of range!\n");
        return ERROR;
    }

    /* Check for external interrupt */

    if (irq >= NVIC_IRQ_FIRST)
    {
        n = irq - NVIC_IRQ_FIRST;
        *regaddr = NVIC_IRQ_ENABLE(n) + offset;
        *bit     = (uint32_t)1 << (n & 0x1f);
    }

    /* Handle processor exceptions.  Only a few can be disabled */
    else
    {
        *regaddr = NVIC_SYSHCON;
        if (irq == MemoryManagement_IRQn+16)
            *bit = NVIC_SYSHCON_MEMFAULTENA;
        else if (irq == BusFault_IRQn+16)
            *bit = NVIC_SYSHCON_BUSFAULTENA;
        else if (irq == UsageFault_IRQn+16)
            *bit = NVIC_SYSHCON_USGFAULTENA;
        else if (irq == SysTick_IRQn+16)
        {
            *regaddr = NVIC_SYSTICK_CTRL;
            *bit = NVIC_SYSTICK_CTRL_ENABLE;
        }
        else
            return ERROR; /* Invalid or unsupported exception */
    }
    return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_irqinitialize
 ****************************************************************************/
void up_irqinitialize(void)
{
    uint32_t regaddr;
    int num_priority_registers;
    int i;
    
    /* Disable all interrupts */
    
    for (i = 0; i < NR_IRQS - NVIC_IRQ_FIRST; i += 32)
        putreg32(0xffffffff, NVIC_IRQ_CLEAR(i));
    
    /* Set the NVIC vector table location to point to our vector table.
     * The vector table location may be in FLASH or RAM depending on the
     * configuration.
     */
    putreg32((uint32_t)_vectors, NVIC_VECTAB);
    
#ifdef CONFIG_ARCH_RAMVECTORS
    /* If CONFIG_ARCH_RAMVECTORS is defined, then we are using a RAM-based
    * vector table that requires special initialization.
    */    
    up_ramvec_initialize();
#endif
    
      /* Set all interrupts (and exceptions) to the default priority */
    
      putreg32(DEFPRIORITY32, NVIC_SYSH4_7_PRIORITY);
      putreg32(DEFPRIORITY32, NVIC_SYSH8_11_PRIORITY);
      putreg32(DEFPRIORITY32, NVIC_SYSH12_15_PRIORITY);
    
    //   __asm("B .");


      /* The NVIC ICTR register (bits 0-4) holds the number of of interrupt
       * lines that the NVIC supports:
       *
       *  0 -> 32 interrupt lines,  8 priority registers
       *  1 -> 64 "       " "   ", 16 priority registers
       *  2 -> 96 "       " "   ", 32 priority registers
       *  ...
       */    
      num_priority_registers = (getreg32(NVIC_ICTR) + 1) * 8;
    
      /* Now set all of the interrupt lines to the default priority */
    
      regaddr = NVIC_IRQ0_3_PRIORITY;
      while (num_priority_registers--)
      {
          putreg32(DEFPRIORITY32, regaddr);
          regaddr += 4;
      }
    
      /* Attach the SVCall and Hard Fault exception handlers.  The SVCall
       * exception is used for performing context switches; The Hard Fault
       * must also be caught because a SVCall may show up as a Hard Fault
       * under certain conditions.
       */
    
      irq_attach(SVCall_IRQn+16, arm_svcall, NULL);
      irq_attach(HardFault_IRQn+16, arm_hardfault, NULL);
    
      /* Set the priority of the SVCall interrupt */
      /* CRITICAL: Always set SVC priority to be higher than BASEPRI level
       * to avoid SVC being blocked and escalating to HardFault */
      sifli_prioritize_syscall(NVIC_SYSH_SVCALL_PRIORITY);
    
      /* If the MPU is enabled, then attach and enable the Memory Management
       * Fault handler.
       */
    
#ifdef CONFIG_ARM_MPU
      irq_attach(MemoryManagement_IRQn+16, arm_memfault, NULL);
      up_enable_irq(MemoryManagement_IRQn+16);
#endif
    
      /* Attach all other processor exceptions (except reset and sys tick) */
    
#ifdef CONFIG_DEBUG_FEATURES
      irq_attach(NonMaskableInt_IRQn+16, sifli_nmi, NULL);
#ifndef CONFIG_ARM_MPU
      irq_attach(MemoryManagement_IRQn+16, arm_memfault, NULL);
#endif
      irq_attach(BusFault_IRQn+16, arm_busfault, NULL);
      irq_attach(UsageFault_IRQn+16, arm_usagefault, NULL);
      irq_attach(DebugMonitor_IRQn+16, sifli_dbgmonitor, NULL);
      irq_attach(0, sifli_reserved, NULL);
#endif
        
#ifndef CONFIG_SUPPRESS_INTERRUPTS
    
      /* And finally, enable interrupts */
    
      up_irq_enable();
#endif

}

/****************************************************************************
 * Name: up_disable_irq
 *
 * Description:
 *   Disable the IRQ specified by 'irq'
 *
 ****************************************************************************/

void up_disable_irq(int irq)
{
    uintptr_t regaddr;
    uint32_t regval;
    uint32_t bit;

    if (sifli_irqinfo(irq, &regaddr, &bit, NVIC_CLRENA_OFFSET) == 0)
    {
        /* Modify the appropriate bit in the register to disable the interrupt.
         * For normal interrupts, we need to set the bit in the associated
         * Interrupt Clear Enable register.  For other exceptions, we need to
         * clear the bit in the System Handler Control and State Register.
         */

        if (irq >= NVIC_IRQ_FIRST)
            putreg32(bit, regaddr);
        else
        {
            regval  = getreg32(regaddr);
            regval &= ~bit;
            putreg32(regval, regaddr);
        }
    }

}

/****************************************************************************
 * Name: up_enable_irq
 *
 * Description:
 *   Enable the IRQ specified by 'irq'
 *
 ****************************************************************************/

void up_enable_irq(int irq)
{
    uintptr_t regaddr;
    uint32_t regval;
    uint32_t bit;

    if (sifli_irqinfo(irq, &regaddr, &bit, NVIC_ENA_OFFSET) == 0)
    {

        /* Modify the appropriate bit in the register to enable the interrupt.
        * For normal interrupts, we need to set the bit in the associated
        * Interrupt Set Enable register.  For other exceptions, we need to
        * set the bit in the System Handler Control and State Register.
        */

        if (irq >= NVIC_IRQ_FIRST)
        {
            /* Check if interrupt is already pending */
            int n = irq - NVIC_IRQ_FIRST;
            uint32_t pend_reg = NVIC_IRQ_PEND(n);
            uint32_t pend_bit = (uint32_t)1 << (n & 0x1f);
            uint32_t is_pending = getreg32(pend_reg) & pend_bit;
            
            if (is_pending)
            {
                /* Clear the pending interrupt before enabling */
                putreg32(pend_bit, NVIC_IRQ_CLRPEND(n));
            }
            
            putreg32(bit, regaddr);
            
            /* Add memory barrier to ensure write completes */
            __asm__ __volatile__ ("dsb" : : : "memory");
            __asm__ __volatile__ ("isb" : : : "memory");
        }
        else
        {
            regval  = getreg32(regaddr);
            regval |= bit;
            putreg32(regval, regaddr);
        }
    }
}

/****************************************************************************
 * Name: arm_ack_irq
 *
 * Description:
 *   Acknowledge the IRQ
 *
 ****************************************************************************/

void arm_ack_irq(int irq)
{
}

/****************************************************************************
 * Name: up_prioritize_irq
 *
 * Description:
 *   Set the priority of an IRQ.
 *
 *   Since this API is not supported on all architectures, it should be
 *   avoided in common implementations where possible.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_IRQPRIO
int up_prioritize_irq(int irq, int priority)
{
    uint32_t regaddr;
    uint32_t regval;
    int shift;
    
    DEBUGASSERT(irq >= MemoryManagement_IRQn+16 && irq < NR_IRQS &&
                (unsigned)priority <= NVIC_SYSH_PRIORITY_MIN);
    
    if (irq < NVIC_IRQ_FIRST)
    {
        /* NVIC_SYSH_PRIORITY() maps {0..15} to one of three priority
         * registers (0-3 are invalid)
         */
    
        regaddr = NVIC_SYSH_PRIORITY(irq);
        irq    -= 4;
    }
    else
    {
        /* NVIC_IRQ_PRIORITY() maps {0..} to one of many priority registers */
    
        irq    -= NVIC_IRQ_FIRST;
        regaddr = NVIC_IRQ_PRIORITY(irq);
    }
    
    regval      = getreg32(regaddr);
    shift       = ((irq & 3) << 3);
    regval     &= ~(0xff << shift);
    regval     |= (priority << shift);
    putreg32(regval, regaddr);    
    return OK;

}
#endif

void up_irqinitialize_from_sleep(void)
{
    putreg32(DEFPRIORITY32, NVIC_SYSH4_7_PRIORITY);
    putreg32(DEFPRIORITY32, NVIC_SYSH8_11_PRIORITY);
    putreg32(DEFPRIORITY32, NVIC_SYSH12_15_PRIORITY);

#ifdef CONFIG_ARMV8M_USEBASEPRI
    sifli_prioritize_syscall(NVIC_SYSH_SVCALL_PRIORITY);
#endif    
}
