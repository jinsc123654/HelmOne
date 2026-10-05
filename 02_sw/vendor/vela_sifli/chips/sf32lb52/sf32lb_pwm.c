/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb_pwm.c
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

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/timers/pwm.h>

#ifdef CONFIG_BSP_USING_PWM2
#  define BSP_USING_PWM2 CONFIG_BSP_USING_PWM2
#endif
#ifdef CONFIG_BSP_USING_PWM3
#  define BSP_USING_PWM3 CONFIG_BSP_USING_PWM3
#endif
#ifdef CONFIG_BSP_USING_PWM4
#  define BSP_USING_PWM4 CONFIG_BSP_USING_PWM4
#endif
#ifdef CONFIG_BSP_USING_PWM5
#  define BSP_USING_PWM5 CONFIG_BSP_USING_PWM5
#endif
#ifdef CONFIG_BSP_USING_PWM6
#  define BSP_USING_PWM6 CONFIG_BSP_USING_PWM6
#endif
#ifdef CONFIG_BSP_USING_PWMA1
#  define BSP_USING_PWMA1 CONFIG_BSP_USING_PWMA1
#endif
#ifdef CONFIG_BSP_USING_PWMA2
#  define BSP_USING_PWMA2 CONFIG_BSP_USING_PWMA2
#endif

#include <bf0_hal.h>
#include "tim_config.h"
#include "sf32lb_pwm.h"

#ifdef CONFIG_BSP_USING_PWM2
#  define SF32LB_PWM2_CONFIG                     \
  {                                             \
    .tim_handle.Instance = GPTIM1,              \
    .tim_handle.core     = GPTIM1_CORE,         \
    .name                = "pwm2",             \
    .channel             = 3,                   \
  }
#endif

#ifdef CONFIG_BSP_USING_PWM3
#  define SF32LB_PWM3_CONFIG                     \
  {                                             \
    .tim_handle.Instance = GPTIM2,              \
    .tim_handle.core     = GPTIM2_CORE,         \
    .name                = "pwm3",             \
    .channel             = 0,                   \
  }
#endif

#ifdef CONFIG_BSP_USING_PWM4
#  define SF32LB_PWM4_CONFIG                     \
  {                                             \
    .tim_handle.Instance = GPTIM3,              \
    .tim_handle.core     = GPTIM3_CORE,         \
    .name                = "pwm4",             \
    .channel             = 0,                   \
  }
#endif

#ifdef CONFIG_BSP_USING_PWM5
#  define SF32LB_PWM5_CONFIG                     \
  {                                             \
    .tim_handle.Instance = GPTIM4,              \
    .tim_handle.core     = GPTIM4_CORE,         \
    .name                = "pwm5",             \
    .channel             = 0,                   \
  }
#endif

#ifdef CONFIG_BSP_USING_PWM6
#  define SF32LB_PWM6_CONFIG                     \
  {                                             \
    .tim_handle.Instance = GPTIM5,              \
    .tim_handle.core     = GPTIM5_CORE,         \
    .name                = "pwm6",             \
    .channel             = 0,                   \
  }
#endif

#ifdef CONFIG_BSP_USING_PWMA1
#  define SF32LB_PWMA1_CONFIG                    \
  {                                             \
    .tim_handle.Instance = (GPT_TypeDef *)ATIM1,\
    .tim_handle.core     = ATIM1_CORE,          \
    .name                = "pwma1",            \
    .channel             = 0,                   \
  }
#endif

#ifdef CONFIG_BSP_USING_PWMA2
#  define SF32LB_PWMA2_CONFIG                    \
  {                                             \
    .tim_handle.Instance = (GPT_TypeDef *)ATIM2,\
    .tim_handle.core     = ATIM2_CORE,          \
    .name                = "pwma2",            \
    .channel             = 0,                   \
  }
#endif

#if defined(CONFIG_BSP_USING_PWM2) || defined(CONFIG_BSP_USING_PWM3) || \
    defined(CONFIG_BSP_USING_PWM4) || defined(CONFIG_BSP_USING_PWM5) || \
    defined(CONFIG_BSP_USING_PWM6) || defined(CONFIG_BSP_USING_PWMA1) || \
    defined(CONFIG_BSP_USING_PWMA2)

