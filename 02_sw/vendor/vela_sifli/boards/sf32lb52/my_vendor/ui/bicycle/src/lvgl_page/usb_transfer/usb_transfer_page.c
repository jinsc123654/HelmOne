/**
 * @file usb_transfer_page.c
 * @brief USB MTP：对齐 pages/usb.html。
 */

#include "usb_transfer_page.h"

#include "bicycle_page_anima.h"
#include "bicycle_page_ids.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "lv_pm_anima.h"
#include "lv_pm_core.h"
#include "lv_pm_port.h"
#include "myvendor_mtp.h"

#include <stdint.h>

struct page_usb_transfer {
    lv_obj_t * root;
    lv_obj_t * hero_t;
    lv_obj_t * hero_s;
    lv_obj_t * pbar;
};

static page_usb_transfer_t * s_usb_page;
static uint32_t s_act_seq;
static uint64_t s_act_done;
static myvendor_mtp_op_t s_act_op;

static const char * mtp_op_text(myvendor_mtp_op_t op)
{
    switch (op) {
    case MYVENDOR_MTP_OP_UPLOAD:
        return "正在写入";
    case MYVENDOR_MTP_OP_DOWNLOAD:
        return "正在导出";
    case MYVENDOR_MTP_OP_MKDIR:
        return "正在建目录";
    case MYVENDOR_MTP_OP_DELETE:
        return "正在删除";
    case MYVENDOR_MTP_OP_RENAME:
        return "正在重命名";
    case MYVENDOR_MTP_OP_MOVE:
        return "正在移动";
    case MYVENDOR_MTP_OP_COPY:
        return "正在复制";
    default:
        return "MTP 传输中";
    }
}

void usb_transfer_page_poll(page_usb_transfer_t * ui)
{
    myvendor_mtp_activity_t act;
    uint8_t pct = 0;

    if (!ui || !ui->hero_t) {
        return;
    }

    myvendor_mtp_get_activity(&act);

    if (!act.active || act.op == MYVENDOR_MTP_OP_IDLE) {
        s_act_seq = 0;
        s_act_done = 0;
        lv_label_set_text(ui->hero_t, "MTP 传输中");
        if (ui->pbar) {
            lv_obj_add_flag(ui->pbar, LV_OBJ_FLAG_HIDDEN);
        }

        return;
    }

    if (act.seq != s_act_seq || act.op != s_act_op) {
        s_act_seq = act.seq;
        s_act_op = act.op;
        s_act_done = UINT64_MAX;
        lv_label_set_text(ui->hero_t, mtp_op_text(act.op));
    }

    if (act.total_bytes > 0) {
        if (act.done_bytes >= act.total_bytes) {
            pct = 100;
        } else {
            pct = (uint8_t)((act.done_bytes * 100ULL) / act.total_bytes);
        }

        if (ui->pbar) {
            lv_obj_clear_flag(ui->pbar, LV_OBJ_FLAG_HIDDEN);
            if (act.done_bytes != s_act_done) {
                s_act_done = act.done_bytes;
                helm_pbar_set(ui->pbar, pct);
            }
        }
    } else if (ui->pbar) {
        lv_obj_add_flag(ui->pbar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void usb_build_ui(lv_pm_page_t page, page_usb_transfer_t * ui)
{
    lv_obj_t * root = page->page;
    lv_obj_t * wrap;
    lv_obj_t * hero;
    lv_obj_t * kv;

    ui->root = root;
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    helm_style_scr(root);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    wrap = lv_obj_create(root);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, PAGE_HOR_RES, HELM_PAGE_H);
    helm_style_scr(wrap);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    helm_mhead_create(wrap, "USB");

    hero = helm_sheet_hero(wrap, HELM_ICO_USB, false);
    ui->hero_t = helm_label(hero, helm_font_title(), HELM_COLOR_INK, "MTP 传输中");
    ui->hero_s = helm_label(hero, helm_font_lab(), HELM_COLOR_HR, "请勿断电");
    ui->pbar = helm_pbar_create(hero);
    lv_obj_add_flag(ui->pbar, LV_OBJ_FLAG_HIDDEN);

    kv = helm_kvbox_create(wrap);
    helm_kv_add(kv, HELM_ICO_PIN, "地图", "可写入");
    helm_kv_add(kv, HELM_ICO_GPX, "轨迹", "可导出");
    helm_kv_add(kv, HELM_ICO_CHIP, "维护", "可传文件");
    helm_kvbox_seal(kv);
}

static page_usb_transfer_t * usb_ui_ensure(lv_pm_page_t page)
{
    page_usb_transfer_t * ui;

    if (page == NULL || page->page == NULL) {
        return NULL;
    }

    if (page->user_data != NULL) {
        s_usb_page = (page_usb_transfer_t *)page->user_data;
        return s_usb_page;
    }

    ui = (page_usb_transfer_t *)lv_pm_malloc(sizeof(*ui));
    if (!ui) {
        return NULL;
    }

    lv_memzero(ui, sizeof(*ui));
    page->user_data = ui;
    s_usb_page = ui;
    usb_build_ui(page, ui);
    return ui;
}

static void usb_on_load(void * pm_page)
{
    usb_ui_ensure(lv_pm_get_pm_page(pm_page));
}

static void usb_on_unload(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    page_usb_transfer_t * ui;

    if (page == NULL) {
        return;
    }

    ui = (page_usb_transfer_t *)page->user_data;
    if (!ui) {
        return;
    }

    lv_pm_free(ui);
    page->user_data = NULL;
    s_usb_page = NULL;
    s_act_seq = 0;
    s_act_done = 0;
}

static void usb_will_appear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    page_usb_transfer_t * ui = usb_ui_ensure(page);

    if (!ui || !page || !page->page) {
        return;
    }

    lv_obj_move_foreground(page->page);
    lv_obj_clear_flag(page->page, LV_OBJ_FLAG_HIDDEN);
}

static void usb_did_appear(void * pm_page)
{
    page_usb_transfer_t * ui = usb_ui_ensure(lv_pm_get_pm_page(pm_page));

    if (ui) {
        usb_transfer_page_poll(ui);
    }
}

static void usb_will_disappear(void * pm_page)
{
    (void)pm_page;
}

void usb_transfer_page_register(void)
{
    lv_pm_page_t page = lv_pm_create_page((lv_pm_id)BICYCLE_PM_ID_USB_TRANSFER,
                                          "UsbTransfer");

    if (page == NULL) {
        LV_LOG_ERROR("page_usb_transfer: register failed");
        return;
    }

    lv_pm_set_open(page, usb_on_load);
    lv_pm_set_will_appear(page, usb_will_appear);
    lv_pm_set_dis_appear(page, usb_did_appear);
    lv_pm_set_will_disappear(page, usb_will_disappear);
    lv_pm_set_close(page, usb_on_unload);
    bicycle_page_anima_apply(page, BICYCLE_PM_ID_USB_TRANSFER);
}

void usb_transfer_page_poll_active(void)
{
    if (s_usb_page) {
        usb_transfer_page_poll(s_usb_page);
    }
}
