/**
 * @file vmap_view.h
 * @brief retained RGB565 地图视图：瓦片泵、跟车平移、导航/REC 叠层。
 *
 * 地图叠层（底 → 顶），跟车平移时整幅一起挪，避免各层错位：
 *   1. 地图瓦片（RGB565 画布最底层）
 *   2. 导航线（盖进画布）
 *   3. 道路/地名标签（盖进画布，不再用 LVGL overlay）
 *   4. REC 轨迹线 + 公里圆标（盖进画布）
 *   5. REC 公里数字（盖进画布，最顶的栅格）
 * LVGL 定位箭头在画布之上；Helm 状态栏/仪表在地图页之上。
 */

#ifndef VMAP_VIEW_H
#define VMAP_VIEW_H

#include "lvgl/lvgl.h"
#include "vmap_config.h"
#include "vmap_render.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vmap_view vmap_view_t;

/**
 * @brief 开机地图中心：合法上次骑行终点，否则北京天安门。
 */
void vmap_boot_lonlat(double *lon, double *lat);
/**
 * @brief 上次定位是否可作开机中心（有限、非原点、在全国图范围内）。
 */
bool vmap_boot_ll_ok(double lon, double lat);

/**
 * @brief 创建带 overscan 的 RGB565 大画布。
 * @param parent 地图视口（通常是 `map_area`）。
 * @param view_w 可视宽。
 * @param view_h 可视高。
 * @param margin 四周 overscan（像素）。
 * @return 视图；失败为 NULL。
 * @details 缓冲区先 `memset` 0；canvas 隐藏，直到第一帧 `render_finish`
 *          才显示，避免把未初始化的 PSRAM 送到 LCD。
 */
vmap_view_t * vmap_view_create(lv_obj_t * parent, int32_t view_w, int32_t view_h,
    int32_t margin);
/**
 * @brief 销毁视图：释放画布、瓦片缓存与箭头绑定。
 */
void vmap_view_destroy(vmap_view_t * view);
/**
 * @brief 底层 LVGL canvas 对象（retained RGB565）。
 */
lv_obj_t * vmap_view_get_obj(const vmap_view_t * view);

/**
 * @brief 设置地图目录（网格 `.vpk` 根，如 `/mnt/fat/map`）。
 */
void vmap_view_set_tile_dir(vmap_view_t * view, const char * dir);
/**
 * @brief 设置中心与缩放。不打开 .vpk；render_pump 第一拍再 ensure_region。
 */
void vmap_view_set_view(vmap_view_t * view, double lon, double lat, int zoom);
/**
 * @brief 打开当前经纬度所在 cell 的 region pack。
 * @return 已就绪或打开成功为 true。
 */
bool vmap_view_ensure_region(vmap_view_t * view, double lon, double lat);
struct vmap_tile_cache;
/** @brief 解析瓦片 LRU / region slot 缓存。 */
struct vmap_tile_cache * vmap_view_tile_cache(vmap_view_t * view);
/** @brief 当前瓦片 zoom（整数级）。 */
int vmap_view_get_zoom(const vmap_view_t * view);
/** @brief 当前显示倍率（相对瓦片像素）。 */
double vmap_view_get_scale(const vmap_view_t * view);
/**
 * @brief 当前跟车中心（经纬度）。
 */
void vmap_view_get_center(const vmap_view_t * view, double * lon, double * lat);
/**
 * @brief 改显示倍率；必要时触发重绘。
 */
void vmap_view_set_scale(vmap_view_t * view, double scale);
/**
 * @brief 显示偏置：把画布内容与箭头一起沿 y 平移多少像素（导航横幅盖住顶部时用）。
 *
 * **只作用于显示位置**，不进跟车/rebase 的数学（pan_* 与投影原点一律不变）。
 * 取值为「需要让出来的那块高度的一半」：通栏导航横幅即 HELM_NAV_BAN_H/2。
 */
void vmap_view_set_y_bias(vmap_view_t * view, int32_t bias);
/**
 * @brief 路名标签字体。
 */
void vmap_view_set_font(vmap_view_t * view, const lv_font_t * font);
/**
 * @brief 开始一帧栅格化。跟车重锚时平移已有像素、只补新边，避免整屏清空。
 *        由 render_pump 分瓦片、限时切片画完。
 * @note MTP quiesce 时直接返回。boot_deferred 期间不要在主题回调里调用，
 *       留给 Startup warmup 的 `set_view` + `render`。
 */
void vmap_view_render(vmap_view_t * view);

/**
 * @brief 按当前瓦片原点重画，不改 follow pan、不清空画布。
 *
 * 用于擦掉已经画进 canvas 的轨迹，避免开/停 REC 时地图跳回中心。
 */
void vmap_view_redraw_keep_pan(vmap_view_t * view);

/**
 * @brief 在已有底图上按叠层盖导航线，再盖 REC 线/数字。
 * @details 不含标签：进度刷新若重描全部路名会把字描胖。完整叠层只在瓦片结束帧做。
 */
void vmap_view_stamp_nav_then_track(vmap_view_t * view);

/**
 * @brief 一次只装/画若干瓦片，并受时间预算限制。phase==0 时视为空闲。
 * @return 本帧已画完或空闲则为 true。
 */