struct sf32lb_pwm_lowerhalf_s
{
  FAR const struct pwm_ops_s *ops;
  GPT_HandleTypeDef tim_handle;
  FAR char *name;
  uint8_t channel;
  uint32_t active_channel;
  bool initialized;
  bool running;
};

static struct sf32lb_pwm_lowerhalf_s g_pwm[] =
{
#if defined(CONFIG_BSP_USING_PWM2)
  SF32LB_PWM2_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_PWM3)
  SF32LB_PWM3_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_PWM4)
  SF32LB_PWM4_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_PWM5)
  SF32LB_PWM5_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_PWM6)
  SF32LB_PWM6_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_PWMA1)
  SF32LB_PWMA1_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_PWMA2)
  SF32LB_PWMA2_CONFIG,
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sf32lb_pwm_setup(struct pwm_lowerhalf_s *dev);
static int sf32lb_pwm_shutdown(struct pwm_lowerhalf_s *dev);
#ifdef CONFIG_PWM_PULSECOUNT
static int sf32lb_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info,
                            void *handle);
#else
static int sf32lb_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info);
#endif
static int sf32lb_pwm_stop(struct pwm_lowerhalf_s *dev);
static int sf32lb_pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,
                            unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pwm_ops_s g_pwmops =
{
  .setup    = sf32lb_pwm_setup,
  .shutdown = sf32lb_pwm_shutdown,
  .start    = sf32lb_pwm_start,
  .stop     = sf32lb_pwm_stop,
  .ioctl    = sf32lb_pwm_ioctl,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t sf32lb_pwm_gptclock(FAR GPT_HandleTypeDef *htim)
{
#ifdef SF32LB52X
  if (htim->Instance == hwp_gptim2)
    {
      return 24000000;
    }
#endif

  return HAL_RCC_GetPCLKFreq(htim->core, 1);
}

static int sf32lb_pwm_timer_config(FAR struct sf32lb_pwm_lowerhalf_s *priv,
                                   uint32_t frequency,
                                   ub16_t duty,
                                   uint32_t channel)
{
  FAR GPT_HandleTypeDef *tim = &priv->tim_handle;
  GPT_OC_InitTypeDef oc_cfg;
  uint32_t timclk;
  uint64_t ticks;
  uint32_t prescaler;
  uint32_t period;
  uint32_t pulse;

  if (frequency == 0)
    {
      return -EINVAL;
    }

  timclk = sf32lb_pwm_gptclock(tim);
  if (timclk == 0)
    {
      return -EINVAL;
    }

  ticks = (uint64_t)timclk / (uint64_t)frequency;
  if (ticks == 0)
    {
      ticks = 1;
    }

  prescaler = (uint32_t)((ticks + 65535ULL - 1ULL) / 65535ULL);
  if (prescaler == 0)
    {
      prescaler = 1;
    }

  period = (uint32_t)(ticks / prescaler);
  if (period == 0)
    {
      period = 1;
    }
  else if (period > 65535)
    {
      period = 65535;
    }

  pulse = (uint32_t)(((uint64_t)duty * (uint64_t)period) >> 16);
  if (pulse > period)
    {
      pulse = period;
    }

  tim->Init.Prescaler         = prescaler - 1;
  tim->Init.CounterMode       = GPT_COUNTERMODE_UP;
  tim->Init.Period            = period - 1;
  tim->Init.RepetitionCounter = 0;

  if (HAL_GPT_PWM_Init(tim) != HAL_OK)
    {
      return -EIO;
    }

  oc_cfg.OCMode       = GPT_OCMODE_PWM1;
  oc_cfg.Pulse        = pulse;
  oc_cfg.OCPolarity   = GPT_OCPOLARITY_HIGH;
  oc_cfg.OCNPolarity  = GPT_OCNPOLARITY_LOW;
  oc_cfg.OCFastMode   = GPT_OCFAST_DISABLE;
  oc_cfg.OCIdleState  = GPT_OCIDLESTATE_RESET;
  oc_cfg.OCNIdleState = GPT_OCNIDLESTATE_RESET;

  if (HAL_GPT_PWM_ConfigChannel(tim, &oc_cfg, channel) != HAL_OK)
    {
      return -EIO;
    }

  return OK;
}

static int sf32lb_pwm_setup(struct pwm_lowerhalf_s *dev)
{
  FAR struct sf32lb_pwm_lowerhalf_s *priv =
    (FAR struct sf32lb_pwm_lowerhalf_s *)dev;

  if (priv->initialized)
    {
      return OK;
    }

  priv->initialized = true;
  return OK;
}

static int sf32lb_pwm_shutdown(struct pwm_lowerhalf_s *dev)
{
  FAR struct sf32lb_pwm_lowerhalf_s *priv =
    (FAR struct sf32lb_pwm_lowerhalf_s *)dev;

  if (priv->running)
    {
      sf32lb_pwm_stop(dev);
    }

  priv->initialized = false;
  return OK;
}

#ifdef CONFIG_PWM_PULSECOUNT
static int sf32lb_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info,
                            void *handle)
