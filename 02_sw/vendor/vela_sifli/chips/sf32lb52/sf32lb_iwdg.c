/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb_iwdg.c
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

#include <inttypes.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/clock.h>
#include <nuttx/timers/watchdog.h>

#include "bf0_hal.h"

#if defined(CONFIG_WATCHDOG)

#ifndef CONFIG_WATCHDOG_AUTOMONITOR_TIMEOUT
#  define CONFIG_WATCHDOG_AUTOMONITOR_TIMEOUT 5
#endif

#ifndef CONFIG_SF32LB_IWDG_DEFTIMOUT
#  define CONFIG_SF32LB_IWDG_DEFTIMOUT \
    (CONFIG_WATCHDOG_AUTOMONITOR_TIMEOUT * 1000 / 2)
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sf32lb_watchdog_lowerhalf_s
{
  FAR const struct watchdog_ops_s *ops;
  WDT_HandleTypeDef handle;
  xcpt_t int_handler;
  uint32_t timeout;
  clock_t lastreset;
  bool started;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sf32lb_start(FAR struct watchdog_lowerhalf_s *lower);
static int sf32lb_stop(FAR struct watchdog_lowerhalf_s *lower);
static int sf32lb_keepalive(FAR struct watchdog_lowerhalf_s *lower);
static int sf32lb_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                             uint32_t timeout);
static xcpt_t sf32lb_capture(FAR struct watchdog_lowerhalf_s *lower,
                             xcpt_t handler);
static int sf32lb_ioctl(FAR struct watchdog_lowerhalf_s *lower,
                        int cmd, unsigned long arg);
static int sf32lb_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                            FAR struct watchdog_status_s *status);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct watchdog_ops_s g_wdgops =
{
  .start      = sf32lb_start,
  .stop       = sf32lb_stop,
  .keepalive  = sf32lb_keepalive,
  .getstatus  = sf32lb_getstatus,
  .settimeout = sf32lb_settimeout,
  .capture    = sf32lb_capture,
  .ioctl      = sf32lb_ioctl,
};

static struct sf32lb_watchdog_lowerhalf_s g_iwdgdev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t sf32lb_wdt_clk_hz(void)
{
  /* SF32LB52 HAL_Init selects RC32K (SEL_LPCLK=1). 9000 Hz is RC10K. */
  if ((hwp_pmuc->CR & PMUC_CR_SEL_LPCLK) != 0)
    {
      return 32768u;
    }

  return 9000u;
}

static uint32_t sf32lb_wdt_ticks(uint32_t timeout_ms)
{
  uint64_t t = ((uint64_t)timeout_ms * sf32lb_wdt_clk_hz()) / 1000u;

  if (t == 0 || t > 0x00ffffffull)
    {
      t = 0x00ffffffull;
    }

  return (uint32_t)t;
}

static void sf32lb_stop_cpu_wdts(void)
{
  unsigned n;

  hwp_wdt1->WDT_WP = WDT_RELEASE_MAGIC;
  hwp_wdt1->WDT_CCR = WDT_CMD_STOP;
  hwp_wdt2->WDT_WP = WDT_RELEASE_MAGIC;
  hwp_wdt2->WDT_CCR = WDT_CMD_STOP;
  for (n = 0; n < 8000u; n++)
    {
      if ((hwp_wdt1->WDT_SR & WDT_WDT_SR_WDT_ACTIVE) == 0 &&
          (hwp_wdt2->WDT_SR & WDT_WDT_SR_WDT_ACTIVE) == 0)
        {
          break;
        }
    }
}

static int sf32lb_start(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct sf32lb_watchdog_lowerhalf_s *priv =
    (FAR struct sf32lb_watchdog_lowerhalf_s *)lower;

  /* Apply default timeout on first start if user did not configure one. */
  if (priv->timeout == 0)
    {
      int ret = sf32lb_settimeout(lower, CONFIG_SF32LB_IWDG_DEFTIMOUT);
      if (ret < 0)
        {
          return ret;
        }
    }

  sf32lb_stop_cpu_wdts();
  priv->handle.Instance->WDT_WP = WDT_RELEASE_MAGIC;
  if (HAL_WDT_Init(&priv->handle) != HAL_OK)
    {
      return -EIO;
    }

  priv->started = true;
  priv->lastreset = clock_systime_ticks();
  return OK;
}

