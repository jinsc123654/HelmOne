/****************************************************************************
 * vendor/sifli/chips/sf32lb52/include/sf32lb_timer.h
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

#ifndef __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_TIMER_H
#define __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_TIMER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/timers/timer.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sf32lb_timer_index_e
{
#if defined(CONFIG_BSP_USING_ATIM1)
  ATIM1_INDEX,
#endif
#if defined(CONFIG_BSP_USING_ATIM2)
  ATIM2_INDEX,
#endif
#if defined(CONFIG_BSP_USING_BTIM1)
  BTIM1_INDEX,
#endif
#if defined(CONFIG_BSP_USING_BTIM2)
  BTIM2_INDEX,
#endif
#if defined(CONFIG_BSP_USING_BTIM3)
  BTIM3_INDEX,
#endif
#if defined(CONFIG_BSP_USING_BTIM4)
  BTIM4_INDEX,
#endif
  BTIM_MAX,
};

/****************************************************************************
 * Public Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_BSP_USING_ATIM1)
#  define SF32LB_TIMER_DEFAULT_INDEX ATIM1_INDEX
#elif defined(CONFIG_BSP_USING_ATIM2)
#  define SF32LB_TIMER_DEFAULT_INDEX ATIM2_INDEX
#elif defined(CONFIG_BSP_USING_BTIM1)
#  define SF32LB_TIMER_DEFAULT_INDEX BTIM1_INDEX
#elif defined(CONFIG_BSP_USING_BTIM2)
#  define SF32LB_TIMER_DEFAULT_INDEX BTIM2_INDEX
#elif defined(CONFIG_BSP_USING_BTIM3)
#  define SF32LB_TIMER_DEFAULT_INDEX BTIM3_INDEX
#elif defined(CONFIG_BSP_USING_BTIM4)
#  define SF32LB_TIMER_DEFAULT_INDEX BTIM4_INDEX
#else
#  define SF32LB_TIMER_DEFAULT_INDEX (-1)
#endif

/* Dedicated BTIM2 for NuttX cpuload EXTCLK (not /dev/timer). */

#if defined(CONFIG_BSP_USING_BTIM2)
#  define SF32LB_CPULOAD_TIMER_INDEX BTIM2_INDEX
#else
#  define SF32LB_CPULOAD_TIMER_INDEX (-1)
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

struct timer_lowerhalf_s *sf32lb_timer_initialize(int chan,
                                                  uint16_t resolution);

#endif /* __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_TIMER_H */
