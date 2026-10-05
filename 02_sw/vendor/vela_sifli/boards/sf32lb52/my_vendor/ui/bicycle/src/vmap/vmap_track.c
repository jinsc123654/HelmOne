/**
 * @file vmap_track.c
 * @brief vmap track 模块。
 */

#include "vmap_track.h"

#include "bicycle_config.h"
#include "bicycle_runtime.h"
#include "vmap_config.h"
#include "vmap_render.h"
#include "vmap_style.h"
#include "vmap_tile_system.h"
#include "vmap_track_arena.h"
#include "vmap_view.h"
#include "vmap_alloc.h"
#include "bicycle_c_debug.h"
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
#include "bicycle_inval_probe.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VMAP_TRACK_LINE_W     4  /* default; runtime uses vmap_style_current()->track_line_w */
#define VMAP_KM_BADGE_SZ      18

static const vmap_track_lap_config_t * track_lap_cfg(void)
{
    return &bicycle_config()->lap;
}

/**
 * @brief vmap track lap config get。
 * @return 请求的值。
 */
void vmap_track_lap_config_get(vmap_track_lap_config_t * out)
{
    if (!out) {
        return;
    }
    *out = bicycle_config()->lap;
}

/**
 * @brief vmap track lap config set。
 */
void vmap_track_lap_config_set(const vmap_track_lap_config_t * cfg)
{
    bicycle_config_t c;

    if (!cfg) {
        return;
    }

    bicycle_config_get(&c);
    if (cfg->min_distance_m > 0.0) {
        c.lap.min_distance_m = cfg->min_distance_m;
    }
    if (cfg->close_radius_m > 0.0) {
        c.lap.close_radius_m = cfg->close_radius_m;
    }
    if (cfg->max_course_diff_deg >= 0.0) {
        c.lap.max_course_diff_deg = cfg->max_course_diff_deg;
    }
    bicycle_config_set(&c);
}

/**
 * @brief vmap track lap config reset defaults。
 */
void vmap_track_lap_config_reset_defaults(void)
{
    bicycle_config_reset_defaults();
}

typedef struct {
    float lon;
    float lat;
    float speed_kph;
} track_pt_t;

typedef struct {
    lv_point_precise_t pt;
    float speed_kph;
} track_draw_pt_t;

typedef struct {
    float lon;
    float lat;
    uint16_t km;
} km_marker_t;

_Static_assert(sizeof(track_pt_t) == BICYCLE_TRACK_PT_BYTES,
    "BICYCLE_TRACK_PT_BYTES out of sync with track_pt_t");
_Static_assert(sizeof(track_draw_pt_t) == BICYCLE_TRACK_DRAW_PT_BYTES,
    "BICYCLE_TRACK_DRAW_PT_BYTES out of sync with track_draw_pt_t");

struct vmap_track {
    vmap_view_t * view;
    const lv_font_t * font;
    track_pt_t * pts;
    track_draw_pt_t * draw;
    km_marker_t km[VMAP_KM_MARKER_CAP];
    uint16_t count;
    uint16_t cap;
    uint16_t draw_n;
    uint16_t km_count;
    uint16_t km_next;
    /** @brief 当前圈显示中首个标记对应的会话公里数。 */
    uint16_t km_lap_base;
    uint16_t last_km_next;
    uint16_t last_count;
    uint16_t last_km_count;
    uint16_t last_paint_count;
    uint32_t last_render_rev;
    float last_paint_scale;
    int last_paint_zoom;
    bool full_repaint;
    float lap_start_lon;
    float lap_start_lat;
    float lap_ref_course_deg;
    bool lap_ref_course_valid;
    vmap_track_lap_cb_t lap_cb;
    void * lap_cb_ud;
    bool recording;
};

static double track_dist_m(float lat1, float lon1, float lat2, float lon2)
{
    const double r = 6371000.0;
    const double dlat = (double)(lat2 - lat1) * M_PI / 180.0;
    const double dlon = (double)(lon2 - lon1) * M_PI / 180.0;
    const double lat1r = (double)lat1 * M_PI / 180.0;
    const double lat2r = (double)lat2 * M_PI / 180.0;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0)
        + cos(lat1r) * cos(lat2r) * sin(dlon / 2.0) * sin(dlon / 2.0);

    return r * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static float track_bearing_deg(float lat1, float lon1, float lat2, float lon2)
{
    const double dlon = (double)(lon2 - lon1) * M_PI / 180.0;
    const double lat1r = (double)lat1 * M_PI / 180.0;
    const double lat2r = (double)lat2 * M_PI / 180.0;
    const double y = sin(dlon) * cos(lat2r);
    const double x = cos(lat1r) * sin(lat2r) - sin(lat1r) * cos(lat2r) * cos(dlon);
    double brng = atan2(y, x) * 180.0 / M_PI;

    if (brng < 0.0) {
        brng += 360.0;
    }
    return (float)brng;
}

static float track_angle_diff_deg(float a, float b)
{
    float d = (float)fabs((double)a - (double)b);

    if (d > 180.0f) {
        d = 360.0f - d;
    }
    return d;
}

static void track_lap_ref_reset(vmap_track_t * track)
{
    if (track) {
        track->lap_ref_course_valid = false;
    }
}

static void track_try_lock_lap_ref_course(vmap_track_t * track, float lon, float lat)
{
    const track_pt_t * prev;
    double dist_start;

    if (!track || track->lap_ref_course_valid || track->count < 2) {
        return;
    }

    dist_start = track_dist_m(lat, lon, track->lap_start_lat, track->lap_start_lon);
    if (dist_start <= track_lap_cfg()->close_radius_m) {
        return;
    }

    prev = &track->pts[track->count - 2];
    track->lap_ref_course_deg = track_bearing_deg(prev->lat, prev->lon, lat, lon);
    track->lap_ref_course_valid = true;
}

static bool track_lap_direction_ok(const vmap_track_t * track, float lon, float lat)
{
    const track_pt_t * prev;
    float motion_deg;
    float course_diff;
    const vmap_track_lap_config_t * lap_cfg = track_lap_cfg();

    if (lap_cfg->max_course_diff_deg <= 0.0) {
        return true;
    }

    if (!track->lap_ref_course_valid || track->count < 2) {
        return false;
    }

    prev = &track->pts[track->count - 2];
    motion_deg = track_bearing_deg(prev->lat, prev->lon, lat, lon);
    course_diff = track_angle_diff_deg(motion_deg, track->lap_ref_course_deg);

    return course_diff <= (float)lap_cfg->max_course_diff_deg;
}

/**
 * @brief vmap track color for speed。
 */
