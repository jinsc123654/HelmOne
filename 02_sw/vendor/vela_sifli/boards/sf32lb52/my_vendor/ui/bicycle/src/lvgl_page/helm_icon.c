/**
 * @file helm_icon.c
 * @brief Lucide 线稿（viewBox 24、stroke 2）+ 顶栏 16×16 2bpp 图标。
 *
 * 菜单大图标走描边。状态栏 ≤16px 用同一套点阵：卫星、蓝牙符、心形、
 * 齿盘、闪电。2bpp（0/33/66/100%）在黑底上给斜边抗锯齿；4bpp 半透明档
 * 太多，半透半反上会塌成灰边，不用。
 */

#include "helm_icon.h"

#include "helm_palette.h"

#include "lvgl/src/draw/lv_draw_arc.h"
#include "lvgl/src/draw/lv_draw_line.h"
#include "lvgl/src/draw/lv_draw_rect.h"

#include <stdint.h>

typedef struct {
    lv_layer_t * layer;
    lv_color_t color;
    int32_t ox;
    int32_t oy;
    int32_t w;
    int32_t stroke;
    /** 绘制透明度（取自对象的 style opa；见 helm_aa_draw 的注释）。 */
    lv_opa_t opa;
} helm_pen_t;

static int32_t helm_sx(const helm_pen_t * p, int x)
{
    return p->ox + (x * p->w + 12) / 24;
}

static int32_t helm_sy(const helm_pen_t * p, int y)
{
    return p->oy + (y * p->w + 12) / 24;
}

static void helm_pen_line(const helm_pen_t * p, int x1, int y1, int x2, int y2)
{
    lv_draw_line_dsc_t d;

    lv_draw_line_dsc_init(&d);
    d.color = p->color;
    d.width = p->stroke;
    d.opa = p->opa;
    d.round_start = 0;
    d.round_end = 0;
    d.p1.x = helm_sx(p, x1);
    d.p1.y = helm_sy(p, y1);
    d.p2.x = helm_sx(p, x2);
    d.p2.y = helm_sy(p, y2);
    lv_draw_line(p->layer, &d);
}

static void helm_pen_poly(const helm_pen_t * p, const int8_t * xy, int n, bool close)
{
    int i;

    for (i = 0; i < n - 1; i++) {
        helm_pen_line(p, xy[i * 2], xy[i * 2 + 1], xy[(i + 1) * 2],
                      xy[(i + 1) * 2 + 1]);
    }

    if (close && n > 2) {
        helm_pen_line(p, xy[(n - 1) * 2], xy[(n - 1) * 2 + 1], xy[0], xy[1]);
    }
}

static void helm_pen_circle(const helm_pen_t * p, int cx, int cy, int r)
{
    lv_draw_arc_dsc_t d;

    lv_draw_arc_dsc_init(&d);
    d.color = p->color;
    d.width = p->stroke;
    d.opa = p->opa;
    d.center.x = helm_sx(p, cx);
    d.center.y = helm_sy(p, cy);
    d.radius = (uint16_t)((r * p->w + 12) / 24);
    if (d.radius < 2) {
        d.radius = 2;
    }

    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(p->layer, &d);
}

static void helm_pen_arc(const helm_pen_t * p, int cx, int cy, int r,
                         int start, int end)
{
    lv_draw_arc_dsc_t d;

    lv_draw_arc_dsc_init(&d);
    d.color = p->color;
    d.width = p->stroke;
    d.opa = p->opa;
    d.center.x = helm_sx(p, cx);
    d.center.y = helm_sy(p, cy);
    d.radius = (uint16_t)((r * p->w + 12) / 24);
    d.start_angle = start;
    d.end_angle = end;
    lv_draw_arc(p->layer, &d);
}

static void helm_pen_rect(const helm_pen_t * p, int x, int y, int w, int h)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a;

    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_width = p->stroke;
    d.border_color = p->color;
    d.border_opa = LV_OPA_COVER;
    d.radius = 0;
    a.x1 = helm_sx(p, x);
    a.y1 = helm_sy(p, y);
    a.x2 = helm_sx(p, x + w);
    a.y2 = helm_sy(p, y + h);
    lv_draw_rect(p->layer, &d, &a);
}

static void helm_pen_fill_rect(const helm_pen_t * p, int x, int y, int w, int h)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a;

    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = p->color;
    d.border_width = 0;
    d.radius = 0;
    a.x1 = helm_sx(p, x);
    a.y1 = helm_sy(p, y);
    a.x2 = helm_sx(p, x + w);
    a.y2 = helm_sy(p, y + h);
    lv_draw_rect(p->layer, &d, &a);
}

