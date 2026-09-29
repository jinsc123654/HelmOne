/* ESP32-S3 bench instrument: a standard BLE cadence sensor (CSC, 0x1816).
 *
 * It exists so the Helm One bike computer has a cadence meter to find, bind and
 * read, without a real one on the bench.  The parts of the behaviour that the
 * client on the other side depends on:
 *
 *   - a stable identity.  Helm One binds a sensor by MAC and keeps the record in
 *     /mnt/kv/bicycle_sensors.tsv, so the address must not rotate between
 *     reboots, reflashes, or even boards.  A static random address (top two bits
 *     of the MSB set) does that, where the chip's public MAC would tie the
 *     binding to this particular module.
 *   - 0x1816 in the advertising payload, connectable, undirected, legacy: that
 *     is how the client's scanner decides the device is worth listing.
 *   - advertising stops on connect.  A client that sees the sensor advertising
 *     while it also holds a link treats it as a zombie and drops the link.
 *   - measurement notifications that carry crank revolution data, sent on the
 *     revolution rather than on our own clock (see csc_sim.c).
 */
#include <string.h>

#include "app.h"
#include "csc_defs.h"
#include "csc_sim.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gatt_csc.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "led.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_random.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "csc-sim";

/* Displayed as C0:DE:CA:DE:00:01 (the array is least significant byte first). */
static const uint8_t s_own_addr[6] = { 0x01, 0x00, 0xde, 0xca, 0xde, 0xc0 };

/* The advertised name ends with the last four hex digits of that address, so
 * several simulators on one bench can be told apart by name alone
 * (Helm-CSC-0001, -0002, ...) instead of by reading addresses. */
#define DEVICE_NAME_BASE            "Helm-CSC"
#define APPEARANCE_CADENCE_SENSOR   0x0483

/* Reset the board on a random cadence: it walks the client through "the sensor
 * vanished and came back" without anyone unplugging anything, and a random gap
 * keeps the reboot from lining up with whatever else the client does on a fixed
 * period.  The range lives in app.h. */
static char s_name[24];
static esp_timer_handle_t s_reset_timer;
static uint32_t s_reset_min_s;
static uint32_t s_reset_max_s;
static uint32_t s_reset_delay_s;
static uint64_t s_reset_started_us;

/* Advertising interval in 0.625 ms units: 30 ms min, 60 ms max.  The client
 * scans passively in 1 s windows, so speed here costs nothing and removes any
 * chance of the sensor being missed. */
#define ADV_ITVL_MIN   0x30
#define ADV_ITVL_MAX   0x60

static const ble_uuid16_t s_csc_uuid = BLE_UUID16_INIT(UUID_CSC_SVC);

static uint8_t s_own_addr_type;
static bool s_adv_on;

static int gap_event(struct ble_gap_event *event, void *arg);

static void addr_to_str(const uint8_t *val, char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             val[5], val[4], val[3], val[2], val[1], val[0]);
}

static void reset_fire(void *arg)
{
    ESP_LOGW(TAG, "scheduled board reset after %u s", (unsigned)s_reset_delay_s);
    esp_restart();
}

/* Called with the lock-free statics only, from the app task or the console. */
void app_reset_schedule(uint32_t min_s, uint32_t max_s)
{
    if (s_reset_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = reset_fire,
            .name = "auto-reset",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_reset_timer));
    } else {
        (void)esp_timer_stop(s_reset_timer);
    }

    s_reset_min_s = min_s;
    s_reset_max_s = (max_s < min_s) ? min_s : max_s;
    s_reset_started_us = (uint64_t)esp_timer_get_time();

    if (min_s == 0) {
        s_reset_delay_s = 0;
        ESP_LOGI(TAG, "scheduled board reset off");
        return;
    }

    s_reset_delay_s = (s_reset_max_s > s_reset_min_s)
                      ? (s_reset_min_s + (esp_random() % (s_reset_max_s - s_reset_min_s + 1u)))
                      : s_reset_min_s;

    ESP_ERROR_CHECK(esp_timer_start_once(s_reset_timer, (uint64_t)s_reset_delay_s * 1000000ULL));

    if (s_reset_max_s > s_reset_min_s) {
        ESP_LOGI(TAG, "board resets in %u s (random %u..%u s)",
                 (unsigned)s_reset_delay_s, (unsigned)s_reset_min_s, (unsigned)s_reset_max_s);
    } else {
        ESP_LOGI(TAG, "board resets in %u s", (unsigned)s_reset_delay_s);
    }
}

void app_reset_range(uint32_t *min_s, uint32_t *max_s)
{
    *min_s = s_reset_min_s;
    *max_s = s_reset_max_s;
}

int app_reset_remaining_s(void)
{
    uint64_t elapsed;

    if (s_reset_delay_s == 0) {
        return -1;
    }

    elapsed = ((uint64_t)esp_timer_get_time() - s_reset_started_us) / 1000000ULL;
    return (elapsed < s_reset_delay_s) ? (int)(s_reset_delay_s - elapsed) : 0;
}

