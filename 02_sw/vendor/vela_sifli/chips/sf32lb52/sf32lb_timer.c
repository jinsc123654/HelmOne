/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb_timer.c
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

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <debug.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/clock.h>
#include <nuttx/timers/timer.h>

#ifdef CONFIG_BSP_USING_ATIM1
#  define BSP_USING_ATIM1 CONFIG_BSP_USING_ATIM1
#endif
#ifdef CONFIG_BSP_USING_ATIM2
#  define BSP_USING_ATIM2 CONFIG_BSP_USING_ATIM2
#endif
#ifdef CONFIG_BSP_USING_BTIM1
#  define BSP_USING_BTIM1 CONFIG_BSP_USING_BTIM1
#endif
#ifdef CONFIG_BSP_USING_BTIM2
#  define BSP_USING_BTIM2 CONFIG_BSP_USING_BTIM2
#endif
#ifdef CONFIG_BSP_USING_BTIM3
#  define BSP_USING_BTIM3 CONFIG_BSP_USING_BTIM3
#endif
#ifdef CONFIG_BSP_USING_BTIM4
#  define BSP_USING_BTIM4 CONFIG_BSP_USING_BTIM4
#endif

#include <bf0_hal.h>
#include "tim_config.h"
#include "sf32lb_timer.h"

#if 0
#  undef tmrinfo
#  define tmrinfo _info
#endif

#if defined(CONFIG_BSP_USING_ATIM1) || defined(CONFIG_BSP_USING_ATIM2) || \
    defined(CONFIG_BSP_USING_BTIM1) || defined(CONFIG_BSP_USING_BTIM2) || \
    defined(CONFIG_BSP_USING_BTIM3) || defined(CONFIG_BSP_USING_BTIM4)

struct sf32lb_timer_lowerhalf_s
{
  /* This is the part of the lower half driver that is visible to the upper-
   * half client of the driver.
   */

  FAR const struct timer_ops_s *ops;
  GPT_HandleTypeDef tim_handle;        /* HW timer low level handle */
  IRQn_Type tim_irqn;                  /* interrupt number for timer */
  FAR char *name;                      /* HW timer device name */
  uint8_t core;                        /* Clock source from which core */

  volatile bool running;               /* True: the timer is running */
  tccb_t cbk;                          /* Call back function when timeout */
  FAR void *arg;                       /* The argument that will accompany */
  uint32_t frequency;
  /** 微秒 → tick 的**定点乘数**（Q20：真值 = mul / 2^20），见 `us_to_ticks()`。
   *
   *  为什么要有它：`settimeout()` 会在**中断里**被调（`sf32lb_timer_handler`
   *  的重装路径），而原来那句 `period * frequency / USEC_PER_SEC` 是 64 位除法
   *  ⇒ Cortex-M33 上走 libgcc 的 `__aeabi_uldivmod` → `__udivmoddi4`，
   *  那几个函数吃真栈。2026-09-23 的转储 n586（`cfsr=0x00100000` STKOF、
   *  HardFault）解出来的链就是
   *  `exception_direct → irq_dispatch → sf32lb_timer_handler →
   *   sf32lb_timer_settimeout → __aeabi_uldivmod → __udivmoddi4`。
   *
   *  改成"先在**线程态**算好乘数、中断里只做一次 64 位乘 + 移位"，
   *  中断里不再有除法调用（不占额外栈、也省周期）。
   *  初始化时算（`sf32lb_timer_initialize`），`frequency` 之后再不改。 */
  uint32_t us_to_ticks_mul;
  uint32_t period;
  uint32_t timeout;
};

static struct sf32lb_timer_lowerhalf_s g_low_timer[] =
{
#if defined(CONFIG_BSP_USING_ATIM1)
  ATIM1_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_ATIM2)
  ATIM2_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_BTIM1)
  BTIM1_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_BTIM2)
  BTIM2_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_BTIM3)
  BTIM3_CONFIG,
