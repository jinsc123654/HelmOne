/**
 * @file myvendor_sys.c
 * @brief 整机只读快照：转发 board_sensor / eta9184 / BLE / USB，不触发 ADC 或 I2C。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/arch.h>    /* up_check_tcbstack（染色高水位，与 ps 同口径） */
#include <nuttx/sched.h>   /* nxsched_get_tcb / nxsched_put_tcb */

#include "myvendor_sys.h"

#include <nuttx/config.h>

#include "eta9184.h"
#include "myvendor_bat_soc.h"
#include "myvendor_board_sensor.h"
#include "myvendor_devctl.h"
#include "myvendor_mono.h"
#include "myvendor_usbdev.h"

#if MYVENDOR_SYS_BAT_MV_EMPTY != MYVENDOR_BAT_SOC_EMPTY_MV
#  error MYVENDOR_SYS_BAT_MV_EMPTY must match MYVENDOR_BAT_SOC_EMPTY_MV
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
#  include "companion_bridge.h"
#endif

#include "companion_proto.h"
#include "myvendor_gnss.h"
#include "myvendor_identity.h"
#include "sf32lb_dvfs.h"

#include "bf0_hal.h"
#include "drv_io.h"

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
#  include "myvendor_mtp.h"
#endif

/**
 * @brief ETA9184 电源枚举 → MYVENDOR_SYS_PWR_*。
 */
static uint8_t sys_map_power(enum eta9184_power_e st)
{
  switch (st)
    {
      case ETA9184_POWER_CHARGING:
        return MYVENDOR_SYS_PWR_CHARGING;
      case ETA9184_POWER_DISCHARGING:
        return MYVENDOR_SYS_PWR_DISCHARGING;
      case ETA9184_POWER_LOW:
        return MYVENDOR_SYS_PWR_LOW;
      default:
        return MYVENDOR_SYS_PWR_IDLE;
    }
}

/**
 * @brief 电源状态短名。
 */
static const char *sys_power_name(uint8_t state)
{
  switch (state)
    {
      case MYVENDOR_SYS_PWR_CHARGING:
        return "charging";
      case MYVENDOR_SYS_PWR_DISCHARGING:
        return "discharging";
      case MYVENDOR_SYS_PWR_LOW:
        return "low";
      default:
        return "idle";
    }
}

/**
 * @brief MTP plug 短名。
 */
static const char *sys_plug_name(uint8_t plug)
{
  switch (plug)
    {
      case MYVENDOR_SYS_USB_PLUG_ENUM:
        return "enum";
      case MYVENDOR_SYS_USB_PLUG_ACTIVE:
        return "active";
      default:
        return "off";
    }
}

int myvendor_sys_battery_mv(void)
{
  return myvendor_board_sensor_bat_mv();
}

int myvendor_sys_battery_percent(void)
{
  return myvendor_board_sensor_bat_pct();
}

int myvendor_sys_battery_percent_cached(void)
{
  return myvendor_sys_battery_percent();
}

uint8_t myvendor_sys_power_state(void)
{
  return sys_map_power(eta9184_power_state());
}

void myvendor_sys_sensor_get(myvendor_sys_sensor_t *out)
{
  if (out == NULL)
    {
      return;
    }

  memset(out, 0, sizeof(*out));

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  {
    struct companion_sensor_telem telem;

    companion_bridge_sensor_get(&telem);
    out->hr_valid = telem.hr_valid;
    out->cadence_valid = telem.cadence_valid;
    out->power_valid = telem.power_valid;
    out->hr_bpm = telem.hr_bpm;
    out->cadence_rpm = telem.cadence_rpm;
    out->power_w = telem.power_w;
  }
#endif
}

/**
 * @brief 同上，但**不阻塞**（UI 线程用；锁忙则返回 false 且不改 @p out）。
 *
 * @details
 * 主骑行页的每拍同步会走这条路：BLE 侧（companion / FS 写盘）持着 bridge 锁时，
 * UI 线程绝不能陪着等 —— 那会把 LVGL、静止覆盖层心跳、按键和拿起唤醒一起冻住。
 */
bool myvendor_sys_sensor_get_try(myvendor_sys_sensor_t *out)
{
  if (out == NULL)
    {
      return false;
    }

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  {
    struct companion_sensor_telem telem;

    if (!companion_bridge_sensor_get_try(&telem))
      {
        return false;
      }

    memset(out, 0, sizeof(*out));
    out->hr_valid = telem.hr_valid;
    out->cadence_valid = telem.cadence_valid;
    out->power_valid = telem.power_valid;
    out->hr_bpm = telem.hr_bpm;
    out->cadence_rpm = telem.cadence_rpm;
    out->power_w = telem.power_w;
    return true;
  }
#else
  memset(out, 0, sizeof(*out));
  return true;
#endif
}

bool myvendor_sys_phone_ble_connected(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  return companion_bridge_phone_get();
#else
  return false;
#endif
}

