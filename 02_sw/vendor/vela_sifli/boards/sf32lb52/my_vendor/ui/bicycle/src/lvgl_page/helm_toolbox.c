/**
 * @file helm_toolbox.c
 * @brief 工具箱仪表（半透半反 HUD）：地磁罗盘、气泡水平仪、G 表、气压/高度计；
 *        系统状态：记录盘 / 地图盘 / 数据盘 / RAM / CPU。
 */

#include "helm_toolbox.h"

#include "bicycle_env.h"
#include "bicycle_runtime.h"
#include "board_malloc.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "myvendor_board_sensor.h"
#include "myvendor_mtp.h"
#include "myvendor_watchdog.h"

#include "lvgl/src/draw/lv_draw_arc.h"
#include "lvgl/src/draw/lv_draw_label.h"
#include "lvgl/src/draw/lv_draw_line.h"
#include "lvgl/src/draw/lv_draw_rect.h"
#include "lvgl/src/draw/lv_draw_triangle.h"
#include "lvgl/src/misc/lv_text.h"

#include <nuttx/config.h>
#include <nuttx/clock.h>

#include <math.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#define TOOL_TICK_FAST_MS   80u
#define TOOL_TICK_SLOW_MS   250u
#define SYS_TICK_MS         400u
#define G_TRAIL             14
#define G_SCALE             1.50f
#define LEVEL_OK_PX         10.0f
#define ENV_G               9.80665f
#define ISA_P0              1013.25f
#define ISA_M               44330.77f
#define BARO_HPA_LO         300.0f
#define BARO_HPA_HI         1100.0f
#define BARO_ALT_LO         0.0f
#define BARO_ALT_HI         4000.0f
#define BARO_ARC_START      150.0f
#define BARO_ARC_SPAN       240.0f
#define BARO_ARC_W          10
#define BARO_FOOT_PX        40

typedef struct {
    lv_obj_t * root;
    lv_obj_t * canvas;
    lv_obj_t * val;
    lv_obj_t * sub;
    lv_timer_t * tick;
    helm_tool_id_t id;
    bool alt_page;
    bool hdg_have;
    float hdg;
    float bub_x;
    float bub_y;
    float grav_x;
    float grav_y;
    float grav_z;
    bool grav_have;
    float trail_x[G_TRAIL];
    float trail_y[G_TRAIL];
    uint8_t trail_n;
    uint8_t trail_i;
    float peak_g;
    float g_now;
    float tilt_deg;
    float alt_hpa;
    bool alt_show_hpa;
    float alt_m;
    float alt_gnss_m;
    bool alt_have;
    bool alt_gnss_ok;
} helm_tool_ui_t;

enum {
    SYS_DISK_LFS = 0, /* 记录盘 /mnt/lfs */
    SYS_DISK_FAT,     /* 地图盘 /mnt/fat */
    SYS_DISK_KV,      /* 数据盘 /mnt/kv */
    SYS_DISK_N
};

typedef struct {
    lv_obj_t * val;
    lv_obj_t * bar;
    bool have;
    uint64_t size;
    uint64_t used;
    uint64_t avail;
} helm_sys_disk_t;

typedef struct {
    lv_obj_t * root;
    helm_sys_disk_t disk[SYS_DISK_N];
    lv_obj_t * ram_val;
    lv_obj_t * ram_bar;
    lv_obj_t * cpu_val;
    lv_obj_t * cpu_bar;
    lv_timer_t * tick;
} helm_sys_ui_t;

static helm_tool_ui_t s_tool;
static helm_sys_ui_t s_sys;

static float s_baro_max;
static float s_baro_min;
static bool s_baro_have_range;

static float tool_clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static float tool_ang_diff(float from, float to)
{
    float d = to - from;

    while (d > 180.0f) {
        d -= 360.0f;
    }

    while (d < -180.0f) {
        d += 360.0f;
    }

    return d;
}

static float tool_wrap360(float deg)
{
    while (deg < 0.0f) {
        deg += 360.0f;
    }

    while (deg >= 360.0f) {
        deg -= 360.0f;
    }

    return deg;
}

static float tool_isa_alt_m(float hpa)
{
    float r;

    if (hpa <= 1.0f) {
        return 0.0f;
    }

    r = hpa / ISA_P0;
    if (r <= 0.0f) {
        return 0.0f;
    }

    return ISA_M * (1.0f - powf(r, 0.190263f));
}

static float tool_isa_hpa(float alt_m)
{
    float k = 1.0f - alt_m / ISA_M;

    if (k <= 0.05f) {
        return 1.0f;
    }

    return ISA_P0 * powf(k, 1.0f / 0.190263f);
}

static void tool_draw_line(lv_layer_t * layer, int32_t x1, int32_t y1,
                           int32_t x2, int32_t y2, int32_t w, lv_color_t c)
{
    lv_draw_line_dsc_t d;

    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.opa = LV_OPA_COVER;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(layer, &d);
}

static void tool_draw_ring(lv_layer_t * layer, int32_t cx, int32_t cy,
                           int32_t r, int32_t w, lv_color_t c)
{
    lv_draw_arc_dsc_t d;

    if (r < 2) {
        return;
    }

    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.opa = LV_OPA_COVER;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)r;
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(layer, &d);
}

static void tool_draw_disc(lv_layer_t * layer, int32_t cx, int32_t cy,
                           int32_t r, lv_color_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a;

    if (r < 1) {
        return;
    }

    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    d.radius = LV_RADIUS_CIRCLE;
    d.border_width = 0;
    a.x1 = (lv_coord_t)(cx - r);
    a.y1 = (lv_coord_t)(cy - r);
    a.x2 = (lv_coord_t)(cx + r);
    a.y2 = (lv_coord_t)(cy + r);
    lv_draw_rect(layer, &d, &a);
}

static void tool_draw_tri(lv_layer_t * layer, int32_t x0, int32_t y0,
                          int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                          lv_color_t c)
{
    lv_draw_triangle_dsc_t d;

    lv_draw_triangle_dsc_init(&d);
    d.color = c;
    d.opa = LV_OPA_COVER;
    d.p[0].x = x0;
    d.p[0].y = y0;
    d.p[1].x = x1;
    d.p[1].y = y1;
    d.p[2].x = x2;
    d.p[2].y = y2;
    lv_draw_triangle(layer, &d);
}

