/**
 * @file vmap_label.c
 * @brief 道路/地名标签：收集后盖进地图 RGB565 画布（导航线之上、REC 之下）。
 */

#include "vmap_label.h"

#include "vmap_alloc.h"
#include "vmap_format.h"
#include "vmap_style.h"
#include <limits.h>
#include <math.h>
#include <string.h>

#define VMAP_PAN_GUARD              12
#define VMAP_ROAD_LABEL_VIS_PAD     24
#define VMAP_ROAD_LABEL_SPACING_MIN_X 80
#define VMAP_ROAD_LABEL_SPACING_MIN_Y 64
#define VMAP_ROAD_LABEL_MAX_PER_WAY 8
#define VMAP_LABEL_PAD              1

typedef struct {
    int32_t road_idx[VMAP_LABEL_CAP];
    int32_t oth_idx[VMAP_LABEL_CAP];
    char layout_buf[VMAP_LABEL_MAX_DRAWN][64];
    lv_area_t placed[VMAP_LABEL_RETAIN_CAP + VMAP_LABEL_MAX_DRAWN];
    int32_t final_idx[VMAP_LABEL_MAX_DRAWN];
} vmap_label_scratch_t;

static vmap_label_scratch_t *s_label_scratch;

static vmap_label_scratch_t *label_scratch(void)
{
    if (s_label_scratch == NULL) {
        s_label_scratch = vmap_malloc(sizeof(*s_label_scratch));
    }

    return s_label_scratch;
}

static int32_t label_dist2(int32_t x, int32_t y, int32_t fx, int32_t fy)
{
    int32_t dx = x - fx;
    int32_t dy = y - fy;
    return dx * dx + dy * dy;
}

static int32_t label_dist2_norm(int32_t x, int32_t y, int32_t fx, int32_t fy,
    double scale)
{
    int64_t d2 = label_dist2(x, y, fx, fy);
    double s2 = scale * scale;
    if (s2 < 0.0625) {
        s2 = 0.0625;
    }
    return (int32_t)((double)d2 / s2);
}

static int32_t other_label_score(uint8_t kind, uint8_t prio,
    int32_t x, int32_t y, int32_t fx, int32_t fy, double scale)
{
    int32_t score = (int32_t)prio;
    if (kind == VMAP_LABEL_PLACE) {
        score += 20;
    } else if (kind == VMAP_LABEL_POI) {
        score += 5;
    } else if (kind == VMAP_LABEL_WATER) {
        score -= 50;
    }
    score -= label_dist2_norm(x, y, fx, fy, scale) / 2000;
    return score;
}

static bool boxes_overlap(const lv_area_t * a, const lv_area_t * b,
    int32_t gap_x, int32_t gap_y)
{
    return a->x1 - gap_x <= b->x2 && a->x2 + gap_x >= b->x1
        && a->y1 - gap_y <= b->y2 && a->y2 + gap_y >= b->y1;
}

static void label_pan_canvas_bounds(int32_t w, int32_t h,
    int32_t * x0, int32_t * y0, int32_t * x1, int32_t * y1)
{
    *x0 = VMAP_PAN_GUARD;
    *y0 = VMAP_PAN_GUARD;
    *x1 = w - VMAP_PAN_GUARD;
    *y1 = h - VMAP_PAN_GUARD;
}

static bool point_in_rect(int32_t sx, int32_t sy,
    int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1;
}

static void road_label_spacing_from_viewport(int32_t view_w, int32_t view_h,
    double * spacing_x, double * spacing_y)
{
    /*
     * Horizontal names have wide boxes and lose more anchors to canvas bounds
     * and collisions. Sample them more densely than the old 0.55× viewport.
     */
    *spacing_x = (double)view_w * 0.42;
    *spacing_y = (double)view_h * 0.42;
    if (*spacing_x < (double)VMAP_ROAD_LABEL_SPACING_MIN_X) {
        *spacing_x = (double)VMAP_ROAD_LABEL_SPACING_MIN_X;
    }
    if (*spacing_y < (double)VMAP_ROAD_LABEL_SPACING_MIN_Y) {
        *spacing_y = (double)VMAP_ROAD_LABEL_SPACING_MIN_Y;
    }
}

static double road_label_arc_spacing(double dx, double dy, double seg_len,
    double spacing_x, double spacing_y)
{
    if (seg_len < 1.0) {
        return spacing_x;
    }
    double nx = fabs(dx) / seg_len;
    double ny = fabs(dy) / seg_len;
    return nx * spacing_x + ny * spacing_y;
}

static void closest_point_on_segment(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    int32_t px, int32_t py, int32_t * out_x, int32_t * out_y)
{
    double dx = (double)(x1 - x0);
    double dy = (double)(y1 - y0);
    double len2 = dx * dx + dy * dy;
    if (len2 < 1.0) {
        *out_x = x0;
        *out_y = y0;
        return;
    }

    double t = ((double)(px - x0) * dx + (double)(py - y0) * dy) / len2;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }

    *out_x = (int32_t)lround(x0 + dx * t);
    *out_y = (int32_t)lround(y0 + dy * t);
}

static int64_t road_canvas_sort_key(int32_t x, int32_t y, int32_t fx, int32_t fy)
{
    return -(int64_t)label_dist2(x, y, fx, fy);
}

