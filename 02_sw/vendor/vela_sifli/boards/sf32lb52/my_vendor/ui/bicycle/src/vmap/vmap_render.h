/**
 * @file vmap_render.h
 * @brief vmap RGB565 软件光栅化（可选子像素抗锯齿 VMAP_RENDER_AA）。
 */

#ifndef VMAP_RENDER_H
#define VMAP_RENDER_H

#include "vmap_config.h"
#include "vmap_tile.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 当前主题背景 RGB565。
 */
uint16_t vmap_style_bg565(void);

typedef struct {
    uint16_t * buf;
    int32_t w;
    int32_t h;
    uint32_t stride;
    /** @brief 额外裁剪矩形数；0 表示只受画布边界限制。 */
    uint8_t clip_n;
    int32_t clip_x1[VMAP_RENDER_DIRTY_MAX];
    int32_t clip_y1[VMAP_RENDER_DIRTY_MAX];
    int32_t clip_x2[VMAP_RENDER_DIRTY_MAX];
    int32_t clip_y2[VMAP_RENDER_DIRTY_MAX];
} vmap_canvas_t;

typedef struct {
    int32_t x;
    int32_t y;
} vmap_point_t;

/**
 * @brief 用纯色清画布。
 * @param c 画布。
 * @param color RGB565。
 */
void vmap_render_clear(vmap_canvas_t * c, uint16_t color);
/**
 * @brief 平移已有像素：dest(x,y) = src(x+dx, y+dy)，露出的边用 fill 填充。
 * @param dx 源相对目的的水平偏移（跟车 pan 像素）。
 * @param dy 源相对目的的垂直偏移。
 */
void vmap_render_scroll(vmap_canvas_t * c, int32_t dx, int32_t dy, uint16_t fill);
void vmap_render_clip_reset(vmap_canvas_t * c);
void vmap_render_clip_add(vmap_canvas_t * c, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2);
void vmap_render_draw_tile(vmap_canvas_t * c, const vmap_tile_t * tile,
    float base_x, float base_y, float scale, vmap_point_t * scratch, int scratch_cap);
void vmap_render_draw_thick_line(vmap_canvas_t * c, int32_t x0, int32_t y0,
    int32_t x1, int32_t y1, int32_t width, uint16_t color);
void vmap_render_draw_thick_line_f(vmap_canvas_t * c, double x0, double y0,
    double x1, double y1, double width, uint16_t color);
void vmap_render_fill_disc(vmap_canvas_t * c, int32_t cx, int32_t cy,
    int32_t radius, uint16_t color);
void vmap_render_fill_disc_f(vmap_canvas_t * c, double cx, double cy,
    double radius, uint16_t color);
int32_t vmap_render_simplify_polyline(vmap_point_t * pts, int32_t n,
    float min_seg_px, float max_dev_px);
void vmap_render_stroke_ribbon(vmap_canvas_t * c, const vmap_point_t * pts,
    int32_t n, int32_t width, uint16_t color);

/**
 * @brief 复位"画布存储次数/字节数"计数（性能取证用，见 render 文件里的 STAT_STORE）。
 *
 * 目的：分辨 (a)成本 ∝ 写入**字节数**，还是 (b)成本 ∝ **每次 store 一次总线事务**。
 * 实测一个 4 字节 store 要约 88 周期，而 16 字节的 stmia 要约 357 ≈ 4×88，
 * 说明写通路会把宽写拆成 4 字节事务 —— 若是 (b)，则"减少 store 次数"才是杠杆。
 */
void vmap_render_stat_reset(void);
/** @brief 取计数；任一参数可为 NULL。 */
void vmap_render_stat_get(uint32_t * stores, uint32_t * bytes);

/** 【临时取证】图元级周期拆分快照（测完即撤）。 */
typedef struct {
    uint32_t poly_fp;    /**< fill_polygon_fp 周期数（AA 多边形 + 折线四边形） */
    uint32_t poly;       /**< fill_polygon 周期数（面要素填充） */
    uint32_t disc;       /**< fill_disc_aa_fp 周期数（圆盘 / 折线拐点） */
    uint32_t n_poly_fp;
    uint32_t n_poly;
    uint32_t n_disc;
    uint32_t cross;      /**< 逐行软除次数 = 活动边 × 行 */
    uint32_t scan;       /**< (行 × 边) 扫描次数（无活动边表的代价） */
    uint32_t hist[6];    /**< n 直方图：<=4 / <=8 / <=16 / <=32 / <=256 / >256 */
    uint32_t cyc_n[6];   /**< 按 n 分桶的周期数 */
} vmap_render_prim_t;

void vmap_render_prim_get(vmap_render_prim_t * out);

/** 【临时取证】多边形定点自测的结果（开机一次，跑完即撤）。 */
typedef struct {
    uint32_t pass;
    uint32_t fp_cyc;
    uint32_t fp_cross;
    uint32_t in_cyc;
    uint32_t in_cross;
} vmap_render_polyself_t;

void vmap_render_polyselftest(vmap_render_polyself_t * out);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_RENDER_H */
