/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/ble_test_app/ble_test_app.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal BLE peripheral test:
 *   1. Enable Bluetooth adapter (local Framework)
 *   2. Register a private GATT service (0xFF00)
 *   3. Start legacy advertising as "Vela-BT"
 *
 * No SMP pairing is required for read/write on 0xFF05 / 0xFF02.
 * Test with nRF Connect: scan, connect, read 0xFF05, write 0xFF02.
 *
 * Usage (NSH):
 *   ble_test          # run in foreground
 *   ble_test &        # run in background
 *
 * Stop: kill <pid>  (or reboot)
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bluetooth.h"
#include "bt_adapter.h"
#include "bt_device.h"
#include "bt_gatts.h"
#include "bt_le_advertiser.h"

#define LOGI(fmt, ...)  printf("ble_test: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("ble_test: ERROR " fmt "\n", ##__VA_ARGS__)

#define ADAPTER_WAIT_SECS       60
#define HCI_DEV_PATH            "/dev/ttyHCI0"
#define MAIN_LOOP_SLEEP_SECS    5

/* Private IOT service (same UUIDs as bttool gatts service 3). */

enum
{
  IOT_SERVICE_ID = 1,
  IOT_RX_CHR_ID,
  IOT_READ_CHR_ID,
};

static uint8_t g_read_char_value[] =
{
  'H', 'e', 'l', 'l', 'o', ' ', 'V', 'E', 'L', 'A', '!'
};

static uint8_t g_rw_char_value[64];
static uint16_t g_rw_char_len;

static bt_instance_t *g_ins;
static void *g_adapter_cookie;
static gatts_handle_t g_gatts_handle;
static bt_advertiser_t *g_adv_handle;

static volatile bool g_running = true;
static volatile bt_adapter_state_t g_adapter_state = BT_ADAPTER_STATE_OFF;

/* Legacy adv: LE only + manufacturer 0x038F; scan rsp: name "Vela-BT". */

static uint8_t g_adv_data[] =
{
  0x02, 0x01, 0x06, 0x03, 0xff, 0x8f, 0x03
};

static uint8_t g_scan_rsp[] =
{
  0x08, 0x09, 'V', 'e', 'l', 'a', '-', 'B', 'T'
};

static uint16_t rx_char_on_read(gatts_handle_t srv_handle, bt_address_t *addr,
                                uint16_t attr_handle, uint32_t req_handle)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("read 0xFF02 from %s", addr_str);

  bt_gatts_response(srv_handle, addr, req_handle, g_rw_char_value, g_rw_char_len);
  return 0;
}

static uint16_t rx_char_on_write(gatts_handle_t srv_handle, bt_address_t *addr,
                                 uint16_t attr_handle, const uint8_t *value,
                                 uint16_t length, uint16_t offset)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  if (offset + length > sizeof(g_rw_char_value))
    {
      return 0;
    }

  if (offset == 0)
    {
      memset(g_rw_char_value, 0, sizeof(g_rw_char_value));
    }

  memcpy(g_rw_char_value + offset, value, length);
  g_rw_char_len = offset + length;

  bt_addr_ba2str(addr, addr_str);
  LOGI("write 0xFF02 from %s, len=%u", addr_str, length);
  return length;
}

static gatt_attr_db_t g_iot_attr_db[] =
{
  GATT_H_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xff00), IOT_SERVICE_ID),
  GATT_H_CHARACTERISTIC_USER_RSP(BT_UUID_DECLARE_16(0xff02),
                               GATT_PROP_READ | GATT_PROP_WRITE_NR | GATT_PROP_WRITE,
                               GATT_PERM_READ | GATT_PERM_WRITE,
                               rx_char_on_read, rx_char_on_write, IOT_RX_CHR_ID),
  GATT_H_CHARACTERISTIC_AUTO_RSP(BT_UUID_DECLARE_16(0xff05),
                                 GATT_PROP_READ, GATT_PERM_READ,
                                 g_read_char_value, sizeof(g_read_char_value),
                                 IOT_READ_CHR_ID),
};

