/****************************************************************************
 * arch/arm/src/sifli/sf32lb_tickless.c
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
#include <syslog.h>
#include <time.h>
#include <debug.h>
#include <nuttx/arch.h>
#include <nuttx/timers/arch_timer.h>
#include <nuttx/timers/timer.h>
#include <arch/board/board.h>

#include "nvic.h"
#include "clock/clock.h"
#include "arm_internal.h"
#include "systick.h"
#include "chip.h"
#include "bf0_hal.h"
#include "system_bf0_ap.h"
#include "tim_config.h"
#include "bf0_hal_lptim.h"


#ifdef CONFIG_SCHED_TICKLESS

#if 0
#undef tmrinfo
#define tmrinfo _info
#endif
/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/


/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sf32_tickless_s
{
  uint32_t frequency;
  uint32_t overflow;           /* Timer counter overflow */
  volatile bool pending;       /* True: pending task */
  uint32_t period;             /* Interval period */

  LPTIM_HandleTypeDef    tim_handle;    /*!< HW timer low level handle */
  IRQn_Type tim_irqn;                 /*!< interrupt number for timer*/
  char *name;                         /*!< HW timer device name*/
};

/****************************************************************************
 * Private Data
 ****************************************************************************/
#ifdef LPTIM1_CONFIG
#undef LPTIM1_CONFIG
#endif
#define LPTIM1_CONFIG                             \
    {                                            \
       .tim_handle.Instance     = hwp_lptim1,        \
       .tim_irqn                = LPTIM1_IRQn,   \
       .name                    = "lptim1",     \
    }
static struct sf32_tickless_s g_tickless = LPTIM1_CONFIG;

/****************************************************************************
 * Private Functions
 ****************************************************************************/
static uint32_t lp_timer_counter_get(struct sf32_tickless_s *tickless_dev)
{
    LPTIM_HandleTypeDef *tim = &tickless_dev->tim_handle;

    return HAL_LPTIM_ReadCounter(tim);
}

static void sf32_get_ts(FAR struct timespec * ts)
{
    uint64_t usec;
    uint32_t counter;
    uint32_t overflow;
    uint32_t sec;
    int pending;

    overflow = g_tickless.overflow;
    pending  = __HAL_LPTIM_GET_FLAG(&g_tickless.tim_handle, LPTIM_FLAG_OF);
    counter  = lp_timer_counter_get(&g_tickless);
    
    /* If an interrupt was pending before we re-enabled interrupts,
     * then the overflow needs to be incremented.
     */
    
    if (pending)
      {
        __HAL_LPTIM_CLEAR_FLAG(&g_tickless.tim_handle, LPTIM_ICR_OFCLR);
    
        /* Increment the overflow count and use the value of the
         * guaranteed to be AFTER the overflow occurred.
         */    
        overflow++;    
        g_tickless.overflow = overflow;
      }
        
    tmrinfo("counter=%lu, overflow=%lu, pending=%i\n",
           (unsigned long)counter, (unsigned long)overflow, pending);
    
    /* Convert the whole thing to units of microseconds.
     *
     *   frequency = ticks / second
     *   seconds   = ticks * frequency
     *   usecs     = (ticks * USEC_PER_SEC) / frequency;
     */
    usec = ((((uint64_t)overflow << 24) + (uint64_t)counter) * USEC_PER_SEC) /
           g_tickless.frequency;
    
    /* And return the value of the timer */    
    sec         = (uint32_t)(usec / USEC_PER_SEC);
    ts->tv_sec  = sec;
    ts->tv_nsec = (usec - (sec * USEC_PER_SEC)) * NSEC_PER_USEC;
    
    tmrinfo("usec=%llu ts=(%lu, %lu)\n",
            usec, (unsigned long)ts->tv_sec, (unsigned long)ts->tv_nsec);
}

static void sf32_interval_handler(void)
{
  tmrinfo("Expired...\n");

  /* Disable the compare interrupt now. */
#ifdef CONFIG_SCHED_TICKLESS_ALARM
  FAR struct timespec ts;

  sf32_get_ts(&ts);
  nxsched_alarm_expiration(&ts);
#else
  nxsched_timer_expiration();
#endif
}
static void sf32_timing_handler(void)
{
  g_tickless.overflow++;
}

