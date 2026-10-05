/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/sf32lb52_idle.c
 *
 * Custom up_idle(): WFI + DWT cycle accounting (CONFIG_ARCH_IDLE_CUSTOM).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/arch.h>

#include "myvendor_idle_stat.h"
#include "myvendor_watchdog.h"

#define MYVENDOR_DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void up_idle(void)
{
  uint32_t t0;
  uint32_t t1;

  myvendor_watchdog_idle_beat();
  myvendor_idle_stat_init();

  t0 = MYVENDOR_DWT_CYCCNT;

#ifdef CONFIG_ARCH_HAVE_WFEONLY
  __asm__ volatile ("wfe");
#else
  __asm__ volatile ("wfi");
#endif

  t1 = MYVENDOR_DWT_CYCCNT;
  myvendor_idle_stat_on_wfi(t1 - t0);
}
