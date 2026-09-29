/* GATT server.
 *
 * Services presented:
 *   0x1816 CSC     - a cadence-only sensor: crank revolution data, no wheel data
 *   0x180F Battery - the liveness probe target of CSC-only clients
 *   0x180A Device Information
 */
#include "gatt_csc.h"

#include <string.h>

#include "csc_defs.h"
#include "csc_sim.h"
#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "csc-gatt";

/* A cadence meter measures crank revolutions only: bit 0 (wheel revolution
 * data) of CSC Feature stays clear, and the measurement never sets the
 * corresponding flag. */
#define CSC_FEATURE_VALUE   (CSC_FEAT_CRANK_REV_DATA)

/* Measurement payload: flags + cumulative crank revolutions + crank event time. */
#define CSC_MEAS_LEN        5

#define CSC_MODEL_STR       "CSC-SIM"
#define CSC_MANUF_STR       "Helm One"
#define CSC_FW_STR          "1.0.0"

static const char *const csc_location_name[] = {
    "other", "top-of-shoe", "in-shoe", "hip", "front-wheel", "left-crank",
    "right-crank", "left-pedal", "right-pedal", "front-hub", "rear-dropout",
    "chainstay", "rear-wheel", "rear-hub", "chest", "spider", "chain-ring",
};

static uint16_t s_meas_handle;
static uint16_t s_cp_handle;
static uint16_t s_bat_handle;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_meas_notify;
static bool s_cp_indicate;
static bool s_bat_notify;

/* Last battery level a client was told about; the sweeping battery is picked up
 * from here on the back of the measurement stream. */
static uint8_t s_bat_last = 0xff;

static uint8_t s_location = CSC_LOC_CHAINSTAY;

static struct ble_npl_event s_tx_ev;

static void tx_handler(struct ble_npl_event *ev);

static void pack_measurement(uint8_t *out)
{
    csc_sample_t smp;

    csc_sim_sample(&smp);

    out[0] = CSC_MEAS_FLAG_CRANK_REV;
    out[1] = (uint8_t)(smp.revs & 0xff);
    out[2] = (uint8_t)((smp.revs >> 8) & 0xff);
    out[3] = (uint8_t)(smp.evt_time & 0xff);
    out[4] = (uint8_t)((smp.evt_time >> 8) & 0xff);
}

/* --- characteristic access --------------------------------------------- */

static int chr_meas_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t buf[CSC_MEAS_LEN];

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    pack_measurement(buf);
    return (os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_feature_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t buf[2];

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    buf[0] = (uint8_t)(CSC_FEATURE_VALUE & 0xff);
    buf[1] = (uint8_t)(CSC_FEATURE_VALUE >> 8);
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

static int chr_cp_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t op = 0;
    uint8_t resp[2];
    struct os_mbuf *om;
    int rc;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    if (os_mbuf_copydata(ctxt->om, 0, sizeof(op), &op) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (!s_cp_indicate) {
        ESP_LOGW(TAG, "control point op=0x%02x with indication CCC off", op);
        return CSC_ATT_ERR_CCC_IMPROPERLY_CFG;
    }

    /* Every control point procedure is about wheel revolutions or about moving
     * a multi-location sensor: a cadence-only sensor supports none of them. */
    resp[0] = CSC_CP_OP_RESPONSE;
    resp[1] = CSC_CP_RESP_OP_NOT_SUPPORTED;

    om = ble_hs_mbuf_from_flat(resp, sizeof(resp));
    if (om == NULL) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    rc = ble_gatts_indicate_custom(conn_handle, s_cp_handle, om);
    ESP_LOGI(TAG, "control point op=0x%02x -> response 0x%02x rc=%d", op, resp[1], rc);

    return 0;
}

static int chr_battery_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t level;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    level = csc_sim_battery_get();
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
        value = CSC_MANUF_STR;
        break;
    case UUID_DIS_MODEL:
        value = CSC_MODEL_STR;
        break;
    case UUID_DIS_FW_REV:
        value = CSC_FW_STR;
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }

    return (os_mbuf_append(ctxt->om, value, (uint16_t)strlen(value)) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_CSC_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_CSC_MEAS),
                .access_cb = chr_meas_access,
                .val_handle = &s_meas_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            }, {
                .uuid = BLE_UUID16_DECLARE(UUID_CSC_FEAT),
                .access_cb = chr_feature_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = BLE_UUID16_DECLARE(UUID_CSC_LOC),
                .access_cb = chr_location_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = BLE_UUID16_DECLARE(UUID_CSC_CP),
                .access_cb = chr_cp_access,
                .val_handle = &s_cp_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_INDICATE,
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
    uint8_t buf[CSC_MEAS_LEN];
    struct os_mbuf *om;
    int rc;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_meas_notify) {
        return;
    }

    pack_measurement(buf);

    om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om == NULL) {
        csc_sim_count_notify(false);
        return;
    }

    rc = ble_gatts_notify_custom(s_conn, s_meas_handle, om);
    csc_sim_count_notify(rc == 0);

    if (rc != 0) {
        ESP_LOGW(TAG, "measurement notify rc=%d", rc);
    }

    /* The battery sweeps on its own; ride the measurement stream to report it
     * rather than running a second timer. */
    if (csc_sim_battery_get() != s_bat_last) {
        gatt_csc_notify_battery();
    }
}

void gatt_csc_kick_tx(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
}

void gatt_csc_notify_battery(void)
{
    uint8_t level = csc_sim_battery_get();
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

void gatt_csc_connected(uint16_t conn_handle)
{
    s_conn = conn_handle;
    s_meas_notify = false;
    s_cp_indicate = false;
    s_bat_notify = false;
}

void gatt_csc_disconnected(uint16_t conn_handle)
{
    if (s_conn == conn_handle) {
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    s_meas_notify = false;
    s_cp_indicate = false;
    s_bat_notify = false;
}

void gatt_csc_on_subscribe(uint16_t attr_handle, bool notify, bool indicate)
{
    if (attr_handle == s_meas_handle) {
        s_meas_notify = notify;
        ESP_LOGI(TAG, "CSC measurement notify=%d", (int)notify);
    } else if (attr_handle == s_cp_handle) {
        s_cp_indicate = indicate;
        ESP_LOGI(TAG, "CSC control point indicate=%d", (int)indicate);
    } else if (attr_handle == s_bat_handle) {
        s_bat_notify = notify;
        ESP_LOGI(TAG, "battery notify=%d", (int)notify);
        if (notify) {
            gatt_csc_notify_battery();
        }
    }
}

int gatt_csc_init(void)
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

    ESP_LOGI(TAG, "services up: CSC 0x1816 (meas=%d cp=%d), battery=%d, location=%s",
             (int)s_meas_handle, (int)s_cp_handle, (int)s_bat_handle,
             csc_location_name[s_location]);

    return 0;
}
