/****************************************************************************
 * vendor/sifli/chips/sf32lb52/include/sf32lb_rtc.h
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

#ifndef __ARCH_ARM_SRC_SIFLI_SF32LB52_RTC_H
#define __ARCH_ARM_SRC_SIFLI_SF32LB52_RTC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#ifdef CONFIG_RTC_DRIVER
struct rtc_lowerhalf_s;
struct rtc_lowerhalf_s *sf32lb_rtc_lowerhalf(void);

/* RTC calendar trust: magic in BKP0 survives VBAT reset (see sf32lb_rtc.c). */
#define SF32LB_RTC_TRUST_MAGIC  0x52544331u  /* 'RTC1' */

bool sf32lb_rtc_trust_is_valid(void);
#endif

#endif /* __ARCH_ARM_SRC_SIFLI_SF32LB52_RTC_H */
