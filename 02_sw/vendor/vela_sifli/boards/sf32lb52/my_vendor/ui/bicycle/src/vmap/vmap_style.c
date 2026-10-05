/**
 * @file vmap_style.c
 * @brief vmap style 模块。
 */

#include "vmap_style.h"

#include "vmap_format.h"
#include <string.h>

#define RS(r, g, b, w) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b), (uint8_t)(w) }

/*
 * Paper luma stays high. Fills keep that luma but push chroma so land/water
 * read as real color on the transflective panel. Roads are orange→yellow
 * (never near-white), cased in brown.
 */
static const vmap_style_t g_vmap_styles[VMAP_STYLE_COUNT] = {
    [VMAP_STYLE_CLASSIC] = {
        .name = "classic",
        .bg_r = 234, .bg_g = 230, .bg_b = 222,
        .forest_r = 156, .forest_g = 204, .forest_b = 132,
        .land_res_r = 236, .land_res_g = 176, .land_res_b = 124,
        .land_com_r = 228, .land_com_g = 156, .land_com_b = 156,
        .land_edu_r = 255, .land_edu_g = 204, .land_edu_b = 96,
        .land_park_r = 140, .land_park_g = 196, .land_park_b = 116,
        .water_r = 104, .water_g = 172, .water_b = 224,
        .road_case_r = 148, .road_case_g = 92, .road_case_b = 40,
        .road_case_extra = 1,
        .waterway_r = 72, .waterway_g = 148, .waterway_b = 212,
        .waterway_w = 2,
        .road = {
            [0] = RS(0, 0, 0, 0),
            [VMAP_ROAD_MOTORWAY]   = RS(255, 128, 24, 5),
            [VMAP_ROAD_PRIMARY]    = RS(255, 160, 32, 4),
            [VMAP_ROAD_SECONDARY]  = RS(204, 152, 48, 3),
            [VMAP_ROAD_TERTIARY]   = RS(255, 212, 64, 3),
            [VMAP_ROAD_RESIDENTIAL]= RS(255, 220, 88, 2),
            [VMAP_ROAD_SERVICE]    = RS(240, 196, 72, 2),
            [VMAP_ROAD_PATH]       = RS(220, 172, 56, 1),
        },
        .track_speed_gradient = true,
        .track_r = 0, .track_g = 168, .track_b = 220,
        .track_line_w = 4,
        .km_badge_r = 20, .km_badge_g = 20, .km_badge_b = 20,
        .lbl_place = 0x000000,
        .lbl_water = 0x0C4A8C,
        .lbl_poi   = 0x145F2D,
        .lbl_road  = 0x000000,
        .ui_root_bg = 0xE4DCD0,
        .ui_status_bg = 0xF5F5F5,
        .ui_status_opa = 255,
        .ui_title_text = 0x101010,
        .ui_accent = 0xFF931E,
        .ui_rec = 0xFF4040,
        .ui_panel_bg = 0xF0F0F0,
        .ui_panel_opa = 255,
        .ui_panel_border = 0xFF931E,
        .ui_panel_border_opa = 128,
        .ui_speed_text = 0xFF931E,
        .ui_heading_text = 0x101010,
        .ui_dist_text = 0x666666,
        .ui_toast_bg = 0xEEEEEE,
        .ui_toast_opa = 255,
        .ui_toast_text = 0x101010,
        .ui_navi_arrow_dark = true,
    },
    [VMAP_STYLE_NAV] = {
        .name = "nav",
        .bg_r = 248, .bg_g = 248, .bg_b = 252,
        .forest_r = 164, .forest_g = 208, .forest_b = 140,
        .land_res_r = 240, .land_res_g = 184, .land_res_b = 136,
        .land_com_r = 232, .land_com_g = 168, .land_com_b = 168,
        .land_edu_r = 255, .land_edu_g = 216, .land_edu_b = 112,
        .land_park_r = 148, .land_park_g = 200, .land_park_b = 124,
        .water_r = 112, .water_g = 180, .water_b = 228,
        .road_case_r = 148, .road_case_g = 92, .road_case_b = 40,
        .road_case_extra = 1,
        .waterway_r = 80, .waterway_g = 152, .waterway_b = 216,
        .waterway_w = 2,
        .road = {
            [0] = RS(0, 0, 0, 0),
            [VMAP_ROAD_MOTORWAY]   = RS(255, 128, 24, 5),
            [VMAP_ROAD_PRIMARY]    = RS(255, 160, 32, 4),
            [VMAP_ROAD_SECONDARY]  = RS(204, 152, 48, 3),
            [VMAP_ROAD_TERTIARY]   = RS(255, 212, 64, 3),
            [VMAP_ROAD_RESIDENTIAL]= RS(255, 220, 88, 2),
            [VMAP_ROAD_SERVICE]    = RS(240, 196, 72, 2),
            [VMAP_ROAD_PATH]       = RS(220, 172, 56, 1),
        },
        .track_speed_gradient = false,
        .track_r = 220, .track_g = 40, .track_b = 40,
        .track_line_w = 4,
        .km_badge_r = 24, .km_badge_g = 24, .km_badge_b = 28,
        .lbl_place = 0x000000,
        .lbl_water = 0x0C4A8C,
        .lbl_poi   = 0x145F2D,
        .lbl_road  = 0x000000,
        .ui_root_bg = 0x101014,
        .ui_status_bg = 0x222228,
        .ui_status_opa = 255,
        .ui_title_text = 0xFFFFFF,
        .ui_accent = 0xFF931E,
        .ui_rec = 0xFF4040,
        .ui_panel_bg = 0x101010,
        .ui_panel_opa = 255,
        .ui_panel_border = 0x404048,
        .ui_panel_border_opa = 255,
        .ui_speed_text = 0xFFFFFF,
        .ui_heading_text = 0xCCCCCC,
        .ui_dist_text = 0xFFFFFF,
        .ui_toast_bg = 0x101010,
        .ui_toast_opa = 255,
        .ui_toast_text = 0xFFFFFF,
        .ui_navi_arrow_dark = false,
    },
    [VMAP_STYLE_OUTDOOR] = {
        .name = "outdoor",
        .bg_r = 32, .bg_g = 34, .bg_b = 38,
        .forest_r = 48, .forest_g = 58, .forest_b = 52,
        .land_res_r = 58, .land_res_g = 48, .land_res_b = 42,
        .land_com_r = 58, .land_com_g = 42, .land_com_b = 42,
        .land_edu_r = 64, .land_edu_g = 54, .land_edu_b = 36,
        .land_park_r = 40, .land_park_g = 52, .land_park_b = 42,
        .water_r = 40, .water_g = 70, .water_b = 110,
        .road_case_r = 0, .road_case_g = 0, .road_case_b = 0,
        .road_case_extra = 0,
        .waterway_r = 60, .waterway_g = 100, .waterway_b = 150,
        .waterway_w = 2,
        .road = {
            [0] = RS(0, 0, 0, 0),
            [VMAP_ROAD_MOTORWAY]   = RS(255, 128, 24, 5),
            [VMAP_ROAD_PRIMARY]    = RS(255, 160, 32, 4),
            [VMAP_ROAD_SECONDARY]  = RS(204, 152, 48, 3),
            [VMAP_ROAD_TERTIARY]   = RS(255, 212, 64, 3),
            [VMAP_ROAD_RESIDENTIAL]= RS(255, 220, 88, 2),
            [VMAP_ROAD_SERVICE]    = RS(240, 196, 72, 2),
            [VMAP_ROAD_PATH]       = RS(220, 172, 56, 1),
        },
        /*
         * 夜间轨迹线用**红色**（与 `nav` 那份同色），不用测速渐变：用户 2026-09-27 实测
         * "夜间导航轨迹线和路线一样了、看不清" —— 渐变低段是青蓝，正好和蓝色的路线撞色。
         * 路线仍是蓝色（`vmap_route.c` 的 route rgb565），轨迹红/路线蓝＝与日光导航一致的
         * 分工。要恢复测速色就把 track_speed_gradient 打开（并解决撞色）。
         */
        .track_speed_gradient = false,
        .track_r = 220, .track_g = 40, .track_b = 40,
        .track_line_w = 4,
        .km_badge_r = 255, .km_badge_g = 255, .km_badge_b = 255,
        .lbl_place = 0xE8E8E8,
        .lbl_water = 0x88BBEE,
        .lbl_poi   = 0x88EEAA,
        .lbl_road  = 0xCCCCCC,
        .ui_root_bg = 0x08080A,
        .ui_status_bg = 0x101014,
        .ui_status_opa = 255,
        .ui_title_text = 0xFFFFFF,
        .ui_accent = 0xFFCC00,
        .ui_rec = 0xFF5050,
        .ui_panel_bg = 0x101014,
        .ui_panel_opa = 255,
        .ui_panel_border = 0xFFCC00,
        .ui_panel_border_opa = 200,
        .ui_speed_text = 0xFFCC00,
        .ui_heading_text = 0xFFFFFF,
        .ui_dist_text = 0xFFFFFF,
        .ui_toast_bg = 0x101014,
        .ui_toast_opa = 255,
        .ui_toast_text = 0xFFFFFF,
        .ui_navi_arrow_dark = false,
    },
};

