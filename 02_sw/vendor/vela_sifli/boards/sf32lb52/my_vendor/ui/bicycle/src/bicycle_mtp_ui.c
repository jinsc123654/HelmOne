/**
 * @file bicycle_mtp_ui.c
 * @brief 自行车 UI — mtp_ui。
 */

#include "bicycle_mtp_ui.h"

#include "bicycle_runtime.h"
#include "lvgl_page.h"
#include "live_map/map_page.h"
#include "myvendor_sound.h"
#include "mtp_features.h"
#include "transfer_backend.h"   /* xfer_usb_page_set()：USB 锁/标志强绑定 MTP 页 */

#include "lvgl/lvgl.h"
#include <string.h>
#include <syslog.h>

#define MTP_PLUG_DEBOUNCE_MS 800u

/**
 * USB MTP 才拆地图存储。BLE companion 每次 READ 也会 lfs_hold（让 vmap 跳过
 * 新的瓦片 I/O），若当成 cover 则会 release + 重载 16 块瓦片，把 ~100 KB/s
 * 的 RLE 下载打成约 2 s / 11 KB。
 */
static bool usb_mtp_covers_map(const myvendor_mtp_status_t * st)
{
    return st->transfer_active
        || st->plug >= MYVENDOR_MTP_PLUG_ENUM
        || st->session_open;
}

static bool host_attached(const myvendor_mtp_status_t * st, const bicycle_mtp_ui_t * ui)
{
    if (st->plug == MYVENDOR_MTP_PLUG_OFF) {
        return false;
    }

    if (st->plug == MYVENDOR_MTP_PLUG_ACTIVE) {
        return true;
    }

    if (ui->need_off_before_enter) {
        return false;
    }

    if (ui->transfer_active_seen && !st->transfer_active) {
        return false;
    }

    return st->ep_present;
}

static void update_plug_edges(bicycle_mtp_ui_t * ui, const myvendor_mtp_status_t * st)
{
    if (ui->last_plug == MYVENDOR_MTP_PLUG_OFF &&
        st->plug >= MYVENDOR_MTP_PLUG_ENUM) {
        ui->need_off_before_enter = false;
        ui->off_sample = false;
    }

    ui->last_plug = st->plug;
}

static void update_off_gate(bicycle_mtp_ui_t * ui, const myvendor_mtp_status_t * st,
                            uint32_t now)
{
    if (!ui->need_off_before_enter) {
        return;
    }

    if (st->plug != MYVENDOR_MTP_PLUG_OFF) {
        ui->off_sample = false;
        return;
    }

    if (!ui->off_sample) {
        ui->off_sample = true;
        ui->off_stable_since = now;
        return;
    }

    if ((now - ui->off_stable_since) >= MTP_PLUG_DEBOUNCE_MS) {
        ui->need_off_before_enter = false;
        ui->off_sample = false;
    }
}

static bool host_attached_stable(bicycle_mtp_ui_t * ui, const myvendor_mtp_status_t * st,
                                 uint32_t now)
{
    const bool attached = host_attached(st, ui);

    if (attached != ui->host_attached_sample) {
        ui->host_attached_sample = attached;
        ui->host_attached_stable_since = now;
    }

    return attached && (now - ui->host_attached_stable_since) >= MTP_PLUG_DEBOUNCE_MS;
}

static bool host_detached_stable(bicycle_mtp_ui_t * ui, const myvendor_mtp_status_t * st,
                                 uint32_t now)
{
    const bool attached = host_attached(st, ui);

    if (attached != ui->host_attached_sample) {
        ui->host_attached_sample = attached;
        ui->host_attached_stable_since = now;
    }

    return !attached && (now - ui->host_attached_stable_since) >= MTP_PLUG_DEBOUNCE_MS;
}

static void try_storage_reload(bicycle_mtp_ui_t * ui)
{
    myvendor_mtp_status_t st;
    map_page_t * map;

    if (!ui->storage_reload_pending) {
        return;
    }

    myvendor_mtp_get_status(&st);
    if (st.plug != MYVENDOR_MTP_PLUG_OFF) {
        return;
    }

    map = lvgl_page_map();
    if (!map) {
        (void)map_page_boot_begin_load();
        map = lvgl_page_map();
    }

    if (map) {
        map_page_resume_after_cover(map);
        map_page_reload_storage(map);
    }

    ui->storage_reload_pending = false;
}

