/**
 * @file boot_sgl.c
 * @brief SGL splash: logo enter, progress, then product name for NuttX.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_sgl.h"
#include "boot_lcd.h"
#include "boot_hw.h"
#include "boot_version.h"

#include <sgl_core.h>
#include <sgl_font.h>
#include "sgl_label.h"
#include "sgl_progress.h"
#include "sgl_img.h"
#include "sgl_battery.h"
#include "boot_font_helm.h"

#include "bf0_hal.h"
#include <string.h>

/* Stripe tall enough that an 80x80 logo + 2px move (even coords) is one
 * QSPI blit. Not a full 240x320 double buffer (153 KB); boot SRAM cannot.
 * TE is off; one blit per frame is what keeps the enter animation from
 * scanning in 8-line bands. */
#define BOOT_SGL_LINE_H     40
#define BOOT_SGL_FB_PIXELS  (BOOT_LCD_WIDTH * BOOT_SGL_LINE_H)

/* Keep in sync with bicycle img_src_boot_logo.h */
#define LOGO_W              80
#define LOGO_H              80
#define LOGO_X              80
#define LOGO_REST_Y         64
#define LOGO_ENTER_Y        120
#define LOGO_ENTER_FRAMES   28
#define LOGO_ENTER_STEP_US  16000u
#define NAME_X              10
#define NAME_Y_REST         212
#define NAME_Y_ENTER        236
#define NAME_W              220
#define NAME_H              24
#define NAME_ENTER_FRAMES   16
#define NAME_ENTER_STEP_US  16000u
#define FAC_X               10
#define FAC_Y               160
#define FAC_W               220
#define FAC_H               20
#define BAR_X               20
#define BAR_Y               212
#define BAR_W               200
#define BAR_H               16
#define CHG_BATT_W          72
#define CHG_BATT_H          144
#define CHG_BATT_X          84
#define CHG_BATT_Y          80
#define CHG_LABEL_Y         184
#define CHG_HINT_Y          240
#define CHG_BREATH_HALF     12u
#define CHG_BREATH_STEP_MS  80u
#define CHG_SOC_STEP_MS     80u
#define CHG_LOW_MIN         10u
#define BOOT_PRODUCT_NAME   "Helm One"
#define CHG_EMERALD         sgl_rgb(0x00, 0xE0, 0x70)
#define CHG_AMBER           sgl_rgb(0xF3, 0x9C, 0x12)
#define CHG_RED             sgl_rgb(0xE7, 0x4C, 0x3C)
#define CHG_BORDER          sgl_rgb(0x90, 0xE8, 0xB0)
#define CHG_BORDER_LOW      sgl_rgb(0xE0, 0x80, 0x80)

extern const sgl_pixmap_t pic1_pixmap;

static sgl_color_t g_line[BOOT_SGL_FB_PIXELS] __attribute__((aligned(4)));
static sgl_obj_t *g_logo;
static sgl_obj_t *g_factory;
static sgl_obj_t *g_label;
static sgl_obj_t *g_name;
static sgl_obj_t *g_bar;
static sgl_obj_t *g_batt;
static char g_status[40];
static int g_ready;
static int g_entering;
static int g_named;
static int g_entered;
static int g_chg_mode;
static int g_chg_full_ui;
static unsigned g_chg_hint_pct = 0xffu;
static unsigned g_chg_anim;
static uint32_t g_chg_last;
static unsigned g_last_pct = 0xffu;

static uint32_t boot_sgl_ease_out(unsigned i, unsigned frames)
{
    uint32_t t = (i * 255u) / frames;
    uint32_t inv = 255u - t;

    return 255u - (inv * inv) / 255u;
}

static void boot_sgl_flush(sgl_area_t *area, sgl_color_t *src)
{
    if (area && src)
    {
        (void)boot_lcd_blit((uint16_t)area->x1, (uint16_t)area->y1,
                            (uint16_t)area->x2, (uint16_t)area->y2,
                            (const uint16_t *)src);
    }
    sgl_fbdev_flush_ready();
}