static int sf32_tickless_handler(int irq, void *context, void *arg)
{
  tmrinfo("tim_isr:%p\n", (void *)g_tickless.tim_handle.Instance->ISR);
  if (__HAL_LPTIM_GET_FLAG(&g_tickless.tim_handle, LPTIM_FLAG_OF) != RESET)
  {
    sf32_timing_handler();
  }

  /* Output compare interrupt */
  if (__HAL_LPTIM_GET_FLAG(&g_tickless.tim_handle, LPTIM_FLAG_OC) != RESET)
  {
    g_tickless.pending = false;
    sf32_interval_handler();    
  }
  HAL_LPTIM_IRQHandler(&g_tickless.tim_handle);

  return OK;
}

static void lp_timer_init(struct sf32_tickless_s *tickless_dev)
{
  LPTIM_HandleTypeDef *tim = &tickless_dev->tim_handle;

  HAL_LPTIM_InitDefault(tim);
  if (HAL_LPTIM_Init(tim) != HAL_OK)
  {
    tmrerr("lptimer init failed");
    return;
  }
  else
  {
    up_enable_irq(tickless_dev->tim_irqn + 16);

    __HAL_LPTIM_AUTORELOAD_SET(tim, LPTIM_MAX_CNT);

    __HAL_LPTIM_ENABLE(tim);
  
    __HAL_LPTIM_ENABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OFIE);
    __HAL_LPTIM_ENABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OFWE);

    tmrinfo("lptimer init success");
  }
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_timer_initialize
 *
 * Description:
 *   Initializes all platform-specific timer facilities.  This function is
 *   called early in the initialization sequence by up_initialize().
 *   On return, the current up-time should be available from
 *   up_timer_gettime() and the interval timer is ready for use (but not
 *   actively timing.
 *
 *   Provided by platform-specific code and called from the architecture-
 *   specific logic.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called early in the initialization sequence before any special
 *   concurrency protections are required.
 *
 ****************************************************************************/

void up_timer_initialize(void)
{
  /* Get the TC frequency that corresponds to the requested resolution */
  g_tickless.frequency = 32768;//USEC_PER_SEC / (uint32_t)CONFIG_USEC_PER_TICK;
  g_tickless.pending   = false;
  g_tickless.period    = 0;
  g_tickless.overflow  = 0;

  tmrinfo("frequency=%lu Hz\n", g_tickless.frequency);

  /* Set up to receive the callback when the counter overflow occurs */

  irq_attach(g_tickless.tim_irqn + 16, sf32_tickless_handler, &g_tickless);

  lp_timer_init(&g_tickless); 

  /* Start timer in single mode */
  __HAL_LPTIM_START_CONTINUOUS(&g_tickless.tim_handle);
}

/****************************************************************************
 * Name: up_timer_gettime
 *
 * Description:
 *   Return the elapsed time since power-up (or, more correctly, since
 *   up_timer_initialize() was called).  This function is functionally
 *   equivalent to:
 *
 *      int clock_gettime(clockid_t clockid, struct timespec *ts);
 *
 *   when clockid is CLOCK_MONOTONIC.
 *
 *   This function provides the basis for reporting the current time and
 *   also is used to eliminate error build-up from small errors in interval
 *   time calculations.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Provides the location in which to return the up-time.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   Called from the normal tasking context.  The implementation must
 *   provide whatever mutual exclusion is necessary for correct operation.
 *   This can include disabling interrupts in order to assure atomic register
 *   operations.
 *
 ****************************************************************************/

int up_timer_gettime(struct timespec *ts)
{
  irqstate_t flags;

  DEBUGASSERT(ts);

  /* Temporarily disable the overflow counter.  NOTE that we have to be
   * careful here because  stm32_tc_getpending() will reset the pending
   * interrupt status.  If we do not handle the overflow here then, it will
   * be lost.
   */

  flags    = enter_critical_section();
  sf32_get_ts(ts);
  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: up_timer_cancel
 *
 * Description:
 *   Cancel the interval timer and return the time remaining on the timer.
 *   These two steps need to be as nearly atomic as possible.
 *   nxsched_timer_expiration() will not be called unless the timer is
 *   restarted with up_timer_start().
 *
 *   If, as a race condition, the timer has already expired when this
 *   function is called, then that pending interrupt must be cleared so
 *   that up_timer_start() and the remaining time of zero should be
 *   returned.
 *
 *   NOTE: This function may execute at a high rate with no timer running (as
 *   when pre-emption is enabled and disabled).
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Location to return the remaining time.  Zero should be returned
 *        if the timer is not active.  ts may be zero in which case the
 *        time remaining is not returned.
 *
 * Returned Value:
 *   Zero (OK) is returned on success.  A call to up_timer_cancel() when
 *   the timer is not active should also return success; a negated errno
 *   value is returned on any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_timer_cancel(struct timespec *ts)
{
  irqstate_t flags;
  uint64_t usec;
  uint64_t sec;
  uint64_t nsec;
  uint32_t count;
  uint32_t period;
#if 1
  /* Was the timer running? */

  tmrinfo("Cancelling...:%d\n", g_tickless.pending);

  flags = enter_critical_section();
  if (!g_tickless.pending)
    {
      /* No.. Just return zero timer remaining and successful cancellation.
       * This function may execute at a high rate with no timer running
       * (as when pre-emption is enabled and disabled).
       */

      if (ts)
        {
          ts->tv_sec  = 0;
          ts->tv_nsec = 0;
        }

      leave_critical_section(flags);
      return OK;
    }

  /* Yes.. Get the timer counter and period registers and disable the compare
   * interrupt.
   */

  /* Disable the interrupt. */

  __HAL_LPTIM_DISABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCIE);
  __HAL_LPTIM_DISABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCWE);

  count  = lp_timer_counter_get(&g_tickless);
  period = g_tickless.period;

  g_tickless.pending = false;
  leave_critical_section(flags);

  /* Did the caller provide us with a location to return the time
   * remaining?
   */

  if (ts != NULL)
    {
      /* Yes.. then calculate and return the time remaining on the
       * oneshot timer.
       */

      tmrinfo("period=%lu count=%lu\n",
             (unsigned long)period, (unsigned long)count);

      if (count > period)
        {
          /* Handle rollover */
          period += LPTIM_MAX_CNT + 1;
        }
      else if (count == period)
        {
          /* No time remaining */

          ts->tv_sec  = 0;
          ts->tv_nsec = 0;
          return OK;
        }

      /* The total time remaining is the difference.  Convert that
       * to units of microseconds.
       *
       *   frequency = ticks / second
       *   seconds   = ticks * frequency
       *   usecs     = (ticks * USEC_PER_SEC) / frequency;
       */

      usec        = (((uint64_t)(period - count)) * USEC_PER_SEC) /
                    g_tickless.frequency;

      /* Return the time remaining in the correct form */

      sec         = usec / USEC_PER_SEC;
      nsec        = ((usec) - (sec * USEC_PER_SEC)) * NSEC_PER_USEC;

      ts->tv_sec  = (time_t)sec;
      ts->tv_nsec = (unsigned long)nsec;

      tmrinfo("remaining (%lu, %lu)\n",
             (unsigned long)ts->tv_sec, (unsigned long)ts->tv_nsec);
    }
#endif
  return OK;
}