static uint32_t utf8_char_bytes(const char * s)
{
    const uint8_t * p = (const uint8_t *)s;
    if (!p || *p == 0) {
        return 0;
    }
    if (*p < 0x80) {
        return 1;
    }
    if ((*p & 0xE0) == 0xC0) {
        return 2;
    }
    if ((*p & 0xF0) == 0xE0) {
        return 3;
    }
    if ((*p & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static void road_label_layout_text(uint8_t kind, uint8_t road_vertical,
    const char * name, char * out, size_t cap)
{
    size_t w = 0;
    const char * p;

    if (!out || cap == 0 || !name) {
        return;
    }

    if (kind != VMAP_LABEL_ROAD || !road_vertical) {
        lv_strlcpy(out, name, cap);
        return;
    }

    p = name;
    while (*p != '\0') {
        uint32_t n = utf8_char_bytes(p);
        if (n == 0) {
            break;
        }
        if (w > 0) {
            if (w + 1 >= cap) {
                break;
            }
            out[w++] = '\n';
        }
        if (w + n >= cap) {
            break;
        }
        lv_memcpy(out + w, p, n);
        w += n;
        p += n;
    }
    out[w] = '\0';
}

static lv_coord_t road_label_single_char_width(const char * utf8, const lv_font_t * font)
{
    char one[8];
    uint32_t n = utf8_char_bytes(utf8);
    lv_point_t sz;

    if (n == 0) {
        return lv_font_get_line_height(font);
    }
    if (n >= sizeof(one)) {
        n = (uint32_t)sizeof(one) - 1U;
    }
    lv_memcpy(one, utf8, n);
    one[n] = '\0';

    lv_text_get_size(&sz, one, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > 0) {
        return sz.x;
    }
    return lv_font_get_line_height(font);
}

static void road_label_measure(uint8_t kind, uint8_t road_vertical, const char * name,
    const lv_font_t * font, char * layout_buf, size_t layout_cap, lv_point_t * sz)
{
    road_label_layout_text(kind, road_vertical, name, layout_buf, layout_cap);
    if (kind == VMAP_LABEL_ROAD && road_vertical) {
        lv_coord_t char_w = road_label_single_char_width(layout_buf, font);
        lv_text_get_size(sz, layout_buf, font, 0, 0, char_w, LV_TEXT_FLAG_NONE);
    } else {
        lv_text_get_size(sz, layout_buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
}

static uint8_t road_prio_from_attr(uint8_t attr)
{
    static const uint8_t k_prio[] = {
        0, 130, 110, 95, 80, 65, 55, 45,
    };
    if (attr >= (uint8_t)(sizeof(k_prio) / sizeof(k_prio[0]))) {
        return 80;
    }
    return k_prio[attr];
}

static bool segment_bbox_hits_vis(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    int32_t vis_x0, int32_t vis_y0, int32_t vis_x1, int32_t vis_y1)
{
    int32_t smin_x = x0 < x1 ? x0 : x1;
    int32_t smax_x = x0 > x1 ? x0 : x1;
    int32_t smin_y = y0 < y1 ? y0 : y1;
    int32_t smax_y = y0 > y1 ? y0 : y1;
    return smax_x >= vis_x0 && smin_x <= vis_x1 && smax_y >= vis_y0 && smin_y <= vis_y1;
}

static void copy_label_name(vmap_label_cand_t * c, const char * text, uint8_t len)
{
    uint8_t n = len < (uint8_t)(sizeof(c->name) - 1) ? len
                                                  : (uint8_t)(sizeof(c->name) - 1);
    if (n > 0 && text) {
        lv_memcpy(c->name, text, n);
    }
    c->name[n] = '\0';
}

static bool label_in_clip(const vmap_label_collector_t * col, int32_t x, int32_t y)
{
    uint8_t i;

    if (!col || col->clip_n == 0) {
        return true;
    }
    for (i = 0; i < col->clip_n; i++) {
        if (x >= col->clip_x1[i] && x < col->clip_x2[i]
            && y >= col->clip_y1[i] && y < col->clip_y2[i]) {
            return true;
        }
    }
    return false;
}

static bool push_label(vmap_label_collector_t * col, int32_t sx, int32_t sy,
    uint8_t kind, uint8_t prio, uint8_t road_vertical,
    const char * text, uint8_t len)
{
    if (!col || col->count >= col->cap || !text || len == 0) {
        return false;
    }
    if (!label_in_clip(col, sx, sy)) {
        return false;
    }

    vmap_label_cand_t * c = &col->items[col->count];
    c->x = sx;
    c->y = sy;
    c->kind = kind;
    c->prio = prio;
    c->road_vertical = road_vertical;
    copy_label_name(c, text, len);
    col->count++;
    return true;
}

static void collect_point_labels(vmap_label_collector_t * col,
    const vmap_tile_t * tile, float base_x, float base_y, float scale)
{
    int32_t vis_x0, vis_y0, vis_x1, vis_y1;
    float u = 256.0f * scale / (float)vmap_tile_extent(tile);
    vmap_label_iter_t it = vmap_tile_label_iter(tile);
    vmap_label_t lb;

    label_pan_canvas_bounds(col->canvas_w, col->canvas_h,
        &vis_x0, &vis_y0, &vis_x1, &vis_y1);

    while (vmap_label_iter_next(&it, &lb)) {
        int32_t sx;
        int32_t sy;

        if (col->count >= col->cap || lb.kind == VMAP_LABEL_ROAD) {
            continue;
        }
        sx = (int32_t)(base_x + lb.x * u);
        sy = (int32_t)(base_y + lb.y * u);
        if (sx < vis_x0 || sx > vis_x1 || sy < vis_y0 || sy > vis_y1) {
            continue;
        }
        push_label(col, sx, sy, lb.kind, lb.priority, 0, lb.text, lb.len);
    }
}

typedef struct {
    vmap_label_collector_t * col;
    int32_t placed;
    double dist_since_label;
    int32_t near_x;
    int32_t near_y;
    int32_t near_d2;
    int32_t near_seg_dx;
    int32_t near_seg_dy;
    bool any_visible;
    int32_t vis_min_x;
    int32_t vis_min_y;
    int32_t vis_max_x;
    int32_t vis_max_y;
    int32_t pan_x0;
    int32_t pan_y0;
    int32_t pan_x1;
    int32_t pan_y1;
    int32_t focus_x;
    int32_t focus_y;
    const char * name_text;
    uint8_t name_len;
    uint8_t road_attr;
} road_collect_ctx_t;

static int32_t road_name_near_index(const vmap_label_collector_t * col,
    const char * name, uint8_t len, int32_t x, int32_t y)
{
    int32_t i;
    int32_t lim;
    int32_t lim2;
    double spacing_x;
    double spacing_y;

    if (!col || !name || len == 0) {
        return -1;
    }

    /*
     * Keep repeats dense enough that a viewport crossing a long road sees a
     * name, while still merging the duplicate anchors emitted by two tiles at
     * their seam.
     */
    road_label_spacing_from_viewport(col->view_w, col->view_h,
        &spacing_x, &spacing_y);
    lim = (int32_t)lround((spacing_x + spacing_y) * 0.5);
    if (lim < 60) {
        lim = 60;
    }
    lim2 = lim * lim;

    for (i = 0; i < col->count; i++) {
        int32_t dx;
        int32_t dy;

        if (col->items[i].kind != VMAP_LABEL_ROAD) {
            continue;
        }
        if (strncmp(col->items[i].name, name, (size_t)len) != 0
            || col->items[i].name[len] != '\0') {
            continue;
        }

        dx = col->items[i].x - x;
        dy = col->items[i].y - y;
        if (dx * dx + dy * dy < lim2) {
            return i;
        }
    }
    return -1;
}

static bool road_push_label(road_collect_ctx_t * ctx, int32_t sx, int32_t sy,
    int32_t seg_dx, int32_t seg_dy)
{
    int32_t near_idx;
    uint8_t vertical;

    if (ctx->placed >= VMAP_ROAD_LABEL_MAX_PER_WAY) {
        return false;
    }
    if (!point_in_rect(sx, sy, ctx->pan_x0, ctx->pan_y0, ctx->pan_x1, ctx->pan_y1)) {
        return false;
    }
    if (!label_in_clip(ctx->col, sx, sy)) {
        return false;
    }

    vertical = ((seg_dy < 0 ? -seg_dy : seg_dy)
        > (seg_dx < 0 ? -seg_dx : seg_dx)) ? 1u : 0u;
    near_idx = road_name_near_index(ctx->col, ctx->name_text,
        ctx->name_len, sx, sy);
    if (near_idx >= 0) {
        vmap_label_cand_t * old = &ctx->col->items[near_idx];

        /*
         * Tile order must not decide the winner. Keep the same-name anchor
         * nearest the viewport focus, so a seam candidate cannot suppress the
         * road name in the visible center.
         */
        if (label_dist2(sx, sy, ctx->focus_x, ctx->focus_y)
            < label_dist2(old->x, old->y, ctx->focus_x, ctx->focus_y)) {
            old->x = sx;
            old->y = sy;
            old->prio = road_prio_from_attr(ctx->road_attr);
            old->road_vertical = vertical;
        }
        return false;
    }
    if (ctx->col->count >= ctx->col->cap) {
        return false;
    }

    {
        vmap_label_cand_t * c = &ctx->col->items[ctx->col->count];
        c->x = sx;
        c->y = sy;
        c->kind = VMAP_LABEL_ROAD;
        c->prio = road_prio_from_attr(ctx->road_attr);
        c->road_vertical = vertical;
        copy_label_name(c, ctx->name_text, ctx->name_len);
        ctx->col->count++;
    }
    ctx->placed++;
    ctx->dist_since_label = 0.0;
    return true;
}

static void collect_road_labels(vmap_label_collector_t * col,
    const vmap_tile_t * tile, float base_x, float base_y, float scale)
{
    float u = 256.0f * scale / (float)vmap_tile_extent(tile);
    int32_t pan_x0, pan_y0, pan_x1, pan_y1;
    int32_t vis_x0, vis_y0, vis_x1, vis_y1;
    double spacing_x = 0.0;
    double spacing_y = 0.0;
    double spacing_avg;
    vmap_feature_iter_t it;
    vmap_feature_t f;
    road_collect_ctx_t ctx;

    label_pan_canvas_bounds(col->canvas_w, col->canvas_h,
        &pan_x0, &pan_y0, &pan_x1, &pan_y1);
    vis_x0 = pan_x0 - VMAP_ROAD_LABEL_VIS_PAD;
    vis_y0 = pan_y0 - VMAP_ROAD_LABEL_VIS_PAD;
    vis_x1 = pan_x1 + VMAP_ROAD_LABEL_VIS_PAD;
    vis_y1 = pan_y1 + VMAP_ROAD_LABEL_VIS_PAD;
    road_label_spacing_from_viewport(col->view_w, col->view_h, &spacing_x, &spacing_y);
    spacing_avg = (spacing_x + spacing_y) * 0.5;

    it = vmap_tile_layer_iter(tile, VMAP_LAYER_ROAD);
    while (vmap_feature_iter_next(&it, &f)) {
        uint16_t i;

        if (col->count >= col->cap || !vmap_feature_has_name(&f) || f.point_count < 2) {
            continue;
        }

        memset(&ctx, 0, sizeof(ctx));
        ctx.col = col;
        ctx.pan_x0 = pan_x0;
        ctx.pan_y0 = pan_y0;
        ctx.pan_x1 = pan_x1;
        ctx.pan_y1 = pan_y1;
        ctx.focus_x = col->focus_x;
        ctx.focus_y = col->focus_y;
        ctx.name_text = f.name_text;
        ctx.name_len = f.name_len;
        ctx.road_attr = f.attr;
        ctx.dist_since_label = spacing_avg;
        ctx.near_d2 = INT32_MAX;
        ctx.vis_min_x = INT32_MAX;
        ctx.vis_min_y = INT32_MAX;
        ctx.vis_max_x = INT32_MIN;
        ctx.vis_max_y = INT32_MIN;

        for (i = 0; i + 1 < f.point_count; i++) {
            int32_t x0 = (int32_t)(base_x + vmap_feature_x(&f, i) * u);
            int32_t y0 = (int32_t)(base_y + vmap_feature_y(&f, i) * u);
            int32_t x1 = (int32_t)(base_x + vmap_feature_x(&f, i + 1) * u);
            int32_t y1 = (int32_t)(base_y + vmap_feature_y(&f, i + 1) * u);
            double dx;
            double dy;
            double seg_len;
            double arc_spacing;
            double pos;

            if (!segment_bbox_hits_vis(x0, y0, x1, y1, vis_x0, vis_y0, vis_x1, vis_y1)) {
                continue;
            }

            ctx.any_visible = true;
            {
                int32_t smin_x = x0 < x1 ? x0 : x1;
                int32_t smax_x = x0 > x1 ? x0 : x1;
                int32_t smin_y = y0 < y1 ? y0 : y1;
                int32_t smax_y = y0 > y1 ? y0 : y1;
                if (smin_x < ctx.vis_min_x) {
                    ctx.vis_min_x = smin_x;
                }
                if (smin_y < ctx.vis_min_y) {
                    ctx.vis_min_y = smin_y;
                }
                if (smax_x > ctx.vis_max_x) {
                    ctx.vis_max_x = smax_x;
                }
                if (smax_y > ctx.vis_max_y) {
                    ctx.vis_max_y = smax_y;
                }
            }

            dx = (double)(x1 - x0);
            dy = (double)(y1 - y0);
            seg_len = sqrt(dx * dx + dy * dy);
            if (seg_len < 1.0) {
                continue;
            }

            {
                int32_t cx = 0;
                int32_t cy = 0;
                int32_t cd2;
                closest_point_on_segment(x0, y0, x1, y1, ctx.focus_x, ctx.focus_y,
                    &cx, &cy);
                cd2 = label_dist2(cx, cy, ctx.focus_x, ctx.focus_y);
                if (cd2 < ctx.near_d2) {
                    ctx.near_d2 = cd2;
                    ctx.near_x = cx;
                    ctx.near_y = cy;
                    ctx.near_seg_dx = (int32_t)dx;
                    ctx.near_seg_dy = (int32_t)dy;
                }
            }

            arc_spacing = road_label_arc_spacing(dx, dy, seg_len, spacing_x, spacing_y);
            pos = 0.0;
            while (pos < seg_len && ctx.placed < VMAP_ROAD_LABEL_MAX_PER_WAY) {
                double need = arc_spacing - ctx.dist_since_label;
                if (need <= 0.0) {
                    need = arc_spacing;
                }
                if (pos + need > seg_len) {
                    ctx.dist_since_label += seg_len - pos;
                    break;
                }
                pos += need;
                {
                    int32_t sx = (int32_t)lround(x0 + dx * (pos / seg_len));
                    int32_t sy = (int32_t)lround(y0 + dy * (pos / seg_len));
                    road_push_label(&ctx, sx, sy, (int32_t)dx, (int32_t)dy);
                }
            }
        }

        if (!ctx.any_visible) {
            continue;
        }

        if (ctx.near_d2 < INT32_MAX) {
            bool dup_near = false;
            int32_t base_idx = col->count - ctx.placed;
            int32_t k;
            for (k = base_idx; k < col->count; k++) {
                if (label_dist2(col->items[k].x, col->items[k].y,
                        ctx.near_x, ctx.near_y) < 400) {
                    dup_near = true;
                    break;
                }
            }
            if (!dup_near) {
                road_push_label(&ctx, ctx.near_x, ctx.near_y,
                    ctx.near_seg_dx, ctx.near_seg_dy);
            }
        }

        if (ctx.placed == 0) {
            road_push_label(&ctx,
                (ctx.vis_min_x + ctx.vis_max_x) / 2,
                (ctx.vis_min_y + ctx.vis_max_y) / 2,
                ctx.vis_max_x - ctx.vis_min_x,
                ctx.vis_max_y - ctx.vis_min_y);
        }
    }
}

/**
 * @brief 初始化标签收集器。
 * @param col 收集器。
 * @return 0 成功，负 errno 失败。
 */
void vmap_label_collector_init(vmap_label_collector_t * col,
    vmap_label_cand_t * buf, int32_t cap,
    vmap_label_retained_t * retained, int32_t retained_cap,
    int32_t canvas_w, int32_t canvas_h, int32_t view_w, int32_t view_h,
    int32_t margin)
{
    if (!col) {
        return;
    }
    memset(col, 0, sizeof(*col));
    col->items = buf;
    col->cap = cap;
    col->retained = retained;
    col->retained_cap = retained_cap;
    col->canvas_w = canvas_w;
    col->canvas_h = canvas_h;
    col->view_w = view_w;
    col->view_h = view_h;
    col->margin = margin;
    col->focus_x = canvas_w / 2;
    col->focus_y = canvas_h / 2;
}

/**
 * @brief 清空已收集标签。
 */
void vmap_label_collector_reset(vmap_label_collector_t * col)
{
    if (col) {
        col->count = 0;
        col->clip_n = 0;
    }
}

void vmap_label_retained_reset(vmap_label_collector_t * col)
{
    if (col) {
        col->retained_count = 0;
    }
}

static bool dirty_covers_inclusive(uint8_t n,
    const int32_t * x1, const int32_t * y1,
    const int32_t * x2, const int32_t * y2,
    int32_t bx1, int32_t by1, int32_t bx2, int32_t by2)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        if (x1[i] <= bx1 && y1[i] <= by1
            && x2[i] > bx2 && y2[i] > by2) {
            return true;
        }
    }
    return false;
}

static void dirty_include_inclusive(uint8_t cap, uint8_t * n,
    int32_t * x1, int32_t * y1, int32_t * x2, int32_t * y2,
    int32_t canvas_w, int32_t canvas_h,
    int32_t bx1, int32_t by1, int32_t bx2, int32_t by2)
{
    int32_t dx1 = bx1;
    int32_t dy1 = by1;
    int32_t dx2 = bx2 + 1;
    int32_t dy2 = by2 + 1;
    uint8_t i;
    uint8_t best;
    int32_t best_grow;

    if (!n || !x1 || !y1 || !x2 || !y2 || cap == 0) {
        return;
    }
    if (dx1 < 0) {
        dx1 = 0;
    }
    if (dy1 < 0) {
        dy1 = 0;
    }
    if (dx2 > canvas_w) {
        dx2 = canvas_w;
    }
    if (dy2 > canvas_h) {
        dy2 = canvas_h;
    }
    if (dx1 >= dx2 || dy1 >= dy2) {
        return;
    }
    if (dirty_covers_inclusive(*n, x1, y1, x2, y2, bx1, by1, bx2, by2)) {
        return;
    }
    if (*n < cap) {
        i = *n;
        x1[i] = dx1;
        y1[i] = dy1;
        x2[i] = dx2;
        y2[i] = dy2;
        (*n)++;
        return;
    }

    best = 0;
    best_grow = INT32_MAX;
    for (i = 0; i < *n; i++) {
        int32_t ux1 = x1[i] < dx1 ? x1[i] : dx1;
        int32_t uy1 = y1[i] < dy1 ? y1[i] : dy1;
        int32_t ux2 = x2[i] > dx2 ? x2[i] : dx2;
        int32_t uy2 = y2[i] > dy2 ? y2[i] : dy2;
        int32_t old_a = (x2[i] - x1[i]) * (y2[i] - y1[i]);
        int32_t new_a = (ux2 - ux1) * (uy2 - uy1);
        int32_t grow = new_a - old_a;

        if (grow < best_grow) {
            best_grow = grow;
            best = i;
        }
    }
    {
        int32_t ux1 = x1[best] < dx1 ? x1[best] : dx1;
        int32_t uy1 = y1[best] < dy1 ? y1[best] : dy1;
        int32_t ux2 = x2[best] > dx2 ? x2[best] : dx2;
        int32_t uy2 = y2[best] > dy2 ? y2[best] : dy2;

        x1[best] = ux1;
        y1[best] = uy1;
        x2[best] = ux2;
        y2[best] = uy2;
    }
}

void vmap_label_retained_scroll(vmap_label_collector_t * col,
    int32_t sx, int32_t sy, uint8_t dirty_cap, uint8_t * dirty_n,
    int32_t * dirty_x1, int32_t * dirty_y1,
    int32_t * dirty_x2, int32_t * dirty_y2)
{
    int32_t i;
    bool dropped;

    if (!col || !col->retained) {
        return;
    }

    for (i = 0; i < col->retained_count; i++) {
        col->retained[i].x -= sx;
        col->retained[i].y -= sy;
        col->retained[i].box.x1 -= sx;
        col->retained[i].box.x2 -= sx;
        col->retained[i].box.y1 -= sy;
        col->retained[i].box.y2 -= sy;
    }

    if (!dirty_n || !dirty_x1 || !dirty_y1 || !dirty_x2 || !dirty_y2) {
        int32_t out = 0;

        for (i = 0; i < col->retained_count; i++) {
            const vmap_label_retained_t * e = &col->retained[i];

            if (e->box.x2 < 0 || e->box.y2 < 0
                || e->box.x1 >= col->canvas_w || e->box.y1 >= col->canvas_h) {
                continue;
            }
            col->retained[out++] = *e;
        }
        col->retained_count = out;
        return;
    }

    do {
        int32_t out = 0;

        dropped = false;
        for (i = 0; i < col->retained_count; i++) {
            vmap_label_retained_t e = col->retained[i];
            bool fully_off;
            bool fully_on;
            bool hit_dirty = false;
            uint8_t d;

            fully_off = (e.box.x2 < 0 || e.box.y2 < 0
                || e.box.x1 >= col->canvas_w || e.box.y1 >= col->canvas_h);
            fully_on = (e.box.x1 >= 0 && e.box.y1 >= 0
                && e.box.x2 < col->canvas_w && e.box.y2 < col->canvas_h);

            if (fully_off) {
                dropped = true;
                continue;
            }

            for (d = 0; d < *dirty_n; d++) {
                lv_area_t dirty;

                dirty.x1 = dirty_x1[d];
                dirty.y1 = dirty_y1[d];
                dirty.x2 = dirty_x2[d] - 1;
                dirty.y2 = dirty_y2[d] - 1;
                if (boxes_overlap(&e.box, &dirty, 0, 0)) {
                    hit_dirty = true;
                    break;
                }
            }

            if (!fully_on || hit_dirty) {
                dirty_include_inclusive(dirty_cap, dirty_n,
                    dirty_x1, dirty_y1, dirty_x2, dirty_y2,
                    col->canvas_w, col->canvas_h,
                    e.box.x1 - 1, e.box.y1 - 1,
                    e.box.x2 + 1, e.box.y2 + 1);
                dropped = true;
                continue;
            }

            col->retained[out++] = e;
        }
        col->retained_count = out;
    } while (dropped);
}

/**
 * @brief 设置焦点（优先显示附近标签）。
 */
void vmap_label_collector_set_focus(vmap_label_collector_t * col, int32_t x, int32_t y)
{
    if (col) {
        col->focus_x = x;
        col->focus_y = y;
    }
}

/**
 * @brief 从瓦片收集标签候选。
 */
void vmap_label_collector_add_tile(vmap_label_collector_t * col,
    const vmap_tile_t * tile, float base_x, float base_y, float scale)
{
    if (!col || !tile) {
        return;
    }
    collect_point_labels(col, tile, base_x, base_y, scale);
    collect_road_labels(col, tile, base_x, base_y, scale);
}

/**
 * @brief 把选中的标签盖进地图画布（导航线之上、REC 之下）。
 */
bool vmap_label_paint_to_canvas(lv_obj_t * canvas, vmap_label_collector_t * col,
    const lv_font_t * font, double scale, uint8_t dirty_n,
    const int32_t * dirty_x1, const int32_t * dirty_y1,
    const int32_t * dirty_x2, const int32_t * dirty_y2,
    lv_area_t * out_drawn)
{
    vmap_label_scratch_t *sc;
    int32_t *road_idx;
    int32_t *oth_idx;
    char (*layout_buf)[64];
    lv_area_t *placed;
    int32_t *final_idx;
    int32_t road_n = 0;
    int32_t oth_n = 0;
    int32_t retained_n = 0;
    int32_t collision_count = 0;
    int32_t selected_count = 0;
    int32_t road_drawn = 0;
    int32_t oth_drawn = 0;
    int32_t i;
    int32_t a;
    int32_t b;
    int32_t k;
    double gap_scale;
    int32_t vis_x0;
    int32_t vis_y0;
    int32_t vis_x1;
    int32_t vis_y1;
    int32_t road_gap_x;
    int32_t road_gap_y;
    int32_t oth_gap_x;
    int32_t oth_gap_y;
    int32_t same_name_gap;
    int32_t max_road;
    lv_layer_t draw_layer;
    lv_draw_label_dsc_t label_dsc;
    lv_display_t * disp;
    bool drew = false;

    if (!canvas || !col || !font || col->count <= 0) {
        return false;
    }

    sc = label_scratch();
    if (sc == NULL) {
        return false;
    }

    LV_UNUSED(dirty_n);
    LV_UNUSED(dirty_x1);
    LV_UNUSED(dirty_y1);
    LV_UNUSED(dirty_x2);
    LV_UNUSED(dirty_y2);

    road_idx = sc->road_idx;
    oth_idx = sc->oth_idx;
    layout_buf = sc->layout_buf;
    placed = sc->placed;
    final_idx = sc->final_idx;

    if (col->retained && col->retained_count > 0) {
        retained_n = col->retained_count;
        if (retained_n > VMAP_LABEL_RETAIN_CAP) {
            retained_n = VMAP_LABEL_RETAIN_CAP;
        }
        for (i = 0; i < retained_n; i++) {
            placed[i] = col->retained[i].box;
        }
        collision_count = retained_n;
    }

    const int32_t fx = col->focus_x;
    const int32_t fy = col->focus_y;

    gap_scale = scale;
    if (gap_scale < 0.35) {
        gap_scale = 0.35;
    } else if (gap_scale > 2.5) {
        gap_scale = 2.5;
    }

    /*
     * Labels belong to the retained large canvas, not only the current
     * viewport. They may be baked off-screen and pan in later, but their full
     * text box must fit inside the large canvas.
     */
    vis_x0 = VMAP_PAN_GUARD;
    vis_y0 = VMAP_PAN_GUARD;
    vis_x1 = col->canvas_w - VMAP_PAN_GUARD - 1;
    vis_y1 = col->canvas_h - VMAP_PAN_GUARD - 1;

    for (i = 0; i < col->count; i++) {
        if (col->items[i].kind == VMAP_LABEL_ROAD) {
            road_idx[road_n++] = i;
        }
    }

    for (a = 1; a < road_n; a++) {
        int32_t v = road_idx[a];
        vmap_label_cand_t * cv = &col->items[v];
        int64_t vk = road_canvas_sort_key(cv->x, cv->y, fx, fy);
        b = a - 1;
        while (b >= 0) {
            vmap_label_cand_t * cb = &col->items[road_idx[b]];
            if (road_canvas_sort_key(cb->x, cb->y, fx, fy) >= vk) {
                break;
            }
            road_idx[b + 1] = road_idx[b];
            b--;
        }
        road_idx[b + 1] = v;
    }

    /*
     * First attempt one closest candidate for every distinct road name, then
     * consider repeats. This prevents one long arterial from consuming the
     * collision budget before shorter visible roads get a label.
     */
    {
        int32_t unique_n = 0;
        int32_t out_n;

        for (i = 0; i < road_n; i++) {
            vmap_label_cand_t * c = &col->items[road_idx[i]];
            bool known = false;
            int32_t j;

            for (j = 0; j < retained_n; j++) {
                const lv_area_t * rb;

                if (col->retained[j].kind != VMAP_LABEL_ROAD
                    || lv_strcmp(col->retained[j].name, c->name) != 0) {
                    continue;
                }
                rb = &col->retained[j].box;
                if (rb->x1 >= 0 && rb->y1 >= 0
                    && rb->x2 < col->canvas_w && rb->y2 < col->canvas_h) {
                    known = true;
                    break;
                }
            }
            for (j = 0; !known && j < unique_n; j++) {
                if (lv_strcmp(col->items[oth_idx[j]].name, c->name) == 0) {
                    known = true;
                }
            }
            if (!known) {
                oth_idx[unique_n++] = road_idx[i];
            }
        }

        out_n = unique_n;
        for (i = 0; i < road_n; i++) {
            bool is_unique = false;
            int32_t j;

            for (j = 0; j < unique_n; j++) {
                if (road_idx[i] == oth_idx[j]) {
                    is_unique = true;
                    break;
                }
            }
            if (!is_unique) {
                oth_idx[out_n++] = road_idx[i];
            }
        }
        lv_memcpy(road_idx, oth_idx, (size_t)road_n * sizeof(road_idx[0]));
    }

    for (i = 0; i < col->count; i++) {
        bool best = true;
        int32_t j;

        if (col->items[i].kind == VMAP_LABEL_ROAD) {
            continue;
        }
        for (j = 0; j < col->count; j++) {
            if (i == j || col->items[j].kind == VMAP_LABEL_ROAD) {
                continue;
            }
            if (lv_strcmp(col->items[i].name, col->items[j].name) != 0) {
                continue;
            }
            if (other_label_score(col->items[j].kind, col->items[j].prio,
                    col->items[j].x, col->items[j].y, fx, fy, scale)
                > other_label_score(col->items[i].kind, col->items[i].prio,
                    col->items[i].x, col->items[i].y, fx, fy, scale)) {
                best = false;
                break;
            }
        }
        if (best) {
            oth_idx[oth_n++] = i;
        }
    }

    for (a = 1; a < oth_n; a++) {
        int32_t v = oth_idx[a];
        vmap_label_cand_t * cv = &col->items[v];
        int32_t sv = other_label_score(cv->kind, cv->prio, cv->x, cv->y, fx, fy, scale);
        b = a - 1;
        while (b >= 0) {
            vmap_label_cand_t * cb = &col->items[oth_idx[b]];
            if (other_label_score(cb->kind, cb->prio, cb->x, cb->y, fx, fy, scale) >= sv) {
                break;
            }
            oth_idx[b + 1] = oth_idx[b];
            b--;
        }
        oth_idx[b + 1] = v;
    }

    road_gap_x = (int32_t)lround((double)col->view_w * 0.025 * gap_scale);
    road_gap_y = (int32_t)lround((double)col->view_h * 0.025 * gap_scale);
    oth_gap_x = (int32_t)lround((double)col->view_w * 0.06 * gap_scale);
    oth_gap_y = (int32_t)lround((double)col->view_h * 0.055 * gap_scale);
    if (road_gap_x < 4) {
        road_gap_x = 4;
    }
    if (road_gap_y < 4) {
        road_gap_y = 4;
    }
    {
        double spacing_x;
        double spacing_y;

        road_label_spacing_from_viewport(col->view_w, col->view_h,
            &spacing_x, &spacing_y);
        same_name_gap = (int32_t)lround((spacing_x + spacing_y) * 0.5);
        if (same_name_gap < 60) {
            same_name_gap = 60;
        }
    }

    max_road = VMAP_LABEL_MAX_ROAD;
    if (scale < 0.85) {
        max_road = (int32_t)(VMAP_LABEL_MAX_ROAD * (0.55 + 0.45 * scale));
        if (max_road < 24) {
            max_road = 24;
        }
    }

    /* road_idx is sorted nearest-focus first; keep that order for collisions. */
    for (a = 0; a < road_n && road_drawn < max_road; a++) {
        int32_t idx = road_idx[a];
        vmap_label_cand_t * c = &col->items[idx];
        int32_t clip_x0 = vis_x0;
        int32_t clip_y0 = vis_y0;
        int32_t clip_x1 = vis_x1;
        int32_t clip_y1 = vis_y1;
        lv_point_t sz;
        int32_t box_w;
        int32_t box_h;
        lv_area_t box;
        int32_t ki;
        bool overlap = false;

        if (c->x < clip_x0 || c->x > clip_x1 || c->y < clip_y0 || c->y > clip_y1) {
            continue;
        }
        if (!label_in_clip(col, c->x, c->y)) {
            continue;
        }
        if (selected_count >= VMAP_LABEL_MAX_DRAWN) {
            break;
        }
        if (col->retained && col->retained_cap > 0
            && retained_n + selected_count >= col->retained_cap) {
            break;
        }

        road_label_measure(c->kind, c->road_vertical, c->name, font,
            layout_buf[selected_count], sizeof(layout_buf[0]), &sz);
        box_w = sz.x + VMAP_LABEL_PAD * 2;
        box_h = sz.y + VMAP_LABEL_PAD * 2;
        box.x1 = c->x - box_w / 2;
        box.y1 = c->y - box_h / 2;
        box.x2 = box.x1 + box_w - 1;
        box.y2 = box.y1 + box_h - 1;
        if (box.x1 < clip_x0 || box.y1 < clip_y0
            || box.x2 > clip_x1 || box.y2 > clip_y1) {
            continue;
        }
        for (ki = 0; ki < collision_count; ki++) {
            const char * pk_name;
            uint8_t pk_kind;
            int32_t pk_x;
            int32_t pk_y;
            int32_t dx;
            int32_t dy;

            if (ki < retained_n) {
                pk_name = col->retained[ki].name;
                pk_kind = col->retained[ki].kind;
                pk_x = col->retained[ki].x;
                pk_y = col->retained[ki].y;
            } else {
                vmap_label_cand_t * pk =
                    &col->items[final_idx[ki - retained_n]];

                pk_name = pk->name;
                pk_kind = pk->kind;
                pk_x = pk->x;
                pk_y = pk->y;
            }
            dx = pk_x - c->x;
            dy = pk_y - c->y;
            if (pk_kind == VMAP_LABEL_ROAD
                && lv_strcmp(pk_name, c->name) == 0
                && (dx * dx + dy * dy) < same_name_gap * same_name_gap) {
                overlap = true;
                break;
            }
            if (boxes_overlap(&box, &placed[ki], road_gap_x, road_gap_y)) {
                overlap = true;
                break;
            }
        }
        if (!overlap) {
            placed[retained_n + selected_count] = box;
            final_idx[selected_count] = idx;
            selected_count++;
            collision_count++;
            road_drawn++;
        }
    }

    for (a = 0; a < oth_n && oth_drawn < VMAP_LABEL_MAX_OTHER; a++) {
        int32_t idx = oth_idx[a];
        vmap_label_cand_t * c = &col->items[idx];
        int32_t clip_x0 = vis_x0;
        int32_t clip_y0 = vis_y0;
        int32_t clip_x1 = vis_x1;
        int32_t clip_y1 = vis_y1;
        lv_point_t sz;
        int32_t box_w;
        int32_t box_h;
        lv_area_t box;
        int32_t ki;
        bool overlap = false;

        if (c->x < clip_x0 || c->x > clip_x1 || c->y < clip_y0 || c->y > clip_y1) {
            continue;
        }
        if (!label_in_clip(col, c->x, c->y)) {
            continue;
        }
        if (selected_count >= VMAP_LABEL_MAX_DRAWN) {
            break;
        }
        if (col->retained && col->retained_cap > 0
            && retained_n + selected_count >= col->retained_cap) {
            break;
        }

        road_label_measure(c->kind, c->road_vertical, c->name, font,
            layout_buf[selected_count], sizeof(layout_buf[0]), &sz);
        box_w = sz.x + VMAP_LABEL_PAD * 2;
        box_h = sz.y + VMAP_LABEL_PAD * 2;
        box.x1 = c->x - box_w / 2;
        box.y1 = c->y - box_h / 2;
        box.x2 = box.x1 + box_w - 1;
        box.y2 = box.y1 + box_h - 1;
        if (box.x1 < clip_x0 || box.y1 < clip_y0
            || box.x2 > clip_x1 || box.y2 > clip_y1) {
            continue;
        }
        for (ki = 0; ki < collision_count; ki++) {
            if (boxes_overlap(&box, &placed[ki], oth_gap_x, oth_gap_y)) {
                overlap = true;
                break;
            }
        }
        if (!overlap) {
            placed[retained_n + selected_count] = box;
            final_idx[selected_count] = idx;
            selected_count++;
            collision_count++;
            oth_drawn++;
        }
    }

    lv_canvas_init_layer(canvas, &draw_layer);
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = font;
    label_dsc.align = LV_TEXT_ALIGN_LEFT;
    label_dsc.text_local = 1;
    label_dsc.outline_stroke_width = 1;
    label_dsc.outline_stroke_opa = LV_OPA_70;
    label_dsc.outline_stroke_color = lv_color_white();

    for (k = 0; k < selected_count; k++) {
        vmap_label_cand_t * c = &col->items[final_idx[k]];
        const vmap_style_t * style = vmap_style_current();
        lv_area_t text_area;
        lv_area_t * box = &placed[retained_n + k];

        switch (c->kind) {
        case VMAP_LABEL_PLACE:
            label_dsc.color = lv_color_hex(style->lbl_place);
            break;
        case VMAP_LABEL_WATER:
            label_dsc.color = lv_color_hex(style->lbl_water);
            break;
        case VMAP_LABEL_POI:
            label_dsc.color = lv_color_hex(style->lbl_poi);
            break;
        default:
            label_dsc.color = lv_color_hex(style->lbl_road);
            break;
        }

        text_area.x1 = (lv_coord_t)(box->x1 + VMAP_LABEL_PAD);
        text_area.y1 = (lv_coord_t)(box->y1 + VMAP_LABEL_PAD);
        text_area.x2 = (lv_coord_t)(box->x2 - VMAP_LABEL_PAD);
        text_area.y2 = (lv_coord_t)(box->y2 - VMAP_LABEL_PAD);
        if (text_area.x2 < text_area.x1) {
            text_area.x2 = text_area.x1;
        }
        if (text_area.y2 < text_area.y1) {
            text_area.y2 = text_area.y1;
        }
        if (col->retained && col->retained_count >= col->retained_cap) {
            /* Never bake ink we cannot record; leftovers become duplicates. */
            break;
        }

        label_dsc.text = layout_buf[k];
        lv_draw_label(&draw_layer, &label_dsc, &text_area);
        if (out_drawn) {
            if (!drew) {
                *out_drawn = *box;
            } else {
                if (box->x1 < out_drawn->x1) {
                    out_drawn->x1 = box->x1;
                }
                if (box->y1 < out_drawn->y1) {
                    out_drawn->y1 = box->y1;
                }
                if (box->x2 > out_drawn->x2) {
                    out_drawn->x2 = box->x2;
                }
                if (box->y2 > out_drawn->y2) {
                    out_drawn->y2 = box->y2;
                }
            }
        }
        drew = true;

        if (col->retained) {
            vmap_label_retained_t * r =
                &col->retained[col->retained_count++];

            r->box = *box;
            r->x = c->x;
            r->y = c->y;
            r->kind = c->kind;
            lv_strlcpy(r->name, c->name, sizeof(r->name));
        }
    }

    disp = lv_obj_get_display(canvas);
    lv_display_enable_invalidation(disp, false);
    lv_canvas_finish_layer(canvas, &draw_layer);
    lv_display_enable_invalidation(disp, true);
    return drew;
}
