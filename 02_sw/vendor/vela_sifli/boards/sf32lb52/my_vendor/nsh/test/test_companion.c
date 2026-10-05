/**
 * @file test_companion.c
 * @brief test sensor/notif/ctrl：BLE companion 外设探针（不经 ctl/sys）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#ifdef CONFIG_MYVENDOR_BLE_COMPANION

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "companion_bridge.h"
#include "test_demos.h"

/**
 * @brief companion_bridge_test_sensor_mode() 的可读名称。
 */
static const char *sensor_mode_name(uint8_t mode)
{
  switch (mode)
    {
      case COMPANION_TEST_SENSOR_SCAN:
        return "scan";
      case COMPANION_TEST_SENSOR_CONNECT:
        return "connect";
      case COMPANION_TEST_SENSOR_DISCONNECT:
        return "disconnect";
      case COMPANION_TEST_SENSOR_STOP:
        return "off";
      case COMPANION_TEST_SENSOR_SCAN_STOP:
        return "idle";
      case COMPANION_TEST_SENSOR_CONNECT_ADDR:
        return "reconnect";
      case COMPANION_TEST_SENSOR_CLEAR_WANT:
        return "clear";
      default:
        return "idle";
    }
}

/**
 * @brief ble_companion 不在主循环则失败。
 * @return 存活返回 0，否则 -1。
 */
static int require_companion(void)
{
  if (!companion_bridge_alive_get())
    {
      printf("test: ble_companion not running (start: ble_companion &)\n");
      return -1;
    }

  return 0;
}

/**
 * @brief 打印已连接的 HR/CSC/CPS 采样（或尚未连接）。
 */
static void print_telem(void)
{
  struct companion_sensor_telem telem;
  bool any;

  companion_bridge_sensor_get(&telem);
  any = telem.hr_valid || telem.cadence_valid || telem.power_valid;
  if (!any)
    {
      printf("test sensor: (none linked)\n");
      return;
    }

  printf("test sensor:");
  if (telem.hr_valid)
    {
      printf(" HR=%u bpm", (unsigned)telem.hr_bpm);
    }

  if (telem.cadence_valid)
    {
      printf(" CSC=%u rpm", (unsigned)telem.cadence_rpm);
    }

  if (telem.power_valid)
    {
      printf(" CPS=%u W", (unsigned)telem.power_w);
    }

  printf("\n");
}

/**
 * @brief `test sensor` 用法（按表格 idx → 地址连接）。
 */
static void sensor_usage(void)
{
  printf("Usage: test sensor <scan [sec]|scan stop|list|connect <idx>|read|disconnect|off>\n");
  printf("  scan [sec]  observer %u s (default), then print HR/CSC/CPS table\n",
         (unsigned)(COMPANION_TEST_SENSOR_SCAN_MS_DEFAULT / 1000u));
  printf("  scan stop   stop observer; keep GATT if connected\n");
  printf("  list        reprint the scan table (for connect idx)\n");
  printf("  connect <idx>  connect table row by BLE address (not name)\n");
  printf("  read        linked HR/CSC/CPS table after connect\n");
  printf("  disconnect  drop GATT links; scan may keep running\n");
  printf("  off         stop scan and disconnect\n");
}

/**
 * @brief NSH `test sensor` — 投递到 companion 线程。
 * @param argc 参数个数。
 * @param argv 子命令与参数（scan/list/connect/read/disconnect/off）。
 * @return EXIT_SUCCESS 或 EXIT_FAILURE。
 */
