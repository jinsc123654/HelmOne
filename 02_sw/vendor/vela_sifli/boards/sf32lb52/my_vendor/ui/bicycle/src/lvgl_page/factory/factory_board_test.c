/**
 * @file factory_board_test.c
 * @brief 工厂板卡测试：分页校验 LCD / 背光 / 蜂鸣 / IMU / GNSS / 气压 / 地磁等。
 *
 * KEY1 短按上一页，KEY2 短按下一页；KEY1 长按退出，KEY2 长按重测本页。
 * 页内不画 KEY 提示，底栏只显示进度或汇总计数。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "factory_board_test.h"

#include <nuttx/config.h>

#include "drv_io.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "myvendor_gnss.h"
#include "myvendor_board_sensor.h"
#include "myvendor_sound.h"
#include "myvendor_sys.h"

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
#  include "companion_bridge.h"
#endif

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FACTORY_POLL_MS       400u
#define FACTORY_RGB_MS        900u
#define FACTORY_BL_LEVELS     5u
#define FACTORY_GNSS_WAIT_MS  15000u
#define FACTORY_SENSOR_OK_N   2u
#define FACTORY_I2C_GRACE_MS  1500u
#define FACTORY_I2C_RESET_MS  12000u

enum factory_test_id
{
  FACTORY_T_RGB = 0,
  FACTORY_T_BACKLIGHT,
  FACTORY_T_BUZZER,
  FACTORY_T_ACCEL,
  FACTORY_T_GNSS,
  FACTORY_T_MAG,
  FACTORY_T_BARO,
  FACTORY_T_GYRO,
  FACTORY_T_BATTERY,
  FACTORY_T_BLE,
  FACTORY_T_STORAGE,
  FACTORY_T_SUMMARY,
  FACTORY_T_COUNT
};

enum factory_result
{
  FACTORY_R_NONE = 0,
  FACTORY_R_PASS,
  FACTORY_R_FAIL,
};

enum factory_ui_kind
{
  FAC_UI_NONE = 0,
  FAC_UI_RGB,
  FAC_UI_SIMPLE,
  FAC_UI_KV,
  FAC_UI_SUM,
};

typedef struct factory_board_test_s
{
  lv_obj_t * root;
  lv_obj_t * panel;
  lv_obj_t * head;
  lv_obj_t * body;
  lv_obj_t * hint;
  lv_obj_t * title_lab;
  lv_obj_t * sub_lab;
  lv_obj_t * badge;
  lv_obj_t * note;
  lv_obj_t * vals[4];
  lv_obj_t * sum_list;
  uint8_t    sum_sel;
  uint8_t    ui_kind;
  lv_timer_t * timer;
  uint8_t    step;
  uint8_t    rgb_idx;
  uint8_t    bl_idx;
  uint8_t    bl_saved;
  uint8_t    results[FACTORY_T_COUNT];
  uint8_t    sensor_ok_streak;
  uint32_t   step_enter_ms;
  uint32_t   reset_ms;
  uint8_t    reset_tried;
  bool       active;
  volatile bool buzzer_busy;
} factory_board_test_t;

static factory_board_test_t g_ft;

static const char * const g_test_names[FACTORY_T_COUNT] =
{
  "LCD RGB",
  "背光",
  "蜂鸣器",
  "加速度",
  "GNSS",
  "地磁",
  "气压",
  "陀螺",
  "电池",
  "BLE",
  "存储",
  "汇总",
};

static uint32_t factory_now_ms(void)
{
  return lv_tick_get();
}

static void factory_fmt_fixed(char *buf, size_t n, float v, unsigned scale)
{
  int neg;
  int scaled;
  unsigned mag;
  unsigned frac;

  if (buf == NULL || n == 0)
    {
      return;
    }

  if (scale != 10u && scale != 100u)
    {
      scale = 100u;
    }

  neg = (v < 0.0f);
  if (neg)
    {
      v = -v;
    }

  scaled = (int)(v * (float)scale + 0.5f);
  if (scaled < 0)
    {
      scaled = 0;
    }

  mag = (unsigned)scaled;
  frac = mag % scale;
  mag /= scale;
  if (neg && (mag != 0 || frac != 0))
    {
      (void)snprintf(buf, n, "-%u.%0*u", mag,
                     scale == 100u ? 2 : 1, frac);
    }
  else if (scale == 100u)
    {
      (void)snprintf(buf, n, "%u.%02u", mag, frac);
    }
  else
    {
      (void)snprintf(buf, n, "%u.%u", mag, frac);
    }
}

static const char *factory_result_text(uint8_t r)
{
  switch (r)
    {
      case FACTORY_R_PASS:
        return "PASS";
      case FACTORY_R_FAIL:
        return "FAIL";
      default:
        return "--";
    }
}

static const char *factory_badge_text(uint8_t r)
{
  switch (r)
    {
      case FACTORY_R_PASS:
        return "PASS";
      case FACTORY_R_FAIL:
        return "FAIL";
      default:
        return "检测中";
    }
}

static uint32_t factory_result_color(uint8_t r)
{
  switch (r)
    {
      case FACTORY_R_PASS:
        return HELM_COLOR_OK;
      case FACTORY_R_FAIL:
        return HELM_COLOR_HR;
      default:
        return HELM_COLOR_HAIR;
    }
}

static void factory_set_result(enum factory_test_id id, enum factory_result r);
static void factory_ui_reset(void);

static void factory_set_result(enum factory_test_id id, enum factory_result r)
{
  if (id < FACTORY_T_COUNT)
    {
      g_ft.results[id] = (uint8_t)r;
    }
}

static bool factory_i2c_step(void)
{
  switch ((enum factory_test_id)g_ft.step)
    {
      case FACTORY_T_ACCEL:
      case FACTORY_T_GYRO:
      case FACTORY_T_MAG:
      case FACTORY_T_BARO:
        return true;
      default:
        return false;
    }
}

/**
 * @brief 采样失败时只重试该路传感器，复位完成前不记 FAIL。
 * @return true 调用方应保持本页等待。
 */
