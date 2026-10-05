/**
 * @file companion_bridge.c
 * @brief ble_companion 的互斥 GNSS / 传感器 / NSH 测试状态。
 *
 * 传感器采样 8 s 过期（与 GATT notify 过期重连相同）。
 * NSH `test sensor` 投递命令，由 companion 线程取出。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "companion_bridge.h"
#include "board_malloc.h"
#include "myvendor_ble_log.h"
#include "myvendor_mono.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SENSOR_STALE_MS  8000u
#define GNSS_STALE_MS    (COMPANION_GNSS_STALE_SEC * 1000u)
#define PHONE_THREAD_MS  8000u

/**
 * @brief CLOCK_MONOTONIC 毫秒。
 */
static uint32_t bridge_now_ms(void)
{
  return myvendor_mono_ms();
}

struct companion_bridge_s
{
  pthread_mutex_t lock;
  bool inited;

  struct companion_gnss_fix gnss;
  bool     gnss_fresh;
  uint32_t gnss_ms;

  uint16_t hr_bpm;
  uint16_t cadence_rpm;
  uint16_t power_w;
  uint32_t hr_ms;
  uint32_t cad_ms;
  uint32_t pwr_ms;

  int32_t  nav_lat[COMPANION_NAV_MAX_PTS];
  int32_t  nav_lon[COMPANION_NAV_MAX_PTS];
  uint8_t  nav_count;
  bool     nav_pending;

  bool     alive;
  uint32_t heartbeat_ms;
  bool     diag_cycle;
  bool     diag_restart;
  bool     teardown_skip_hci;
  bool     phone_connected;
  bool     ride_session;
  bool     ride_moving;
  uint8_t  sensor_cmd;
  bool     sensor_cmd_pend;
  uint8_t  sensor_mode;
  uint32_t sensor_scan_ms;
  uint32_t pair_secs;        /**< 配对窗口秒数（PAIR_OPEN 用）。 */
  uint8_t  pair_unbind_slot; /**< PAIR_UNBIND：0xff=全部，否则 0..2 槽。 */
  char     pair_report[320]; /**< 配对/连接快照（companion 填、ctl 读）。 */
  uint32_t pair_report_gen;
  uint8_t  sensor_connect_idx;
  uint8_t  sensor_connect_kind; /**< CONNECT 的类型；AUTO 表示不指定。 */
  uint8_t  sensor_kind;
  struct companion_sensor_bind sensor_bind;
  struct companion_sensor_ui sensor_ui;
  bool     notif_print;
  bool     ctrl_print;

  bool     policy_radio;
  bool     policy_sensor;
  bool     policy_notif;
  bool     policy_calls;
  bool     policy_radio_dirty;
  bool     policy_sensor_dirty;

  struct companion_notif_item *inbox;
  uint8_t  inbox_n;
  struct companion_notif_item *pending;
  uint8_t  pend_head;
  uint8_t  pend_n;
};

static struct companion_bridge_s g_bridge;
static volatile const char *g_bridge_phase = "-";

/**
 * @brief 一次性互斥量初始化，以及默认扫描窗口。
 */
static void bridge_init_once(void)
{
  pthread_mutex_init(&g_bridge.lock, NULL);
  g_bridge.sensor_scan_ms = COMPANION_TEST_SENSOR_SCAN_MS_DEFAULT;
  g_bridge.pair_secs = COMPANION_TEST_PAIR_OPEN_SECS_DEFAULT;
  g_bridge.pair_unbind_slot = 0xffu;
  g_bridge.sensor_connect_kind = COMPANION_SENSOR_KIND_AUTO;
  g_bridge.policy_radio = true;
  g_bridge.policy_notif = true;
  g_bridge.inbox = board_malloc_psram(
      sizeof(struct companion_notif_item) * COMPANION_NOTIF_INBOX_MAX);
  g_bridge.pending = board_malloc_psram(
      sizeof(struct companion_notif_item) * COMPANION_NOTIF_QUEUE_MAX);
  if (g_bridge.inbox != NULL)
    {
      memset(g_bridge.inbox, 0,
             sizeof(struct companion_notif_item) * COMPANION_NOTIF_INBOX_MAX);
    }

  if (g_bridge.pending != NULL)
    {
      memset(g_bridge.pending, 0,
             sizeof(struct companion_notif_item) * COMPANION_NOTIF_QUEUE_MAX);
    }

  g_bridge.inited = true;
}

/**
 * @brief 惰性初始化 bridge（只跑一次）。
 *
 * @details
 * 原来用 `if (g_bridge.inited) return;` 的裸检查：两个线程同时首次调用会
 * 双双通过检查，于是 pthread_mutex_init 被调两次、两次 board_malloc_psram
 * 里有一块被后写覆盖 —— 直接泄漏一块 PSRAM。用 pthread_once 让内核保证
 * 只执行一次，并且后来的调用者会等第一次跑完。
 */