void myvendor_sys_sensor_ui_get(myvendor_sys_sensor_ui_t *out)
{
  if (out == NULL)
    {
      return;
    }

  memset(out, 0, sizeof(*out));
  {
    uint8_t z;

    for (z = 0; z < MYVENDOR_SYS_SENSOR_KIND_N; z++)
      {
        out->slot[z].bat_pct = -1;
      }
  }

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  {
    struct companion_sensor_ui ui;
    uint8_t i;

    companion_bridge_sensor_ui_get(&ui);
    out->scanning = ui.scanning;
    out->found_n = ui.found_n;
    if (out->found_n > MYVENDOR_SYS_SENSOR_FOUND_MAX)
      {
        out->found_n = MYVENDOR_SYS_SENSOR_FOUND_MAX;
      }

    for (i = 0; i < out->found_n; i++)
      {
        out->found[i].table_idx = ui.found[i].table_idx;
        out->found[i].kind_mask = ui.found[i].kind_mask;
        out->found[i].rssi = ui.found[i].rssi;
        out->found[i].linked = ui.found[i].linked;
        out->found[i].addr_type = ui.found[i].addr_type;
        memcpy(out->found[i].addr, ui.found[i].addr,
               MYVENDOR_SYS_SENSOR_ADDR_LEN);
        memcpy(out->found[i].name, ui.found[i].name,
               MYVENDOR_SYS_SENSOR_NAME_MAX);
        out->found[i].name[MYVENDOR_SYS_SENSOR_NAME_MAX - 1u] = '\0';
      }

    for (i = 0; i < MYVENDOR_SYS_SENSOR_KIND_N; i++)
      {
        out->slot[i].link = ui.slot[i].link;
        out->slot[i].bat_pct = ui.slot[i].bat_pct;
        out->slot[i].addr_type = ui.slot[i].addr_type;
        memcpy(out->slot[i].addr, ui.slot[i].addr, MYVENDOR_SYS_SENSOR_ADDR_LEN);
        memcpy(out->slot[i].name, ui.slot[i].name,
               MYVENDOR_SYS_SENSOR_NAME_MAX);
        out->slot[i].name[MYVENDOR_SYS_SENSOR_NAME_MAX - 1u] = '\0';
      }
  }
#endif
}

bool myvendor_sys_sensor_bound(uint8_t kind)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  return companion_bridge_sensor_slot_bound(kind);
#else
  (void)kind;
  return false;
#endif
}

bool myvendor_sys_phone_gnss_get(myvendor_sys_gnss_t *out)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  struct companion_gnss_fix fix;

  if (!companion_bridge_gnss_get(&fix))
    {
      if (out != NULL)
        {
          memset(out, 0, sizeof(*out));
        }

      return false;
    }

  if (out != NULL)
    {
      /* 先整份清零再逐字段填。手机协议（0xFF14）里没有 PVT 那几项
       * （gSpeed / sAcc / fixType），它们必须明确为 0/false —— 调用方
       * （bicycle_runtime 的路由）用的是**未清零的栈上结构**，漏掉就会把栈垃圾
       * 当成 pvt_valid / sAcc 去用（PVT 走得通之后才有的坑）。 */
      memset(out, 0, sizeof(*out));
      out->valid = fix.fix_quality != COMPANION_FIX_NONE;
      out->alive = true;
      out->lat_e7 = fix.lat_e7;
      out->lon_e7 = fix.lon_e7;
      out->alt_mm = fix.alt_mm;
      out->speed_centi_kmh = fix.speed_centi_kmh;
      out->course_deg = fix.course_deg;
      out->satellites = fix.satellites;
      out->sats_heard = fix.satellites;
      out->sats_in_view = fix.satellites;
      out->fix_quality = fix.fix_quality;
      out->hdop_x10 = fix.hdop_x10;
      out->utc_sec = fix.utc_sec;
      /* 手机协议（0xFF14，24 B）只带一个总星数，没有 GSV 细分，所以上面三个
       * 星数字段同值；时间戳取 bridge 记录的接收时刻，让消费方能算年龄。 */
      out->stamp_ms = companion_bridge_gnss_stamp_ms();
    }

  return true;
#else
  if (out != NULL)
    {
      memset(out, 0, sizeof(*out));
    }

  return false;
#endif
}

bool myvendor_sys_onboard_gnss_get(myvendor_sys_gnss_t *out)
{
  return myvendor_gnss_get(out);
}

bool myvendor_sys_gnss_eph_busy(void)
{
  myvendor_sys_gnss_t g;

  /* 「星历同步」不能比读线程活得久。
   *
   * `g_eph_busy` 在注入开始时置位、注入**结束**才清。读线程在注入中途卡死时
   * （现场第 4 次：`phase=read-poll`、`busy=1`，diag 每 30 s 重试回收都失败）
   * 没人清它 —— 而 UI 判「星历同步」的那一支**排在最前面**，会直接盖掉后面
   * 「无数据 / GNSS 失效」的分支。结果屏幕一直显示"星历同步"，而 GNSS 其实
   * 已经死了、那个状态永远不会变。这与 BLE 侧那个"粘滞 connected"是同一类毛病：
   * 状态标志没有时限、不从事实重新推导。
   *
   * 这里按**读线程健康**裁掉：只有 HL_OK 才承认"正在同步星历"。判据就在 UI 拿到
   * 的那份快照里（`rd_state`，见 myvendor_sys_gnss_health_e），所以这一层不需要
   * 额外时基，也不需要读线程配合。 */
  if (!myvendor_sys_onboard_gnss_get(&g) ||
      g.rd_state != MYVENDOR_SYS_GNSS_HL_OK)
    {
      return false;
    }

  return myvendor_gnss_eph_busy();
}

