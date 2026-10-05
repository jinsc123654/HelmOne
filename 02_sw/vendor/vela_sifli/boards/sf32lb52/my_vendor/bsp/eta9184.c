/**
 * @file eta9184.c
 * @brief ETA9184：EN 默认关；STAT/DISCHRG/PULSE 用 GPIO 中断；4 秒窗口用独立软件定时器。
 *
 * 不要用 LPWORK work_queue 做周期任务：那会反复启动整条队列共用的
 * watchdog，系统节拍 10 ms 时容易把 g_wdactivelist 插坏（hardfault in
 * wd_insert）。窗口定时与按键一样用自己的 struct wdog_s + wd_start。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/wdog.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include "bf0_hal.h"
#include "drv_io.h"
#include "eta9184.h"
#include "sifli_gpio.h"

#define ETA_STAT_PIN            GET_PIN_2(hwp_gpio1, 25) /**< PA25 STAT。 */
#define ETA_DISCHG_PIN          GET_PIN_2(hwp_gpio1, 24) /**< PA24 DISCHRG。 */
#define ETA_PULSE_PIN           GET_PIN_2(hwp_gpio1, 26) /**< PA26 PULSE。 */
#define ETA_EN_GPIO             28 /**< PA28 ENBST Boost。 */

#define ETA_PULSE_DEBOUNCE_MS   20   /**< PULSE 上升沿去抖。 */
#define ETA_EDGE_DEBOUNCE_MS    5    /**< STAT/DISCHRG 边沿去抖。 */
#define ETA_WINDOW_MS           4000 /**< 电量统计窗口。 */
#define ETA_DISCHG_LOW_EDGES    6    /**< 低电量 DISCHRG 边沿阈值。 */

static bool g_inited;                          /**< 是否已 eta9184_init。 */
static volatile bool g_running;                /**< 采样是否在运行。 */
static bool g_irq_on;                          /**< GPIO 中断是否已使能。 */
static bool g_boost_on;                        /**< Boost 软件开关。 */
static volatile bool g_stat_high;              /**< STAT 最近稳定电平。 */
static volatile bool g_dischg_high;            /**< DISCHRG 最近稳定电平。 */
static volatile bool g_pulse_high;             /**< PULSE 窗口结束时电平。 */
static volatile int g_pulse_count;             /**< 当前窗口 PULSE 次数。 */
static volatile int g_dischg_edges;            /**< 当前窗口 DISCHRG 边沿数。 */
static volatile int g_bat_level;               /**< 0 未知，1～4 四分位，5 满。 */
static volatile int g_dischg_status;           /**< DISCHRG 细粒度状态。 */
static volatile int g_chg_status;              /**< STAT 细粒度状态。 */
static volatile bool g_full_edge;             /**< 刚进入充满，待 VBATS 校准。 */
static volatile unsigned int g_timer_ticks;    /**< 4 秒软件定时器次数。 */
static volatile unsigned int g_pulse_edges;    /**< 累计 PULSE 上升沿。 */
static volatile unsigned int g_dischg_edge_total; /**< 累计 DISCHRG 边沿。 */
static volatile unsigned int g_windows;        /**< 已完成 4 秒窗口数。 */
static volatile clock_t g_last_pulse_tick;     /**< 上次接受的 PULSE 沿。 */
static volatile clock_t g_last_dischg_tick;    /**< 上次接受的 DISCHRG 沿。 */
static volatile clock_t g_last_stat_tick;      /**< 上次接受的 STAT 沿。 */
static struct wdog_s g_wdog;                   /**< 独立 4 秒软件定时器。 */

/** @brief 读 ETA9184 GPIO 原始电平。 */
static bool eta_gpio_high(uint32_t pin)
{
  return sifli_gpio_read(pin);
}

/** @brief 配置上拉并 mux ETA9184 状态脚。 */
static void eta_status_pullup(void)
{
  HAL_PIN_Set(PAD_PA24, GPIO_A24, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA25, GPIO_A25, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_PULLUP, 1);
}