/****************************************************************************
 * Name: up_timer_start
 *
 * Description:
 *   Start the interval timer.  nxsched_timer_expiration() will be
 *   called at the completion of the timeout (unless up_timer_cancel
 *   is called to stop the timing.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Provides the time interval until nxsched_timer_expiration() is
 *        called.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_timer_start(const struct timespec *ts)
{
  uint64_t usec;
  uint64_t period;
  uint32_t count;
  irqstate_t flags;
#if 1
  
  tmrinfo("ts1=(%lu, %lu)\n",
          (unsigned long)ts->tv_sec, (unsigned long)ts->tv_nsec);
  DEBUGASSERT(ts);

  /* Was an interval already running? */

  flags = enter_critical_section();
  if (g_tickless.pending)
    {
      /* Yes.. then cancel it */

      tmrinfo("Already running... cancelling\n");
      up_timer_cancel(NULL);
    }

  /* Express the delay in microseconds */

  usec = (uint64_t)ts->tv_sec * USEC_PER_SEC +
         (uint64_t)(ts->tv_nsec / NSEC_PER_USEC);

  /* Get the timer counter frequency and determine the number of counts need
   * to achieve the requested delay.
   *
   *   frequency = ticks / second
   *   ticks     = seconds * frequency
   *             = (usecs * frequency) / USEC_PER_SEC;
   */

  period = (usec * (uint64_t)g_tickless.frequency) / USEC_PER_SEC;
  count  = lp_timer_counter_get(&g_tickless);

  tmrinfo("usec=%llu period=%08llx,%lu\n", usec, period, count);

  /* Set interval compare value. Rollover is fine,
   * channel will trigger on the next period.
   */
  DEBUGASSERT(period <= LPTIM_MAX_CNT);
  g_tickless.period = (period + count);
  if (g_tickless.period > LPTIM_MAX_CNT)
  {
    g_tickless.period -= (LPTIM_MAX_CNT + 1);
  }
  tmrinfo("hw period %lu, %lu\n", g_tickless.period, count);

  __HAL_LPTIM_COMPARE_SET(&g_tickless.tim_handle, g_tickless.period);

  /* Enable interrupts.  We should get the callback when the interrupt
   * occurs.
   */

  /* Output compare Interrupt Enablet */
  __HAL_LPTIM_ENABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCIE);
  __HAL_LPTIM_ENABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCWE);

  g_tickless.pending = true;
  leave_critical_section(flags);