lv_color_t vmap_track_color_for_speed(float speed_kph)
{
    const vmap_style_t * style = vmap_style_current();

    if (!style->track_speed_gradient) {
        return lv_color_make(style->track_r, style->track_g, style->track_b);
    }

    float kph = speed_kph;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (kph < 0.0f) {
        kph = 0.0f;
    }

    /*
     * 青绿 → 蓝 → 洋红红。中间避开橙黄，才不会和底图路网糊成一条。
     * 0–16–32 km/h 两段，以上满红。
     */
    {
        const uint8_t r0 = 8;
        const uint8_t g0 = 196;
        const uint8_t b0 = 164;
        const uint8_t r1 = 40;
        const uint8_t g1 = 92;
        const uint8_t b1 = 255;
        const uint8_t r2 = 236;
        const uint8_t g2 = 36;
        const uint8_t b2 = 72;
        float t;

        if (kph <= 16.0f) {
            t = kph / 16.0f;
            r = (uint8_t)((float)r0 + ((float)r1 - (float)r0) * t);
            g = (uint8_t)((float)g0 + ((float)g1 - (float)g0) * t);
            b = (uint8_t)((float)b0 + ((float)b1 - (float)b0) * t);
        } else {
            t = (kph >= 32.0f) ? 1.0f : ((kph - 16.0f) / 16.0f);
            r = (uint8_t)((float)r1 + ((float)r2 - (float)r1) * t);
            g = (uint8_t)((float)g1 + ((float)g2 - (float)g1) * t);
            b = (uint8_t)((float)b1 + ((float)b2 - (float)b1) * t);
        }
    }

    return lv_color_make(r, g, b);
}

static uint16_t track_speed_rgb565(float speed_kph)
{
    return lv_color_to_u16(vmap_track_color_for_speed(speed_kph));
}

static bool track_geo_to_canvas(const vmap_track_t * track, float lon, float lat,
    int32_t * out_x, int32_t * out_y)
{
    if (!track || !track->view) {
        return false;
    }

    return vmap_view_geo_to_canvas_raw(track->view, (double)lon, (double)lat,
        out_x, out_y);
}

#define TRACK_CANVAS_VIS_PAD 48

static int32_t track_vis_pad(const vmap_track_t * track)
{
    double scale = 1.0;

    if (!track || !track->view) {
        return TRACK_CANVAS_VIS_PAD;
    }

    scale = vmap_view_get_scale(track->view);
    if (scale < 0.25) {
        scale = 0.25;
    }

    {
        int32_t pad = (int32_t)((double)TRACK_CANVAS_VIS_PAD / scale);

        if (pad > 640) {
            pad = 640;
        }
        if (pad < TRACK_CANVAS_VIS_PAD) {
            pad = TRACK_CANVAS_VIS_PAD;
        }
        return pad;
    }
}

static bool track_pt_in_view_pad(const vmap_track_t * track, int32_t x, int32_t y)
{
    return track && track->view
        && vmap_view_canvas_point_in_pad(track->view, x, y, track_vis_pad(track));
}

static uint16_t track_nearest_index(const vmap_track_t * track, uint16_t count,
    double lon, double lat)
{
    uint16_t best = 0;
    double best_d = 1e99;
    uint16_t stride = 1u;
    uint16_t i;
    uint16_t lo;
    uint16_t hi;

    if (count > 512u) {
        stride = (uint16_t)((count + 255u) / 256u);
        if (stride < 1u) {
            stride = 1u;
        }
    }

    for (i = 0; i < count; i = (uint16_t)(i + stride)) {
        const double d = track_dist_m((float)lat, (float)lon,
            track->pts[i].lat, track->pts[i].lon);

        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }

    lo = best > stride ? (uint16_t)(best - stride) : 0u;
    hi = (uint16_t)(best + stride);
    if (hi >= count) {
        hi = (uint16_t)(count - 1u);
    }
    for (i = lo; i <= hi; i++) {
        const double d = track_dist_m((float)lat, (float)lon,
            track->pts[i].lat, track->pts[i].lon);

        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }

    return best;
}

#define TRACK_SPAN_MAX   24u
#define TRACK_SPAN_MISS  12u

typedef struct {
    uint16_t first;
    uint16_t last;
} track_span_t;

static void track_span_close(track_span_t * spans, uint8_t * n, uint8_t max_n,
    uint16_t first, uint16_t last, uint16_t count)
{
    if (*n >= max_n || first > last) {
        return;
    }

    if (first > 0u) {
        first--;
    }
    if (last + 1u < count) {
        last++;
    }
    if (first >= last) {
        return;
    }
    spans[*n].first = first;
    spans[*n].last = last;
    (*n)++;
}

/** @brief 视口+边距的经纬度盒（偏松，只用来跳过墨卡托）。 */
static bool track_view_lonlat_box(const vmap_track_t * track,
    float * lon0, float * lat0, float * lon1, float * lat1)
{
    double lon;
    double lat;
    double scale;
    double half_lon;
    double half_lat;
    int32_t cw = 0;
    int32_t ch = 0;
    int32_t pad;
    uint32_t map_size;

    if (!track || !track->view || !lon0 || !lat0 || !lon1 || !lat1) {
        return false;
    }

    vmap_view_get_center(track->view, &lon, &lat);
    vmap_view_get_buffer_size(track->view, &cw, &ch);
    pad = track_vis_pad(track);
    scale = vmap_view_get_scale(track->view);
    if (scale < 0.25) {
        scale = 0.25;
    }
    map_size = vmap_map_size(vmap_view_get_zoom(track->view));
    if (map_size < 256u) {
        return false;
    }

    half_lon = (((double)cw + 2.0 * (double)pad) / scale)
        / (double)map_size * 360.0;
    half_lat = (((double)ch + 2.0 * (double)pad) / scale)
        / (double)map_size * 360.0;
    half_lon *= 1.5;
    half_lat *= 1.5;
    if (half_lon < 0.002) {
        half_lon = 0.002;
    }
    if (half_lat < 0.002) {
        half_lat = 0.002;
    }

    *lon0 = (float)(lon - half_lon);
    *lon1 = (float)(lon + half_lon);
    *lat0 = (float)(lat - half_lat);
    *lat1 = (float)(lat + half_lat);
    return true;
}

/**
 * @brief 收集视口内全部轨迹跨度（去程 + 回程）。
 *
 * 旧逻辑只从最新点往回扫 `draw_cap*3` 个点。骑出 10 km 再回来时，
 * 去程仍在 RAM，但不在这段窗口里，瓦片重贴后只剩公里标。
 */
