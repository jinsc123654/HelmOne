/**
 * @file factory_page.c
 * @brief 工厂模式首页：与主菜单相同的 KEY1 列表 / KEY2 确认。
 */

#include "factory_page.h"
#include "factory_board_test.h"

#include <nuttx/config.h>

#include "Vendor/Board/lv_port/lv_port_buttons.h"

#include "bicycle_page_anima.h"
#include "bicycle_page_ids.h"
#include "companion_proto.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "lv_pm_core.h"
#include "lv_pm_port.h"
#include "myvendor_fw_slot.h"
#include "myvendor_identity.h"
#include "myvendor_mtp.h"
#include "myvendor_sound.h"

#include "lvgl/lvgl.h"

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
#include "companion_bridge.h"
#endif

#include <string.h>

#define FACTORY_POLL_MS 400u

enum {
    FAC_ROW_TEST = 0,
    FAC_ROW_BOOT,
    FAC_ROW_DEL,
    FAC_ROW_MTP,
    FAC_ROW_BLE,
    FAC_ROW_SN,
    FAC_ROW_FW,
    FAC_ROW_N
};

enum {
    FAC_BOOT_FW = 0,
    FAC_BOOT_MAIN,
    FAC_BOOT_N
};

typedef struct page_factory {
    lv_obj_t * root;
    lv_obj_t * home_wrap;
    lv_obj_t * boot_wrap;
    lv_obj_t * list;
    lv_obj_t * boot_list;
    lv_obj_t * pbar;
    lv_obj_t * fw_mask;
    lv_timer_t * poll_timer;
    uint8_t sel;
    uint8_t boot_sel;
    bool fw_confirm;
    bool boot_pick;
    char mtp_val[24];
    char mtp_sub[40];
    char ble_val[24];
    char sn_val[24];
    char sn_sub[32];
    char fw_val[48];
    char fw_sub[16];
    char boot_val[16];
    char del_sub[40];
} page_factory_t;

static page_factory_t * s_factory;

static void factory_home_key1(void * ud);
static void factory_home_key2(void * ud);
static void factory_test_key_prev(void * ud);
static void factory_test_key_next(void * ud);
static void factory_test_key_exit(void * ud);
static void factory_test_key_retest(void * ud);
static void factory_fw_cancel(void * ud);
static void factory_fw_confirm(void * ud);
static void factory_boot_key1(void * ud);
static void factory_boot_key2(void * ud);
static void factory_boot_cancel(void * ud);
static void factory_bind_home_keys(void);
static void factory_bind_boot_keys(void);
static void factory_fw_close(page_factory_t * ui);
static void factory_boot_close(page_factory_t * ui);
static void factory_boot_open(page_factory_t * ui);
static void factory_boot_paint(page_factory_t * ui);
static void factory_refresh(page_factory_t * ui);
static void factory_home_paint(page_factory_t * ui);
static void factory_build_boot_ui(page_factory_t * ui, lv_obj_t * root);

static void factory_bind_home_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(factory_home_key1, s_factory);
    lv_port_buttons_set_page_confirm_cb(factory_home_key2, s_factory);
    lv_port_buttons_set_page_longpress_cb(NULL, NULL);
    lv_port_buttons_set_page_longpress2_cb(NULL, NULL);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
}

static void factory_bind_fw_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(factory_fw_cancel, s_factory);
    lv_port_buttons_set_page_confirm_cb(factory_fw_confirm, s_factory);
    lv_port_buttons_set_page_longpress_cb(factory_fw_cancel, s_factory);
    lv_port_buttons_set_page_longpress2_cb(NULL, NULL);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
}

static void factory_bind_boot_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(factory_boot_key1, s_factory);
    lv_port_buttons_set_page_confirm_cb(factory_boot_key2, s_factory);
    lv_port_buttons_set_page_longpress_cb(factory_boot_cancel, s_factory);
    lv_port_buttons_set_page_longpress2_cb(NULL, NULL);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
}