#endif  
  return OK;
}


#if !defined(CONFIG_SCHED_TICKLESS_ALARM)&&defined(CONFIG_ARCH_TIMER)

/****************************************************************************
 * Name: up_timer_cancel
 *
 * Description:
 *   Cancel the interval timer and return the time remaining on the timer.
 *   These two steps need to be as nearly atomic as possible.
 *   nxsched_timer_expiration() will not be called unless the timer is
 *   restarted with up_timer_start().
 *
 *   If, as a race condition, the timer has already expired when this
 *   function is called, then that pending interrupt must be cleared so
 *   that up_timer_start() and the remaining time of zero should be
 *   returned.
 *
 *   NOTE: This function may execute at a high rate with no timer running (as
 *   when pre-emption is enabled and disabled).
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Location to return the remaining time.  Zero should be returned
 *        if the timer is not active.  ts may be zero in which case the
 *        time remaining is not returned.
 *
 * Returned Value:
 *   Zero (OK) is returned on success.  A call to up_timer_cancel() when
 *   the timer is not active should also return success; a negated errno
 *   value is returned on any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/
int up_timer_tick_cancel(FAR clock_t *ticks)
{
    if (ticks) {
        struct timespec ts;
        
        up_timer_cancel(&ts);
        *ticks=ts.tv_nsec/NSEC_PER_TICK;
        *ticks+= ts.tv_sec*TICK_PER_SEC;
    }
    else
        up_timer_cancel(NULL);
    return OK;
}


int up_timer_tick_start(clock_t ticks)
{
    struct timespec ts;

    ts.tv_nsec = (ticks%TICK_PER_SEC)*NSEC_PER_TICK;
    ts.tv_sec = ticks/TICK_PER_SEC;
    up_timer_start(&ts);
    return OK;
}

int up_timer_gettick(FAR clock_t *ticks)
{
    struct timespec ts;

    if (ticks) {
        up_timer_gettime(&ts);
        *ticks=ts.tv_nsec/NSEC_PER_TICK;
        *ticks+=ts.tv_sec*TICK_PER_SEC;
    }
    return OK;
}

#endif

int up_alarm_cancel(struct timespec *ts)
{
  irqstate_t flags;
  uint64_t usec;
  uint64_t sec;
  uint64_t nsec;
  uint32_t count;
  uint32_t period;
#if 1
  /* Was the timer running? */

  flags = enter_critical_section();
  if (!g_tickless.pending)
    {
      /* No.. Just return zero timer remaining and successful cancellation.
       * This function may execute at a high rate with no timer running
       * (as when pre-emption is enabled and disabled).
       */

      if (ts)
        {
          ts->tv_sec  = 0;
          ts->tv_nsec = 0;
        }

      leave_critical_section(flags);
      return OK;
    }

  /* Yes.. Get the timer counter and period registers and disable the compare
   * interrupt.
   */

  tmrinfo("Cancelling...\n");

  /* Disable the interrupt. */

  __HAL_LPTIM_DISABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCIE);
  __HAL_LPTIM_DISABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCWE);

  count  = lp_timer_counter_get(&g_tickless);
  period = g_tickless.period;

  g_tickless.pending = false;
  leave_critical_section(flags);

  /* Did the caller provide us with a location to return the time
   * remaining?
   */

  if (ts != NULL)
    {
      /* Yes.. then calculate and return the time remaining on the
       * oneshot timer.
       */

      tmrinfo("period=%lu count=%lu\n",
             (unsigned long)period, (unsigned long)count);

      if (count > period)
        {
          /* Handle rollover */
          period += LPTIM_MAX_CNT + 1;
        }
      else if (count == period)
        {
          /* No time remaining */

          ts->tv_sec  = 0;
          ts->tv_nsec = 0;
          return OK;
        }

      /* The total time remaining is the difference.  Convert that
       * to units of microseconds.
       *
       *   frequency = ticks / second
       *   seconds   = ticks * frequency
       *   usecs     = (ticks * USEC_PER_SEC) / frequency;
       */

      usec        = (((uint64_t)(period - count)) * USEC_PER_SEC) /
                    g_tickless.frequency;

      /* Return the time remaining in the correct form */

      sec         = usec / USEC_PER_SEC;
      nsec        = ((usec) - (sec * USEC_PER_SEC)) * NSEC_PER_USEC;

      ts->tv_sec  = (time_t)sec;
      ts->tv_nsec = (unsigned long)nsec;

      tmrinfo("remaining (%lu, %lu)\n",
             (unsigned long)ts->tv_sec, (unsigned long)ts->tv_nsec);
    }