static uint8_t track_collect_visible_spans(const vmap_track_t * track,
    uint16_t count, track_span_t * spans, uint8_t max_spans)
{
    vmap_view_proj_t proj;
    uint8_t n = 0;
    bool in_span = false;
    bool have_box;
    uint16_t first = 0;
    uint16_t last = 0;
    uint16_t miss = 0;
    uint16_t i;
    float lon0 = 0.f;
    float lat0 = 0.f;
    float lon1 = 0.f;
    float lat1 = 0.f;

    if (!track || !track->view || !spans || max_spans == 0u || count < 2u) {
        return 0;
    }

    have_box = track_view_lonlat_box(track, &lon0, &lat0, &lon1, &lat1);
    vmap_view_proj_begin(track->view, &proj);
    for (i = 0; i < count; i++) {
        int32_t x = 0;
        int32_t y = 0;
        bool in_view;

        if (have_box
            && (track->pts[i].lon < lon0 || track->pts[i].lon > lon1
                || track->pts[i].lat < lat0 || track->pts[i].lat > lat1)) {
            in_view = false;
        } else {
            vmap_view_proj_point(&proj, (double)track->pts[i].lon,
                (double)track->pts[i].lat, &x, &y);
            in_view = track_pt_in_view_pad(track, x, y);
        }
        if (in_view) {
            if (!in_span) {
                first = i;
                in_span = true;
            }
            last = i;
            miss = 0;
        } else if (in_span) {
            miss++;
            if (miss >= TRACK_SPAN_MISS) {
                track_span_close(spans, &n, max_spans, first, last, count);
                in_span = false;
                miss = 0;
            }
        }
    }

    if (in_span) {
        track_span_close(spans, &n, max_spans, first, last, count);
    }

    if (n == 0u) {
        double lon;
        double lat;
        uint16_t center;
        uint16_t half;
        uint16_t cap = bicycle_config_track_draw_cap();

        vmap_view_get_center(track->view, &lon, &lat);
        center = track_nearest_index(track, count, lon, lat);
        half = cap / 2u;
        if (half < 1u) {
            half = 1u;
        }
        spans[0].first = center > half ? (uint16_t)(center - half) : 0u;
        spans[0].last = (uint16_t)(center + half);
        if (spans[0].last >= count) {
            spans[0].last = (uint16_t)(count - 1u);
        }
        n = 1u;
    }

    return n;
}

#define TRACK_DISP_KF_WARM  32u
#define TRACK_DISP_KF_R     49.0f
#define TRACK_DISP_KF_QP    0.08f
#define TRACK_DISP_KF_QV    0.90f
#define TRACK_DISP_KF_M_LAT 111320.0f

typedef struct {
    float x;
    float v;
    float p00;
    float p01;
    float p11;
} track_axis_kf_t;

typedef struct {
    bool on;
    float lat0;
    float lon0;
    float m_per_lon;
    track_axis_kf_t e;
    track_axis_kf_t n;
} track_disp_kf_t;

static void track_axis_kf_init(track_axis_kf_t * a, float z)
{
    a->x = z;
    a->v = 0.0f;
    a->p00 = TRACK_DISP_KF_R;
    a->p01 = 0.0f;
    a->p11 = 16.0f;
}

static void track_axis_kf_update(track_axis_kf_t * a, float z)
{
    float p00;
    float p01;
    float p11;
    float s;
    float k0;
    float k1;
    float innov;

    a->x += a->v;
    p00 = a->p00 + 2.0f * a->p01 + a->p11 + TRACK_DISP_KF_QP;
    p01 = a->p01 + a->p11;
    p11 = a->p11 + TRACK_DISP_KF_QV;

    s = p00 + TRACK_DISP_KF_R;
    if (s < 1e-6f) {
        s = 1e-6f;
    }
    k0 = p00 / s;
    k1 = p01 / s;
    innov = z - a->x;
    a->x += k0 * innov;
    a->v += k1 * innov;
    a->p00 = (1.0f - k0) * p00;
    a->p01 = (1.0f - k0) * p01;
    a->p11 = p11 - k1 * p01;
    if (a->p00 < 1e-4f) {
        a->p00 = 1e-4f;
    }
    if (a->p11 < 1e-4f) {
        a->p11 = 1e-4f;
    }
}

static void track_disp_kf_init(track_disp_kf_t * kf, float lon, float lat)
{
    const float lat_rad = lat * (float)(M_PI / 180.0);

    kf->on = true;
    kf->lat0 = lat;
    kf->lon0 = lon;
    kf->m_per_lon = TRACK_DISP_KF_M_LAT * cosf(lat_rad);
    if (kf->m_per_lon < 10000.0f) {
        kf->m_per_lon = 10000.0f;
    }
    track_axis_kf_init(&kf->e, 0.0f);
    track_axis_kf_init(&kf->n, 0.0f);
}

static void track_disp_kf_update(track_disp_kf_t * kf, float lon, float lat)
{
    float e;
    float n;

    if (!kf->on) {
        track_disp_kf_init(kf, lon, lat);
        return;
    }

    e = (lon - kf->lon0) * kf->m_per_lon;
    n = (lat - kf->lat0) * TRACK_DISP_KF_M_LAT;
    track_axis_kf_update(&kf->e, e);
    track_axis_kf_update(&kf->n, n);
}

static void track_disp_kf_get(const track_disp_kf_t * kf, float * lon, float * lat)
{
    *lon = kf->lon0 + kf->e.x / kf->m_per_lon;
    *lat = kf->lat0 + kf->n.x / TRACK_DISP_KF_M_LAT;
}

static bool track_project_ll(const vmap_track_t * track, float lon, float lat,
    float speed_kph, track_draw_pt_t * out)
{
    int32_t x = 0;
    int32_t y = 0;

    if (!track_geo_to_canvas(track, lon, lat, &x, &y)
        || !track->view || !vmap_view_canvas_point_sane(track->view, x, y)) {
        return false;
    }

    out->pt.x = (lv_coord_t)x;
    out->pt.y = (lv_coord_t)y;
    out->speed_kph = speed_kph;
    return true;
}

static bool track_project_pt_sane(const vmap_track_t * track, uint16_t idx,
    track_draw_pt_t * out)
{
    return track_project_ll(track, track->pts[idx].lon, track->pts[idx].lat,
        track->pts[idx].speed_kph, out);
}

static int32_t track_line_width(void)
{
    const vmap_style_t * style = vmap_style_current();
    int32_t line_w = (int32_t)style->track_line_w;

    if (line_w < 1) {
        line_w = VMAP_TRACK_LINE_W;
    }
    return line_w;
}

typedef struct {
    double x;
    double y;
    float speed_kph;
} track_fpt_t;