static void bridge_init(void)
{
  static pthread_once_t once = PTHREAD_ONCE_INIT;

  (void)pthread_once(&once, bridge_init_once);
}

/**
 * @brief 保存一次手机 GNSS 定位（0xFF14）。
 */
void companion_bridge_gnss_set(const struct companion_gnss_fix *fix,
                               uint32_t now_ms)
{
  if (fix == NULL)
    {
      return;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.gnss       = *fix;
  g_bridge.gnss_fresh  = true;
  g_bridge.gnss_ms    = now_ms;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 若未过期则拷贝最近一次手机 GNSS。
 */
bool companion_bridge_gnss_get(struct companion_gnss_fix *out)
{
  return companion_bridge_gnss_get_within(out, GNSS_STALE_MS);
}

/**
 * @brief 同 [companion_bridge_gnss_get]，但由调用方给新鲜度窗口。
 *
 * 辅助数据（灌给模组的粗位置）用得上这个：它要的是「一分钟前那份也还行」，
 * 而 [companion_bridge_gnss_get] 的 5 秒窗口是给「显示当前定位」定的 ——
 * 两者需求不同，不要改前者的语义，另开一个入口。
 */
bool companion_bridge_gnss_get_within(struct companion_gnss_fix *out,
                                      uint32_t max_age_ms)
{
  bool ok = false;
  uint32_t now;

  if (out == NULL)
    {
      return false;
    }

  now = bridge_now_ms();
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  if (g_bridge.gnss_fresh &&
      g_bridge.gnss_ms != 0 &&
      myvendor_mono_elapsed_ms(now, g_bridge.gnss_ms) < max_age_ms)
    {
      *out = g_bridge.gnss;
      ok = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
  return ok;
}

/**
 * @brief 最近一次手机 GNSS 的接收时刻（CLOCK_MONOTONIC 毫秒）；0 表示无。
 *
 * @details
 * 手机 GNSS 与板载一样要能算年龄：消费方把它填进
 * myvendor_sys_gnss_t::stamp_ms，就不用为"这份数据是刚到的还是旧的"另开
 * 一套判断。本函数只读时间戳，不改 companion_bridge_gnss_get 的过期语义。
 */
uint32_t companion_bridge_gnss_stamp_ms(void)
{
  uint32_t ms;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  ms = g_bridge.gnss_fresh ? g_bridge.gnss_ms : 0u;
  pthread_mutex_unlock(&g_bridge.lock);
  return ms;
}

/**
 * @brief 丢掉已存的手机 GNSS（链路断开）。
 */
void companion_bridge_gnss_clear(void)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.gnss_fresh = false;
  g_bridge.gnss_ms    = 0;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 发布 GATT 客户端的心率采样。
 */
void companion_bridge_sensor_set_hr(uint16_t bpm, uint32_t now_ms)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.hr_bpm = bpm;
  g_bridge.hr_ms  = now_ms;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 发布 CSC 踏频采样。
 */
void companion_bridge_sensor_set_cadence(uint16_t rpm, uint32_t now_ms)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.cadence_rpm = rpm;
  g_bridge.cad_ms      = now_ms;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 发布骑行功率采样。
 */
void companion_bridge_sensor_set_power(uint16_t watts, uint32_t now_ms)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.power_w = watts;
  g_bridge.pwr_ms  = now_ms;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 快照传感器遥测；超过 8 s 的字段无效。
 * @param try_only true = 锁被占就立刻返回 false，绝不阻塞（UI 线程用）。
 */
static bool companion_bridge_sensor_get_impl(struct companion_sensor_telem *out,
                                             bool try_only)
{
  uint32_t now;

  if (out == NULL)
    {
      return false;
    }

  memset(out, 0, sizeof(*out));
  now = bridge_now_ms();
  bridge_init();

  if (try_only)
    {
      /* **UI 线程专用**：BLE 侧（companion/FS）持锁时不能把 LVGL 一起拖住 ——
       * 那会让静止覆盖层的心跳、按键、拿起唤醒全部停摆（"静止界面卡住"）。
       * 抢不到就这一拍跳过，下一拍再来。 */
      if (pthread_mutex_trylock(&g_bridge.lock) != 0)
        {
          return false;
        }
    }
  else
    {
      pthread_mutex_lock(&g_bridge.lock);
    }

  if (g_bridge.hr_ms != 0 &&
      myvendor_mono_elapsed_ms(now, g_bridge.hr_ms) < SENSOR_STALE_MS)
    {
      out->hr_valid = true;
      out->hr_bpm   = g_bridge.hr_bpm;
    }

  if (g_bridge.cad_ms != 0 &&
      myvendor_mono_elapsed_ms(now, g_bridge.cad_ms) < SENSOR_STALE_MS)
    {
      out->cadence_valid = true;
      out->cadence_rpm   = g_bridge.cadence_rpm;
    }

  if (g_bridge.pwr_ms != 0 &&
      myvendor_mono_elapsed_ms(now, g_bridge.pwr_ms) < SENSOR_STALE_MS)
    {
      out->power_valid = true;
      out->power_w     = g_bridge.power_w;
    }

  pthread_mutex_unlock(&g_bridge.lock);
  return true;
}

void companion_bridge_sensor_get(struct companion_sensor_telem *out)
{
  (void)companion_bridge_sensor_get_impl(out, false);
}

bool companion_bridge_sensor_get_try(struct companion_sensor_telem *out)
{
  return companion_bridge_sensor_get_impl(out, true);
}

/**
 * @brief 保存手机导航折线（0xFF19）。
 */
void companion_bridge_nav_store(const int32_t *lat_e7, const int32_t *lon_e7,
                                uint8_t count)
{
  if (lat_e7 == NULL || lon_e7 == NULL || count == 0)
    {
      return;
    }

  if (count > COMPANION_NAV_MAX_PTS)
    {
      count = COMPANION_NAV_MAX_PTS;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  memcpy(g_bridge.nav_lat, lat_e7, count * sizeof(int32_t));
  memcpy(g_bridge.nav_lon, lon_e7, count * sizeof(int32_t));
  g_bridge.nav_count   = count;
  g_bridge.nav_pending = true;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 取出挂起的导航折线（一次性）。
 */
bool companion_bridge_nav_take(int32_t *lat_e7, int32_t *lon_e7,
                               uint8_t *count)
{
  bool ok = false;

  if (lat_e7 == NULL || lon_e7 == NULL || count == NULL)
    {
      return false;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  if (g_bridge.nav_pending && g_bridge.nav_count > 0)
    {
      memcpy(lat_e7, g_bridge.nav_lat, g_bridge.nav_count * sizeof(int32_t));
      memcpy(lon_e7, g_bridge.nav_lon, g_bridge.nav_count * sizeof(int32_t));
      *count = g_bridge.nav_count;
      g_bridge.nav_pending = false;
      ok = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
  return ok;
}

/**
 * @brief 标记 ble_companion 主循环正在跑。
 */
void companion_bridge_alive_set(bool alive)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.alive = alive;
  g_bridge.heartbeat_ms = alive ? bridge_now_ms() : 0;
  pthread_mutex_unlock(&g_bridge.lock);
  BLE_LOG("companion alive=%d", alive ? 1 : 0);
}

void companion_bridge_heartbeat(void)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.heartbeat_ms = bridge_now_ms();
  pthread_mutex_unlock(&g_bridge.lock);
}

bool companion_bridge_thread_ok(uint32_t stall_ms)
{
  uint32_t hb;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  hb = g_bridge.heartbeat_ms;
  pthread_mutex_unlock(&g_bridge.lock);

  if (hb == 0)
    {
      return false;
    }

  if (stall_ms == 0)
    {
      stall_ms = 8000u;
    }

  return myvendor_mono_elapsed_ms(bridge_now_ms(), hb) < stall_ms;
}

uint32_t companion_bridge_heartbeat_age_ms(void)
{
  uint32_t hb;
  uint32_t now;

  bridge_init();
  now = bridge_now_ms();
  pthread_mutex_lock(&g_bridge.lock);
  hb = g_bridge.heartbeat_ms;
  pthread_mutex_unlock(&g_bridge.lock);
  if (hb == 0)
    {
      return UINT32_MAX;
    }

  return myvendor_mono_elapsed_ms(now, hb);
}

void companion_bridge_phase_set(const char *phase)
{
  g_bridge_phase = (phase != NULL && phase[0] != '\0') ? phase : "-";
}

const char *companion_bridge_phase_get(void)
{
  const char *phase = (const char *)g_bridge_phase;

  return (phase != NULL && phase[0] != '\0') ? phase : "-";
}

/**
 * @brief NSH 测试是否可以投递命令。
 */
bool companion_bridge_alive_get(void)
{
  bool alive;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  alive = g_bridge.alive;
  pthread_mutex_unlock(&g_bridge.lock);
  return alive;
}

void companion_bridge_diag_cycle_post(void)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.diag_cycle = true;
  pthread_mutex_unlock(&g_bridge.lock);
  BLE_LOG("diag_cycle posted hb=%u",
          (unsigned)companion_bridge_heartbeat_age_ms());
}

void companion_bridge_diag_restart_post(void)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.diag_restart = true;
  pthread_mutex_unlock(&g_bridge.lock);
  BLE_LOG("task restart requested by companion");
}

bool companion_bridge_diag_restart_take(void)
{
  bool pending;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  pending = g_bridge.diag_restart;
  g_bridge.diag_restart = false;
  pthread_mutex_unlock(&g_bridge.lock);
  return pending;
}

bool companion_bridge_diag_cycle_take(void)
{
  bool pending;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  pending = g_bridge.diag_cycle;
  g_bridge.diag_cycle = false;
  pthread_mutex_unlock(&g_bridge.lock);
  return pending;
}

bool companion_bridge_diag_cycle_pending(void)
{
  bool pending;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  pending = g_bridge.diag_cycle;
  pthread_mutex_unlock(&g_bridge.lock);
  return pending;
}

void companion_bridge_teardown_skip_hci_set(bool on)
{
  bool prev;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  prev = g_bridge.teardown_skip_hci;
  g_bridge.teardown_skip_hci = on;
  pthread_mutex_unlock(&g_bridge.lock);
  if (prev != on)
    {
      BLE_LOG("teardown_skip_hci %d -> %d", prev ? 1 : 0, on ? 1 : 0);
    }
}

bool companion_bridge_teardown_skip_hci(void)
{
  bool on;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.teardown_skip_hci;
  pthread_mutex_unlock(&g_bridge.lock);
  return on;
}

/**
 * @brief 标记手机 Companion GATT 已连接。
 */
void companion_bridge_phone_set(bool on)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.phone_connected = on;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 手机 Companion GATT 是否已连接。
 *
 * companion 心跳停了则视为未连接：否则 UI 会一直显示「已连接」，
 * 而主机 GATT / HCI 已经死掉。
 */
bool companion_bridge_phone_get(void)
{
  bool on;
  uint32_t hb;
  uint32_t now;

  bridge_init();
  now = bridge_now_ms();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.phone_connected;
  hb = g_bridge.heartbeat_ms;
  pthread_mutex_unlock(&g_bridge.lock);

  if (!on)
    {
      return false;
    }

  if (hb == 0)
    {
      return false;
    }

  return myvendor_mono_elapsed_ms(now, hb) < PHONE_THREAD_MS;
}

void companion_bridge_ride_set(bool session, bool moving)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.ride_session = session;
  g_bridge.ride_moving = session && moving;
  pthread_mutex_unlock(&g_bridge.lock);
}

void companion_bridge_ride_get(bool *session, bool *moving)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  if (session)
    {
      *session = g_bridge.ride_session;
    }

  if (moving)
    {
      *moving = g_bridge.ride_moving;
    }

  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 投递一条传感器命令（ctl 产品路径；DUMP 不改 sensor_mode）。
 */
void companion_bridge_sensor_cmd_post(uint8_t cmd)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  /* 单槽命令：SCAN_STOP 盖掉 CONNECT 后只会停扫描、永远不建链。 */
  if (g_bridge.sensor_cmd_pend &&
      cmd == COMPANION_TEST_SENSOR_SCAN_STOP &&
      (g_bridge.sensor_cmd == COMPANION_TEST_SENSOR_CONNECT ||
       g_bridge.sensor_cmd == COMPANION_TEST_SENSOR_CONNECT_ADDR))
    {
      pthread_mutex_unlock(&g_bridge.lock);
      return;
    }

  g_bridge.sensor_cmd      = cmd;
  g_bridge.sensor_cmd_pend = true;
  if (cmd != COMPANION_TEST_SENSOR_DUMP && cmd != COMPANION_TEST_SENSOR_NONE)
    {
      g_bridge.sensor_mode = cmd;
    }

  pthread_mutex_unlock(&g_bridge.lock);
}

void companion_bridge_sensor_cmd_set_scan_ms(uint32_t ms)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.sensor_scan_ms = (ms == 0) ? COMPANION_TEST_SENSOR_SCAN_MS_DEFAULT : ms;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 设置"下一次开配对窗口"的秒数（`COMPANION_TEST_PAIR_OPEN` 命令的参数）。
 *
 * @param secs 秒数；0 = 用默认 `COMPANION_TEST_PAIR_OPEN_SECS_DEFAULT`。
 */
void companion_bridge_sensor_cmd_set_pair_secs(uint32_t secs)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.pair_secs = (secs == 0u) ? COMPANION_TEST_PAIR_OPEN_SECS_DEFAULT : secs;
  pthread_mutex_unlock(&g_bridge.lock);
}