/** @brief 根据当前电平更新充放电细粒度状态。 */
static void eta_apply_charge_discharge(void)
{
  if (g_dischg_edges >= ETA_DISCHG_LOW_EDGES)
    {
      g_dischg_status = ETA9184_DISCHG_LOW;
    }
  else if (!g_dischg_high)
    {
      g_dischg_status = ETA9184_DISCHG_ON;
    }
  else
    {
      g_dischg_status = ETA9184_DISCHG_OFF;
    }

  if (!g_stat_high)
    {
      g_chg_status = ETA9184_CHG_CHARGING;
      g_full_edge = false;
    }
  else if (g_dischg_high)
    {
      /* 只在充电→充满边沿校准。开机/空闲时 STAT 高+DISCHRG 高与充满相同，
       * 若当边沿会把当前电压（可能已是 3.98 V）写成 100%。 */
      if (g_chg_status == ETA9184_CHG_CHARGING)
        {
          g_full_edge = true;
        }

      g_chg_status = ETA9184_CHG_FULL;
    }
  else
    {
      g_chg_status = ETA9184_CHG_IDLE;
      g_full_edge = false;
    }
}

/** @brief 4 秒窗口结束：映射 PULSE 次数为 bat_level。ISR 可调用。 */
static void eta_cycle_end(void)
{
  int count = g_pulse_count;
  bool held_high;

  g_pulse_high = eta_gpio_high(ETA_PULSE_PIN);
  g_stat_high = eta_gpio_high(ETA_STAT_PIN);
  g_dischg_high = eta_gpio_high(ETA_DISCHG_PIN);

  held_high = (count == 0 && g_pulse_high);

  if (held_high || count >= 4)
    {
      g_bat_level = 5;
    }
  else if (count >= 1)
    {
      g_bat_level = count;
    }

  g_pulse_count = 0;
  g_dischg_edges = 0;
  g_windows++;
  g_timer_ticks++;
  eta_apply_charge_discharge();
}

/** @brief 独立软件定时器到期：关窗口并再开 4 秒。 */
static void eta_window_cb(wdparm_t arg)
{
  UNUSED(arg);

  eta_cycle_end();

  if (g_running)
    {
      (void)wd_start(&g_wdog, MSEC2TICK(ETA_WINDOW_MS), eta_window_cb, 0);
    }
}

/** @brief 距上次边沿的滴答；回退则视为 0，避免去抖失效。 */
static clock_t eta_tick_age(clock_t now, clock_t last)
{
  clock_t dt = now - last;

  if (dt > ((clock_t)-1 / 2))
    {
      return 0;
    }

  return dt;
}

/** @brief PA26 上升沿：计一次电量脉冲。 */
static void eta_pulse_isr(void *arg)
{
  clock_t now = clock_systime_ticks();

  UNUSED(arg);
  if (eta_tick_age(now, g_last_pulse_tick) < MSEC2TICK(ETA_PULSE_DEBOUNCE_MS))
    {
      return;
    }

  g_last_pulse_tick = now;
  g_pulse_count++;
  g_pulse_edges++;
}

/** @brief PA24 双沿：计 DISCHRG 翻转（低电约 1 Hz）。 */
static void eta_dischg_isr(void *arg)
{
  clock_t now = clock_systime_ticks();

  UNUSED(arg);
  if (eta_tick_age(now, g_last_dischg_tick) < MSEC2TICK(ETA_EDGE_DEBOUNCE_MS))
    {
      return;
    }

  g_last_dischg_tick = now;
  g_dischg_edges++;
  g_dischg_edge_total++;
  g_dischg_high = eta_gpio_high(ETA_DISCHG_PIN);
  eta_apply_charge_discharge();
}

/** @brief PA25 双沿：充电 STAT。 */
static void eta_stat_isr(void *arg)
{
  clock_t now = clock_systime_ticks();

  UNUSED(arg);
  if (eta_tick_age(now, g_last_stat_tick) < MSEC2TICK(ETA_EDGE_DEBOUNCE_MS))
    {
      return;
    }

  g_last_stat_tick = now;
  g_stat_high = eta_gpio_high(ETA_STAT_PIN);
  eta_apply_charge_discharge();
}

