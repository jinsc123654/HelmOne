/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/myvendor_cpuload.c
 *
 * CPULOAD_EXTCLK via BTIM2 period timer (async with 10 ms sys tick).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_MYVENDOR_CPULOAD_EXTCLK) && CONFIG_MYVENDOR_CPULOAD_EXTCLK

#include "myvendor_cpuload.h"
#include "sf32lb_timer.h"

#include <nuttx/clock.h>
#include <errno.h>
#include <syslog.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int myvendor_cpuload_extclk_init(void)
{
#if defined(CONFIG_CPULOAD_PERIOD) && defined(CONFIG_SCHED_CPULOAD_EXTCLK)
  FAR struct timer_lowerhalf_s *lower;

#  if SF32LB_CPULOAD_TIMER_INDEX < 0
  syslog(LOG_ERR, "cpuload: no BTIM for EXTCLK (enable BSP_USING_BTIM2)\n");
  return -ENODEV;
#  else
  lower = sf32lb_timer_initialize(SF32LB_CPULOAD_TIMER_INDEX, 1000000);
  if (lower == NULL)
    {
      syslog(LOG_ERR, "cpuload: sf32lb_timer_initialize failed\n");
      return -ENODEV;
    }

  nxsched_period_extclk(lower);
  syslog(LOG_NOTICE,
         "cpuload: EXTCLK BTIM idx=%d %d Hz entropy=%d (ps CPU%%)\n",
         SF32LB_CPULOAD_TIMER_INDEX,
         CONFIG_SCHED_CPULOAD_TICKSPERSEC,
         CONFIG_CPULOAD_ENTROPY);
  return 0;
#  endif
#else
  syslog(LOG_ERR, "cpuload: need CPULOAD_PERIOD + SCHED_CPULOAD_EXTCLK\n");
  return -ENOTSUP;
#endif
}

#endif /* CONFIG_MYVENDOR_CPULOAD_EXTCLK */
