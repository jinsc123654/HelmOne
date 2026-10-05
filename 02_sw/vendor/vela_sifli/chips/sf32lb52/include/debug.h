/****************************************************************************
 * vendor/sifli/chips/sf32lb52/include/debug.h
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

#ifndef __VENDOR_SIFLI_SF32LB52_INCLUDE_DEBUG_H
#define __VENDOR_SIFLI_SF32LB52_INCLUDE_DEBUG_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

int sifli_arch_syslog(int priority, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Route debug output through SiFli low-level logger. */

#define __arch_syslog sifli_arch_syslog

/* This header is reachable as <debug.h> because of chip include path
 * priority. Forward to NuttX's real debug.h in that case so subsystem
 * macros (sinfo/serr/tmrerr/...) are still defined.
 */

#ifndef __INCLUDE_DEBUG_H
#  include_next <debug.h>
#endif

#endif /* __VENDOR_SIFLI_SF32LB52_INCLUDE_DEBUG_H */