static void boot_sgl_pace(uint32_t t0_ms, uint32_t step_us)
{
    uint32_t want_ms = (step_us + 999u) / 1000u;
    uint32_t dt = HAL_GetTick() - t0_ms;

    if (dt < want_ms)
    {
        HAL_Delay_us((want_ms - dt) * 1000u);
    }
}

static void boot_sgl_logo_place(int y, uint8_t alpha)
{
    if (g_logo == NULL)
    {
        return;
    }
    y &= ~1;
    sgl_obj_set_pos(g_logo, LOGO_X, (int16_t)y);
    sgl_img_set_alpha(g_logo, alpha);
}

static void boot_sgl_name_place(int y, uint8_t alpha)
{
    if (g_name == NULL)
    {
        return;
    }
    y &= ~1;
    sgl_obj_set_pos(g_name, NAME_X, (int16_t)y);
    sgl_label_set_alpha(g_name, alpha);
}

static void boot_sgl_enter(void)
{
    unsigned i;

    if (g_logo == NULL)
    {
        return;
    }

    g_entering = 1;
    for (i = 0; i <= LOGO_ENTER_FRAMES; i++)
    {
        uint32_t t0 = HAL_GetTick();
        uint32_t e = boot_sgl_ease_out(i, LOGO_ENTER_FRAMES);
        int y = LOGO_ENTER_Y +
                (((int)LOGO_REST_Y - (int)LOGO_ENTER_Y) * (int)e) / 255;

        boot_sgl_logo_place(y, (uint8_t)e);
        sgl_task_handler_sync();
        boot_hw_wdt_pet();
        boot_sgl_pace(t0, LOGO_ENTER_STEP_US);
    }
    boot_sgl_logo_place(LOGO_REST_Y, SGL_ALPHA_MAX);
    sgl_task_handler_sync();
    g_entering = 0;
}

static void boot_sgl_discard_obj(sgl_obj_t *obj)
{
    if (obj == NULL)
    {
        return;
    }
    /* Hide skips harvest; push the old rect so the black page can erase it. */
    sgl_obj_update_area(&obj->area);
    sgl_obj_set_hidden(obj);
}

static void boot_sgl_hide_chrome(void)
{
    boot_sgl_discard_obj(g_factory);
    boot_sgl_discard_obj(g_label);
    boot_sgl_discard_obj(g_bar);
    boot_sgl_discard_obj(g_batt);
}

static void boot_sgl_name_rest(void)
{
    if (g_name == NULL)
    {
        return;
    }
    sgl_label_set_text(g_name, BOOT_PRODUCT_NAME);
    sgl_obj_set_visible(g_name);
    boot_sgl_name_place(NAME_Y_REST, SGL_ALPHA_MAX);
    g_named = 1;
}

static void boot_sgl_name_reveal(void)
{
    unsigned i;

    if (g_named)
    {
        return;
    }

    boot_sgl_hide_chrome();
    sgl_task_handler_sync();
    g_named = 1;

    if (g_name == NULL)
    {
        return;
    }

    sgl_label_set_text(g_name, BOOT_PRODUCT_NAME);
    sgl_obj_set_visible(g_name);
    boot_sgl_name_place(NAME_Y_ENTER, 0);
    sgl_task_handler_sync();
    boot_hw_wdt_pet();

    for (i = 0; i <= NAME_ENTER_FRAMES; i++)
    {
        uint32_t t0 = HAL_GetTick();
        uint32_t e = boot_sgl_ease_out(i, NAME_ENTER_FRAMES);
        int y = NAME_Y_ENTER +
                (((int)NAME_Y_REST - (int)NAME_Y_ENTER) * (int)e) / 255;

        boot_sgl_name_place(y, (uint8_t)e);
        sgl_task_handler_sync();
        boot_hw_wdt_pet();
        boot_sgl_pace(t0, NAME_ENTER_STEP_US);
    }
    boot_sgl_name_rest();
    sgl_task_handler_sync();
}