static void factory_bind_test_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(factory_test_key_prev, s_factory);
    lv_port_buttons_set_page_confirm_cb(factory_test_key_next, s_factory);
    lv_port_buttons_set_page_longpress_cb(factory_test_key_exit, s_factory);
    lv_port_buttons_set_page_longpress2_cb(factory_test_key_retest, s_factory);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
}

static void factory_fw_close(page_factory_t * ui)
{
    if (ui == NULL) {
        return;
    }

    if (ui->fw_mask != NULL) {
        lv_obj_delete(ui->fw_mask);
        ui->fw_mask = NULL;
    }

    ui->fw_confirm = false;
}

static void factory_boot_close(page_factory_t * ui)
{
    if (ui == NULL) {
        return;
    }

    if (ui->boot_wrap) {
        lv_obj_add_flag(ui->boot_wrap, LV_OBJ_FLAG_HIDDEN);
    }

    ui->boot_pick = false;
}

static void factory_home_restore(page_factory_t * ui)
{
    if (ui == NULL) {
        return;
    }

    factory_fw_close(ui);
    factory_boot_close(ui);
    if (ui->home_wrap) {
        lv_obj_clear_flag(ui->home_wrap, LV_OBJ_FLAG_HIDDEN);
    }

    factory_bind_home_keys();
    factory_refresh(ui);
}

static void factory_fill_spec(page_factory_t * ui, uint8_t i, helm_item_t * spec)
{
    memset(spec, 0, sizeof(*spec));
    spec->sel = (ui->sel == i);

    switch (i) {
    case FAC_ROW_TEST:
        spec->ico = HELM_ICO_CHIP;
        spec->title = "板卡测试";
        spec->sub = "外设分页校验";
        spec->kind = HELM_ITEM_GO;
        break;
    case FAC_ROW_BOOT:
        spec->ico = HELM_ICO_GEAR;
        spec->title = "启动";
        spec->sub = "fw / main";
        spec->value = ui->boot_val;
        spec->kind = HELM_ITEM_GO;
        break;
    case FAC_ROW_DEL:
        spec->ico = HELM_ICO_XMARK;
        spec->title = "删除最老固件";
        spec->sub = ui->del_sub[0] ? ui->del_sub : "删除最老一份";
        spec->kind = HELM_ITEM_GO;
        break;
    case FAC_ROW_MTP:
        spec->ico = HELM_ICO_USB;
        spec->title = "MTP";
        spec->sub = ui->mtp_sub;
        spec->value = ui->mtp_val;
        spec->kind = HELM_ITEM_VAL;
        break;
    case FAC_ROW_BLE:
        spec->ico = HELM_ICO_BLE;
        spec->title = "BLE";
        spec->sub = "已强制开启";
        spec->value = ui->ble_val;
        spec->kind = HELM_ITEM_VAL;
        break;
    case FAC_ROW_SN:
        spec->ico = HELM_ICO_LIST;
        spec->title = "序列号";
        spec->sub = ui->sn_sub;
        spec->value = ui->sn_val;
        spec->kind = HELM_ITEM_VAL;
        break;
    case FAC_ROW_FW:
        spec->ico = HELM_ICO_SAVE;
        spec->title = "固件";
        spec->sub = ui->fw_sub;
        spec->value = ui->fw_val;
        spec->kind = HELM_ITEM_VAL;
        break;
    default:
        break;
    }
}

static void factory_home_paint(page_factory_t * ui)
{
    helm_item_t spec;
    uint8_t i;
    bool rebuild;

    if (ui == NULL || ui->list == NULL) {
        return;
    }

    rebuild = (lv_obj_get_child_count(ui->list) != FAC_ROW_N);
    if (rebuild) {
        lv_obj_clean(ui->list);
    }

    for (i = 0; i < FAC_ROW_N; i++) {
        factory_fill_spec(ui, i, &spec);
        if (rebuild) {
            helm_mitem_create(ui->list, &spec);
        } else if (!helm_mitem_refresh(lv_obj_get_child(ui->list, i), &spec)) {
            rebuild = true;
            lv_obj_clean(ui->list);
            i = 0;
            factory_fill_spec(ui, i, &spec);
            helm_mitem_create(ui->list, &spec);
        }
    }

    if (rebuild) {
        helm_obj_stagger_in(ui->list);
    }
}