static bool track_project_ll_f(const vmap_track_t * track,
    const vmap_view_proj_t * proj, float lon, float lat, float speed_kph,
    track_fpt_t * out)
{
    double x = 0.0;
    double y = 0.0;

    if (!track || !track->view || !proj || !out) {
        return false;
    }

    vmap_view_proj_point_f(proj, (double)lon, (double)lat, &x, &y);
    if (!vmap_view_canvas_point_sane(track->view, (int32_t)lround(x),
            (int32_t)lround(y))) {
        return false;
    }

    out->x = x;
    out->y = y;
    out->speed_kph = speed_kph;
    return true;
}

static uint16_t track_seg_color565_kph(float a_kph, float b_kph)
{
    return track_speed_rgb565((a_kph + b_kph) * 0.5f);
}

/** @brief 单段轨迹最大屏幕长度（防止错误投影）。 */
static int32_t track_max_seg_px(const vmap_track_t * track)
{
    int32_t cw = 0;
    int32_t ch = 0;

    if (!track || !track->view) {
        return 256;
    }

    vmap_view_get_buffer_size(track->view, &cw, &ch);
    return (cw > ch ? cw : ch) / 2;
}

/** @brief 亚像素描边；端点圆帽盖住折角。 */
static bool track_paint_draw_f(vmap_track_t * track, vmap_canvas_t * canvas,
    const track_fpt_t * p0, const track_fpt_t * p1)
{
    double dx;
    double dy;
    double max_px;
    double hw;
    uint16_t color;

    if (!track || !canvas || !p0 || !p1) {
        return false;
    }

    max_px = (double)track_max_seg_px(track);
    dx = p1->x - p0->x;
    dy = p1->y - p0->y;
    if (dx * dx + dy * dy > max_px * max_px) {
        return false;
    }

    hw = (double)track_line_width() * 0.5;
    color = track_seg_color565_kph(p0->speed_kph, p1->speed_kph);
    vmap_render_draw_thick_line_f(canvas, p0->x, p0->y, p1->x, p1->y,
        (double)track_line_width(), color);
    vmap_render_fill_disc_f(canvas, p0->x, p0->y, hw, color);
    vmap_render_fill_disc_f(canvas, p1->x, p1->y, hw, color);
    return true;
}

#define TRACK_DISP_MIN_PX2 1.44 /* 跳过 <1.2px 的碎段，避免 3m 点锯齿 */

/**
 * @brief 只把滤波后的折线画上画布。pts[] / GPX / 里程仍是原始点。
 *
 * 分数墨卡托 + RGB565 亚像素描边。Chaikin 一轮只圆显示折角。
 */
static void track_paint_span_segments(vmap_track_t * track, vmap_canvas_t * canvas,
    uint16_t first, uint16_t last, uint16_t cap)
{
    vmap_view_proj_t proj;
    track_disp_kf_t kf;
    track_fpt_t raw_prev;
    track_fpt_t cha_prev;
    const uint32_t span = (uint32_t)(last - first + 1u);
    uint32_t step = 1u;
    uint16_t warm0;
    uint16_t i;
    bool have_raw = false;
    bool have_cha = false;
    bool first_pair = true;

    if (!track || !canvas || !track->view || span <= 1u) {
        return;
    }

    if (span > cap && cap > 1u) {
        step = (span + cap - 1u) / cap;
    }
    warm0 = first > TRACK_DISP_KF_WARM ? (uint16_t)(first - TRACK_DISP_KF_WARM) : 0u;

    memset(&kf, 0, sizeof(kf));
    memset(&raw_prev, 0, sizeof(raw_prev));
    memset(&cha_prev, 0, sizeof(cha_prev));
    track_disp_kf_init(&kf, track->pts[warm0].lon, track->pts[warm0].lat);
    vmap_view_proj_begin(track->view, &proj);

    for (i = warm0; i <= last; i++) {
        float slon;
        float slat;
        track_fpt_t cur;
        bool emit;

        if (i > warm0) {
            track_disp_kf_update(&kf, track->pts[i].lon, track->pts[i].lat);
        }
        if (i < first) {
            continue;
        }

        emit = (i == first) || (i == last)
            || (((uint32_t)(i - first) % step) == 0u);
        if (!emit) {
            continue;
        }

        track_disp_kf_get(&kf, &slon, &slat);
        if (!track_project_ll_f(track, &proj, slon, slat, track->pts[i].speed_kph,
                &cur)) {
            have_raw = false;
            have_cha = false;
            first_pair = true;
            continue;
        }

        if (have_raw) {
            const double dx = cur.x - raw_prev.x;
            const double dy = cur.y - raw_prev.y;
            track_fpt_t q;
            track_fpt_t r;

            if (i != last && (dx * dx + dy * dy) < TRACK_DISP_MIN_PX2) {
                continue;
            }

            q.x = raw_prev.x * 0.75 + cur.x * 0.25;
            q.y = raw_prev.y * 0.75 + cur.y * 0.25;
            q.speed_kph = raw_prev.speed_kph * 0.75f + cur.speed_kph * 0.25f;
            r.x = raw_prev.x * 0.25 + cur.x * 0.75;
            r.y = raw_prev.y * 0.25 + cur.y * 0.75;
            r.speed_kph = raw_prev.speed_kph * 0.25f + cur.speed_kph * 0.75f;

            if (first_pair) {
                (void)track_paint_draw_f(track, canvas, &raw_prev, &q);
                (void)track_paint_draw_f(track, canvas, &q, &r);
                first_pair = false;
            } else {
                (void)track_paint_draw_f(track, canvas, &cha_prev, &q);
                (void)track_paint_draw_f(track, canvas, &q, &r);
            }
            cha_prev = r;
            have_cha = true;
        }
        raw_prev = cur;
        have_raw = true;
    }

    if (have_cha && have_raw) {
        (void)track_paint_draw_f(track, canvas, &cha_prev, &raw_prev);
    }
}

static void track_paint_tail(vmap_track_t * track, vmap_canvas_t * canvas)
{
    const uint16_t count = track->count;
    uint16_t first;

    if (!track || !canvas || count < 2) {
        return;
    }

    first = track->last_paint_count;
    if (first >= count) {
        return;
    }
    if (first > 0u) {
        first--;
    }
    track_paint_span_segments(track, canvas, first, (uint16_t)(count - 1u),
        bicycle_config_track_draw_cap());
}

static void track_dirty_include(lv_area_t * dirty, bool * valid,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (!dirty || !valid) {
        return;
    }

    if (!*valid) {
        dirty->x1 = x1;
        dirty->y1 = y1;
        dirty->x2 = x2;
        dirty->y2 = y2;
        *valid = true;
        return;
    }

    if (x1 < dirty->x1) {
        dirty->x1 = x1;
    }
    if (y1 < dirty->y1) {
        dirty->y1 = y1;
    }
    if (x2 > dirty->x2) {
        dirty->x2 = x2;
    }
    if (y2 > dirty->y2) {
        dirty->y2 = y2;
    }
}