static int boot_sgl_init(void)
{
    sgl_fbinfo_t fb;
    sgl_obj_t *page;

    if (g_ready)
    {
        return 0;
    }
    if (boot_lcd_init() != 0)
    {
        return -1;
    }

    memset(&fb, 0, sizeof(fb));
    fb.buffer[0] = g_line;
    fb.buffer[1] = NULL;
    fb.buffer_size = BOOT_SGL_FB_PIXELS;
    fb.xres = BOOT_LCD_WIDTH;
    fb.yres = BOOT_LCD_HEIGHT;
    fb.flush_area = boot_sgl_flush;
    if (sgl_fbdev_register(&fb) != 0)
    {
        return -1;
    }

    sgl_set_system_font(&consolas14);
    if (sgl_init() != 0)
    {
        return -1;
    }

    page = sgl_screen_act();
    if (page == NULL)
    {
        return -1;
    }
    sgl_page_set_color(page, SGL_COLOR_BLACK);

    g_logo = sgl_img_create(page);
    if (g_logo != NULL)
    {
        sgl_obj_set_size(g_logo, LOGO_W, LOGO_H);
        sgl_img_set_pixmap(g_logo, &pic1_pixmap);
        boot_sgl_logo_place(LOGO_ENTER_Y, 0);
        sgl_obj_set_hidden(g_logo);
    }

    g_factory = sgl_label_create(page);
    if (g_factory != NULL)
    {
        sgl_obj_set_size(g_factory, FAC_W, FAC_H);
        sgl_obj_set_pos(g_factory, FAC_X, FAC_Y);
        sgl_label_set_text_align(g_factory, SGL_ALIGN_CENTER);
        sgl_label_set_text_color(g_factory, SGL_COLOR_CYAN);
        sgl_label_set_font(g_factory, &consolas14);
        sgl_label_set_text(g_factory, "factory");
        sgl_obj_set_hidden(g_factory);
    }

    g_label = sgl_label_create(page);
    if (g_label == NULL)
    {
        return -1;
    }
    sgl_obj_set_size(g_label, 220, 20);
    sgl_obj_set_pos(g_label, 10, CHG_LABEL_Y);
    sgl_label_set_text_align(g_label, SGL_ALIGN_CENTER);
    sgl_label_set_text_color(g_label, SGL_COLOR_WHITE);
    sgl_label_set_font(g_label, &consolas14);
    strncpy(g_status, "2SFBL " BOOT_LOADER_VERSION, sizeof(g_status) - 1u);
    g_status[sizeof(g_status) - 1u] = 0;
    sgl_label_set_text(g_label, g_status);
    sgl_obj_set_hidden(g_label);

    g_bar = sgl_progress_create(page);
    if (g_bar == NULL)
    {
        return -1;
    }
    sgl_obj_set_size(g_bar, BAR_W, BAR_H);
    sgl_obj_set_pos(g_bar, BAR_X, BAR_Y);
    sgl_progress_set_track_color(g_bar, SGL_COLOR_DARK_GRAY);
    sgl_progress_set_fill_color(g_bar, SGL_COLOR_CYAN);
    sgl_progress_set_border_width(g_bar, 1);
    sgl_progress_set_border_color(g_bar, SGL_COLOR_GRAY);
    sgl_progress_set_value(g_bar, 0);
    sgl_obj_set_hidden(g_bar);

    g_batt = sgl_battery_create(page);
    if (g_batt != NULL)
    {
        sgl_obj_set_size(g_batt, CHG_BATT_W, CHG_BATT_H);
        sgl_obj_set_pos(g_batt, CHG_BATT_X, CHG_BATT_Y);
        sgl_obj_set_radius(g_batt, 8);
        sgl_battery_set_vertical(g_batt, true);
        sgl_battery_set_alpha(g_batt, SGL_ALPHA_MAX);
        sgl_battery_set_high_color(g_batt, CHG_EMERALD);
        sgl_battery_set_medium_color(g_batt, CHG_AMBER);
        sgl_battery_set_low_color(g_batt, CHG_RED);
        sgl_battery_set_border_color(g_batt, CHG_BORDER);
        sgl_battery_set_bg_color(g_batt, sgl_rgb(0x10, 0x18, 0x12));
        sgl_battery_set_charging(g_batt, true);
        sgl_battery_show_percentage(g_batt, false);
        sgl_battery_set_level(g_batt, 0);
        sgl_obj_set_hidden(g_batt);
    }

    g_name = sgl_label_create(page);
    if (g_name != NULL)
    {
        sgl_obj_set_size(g_name, NAME_W, NAME_H);
        sgl_obj_set_pos(g_name, NAME_X, NAME_Y_ENTER);
        sgl_label_set_text_align(g_name, SGL_ALIGN_CENTER);
        sgl_label_set_text_color(g_name, SGL_COLOR_WHITE);
        sgl_label_set_font(g_name, &boot_font_helm);
        sgl_label_set_text(g_name, BOOT_PRODUCT_NAME);
        sgl_label_set_alpha(g_name, 0);
        sgl_obj_set_hidden(g_name);
    }

    g_ready = 1;
    sgl_obj_set_dirty(page);
    sgl_task_handler_sync();
    return 0;
}

