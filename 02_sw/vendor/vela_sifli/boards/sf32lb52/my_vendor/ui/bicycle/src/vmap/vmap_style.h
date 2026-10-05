/**
 * @file vmap_style.h
 * @brief vmap style 模块。
 */

#ifndef VMAP_STYLE_H
#define VMAP_STYLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMAP_STYLE_ROAD_CLASSES 8

typedef enum {
    VMAP_STYLE_CLASSIC = 0,
    VMAP_STYLE_NAV,
    VMAP_STYLE_OUTDOOR,
    VMAP_STYLE_COUNT
} vmap_style_id_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t w;
} vmap_style_road_t;

typedef struct {
    const char * name;

    /* Map canvas (RGB888 → rasterizer converts to RGB565). */
    uint8_t bg_r;
    uint8_t bg_g;
    uint8_t bg_b;
    uint8_t forest_r;
    uint8_t forest_g;
    uint8_t forest_b;
    uint8_t land_res_r;
    uint8_t land_res_g;
    uint8_t land_res_b;
    uint8_t land_com_r;
    uint8_t land_com_g;
    uint8_t land_com_b;
    uint8_t land_edu_r;
    uint8_t land_edu_g;
    uint8_t land_edu_b;
    uint8_t land_park_r;
    uint8_t land_park_g;
    uint8_t land_park_b;
    uint8_t water_r;
    uint8_t water_g;
    uint8_t water_b;
    uint8_t road_case_r;
    uint8_t road_case_g;
    uint8_t road_case_b;
    uint8_t road_case_extra;
    uint8_t waterway_r;
    uint8_t waterway_g;
    uint8_t waterway_b;
    uint8_t waterway_w;
    vmap_style_road_t road[VMAP_STYLE_ROAD_CLASSES];

    /* Track overlay on map canvas, above labels. */
    bool track_speed_gradient;
    uint8_t track_r;
    uint8_t track_g;
    uint8_t track_b;
    uint8_t track_line_w;
    uint8_t km_badge_r;
    uint8_t km_badge_g;
    uint8_t km_badge_b;

    /* Map labels (0xRRGGBB). */
    uint32_t lbl_place;
    uint32_t lbl_water;
    uint32_t lbl_poi;
    uint32_t lbl_road;

    /* LVGL chrome (0xRRGGBB, opa 0–255). */
    uint32_t ui_root_bg;
    uint32_t ui_status_bg;
    uint8_t  ui_status_opa;
    uint32_t ui_title_text;
    uint32_t ui_accent;
    uint32_t ui_rec;
    uint32_t ui_panel_bg;
    uint8_t  ui_panel_opa;
    uint32_t ui_panel_border;
    uint8_t  ui_panel_border_opa;
    uint32_t ui_speed_text;
    uint32_t ui_heading_text;
    uint32_t ui_dist_text;
    uint32_t ui_toast_bg;
    uint8_t  ui_toast_opa;
    uint32_t ui_toast_text;
    bool     ui_navi_arrow_dark;
} vmap_style_t;

#define VMAP_STYLE_RGB565(r, g, b) \
    ((uint16_t)(((((uint16_t)(r)) & 0xF8u) << 8) | \
                ((((uint16_t)(g)) & 0xFCu) << 3) | \
                (((uint16_t)(b)) >> 3)))

/**
 * @brief vmap style rgb565。
 */
uint16_t vmap_style_rgb565(uint8_t r, uint8_t g, uint8_t b);
/**
 * @brief 当前主题背景 RGB565。
 */
uint16_t vmap_style_bg565(void);

/**
 * @brief vmap style count。
 * @return 请求的值。
 */
unsigned vmap_style_count(void);
/**
 * @brief vmap style get id。
 * @return 请求的值。
 */
vmap_style_id_t vmap_style_get_id(void);
/**
 * @brief vmap style current。
 */
const vmap_style_t * vmap_style_current(void);
/**
 * @brief vmap style get。
 * @return 请求的值。
 */
const vmap_style_t * vmap_style_get(vmap_style_id_t id);
/**
 * @brief vmap style name。
 */
const char * vmap_style_name(vmap_style_id_t id);

/**
 * @brief vmap style set id。
 * @return 请求的值。
 */
bool vmap_style_set_id(vmap_style_id_t id);
/**
 * @brief vmap style set。
 */
bool vmap_style_set(vmap_style_id_t id);
/**
 * @brief vmap style set next。
 */
bool vmap_style_set_next(void);
/**
 * @brief vmap style set by name。
 */
bool vmap_style_set_by_name(const char * name);

typedef void (*vmap_style_changed_cb_t)(vmap_style_id_t id, void * user_data);
/**
 * @brief vmap style set changed cb。
 */
void vmap_style_set_changed_cb(vmap_style_changed_cb_t cb, void * user_data);
/** @deprecated 请优先使用 lv_pm_theme_set + lv_pm_set_theme_changed。 */

#ifdef __cplusplus
}
#endif

#endif /* VMAP_STYLE_H */