static void tool_draw_text(lv_layer_t * layer, int32_t x, int32_t y,
                           const char * txt, const lv_font_t * font,
                           lv_color_t c)
{
    lv_draw_label_dsc_t d;
    lv_point_t sz;
    lv_area_t a;

    if (txt == NULL || txt[0] == '\0') {
        return;
    }

    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.text_local = 1;
    d.font = font;
    d.color = c;
    d.opa = LV_OPA_COVER;
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    d.text_size = sz;
    a.x1 = (lv_coord_t)(x - sz.x / 2);
    a.y1 = (lv_coord_t)(y - sz.y / 2);
    a.x2 = (lv_coord_t)(a.x1 + sz.x);
    a.y2 = (lv_coord_t)(a.y1 + sz.y);
    lv_draw_label(layer, &d, &a);
}

static void tool_draw_arc_span(lv_layer_t * layer, int32_t cx, int32_t cy,
                               int32_t r, int32_t w, float start, float end,
                               lv_color_t c, lv_opa_t opa, bool rounded)
{
    lv_draw_arc_dsc_t d;

    if (r < 4 || w < 1 || end - start < 0.5f) {
        return;
    }

    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.opa = opa;
    d.rounded = rounded ? 1 : 0;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)r;
    d.start_angle = start;
    d.end_angle = end;
    lv_draw_arc(layer, &d);
}

static void tool_draw_pill(lv_layer_t * layer, int32_t cx, int32_t cy,
                           int32_t w, int32_t h, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a;

    if (w < 2 || h < 2) {
        return;
    }

    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    d.radius = h / 2;
    d.border_width = 0;
    a.x1 = (lv_coord_t)(cx - w / 2);
    a.y1 = (lv_coord_t)(cy - h / 2);
    a.x2 = (lv_coord_t)(cx + w / 2);
    a.y2 = (lv_coord_t)(cy + h / 2);
    lv_draw_rect(layer, &d, &a);
}

static void tool_draw_face(lv_layer_t * layer, int32_t cx, int32_t cy,
                           int32_t r)
{
    tool_draw_disc(layer, cx, cy, r, helm_color(HELM_COLOR_PAPER), LV_OPA_COVER);
    tool_draw_ring(layer, cx, cy, r, 2, helm_color(HELM_COLOR_HAIR));
}

static void tool_draw_crosshair(lv_layer_t * layer, int32_t cx, int32_t cy,
                                int32_t r, int32_t gap)
{
    const lv_color_t hair = helm_color(HELM_COLOR_HAIR);

    if (r <= gap) {
        return;
    }

    tool_draw_line(layer, cx - r, cy, cx - gap, cy, 1, hair);
    tool_draw_line(layer, cx + gap, cy, cx + r, cy, 1, hair);
    tool_draw_line(layer, cx, cy - r, cx, cy - gap, 1, hair);
    tool_draw_line(layer, cx, cy + gap, cx, cy + r, 1, hair);
}

static void tool_polar(int32_t cx, int32_t cy, float deg, float r,
                       int32_t * x, int32_t * y)
{
    float a = deg * ((float)M_PI / 180.0f);

    *x = cx + (int32_t)(cosf(a) * r);
    *y = cy + (int32_t)(sinf(a) * r);
}

static float tool_scale_ang(float v, float lo, float hi)
{
    float t = (v - lo) / (hi - lo);

    t = tool_clampf(t, 0.0f, 1.0f);
    return BARO_ARC_START + t * BARO_ARC_SPAN;
}

static void tool_baro_note(float hpa)
{
    if (hpa <= 1.0f) {
        return;
    }

    if (!s_baro_have_range) {
        s_baro_max = hpa;
        s_baro_min = hpa;
        s_baro_have_range = true;
        return;
    }

    if (hpa > s_baro_max) {
        s_baro_max = hpa;
    }

    if (hpa < s_baro_min) {
        s_baro_min = hpa;
    }
}

static void tool_set_meters(bool on)
{
    if (s_tool.val) {
        if (on) {
            lv_obj_clear_flag(s_tool.val, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_tool.val, LV_OBJ_FLAG_IGNORE_LAYOUT);
        } else {
            lv_obj_add_flag(s_tool.val, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_tool.val, LV_OBJ_FLAG_IGNORE_LAYOUT);
        }
    }

    if (s_tool.sub) {
        if (on) {
            lv_obj_clear_flag(s_tool.sub, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_tool.sub, LV_OBJ_FLAG_IGNORE_LAYOUT);
        } else {
            lv_obj_add_flag(s_tool.sub, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_tool.sub, LV_OBJ_FLAG_IGNORE_LAYOUT);
        }
    }
}

static void tool_draw_dots(lv_layer_t * layer, const lv_area_t * box, uint8_t on)
{
    const int32_t cx = (box->x1 + box->x2) / 2;
    const int32_t y = box->y2 - 8;
    const lv_color_t nav = helm_color(HELM_COLOR_NAV);
    const lv_color_t hair = helm_color(HELM_COLOR_HAIR);

    tool_draw_pill(layer, cx - 10, y, 12, 5, on == 0 ? nav : hair);
    tool_draw_pill(layer, cx + 10, y, 12, 5, on == 1 ? nav : hair);
}

static bool tool_heading_deg(float * out)
{
    myvendor_sys_mag_t mag;
    myvendor_sys_vec3_t acc;
    float mx;
    float my;
    float mz;
    float ax;
    float ay;
    float az;
    float an;
    float pitch;
    float roll;
    float sp;
    float cp;
    float sr;
    float cr;
    float hx;
    float hy;
    float deg;

    myvendor_board_sensor_mag_get(&mag);
    myvendor_board_sensor_accel_get(&acc);
    if (!mag.valid) {
        return false;
    }

    mx = mag.x;
    my = mag.y;
    mz = mag.z;
    if (acc.valid) {
        ax = acc.x;
        ay = acc.y;
        az = acc.z;
        an = sqrtf(ax * ax + ay * ay + az * az);
        if (an > 1.0f) {
            ax /= an;
            ay /= an;
            az /= an;
            /* Portrait：Y 沿屏幕，Z 出屏。倾角补偿后水平磁场。 */
            pitch = atan2f(-ay, hypotf(ax, az));
            roll = atan2f(ax, az);
            sp = sinf(pitch);
            cp = cosf(pitch);
            sr = sinf(roll);
            cr = cosf(roll);
            hx = mx * cp + mz * sp;
            hy = mx * sr * sp + my * cr - mz * sr * cp;
        } else {
            hx = mx;
            hy = my;
        }
    } else {
        hx = mx;
        hy = my;
    }

    if (hx * hx + hy * hy < 4.0f) {
        return false;
    }

    deg = atan2f(-hy, hx) * (180.0f / (float)M_PI);
    if (out) {
        *out = tool_wrap360(deg);
    }

    return true;
}