static void boot_sgl_bar_boot(void)
{
    if (g_bar == NULL)
    {
        return;
    }
    sgl_obj_set_size(g_bar, BAR_W, BAR_H);
    sgl_obj_set_pos(g_bar, BAR_X, BAR_Y);
    sgl_progress_set_radius(g_bar, 0);
    sgl_progress_set_fill_width(g_bar, 4);
    sgl_progress_set_fill_gap(g_bar, 4);
    sgl_progress_set_fill_color(g_bar, SGL_COLOR_CYAN);
    sgl_progress_set_border_color(g_bar, SGL_COLOR_GRAY);
    sgl_progress_set_value(g_bar, 0);
    g_last_pct = 0xffu;
}

static void boot_sgl_boot_chrome(void)
{
    if (g_entered)
    {
        return;
    }
    boot_lcd_backlight_on();
    if (g_logo != NULL)
    {
        sgl_obj_set_visible(g_logo);
    }
    boot_sgl_bar_boot();
    strncpy(g_status, "2SFBL " BOOT_LOADER_VERSION, sizeof(g_status) - 1u);
    g_status[sizeof(g_status) - 1u] = 0;
    if (g_label != NULL)
    {
        sgl_label_set_text(g_label, g_status);
    }
    boot_sgl_enter();
    if (g_label != NULL)
    {
        sgl_obj_set_visible(g_label);
    }
    if (g_bar != NULL)
    {
        sgl_obj_set_visible(g_bar);
    }
    sgl_task_handler_sync();
    g_entered = 1;
}

int boot_sgl_start(void)
{
    if (boot_sgl_init() != 0)
    {
        return -1;
    }
    if (!g_chg_mode)
    {
        boot_sgl_boot_chrome();
    }
    return 0;
}

int boot_sgl_charge_begin(void)
{
    sgl_obj_t *page;

    if (boot_sgl_init() != 0)
    {
        return -1;
    }
    g_chg_mode = 1;
    g_chg_anim = 0;
    g_chg_last = 0;
    g_chg_full_ui = 0;
    g_chg_hint_pct = 0xffu;
    boot_lcd_backlight_off();
    if (g_logo != NULL)
    {
        sgl_obj_set_hidden(g_logo);
    }
    if (g_name != NULL)
    {
        sgl_obj_set_hidden(g_name);
    }
    if (g_factory != NULL)
    {
        sgl_obj_set_hidden(g_factory);
    }
    if (g_bar != NULL)
    {
        sgl_obj_set_hidden(g_bar);
    }
    if (g_label != NULL)
    {
        sgl_obj_set_hidden(g_label);
    }
    if (g_batt != NULL)
    {
        sgl_battery_set_level(g_batt, CHG_LOW_MIN);
        sgl_battery_set_charging(g_batt, true);
        sgl_obj_set_visible(g_batt);
    }
    page = sgl_screen_act();
    if (page != NULL)
    {
        sgl_obj_set_dirty(page);
    }
    sgl_task_handler_sync();
    return 0;
}