bool myvendor_sys_nav_take(int32_t *lat_e7, int32_t *lon_e7, uint8_t *count)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  return companion_bridge_nav_take(lat_e7, lon_e7, count);
#else
  (void)lat_e7;
  (void)lon_e7;
  if (count != NULL)
    {
      *count = 0;
    }

  return false;
#endif
}

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
static void sys_copy_notif(myvendor_sys_notif_t *dst,
                           const struct companion_notif_item *src)
{
  memset(dst, 0, sizeof(*dst));
  dst->type = src->type;
  memcpy(dst->title, src->title, MYVENDOR_SYS_NOTIF_TITLE_MAX);
  dst->title[MYVENDOR_SYS_NOTIF_TITLE_MAX] = '\0';
  memcpy(dst->body, src->body, MYVENDOR_SYS_NOTIF_BODY_MAX);
  dst->body[MYVENDOR_SYS_NOTIF_BODY_MAX] = '\0';
  memcpy(dst->icon, src->icon, MYVENDOR_SYS_NOTIF_ICON_MAX - 1u);
  dst->icon[MYVENDOR_SYS_NOTIF_ICON_MAX - 1u] = '\0';
}
#endif

bool myvendor_sys_notif_take(myvendor_sys_notif_t *out)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  struct companion_notif_item item;

  if (out == NULL)
    {
      return false;
    }

  if (!companion_bridge_notif_take(&item))
    {
      memset(out, 0, sizeof(*out));
      return false;
    }

  sys_copy_notif(out, &item);
  return true;
#else
  if (out != NULL)
    {
      memset(out, 0, sizeof(*out));
    }

  return false;
#endif
}

void myvendor_sys_inbox_get(myvendor_sys_notif_t *out, uint8_t *n, uint8_t maxn)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  struct companion_notif_item box[COMPANION_NOTIF_INBOX_MAX];
  uint8_t nn = 0;
  uint8_t i;

  companion_bridge_inbox_get(out == NULL ? NULL : box, &nn,
                             COMPANION_NOTIF_INBOX_MAX);
  if (n != NULL)
    {
      *n = nn;
    }

  if (out == NULL || maxn == 0)
    {
      return;
    }

  if (nn > maxn)
    {
      nn = maxn;
    }

  if (n != NULL)
    {
      *n = nn;
    }

  for (i = 0; i < nn; i++)
    {
      sys_copy_notif(&out[i], &box[i]);
    }
#else
  if (n != NULL)
    {
      *n = 0;
    }

  if (out != NULL && maxn > 0)
    {
      memset(out, 0, sizeof(*out));
    }
#endif
}

void myvendor_sys_accel_get(myvendor_sys_vec3_t *out)
{
  if (out == NULL)
    {
      return;
    }

#if defined(CONFIG_BOARD_BMI270)
  myvendor_board_sensor_accel_get(out);
#else
  memset(out, 0, sizeof(*out));
#endif
}

void myvendor_sys_gyro_get(myvendor_sys_vec3_t *out)
{
  if (out == NULL)
    {
      return;
    }

#if defined(CONFIG_BOARD_BMI270)
  myvendor_board_sensor_gyro_get(out);
#else
  memset(out, 0, sizeof(*out));
#endif
}

void myvendor_sys_baro_get(myvendor_sys_baro_t *out)
{
  if (out == NULL)
    {
      return;
    }

#if defined(CONFIG_BOARD_BMP388)
  myvendor_board_sensor_baro_get(out);
#else
  memset(out, 0, sizeof(*out));
#endif
}

void myvendor_sys_mag_get(myvendor_sys_mag_t *out)
{
  if (out == NULL)
    {
      return;
    }

#if defined(CONFIG_BOARD_MMC5983MA)
  myvendor_board_sensor_mag_get(out);
#else
  memset(out, 0, sizeof(*out));
#endif
}

