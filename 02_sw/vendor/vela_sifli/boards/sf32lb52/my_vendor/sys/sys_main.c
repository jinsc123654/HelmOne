/**
 * @file sys_main.c
 * @brief NSH `sys`：只读快照（不写 persist，不开关设备）。
 *
 *   sys                 全部
 *   sys bl              背光（策略 + PWM）
 *   sys bat             电量
 *   sys radio           手机广播策略 + companion
 *   sys hr              HR / 踏频 / 功率
 *   sys usb             USB 控制器 / worker
 *   sys notif           手机通知
 *   sys sound           蜂鸣器
 *   sys accel|gyro|baro|mag|phone|onboard
 *   sys dvfs            HCPU 72/96/144/240 MHz
 *
 * 写入走 `ctl`（ctl bl 40）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_sys.h"
#include "sf32lb_dvfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 本文件是 nuttx_add_application（目标 `sys`），include 路径只有
 * board/include，**芯片层头文件不在里面**，所以只能本地声明。
 * （对照：services/ 下的文件属于 board 目标，可以直接 `#include "sf32lb_sdio.h"`。）
 * 完整说明见 chips/sf32lb52/include/sf32lb_sdio.h 里 sf32lb_sd_clk_probe 的
 * Doxygen —— 只读打印 SD 时钟拓扑，用来确认 SD 挂在哪条时钟分支上、
 * 以及分频反推的 SDCLK 是否等于预期值。 */
extern void sf32lb_sd_clk_probe(void);

/* 同上的理由：`ctl/` 不在本 app 的 include 路径里，只能本地声明。 */
extern void myvendor_devctl_sensor_rec_dump(void);
extern void myvendor_devctl_sensor_link_dump(void);

/**
 * @brief 有符号定点数，不用 %f（sys 任务栈上 dtoa 会打穿 NSH）。
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

/** @brief 打印用法。 */
static void usage(void)
{
  printf("sys — read snapshot (no persist writes)\n");
  printf("  sys                 all (no IMU I2C)\n");
  printf("  sys bl              backlight policy + PWM\n");
  printf("  sys bat             battery / charge\n");
  printf("  sys radio           phone adv policy + companion\n");
  printf("  sys hr              BLE HR / cadence / power\n");
  printf("  sys usb             USB controller + MTP worker\n");
  printf("  sys accel|gyro      BMI270 (one sample)\n");
  printf("  sys baro            BMP388 (one sample)\n");
  printf("  sys mag             MMC5983 (one sample)\n");
  printf("  sys phone           phone GNSS\n");
  printf("  sys onboard         onboard MAX-M10S-00B-01 GNSS\n");
  printf("  sys notif           phone notify policy + inbox\n");
  printf("  sys sound           piezo UI beeps\n");
  printf("  sys eph             GNSS eph auto inject/dump\n");
  printf("  sys dvfs            HCPU 72/96/144/240 MHz\n");
  printf("  sys clk             SYSCLK/HCLK/DLL + PSRAM/NAND actual Hz\n");
  printf("Writes: ctl bl 40 / ctl radio off / ...\n");
}

/**
 * @brief 打印一份快照里的指定行。
 */