static const char * tool_cardinal(float deg)
{
    static const char * const names[8] = {
        "北", "东北", "东", "东南", "南", "西南", "西", "西北"
    };
    int i = (int)floorf((tool_wrap360(deg) + 22.5f) / 45.0f);

    return names[i & 7];
}

static void tool_reset_motion(void)
{
    s_tool.hdg_have = false;
    s_tool.hdg = 0.0f;
    s_tool.bub_x = 0.0f;
    s_tool.bub_y = 0.0f;
    s_tool.grav_have = false;
    s_tool.trail_n = 0;
    s_tool.trail_i = 0;
    s_tool.peak_g = 0.0f;
    s_tool.g_now = 0.0f;
    s_tool.tilt_deg = 0.0f;
    s_tool.alt_page = false;
    s_tool.alt_hpa = 0.0f;
    s_tool.alt_show_hpa = false;
    s_tool.alt_have = false;
    s_tool.alt_gnss_ok = false;
    s_tool.alt_gnss_m = 0.0f;
}

static void tool_draw_compass(lv_layer_t * layer, const lv_area_t * box)
{
    const int32_t cx = (box->x1 + box->x2) / 2;
    const int32_t cy = (box->y1 + box->y2) / 2;
    const int32_t span = LV_MIN(lv_area_get_width(box), lv_area_get_height(box));
    const int32_t r = span / 2 - 8;
    const float rose = -s_tool.hdg * ((float)M_PI / 180.0f);
    const lv_color_t ink = helm_color(HELM_COLOR_INK);
    const lv_color_t hair = helm_color(HELM_COLOR_HAIR);
    const lv_color_t nav = helm_color(HELM_COLOR_NAV);
    char line[16];
    int i;

    tool_draw_face(layer, cx, cy, r);
    tool_draw_ring(layer, cx, cy, r - 22, 1, hair);

    for (i = 0; i < 24; i++) {
        const float a = rose + (float)i * ((float)M_PI / 12.0f);
        const float s = sinf(a);
        const float c = cosf(a);
        const bool cardinal = (i % 6) == 0;
        const bool mid = (i % 2) == 0;
        const int32_t inner = cardinal ? (r - 20) : (mid ? (r - 14) : (r - 10));
        const int32_t outer = r - 4;

        tool_draw_line(layer,
                       cx + (int32_t)(s * (float)inner),
                       cy - (int32_t)(c * (float)inner),
                       cx + (int32_t)(s * (float)outer),
                       cy - (int32_t)(c * (float)outer),
                       cardinal ? 2 : 1, cardinal ? ink : hair);
    }

    {
        static const char * const lab[4] = { "N", "E", "S", "W" };
        int k;

        for (k = 0; k < 4; k++) {
            const float a = rose + (float)k * ((float)M_PI / 2.0f);
            const int32_t lx = cx + (int32_t)(sinf(a) * (float)(r - 34));
            const int32_t ly = cy - (int32_t)(cosf(a) * (float)(r - 34));

            tool_draw_text(layer, lx, ly, lab[k], helm_font_title(),
                           k == 0 ? nav : ink);
        }
    }

    tool_draw_tri(layer, cx, box->y1 + 4, cx - 7, box->y1 + 16, cx + 7,
                  box->y1 + 16, nav);

    if (s_tool.hdg_have) {
        int deg = (int)(s_tool.hdg + 0.5f);

        if (deg >= 360) {
            deg = 0;
        }

        lv_snprintf(line, sizeof(line), "%d", deg);
        tool_draw_text(layer, cx, cy - 10, line, helm_font_quad(), ink);
        tool_draw_text(layer, cx, cy + 16, tool_cardinal(s_tool.hdg),
                       helm_font_title(), ink);
    } else {
        tool_draw_text(layer, cx, cy - 8, "--", helm_font_quad(), ink);
        tool_draw_text(layer, cx, cy + 16, "等待地磁", helm_font_lab(),
                       helm_color(HELM_COLOR_MUTE));
    }
}

static void tool_draw_level(lv_layer_t * layer, const lv_area_t * box)
{
    const int32_t cx = (box->x1 + box->x2) / 2;
    const int32_t cy = (box->y1 + box->y2) / 2 - 6;
    const int32_t span = LV_MIN(lv_area_get_width(box), lv_area_get_height(box));
    const int32_t r = span / 2 - 14;
    const int32_t br = 16;
    const int32_t bx = cx + (int32_t)(s_tool.bub_x + 0.5f);
    const int32_t by = cy + (int32_t)(s_tool.bub_y + 0.5f);
    const float dist = hypotf(s_tool.bub_x, s_tool.bub_y);
    const bool ok = dist <= LEVEL_OK_PX;
    const lv_color_t ink = helm_color(HELM_COLOR_INK);
    const lv_color_t hair = helm_color(HELM_COLOR_HAIR);
    const lv_color_t accent = helm_color(ok ? HELM_COLOR_OK : HELM_COLOR_NAV);
    const lv_color_t paper = helm_color(HELM_COLOR_PAPER);
    char line[16];

    tool_draw_face(layer, cx, cy, r);
    tool_draw_ring(layer, cx, cy, r, 6, accent);
    tool_draw_ring(layer, cx, cy, r - 6, 2, paper);
    tool_draw_ring(layer, cx, cy, (int32_t)((float)r * 0.58f), 1, hair);
    tool_draw_ring(layer, cx, cy, br + 6, 2, ok ? accent : hair);
    tool_draw_crosshair(layer, cx, cy, r - 10, br + 10);
    tool_draw_disc(layer, bx, by, br, accent, LV_OPA_COVER);
    tool_draw_disc(layer, bx - 4, by - 4, 5, paper, LV_OPA_COVER);
    tool_draw_ring(layer, bx, by, br, 2, ink);

    if (ok) {
        tool_draw_text(layer, cx, box->y2 - 18, "水平", helm_font_title(),
                       accent);
    } else {
        lv_snprintf(line, sizeof(line), "%d°", (int)(s_tool.tilt_deg + 0.5f));
        tool_draw_text(layer, cx, box->y2 - 18, line, helm_font_title(),
                       accent);
    }
}