static void factory_fw_open(page_factory_t * ui)
{
    struct myvendor_fw_slot_info info[MYVENDOR_FW_SLOT_SCAN_MAX];
    lv_obj_t * card;
    lv_obj_t * body;
    lv_obj_t * lab;
    unsigned n;

    if (ui == NULL || ui->root == NULL || ui->fw_confirm || ui->boot_pick ||
        factory_board_test_active()) {
        return;
    }

    n = myvendor_fw_slot_list(info, MYVENDOR_FW_SLOT_SCAN_MAX);
    if (n < 2) {
        lv_snprintf(ui->del_sub, sizeof(ui->del_sub),
                    n == 0 ? "无固件" : "仅一份，无法删除");
        factory_home_paint(ui);
        myvendor_sound_warn();
        return;
    }

    ui->fw_mask = helm_mask_create(ui->root);
    card = helm_card_create(ui->fw_mask, HELM_ICO_XMARK, "删除最老固件？", true);
    body = helm_card_body(card);
    lab = helm_label(body, helm_font_title(), HELM_COLOR_INK, info[n - 1u].name);
    lv_obj_set_width(lab, lv_pct(100));
    lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    helm_label(body, helm_font_lab(), HELM_COLOR_INK, "将删除最老一份");
    helm_softkeys_create(body, HELM_COLOR_INK);
    lv_obj_move_foreground(ui->fw_mask);

    ui->fw_confirm = true;
    factory_bind_fw_keys();
    myvendor_sound_prompt();
}

static const char * target_text(void)
{
    switch (myvendor_boot_target()) {
    case COMPANION_SLOT_FW:
        return "fw";
    case COMPANION_SLOT_MAIN:
        return "main";
    case COMPANION_SLOT_FACTORY:
        return "factory";
    default:
        return "-";
    }
}

static void factory_boot_fill_spec(page_factory_t * ui, uint8_t i,
                                  helm_item_t * spec)
{
    memset(spec, 0, sizeof(*spec));
    spec->sel = (ui->boot_sel == i);
    spec->kind = HELM_ITEM_GO;
    if (i == FAC_BOOT_FW) {
        spec->ico = HELM_ICO_SAVE;
        spec->title = "fw";
        spec->sub = "KV /fw";
        spec->value = (ui->boot_sel == FAC_BOOT_FW) ? "当前" : "";
    } else {
        spec->ico = HELM_ICO_CHIP;
        spec->title = "main";
        spec->sub = "nsh";
        spec->value = (ui->boot_sel == FAC_BOOT_MAIN) ? "当前" : "";
    }
}

static void factory_boot_paint(page_factory_t * ui)
{
    helm_item_t spec;
    uint8_t i;
    bool rebuild;

    if (ui == NULL || ui->boot_list == NULL) {
        return;
    }

    rebuild = (lv_obj_get_child_count(ui->boot_list) != FAC_BOOT_N);
    if (rebuild) {
        lv_obj_clean(ui->boot_list);
    }

    for (i = 0; i < FAC_BOOT_N; i++) {
        factory_boot_fill_spec(ui, i, &spec);
        if (rebuild) {
            helm_mitem_create(ui->boot_list, &spec);
        } else if (!helm_mitem_refresh(lv_obj_get_child(ui->boot_list, i),
                                       &spec)) {
            rebuild = true;
            lv_obj_clean(ui->boot_list);
            i = 0;
            factory_boot_fill_spec(ui, i, &spec);
            helm_mitem_create(ui->boot_list, &spec);
        }
    }

    if (rebuild) {
        helm_obj_stagger_in(ui->boot_list);
    }
}