#define HELM_DOT_N          16
#define HELM_DOT_AA_BYTES   (HELM_DOT_N * HELM_DOT_N / 4)

/* 16×16 2bpp，每字节 4 像素、高位在左。覆盖 0/1/2/3 → 透明到实心。 */
static const uint8_t s_aa_ble[HELM_DOT_AA_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x07u, 0x80u, 0x00u, 0x00u, 0x07u, 0xe0u, 0x00u,
    0x00u, 0x07u, 0xf8u, 0x00u, 0x03u, 0x87u, 0x6fu, 0x00u, 0x02u, 0xebu, 0x6eu, 0x00u,
    0x00u, 0xbfu, 0xf8u, 0x00u, 0x00u, 0x1fu, 0xd0u, 0x00u, 0x00u, 0x1fu, 0xd0u, 0x00u,
    0x00u, 0xbfu, 0xf8u, 0x00u, 0x02u, 0xebu, 0x6eu, 0x00u, 0x03u, 0x87u, 0x6fu, 0x00u,
    0x00u, 0x07u, 0xf8u, 0x00u, 0x00u, 0x07u, 0xe0u, 0x00u, 0x00u, 0x07u, 0x80u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u,
};

static const uint8_t s_aa_hr[HELM_DOT_AA_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x07u, 0xf4u, 0x1fu, 0xd0u, 0x1fu, 0xfdu, 0x7fu, 0xf4u,
    0x3fu, 0xfeu, 0xbfu, 0xfcu, 0x3fu, 0xffu, 0xffu, 0xfcu, 0x3fu, 0xffu, 0xffu, 0xfcu,
    0x3fu, 0xffu, 0xffu, 0xfcu, 0x1fu, 0xffu, 0xffu, 0xf4u, 0x0fu, 0xffu, 0xffu, 0xf0u,
    0x07u, 0xffu, 0xffu, 0xd0u, 0x01u, 0xffu, 0xffu, 0x40u, 0x00u, 0x7fu, 0xfdu, 0x00u,
    0x00u, 0x1fu, 0xf8u, 0x00u, 0x00u, 0x0bu, 0xe0u, 0x00u, 0x00u, 0x02u, 0x80u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u,
};

static const uint8_t s_aa_cad[HELM_DOT_AA_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x07u, 0xd0u, 0x00u, 0x00u, 0x07u, 0xd0u, 0x00u,
    0x00u, 0x1bu, 0xe4u, 0x00u, 0x00u, 0xbfu, 0xfeu, 0x00u, 0x01u, 0xf9u, 0x6fu, 0x40u,
    0x17u, 0xe0u, 0x0bu, 0xd4u, 0x3fu, 0xd0u, 0x07u, 0xfcu, 0x3fu, 0xd0u, 0x07u, 0xfcu,
    0x17u, 0xe0u, 0x0bu, 0xd4u, 0x01u, 0xf9u, 0x6fu, 0x40u, 0x00u, 0xbfu, 0xfeu, 0x00u,
    0x00u, 0x1bu, 0xe4u, 0x00u, 0x00u, 0x07u, 0xd0u, 0x00u, 0x00u, 0x07u, 0xd0u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u,
};

static const uint8_t s_aa_gps[HELM_DOT_AA_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x3cu, 0x00u, 0x00u, 0x00u, 0xebu, 0x00u, 0x00u, 0x00u, 0x3cu, 0x00u,
    0x0fu, 0xc3u, 0xfcu, 0x3cu, 0x0fu, 0xcfu, 0xffu, 0x3cu, 0x0fu, 0xcfu, 0xafu, 0x3cu,
    0x0fu, 0xcfu, 0xffu, 0x3cu, 0x0fu, 0xc3u, 0xfcu, 0x3cu, 0x00u, 0x00u, 0x3cu, 0x00u,
    0x00u, 0x00u, 0xc3u, 0x00u, 0x00u, 0x00u, 0x3cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u,
};