static void track_tail_dirty(const vmap_track_t * track, lv_area_t * dirty,
    bool * valid)
{
    track_draw_pt_t a;
    track_draw_pt_t b;
    int32_t pad;

    if (!track || track->count < 2u) {
        return;
    }

    if (!track_project_pt_sane(track, (uint16_t)(track->count - 2u), &a)
        || !track_project_pt_sane(track, (uint16_t)(track->count - 1u), &b)) {
        return;
    }

    pad = track_line_width() / 2 + 3;
    track_dirty_include(dirty, valid,
        ((int32_t)a.pt.x < (int32_t)b.pt.x ? a.pt.x : b.pt.x) - pad,
        ((int32_t)a.pt.y < (int32_t)b.pt.y ? a.pt.y : b.pt.y) - pad,
        ((int32_t)a.pt.x > (int32_t)b.pt.x ? a.pt.x : b.pt.x) + pad,
        ((int32_t)a.pt.y > (int32_t)b.pt.y ? a.pt.y : b.pt.y) + pad);
}

static uint16_t track_km_slot_index(uint16_t km_num, uint16_t km_cap)
{
    if (km_num == 0 || km_cap == 0) {
        return 0;
    }
    if (km_num <= km_cap) {
        return (uint16_t)(km_num - 1u);
    }
    return (uint16_t)((km_num - 1u) % km_cap);
}

static void track_paint_km_marker_slot(vmap_track_t * track, vmap_canvas_t * canvas,
    uint16_t idx)
{
    const int32_t half = VMAP_KM_BADGE_SZ / 2;
    const vmap_style_t * style = vmap_style_current();
    const uint16_t badge = vmap_style_rgb565(style->km_badge_r, style->km_badge_g,
        style->km_badge_b);
    int32_t x = 0;
    int32_t y = 0;

    if (!track || !canvas || idx >= bicycle_config_km_marker_max()) {
        return;
    }

    if (!track_geo_to_canvas(track, track->km[idx].lon, track->km[idx].lat, &x, &y)) {
        return;
    }

    vmap_render_fill_disc(canvas, x, y, half, badge);
}

static void track_paint_km_markers(vmap_track_t * track, vmap_canvas_t * canvas,
    uint16_t from_km)
{
    const uint16_t km_cap = bicycle_config_km_marker_max();
    uint16_t km;

    if (!track || !canvas || track->km_next <= track->km_lap_base) {
        return;
    }

    if (from_km < track->km_lap_base) {
        from_km = track->km_lap_base;
    }

    for (km = from_km; km < track->km_next; km++) {
        const uint16_t idx = track_km_slot_index(km, km_cap);

        track_paint_km_marker_slot(track, canvas, idx);
    }
}

