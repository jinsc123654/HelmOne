/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb_rtc.c
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

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/timers/rtc.h>
#include <nuttx/timers/arch_rtc.h>

#include "arm_internal.h"
#include "bf0_hal.h"
#include "sf32lb_rtc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Backup register index for RTC trust magic (persists in VBAT domain). */
#define SF32LB_RTC_TRUST_BKP_IDX  0u

#if defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_PERIODIC)
typedef CODE void (*sf32lb_rtc_callback_t)(FAR void *priv, int alarmid);

struct sf32lb_cbinfo_s
{
  volatile sf32lb_rtc_callback_t cb;
  volatile FAR void *priv;
};
#endif

struct sf32lb_lowerhalf_s
{
  FAR const struct rtc_ops_s *ops;

  uint8_t initialized;
  uint8_t reason;
  RTC_HandleTypeDef rtc_handler;

#ifdef CONFIG_RTC_ALARM
  struct sf32lb_cbinfo_s cbinfo;
#endif

#ifdef CONFIG_RTC_PERIODIC
  struct sf32lb_cbinfo_s periodic;
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sf32lb_rdtime(FAR struct rtc_lowerhalf_s *lower,
                         FAR struct rtc_time *rtctime);
static int sf32lb_settime(FAR struct rtc_lowerhalf_s *lower,
                          FAR const struct rtc_time *rtctime);
static bool sf32lb_havesettime(FAR struct rtc_lowerhalf_s *lower);

#ifdef CONFIG_RTC_ALARM
static int sf32lb_setalarm(FAR struct rtc_lowerhalf_s *lower,
                           FAR const struct lower_setalarm_s *alarminfo);
static int sf32lb_setrelative(FAR struct rtc_lowerhalf_s *lower,
                              FAR const struct lower_setrelative_s *alarminfo);
static int sf32lb_cancelalarm(FAR struct rtc_lowerhalf_s *lower, int alarmid);
static int sf32lb_rdalarm(FAR struct rtc_lowerhalf_s *lower,
                          FAR struct lower_rdalarm_s *alarminfo);
#endif

#ifdef CONFIG_RTC_PERIODIC
static int sf32lb_setperiodic(FAR struct rtc_lowerhalf_s *lower,
                              FAR const struct lower_setperiodic_s *alarminfo);
static int sf32lb_cancelperiodic(FAR struct rtc_lowerhalf_s *lower, int id);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rtc_ops_s g_rtc_ops =
{
  .rdtime      = sf32lb_rdtime,
  .settime     = sf32lb_settime,
  .havesettime = sf32lb_havesettime,
#ifdef CONFIG_RTC_ALARM
  .setalarm    = sf32lb_setalarm,
  .setrelative = sf32lb_setrelative,
  .cancelalarm = sf32lb_cancelalarm,
  .rdalarm     = sf32lb_rdalarm,
#endif
#ifdef CONFIG_RTC_PERIODIC
  .setperiodic    = sf32lb_setperiodic,
  .cancelperiodic = sf32lb_cancelperiodic,
#endif
};

static struct sf32lb_lowerhalf_s g_rtc_lowerhalf =
{
  .ops = &g_rtc_ops,
};

/* SiFli HAL stores Year as offset from 1900 (0-199), with 0-69 meaning 2000-2069.
 * NuttX rtc_time.tm_year is years since 1900 (126 for 2026).
 */

static uint8_t rtc_tm_year_to_chip(int tm_year)
{
  int year = tm_year + 1900;

  if (year >= 2000 && year < 2070)
    {
      return (uint8_t)(year - 2000);
    }

  return (uint8_t)(year - 1900);
}

static int chip_year_to_rtc_tm_year(uint8_t chip_year)
{
  if (chip_year < 70)
    {
      return (2000 + chip_year) - 1900;
    }

  return (int)chip_year;
}

static void sf32lb_rtc_trust_mark(FAR struct sf32lb_lowerhalf_s *lh)
{
  HAL_RTC_set_backup(&lh->rtc_handler, SF32LB_RTC_TRUST_BKP_IDX,
                     SF32LB_RTC_TRUST_MAGIC);
}

bool sf32lb_rtc_trust_is_valid(void)
{
  if (!g_rtc_lowerhalf.initialized)
    {
      return false;
    }

  return HAL_RTC_get_backup(&g_rtc_lowerhalf.rtc_handler,
                            SF32LB_RTC_TRUST_BKP_IDX) == SF32LB_RTC_TRUST_MAGIC;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#if defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_PERIODIC)
static int sf32lb_alarm_callback(int irq, FAR void *context, FAR void *arg)
{
  FAR struct sf32lb_lowerhalf_s *lower;
  FAR struct sf32lb_cbinfo_s *cbinfo = NULL;
  sf32lb_rtc_callback_t cb = NULL;
  FAR void *priv = NULL;

  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  lower = &g_rtc_lowerhalf;

  HAL_RTC_IRQHandler(&lower->rtc_handler);

#ifdef CONFIG_RTC_ALARM
  if (lower->reason == RTC_CBK_ALARM)
    {
      cbinfo = &lower->cbinfo;
    }
#endif

#ifdef CONFIG_RTC_PERIODIC
  if (lower->reason == RTC_CBK_WAKEUP)
    {
      cbinfo = &lower->periodic;
    }
#endif

  if (cbinfo != NULL)
    {
      cb = (sf32lb_rtc_callback_t)cbinfo->cb;
      priv = (FAR void *)cbinfo->priv;
      cbinfo->cb = NULL;
      cbinfo->priv = NULL;
    }

  if (cb != NULL)
    {
      cb(priv, 0);
    }

  return OK;
}
#endif

static int sf32lb_rdtime(FAR struct rtc_lowerhalf_s *lower,
                         FAR struct rtc_time *rtctime)
{
  FAR struct sf32lb_lowerhalf_s *lh = (FAR struct sf32lb_lowerhalf_s *)lower;
  RTC_TimeTypeDef rtc_time;
  RTC_DateTypeDef rtc_date;

  memset(rtctime, 0, sizeof(struct rtc_time));
  memset(&rtc_time, 0, sizeof(rtc_time));
  memset(&rtc_date, 0, sizeof(rtc_date));

  HAL_RTC_GetTime(&lh->rtc_handler, &rtc_time, RTC_FORMAT_BIN);
  while (HAL_RTC_GetDate(&lh->rtc_handler, &rtc_date, RTC_FORMAT_BIN) == HAL_ERROR)
    {
      HAL_RTC_GetTime(&lh->rtc_handler, &rtc_time, RTC_FORMAT_BIN);
    }

  rtctime->tm_sec = rtc_time.Seconds;
  rtctime->tm_min = rtc_time.Minutes;
  rtctime->tm_hour = rtc_time.Hours;
  rtctime->tm_mday = rtc_date.Date;
  rtctime->tm_mon = rtc_date.Month - 1;
  rtctime->tm_wday = rtc_date.WeekDay - 1;
  rtctime->tm_year = chip_year_to_rtc_tm_year(rtc_date.Year);

#if defined(CONFIG_RTC_HIRES) || defined(CONFIG_ARCH_HAVE_RTC_SUBSECONDS)
  rtctime->tm_nsec = rtc_time.SubSeconds * 1000000 / 256 * 1000;
#endif

  return OK;
}

static int sf32lb_settime(FAR struct rtc_lowerhalf_s *lower,
                          FAR const struct rtc_time *rtctime)
{
  FAR struct sf32lb_lowerhalf_s *lh = (FAR struct sf32lb_lowerhalf_s *)lower;
  RTC_TimeTypeDef rtc_time;
  RTC_DateTypeDef rtc_date;

  memset(&rtc_time, 0, sizeof(rtc_time));
  memset(&rtc_date, 0, sizeof(rtc_date));

  rtc_time.Seconds = rtctime->tm_sec;
  rtc_time.Minutes = rtctime->tm_min;
  rtc_time.Hours = rtctime->tm_hour;

  rtc_date.Date = rtctime->tm_mday;
  rtc_date.Month = rtctime->tm_mon + 1;
  rtc_date.Year = rtc_tm_year_to_chip(rtctime->tm_year);
  rtc_date.WeekDay = rtctime->tm_wday + 1;

  if (HAL_RTC_SetTime(&lh->rtc_handler, &rtc_time, RTC_FORMAT_BIN) != HAL_OK)
    {
      return -EIO;
    }

  if (HAL_RTC_SetDate(&lh->rtc_handler, &rtc_date, RTC_FORMAT_BIN) != HAL_OK)
    {
      return -EIO;
    }

  sf32lb_rtc_trust_mark(lh);

  if (HAL_PMU_LXT_DISABLED())
    {
      /* Reserved for future software calibration workflow. */
    }

  return OK;
}

static bool sf32lb_havesettime(FAR struct rtc_lowerhalf_s *lower)
{
  FAR struct sf32lb_lowerhalf_s *lh = (FAR struct sf32lb_lowerhalf_s *)lower;

  if (!lh->initialized)
    {
      return false;
    }

  return HAL_RTC_get_backup(&lh->rtc_handler, SF32LB_RTC_TRUST_BKP_IDX)
         == SF32LB_RTC_TRUST_MAGIC;
}

int up_rtc_settime(FAR const struct timespec *tp)
{
  struct tm newtime;

  gmtime_r(&tp->tv_sec, &newtime);
  return sf32lb_settime((FAR struct rtc_lowerhalf_s *)&g_rtc_lowerhalf,
                        (FAR const struct rtc_time *)&newtime);
}

#ifdef CONFIG_RTC_ALARM
static int sf32lb_setalarm(FAR struct rtc_lowerhalf_s *lower,
                           FAR const struct lower_setalarm_s *alarminfo)
{
  FAR struct sf32lb_lowerhalf_s *lh;
  FAR struct sf32lb_cbinfo_s *cbinfo;
  RTC_AlarmTypeDef alarm;

  DEBUGASSERT(lower != NULL && alarminfo != NULL);

  lh = (FAR struct sf32lb_lowerhalf_s *)lower;
  cbinfo = &lh->cbinfo;
  cbinfo->cb = (sf32lb_rtc_callback_t)alarminfo->cb;
  cbinfo->priv = alarminfo->priv;
  lh->reason = RTC_CBK_ALARM;

  memset(&alarm, 0, sizeof(alarm));
  alarm.AlarmTime.Hours = alarminfo->time.tm_hour;
  alarm.AlarmTime.Minutes = alarminfo->time.tm_min;
  alarm.AlarmTime.Seconds = alarminfo->time.tm_sec;
  alarm.AlarmDate.Date = alarminfo->time.tm_mday;
  alarm.AlarmDate.Month = alarminfo->time.tm_mon + 1;
  alarm.AlarmDate.Year = alarminfo->time.tm_year;
  alarm.AlarmDate.WeekDay = alarminfo->time.tm_wday + 1;

  /* Use high precision on subsecond match to avoid repetitive IRQ trigger. */
  alarm.AlarmMask = (10 << RTC_ALRMDR_MSKSS_Pos);

  if (HAL_RTC_SetAlarm(&lh->rtc_handler, &alarm, RTC_FORMAT_BIN) != HAL_OK)
    {
      return -EIO;
    }

  return OK;
}

static void add_timeout(FAR struct rtc_time *rtc_tm, time_t delta)
{
  time_t timesp;
  FAR struct tm *tm;

  timesp = mktime((FAR struct tm *)rtc_tm);
  timesp += delta;

  tm = localtime(&timesp);
  if (tm == NULL)
    {
      return;
    }

  rtc_tm->tm_sec = tm->tm_sec;
  rtc_tm->tm_min = tm->tm_min;
  rtc_tm->tm_hour = tm->tm_hour;
  rtc_tm->tm_mday = tm->tm_mday;
  rtc_tm->tm_mon = tm->tm_mon;
  rtc_tm->tm_year = tm->tm_year;
  rtc_tm->tm_wday = tm->tm_wday;
  rtc_tm->tm_yday = tm->tm_yday;
  rtc_tm->tm_isdst = tm->tm_isdst;
}

static int sf32lb_setrelative(FAR struct rtc_lowerhalf_s *lower,
                              FAR const struct lower_setrelative_s *alarminfo)
{
  FAR struct sf32lb_lowerhalf_s *lh;
  FAR struct sf32lb_cbinfo_s *cbinfo;
  struct rtc_time alarm_tm;
  RTC_AlarmTypeDef alarm;

  DEBUGASSERT(lower != NULL && alarminfo != NULL);

  sf32lb_rdtime(lower, &alarm_tm);
  add_timeout(&alarm_tm, alarminfo->reltime);

  lh = (FAR struct sf32lb_lowerhalf_s *)lower;
  cbinfo = &lh->cbinfo;
  cbinfo->cb = (sf32lb_rtc_callback_t)alarminfo->cb;
  cbinfo->priv = alarminfo->priv;
  lh->reason = RTC_CBK_ALARM;

  memset(&alarm, 0, sizeof(alarm));
  alarm.AlarmTime.Hours = alarm_tm.tm_hour;
  alarm.AlarmTime.Minutes = alarm_tm.tm_min;
  alarm.AlarmTime.Seconds = alarm_tm.tm_sec;
  alarm.AlarmDate.Date = alarm_tm.tm_mday;
  alarm.AlarmDate.Month = alarm_tm.tm_mon + 1;
  alarm.AlarmDate.Year = alarm_tm.tm_year;
  alarm.AlarmDate.WeekDay = alarm_tm.tm_wday + 1;
  alarm.AlarmMask = (10 << RTC_ALRMDR_MSKSS_Pos);

  if (HAL_RTC_SetAlarm(&lh->rtc_handler, &alarm, RTC_FORMAT_BIN) != HAL_OK)
    {
      return -EIO;
    }

  return OK;
}

static int sf32lb_cancelalarm(FAR struct rtc_lowerhalf_s *lower, int alarmid)
{
  FAR struct sf32lb_lowerhalf_s *lh = (FAR struct sf32lb_lowerhalf_s *)lower;

  UNUSED(alarmid);
  HAL_RTC_DeactivateAlarm(&lh->rtc_handler);
  return OK;
}

static int sf32lb_rdalarm(FAR struct rtc_lowerhalf_s *lower,
                          FAR struct lower_rdalarm_s *alarminfo)
{
  FAR struct sf32lb_lowerhalf_s *lh;
  RTC_AlarmTypeDef alarm;

  DEBUGASSERT(lower != NULL && alarminfo != NULL && alarminfo->time != NULL);

  lh = (FAR struct sf32lb_lowerhalf_s *)lower;
  memset(&alarm, 0, sizeof(alarm));

  HAL_RTC_GetAlarm(&lh->rtc_handler, &alarm, RTC_FORMAT_BIN);

  alarminfo->time->tm_sec = alarm.AlarmTime.Seconds;
  alarminfo->time->tm_min = alarm.AlarmTime.Minutes;
  alarminfo->time->tm_hour = alarm.AlarmTime.Hours;
  alarminfo->time->tm_mday = alarm.AlarmDate.Date;
  alarminfo->time->tm_mon = alarm.AlarmDate.Month - 1;
  alarminfo->time->tm_wday = alarm.AlarmDate.WeekDay - 1;
  alarminfo->time->tm_year = alarm.AlarmDate.Year;

  return OK;
}
#endif

#ifdef CONFIG_RTC_PERIODIC
static int sf32lb_setperiodic(FAR struct rtc_lowerhalf_s *lower,
                              FAR const struct lower_setperiodic_s *alarminfo)
{
  FAR struct sf32lb_lowerhalf_s *lh;
  uint32_t counter;

  DEBUGASSERT(lower != NULL && alarminfo != NULL);

  lh = (FAR struct sf32lb_lowerhalf_s *)lower;

  /* RTC subsecond tick is 1/256 second. */
  counter = alarminfo->period.tv_nsec / 1000000 * 256 / 1000;
  counter += alarminfo->period.tv_sec * 256;
  lh->reason = RTC_CBK_WAKEUP;

  if (HAL_RTC_SetWakeUpTimer(&lh->rtc_handler, counter, 1) != HAL_OK)
    {
      return -EIO;
    }

  return OK;
}

static int sf32lb_cancelperiodic(FAR struct rtc_lowerhalf_s *lower, int id)
{
  FAR struct sf32lb_lowerhalf_s *lh;

  DEBUGASSERT(lower != NULL);
  UNUSED(id);

  lh = (FAR struct sf32lb_lowerhalf_s *)lower;
  HAL_RTC_DeactivateWakeUpTimer(&lh->rtc_handler);
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct rtc_lowerhalf_s *sf32lb_rtc_lowerhalf(void)
{
  return (FAR struct rtc_lowerhalf_s *)&g_rtc_lowerhalf;
}

int up_rtc_getdatetime(FAR struct tm *tp)
{
  return sf32lb_rdtime((FAR struct rtc_lowerhalf_s *)&g_rtc_lowerhalf,
                       (FAR struct rtc_time *)tp);
}

int up_rtc_initialize(void)
{
  rtcinfo("RTC initialization\n");

  g_rtc_lowerhalf.rtc_handler.Instance = hwp_rtc;
  g_rtc_lowerhalf.rtc_handler.Init.DivAInt = 0x80;
  g_rtc_lowerhalf.rtc_handler.Init.DivAFrac = 0x0;
  g_rtc_lowerhalf.rtc_handler.Init.DivB = 0x100;
  g_rtc_lowerhalf.rtc_handler.Init.HourFormat = RTC_HOURFORMAT_24;

  if (HAL_RTC_Init(&g_rtc_lowerhalf.rtc_handler, 0) != HAL_OK)
    {
      return -EIO;
    }

#if defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_PERIODIC)
  irq_attach(RTC_IRQn + 16, sf32lb_alarm_callback, &g_rtc_lowerhalf);
  up_enable_irq(RTC_IRQn + 16);
#endif

  g_rtc_lowerhalf.initialized = 1;
  up_rtc_set_lowerhalf((FAR struct rtc_lowerhalf_s *)&g_rtc_lowerhalf, true);
  return OK;
}
