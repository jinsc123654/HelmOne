/**
 * @file ble_supervisor.c
 * @brief BLE 角色故障计数、升级与 adapter cycle 限频。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "ble_supervisor.h"
#include "myvendor_ble_log.h"

#define LOGI(fmt, ...) syslog(LOG_INFO, "ble_supervisor: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) syslog(LOG_ERR, "ble_supervisor: " fmt "\n", ##__VA_ARGS__)

/** 状态一份、锁一把。
 *
 *  写者有三个线程：BT 框架线程（广播/连接回调里的 report）、companion 主线
 *  程（pump/tick/cycle）、以及 diag 线程的只读查询。之前**一把锁都没有**，
 *  所以计数与 cycle 闸门都是裸并发。
 *
 *  **日志一律在解锁之后打**：syslog 会落到串口，在锁里打日志等于让另一个
 *  线程（尤其 BT 框架线程）等串口排空 —— 那正是我们要避免的"拖住协议栈"。
 *  所以每个入口把"要打的事件"记在栈上，出了临界区再 emit。
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

enum sup_evt_code
{
  SUP_EVT_NONE = 0,
  SUP_EVT_CYCLE_WINDOW,    /**< a=window count, b=total, fault=reason。 */
  SUP_EVT_CYCLE_IMMEDIATE, /**< b=total, fault=reason。 */
  SUP_EVT_ADV_STALL,       /**< a=missing ms, b=expected|active<<1。 */
  SUP_EVT_CYCLE_HUNG,      /**< a=hung ms。 */
  SUP_EVT_RETRY_CYCLE,     /**< 失败后重试（无参数）。 */
  SUP_EVT_CYCLE_BEGIN,     /**< a=cycles, fault=reason。 */
  SUP_EVT_CYCLE_OK,        /**< a=cooldown ms。 */
  SUP_EVT_CYCLE_FAIL,      /**< a=failures, b=cooldown ms。 */
  SUP_EVT_CYCLE_FATAL,     /**< a=failures。 */
  SUP_EVT_ESCALATE,        /**< a=failures，请求升到控制器复位那一级。 */
};

struct sup_evt
{
  unsigned                 code;
  enum ble_supervisor_fault fault;
  uint32_t                 a;
  uint32_t                 b;
};

#define SUP_EVT_MAX 3u

/** @brief 解锁之后才调用：这里可以阻塞（串口），锁已放开。 */
static void sup_emit(const struct sup_evt *evts, unsigned n)
{
  unsigned i;

  for (i = 0; i < n; i++)
    {
      const struct sup_evt *e = &evts[i];
      const char *reason = ble_supervisor_fault_name(e->fault);

      switch (e->code)
        {
          case SUP_EVT_CYCLE_WINDOW:
            LOGE("request adapter cycle reason=%s window=%u total=%u",
                 reason, (unsigned)e->a, (unsigned)e->b);
            break;

          case SUP_EVT_CYCLE_IMMEDIATE:
            LOGE("request adapter cycle reason=%s (immediate) total=%u",
                 reason, (unsigned)e->b);
            break;

          case SUP_EVT_ADV_STALL:
            BLE_LOG("adv stall expected=%d active=%d missing=%u",
                    (int)(e->b & 1u), (int)((e->b >> 1) & 1u),
                    (unsigned)e->a);
            break;

          case SUP_EVT_CYCLE_HUNG:
            LOGE("adapter cycle hung %u ms, releasing latch", (unsigned)e->a);
            break;

          case SUP_EVT_RETRY_CYCLE:
            LOGE("retry adapter cycle after recovery failure");
            break;

          case SUP_EVT_CYCLE_BEGIN:
            LOGI("adapter cycle begin count=%u reason=%s",
                 (unsigned)e->a, reason);
            break;

          case SUP_EVT_CYCLE_OK:
            LOGI("adapter cycle recovered, cooldown=%u ms", (unsigned)e->a);
            break;

          case SUP_EVT_CYCLE_FAIL:
            LOGE("adapter cycle failed count=%u, retry in %u ms",
                 (unsigned)e->a, (unsigned)e->b);
            break;

          case SUP_EVT_CYCLE_FATAL:
            LOGE("fatal BLE recovery: repeated adapter cycle failure");
            break;

          case SUP_EVT_ESCALATE:
            LOGE("BLE recovery escalate: %u adapter cycles failed, "
                 "controller reset requested", (unsigned)e->a);
            break;

          default:
            break;
        }
    }
}