#else
static int sf32lb_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info)
#endif
{
  FAR struct sf32lb_pwm_lowerhalf_s *priv =
    (FAR struct sf32lb_pwm_lowerhalf_s *)dev;
  uint32_t channel;
  ub16_t duty;
  int ret;
#ifdef CONFIG_PWM_PULSECOUNT
  UNUSED(handle);
  if (info->count > 0)
    {
      return -ENOTSUP;
    }
#endif

  DEBUGASSERT(info != NULL);

  if (!priv->initialized)
    {
      ret = sf32lb_pwm_setup(dev);
      if (ret < 0)
        {
          return ret;
        }
    }

#ifdef CONFIG_PWM_MULTICHAN
  if (info->channels[0].channel < 1)
    {
      return -EINVAL;
    }

  channel = (uint32_t)((info->channels[0].channel - 1) * 4);
  duty = info->channels[0].duty;
#else
  channel = (uint32_t)(priv->channel * 4);
  duty = info->duty;
#endif

  if (priv->running)
    {
      sf32lb_pwm_stop(dev);
    }

  ret = sf32lb_pwm_timer_config(priv, info->frequency, duty, channel);
  if (ret < 0)
    {
      return ret;
    }

  if (HAL_GPT_PWM_Start(&priv->tim_handle, channel) != HAL_OK)
    {
      return -EIO;
    }

  priv->active_channel = channel;
  priv->running = true;
  return OK;
}

static int sf32lb_pwm_stop(struct pwm_lowerhalf_s *dev)
{
  FAR struct sf32lb_pwm_lowerhalf_s *priv =
    (FAR struct sf32lb_pwm_lowerhalf_s *)dev;
  uint32_t channel;

  if (!priv->running)
    {
      return OK;
    }

  channel = priv->active_channel;
  if (HAL_GPT_PWM_Stop(&priv->tim_handle, channel) != HAL_OK)
    {
      return -EIO;
    }

  priv->running = false;
  return OK;
}

static int sf32lb_pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,
                            unsigned long arg)
{
  UNUSED(dev);
  UNUSED(cmd);
  UNUSED(arg);
  return -ENOTTY;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct pwm_lowerhalf_s *sf32lb_pwm_initialize(int pwmid)
{
  FAR struct sf32lb_pwm_lowerhalf_s *priv;

  if (pwmid < 0 || pwmid >= PWM_MAX)
    {
      return NULL;
    }

  priv = &g_pwm[pwmid];
  priv->ops = &g_pwmops;
  priv->active_channel = (uint32_t)(priv->channel * 4);
  priv->initialized = false;
  priv->running = false;

  return (struct pwm_lowerhalf_s *)priv;
}

#else

struct pwm_lowerhalf_s *sf32lb_pwm_initialize(int pwmid)
{
  UNUSED(pwmid);
  return NULL;
}

#endif