static bool factory_i2c_hold_fail(void)
{
  if (myvendor_board_sensor_recovering())
    {
      if (!g_ft.reset_tried)
        {
          g_ft.reset_tried = 1;
          g_ft.reset_ms = factory_now_ms();
        }

      factory_set_result((enum factory_test_id)g_ft.step, FACTORY_R_NONE);
      return true;
    }

  if (!g_ft.reset_tried)
    {
      if ((factory_now_ms() - g_ft.step_enter_ms) < FACTORY_I2C_GRACE_MS)
        {
          factory_set_result((enum factory_test_id)g_ft.step, FACTORY_R_NONE);
          return true;
        }

      g_ft.reset_tried = 1;
      g_ft.reset_ms = factory_now_ms();
      factory_set_result((enum factory_test_id)g_ft.step, FACTORY_R_NONE);
      myvendor_board_sensor_request_reopen();
      return true;
    }

  if ((factory_now_ms() - g_ft.reset_ms) < FACTORY_I2C_RESET_MS)
    {
      factory_set_result((enum factory_test_id)g_ft.step, FACTORY_R_NONE);
      return true;
    }

  return false;
}

static void factory_ui_reset(void)
{
  g_ft.ui_kind = FAC_UI_NONE;
  g_ft.title_lab = NULL;
  g_ft.sub_lab = NULL;
  g_ft.badge = NULL;
  g_ft.note = NULL;
  g_ft.sum_list = NULL;
  g_ft.sum_sel = 0;
  memset(g_ft.vals, 0, sizeof(g_ft.vals));
}

static void factory_clear_panel(void)
{
  if (g_ft.body != NULL)
    {
      lv_obj_clean(g_ft.body);
    }

  factory_ui_reset();
}