/** @brief 取一条空事件（调用方已持有 g_lock）。 */
static struct sup_evt *sup_push(struct sup_evt *evts, unsigned *n)
{
  if (*n >= SUP_EVT_MAX)
    {
      return NULL;
    }

  memset(&evts[*n], 0, sizeof(evts[*n]));
  return &evts[(*n)++];
}

#define SUPERVISOR_FAULT_WINDOW_MS       60000u
#define SUPERVISOR_FAULT_THRESHOLD       2u   /* 3->2：少等一次间隔 */
#define SUPERVISOR_FAULT_SPACING_MS      1000u /* 2s->1s：故障积累从 6 s 压到 2 s */
#define SUPERVISOR_ADV_STALL_MS          6000u  /* 12s->6s：广播没了多久算停滞 */
#define SUPERVISOR_ADV_GRACE_MS          15000u /* 45s->15s：这块是事后才动的大头。敢压短是因为
                                                 * 起播现在按适配器就绪门控（不可用就不发命令、
                                                 * 也不产生故障），原来 45 s 主要就是护那个窗口。 */
#define SUPERVISOR_SUCCESS_COOLDOWN_MS   30000u
#define SUPERVISOR_FAILURE_COOLDOWN_MS   60000u
#define SUPERVISOR_RECOVERING_TIMEOUT_MS 15000u

struct fault_window
{
  uint32_t first_ms;
  uint32_t last_ms;
  uint8_t  count;
};

