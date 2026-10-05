/**
 * @file map_page.h
 * @brief 实况地图页。
 *
 * 地图画布叠层（底 → 顶）：底图 → 导航线 → 标签 → REC 线 → REC 数字。
 * 定位箭头是 LVGL 控件，在画布之上。
 */

#ifndef MAP_PAGE_H
#define MAP_PAGE_H

#include "vmap/vmap_view.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_style.h"
#include "lv_pm_core.h"
#include "lvgl/lvgl.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct map_page map_page_t;

/**
 * @brief 向 lv_pm 注册 LiveMap（`lvgl_page_init` 调用）。
 */
void live_map_page_register(void);
/** @brief 已注册的 LiveMap 页描述符（开机静默 push 用）。 */
lv_pm_page_t live_map_page_def(void);
/**
 * @brief 当前 LiveMap 实例。
 * @return 尚未 `create` 则为 NULL。
 */
map_page_t * live_map_page_instance(void);

/**
 * @brief 在 @p parent 上创建地图页（画布、箭头、Helm chrome）。
 * @param parent lv_pm 页面根。
 * @return 实例；失败为 NULL。
 * @note `boot_deferred` 时只建空壳，vmap 留给 `map_page_boot_begin_load()`。
 */
map_page_t * map_page_create(lv_obj_t * parent);
/**
 * @brief 销毁地图页（关 GPX、卸瓦片、删 LVGL 对象）。
 * @param page 实例；可为 NULL。
 */
void map_page_destroy(map_page_t * page);
/**
 * @brief 把当前画布 / 轨迹打到调试缓冲（NSH snap）。
 * @param page 实例。
 */
void map_page_debug_snap(map_page_t * page);

/** @brief 循环主题（classic → nav → outdoor）并刷新 chrome。 */
bool map_page_cycle_style(map_page_t * page);
/**
 * @brief 当前主题名。
 * @param page 未使用，可为 NULL。
 */
const char * map_page_style_name(const map_page_t * page);
/**
 * @brief 按样式 id 切主题。
 * @param page LiveMap。
 * @param id   `vmap_style_id_t`。
 */
bool map_page_set_style(map_page_t * page, vmap_style_id_t id);
/**
 * @brief 按名字切主题（`classic` / `nav` / `outdoor`）。
 */
bool map_page_set_style_by_name(map_page_t * page, const char * name);
/**
 * @brief 按当前主题重刷 chrome / 箭头（不一定重栅格）。
 */
void map_page_redraw(map_page_t * page);

/**
 * @brief 显示/隐藏定位方向箭头（骑行者箭头）。
 * @param page LiveMap。
 * @param visible true 露出箭头；false 藏起来。
 * @note 平时由导航路径自己管（`nav_stop` / `nav_from_gpx` 会打开，"只看轨迹"的
 *       `map_page_show_gpx()` 会关掉）。演示模式用它兜底：万一上一轮进过"回放/只看轨迹"，
 *       箭头是关着的，而**规划导航不会自己把它打开** ⇒ 会看不到方向箭头。
 */
void map_page_set_arrow_visible(map_page_t * page, bool visible);

/** @brief 在 LVGL 线程应用缩放 ctl：in/out/reset/z,N/s,F；非法时返回 false。 */
bool map_page_apply_zoom_cmd(map_page_t * page, const char * arg);
/**
 * @brief 把当前 zoom/scale 写进 ctl 状态，供 NSH 查询。
 */
void map_page_sync_zoom_state(map_page_t * page);

/** @brief UsbTransfer 覆盖 LiveMap：暂停地图 UI；GNSS/轨迹录制继续。不关瓦片句柄。 */
void map_page_pause_for_cover(map_page_t * page);

/** @brief UsbTransfer pop 后恢复地图渲染（从 RAM 重绘轨迹）。 */
void map_page_resume_after_cover(map_page_t * page);

/** @brief MTP 会话结束（拔线）后重载地图瓦片。 */
void map_page_reload_storage(map_page_t * page);
/** @brief 关闭打开的地图分片句柄（MTP 静默）；保留画布与 RAM 缓存。 */
void map_page_release_storage(map_page_t * page);

#if VMAP_ROUTE_ENABLE
#include "vmap/vmap_route_trip.h"
/** @brief 规划路线。可显式传入 from_lon/lat，或 NAN 表示调用时冻结 GNSS。 */
bool map_page_nav_plan(map_page_t * page, double from_lon, double from_lat,
    double to_lon, double to_lat);