static void app_adv_start(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_gap_adv_params params = { 0 };
    int rc;

    if (s_adv_on) {
        return;
    }

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids16 = &s_csc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = APPEARANCE_CADENCE_SENSOR;
    fields.appearance_is_present = 1;
    fields.name = (uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = ADV_ITVL_MIN;
    params.itvl_max = ADV_ITVL_MAX;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
        return;
    }

    s_adv_on = true;
    led_set(LED_IDLE);
    ESP_LOGI(TAG, "advertising as %s", s_name);
}

static void app_adv_stop(void)
{
    if (!s_adv_on) {
        return;
    }
    s_adv_on = false;
    (void)ble_gap_adv_stop();
    led_set(LED_OFF);
    ESP_LOGI(TAG, "advertising stopped");
}

void app_adv_set(bool on)
{
    if (on) {
        app_adv_start();
    } else {
        app_adv_stop();
    }
}

bool app_adv_on(void)
{
    return s_adv_on;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        char peer[24] = "?";
        struct ble_gap_conn_desc desc;

        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            app_adv_start();
            break;
        }

        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
            addr_to_str(desc.peer_ota_addr.val, peer, sizeof(peer));
        }

        gatt_csc_connected(event->connect.conn_handle);
        csc_sim_link_up(peer);

        /* Stop advertising before anything else: while linked the sensor must
         * not be discoverable again. */
        app_adv_stop();
        led_set(LED_LINKED);

        ESP_LOGI(TAG, "connected handle=%u peer=%s itvl=%u us timeout=%u ms",
                 (unsigned)event->connect.conn_handle, peer,
                 (unsigned)(desc.conn_itvl * 1250), (unsigned)(desc.supervision_timeout * 10));
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        const uint16_t handle = event->disconnect.conn.conn_handle;

        ESP_LOGI(TAG, "disconnected handle=%u reason=0x%02x", (unsigned)handle,
                 (unsigned)event->disconnect.reason);

        gatt_csc_disconnected(handle);
        csc_sim_link_down();
        app_adv_start();
        break;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete reason=0x%02x", (unsigned)event->adv_complete.reason);
        s_adv_on = false;
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        gatt_csc_on_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify,
                              event->subscribe.cur_indicate);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", (unsigned)event->mtu.value);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;

        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "conn update status=%d itvl=%u us latency=%u timeout=%u ms",
                     event->conn_update.status, (unsigned)(desc.conn_itvl * 1250),
                     (unsigned)desc.conn_latency, (unsigned)(desc.supervision_timeout * 10));
        }
        break;
    }

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "phy update status=%d tx=%u rx=%u", event->phy_updated.status,
                 (unsigned)event->phy_updated.tx_phy, (unsigned)event->phy_updated.rx_phy);
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* A cadence sensor needs no security: let the peer pair again from
         * scratch rather than resurrecting keys we never kept. */
        ESP_LOGW(TAG, "repeat pairing on handle=%u, ignored",
                 (unsigned)event->repeat_pairing.conn_handle);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;

    default:
        break;
    }

    return 0;
}

static void on_sync(void)
{
    char own[24];
    int rc;

    /* Must be set before the first advertising start; the host pushes it to the
     * controller with LE Set Random Address. */
    rc = ble_hs_id_set_rnd(s_own_addr);
    if (rc != 0) {
        ESP_LOGE(TAG, "set random address rc=%d", rc);
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable own address, rc=%d", rc);
        return;
    }

    addr_to_str(s_own_addr, own, sizeof(own));
    ESP_LOGI(TAG, "identity %s (own_addr_type=%u, %s random)",
             own, (unsigned)s_own_addr_type,
             ((s_own_addr[5] & 0xc0) == 0xc0) ? "static" : "non-static");
    ESP_LOGI(TAG, "bind me on the bike computer: ctl sensor bind 1CSC %s %u",
             own, (unsigned)s_own_addr_type);

    app_adv_start();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset reason=%d", reason);
    s_adv_on = false;
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "nimble host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* The name carries the last four hex digits of our own address. */
    snprintf(s_name, sizeof(s_name), "%s-%02X%02X", DEVICE_NAME_BASE,
             s_own_addr[1], s_own_addr[0]);
    led_init();

    ESP_ERROR_CHECK(nimble_port_init());

    /* No bonding: a CSC client links without security, and keeping keys would
     * only complicate the bench. */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    int rc = ble_svc_gap_device_name_set(s_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device name rc=%d", rc);
    }
    rc = ble_svc_gap_device_appearance_set(APPEARANCE_CADENCE_SENSOR);
    if (rc != 0) {
        ESP_LOGE(TAG, "appearance rc=%d", rc);
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(gatt_csc_init());

    csc_sim_init();
    csc_sim_set_tx_cb(gatt_csc_kick_tx);

    ESP_LOGI(TAG, "csc simulator ready (%s)", s_name);
    cli_start();
    app_reset_schedule(APP_RESET_MIN_S, APP_RESET_MAX_S);

    nimble_port_freertos_init(host_task);
}