static void tool_draw_gmeter(lv_layer_t * layer, const lv_area_t * box)
{
    const int32_t cx = (box->x1 + box->x2) / 2;
    const int32_t cy = (box->y1 + box->y2) / 2 - 8;
    const int32_t span = LV_MIN(lv_area_get_width(box),
                                lv_area_get_height(box) - 28);
    const int32_t r = span / 2 - 8;
    const float pix_g = (float)r / G_SCALE;
    const lv_color_t ink = helm_color(HELM_COLOR_INK);
    const lv_color_t hair = helm_color(HELM_COLOR_HAIR);
    const lv_color_t nav = helm_color(HELM_COLOR_NAV);
    const lv_color_t paper = helm_color(HELM_COLOR_PAPER);
    char line[20];
    uint8_t i;
    uint8_t n;

    tool_draw_face(layer, cx, cy, r);
    tool_draw_ring(layer, cx, cy, (int32_t)(pix_g * 1.0f), 2, hair);
    tool_draw_ring(layer, cx, cy, (int32_t)(pix_g * 0.5f), 1, hair);
    tool_draw_crosshair(layer, cx, cy, r - 8, 10);
    tool_draw_text(layer, cx + (int32_t)(pix_g * 1.0f) - 2, cy + 12, "1g",
                   helm_font_lab(), ink);
    tool_draw_text(layer, cx, cy - r + 14, "前", helm_font_lab(), ink);
    tool_draw_text(layer, cx + r - 14, cy, "右", helm_font_lab(), ink);

    n = s_tool.trail_n;
    for (i = 0; i < n; i++) {
        uint8_t idx;
        int32_t px;
        int32_t py;
        int32_t rad;

        idx = (uint8_t)((s_tool.trail_i + G_TRAIL - n + i) % G_TRAIL);
        px = cx + (int32_t)(s_tool.trail_x[idx] * pix_g);
        py = cy + (int32_t)(s_tool.trail_y[idx] * pix_g);
        rad = (i + 2) / 3;
        if (rad < 2) {
            rad = 2;
        }

        tool_draw_disc(layer, px, py, rad, i + 3 >= n ? nav : hair,
                       LV_OPA_COVER);
    }

    if (n > 0) {
        uint8_t last = (uint8_t)((s_tool.trail_i + G_TRAIL - 1u) % G_TRAIL);
        int32_t px = cx + (int32_t)(s_tool.trail_x[last] * pix_g);
        int32_t py = cy + (int32_t)(s_tool.trail_y[last] * pix_g);

        tool_draw_disc(layer, px, py, 7, nav, LV_OPA_COVER);
        tool_draw_disc(layer, px - 2, py - 2, 2, paper, LV_OPA_COVER);
        tool_draw_ring(layer, px, py, 7, 2, ink);
    }

    lv_snprintf(line, sizeof(line), "%0.2f g", (double)s_tool.g_now);
    tool_draw_text(layer, cx, box->y2 - 28, line, helm_font_val(), ink);
    lv_snprintf(line, sizeof(line), "峰值 %0.2f", (double)s_tool.peak_g);
    tool_draw_text(layer, cx, box->y2 - 12, line, helm_font_lab(),
                   helm_color(HELM_COLOR_MUTE));
}

static void tool_draw_alt_gauge(lv_layer_t * layer, int32_t cx, int32_t cy,
                                int32_t r, float lo, float hi, float baro_v,
                                bool have_baro, float gnss_v, bool have_gnss,
                                int lab0, int lab_hi)
{
    const float a0 = BARO_ARC_START;
    const float a1 = BARO_ARC_START + BARO_ARC_SPAN;
    const lv_color_t ink = helm_color(HELM_COLOR_INK);
    const lv_color_t hair = helm_color(HELM_COLOR_HAIR);
    const lv_color_t nav = helm_color(HELM_COLOR_NAV);
    const lv_color_t paper = helm_color(HELM_COLOR_PAPER);
    int i;
    char line[16];

    tool_draw_disc(layer, cx, cy, r - 20, paper, LV_OPA_COVER);
    tool_draw_ring(layer, cx, cy, r - 20, 2, hair);
    tool_draw_arc_span(layer, cx, cy, r, BARO_ARC_W, a0, a1, hair, LV_OPA_COVER,
                       true);

    if (have_baro) {
        tool_draw_arc_span(layer, cx, cy, r, BARO_ARC_W, a0,
                           tool_scale_ang(baro_v, lo, hi), nav, LV_OPA_COVER,
                           true);
    }

    for (i = 0; i <= 8; i++) {
        const float a = a0 + (float)i * (BARO_ARC_SPAN / 8.0f);
        const bool major = (i % 4) == 0;
        int32_t x0;
        int32_t y0;
        int32_t x1;
        int32_t y1;

        tool_polar(cx, cy, a, (float)(r - 12), &x0, &y0);
        tool_polar(cx, cy, a, (float)(r - (major ? 20 : 16)), &x1, &y1);
        tool_draw_line(layer, x0, y0, x1, y1, 1, hair);
        if (i == 0 || i == 8) {
            int32_t lx;
            int32_t ly;
            int lab = (i == 0) ? lab0 : lab_hi;

            lv_snprintf(line, sizeof(line), "%d", lab);
            tool_polar(cx, cy, a, (float)(r - 30), &lx, &ly);
            tool_draw_text(layer, lx, ly, line, helm_font_lab(), ink);
        }
    }

    if (have_gnss) {
        int32_t mx;
        int32_t my;

        tool_polar(cx, cy, tool_scale_ang(gnss_v, lo, hi),
                   (float)(r - BARO_ARC_W / 2), &mx, &my);
        tool_draw_disc(layer, mx, my, 4, ink, LV_OPA_COVER);
        tool_draw_disc(layer, mx, my, 2, paper, LV_OPA_COVER);
    }
}

static void tool_draw_alt_readout(lv_layer_t * layer, int32_t cx, int32_t cy,
                                  const char * top, const char * unit,
                                  const char * bot, bool gnss_ok)
{
    const lv_color_t ink = helm_color(HELM_COLOR_INK);
    const lv_color_t warn = helm_color(HELM_COLOR_NAV);

    tool_draw_text(layer, cx, cy - 16, top, helm_font_quad(), ink);
    tool_draw_text(layer, cx, cy + 4, unit, helm_font_lab(), ink);
    tool_draw_text(layer, cx, cy + 24, bot, helm_font_val(),
                   gnss_ok ? ink : warn);
    tool_draw_text(layer, cx, cy + 42, gnss_ok ? "GNSS" : "无GNSS",
                   helm_font_lab(),
                   gnss_ok ? helm_color(HELM_COLOR_MUTE) : warn);
}