static vmap_style_id_t g_vmap_style_id = VMAP_STYLE_CLASSIC;
static vmap_style_changed_cb_t g_vmap_style_cb;
static void * g_vmap_style_cb_user;

/**
 * @brief vmap style rgb565。
 */
uint16_t vmap_style_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return VMAP_STYLE_RGB565(r, g, b);
}

/**
 * @brief 当前主题背景 RGB565。
 */
uint16_t vmap_style_bg565(void)
{
    const vmap_style_t * s = vmap_style_current();

    return vmap_style_rgb565(s->bg_r, s->bg_g, s->bg_b);
}

/**
 * @brief vmap style count。
 * @return 请求的值。
 */
unsigned vmap_style_count(void)
{
    return (unsigned)VMAP_STYLE_COUNT;
}

/**
 * @brief vmap style get id。
 * @return 请求的值。
 */
vmap_style_id_t vmap_style_get_id(void)
{
    return g_vmap_style_id;
}

/**
 * @brief vmap style current。
 */
const vmap_style_t * vmap_style_current(void)
{
    return &g_vmap_styles[g_vmap_style_id];
}

/**
 * @brief vmap style get。
 * @return 请求的值。
 */
const vmap_style_t * vmap_style_get(vmap_style_id_t id)
{
    if ((unsigned)id >= (unsigned)VMAP_STYLE_COUNT) {
        return &g_vmap_styles[VMAP_STYLE_CLASSIC];
    }

    return &g_vmap_styles[id];
}

