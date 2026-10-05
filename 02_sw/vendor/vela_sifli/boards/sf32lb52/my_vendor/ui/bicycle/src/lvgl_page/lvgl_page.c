/**
 * @file lvgl_page.c
 * @brief Bicycle LVGL 入口：注册页面、开机栈、主题/语言与 MTP 叠层。
 *
 * 产品开机走 `lvgl_page_open_boot_sequence()`。见 `doc/splash.md`。
 */
#include "lvgl_page.h"

#include "bicycle_page_anima.h"
#include "bicycle_runtime.h"
#include "helm_palette.h"
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
#include "factory/factory_page.h"
#endif
#include "usb_transfer/usb_transfer_page.h"
#include "startup/startup_page.h"

#include "lv_pm_core.h"
#include "lv_pm_port.h"
#include "lv_pm_theme.h"
#include "lv_pm_i18n.h"
#include "bicycle_status_bar.h"
#include "lv_pm_bar.h"
#include "vmap/vmap_config.h"
#include "live_map/map_page.h"
#include "helm_menu.h"
#include "myvendor_identity.h"
#include "lvgl/lvgl.h"

static bool s_app_started;

static void lvgl_page_on_nav(lv_pm_page_t page, lv_pm_nav_evt_t evt, void * user_data)
{
    bicycle_page_evt_kind_t kind;

    LV_UNUSED(user_data);

    if (page == NULL) {
        return;
    }

    switch (evt) {
    case LV_PM_NAV_EVT_APPEAR_DONE:
        if (page->id == (lv_pm_id)BICYCLE_PM_ID_STARTUP) {
            startup_page_sync_boot();
        }
        kind = BICYCLE_PAGE_EVT_APPEARED;
        break;
    case LV_PM_NAV_EVT_DISAPPEAR_DONE:
        kind = BICYCLE_PAGE_EVT_DISAPPEARED;
        break;
    default:
        return;
    }

    bicycle_runtime_emit_page_evt((uint8_t)page->id, page->name, kind);
}

/**
 * @brief 初始化 lv_pm、注册页面、套默认主题与状态栏。
 */
void lvgl_page_init(void)
{
    if (lv_pm_init((int)BICYCLE_PM_ID_COUNT) != 0) {
        LV_LOG_ERROR("lvgl_page: lv_pm_init failed");
        return;
    }

    bicycle_runtime_ui_init();
    lv_pm_set_nav_notify_cb(lvgl_page_on_nav, NULL);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);

    if (myvendor_is_factory()) {
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
        factory_page_register();
        LV_LOG_INFO("lvgl_page: factory page (no TTF / LiveMap)");
        return;
#else
        LV_LOG_ERROR("lvgl_page: factory image missing CONFIG_MYVENDOR_FACTORY_MODE");
        return;
#endif
    }

    live_map_page_register();
    usb_transfer_page_register();
    startup_page_register();
    helm_menu_page_register();

    lv_pm_theme_set(0);
    /* 把 helm 调色板挂到 lv_pm 的主题框架上：主题一变就换表 + 整屏失效。 */
    helm_palette_follow_theme_hook();
    lv_pm_i18n_set(LV_PM_I18N_DEFAULT_LOCALE);
    lv_pm_status_bar_set_height(VMAP_STATUSBAR_H);
    bicycle_status_bar_init();

    LV_LOG_INFO("lvgl_page: pages registered");
}

/**
 * @brief 只打开 Startup（调试用）。产品开机请用 boot_sequence。
 */
int lvgl_page_open_startup(void)
{
    return lv_pm_open_page_msg((lv_pm_id)BICYCLE_PM_ID_STARTUP, NULL);
}

/**
 * @brief 静默 push LiveMap 空壳，再 push Startup splash。
 */
int lvgl_page_open_boot_sequence(void)
{
    lv_pm_page_t map;

    if (myvendor_is_factory()) {
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
        return lv_pm_open_page_msg((lv_pm_id)BICYCLE_PM_ID_FACTORY, NULL);
#else
        return -1;
#endif
    }

    map = live_map_page_def();

    /* splash 底下的空壳；vmap / TTF 在白屏事项里再加载。 */
    map_page_set_boot_deferred(true);

    if (map) {
        bicycle_page_anima_apply_silent(map);
    }

    if (lvgl_page_open_home() != 0) {
        map_page_set_boot_deferred(false);
        return -1;
    }

    if (lvgl_page_open_startup() != 0) {
        map_page_set_boot_deferred(false);
        return -1;
    }

    return 0;
}

/**
 * @brief 打开（或复用 cache 的）LiveMap。
 */
int lvgl_page_open_home(void)
{
    return lv_pm_open_page_msg((lv_pm_id)BICYCLE_PM_ID_MAP, NULL);
}

/**
 * @brief 当前 LiveMap 实例。
 */
map_page_t * lvgl_page_map(void)
{
    return live_map_page_instance();
}

/**
 * @brief 栈顶页面 ID。
 */
bicycle_pm_id_t lvgl_page_current_id(void)
{
    lv_pm_page_t page = lv_pm_get_crr_page();

    if (page == NULL) {
        return (bicycle_pm_id_t)0xff;
    }

    return (bicycle_pm_id_t)page->id;
}

/**
 * @brief 栈顶是否为 UsbTransfer。
 */
bool lvgl_page_is_usb_transfer(void)
{
    return lvgl_page_current_id() == BICYCLE_PM_ID_USB_TRANSFER;
}

/**
 * @brief 转场动画尚未结束。
 */
bool lvgl_page_nav_busy(void)
{
    return lv_pm_nav_busy();
}

/**
 * @brief 当前主题名。
 */
const char * lvgl_page_theme_name(void)
{
    return lv_pm_theme_name(lv_pm_theme_get());
}

/**
 * @brief 按名字切主题。
 */
bool lvgl_page_theme_set_by_name(const char * name)
{
    return lv_pm_theme_set_by_name(name);
}

/**
 * @brief 循环主题。
 */
bool lvgl_page_theme_cycle(void)
{
    map_page_t * map = lvgl_page_map();

    return map_page_cycle_style(map);
}

/**
 * @brief 当前 locale 名。
 */
const char * lvgl_page_locale_name(void)
{
    return lv_pm_i18n_locale_name(lv_pm_i18n_get());
}

/**
 * @brief 按名字切 locale。
 */
bool lvgl_page_locale_set_by_name(const char * name)
{
    return lv_pm_i18n_set_by_name(name);
}

/**
 * @brief 循环 locale。
 */
bool lvgl_page_locale_cycle(void)
{
    return lv_pm_i18n_set_next();
}

/**
 * @brief push UsbTransfer overlay。
 */
int lvgl_page_push_usb_transfer(void)
{
    return lv_pm_open_page_msg((lv_pm_id)BICYCLE_PM_ID_USB_TRANSFER, NULL);
}

/**
 * @brief pop UsbTransfer，露出 LiveMap。
 */
int lvgl_page_pop_usb_transfer(void)
{
    return lv_pm_close_page_msg(NULL);
}

/**
 * @brief 标记开机 splash 是否已结束。
 */
void lvgl_page_set_app_started(bool started)
{
    s_app_started = started;
}

/**
 * @brief splash 已结束、主界面可交互。
 */
bool lvgl_page_app_started(void)
{
    return s_app_started;
}