void companion_bridge_sensor_cmd_set_pair_unbind_slot(uint8_t slot)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.pair_unbind_slot = slot;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 存一份 `ctl pair list` 的文本快照（由 **companion 线程**填）。
 *
 * 为什么绕这一圈：连接链表只能在协议栈 / companion 线程里遍历，ctl 线程直接遍历会
 * panic（见 `myvendor_bt_pair.c` 里 `myvendor_bt_pair_cmd()` 的注释）。
 *
 * @param text 快照文本（可为 NULL = 清空）。
 */
void companion_bridge_pair_report_set(const char *text)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  snprintf(g_bridge.pair_report, sizeof(g_bridge.pair_report), "%s",
           text != NULL ? text : "");
  g_bridge.pair_report_gen++;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 取 `ctl pair list` 快照，并用 generation 判断"是不是新的"。
 *
 * @param out 输出缓冲（可为 NULL = 只做新鲜度判断）。
 * @param len 输出缓冲长度。
 * @param gen 传入上次取到的 generation（可为 NULL = 跳过判断）；
 *            **内容有更新时就地刷新**成新值。
 * @return 1 = 相对传入的 gen 是新的（或调用方没传 gen）；0 = 没变化。
 */
int companion_bridge_pair_report_get(char *out, size_t len, uint32_t *gen)
{
  int fresh = 0;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  if (gen != NULL && *gen != g_bridge.pair_report_gen) {
    fresh = 1;
    *gen = g_bridge.pair_report_gen;
  }
  if (out != NULL && len > 0u) {
    snprintf(out, len, "%s", g_bridge.pair_report);
  }
  pthread_mutex_unlock(&g_bridge.lock);
  return fresh;
}