static const uint8_t s_aa_bolt[HELM_DOT_AA_BYTES] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0x02u, 0x80u, 0x00u,
    0x00u, 0x07u, 0x40u, 0x00u, 0x00u, 0x1fu, 0x40u, 0x00u, 0x00u, 0x3fu, 0x00u, 0x00u,
    0x00u, 0xbfu, 0x95u, 0x40u, 0x02u, 0xffu, 0xffu, 0x40u, 0x07u, 0xffu, 0xfeu, 0x00u,
    0x05u, 0x5bu, 0xf8u, 0x00u, 0x00u, 0x03u, 0xf0u, 0x00u, 0x00u, 0x07u, 0xd0u, 0x00u,
    0x00u, 0x07u, 0x40u, 0x00u, 0x00u, 0x0au, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u,
};

/* 半透屏暗绿会塌灰：1 档提到 ~55%，不用字库那种 33%。 */
static const lv_opa_t s_aa_opa[4] = {
    LV_OPA_TRANSP, 140, 200, LV_OPA_COVER
};

static const uint8_t * helm_aa_map(helm_ico_id_t id)
{
    switch (id) {
    case HELM_ICO_GPS:
        return s_aa_gps;
    case HELM_ICO_BLE:
        return s_aa_ble;
    case HELM_ICO_HR:
        return s_aa_hr;
    case HELM_ICO_CAD:
        return s_aa_cad;
    case HELM_ICO_BOLT:
        return s_aa_bolt;
    default:
        return NULL;
    }
}

static uint8_t helm_aa_px(const uint8_t * map, int x, int y)
{
    uint8_t b = map[y * (HELM_DOT_N / 4) + (x >> 2)];

    return (uint8_t)((b >> (6 - ((x & 3) << 1))) & 3u);
}

static void helm_aa_draw(lv_layer_t * layer, int32_t ox, int32_t oy, int scale,
                         const uint8_t * map, lv_color_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    int y;

    lv_draw_rect_dsc_init(&d);
    d.bg_color = color;
    /* ⚠ 透明度必须由**调用方**传进来：图标是自绘的，`lv_obj_set_style_opa(obj)` 对这个
     * 自绘路径不生效（对象本身没有自己的绘制内容可被叠加）⇒ 状态栏"透明显隐"的闪烁要靠这里
     * （用户 2026-09-27：要求闪烁用透明度、不要隐藏，因为隐藏会让 flex 把邻居挤动）。 */
    d.bg_opa = opa;
    d.border_width = 0;
    d.radius = 0;
    for (y = 0; y < HELM_DOT_N; y++) {
        int x = 0;

        while (x < HELM_DOT_N) {
            lv_area_t a;
            int x0;
            uint8_t v = helm_aa_px(map, x, y);

            if (v == 0u) {
                x++;
                continue;
            }

            x0 = x;
            while (x < HELM_DOT_N && helm_aa_px(map, x, y) == v) {
                x++;
            }

            /* ⚠ 这里是**逐像素**写 alpha（AA 覆盖度）——所以上面传进来的 `opa` 必须在这里
             * **相乘**，否则会被整段盖掉（状态栏"透明显隐"的闪烁就是这么失效的：
             * 用户 2026-09-27 实测"变成黄色但没闪烁"）。opa=COVER 时与原来逐像素等价。 */
            d.bg_opa = (lv_opa_t)(((unsigned)s_aa_opa[v] * (unsigned)opa) / 255u);
            a.x1 = ox + x0 * scale;
            a.y1 = oy + y * scale;
            a.x2 = ox + x * scale - 1;
            a.y2 = oy + (y + 1) * scale - 1;
            lv_draw_rect(layer, &d, &a);
        }
    }
}

static void helm_pen_dot(const helm_pen_t * p, int cx, int cy, int r)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a;
    int32_t R = (r * p->w + 12) / 24;

    if (R < 1) {
        R = 1;
    }

    lv_draw_rect_dsc_init(&d);
    d.bg_color = p->color;
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    d.border_width = 0;
    a.x1 = helm_sx(p, cx) - R;
    a.y1 = helm_sy(p, cy) - R;
    a.x2 = helm_sx(p, cx) + R;
    a.y2 = helm_sy(p, cy) + R;
    lv_draw_rect(p->layer, &d, &a);
}

static void helm_pen_cubic(const helm_pen_t * p, int x0, int y0, int x1, int y1,
                           int x2, int y2, int x3, int y3)
{
    int i;
    int px = x0;
    int py = y0;

    for (i = 1; i <= 8; i++) {
        float t = (float)i / 8.0f;
        float u = 1.0f - t;
        float x = u * u * u * (float)x0 + 3.0f * u * u * t * (float)x1 +
                  3.0f * u * t * t * (float)x2 + t * t * t * (float)x3;
        float y = u * u * u * (float)y0 + 3.0f * u * u * t * (float)y1 +
                  3.0f * u * t * t * (float)y2 + t * t * t * (float)y3;
        int ix = (int)(x + 0.5f);
        int iy = (int)(y + 0.5f);

        helm_pen_line(p, px, py, ix, iy);
        px = ix;
        py = iy;
    }
}