static void tool_draw_altimeter(lv_layer_t * layer, const lv_area_t * box)
{
    const int32_t cx = (box->x1 + box->x2) / 2;
    const int32_t w = lv_area_get_width(box);
    const int32_t h = lv_area_get_height(box);
    const int32_t cy = box->y1 + (h - BARO_FOOT_PX) / 2;
    const lv_color_t ink = helm_color(HELM_COLOR_INK);
    const bool page_alt = s_tool.alt_page;
    const bool gnss_ok = s_tool.alt_gnss_ok;
    char top[16];
    char bot[16];
    int32_t r;

    r = w / 2 - 8;
    r = LV_MIN(r, cy - box->y1 - 2);
    r = LV_MIN(r, (box->y2 - BARO_FOOT_PX) - cy);
    if (r < 72) {
        r = 72;
    }

    if (!page_alt) {
        tool_draw_alt_gauge(layer, cx, cy, r, BARO_HPA_LO, BARO_HPA_HI,
                            s_tool.alt_hpa, s_tool.alt_show_hpa,
                            tool_isa_hpa(s_tool.alt_gnss_m), gnss_ok,
                            300, 1100);
        if (s_tool.alt_show_hpa) {
            lv_snprintf(top, sizeof(top), "%d", (int)(s_tool.alt_hpa + 0.5f));
        } else {
            lv_snprintf(top, sizeof(top), "--");
        }

        if (gnss_ok) {
            lv_snprintf(bot, sizeof(bot), "%d",
                        (int)(tool_isa_hpa(s_tool.alt_gnss_m) + 0.5f));
        } else {
            lv_snprintf(bot, sizeof(bot), "--");
        }

        tool_draw_alt_readout(layer, cx, cy, top, "hPa", bot, gnss_ok);
        if (s_baro_have_range) {
            lv_snprintf(top, sizeof(top), "↑ %d",
                        (int)(s_baro_max + 0.5f));
            tool_draw_text(layer, box->x1 + 40, box->y2 - 26, top,
                           helm_font_lab(), ink);
            lv_snprintf(top, sizeof(top), "↓ %d",
                        (int)(s_baro_min + 0.5f));
            tool_draw_text(layer, box->x2 - 40, box->y2 - 26, top,
                           helm_font_lab(), ink);
        }
    } else {
        tool_draw_alt_gauge(layer, cx, cy, r, BARO_ALT_LO, BARO_ALT_HI,
                            s_tool.alt_m, s_tool.alt_have, s_tool.alt_gnss_m,
                            gnss_ok, 0, 4000);
        if (s_tool.alt_have) {
            lv_snprintf(top, sizeof(top), "%d", (int)(s_tool.alt_m + 0.5f));
        } else {
            lv_snprintf(top, sizeof(top), "--");
        }

        if (gnss_ok) {
            lv_snprintf(bot, sizeof(bot), "%d",
                        (int)(s_tool.alt_gnss_m + 0.5f));
        } else {
            lv_snprintf(bot, sizeof(bot), "--");
        }

        tool_draw_alt_readout(layer, cx, cy, top, "m", bot, gnss_ok);
        if (s_baro_have_range) {
            lv_snprintf(top, sizeof(top), "↑ %d",
                        (int)(tool_isa_alt_m(s_baro_min) + 0.5f));
            tool_draw_text(layer, box->x1 + 40, box->y2 - 26, top,
                           helm_font_lab(), ink);
            lv_snprintf(top, sizeof(top), "↓ %d",
                        (int)(tool_isa_alt_m(s_baro_max) + 0.5f));
            tool_draw_text(layer, box->x2 - 40, box->y2 - 26, top,
                           helm_font_lab(), ink);
        }
    }

    tool_draw_dots(layer, box, page_alt ? 1 : 0);
}

static void tool_canvas_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;

    if (obj == NULL || layer == NULL) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    switch (s_tool.id) {
    case HELM_TOOL_COMPASS:
        tool_draw_compass(layer, &coords);
        break;
    case HELM_TOOL_LEVEL:
        tool_draw_level(layer, &coords);
        break;
    case HELM_TOOL_GMETER:
        tool_draw_gmeter(layer, &coords);
        break;
    case HELM_TOOL_ALTIMETER:
        tool_draw_altimeter(layer, &coords);
        break;
    default:
        break;
    }
}

static void tool_poll_compass(void)
{
    float hdg;
    char buf[24];

    if (!tool_heading_deg(&hdg)) {
        lv_label_set_text(s_tool.val, "--");
        lv_label_set_text(s_tool.sub, "等待地磁");
        return;
    }

    if (!s_tool.hdg_have) {
        s_tool.hdg = hdg;
        s_tool.hdg_have = true;
    } else {
        s_tool.hdg = tool_wrap360(s_tool.hdg +
                                  0.28f * tool_ang_diff(s_tool.hdg, hdg));
    }

    {
        int deg = (int)(s_tool.hdg + 0.5f);

        if (deg >= 360) {
            deg = 0;
        }

        lv_snprintf(buf, sizeof(buf), "%d° %s", deg, tool_cardinal(s_tool.hdg));
    }
    lv_label_set_text(s_tool.val, buf);
    lv_label_set_text(s_tool.sub, "磁北");
}