static void track_paint_km_marker_label_slot(vmap_track_t * track, lv_layer_t * layer,
    lv_draw_label_dsc_t * label_dsc, uint16_t idx)
{
    char buf[8];
    int32_t x = 0;
    int32_t y = 0;
    lv_area_t text_area;
    lv_point_t text_sz;

    if (!track || !layer || !label_dsc || idx >= bicycle_config_km_marker_max()) {
        return;
    }

    if (!track_geo_to_canvas(track, track->km[idx].lon, track->km[idx].lat, &x, &y)) {
        return;
    }

    snprintf(buf, sizeof(buf), "%u", (unsigned)track->km[idx].km);
    lv_text_get_size(&text_sz, buf, label_dsc->font, 0, 0,
        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    text_area.x1 = (lv_coord_t)(x - text_sz.x / 2);
    text_area.y1 = (lv_coord_t)(y - text_sz.y / 2);
    text_area.x2 = (lv_coord_t)(text_area.x1 + text_sz.x);
    text_area.y2 = (lv_coord_t)(text_area.y1 + text_sz.y);
    label_dsc->text = buf;
    lv_draw_label(layer, label_dsc, &text_area);
}

static void track_paint_km_marker_label_km(vmap_track_t * track, lv_layer_t * layer,
    lv_draw_label_dsc_t * label_dsc, uint16_t km_num)
{
    const uint16_t km_cap = bicycle_config_km_marker_max();
    const uint16_t idx = track_km_slot_index(km_num, km_cap);

    track_paint_km_marker_label_slot(track, layer, label_dsc, idx);
}

static void track_paint_km_marker_labels(vmap_track_t * track, uint16_t from_km)
{
    lv_obj_t * canvas;
    lv_display_t * disp;
    lv_layer_t layer;
    lv_draw_label_dsc_t label_dsc;
    uint16_t km;

    if (!track || !track->font || track->km_next <= track->km_lap_base || !track->view) {
        return;
    }

    if (from_km < track->km_lap_base) {
        from_km = track->km_lap_base;
    }

    canvas = vmap_view_get_obj(track->view);
    if (canvas == NULL) {
        return;
    }

    lv_canvas_init_layer(canvas, &layer);
    lv_draw_label_dsc_init(&label_dsc);
    /*
     * 角标底浅就写黑字、底深就写白字。原来恒为白色 ⇒ outdoor（夜间）那份**白角标上白字**
     * 根本看不见 —— 用户 2026-09-27 报的"里程碑数字和圆都是白色"。判据与角标取色同源
     * （同一个 style->km_badge_*），用亮度阈值分档。
     */
    {
        const vmap_style_t * style = vmap_style_current();
        const unsigned luma = (unsigned)style->km_badge_r * 30u
            + (unsigned)style->km_badge_g * 59u + (unsigned)style->km_badge_b * 11u;

        label_dsc.color = (luma > 12800u) ? lv_color_black() : lv_color_white();
    }
    label_dsc.font = track->font;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.text_local = 1;

    for (km = from_km; km < track->km_next; km++) {
        track_paint_km_marker_label_km(track, &layer, &label_dsc, km);
    }

    /*
     * finish_layer invalidates the complete canvas. The refresh caller knows
     * the changed track/marker bounds and performs a smaller invalidation.
     */
    disp = lv_obj_get_display(canvas);
    lv_display_enable_invalidation(disp, false);
    lv_canvas_finish_layer(canvas, &layer);
    lv_display_enable_invalidation(disp, true);
}

static void track_km_dirty(const vmap_track_t * track, uint16_t from_km,
    lv_area_t * dirty, bool * valid)
{
    const uint16_t km_cap = bicycle_config_km_marker_max();
    const int32_t pad = VMAP_KM_BADGE_SZ / 2 + 8;
    uint16_t km;

    if (!track || !track->view || from_km >= track->km_next) {
        return;
    }
    if (from_km < track->km_lap_base) {
        from_km = track->km_lap_base;
    }

    for (km = from_km; km < track->km_next; km++) {
        const uint16_t idx = track_km_slot_index(km, km_cap);
        int32_t x = 0;
        int32_t y = 0;

        if (track_geo_to_canvas(track, track->km[idx].lon, track->km[idx].lat,
                &x, &y)) {
            track_dirty_include(dirty, valid, x - pad, y - pad, x + pad, y + pad);
        }
    }
}

static uint16_t track_paint_polyline(vmap_track_t * track, vmap_canvas_t * canvas)
{
    track_span_t spans[TRACK_SPAN_MAX];
    const uint16_t count = track->count;
    const uint16_t cap = bicycle_config_track_draw_cap();
    uint16_t drawn = 0;
    uint8_t n;
    uint8_t i;
    uint16_t cap_each;

    if (!track->view || count < 2) {
        return 0;
    }

    n = track_collect_visible_spans(track, count, spans, TRACK_SPAN_MAX);
    if (n == 0u) {
        return 0;
    }

    cap_each = (uint16_t)(cap / n);
    if (cap_each < 32u) {
        cap_each = 32u;
    }

    for (i = 0; i < n; i++) {
        if (spans[i].first >= spans[i].last) {
            continue;
        }

        track_paint_span_segments(track, canvas, spans[i].first, spans[i].last,
            cap_each);
        drawn = (uint16_t)(drawn + (spans[i].last - spans[i].first + 1u));
    }

    return drawn;
}

static void track_sync_paint_view_state(vmap_track_t * track)
{
    if (!track || !track->view) {
        return;
    }

    track->last_paint_scale = (float)vmap_view_get_scale(track->view);
    track->last_paint_zoom = vmap_view_get_zoom(track->view);
}

static bool track_view_paint_changed(const vmap_track_t * track)
{
    if (!track || !track->view) {
        return false;
    }

    return vmap_view_get_scale(track->view) != (double)track->last_paint_scale
        || vmap_view_get_zoom(track->view) != track->last_paint_zoom;
}

/**
 * @brief 把 REC 线、公里圆标、公里数字盖进画布（标签之上；数字最后）。
 */
void vmap_track_paint_to_canvas(vmap_track_t * track, vmap_canvas_t * canvas)
{
    const bool full_canvas = canvas && canvas->clip_n == 0u;

    if (!track || !canvas || !canvas->buf) {
        return;
    }

    if (track->count < 2 && track->km_next <= track->km_lap_base) {
        if (full_canvas) {
            track->draw_n = 0;
            track->last_paint_count = track->count;
            track->full_repaint = false;
        }
        return;
    }

    track->draw_n = track_paint_polyline(track, canvas);
    track_paint_km_markers(track, canvas, 0);
    /* REC 数字在线/圆标之上。 */
    track_paint_km_marker_labels(track, 0);
    if (!full_canvas) {
        /*
         * Scroll-rebase only painted edge strips.  Do not claim that points
         * added during the pump were painted in the untouched interior.
         */
        return;
    }
    track->last_paint_count = track->count;
    track->last_count = track->count;
    track->last_km_count = track->km_count;
    track->last_km_next = track->km_next;
    track->last_render_rev = vmap_view_render_revision(track->view);
    track_sync_paint_view_state(track);
    track->full_repaint = false;
}

static bool track_paint_incremental(vmap_track_t * track)
{
    vmap_canvas_t canvas;

    if (!track || !track->view || track->count < 2) {
        return false;
    }

    if (!vmap_view_get_canvas(track->view, &canvas)) {
        return false;
    }

    if (track->full_repaint || track->count < track->last_paint_count
            || track_view_paint_changed(track)) {
        vmap_track_paint_to_canvas(track, &canvas);
        return true;
    }

    if (track->count == track->last_paint_count) {
        if (track->km_next != track->last_km_next) {
            track_paint_km_markers(track, &canvas, track->last_km_next);
            track_paint_km_marker_labels(track, track->last_km_next);
            return true;
        }
        return false;
    }

    track_paint_tail(track, &canvas);
    track->draw_n = track->count;

    /* 新尾巴可能盖住圆标；整圈重描会把 gpx_timer 打到 100ms。rebase 时再全量盖。 */
    if (track->km_next != track->last_km_next) {
        track_paint_km_markers(track, &canvas, track->last_km_next);
        track_paint_km_marker_labels(track, track->last_km_next);
    }

    track->last_paint_count = track->count;
    return true;
}

static void track_invalidate_canvas(const vmap_track_t * track)
{
    lv_obj_t * canvas;

    if (!track || !track->view) {
        return;
    }

    canvas = vmap_view_get_obj(track->view);
    if (canvas) {
        lv_obj_invalidate(canvas);
    }
}

static void track_invalidate_dirty(const vmap_track_t * track,
    const lv_area_t * dirty, bool valid)
{
    if (!track || !track->view || !dirty || !valid) {
        track_invalidate_canvas(track);
        return;
    }

    vmap_view_invalidate_canvas_area(track->view,
        dirty->x1, dirty->y1, dirty->x2, dirty->y2);
}

static void track_km_reset(vmap_track_t * track)
{
    track->km_count = 0;
    track->km_next = 1;
    track->km_lap_base = 1;
}

static void track_km_clear_display(vmap_track_t * track)
{
    if (track) {
        track->km_lap_base = track->km_next;
        track->km_count = 0;
    }
}

static void track_begin_new_lap(vmap_track_t * track, float lon, float lat,
    float speed_kph)
{
    bicycle_runtime_on_lap_closed();
    track->lap_start_lon = lon;
    track->lap_start_lat = lat;
    track_lap_ref_reset(track);

    /*
     * Display-only reset: erase previous lap polyline + km badges from the map.
     * session_distance_m / km_next stay cumulative (bottom panel, ring slot numbers).
     */
    track->count = 0;
    track->draw_n = 0;
    track_km_clear_display(track);
    track->last_count = 0;
    track->last_km_count = 0;
    track->last_km_next = track->km_next;
    track->last_paint_count = 0;
    track->full_repaint = true;

    track->pts[0].lon = lon;
    track->pts[0].lat = lat;
    track->pts[0].speed_kph = speed_kph;
    track->count = 1;

    if (track->lap_cb) {
        track->lap_cb(track, bicycle_runtime_get()->lap_count, track->lap_cb_ud);
    }

    bicycle_runtime_set_track_metrics(track->count, track->km_count);

    if (track->view) {
        vmap_view_redraw_keep_pan(track->view);
    }
}

static bool track_check_lap_closure(vmap_track_t * track, float lon, float lat,
    float speed_kph)
{
    double d;

    const vmap_track_lap_config_t * lap_cfg = track_lap_cfg();

    if (track->count < 3
            || bicycle_runtime_lap_since_start_m() < lap_cfg->min_distance_m) {
        return false;
    }

    d = track_dist_m(lat, lon, track->lap_start_lat, track->lap_start_lon);
    if (d > lap_cfg->close_radius_m) {
        return false;
    }

    if (!track_lap_direction_ok(track, lon, lat)) {
        return false;
    }

    track_begin_new_lap(track, lon, lat, speed_kph);
    return true;
}

static void track_km_collect_segment(vmap_track_t * track,
    const track_pt_t * a, const track_pt_t * b)
{
    const double interval = bicycle_config()->km_marker_interval_m;
    const uint16_t km_cap = bicycle_config_km_marker_max();
    const double seg_m = track_dist_m(a->lat, a->lon, b->lat, b->lon);
    const double seg_end_m = bicycle_runtime_get()->session_distance_m;
    const double seg_start_m = seg_end_m - seg_m;

    if (seg_m < 1e-3 || interval <= 0.0 || km_cap == 0) {
        return;
    }

    while ((double)track->km_next * interval <= seg_end_m + 1e-3) {
        const double target_m = (double)track->km_next * interval;
        const double along = target_m - seg_start_m;
        const double t = along / seg_m;
        const uint16_t idx = track_km_slot_index(track->km_next, km_cap);
        km_marker_t * m = &track->km[idx];

        m->km = track->km_next;
        m->lon = a->lon + (b->lon - a->lon) * (float)t;
        m->lat = a->lat + (b->lat - a->lat) * (float)t;
        track->km_next++;
    }

    track->km_count = (uint16_t)(track->km_next - track->km_lap_base);
}

static uint16_t track_compute_point_cap(size_t draw_bytes)
{
    const size_t total = vmap_track_arena_total_bytes();
    const uint16_t cfg_cap = bicycle_config_track_point_cap();
    size_t avail;
    uint16_t arena_cap;

    if (total <= draw_bytes + sizeof(track_pt_t)) {
        return 0;
    }

    avail = total - draw_bytes;
    if (avail / sizeof(track_pt_t) > 65535u) {
        arena_cap = 65535;
    } else {
        arena_cap = (uint16_t)(avail / sizeof(track_pt_t));
    }

    return arena_cap < cfg_cap ? arena_cap : cfg_cap;
}

/**
 * @brief vmap track create on view。
 * @return 0 成功，负 errno 失败。
 */
vmap_track_t * vmap_track_create_on_view(vmap_view_t * view)
{
    vmap_track_t * track = (vmap_track_t *)calloc(1, sizeof(*track));

    if (!track || !view) {
        free(track);
        return NULL;
    }

    if (vmap_track_arena_init() != 0) {
        free(track);
        return NULL;
    }

    vmap_track_arena_reset_alloc();

    track->view = view;
    {
        const uint16_t draw_cap = bicycle_config_track_draw_cap();
        const size_t draw_bytes =
            (size_t)draw_cap * sizeof(track_draw_pt_t);
        track->cap = track_compute_point_cap(draw_bytes);
        track->draw = (track_draw_pt_t *)vmap_track_arena_alloc(draw_bytes);
        track->pts = (track_pt_t *)vmap_track_arena_alloc(
            (size_t)track->cap * sizeof(track->pts[0]));
    }
    if (track->cap < 2 || !track->pts || !track->draw) {
        vmap_track_destroy(track);
        return NULL;
    }

    vmap_view_attach_track(view, track);
    track->km_next = 1;
    track->km_lap_base = 1;
    track->last_km_next = 1;
    return track;
}

/**
 * @brief vmap track destroy。
 */
void vmap_track_destroy(vmap_track_t * track)
{
    if (!track) {
        return;
    }

    if (track->view) {
        vmap_view_attach_track(track->view, NULL);
    }

    vmap_track_arena_reset_alloc();
    free(track);
}

/**
 * @brief vmap track bind view。
 */
void vmap_track_bind_view(vmap_track_t * track, vmap_view_t * view)
{
    if (track) {
        if (track->view) {
            vmap_view_attach_track(track->view, NULL);
        }
        track->view = view;
        if (view) {
            vmap_view_attach_track(view, track);
        }
        track->last_render_rev = 0;
        track->last_paint_count = 0;
        track->full_repaint = true;
    }
}

/**
 * @brief vmap track set font。
 */
void vmap_track_set_font(vmap_track_t * track, const lv_font_t * font)
{
    if (track) {
        track->font = font;
    }
}

/**
 * @brief vmap track set lap cb。
 */
void vmap_track_set_lap_cb(vmap_track_t * track, vmap_track_lap_cb_t cb,
    void * user_data)
{
    if (track) {
        track->lap_cb = cb;
        track->lap_cb_ud = user_data;
    }
}

/**
 * @brief vmap track set recording。
 */
void vmap_track_set_recording(vmap_track_t * track, bool enable)
{
    if (!track) {
        return;
    }
    if (enable && !track->recording) {
        vmap_track_clear(track);
    }
    track->recording = enable;
    bicycle_runtime_set_recording(enable);
}

void vmap_track_pause(vmap_track_t * track)
{
    if (!track) {
        return;
    }

    track->recording = false;
    bicycle_runtime_set_recording(false);
}

void vmap_track_resume(vmap_track_t * track)
{
    if (!track) {
        return;
    }

    track->recording = true;
    bicycle_runtime_set_recording(true);
}

/**
 * @brief vmap track is recording。
 * @return 请求的值。
 */
bool vmap_track_is_recording(const vmap_track_t * track)
{
    return track && track->recording;
}

/**
 * @brief vmap track clear。
 */
void vmap_track_clear(vmap_track_t * track)
{
    bool had_ink;

    if (!track) {
        return;
    }

    /* Track is composited into the map canvas. Re-stamp tiles in place so the
     * polyline disappears; do not vmap_view_render() — that zeros follow pan
     * and flashes the canvas (start/stop REC). */
    had_ink = track->count > 0 || track->draw_n > 0
        || track->last_paint_count > 0 || track->km_count > 0;

    track->count = 0;
    track->last_count = 0;
    track->draw_n = 0;
    track_km_reset(track);
    track->last_km_count = 0;
    track->last_km_next = track->km_next;
    track->lap_start_lon = 0.0f;
    track->lap_start_lat = 0.0f;
    track_lap_ref_reset(track);
    bicycle_runtime_reset_session();
    track->last_paint_count = 0;
    track->full_repaint = true;
    if (had_ink && track->view) {
        vmap_view_redraw_keep_pan(track->view);
    }
}

/**
 * @brief vmap track point count。
 * @return 请求的值。
 */
uint16_t vmap_track_point_count(const vmap_track_t * track)
{
    return track ? track->count : 0;
}

/**
 * @brief vmap track point capacity。
 */
uint16_t vmap_track_point_capacity(const vmap_track_t * track)
{
    return track ? track->cap : 0;
}

/**
 * @brief vmap track lap count。
 * @return 请求的值。
 */
uint16_t vmap_track_lap_count(const vmap_track_t * track)
{
    (void)track;
    return bicycle_runtime_get()->lap_count;
}

/**
 * @brief vmap track session distance m。
 */
double vmap_track_session_distance_m(const vmap_track_t * track)
{
    (void)track;
    return bicycle_runtime_get()->session_distance_m;
}

/**
 * @brief vmap track lap distance m。
 */
double vmap_track_lap_distance_m(const vmap_track_t * track)
{
    (void)track;
    return bicycle_runtime_lap_since_start_m();
}

/**
 * @brief vmap track draw point count。
 * @return 请求的值。
 */
uint16_t vmap_track_draw_point_count(const vmap_track_t * track)
{
    return track ? track->draw_n : 0;
}

/**
 * @brief vmap track km marker count。
 * @return 请求的值。
 */
uint16_t vmap_track_km_marker_count(const vmap_track_t * track)
{
    return track ? track->km_count : 0;
}

/**
 * @brief vmap track push。
 */
bool vmap_track_push(vmap_track_t * track, double lon, double lat, float speed_kph)
{
    double seg_m = 0.0;
    bool first_pt = false;

    if (!track || !track->recording) {
        return false;
    }

    if (track->count > 0) {
        const track_pt_t * last = &track->pts[track->count - 1];
        seg_m = track_dist_m(last->lat, last->lon, (float)lat, (float)lon);
        if (seg_m < bicycle_config_track_point_min_dist_m()) {
            return false;
        }
    } else {
        first_pt = true;
    }

    if (track->count >= track->cap) {
        memmove(track->pts, track->pts + 1,
            (size_t)(track->cap - 1) * sizeof(track->pts[0]));
        track->count = (uint16_t)(track->cap - 1);
        if (track->last_paint_count > 0u) {
            track->last_paint_count--;
        }
        if (track->last_count > 0u) {
            track->last_count--;
        }
    }

    track->pts[track->count].lon = (float)lon;
    track->pts[track->count].lat = (float)lat;
    track->pts[track->count].speed_kph = speed_kph;
    track->count++;

    if (first_pt) {
        track->lap_start_lon = (float)lon;
        track->lap_start_lat = (float)lat;
        track_lap_ref_reset(track);
    } else {
        const track_pt_t * last = &track->pts[track->count - 2];
        const float motion_deg = track_bearing_deg(last->lat, last->lon,
            (float)lat, (float)lon);

        bicycle_runtime_add_segment(seg_m, motion_deg, speed_kph);
        track_try_lock_lap_ref_course(track, (float)lon, (float)lat);

        /*
         * Lap closure before km collect: closing segment must not leave stale
         * badges that get cleared anyway; km_next stays for the next lap display.
         */
        if (track_check_lap_closure(track, (float)lon, (float)lat, speed_kph)) {
            return true;
        }
    }

    if (track->count >= 2) {
        track_km_collect_segment(track,
            &track->pts[track->count - 2], &track->pts[track->count - 1]);
    }

    bicycle_runtime_set_track_metrics(track->count, track->km_count);
    return true;
}

/**
 * @brief 隐藏期间积压的轨迹是否必须随瓦片重贴。
 */
bool vmap_track_needs_restamp(const vmap_track_t * track)
{
    uint16_t unpainted;

    if (!track) {
        return false;
    }
    if (track->full_repaint || track->count < track->last_paint_count) {
        return true;
    }
    if (track->count < 2u) {
        return false;
    }
    unpainted = (uint16_t)(track->count - track->last_paint_count);
    return unpainted > 2u;
}

void vmap_track_refresh(vmap_track_t * track)
{
    uint32_t render_rev;
    bool km_dirty;
    bool painted;
    bool full_paint;
    bool dirty_valid = false;
    lv_area_t dirty;

    if (!track || !track->view) {
        return;
    }

    /* 瓦片泵进行中画布原点在变。这时画尾巴会把 last_paint_count 提前，
     * 结束帧再 rebase 时留下错位 REC 线。 */
    if (vmap_view_render_busy(track->view)) {
        return;
    }

    render_rev = vmap_view_render_revision(track->view);
    km_dirty = track->km_next != track->last_km_next;

    if (track->count < 2 && track->km_next <= track->km_lap_base) {
        track->draw_n = 0;
        track->last_paint_count = 0;
        return;
    }

    if (track->count == track->last_count && render_rev == track->last_render_rev
            && !km_dirty && !track->full_repaint) {
        bicycle_c_debug_track_refresh(false);
        return;
    }

    if (render_rev != track->last_render_rev) {
        track->last_render_rev = render_rev;
        track_sync_paint_view_state(track);
        if (track->count == track->last_paint_count
            && !km_dirty && !track->full_repaint) {
            track->last_count = track->count;
            track->last_km_count = track->km_count;
            track->last_km_next = track->km_next;
            bicycle_c_debug_track_refresh(
                track->draw_n >= 2 || track->km_count > 0);
            bicycle_c_debug_track_state(track->draw_n, track->km_count,
                bicycle_runtime_get()->lap_distance_m);
            return;
        }
    }

    full_paint = track->full_repaint
        || track->count < track->last_paint_count
        || track_view_paint_changed(track);
    if (!full_paint) {
        if (track->count > track->last_paint_count) {
            if ((uint16_t)(track->count - track->last_paint_count) <= 2u) {
                track_tail_dirty(track, &dirty, &dirty_valid);
            }
        }
        if (km_dirty) {
            track_km_dirty(track, track->last_km_next, &dirty, &dirty_valid);
        }
    }

    painted = track_paint_incremental(track);
    if (painted || km_dirty) {
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
        bicycle_inval_probe_tag("track_refresh");
#endif
        if (full_paint) {
            track_invalidate_canvas(track);
        } else {
            track_invalidate_dirty(track, &dirty, dirty_valid);
        }
        bicycle_c_debug_track_refresh(true);
    } else {
        bicycle_c_debug_track_refresh(false);
    }

    track->last_count = track->count;
    track->last_km_count = track->km_count;
    track->last_km_next = track->km_next;
    track->last_render_rev = render_rev;
    bicycle_c_debug_track_state(track->draw_n, track->km_count,
        bicycle_runtime_get()->lap_distance_m);
}