/**
 * @brief 读回"下次开窗秒数"（`ctl pair open [秒]` / BLE 控制帧共用）。
 *
 * @return 秒数；setter 已把 0 归一成默认值，所以恒 > 0。
 */
uint32_t companion_bridge_test_pair_secs(void)
{
  uint32_t v;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  v = g_bridge.pair_secs;
  pthread_mutex_unlock(&g_bridge.lock);
  return v;
}

uint8_t companion_bridge_test_pair_unbind_slot(void)
{
  uint8_t v;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  v = g_bridge.pair_unbind_slot;
  pthread_mutex_unlock(&g_bridge.lock);
  return v;
}

void companion_bridge_sensor_cmd_set_connect_idx(uint8_t idx)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.sensor_connect_idx = idx;
  pthread_mutex_unlock(&g_bridge.lock);
}

void companion_bridge_sensor_cmd_set_kind(uint8_t kind)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.sensor_kind = kind;
  pthread_mutex_unlock(&g_bridge.lock);
}

void companion_bridge_sensor_cmd_set_connect_kind(uint8_t kind)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.sensor_connect_kind = kind;
  pthread_mutex_unlock(&g_bridge.lock);
}

uint8_t companion_bridge_sensor_cmd_take_connect_kind(void)
{
  uint8_t kind;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  kind = g_bridge.sensor_connect_kind;
  g_bridge.sensor_connect_kind = COMPANION_SENSOR_KIND_AUTO;
  pthread_mutex_unlock(&g_bridge.lock);

  return kind;
}