static void tool_poll_level(void)
{
    myvendor_sys_vec3_t a;
    char buf[32];
    int32_t r;
    float lim;
    float nx;
    float ny;
    bool ok;

    myvendor_board_sensor_accel_get(&a);
    if (!a.valid) {
        lv_label_set_text(s_tool.val, "--");
        lv_label_set_text(s_tool.sub, "等待 IMU");
        return;
    }

    r = 0;
    if (s_tool.canvas) {
        r = LV_MIN(lv_obj_get_width(s_tool.canvas),
                   lv_obj_get_height(s_tool.canvas)) / 2 - 36;
    }

    if (r < 20) {
        r = 70;
    }

    lim = (float)r;
    nx = tool_clampf((a.x / ENV_G) * lim, -lim, lim);
    ny = tool_clampf(-(a.y / ENV_G) * lim, -lim, lim);
    s_tool.bub_x += 0.35f * (nx - s_tool.bub_x);
    s_tool.bub_y += 0.35f * (ny - s_tool.bub_y);
    ok = hypotf(s_tool.bub_x, s_tool.bub_y) <= LEVEL_OK_PX;
    {
        float mag = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);

        s_tool.tilt_deg = 0.0f;
        if (mag > 1.0f) {
            s_tool.tilt_deg = acosf(tool_clampf(fabsf(a.z) / mag, 0.0f, 1.0f)) *
                              (180.0f / (float)M_PI);
        }
    }
    lv_snprintf(buf, sizeof(buf), "%s", ok ? "水平" : "倾斜");
    lv_label_set_text(s_tool.val, buf);
    lv_snprintf(buf, sizeof(buf), "x %+0.1f  y %+0.1f",
                (double)(a.x / ENV_G), (double)(a.y / ENV_G));
    lv_label_set_text(s_tool.sub, buf);
}

static void tool_poll_gmeter(void)
{
    myvendor_sys_vec3_t a;
    char buf[32];
    float lx;
    float ly;
    float g;
    uint8_t i;

    myvendor_board_sensor_accel_get(&a);
    if (!a.valid) {
        lv_label_set_text(s_tool.val, "--");
        lv_label_set_text(s_tool.sub, "等待 IMU");
        return;
    }

    if (!s_tool.grav_have) {
        s_tool.grav_x = a.x;
        s_tool.grav_y = a.y;
        s_tool.grav_z = a.z;
        s_tool.grav_have = true;
    } else {
        s_tool.grav_x = s_tool.grav_x * 0.92f + a.x * 0.08f;
        s_tool.grav_y = s_tool.grav_y * 0.92f + a.y * 0.08f;
        s_tool.grav_z = s_tool.grav_z * 0.92f + a.z * 0.08f;
    }

    /* 屏幕平面：X 左右，Y 前后（上为前，故取负）。 */
    lx = (a.x - s_tool.grav_x) / ENV_G;
    ly = -(a.y - s_tool.grav_y) / ENV_G;
    lx = tool_clampf(lx, -G_SCALE, G_SCALE);
    ly = tool_clampf(ly, -G_SCALE, G_SCALE);
    i = s_tool.trail_i;
    s_tool.trail_x[i] = lx;
    s_tool.trail_y[i] = ly;
    s_tool.trail_i = (uint8_t)((i + 1u) % G_TRAIL);
    if (s_tool.trail_n < G_TRAIL) {
        s_tool.trail_n++;
    }

    g = hypotf(lx, ly);
    s_tool.g_now = g;
    if (g > s_tool.peak_g) {
        s_tool.peak_g = g;
    }

    lv_snprintf(buf, sizeof(buf), "%0.2f g", (double)g);
    lv_label_set_text(s_tool.val, buf);
    lv_snprintf(buf, sizeof(buf), "峰值 %0.2f g", (double)s_tool.peak_g);
    lv_label_set_text(s_tool.sub, buf);
}

static void tool_poll_altimeter(void)
{
    myvendor_sys_baro_t baro;
    const bicycle_runtime_t * rt;

    bicycle_env_tick();
    s_tool.alt_have = false;
    s_tool.alt_gnss_ok = false;
    s_tool.alt_show_hpa = false;
    s_tool.alt_gnss_m = 0.0f;

    myvendor_board_sensor_baro_get(&baro);
    if (baro.valid && baro.hpa > 1.0f) {
        s_tool.alt_hpa = baro.hpa;
        s_tool.alt_show_hpa = true;
        s_tool.alt_m = tool_isa_alt_m(baro.hpa);
        s_tool.alt_have = true;
        tool_baro_note(baro.hpa);
    }

    rt = bicycle_runtime_get();
    if (rt && rt->gnss_valid && rt->gnss_has_altitude) {
        s_tool.alt_gnss_ok = true;
        s_tool.alt_gnss_m = rt->gnss_altitude_m;
    }
}

static void tool_tick_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if (s_tool.root == NULL ||
        lv_obj_has_flag(s_tool.root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    switch (s_tool.id) {
    case HELM_TOOL_COMPASS:
        tool_poll_compass();
        break;
    case HELM_TOOL_LEVEL:
        tool_poll_level();
        break;
    case HELM_TOOL_GMETER:
        tool_poll_gmeter();
        break;
    case HELM_TOOL_ALTIMETER:
        tool_poll_altimeter();
        break;
    default:
        break;
    }

    if (s_tool.canvas) {
        lv_obj_invalidate(s_tool.canvas);
    }
}

lv_obj_t * helm_toolbox_build(lv_obj_t * parent)
{
    memset(&s_tool, 0, sizeof(s_tool));
    s_tool.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_tool.root);
    helm_grow_y(s_tool.root);
    lv_obj_set_flex_flow(s_tool.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_tool.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_tool.root, 8, 0);
    lv_obj_set_style_pad_bottom(s_tool.root, 6, 0);
    lv_obj_add_flag(s_tool.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_tool.root, LV_OBJ_FLAG_SCROLLABLE);

    s_tool.canvas = lv_obj_create(s_tool.root);
    lv_obj_remove_style_all(s_tool.canvas);
    helm_grow_y(s_tool.canvas);
    lv_obj_set_style_bg_opa(s_tool.canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_tool.canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_tool.canvas, tool_canvas_draw, LV_EVENT_DRAW_MAIN,
                        NULL);

    s_tool.val = helm_label(s_tool.root, helm_font_val(), HELM_COLOR_INK, "--");
    lv_obj_set_width(s_tool.val, lv_pct(100));
    lv_obj_set_style_text_align(s_tool.val, LV_TEXT_ALIGN_CENTER, 0);
    /* 仪表页的次级行（"按住 KEY2 归零"这类）跟菜单同一层级：MUTE 12px。 */
    s_tool.sub = helm_label(s_tool.root, helm_font_lab(), HELM_COLOR_MUTE, "");
    lv_obj_set_width(s_tool.sub, lv_pct(100));
    lv_obj_set_style_text_align(s_tool.sub, LV_TEXT_ALIGN_CENTER, 0);

    s_tool.tick = lv_timer_create(tool_tick_cb, TOOL_TICK_FAST_MS, NULL);
    lv_timer_pause(s_tool.tick);
    return s_tool.root;
}