/** @brief 使能或关闭状态脚外部中断。 */
static int eta_irq_set(bool enable)
{
  int ret;

  if (enable == g_irq_on)
    {
      return 0;
    }

  if (!enable)
    {
      (void)sifli_gpio_set_event(ETA_PULSE_PIN, false, false, NULL, NULL);
      (void)sifli_gpio_set_event(ETA_DISCHG_PIN, false, false, NULL, NULL);
      (void)sifli_gpio_set_event(ETA_STAT_PIN, false, false, NULL, NULL);
      g_irq_on = false;
      return 0;
    }

  ret = sifli_gpio_set_event(ETA_PULSE_PIN, true, false,
                             eta_pulse_isr, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "eta9184: PULSE irq failed: %d\n", ret);
      return ret;
    }

  ret = sifli_gpio_set_event(ETA_DISCHG_PIN, true, true,
                             eta_dischg_isr, NULL);
  if (ret < 0)
    {
      (void)sifli_gpio_set_event(ETA_PULSE_PIN, false, false, NULL, NULL);
      syslog(LOG_ERR, "eta9184: DISCHRG irq failed: %d\n", ret);
      return ret;
    }

  ret = sifli_gpio_set_event(ETA_STAT_PIN, true, true,
                             eta_stat_isr, NULL);
  if (ret < 0)
    {
      (void)sifli_gpio_set_event(ETA_PULSE_PIN, false, false, NULL, NULL);
      (void)sifli_gpio_set_event(ETA_DISCHG_PIN, false, false, NULL, NULL);
      syslog(LOG_ERR, "eta9184: STAT irq failed: %d\n", ret);
      return ret;
    }

  g_irq_on = true;
  return 0;
}

/** @brief 从当前 GPIO 采样并复位边沿去抖。 */
static void eta_level_reset(void)
{
  irqstate_t flags;
  clock_t now;

  g_stat_high = eta_gpio_high(ETA_STAT_PIN);
  g_dischg_high = eta_gpio_high(ETA_DISCHG_PIN);
  g_pulse_high = eta_gpio_high(ETA_PULSE_PIN);

  now = clock_systime_ticks();
  flags = up_irq_save();
  g_last_pulse_tick = now;
  g_last_dischg_tick = now;
  g_last_stat_tick = now;
  up_irq_restore(flags);
}

/**
 * @brief 打开或关闭 5V Boost（ENBST）。
 */
void eta9184_boost_set(bool on)
{
  g_boost_on = on;
  HAL_PIN_Set(PAD_PA28, GPIO_A28, on ? PIN_PULLUP : PIN_PULLDOWN, 1);
  BSP_GPIO_Set(ETA_EN_GPIO, on ? 1 : 0, 1);
}

/**
 * @brief 读取软件记录的 Boost 开关。
 */
bool eta9184_boost_get(void)
{
  return g_boost_on;
}

/**
 * @brief PULSE 窗口估电量（不可信）。
 *
 * @return -1 初始化中；否则 20/40/60/80/100。
 */
int eta9184_battery_percent(void)
{
  irqstate_t flags = up_irq_save();
  int pct;

  switch (g_bat_level)
    {
      case 1:
        pct = 20;
        break;
      case 2:
        pct = 40;
        break;
      case 3:
        pct = 60;
        break;
      case 4:
        pct = 80;
        break;
      case 5:
        pct = 100;
        break;
      default:
        pct = -1;
        break;
    }

  up_irq_restore(flags);
  return pct;
}

/**
 * @brief 充满 IO：STAT 高且 DISCHRG 高。
 */
bool eta9184_charge_full(void)
{
  irqstate_t flags = up_irq_save();
  bool full = (g_chg_status == ETA9184_CHG_FULL);

  up_irq_restore(flags);
  return full;
}

/**
 * @brief 取出并清除“刚进入充满”边沿。
 */
bool eta9184_take_full_edge(void)
{
  irqstate_t flags;
  bool edge;

  flags = up_irq_save();
  edge = g_full_edge;
  g_full_edge = false;
  up_irq_restore(flags);
  return edge;
}

/**
 * @brief 读取充电 / 放电 / 低电量 / 空闲。
 */
enum eta9184_power_e eta9184_power_state(void)
{
  irqstate_t flags = up_irq_save();
  enum eta9184_power_e st;

  /* g_chg_status 与 g_dischg_status 由 ISR / wdog 回调更新。原来连读两次不掩
   * 中断，ISR 插在中间就会返回"跨时刻"的组合。整段读进同一临界区。 */
  if (g_chg_status == ETA9184_CHG_CHARGING)
    {
      st = ETA9184_POWER_CHARGING;
    }
  else if (g_dischg_status == ETA9184_DISCHG_LOW)
    {
      st = ETA9184_POWER_LOW;
    }
  else if (g_dischg_status == ETA9184_DISCHG_ON)
    {
      st = ETA9184_POWER_DISCHARGING;
    }
  else
    {
      st = ETA9184_POWER_IDLE;
    }

