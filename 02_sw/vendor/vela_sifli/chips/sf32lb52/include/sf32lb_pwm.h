/****************************************************************************
 * vendor/sifli/chips/sf32lb52/include/sf32lb_pwm.h
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

#ifndef __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_PWM_H
#define __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_PWM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/timers/pwm.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sf32lb_pwm_index_e
{
#if defined(CONFIG_BSP_USING_PWM2)
  PWM2_INDEX,
#endif
#if defined(CONFIG_BSP_USING_PWM3)
  PWM3_INDEX,
#endif
#if defined(CONFIG_BSP_USING_PWM4)
  PWM4_INDEX,
#endif
#if defined(CONFIG_BSP_USING_PWM5)
  PWM5_INDEX,
#endif
#if defined(CONFIG_BSP_USING_PWM6)
  PWM6_INDEX,
#endif
#if defined(CONFIG_BSP_USING_PWMA1)
  PWMA1_INDEX,
#endif
#if defined(CONFIG_BSP_USING_PWMA2)
  PWMA2_INDEX,
#endif
  PWM_MAX,
};

/****************************************************************************
 * Public Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_BSP_USING_PWM2)
#  define SF32LB_PWM_DEFAULT_INDEX PWM2_INDEX
#elif defined(CONFIG_BSP_USING_PWM3)
#  define SF32LB_PWM_DEFAULT_INDEX PWM3_INDEX
#elif defined(CONFIG_BSP_USING_PWM4)
#  define SF32LB_PWM_DEFAULT_INDEX PWM4_INDEX
#elif defined(CONFIG_BSP_USING_PWM5)
#  define SF32LB_PWM_DEFAULT_INDEX PWM5_INDEX
#elif defined(CONFIG_BSP_USING_PWM6)
#  define SF32LB_PWM_DEFAULT_INDEX PWM6_INDEX
#elif defined(CONFIG_BSP_USING_PWMA1)
#  define SF32LB_PWM_DEFAULT_INDEX PWMA1_INDEX
#elif defined(CONFIG_BSP_USING_PWMA2)
#  define SF32LB_PWM_DEFAULT_INDEX PWMA2_INDEX
#else
#  define SF32LB_PWM_DEFAULT_INDEX (-1)
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

struct pwm_lowerhalf_s *sf32lb_pwm_initialize(int pwmid);

#endif /* __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_PWM_H */