#endif
  return OK;
}
/****************************************************************************
 * Name: up_alarm_start
 ****************************************************************************/

int up_alarm_start(const struct timespec *ts)
{
  uint64_t usec;
  uint64_t period;
  irqstate_t flags;

  tmrinfo("ts=(%lu, %lu)\n",
          (unsigned long)ts->tv_sec, (unsigned long)ts->tv_nsec);
  DEBUGASSERT(ts);

  /* Was an interval already running? */

  flags = enter_critical_section();
  if (g_tickless.pending)
    {
      /* Yes.. then cancel it */

      tmrinfo("Already running... cancelling\n");
      up_alarm_cancel(NULL);
    }

  /* Express the delay in microseconds */

  usec = (uint64_t)ts->tv_sec * USEC_PER_SEC +
         (uint64_t)(ts->tv_nsec / NSEC_PER_USEC);

  /* Get the timer counter frequency and determine the number of counts need
   * to achieve the requested delay.
   *
   *   frequency = ticks / second
   *   ticks     = seconds * frequency
   *             = (usecs * frequency) / USEC_PER_SEC;
   */

  period = (usec * (uint64_t)g_tickless.frequency) / USEC_PER_SEC;

  tmrinfo("usec=%llu period=%08llx\n", usec, period);

  /* Set interval compare value. Rollover is fine,
   * channel will trigger on the next period.
   */
  g_tickless.period = period;
  if (g_tickless.period > LPTIM_MAX_CNT)
  {
    g_tickless.period -= (LPTIM_MAX_CNT + 1);
  }

  __HAL_LPTIM_COMPARE_SET(&g_tickless.tim_handle, g_tickless.period);

  /* Enable interrupts.  We should get the callback when the interrupt
   * occurs.
   */

  /* Output compare Interrupt Enablet */
  __HAL_LPTIM_ENABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCIE);
  __HAL_LPTIM_ENABLE_IT(&g_tickless.tim_handle, LPTIM_IT_OCWE);

  g_tickless.pending = true;
  leave_critical_section(flags);


  return OK;
}

#else /* !CONFIG_SCHED_TICKLESS */

/****************************************************************************
 * Name: up_timer_initialize
 *
 * Description:
 *   This function is called during start-up to initialize
 *   the timer interrupt for non-tickless mode.
 *
 ****************************************************************************/

void up_timer_initialize(void)
{
  FAR struct timer_lowerhalf_s *lower;
  uint32_t systick_clock;

  /* In non-tickless mode with CONFIG_TIMER_ARCH enabled, hook SysTick
   * through the common timer lower-half path so timekeeping and sleep APIs
   * share one consistent backend.
   */

  systick_clock = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
  if (systick_clock == 0)
    {
      tmrerr("ERROR: invalid HCLK for SysTick\n");
      return;
    }

  lower = systick_initialize(true, systick_clock, -1);
  if (lower == NULL)
    {
      tmrerr("ERROR: systick_initialize failed\n");
      return;
    }

  up_timer_set_lowerhalf(lower);
}

#endif /* CONFIG_SCHED_TICKLESS */