void boot_sgl_charge_end(void)
{
    sgl_obj_t *page;

    if (!g_ready)
    {
        return;
    }
    g_chg_mode = 0;
    g_chg_full_ui = 0;
    g_chg_hint_pct = 0xffu;
    if (g_label != NULL)
    {
        sgl_obj_set_pos(g_label, 10, CHG_LABEL_Y);
        sgl_label_set_text_color(g_label, SGL_COLOR_WHITE);
    }
    boot_sgl_discard_obj(g_batt);
    boot_sgl_discard_obj(g_label);
    page = sgl_screen_act();
    if (page != NULL)
    {
        sgl_obj_set_dirty(page);
    }
    sgl_task_handler_sync();
    boot_sgl_boot_chrome();
}

int boot_sgl_ready(void)
{
    return g_ready;
}

void boot_sgl_tick(void)
{
}

static unsigned boot_sgl_chg_disp_pct(int pct, int full)
{
    if (full)
    {
        return 100u;
    }
    if (pct < 0)
    {
        return 0u;
    }
    /* Voltage already at 100% but charge-full IO not asserted: stay 99%. */
    if (pct >= 100)
    {
        return 99u;
    }
    return (unsigned)pct;
}

static unsigned boot_sgl_chg_fill(unsigned disp)
{
    if (disp >= 100u)
    {
        return 100u;
    }
    if (disp < CHG_LOW_MIN)
    {
        return CHG_LOW_MIN;
    }
    return disp;
}

static void boot_sgl_charge_hint(int full, unsigned disp)
{
    char buf[8];

    if (g_label == NULL)
    {
        return;
    }
    if (g_chg_full_ui == (full ? 1 : 0) && g_chg_hint_pct == disp)
    {
        return;
    }
    g_chg_full_ui = full ? 1 : 0;
    g_chg_hint_pct = disp;
    if (disp >= 100u)
    {
        buf[0] = '1';
        buf[1] = '0';
        buf[2] = '0';
        buf[3] = '%';
        buf[4] = 0;
    }
    else if (disp >= 10u)
    {
        buf[0] = (char)('0' + disp / 10u);
        buf[1] = (char)('0' + disp % 10u);
        buf[2] = '%';
        buf[3] = 0;
    }
    else
    {
        buf[0] = (char)('0' + disp);
        buf[1] = '%';
        buf[2] = 0;
    }
    sgl_obj_set_pos(g_label, 10, CHG_HINT_Y);
    if (full)
    {
        sgl_label_set_text_color(g_label, CHG_EMERALD);
    }
    else if (disp < 20u)
    {
        sgl_label_set_text_color(g_label, CHG_RED);
    }
    else
    {
        sgl_label_set_text_color(g_label, SGL_COLOR_WHITE);
    }
    sgl_label_set_text(g_label, buf);
    sgl_obj_set_visible(g_label);
}