/** @brief 规划路线后按 VMAP_NAV_SIM_SPEED_KPH 沿折线驱动。 */
bool map_page_nav_plan_sim(map_page_t * page, double from_lon, double from_lat,
    double to_lon, double to_lat);
/** @brief 多途经点：当前坐标不算，一次规划下一点与下下一点；过点后静默补点。 */
bool map_page_nav_trip_plan(map_page_t * page,
    const vmap_route_waypoint_t * waypoints, uint32_t count);
/** @brief 设置多点导航显示名；当前 leg 名称用于顶部“去 xxx”。 */
void map_page_nav_trip_set_names(map_page_t * page,
    const char * const * names, uint32_t count);
/**
 * @brief 就近规划：扫最近的 GPX 点或途经点，以该点为目标向后重新规划。
 * @return 已提交/已对齐为 true；无定位、已到终点或无法规划为 false。
 */
bool map_page_nav_plan_nearest(map_page_t * page);
/**
 * @brief 停止导航并清路线叠层。
 */
void map_page_nav_stop(map_page_t * page);
#endif

/**
 * @brief Startup 进度页内准备地图文件，不解码或绘制瓦片。
 * @param page LiveMap 实例；NULL 则先 `boot_begin_load`。
 * @return 文件准备流程已经结束为 true；资源尚不可用时返回 false。
 * @details 加载 catalog 和启动坐标 region pack。Canvas 只清零并隐藏，
 *          首帧在 LiveMap 淡入完成后开始。
 */
bool map_page_boot_prepare(map_page_t * page);

/**
 * @brief 延迟 `map_page_create` 到 Startup 白屏之后（boot 序列）。
 * @param deferred true：先进空壳；false：create 时立刻 set_view。
 */
void map_page_set_boot_deferred(bool deferred);

/**
 * @brief 在已进栈的 LiveMap 空壳上创建 vmap（logo 变白后调用）。
 * @return 已有实例或创建成功为 true。
 * @note 创建后 `covered=true`，所有 LiveMap timer 保持暂停。
 */
bool map_page_boot_begin_load(void);

/**
 * @brief 是否仍处于 boot 延迟创建（空壳、尚未 `begin_load` 收尾）。
 * @return `map_page_set_boot_deferred(true)` 且尚未完成 begin_load 时为 true。
 */
bool map_page_boot_deferred(void);

/** @brief 开始/停止录制：开 REC 时创建 `/mnt/lfs/mtp/record/TRK_*.gpx`。 */
void map_page_set_recording(map_page_t * page, bool enable);
/** @brief 续录：把旧轨迹点画进 REC 线（须已 recording）。 */
void map_page_seed_track(map_page_t * page, const float * lon,
                         const float * lat, uint16_t n);
/** @brief 暂停本次骑行（计时停止，轨迹与 GPX 文件保留）。 */
void map_page_pause_ride(map_page_t * page);
/** @brief 从暂停继续本次骑行。 */
void map_page_resume_ride(map_page_t * page);
/** @brief 结束骑行。未 commit 的 GPX 会删除。 */
void map_page_end_ride(map_page_t * page);
/** @brief 若正在分片重绘，确保 render timer 在跑（结束骑行擦轨迹后调用）。 */
void map_page_continue_render(map_page_t * page);

/** @brief LiveMap 内容根（Helm chrome 挂在这上面）。 */
lv_obj_t * map_page_root(map_page_t * page);

/**
 * @brief 显示或隐藏地图区（Helm 翻到数据页时隐藏，画布仍保留）。
 * @param page    LiveMap。
 * @param visible true 露出 canvas。第一次从 false→true 会记下 `map_ui_seen`，
 *                此后隐藏页才允许 GNSS 跟车 rebase。
 */
void map_page_set_map_ui_visible(map_page_t * page, bool visible);
/** @brief 正在导航（有路线且未到达）。 */
bool map_page_nav_active(const map_page_t * page);
/** @brief 路线规划 worker 尚未完成。 */
bool map_page_nav_planning(const map_page_t * page);
/** @brief MCU 正在执行偏航重规划。 */
bool map_page_nav_replanning(const map_page_t * page);
/** @brief 轨迹回放 / 审阅模式。 */
bool map_page_review_active(const map_page_t * page);
/** @brief 整趟导航已到达终点（多站行程只在最后一站成立，中间站不算）。 */
bool map_page_nav_arrived(const map_page_t * page);
/**
 * @brief 多站行程进度：当前站序号（1 起）与总站数。
 * @return 是多站行程（总站数 >= 2）时为 true；单站导航没有「第几站」可言。
 */