bool vmap_view_render_pump(vmap_view_t * view, unsigned max_tiles);

/**
 * @brief 是否正在分片绘制一帧。
 */
bool vmap_view_render_busy(const vmap_view_t * view);

/**
 * @brief 跟车：改中心经纬度。未越 overscan 只平移 canvas；越界才 rebase。
 * @note 开机 `map_ui_seen==false` 时页面层禁止调用，以免提前开出半帧。
 */
void vmap_view_set_center(vmap_view_t * view, double lon, double lat);
/**
 * @brief 绑定定位箭头所在 LVGL 层（通常是 `map_area`）。
 */
void vmap_view_bind_arrow_layer(vmap_view_t * view, lv_obj_t * layer);
/**
 * @brief 设置箭头图源；跟随模式创建后立刻居中。
 */
void vmap_view_set_arrow_image(vmap_view_t * view, const void * src);
/**
 * @brief 跟随模式：箭头钉在视口中心，平移画布。
 */
void vmap_view_set_follow_mode(vmap_view_t * view, bool enable);
/**
 * @brief 按经纬度与航向摆箭头。
 */
void vmap_view_update_arrow(vmap_view_t * view, double lon, double lat, float course_deg);
/** @brief 显示或隐藏定位箭头（轨迹回放时关掉）。 */
void vmap_view_set_arrow_visible(vmap_view_t * view, bool visible);
/**
 * @brief 把箭头提到 map_area 最前（chrome 之下）。
 */
void vmap_view_bring_arrow_to_front(vmap_view_t * view);

/** @brief 丢弃瓦片索引/arena 并重绘（MTP 释放 LFS 后）。 */
void vmap_view_reload_storage(vmap_view_t * view);
/** @brief 关闭已打开的地图分片句柄；保留 PSRAM 瓦片缓存与画布。 */
void vmap_view_release_storage(vmap_view_t * view);

/**
 * @brief 经纬度 → 视口像素（含 follow pan）。
 * @return 在视口（含少量 pad）内为 true。
 */
bool vmap_view_geo_to_viewport(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/**
 * @brief 经纬度 → 视口像素，不做可见性裁剪。
 */
bool vmap_view_geo_to_viewport_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/**
 * @brief 经纬度 → 画布整数像素（相对 canvas 左上）。
 */
bool vmap_view_geo_to_canvas_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/**
 * @brief 经纬度 → 画布浮点像素。
 */
bool vmap_view_geo_to_canvas_f(const vmap_view_t * view, double lon, double lat,
    double * out_x, double * out_y);

/** @brief 缓存画布投影原点，避免轨迹扫描时每点重复算中心墨卡托。 */
typedef struct {
    int base_x;
    int base_y;
    int zoom;
    double scale;
    double half_w;
    double half_h;
} vmap_view_proj_t;

/** @brief 从当前视图填投影缓存。 */
void vmap_view_proj_begin(const vmap_view_t * view, vmap_view_proj_t * proj);
/** @brief 用缓存把一点投到画布整数坐标。 */
void vmap_view_proj_point(const vmap_view_proj_t * proj, double lon, double lat,
    int32_t * out_x, int32_t * out_y);
/** @brief 用缓存把一点投到画布浮点坐标。 */
void vmap_view_proj_point_f(const vmap_view_proj_t * proj, double lon, double lat,
    double * out_x, double * out_y);

/**
 * @brief retained 画布宽高（含 overscan）。
 */
void vmap_view_get_buffer_size(const vmap_view_t * view, int32_t * w, int32_t * h);
/** @brief 画布坐标点可安全描边时为 true（避免色带填充伪影）。 */
bool vmap_view_canvas_point_sane(const vmap_view_t * view, int32_t x, int32_t y);
/**
 * @brief 画布点是否落在可视窗口内。
 */
bool vmap_view_canvas_point_visible(const vmap_view_t * view, int32_t x, int32_t y);
/**
 * @brief 画布点是否落在可视窗口外扩 @p pad 像素内。
 */
bool vmap_view_canvas_point_in_pad(const vmap_view_t * view, int32_t x, int32_t y,
    int32_t pad);
/**
 * @brief 取出当前 RGB565 画布描述。
 */
bool vmap_view_get_canvas(const vmap_view_t * view, vmap_canvas_t * canvas);
/**
 * @brief 仅刷新画布局部区域（坐标相对 canvas 左上角）。
 */
void vmap_view_invalidate_canvas_area(const vmap_view_t * view,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2);
/**
 * @brief 地图存储世代（MTP reload 后递增）。
 */
uint32_t vmap_view_map_revision(const vmap_view_t * view);
/**
 * @brief 已完成的渲染帧计数。
 * @return 0 表示从未 `render_finish`；开机 splash 要等非 0 且 `!busy`。
 */
uint32_t vmap_view_render_revision(const vmap_view_t * view);

#if VMAP_ROUTE_ENABLE
struct vmap_route;
/**
 * @brief 把规划路线绑到本视图（导航线叠层）。
 */
void vmap_view_attach_route(vmap_view_t * view, struct vmap_route * route);
#endif

#ifdef __cplusplus
}
#endif

#endif /* VMAP_VIEW_H */