static void factory_body_align(bool center)
{
  if (g_ft.body == NULL)
    {
      return;
    }

  if (center)
    {
      lv_obj_set_flex_align(g_ft.body, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_clear_flag(g_ft.body, LV_OBJ_FLAG_SCROLLABLE);
    }
  else
    {
      lv_obj_set_flex_align(g_ft.body, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
      lv_obj_add_flag(g_ft.body, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_scroll_dir(g_ft.body, LV_DIR_VER);
      lv_obj_set_scrollbar_mode(g_ft.body, LV_SCROLLBAR_MODE_OFF);
    }
}

static void factory_kv_bind(lv_obj_t *row, uint8_t i)
{
  if (row == NULL || i >= 4)
    {
      return;
    }

  g_ft.vals[i] = lv_obj_get_child(row, 2);
}

static void factory_face_hero(helm_ico_id_t ico, const char *title)
{
  lv_obj_t *row;
  lv_obj_t *icon;

  row = lv_obj_create(g_ft.body);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_set_style_pad_bottom(row, 6, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  icon = helm_icon_create(row, ico, 28);
  helm_icon_set_color(icon, helm_color(HELM_COLOR_NAV));
  g_ft.title_lab = helm_label(row, helm_font_val(), HELM_COLOR_INK,
                              title ? title : "");
}

static void factory_face_badge(void)
{
  g_ft.badge = helm_label(g_ft.body, helm_font_title(), HELM_COLOR_HAIR,
                          "检测中");
  lv_obj_set_width(g_ft.badge, lv_pct(100));
  lv_obj_set_style_text_align(g_ft.badge, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(g_ft.badge, 8, 0);
}

static void factory_set_sub(const char *txt)
{
  if (g_ft.sub_lab == NULL)
    {
      return;
    }

  lv_label_set_text(g_ft.sub_lab, txt ? txt : "");
}

static void factory_set_badge(uint8_t r)
{
  if (g_ft.badge == NULL)
    {
      return;
    }

  lv_label_set_text(g_ft.badge, factory_badge_text(r));
  lv_obj_set_style_text_color(g_ft.badge, helm_color(factory_result_color(r)),
                              0);
}

static void factory_set_note(const char *txt)
{
  if (g_ft.note == NULL)
    {
      return;
    }

  if (txt == NULL || txt[0] == '\0')
    {
      lv_label_set_text(g_ft.note, "");
      lv_obj_add_flag(g_ft.note, LV_OBJ_FLAG_HIDDEN);
      return;
    }

  lv_label_set_text(g_ft.note, txt);
  lv_obj_clear_flag(g_ft.note, LV_OBJ_FLAG_HIDDEN);
}

static void factory_set_val(uint8_t i, const char *txt)
{
  if (i >= 4 || g_ft.vals[i] == NULL)
    {
      return;
    }

  lv_label_set_text(g_ft.vals[i], txt ? txt : "--");
}

static void factory_face_simple(helm_ico_id_t ico, const char *title,
                               const char *sub)
{
  if (g_ft.ui_kind != FAC_UI_SIMPLE)
    {
      factory_clear_panel();
      factory_body_align(false);
      factory_face_hero(ico, title);
      g_ft.sub_lab = helm_label(g_ft.body, helm_font_lab(), HELM_COLOR_INK,
                                sub ? sub : "");
      lv_obj_set_width(g_ft.sub_lab, lv_pct(100));
      factory_face_badge();
      g_ft.ui_kind = FAC_UI_SIMPLE;
    }
  else if (g_ft.title_lab)
    {
      lv_label_set_text(g_ft.title_lab, title ? title : "");
      factory_set_sub(sub);
    }
}

static void factory_face_kv(helm_ico_id_t ico, const char *title,
                           const helm_ico_id_t *icos, const char * const *keys,
                           uint8_t n)
{
  lv_obj_t *kv;
  uint8_t i;

  if (n > 4)
    {
      n = 4;
    }

  if (g_ft.ui_kind != FAC_UI_KV)
    {
      factory_clear_panel();
      factory_body_align(false);
      factory_face_hero(ico, title);
      kv = helm_kvbox_create(g_ft.body);
      for (i = 0; i < n; i++)
        {
          factory_kv_bind(helm_kv_add(kv, icos[i], keys[i], "--"), i);
        }

      helm_kvbox_seal(kv);
      factory_face_badge();
      g_ft.note = helm_label(g_ft.body, helm_font_lab(), HELM_COLOR_NAV, "");
      lv_obj_set_width(g_ft.note, lv_pct(100));
      lv_obj_set_style_text_align(g_ft.note, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_add_flag(g_ft.note, LV_OBJ_FLAG_HIDDEN);
      g_ft.ui_kind = FAC_UI_KV;
    }
  else if (g_ft.title_lab)
    {
      lv_label_set_text(g_ft.title_lab, title ? title : "");
    }
}

static void factory_show_i2c_reset(void)
{
  char line[48];
  uint32_t left;
  uint32_t age;

  if (myvendor_board_sensor_recovering())
    {
      factory_set_note("传感器重试中");
      return;
    }

  if (!g_ft.reset_tried)
    {
      factory_set_note("");
      return;
    }

  age = factory_now_ms() - g_ft.reset_ms;
  if (age >= FACTORY_I2C_RESET_MS)
    {
      factory_set_note("");
      return;
    }

  left = FACTORY_I2C_RESET_MS - age;
  lv_snprintf(line, sizeof(line), "复位后等待 %u s",
              (unsigned)((left + 999u) / 1000u));
  factory_set_note(line);
}

static void factory_show_hint(void)
{
  char buf[40];
  uint8_t i;
  uint8_t pass;
  uint8_t fail;

  if (g_ft.hint == NULL)
    {
      return;
    }

  if (g_ft.step >= FACTORY_T_SUMMARY)
    {
      pass = 0;
      fail = 0;
      for (i = 0; i < FACTORY_T_SUMMARY; i++)
        {
          if (g_ft.results[i] == FACTORY_R_PASS)
            {
              pass++;
            }
          else if (g_ft.results[i] == FACTORY_R_FAIL)
            {
              fail++;
            }
        }

      lv_obj_set_style_text_font(g_ft.hint, helm_font_val(), 0);
      lv_snprintf(buf, sizeof(buf), "PASS %u  FAIL %u",
                  (unsigned)pass, (unsigned)fail);
      lv_obj_set_style_text_color(g_ft.hint,
                                  helm_color(fail == 0 ? HELM_COLOR_OK :
                                             HELM_COLOR_HR), 0);
    }
  else
    {
      lv_obj_set_style_text_font(g_ft.hint, helm_font_title(), 0);
      lv_snprintf(buf, sizeof(buf), "%u / %u",
                  (unsigned)(g_ft.step + 1u),
                  (unsigned)FACTORY_T_COUNT);
      if (g_ft.step != FACTORY_T_RGB)
        {
          lv_obj_set_style_text_color(g_ft.hint, helm_color(HELM_COLOR_INK), 0);
        }
    }

  lv_label_set_text(g_ft.hint, buf);
}

static void factory_eval_current(void);

static void *factory_buzzer_thread(void *arg)
{
  static const uint16_t hz[] = {1000u, 2000u, 4000u, 8000u};
  size_t i;

  (void)arg;
  for (i = 0; i < sizeof(hz) / sizeof(hz[0]); i++)
    {
      (void)myvendor_sound_diag_pwm(hz[i], 350u);
      usleep(80000);
    }

  g_ft.buzzer_busy = false;
  return NULL;
}

static void factory_run_buzzer(void)
{
  pthread_t th;
  pthread_attr_t attr;
  int ret;

  if (g_ft.buzzer_busy)
    {
      return;
    }

  g_ft.buzzer_busy = true;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&th, &attr, factory_buzzer_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      g_ft.buzzer_busy = false;
      factory_set_result(FACTORY_T_BUZZER, FACTORY_R_FAIL);
    }
}

static void factory_eval_rgb(void)
{
  /** 人工目检：能看完 RGB 轮播即记 PASS（离开页或下一页时确认）。 */
  factory_set_result(FACTORY_T_RGB, FACTORY_R_PASS);
}

static void factory_eval_backlight(void)
{
  factory_set_result(FACTORY_T_BACKLIGHT, FACTORY_R_PASS);
}

static void factory_eval_buzzer(void)
{
  if (!g_ft.buzzer_busy)
    {
      factory_set_result(FACTORY_T_BUZZER, FACTORY_R_PASS);
    }
}

static void factory_eval_accel(void)
{
  myvendor_sys_vec3_t a;
  float mag;
  bool ok;

  myvendor_board_sensor_accel_get(&a);
  mag = 0.0f;
  if (a.valid)
    {
      mag = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    }

  ok = a.valid && mag >= 7.0f && mag <= 12.5f;
  if (!ok)
    {
      g_ft.sensor_ok_streak = 0;
      if (factory_i2c_hold_fail())
        {
          return;
        }

      factory_set_result(FACTORY_T_ACCEL, FACTORY_R_FAIL);
      return;
    }

  if (g_ft.sensor_ok_streak < 255)
    {
      g_ft.sensor_ok_streak++;
    }

  if (g_ft.sensor_ok_streak >= FACTORY_SENSOR_OK_N)
    {
      factory_set_result(FACTORY_T_ACCEL, FACTORY_R_PASS);
    }
}

static void factory_eval_gyro(void)
{
  myvendor_sys_vec3_t g;

  myvendor_board_sensor_gyro_get(&g);
  if (!g.valid)
    {
      g_ft.sensor_ok_streak = 0;
      if (factory_i2c_hold_fail())
        {
          return;
        }

      factory_set_result(FACTORY_T_GYRO, FACTORY_R_FAIL);
      return;
    }

  if (fabsf(g.x) < 0.8f && fabsf(g.y) < 0.8f && fabsf(g.z) < 0.8f)
    {
      if (g_ft.sensor_ok_streak < 255)
        {
          g_ft.sensor_ok_streak++;
        }
    }
  else
    {
      g_ft.sensor_ok_streak = 0;
    }

  if (g_ft.sensor_ok_streak >= FACTORY_SENSOR_OK_N)
    {
      factory_set_result(FACTORY_T_GYRO, FACTORY_R_PASS);
    }
}

static void factory_eval_mag(void)
{
  myvendor_sys_mag_t m;
  float mag;
  bool ok;

  myvendor_board_sensor_mag_get(&m);
  mag = 0.0f;
  if (m.valid)
    {
      mag = sqrtf(m.x * m.x + m.y * m.y + m.z * m.z);
    }

  ok = m.valid && mag >= 15.0f && mag <= 120.0f;
  if (!ok)
    {
      if (factory_i2c_hold_fail())
        {
          return;
        }

      factory_set_result(FACTORY_T_MAG, FACTORY_R_FAIL);
      return;
    }

  factory_set_result(FACTORY_T_MAG, FACTORY_R_PASS);
}

static void factory_eval_baro(void)
{
  myvendor_sys_baro_t b;
  bool ok;

  myvendor_board_sensor_baro_get(&b);
  ok = b.valid && b.hpa >= 800.0f && b.hpa <= 1100.0f;
  if (!ok)
    {
      if (factory_i2c_hold_fail())
        {
          return;
        }

      factory_set_result(FACTORY_T_BARO, FACTORY_R_FAIL);
      return;
    }

  factory_set_result(FACTORY_T_BARO, FACTORY_R_PASS);
}

static void factory_eval_gnss(void)
{
  myvendor_sys_gnss_t fix;
  uint32_t age;

  if (!myvendor_sys_onboard_gnss_get(&fix))
    {
      factory_set_result(FACTORY_T_GNSS, FACTORY_R_FAIL);
      return;
    }

  age = factory_now_ms() - g_ft.step_enter_ms;
  if (fix.valid)
    {
      factory_set_result(FACTORY_T_GNSS, FACTORY_R_PASS);
      return;
    }

  if (fix.alive && fix.sats_heard > 0)
    {
      factory_set_result(FACTORY_T_GNSS, FACTORY_R_PASS);
      return;
    }

  if (age >= FACTORY_GNSS_WAIT_MS)
    {
      factory_set_result(FACTORY_T_GNSS, FACTORY_R_FAIL);
    }
}

static void factory_eval_battery(void)
{
  int mv = myvendor_sys_battery_mv();
  int pct = myvendor_sys_battery_percent();

  if (mv >= MYVENDOR_SYS_BAT_MV_EMPTY && pct >= 0)
    {
      factory_set_result(FACTORY_T_BATTERY, FACTORY_R_PASS);
    }
  else
    {
      factory_set_result(FACTORY_T_BATTERY, FACTORY_R_FAIL);
    }
}

static void factory_eval_ble(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  factory_set_result(FACTORY_T_BLE,
                     companion_bridge_alive_get() ?
                     FACTORY_R_PASS : FACTORY_R_FAIL);
#else
  factory_set_result(FACTORY_T_BLE, FACTORY_R_FAIL);
#endif
}

static bool factory_mnt_ok(const char *path)
{
  return path != NULL && access(path, R_OK) == 0;
}

static void factory_eval_storage(void)
{
  bool lfs = factory_mnt_ok("/mnt/lfs");
  bool kv = factory_mnt_ok("/mnt/kv");
  bool fat = factory_mnt_ok("/mnt/fat");

  factory_set_result(FACTORY_T_STORAGE,
                     (lfs && kv && fat) ? FACTORY_R_PASS : FACTORY_R_FAIL);
}

static void factory_eval_current(void)
{
  switch ((enum factory_test_id)g_ft.step)
    {
      case FACTORY_T_RGB:
        factory_eval_rgb();
        break;
      case FACTORY_T_BACKLIGHT:
        factory_eval_backlight();
        break;
      case FACTORY_T_BUZZER:
        factory_eval_buzzer();
        break;
      case FACTORY_T_ACCEL:
        factory_eval_accel();
        break;
      case FACTORY_T_GNSS:
        factory_eval_gnss();
        break;
      case FACTORY_T_MAG:
        factory_eval_mag();
        break;
      case FACTORY_T_BARO:
        factory_eval_baro();
        break;
      case FACTORY_T_GYRO:
        factory_eval_gyro();
        break;
      case FACTORY_T_BATTERY:
        factory_eval_battery();
        break;
      case FACTORY_T_BLE:
        factory_eval_ble();
        break;
      case FACTORY_T_STORAGE:
        factory_eval_storage();
        break;
      default:
        break;
    }
}

static void factory_rgb_apply(uint8_t idx)
{
  static const uint32_t colors[] =
  {
    0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFFFFu, 0x000000u
  };
  static const char *names[] = {"红", "绿", "蓝", "白", "黑"};
  uint8_t n = (uint8_t)(sizeof(colors) / sizeof(colors[0]));
  char line[32];
  uint32_t fg;

  if (idx >= n)
    {
      idx = 0;
    }

  g_ft.rgb_idx = idx;
  lv_obj_set_style_bg_color(g_ft.panel, lv_color_hex(colors[idx]), 0);
  lv_obj_set_style_bg_opa(g_ft.panel, LV_OPA_COVER, 0);
  if (g_ft.head != NULL)
    {
      lv_obj_add_flag(g_ft.head, LV_OBJ_FLAG_HIDDEN);
    }

  fg = (idx >= 4u) ? HELM_COLOR_PAPER : HELM_COLOR_INK;
  lv_snprintf(line, sizeof(line), "%s  目检坏点", names[idx]);
  if (g_ft.ui_kind != FAC_UI_RGB)
    {
      factory_clear_panel();
      factory_body_align(true);
      g_ft.title_lab = helm_label(g_ft.body, helm_font_val(), fg, line);
      g_ft.ui_kind = FAC_UI_RGB;
    }
  else if (g_ft.title_lab)
    {
      lv_label_set_text(g_ft.title_lab, line);
      lv_obj_set_style_text_color(g_ft.title_lab, helm_color(fg), 0);
    }

  if (g_ft.hint)
    {
      lv_obj_set_style_text_color(g_ft.hint, helm_color(fg), 0);
    }
}

static void factory_show_backlight(void)
{
  static const uint8_t levels[FACTORY_BL_LEVELS] = {0u, 15u, 35u, 65u, 100u};
  char line[48];

  if (g_ft.bl_idx >= FACTORY_BL_LEVELS)
    {
      g_ft.bl_idx = 0;
    }

  (void)BSP_LCD_BL_SetPct(levels[g_ft.bl_idx]);
  lv_obj_set_style_bg_color(g_ft.panel, lv_color_hex(HELM_COLOR_SCR), 0);
  if (g_ft.head != NULL)
    {
      lv_obj_clear_flag(g_ft.head, LV_OBJ_FLAG_HIDDEN);
    }

  lv_snprintf(line, sizeof(line), "背光 %u%%", (unsigned)levels[g_ft.bl_idx]);
  factory_face_simple(HELM_ICO_SUN, line,
                      levels[g_ft.bl_idx] == 0u ? "关背光应仍能看清" : "目视亮度变化");
  factory_set_badge(g_ft.results[FACTORY_T_BACKLIGHT]);
}

static void factory_show_buzzer(void)
{
  factory_face_simple(HELM_ICO_SOUND, "蜂鸣器",
                      g_ft.buzzer_busy ? "播放 1/2/4/8 kHz" : "听完即通过");
  factory_set_badge(g_ft.results[FACTORY_T_BUZZER]);
}

static void factory_show_xyz(helm_ico_id_t ico, const char *title,
                             float x, float y, float z, bool valid,
                             unsigned scale, uint8_t result)
{
  static const helm_ico_id_t icos[] =
    { HELM_ICO_NAV, HELM_ICO_NAV, HELM_ICO_NAV };
  static const char * const keys[] = { "X", "Y", "Z" };
  char fx[16];
  char fy[16];
  char fz[16];

  factory_face_kv(ico, title, icos, keys, 3);
  if (valid)
    {
      factory_fmt_fixed(fx, sizeof(fx), x, scale);
      factory_fmt_fixed(fy, sizeof(fy), y, scale);
      factory_fmt_fixed(fz, sizeof(fz), z, scale);
      factory_set_val(0, fx);
      factory_set_val(1, fy);
      factory_set_val(2, fz);
    }
  else
    {
      factory_set_val(0, "--");
      factory_set_val(1, "--");
      factory_set_val(2, "--");
    }

  factory_set_badge(result);
}

static void factory_show_accel(void)
{
  myvendor_sys_vec3_t a;

  myvendor_board_sensor_accel_get(&a);
  factory_eval_accel();
  factory_show_xyz(HELM_ICO_BOLT, "加速度 m/s2", a.x, a.y, a.z, a.valid, 100u,
                   g_ft.results[FACTORY_T_ACCEL]);
  factory_show_i2c_reset();
}

static void factory_show_gyro(void)
{
  myvendor_sys_vec3_t g;

  myvendor_board_sensor_gyro_get(&g);
  factory_eval_gyro();
  factory_show_xyz(HELM_ICO_REV, "陀螺 rad/s", g.x, g.y, g.z, g.valid, 100u,
                   g_ft.results[FACTORY_T_GYRO]);
  factory_show_i2c_reset();
  if (g_ft.note != NULL && lv_obj_has_flag(g_ft.note, LV_OBJ_FLAG_HIDDEN))
    {
      factory_set_note("设备静止");
    }
}

static void factory_show_gnss(void)
{
  static const helm_ico_id_t icos[] =
    { HELM_ICO_GPS, HELM_ICO_LIST, HELM_ICO_TIME };
  static const char * const keys[] = { "定位", "搜星", "等待" };
  myvendor_sys_gnss_t fix;
  char a[24];
  char b[24];
  char c[24];
  uint32_t left;

  factory_face_kv(HELM_ICO_GPS, "GNSS", icos, keys, 3);

  if (myvendor_sys_onboard_gnss_get(&fix))
    {
      lv_snprintf(a, sizeof(a), "%s", fix.valid ? "OK" : "无");
      lv_snprintf(b, sizeof(b), "%u / %u",
                  (unsigned)fix.sats_heard, (unsigned)fix.sats_in_view);
      lv_snprintf(c, sizeof(c), "%s", fix.alive ? "NMEA" : "无数据");
    }
  else
    {
      left = FACTORY_GNSS_WAIT_MS;
      if (factory_now_ms() > g_ft.step_enter_ms)
        {
          left = FACTORY_GNSS_WAIT_MS -
                 (factory_now_ms() - g_ft.step_enter_ms);
        }

      lv_snprintf(a, sizeof(a), "--");
      lv_snprintf(b, sizeof(b), "--");
      lv_snprintf(c, sizeof(c), "%u s", (unsigned)((left + 999u) / 1000u));
    }

  factory_eval_gnss();
  factory_set_val(0, a);
  factory_set_val(1, b);
  factory_set_val(2, c);
  factory_set_badge(g_ft.results[FACTORY_T_GNSS]);
}

static void factory_show_mag(void)
{
  myvendor_sys_mag_t m;

  myvendor_board_sensor_mag_get(&m);
  factory_eval_mag();
  factory_show_xyz(HELM_ICO_PIN, "地磁 uT", m.x, m.y, m.z, m.valid, 10u,
                   g_ft.results[FACTORY_T_MAG]);
  factory_show_i2c_reset();
}

static void factory_show_baro(void)
{
  static const helm_ico_id_t icos[] = { HELM_ICO_CLIMB, HELM_ICO_ALT };
  static const char * const keys[] = { "hPa", "C" };
  myvendor_sys_baro_t b;
  char hp[16];
  char tc[16];

  myvendor_board_sensor_baro_get(&b);
  factory_eval_baro();
  factory_face_kv(HELM_ICO_CLIMB, "气压", icos, keys, 2);
  if (b.valid)
    {
      factory_fmt_fixed(hp, sizeof(hp), b.hpa, 10);
      factory_fmt_fixed(tc, sizeof(tc), b.temp_c, 10);
      factory_set_val(0, hp);
      factory_set_val(1, tc);
    }
  else
    {
      factory_set_val(0, "--");
      factory_set_val(1, "--");
    }

  factory_set_badge(g_ft.results[FACTORY_T_BARO]);
  factory_show_i2c_reset();
}

static void factory_show_battery(void)
{
  static const helm_ico_id_t icos[] =
    { HELM_ICO_BATT, HELM_ICO_BOLT, HELM_ICO_POWER };
  static const char * const keys[] = { "电压", "%", "电源" };
  char line[24];
  char volt[12];
  int mv = myvendor_sys_battery_mv();
  int pct = myvendor_sys_battery_percent();
  uint8_t st = myvendor_sys_power_state();

  factory_eval_battery();
  factory_face_kv(HELM_ICO_BATT, "电池", icos, keys, 3);
  if (mv >= 0)
    {
      /* 与 sys bat 同一路 VBATS；出厂分压校准后约 3.7～4.2 V。 */
      factory_fmt_fixed(volt, sizeof(volt), (float)mv / 1000.0f, 100u);
      lv_snprintf(line, sizeof(line), "%s V", volt);
      factory_set_val(0, line);
      lv_snprintf(line, sizeof(line), "%d", pct);
      factory_set_val(1, line);
    }
  else
    {
      factory_set_val(0, "ADC");
      factory_set_val(1, "--");
    }

  lv_snprintf(line, sizeof(line), "%u", (unsigned)st);
  factory_set_val(2, line);
  factory_set_badge(g_ft.results[FACTORY_T_BATTERY]);
}

static void factory_show_ble(void)
{
  static const helm_ico_id_t icos[] = { HELM_ICO_BLE, HELM_ICO_CALL };
  static const char * const keys[] = { "Companion", "手机" };
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  bool alive = companion_bridge_alive_get();
  bool phone = myvendor_sys_phone_ble_connected();
#else
  bool alive = false;
  bool phone = false;
#endif

  factory_eval_ble();
  factory_face_kv(HELM_ICO_BLE, "BLE", icos, keys, 2);
  factory_set_val(0, alive ? "运行" : "停止");
  factory_set_val(1, phone ? "已连" : "未连");
  factory_set_badge(g_ft.results[FACTORY_T_BLE]);
}

static void factory_show_storage(void)
{
  static const helm_ico_id_t icos[] =
    { HELM_ICO_SAVE, HELM_ICO_GEAR, HELM_ICO_LIST };
  static const char * const keys[] = { "lfs", "kv", "fat" };
  bool lfs = factory_mnt_ok("/mnt/lfs");
  bool kv = factory_mnt_ok("/mnt/kv");
  bool fat = factory_mnt_ok("/mnt/fat");

  factory_set_result(FACTORY_T_STORAGE,
                     (lfs && kv && fat) ? FACTORY_R_PASS : FACTORY_R_FAIL);
  factory_face_kv(HELM_ICO_SAVE, "存储", icos, keys, 3);
  factory_set_val(0, lfs ? "OK" : "FAIL");
  factory_set_val(1, kv ? "OK" : "FAIL");
  factory_set_val(2, fat ? "OK" : "FAIL");
  factory_set_badge(g_ft.results[FACTORY_T_STORAGE]);
}

static void factory_sum_row_tint(lv_obj_t *row, bool sel)
{
  if (row == NULL)
    {
      return;
    }

  lv_obj_set_style_bg_color(row,
                            helm_color(sel ? HELM_COLOR_NAV : HELM_COLOR_PAPER),
                            0);
}

static void factory_sum_row(lv_obj_t *list, uint8_t i, bool sel)
{
  lv_obj_t *row;
  lv_obj_t *ico;
  lv_obj_t *name;
  lv_obj_t *val;
  uint8_t r = g_ft.results[i];
  uint32_t col = factory_result_color(r);
  helm_ico_id_t id;

  if (r == FACTORY_R_PASS)
    {
      id = HELM_ICO_CHECK;
    }
  else if (r == FACTORY_R_FAIL)
    {
      id = HELM_ICO_XMARK;
    }
  else
    {
      id = HELM_ICO_WARN;
    }

  row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), HELM_LIST_ROW_H);
  lv_obj_set_style_radius(row, HELM_RADIUS, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  factory_sum_row_tint(row, sel);
  lv_obj_set_style_pad_hor(row, 10, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  ico = helm_icon_create(row, id, 22);
  helm_icon_set_color(ico, helm_color(col));
  name = helm_label(row, helm_font_val(), HELM_COLOR_INK, g_test_names[i]);
  helm_grow_x(name);
  val = helm_label(row, helm_font_val(), col, factory_result_text(r));
  lv_obj_set_style_min_width(val, 56, 0);
}

static void factory_sum_move(int dir)
{
  uint8_t n = (uint8_t)FACTORY_T_SUMMARY;
  uint8_t from;
  uint8_t to;
  lv_obj_t *row;

  if (g_ft.sum_list == NULL || n == 0)
    {
      return;
    }

  from = g_ft.sum_sel;
  if (dir > 0)
    {
      if (from + 1u >= n)
        {
          return;
        }

      to = (uint8_t)(from + 1u);
    }
  else
    {
      if (from == 0)
        {
          return;
        }

      to = (uint8_t)(from - 1u);
    }

  factory_sum_row_tint(lv_obj_get_child(g_ft.sum_list, from), false);
  row = lv_obj_get_child(g_ft.sum_list, to);
  factory_sum_row_tint(row, true);
  if (row)
    {
      lv_obj_scroll_to_view(row, LV_ANIM_ON);
      helm_obj_press(row);
    }

  g_ft.sum_sel = to;
  myvendor_sound_key();
}

static void factory_show_summary(void)
{
  uint8_t i;

  factory_clear_panel();
  factory_body_align(false);
  lv_obj_clear_flag(g_ft.body, LV_OBJ_FLAG_SCROLLABLE);
  g_ft.sum_list = helm_mlist_create(g_ft.body);
  g_ft.sum_sel = 0;
  for (i = 0; i < FACTORY_T_SUMMARY; i++)
    {
      factory_sum_row(g_ft.sum_list, i, i == 0);
    }

  if (lv_obj_get_child_count(g_ft.sum_list) > 0)
    {
      lv_obj_scroll_to_view(lv_obj_get_child(g_ft.sum_list, 0), LV_ANIM_OFF);
    }

  g_ft.ui_kind = FAC_UI_SUM;
}

static void factory_show_step(uint8_t step)
{
  g_ft.step = step;
  g_ft.step_enter_ms = factory_now_ms();
  g_ft.sensor_ok_streak = 0;
  g_ft.reset_tried = 0;
  g_ft.reset_ms = 0;
  factory_clear_panel();

  if (g_ft.panel != NULL)
    {
      lv_obj_set_style_bg_color(g_ft.panel, helm_color(HELM_COLOR_SCR), 0);
    }

  if (g_ft.head != NULL)
    {
      helm_mhead_set(g_ft.head, g_test_names[step]);
      if (step == FACTORY_T_RGB)
        {
          lv_obj_add_flag(g_ft.head, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_clear_flag(g_ft.head, LV_OBJ_FLAG_HIDDEN);
        }
    }

  switch ((enum factory_test_id)step)
    {
      case FACTORY_T_RGB:
        factory_rgb_apply(g_ft.rgb_idx);
        break;
      case FACTORY_T_BACKLIGHT:
        factory_show_backlight();
        break;
      case FACTORY_T_BUZZER:
        factory_show_buzzer();
        factory_run_buzzer();
        break;
      case FACTORY_T_ACCEL:
        factory_show_accel();
        break;
      case FACTORY_T_GNSS:
        factory_show_gnss();
        break;
      case FACTORY_T_MAG:
        factory_show_mag();
        break;
      case FACTORY_T_BARO:
        factory_show_baro();
        break;
      case FACTORY_T_GYRO:
        factory_show_gyro();
        break;
      case FACTORY_T_BATTERY:
        factory_show_battery();
        break;
      case FACTORY_T_BLE:
        factory_show_ble();
        break;
      case FACTORY_T_STORAGE:
        factory_show_storage();
        break;
      case FACTORY_T_SUMMARY:
        factory_eval_current();
        factory_show_summary();
        break;
      default:
        break;
    }

  factory_show_hint();
}

static void factory_timer_cb(lv_timer_t * tmr)
{
  LV_UNUSED(tmr);
  factory_board_test_poll();
}

static void factory_build_shell(void)
{
  lv_obj_t *foot;

  if (g_ft.root == NULL)
    {
      return;
    }

  g_ft.panel = lv_obj_create(g_ft.root);
  lv_obj_remove_style_all(g_ft.panel);
  lv_obj_set_size(g_ft.panel, PAGE_HOR_RES, PAGE_VER_RES);
  lv_obj_align(g_ft.panel, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_clear_flag(g_ft.panel, LV_OBJ_FLAG_SCROLLABLE);
  helm_style_scr(g_ft.panel);
  lv_obj_set_flex_flow(g_ft.panel, LV_FLEX_FLOW_COLUMN);

  g_ft.head = helm_mhead_create(g_ft.panel, "板卡测试");
  g_ft.body = lv_obj_create(g_ft.panel);
  lv_obj_remove_style_all(g_ft.body);
  helm_grow_y(g_ft.body);
  lv_obj_set_style_pad_all(g_ft.body, 8, 0);
  lv_obj_set_style_pad_row(g_ft.body, 6, 0);
  lv_obj_set_flex_flow(g_ft.body, LV_FLEX_FLOW_COLUMN);
  factory_body_align(false);

  foot = lv_obj_create(g_ft.panel);
  lv_obj_remove_style_all(foot);
  lv_obj_set_size(foot, PAGE_HOR_RES, 28);
  lv_obj_set_flex_grow(foot, 0);
  lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
  g_ft.hint = helm_label(foot, helm_font_title(), HELM_COLOR_INK, "");
}

void factory_board_test_begin(lv_obj_t * root)
{
  if (root == NULL || g_ft.active)
    {
      return;
    }

  memset(&g_ft, 0, sizeof(g_ft));
  g_ft.root = root;
  g_ft.active = true;
  g_ft.bl_saved = BSP_LCD_BL_GetSavedPct();
  g_ft.rgb_idx = 0;
  g_ft.bl_idx = 0;

  factory_build_shell();
  helm_obj_enter(g_ft.panel, 1);
  factory_show_step(FACTORY_T_RGB);

  if (g_ft.timer == NULL)
    {
      g_ft.timer = lv_timer_create(factory_timer_cb, FACTORY_POLL_MS, NULL);
    }
}

void factory_board_test_end(void)
{
  if (!g_ft.active)
    {
      return;
    }

  if (g_ft.timer != NULL)
    {
      lv_timer_del(g_ft.timer);
      g_ft.timer = NULL;
    }

  (void)BSP_LCD_BL_SetPct(g_ft.bl_saved);

  if (g_ft.panel != NULL)
    {
      lv_obj_delete(g_ft.panel);
      g_ft.panel = NULL;
    }

  g_ft.head = NULL;
  g_ft.body = NULL;
  g_ft.hint = NULL;
  factory_ui_reset();
  g_ft.active = false;
  g_ft.root = NULL;
}

bool factory_board_test_active(void)
{
  return g_ft.active;
}

void factory_board_test_poll(void)
{
  if (!g_ft.active)
    {
      return;
    }

  switch ((enum factory_test_id)g_ft.step)
    {
      case FACTORY_T_RGB:
        {
          static uint32_t last_rgb_ms;

          if (factory_now_ms() - last_rgb_ms >= FACTORY_RGB_MS)
            {
              last_rgb_ms = factory_now_ms();
              factory_rgb_apply((uint8_t)((g_ft.rgb_idx + 1u) % 5u));
            }
        }
        break;
      case FACTORY_T_BACKLIGHT:
        {
          static uint32_t last_bl_ms;

          if (factory_now_ms() - last_bl_ms >= 1200u)
            {
              last_bl_ms = factory_now_ms();
              g_ft.bl_idx = (uint8_t)((g_ft.bl_idx + 1u) % FACTORY_BL_LEVELS);
              factory_show_backlight();
            }
        }
        break;
      case FACTORY_T_BUZZER:
        factory_show_buzzer();
        if (!g_ft.buzzer_busy &&
            g_ft.results[FACTORY_T_BUZZER] == FACTORY_R_NONE)
          {
            factory_set_result(FACTORY_T_BUZZER, FACTORY_R_PASS);
          }
        break;
      case FACTORY_T_ACCEL:
        factory_show_accel();
        break;
      case FACTORY_T_GNSS:
        factory_show_gnss();
        break;
      case FACTORY_T_MAG:
        factory_show_mag();
        break;
      case FACTORY_T_BARO:
        factory_show_baro();
        break;
      case FACTORY_T_GYRO:
        factory_show_gyro();
        break;
      case FACTORY_T_BATTERY:
        factory_show_battery();
        break;
      case FACTORY_T_BLE:
        factory_show_ble();
        break;
      case FACTORY_T_STORAGE:
        factory_show_storage();
        break;
      default:
        break;
    }
}

static void factory_leave_step(void)
{
  factory_eval_current();
  if (g_ft.step == FACTORY_T_BACKLIGHT)
    {
      (void)BSP_LCD_BL_SetPct(g_ft.bl_saved);
    }
}

void factory_board_test_prev(void)
{
  if (!g_ft.active)
    {
      return;
    }

  if (g_ft.step >= FACTORY_T_SUMMARY)
    {
      factory_sum_move(-1);
      return;
    }

  if (g_ft.step == 0)
    {
      return;
    }

  factory_leave_step();
  factory_show_step((uint8_t)(g_ft.step - 1u));
}

void factory_board_test_next(void)
{
  if (!g_ft.active)
    {
      return;
    }

  if (g_ft.step >= FACTORY_T_SUMMARY)
    {
      factory_sum_move(1);
      return;
    }

  factory_leave_step();
  factory_show_step((uint8_t)(g_ft.step + 1u));
}

void factory_board_test_exit(void)
{
  if (!g_ft.active)
    {
      return;
    }

  factory_leave_step();
  factory_board_test_end();
}

void factory_board_test_retest(void)
{
  if (!g_ft.active)
    {
      return;
    }

  if (g_ft.step >= FACTORY_T_SUMMARY)
    {
      memset(g_ft.results, 0, sizeof(g_ft.results));
      g_ft.rgb_idx = 0;
      g_ft.bl_idx = 0;
      factory_show_step(FACTORY_T_RGB);
      myvendor_sound_ok();
      return;
    }

  g_ft.results[g_ft.step] = FACTORY_R_NONE;
  g_ft.sensor_ok_streak = 0;
  g_ft.step_enter_ms = factory_now_ms();
  g_ft.reset_tried = 0;
  g_ft.reset_ms = 0;

  if (factory_i2c_step())
    {
      myvendor_board_sensor_request_reopen();
      g_ft.reset_tried = 1;
      g_ft.reset_ms = factory_now_ms();
    }

  switch ((enum factory_test_id)g_ft.step)
    {
      case FACTORY_T_RGB:
        g_ft.rgb_idx = 0;
        factory_rgb_apply(0);
        break;
      case FACTORY_T_BACKLIGHT:
        g_ft.bl_idx = 0;
        factory_show_backlight();
        break;
      case FACTORY_T_BUZZER:
        factory_show_buzzer();
        factory_run_buzzer();
        break;
      case FACTORY_T_ACCEL:
        factory_show_accel();
        break;
      case FACTORY_T_GNSS:
        factory_show_gnss();
        break;
      case FACTORY_T_MAG:
        factory_show_mag();
        break;
      case FACTORY_T_BARO:
        factory_show_baro();
        break;
      case FACTORY_T_GYRO:
        factory_show_gyro();
        break;
      case FACTORY_T_BATTERY:
        factory_show_battery();
        break;
      case FACTORY_T_BLE:
        factory_show_ble();
        break;
      case FACTORY_T_STORAGE:
        factory_show_storage();
        break;
      case FACTORY_T_SUMMARY:
        factory_show_summary();
        break;
      default:
        break;
    }
}

bool factory_board_test_on_summary(void)
{
  return g_ft.active && g_ft.step >= FACTORY_T_SUMMARY;
}
