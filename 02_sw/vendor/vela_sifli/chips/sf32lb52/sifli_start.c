/****************************************************************************
 * vendor/sifli/chip/sf32lb52/sifli_start.c
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

#include <stdarg.h>
#include <stdio.h>

#include <nuttx/arch.h>
#include <nuttx/init.h>
#include <nuttx/cache.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "system_bf0_ap.h"
#include "bf0_hal.h"

void arm_earlyserialinit(void);

/* SiFli SDK system init helpers (drivers/cmsis/.../system_bf0_ap.c).
 * mpu_config() programs all MPU regions for SF32LB52 (PSRAM CBUS code at
 * 0x10000000 cacheable/executable, SRAM non-cacheable, PSRAM SBUS data
 * write-through, peripherals device/XN, NAND window, ...) and then enables
 * the I/D caches. The SDK app calls this from SystemInit() before scatter
 * load and main(); NuttX has its own __start and must call it explicitly,
 * otherwise we run from PSRAM with default MPU attributes and no cache,
 * diverging from the (working) SDK configuration.
 */
extern void mpu_config(void);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* .data is positioned first in the primary RAM followed immediately by .bss.
 * The IDLE thread stack lies just after .bss and has size give by
 * CONFIG_IDLETHREAD_STACKSIZE;  The heap then begins just after the IDLE.
 * ARM EABI requires 64 bit stack alignment.
 */

#define HEAP_BASE      ((uintptr_t)_ebss + CONFIG_IDLETHREAD_STACKSIZE)

extern uint32_t _siramfunc;
extern uint32_t _sramfunc;
extern uint32_t _eramfunc;

void arm_lowputs(const char *str)
{
  while (*str)
    {
      arm_lowputc(*str++);
    }
}

/* Early printf implementation */
int arm_lowprintf(const char *fmt, ...)
{
  va_list ap;
  char buf[256];
  int ret;

  /* Format the message */
  va_start(ap, fmt);
  ret = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  /* Output the formatted string */
  arm_lowputs(buf);

  return ret;
}

/* Early/critical-context logger used by chip debug override. */
int sifli_arch_syslog(int priority, const char *fmt, ...)
{
  va_list ap;
  char buf[256];
  int ret;

  /* Format the message */
  va_start(ap, fmt);
  ret = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  /* Output the formatted string */
  arm_lowputs(buf);

  return ret;
}

#ifdef CONFIG_DEBUG_FEATURES
#  define showprogress(c) arm_lowputc(c)
#else
#  define showprogress(c)
#endif



/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * ROM Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* g_idle_topstack: _sbss is the start of the BSS region as defined by the
 * linker script. _ebss lies at the end of the BSS region. The idle task
 * stack starts at the end of BSS and is of size CONFIG_IDLETHREAD_STACKSIZE.
 * The IDLE thread is the thread that the system boots on and, eventually,
 * becomes the IDLE, do nothing task that runs only when there is nothing
 * else to run.  The heap continues from there until the end of memory.
 * g_idle_topstack is a read-only variable the provides this computed
 * address.
 */

const uintptr_t g_idle_topstack = HEAP_BASE;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: __sifli_start
 ****************************************************************************/

void __start(void)
{
  uint32_t *dest;
  const uint32_t *src;

  /* The secondary bootloader already configured USART1 (it printed "SFBL").
   * arm_lowputc() writes directly to USART1->TDR, so we can emit a byte here
   * WITHOUT any HAL/clock init. This is the earliest possible proof that the
   * bootloader actually jumped into NuttX. Do NOT call arm_earlyserialinit()
   * yet: HAL_UART_Init() needs the HAL clock tree (HAL_Init) which has not
   * run, and would compute a bad baud divisor / HAL_ASSERT and hang here.
   */
  arm_lowputc('A'); /* reached __start (using bootloader's UART1 setup) */

  /* Disable all interrupts at the very beginning to prevent any ISR
   * from firing during initialization. This is critical because HAL_Init()
   * and other early initialization code may trigger hardware interrupts
   * before NuttX interrupt system is ready.
   */
  __asm volatile ("cpsid i" : : : "memory");

  /* Configure Vector Table Offset Register (VTOR) for Cortex-M33.
   * NAND boot: vectors at PSRAM1 CBUS (0x10000000), loaded by bootloader.
   */
#define SCB_VTOR (*((volatile uint32_t *)0xE000ED08))
  SCB_VTOR = (uint32_t)_vectors;

  /* Configure FPU before any floating point operations */

  arm_fpuconfig();

  /* Program the MPU and enable I/D caches exactly as the SiFli SDK does in
   * SystemInit(), BEFORE touching SRAM (.data/.bss). This sets PSRAM CBUS
   * (0x10000000) as cacheable executable code, SRAM as non-cacheable, PSRAM
   * SBUS (0x60000000) as write-through cacheable data, peripherals as
   * device/non-executable, and the NAND window. mpu_config() also calls
   * SCB_EnableICache()/SCB_EnableDCache() at the end.
   */
  mpu_config();

  arm_lowputc('B'); /* MPU + cache configured */

  /* Clear BSS section - critical for proper variable initialization */

  for (dest = (uint32_t *)_sbss; dest < (uint32_t *)_ebss; )
    {
      *dest++ = 0;
    }

  /* Copy initialized data from flash to SRAM */

  for (src = (const uint32_t *)_eronly,
       dest = (uint32_t *)_sdata; dest < (uint32_t *)_edata; )
    {
      *dest++ = *src++;
    }

  /* Copy .ramfunc section from flash to SRAM */

  for (src = (const uint32_t *)&_siramfunc,
       dest = (uint32_t *)&_sramfunc; dest < (uint32_t *)&_eramfunc; )
    {
      *dest++ = *src++;
    }

  arm_lowputc('C'); /* bss/data/ramfunc init done */

  /* Call HAL_Init() with interrupts disabled.
   * Some HAL functions may trigger hardware events that could
   * generate interrupts, but they won't fire while interrupts are disabled.
   */
  HAL_Init();
    arm_lowputc('D'); /* HAL init done */

  /* NAND boot: HCPU 240 MHz is applied in board_late_initialize() after
   * NAND/BBM (before NSH). Raising it here faults XIP/BBM on PSRAM boot. */

  /* Now that HAL_Init() has set up the clock tree, it is safe to (re)init
   * USART1 with NuttX's parameters for the early console.
   */
  arm_earlyserialinit();

  /* Disable SysTick that was enabled by HAL_Init().
   * NuttX uses its own timer system (LPTIM for tickless mode).
   * SysTick must be disabled to prevent unexpected interrupts.
   */
#define NVIC_SYSTICK_CTRL_REG   (*((volatile uint32_t *)0xE000E010))
  NVIC_SYSTICK_CTRL_REG = 0;  /* Disable SysTick completely */


  arm_lowputc('E'); /* about to start system */

  /* nx_start() will initialize the interrupt system and enable interrupts.
   * Interrupts remain disabled until the system is fully ready.
   */
  nx_start();
  
  showprogress('X'); /* should never reach here */

  for (; ; );
}