void myvendor_sys_snapshot(myvendor_sys_snapshot_t *out)
{
  if (out == NULL)
    {
      return;
    }

  memset(out, 0, sizeof(*out));

  out->power.mv = myvendor_sys_battery_mv();
  out->power.pct = myvendor_sys_battery_percent();
  out->power.full_mv = myvendor_devctl_bat_full_mv_get();
  out->power.full_cal = myvendor_devctl_bat_full_have();
  out->power.state = sys_map_power(eta9184_power_state());
  out->power.boost = eta9184_boost_get();

  (void)myvendor_sys_phone_gnss_get(&out->phone);
  (void)myvendor_sys_onboard_gnss_get(&out->onboard);
  myvendor_sys_sensor_get(&out->sensor);

#if defined(CONFIG_BOARD_BMI270) || defined(CONFIG_BOARD_BMP388) || \
    defined(CONFIG_BOARD_MMC5983MA)
  myvendor_sys_accel_get(&out->accel);
  myvendor_sys_gyro_get(&out->gyro);
  myvendor_sys_baro_get(&out->baro);
  myvendor_sys_mag_get(&out->mag);
#endif

  out->usb.controller_on = myvendor_usbdev_get();
#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  {
    myvendor_mtp_status_t st;

    if (myvendor_mtp_get_status(&st) == 0)
      {
        out->usb.worker_running = st.worker_running;
        if (st.plug == MYVENDOR_MTP_PLUG_ACTIVE)
          {
            out->usb.plug = MYVENDOR_SYS_USB_PLUG_ACTIVE;
          }
        else if (st.plug == MYVENDOR_MTP_PLUG_ENUM)
          {
            out->usb.plug = MYVENDOR_SYS_USB_PLUG_ENUM;
          }
      }
  }
#endif

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  out->companion_alive = companion_bridge_alive_get();
#endif

  out->policy_radio = myvendor_devctl_radio_get();
  out->policy_sensor = myvendor_devctl_sensor_get();
  out->policy_mtp = myvendor_devctl_mtp_get();
  out->policy_notif = myvendor_devctl_notif_get();
  out->policy_calls = myvendor_devctl_notif_calls_only_get();
  out->policy_sound = myvendor_devctl_sound_get();
  out->policy_eph_auto = myvendor_devctl_eph_auto_get();
  out->bl_pct = myvendor_devctl_bl_get();
  out->bl_applied = BSP_LCD_BL_GetPct();
  out->bl_max = BSP_LCD_BL_GetMaxPct();
}

/**
 * @brief 有符号定点数，不用 %f（NSH 小栈上 dtoa 会打穿）。
 *
 * @param v      值。
 * @param scale  10=一位小数，100=两位。
 */
static void sys_print_fixed(float v, unsigned scale)
{
  int neg;
  int scaled;
  unsigned mag;
  unsigned frac;

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
      putchar('-');
    }

  if (scale == 100u)
    {
      printf("%u.%02u", mag, frac);
    }
  else
    {
      printf("%u.%u", mag, frac);
    }
}

/**
 * @brief 打印一份 GNSS 快照（板载或手机）的年龄。
 *
 * @details
 * stamp_ms 是本份数据的采样时刻。0 表示来源没给时间戳（未知），此时打 "--"
 * 而不是打一个看起来很小的假年龄。消费方必须能区分"刚采到"和"恢复期里
 * 被 hold 住的旧值"，否则 sat/age 一起看会得出错误结论。
 *
 * @param g 快照。
 */
static void sys_print_gnss_age(const myvendor_sys_gnss_t *g)
{
  if (g->stamp_ms == 0)
    {
      printf("  age --");
      return;
    }

  printf("  age %u.%03u s",
         (unsigned)(myvendor_mono_elapsed_ms(myvendor_mono_ms(),
                                            g->stamp_ms) / 1000u),
         (unsigned)(myvendor_mono_elapsed_ms(myvendor_mono_ms(),
                                            g->stamp_ms) % 1000u));
}

void myvendor_sys_gnss_print_pvt(const myvendor_sys_gnss_t *g)
{
  const char *fix;

  if (g == NULL || !g->pvt_valid)
    {
      return;
    }

  /* fixType 打成 2D/3D 字样，**不要打数字**：同一行里 `q %u` 是 GGA/GSA 的
   * 定位质量（0 无 / 1 2D / 2 3D），PVT 的 fixType 是另一套编号
   * （0 无 / 2 2D / 3 3D）—— 两个 "2" 含义相反，数字并排会被读成一致。
   * 现场第一次看就撞上了：`q 2`（3D）配 `fix 2`（2D），实际是两边不一致。 */
  switch (g->pvt_fix_type)
    {
      case 2:  fix = "2D"; break;
      case 3:  fix = "3D"; break;
      case 4:  fix = "DR"; break;
      case 5:  fix = "time"; break;
      default: fix = "none"; break;
    }

  /* 单位：gs 与 speed_centi_kmh 同为 0.01 km/h；sacc 是 mm/s，这里打成 m/s
   * （两位小数），与 `gnss: pvt` 那行的 mm/s 是同一条数据。 */
  printf("  gs %u.%02u km/h  sacc %u.%02u m/s  pvt %s ok %u",
         (unsigned)(g->speed_pvt_centi_kmh / 100u),
         (unsigned)(g->speed_pvt_centi_kmh % 100u),
         (unsigned)(g->speed_acc_mm_s / 1000u),
         (unsigned)((g->speed_acc_mm_s % 1000u) / 10u),
         fix,
         g->pvt_gnss_ok ? 1u : 0u);
}