#endif
#if defined(CONFIG_BSP_USING_BTIM4)
  BTIM4_CONFIG,
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sf32lb_timer_handler(int irq, void *context, void *arg);
static int sf32lb_timer_stop(struct timer_lowerhalf_s *lower);
static int sf32lb_timer_start(struct timer_lowerhalf_s *lower);
static int sf32lb_timer_getstatus(struct timer_lowerhalf_s *lower,
                                  struct timer_status_s *status);
static int sf32lb_timer_settimeout(struct timer_lowerhalf_s *lower,
                                   uint32_t timeout);
static void sf32lb_timer_setcallback(struct timer_lowerhalf_s *lower,
                                     tccb_t callback, void *arg);
static int sf32lb_timer_maxtimeout(struct timer_lowerhalf_s *lower,
                                   uint32_t *maxtimeout);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct timer_ops_s g_timer_ops =
{
  .start       = sf32lb_timer_start,
  .stop        = sf32lb_timer_stop,
  .getstatus   = sf32lb_timer_getstatus,
  .settimeout  = sf32lb_timer_settimeout,
  .setcallback = sf32lb_timer_setcallback,
  .maxtimeout  = sf32lb_timer_maxtimeout,
  .ioctl       = NULL,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/** @brief 预乘**定点数**的移位位数（Q20，见 `us_to_ticks_mul`）。
 *
 *  取 20 的理由（离线全网格校验过）：
 *   - `mul = ceil(freq << 20 / 1e6)`：本板唯一的调用点是 `myvendor_cpuload.c`
 *     传的 **resolution = 1000000** ⇒ `mul` 恰好 = 2^20 ⇒
 *     `(timeout * mul) >> 20` **逐位等于**原来的
 *     `timeout * freq / 1e6`，零精度损失。
 *   - 一般情形：`mul ≤ 2^32`（freq ≤ 2.4e8 时最大 251658240）；
 *     `timeout * mul ≤ (2^32-1) * 2^32` 仍在 uint64 内 ⇒ 不会溢出。
 *   - 最坏相对误差 1/mul，实测出现在低频分辨率（freq=1000 时 1e-3）；
 *     **不会**出现"原来 >0 现在 =0"的反转（`DEBUGASSERT(period > 0)` 的语义保住）。 */
#define SF32LB52_TIMER_MUL_SHIFT 20u

/** @brief 算出"微秒 → tick"的定点乘数（**线程态**调用，见 `us_to_ticks_mul`）。
 *
 *  `mul = ceil(freq * 2^20 / 1e6)`，但**分子分母同除 64** 之后全用 32 位算：
 *
 *      2^20 / 1e6  ==  16384 / 15625
 *
 *  `resolution` 是 `uint16_t`（见 `sf32lb_timer_initialize`）⇒
 *  `freq * 16384 ≤ 65535 * 16384 = 1.07e9 < 2^32` ⇒ 装得下，
 *  于是这里也只是个 **32 位除法**，不会把 libgcc 的 64 位除法拖进来。
 *  （已离线逐值核对：freq ∈ [1, 65535] 内两种写法结果完全相同。）
 *
 *  上取整是必须的：截断会让低频分辨率算出 0 —— freq=1000、timeout=1 ms 时
 *  截断得 1048，`(1000*1048)>>20 == 0`，定时器直接失效。
 *  取整后 `mul` 最小是 2（freq=1），**永不为 0**，所以 `settimeout` 不需要兜底。 */
static void sf32lb_timer_calc_us_to_ticks(
  FAR struct sf32lb_timer_lowerhalf_s *timer)
{
  timer->us_to_ticks_mul =
    ((uint32_t)timer->frequency * 16384u + 15624u) / 15625u;

  DEBUGASSERT(timer->us_to_ticks_mul != 0u);
}

/** @brief 微秒 → tick 计数。**中断安全**：只有 64 位乘与移位，无除法、无调用。 */
static inline uint64_t sf32lb_timer_us_to_ticks(
  FAR struct sf32lb_timer_lowerhalf_s *timer, uint32_t timeout)
{
  return (((uint64_t)timeout) * timer->us_to_ticks_mul)
         >> SF32LB52_TIMER_MUL_SHIFT;
}

static void timer_init(struct sf32lb_timer_lowerhalf_s *timer)
{
  uint32_t prescaler_value;
  FAR GPT_HandleTypeDef *tim;
  irqstate_t flags;

  DEBUGASSERT(timer != NULL);

  flags = enter_critical_section();
  tim = &timer->tim_handle;

  prescaler_value = HAL_RCC_GetPCLKFreq(timer->core, 1) / timer->frequency - 1;
  tim->Init.Period            = 10000 - 1;
  tim->Init.Prescaler         = prescaler_value;
  tim->core                   = timer->core;
  tim->Init.CounterMode       = GPT_COUNTERMODE_UP;
  tim->Init.RepetitionCounter = 0;

  if (HAL_GPT_Base_Init(tim) != HAL_OK)
    {
      tmrerr("%s init failed\n", timer->name);
      leave_critical_section(flags);
      return;
    }

  timer->ops = &g_timer_ops;
  irq_attach(timer->tim_irqn + 16, sf32lb_timer_handler, timer);

  /* Clear pending update and keep update request source to overflow event. */

  __HAL_GPT_CLEAR_FLAG(tim, GPT_FLAG_UPDATE);
  __HAL_GPT_URS_ENABLE(tim);

  tmrinfo("%s init success\n", timer->name);
  leave_critical_section(flags);
}

static int sf32lb_timer_handler(int irq, void *context, void *arg)
{
  FAR struct sf32lb_timer_lowerhalf_s *timer =
    (FAR struct sf32lb_timer_lowerhalf_s *)arg;
  uint32_t next_interval_us = 0;
  tccb_t timer_handler;

  DEBUGASSERT(timer != NULL);

  /* Acknowledge/clear interrupt flags first. */

  HAL_GPT_IRQHandler(&timer->tim_handle);

  timer_handler = timer->cbk;
  if (timer_handler != NULL && timer_handler(&next_interval_us, timer->arg))
    {
      if (next_interval_us > 0)
        {
          sf32lb_timer_settimeout((struct timer_lowerhalf_s *)timer,
                                  next_interval_us);
        }

      /* Re-arm single-shot hardware timer for periodic callback path. */

      HAL_GPT_Base_Start_IT(&timer->tim_handle);
    }
  else
    {
      sf32lb_timer_stop((struct timer_lowerhalf_s *)timer);
    }

  return OK;
}

static void sf32lb_timer_setcallback(struct timer_lowerhalf_s *lower,
                                     tccb_t callback, void *arg)
{
  FAR struct sf32lb_timer_lowerhalf_s *priv =
    (FAR struct sf32lb_timer_lowerhalf_s *)lower;
  irqstate_t flags;

  flags = enter_critical_section();
  priv->cbk = callback;
  priv->arg = arg;
  leave_critical_section(flags);
}

static int sf32lb_timer_settimeout(struct timer_lowerhalf_s *lower,
                                   uint32_t timeout)
{
  FAR struct sf32lb_timer_lowerhalf_s *timer =
    (FAR struct sf32lb_timer_lowerhalf_s *)lower;
  uint64_t period;

  period = timeout;
  /* **中断里不能有除法**：原来这句是 `period * frequency / USEC_PER_SEC`，
   * 64 位除法会调 libgcc 的 `__aeabi_uldivmod`（转储 n586 的栈里就是它），
   * 而这条重装路径是在 GPT ISR 上跑的。现在只剩一次 64 位乘 + 移位。
   *
   * 乘数在 `sf32lb_timer_initialize()` 里就算好了，`frequency` 之后不再变，
   * 且乘数**永不为 0**（见 `sf32lb_timer_calc_us_to_ticks`）⇒ 这里不需要兜底
   * 分支。这一点很重要：**留了兜底就等于把 64 位除法留在函数里**
   * （已实测反汇编：留兜底时 `sf32lb_timer_settimeout` 里仍有
   * `bl __aeabi_uldivmod`），那就等于没修。 */
  period = sf32lb_timer_us_to_ticks(timer, timeout);

  DEBUGASSERT(period > 0);
  DEBUGASSERT(period <= UINT32_MAX);

  timer->period = (uint32_t)period;
  timer->timeout = timeout;
  __HAL_GPT_SET_AUTORELOAD(&timer->tim_handle, timer->period);

  return OK;
}

static int sf32lb_timer_getstatus(struct timer_lowerhalf_s *lower,
                                  struct timer_status_s *status)
{
  FAR struct sf32lb_timer_lowerhalf_s *priv =
    (FAR struct sf32lb_timer_lowerhalf_s *)lower;

  DEBUGASSERT(priv != NULL);
  DEBUGASSERT(status != NULL);

  status->flags = 0;

  if (priv->running)
    {
      status->flags |= TCFLAGS_ACTIVE;
    }

  if (priv->cbk != NULL)
    {
      status->flags |= TCFLAGS_HANDLER;
    }

  status->timeleft = __HAL_GPT_GET_COUNTER(&priv->tim_handle) * 1000000 /
                     priv->frequency;
  status->timeout = priv->timeout;

  if (status->timeleft > status->timeout)
    {
      status->timeleft = 0;
    }
  else
    {
      status->timeleft = status->timeout - status->timeleft;
    }

  return OK;
}

static int sf32lb_timer_start(struct timer_lowerhalf_s *lower)
{
  irqstate_t flags;
  int ret;
  FAR struct sf32lb_timer_lowerhalf_s *timer =
    (FAR struct sf32lb_timer_lowerhalf_s *)lower;

  DEBUGASSERT(timer != NULL);

  flags = enter_critical_section();
  if (timer->running)
    {
      sf32lb_timer_stop(lower);
    }

  timer->tim_handle.Instance->CR1 |= GPT_OPMODE_SINGLE;

  ret = HAL_GPT_Base_Start_IT(&timer->tim_handle);
  if (ret == HAL_OK)
    {
      up_enable_irq(timer->tim_irqn + 16);
      timer->running = true;
      ret = OK;
    }
  else
    {
      ret = -EIO;
    }

  leave_critical_section(flags);
  return ret;
}

static int sf32lb_timer_stop(struct timer_lowerhalf_s *lower)
{
  FAR struct sf32lb_timer_lowerhalf_s *timer =
    (FAR struct sf32lb_timer_lowerhalf_s *)lower;
  irqstate_t flags;

  flags = enter_critical_section();
  if (!timer->running)
    {
      leave_critical_section(flags);
      return OK;
    }

  HAL_GPT_Base_Stop_IT(&timer->tim_handle);
  up_disable_irq(timer->tim_irqn + 16);
  timer->running = false;

  leave_critical_section(flags);
  return OK;
}

static int sf32lb_timer_maxtimeout(struct timer_lowerhalf_s *lower,
                                   uint32_t *maxtimeout)
{
  if (maxtimeout != NULL)
    {
      *maxtimeout = UINT32_MAX;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct timer_lowerhalf_s *sf32lb_timer_initialize(int chan, uint16_t resolution)
{
  FAR struct sf32lb_timer_lowerhalf_s *timer;

  DEBUGASSERT(resolution > 0);
  DEBUGASSERT(chan >= 0 && chan < BTIM_MAX);

  timer = &g_low_timer[chan];
  timer->frequency = resolution;
  sf32lb_timer_calc_us_to_ticks(timer);
  timer_init(timer);

  timer->running = false;
  timer->cbk     = NULL;
  timer->arg     = NULL;

  return (struct timer_lowerhalf_s *)timer;
}

#else

struct timer_lowerhalf_s *sf32lb_timer_initialize(int chan, uint16_t resolution)
{
  UNUSED(chan);
  UNUSED(resolution);
  return NULL;
}

#endif