bool map_page_nav_trip_progress(const map_page_t * page, uint32_t * idx,
    uint32_t * total);
/**
 * @brief 下一转向：图标、距离、辅文案。
 * @return 有转向信息为 true。
 */
bool map_page_nav_turn_info(const map_page_t * page,
                            char * ico, size_t ico_n,
                            char * dist, size_t dist_n,
                            char * sub, size_t sub_n);
/** @brief 导航中顶栏右侧文案（途经 1/3 或 GPX 名）；无导航时 false。 */
bool map_page_nav_status_text(const map_page_t * page, char * buf, size_t n);
/** @brief 横幅「偏航」阈值（米）。返航 GPX 放宽，避免对向车道误报。 */
double map_page_nav_off_warn_m(const map_page_t * page);
/**
 * @brief 到下一个转向点的距离（米）。
 * @return 无转向信息（纯轨迹跟随、或前方无路口）时为 -1。
 * @note  横幅"临近路口才悬挂"用它判阈值；与 `map_page_nav_turn_info()` 的距离文案同源。
 */
double map_page_nav_turn_dist_m(const map_page_t * page);
/**
 * @brief 剩余里程（米，含到终点的 display 口径）。
 * @return 无导航（无路线 / 已结束）时为 -1。
 * @note  顶栏右槽"距下一路口 / 剩余里程"用它 —— 横幅按显示策略大部分时间不挂，
 *        这里补上那个空档的读数。
 */
double map_page_nav_remain_m(const map_page_t * page);
/**
 * @brief 本次导航的总里程（米，GPX 跟线的全长）。
 * @return 不是 GPX 跟线、或长度未知时为 -1。
 * @note  左下角"剩余 / 全程"两行里的全程用它。
 */
double map_page_nav_total_m(const map_page_t * page);
/**
 * @brief 地图当前每像素多少米（按 Web Mercator：156543.03·cos(纬度)/2^zoom/scale）。
 * @return 地图不可用时为 0。
 * @note  右下角比例尺用它乘以标尺像素宽度，再取整成 1/2/5×10^k。
 */
double map_page_map_mpp(const map_page_t * page);
/**
 * @brief 顶部为常驻 UI（导航横幅）预留 px 高度：地图可见窗口下移并缩短 px，
 *        同时把视图对象上移 px/2，使**跟随中心落在可见窗口的中心**。
 * @param px 预留高度（0 = 铺满，未导航时用）
 * @note  运行期改几何：容器移动/缩放是廉价的，视图本身尺寸不变（只挪窗口）。
 */
void map_page_set_banner_inset(map_page_t * page, int32_t px);
/** @brief 打开 GPX：一次只加载前方 N km，接近窗口末端再动态续载。reverse 为反向。 */
bool map_page_nav_from_gpx(map_page_t * page, const char * path, bool reverse);
/** @brief 在地图上显示 GPX 轨迹（不导航、不模拟定位）。 */
bool map_page_show_gpx(map_page_t * page, const char * path);
/** @brief 用 GPX 回放模拟 GNSS。path 为 NULL 则停止。reverse 为反向。 */
int map_page_gnss_sim(map_page_t * page, const char * path, bool reverse,
    float speed_kph, float skip_km);
#if VMAP_ROUTE_ENABLE
/** @brief 路网 DEM 采样海拔（米）；未知为 `INT16_MIN`。 */
int16_t map_page_sample_ele_m(const map_page_t * page, double lon, double lat);
/** @brief 当前路线剩余爬升（米）；未知为 -1。 */
int32_t map_page_nav_remain_gain_m(const map_page_t * page);
/** @brief 整条路线海拔剖面（按总里程均匀取样）。now_idx 为当前位置下标（可 NULL）。 */
uint32_t map_page_nav_profile_ele(const map_page_t * page, int16_t * out,
    uint32_t cap, uint32_t * now_idx);
/** @brief 导航进度戳：路线点数 / 总长厘米 / 剖面当前位置下标。无导航则 false。 */
bool map_page_nav_progress(const map_page_t * page, uint32_t * pts,
    uint32_t * total_cm, uint32_t * now_idx, uint32_t prof_n);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MAP_PAGE_H */