void boot_sgl_charge_tick(void)
{
    uint32_t now;
    int pct;
    unsigned show;
    unsigned hint;
    int full;

    if (!g_ready || !g_chg_mode || g_batt == NULL)
    {
        return;
    }

    now = HAL_GetTick();
    if (g_chg_last != 0u)
    {
        uint32_t step = g_chg_full_ui ? CHG_BREATH_STEP_MS : CHG_SOC_STEP_MS;

        if ((now - g_chg_last) < step)
        {
            return;
        }
    }
    g_chg_last = now;

    pct = boot_hw_bat_pct();
    /* Full UI is charge-full IO only. Voltage 100% without IO stays charging. */
    full = boot_hw_charge_full();
    hint = boot_sgl_chg_disp_pct(pct, full);
    show = full ? 100u : boot_sgl_chg_fill(hint);
    sgl_battery_set_border_color(g_batt, (show < 20u) ? CHG_BORDER_LOW : CHG_BORDER);
    if (full)
    {
        unsigned phase;
        unsigned tri;
        uint8_t a;

        if (!g_chg_full_ui)
        {
            g_chg_anim = 0;
        }
        g_chg_anim++;
        phase = g_chg_anim % (CHG_BREATH_HALF * 2u);
        tri = (phase <= CHG_BREATH_HALF) ? phase : (CHG_BREATH_HALF * 2u - phase);
        a = (uint8_t)(110u + tri * 12u);
        sgl_battery_set_charging(g_batt, false);
        sgl_battery_set_alpha(g_batt, a);
        sgl_battery_show_percentage(g_batt, false);
        boot_sgl_charge_hint(1, hint);
    }
    else
    {
        sgl_battery_set_charging(g_batt, true);
        sgl_battery_set_alpha(g_batt, SGL_ALPHA_MAX);
        sgl_battery_show_percentage(g_batt, false);
        boot_sgl_charge_hint(0, hint);
    }
    sgl_battery_set_level(g_batt, (uint8_t)show);
    sgl_task_handler_sync();
}

void boot_sgl_hold(void)
{
    if (!g_ready)
    {
        return;
    }
    boot_sgl_hide_chrome();
    if (g_logo)
    {
        boot_sgl_logo_place(LOGO_REST_Y, SGL_ALPHA_MAX);
    }
    if (!g_named)
    {
        boot_sgl_name_rest();
    }
    else
    {
        boot_sgl_name_place(NAME_Y_REST, SGL_ALPHA_MAX);
    }
    sgl_task_handler_sync();
}

void boot_sgl_status(const char *text)
{
    if (!g_ready || text == NULL || g_label == NULL || g_named)
    {
        return;
    }
    strncpy(g_status, text, sizeof(g_status) - 1u);
    g_status[sizeof(g_status) - 1u] = 0;
    sgl_label_set_text(g_label, g_status);
    sgl_obj_set_visible(g_label);
    sgl_task_handler_sync();
}

void boot_sgl_show_factory(void)
{
    if (!g_ready || g_factory == NULL)
    {
        return;
    }
    sgl_label_set_text_color(g_factory, SGL_COLOR_CYAN);
    sgl_label_set_text(g_factory, "factory");
    sgl_obj_set_visible(g_factory);
    sgl_task_handler_sync();
}

void boot_sgl_alert(const char *text, int danger)
{
    if (!g_ready || text == NULL || g_factory == NULL)
    {
        return;
    }
    if (g_name != NULL)
    {
        sgl_obj_set_hidden(g_name);
    }
    sgl_label_set_text_color(g_factory,
                             danger ? SGL_COLOR_RED : SGL_COLOR_YELLOW);
    sgl_label_set_text(g_factory, text);
    sgl_obj_set_visible(g_factory);
    if (danger)
    {
        if (g_bar != NULL)
        {
            sgl_obj_set_hidden(g_bar);
        }
        if (g_label != NULL)
        {
            sgl_obj_set_hidden(g_label);
        }
    }
    sgl_task_handler_sync();
}

void boot_sgl_progress(unsigned pct)
{
    if (!g_ready || g_chg_mode)
    {
        return;
    }
    if (pct > 100u)
    {
        pct = 100u;
    }
    if (pct != g_last_pct)
    {
        g_last_pct = pct;
        if (g_bar != NULL)
        {
            sgl_progress_set_value(g_bar, (uint8_t)pct);
            sgl_task_handler_sync();
        }
    }
    if (pct == 100u)
    {
        boot_sgl_name_reveal();
    }
}