static void sys_print_gnss(const char *name, const myvendor_sys_gnss_t *g)
{
  if (g->valid)
    {
      /* lock=解算用星，heard=SNR>0，view=GSV 累加值（不是颗数）。 */
      printf("  %-7s %u.%02u km/h  lock %u heard %u view %u  q %u  hdop %u.%u  utc %u",
             name,
             (unsigned)(g->speed_centi_kmh / 100u),
             (unsigned)(g->speed_centi_kmh % 100u),
             (unsigned)g->satellites,
             (unsigned)g->sats_heard,
             (unsigned)g->sats_in_view,
             (unsigned)g->fix_quality,
             (unsigned)(g->hdop_x10 / 10u),
             (unsigned)(g->hdop_x10 % 10u),
             (unsigned)g->utc_sec);
      myvendor_sys_gnss_print_pvt(g);
      sys_print_gnss_age(g);
      putchar('\n');
    }
  else if (g->alive)
    {
      printf("  %-7s search  lock %u heard %u view %u  hdop %u.%u  utc %u",
             name,
             (unsigned)g->satellites,
             (unsigned)g->sats_heard,
             (unsigned)g->sats_in_view,
             (unsigned)(g->hdop_x10 / 10u),
             (unsigned)(g->hdop_x10 % 10u),
             (unsigned)g->utc_sec);
      myvendor_sys_gnss_print_pvt(g);
      sys_print_gnss_age(g);
      putchar('\n');
    }
  else
    {
      printf("  %-7s --\n", name);
    }
}

/** @brief 时钟源名称，与 bsp_init.c 的打印口径一致。 */
const char *myvendor_sys_clk_src_name(uint8_t src)
{
  switch (src)
    {
      case RCC_CLK_FLASH_SYSCLK:
        return "SYS";
      case RCC_CLK_SRC_DLL1:
        return "DLL1";
      case RCC_CLK_SRC_DLL2:
        return "DLL2";
      case RCC_CLK_SRC_DLL3:
        return "DLL3";
      default:
        return "?";
    }
}

/** @brief 时钟源频率；未使能的 DLL 由 HAL 返回 0。 */
static uint32_t sys_clk_src_hz(uint8_t src)
{
  if (src == RCC_CLK_SRC_DLL2)
    {
      return HAL_RCC_HCPU_GetDLL2Freq();
    }

  if (src == RCC_CLK_SRC_DLL1)
    {
      return HAL_RCC_HCPU_GetDLL1Freq();
    }

  return HAL_RCC_GetSysCLKFreq(CORE_ID_HCPU);
}

/**
 * @brief 推一个 MPI/FLASH 模块的实际时钟。
 *
 * @details
 * hz = 时钟源 / MPI PSCLR.DIV。PSCLR 是 MPI 的直接整数分频
 * （HAL_FLASH_SET_CLK_rom 就是 `PSCLR = div`），而软件分频变量
 * BSP_GetFlashxDIV() 只在 HAL_MPI_PSRAM_Init 时被写进硬件、之后可能过期，
 * 所以这里以硬件为准，软件值只带出来对照。
 *
 * @param clk_module RCC_CLK_MOD_FLASH1 / FLASH2。
 * @param mpi 该模块的 MPI 实例（hwp_qspi1 / hwp_qspi2）。
 * @param sw_div BSP_GetFlashxDIV()，仅用于对照。
 * @param hz 输出频率。
 * @param src 输出时钟源。
 * @param psclr 输出硬件分频。
 * @param sw 输出软件分频。
 */
static void sys_clk_probe_flash(int clk_module, MPI_TypeDef *mpi,
                                uint16_t sw_div, uint32_t *hz, uint8_t *src,
                                uint8_t *psclr, uint8_t *sw)
{
  uint32_t base;

  *src = (uint8_t)HAL_RCC_HCPU_GetClockSrc(clk_module);
  *psclr = (mpi != NULL)
               ? (uint8_t)GET_REG_VAL(mpi->PSCLR, MPI_PSCLR_DIV_Msk,
                                      MPI_PSCLR_DIV_Pos)
               : 0u;
  *sw = (uint8_t)(sw_div > 255u ? 255u : sw_div);

  base = sys_clk_src_hz(*src);
  *hz = base / ((*psclr != 0u) ? *psclr : 1u);
}

void myvendor_sys_clk_get(myvendor_sys_clk_t *out)
{
  if (out == NULL)
    {
      return;
    }

  memset(out, 0, sizeof(*out));

  out->sysclk_hz = HAL_RCC_GetSysCLKFreq(CORE_ID_HCPU);
  out->hclk_hz = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);

  /* DLL 只如实上报使能位与频率，不碰任何配置。 */
  if ((hwp_hpsys_rcc->DLL1CR & HPSYS_RCC_DLL1CR_EN) != 0u)
    {
      out->dll1_en = 1u;
      out->dll1_hz = HAL_RCC_HCPU_GetDLL1Freq();
    }

  if ((hwp_hpsys_rcc->DLL2CR & HPSYS_RCC_DLL2CR_EN) != 0u)
    {
      out->dll2_en = 1u;
      out->dll2_hz = HAL_RCC_HCPU_GetDLL2Freq();
    }

  sys_clk_probe_flash(RCC_CLK_MOD_FLASH1, hwp_qspi1, BSP_GetFlash1DIV(),
                      &out->flash1_hz, &out->flash1_src, &out->flash1_psclr,
                      &out->flash1_sw_div);
  sys_clk_probe_flash(RCC_CLK_MOD_FLASH2, hwp_qspi2, BSP_GetFlash2DIV(),
                      &out->flash2_hz, &out->flash2_src, &out->flash2_psclr,
                      &out->flash2_sw_div);

  out->mismatch = (out->flash1_psclr != out->flash1_sw_div ||
                   out->flash2_psclr != out->flash2_sw_div);
}

