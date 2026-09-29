/* GATT server.
 *
 * Services presented:
 *   0x180D Heart Rate - bpm plus sensor contact, 8 bit value
 *   0x180F Battery    - the liveness probe target of heart-rate clients
 *   0x180A Device Information
 *
 * The Heart Rate Control Point (0x2A39) is deliberately absent: it only exists
 * to reset the energy-expended field, and this strap reports no energy, so
 * there would be nothing to reset.  See the README for adding both.
 */
#include "gatt_hr.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "hr_defs.h"
#include "hr_sim.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "hr-gatt";

#define HR_MEAS_LEN        2      /* flags + 8 bit bpm */
#define HR_MODEL_STR       "HRS-SIM"
#define HR_MANUF_STR       "Helm One"
#define HR_FW_STR          "1.0.0"

static uint16_t s_meas_handle;
static uint16_t s_bat_handle;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_meas_notify;
static bool s_bat_notify;

/* Last battery level a client was told about; the sweeping battery is picked up
 * from here on the back of the measurement stream. */
static uint8_t s_bat_last = 0xff;

static uint8_t s_location = HR_LOC_CHEST;

static struct ble_npl_event s_tx_ev;

static void tx_handler(struct ble_npl_event *ev);

static void pack_measurement(uint8_t *out)
{
    hr_sample_t smp;

    hr_sim_sample(&smp);

    /* Sensor contact is supported either way; whether it is currently detected
     * is what changes when the strap comes off a body. */
    out[0] = HR_FLAG_CONTACT_SUPPORTED;
    if (smp.contact) {
        out[0] |= HR_FLAG_CONTACT_DETECTED;
    }
    out[1] = smp.bpm;
}

/* --- characteristic access --------------------------------------------- */

static int chr_meas_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t buf[HR_MEAS_LEN];

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    pack_measurement(buf);
    return (os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_location_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return (os_mbuf_append(ctxt->om, &s_location, sizeof(s_location)) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_battery_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t level;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    level = hr_sim_battery_get();
    return (os_mbuf_append(ctxt->om, &level, sizeof(level)) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_device_info_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *value;
    const uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (uuid) {
    case UUID_DIS_MANUF:
        value = HR_MANUF_STR;
        break;
    case UUID_DIS_MODEL:
        value = HR_MODEL_STR;
        break;
    case UUID_DIS_FW_REV:
        value = HR_FW_STR;
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }

    return (os_mbuf_append(ctxt->om, value, (uint16_t)strlen(value)) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_HR_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_HR_MEAS),
                .access_cb = chr_meas_access,
                .val_handle = &s_meas_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            }, {
                .uuid = BLE_UUID16_DECLARE(UUID_HR_LOC),
                .access_cb = chr_location_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                0,
            },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_BAS_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_BAS_LEVEL),
                .access_cb = chr_battery_access,
                .val_handle = &s_bat_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            }, {
                0,
            },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_DIS_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_DIS_MANUF),
                .access_cb = chr_device_info_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = BLE_UUID16_DECLARE(UUID_DIS_MODEL),
                .access_cb = chr_device_info_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = BLE_UUID16_DECLARE(UUID_DIS_FW_REV),
                .access_cb = chr_device_info_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                0,
            },
        },
    },
    {
        0,
    },
};

/* --- notifications ------------------------------------------------------ */

static void tx_handler(struct ble_npl_event *ev)
{
    uint8_t buf[HR_MEAS_LEN];
    struct os_mbuf *om;
    int rc;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_meas_notify) {
        return;
    }

    pack_measurement(buf);

    om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om == NULL) {
        hr_sim_count_notify(false);
        return;
    }

    rc = ble_gatts_notify_custom(s_conn, s_meas_handle, om);
    hr_sim_count_notify(rc == 0);

    if (rc != 0) {
        ESP_LOGW(TAG, "measurement notify rc=%d", rc);
    }

    /* The battery sweeps on its own; ride the measurement stream to report it
     * rather than running a second timer. */
    if (hr_sim_battery_get() != s_bat_last) {
        gatt_hr_notify_battery();
    }
}

void gatt_hr_kick_tx(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
}

void gatt_hr_notify_battery(void)
{
    uint8_t level = hr_sim_battery_get();
    struct os_mbuf *om;
    int rc;

    s_bat_last = level;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_bat_notify) {
        return;
    }

    om = ble_hs_mbuf_from_flat(&level, sizeof(level));
    if (om == NULL) {
        return;
    }

    rc = ble_gatts_notify_custom(s_conn, s_bat_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "battery notify rc=%d", rc);
    }
}

/* --- link bookkeeping --------------------------------------------------- */

void gatt_hr_connected(uint16_t conn_handle)
{
    s_conn = conn_handle;
    s_meas_notify = false;
    s_bat_notify = false;
}

void gatt_hr_disconnected(uint16_t conn_handle)
{
    if (s_conn == conn_handle) {
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    s_meas_notify = false;
    s_bat_notify = false;
}

void gatt_hr_on_subscribe(uint16_t attr_handle, bool notify, bool indicate)
{
    if (attr_handle == s_meas_handle) {
        s_meas_notify = notify;
        ESP_LOGI(TAG, "heart rate notify=%d", (int)notify);
    } else if (attr_handle == s_bat_handle) {
        s_bat_notify = notify;
        ESP_LOGI(TAG, "battery notify=%d", (int)notify);
        if (notify) {
            gatt_hr_notify_battery();
        }
    }
}

int gatt_hr_init(void)
{
    int rc;

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return rc;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return rc;
    }

    ble_npl_event_init(&s_tx_ev, tx_handler, NULL);

    ESP_LOGI(TAG, "services up: Heart Rate 0x180D, battery, body location=0x%02x",
             (unsigned)s_location);

    return 0;
}