void companion_bridge_sensor_cmd_set_bind(const struct companion_sensor_bind *bind)
{
  if (bind == NULL)
    {
      return;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.sensor_bind = *bind;
  g_bridge.sensor_kind = bind->kind;
  pthread_mutex_unlock(&g_bridge.lock);
}

void companion_bridge_sensor_cmd_get_bind(struct companion_sensor_bind *out)
{
  if (out == NULL)
    {
      return;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  *out = g_bridge.sensor_bind;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief NSH `test sensor` 探针：与产品命令同一队列。
 */
void companion_bridge_test_sensor_post(uint8_t cmd)
{
  companion_bridge_sensor_cmd_post(cmd);
}

/**
 * @brief 设置下一次 SCAN 命令的观察时长。
 */
void companion_bridge_test_sensor_set_scan_ms(uint32_t ms)
{
  companion_bridge_sensor_cmd_set_scan_ms(ms);
}

/**
 * @brief NSH 最近一次投递的观察时长。
 */
uint32_t companion_bridge_test_sensor_scan_ms(void)
{
  uint32_t ms;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  ms = g_bridge.sensor_scan_ms;
  pthread_mutex_unlock(&g_bridge.lock);
  return ms;
}

/**
 * @brief CONNECT 用的扫描表 1-based 下标（按地址）。
 */
void companion_bridge_test_sensor_set_connect_idx(uint8_t idx)
{
  companion_bridge_sensor_cmd_set_connect_idx(idx);
}

/**
 * @brief `test sensor connect` 最近投递的下标。
 */
uint8_t companion_bridge_test_sensor_connect_idx(void)
{
  uint8_t idx;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  idx = g_bridge.sensor_connect_idx;
  pthread_mutex_unlock(&g_bridge.lock);
  return idx;
}

/**
 * @brief DISCONNECT_KIND 用的槽位。
 */
void companion_bridge_test_sensor_set_kind(uint8_t kind)
{
  companion_bridge_sensor_cmd_set_kind(kind);
}

/**
 * @brief 最近一次 DISCONNECT_KIND 的槽位。
 */
uint8_t companion_bridge_test_sensor_kind(void)
{
  uint8_t kind;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  kind = g_bridge.sensor_kind;
  pthread_mutex_unlock(&g_bridge.lock);
  return kind;
}

/**
 * @brief 发布扫描表与槽位状态。
 */
void companion_bridge_sensor_ui_set(const struct companion_sensor_ui *ui)
{
  if (ui == NULL)
    {
      return;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.sensor_ui = *ui;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 拷贝最新扫描/连接快照。
 */
void companion_bridge_sensor_ui_get(struct companion_sensor_ui *out)
{
  if (out == NULL)
    {
      return;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  *out = g_bridge.sensor_ui;
  pthread_mutex_unlock(&g_bridge.lock);
}

bool companion_bridge_sensor_slot_bound(uint8_t kind)
{
  bool bound = false;

  if (kind >= COMPANION_SENSOR_KIND_N)
    {
      return false;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  {
    const uint8_t *a = g_bridge.sensor_ui.slot[kind].addr;
    uint8_t i;

    for (i = 0; i < COMPANION_SENSOR_ADDR_LEN; i++)
      {
        if (a[i] != 0u)
          {
            bound = true;
            break;
          }
      }
  }

  pthread_mutex_unlock(&g_bridge.lock);
  return bound;
}

/**
 * @brief 取出一条挂起的 NSH 传感器命令（companion 线程）。
 */
bool companion_bridge_test_sensor_take(uint8_t *cmd)
{
  bool ok = false;

  if (cmd == NULL)
    {
      return false;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  if (g_bridge.sensor_cmd_pend)
    {
      *cmd = g_bridge.sensor_cmd;
      g_bridge.sensor_cmd_pend = false;
      g_bridge.sensor_cmd      = COMPANION_TEST_SENSOR_NONE;
      ok = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
  return ok;
}

/**
 * @brief 最近一次传感器模式（DUMP 不改这个值）。
 */
uint8_t companion_bridge_test_sensor_mode(void)
{
  uint8_t mode;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  mode = g_bridge.sensor_mode;
  pthread_mutex_unlock(&g_bridge.lock);
  return mode;
}

/**
 * @brief 是否在 NSH 打印每条 0xFF17 通知。
 */
void companion_bridge_test_notif_set(bool on)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.notif_print = on;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 0xFF17 打印是否开启。
 */
bool companion_bridge_test_notif_get(void)
{
  bool on;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.notif_print;
  pthread_mutex_unlock(&g_bridge.lock);
  return on;
}

/**
 * @brief 是否在 NSH 打印 0xFF13 / 0xFF19。
 */
void companion_bridge_test_ctrl_set(bool on)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.ctrl_print = on;
  pthread_mutex_unlock(&g_bridge.lock);
}

/**
 * @brief 控制/导航打印是否开启。
 */
bool companion_bridge_test_ctrl_get(void)
{
  bool on;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.ctrl_print;
  pthread_mutex_unlock(&g_bridge.lock);
  return on;
}

/**
 * @brief 设置 BLE 广播/传感器策略（devctl 用）。
 * @param on true 允许 radio。
 * @param notify true 则 companion 线程需 take 变更。
 */
void companion_bridge_policy_set_radio(bool on, bool notify)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.policy_radio = on;
  if (notify)
    {
      g_bridge.policy_radio_dirty = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
}

/** @brief 当前 radio 策略是否允许。 */
bool companion_bridge_policy_radio(void)
{
  bool on;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.policy_radio;
  pthread_mutex_unlock(&g_bridge.lock);
  return on;
}

/**
 * @brief 取出一次 radio 策略变更（一次性）。
 * @param[out] on 新策略值。
 * @return 有变更则为 true。
 */
bool companion_bridge_policy_radio_take(bool *on)
{
  bool dirty = false;

  if (on == NULL)
    {
      return false;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  if (g_bridge.policy_radio_dirty)
    {
      *on = g_bridge.policy_radio;
      g_bridge.policy_radio_dirty = false;
      dirty = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
  return dirty;
}

/**
 * @brief 设置传感器扫描/连接策略。
 * @param on true 允许传感器 central。
 * @param notify true 则 companion 线程需 take 变更。
 */
void companion_bridge_policy_set_sensor(bool on, bool notify)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.policy_sensor = on;
  if (notify)
    {
      g_bridge.policy_sensor_dirty = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
}

/** @brief 当前传感器策略是否允许。 */
bool companion_bridge_policy_sensor(void)
{
  bool on;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.policy_sensor;
  pthread_mutex_unlock(&g_bridge.lock);
  return on;
}

/**
 * @brief 取出一次传感器策略变更（一次性）。
 * @param[out] on 新策略值。
 * @return 有变更则为 true。
 */
bool companion_bridge_policy_sensor_take(bool *on)
{
  bool dirty = false;

  if (on == NULL)
    {
      return false;
    }

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  if (g_bridge.policy_sensor_dirty)
    {
      *on = g_bridge.policy_sensor;
      g_bridge.policy_sensor_dirty = false;
      dirty = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
  return dirty;
}

void companion_bridge_policy_set_notif(bool on)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.policy_notif = on;
  pthread_mutex_unlock(&g_bridge.lock);
}

bool companion_bridge_policy_notif(void)
{
  bool on;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.policy_notif;
  pthread_mutex_unlock(&g_bridge.lock);
  return on;
}

void companion_bridge_policy_set_calls_only(bool on)
{
  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  g_bridge.policy_calls = on;
  pthread_mutex_unlock(&g_bridge.lock);
}

bool companion_bridge_policy_calls_only(void)
{
  bool on;

  bridge_init();
  pthread_mutex_lock(&g_bridge.lock);
  on = g_bridge.policy_calls;
  pthread_mutex_unlock(&g_bridge.lock);
  return on;
}

/* ---------------------------------------------------------------------------
 * 通知过滤（板级，2026-09-24）：防"常驻通知刷屏"把 UI 顶死
 *
 * 现场：**微信通话**通知是 ongoing 的，手机侧持续下发（状态/计时一变就一条），
 * 设备端原先"来一条就塞 inbox + 投 pending"，于是 UI 每抽到一条就
 * `lv_pm_notify_show_ex()` 重弹一次 modal（3 s 一次）⇒ 整块 UI 卡死。
 *
 * 两道闸（都只改"弹不弹 / 弹几次"，**inbox 列表仍然是最新状态**）：
 *   ① **同键合并**：`type + title` 相同、且距上次看到 < NOTIF_MERGE_MS
 *      ⇒ 就地更新 inbox 里那条（并提到队首），**不投 pending**（不重弹）；
 *   ② **弹窗限流**：两条**不同**通知之间，pending 投递至少隔 NOTIF_POPUP_GAP_MS，
 *      更密的只进 inbox、不弹（计 `throttled`）。
 *
 * 计数：`rx / merged / throttled / evicted`，经 `ctl cnt notif` 周期报出；
 * `evicted`（inbox 满丢条目）由 diag 以 ERROR 直报（真丢通知），
 * merged/throttled 是"过滤在工作"的证据，WARN 级限频报。
 *
 * **只在设备端做，app 不需要改**（协议里没有 id/ongoing 字段，只能用
 * type+title 近似"同一条通知"，这也正是"常驻"能被识别出来的依据）。
 */
#define NOTIF_MERGE_MS      15000u /**< 同键多久内算"同一条的更新"。 */
#define NOTIF_POPUP_GAP_MS    800u /**< 两次弹窗之间的最短间隔。 */

static uint32_t g_notif_rx_n;
static uint32_t g_notif_merged_n;
static uint32_t g_notif_throttled_n;
static uint32_t g_notif_evict_n;
static uint32_t g_notif_last_seen_ms;  /**< 上次收到（任意）通知的时刻，管合并窗口。 */
static uint32_t g_notif_last_popup_ms; /**< 上次投 pending（= 会弹）的时刻，管限流。 */
static uint8_t  g_notif_last_type;     /**< 上次看到的 type，配合 title 做键。 */
static char     g_notif_last_title[COMPANION_NOTIF_TITLE_MAX + 1];

/** @brief 计数快照（`ctl cnt notif` / 异常判定用）。 */
void companion_bridge_notif_stats(uint32_t *rx, uint32_t *merged,
                                  uint32_t *throttled, uint32_t *evicted)
{
  pthread_mutex_lock(&g_bridge.lock);

  if (rx != NULL)
    {
      *rx = g_notif_rx_n;
    }

  if (merged != NULL)
    {
      *merged = g_notif_merged_n;
    }

  if (throttled != NULL)
    {
      *throttled = g_notif_throttled_n;
    }

  if (evicted != NULL)
    {
      *evicted = g_notif_evict_n;
    }

  pthread_mutex_unlock(&g_bridge.lock);
}

static void notif_copy(struct companion_notif_item *dst,
                       const struct companion_notif_item *src)
{
  *dst = *src;
  dst->title[COMPANION_NOTIF_TITLE_MAX] = '\0';
  dst->body[COMPANION_NOTIF_BODY_MAX] = '\0';
  dst->icon[COMPANION_NOTIF_ICON_PATH_MAX - 1u] = '\0';
}

void companion_bridge_notif_post(const struct companion_notif_item *item)
{
  uint32_t now;
  bool merge;
  int found = -1;
  uint8_t i;

  if (item == NULL)
    {
      return;
    }

  bridge_init();
  if (g_bridge.inbox == NULL || g_bridge.pending == NULL)
    {
      return;
    }

  now = bridge_now_ms();

  pthread_mutex_lock(&g_bridge.lock);

  g_notif_rx_n++;

  /* ① 是不是"同一条通知的又一次更新"（type+title 同键、在合并窗口内）。 */
  merge = (g_notif_last_title[0] != '\0') &&
          (item->type == g_notif_last_type) &&
          (strcmp(item->title, g_notif_last_title) == 0) &&
          ((int32_t)(now - g_notif_last_seen_ms) < (int32_t)NOTIF_MERGE_MS);

  /* inbox：同键就地更新（并提到队首），否则照旧 push-front + 满则丢最旧。 */
  for (i = 0; i < g_bridge.inbox_n; i++)
    {
      if (g_bridge.inbox[i].type == item->type &&
          strcmp(g_bridge.inbox[i].title, item->title) == 0)
        {
          found = (int)i;
          break;
        }
    }

  if (found > 0)
    {
      /* 上移到队首：先把这条挪出去，再整体后移，最后放回 [0]。 */
      struct companion_notif_item tmp;

      notif_copy(&tmp, &g_bridge.inbox[found]);
      memmove(&g_bridge.inbox[1], &g_bridge.inbox[0],
              (size_t)found * sizeof(g_bridge.inbox[0]));
      notif_copy(&g_bridge.inbox[0], &tmp);
      found = 0;
    }

  if (found == 0)
    {
      /* 同一条通知的新状态：只覆盖内容（body/icon 会变，type/title 不变）。 */
      notif_copy(&g_bridge.inbox[0], item);
    }
  else
    {
      if (g_bridge.inbox_n >= COMPANION_NOTIF_INBOX_MAX)
        {
          g_bridge.inbox_n = COMPANION_NOTIF_INBOX_MAX - 1u;
          g_notif_evict_n++;
        }

      if (g_bridge.inbox_n > 0)
        {
          memmove(&g_bridge.inbox[1], &g_bridge.inbox[0],
                  g_bridge.inbox_n * sizeof(g_bridge.inbox[0]));
        }

      notif_copy(&g_bridge.inbox[0], item);
      g_bridge.inbox_n++;
    }

  /* ② pending（= 会弹窗的那条路）：合并窗口内 或 距上次弹太近 ⇒ 不投。 */
  if (merge)
    {
      g_notif_merged_n++;
    }
  else if (g_notif_last_popup_ms != 0u &&
           (int32_t)(now - g_notif_last_popup_ms) < (int32_t)NOTIF_POPUP_GAP_MS)
    {
      g_notif_throttled_n++;
    }
  else
    {
      uint8_t tail;

      if (g_bridge.pend_n >= COMPANION_NOTIF_QUEUE_MAX)
        {
          g_bridge.pend_head = (uint8_t)((g_bridge.pend_head + 1u) %
                                         COMPANION_NOTIF_QUEUE_MAX);
          g_bridge.pend_n = COMPANION_NOTIF_QUEUE_MAX - 1u;
        }

      tail = (uint8_t)((g_bridge.pend_head + g_bridge.pend_n) %
                       COMPANION_NOTIF_QUEUE_MAX);
      notif_copy(&g_bridge.pending[tail], item);
      g_bridge.pend_n++;

      g_notif_last_popup_ms = now ? now : 1u;
    }

  g_notif_last_seen_ms = now ? now : 1u;
  g_notif_last_type = item->type;
  snprintf(g_notif_last_title, sizeof(g_notif_last_title), "%s", item->title);

  pthread_mutex_unlock(&g_bridge.lock);
}

bool companion_bridge_notif_take(struct companion_notif_item *out)
{
  bool ok = false;

  if (out == NULL)
    {
      return false;
    }

  memset(out, 0, sizeof(*out));
  bridge_init();
  if (g_bridge.pending == NULL)
    {
      return false;
    }

  pthread_mutex_lock(&g_bridge.lock);
  if (g_bridge.pend_n > 0)
    {
      notif_copy(out, &g_bridge.pending[g_bridge.pend_head]);
      g_bridge.pend_head = (uint8_t)((g_bridge.pend_head + 1u) %
                                     COMPANION_NOTIF_QUEUE_MAX);
      g_bridge.pend_n--;
      ok = true;
    }

  pthread_mutex_unlock(&g_bridge.lock);
  return ok;
}

void companion_bridge_inbox_get(struct companion_notif_item *out, uint8_t *n,
                                uint8_t maxn)
{
  uint8_t i;
  uint8_t count;

  bridge_init();
  if (g_bridge.inbox == NULL)
    {
      if (n != NULL)
        {
          *n = 0;
        }

      return;
    }

  pthread_mutex_lock(&g_bridge.lock);
  count = g_bridge.inbox_n;
  if (n != NULL)
    {
      *n = count;
    }

  if (out != NULL && maxn > 0)
    {
      if (count > maxn)
        {
          count = maxn;
        }

      for (i = 0; i < count; i++)
        {
          notif_copy(&out[i], &g_bridge.inbox[i]);
        }
    }

  pthread_mutex_unlock(&g_bridge.lock);
}