/** @brief 汇总行：一行看清 SYSCLK/HCLK/PSRAM/NAND 的实际频率。 */
static void sys_print_clk(void)
{
  myvendor_sys_clk_t c;

  myvendor_sys_clk_get(&c);
  /* PSRAM 报**时钟**口径（= 数据率/2）：厂商文档表与 HAL 的延迟分档表
   * （`HAL_QSPI_GET_CLK()/2` 之后比 66/109/133/166/200）用的都是这个数 ——
   * 官方配置 DLL2 288 / 分频 1 就是"144 MHz"。`ddr` 那栏才是 `HAL_QSPI_GET_CLK`。 */
  printf("  clk     sys %u MHz  hclk %u MHz  psram %u MHz clock (%s/%u, ddr %u)  nand %u MHz (%s/%u)\n",
         (unsigned)(c.sysclk_hz / 1000000u), (unsigned)(c.hclk_hz / 1000000u),
         (unsigned)(c.flash1_hz / 2000000u),
         myvendor_sys_clk_src_name(c.flash1_src), (unsigned)c.flash1_psclr,
         (unsigned)(c.flash1_hz / 1000000u),
         (unsigned)(c.flash2_hz / 1000000u),
         myvendor_sys_clk_src_name(c.flash2_src), (unsigned)c.flash2_psclr);

  if (c.mismatch)
    {
      /* 软件分频变量与硬件 PSCLR 不一致时明确点出来：按 sw_div 算出来的频率
       * 是错的，以上一行的 psclr 为准。 */
      printf("  clk     MISMATCH sw_div psram=%u nand=%u  vs hw psclr psram=%u nand=%u\n",
             (unsigned)c.flash1_sw_div, (unsigned)c.flash2_sw_div,
             (unsigned)c.flash1_psclr, (unsigned)c.flash2_psclr);
    }

  /* PSRAM 的读写命令延迟（`HAL_MPI_MODIFY_RCMD/WCMD_DELAY` 写的就是这两个
   * `DCYC` 字段，值 = 延迟档 − 1）。这张档位表是**开机算一次**的，而总线频率
   * 会随档位走 ⇒ 把实际值打出来，才能判断"延迟码和当前频率还配不配"。
   * 参照：`HAL_MPI_PSRAM_Init` 里 sys_clk(=总线/2) ≤66→3 / ≤109→4 /
   * ≤133→5 / ≤166→6 / ≤200→7（该表没有 else，>200 会落空）。 */
  printf("  clk     psram lat rcmd=%u wcmd=%u (code = 该值+1)\n",
         (unsigned)GET_REG_VAL(hwp_qspi1->HRCCR, MPI_HRCCR_DCYC_Msk,
                               MPI_HRCCR_DCYC_Pos),
         (unsigned)GET_REG_VAL(hwp_qspi1->HWCCR, MPI_HWCCR_DCYC_Msk,
                               MPI_HWCCR_DCYC_Pos));
}

/** @brief 堆栈摘要：最紧的三个 + 超过 80% 的个数。
 *
 *  完整表格在 `ps` 里；`sys` 要的是"一眼看出有没有快溢出的任务"，所以只报
 *  最紧的三个。占用率用 `up_check_tcbstack()`（染色高水位）算，与 `ps` 的
 *  FILLED 同一口径。
 */
static void sys_print_stacks(void)
{
  struct row
  {
    char     name[16];
    int      pid;
    unsigned used;
    unsigned size;
    unsigned pct;
  };

  struct row top[3];
  unsigned over80 = 0;
  unsigned n = 0;
  unsigned i;
  int pid;

  /* CONFIG_MAX_TASKS 未必定义（这个文件是 NSH app 目标，配置宏不一定齐），
   * 给个上界即可 —— pid 是稀疏的小整数，扫一遍花费可忽略。 */
#ifndef CONFIG_MAX_TASKS
#  define SYS_TASK_SCAN_MAX 128
#else
#  define SYS_TASK_SCAN_MAX CONFIG_MAX_TASKS
#endif

  memset(top, 0, sizeof(top));

  for (pid = 0; pid < SYS_TASK_SCAN_MAX; pid++)
    {
      FAR struct tcb_s *tcb = nxsched_get_tcb((pid_t)pid);
      unsigned used;
      unsigned size;
      unsigned pct;

      if (tcb == NULL)
        {
          continue;
        }

      size = (unsigned)tcb->adj_stack_size;
#ifdef CONFIG_STACK_COLORATION
      used = (size > 0u) ? (unsigned)up_check_tcbstack(tcb, tcb->adj_stack_size)
                         : 0u;
#else
      used = 0u;
#endif
      pct = (size > 0u) ? (unsigned)((100u * used) / size) : 0u;
      if (pct >= 80u)
        {
          over80++;
        }

      for (i = 0; i < 3u; i++)
        {
          if (pct > top[i].pct)
            {
              unsigned j;

              for (j = 2u; j > i; j--)
                {
                  top[j] = top[j - 1u];
                }

              /* 名字要抄出来：put_tcb 之后 TCB 可能已经没了。 */
              strncpy(top[i].name, tcb->name, sizeof(top[i].name) - 1u);
              top[i].name[sizeof(top[i].name) - 1u] = '\0';
              top[i].pid  = (int)tcb->pid;
              top[i].used = used;
              top[i].size = size;
              top[i].pct  = pct;
              break;
            }
        }

      n++;
      nxsched_put_tcb(tcb);
    }

  if (n == 0u)
    {
      return;
    }

  printf("  stk     %u tasks, %u over 80%%:\n", n, over80);
  for (i = 0; i < 3u && top[i].size != 0u; i++)
    {
      printf("  stk     %-14s %5u/%5u B  %3u%%\n", top[i].name,
             top[i].used, top[i].size, top[i].pct);
    }
}

