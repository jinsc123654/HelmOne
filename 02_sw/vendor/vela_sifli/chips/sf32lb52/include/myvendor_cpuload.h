/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/include/myvendor_cpuload.h
 *
 * External hardware timer for NuttX cpuload (accurate ps CPU%).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef MYVENDOR_CPULOAD_H
#define MYVENDOR_CPULOAD_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_MYVENDOR_CPULOAD_EXTCLK) && CONFIG_MYVENDOR_CPULOAD_EXTCLK

int myvendor_cpuload_extclk_init(void);

#else

static inline int myvendor_cpuload_extclk_init(void)
{
  return 0;
}

#endif

#endif /* MYVENDOR_CPULOAD_H */