static struct fault_window g_windows[BLE_SUPERVISOR_FAULT_COUNT];
static uint16_t g_totals[BLE_SUPERVISOR_FAULT_COUNT];
static enum ble_supervisor_fault g_cycle_reason;
static uint32_t g_adv_missing_ms;
static uint32_t g_adv_grace_until_ms;
static uint32_t g_cooldown_until_ms;
static uint16_t g_adapter_cycles;
static uint16_t g_cycle_failures;
static uint32_t g_recovering_since_ms;
static bool g_cycle_pending;
static bool g_recovering;
static bool g_retry_cycle;
static bool g_escalation_req;
static bool g_heavy_req;

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
  return deadline_ms == 0 ||
         (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint16_t sat_inc16(uint16_t value)
{
  return value == UINT16_MAX ? value : (uint16_t)(value + 1u);
}

const char *ble_supervisor_fault_name(enum ble_supervisor_fault fault)
{
  switch (fault)
    {
      case BLE_SUPERVISOR_FAULT_ADV:
        return "adv";
      case BLE_SUPERVISOR_FAULT_SCAN:
        return "scan";
      case BLE_SUPERVISOR_FAULT_GATTC:
        return "gattc";
      case BLE_SUPERVISOR_FAULT_ADAPTER:
        return "adapter";
      default:
        return "unknown";
    }
}

static void supervisor_adv_grace(uint32_t now_ms)
{
  g_adv_grace_until_ms = now_ms + SUPERVISOR_ADV_GRACE_MS;
  g_adv_missing_ms = 0;
}

void ble_supervisor_init(uint32_t now_ms)
{
  pthread_mutex_lock(&g_lock);
  memset(g_windows, 0, sizeof(g_windows));
  memset(g_totals, 0, sizeof(g_totals));
  g_cycle_reason = BLE_SUPERVISOR_FAULT_ADAPTER;
  g_adv_missing_ms = 0;
  g_cooldown_until_ms = 0;
  g_adapter_cycles = 0;
  g_cycle_failures = 0;
  g_cycle_pending = false;
  g_recovering = false;
  g_recovering_since_ms = 0;
  g_retry_cycle = false;
  g_escalation_req = false;
  supervisor_adv_grace(now_ms);
  pthread_mutex_unlock(&g_lock);
}

/**
 * @brief 记一次故障（调用方持有 g_lock；要打的事件写进 evts）。
 *
 * 单独抽出来是因为 `ble_supervisor_watch_advertising()` 也要用它 ——
 * 公开函数之间互相调用会在非递归锁上自锁。
 */
static void sup_report_locked(enum ble_supervisor_fault fault, uint32_t now_ms,
                              struct sup_evt *evts, unsigned *n)
{
  struct fault_window *window;

  /* Boot / dual-role (phone ADV + HR central) start failures are normal. */
  if (fault == BLE_SUPERVISOR_FAULT_ADV &&
      !deadline_reached(now_ms, g_adv_grace_until_ms))
    {
      return;
    }

  window = &g_windows[fault];
  if (window->count == 0 ||
      (uint32_t)(now_ms - window->first_ms) >
      SUPERVISOR_FAULT_WINDOW_MS)
    {
      window->first_ms = now_ms;
      window->last_ms = 0;
      window->count = 0;
    }

  if (window->last_ms != 0 &&
      (uint32_t)(now_ms - window->last_ms) < SUPERVISOR_FAULT_SPACING_MS)
    {
      return;
    }

  window->last_ms = now_ms;
  if (window->count < UINT8_MAX)
    {
      window->count++;
    }

  g_totals[fault] = sat_inc16(g_totals[fault]);

  if (window->count >= SUPERVISOR_FAULT_THRESHOLD &&
      !g_recovering && !g_cycle_pending &&
      deadline_reached(now_ms, g_cooldown_until_ms))
    {
      struct sup_evt *e;

      g_cycle_reason = fault;
      g_cycle_pending = true;

      e = sup_push(evts, n);
      if (e != NULL)
        {
          e->code  = SUP_EVT_CYCLE_WINDOW;
          e->fault = fault;
          e->a     = window->count;
          e->b     = g_totals[fault];
        }
    }
}

void ble_supervisor_report(enum ble_supervisor_fault fault, uint32_t now_ms)
{
  struct sup_evt evts[SUP_EVT_MAX];
  unsigned n = 0;

  if (fault >= BLE_SUPERVISOR_FAULT_COUNT)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  sup_report_locked(fault, now_ms, evts, &n);
  pthread_mutex_unlock(&g_lock);

  sup_emit(evts, n);
}

void ble_supervisor_watch_advertising(bool expected, bool active,
                                      uint32_t now_ms)
{
  struct sup_evt evts[SUP_EVT_MAX];
  unsigned n = 0;

  pthread_mutex_lock(&g_lock);

  if (!expected || active || g_recovering ||
      !deadline_reached(now_ms, g_adv_grace_until_ms))
    {
      g_adv_missing_ms = 0;
    }
  else if (g_adv_missing_ms == 0)
    {
      g_adv_missing_ms = now_ms;
    }
  else if ((uint32_t)(now_ms - g_adv_missing_ms) >=
           SUPERVISOR_ADV_STALL_MS)
    {
      struct sup_evt *e;

      e = sup_push(evts, &n);
      if (e != NULL)
        {
          e->code = SUP_EVT_ADV_STALL;
          e->a    = now_ms - g_adv_missing_ms;
          e->b    = (expected ? 1u : 0u) | (active ? 2u : 0u);
        }

      g_adv_missing_ms = now_ms;
      sup_report_locked(BLE_SUPERVISOR_FAULT_ADV, now_ms, evts, &n);
    }

  pthread_mutex_unlock(&g_lock);

  sup_emit(evts, n);
}

void ble_supervisor_tick(uint32_t now_ms)
{
  struct sup_evt evts[SUP_EVT_MAX];
  unsigned n = 0;

  pthread_mutex_lock(&g_lock);

  if (g_recovering && g_recovering_since_ms != 0 &&
      (uint32_t)(now_ms - g_recovering_since_ms) >=
      SUPERVISOR_RECOVERING_TIMEOUT_MS)
    {
      struct sup_evt *e = sup_push(evts, &n);

      if (e != NULL)
        {
          e->code = SUP_EVT_CYCLE_HUNG;
          e->a    = now_ms - g_recovering_since_ms;
        }

      g_recovering = false;
      g_recovering_since_ms = 0;
      g_retry_cycle = true;
      g_cooldown_until_ms = 0;
    }

  if (g_retry_cycle && !g_recovering && !g_cycle_pending &&
      deadline_reached(now_ms, g_cooldown_until_ms))
    {
      struct sup_evt *e = sup_push(evts, &n);

      if (e != NULL)
        {
          e->code = SUP_EVT_RETRY_CYCLE;
        }

      g_cycle_reason = BLE_SUPERVISOR_FAULT_ADAPTER;
      g_cycle_pending = true;
      g_retry_cycle = false;
    }

  pthread_mutex_unlock(&g_lock);

  sup_emit(evts, n);
}

void ble_supervisor_request_cycle(enum ble_supervisor_fault fault,
                                  uint32_t now_ms)
{
  struct sup_evt evts[SUP_EVT_MAX];
  unsigned n = 0;

  if (fault >= BLE_SUPERVISOR_FAULT_COUNT)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);

  /* HCI RX 卡住时 cooldown 只会让 -ENOMEM 再拖几十秒。 */
  if (!g_recovering &&
      (fault == BLE_SUPERVISOR_FAULT_ADAPTER ||
       deadline_reached(now_ms, g_cooldown_until_ms)))
    {
      if (g_cycle_pending)
        {
          if (fault == BLE_SUPERVISOR_FAULT_ADAPTER)
            {
              g_cycle_reason = fault;
            }
        }
      else
        {
          struct sup_evt *e;

          g_cycle_reason = fault;
          g_cycle_pending = true;
          g_totals[fault] = sat_inc16(g_totals[fault]);

          e = sup_push(evts, &n);
          if (e != NULL)
            {
              e->code  = SUP_EVT_CYCLE_IMMEDIATE;
              e->fault = fault;
              e->b     = g_totals[fault];
            }
        }
    }

  pthread_mutex_unlock(&g_lock);

  sup_emit(evts, n);
}