void helm_toolbox_open(helm_tool_id_t id)
{
    if (s_tool.root == NULL) {
        return;
    }

    if (s_tool.id != id || lv_obj_has_flag(s_tool.root, LV_OBJ_FLAG_HIDDEN)) {
        s_tool.id = id;
        tool_reset_motion();
    } else {
        s_tool.id = id;
    }

    lv_obj_clear_flag(s_tool.root, LV_OBJ_FLAG_HIDDEN);
    tool_set_meters(false);
    if (s_tool.tick) {
        lv_timer_set_period(s_tool.tick,
                            id == HELM_TOOL_ALTIMETER ? TOOL_TICK_SLOW_MS :
                            TOOL_TICK_FAST_MS);
        lv_timer_resume(s_tool.tick);
        tool_tick_cb(s_tool.tick);
    }
}

void helm_toolbox_hide(void)
{
    if (s_tool.root) {
        lv_obj_add_flag(s_tool.root, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_tool.tick) {
        lv_timer_pause(s_tool.tick);
    }
}

void helm_toolbox_unload(void)
{
    if (s_tool.tick) {
        lv_timer_delete(s_tool.tick);
        s_tool.tick = NULL;
    }

    memset(&s_tool, 0, sizeof(s_tool));
}

const char * helm_toolbox_title_for(helm_tool_id_t id)
{
    switch (id) {
    case HELM_TOOL_COMPASS:
        return "指南针";
    case HELM_TOOL_LEVEL:
        return "水平仪";
    case HELM_TOOL_GMETER:
        return "加速度计";
    case HELM_TOOL_ALTIMETER:
        return s_tool.alt_page ? "高度" : "气压";
    default:
        return "工具箱";
    }
}

const char * helm_toolbox_title(void)
{
    return helm_toolbox_title_for(s_tool.id);
}

static bool tool_alt_flip(int dir)
{
    int page;

    if (s_tool.id != HELM_TOOL_ALTIMETER) {
        return false;
    }

    page = s_tool.alt_page ? 1 : 0;
    page = (page + (dir > 0 ? 1 : -1) + 2) % 2;
    s_tool.alt_page = (page != 0);
    tool_poll_altimeter();
    if (s_tool.canvas) {
        lv_obj_invalidate(s_tool.canvas);
    }

    return true;
}

bool helm_toolbox_key1(void)
{
    return tool_alt_flip(-1);
}

bool helm_toolbox_key2(void)
{
    return tool_alt_flip(1);
}

static uint8_t sys_pct(uint64_t part, uint64_t total)
{
    if (total == 0) {
        return 0;
    }

    if (part >= total) {
        return 100;
    }

    return (uint8_t)((part * 100ull + total / 2ull) / total);
}

static void sys_fmt_bytes(char * buf, size_t n, uint64_t bytes)
{
    if (bytes >= (1024ull * 1024ull)) {
        unsigned mb10 = (unsigned)((bytes * 10ull) / (1024ull * 1024ull));

        lv_snprintf(buf, n, "%u.%u MB", mb10 / 10u, mb10 % 10u);
        return;
    }

    lv_snprintf(buf, n, "%u KB", (unsigned)(bytes / 1024ull));
}

/** @brief 磁盘用量：≥1MiB 用 M（256M/1024M），否则用 K。 */
static void sys_fmt_disk(char * buf, size_t n, uint64_t bytes)
{
    if (bytes >= (1024ull * 1024ull)) {
        unsigned m = (unsigned)((bytes + 512ull * 1024ull) / (1024ull * 1024ull));

        lv_snprintf(buf, n, "%uM", m);
        return;
    }

    lv_snprintf(buf, n, "%uK", (unsigned)((bytes + 512ull) / 1024ull));
}

static const char * const s_sys_disk_path[SYS_DISK_N] = {
    "/mnt/lfs",
    "/mnt/fat",
    "/mnt/kv",
};

static const char * const s_sys_disk_name[SYS_DISK_N] = {
    "记录盘",
    "地图盘",
    "数据盘",
};

static const helm_ico_id_t s_sys_disk_ico[SYS_DISK_N] = {
    HELM_ICO_SAVE,
    HELM_ICO_NAV,
    HELM_ICO_CHIP,
};

static bool sys_disk_refresh_one(unsigned idx)
{
    struct statfs sfs;
    uint64_t size;
    uint64_t avail;
    helm_sys_disk_t * d;

    if (idx >= SYS_DISK_N) {
        return false;
    }

    d = &s_sys.disk[idx];
    if (idx == SYS_DISK_LFS && myvendor_mtp_lfs_quiesce()) {
        return d->have;
    }

    memset(&sfs, 0, sizeof(sfs));
    myvendor_watchdog_hw_pet();
    if (statfs(s_sys_disk_path[idx], &sfs) != 0 || sfs.f_bsize == 0 ||
        sfs.f_blocks == 0) {
        myvendor_watchdog_hw_pet();
        return d->have;
    }

    myvendor_watchdog_hw_pet();
    size = (uint64_t)sfs.f_bsize * (uint64_t)sfs.f_blocks;
    avail = (uint64_t)sfs.f_bsize * (uint64_t)sfs.f_bavail;
    d->size = size;
    d->avail = avail;
    d->used = (size > avail) ? (size - avail) : 0ull;
    d->have = true;
    return true;
}

static void sys_disk_refresh_all(void)
{
    unsigned i;

    for (i = 0; i < SYS_DISK_N; i++) {
        if (!s_sys.disk[i].have) {
            (void)sys_disk_refresh_one(i);
        }
    }
}

static uint8_t sys_cpu_idle(void)
{
#if !defined(CONFIG_SCHED_CPULOAD_NONE)
    struct cpuload_s load;

    /* PID 0 是 idle 线程，active/total 即空闲比例。 */
    if (clock_cpuload(0, &load) == 0 && load.total > 0) {
        return (uint8_t)((100ull * (uint64_t)load.active) /
                         (uint64_t)load.total);
    }
#endif
    return 0;
}

static void sys_ram_info(uint64_t * free_b, uint64_t * total_b)
{
    struct mallinfo umem;
    struct mallinfo psram;

    umem = mallinfo();
    psram = board_psram_mallinfo();
    *total_b = (uint64_t)umem.arena + (uint64_t)psram.arena;
    *free_b = (uint64_t)umem.fordblks + (uint64_t)psram.fordblks;
}

static void sys_bar_fill(lv_obj_t * bar, uint32_t hex)
{
    lv_obj_t * fill;

    if (bar == NULL) {
        return;
    }

    fill = (lv_obj_t *)lv_obj_get_user_data(bar);
    if (fill) {
        lv_obj_set_style_bg_color(fill, helm_color(hex), 0);
    }
}

static uint32_t sys_used_fill(uint8_t pct)
{
    if (pct >= 90u) {
        return HELM_COLOR_HR;
    }

    if (pct >= 70u) {
        return HELM_COLOR_YEL;
    }

    return HELM_COLOR_CAD;
}

static void sys_set_row(lv_obj_t * val, lv_obj_t * bar, const char * text,
                        uint8_t pct, bool used_warn)
{
    if (val) {
        lv_label_set_text(val, text ? text : "--");
    }

    helm_pbar_set(bar, pct);
    sys_bar_fill(bar, used_warn ? sys_used_fill(pct) : HELM_COLOR_CAD);
}

static void sys_disk_paint(void)
{
    unsigned i;
    char line[32];
    char used[16];
    char total[16];

    for (i = 0; i < SYS_DISK_N; i++) {
        const helm_sys_disk_t * d = &s_sys.disk[i];

        if (!d->have) {
            sys_set_row(d->val, d->bar, "--", 0, true);
            continue;
        }

        sys_fmt_disk(used, sizeof(used), d->used);
        sys_fmt_disk(total, sizeof(total), d->size);
        lv_snprintf(line, sizeof(line), "%s/%s", used, total);
        sys_set_row(d->val, d->bar, line, sys_pct(d->used, d->size), true);
    }
}

static void sys_tick_cb(lv_timer_t * t)
{
    char line[40];
    char a[16];
    char b[16];
    uint64_t free_b;
    uint64_t total_b;
    uint8_t idle;

    LV_UNUSED(t);
    if (s_sys.root == NULL ||
        lv_obj_has_flag(s_sys.root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    sys_disk_refresh_all();
    sys_disk_paint();

    sys_ram_info(&free_b, &total_b);
    sys_fmt_bytes(a, sizeof(a), free_b);
    sys_fmt_bytes(b, sizeof(b), total_b);
    lv_snprintf(line, sizeof(line), "%s/%s", a, b);
    sys_set_row(s_sys.ram_val, s_sys.ram_bar, line, sys_pct(free_b, total_b),
                false);

    idle = sys_cpu_idle();
    if (idle > 100u) {
        idle = 100u;
    }

    lv_snprintf(line, sizeof(line), "%u%%", (unsigned)idle);
    sys_set_row(s_sys.cpu_val, s_sys.cpu_bar, line, idle, false);
}

static lv_obj_t * sys_card_create(lv_obj_t * parent, const char * title)
{
    lv_obj_t * card;

    card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    helm_style_card(card);
    lv_obj_set_style_pad_hor(card, 10, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    helm_label(card, helm_font_lab(), HELM_COLOR_INK, title);
    return card;
}

static void sys_add_metric(lv_obj_t * card, helm_ico_id_t ico, const char * title,
                           lv_obj_t ** val, lv_obj_t ** bar)
{
    lv_obj_t * block;
    lv_obj_t * head;
    lv_obj_t * left;
    lv_obj_t * icon;

    block = lv_obj_create(card);
    lv_obj_remove_style_all(block);
    lv_obj_set_width(block, lv_pct(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 4, 0);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);

    head = lv_obj_create(block);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    left = lv_obj_create(head);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 6, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    icon = helm_icon_create(left, ico, 16);
    helm_icon_set_color(icon, helm_color(HELM_COLOR_NAV));
    helm_label(left, helm_font_title(), HELM_COLOR_INK, title);
    *val = helm_label(head, helm_font_lab(), HELM_COLOR_INK, "--");

    *bar = helm_pbar_create(block);
    lv_obj_set_height(*bar, 8);
    lv_obj_set_style_radius(*bar, 4, 0);
    lv_obj_set_style_margin_top(*bar, 0, 0);
    lv_obj_set_style_bg_color(*bar, helm_color(HELM_COLOR_HAIR), 0);
}

lv_obj_t * helm_sysstat_build(lv_obj_t * parent)
{
    lv_obj_t * store;
    lv_obj_t * sys;
    unsigned i;

    memset(&s_sys, 0, sizeof(s_sys));
    s_sys.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_sys.root);
    helm_grow_y(s_sys.root);
    lv_obj_set_flex_flow(s_sys.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_sys.root, 8, 0);
    lv_obj_set_style_pad_ver(s_sys.root, 6, 0);
    lv_obj_set_style_pad_row(s_sys.root, 8, 0);
    lv_obj_add_flag(s_sys.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_sys.root, LV_OBJ_FLAG_SCROLLABLE);

    store = sys_card_create(s_sys.root, "存储");
    for (i = 0; i < SYS_DISK_N; i++) {
        sys_add_metric(store, s_sys_disk_ico[i], s_sys_disk_name[i],
                       &s_sys.disk[i].val, &s_sys.disk[i].bar);
    }

    sys = sys_card_create(s_sys.root, "系统");
    sys_add_metric(sys, HELM_ICO_CHIP, "RAM 剩余", &s_sys.ram_val,
                   &s_sys.ram_bar);
    sys_add_metric(sys, HELM_ICO_BOLT, "CPU 空闲", &s_sys.cpu_val,
                   &s_sys.cpu_bar);

    s_sys.tick = lv_timer_create(sys_tick_cb, SYS_TICK_MS, NULL);
    lv_timer_pause(s_sys.tick);
    return s_sys.root;
}

void helm_sysstat_show(void)
{
    if (s_sys.root == NULL) {
        return;
    }

    lv_obj_clear_flag(s_sys.root, LV_OBJ_FLAG_HIDDEN);
    {
        unsigned i;

        for (i = 0; i < SYS_DISK_N; i++) {
            s_sys.disk[i].have = false;
        }
    }
    if (s_sys.tick) {
        lv_timer_resume(s_sys.tick);
        sys_tick_cb(s_sys.tick);
    }
}

void helm_sysstat_hide(void)
{
    if (s_sys.root) {
        lv_obj_add_flag(s_sys.root, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_sys.tick) {
        lv_timer_pause(s_sys.tick);
    }
}

void helm_sysstat_unload(void)
{
    if (s_sys.tick) {
        lv_timer_delete(s_sys.tick);
        s_sys.tick = NULL;
    }

    memset(&s_sys, 0, sizeof(s_sys));
}