static bool enter_allowed(const bicycle_mtp_ui_t * ui)
{
    (void)ui;

    if (!lvgl_page_app_started()) {
        return false;
    }

    if (lvgl_page_is_usb_transfer()) {
        return false;
    }

    return true;
}

static void finish_leave(bicycle_mtp_ui_t * ui)
{
    /* 真退出页面才清这一位：USB 锁与 app 的"USB 占用"都强绑定在它上面。 */
    xfer_usb_page_set(false);
    ui->leaving = false;
    ui->storage_reload_pending = true;
    ui->storage_released = false;
    ui->need_off_before_enter = true;
    ui->off_sample = false;
    ui->transfer_active_seen = false;
    try_storage_reload(ui);
    bicycle_runtime_emit_mtp_evt(false, false);
    LV_LOG_INFO("MTP UI leave transfer (LiveMap resume)");
}

static void try_leave(bicycle_mtp_ui_t * ui, const myvendor_mtp_status_t * st, uint32_t now)
{
    if (ui->leaving) {
        if (lvgl_page_is_usb_transfer() && lvgl_page_pop_usb_transfer() != 0) {
            return;
        }

        finish_leave(ui);
        return;
    }

    if (!lvgl_page_is_usb_transfer()) {
        return;
    }

    if (!host_detached_stable(ui, st, now)) {
        return;
    }

    ui->leaving = true;

    if (lvgl_page_pop_usb_transfer() != 0) {
        ui->leaving = false;
        return;
    }

    finish_leave(ui);
}

static void try_enter(bicycle_mtp_ui_t * ui, const myvendor_mtp_status_t * st, uint32_t now)
{
    if (!host_attached_stable(ui, st, now)) {
        return;
    }

    if (!enter_allowed(ui)) {
        return;
    }

    if (lvgl_page_push_usb_transfer() == 0) {
        /* 页面弹出 = 占用（LFS 锁 + app 的 USB_MTP_BUSY 都看这一位）；
         * 页面不在（比如现在这块屏是地图）= 一律不占用。 */
        xfer_usb_page_set(true);
        bicycle_runtime_emit_mtp_evt(true, st->transfer_active);
        myvendor_sound_prompt();
#if MTP_FEAT_INFO
        syslog(LOG_NOTICE, "MTP UI enter transfer plug=%d", (int)st->plug);
#endif
    } else {
        syslog(LOG_WARNING, "MTP push UsbTransfer failed busy=%d cur=%d",
            lvgl_page_nav_busy() ? 1 : 0, (int)lvgl_page_current_id());
    }
}

/**
 * @brief 自行车 mtp ui init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_mtp_ui_init(bicycle_mtp_ui_t * ui)
{
    ui->leaving = false;
    ui->host_attached_sample = false;
    ui->host_attached_stable_since = lv_tick_get();
    ui->need_off_before_enter = false;
    ui->off_sample = false;
    ui->off_stable_since = lv_tick_get();
    ui->storage_reload_pending = false;
    ui->storage_released = false;
    ui->transfer_active_seen = false;
    ui->last_plug = MYVENDOR_MTP_PLUG_OFF;
}

/**
 * @brief 自行车 mtp ui run。
 */
void bicycle_mtp_ui_run(bicycle_mtp_ui_t * ui)
{
    myvendor_mtp_status_t st;
    const uint32_t now = lv_tick_get();

    myvendor_mtp_get_status(&st);

    if (usb_mtp_covers_map(&st)) {
        if (!ui->storage_released) {
            map_page_t * map = lvgl_page_map();

            if (map) {
                map_page_pause_for_cover(map);
                map_page_release_storage(map);
            }

            ui->storage_released = true;
        }
    } else if (ui->storage_released && !lvgl_page_is_usb_transfer() &&
               !ui->leaving) {
        ui->storage_reload_pending = true;
        try_storage_reload(ui);
        ui->storage_released = false;
    }

    if (st.transfer_active) {
        ui->transfer_active_seen = true;
    }

    update_plug_edges(ui, &st);
    update_off_gate(ui, &st, now);
    try_storage_reload(ui);

    if (lvgl_page_is_usb_transfer() || ui->leaving) {
        try_leave(ui, &st, now);
        return;
    }

    try_enter(ui, &st, now);
}