/**
 * @brief vmap style name。
 */
const char * vmap_style_name(vmap_style_id_t id)
{
    return vmap_style_get(id)->name;
}

/**
 * @brief vmap style set changed cb。
 */
void vmap_style_set_changed_cb(vmap_style_changed_cb_t cb, void * user_data)
{
    g_vmap_style_cb = cb;
    g_vmap_style_cb_user = user_data;
}

/**
 * @brief vmap style set id。
 * @return 请求的值。
 */
bool vmap_style_set_id(vmap_style_id_t id)
{
    if ((unsigned)id >= (unsigned)VMAP_STYLE_COUNT) {
        return false;
    }

    g_vmap_style_id = id;
    return true;
}

/**
 * @brief vmap style set。
 */
bool vmap_style_set(vmap_style_id_t id)
{
    if (!vmap_style_set_id(id)) {
        return false;
    }

    if (g_vmap_style_cb != NULL) {
        g_vmap_style_cb(id, g_vmap_style_cb_user);
    }

    return true;
}

/**
 * @brief vmap style set next。
 */
bool vmap_style_set_next(void)
{
    vmap_style_id_t next = (vmap_style_id_t)(((unsigned)g_vmap_style_id + 1u)
        % (unsigned)VMAP_STYLE_COUNT);

    return vmap_style_set(next);
}

/**
 * @brief vmap style set by name。
 */
bool vmap_style_set_by_name(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (unsigned i = 0; i < (unsigned)VMAP_STYLE_COUNT; i++) {
        if (strcmp(g_vmap_styles[i].name, name) == 0) {
            return vmap_style_set((vmap_style_id_t)i);
        }
    }

    return false;
}