static void helm_draw_id(const helm_pen_t * p, helm_ico_id_t id)
{
    switch (id) {
    case HELM_ICO_NAV: {
        static const int8_t d[] = { 12, 4, 14, 12, 12, 20, 10, 12 };

        helm_pen_circle(p, 12, 12, 8);
        helm_pen_poly(p, d, 4, true);
        break;
    }
    case HELM_ICO_BLE: {
        static const int8_t d[] = { 7, 8, 17, 16, 12, 20, 12, 4, 17, 8, 7, 16 };

        helm_pen_poly(p, d, 6, false);
        break;
    }
    case HELM_ICO_USB:
        helm_pen_line(p, 12, 3, 12, 14);
        helm_pen_line(p, 12, 14, 7, 18);
        helm_pen_line(p, 12, 14, 17, 18);
        helm_pen_circle(p, 7, 19, 2);
        helm_pen_rect(p, 15, 17, 4, 4);
        break;
    case HELM_ICO_LIST:
        helm_pen_line(p, 6, 7, 18, 7);
        helm_pen_line(p, 6, 12, 18, 12);
        helm_pen_line(p, 6, 17, 18, 17);
        break;
    case HELM_ICO_GEAR:
        helm_pen_rect(p, 7, 7, 10, 10);
        helm_pen_rect(p, 10, 10, 4, 4);
        break;
    case HELM_ICO_SUN:
        helm_pen_circle(p, 12, 12, 4);
        helm_pen_line(p, 12, 3, 12, 6);
        helm_pen_line(p, 12, 18, 12, 21);
        helm_pen_line(p, 3, 12, 6, 12);
        helm_pen_line(p, 18, 12, 21, 12);
        break;
    case HELM_ICO_EYE: {
        static const int8_t d[] = { 3, 12, 7, 8, 12, 6, 17, 8, 21, 12,
                                    17, 16, 12, 18, 7, 16, 3, 12 };

        helm_pen_poly(p, d, 9, false);
        helm_pen_circle(p, 12, 12, 2);
        break;
    }
    case HELM_ICO_UNIT:
        helm_pen_line(p, 7, 17, 7, 7);
        helm_pen_line(p, 7, 7, 13, 17);
        helm_pen_line(p, 13, 17, 13, 7);
        helm_pen_line(p, 16, 7, 20, 7);
        helm_pen_line(p, 18, 7, 18, 17);
        break;
    case HELM_ICO_PAUSE:
        helm_pen_line(p, 8, 6, 8, 18);
        helm_pen_line(p, 16, 6, 16, 18);
        break;
    case HELM_ICO_BELL:
        helm_pen_arc(p, 12, 10, 6, 180, 360);
        helm_pen_line(p, 18, 10, 18, 14);
        helm_pen_line(p, 18, 14, 20, 17);
        helm_pen_line(p, 20, 17, 4, 17);
        helm_pen_line(p, 4, 17, 6, 14);
        helm_pen_line(p, 6, 14, 6, 10);
        helm_pen_arc(p, 12, 17, 3, 20, 160);
        break;
    case HELM_ICO_SOUND: {
        static const int8_t d[] = { 4, 9, 8, 9, 13, 5, 13, 19, 8, 15, 4, 15 };

        helm_pen_poly(p, d, 6, true);
        helm_pen_arc(p, 13, 12, 4, 300, 60);
        helm_pen_arc(p, 13, 12, 7, 300, 60);
        break;
    }
    case HELM_ICO_CALL:
        helm_pen_rect(p, 8, 4, 8, 16);
        helm_pen_line(p, 10, 18, 14, 18);
        break;
    case HELM_ICO_INBOX:
        helm_pen_rect(p, 4, 8, 16, 11);
        helm_pen_line(p, 4, 8, 12, 13);
        helm_pen_line(p, 12, 13, 20, 8);
        break;
    case HELM_ICO_CHIP:
        helm_pen_rect(p, 6, 6, 12, 12);
        helm_pen_line(p, 9, 3, 9, 6);
        helm_pen_line(p, 15, 3, 15, 6);
        helm_pen_line(p, 9, 18, 9, 21);
        helm_pen_line(p, 15, 18, 15, 21);
        helm_pen_line(p, 3, 9, 6, 9);
        helm_pen_line(p, 3, 15, 6, 15);
        helm_pen_line(p, 18, 9, 21, 9);
        helm_pen_line(p, 18, 15, 21, 15);
        break;
    case HELM_ICO_GPX:
        helm_pen_cubic(p, 4, 18, 8, 18, 8, 6, 12, 6);
        helm_pen_cubic(p, 12, 6, 16, 6, 16, 18, 20, 18);
        break;
    case HELM_ICO_STAR: {
        static const int8_t d[] = { 12, 4, 14, 10, 20, 10, 15, 13, 17, 19,
                                    12, 16, 7, 19, 9, 13, 4, 10, 10, 10 };

        helm_pen_poly(p, d, 10, true);
        break;
    }
    case HELM_ICO_REV:
        helm_pen_line(p, 7, 8, 15, 8);
        helm_pen_arc(p, 15, 12, 4, 270, 90);
        helm_pen_line(p, 15, 16, 9, 16);
        helm_pen_line(p, 11, 13, 7, 16);
        helm_pen_line(p, 7, 16, 11, 19);
        break;
    case HELM_ICO_PLAY: {
        static const int8_t d[] = { 8, 6, 18, 12, 8, 18 };

        helm_pen_poly(p, d, 3, true);
        break;
    }
    case HELM_ICO_TIME:
        helm_pen_circle(p, 12, 12, 8);
        helm_pen_line(p, 12, 8, 12, 12);
        helm_pen_line(p, 12, 12, 15, 14);
        break;
    case HELM_ICO_DIST:
        helm_pen_circle(p, 6, 18, 2);
        helm_pen_circle(p, 18, 6, 2);
        helm_pen_line(p, 8, 16, 16, 8);
        break;
    case HELM_ICO_AVG: {
        static const int8_t d[] = { 4, 16, 10, 10, 14, 14, 20, 6 };

        helm_pen_poly(p, d, 4, false);
        helm_pen_line(p, 16, 6, 20, 6);
        helm_pen_line(p, 20, 6, 20, 10);
        break;
    }
    case HELM_ICO_MAX: {
        static const int8_t d[] = { 5, 19, 12, 5, 19, 19 };

        helm_pen_poly(p, d, 3, true);
        break;
    }
    case HELM_ICO_CLIMB: {
        static const int8_t d[] = { 3, 18, 9, 10, 13, 13, 21, 4 };

        helm_pen_poly(p, d, 4, false);
        helm_pen_line(p, 3, 18, 21, 18);
        break;
    }
    case HELM_ICO_ALT: {
        static const int8_t d[] = { 4, 16, 12, 6, 20, 16 };

        helm_pen_poly(p, d, 3, true);
        helm_pen_line(p, 9, 16, 15, 16);
        break;
    }
    case HELM_ICO_SAVE:
        helm_pen_rect(p, 5, 5, 14, 14);
        helm_pen_line(p, 8, 5, 8, 10);
        helm_pen_line(p, 8, 10, 16, 10);
        helm_pen_line(p, 16, 10, 16, 5);
        break;
    case HELM_ICO_FLAG:
        helm_pen_line(p, 7, 20, 7, 4);
        helm_pen_line(p, 7, 4, 18, 8);
        helm_pen_line(p, 18, 8, 7, 12);
        break;
    case HELM_ICO_PIN:
        helm_pen_circle(p, 12, 10, 4);
        helm_pen_line(p, 12, 14, 12, 20);
        break;
    case HELM_ICO_SKIP: {
        static const int8_t d[] = { 6, 6, 14, 12, 6, 18 };

        helm_pen_poly(p, d, 3, true);
        helm_pen_line(p, 17, 6, 17, 18);
        break;
    }
    case HELM_ICO_HR:
        helm_pen_arc(p, 8, 9, 4, 180, 20);
        helm_pen_arc(p, 16, 9, 4, 160, 0);
        helm_pen_line(p, 5, 12, 12, 19);
        helm_pen_line(p, 12, 19, 19, 12);
        break;
    case HELM_ICO_CAD:
        helm_pen_circle(p, 8, 16, 3);
        helm_pen_circle(p, 17, 8, 3);
        helm_pen_line(p, 10, 14, 16, 9);
        break;
    case HELM_ICO_BATT:
        helm_pen_rect(p, 4, 8, 14, 10);
        helm_pen_fill_rect(p, 18, 11, 2, 4);
        helm_pen_line(p, 7, 11, 13, 11);
        break;
    case HELM_ICO_BOLT: {
        static const int8_t d[] = { 13, 3, 6, 13, 11, 13, 10, 21, 18, 10, 13, 10 };

        helm_pen_poly(p, d, 6, true);
        break;
    }
    case HELM_ICO_POWER:
        helm_pen_arc(p, 12, 13, 7, 240, 120);
        helm_pen_line(p, 12, 6, 12, 13);
        break;
    case HELM_ICO_WARN: {
        static const int8_t d[] = { 12, 4, 21, 20, 3, 20 };

        helm_pen_poly(p, d, 3, true);
        helm_pen_line(p, 12, 10, 12, 14);
        helm_pen_dot(p, 12, 17, 1);
        break;
    }
    case HELM_ICO_GPS:
        /* 卫星：左右翼板、中心体、天线。 */
        helm_pen_rect(p, 3, 9, 6, 6);
        helm_pen_rect(p, 15, 9, 6, 6);
        helm_pen_rect(p, 9, 8, 6, 8);
        helm_pen_line(p, 12, 8, 12, 4);
        helm_pen_line(p, 12, 4, 17, 6);
        break;
    case HELM_ICO_XMARK:
        helm_pen_line(p, 6, 6, 18, 18);
        helm_pen_line(p, 18, 6, 6, 18);
        break;
    case HELM_ICO_CHECK:
        helm_pen_line(p, 5, 12, 10, 17);
        helm_pen_line(p, 10, 17, 19, 7);
        break;
    case HELM_ICO_GO:
        helm_pen_line(p, 9, 6, 16, 12);
        helm_pen_line(p, 16, 12, 9, 18);
        break;
    default:
        break;
    }
}