/**
 * @brief 按当前 HCLK 重新装定 SysTick（LOAD），让 tick 频率恒为 100 Hz。
 *
 * @details SysTick 是在 `up_timer_initialize()` 里按**当时**的 HCLK 装定的
 *          （通常是 48 MHz），但之后 HCLK 还会变两次：NAND 启动时 board bring-up
 *          在更晚的阶段把 HCPU 提频，运行期 DVFS 又会切档。LB52 的 `ConfigHCLK`
 *          **不会**自己改写 SysTick，所以每次切档后必须把 LOAD 跟着 HCLK 更新，
 *          否则 `CLOCK_MONOTONIC` / `poll` / `usleep` 都会偏离 `CONFIG_USEC_PER_TICK`。
 *
 *          本函数做三件事：重新装定 SysTick、`TIM_SETTIMEOUT`、`TIM_START`，
 *          并在重装后打一行取证（`CLK: SysTick rearm #N ticks A->B ctrl=… val=…
 *          load=…`，前 4 次和之后每 30 次一条）。**`load` 应当恒等于
 *          上一档 HCLK/100**，`en/int/src` 恒为 1 —— 这是"时间基准没坏"的判据，
 *          详见 docs/gnss_ble_diag_log.md §2.3。
 *
 * @warning **不要再调 `up_timer_set_lowerhalf()`**。那会重启 arch_timer、
 *          从头重挂 SysTick；而在 idle DVFS 之后它会让 `poll()` 的看门狗超时
 *          **永久卡住**（现场表现：GNSS 读线程与 diag join 挂死、BLE 的 UI 数据
 *          不再刷新）。切档只需要上面那三件事。
 */

void sifli_systick_reclock(void)
{
#ifndef CONFIG_SCHED_TICKLESS
  FAR struct timer_lowerhalf_s *lower;
  uint32_t systick_clock;

  systick_clock = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
  if (systick_clock == 0)
    {
      tmrerr("ERROR: sifli_systick_reclock: HCLK is 0\n");
      return;
    }

  SystemCoreClock = systick_clock;

  /* Refresh g_systick_lower.freq. This briefly clears ENABLE; START below
   * turns it back on with the new LOAD. Keep the original arch_timer
   * callback / timebase.
   */

  /* 时基取证：上面这段注释里那句 "after idle DVFS has left poll() watchdog
   * timeouts stuck (GNSS reader + diag join hang, BLE UI stale)" 就是现场抓到过的
   * 三个症状，而本函数是"避开 up_timer_set_lowerhalf()"之后的版本 —— 症状却
   * 依然出现（16:51 版日志里 GNSS 读线程卡了 80 秒）。所以这里量三件事：
   *   ticks : 软件 tick 计数（clock_systime_ticks），应只增加一两个；**没动或
   *           回退 = 绝对 tick 的 wdog 截止时间失效** → 所有 poll/usleep 类超时
   *           长时间不触发；
   *   ctrl  : SysTick CTRL，bit0=ENABLE、bit1=TICKINT、bit2=CLKSOURCE、
   *           bit16=COUNTFLAG。**after 时 bit0=0 就是"表停了"** —— 直接解释
   *           80 秒卡死；
   *   val/load : 计数与重载是否合理（val 应 < load）。
   * 节奏上只打前几次 + 之后每 30 次一次，避免 hop 压测把日志刷满。
   */
  static unsigned rearm_n;
  uint32_t ticks_before = (uint32_t)clock_systime_ticks();
  uint32_t ctrl_after;
  uint32_t val_after;
  uint32_t load_after;

  lower = systick_initialize(true, systick_clock, -1);
  if (lower == NULL)
    {
      tmrerr("ERROR: sifli_systick_reclock: systick_initialize failed\n");
      return;
    }

  ctrl_after = *(volatile uint32_t *)0xe000e010u;
  val_after  = *(volatile uint32_t *)0xe000e018u;
  load_after = *(volatile uint32_t *)0xe000e014u;

  (void)TIMER_SETTIMEOUT(lower, CONFIG_USEC_PER_TICK);
  (void)TIMER_START(lower);

  rearm_n++;
  if (rearm_n <= 4u || (rearm_n % 30u) == 0u)
    {
      syslog(LOG_INFO,
             "CLK: SysTick rearm #%u ticks %lu->%lu ctrl=0x%08lx(en=%lu int=%lu src=%lu flg=%lu) val=%lu load=%lu\n",
             rearm_n, (unsigned long)ticks_before,
             (unsigned long)(uint32_t)clock_systime_ticks(),
             (unsigned long)ctrl_after,
             (unsigned long)(ctrl_after & 1u),
             (unsigned long)((ctrl_after >> 1) & 1u),
             (unsigned long)((ctrl_after >> 2) & 1u),
             (unsigned long)((ctrl_after >> 16) & 1u),
             (unsigned long)val_after, (unsigned long)load_after);
    }

  syslog(LOG_INFO, "CLK: SysTick reclocked to HCLK=%lu Hz\n",
         (unsigned long)systick_clock);
#endif
}