  up_irq_restore(flags);
  return st;
}

/**
 * @brief 复制当前采样快照。
 */
void eta9184_get_status(FAR struct eta9184_status_s *out)
{
  irqstate_t flags;

  if (out == NULL)
    {
      return;
    }

  flags = up_irq_save();
  out->chrg_high = eta_gpio_high(ETA_STAT_PIN);
  out->stat_high = eta_gpio_high(ETA_DISCHG_PIN);
  out->pulse_high = eta_gpio_high(ETA_PULSE_PIN);
  out->boost_on = g_boost_on;
  out->bat_level = g_bat_level;
  out->pulse_count = g_pulse_count;
  out->dischg_edges = g_dischg_edges;
  out->timer_ticks = g_timer_ticks;
  out->pulse_irqs = g_pulse_edges;
  out->dischg_irqs = g_dischg_edge_total;
  out->windows = g_windows;
  out->chg = (enum eta9184_chg_e)g_chg_status;
  out->dischg = (enum eta9184_dischg_e)g_dischg_status;
  out->power = eta9184_power_state();
  up_irq_restore(flags);

  out->bat_pct = -1;
  out->pulse_pct = eta9184_battery_percent();
  out->pulse_trusted = false;
}

/**
 * @brief 配置引脚，Boost / EN 默认关。不上报电量。
 *
 * @return 0 成功，负值为错误码。
 */
int eta9184_init(void)
{
  if (g_inited)
    {
      return 0;
    }

  eta_status_pullup();
  sifli_gpio_config(ETA_STAT_PIN, GPIO_INPUT);
  sifli_gpio_config(ETA_DISCHG_PIN, GPIO_INPUT);
  sifli_gpio_config(ETA_PULSE_PIN, GPIO_INPUT);
  eta9184_boost_set(false);

  memset(&g_wdog, 0, sizeof(g_wdog));
  eta_level_reset();
  g_pulse_count = 0;
  g_dischg_edges = 0;
  g_bat_level = 0;
  g_chg_status = ETA9184_CHG_IDLE;
  g_dischg_status = ETA9184_DISCHG_OFF;
  eta_apply_charge_discharge();
  g_timer_ticks = 0;
  g_pulse_edges = 0;
  g_dischg_edge_total = 0;
  g_windows = 0;
  g_irq_on = false;
  g_running = false;
  g_inited = true;
  syslog(LOG_INFO, "eta9184: STAT=PA25 DISCHRG=PA24 PULSE=PA26 EN=off\n");
  return 0;
}

/**
 * @brief 挂 GPIO 中断，启动 4 秒软件定时器。不改 Boost。
 *
 * @return 0 成功，负值为错误码。
 */
int eta9184_start(void)
{
  int ret;

  ret = eta9184_init();
  if (ret < 0)
    {
      return ret;
    }

  if (g_running)
    {
      return 0;
    }

  eta_status_pullup();
  eta_level_reset();
  eta_apply_charge_discharge();
  g_pulse_count = 0;
  g_dischg_edges = 0;
  g_timer_ticks = 0;
  g_pulse_edges = 0;
  g_dischg_edge_total = 0;
  g_windows = 0;

  ret = eta_irq_set(true);
  if (ret < 0)
    {
      return ret;
    }

  g_running = true;
  ret = wd_start(&g_wdog, MSEC2TICK(ETA_WINDOW_MS), eta_window_cb, 0);
  if (ret < 0)
    {
      g_running = false;
      syslog(LOG_ERR, "eta9184: window timer start failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "eta9184: sampling (GPIO irq + 4s wdog, EN %s)\n",
         g_boost_on ? "on" : "off");
  return 0;
}

/**
 * @brief 停止 4 秒软件定时器。不关 EN。
 */
void eta9184_stop(void)
{
  if (!g_running)
    {
      return;
    }

  g_running = false;
  wd_cancel(&g_wdog);
}

/**
 * @brief 采样是否在运行。
 */
bool eta9184_running(void)
{
  return g_running;
}