static void helm_icon_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    helm_pen_t pen;
    lv_area_t coords;
    lv_coord_t w;
    lv_coord_t h;
    helm_ico_id_t id;

    if (obj == NULL || layer == NULL) {
        return;
    }

    id = (helm_ico_id_t)(uintptr_t)lv_obj_get_user_data(obj);
    if (id == HELM_ICO_NONE) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    w = lv_area_get_width(&coords);
    h = lv_area_get_height(&coords);
    if (w < 4 || h < 4) {
        return;
    }

    {
        const uint8_t * map = helm_aa_map(id);
        lv_color_t color = lv_obj_get_style_text_color(obj, 0);

        if (map != NULL) {
            int box = w < h ? (int)w : (int)h;

            if (box <= HELM_DOT_N) {
                int scale = box / HELM_DOT_N;
                int dw;
                int32_t ox;
                int32_t oy;

                if (scale < 1) {
                    scale = 1;
                }

                dw = HELM_DOT_N * scale;
                ox = coords.x1 + (w - dw) / 2;
                oy = coords.y1 + (h - dw) / 2;
                helm_aa_draw(layer, ox, oy, scale, map, color,
                             lv_obj_get_style_opa(obj, 0));
                return;
            }
        }
    }

    pen.layer = layer;
    pen.opa = lv_obj_get_style_opa(obj, 0);
    pen.color = lv_obj_get_style_text_color(obj, 0);
    pen.w = w < h ? w : h;
    pen.ox = coords.x1 + (w - pen.w) / 2;
    pen.oy = coords.y1 + (h - pen.w) / 2;
    pen.stroke = (2 * pen.w + 12) / 24;
    if (pen.stroke < 2) {
        pen.stroke = 2;
    }

    helm_draw_id(&pen, id);
}

lv_obj_t * helm_icon_create(lv_obj_t * parent, helm_ico_id_t id, lv_coord_t size)
{
    lv_obj_t * o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(o, helm_color(HELM_COLOR_INK), 0);
    lv_obj_set_user_data(o, (void *)(uintptr_t)id);
    lv_obj_add_event_cb(o, helm_icon_draw, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

void helm_icon_set(lv_obj_t * obj, helm_ico_id_t id)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_user_data(obj, (void *)(uintptr_t)id);
    lv_obj_invalidate(obj);
}

void helm_icon_set_color(lv_obj_t * obj, lv_color_t color)
{
    if (obj == NULL) {
        return;
    }

    /* 同色直接返回：图标是自绘字形（helm_icon_draw 每次重画），
     * 重复设色 = 重复重画整个矢量字形，白花时间。 */
    if (lv_color_eq(lv_obj_get_style_text_color(obj, 0), color)) {
        return;
    }

    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_invalidate(obj);
}