static gatt_srv_db_t g_iot_service_db =
{
  .attr_db = g_iot_attr_db,
  .attr_num = sizeof(g_iot_attr_db) / sizeof(gatt_attr_db_t),
};

static void on_adapter_state_changed(void *cookie, bt_adapter_state_t state)
{
  g_adapter_state = state;
  LOGI("adapter state -> %d", (int)state);
}

static void on_connection_state_changed(void *cookie, bt_address_t *addr,
                                         bt_transport_t transport,
                                         connection_state_t state)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("connection %s transport=%d state=%d", addr_str, (int)transport,
       (int)state);
}

static const adapter_callbacks_t g_adapter_cbs =
{
  .on_adapter_state_changed = on_adapter_state_changed,
  .on_connection_state_changed = on_connection_state_changed,
};

static void gatts_connect_callback(gatts_handle_t srv_handle, bt_address_t *addr)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("GATT connected: %s", addr_str);
}

static void gatts_disconnect_callback(gatts_handle_t srv_handle,
                                      bt_address_t *addr)
{
  char addr_str[BT_ADDR_STR_LENGTH];

  bt_addr_ba2str(addr, addr_str);
  LOGI("GATT disconnected: %s", addr_str);
}

static void gatts_attr_table_added(gatts_handle_t srv_handle,
                                   gatt_status_t status, uint16_t attr_handle)
{
  LOGI("GATT table added handle=0x%04x status=%d", attr_handle, (int)status);
}

static const gatts_callbacks_t g_gatts_cbs =
{
  sizeof(g_gatts_cbs),
  gatts_connect_callback,
  gatts_disconnect_callback,
  gatts_attr_table_added,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
};

static void on_advertising_start(bt_advertiser_t *adv, uint8_t adv_id,
                                 uint8_t status)
{
  LOGI("advertising started id=%u status=%u handle=%p", adv_id, status, adv);
}

static void on_advertising_stopped(bt_advertiser_t *adv, uint8_t adv_id)
{
  LOGI("advertising stopped id=%u handle=%p", adv_id, adv);
}

static const advertiser_callback_t g_adv_cbs =
{
  sizeof(g_adv_cbs),
  on_advertising_start,
  on_advertising_stopped,
};

static int wait_hci_device(int timeout_secs)
{
  int i;

  for (i = 0; i < timeout_secs * 10; i++)
    {
      if (access(HCI_DEV_PATH, F_OK) == 0)
        {
          LOGI("HCI device %s OK", HCI_DEV_PATH);
          return 0;
        }

      usleep(100 * 1000);
    }

  LOGE("%s missing — enable CONFIG_UART_BTH4 and rebuild", HCI_DEV_PATH);
  return -ENOENT;
}

static int wait_adapter_ready(int timeout_secs)
{
  int i;
  bt_adapter_state_t last = BT_ADAPTER_STATE_OFF;

  for (i = 0; i < timeout_secs * 10; i++)
    {
      bt_adapter_state_t state = bt_adapter_get_state(g_ins);

      if (state != last)
        {
          LOGI("adapter state -> %d", (int)state);
          last = state;
        }

      if (state == BT_ADAPTER_STATE_ON)
        {
          return 0;
        }

      usleep(100 * 1000);
    }

  LOGE("adapter not ready, state=%d (expect 4=ON)", (int)bt_adapter_get_state(g_ins));
  return -ETIMEDOUT;
}

