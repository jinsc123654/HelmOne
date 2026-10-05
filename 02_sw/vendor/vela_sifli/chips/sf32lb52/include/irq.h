/****************************************************************************
 * vendor/sifli/chip/sf32lb52/include/irq.h
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

/* This file should never be included directly but, rather, only indirectly
 * through nuttx/irq.h
 */

#ifndef __VENDOR_SIFLI_CHIP_SF32LB52_INCLUDE_IRQ_H
#define __VENDOR_SIFLI_CHIP_SF32LB52_INCLUDE_IRQ_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

/* Include chip-specific IRQ definitions (including IRQ numbers) */

#include <nuttx/config.h>

#include <sys/types.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NVIC_IRQ_FIRST  (16)   /* Vector number of the first interrupt */

/* IRQ numbers.  The IRQ number corresponds vector number and hence map
 * directly to bits in the INTC.  This does, however, waste several
 * words of memory in the IRQ to handle mapping tables.
 *
 * The sf32lb52x has up to 98 peripheral interrupts (see register.h),
 * plus 16 internal ARM exceptions, so NR_IRQS must be at least 115.
 */

#define NR_IRQS (115)

/* NVIC priority levels */

#define NVIC_SYSH_PRIORITY_MIN  0xff /* All bits set in minimum priority */

#define NVIC_SYSH_PRIORITY_DEFAULT  0x40 /* Midpoint is the default */

#define NVIC_SYSH_PRIORITY_MAX   0x00 /* Zero is maximum priority */

#define NVIC_SYSH_PRIORITY_STEP 0x40 /* Three bits priority used, bits[7-6] as group */

#define NVIC_SYSH_PRIORITY_SUBSTEP  0x20 /* Three bits priority used, bit[5] as sub */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Inline functions
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __VENDOR_SIFLI_CHIP_SF32LB52_INCLUDE_IRQ_H */