static void factory_boot_open(page_factory_t * ui)
{
    uint8_t cur;

    if (ui == NULL || ui->boot_wrap == NULL || ui->fw_confirm ||
        ui->boot_pick || factory_board_test_active()) {
        return;
    }

    cur = myvendor_boot_target();
    ui->boot_sel = (cur == COMPANION_SLOT_MAIN) ? FAC_BOOT_MAIN : FAC_BOOT_FW;
    if (ui->home_wrap) {
        lv_obj_add_flag(ui->home_wrap, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(ui->boot_wrap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->boot_wrap);
    ui->boot_pick = true;
    factory_boot_paint(ui);
    helm_mlist_sel_snap(ui->boot_list, ui->boot_sel);
    factory_bind_boot_keys();
    myvendor_sound_ok();
}

static void factory_boot_key1(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;
    uint8_t from;

    if (ui == NULL || !ui->boot_pick || ui->boot_list == NULL) {
        return;
    }

    from = ui->boot_sel;
    ui->boot_sel = (uint8_t)((ui->boot_sel + 1u) % FAC_BOOT_N);
    factory_boot_paint(ui);
    helm_mlist_move_sel(ui->boot_list, from, ui->boot_sel);
    myvendor_sound_key();
}

static void factory_boot_key2(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;
    uint8_t slot;
    int ret;

    if (ui == NULL || !ui->boot_pick) {
        return;
    }

    helm_mlist_press_sel(ui->boot_list, ui->boot_sel);
    slot = (ui->boot_sel == FAC_BOOT_MAIN) ? COMPANION_SLOT_MAIN :
                                            COMPANION_SLOT_FW;
    ret = myvendor_boot_target_set(slot);
    factory_home_restore(ui);
    if (ret == 0) {
        myvendor_sound_ok();
    } else {
        myvendor_sound_warn();
    }
}

static void factory_boot_cancel(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;

    factory_home_restore(ui);
    myvendor_sound_back();
}

static void factory_home_key1(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;
    uint8_t from;

    if (ui == NULL || ui->list == NULL || ui->fw_confirm || ui->boot_pick ||
        factory_board_test_active()) {
        return;
    }

    from = ui->sel;
    ui->sel = (uint8_t)((ui->sel + 1u) % FAC_ROW_N);
    helm_mlist_move_sel(ui->list, from, ui->sel);
    myvendor_sound_key();
}

static void factory_fw_cancel(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;

    factory_fw_close(ui);
    factory_home_restore(ui);
    myvendor_sound_back();
}

static void factory_fw_confirm(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;
    char name[MYVENDOR_FW_SLOT_NAME_MAX];
    int ret;

    if (ui == NULL || !ui->fw_confirm) {
        return;
    }

    memset(name, 0, sizeof(name));
    ret = myvendor_fw_slot_delete_oldest(name, sizeof(name));
    factory_fw_close(ui);
    factory_bind_home_keys();

    if (ret == 0) {
        myvendor_sound_ok();
    } else {
        myvendor_sound_warn();
    }

    ui->del_sub[0] = '\0';
    factory_refresh(ui);
}

static void factory_home_key2(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;

    if (ui == NULL || ui->root == NULL || factory_board_test_active() ||
        ui->fw_confirm || ui->boot_pick) {
        return;
    }

    helm_mlist_press_sel(ui->list, ui->sel);

    switch (ui->sel) {
    case FAC_ROW_TEST:
        myvendor_sound_ok();
        if (ui->home_wrap) {
            lv_obj_add_flag(ui->home_wrap, LV_OBJ_FLAG_HIDDEN);
        }

        factory_board_test_begin(ui->root);
        factory_bind_test_keys();
        break;
    case FAC_ROW_BOOT:
        factory_boot_open(ui);
        break;
    case FAC_ROW_DEL:
        factory_fw_open(ui);
        break;
    default:
        helm_obj_squash_pulse(lv_obj_get_child(ui->list, ui->sel));
        myvendor_sound_ok();
        break;
    }
}

static void factory_test_key_prev(void * ud)
{
    LV_UNUSED(ud);
    factory_board_test_prev();
}

static void factory_test_key_next(void * ud)
{
    LV_UNUSED(ud);
    factory_board_test_next();
}

static void factory_test_key_exit(void * ud)
{
    page_factory_t * ui = (page_factory_t *)ud;

    factory_board_test_exit();
    factory_home_restore(ui);
}

static void factory_test_key_retest(void * ud)
{
    LV_UNUSED(ud);
    factory_board_test_retest();
}

static const char * slot_text(void)
{
    switch (myvendor_boot_slot()) {
    case COMPANION_SLOT_FW:
        return "fw";
    case COMPANION_SLOT_MAIN:
        return "main";
    case COMPANION_SLOT_FACTORY:
        return "factory";
    default:
        return "-";
    }
}

static const char * mtp_state_text(const myvendor_mtp_status_t * st)
{
    if (!st->worker_running) {
        return "启动中";
    }

    switch (st->plug) {
    case MYVENDOR_MTP_PLUG_ACTIVE:
        return "传输中";
    case MYVENDOR_MTP_PLUG_ENUM:
        return "已连接";
    default:
        return "已开启";
    }
}

static const char * ble_state_text(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
    if (!companion_bridge_alive_get()) {
        return "启动中";
    }

    return "已开启";
#else
    return "-";
#endif
}

static void factory_refresh(page_factory_t * ui)
{
    myvendor_mtp_status_t st;
    myvendor_mtp_activity_t act;
    uint8_t pct = 0;
    struct myvendor_fw_slot_info info[MYVENDOR_FW_SLOT_KEEP];
    unsigned n;
    const char * sn;

    if (!ui) {
        return;
    }

    memset(&st, 0, sizeof(st));
    memset(&act, 0, sizeof(act));
    (void)myvendor_mtp_get_status(&st);
    (void)myvendor_mtp_get_activity(&act);

    lv_snprintf(ui->mtp_val, sizeof(ui->mtp_val), "%s", mtp_state_text(&st));
    if (act.active && act.op != MYVENDOR_MTP_OP_IDLE) {
        lv_snprintf(ui->mtp_sub, sizeof(ui->mtp_sub), "%s",
                    act.target[0] ? act.target : "MTP");
        if (act.total_bytes > 0) {
            if (act.done_bytes >= act.total_bytes) {
                pct = 100;
            } else {
                pct = (uint8_t)((act.done_bytes * 100ULL) / act.total_bytes);
            }
        }

        if (ui->pbar) {
            lv_obj_clear_flag(ui->pbar, LV_OBJ_FLAG_HIDDEN);
            helm_pbar_set(ui->pbar, pct);
        }
    } else {
        lv_snprintf(ui->mtp_sub, sizeof(ui->mtp_sub), "数据通道");
        if (ui->pbar) {
            lv_obj_add_flag(ui->pbar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_snprintf(ui->ble_val, sizeof(ui->ble_val), "%s", ble_state_text());

    sn = myvendor_identity_serial();
    lv_snprintf(ui->sn_val, sizeof(ui->sn_val), "%s",
                (sn && sn[0]) ? sn : "-");
    {
        const char * id = myvendor_identity_name();

        lv_snprintf(ui->sn_sub, sizeof(ui->sn_sub), "%s",
                    (id && id[0]) ? id : "-");
    }

    lv_snprintf(ui->fw_sub, sizeof(ui->fw_sub), "%s", slot_text());
    lv_snprintf(ui->boot_val, sizeof(ui->boot_val), "%s", target_text());
    if (!st.lfs_quiesce && !st.transfer_active) {
        n = myvendor_fw_slot_list(info, MYVENDOR_FW_SLOT_KEEP);
        lv_snprintf(ui->fw_val, sizeof(ui->fw_val), "%s",
                    n > 0 ? info[0].name : "-");

        if (n < 2) {
            lv_snprintf(ui->del_sub, sizeof(ui->del_sub),
                        n == 0 ? "无固件" : "仅一份，无法删除");
        } else {
            lv_snprintf(ui->del_sub, sizeof(ui->del_sub), "删除最老一份");
        }
    }

    if (ui->home_wrap && !lv_obj_has_flag(ui->home_wrap, LV_OBJ_FLAG_HIDDEN) &&
        !ui->fw_confirm && !ui->boot_pick) {
        factory_home_paint(ui);
    }
}

static void factory_poll_cb(lv_timer_t * tmr)
{
    page_factory_t * ui = (page_factory_t *)lv_timer_get_user_data(tmr);

    factory_refresh(ui);
}

static void factory_build_ui(lv_pm_page_t page, page_factory_t * ui)
{
    lv_obj_t * root = page->page;
    lv_obj_t * wrap;

    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    helm_style_scr(root);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    wrap = lv_obj_create(root);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, PAGE_HOR_RES, PAGE_VER_RES);
    helm_style_scr(wrap);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    ui->root = root;
    ui->home_wrap = wrap;
    ui->sel = FAC_ROW_TEST;

    {
        lv_obj_t * logo;
        lv_obj_t * ico;
        lv_obj_t * title;
        lv_obj_t * ver;
        char line[48];
        const char * sw = myvendor_sw_version();
        const char * id = myvendor_identity_name();
        const char * bld = myvendor_build_date();

        logo = lv_obj_create(wrap);
        lv_obj_remove_style_all(logo);
        lv_obj_set_width(logo, lv_pct(100));
        lv_obj_set_height(logo, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(logo, 0);
        lv_obj_set_flex_flow(logo, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(logo, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(logo, 10, 0);
        lv_obj_set_style_pad_bottom(logo, 6, 0);
        lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);

        ico = helm_icon_create(logo, HELM_ICO_CHIP, 40);
        helm_icon_set_color(ico, helm_color(HELM_COLOR_NAV));
        lv_obj_set_style_margin_bottom(ico, 6, 0);

        title = helm_label(logo, helm_font_val(), HELM_COLOR_NAV, "工厂模式");
        lv_obj_set_style_text_letter_space(title, 2, 0);

        lv_snprintf(line, sizeof(line), "%s",
                    (id && id[0]) ? id : "-");
        helm_label(logo, helm_font_lab(), HELM_COLOR_INK, line);

        lv_snprintf(line, sizeof(line), "%s · %s",
                    (sw && sw[0]) ? sw : "-", slot_text());
        ver = helm_label(logo, helm_font_lab(), HELM_COLOR_INK, line);
        lv_snprintf(line, sizeof(line), "%s",
                    (bld && bld[0]) ? bld : "-");
        helm_label(logo, helm_font_lab(), HELM_COLOR_INK, line);
        lv_obj_set_style_pad_top(ver, 2, 0);
    }

    ui->list = helm_mlist_create(wrap);
    ui->pbar = helm_pbar_create(wrap);
    lv_obj_add_flag(ui->pbar, LV_OBJ_FLAG_HIDDEN);

    lv_snprintf(ui->mtp_sub, sizeof(ui->mtp_sub), "数据通道");
    lv_snprintf(ui->mtp_val, sizeof(ui->mtp_val), "启动中");
    lv_snprintf(ui->ble_val, sizeof(ui->ble_val), "启动中");
    lv_snprintf(ui->sn_val, sizeof(ui->sn_val), "-");
    lv_snprintf(ui->sn_sub, sizeof(ui->sn_sub), "-");
    lv_snprintf(ui->fw_val, sizeof(ui->fw_val), "-");
    lv_snprintf(ui->fw_sub, sizeof(ui->fw_sub), "%s", slot_text());
    lv_snprintf(ui->boot_val, sizeof(ui->boot_val), "%s", target_text());
    lv_snprintf(ui->del_sub, sizeof(ui->del_sub), "删除最老一份");
    factory_home_paint(ui);
    factory_build_boot_ui(ui, root);
}

static void factory_build_boot_ui(page_factory_t * ui, lv_obj_t * root)
{
    lv_obj_t * wrap;

    wrap = lv_obj_create(root);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, PAGE_HOR_RES, PAGE_VER_RES);
    helm_style_scr(wrap);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(wrap, LV_ALIGN_TOP_LEFT, 0, 0);

    helm_mhead_create(wrap, "启动");
    ui->boot_list = helm_mlist_create(wrap);
    ui->boot_wrap = wrap;
}

static page_factory_t * factory_ui_ensure(lv_pm_page_t page)
{
    page_factory_t * ui;

    if (page == NULL || page->page == NULL) {
        return NULL;
    }

    if (page->user_data != NULL) {
        s_factory = (page_factory_t *)page->user_data;
        return s_factory;
    }

    ui = (page_factory_t *)lv_pm_malloc(sizeof(*ui));
    if (!ui) {
        return NULL;
    }

    lv_memzero(ui, sizeof(*ui));
    page->user_data = ui;
    s_factory = ui;
    factory_build_ui(page, ui);
    return ui;
}

static void factory_on_load(void * pm_page)
{
    factory_ui_ensure(lv_pm_get_pm_page(pm_page));
}

static void factory_on_unload(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    page_factory_t * ui;

    if (page == NULL) {
        return;
    }

    ui = (page_factory_t *)page->user_data;
    if (!ui) {
        return;
    }

    if (ui->poll_timer) {
        lv_timer_del(ui->poll_timer);
        ui->poll_timer = NULL;
    }

    if (factory_board_test_active()) {
        factory_board_test_exit();
    }

    factory_fw_close(ui);
    factory_boot_close(ui);

    lv_pm_free(ui);
    page->user_data = NULL;
    s_factory = NULL;
}

static void factory_will_appear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    page_factory_t * ui = factory_ui_ensure(page);

    if (!ui || !page || !page->page) {
        return;
    }

    lv_obj_move_foreground(page->page);
    lv_obj_clear_flag(page->page, LV_OBJ_FLAG_HIDDEN);
    (void)myvendor_fw_slot_prune();
    factory_refresh(ui);
    if (ui->boot_pick) {
        factory_bind_boot_keys();
    } else if (!ui->fw_confirm && !factory_board_test_active()) {
        factory_bind_home_keys();
    }

    if (ui->poll_timer == NULL) {
        ui->poll_timer = lv_timer_create(factory_poll_cb, FACTORY_POLL_MS, ui);
    }
}

static void factory_will_disappear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    page_factory_t * ui = page ? (page_factory_t *)page->user_data : NULL;

    if (ui && ui->poll_timer) {
        lv_timer_del(ui->poll_timer);
        ui->poll_timer = NULL;
    }

    if (ui) {
        helm_mlist_cursor_off(ui->list);
        helm_mlist_cursor_off(ui->boot_list);
    }
}

void factory_page_register(void)
{
    lv_pm_page_t page = lv_pm_create_page((lv_pm_id)BICYCLE_PM_ID_FACTORY,
                                          "Factory");

    if (page == NULL) {
        LV_LOG_ERROR("factory_page: register failed");
        return;
    }

    lv_pm_set_open(page, factory_on_load);
    lv_pm_set_will_appear(page, factory_will_appear);
    lv_pm_set_will_disappear(page, factory_will_disappear);
    lv_pm_set_close(page, factory_on_unload);
    bicycle_page_anima_apply(page, BICYCLE_PM_ID_FACTORY);
}

void factory_page_poll_active(void)
{
    if (factory_board_test_active()) {
        factory_board_test_poll();
        return;
    }

    if (s_factory) {
        factory_refresh(s_factory);
    }
}