static int sf32lb_stop(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct sf32lb_watchdog_lowerhalf_s *priv =
    (FAR struct sf32lb_watchdog_lowerhalf_s *)lower;

  __HAL_WDT_STOP(&priv->handle);
  priv->started = false;
  return OK;
}

static int sf32lb_keepalive(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct sf32lb_watchdog_lowerhalf_s *priv =
    (FAR struct sf32lb_watchdog_lowerhalf_s *)lower;

  priv->handle.Instance->WDT_WP = WDT_RELEASE_MAGIC;
  __HAL_WDT_RELOAD_COUNTER(&priv->handle);
  priv->handle.Instance->WDT_WP = WDT_RELEASE_MAGIC;
  if (HAL_WDT_Refresh(&priv->handle) != HAL_OK)
    {
      return -EIO;
    }

  priv->lastreset = clock_systime_ticks();
  return OK;
}

static int sf32lb_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                             uint32_t timeout)
{
  FAR struct sf32lb_watchdog_lowerhalf_s *priv =
    (FAR struct sf32lb_watchdog_lowerhalf_s *)lower;

  if (timeout == 0)
    {
      return -EINVAL;
    }

  priv->timeout = timeout;
  priv->handle.Init.Reload = sf32lb_wdt_ticks(timeout);
  priv->handle.Init.Reload2 = sf32lb_wdt_ticks(5);

  /* Keep watchdog stopped until explicit start request from upper-half. */
  if (!priv->started)
    {
      priv->lastreset = clock_systime_ticks();
      return OK;
    }

  priv->handle.Instance->WDT_WP = WDT_RELEASE_MAGIC;
  if (HAL_WDT_Init(&priv->handle) != HAL_OK)
    {
      return -EIO;
    }

  priv->lastreset = clock_systime_ticks();
  return OK;
}

static xcpt_t sf32lb_capture(FAR struct watchdog_lowerhalf_s *lower,
                             xcpt_t handler)
{
  FAR struct sf32lb_watchdog_lowerhalf_s *priv =
    (FAR struct sf32lb_watchdog_lowerhalf_s *)lower;
  xcpt_t oldhandler = priv->int_handler;

  /* IWDT supports reset mode only; keep capture API for upper-half ABI. */
  if (handler != NULL)
    {
      wderr("ERROR: IWDT does not support interrupt capture\n");
    }

  return oldhandler;
}

static int sf32lb_ioctl(FAR struct watchdog_lowerhalf_s *lower,
                        int cmd, unsigned long arg)
{
  UNUSED(lower);
  UNUSED(cmd);
  UNUSED(arg);
  return -ENOTTY;
}

static int sf32lb_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                            FAR struct watchdog_status_s *status)
{
  FAR struct sf32lb_watchdog_lowerhalf_s *priv =
    (FAR struct sf32lb_watchdog_lowerhalf_s *)lower;
  uint32_t elapsed;

  DEBUGASSERT(priv != NULL && status != NULL);

  status->flags = WDFLAGS_RESET;
  if (priv->started)
    {
      status->flags |= WDFLAGS_ACTIVE;
    }

  if (priv->int_handler != NULL)
    {
      status->flags |= WDFLAGS_CAPTURE;
    }

  status->timeout = priv->timeout;

  elapsed = (uint32_t)TICK2MSEC(clock_systime_ticks() - priv->lastreset);
  if (elapsed >= priv->timeout)
    {
      status->timeleft = 0;
    }
  else
    {
      status->timeleft = priv->timeout - elapsed;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void sf32lb_iwdginitialize(FAR const char *devpath)
{
  FAR struct sf32lb_watchdog_lowerhalf_s *priv = &g_iwdgdev;

  priv->ops = &g_wdgops;
  priv->started = false;
  priv->int_handler = NULL;
  priv->timeout = CONFIG_SF32LB_IWDG_DEFTIMOUT;
  priv->lastreset = clock_systime_ticks();
  priv->handle.Instance = hwp_iwdt;
  priv->handle.Init.Reload = sf32lb_wdt_ticks(priv->timeout);
  priv->handle.Init.Reload2 = sf32lb_wdt_ticks(5);

  (void)watchdog_register(devpath,
                          (FAR struct watchdog_lowerhalf_s *)priv);
}

#endif /* CONFIG_WATCHDOG */