static int ble_start_peripheral(void)
{
  ble_adv_params_t params;
  bt_status_t ret;
  char addr_str[BT_ADDR_STR_LENGTH];
  bt_address_t le_addr;
  ble_addr_type_t le_addr_type;

  if (wait_hci_device(10) != 0)
    {
      return -1;
    }

  ret = bt_adapter_enable(g_ins);
  LOGI("bt_adapter_enable -> %d", (int)ret);
  if (ret != BT_STATUS_SUCCESS && ret != BT_STATUS_DONE)
    {
      LOGE("bt_adapter_enable failed: %d", (int)ret);
      return -1;
    }

  if (wait_adapter_ready(ADAPTER_WAIT_SECS) != 0)
    {
      return -1;
    }

  bt_device_set_bondable_le(g_ins, false);

  ret = bt_gatts_register_service(g_ins, &g_gatts_handle, (gatts_callbacks_t *)&g_gatts_cbs);
  if (ret != BT_STATUS_SUCCESS || g_gatts_handle == NULL)
    {
      LOGE("bt_gatts_register_service failed: %d", (int)ret);
      return -1;
    }

  ret = bt_gatts_add_attr_table(g_gatts_handle, &g_iot_service_db);
  if (ret != BT_STATUS_SUCCESS)
    {
      LOGE("bt_gatts_add_attr_table failed: %d", (int)ret);
      return -1;
    }

  memset(&params, 0, sizeof(params));
  params.adv_type = BT_LE_LEGACY_ADV_IND;
  params.peer_addr_type = BT_LE_ADDR_TYPE_PUBLIC;
  params.own_addr_type = BT_LE_ADDR_TYPE_PUBLIC;
  params.interval = 320;
  params.tx_power = 0;
  params.channel_map = BT_LE_ADV_CHANNEL_DEFAULT;
  params.filter_policy = BT_LE_ADV_FILTER_WHITE_LIST_FOR_NONE;

  g_adv_handle = bt_le_start_advertising(g_ins, &params,
                                         g_adv_data, sizeof(g_adv_data),
                                         g_scan_rsp, sizeof(g_scan_rsp),
                                         (advertiser_callback_t *)&g_adv_cbs);
  if (g_adv_handle == NULL)
    {
      LOGE("bt_le_start_advertising failed");
      return -1;
    }

  if (bt_adapter_get_le_address(g_ins, &le_addr, &le_addr_type) == BT_STATUS_SUCCESS)
    {
      bt_addr_ba2str(&le_addr, addr_str);
      LOGI("LE address: %s", addr_str);
    }

  LOGI("ready — scan for \"Vela-BT\" (nRF Connect)");
  LOGI("  service 0xFF00, read 0xFF05, write 0xFF02");
  return 0;
}

static void ble_cleanup(void)
{
  if (g_adv_handle != NULL && g_ins != NULL)
    {
      bt_le_stop_advertising(g_ins, g_adv_handle);
      g_adv_handle = NULL;
    }

  if (g_gatts_handle != NULL)
    {
      bt_gatts_remove_attr_table(g_gatts_handle, IOT_SERVICE_ID);
      bt_gatts_unregister_service(g_gatts_handle);
      g_gatts_handle = NULL;
    }

  if (g_ins != NULL)
    {
      bt_adapter_disable(g_ins);
    }

  if (g_adapter_cookie != NULL && g_ins != NULL)
    {
      bt_adapter_unregister_callback(g_ins, g_adapter_cookie);
      g_adapter_cookie = NULL;
    }

  if (g_ins != NULL)
    {
      bluetooth_delete_instance(g_ins);
      g_ins = NULL;
    }
}

static void signal_handler(int signo)
{
  (void)signo;
  g_running = false;
}

int main(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  memcpy(g_rw_char_value, g_read_char_value, sizeof(g_read_char_value));
  g_rw_char_len = sizeof(g_read_char_value);

  g_ins = bluetooth_create_instance();
  if (g_ins == NULL)
    {
      LOGE("bluetooth_create_instance failed");
      return EXIT_FAILURE;
    }

  g_adapter_cookie = bt_adapter_register_callback(g_ins, &g_adapter_cbs);
  if (g_adapter_cookie == NULL)
    {
      LOGE("bt_adapter_register_callback failed");
      ble_cleanup();
      return EXIT_FAILURE;
    }

  if (ble_start_peripheral() != 0)
    {
      ble_cleanup();
      return EXIT_FAILURE;
    }

  while (g_running)
    {
      sleep(MAIN_LOOP_SLEEP_SECS);
    }

  LOGI("shutting down...");
  ble_cleanup();
  LOGI("done");
  return EXIT_SUCCESS;
}
