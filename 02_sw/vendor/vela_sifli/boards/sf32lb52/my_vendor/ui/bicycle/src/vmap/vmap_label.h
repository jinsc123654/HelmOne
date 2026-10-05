/**
 * @file vmap_label.h
 * @brief TTF 标签收集与绘制（道路/地名/水域/POI）。
 *
 * 标签盖进地图 RGB565 画布，叠层见 vmap_view.h：底图 → 导航线 → 标签 → REC 线 → REC 数字。
 */

#ifndef VMAP_LABEL_H
#define VMAP_LABEL_H

#include "vmap_tile.h"
#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMAP_LABEL_CAP          256
#define VMAP_LABEL_MAX_ROAD       96
#define VMAP_LABEL_MAX_OTHER      8
#define VMAP_LABEL_MAX_DRAWN      (VMAP_LABEL_MAX_ROAD + VMAP_LABEL_MAX_OTHER)
/** 跨多轮 dirty-edge 增量烘焙的固定 PSRAM 占位容量。 */
#define VMAP_LABEL_RETAIN_CAP      256

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t kind;
    uint8_t prio;
    uint8_t road_vertical;
    char name[64];
} vmap_label_cand_t;

/** 已烤进 retained canvas 的标签占位；固定分配在 PSRAM。 */
typedef struct {
    lv_area_t box;
    int32_t x;
    int32_t y;
    uint8_t kind;
    char name[64];
} vmap_label_retained_t;

typedef struct {
    vmap_label_cand_t * items;
    int32_t count;
    int32_t cap;
    int32_t focus_x;
    int32_t focus_y;
    int32_t canvas_w;
    int32_t canvas_h;
    int32_t view_w;
    int32_t view_h;
    int32_t margin;
    uint8_t clip_n;
    int32_t clip_x1[2];
    int32_t clip_y1[2];
    int32_t clip_x2[2];
    int32_t clip_y2[2];
    vmap_label_retained_t * retained;
    int32_t retained_count;
    int32_t retained_cap;
} vmap_label_collector_t;

void vmap_label_collector_init(vmap_label_collector_t * col,
    vmap_label_cand_t * buf, int32_t cap,
    vmap_label_retained_t * retained, int32_t retained_cap,
    int32_t canvas_w, int32_t canvas_h, int32_t view_w, int32_t view_h,
    int32_t margin);

/**
 * @brief 清空已收集标签。
 */
void vmap_label_collector_reset(vmap_label_collector_t * col);
/** 清除所有已烤标签占位（全画布清屏/重画时调用）。 */
void vmap_label_retained_reset(vmap_label_collector_t * col);
/**
 * @brief 画布 scroll 后同步移动占位。
 *
 * 完全离开大画布的占位直接丢弃。文本框不再完整落在画布内、或与 dirty
 * 相交时，把该框仍在画布上的像素并入 dirty（整框擦除）再删占位，避免
 * 留下无记录残字。`*dirty_n` 会被扩大。
 */
void vmap_label_retained_scroll(vmap_label_collector_t * col,
    int32_t sx, int32_t sy, uint8_t dirty_cap, uint8_t * dirty_n,
    int32_t * dirty_x1, int32_t * dirty_y1,
    int32_t * dirty_x2, int32_t * dirty_y2);
/**
 * @brief 设置焦点（优先显示附近标签）。
 */
void vmap_label_collector_set_focus(vmap_label_collector_t * col, int32_t x, int32_t y);

void vmap_label_collector_add_tile(vmap_label_collector_t * col,
    const vmap_tile_t * tile, float base_x, float base_y, float scale);

/**
 * @brief 把选中的标签盖进地图画布（导航线之上、REC 之下）。
 * @param dirty_n 仅描述本轮底图脏区；PSRAM 占位负责阻止 retained 标签重描。
 * @param out_drawn 返回本轮新增标签的 canvas 包围框，供 REC 在其上补描。
 */
bool vmap_label_paint_to_canvas(lv_obj_t * canvas, vmap_label_collector_t * col,
    const lv_font_t * font, double scale, uint8_t dirty_n,
    const int32_t * dirty_x1, const int32_t * dirty_y1,
    const int32_t * dirty_x2, const int32_t * dirty_y2,
    lv_area_t * out_drawn);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_LABEL_H */