static int print_topic(const char *topic, const myvendor_sys_snapshot_t *s)
{
  if (strcmp(topic, "bl") == 0 || strcmp(topic, "backlight") == 0)
    {
      printf("sys bl  %u%% applied (policy %u, max %u)\n",
             (unsigned)s->bl_applied,
             (unsigned)s->bl_pct,
             (unsigned)s->bl_max);
      return 0;
    }

  if (strcmp(topic, "bat") == 0 || strcmp(topic, "battery") == 0)
    {
      const char *st;

      switch (s->power.state)
        {
          case MYVENDOR_SYS_PWR_CHARGING:
            st = "charging";
            break;
          case MYVENDOR_SYS_PWR_DISCHARGING:
            st = "discharging";
            break;
          case MYVENDOR_SYS_PWR_LOW:
            st = "low";
            break;
          default:
            st = "idle";
            break;
        }

      if (s->power.pct < 0)
        {
          printf("sys bat  --  (%d)  cal %d mV%s  %s  boost %s\n",
                 s->power.mv, s->power.full_mv,
                 s->power.full_cal ? "" : " (default)",
                 st, s->power.boost ? "on" : "off");
        }
      else
        {
          printf("sys bat  %d%%  %d mV  cal %d mV%s  %s  boost %s\n",
                 s->power.pct, s->power.mv, s->power.full_mv,
                 s->power.full_cal ? "" : " (default)",
                 st, s->power.boost ? "on" : "off");
        }

      return 0;
    }

  if (strcmp(topic, "radio") == 0 || strcmp(topic, "bt") == 0)
    {
      printf("sys radio  policy %s  companion %s\n",
             s->policy_radio ? "on" : "off",
             s->companion_alive ? "alive" : "down");
      return 0;
    }

  if (strcmp(topic, "hr") == 0 || strcmp(topic, "sensor") == 0)
    {
      /* BLE 传感器链路/扫描表：走 myvendor_sys_sensor_ui_get 这一份快照
       * （bridge 锁内的拷贝，由 sensor_ui_publish 从加锁的槽位表生成），
       * 不直接读 ble_sensor 的内部表。 */
      myvendor_sys_sensor_ui_t ui;
      uint8_t k;
      uint8_t i;

      printf("sys hr  policy %s", s->policy_sensor ? "on" : "off");
      if (s->sensor.hr_valid)
        {
          printf("  H%u", (unsigned)s->sensor.hr_bpm);
        }

      if (s->sensor.cadence_valid)
        {
          printf("  C%u", (unsigned)s->sensor.cadence_rpm);
        }

      if (s->sensor.power_valid)
        {
          printf("  P%u", (unsigned)s->sensor.power_w);
        }

      if (!s->sensor.hr_valid && !s->sensor.cadence_valid &&
          !s->sensor.power_valid)
        {
          printf("  --");
        }

      printf("\n");

      myvendor_sys_sensor_ui_get(&ui);

      for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++)
        {
          const char *ln = (ui.slot[k].link == MYVENDOR_SYS_SENSOR_LINK_READY)
                               ? "ready"
                           : (ui.slot[k].link ==
                              MYVENDOR_SYS_SENSOR_LINK_CONNECTING)
                               ? "conn"
                               : "idle";

          printf("sys slot %u  %s", (unsigned)k, ln);
          if (ui.slot[k].bat_pct >= 0)
            {
              printf("  bat %d%%", (int)ui.slot[k].bat_pct);
            }

          if (ui.slot[k].name[0] != '\0')
            {
              printf("  %s", ui.slot[k].name);
            }

          if (ui.slot[k].addr[0] != 0 || ui.slot[k].addr[1] != 0 ||
              ui.slot[k].addr[2] != 0 || ui.slot[k].addr[3] != 0 ||
              ui.slot[k].addr[4] != 0 || ui.slot[k].addr[5] != 0)
            {
              printf("  %02x:%02x:%02x:%02x:%02x:%02x",
                     ui.slot[k].addr[5], ui.slot[k].addr[4],
                     ui.slot[k].addr[3], ui.slot[k].addr[2],
                     ui.slot[k].addr[1], ui.slot[k].addr[0]);
            }

          printf("\n");
        }

      printf("sys scan  scanning=%d  found=%u\n", ui.scanning ? 1 : 0,
             (unsigned)ui.found_n);

      for (i = 0; i < ui.found_n; i++)
        {
          printf("sys found %u  rssi %d  kinds 0x%x%s  %02x:%02x:%02x:%02x:%02x:%02x  %s\n",
                 (unsigned)ui.found[i].table_idx, (int)ui.found[i].rssi,
                 (unsigned)ui.found[i].kind_mask,
                 ui.found[i].linked ? " (linked)" : "",
                 ui.found[i].addr[5], ui.found[i].addr[4], ui.found[i].addr[3],
                 ui.found[i].addr[2], ui.found[i].addr[1], ui.found[i].addr[0],
                 ui.found[i].name);
        }

      /* 已保存（开机回连）的记录。上面 slot 是**本次开机**的链路态，这里是
       * 持久化的绑定表 —— 判断「保存下来了没有」只看这一段。 */
      myvendor_devctl_sensor_rec_dump();

      /* 再对一次账：记录里的 autorc 目标 ↔ 槽位实际在等谁。回连只由前者
       * 驱动，所以两边对不上就是问题本身（见函数头注释）。 */
      myvendor_devctl_sensor_link_dump();

      return 0;
    }

  if (strcmp(topic, "clk") == 0 || strcmp(topic, "clock") == 0)
    {
      /* 现算，不依赖 bsp_init 的启动打印（那是开机时刻的快照，切频后不再更新）。 */
      myvendor_sys_clk_t c;

      myvendor_sys_clk_get(&c);
      printf("sys clk  sysclk %lu Hz  hclk %lu Hz\n",
             (unsigned long)c.sysclk_hz, (unsigned long)c.hclk_hz);
      printf("sys clk  dll1 %s %lu Hz  dll2 %s %lu Hz\n",
             c.dll1_en ? "on " : "off", (unsigned long)c.dll1_hz,
             c.dll2_en ? "on " : "off", (unsigned long)c.dll2_hz);
      printf("sys clk  psram(FLASH1) %lu Hz = src %s / psclr %u  (sw_div %u)\n",
             (unsigned long)c.flash1_hz,
             myvendor_sys_clk_src_name(c.flash1_src),
             (unsigned)c.flash1_psclr, (unsigned)c.flash1_sw_div);
      printf("sys clk  nand (FLASH2) %lu Hz = src %s / psclr %u  (sw_div %u)\n",
             (unsigned long)c.flash2_hz,
             myvendor_sys_clk_src_name(c.flash2_src),
             (unsigned)c.flash2_psclr, (unsigned)c.flash2_sw_div);
      if (c.mismatch)
        {
          printf("sys clk  MISMATCH: sw_div != hw psclr; trust the psclr value\n");
        }

      /* SD 时钟拓扑。52x 的 CSR 里没有 SEL_SDMMC，而硬件图上 SD 与 MPI 共用
       * 那一路时钟门，所以要直接把现场读出来判断父时钟是谁。只读。 */
      sf32lb_sd_clk_probe();

      return 0;
    }

  if (strcmp(topic, "usb") == 0 || strcmp(topic, "mtp") == 0)
    {
      printf("sys usb  policy %s  hw %s  worker %s\n",
             s->policy_mtp ? "on" : "off",
             s->usb.controller_on ? "on" : "off",
             s->usb.worker_running ? "running" : "stopped");
      return 0;
    }

  if (strcmp(topic, "notif") == 0)
    {
      uint8_t inbox_n = 0;

      myvendor_sys_inbox_get(NULL, &inbox_n, 0);
      printf("sys notif  policy %s  calls-only %s  inbox %u\n",
             s->policy_notif ? "on" : "off",
             s->policy_calls ? "on" : "off",
             (unsigned)inbox_n);
      return 0;
    }

  if (strcmp(topic, "sound") == 0)
    {
      printf("sys sound  policy %s\n", s->policy_sound ? "on" : "off");
      return 0;
    }

  if (strcmp(topic, "eph") == 0)
    {
      printf("sys eph  auto %s\n", s->policy_eph_auto ? "on" : "off");
      return 0;
    }

  if (strcmp(topic, "accel") == 0)
    {
      if (!s->accel.valid)
        {
          printf("sys accel  --\n");
        }
      else
        {
          printf("sys accel  ");
          sys_print_fixed(s->accel.x, 100);
          putchar(' ');
          sys_print_fixed(s->accel.y, 100);
          putchar(' ');
          sys_print_fixed(s->accel.z, 100);
          printf(" m/s2\n");
        }

      return 0;
    }

  if (strcmp(topic, "gyro") == 0)
    {
      if (!s->gyro.valid)
        {
          printf("sys gyro  --\n");
        }
      else
        {
          printf("sys gyro  ");
          sys_print_fixed(s->gyro.x, 100);
          putchar(' ');
          sys_print_fixed(s->gyro.y, 100);
          putchar(' ');
          sys_print_fixed(s->gyro.z, 100);
          printf(" rad/s\n");
        }

      return 0;
    }

  if (strcmp(topic, "baro") == 0)
    {
      if (!s->baro.valid)
        {
          printf("sys baro  --\n");
        }
      else
        {
          printf("sys baro  ");
          sys_print_fixed(s->baro.hpa, 10);
          printf(" hPa  ");
          sys_print_fixed(s->baro.temp_c, 10);
          printf(" C\n");
        }

      return 0;
    }

  if (strcmp(topic, "mag") == 0)
    {
      if (!s->mag.valid)
        {
          printf("sys mag  --\n");
        }
      else
        {
          printf("sys mag  ");
          sys_print_fixed(s->mag.x, 10);
          putchar(' ');
          sys_print_fixed(s->mag.y, 10);
          putchar(' ');
          sys_print_fixed(s->mag.z, 10);
          printf(" uT\n");
        }

      return 0;
    }

  if (strcmp(topic, "phone") == 0)
    {
      if (s->phone.valid)
        {
          printf("sys phone  %u.%02u km/h  lock %u heard %u view %u  q %u  utc %u  stamp %u\n",
                 (unsigned)(s->phone.speed_centi_kmh / 100u),
                 (unsigned)(s->phone.speed_centi_kmh % 100u),
                 (unsigned)s->phone.satellites,
                 (unsigned)s->phone.sats_heard,
                 (unsigned)s->phone.sats_in_view,
                 (unsigned)s->phone.fix_quality,
                 (unsigned)s->phone.utc_sec,
                 (unsigned)s->phone.stamp_ms);
        }
      else if (s->phone.alive)
        {
          printf("sys phone  search  lock %u heard %u view %u  utc %u  stamp %u\n",
                 (unsigned)s->phone.satellites,
                 (unsigned)s->phone.sats_heard,
                 (unsigned)s->phone.sats_in_view,
                 (unsigned)s->phone.utc_sec,
                 (unsigned)s->phone.stamp_ms);
        }
      else
        {
          printf("sys phone  --\n");
        }

      return 0;
    }

  if (strcmp(topic, "onboard") == 0 || strcmp(topic, "gnss") == 0)
    {
      if (s->onboard.valid)
        {
          printf("sys onboard  %u.%02u km/h  lock %u heard %u view %u  q %u  utc %u  stamp %u",
                 (unsigned)(s->onboard.speed_centi_kmh / 100u),
                 (unsigned)(s->onboard.speed_centi_kmh % 100u),
                 (unsigned)s->onboard.satellites,
                 (unsigned)s->onboard.sats_heard,
                 (unsigned)s->onboard.sats_in_view,
                 (unsigned)s->onboard.fix_quality,
                 (unsigned)s->onboard.utc_sec,
                 (unsigned)s->onboard.stamp_ms);
          /* PVT 段：gs = 模块滤波过的多普勒（与上面那个 km/h 同单位可直接比），
           * sacc = 它自己的 1σ 精度。没有 PVT 时这个函数什么都不打。 */
          myvendor_sys_gnss_print_pvt(&s->onboard);
          printf("\n");
        }
      else if (s->onboard.alive)
        {
          printf("sys onboard  search  lock %u heard %u view %u  utc %u  stamp %u",
                 (unsigned)s->onboard.satellites,
                 (unsigned)s->onboard.sats_heard,
                 (unsigned)s->onboard.sats_in_view,
                 (unsigned)s->onboard.utc_sec,
                 (unsigned)s->onboard.stamp_ms);
          myvendor_sys_gnss_print_pvt(&s->onboard);
          printf("\n");
        }
      else
        {
          printf("sys onboard  --\n");
        }

      return 0;
    }

  return -1;
}

int main(int argc, FAR char *argv[])
{
  myvendor_sys_snapshot_t s;

  if (argc < 2)
    {
      myvendor_sys_dump();
      fflush(stdout);
      return 0;
    }

  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0)
    {
      usage();
      return 0;
    }

  if (strcmp(argv[1], "dvfs") == 0 || strcmp(argv[1], "hclk") == 0)
    {
      sf32lb_dvfs_print();
      fflush(stdout);
      return 0;
    }

  myvendor_sys_snapshot(&s);

  if (strcmp(argv[1], "accel") == 0)
    {
      myvendor_sys_accel_get(&s.accel);
    }
  else if (strcmp(argv[1], "gyro") == 0)
    {
      myvendor_sys_gyro_get(&s.gyro);
    }
  else if (strcmp(argv[1], "baro") == 0)
    {
      myvendor_sys_baro_get(&s.baro);
    }
  else if (strcmp(argv[1], "mag") == 0)
    {
      myvendor_sys_mag_get(&s.mag);
    }

  if (print_topic(argv[1], &s) != 0)
    {
      fprintf(stderr, "sys: unknown '%s'\n", argv[1]);
      usage();
      return EXIT_FAILURE;
    }

  fflush(stdout);
  return 0;
}