/**
 * @brief test sensor BLE 传感器扫描/连接演示。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_sensor_main(int argc, FAR char *argv[])
{
  if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
      sensor_usage();
      printf("companion=%s  mode=%s  notif=%s  ctrl=%s\n",
             companion_bridge_alive_get() ? "up" : "down",
             sensor_mode_name(companion_bridge_test_sensor_mode()),
             companion_bridge_test_notif_get() ? "on" : "off",
             companion_bridge_test_ctrl_get() ? "on" : "off");
      print_telem();
      return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (require_companion() != 0)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "scan") == 0)
    {
      unsigned long sec = COMPANION_TEST_SENSOR_SCAN_MS_DEFAULT / 1000ul;
      char *end = NULL;

      if (argc >= 3 && strcmp(argv[2], "stop") == 0)
        {
          companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_SCAN_STOP);
          printf("test sensor: scan stop posted\n");
          return EXIT_SUCCESS;
        }

      if (argc >= 3)
        {
          sec = strtoul(argv[2], &end, 10);
          if (end == argv[2] || sec == 0 || sec > 600ul)
            {
              printf("test sensor: scan [sec]|scan stop\n");
              return EXIT_FAILURE;
            }
        }

      companion_bridge_test_sensor_set_scan_ms((uint32_t)sec * 1000u);
      companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_SCAN);
      printf("test sensor: scan %lu s, then sensor table (list/read to reprint)\n",
             sec);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "list") == 0)
    {
      companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_DUMP);
      usleep(150000);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "connect") == 0)
    {
      unsigned long idx;
      char *end = NULL;

      if (argc < 3)
        {
          printf("test sensor: connect <idx>  (idx from scan table, by address)\n");
          companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_DUMP);
          usleep(150000);
          return EXIT_FAILURE;
        }

      idx = strtoul(argv[2], &end, 10);
      if (end == argv[2] || idx == 0 || idx > 16ul)
        {
          printf("test sensor: connect idx 1..16 from the table\n");
          return EXIT_FAILURE;
        }

      companion_bridge_test_sensor_set_connect_idx((uint8_t)idx);
      companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_CONNECT);
      printf("test sensor: connect idx=%lu by address\n", idx);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "read") == 0)
    {
      print_telem();
      companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_DUMP);
      usleep(150000);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "disconnect") == 0)
    {
      companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_DISCONNECT);
      printf("test sensor: disconnect posted\n");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "stop") == 0)
    {
      companion_bridge_test_sensor_post(COMPANION_TEST_SENSOR_STOP);
      printf("test sensor: off posted\n");
      return EXIT_SUCCESS;
    }

  printf("test sensor: unknown '%s'\n", argv[1]);
  sensor_usage();
  return EXIT_FAILURE;
}

/**
 * @brief `test notif` 用法。
 */
static void notif_usage(void)
{
  printf("Usage: test notif <on|off>\n");
  printf("  on   print each phone 0xFF17 notification (title/body/icon filename)\n");
  printf("  off  stop printing\n");
}

/**
 * @brief NSH `test notif on|off` — 打印到达的 0xFF17。
 */
/**
 * @brief test notif 手机 0xFF17 通知打印开关。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_notif_main(int argc, FAR char *argv[])
{
  if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
      notif_usage();
      printf("companion=%s  notif=%s\n",
             companion_bridge_alive_get() ? "up" : "down",
             companion_bridge_test_notif_get() ? "on" : "off");
      return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "on") == 0)
    {
      if (require_companion() != 0)
        {
          return EXIT_FAILURE;
        }

      companion_bridge_test_notif_set(true);
      printf("test notif: on (phone notifications print immediately)\n");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "off") == 0)
    {
      companion_bridge_test_notif_set(false);
      printf("test notif: off\n");
      return EXIT_SUCCESS;
    }

  printf("test notif: unknown '%s'\n", argv[1]);
  notif_usage();
  return EXIT_FAILURE;
}

/**
 * @brief `test ctrl` 用法。
 */
static void ctrl_usage(void)
{
  printf("Usage: test ctrl <on|off>\n");
  printf("  on   print 0xFF13 control opcodes and 0xFF19 nav points (no UI nav)\n");
  printf("  off  stop printing\n");
}

/**
 * @brief NSH `test ctrl on|off` — 打印 0xFF13 / 0xFF19（不启动 UI 导航）。
 */
/**
 * @brief test ctrl 手机 0xFF13/0xFF19 控制打印开关。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_ctrl_main(int argc, FAR char *argv[])
{
  if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
      ctrl_usage();
      printf("companion=%s  ctrl=%s\n",
             companion_bridge_alive_get() ? "up" : "down",
             companion_bridge_test_ctrl_get() ? "on" : "off");
      return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "on") == 0)
    {
      if (require_companion() != 0)
        {
          return EXIT_FAILURE;
        }

      companion_bridge_test_ctrl_set(true);
      printf("test ctrl: on (control/nav print only, navigation not started)\n");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "off") == 0)
    {
      companion_bridge_test_ctrl_set(false);
      printf("test ctrl: off\n");
      return EXIT_SUCCESS;
    }

  printf("test ctrl: unknown '%s'\n", argv[1]);
  ctrl_usage();
  return EXIT_FAILURE;
}

#endif /* CONFIG_MYVENDOR_BLE_COMPANION */