bool ble_supervisor_take_escalation(void)
{
  bool req;

  pthread_mutex_lock(&g_lock);
  req = g_escalation_req;
  g_escalation_req = false;
  pthread_mutex_unlock(&g_lock);
  return req;
}

void ble_supervisor_escalation_done(uint32_t now_ms)
{
  pthread_mutex_lock(&g_lock);
  g_cycle_failures = 0;
  g_retry_cycle = false;
  g_escalation_req = false;
  g_cooldown_until_ms = now_ms + SUPERVISOR_SUCCESS_COOLDOWN_MS;
  pthread_mutex_unlock(&g_lock);
}

bool ble_supervisor_cycle_requested(void)
{
  bool pending;

  pthread_mutex_lock(&g_lock);
  pending = g_cycle_pending && !g_recovering;
  pthread_mutex_unlock(&g_lock);
  return pending;
}

enum ble_supervisor_fault ble_supervisor_pending_reason(void)
{
  enum ble_supervisor_fault reason;

  pthread_mutex_lock(&g_lock);
  reason = g_cycle_reason;
  pthread_mutex_unlock(&g_lock);
  return reason;
}

bool ble_supervisor_take_adapter_cycle(enum ble_supervisor_fault *reason)
{
  bool taken = false;

  pthread_mutex_lock(&g_lock);

  if (g_cycle_pending && !g_recovering)
    {
      if (reason != NULL)
        {
          *reason = g_cycle_reason;
        }

      g_cycle_pending = false;
      taken = true;
    }

  pthread_mutex_unlock(&g_lock);
  return taken;
}

void ble_supervisor_cycle_begin(uint32_t now_ms)
{
  struct sup_evt evts[SUP_EVT_MAX];
  unsigned n = 0;
  struct sup_evt *e;

  pthread_mutex_lock(&g_lock);

  g_recovering = true;
  g_recovering_since_ms = (now_ms == 0) ? 1u : now_ms;
  g_cycle_pending = false;
  g_adapter_cycles = sat_inc16(g_adapter_cycles);

  e = sup_push(evts, &n);
  if (e != NULL)
    {
      e->code  = SUP_EVT_CYCLE_BEGIN;
      e->fault = g_cycle_reason;
      e->a     = g_adapter_cycles;
    }

  pthread_mutex_unlock(&g_lock);

  sup_emit(evts, n);
}