/** @brief COMPANION_SLOT_* → 短名，供 `sys` 一眼认槽。 */
static const char *sys_slot_name(uint8_t slot)
{
  switch (slot)
    {
      case COMPANION_SLOT_FW:
        return "fw";
      case COMPANION_SLOT_MAIN:
        return "main";
      case COMPANION_SLOT_FACTORY:
        return "factory";
      default:
        return "--";
    }
}

void myvendor_sys_dump(void)
{
  myvendor_sys_snapshot_t s;

  myvendor_sys_snapshot(&s);

  printf("sys  (read)\n");
  /* 板卡身份放最前：翻串口/截图时先确认「烧的是哪一版、从哪槽起来」。
   * 全部走 myvendor_identity（与关于页 / MTP / BLE 同源），不另开配置。 */
  {
    const char *id  = myvendor_identity_name();
    const char *sn  = myvendor_identity_serial();
    const char *sw  = myvendor_sw_version();
    const char *hw  = myvendor_hw_version();
    const char *bld = myvendor_build_date();
    const char *bl  = myvendor_boot_version();
    const char *fw  = myvendor_boot_fw_name();
    uint8_t slot = myvendor_boot_slot();
    uint8_t tgt  = myvendor_boot_target();

    printf("  board   %s  sn %s\n",
           (id != NULL && id[0] != '\0') ? id : "--",
           (sn != NULL && sn[0] != '\0') ? sn : "--");
    printf("  fw      %s  hw %s  build %s\n",
           (sw != NULL && sw[0] != '\0') ? sw : "--",
           (hw != NULL && hw[0] != '\0') ? hw : "--",
           (bld != NULL && bld[0] != '\0') ? bld : "--");
    printf("  slot    %s", sys_slot_name(slot));
    if (slot == COMPANION_SLOT_FW && fw != NULL && fw[0] != '\0')
      {
        printf(" (%s)", fw);
      }

    printf("  target %s  bl %s%s\n",
           sys_slot_name(tgt),
           (bl != NULL && bl[0] != '\0') ? bl : "--",
           myvendor_is_factory() ? "  factory-image" : "");
  }

  /* 运行时间。日志每行前缀（`[  108.321770]`）本来就是它，但事后翻日志时手上
   * 往往没有那一行 —— `sys` 里问一句最省事。
   * `boot` 只有 RTC 对过时才有意义（对时前 `time(NULL)` 不可信），给不出来
   * 就显示 `--`；那边界与全工程一致（GNSS_TIME_MIN_UNIX = 2024-01-01）。 */
  {
    struct timespec ts;
    long long up_s = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
      {
        up_s = (long long)ts.tv_sec;
      }

    {
      unsigned hh = (unsigned)(up_s / 3600);
      unsigned mm = (unsigned)((up_s / 60) % 60);
      unsigned ss = (unsigned)(up_s % 60);
      time_t now = time(NULL);

      if (now >= (time_t)1704067200)
        {
          time_t boot = (time_t)((long long)now - up_s);
          struct tm tmv;

          if (gmtime_r(&boot, &tmv) != NULL)
            {
              printf("  up      %u h %02u m %02u s"
                     "   boot %04d-%02d-%02d %02d:%02d:%02d Z\n",
                     hh, mm, ss,
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            }
          else
            {
              printf("  up      %u h %02u m %02u s   boot %ld (utc)\n",
                     hh, mm, ss, (long)boot);
            }
        }
      else
        {
          printf("  up      %u h %02u m %02u s   boot -- (no RTC yet)\n",
                 hh, mm, ss);
        }
    }
  }

  /* 当前时间。local 用 gmtime_r(now + tz) 算 —— 跨日/跨月由它处理，不手算。 */
  {
    time_t now = time(NULL);

    if (now >= (time_t)1704067200)
      {
        int16_t tzm = myvendor_devctl_tz_min_get();
        time_t local = now + (time_t)tzm * 60;
        struct tm utm;
        struct tm ltm;

        if (gmtime_r(&now, &utm) != NULL && gmtime_r(&local, &ltm) != NULL)
          {
            printf("  now     %04d-%02d-%02d %02d:%02d:%02d local (UTC%+d)"
                   "   utc %02d:%02d:%02d\n",
                   ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday,
                   ltm.tm_hour, ltm.tm_min, ltm.tm_sec,
                   (int)(tzm / 60),
                   utm.tm_hour, utm.tm_min, utm.tm_sec);
          }
      }
    else
      {
        printf("  now     -- (no RTC yet)\n");
      }
  }

  sys_print_stacks();

  if (s.power.pct < 0)
    {
      printf("  bat     --  (%d)  cal %d mV%s  %s  boost %s\n",
             s.power.mv,
             s.power.full_mv,
             s.power.full_cal ? "" : " (default)",
             sys_power_name(s.power.state),
             s.power.boost ? "on" : "off");
    }
  else
    {
      printf("  bat     %d%%  %d mV  cal %d mV%s  %s  boost %s\n",
             s.power.pct,
             s.power.mv,
             s.power.full_mv,
             s.power.full_cal ? "" : " (default)",
             sys_power_name(s.power.state),
             s.power.boost ? "on" : "off");
    }

  printf("  bl      %u%% applied (policy %u, max %u)\n",
         (unsigned)s.bl_applied,
         (unsigned)s.bl_pct,
         (unsigned)s.bl_max);
  {
    int16_t tzm = myvendor_devctl_tz_min_get();
    int h = tzm / 60;

    printf("  tz      UTC%+d\n", h);
  }

  sys_print_gnss("phone", &s.phone);
  sys_print_gnss("onboard", &s.onboard);

  if (s.accel.valid)
    {
      printf("  accel   ");
      sys_print_fixed(s.accel.x, 100);
      putchar(' ');
      sys_print_fixed(s.accel.y, 100);
      putchar(' ');
      sys_print_fixed(s.accel.z, 100);
      printf(" m/s2\n");
    }
  else
    {
      printf("  accel   --\n");
    }

  if (s.gyro.valid)
    {
      printf("  gyro    ");
      sys_print_fixed(s.gyro.x, 100);
      putchar(' ');
      sys_print_fixed(s.gyro.y, 100);
      putchar(' ');
      sys_print_fixed(s.gyro.z, 100);
      printf(" rad/s\n");
    }
  else
    {
      printf("  gyro    --\n");
    }

  if (s.baro.valid)
    {
      printf("  baro    ");
      sys_print_fixed(s.baro.hpa, 10);
      printf(" hPa  ");
      sys_print_fixed(s.baro.temp_c, 10);
      printf(" C\n");
    }
  else
    {
      printf("  baro    --\n");
    }

  if (s.mag.valid)
    {
      printf("  mag     ");
      sys_print_fixed(s.mag.x, 10);
      putchar(' ');
      sys_print_fixed(s.mag.y, 10);
      putchar(' ');
      sys_print_fixed(s.mag.z, 10);
      printf(" uT\n");
    }
  else
    {
      printf("  mag     --\n");
    }

  if (!s.sensor.hr_valid && !s.sensor.cadence_valid && !s.sensor.power_valid)
    {
      printf("  hr      policy %s  --\n", s.policy_sensor ? "on" : "off");
    }
  else
    {
      printf("  hr      policy %s", s.policy_sensor ? "on" : "off");
      if (s.sensor.hr_valid)
        {
          printf(" H%u", (unsigned)s.sensor.hr_bpm);
        }

      if (s.sensor.cadence_valid)
        {
          printf(" C%u", (unsigned)s.sensor.cadence_rpm);
        }

      if (s.sensor.power_valid)
        {
          printf(" P%u", (unsigned)s.sensor.power_w);
        }

      printf("\n");
    }

  printf("  radio   policy %s  companion %s\n",
         s.policy_radio ? "on" : "off",
         s.companion_alive ? "alive" : "down");
  {
    uint8_t inbox_n = 0;

    myvendor_sys_inbox_get(NULL, &inbox_n, 0);
    printf("  notif   policy %s  calls-only %s  inbox %u\n",
           s.policy_notif ? "on" : "off",
           s.policy_calls ? "on" : "off",
           (unsigned)inbox_n);
  }
  printf("  sound   policy %s\n", s.policy_sound ? "on" : "off");
  printf("  eph     auto %s\n", s.policy_eph_auto ? "on" : "off");
  /* 已保存（开机回连）的传感器记录。`sys` 全量输出里也带上，
   * 免得为了查一条绑定再跑一次 `sys hr`。 */
  myvendor_devctl_sensor_rec_dump();
  sf32lb_dvfs_print();
  sys_print_clk();
  printf("  usb     policy %s  hw %s  worker %s  plug %s\n",
         s.policy_mtp ? "on" : "off",
         s.usb.controller_on ? "on" : "off",
         s.usb.worker_running ? "running" : "stopped",
         sys_plug_name(s.usb.plug));
  fflush(stdout);
}