void ble_supervisor_cycle_complete(bool success, uint32_t now_ms)
{
  struct sup_evt evts[SUP_EVT_MAX];
  unsigned n = 0;
  struct sup_evt *e;
  bool fatal = false;
  uint16_t failures = 0;

  pthread_mutex_lock(&g_lock);

  g_recovering = false;
  g_recovering_since_ms = 0;
  supervisor_adv_grace(now_ms);

  if (success)
    {
      memset(g_windows, 0, sizeof(g_windows));
      g_cycle_failures = 0;
      g_retry_cycle = false;
      g_cooldown_until_ms = now_ms + SUPERVISOR_SUCCESS_COOLDOWN_MS;

      e = sup_push(evts, &n);
      if (e != NULL)
        {
          e->code = SUP_EVT_CYCLE_OK;
          e->a    = SUPERVISOR_SUCCESS_COOLDOWN_MS;
        }
    }
  else
    {
      g_cycle_failures = sat_inc16(g_cycle_failures);
      g_retry_cycle = true;
      g_cooldown_until_ms = now_ms + SUPERVISOR_FAILURE_COOLDOWN_MS;
      failures = g_cycle_failures;
      /* 1->2 次：enable 都回不来就别再等 60 s 冷却试第二遍，直接复位控制器。
       * 复位本身有 5 分钟限频兜着，不会变成新循环。 */
      fatal = (g_cycle_failures >= 1);

      /* **第一次** cycle 失败就换级**是故意的**（`fatal = failures >= 1`），
       * 理由见上面 1->2 那条：cycle 这一档已经证明 enable 回不来，再等 60 s
       * 冷却试第二遍只是空转（现场 23 次）。复位本身有 5 分钟限频兜着，
       * 不会变成新循环。
       *
       * 2026-09-23 更正这里的注释：原来写的是"连续两次救不回来 ⇒ 换级"，
       * 与代码（`>= 1`）和上面那条注释都矛盾，审计时按"代码与注释不一致"
       * 记了一笔。核对后确认**行为是对的、错的是这段注释** —— 保持 `>= 1`
       * 不动，只把说法改对，免得下一个人照着它去改成 `>= 2`。
       *
       * 置请求，让 companion 线程去复位控制器；同时把普通重试停掉，
       * 避免复位和 cycle 抢同一根绳。 */
      if (fatal)
        {
          struct sup_evt *e2;

          g_escalation_req = true;
          g_retry_cycle = false;

          e2 = sup_push(evts, &n);
          if (e2 != NULL)
            {
              e2->code = SUP_EVT_ESCALATE;
              e2->a    = failures;
            }
        }

      e = sup_push(evts, &n);
      if (e != NULL)
        {
          e->code = SUP_EVT_CYCLE_FAIL;
          e->a    = failures;
          e->b    = SUPERVISOR_FAILURE_COOLDOWN_MS;
        }

      if (fatal)
        {
          e = sup_push(evts, &n);
          if (e != NULL)
            {
              e->code = SUP_EVT_CYCLE_FATAL;
              e->a    = failures;
            }
        }
    }

  pthread_mutex_unlock(&g_lock);

  sup_emit(evts, n);
}

void ble_supervisor_get_stats(struct ble_supervisor_stats *stats)
{
  if (stats == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  memset(stats, 0, sizeof(*stats));
  stats->adv_failures = g_totals[BLE_SUPERVISOR_FAULT_ADV];
  stats->scan_failures = g_totals[BLE_SUPERVISOR_FAULT_SCAN];
  stats->gattc_recreates = g_totals[BLE_SUPERVISOR_FAULT_GATTC];
  stats->adapter_failures = g_totals[BLE_SUPERVISOR_FAULT_ADAPTER];
  stats->adapter_cycles = g_adapter_cycles;
  stats->adapter_cycle_failures = g_cycle_failures;
  stats->recovering = g_recovering;
  stats->cycle_pending = g_cycle_pending;
  pthread_mutex_unlock(&g_lock);
}
