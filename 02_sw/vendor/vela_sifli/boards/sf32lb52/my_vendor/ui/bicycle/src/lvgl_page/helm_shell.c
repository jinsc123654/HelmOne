/**
 * @file helm_shell.c
 * @brief 主界面：按 MCU 线稿排版（subbar / rotbar / 十字分割）。
 *
 * @note 时速 / 心率数字与走势按 18/32 km/h、120/150 bpm 三档跳色（低绿、中亮黄、高红），
 *       不做渐变。数据页拆成概览 / 心率 / 速度三屏，KEY 上滑切换。
 *       GPX 导航海拔剖面按 |坡度| 3%/8% 跳色，烘焙到 RGB565 画布。
 *       KEY 翻页骑行中先切内容，再让共享内容根做 8 px / 100 ms 轻微入场；
 *       不透明度交叉淡入、不 stagger 子控件，也不改地图 canvas 自身 pan，
 *       数字放到下一拍再刷，避免和切页抢同一帧。
 */

#include "bicycle_gpx_sim.h"
#include "helm_shell.h"
#include "bicycle_page_ids.h"
#include "lvgl_page.h"

#include "bicycle_runtime.h"
#include "bicycle_ride_gpx.h"
#include "companion_bridge.h"
#include "helm_font.h"
#include "helm_icon.h"
#include "helm_idle.h"
#include "helm_menu.h"
#include "helm_pwr.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "lv_pm_overlay.h"
#include "myvendor_devctl.h"
#include "myvendor_sys.h"
#include "myvendor_sound.h"
#include "Vendor/Board/lv_port/lv_port_buttons.h"
#include "vmap/vmap_alloc.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_format.h"
#include "lvgl/src/draw/lv_draw_buf.h"
#include "lvgl/src/draw/lv_draw_label.h"
#include "lvgl/src/widgets/canvas/lv_canvas.h"
#include "lvgl/src/draw/lv_draw_arc.h"
#include "lvgl/src/draw/lv_draw_line.h"
#include "lvgl/src/draw/lv_draw_rect.h"
#include "lvgl/src/draw/lv_draw_triangle.h"
#include "lvgl/src/misc/lv_math.h"
#include "lvgl/src/misc/lv_text.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include "lv_pm_gpu_anima.h"
#include "board_malloc.h"
#include <time.h>

/* 待机页信息卡刷新（定义在 helm_build_standby_card 之后）。 */
static void helm_standby_card_tick(const bicycle_runtime_t * rt);

#define HELM_ROT_MS  3000u
#define HELM_UI_MS   500u
#define HELM_AUTOPAUSE_MS  4000u
#define HELM_AUTORESUME_MS 1500u
#define HELM_AUTOPAUSE_HOLD_MS 60000u
#define HELM_HIST_N  60
#define HELM_HIST_PERIOD_MS  1000u
#define HELM_AVG_WIN_MS      30000u
#define HELM_AVG_N           64

/* 搜星扫描圈的重绘节流。
 *
 * lv_anim 的执行回调按 LVGL 全局动画周期（LV_DEF_REFR_PERIOD，约 16 ms）调用，
 * 而 hero 的 draw 回调每帧都要重建整圈折线、重画整条 track 弧、再画三个刻度
 * 文字和游标点——这些都是纯软件光栅化。搜星期间 dial_lock==1，扫描动画
 * LV_ANIM_REPEAT_INFINITE，本板又长期收不到定位（hear 只有个位数），于是这条
 * 路径变成常驻满帧负载。
 *
 * 一圈扫描要 1600 ms 单程，本不需要 60 fps。这里只节流"让 LVGL 重绘"这一步：
 * 相位仍在每个动画帧更新（代价近乎为零），重绘降到 HELM_HERO_SPIN_MS 一次。
 * 可视效果是同一个 ease_in_out 慢扫描，只是重绘帧率下降。
 */
/* 主界面切换：**动一帧就要重光栅化一整屏**（实测一次切换 176~413 ms、却只有 2~3 帧，
 * 也就是每帧 ~100~200 ms），所以"动画"在这块屏上买不到流畅 —— 反而把切换拖长了。
 * 置 0 = 直接 snap（复用现成的 helm_pane_snap，无动画）；想要过渡再置 1。 */
#ifndef HELM_VIEW_ANIM
#define HELM_VIEW_ANIM 0
#endif
/* 页面切换动效（实测口径 [vperf] ui page switch，耗时/帧数为板测）：
 *   0 = snap（当前默认，用户 2026-09-26 定）：无动画，只付"新页面画一次"。
 *   1 = 单屏平移：进屏滑 16px / 90 ms，出屏不动。实测 176~413 ms / 2~3 帧 ——
 *       平移让整屏失效 ⇒ 每帧重光栅化一整屏（约 100 ms/帧）。
 *   2 = pm fade（交叉淡入 220 ms）：**实测最差 458~906 ms**，每帧整屏 alpha 混合。
 *       pm 的 fade 是给"页面级"用的，搬到全屏 pane 上不成立。
 *   3 = pm 的 GPU zoom/transform：transform 图层 + EPIC 混合，但每帧仍要把内容渲进图层。
 *   4 = 左右推挤：两屏各拍一张位图后对推，每帧只剩两次贴图。**实测最好
 *       276~398 ms / 6 帧**；固定开销是切换前两次整屏快照（约 150~250 ms）。
 * 结论：这块屏上"整页动"必付整屏重绘的钱。想既好看又不卡，得先让页面有常驻位图 ——
 * vmap 已经这么做了（输出就在我们自己的 draw_buf 里，见 vmap_view.c:1032），
 * HUD 要是也享受同等待遇，切换才可能真正零成本。四个实验模式都保留在下面，改宏可复现。
 */
#ifndef HELM_PM_ZOOM_SCALE
#define HELM_PM_ZOOM_SCALE 225      /* ~88%，与 lv_pm_gpu_depth_anima 一致 */
#endif
#ifndef HELM_PM_ZOOM_MS
#define HELM_PM_ZOOM_MS 200u
#endif

#ifndef HELM_HERO_SPIN_MS
#define HELM_HERO_SPIN_MS 80u
#endif
#define HELM_SPARK_PTS 30
/** @brief 海拔剖面均匀采样点数（段数 = N−1）。 */
#define HELM_CLIMB_PROF_N 60
/** @brief 当前位置指示器（点阵倒三角）的宽与高，单位 px。尖端朝下指着剖面点。
 *         **必须与下面 `s_climb_mark_data` 的实际尺寸一致**（15×10）。 */
#define HELM_CLIMB_MARK_W 15
#define HELM_CLIMB_MARK_H 10
/** @brief 「经过海拔」剖面填充：图表最上沿的 alpha（往下线性淡到 0）。
 *         原实现是"每段三角 50 + 一块 20% 淡洗"，观感偏薄；改成整幅淡出后取 110。 */
#define HELM_CLIMB_FILL_TOP_OPA 110u
/** @brief 海拔剖面「平坦」档：|坡度|低于此百分数为绿。
 *  @note 2026-09-26 由 3 降到 **1**、陡档 8 降到 **4**：用户实测一条 20.3 km /
 *        起伏 11 m 的路线（最大 2.1%）。用他那条文件算过三组：
 *        3/8 → 绿 59、2/5 → 绿 58、**1/4 → 绿 52 / 黄 7**。
 *        1%/4% 也是码表常见的分档（1% 在这颗屏上是"能感觉到的缓坡"），
 *        真爬坡（≥4%）照样红。 */
#define HELM_CLIMB_GRADE_FLAT  1u
/** @brief 海拔剖面「陡」档：|坡度|大于等于此百分数为红；两档之间为黄。 */
#define HELM_CLIMB_GRADE_STEEP 4u
/** @brief 时速低档上限：低于此 km/h 为绿。 */
#define HELM_SPEED_ZONE_MID   18.0f
/** @brief 时速高档下限：大于等于此 km/h 为红；两档之间为亮黄。 */
#define HELM_SPEED_ZONE_HIGH  32.0f
/** @brief 心率低档上限：低于此 bpm 为绿。 */
#define HELM_HR_ZONE_MID      120u
/** @brief 心率高档下限：大于等于此 bpm 为红；两档之间为亮黄。 */
#define HELM_HR_ZONE_HIGH     150u
/**
 * @brief 主页独立心率页。
 * @details 0：翻页跳过该页（概览仍有均心率，底栏仍可轮到心率）。
 *          改回 1 即恢复 KEY 上滑的心率整页。
 */
#define HELM_CLIMB_REC_MS  (30u * 60u * 1000u)
#define HELM_CLIMB_REC_PERIOD_MS  (HELM_CLIMB_REC_MS / HELM_CLIMB_PROF_N)
/* 5 行骑行数据 + 底部三格 2 + 爬升页 2 = 9 根，留一格余量（顶满时后挂的会
 * 被 helm_spark_bind 静默丢弃，症状是"某一格永远没曲线"）。 */
#define HELM_SPARK_MAX 10
#define HELM_SPEED_HEAD_H  22
/**
 * @name 骑行页（X1 仪表台）几何
 * @brief 左栏时速锁死（速度 + 速度走势）；右栏固定数据行，**不轮转**。
 * @details 宽度是**从实测字宽反推**的，不是估数：num_56 画 "28.6" 实测 116px
 *          ⇒ 左栏 124（左右各 4px 边距）；剩下 240-6-124-6-6 = 98 给右栏，
 *          内宽 98-6-6 = 86 ≥ 色条 3 + 隙 6 + num_28 "52:18" 74 = 83。
 *          右栏数值因此只能用 num_28：num_32 的 "52:18" 是 86px，放不下。
 *          卡片高 = 页高 296 - pad 6*2 - 页头 22 - pad_row 4 = 258 ⇒ 4 行各 64，
 *          行内"标签 14 + 数值 30 = 44" + 行底 13px 走势条 = 57 ≤ 64。
 * @note 用户 2026-09-26 原话："右侧数据不需要轮转"——第 5 行轮转去掉，右边就是
 *       4 项固定（时间/里程/均速/均心率），与数据页的分工靠页集合区分。
 * @{
 */
#define HELM_RIDE_HERO_W   124
#define HELM_RIDE_ROWS     4
/** @brief 数据页条目数（2 列 x 5 行 = 10 项，用户 2026-09-26 给的清单：
 *  时间 / 里程 / 均速 / 极速 / 心率 / 均心率 / 最大心率 / 踏频 / 功率 / 爬升）。
 *  行高 = 卡内高 / 5，每行"标签 14 + 数值 30 = 44"⇒ 卡高 258 时行高 51 ✓。 */
#define HELM_ALL_N         10
/** @brief 海拔页条目数（2 列 x 3 行，用户 2026-09-26 给的图：
 *  海拔(m) / 坡度(%) / 爬升(m) / 下降(m) / 最高(m) / 最低(m)）。 */
#define HELM_CLIMB_N       6
/** @brief 最后一行 = 功率。**只有功率计绑定了才显示**（用户 2026-09-26：
 *         "只要有绑定，无论是否连上都是 5 栏"/"功率计没绑定就是 4 个数据"）。
 *         行高走 flex_grow 而不是百分比：隐藏这一行后上面几行自己长满卡片。
 * @note 右栏现在是 里程/均速/心率/功率 —— **时间**挪到左卡的小结块里了
 *       （用户 2026-09-26："给骑行时间还有一些综合数据"），同一页不重复出现。 */
#define HELM_RIDE_PWR_ROW  (HELM_RIDE_ROWS - 1)
/** @brief 左卡小结块里的"综合数据"项数（骑行时间之外的那几行）。
 *  现在是 爬升 / 坡度（用户 2026-10-05 把"极速"换成"坡度"）；
 *  宽度账：色条 3 + 隙 5 + 标签 24 + … + 值 58 + 单位 12
 *  ≤ 左卡内宽 104 ⇒ 标签最多 3 个汉字、值的单位最多 3 个 ASCII 字符。 */
#define HELM_RIDE_TRIP_N   2
/** @brief 小结块顶端相对左卡顶的偏移（量条下 12，原走势区的位置）。
 *  块高：头 18 + 时间 35 + 发丝线(57) + 2×30 = 122 ≤ 左卡 258 - 117 - 8。 */
#define HELM_RIDE_TRIP_Y   125
/**
 * @name 小结块"值 / 单位"的水平几何
 * @brief **宽度是从实测字宽反推的**：num_28 的 4 位数（"1234"）实测 67px ——
 *        爬升到 1000 m 以上就会有 4 位，盒子留 62 会截断。72 够 4 位。
 *        右边缘钉在 20+72 = 92，单位从 94 起；左卡内宽 100（12..112）⇒
 *        "m"（12px）收在 106 ✓；标签最多到 20+36 = 56，不与 92 撞。
 * @{
 */
#define HELM_RIDE_TRIP_VAL_X   20
#define HELM_RIDE_TRIP_VAL_W   72
/** @brief 综合项"单位"的左端（值右边缘 + 2），最宽 12px（"m"）。 */
#define HELM_RIDE_TRIP_UNIT_X  94
/** @} */
#define HELM_RIDE_RAIL_H   40
#define HELM_RIDE_SPARK_H  145
/** @name 行底告警阈值（"默认不上色，数据高了才半透红"）
 * @brief 用户 2026-09-26："均速 心率 功率 默认都不颜色（现在功率是有默认的矩形
 *        颜色的），只有各项数据高了才设置为半透的红色"。
 * @note 阈值是**经验值**，调这三个宏即可（km/h、bpm、W）。半透明强度见
 *       `helm_ride_alert()` 里的 LV_OPA_30（与 helm_palette.h 的用色规则一致：
 *       颜色只做形状与状态，整格淡底做告警）。
 * @{
 */
#define HELM_ALERT_SPEED_KPH  30.0f
#define HELM_ALERT_HR_BPM     150u
#define HELM_ALERT_PWR_W      250u
/** @} */

/** @brief 右栏每行走势图的高度：**0 = 占满整行（数字背后的背景层）**。
 * @note 用户 2026-09-26 定稿："均速 心率 功率，背景都是有一个 chart 的"。
 *       这三行正是 helm_hist_id 取得到历史序列的三项（均速→速度历史、心率→
 *       心率历史、功率→功率历史）；时间/里程没有独立历史，自然什么都不画。
 *       设成正数则退化成"行底一条细条"（5 行时行高 51，"标签14+数值30=44"
 *       已占满，细条只能和数字挤在一起）。 */
#define HELM_RIDE_ROW_SPARK_H  0
/** @} */
/* 海拔页剖面卡高度（用户 2026-09-26 的图）：整页 296 = pad 6*2 + 页头 22 +
 * pad_row 4 + 剖面卡 108 + pad_row 4 + 六格卡 146 ⇒ 正好 */
#define HELM_CLIMB_CARD_H  108
#define HELM_VIEW_MS  90
#define HELM_VIEW_PX  16
#define HELM_DATA_SLIDE_PX  24
#define HELM_RIDE_ENTER_MS  100
#define HELM_RIDE_ENTER_PX  8
/** 连按 KEY 翻页：上一刀未落地则丢这次，避免同一 LVGL 拍叠两页。 */
#define HELM_FLIP_GUARD_MS  ((uint32_t)HELM_RIDE_ENTER_MS)
/** 切页后下一拍再刷数字，先把新页画出来。 */
#define HELM_NUMS_DEFER_MS  16u
/** 菜单淡出 ~180ms、主页淡入 220ms：折线放到转场后再算。 */
#define HELM_POLY_DEFER_MS  240u
#define HELM_DIAL_SPAN     270
#define HELM_DIAL_MAX_KPH  60
#define HELM_DIAL_SWEEP    48
#define HELM_POLY_N        8
#define HELM_POLY_NX       120
#define HELM_POLY_NY       24

typedef enum {
    HELM_PAGE_STANDBY = 0, /**< 主页面（地图 + 信息卡）。 */
    HELM_PAGE_DATA,        /**< 骑行页：X1 仪表台。 */
    HELM_PAGE_DATA_ALL,    /**< 数据页：2x5 十项全量（用户 2026-09-26 定）。 */
    HELM_PAGE_MAP,         /**< 地图页。 */
    HELM_PAGE_TURN,
    HELM_PAGE_CLIMB,       /**< 海拔页。 */
} helm_page_id_t;

typedef struct {
    map_page_t * map;
    lv_obj_t * standby;
    lv_obj_t * data;
    lv_obj_t * climb;
    lv_obj_t * turn;
    lv_obj_t * subbar;
    lv_obj_t * sub_left;
    lv_obj_t * sub_mid;
    lv_obj_t * sub_right;
    lv_obj_t * rotbar;
    lv_obj_t * rot_cell[3];
    lv_obj_t * rot_lab[3];
    lv_obj_t * rot_val[3];
    lv_obj_t * nav_ban;
    lv_obj_t * nav_ban_badge;   /**< 转向图标徽章（实心圆角，颜色跟状态走）。 */
    lv_obj_t * nav_ban_ico;     /**< 徽章里的图标字（反白）。 */
    lv_obj_t * nav_ban_m;       /**< 主字：距离（墨色大字）。 */
    lv_obj_t * nav_ban_act;     /**< 次字：动作（灰小字，可为空）。 */
    lv_obj_t * nav_ban_rd;      /**< 次字：目的地（路牌放不下，已恒为空）。 */
    lv_obj_t * nav_sum;         /**< 左下角两行：导航剩余 / 全程。 */
    lv_obj_t * nav_sum_l1;
    lv_obj_t * nav_sum_l2;
    lv_obj_t * nav_scale;       /**< 右下角比例尺（文字 + 1px 下边框当标尺）。 */
    lv_obj_t * nav_scale_lab;
    lv_obj_t * speed_big;
    lv_obj_t * speed_unit;
    lv_obj_t * standby_hint;
    /* 待机页新信息卡（2026-09-26）。几何与预览器
     * zcode/analysis/helm_preview/standby.py 的 compose="tight" 一一对应。 */
    lv_obj_t * sb_card;      /**< 信息卡本体：**挂在 LiveMap 的 root 上**（不是待机
                              *   pane 上）——用户 2026-09-26："把这个下半部分的组件
                              *   放到 livemap 界面，然后把 livemap 界面作为主界面"。
                              *   挂 root 还顺带修掉坐标偏差：原来挂在 pane（pad 6）
                              *   下，位置会被那 6px 内边距顶偏。 */
    lv_obj_t * sb_date;
    lv_obj_t * sb_time;
    lv_obj_t * sb_st[3];     /**< 状态列：卫星 / 定位 / 上次距离 */
    lv_obj_t * sb_ring;      /**< 浮动提示环（按下） */
    uint16_t sb_hm;          /**< 上次刷过的 HHMM，避免每分钟重复设同一文本 */
    lv_obj_t * hero;
    lv_obj_t * hero_mode;
    lv_obj_t * gps_val;
    lv_obj_t * last_dist;
    lv_obj_t * data_all;      /**< 数据页 pane（2x5 十项）。 */
    lv_obj_t * climb_lab[HELM_CLIMB_N];  /**< 海拔页六格（沿用数据页那套格子）。 */
    lv_obj_t * climb_val[HELM_CLIMB_N];
    lv_obj_t * climb_rail[HELM_CLIMB_N];
    lv_obj_t * all_lab[HELM_ALL_N];
    lv_obj_t * all_val[HELM_ALL_N];
    lv_obj_t * all_rail[HELM_ALL_N];
    lv_obj_t * data_speed;
    lv_obj_t * data_unit;
    lv_obj_t * data_mark;
    lv_obj_t * data_title;
    lv_obj_t * data_hero_key;
    /** @brief 左栏时速量条填充（宽度每拍按当前时速改）。 */
    lv_obj_t * ride_rail;
    lv_obj_t * ride_trip_ico;  /**< 小结块的时钟字形（helm_fa_16）。 */
    lv_obj_t * ride_trip_lab;  /**< 小结块头行"时间"。 */
    lv_obj_t * ride_trip_val;  /**< 骑行时间（num_32）。 */
    lv_obj_t * ride_trip_k[HELM_RIDE_TRIP_N];   /**< 综合项标签。 */
    lv_obj_t * ride_trip_v[HELM_RIDE_TRIP_N];   /**< 综合项数值（num_28）。 */
    lv_obj_t * ride_trip_u[HELM_RIDE_TRIP_N];   /**< 综合项单位。 */
    lv_obj_t * ride_trip_rail[HELM_RIDE_TRIP_N];/**< 综合项左缘 3px 色条。 */
    lv_obj_t * cell_lab[HELM_RIDE_ROWS];
    lv_obj_t * cell_val[HELM_RIDE_ROWS];
    lv_obj_t * cell_box[HELM_RIDE_ROWS];
    /** @brief 每行左缘 3px 色调色条（身份靠形状，不靠小字上色）。 */
    lv_obj_t * cell_rail[HELM_RIDE_ROWS];
    lv_obj_t * turn_ico;
    lv_obj_t * turn_dist;
    lv_obj_t * turn_sub;
    lv_obj_t * climb_mark;
    lv_obj_t * climb_chart;  /**< 剖面坐标源；导航烘焙成功后隐藏。 */
    lv_obj_t * climb_canvas; /**< 导航剖面 RGB565 烘焙图；切页只 blit。 */
    lv_obj_t * climb_cursor; /**< 烘焙图上的当前位置竖线（2 px）。 */
    lv_obj_t * climb_title;
    lv_chart_series_t * climb_ser;
    uint8_t climb_n;
    uint8_t climb_now;
    bool climb_nav;
    lv_obj_t * pause_dock;
    lv_obj_t * pause_title;
    lv_obj_t * pause_time;
    lv_obj_t * pause_dist;
    lv_obj_t * pause_avg;
    lv_obj_t * pause_hr;
    lv_obj_t * save_mask;
    lv_obj_t * save_time;
    lv_obj_t * save_dist;
    lv_obj_t * save_avg;
    lv_obj_t * save_gain;
    lv_obj_t * save_max;
    lv_obj_t * save_hr;
    lv_obj_t * arrive_dock;
    const lv_font_t * font_lab;
    const lv_font_t * font_title;
    const lv_font_t * font_val;
    const lv_font_t * font_quad;
    const lv_font_t * font_rot;
    const lv_font_t * font_speed;
    const lv_font_t * font_speed_ride;
    const lv_font_t * font_mark;
    lv_timer_t * rot_timer;
    lv_timer_t * ui_timer;
    lv_timer_t * poly_timer;
    lv_timer_t * nums_timer;
    uint32_t flip_t0;
    uint32_t flip_ms;
    uint8_t home_idx; /* idle：待机 / 概览 /（心率）/ 速度 / 地图 */
    uint8_t ride_idx; /* ride：概览 /（心率）/ 速度 / 地图 / 转向或爬升 */
    uint8_t rot_idx;
    bool paused;
    bool pause_auto;
    bool attached;
    bool ui_covered;
    bool arrive_dismissed;
    bool view_anim;
    int8_t view_dir;
    helm_page_id_t view_id;
    float last_km;
    uint16_t dial_kph10;
    uint16_t dial_phase;
    uint8_t dial_lock; /* 0 无数据 / 1 搜星 / 2 已定位 */
    bool dial_spin;
    bool dial_poly_defer; /* 菜单返回时先画数字，折线延后 */
} helm_shell_t;

static helm_shell_t s_sh;
static uint32_t s_still_since;
static uint32_t s_move_since;
static uint32_t s_autopause_hold_since;
/** 搜星扫描圈上次真正触发重绘的时刻（见 HELM_HERO_SPIN_MS）。 */
static uint32_t s_hero_spin_redraw_ms;
static int16_t s_rec_ele[HELM_CLIMB_PROF_N];
static uint8_t s_rec_ele_n;
static uint32_t s_rec_ele_slot = 0xffffffffu;
static int16_t s_climb_eles[HELM_CLIMB_PROF_N];
/**
 * @brief GPX 导航海拔剖面每段坡度档（仅换路线时重算）。
 * @details 下标 i 对应采样点 i→i+1：0 平坦（绿）、1 中等（黄）、2 陡（红）。
 *          已骑过路段在绘制时改灰，不改写本表。
 */
static uint8_t s_climb_zone[HELM_CLIMB_PROF_N];
/**
 * @brief 导航剖面画布像素缓冲（PSRAM，`vmap_malloc`）。
 * @details 宽高随图表变化；分配失败则回退 `helm_climb_draw_task` 现场绘制。
 */
static void * s_climb_cbuf;
/** @brief `s_climb_cbuf` 当前宽（像素）。 */
static int32_t s_climb_cw;
/** @brief `s_climb_cbuf` 当前高（像素）。 */
static int32_t s_climb_ch;
static uint32_t s_climb_fp_pts = 0xffffffffu;
static uint32_t s_climb_fp_total_cm = 0xffffffffu;
static uint8_t s_climb_fp_rec_n = 0xff;
static uint32_t s_climb_fp_rec_slot = 0xffffffffu;
/** @brief 换主题后"只重烘焙既有剖面数据"的待办（见 helm_shell_theme_mark 的说明）。 */
static bool s_climb_rebake;

typedef enum {
    HELM_HIST_NONE = 0,
    HELM_HIST_SPEED,
    HELM_HIST_HR,
    HELM_HIST_CAD,
    HELM_HIST_PWR,
    HELM_HIST_ALT,
    HELM_HIST_GRADE,
    HELM_HIST_GAIN,
    HELM_HIST_LOSS,
    HELM_HIST_TIME,
    HELM_HIST_DIST,
    HELM_HIST_COUNT
} helm_hist_id_t;

typedef struct {
    lv_obj_t * obj;
    lv_obj_t * lab;
    lv_chart_series_t * ser;
    helm_hist_id_t id;
    uint32_t tone; /**< 上次描边色；时速/心率换档时据此标脏，避免整图重采样。 */
} helm_spark_t;

static float (*s_hist)[HELM_HIST_N];
static uint8_t s_hist_n;
static uint32_t s_hist_min = 0xffffffffu;
static double s_avg_dist[HELM_AVG_N];
static uint32_t s_avg_tick[HELM_AVG_N];
static uint8_t s_avg_n;
static uint8_t s_avg_i;
static helm_spark_t s_spark[HELM_SPARK_MAX];
static uint8_t s_spark_n;

static void helm_apply_view(void);
static void helm_refresh_numbers(void);
static void helm_spark_sync(bool push);
static void helm_nums_defer_begin(void);
static void helm_nums_defer_cancel(void);
static void helm_prompt_save(void);
static bool helm_save_open(void);
static void helm_save_hide(void);
/** @brief 路线指纹变化时重载海拔剖面、坡度档，导航成功则烘焙画布。 */
static void helm_climb_profile_reload(void);
static bool helm_nav_on(void);
static bool helm_riding(void);
static void helm_hero_spin_set(bool on);
/* 骑行页行底告警：定义在 helm_ride_row 那一带，但 helm_refresh_numbers 先用。 */
static bool helm_ride_alert_on(const char * lab, const bicycle_runtime_t * rt);
static void helm_ride_alert(lv_obj_t * row, bool on);
/* 数据页十项取数：定义在 helm_build_data_all 那一带，refresh 也先用。 */
static void helm_fill_all(const bicycle_runtime_t * rt, const char ** lab,
                          char val[][16]);

static void helm_show(lv_obj_t * obj, bool on)
{
    if (obj == NULL) {
        return;
    }

    if (on) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(obj);
        }
    } else if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool helm_obj_shown(const lv_obj_t * obj)
{
    return obj != NULL && !lv_obj_has_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
}

static void helm_raise_if_shown(lv_obj_t * obj)
{
    lv_obj_t * parent;

    if (!helm_obj_shown(obj)) {
        return;
    }

    parent = lv_obj_get_parent(obj);
    if (parent == NULL) {
        return;
    }

    if (lv_obj_get_index(obj)
        != (int32_t)lv_obj_get_child_count(parent) - 1) {
        lv_obj_move_foreground(obj);
    }
}

static lv_obj_t * helm_pane_of(helm_page_id_t id)
{
    switch (id) {
    case HELM_PAGE_STANDBY:
        return s_sh.standby;
    case HELM_PAGE_DATA:
        return s_sh.data;
    case HELM_PAGE_DATA_ALL:
        return s_sh.data_all;
    case HELM_PAGE_TURN:
        return s_sh.turn;
    case HELM_PAGE_CLIMB:
        return s_sh.climb;
    default:
        return NULL;
    }
}

static bool helm_page_is_data(helm_page_id_t id)
{
    return id == HELM_PAGE_DATA || id == HELM_PAGE_DATA_ALL;
}

static void helm_pane_hide(lv_obj_t * obj);
static void helm_sw_report(void);
#if HELM_VIEW_ANIM == 4
static bool helm_pane_push(lv_obj_t * out, lv_obj_t * in, int dir);
#endif
#if HELM_VIEW_ANIM == 2
static bool helm_pane_pm_fade(lv_obj_t * out, lv_obj_t * in);
#endif
#if HELM_VIEW_ANIM == 3
static bool helm_pane_pm_zoom(lv_obj_t * out, lv_obj_t * in);
#endif
static void helm_pane_y(void * var, int32_t v);
static void helm_pane_reset(lv_obj_t * obj);
static lv_obj_t * helm_pane_anim_able(lv_obj_t * obj);

/* 打桩：主界面切换的时长与动画帧数（[vperf] ui page switch）。
 * 背景：现在两边一起动 —— 出屏 -16px、进屏 +16px、90 ms；而"位移"会让整屏失效，
 * 于是每一帧要把**两整屏**重新光栅化（各 240x320），90 ms 的动画实际要烧十几帧的工作量。
 * 先量出来再决定：只动进屏、或干脆 snap（helm_pane_snap 已存在）。 */
static uint32_t s_sw_t0;
static uint32_t s_sw_frames;
static lv_obj_t * s_sw_out;
static void helm_pane_y(void * var, int32_t v)
{
    s_sw_frames++;
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

static void helm_flip_arm(uint32_t ms)
{
    s_sh.flip_t0 = lv_tick_get();
    if (s_sh.flip_t0 == 0u) {
        s_sh.flip_t0 = 1u;
    }
    s_sh.flip_ms = ms;
}

static bool helm_flip_busy(void)
{
    return s_sh.flip_ms != 0u && lv_tick_elaps(s_sh.flip_t0) < s_sh.flip_ms;
}

static void helm_pane_hide_ready(lv_anim_t * a)
{
    lv_obj_t * obj = (lv_obj_t *)a->var;

    if (obj == NULL) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
}

static void helm_pane_in_ready(lv_anim_t * a)
{
    LV_UNUSED(a);
    if (s_sh.view_id != HELM_PAGE_MAP && s_sh.map) {
        map_page_set_map_ui_visible(s_sh.map, false);
    }

    /* 单屏动画：进屏停稳之后再收出屏（它整段没动过 ⇒ 一帧都没重画）。 */
    if (s_sw_out != NULL) {
        helm_pane_hide(s_sw_out);
        s_sw_out = NULL;
    }

    helm_sw_report();
}

/** @brief 切换打桩：只在慢的时候打一行（含动画帧数）。 */
static void helm_sw_report(void)
{
    if (s_sw_t0 != 0u) {
        uint32_t dt = lv_tick_elaps(s_sw_t0);

        if (dt >= 60u) {
            syslog(LOG_WARNING, "[vperf] ui page switch %ums frames=%u mode=%d\n",
                   (unsigned)dt, (unsigned)s_sw_frames, (int)HELM_VIEW_ANIM);
        }
    }
}


static void helm_pane_reset(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_anim_delete(obj, NULL);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
    helm_obj_stagger_clear(obj);
}

static void helm_pane_hide(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
}

/**
 * @brief 主页面（STANDBY）没有"自己的一页"：它的可见内容 = 地图 + root 上的信息卡。
 *
 * @details 那个 pane 是**旧版整页的不透明底**（内容早被 `helm_build_standby_card()`
 *          取代，只剩一块 SCR 色满屏底）。翻页动画会把"进入的那一页" unhide 并提到
 *          最前 ⇒ 从数据页翻回主页面时它就盖在地图上，现象就是"**切出去再回来
 *          livemap 失效**"。所以它**永远不参与进出场**：离开时不用动画一个看不见的
 *          东西，进入时干脆不 unhide —— 主页面靠下面的地图和卡片直接露出来。
 * @return 参与动画的对象（待机 pane 一律换成 NULL）。
 */
static lv_obj_t * helm_pane_anim_able(lv_obj_t * obj)
{
    return (obj == s_sh.standby) ? NULL : obj;
}

static void helm_pane_snap(lv_obj_t * out, lv_obj_t * in)
{
    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    helm_pane_reset(out);
    helm_pane_reset(in);
    if (out && out != in) {
        helm_pane_hide(out);
    }
    if (in) {
        lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(in);
        lv_obj_set_style_opa(in, LV_OPA_COVER, 0);
        lv_obj_set_style_translate_y(in, 0, 0);
    }
}

#if HELM_VIEW_ANIM == 4
#ifndef HELM_PUSH_MS
#define HELM_PUSH_MS 220u
#endif

static lv_draw_buf_t * s_push_buf[2];
static void * s_push_psram[2];
static lv_obj_t * s_push_layer[2];
static lv_obj_t * s_push_out;
static lv_obj_t * s_push_in;

/** @brief 取一块整屏位图（默认分配器优先，失败落到 PSRAM 池）。 */
static lv_draw_buf_t * helm_push_buf(int i)
{
    uint32_t stride;
    void * px;
    lv_draw_buf_t * b;

    if (s_push_buf[i] != NULL) {
        return s_push_buf[i];
    }

    b = lv_draw_buf_create(240, 320, LV_COLOR_FORMAT_RGB565, 0);
    if (b != NULL) {
        s_push_buf[i] = b;
        return b;
    }

    stride = lv_draw_buf_width_to_stride(240, LV_COLOR_FORMAT_RGB565);
    px = board_malloc_psram((size_t)stride * 320u);
    if (px == NULL) {
        return NULL;
    }

    b = lv_malloc(sizeof(lv_draw_buf_t));
    if (b == NULL) {
        board_free_psram(px);
        return NULL;
    }

    lv_draw_buf_init(b, 240, 320, LV_COLOR_FORMAT_RGB565, stride, px,
                     stride * 320u);
    s_push_buf[i] = b;
    s_push_psram[i] = px;
    syslog(LOG_WARNING, "[vperf] ui push buf%d: PSRAM 兜底 %p\n", i, px);
    return b;
}

static lv_obj_t * helm_push_layer(int i)
{
    lv_draw_buf_t * b = helm_push_buf(i);

    if (s_push_layer[i] != NULL || b == NULL) {
        return s_push_layer[i];
    }

    s_push_layer[i] = lv_canvas_create(lv_layer_top());
    if (s_push_layer[i] == NULL) {
        return NULL;
    }

    lv_canvas_set_draw_buf(s_push_layer[i], b);
    lv_obj_set_style_pad_all(s_push_layer[i], 0, 0);
    lv_obj_set_style_border_width(s_push_layer[i], 0, 0);
    lv_obj_set_style_bg_opa(s_push_layer[i], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_push_layer[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_push_layer[i], LV_OBJ_FLAG_HIDDEN);
    return s_push_layer[i];
}

static void helm_push_ready(lv_anim_t * a)
{
    LV_UNUSED(a);

    for (int i = 0; i < 2; i++) {
        if (s_push_layer[i] != NULL) {
            lv_obj_add_flag(s_push_layer[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_translate_x(s_push_layer[i], 0, 0);
        }
    }

    s_push_out = NULL;
    s_push_in = NULL;

    if (s_sh.view_id != HELM_PAGE_MAP && s_sh.map) {
        map_page_set_map_ui_visible(s_sh.map, false);
    }

    helm_sw_report();
}

static void helm_push_x(void * var, int32_t v)
{
    s_sw_frames++;
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
}

/** @brief 左右推挤：两屏拍成位图后对推（每帧只有两次贴图）。 */
static bool helm_pane_push(lv_obj_t * out, lv_obj_t * in, int dir)
{
    lv_coord_t w = 240;
    lv_coord_t from = (dir >= 0) ? w : -w;
    lv_anim_t ax;

    if (helm_push_layer(0) == NULL || helm_push_layer(1) == NULL) {
        return false;
    }

    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    if (in == NULL || out == NULL || out == in) {
        return false;
    }

    lv_obj_update_layout(out);
    lv_obj_update_layout(in);
    if (lv_snapshot_take_to_draw_buf(out, LV_COLOR_FORMAT_RGB565, s_push_buf[0]) !=
            LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(in, LV_COLOR_FORMAT_RGB565, s_push_buf[1]) !=
            LV_RESULT_OK) {
        return false;
    }

    s_push_out = out;
    s_push_in = in;

    /* 真进屏先就位（两位图盖着它），出屏退场。 */
    helm_pane_reset(in);
    lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(in);
    helm_pane_reset(out);
    helm_pane_hide(out);

    /* 位图 A = 旧页（从 0 推到 -from），位图 B = 新页（从 +from 推到 0）。 */
    lv_obj_set_pos(s_push_layer[0], 0, 0);
    lv_obj_set_style_translate_x(s_push_layer[0], 0, 0);
    lv_obj_set_style_opa(s_push_layer[0], LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_push_layer[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_push_layer[0]);

    lv_obj_set_pos(s_push_layer[1], 0, 0);
    lv_obj_set_style_translate_x(s_push_layer[1], from, 0);
    lv_obj_set_style_opa(s_push_layer[1], LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_push_layer[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_push_layer[1]);

    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;

    lv_anim_init(&ax);
    lv_anim_set_var(&ax, s_push_layer[0]);
    lv_anim_set_values(&ax, 0, -from);
    lv_anim_set_time(&ax, HELM_PUSH_MS);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&ax, helm_push_x);
    lv_anim_set_ready_cb(&ax, helm_push_ready);
    lv_anim_start(&ax);

    lv_anim_init(&ax);
    lv_anim_set_var(&ax, s_push_layer[1]);
    lv_anim_set_values(&ax, from, 0);
    lv_anim_set_time(&ax, HELM_PUSH_MS);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&ax, helm_push_x);
    lv_anim_start(&ax);
    return true;
}
#endif /* HELM_VIEW_ANIM == 4 */

/** 待机翻页：只位移，不改 opa、不 stagger。 */
static void helm_pane_slide(lv_obj_t * out, lv_obj_t * in, int dir)
{
    lv_anim_t ay;
    lv_coord_t dy = (dir >= 0) ? HELM_VIEW_PX : -HELM_VIEW_PX;

#if HELM_VIEW_ANIM == 4
    if (helm_pane_push(out, in, dir)) {
        return;
    }

    helm_pane_snap(out, in);      /* 缓冲/快照失败就退回 snap */
    return;
#elif HELM_VIEW_ANIM == 2
    if (helm_pane_pm_fade(out, in)) {
        return;
    }

    helm_pane_snap(out, in);      /* 待机 pane 之类不参与动画时退回 snap */
    return;
#elif HELM_VIEW_ANIM == 3
    if (helm_pane_pm_zoom(out, in)) {
        return;
    }

    helm_pane_snap(out, in);
    return;
#elif HELM_VIEW_ANIM == 0
    /* 实测：一次切换 176~413 ms，而动画只跑了 2~3 帧 ⇒ 每帧 100~200 ms（整屏重绘）。
     * 帧数这么少是因为渲染跟不上 16 ms 的动画节拍，动画被"拖"成 200~400 ms。
     * snap 之后切换只剩一次新页面的绘制，观感是"立刻到位"，比慢慢挪 16px 更快。 */
    helm_pane_snap(out, in);
    return;
#else
    LV_UNUSED(dy);
#endif

    /* 待机 pane 不参与动画（它一显示就盖住地图，见 helm_pane_anim_able）。 */
    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    helm_pane_reset(out);
    helm_pane_reset(in);

    /* ⚠ 出屏**不参与动画**：它一帧都不重画（原来两边一起动 = 每帧整屏重画两次），
     * 由进屏从 +16px 处滑过来盖住它，等进屏停稳后再把它隐藏（见 helm_pane_in_ready）。 */
    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;
    s_sw_out = (out != NULL && out != in) ? out : NULL;
    if (s_sw_out == NULL && out != NULL && out == in) {
        helm_pane_reset(out);
    }

    if (in) {
        lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(in);
        lv_obj_set_style_translate_y(in, dy, 0);
        lv_obj_set_style_opa(in, LV_OPA_COVER, 0);

        lv_anim_init(&ay);
        lv_anim_set_var(&ay, in);
        lv_anim_set_values(&ay, dy, 0);
        lv_anim_set_time(&ay, HELM_VIEW_MS);
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&ay, helm_pane_y);
        lv_anim_set_ready_cb(&ay, helm_pane_in_ready);
        lv_anim_start(&ay);
    }
}

#if HELM_VIEW_ANIM == 3
static lv_obj_t * s_sw_pm_in;

static void helm_pane_pm_exec(void * var, int32_t p)
{
    lv_obj_t * o = (lv_obj_t *)var;
    int32_t scale = HELM_PM_ZOOM_SCALE +
                    (LV_SCALE_NONE - HELM_PM_ZOOM_SCALE) * p / 255;

    lv_obj_set_style_transform_scale(o, scale, 0);
    lv_obj_set_style_opa(o, (lv_opa_t)p, 0);
}

static void helm_pane_pm_ready(lv_anim_t * a)
{
    LV_UNUSED(a);

    /* 收尾：清掉 transform（pm 自带这个复位），再把出屏收掉。 */
    lv_pm_gpu_page_reset_transform(s_sw_pm_in);
    s_sw_pm_in = NULL;
    if (s_sw_out != NULL) {
        helm_pane_hide(s_sw_out);
        s_sw_out = NULL;
    }

    helm_sw_report();
}

/** @brief 按 lv_pm 的 GPU anima 做法切换：进屏 transform 缩放 + 淡入。 */
static bool helm_pane_pm_zoom(lv_obj_t * out, lv_obj_t * in)
{
    lv_anim_t ay;

    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    if (in == NULL) {
        return false;
    }

    helm_pane_reset(in);
    lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(in);
    lv_obj_set_style_transform_pivot_x(in, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(in, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(in, HELM_PM_ZOOM_SCALE, 0);
    lv_obj_set_style_opa(in, LV_OPA_TRANSP, 0);

    /* 出屏不参与动画（pm 那边是直接换页）：等进屏停稳再收掉。 */
    s_sw_out = (out != NULL && out != in) ? out : NULL;
    s_sw_pm_in = in;

    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, in);
    lv_anim_set_values(&ay, 0, 255);
    lv_anim_set_time(&ay, HELM_PM_ZOOM_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_pm_exec);
    lv_anim_set_ready_cb(&ay, helm_pane_pm_ready);
    lv_anim_start(&ay);
    return true;
}
#endif

#if HELM_VIEW_ANIM == 2
#ifndef HELM_PM_FADE_MS
#define HELM_PM_FADE_MS 220u          /* 与 bicycle_page_anima.c 的 MAP 行一致 */
#endif

static void helm_pane_fade_exec(void * var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void helm_pane_pm_fade_ready(lv_anim_t * a)
{
    LV_UNUSED(a);

    /* 出屏一直垫在下面参与交叉淡入，等进屏满不透明了再收掉。 */
    if (s_sw_out != NULL) {
        helm_pane_hide(s_sw_out);
        s_sw_out = NULL;
    }

    helm_sw_report();
}

/** @brief pm 风格 fade：进屏 0→255 交叉淡入，出屏不参与动画、收尾时收起。 */
static bool helm_pane_pm_fade(lv_obj_t * out, lv_obj_t * in)
{
    lv_anim_t ay;

    out = helm_pane_anim_able(out);
    in = helm_pane_anim_able(in);
    if (in == NULL) {
        return false;
    }

    helm_pane_reset(in);
    lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(in);
    lv_obj_set_style_opa(in, LV_OPA_TRANSP, 0);

    s_sw_out = (out != NULL && out != in) ? out : NULL;

    s_sw_t0 = lv_tick_get();
    if (s_sw_t0 == 0u) {
        s_sw_t0 = 1u;
    }

    s_sw_frames = 0;

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, in);
    lv_anim_set_values(&ay, 0, 255);
    lv_anim_set_time(&ay, HELM_PM_FADE_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_fade_exec);
    lv_anim_set_ready_cb(&ay, helm_pane_pm_fade_ready);
    lv_anim_start(&ay);
    return true;
}
#endif

static void helm_pane_nudge(lv_obj_t * obj, int dir)
{
    lv_anim_t ay;
    lv_coord_t dy = (dir >= 0) ? HELM_DATA_SLIDE_PX : -HELM_DATA_SLIDE_PX;

    if (obj == NULL) {
        return;
    }

    helm_pane_reset(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(obj);
    lv_obj_set_style_translate_y(obj, dy, 0);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, obj);
    lv_anim_set_values(&ay, dy, 0);
    lv_anim_set_time(&ay, HELM_VIEW_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_y);
    lv_anim_start(&ay);
}

/**
 * @brief 骑行页轻量入场：只动共享内容根 8 px，不碰地图 canvas 自身 pan。
 */
static void helm_riding_view_enter(int dir)
{
    lv_obj_t * root = map_page_root(s_sh.map);
    lv_anim_t ay;
    lv_coord_t dy = (dir >= 0) ? HELM_RIDE_ENTER_PX : -HELM_RIDE_ENTER_PX;

    if (root == NULL) {
        return;
    }

    lv_anim_delete(root, helm_pane_y);
    lv_obj_set_style_translate_y(root, dy, 0);
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, root);
    lv_anim_set_values(&ay, dy, 0);
    lv_anim_set_time(&ay, HELM_RIDE_ENTER_MS);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ay, helm_pane_y);
    lv_anim_start(&ay);
}

static void helm_raise_chrome(void)
{
    if (!s_sh.attached || s_sh.ui_covered) {
        return;
    }

    helm_raise_if_shown(s_sh.subbar);
    helm_raise_if_shown(s_sh.nav_ban);
    helm_raise_if_shown(s_sh.rotbar);
    helm_raise_if_shown(s_sh.pause_dock);
    helm_raise_if_shown(s_sh.arrive_dock);
    if (helm_save_open()) {
        lv_obj_move_foreground(s_sh.save_mask);
    }
}

static lv_obj_t * helm_overlay(lv_obj_t * parent, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t * o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    helm_style_scr(o);
    lv_obj_set_style_pad_all(o, HELM_INSET, 0);
    lv_obj_set_style_pad_row(o, HELM_GAP, 0);
    lv_obj_set_size(o, PAGE_HOR_RES, h);
    lv_obj_add_flag(o, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t * helm_lab(lv_obj_t * parent, const lv_font_t * font, uint32_t color,
                           const char * txt)
{
    lv_obj_t * l = lv_label_create(parent);

    if (font) {
        lv_obj_set_style_text_font(l, font, 0);
    }

    lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(l, helm_color(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t * helm_col(lv_obj_t * parent, lv_obj_t ** lab,
                            lv_obj_t ** val, const lv_font_t * vfont)
{
    lv_obj_t * col = lv_obj_create(parent);

    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    helm_style_card(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    *lab = helm_lab(col, s_sh.font_lab, HELM_COLOR_INK, "--");
    *val = helm_lab(col, vfont ? vfont : s_sh.font_val, HELM_COLOR_INK, "--");
    return col;
}

/** @brief 地图底栏三格：父宽已固定，flex 均分。 */
static lv_obj_t * helm_rot_col(lv_obj_t * parent, lv_obj_t ** lab,
                              lv_obj_t ** val, const lv_font_t * vfont)
{
    lv_obj_t * col = lv_obj_create(parent);

    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    helm_style_card(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_HIDDEN);

    *lab = helm_lab(col, s_sh.font_lab, HELM_COLOR_INK, "--");
    *val = helm_lab(col, vfont ? vfont : s_sh.font_val, HELM_COLOR_INK, "--");
    lv_obj_remove_flag(*lab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(*val, LV_OBJ_FLAG_HIDDEN);
    return col;
}

/**
 * @brief 速度文本（1 位小数；**≥100 km/h 只留整数**）。
 *
 * @note 用户 2026-09-27 定：超过 100 不显示小数。两条理由都成立：
 *       ① 定宽值盒（`HELM_RIDE_TRIP_VAL_W=72`）是按 num_28 的**4 字形**反推的（实测 67px），
 *          "100.5" 这种 5 字形 ≈ 82px 放不下 ⇒ 默认换行后第二行被行高切掉，
 *          现场看上去就是"超过 100 小数消失"；
 *       ② 100 km/h 以上的 0.1 km/h 本来也没有意义。
 *       门槛用**四舍五入后的值**判（`v >= 1000` ⇒ 显示 "100" 而不是 "100.0"）。
 */
static void helm_fmt_speed(char * buf, size_t n, float kph)
{
    int v = (int)(kph * 10.0f + 0.5f);

    if (v < 0) {
        v = 0;
    }

    if (v >= 1000) {
        lv_snprintf(buf, n, "%d", v / 10);
    } else {
        lv_snprintf(buf, n, "%d.%d", v / 10, v % 10);
    }
}

static void helm_fmt_time(char * buf, size_t n, uint64_t ms)
{
    unsigned s = (unsigned)(ms / 1000ull);
    unsigned min = s / 60u;
    unsigned sec = s % 60u;

    /* 未满 100 分钟：MM:SS；到顶后低位改分钟，HH:MM（100 min → 01:40）。 */
    if (min < 100u) {
        lv_snprintf(buf, n, "%02u:%02u", min, sec);
    } else {
        lv_snprintf(buf, n, "%02u:%02u", min / 60u, min % 60u);
    }
}

static void helm_fmt_hms(char * buf, size_t n, uint64_t ms)
{
    helm_fmt_time(buf, n, ms);
}

/**
 * @brief 里程文本（1 位小数；**≥100 km 只留整数**）。
 * @note 用户 2026-09-27 定，与 `helm_fmt_speed()` 同规则、同一个定宽盒约束
 *       （见那边的注释）。
 */
static void helm_fmt_km(char * buf, size_t n, float km)
{
    int d10 = (int)(km * 10.0f + 0.5f);

    if (d10 < 0) {
        d10 = 0;
    }

    if (d10 >= 1000) {
        lv_snprintf(buf, n, "%d", d10 / 10);
    } else {
        lv_snprintf(buf, n, "%d.%d", d10 / 10, d10 % 10);
    }
}

static float helm_session_avg_kph(const bicycle_runtime_t * rt)
{
    double hours;

    if (rt == NULL || rt->session_elapsed_ms < 1000) {
        return 0.0f;
    }

    hours = (double)rt->session_elapsed_ms / 3600000.0;
    return (float)((rt->session_distance_m / 1000.0) / hours);
}

static void helm_avg_sample(const bicycle_runtime_t * rt)
{
    s_avg_dist[s_avg_i] = rt ? rt->session_distance_m : 0.0;
    s_avg_tick[s_avg_i] = lv_tick_get();
    s_avg_i = (uint8_t)((s_avg_i + 1u) % HELM_AVG_N);
    if (s_avg_n < HELM_AVG_N) {
        s_avg_n++;
    }
}

/** @brief 最近 30 秒均速（这段位移 / 时间）。 */
static float helm_avg_kph(const bicycle_runtime_t * rt)
{
    uint32_t now;
    uint32_t dt;
    uint8_t i;
    uint8_t idx;
    uint8_t oldest;
    double dd;
    double hours;

    if (rt == NULL || s_avg_n < 2) {
        return 0.0f;
    }

    now = lv_tick_get();
    oldest = 0xff;
    for (i = 0; i < s_avg_n; i++) {
        idx = (uint8_t)((s_avg_i + HELM_AVG_N - s_avg_n + i) % HELM_AVG_N);
        dt = now - s_avg_tick[idx];
        if (dt > HELM_AVG_WIN_MS) {
            continue;
        }

        oldest = idx;
        break;
    }

    if (oldest == 0xff) {
        return 0.0f;
    }

    dt = now - s_avg_tick[oldest];
    if (dt < 1000u) {
        return 0.0f;
    }

    dd = rt->session_distance_m - s_avg_dist[oldest];
    if (dd < 0.0) {
        dd = 0.0;
    }

    hours = (double)dt / 3600000.0;
    return (float)((dd / 1000.0) / hours);
}

static const char * helm_dash(bool ok, char * buf, size_t n, const char * fmt,
                              int v)
{
    if (!ok) {
        return "--";
    }

    lv_snprintf(buf, n, fmt, v);
    return buf;
}

/**
 * @brief 按数据格标签给出默认数字色（静态主题色，不做实时跳色）。
 * @param lab 标签 UTF-8 文本。
 * @return RGB888 主题色。
 * @note 「当前时速 / 心率」的实时三档色走 `helm_live_speed_ink()` /
 *       `helm_live_hr_ink()`，不经过本函数。均速 / 极速仍用导航橙。
 */
static uint32_t helm_tone(const char * lab)
{
    if (lab == NULL) {
        return HELM_COLOR_INK;
    }

    if (strncmp(lab, "心率", 6) == 0 || strncmp(lab, "均心率", 9) == 0 ||
        strncmp(lab, "最大心率", 12) == 0) {
        return HELM_COLOR_HR;
    }

    if (strncmp(lab, "踏频", 6) == 0) {
        return HELM_COLOR_CAD;
    }

    if (strncmp(lab, "功率", 6) == 0) {
        return HELM_COLOR_PWR;
    }

    if (strncmp(lab, "海拔", 6) == 0 || strncmp(lab, "累计爬升", 12) == 0 ||
        strncmp(lab, "爬升", 6) == 0 || strncmp(lab, "坡度", 6) == 0 ||
        strncmp(lab, "下降", 6) == 0) {
        return HELM_COLOR_CLIMB;
    }

    if (strncmp(lab, "速度", 6) == 0 || strncmp(lab, "均速", 6) == 0 ||
        strncmp(lab, "极速", 6) == 0) {
        return HELM_COLOR_NAV;
    }

    return HELM_COLOR_INK;
}

/**
 * @brief 按数据格标签给出卡片底色 / 走势是否启用。
 * @param lab 标签 UTF-8 文本。
 * @return 填充色；`HELM_COLOR_PAPER` 表示无走势。
 */
static uint32_t helm_tone_fill(const char * lab)
{
    if (lab == NULL) {
        return HELM_COLOR_PAPER;
    }

    if (strncmp(lab, "心率", 6) == 0) {
        return HELM_COLOR_HR_FILL;
    }

    if (strncmp(lab, "均心率", 9) == 0 || strncmp(lab, "最大心率", 12) == 0) {
        return HELM_COLOR_PAPER;
    }

    if (strncmp(lab, "踏频", 6) == 0) {
        return HELM_COLOR_CAD_FILL;
    }

    if (strncmp(lab, "功率", 6) == 0) {
        return HELM_COLOR_PWR_FILL;
    }

    if (strncmp(lab, "海拔", 6) == 0 || strncmp(lab, "累计爬升", 12) == 0 ||
        strncmp(lab, "爬升", 6) == 0 || strncmp(lab, "坡度", 6) == 0 ||
        strncmp(lab, "下降", 6) == 0) {
        return HELM_COLOR_CLIMB_FILL;
    }

    if (strncmp(lab, "均速", 6) == 0 || strncmp(lab, "速度", 6) == 0) {
        return HELM_COLOR_NAV_FILL;
    }

    return HELM_COLOR_PAPER;
}

/**
 * @brief 时速 / 心率三档对应的数字描边色。
 * @param z 0 低（绿）、1 中（亮黄）、2 高（红）。
 * @return RGB888；绘制前再 `helm_color()`。
 */
static uint32_t helm_zone_ink(uint8_t z)
{
    if (z == 0u) {
        return HELM_COLOR_CAD;
    }

    if (z == 1u) {
        return HELM_COLOR_YEL;
    }

    return HELM_COLOR_HR;
}

/**
 * @brief 时速 / 心率三档对应的走势填充色。
 * @param z 0 低（绿）、1 中（亮黄）、2 高（红）。
 * @return RGB888 走势面积色（跳过 `helm_color()`）。
 */
static uint32_t helm_zone_spark(uint8_t z)
{
    if (z == 0u) {
        return HELM_COLOR_CAD_SPARK;
    }

    if (z == 1u) {
        return HELM_COLOR_YEL_SPARK;
    }

    return HELM_COLOR_HR_SPARK;
}

/**
 * @brief 把时速归入低 / 中 / 高三档（档内数字跳色）。
 * @param kph 当前时速（km/h）。
 * @return 0 低于 18；1 为 18–32；2 大于等于 32。
 */
static uint8_t helm_speed_zone(float kph)
{
    if (kph < HELM_SPEED_ZONE_MID) {
        return 0u;
    }

    if (kph < HELM_SPEED_ZONE_HIGH) {
        return 1u;
    }

    return 2u;
}

/**
 * @brief 把心率归入低 / 中 / 高三档（不做渐变）。
 * @param bpm 当前心率。
 * @return 0 低于 120；1 为 120–149；2 大于等于 150。
 */
static uint8_t helm_hr_zone(uint16_t bpm)
{
    if (bpm < HELM_HR_ZONE_MID) {
        return 0u;
    }

    if (bpm < HELM_HR_ZONE_HIGH) {
        return 1u;
    }

    return 2u;
}

/**
 * @brief 当前时速的三档数字色。
 * @return `helm_zone_ink()` 结果；无 runtime 时按 0 km/h（绿）。
 */
static uint32_t helm_live_speed_ink(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    return helm_zone_ink(helm_speed_zone(rt ? rt->speed_kph : 0.0f));
}

/**
 * @brief 当前心率的三档数字色。
 * @return 无效心率保持 `HELM_COLOR_HR`，不跳档。
 */
static uint32_t helm_live_hr_ink(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (rt == NULL || !rt->hr_valid) {
        return HELM_COLOR_HR;
    }

    return helm_zone_ink(helm_hr_zone(rt->hr_bpm));
}

static void helm_set_text_hex(lv_obj_t * obj, uint32_t hex)
{
    if (obj) {
        lv_obj_set_style_text_color(obj, helm_color(hex), 0);
    }
}

/**
 * @brief 把时速数字和单位刷成同一档实时色。
 * @param num 时速数字标签。
 * @param unit 单位标签（可为 NULL）。
 */
static void helm_paint_speed_pair(lv_obj_t * num, lv_obj_t * unit)
{
    uint32_t ink = helm_live_speed_ink();

    helm_set_text_hex(num, ink);
    helm_set_text_hex(unit, ink);
}

static bool helm_lab_eq(const char * lab, const char * key)
{
    return lab != NULL && key != NULL && strcmp(lab, key) == 0;
}

/** @brief 当前心率用实时跳色；均心按会话平均跳档；其余走主题色。 */
static uint32_t helm_metric_ink(const char * lab)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (helm_lab_eq(lab, "心率")) {
        return helm_live_hr_ink();
    }

    if (helm_lab_eq(lab, "均心率") && rt != NULL && rt->hr_avg_bpm > 0u) {
        return helm_zone_ink(helm_hr_zone(rt->hr_avg_bpm));
    }

    return helm_tone(lab);
}

/**
 * @brief 走势折线描边色。
 * @param id 历史序列种类。
 * @return RGB888。时速 / 心率跟当前档位走，其余用主题色。
 */
static uint32_t helm_spark_stroke_of(helm_hist_id_t id)
{
    switch (id) {
    case HELM_HIST_HR:
        return helm_live_hr_ink();
    case HELM_HIST_CAD:
        return HELM_COLOR_CAD;
    case HELM_HIST_PWR:
        return HELM_COLOR_PWR;
    case HELM_HIST_ALT:
    case HELM_HIST_GRADE:
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        return HELM_COLOR_CLIMB;
    case HELM_HIST_SPEED:
        return helm_live_speed_ink();
    case HELM_HIST_TIME:
    case HELM_HIST_DIST:
        return HELM_COLOR_NAV;
    default:
        return HELM_COLOR_INK;
    }
}

/**
 * @brief 走势面积填充色。
 * @param id 历史序列种类。
 * @return RGB888。时速 / 心率跟当前档位走；无效心率用 `HELM_COLOR_HR_SPARK`。
 */
static uint32_t helm_spark_fill_of(helm_hist_id_t id)
{
    uint8_t z;

    switch (id) {
    case HELM_HIST_HR: {
        const bicycle_runtime_t * rt = bicycle_runtime_get();

        if (rt == NULL || !rt->hr_valid) {
            return HELM_COLOR_HR_SPARK;
        }

        return helm_zone_spark(helm_hr_zone(rt->hr_bpm));
    }
    case HELM_HIST_CAD:
        return HELM_COLOR_CAD_SPARK;
    case HELM_HIST_PWR:
        return HELM_COLOR_PWR_SPARK;
    case HELM_HIST_ALT:
    case HELM_HIST_GRADE:
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        return HELM_COLOR_CLIMB_SPARK;
    case HELM_HIST_SPEED: {
        const bicycle_runtime_t * rt = bicycle_runtime_get();

        z = helm_speed_zone(rt ? rt->speed_kph : 0.0f);
        return helm_zone_spark(z);
    }
    case HELM_HIST_TIME:
    case HELM_HIST_DIST:
        return HELM_COLOR_NAV_SPARK;
    default:
        return HELM_COLOR_PAPER;
    }
}

static void helm_draw_seg(lv_layer_t * layer, int32_t x0, int32_t y0,
                          int32_t x1, int32_t y1, uint16_t w, lv_color_t color)
{
    lv_draw_line_dsc_t ld;

    lv_draw_line_dsc_init(&ld);
    ld.color = color;
    ld.width = w;
    ld.opa = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end = 1;
    ld.p1.x = x0;
    ld.p1.y = y0;
    ld.p2.x = x1;
    ld.p2.y = y1;
    lv_draw_line(layer, &ld);
}

static void helm_draw_pip(lv_layer_t * layer, int32_t x, int32_t y,
                          lv_coord_t pr, lv_color_t color)
{
    lv_draw_rect_dsc_t rd;
    lv_area_t a;

    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = color;
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 1;
    rd.border_color = helm_color(HELM_COLOR_INK);
    rd.border_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    a.x1 = (lv_coord_t)(x - pr);
    a.y1 = (lv_coord_t)(y - pr);
    a.x2 = (lv_coord_t)(x + pr);
    a.y2 = (lv_coord_t)(y + pr);
    lv_draw_rect(layer, &rd, &a);
}

static helm_hist_id_t helm_hist_id(const char * lab)
{
    if (lab == NULL) {
        return HELM_HIST_NONE;
    }

    if (strncmp(lab, "累计爬升", 12) == 0 || strncmp(lab, "爬升", 6) == 0) {
        return HELM_HIST_GAIN;
    }

    if (strncmp(lab, "极速", 6) == 0) {
        return HELM_HIST_NONE;
    }

    if (strncmp(lab, "均速", 6) == 0 || strncmp(lab, "速度", 6) == 0) {
        return HELM_HIST_SPEED;
    }

    if (strncmp(lab, "均心率", 9) == 0 || strncmp(lab, "最大心率", 12) == 0) {
        return HELM_HIST_NONE;
    }

    if (strncmp(lab, "心率", 6) == 0) {
        return HELM_HIST_HR;
    }

    if (strncmp(lab, "踏频", 6) == 0) {
        return HELM_HIST_CAD;
    }

    if (strncmp(lab, "功率", 6) == 0) {
        return HELM_HIST_PWR;
    }

    if (strncmp(lab, "海拔", 6) == 0) {
        return HELM_HIST_ALT;
    }

    if (strncmp(lab, "坡度", 6) == 0) {
        return HELM_HIST_GRADE;
    }

    /* 时间 / 里程只显示数字，不铺底走势。 */
    return HELM_HIST_NONE;
}

static void helm_hist_reset(void)
{
    s_hist_n = 0;
    s_hist_min = 0xffffffffu;
    s_avg_n = 0;
    s_avg_i = 0;
    s_rec_ele_n = 0;
    s_rec_ele_slot = 0xffffffffu;
    if (s_hist != NULL) {
        memset(s_hist, 0, sizeof(float) * (size_t)HELM_HIST_COUNT * HELM_HIST_N);
    }
    memset(s_rec_ele, 0, sizeof(s_rec_ele));
    s_climb_fp_pts = 0xffffffffu;
    s_climb_fp_total_cm = 0xffffffffu;
    s_climb_fp_rec_n = 0xff;
    s_climb_fp_rec_slot = 0xffffffffu;
}

static bool helm_hist_push(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    uint32_t minute;
    int i;

    if (s_hist == NULL || rt == NULL) {
        return false;
    }

    /* 采样节拍：**记录中**按会话时间（暂停/继续与里程计时一致），**没开录**按
     * 系统 tick。旧写法是"没 recording 就整支不采样"，结果骑行页在台子上
     * （没开录）翻到时，速度/心率/功率三根走势全都没有线——用户 2026-09-26
     * 实测："还是没有任何线条绘制出来"。走势是"最近一分钟的实时值"，和会话记不
     * 记录无关：待机翻页就能看到这一页，也该有线。
     * 开录/续录时会 helm_hist_reset()（会话起点从空图开始），所以底座切换不会串。 */
    if (rt->recording || s_sh.paused) {
        minute = (uint32_t)(rt->session_elapsed_ms / HELM_HIST_PERIOD_MS);
    } else {
        minute = lv_tick_get() / HELM_HIST_PERIOD_MS;
    }

    if (s_hist_n > 0 && minute == s_hist_min) {
        return false;
    }

    s_hist_min = minute;
    if (s_hist_n < HELM_HIST_N) {
        i = s_hist_n;
        s_hist_n++;
    } else {
        for (i = 0; i < HELM_HIST_COUNT; i++) {
            memmove(&s_hist[i][0], &s_hist[i][1],
                    (HELM_HIST_N - 1u) * sizeof(float));
        }

        i = HELM_HIST_N - 1;
    }

    s_hist[HELM_HIST_SPEED][i] = helm_avg_kph(rt);
    s_hist[HELM_HIST_HR][i] = (rt && rt->hr_valid) ? (float)rt->hr_bpm : 0.0f;
    s_hist[HELM_HIST_CAD][i] = (rt && rt->cadence_valid) ? (float)rt->cadence_rpm : 0.0f;
    s_hist[HELM_HIST_PWR][i] = (rt && rt->power_valid) ? (float)rt->power_w : 0.0f;
    s_hist[HELM_HIST_ALT][i] = rt ? rt->altitude_m : 0.0f;
    s_hist[HELM_HIST_GRADE][i] = rt ? rt->grade_pct : 0.0f;
    s_hist[HELM_HIST_GAIN][i] = rt ? rt->gain_m : 0.0f;
    s_hist[HELM_HIST_LOSS][i] = rt ? rt->loss_m : 0.0f;
    s_hist[HELM_HIST_TIME][i] = rt ? (float)(rt->session_elapsed_ms / 1000.0) : 0.0f;
    s_hist[HELM_HIST_DIST][i] = rt ? (float)rt->session_distance_m : 0.0f;
    return true;
}

static int32_t helm_spark_sample(helm_hist_id_t id, int i)
{
    float v;

    if (id <= HELM_HIST_NONE || id >= HELM_HIST_COUNT || s_hist == NULL) {
        return 0;
    }

    if (s_hist_n <= 0) {
        return 0;
    }

    if (i < 0) {
        i = 0;
    } else if (i >= s_hist_n) {
        i = s_hist_n - 1;
    }

    v = s_hist[id][i];
    switch (id) {
    case HELM_HIST_SPEED:
    case HELM_HIST_ALT:
    case HELM_HIST_GRADE:
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        return (int32_t)(v * 10.0f);
    default:
        return (int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
    }
}

static void helm_spark_axis(helm_hist_id_t id, int32_t * mn, int32_t * mx)
{
    switch (id) {
    case HELM_HIST_HR:
        *mn = 40;
        *mx = 220;
        break;
    case HELM_HIST_CAD:
        *mn = 0;
        *mx = 180;
        break;
    case HELM_HIST_PWR:
        *mn = 0;
        *mx = 500;
        break;
    case HELM_HIST_ALT:
        *mn = -50;
        *mx = 4000;
        break;
    case HELM_HIST_GRADE:
        *mn = -150;
        *mx = 200;
        break;
    case HELM_HIST_GAIN:
    case HELM_HIST_LOSS:
        *mn = 0;
        *mx = 5000;
        break;
    case HELM_HIST_SPEED:
        *mn = 0;
        *mx = 600;
        break;
    case HELM_HIST_TIME:
        *mn = 0;
        *mx = 3600;
        break;
    case HELM_HIST_DIST:
        *mn = 0;
        *mx = 20000;
        break;
    default:
        *mn = 0;
        *mx = 100;
        break;
    }
}

/**
 * @brief 图表上那串时速度点的平均值（骑行页"均速"显示用）。
 *
 * 用户 2026-10-05："均速和显示的图表点挂钩，这个均速就设计为图表点数的
 * 平均值"——骑行页那一行背景 chart 画的是最近一分钟的时速走势，而旁边的
 * 数字原来取的是"总里程 / 总时间"，两个数天然对不上。现在显示值 = chart
 * 上那 HELM_SPARK_PTS 个点的平均，取样方式与 `helm_spark_reload()` **逐字
 * 相同**（同样的下标映射、同样走 `helm_spark_sample()`），保证"数字"和
 * "图上看到的曲线"永远对得上。
 * @return km/h；还没有两个以上历史点时给 0。
 */
static float helm_spark_avg_kph(void)
{
    int32_t sum = 0;
    int i;

    if (s_hist_n < 2) {
        return 0.0f;
    }

    for (i = 0; i < HELM_SPARK_PTS; i++) {
        const int si = (i * (s_hist_n - 1)) / (HELM_SPARK_PTS - 1);

        sum += helm_spark_sample(HELM_HIST_SPEED, si);
    }

    /* helm_spark_sample 对时速给的是 ×10 定点。 */
    return (float)sum / ((float)HELM_SPARK_PTS * 10.0f);
}

/**
 * @brief 走势面积：按当前档位填色（时速 / 心率会跳档）。
 * @param e LV_EVENT_DRAW_TASK_ADDED。
 */
static void helm_spark_add_faded_area(lv_event_t * e)
{
    helm_spark_t * sp = lv_event_get_user_data(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc;
    lv_draw_line_dsc_t * draw_line_dsc;
    lv_color_t fill;
    lv_area_t coords;
    int32_t full_h;

    if (sp == NULL || obj == NULL || draw_task == NULL || sp->id == HELM_HIST_NONE) {
        return;
    }

    base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    draw_line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (base_dsc == NULL || base_dsc->layer == NULL ||
        draw_line_dsc == NULL || draw_line_dsc->points == NULL ||
        draw_line_dsc->point_cnt < 2) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    full_h = lv_obj_get_height(obj);
    if (full_h < 1) {
        return;
    }

    fill = helm_color_snap(helm_spark_fill_of(sp->id));

    /* 逐行填（板级第二版，2026-09-25）：每行画**纯色**矩形，alpha 取**同一条全局
     * 斜坡**（行内为常数），矩形的 x 区间 = 折线在该行"位于曲线上方"的连续段。
     *
     * 为什么换掉"逐段三角形 + 矩形"（上游示例 lv_example_chart_area_gradient 的写法）：
     * 那个写法有两类 1 px 竖线，且都跟曲线形状有关（所以"有时候"才出现）——
     *   · 峰顶：相邻两段的三角形竖边**落在同一列**、各投满一次色带 ⇒ 混合两次 ⇒
     *     偏深竖线（现场："有时候又竖向的一个小黑线"）；
     *   · 段边界：那一列两边的形状都不覆盖 ⇒ 缺口 ⇒ 偏浅竖线。
     * 逐行填里每行只按 x 区间划分，相邻行、相邻区间互不重叠 ⇒ 两类竖线都不存在。
     *
     * 顺带更省：本板 EPIC 加速单元**没有 TRIANGLE**（只有 FILL/IMAGE/BORDER/LABEL/
     * LAYER），三角形会回退到 SW 并逐行生成遮罩；改成纯色矩形后整片面积全部走
     * EPIC 的 FILL。代价是 H 行 × W 列一次整型扫描（约 55×200），亚毫秒级。 */
    {
        int32_t x_first = (int32_t)draw_line_dsc->points[0].x;
        int32_t x_last = (int32_t)draw_line_dsc->points[draw_line_dsc->point_cnt - 1].x;
        int32_t y;

        if (x_last <= x_first) {
            return;
        }

        for (y = coords.y1; y <= coords.y2; y++) {
            lv_draw_rect_dsc_t rect_dsc;
            lv_area_t rect_area;
            lv_opa_t opa = (lv_opa_t)(255 - (y - coords.y1) * 255 / full_h);
            uint32_t seg = 0;
            int32_t run = 0;
            bool in = false;
            int32_t x;

            if (opa < 8) {
                break;              /* 再淡肉眼已不可见：省掉剩下的绘制 */
            }

            lv_draw_rect_dsc_init(&rect_dsc);
            rect_dsc.bg_color = fill;
            rect_dsc.bg_opa = opa;
            rect_area.y1 = y;
            rect_area.y2 = y;

            for (x = x_first; x <= x_last; x++) {
                bool under = false;

                while (seg < draw_line_dsc->point_cnt - 2 &&
                       (int32_t)draw_line_dsc->points[seg + 1].x < x) {
                    seg++;
                }

                {
                    lv_point_precise_t a = draw_line_dsc->points[seg];
                    lv_point_precise_t b = draw_line_dsc->points[seg + 1];

                    /* 端点无效（NONE）或该段不向右延伸：这一列当作"曲线不在上方"、
                     * 不填 —— 与老写法"跳过无效段"的语义一致。 */
                    if (a.x != LV_DRAW_LINE_POINT_NONE &&
                        a.y != LV_DRAW_LINE_POINT_NONE &&
                        b.x != LV_DRAW_LINE_POINT_NONE &&
                        b.y != LV_DRAW_LINE_POINT_NONE &&
                        b.x > a.x) {
                        int32_t yc = (int32_t)a.y +
                                     (int32_t)((b.y - a.y) * (x - a.x) / (b.x - a.x));

                        under = (yc < y);
                    }
                }

                if (under && !in) {
                    run = x;
                    in = true;
                } else if (!under && in) {
                    rect_area.x1 = run;
                    rect_area.x2 = x - 1;
                    if (rect_area.x2 >= rect_area.x1) {
                        lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                    }
                    in = false;
                }
            }

            if (in) {
                rect_area.x1 = run;
                rect_area.x2 = x_last;
                if (rect_area.x2 >= rect_area.x1) {
                    lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                }
            }
        }
    }
}

/**
 * @brief 走势折线绘制前回调：把面积刷成当前档位色。
 * @param e LV_EVENT_DRAW_TASK_ADDED。
 */
static void helm_spark_draw_task(lv_event_t * e)
{
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc;

    if (draw_task == NULL) {
        return;
    }

    base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    if (base_dsc == NULL) {
        return;
    }

    if (base_dsc->part == LV_PART_ITEMS &&
        lv_draw_task_get_type(draw_task) == LV_DRAW_TASK_TYPE_LINE) {
        helm_spark_add_faded_area(e);
    }
}

/**
 * @brief 走势绘制后补描边：时速 / 心率用当前档位色。
 * @param e LV_EVENT_DRAW_POST。
 */
static void helm_spark_draw_post(lv_event_t * e)
{
    helm_spark_t * sp = lv_event_get_user_data(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t p0;
    lv_point_t p1;
    lv_color_t stroke;
    uint32_t n;

    if (sp == NULL || layer == NULL || sp->obj == NULL || sp->ser == NULL ||
        sp->id == HELM_HIST_NONE) {
        return;
    }

    if (lv_obj_has_flag(sp->obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    n = lv_chart_get_point_count(sp->obj);
    if (n < 2) {
        return;
    }

    lv_chart_get_point_pos_by_id(sp->obj, sp->ser, n - 2, &p0);
    lv_chart_get_point_pos_by_id(sp->obj, sp->ser, n - 1, &p1);
    lv_obj_get_coords(sp->obj, &coords);
    stroke = helm_color_snap(helm_spark_stroke_of(sp->id));
    helm_draw_seg(layer, coords.x1 + p0.x, coords.y1 + p0.y,
                  coords.x1 + p1.x, coords.y1 + p1.y, 4, stroke);
    helm_draw_pip(layer, coords.x1 + p1.x, coords.y1 + p1.y, 3, stroke);
}

/**
 * @brief 祖先链上没有 HIDDEN 时走势才参与刷新 / 绘制。
 */
static bool helm_spark_tree_shown(const lv_obj_t * obj)
{
    while (obj != NULL) {
        if (lv_obj_has_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
        obj = lv_obj_get_parent(obj);
    }
    return true;
}

/**
 * @brief 按标签绑定走势图并写入当前历史点。
 * @param sp 走势槽。
 * @param id 历史序列；`HELM_HIST_NONE` 则隐藏。
 * @details 记下 `tone`，供 `helm_spark_sync()` 检测时速 / 心率换档。
 */
static void helm_spark_reload(helm_spark_t * sp, helm_hist_id_t id)
{
    int32_t mn;
    int32_t mx;
    int i;
    int n;

    sp->id = id;
    if (sp->obj == NULL || sp->ser == NULL) {
        return;
    }

    if (id == HELM_HIST_NONE || s_hist_n < 2
        || !helm_spark_tree_shown(lv_obj_get_parent(sp->obj))) {
        lv_obj_add_flag(sp->obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(sp->obj, LV_OBJ_FLAG_HIDDEN);
    sp->tone = helm_spark_stroke_of(id);
    lv_chart_set_series_color(sp->obj, sp->ser, helm_color_snap(sp->tone));
    helm_spark_axis(id, &mn, &mx);
    lv_chart_set_axis_range(sp->obj, LV_CHART_AXIS_PRIMARY_Y, mn, mx);
    n = s_hist_n;
    for (i = 0; i < HELM_SPARK_PTS; i++) {
        int si = (n > 1) ? (i * (n - 1)) / (HELM_SPARK_PTS - 1) : 0;

        lv_chart_set_series_value_by_id(sp->obj, sp->ser, (uint32_t)i,
                                        helm_spark_sample(id, si));
    }

    lv_chart_set_x_start_point(sp->obj, sp->ser, 0);
}

/**
 * @brief 给数据格挂上走势图。
 * @param cell 格子容器。
 * @param lab 指标标签（据此选历史序列与配色）。
 */
/** @param strip_h 非 0 时把走势图压成**底部条**（高度固定、贴底），避免折线
 *                 画到数字上；0 = 原来的整格背景层。 */
static void helm_spark_bind(lv_obj_t * cell, lv_obj_t * lab, lv_coord_t strip_h)
{
    helm_spark_t * sp;
    lv_obj_t * chart;
    lv_obj_t * last;

    if (cell == NULL || lab == NULL || s_spark_n >= HELM_SPARK_MAX) {
        return;
    }

    sp = &s_spark[s_spark_n];
    s_spark_n++;
    chart = lv_chart_create(cell);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    if (strip_h > 0) {
        lv_obj_set_size(chart, lv_pct(100), strip_h);
        lv_obj_align(chart, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else {
        lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
        lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_radius(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_point_count(chart, HELM_SPARK_PTS);
    lv_chart_set_div_line_count(chart, 0, 0);
    sp->obj = chart;
    sp->lab = lab;
    /* 对象被别处删掉时把登记项置空（LVGL 原生做法）⇒ `helm_spark_sync()` 按 NULL
     * 跳过，不会去摸野指针。地址必须稳定 ⇒ 登记项是静态数组元素，正好满足。 */
    lv_obj_null_on_delete(&sp->obj);
    lv_obj_null_on_delete(&sp->lab);
    sp->ser = lv_chart_add_series(chart, helm_color_snap(HELM_COLOR_NAV),
                                  LV_CHART_AXIS_PRIMARY_Y);
    sp->id = HELM_HIST_NONE;
    sp->tone = 0;
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(chart, helm_spark_draw_task, LV_EVENT_DRAW_TASK_ADDED, sp);
    lv_obj_add_event_cb(chart, helm_spark_draw_post, LV_EVENT_DRAW_POST, sp);
    lv_obj_move_background(chart);
    lv_obj_move_foreground(lab);
    last = lv_obj_get_child(cell, -1);
    if (last != NULL && last != chart) {
        lv_obj_move_foreground(last);
    }

    helm_spark_reload(sp, helm_hist_id(lv_label_get_text(lab)));
}

/**
 * @brief 同步走势图数据与配色。
 * @param push true 则重采样历史点；false 只刷新当前可见走势。
 * @details 时速 / 心率换档时 `tone` 变化：只改系列色并 invalidate，不重算点。
 */
static void helm_spark_sync(bool push)
{
    uint8_t i;

    for (i = 0; i < s_spark_n; i++) {
        helm_spark_t * sp = &s_spark[i];
        helm_hist_id_t id;
        const char * txt;

        if (sp->obj == NULL || sp->lab == NULL || sp->ser == NULL) {
            continue;
        }

        /* MAP 只刷 rotbar 两根；隐藏数据页走势不要 unhide / 写 30 点。 */
        if (!helm_spark_tree_shown(lv_obj_get_parent(sp->obj))) {
            lv_obj_add_flag(sp->obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        txt = lv_label_get_text(sp->lab);
        id = helm_hist_id(txt);
        if (id != sp->id || s_hist_n < 2
            || lv_obj_has_flag(sp->obj, LV_OBJ_FLAG_HIDDEN)) {
            helm_spark_reload(sp, id);
            continue;
        }

        if (id == HELM_HIST_SPEED || id == HELM_HIST_HR) {
            uint32_t stroke = helm_spark_stroke_of(id);

            if (sp->tone != stroke) {
                sp->tone = stroke;
                lv_chart_set_series_color(sp->obj, sp->ser,
                                          helm_color_snap(stroke));
                lv_obj_invalidate(sp->obj);
            }
        }

        if (!push || id == HELM_HIST_NONE) {
            continue;
        }

        helm_spark_reload(sp, id);
    }
}

static void helm_rec_ele_push(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    uint32_t slot;
    int16_t alt;

    if (!helm_riding() || rt == NULL) {
        return;
    }

    if (rt->has_altitude) {
        alt = (int16_t)(rt->altitude_m + (rt->altitude_m >= 0.0f ? 0.5f : -0.5f));
    } else {
        /* DEM 已在 gpx_timer 写入 runtime；这里再扫路网会把 MAP 的 500ms 拍打满。 */
        return;
    }

    slot = (uint32_t)(rt->session_elapsed_ms / HELM_CLIMB_REC_PERIOD_MS);
    if (s_rec_ele_n > 0 && slot == s_rec_ele_slot) {
        s_rec_ele[s_rec_ele_n - 1u] = alt;
        return;
    }

    s_rec_ele_slot = slot;
    if (s_rec_ele_n >= HELM_CLIMB_PROF_N) {
        memmove(&s_rec_ele[0], &s_rec_ele[1],
                (size_t)(HELM_CLIMB_PROF_N - 1u) * sizeof(s_rec_ele[0]));
        s_rec_ele_n = (uint8_t)(HELM_CLIMB_PROF_N - 1u);
    }

    s_rec_ele[s_rec_ele_n++] = alt;
}

static void helm_label_set(lv_obj_t * lab, const char * s)
{
    const char * cur;

    if (lab == NULL || s == NULL) {
        return;
    }
    cur = lv_label_get_text(lab);
    if (cur != NULL && strcmp(cur, s) == 0) {
        return;
    }
    lv_label_set_text(lab, s);
}

/**
 * @brief 从当前位置起，沿剖面累计剩余爬升（米）。
 * @param now_idx 当前采样点下标。
 * @return 剩余上升米数；剖面不足两点则 -1。
 */
static int32_t helm_climb_remain_from_eles(uint32_t now_idx)
{
    uint32_t i;
    int32_t gain = 0;

    if (s_sh.climb_n < 2u) {
        return -1;
    }
    for (i = now_idx; i + 1u < s_sh.climb_n; i++) {
        const int16_t a = s_climb_eles[i];
        const int16_t b = s_climb_eles[i + 1u];

        if (a == (int16_t)VGRF_ELE_UNKNOWN || b == (int16_t)VGRF_ELE_UNKNOWN) {
            continue;
        }
        if (b > a) {
            gain += (int32_t)b - (int32_t)a;
        }
    }
    return gain;
}

/**
 * @brief 按相邻采样点高差与水平步长，把该段归入平坦 / 中等 / 陡。
 * @param e0 段起点海拔（米）。
 * @param e1 段终点海拔（米）。
 * @param step_cm 该段水平距离（厘米）；剖面按总里程均分。
 * @return 0 平坦，1 中等，2 陡。上坡下坡都用 |高差|，档位直接跳、不插值。
 *
 * @note 只在 `helm_climb_profile_reload()` 换路线时调用，骑行中不重算。
 */
static uint8_t helm_climb_grade_zone(int16_t e0, int16_t e1, uint32_t step_cm)
{
    uint32_t de;
    uint32_t g;

    if (step_cm < 1u) {
        return 0;
    }
    de = (e1 >= e0) ? (uint32_t)((int32_t)e1 - (int32_t)e0)
                    : (uint32_t)((int32_t)e0 - (int32_t)e1);
    /* 坡度% = |Δh|m / 步长m × 100 = de × 10000 / step_cm */
    g = (de * 10000u) / step_cm;
    if (g < HELM_CLIMB_GRADE_FLAT) {
        return 0;
    }
    if (g < HELM_CLIMB_GRADE_STEEP) {
        return 1;
    }
    return 2;
}

/**
 * @brief 坡度档对应的面积填充色（RGB888，绘制时再 `helm_color_snap`）。
 * @param z `helm_climb_grade_zone()` 的返回值。
 * @return `HELM_COLOR_ELEV_*` 填充色。
 */
static uint32_t helm_climb_zone_fill(uint8_t z)
{
    if (z == 1u) {
        return HELM_COLOR_ELEV_MID;
    }
    if (z == 2u) {
        return HELM_COLOR_ELEV_STEEP;
    }
    return HELM_COLOR_ELEV_FLAT;
}

/**
 * @brief 坡度档对应的脊线色（RGB888，绘制时再 `helm_color`）。
 * @param z `helm_climb_grade_zone()` 的返回值。
 * @return `HELM_COLOR_ELEV_*_INK`。
 */
static uint32_t helm_climb_zone_ink(uint8_t z)
{
    if (z == 1u) {
        return HELM_COLOR_ELEV_MID_INK;
    }
    if (z == 2u) {
        return HELM_COLOR_ELEV_STEEP_INK;
    }
    return HELM_COLOR_ELEV_FLAT_INK;
}

/**
 * @brief 判断剖面一段是否与当前脏区相交，避免竖线移动时提交整图三角形。
 * @param clip LVGL 层裁剪区（绝对坐标）；NULL 视为全部可见。
 * @param p1 段起点（屏坐标）。
 * @param p2 段终点（屏坐标）。
 * @param y_base 填充底边的 y。
 * @param pad 线宽余量（像素），至少 2。
 * @return true 需要提交绘制。
 * @note 导航烘焙成功后 DRAW_TASK 已跳过；本函数只服务回退路径和「经过海拔」。
 */
static bool helm_climb_seg_in_clip(const lv_area_t * clip,
    lv_point_precise_t p1, lv_point_precise_t p2, int32_t y_base, int32_t pad)
{
    int32_t x1;
    int32_t x2;
    int32_t y1;

    if (clip == NULL) {
        return true;
    }
    if (pad < 2) {
        pad = 2;
    }
    x1 = (int32_t)(p1.x < p2.x ? p1.x : p2.x) - pad;
    x2 = (int32_t)(p1.x > p2.x ? p1.x : p2.x) + pad;
    y1 = (int32_t)(p1.y < p2.y ? p1.y : p2.y) - pad;
    if (x2 < clip->x1 || x1 > clip->x2) {
        return false;
    }
    if (y_base < clip->y1 || y1 > clip->y2) {
        return false;
    }
    return true;
}

/**
 * @brief 画剖面一段：实心色块（两三角形到基线）加脊线，无纵向渐变。
 * @param layer 目标图层。
 * @param p1 段起点。
 * @param p2 段终点。
 * @param y_base 填充落到图表底边的 y。
 * @param fill 面积色。
 * @param stroke 脊线色。
 * @param lw 脊线宽度；≤0 时按 2 像素。
 */
static void helm_climb_paint_seg(lv_layer_t * layer, lv_point_precise_t p1,
    lv_point_precise_t p2, int32_t y_base, lv_color_t fill, lv_color_t stroke,
    int32_t lw)
{
    lv_draw_triangle_dsc_t tri;
    lv_draw_line_dsc_t ld;

    lv_draw_triangle_dsc_init(&tri);
    tri.color = fill;
    tri.opa = LV_OPA_COVER;
    tri.grad.dir = LV_GRAD_DIR_NONE;
    tri.p[0] = p1;
    tri.p[1] = p2;
    tri.p[2].x = p2.x;
    tri.p[2].y = y_base;
    lv_draw_triangle(layer, &tri);
    tri.p[1].x = p2.x;
    tri.p[1].y = y_base;
    tri.p[2].x = p1.x;
    tri.p[2].y = y_base;
    lv_draw_triangle(layer, &tri);

    lv_draw_line_dsc_init(&ld);
    ld.color = stroke;
    ld.width = lw > 0 ? lw : 2;
    ld.opa = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end = 1;
    ld.p1 = p1;
    ld.p2 = p2;
    lv_draw_line(layer, &ld);
}

/**
 * @brief 隐藏导航剖面画布和竖线（非导航、无数据或回退现场绘制时）。
 */
static void helm_climb_canvas_hide(void)
{
    if (s_sh.climb_canvas) {
        lv_obj_add_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_sh.climb_cursor) {
        lv_obj_add_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 把当前位置竖线放到画布对应 x。
 * @details 坐标仍来自隐藏的 `lv_chart`（`get_point_pos_by_id`），不重绘剖面。
 */
static void helm_climb_cursor_place(void)
{
    lv_point_t p;
    int32_t cw;
    int32_t chh;
    int32_t x;
    int32_t y;

    if (s_sh.climb_cursor == NULL || s_sh.climb_chart == NULL ||
        s_sh.climb_ser == NULL || s_sh.climb_n < 2u) {
        return;
    }
    lv_obj_update_layout(s_sh.climb_chart);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser,
        (uint32_t)s_sh.climb_now, &p);

    /* 指示器跟着**剖面点**：尖端贴在点**上方 2 px**（整个三角在剖面之上），
     * 水平居中于该点 —— 也就是"下角指向灰绿交接处、高度在折线图上方"。
     *
     * ⚠⚠ 坐标约定（2026-09-25 踩过，别再"修"）：本页剖面有两条绘制路径 ——
     * 现场 `helm_climb_draw_task()` 的填充、导航 `helm_climb_canvas_seg()` 的
     * 烘焙 —— 都把 `lv_chart_get_point_pos_by_id()` 返回的**内容区坐标直接当
     * 画布/盒坐标用**（不补图表自己的 pad）。指示器**必须沿用同一套**，
     * 否则整条差一个 pad（pad_top = 22 px ⇒ 实测三角掉到折线下方、看着"位置全错"）。
     * 真要统一，得两条绘制路径 + 指示器一起改，不能只挪指示器。 */
    cw = lv_obj_get_width(s_sh.climb_chart);
    chh = lv_obj_get_height(s_sh.climb_chart);
    x = p.x - HELM_CLIMB_MARK_W / 2;
    y = p.y - 2 - (HELM_CLIMB_MARK_H - 1);
    if (x > cw - HELM_CLIMB_MARK_W && cw > HELM_CLIMB_MARK_W) {
        x = cw - HELM_CLIMB_MARK_W;
    }
    if (x < 0) {
        x = 0;
    }
    if (y > chh - HELM_CLIMB_MARK_H && chh > HELM_CLIMB_MARK_H) {
        y = chh - HELM_CLIMB_MARK_H;
    }
    if (y < 0) {
        y = 0;
    }
    lv_obj_set_pos(s_sh.climb_cursor, x, y);
    lv_obj_clear_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 按图表实际宽高分配/复用 RGB565 画布缓冲。
 * @return 画布可用则为 true。
 * @details 缓冲在 PSRAM。宽高未变则复用。分配失败返回 false，
 *          调用方回退 `helm_climb_draw_task` 现场绘制。
 */
static bool helm_climb_canvas_ensure(void)
{
    int32_t w;
    int32_t h;
    size_t bytes;

    if (s_sh.climb_canvas == NULL || s_sh.climb_chart == NULL) {
        return false;
    }
    if (s_sh.climb) {
        lv_obj_update_layout(s_sh.climb);
    }
    lv_obj_update_layout(s_sh.climb_chart);
    w = lv_obj_get_width(s_sh.climb_chart);
    h = lv_obj_get_height(s_sh.climb_chart);
    if (w < 8 || h < 8) {
        return false;
    }
    if (s_climb_cbuf != NULL && w == s_climb_cw && h == s_climb_ch) {
        /*
         * ⚠ 尺寸没变也**必须重新挂一次缓冲区**：画布对象可能刚被重建过（换主题 =
         * `helm_shell_rebuild()` 删掉整棵 shell 再建），而 `s_climb_cbuf` 是我们自己的
         * 内存、不属于 LVGL、重建不会动它 ⇒ 尺寸也恰好没变（同一个布局）⇒ 原来这里直接
         * return，**新画布对象上从来没 set 过 buffer** ⇒ `lv_canvas_fill_bg()`/图层绘制
         * 全落在空处，屏上只剩纸色底（用户 2026-09-27 深挖到的真因：数据/几何/可见性/烘焙
         * 日志全都"正常"，就是没挂 buffer）。set_buffer 是幂等的，重复调用无副作用。
         */
        lv_canvas_set_buffer(s_sh.climb_canvas, s_climb_cbuf, w, h,
            LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s_sh.climb_canvas, w, h);
        syslog(LOG_NOTICE, "[climb] canvas buffer re-attached (reuse %dx%d)\n", (int)w, (int)h);
        return true;
    }
    bytes = (size_t)lv_draw_buf_width_to_stride((uint32_t)w,
        LV_COLOR_FORMAT_RGB565) * (size_t)h;
    vmap_free(s_climb_cbuf);
    s_climb_cbuf = vmap_malloc(bytes);
    if (s_climb_cbuf == NULL) {
        s_climb_cw = 0;
        s_climb_ch = 0;
        return false;
    }
    s_climb_cw = w;
    s_climb_ch = h;
    lv_canvas_set_buffer(s_sh.climb_canvas, s_climb_cbuf, w, h,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_sh.climb_canvas, w, h);
    return true;
}

/**
 * @brief 把剖面一段画进画布图层。
 * @param layer 画布图层。
 * @param i 段下标（点 i→i+1）。
 * @param as_done true 则强制灰色（刚骑过的那截）。
 */
static void helm_climb_canvas_seg(lv_layer_t * layer, uint32_t i, bool as_done)
{
    lv_point_t a;
    lv_point_t b;
    lv_point_precise_t p1;
    lv_point_precise_t p2;
    lv_color_t fill;
    lv_color_t ink;
    uint8_t z;

    if (layer == NULL || s_sh.climb_chart == NULL || s_sh.climb_ser == NULL ||
        i + 1u >= s_sh.climb_n) {
        return;
    }
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i, &a);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i + 1u, &b);
    p1.x = a.x;
    p1.y = a.y;
    p2.x = b.x;
    p2.y = b.y;
    if (as_done || i < (uint32_t)s_sh.climb_now) {
        fill = helm_color_snap(HELM_COLOR_ELEV_DONE);
        ink = helm_color(HELM_COLOR_ELEV_DONE_INK);
    } else {
        z = s_climb_zone[i];
        fill = helm_color_snap(helm_climb_zone_fill(z));
        ink = helm_color(helm_climb_zone_ink(z));
    }
    helm_climb_paint_seg(layer, p1, p2, s_climb_ch - 1, fill, ink, 2);
}

/**
 * @brief 标脏画布上 [i0, i1] 对应的水平窄条。
 * @param i0 起始采样点下标。
 * @param i1 结束采样点下标。
 * @details 关闭 `lv_canvas_finish_layer` 的整对象 invalidate 后，用本函数只刷刚改的那一截。
 */
static void helm_climb_canvas_inval_span(uint32_t i0, uint32_t i1)
{
    lv_point_t a;
    lv_point_t b;
    lv_area_t area;
    int32_t orig;
    int32_t lo;
    int32_t hi;
    uint32_t last;

    if (s_sh.climb_canvas == NULL || s_sh.climb_n < 2u) {
        return;
    }
    last = (uint32_t)s_sh.climb_n - 1u;
    if (i0 > last) {
        i0 = last;
    }
    if (i1 > last) {
        i1 = last;
    }
    lv_obj_get_coords(s_sh.climb_canvas, &area);
    orig = area.x1;
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i0, &a);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, i1, &b);
    lo = a.x < b.x ? a.x : b.x;
    hi = a.x < b.x ? b.x : a.x;
    area.x1 = orig + lo - 2;
    area.x2 = orig + hi + 3;
    lv_obj_invalidate_area(s_sh.climb_canvas, &area);
}

/**
 * @brief 把 [i0, i1) 段画进已有画布（竖线前进时只涂灰，不整图重绘）。
 * @param i0 起始段。
 * @param i1 结束段（不含）。
 * @param as_done 强制灰色。
 * @details `lv_canvas_finish_layer` 前后关掉 display invalidation，再按窄条标脏，
 *          避免整张画布进入切页脏区。
 */
static void helm_climb_canvas_paint_range(uint32_t i0, uint32_t i1, bool as_done)
{
    lv_layer_t layer;
    lv_display_t * disp;
    uint32_t i;

    if (s_sh.climb_canvas == NULL || s_sh.climb_n < 2u || i0 >= i1) {
        return;
    }
    if (i1 > (uint32_t)s_sh.climb_n - 1u) {
        i1 = (uint32_t)s_sh.climb_n - 1u;
    }
    lv_canvas_init_layer(s_sh.climb_canvas, &layer);
    for (i = i0; i < i1; i++) {
        helm_climb_canvas_seg(&layer, i, as_done);
    }
    disp = lv_obj_get_display(s_sh.climb_canvas);
    if (disp) {
        lv_display_enable_invalidation(disp, false);
    }
    lv_canvas_finish_layer(s_sh.climb_canvas, &layer);
    if (disp) {
        lv_display_enable_invalidation(disp, true);
    }
    helm_climb_canvas_inval_span(i0, i1);
}

/**
 * @brief 把当前路线海拔剖面烘焙到画布（只在换路线时调用）。
 * @return 成功则图表隐藏，切页只贴这一张图。
 * @details 切页 220 ms crossfade 若走 DRAW_TASK，每帧会提交约 59 段三角形，
 *          `lv_timer_handler` 可达 150–270 ms。烘焙后只 blit。失败返回 false。
 */
static bool helm_climb_canvas_bake(void)
{
    lv_layer_t layer;
    uint32_t i;

    if (!s_sh.climb_nav || s_sh.climb_n < 2u || !helm_climb_canvas_ensure()) {
        return false;
    }
    lv_canvas_fill_bg(s_sh.climb_canvas, helm_color(HELM_COLOR_PAPER),
        LV_OPA_COVER);
    lv_canvas_init_layer(s_sh.climb_canvas, &layer);
    for (i = 0; i + 1u < s_sh.climb_n; i++) {
        helm_climb_canvas_seg(&layer, i, false);
    }
    lv_canvas_finish_layer(s_sh.climb_canvas, &layer);
    lv_obj_add_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);
    helm_climb_cursor_place();

    /*
     * 烘焙收尾（2026-09-27）：
     *   ① `lv_obj_update_layout()`：重建后布局可能还没跑过（几何为 0 ⇒ invalidate 的是空区域）；
     *   ② 显式 `lv_obj_invalidate()`：`lv_canvas_finish_layer()` 只标画布自身的脏区，而换主题
     *      这条路上画布是刚重建的新对象 ⇒ 补一次，保证新配色真的上屏。
     * 保留一行低频日志（一次烘焙一行，路线变化才触发）：换主题后再遇到"图是空的"，
     * 这一行就能分清"没数据（n/ele 为 0）"还是"有数据但没上屏"。
     */
    lv_obj_update_layout(s_sh.climb_canvas);
    lv_obj_invalidate(s_sh.climb_canvas);
    lv_obj_invalidate(s_sh.climb_chart);
    syslog(LOG_NOTICE, "[climb] bake nav=%d n=%d ele=%d..%d\n",
           (int)s_sh.climb_nav, (int)s_sh.climb_n,
           (int)(s_sh.climb_n >= 2u ? (s_climb_eles[0] < s_climb_eles[s_sh.climb_n - 1u]
                                      ? s_climb_eles[0] : s_climb_eles[s_sh.climb_n - 1u]) : 0),
           (int)(s_sh.climb_n >= 2u ? (s_climb_eles[0] > s_climb_eles[s_sh.climb_n - 1u]
                                      ? s_climb_eles[0] : s_climb_eles[s_sh.climb_n - 1u]) : 0));

    return true;
}

/**
 * @brief 只标脏当前位置竖线附近一条窄带（非导航「经过海拔」用）。
 * @param x 点在图表内的相对 x。
 */
/**
 * @brief 标脏当前位置指示器（小倒三角）所在的小方块。
 * @param x 图表内相对 x（三角中心）。
 * @param y 图表内相对 y（尖端）。
 * @details 2026-09-25：指示器由"2 px 通高竖线"改成小倒三角后，标脏也从
 *          "整列高度 × 7 px 宽"缩成三角的包围盒 + 1 px 余量 —— 导航烘焙模式下
 *          这一条要重绘画布，小方块明显更省。
 */
static void helm_climb_inval_cursor_at(int32_t x, int32_t y)
{
    lv_area_t a;
    const int32_t half = HELM_CLIMB_MARK_W / 2 + 1;

    if (s_sh.climb_chart == NULL) {
        return;
    }
    lv_obj_get_coords(s_sh.climb_chart, &a);
    a.x1 += x - half;
    a.x2 = a.x1 + 2 * half;
    a.y1 += y - (HELM_CLIMB_MARK_H + 1);
    a.y2 = a.y1 + (HELM_CLIMB_MARK_H + 2);
    lv_obj_invalidate_area(s_sh.climb_chart, &a);
}

/**
 * @brief 标脏 [xa, xb] 整列高度，让刚骑过的那截从彩块改成灰。
 * @param xa 图表内相对 x。
 * @param xb 图表内相对 x。
 *
 * @note 导航剖面专用。竖线未动时不要调用。
 */
static void helm_climb_inval_span_x(int32_t xa, int32_t xb)
{
    lv_area_t a;
    int32_t orig;
    int32_t lo;
    int32_t hi;

    if (s_sh.climb_chart == NULL) {
        return;
    }
    lv_obj_get_coords(s_sh.climb_chart, &a);
    orig = a.x1;
    lo = xa < xb ? xa : xb;
    hi = xa < xb ? xb : xa;
    a.x1 = orig + lo - 2;
    a.x2 = orig + hi + 3;
    lv_obj_invalidate_area(s_sh.climb_chart, &a);
}

/**
 * @brief 当前位置下标变化时只移动竖线（及导航时那一窄条灰色），不 refresh 整张图。
 * @param id 新的采样点下标。
 * @details 画布已显示：前进则把 [prev, id) 涂灰并挪竖线；后退则整图重烘焙。
 *          未烘焙：只标脏窄条。画布可见时即使图表已隐藏也要处理。
 */
static void helm_climb_cursor_move(uint32_t id)
{
    lv_point_t p;
    lv_point_t prev_p;
    uint32_t n;
    uint8_t prev;
    bool baked;

    if (s_sh.climb_ser == NULL || s_sh.climb_n < 2u) {
        return;
    }
    baked = (s_sh.climb_canvas != NULL) &&
            !lv_obj_has_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);
    if (!baked && (s_sh.climb_chart == NULL ||
            lv_obj_has_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN))) {
        return;
    }

    n = baked ? (uint32_t)s_sh.climb_n : lv_chart_get_point_count(s_sh.climb_chart);
    if (n < 2u) {
        return;
    }
    if (id >= n) {
        id = n - 1u;
    }
    if (s_sh.climb_now == (uint8_t)id) {
        return;
    }

    prev = s_sh.climb_now;
    s_sh.climb_now = (uint8_t)id;
    if (baked) {
        if ((uint32_t)id > (uint32_t)prev) {
            helm_climb_canvas_paint_range((uint32_t)prev, (uint32_t)id, true);
            helm_climb_cursor_place();
        } else {
            (void)helm_climb_canvas_bake();
        }
        return;
    }

    lv_obj_update_layout(s_sh.climb_chart);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, prev, &prev_p);
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, id, &p);
    if (s_sh.climb_nav) {
        helm_climb_inval_span_x(prev_p.x, p.x);
    } else {
        /* 指示器跟着剖面点 ⇒ 新旧位置各标脏一次；坐标与绘制同约定（不补 pad）。 */
        helm_climb_inval_cursor_at(prev_p.x, prev_p.y - 2);
        helm_climb_inval_cursor_at(p.x, p.y - 2);
    }
}

/**
 * @brief 按剖面海拔范围加边距，得到图表 Y 轴。
 * @param eles 海拔采样（米）。
 * @param n 采样点数。
 * @param[out] mn 轴下限。
 * @param[out] mx 轴上限。
 */
static void helm_climb_axis(const int16_t * eles, uint32_t n,
    int32_t * mn, int32_t * mx)
{
    uint32_t i;
    int32_t lo = 32767;
    int32_t hi = -32767;
    int32_t pad;

    for (i = 0; i < n; i++) {
        if (eles[i] == (int16_t)VGRF_ELE_UNKNOWN) {
            continue;
        }
        if ((int32_t)eles[i] < lo) {
            lo = eles[i];
        }
        if ((int32_t)eles[i] > hi) {
            hi = eles[i];
        }
    }
    if (lo > hi) {
        lo = 0;
        hi = 100;
    }
    pad = (hi - lo) / 10;
    if (pad < 8) {
        pad = 8;
    }
    *mn = lo - pad;
    *mx = hi + pad;
}

/**
 * @brief 路线或近 30 分钟记录变化时重载海拔剖面。
 * @details 导航：按总里程均匀取样，并一次性写入 `s_climb_zone`（骑行中不重算坡度）。
 *          烘焙成功则隐藏图表、不再 `lv_chart_refresh`。
 *          未导航或烘焙失败：显示图表并 refresh 一次（「经过海拔」仍用青绿渐变）。
 */
static void helm_climb_profile_reload(void)
{
    int16_t eles[HELM_CLIMB_PROF_N];
    uint32_t n = 0;
    uint32_t now_idx = LV_CHART_POINT_NONE;
    uint32_t i;
    int32_t mn;
    int32_t mx;
    bool nav;
    int16_t last = 0;
    bool have = false;
    uint32_t fp_pts = 0;
    uint32_t fp_total_cm = 0;

    if (s_sh.climb_chart == NULL || s_sh.climb_ser == NULL) {
        return;
    }

    nav = helm_nav_on();
#if VMAP_ROUTE_ENABLE
    if (nav) {
        uint32_t cur = 0;

        n = map_page_nav_profile_ele(s_sh.map, eles, HELM_CLIMB_PROF_N, &cur);
        (void)map_page_nav_progress(s_sh.map, &fp_pts, &fp_total_cm, NULL, n);
        if (n >= 2u) {
            now_idx = cur;
        }
    } else
#endif
    if (helm_riding() && s_rec_ele_n >= 2u) {
        n = s_rec_ele_n;
        memcpy(eles, s_rec_ele, (size_t)n * sizeof(eles[0]));
        fp_pts = n;
        fp_total_cm = n;
    }

    if (s_sh.climb_title) {
        /* 非导航时这条剖面来自本机记录采样；还没采到点就明说「记录中」，
         * 而不是让人对着一张空图猜（用户 2026-09-26）。 */
        if (!nav && s_rec_ele_n < 2u) {
            helm_label_set(s_sh.climb_title, "记录中");
        } else {
            helm_label_set(s_sh.climb_title, nav ? "路线海拔" : "经过海拔");
        }
    }

    s_climb_fp_pts = fp_pts;
    s_climb_fp_total_cm = fp_total_cm;
    s_climb_fp_rec_n = nav ? 0 : (uint8_t)s_rec_ele_n;
    s_climb_fp_rec_slot = nav ? 0xffffffffu : s_rec_ele_slot;
    s_sh.climb_nav = nav;

    if (n < 2u) {
        s_sh.climb_n = 0;
        s_sh.climb_now = 0;
        helm_climb_canvas_hide();
        lv_obj_add_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    for (i = 0; i < n; i++) {
        if (eles[i] != (int16_t)VGRF_ELE_UNKNOWN) {
            last = eles[i];
            have = true;
            break;
        }
    }
    if (!have) {
        s_sh.climb_n = 0;
        helm_climb_canvas_hide();
        lv_obj_add_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    for (i = 0; i < n; i++) {
        int16_t e = eles[i];

        if (e == (int16_t)VGRF_ELE_UNKNOWN) {
            e = last;
        } else {
            last = e;
        }
        s_climb_eles[i] = e;
    }
    if (nav && n >= 2u) {
        uint32_t step_cm = (fp_total_cm > 0u) ? (fp_total_cm / (n - 1u)) : 0u;

        if (step_cm < 1u) {
            step_cm = 1u;
        }
        for (i = 0; i + 1u < n; i++) {
            s_climb_zone[i] = helm_climb_grade_zone(s_climb_eles[i],
                s_climb_eles[i + 1u], step_cm);
        }
    }

    helm_climb_axis(s_climb_eles, n, &mn, &mx);
    if (lv_chart_get_point_count(s_sh.climb_chart) != n) {
        lv_chart_set_point_count(s_sh.climb_chart, n);
    }
    lv_chart_set_axis_range(s_sh.climb_chart, LV_CHART_AXIS_PRIMARY_Y, mn, mx);
    {
        lv_color_t col = helm_color_snap(HELM_COLOR_CLIMB);

        if (!lv_color_eq(lv_chart_get_series_color(s_sh.climb_chart,
                s_sh.climb_ser), col)) {
            lv_chart_set_series_color(s_sh.climb_chart, s_sh.climb_ser, col);
        }
    }
    {
        int32_t * y = lv_chart_get_series_y_array(s_sh.climb_chart,
            s_sh.climb_ser);

        if (y != NULL) {
            for (i = 0; i < n; i++) {
                y[i] = (int32_t)s_climb_eles[i];
            }
        }
    }
    lv_chart_set_x_start_point(s_sh.climb_chart, s_sh.climb_ser, 0);
    s_sh.climb_n = (uint8_t)n;
    s_sh.climb_now = (uint8_t)((nav && now_idx < n) ? now_idx : (n - 1u));
    if (nav && helm_climb_canvas_bake()) {
        return;
    }
    helm_climb_canvas_hide();
    lv_obj_remove_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN);
    lv_chart_refresh(s_sh.climb_chart);
}

/**
 * @brief 爬升页周期刷新：路线指纹未变则只挪竖线，否则重载剖面。
 * @param rt 当前骑行快照，可 NULL。
 * @param page_i 当前页序号（无剩余爬升时显示「页码」）。
 * @param page_n 总页数。
 * @details 500 ms UI 节拍。指纹（点数 / 总里程）不变时不重算 `s_climb_zone`、不烘焙。
 */
static void helm_climb_tick(const bicycle_runtime_t * rt, uint8_t page_i,
    uint8_t page_n)
{
    char buf[32];
    uint32_t now_idx = 0;
    int32_t remain_gain = -1;
    bool reload = true;
    int i;

    if (s_sh.climb_chart == NULL) {
        return;
    }

#if VMAP_ROUTE_ENABLE
    if (helm_nav_on()) {
        uint32_t pts = 0;
        uint32_t total_cm = 0;
        uint32_t prof_n = s_sh.climb_n >= 2u ? s_sh.climb_n : HELM_CLIMB_PROF_N;

        if (map_page_nav_progress(s_sh.map, &pts, &total_cm, &now_idx, prof_n)
            && pts == s_climb_fp_pts && total_cm == s_climb_fp_total_cm
            && s_sh.climb_nav) {
            reload = false;
        }
    } else
#endif
    if (!s_sh.climb_nav && s_rec_ele_n == s_climb_fp_rec_n
        && s_rec_ele_slot == s_climb_fp_rec_slot) {
        reload = false;
        now_idx = s_sh.climb_n >= 2u ? (uint32_t)s_sh.climb_n - 1u : 0u;
    }

    if (s_climb_rebake) {
        s_climb_rebake = false;
        /* 用既有数据重烘（没导航剖面时 bake 自己返回假）；真烘到了才不用再去取一遍数。 */
        if (helm_climb_canvas_bake()) {
            reload = false;
        }
    }

    if (reload) {
        helm_climb_profile_reload();
        now_idx = s_sh.climb_now;
    } else {
        helm_climb_cursor_move(now_idx);
    }

#if VMAP_ROUTE_ENABLE
    if (s_sh.climb_nav && s_sh.climb_n >= 2u) {
        remain_gain = helm_climb_remain_from_eles(s_sh.climb_now);
    }
#endif
    /* 海拔页六格：海拔 / 坡度 / 爬升 / 下降 / 最高 / 最低。
     * 刻度与文案都照用户 2026-09-26 给的图；单位放进标签里
     * （"1284 m" 在 num_28 下约 101px，格内宽只有 ~91px，放不下）。 */
    {
        const char * klab[HELM_CLIMB_N] = {
            "海拔 (m)", "坡度 (%)", "爬升 (m)", "下降 (m)", "最高 (m)",
            "最低 (m)"
        };
        char kval[HELM_CLIMB_N][16];
        int alt = 0;
        bool have = false;

        if (rt && rt->has_altitude) {
            alt = (int)(rt->altitude_m + 0.5f);
            have = true;
        }
        if (!have && s_sh.climb_n >= 2u) {
            const int16_t e = s_climb_eles[s_sh.climb_now];

            if (e != (int16_t)VGRF_ELE_UNKNOWN) {
                alt = (int)e;
                have = true;
            }
        }

        if (have) {
            lv_snprintf(kval[0], 16, "%d", alt);
        } else {
            lv_snprintf(kval[0], 16, "--");
        }

        /* 坡度：num_* 字库里没有 '-'，所以这里给**绝对值**（方向由爬升/下降
         * 两格和剖面本身表达）。 */
        {
            int g10 = rt ? (int)(rt->grade_pct * 10.0f) : 0;

            lv_snprintf(kval[1], 16, "%d.%d", g10 / 10,
                        (g10 < 0 ? -g10 : g10) % 10);
        }

        lv_snprintf(kval[2], 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
        lv_snprintf(kval[3], 16, "%d", rt ? (int)(rt->loss_m + 0.5f) : 0);
        /* 最高/最低：**导航时给这条路线剖面的极值**（用户 2026-09-26：一条
         * 起伏 11 m 的路线整条全绿、看不出"这线到底有多平"）。剖面数据就是
         * s_climb_eles（已把 UNKNOWN 用前值填过），扫一遍即可。
         * 非导航时仍是会话极值；没记录过给 "--"（0 m 是合法海拔，不能当没数据）。 */
        {
            bool have_mm = false;
            int16_t emin = 0;
            int16_t emax = 0;

            if (s_sh.climb_nav && s_sh.climb_n >= 2u) {
                for (i = 0; i < (int)s_sh.climb_n; i++) {
                    int16_t e = s_climb_eles[i];

                    if (e == (int16_t)VGRF_ELE_UNKNOWN) {
                        continue;
                    }
                    if (!have_mm) {
                        have_mm = true;
                        emin = e;
                        emax = e;
                    } else if (e < emin) {
                        emin = e;
                    } else if (e > emax) {
                        emax = e;
                    }
                }
            } else if (rt && rt->alt_range_valid) {
                have_mm = true;
                emin = (int16_t)(rt->altitude_min_m + 0.5f);
                emax = (int16_t)(rt->altitude_max_m + 0.5f);
            }

            if (have_mm) {
                lv_snprintf(kval[4], 16, "%d", (int)emax);
                lv_snprintf(kval[5], 16, "%d", (int)emin);
            } else {
                lv_snprintf(kval[4], 16, "--");
                lv_snprintf(kval[5], 16, "--");
            }
        }

        for (i = 0; i < HELM_CLIMB_N; i++) {
            if (s_sh.climb_val[i] == NULL) {
                continue;
            }
            helm_label_set(s_sh.climb_val[i], kval[i]);
            helm_set_text_hex(s_sh.climb_val[i], HELM_COLOR_INK);
            if (s_sh.climb_rail[i] != NULL) {
                lv_obj_set_style_bg_color(s_sh.climb_rail[i],
                    helm_color(helm_tone(klab[i])), 0);
            }
        }
        LV_UNUSED(klab);
    }

    if (s_sh.climb_mark) {
        /* 剖面卡右上角："剩余爬升 280 m"（没有航线海拔时整条不显示，
         * 不退回页码 —— 用户 2026-09-26）。 */
        if (remain_gain >= 0) {
            lv_snprintf(buf, sizeof(buf), "剩余爬升 %ldm", (long)remain_gain);
            helm_label_set(s_sh.climb_mark, buf);
        } else {
            helm_label_set(s_sh.climb_mark, "");
        }
    }
}

static bool helm_nav_on(void)
{
    return map_page_nav_active(s_sh.map);
}

static bool helm_riding(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    return (rt && rt->recording) || s_sh.paused;
}

static void helm_ride_dwell_reset(void)
{
    s_still_since = 0;
    s_move_since = 0;
}

static void helm_autopause_hold_clear(void)
{
    s_autopause_hold_since = 0;
}

static bool helm_autopause_held(void)
{
    if (s_autopause_hold_since == 0u) {
        return false;
    }

    if (lv_tick_elaps(s_autopause_hold_since) >= HELM_AUTOPAUSE_HOLD_MS) {
        s_autopause_hold_since = 0;
        return false;
    }

    return true;
}

static void helm_pause_title_sync(void)
{
    if (s_sh.pause_title == NULL) {
        return;
    }

    lv_label_set_text(s_sh.pause_title,
        s_sh.pause_auto ? "自动暂停" : "手动暂停");
}

static void helm_pause_dock_apply(void)
{
    bool pause_was = helm_obj_shown(s_sh.pause_dock);

    if (s_sh.ui_covered) {
        return;
    }

    if (s_sh.paused) {
        helm_pause_title_sync();
        helm_show(s_sh.pause_dock, true);
        helm_raise_if_shown(s_sh.pause_dock);
        if (!pause_was) {
            helm_obj_stagger_in(s_sh.pause_dock);
        }
    } else {
        helm_obj_stagger_clear(s_sh.pause_dock);
        helm_show(s_sh.pause_dock, false);
    }
    helm_raise_chrome();
}

static void helm_pause_resume_by_key(void)
{
    bool was_auto = s_sh.pause_auto;

    map_page_resume_ride(s_sh.map);
    if (was_auto) {
        s_autopause_hold_since = lv_tick_get();
        if (s_autopause_hold_since == 0u) {
            s_autopause_hold_since = 1u;
        }
    }
    s_sh.pause_auto = false;
    helm_shell_set_paused(false);
    helm_ride_dwell_reset();
    myvendor_sound_ride();
}

static void helm_ride_status_publish(void)
{
    bool session = helm_riding();
    bool moving = session && !s_sh.paused && bicycle_runtime_is_moving(false);

    companion_bridge_ride_set(session, moving);
}

static void helm_autopause_tick(void)
{
    uint32_t now;
    const bicycle_runtime_t * rt;
    bool recording;
    bool moving;

    if (!helm_riding()) {
        helm_ride_dwell_reset();
        return;
    }

    /* 保存框仍在主界面上，不在背后偷偷恢复。菜单盖住时继续判运动。 */
    if (helm_save_open()) {
        return;
    }

    if (s_sh.paused && !s_sh.pause_auto) {
        helm_ride_dwell_reset();
        return;
    }

    if (!myvendor_devctl_autopause_get()) {
        helm_ride_dwell_reset();
        return;
    }

    now = lv_tick_get();
    rt = bicycle_runtime_get();
    recording = rt && rt->recording;
    moving = s_sh.paused ? bicycle_runtime_is_moving(true)
                         : bicycle_runtime_is_moving(false);

    if (moving) {
        s_still_since = 0;
        if (s_move_since == 0) {
            s_move_since = now;
        }

        if (s_sh.paused && s_sh.pause_auto &&
            lv_tick_elaps(s_move_since) >= HELM_AUTORESUME_MS) {
            map_page_resume_ride(s_sh.map);
            s_sh.pause_auto = false;
            helm_shell_set_paused(false);
            s_autopause_hold_since = now;
            if (s_autopause_hold_since == 0u) {
                s_autopause_hold_since = 1u;
            }
            helm_ride_dwell_reset();
        }
    } else {
        s_move_since = 0;
        if (helm_autopause_held()) {
            s_still_since = 0;
        } else if (s_still_since == 0) {
            s_still_since = now;
        }

        /* 模拟轨迹回放期间**不要自动暂停**：回放的 GPS 数据不是真实运动
         * （速度/是否移动由 runtime 的判定决定），一暂停就把"导航测试"打断，
         * 屏幕上弹"自动暂停"。测试链路要的是纯导航，所以这里直接屏蔽。 */
        if (recording && !bicycle_gpx_sim_active() && !s_sh.paused &&
            s_still_since != 0 &&
            lv_tick_elaps(s_still_since) >= HELM_AUTOPAUSE_MS) {
            map_page_pause_ride(s_sh.map);
            s_sh.pause_auto = true;
            helm_shell_set_paused(true);
            helm_ride_dwell_reset();
        }
    }
}

/** @brief 录制、暂停或导航中：用骑行翻页（数据/地图/转向/爬升）。 */
static bool helm_in_session(void)
{
    return helm_riding() || helm_nav_on();
}

static uint8_t * helm_active_idx(void)
{
    return helm_in_session() ? &s_sh.ride_idx : &s_sh.home_idx;
}

/**
 * @brief 页面总数（用户 2026-09-26 的清单，第二轮修正后）。
 * @details 待机 3 页：主页面 / 骑行页 / 数据页。
 *          骑行 4 页：骑行页 / 数据页 / 地图页 / 海拔页。
 * @note **待机没有单独的"地图页"**：用户原话"主页面就是地图页，只是下半部分
 *       组件不一样了" —— 主页面（STANDBY）本来就是地图 + 信息卡，`map_on` 里
 *       STANDBY 与 MAP 都会让地图显示，所以待机不必再列一页地图。
 * @note **导航也不再插转向页**：导航时同样是这 4 页（转向信息走地图上的
 *       通栏横幅/路牌，不需要单独一页）。TURN pane 仍建着、只是不在轮转表里。
 */
static uint8_t helm_page_count(void)
{
    return helm_in_session() ? 4u : 3u;
}

static helm_page_id_t helm_page_at(uint8_t idx)
{
    if (!helm_in_session()) {
        static const helm_page_id_t idle[] = {
            HELM_PAGE_STANDBY, HELM_PAGE_DATA, HELM_PAGE_DATA_ALL
        };
        return idle[idx % (uint8_t)(sizeof(idle) / sizeof(idle[0]))];
    }

    {
        static const helm_page_id_t r[] = {
            HELM_PAGE_DATA, HELM_PAGE_DATA_ALL, HELM_PAGE_MAP,
            HELM_PAGE_CLIMB
        };
        return r[idx % (uint8_t)(sizeof(r) / sizeof(r[0]))];
    }
}

/* index.html startRide: idle map stays map; otherwise open 数据. */
static void helm_enter_ride(void)
{
    helm_hist_reset();
    bicycle_runtime_reset_session();
    s_sh.paused = false;
    s_sh.pause_auto = false;
    helm_ride_dwell_reset();
    helm_autopause_hold_clear();
    map_page_set_recording(s_sh.map, true);
    /* 已在导航中则留在当前骑行页，勿被待机页索引带跑。 */
    if (!helm_nav_on()) {
        /* 待机 → 骑行：待机 3 页 [主页面, 骑行页, 数据页]、
         * 骑行 4 页 [骑行页, 数据页, 地图页, 海拔页] ⇒
         * 主页面/骑行页都落到骑行页[0]，数据页落到数据页[1]。 */
        s_sh.ride_idx = (s_sh.home_idx >= 2u) ? 1u : 0u;
    }
    helm_apply_view();
    helm_refresh_numbers();
    myvendor_sound_ride();
}

void helm_shell_ensure_ride(void)
{
    if (!s_sh.attached) {
        return;
    }

    if (s_sh.paused) {
        helm_shell_set_paused(false);
        map_page_resume_ride(s_sh.map);
        return;
    }

    if (!helm_riding()) {
        helm_enter_ride();
    }
}

void helm_shell_continue_ride(float km, uint32_t sec, const float * lon,
                              const float * lat, uint16_t n)
{
    if (!s_sh.attached || helm_riding()) {
        bicycle_ride_gpx_cancel_continue();
        return;
    }

    helm_hist_reset();
    s_sh.paused = false;
    s_sh.pause_auto = false;
    helm_ride_dwell_reset();
    helm_autopause_hold_clear();
    map_page_set_recording(s_sh.map, true);
    if (km < 0.0f) {
        km = 0.0f;
    }

    bicycle_runtime_seed_session((double)km * 1000.0, (uint64_t)sec * 1000ull);
    map_page_seed_track(s_sh.map, lon, lat, n);
    bicycle_runtime_seed_session((double)km * 1000.0, (uint64_t)sec * 1000ull);
    if (!helm_nav_on()) {
        s_sh.ride_idx = (s_sh.home_idx >= 2u) ? 1u : 0u;
    }

    helm_apply_view();
    helm_refresh_numbers();
    myvendor_sound_ride();
}

static void helm_leave_ride(void)
{
    helm_hist_reset();
    map_page_end_ride(s_sh.map);
    s_sh.pause_auto = false;
    helm_ride_dwell_reset();
    helm_autopause_hold_clear();
    helm_shell_set_paused(false);
    helm_raise_chrome();
    map_page_continue_render(s_sh.map);
}

static void helm_finish_ride(bool keep)
{
    bicycle_runtime_save_last_pos();
    if (keep) {
        const bicycle_runtime_t * rt = bicycle_runtime_get();
        char path[128];
        const char * base;

        path[0] = '\0';

        {
            int gpx_ret = bicycle_ride_gpx_commit(path, sizeof(path));

            if (gpx_ret == 0 && path[0] != '\0') {
                base = strrchr(path, '/');
                base = (base != NULL && base[1] != '\0') ? base + 1 : path;
                if (rt != NULL) {
                    s_sh.last_km = (float)(rt->session_distance_m / 1000.0);
                    helm_ride_stat_remember(path,
                        rt->session_distance_m / 1000.0,
                        (uint32_t)(rt->session_elapsed_ms / 1000ull));
                    myvendor_devctl_last_ride_m_set(
                        (int32_t)(rt->session_distance_m + 0.5));
                }

                lv_pm_notify_show("已保存", base, 2000);
            } else if (gpx_ret == -ENODATA) {
                lv_pm_notify_show("骑行", "无轨迹，未保存", 2000);
            } else {
                lv_pm_notify_show("骑行", "GPX 保存失败", 2000);
            }
        }

        myvendor_sound_save();
    } else {
        (void)bicycle_ride_gpx_discard();
    }

    helm_save_hide();
    helm_leave_ride();
}

static void helm_discard_ride(void)
{
    helm_finish_ride(false);
    myvendor_sound_discard();
    lv_pm_notify_show("骑行", "未保存", 1500);
}

static void helm_fill_group(uint8_t gi, const char ** lab, char val[][16])
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    char tmp[16];
    float avg;
    float dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;

    avg = helm_session_avg_kph(rt);

    if (gi == 0) {
        lab[0] = "时间";
        helm_fmt_time(val[0], 16, rt ? rt->session_elapsed_ms : 0);
        lab[1] = "里程";
        helm_fmt_km(val[1], 16, dist_km);
        lab[2] = "均速";
        helm_fmt_speed(val[2], 16, avg);
        /* 第 4 行是**实时心率**（用户 2026-09-26 按设计图定稿：骑行中要抬眼
         * 看到的是当前心率；均值挪到数据页）。 */
        lab[3] = "心率";
        lv_snprintf(val[3], 16, "%s",
                    helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_bpm : 0));
        return;
    }

    if (gi == 1) {
        lab[0] = "心率";
        lv_snprintf(val[0], 16, "%s",
                    helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_bpm : 0));
        lab[1] = "均心率";
        lv_snprintf(val[1], 16, "%s",
                    helm_dash(rt && rt->hr_avg_bpm > 0u, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_avg_bpm : 0));
        lab[2] = "最大心率";
        lv_snprintf(val[2], 16, "%s",
                    helm_dash(rt && rt->hr_max_bpm > 0u, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_max_bpm : 0));
        lab[3] = "踏频";
        lv_snprintf(val[3], 16, "%s",
                    helm_dash(rt && rt->cadence_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->cadence_rpm : 0));
        return;
    }

    lab[0] = "均速";
    helm_fmt_speed(val[0], 16, helm_avg_kph(rt));
    lab[1] = "极速";
    helm_fmt_speed(val[1], 16, rt ? rt->speed_max_kph : 0);
    lab[2] = "功率";
    lv_snprintf(val[2], 16, "%s",
                helm_dash(rt && rt->power_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->power_w : 0));
    lab[3] = "爬升";
    lv_snprintf(val[3], 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
}

static void helm_fill_rot(uint8_t ri, const char ** la, char va[16],
                          const char ** lb, char vb[16])
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    char tmp[16];
    float avg = helm_avg_kph(rt);
    float dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;
    int alt = rt ? (int)(rt->altitude_m + 0.5f) : 0;

    switch (ri % 5u) {
    case 0:
        *la = "时间";
        helm_fmt_time(va, 16, rt ? rt->session_elapsed_ms : 0);
        *lb = "里程";
        helm_fmt_km(vb, 16, dist_km);
        break;
    case 1:
        *la = "均速";
        helm_fmt_speed(va, 16, avg);
        *lb = "极速";
        helm_fmt_speed(vb, 16, rt ? rt->speed_max_kph : 0);
        break;
    case 2:
        *la = "心率";
        lv_snprintf(va, 16, "%s",
                    helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_bpm : 0));
        *lb = "均心率";
        lv_snprintf(vb, 16, "%s",
                    helm_dash(rt && rt->hr_avg_bpm > 0u, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->hr_avg_bpm : 0));
        break;
    case 3:
        *la = "海拔";
        lv_snprintf(va, 16, "%d", alt);
        /* 坡度保留：用户 2026-09-26 的海拔页图里有"坡度 (%)"，早先那句
         * "不需要坡度显示了"指的是**骑行页左卡别放坡度牌**，不是全线下线。 */
        *lb = "坡度";
        if (rt) {
            int g10 = (int)(rt->grade_pct * 10.0f);

            lv_snprintf(vb, 16, "%d.%d", g10 / 10,
                        (g10 < 0 ? -g10 : g10) % 10);
        } else {
            lv_snprintf(vb, 16, "--");
        }
        break;
    default:
        *la = "爬升";
        lv_snprintf(va, 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
        *lb = "功率";
        lv_snprintf(vb, 16, "%s",
                    helm_dash(rt && rt->power_valid, tmp, sizeof(tmp), "%u",
                              rt ? (int)rt->power_w : 0));
        break;
    }
}

static void helm_apply_view(void)
{
    helm_page_id_t id;
    helm_page_id_t prev;
    bool map_on;
    bool rot_on;
    bool nav_ban;
    bool anim;
    bool rot_was;
    bool pause_was;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0 = lv_tick_get();
#endif

    if (!s_sh.attached || s_sh.map == NULL) {
        return;
    }

    if (s_sh.ui_covered) {
        return;
    }

    id = helm_page_at(*helm_active_idx());
    prev = s_sh.view_id;
    anim = s_sh.view_anim && (prev != id);
    s_sh.view_anim = false;
    s_sh.view_id = id;
    /* 主页态 = LiveMap 页 + 底部信息卡（用户 2026-09-26："把这个下半部分的组件
     * 放到 livemap 界面，然后把 livemap 界面作为主界面"）⇒ 待机态也要显示地图。
     * 注：上一版曾把同样的改动误判成"上板花屏"的原因并回退——花屏其实是**面板
     * 硬件横线**，与这里无关。 */
    map_on = (id == HELM_PAGE_MAP || id == HELM_PAGE_STANDBY);
    /* 底栏三格：只在**地图页**出现。海拔页整页 296 都给了剖面卡 + 六格
     * （用户 2026-09-26 的图），80px 数据条会把剖面卡从 108 压到 56 ⇒ 去掉；
     * 转向页本身已不在轮转表里，留着判据无害。 */
    rot_on = (id == HELM_PAGE_MAP || id == HELM_PAGE_TURN);
    /* 路牌：导航中一直挂在左上角（小牌子不吃屏幕），切页时跟着显隐。 */
    nav_ban = map_on && map_page_nav_active(s_sh.map);
    rot_was = (s_sh.rotbar != NULL) &&
              !lv_obj_has_flag(s_sh.rotbar, LV_OBJ_FLAG_HIDDEN);
    pause_was = (s_sh.pause_dock != NULL) &&
                !lv_obj_has_flag(s_sh.pause_dock, LV_OBJ_FLAG_HIDDEN);

    /* 只改可见标志，不在 KEY 这一拍里 catchup/重绘地图。 */
    map_page_set_map_ui_visible(s_sh.map, map_on);

    if (anim) {
        lv_obj_t * panes[5];
        lv_obj_t * po = helm_pane_of(prev);
        lv_obj_t * pi = helm_pane_of(id);
        unsigned i;

        panes[0] = s_sh.standby;
        panes[1] = s_sh.data;
        panes[2] = s_sh.data_all;
        panes[3] = s_sh.turn;
        panes[4] = s_sh.climb;
        if (po == pi && po != NULL) {
            helm_pane_nudge(po, s_sh.view_dir);
        } else if (helm_riding()) {
            helm_pane_snap(po, pi);
            helm_riding_view_enter(s_sh.view_dir);
        } else {
            helm_pane_slide(po, pi, s_sh.view_dir);
        }

        for (i = 0; i < 5; i++) {
            if (panes[i] && panes[i] != po && panes[i] != pi) {
                helm_pane_reset(panes[i]);
                lv_obj_add_flag(panes[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        helm_pane_reset(s_sh.standby);
        helm_pane_reset(s_sh.data);
        helm_pane_reset(s_sh.data_all);
        helm_pane_reset(s_sh.turn);
        helm_pane_reset(s_sh.climb);
        /* 待机 pane 是**不透明满页底**，一旦显示就把地图盖住 ⇒ 永不再显示它。
         * 主页态改由底部信息卡承担（卡片挂在 root 上，见 helm_build_standby_card）。 */
        helm_show(s_sh.standby, false);
        helm_show(s_sh.data, id == HELM_PAGE_DATA);
        helm_show(s_sh.data_all, id == HELM_PAGE_DATA_ALL);
        helm_show(s_sh.turn, id == HELM_PAGE_TURN);
        helm_show(s_sh.climb, id == HELM_PAGE_CLIMB);
    }
    /* 主页态（待机）显示信息卡；骑行态下面是三格数据条（rotbar） */
    helm_show(s_sh.sb_card, id == HELM_PAGE_STANDBY);
    helm_show(s_sh.rotbar, rot_on);
    helm_show(s_sh.nav_ban, nav_ban);
    /* 左下两行 / 右下比例尺：切页时先收起，回到地图页由周期块按导航状态重新决定
     * （它们挂在 root 上，不收起来会飘到别的页面上）。 */
    helm_show(s_sh.nav_sum, false);
    helm_show(s_sh.nav_scale, false);
    helm_show(s_sh.pause_dock, s_sh.paused);
    {
        bool arrive = !s_sh.paused && !helm_save_open() &&
                      !s_sh.arrive_dismissed && map_page_nav_arrived(s_sh.map);
        bool arrive_was = (s_sh.arrive_dock != NULL) &&
                          !lv_obj_has_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);

        helm_show(s_sh.arrive_dock, arrive);
        if (arrive && !arrive_was) {
            myvendor_sound_arrive();
            helm_obj_stagger_in(s_sh.arrive_dock);
        }
    }
    /* 状态栏整条剔除（用户 2026-09-25：LAP 那一条没用还占 22 px）。对象仍建着、代码路径
     * 不变，只是永不显示；它承载的行程标签 / 计时读数如需保留，另行安排位置。 */
    helm_show(s_sh.subbar, false);
    if (id != HELM_PAGE_STANDBY) {
        helm_raise_if_shown(s_sh.subbar);
    }

    if (nav_ban) {
        helm_raise_if_shown(s_sh.nav_ban);
    }

    if (rot_on) {
        helm_raise_if_shown(s_sh.rotbar);
        if (!rot_was && !anim) {
            helm_obj_stagger_in(s_sh.rotbar);
        }
    }

    if (s_sh.paused) {
        helm_pause_title_sync();
        helm_raise_if_shown(s_sh.pause_dock);
        if (!pause_was) {
            helm_obj_stagger_in(s_sh.pause_dock);
        }
    }

    helm_raise_if_shown(s_sh.arrive_dock);

    if (helm_save_open()) {
        lv_obj_move_foreground(s_sh.save_mask);
    }

#if MYVENDOR_LVGL_STALL_LOG
    {
        uint32_t dt = lv_tick_elaps(t0);

        if (dt >= 20u) {
            LVGL_STALL("helm_apply_view %ums id=%u anim=%d",
                (unsigned)dt, (unsigned)id, (int)anim);
        }
    }
#endif
}

static uint32_t helm_gps_label(const bicycle_runtime_t * rt, char * buf, size_t n)
{
    uint8_t sats = rt ? rt->gnss_satellites : 0;
    uint8_t q = rt ? rt->gnss_fix_quality : 0;
    uint8_t h = rt ? rt->gnss_hdop_x10 : 0;
    bool valid = rt && rt->gnss_valid;
    bool alive = rt && rt->gnss_alive;
    bool eph = rt && rt->gnss_eph_busy;
    const char * conf = NULL;

    if (eph) {
        lv_snprintf(buf, n, "星历同步");
        return HELM_COLOR_WARN;
    }

    if (!valid) {
        if (!alive) {
            if (helm_idle_is_sleeping()) {
                lv_snprintf(buf, n, "无定位");
                return HELM_COLOR_INK;
            }

            lv_snprintf(buf, n, "无数据");
            return HELM_COLOR_GPS_DEAD;
        }

        if (sats == 0) {
            lv_snprintf(buf, n, "无定位");
            return HELM_COLOR_INK;
        }

        lv_snprintf(buf, n, "搜索 %u", (unsigned)sats);
        return HELM_COLOR_WARN;
    }

    if (h > 0) {
        if (h <= 10) {
            conf = "高";
        } else if (h <= 20) {
            conf = "中";
        } else {
            conf = "低";
        }
    }

    if (q <= 1) {
        if (conf != NULL) {
            lv_snprintf(buf, n, "2D %s", conf);
        } else {
            lv_snprintf(buf, n, "2D定位");
        }

        return (h > 0 && h <= 20) ? HELM_COLOR_INK : HELM_COLOR_WARN;
    }

    if (conf != NULL) {
        lv_snprintf(buf, n, "3D %s", conf);
    } else {
        lv_snprintf(buf, n, "3D定位");
    }

    return (h > 20) ? HELM_COLOR_INK : HELM_COLOR_CAD;
}

/**
 * @brief 刷新当前页数字、时速/心率三档色与走势。
 * @details 待机 / 数据页 / rotbar 的当前时速走 `helm_paint_speed_pair()` /
 *          `helm_live_speed_ink()`；心率走 `helm_live_hr_ink()`。
 *          均速 / 极速仍用导航橙。
 */
static void helm_refresh_numbers(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (s_sh.ui_covered) {
        return;
    }
    char buf[32];
    char line[40];
    const char * labs[HELM_RIDE_ROWS];
    char vals[HELM_RIDE_ROWS][16];
    const char * la;
    const char * lb;
    char va[16];
    char vb[16];
    int i;
    uint8_t n;
    helm_page_id_t id;
    float dist_km;
    float avg;

    if (!s_sh.attached) {
        return;
    }

    n = helm_page_count();
    {
        uint8_t * idx = helm_active_idx();

        if (*idx >= n) {
            *idx = (uint8_t)(n - 1u);
            helm_apply_view();
        }
    }

    id = helm_page_at(*helm_active_idx());
    dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;
    avg = helm_session_avg_kph(rt);

    /* Hidden helm pages only keep runtime/hist data. Draw the active page. */
    if (id == HELM_PAGE_STANDBY) {
        uint16_t kph10 = 0;
        uint8_t lock = 0;
        uint32_t mode_c = HELM_COLOR_INK;
        const char * mode = "0Hz";

        helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_kph : 0);
        helm_label_set(s_sh.speed_big, buf);

        helm_paint_speed_pair(s_sh.speed_big, s_sh.speed_unit);

        if (s_sh.gps_val) {
            uint32_t col = helm_gps_label(rt, buf, sizeof(buf));

            helm_label_set(s_sh.gps_val, buf);
            lv_obj_set_style_text_color(s_sh.gps_val, helm_color(col), 0);
        }

        if (rt) {
            int v = (int)(rt->speed_kph * 10.0f + 0.5f);

            kph10 = (v < 0) ? 0 : (uint16_t)v;
            if (rt->gnss_eph_busy) {
                lock = 1;
                mode = "星历同步";
                mode_c = HELM_COLOR_WARN;
            } else if (rt->gnss_valid) {
                lock = 2;
                lv_snprintf(buf, sizeof(buf), "%uHz", (unsigned)rt->gnss_rx_hz);
                mode = buf;
                mode_c = HELM_COLOR_GPS;
            } else if (rt->gnss_alive || rt->gnss_rx_hz > 0) {
                lock = 1;
                lv_snprintf(buf, sizeof(buf), "%uHz", (unsigned)rt->gnss_rx_hz);
                mode = buf;
                mode_c = HELM_COLOR_NAV;
            } else {
                lv_snprintf(buf, sizeof(buf), "%uHz", (unsigned)rt->gnss_rx_hz);
                mode = buf;
                mode_c = HELM_COLOR_INK;
            }
        }

        bool hero_dirty = (kph10 != s_sh.dial_kph10 || lock != s_sh.dial_lock);

        s_sh.dial_kph10 = kph10;
        s_sh.dial_lock = lock;
        helm_label_set(s_sh.hero_mode, mode);
        if (s_sh.hero_mode) {
            lv_obj_set_style_text_color(s_sh.hero_mode, helm_color(mode_c), 0);
        }

        if (s_sh.dial_poly_defer) {
            helm_hero_spin_set(false);
        } else {
            helm_hero_spin_set(lock == 1);
            if (s_sh.hero && hero_dirty) {
                lv_obj_invalidate(s_sh.hero);
            }
        }

        helm_label_set(s_sh.standby_hint,
                       map_page_nav_active(s_sh.map) ? "导航中" : "待开始");

        /* 新信息卡：40px 时钟 + 状态列三行（"待开始"标签已隐藏，用户 09-26
         * 说不需要显示；下面这行保留只为让旧对象保持有值，不显示） */
        helm_standby_card_tick(rt);

        if (s_sh.last_dist) {
            if (s_sh.last_km > 0.05f) {
                helm_fmt_km(buf, sizeof(buf), s_sh.last_km);
                lv_snprintf(line, sizeof(line), "%s km", buf);
                helm_label_set(s_sh.last_dist, line);
            } else {
                helm_label_set(s_sh.last_dist, "-- km");
            }
        }
    } else {
        helm_hero_spin_set(false);
    }

    if (id == HELM_PAGE_DATA) {
        /* 骑行页（原来的"心率页/速度页"已并进数据页，这里只剩一种形态）。 */
        helm_label_set(s_sh.data_title, "骑行");
        helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_kph : 0);
        helm_label_set(s_sh.data_speed, buf);
        helm_label_set(s_sh.data_unit, "km/h");
        helm_label_set(s_sh.data_hero_key, "速度");
        helm_paint_speed_pair(s_sh.data_speed, s_sh.data_unit);

        /* 页码 "n/6" 已删（用户 2026-09-26：右上角的页面位置指示不需要显示）。
         * 标签本身在 helm_build_data 里就没建 ⇒ data_mark 恒为 NULL。 */
        LV_UNUSED(n);

        /* 左栏量条：0~60 km/h 与时速表盘同量程（HELM_DIAL_MAX_KPH）。 */
        if (s_sh.ride_rail != NULL) {
            int32_t rw = HELM_RIDE_HERO_W - 24;
            float kph = rt ? rt->speed_kph : 0.0f;
            int32_t fw;

            if (kph < 0.0f) {
                kph = 0.0f;
            }
            if (kph > (float)HELM_DIAL_MAX_KPH) {
                kph = (float)HELM_DIAL_MAX_KPH;
            }
            fw = (int32_t)((float)rw * kph / (float)HELM_DIAL_MAX_KPH);
            lv_obj_set_width(s_sh.ride_rail, fw);
            lv_obj_set_style_bg_color(s_sh.ride_rail,
                helm_color(helm_zone_ink(helm_speed_zone(kph))), 0);
        }

        /* 右栏：5 行常驻（时间/里程/均速/心率 + 功率），不轮转。
         *
         * 第 5 行的显隐只看**功率计有没有绑定**，不看这一秒有没有数据。用户
         * 2026-09-26 原话："只要有绑定，无论是否连上都是 5 栏（绑定后也是从菜单
         * 回来，这个时候 4 变 5，删除同理）"。
         * 判据 = `myvendor_sys_sensor_bound(CPS)`（槽位里有保存的对端地址；
         * ble_sensor 的 ui 快照**绑定就发布地址**、与 link 状态无关，见
         * ble_sensor.c:3880）⇒ 停车断链、信号不好都不会让这一栏消失，只有删除
         * 绑定才收掉。没数据时值显示 "--"（helm_dash），不会空着。
         * 心率页 / 速度页（gi != 0）仍是 4 行：那两页的条目已经把各自域占满。 */
        helm_fill_group(0u, labs, vals);
        {
            /* 骑行页：组里第 0 项是"时间"，它已经挪到左卡的小结块里（同一页不
             * 出现两次）⇒ 右栏整体下移一位，最后一行留给功率。 */
            for (i = 0; i + 1u < 4u; i++) {
                labs[i] = labs[i + 1u];
                memcpy(vals[i], vals[i + 1u], sizeof(vals[0]));
            }
        }
        {
            /* 均速跟它那一行的背景 chart 挂钩（用户 2026-10-05）：显示值 =
             * chart 上那串时速度点的平均，不再走"总里程/总时间"。按标签找行，
             * 不写死下标（行序以后变了也不会默默错位）。 */
            for (i = 0; i + 1u < HELM_RIDE_PWR_ROW; i++) {
                if (helm_lab_eq(labs[i], "均速")) {
                    helm_fmt_speed(vals[i], sizeof(vals[0]),
                                   helm_spark_avg_kph());
                    break;
                }
            }
        }
        {
            /* 第 4 行：**骑行页**是功率（按"功率计绑定"显隐）；心率页/速度页
             * 是组里那一项（踏频 / 爬升），恒显示——它们没有可显隐的第 5 项，
             * 藏掉这一行等于把该项的读数丢掉。 */
            bool pwr_row = myvendor_sys_sensor_bound(
                MYVENDOR_SYS_SENSOR_KIND_CPS);


            if (s_sh.cell_box[HELM_RIDE_PWR_ROW] != NULL &&
                helm_obj_shown(s_sh.cell_box[HELM_RIDE_PWR_ROW]) != pwr_row) {
                helm_show(s_sh.cell_box[HELM_RIDE_PWR_ROW], pwr_row);
            }
            if (pwr_row) {
                char tmp_pw[16];

                labs[HELM_RIDE_PWR_ROW] = "功率";
                lv_snprintf(vals[HELM_RIDE_PWR_ROW],
                            sizeof(vals[0]), "%s",
                            helm_dash(rt != NULL && rt->power_valid,
                                      tmp_pw, sizeof(tmp_pw), "%u",
                                      rt ? (unsigned)rt->power_w : 0u));
            }
        }
        for (i = 0; i < HELM_RIDE_ROWS; i++) {
            uint32_t tone;
            uint32_t ink;

            if (s_sh.cell_box[i] == NULL ||
                !helm_obj_shown(s_sh.cell_box[i])) {
                continue;
            }
            tone = helm_tone(labs[i]);
            ink = helm_metric_ink(labs[i]);

            /* 标签一律 MUTE（色条负责身份）；数值一律 INK，只有**活体分区色**
             * 例外（"心率"这类 helm_metric_ink != helm_tone 的），因为那颜色
             * 本身带语义、是这一行最该被看见的信息。 */
            helm_label_set(s_sh.cell_lab[i], labs[i]);
            helm_set_text_hex(s_sh.cell_lab[i], HELM_COLOR_MUTE);
            if (s_sh.cell_rail[i] != NULL) {
                lv_obj_set_style_bg_color(s_sh.cell_rail[i],
                                          helm_color(tone), 0);
            }

            ink = helm_lab_eq(labs[i], "心率") ? helm_metric_ink(labs[i])
                                               : HELM_COLOR_INK;
            helm_label_set(s_sh.cell_val[i], vals[i]);
            helm_set_text_hex(s_sh.cell_val[i], ink);

            /* 默认不上底色；只有"这项数据高了"才整格半透红（用户 2026-09-26）。 */
            helm_ride_alert(s_sh.cell_box[i], helm_ride_alert_on(labs[i], rt));
        }
    }

    /* 左卡小结块：骑行时间 + 两项综合（爬升 / 坡度）。三个数据页共用这一块
     * （都是同一个会话的小结），每拍只刷文本——标签/单位/色条建好就不动。 */
    if (id == HELM_PAGE_DATA && s_sh.ride_trip_val != NULL) {
        char tbuf[16];
        float grade = rt ? rt->grade_pct : 0.0f;

        helm_fmt_time(tbuf, sizeof(tbuf), rt ? rt->session_elapsed_ms : 0ull);
        helm_label_set(s_sh.ride_trip_val, tbuf);
        lv_snprintf(tbuf, sizeof(tbuf), "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
        helm_label_set(s_sh.ride_trip_v[0], tbuf);
        /* 坡度：整数百分比，不带小数点（用户 2026-10-05）。num_* 字库没有
         * '-'（见海拔页同一处理），所以取绝对值 —— 方向看爬升/下降。 */
        if (grade < 0.0f) {
            grade = -grade;
        }
        lv_snprintf(tbuf, sizeof(tbuf), "%d", (int)(grade + 0.5f));
        helm_label_set(s_sh.ride_trip_v[1], tbuf);
    }

    if (id == HELM_PAGE_DATA_ALL) {
        const char * alabs[HELM_ALL_N];
        char avals[HELM_ALL_N][16];

        helm_fill_all(rt, alabs, avals);
        for (i = 0; i < HELM_ALL_N; i++) {
            helm_label_set(s_sh.all_lab[i], alabs[i]);
            helm_set_text_hex(s_sh.all_lab[i], HELM_COLOR_MUTE);
            if (s_sh.all_rail[i] != NULL) {
                lv_obj_set_style_bg_color(s_sh.all_rail[i],
                                          helm_color(helm_tone(alabs[i])), 0);
            }
            helm_label_set(s_sh.all_val[i], avals[i]);
            /* 数值一律 INK；只有"心率"跟着实时区间变色（骑行页同一条规则）。 */
            helm_set_text_hex(s_sh.all_val[i],
                helm_lab_eq(alabs[i], "心率") ? helm_metric_ink(alabs[i])
                                             : HELM_COLOR_INK);
        }
    }

    if (id == HELM_PAGE_CLIMB) {
        helm_climb_tick(rt, *helm_active_idx(), n);
    }

    if (id == HELM_PAGE_TURN && s_sh.turn_dist && s_sh.turn_sub && s_sh.turn_ico) {
        char ico[8];
        char dist[40];
        char sub[32];

        if (map_page_nav_turn_info(s_sh.map, ico, sizeof(ico), dist, sizeof(dist),
                                   sub, sizeof(sub))) {
            helm_label_set(s_sh.turn_ico, ico);
            helm_label_set(s_sh.turn_dist, dist);
            helm_label_set(s_sh.turn_sub, sub);
        } else {
            helm_label_set(s_sh.turn_ico, "↑");
            helm_label_set(s_sh.turn_dist, "--");
            helm_label_set(s_sh.turn_sub, "转向");
        }
    }

    if (helm_obj_shown(s_sh.rotbar)) {
        helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_kph : 0);
        helm_label_set(s_sh.rot_val[0], buf);
        helm_set_text_hex(s_sh.rot_val[0], helm_live_speed_ink());

        helm_fill_rot(s_sh.rot_idx, &la, va, &lb, vb);
        lv_snprintf(buf, sizeof(buf), "%s  %u/5", la,
                    (unsigned)(s_sh.rot_idx % 5u) + 1u);
        helm_label_set(s_sh.rot_lab[1], buf);
        helm_set_text_hex(s_sh.rot_lab[1], helm_metric_ink(la));

        {
            uint32_t fill = helm_tone_fill(la);
            bool spark = (fill != HELM_COLOR_PAPER);
            uint32_t ink = helm_metric_ink(la);

            if (spark && !helm_lab_eq(la, "心率")) {
                ink = HELM_COLOR_INK;
            }
            helm_label_set(s_sh.rot_val[1], va);
            helm_set_text_hex(s_sh.rot_val[1], ink);
        }

        helm_label_set(s_sh.rot_lab[2], lb);
        helm_set_text_hex(s_sh.rot_lab[2], helm_metric_ink(lb));

        {
            uint32_t fill = helm_tone_fill(lb);
            bool spark = (fill != HELM_COLOR_PAPER);
            uint32_t ink = helm_metric_ink(lb);

            if (spark && !helm_lab_eq(lb, "心率")) {
                ink = HELM_COLOR_INK;
            }
            helm_label_set(s_sh.rot_val[2], vb);
            helm_set_text_hex(s_sh.rot_val[2], ink);
        }
    }

    if (helm_obj_shown(s_sh.subbar)) {
        const char * tag = "DATA";
        uint32_t col = HELM_COLOR_INK;

        if (rt && rt->recording) {
            tag = "REC";
            col = HELM_COLOR_HR;
        } else if (s_sh.paused) {
            tag = "PAUSE";
        } else if (helm_nav_on()) {
            tag = "NAV";
            col = HELM_COLOR_NAV;
        } else if (id == HELM_PAGE_MAP) {
            tag = "MAP";
        }

        helm_label_set(s_sh.sub_left, tag);
        if (s_sh.sub_left) {
            lv_obj_set_style_text_color(s_sh.sub_left, helm_color(col), 0);
        }

        helm_fmt_hms(buf, sizeof(buf), rt ? rt->session_elapsed_ms : 0);
        helm_label_set(s_sh.sub_mid, buf);

        /* 顶栏右槽**不再**显示"当前导航信息"（站名 / 行程进度）：
         * 那串东西同时出现在 status 栏右槽、状态栏下的导航横幅（`nav_ban` 的
         * `dest`）、以及导航页正文的"大箭头 + 距离 + 方向"里 —— 用户 2026-09-25
         * 判定重复太多，这里只留 LAP 计数（与无导航时一致）。
         * `map_page_nav_status_text()` 仍被下面的横幅使用，别删那个函数。 */
        /* 顶栏右槽：导航中给"距下一路口 / 剩余里程"（用户 2026-09-25 要）—— 横幅按显示策略
         * 大部分时间不挂，这里补上那个空档的读数；无导航时仍是 LAP。
         * 这一槽走点阵字库，缺字会整块不显示，所以只用 ASCII（"→" 尽力而为；万一没有，
         * 还剩数字 + 橙色可区分）。 */
        {
            const double td = map_page_nav_turn_dist_m(s_sh.map);
            const double rm = map_page_nav_remain_m(s_sh.map);
            uint32_t col = HELM_COLOR_INK;

            if (td >= 0.0 && td <= 1000.0) {
                lv_snprintf(buf, sizeof(buf), "→ %d m", (int)(td + 0.5));
                col = HELM_COLOR_NAV;
            } else if (rm >= 0.0) {
                if (rm >= 1000.0) {
                    lv_snprintf(buf, sizeof(buf), "%.1f km", rm / 1000.0);
                } else {
                    lv_snprintf(buf, sizeof(buf), "%d m", (int)(rm + 0.5));
                }
            } else {
                /* LAP 栏剔除（用户 2026-09-25：判定无用，几个页面一起去掉）：
                 * 没有导航读数时这一槽留空，不再显示圈数。lap_count 的**记录**仍在，
                 * 骑行记录里还用它。 */
                buf[0] = '\0';
            }
            helm_label_set(s_sh.sub_right, buf);
            lv_obj_set_style_text_color(s_sh.sub_right, helm_color(col), 0);
        }
    }

/* 横幅四态自检（临时）：1 = 开机后每 3 秒轮播一遍 常态导航 / 转向 / 偏航 / 到达；
 * 0 = 关闭（用户 2026-09-25 已看完四态，恢复常态）。 */
#define HELM_BANNER_DEMO_ON_BOOT 0

#if VMAP_ROUTE_ENABLE
    /* 路牌可见性 + 左下两行/右下比例尺。`helm_apply_view()` 只在切页/事件时跑，导航刚起步
     * 那一下不会触发，所以这里每拍补一次（幂等：只在状态变化时改标志）。
     * 左下两行只在导航中才有意义；比例尺只看是不是地图页。 */
    if (s_sh.view_id == HELM_PAGE_MAP) {
        const bool nav_live = map_page_nav_active(s_sh.map) ||
                              (HELM_BANNER_DEMO_ON_BOOT != 0);

        /* 地图几何**不再**随导航状态变动（用户 2026-09-25：动态改几何导致指针与实际地图
         * 位置出现偏差，而且反复局部重绘会让已画好的导航线丢段）。恢复创建时的静态几何，
         * 横幅就覆盖在地图顶部。map_page_set_banner_inset() 保留但不再调用。 */
        const double mpp = map_page_map_mpp(s_sh.map);

        helm_show(s_sh.nav_ban, nav_live);
        helm_show(s_sh.nav_sum, nav_live);
        helm_show(s_sh.nav_scale, mpp > 0.0);

        if (nav_live && s_sh.nav_sum_l1) {
            const double rm = map_page_nav_remain_m(s_sh.map);
            const double tm = map_page_nav_total_m(s_sh.map);

            if (rm >= 1000.0) {
                /* ≥100km 不要小数（用户 2026-09-27，与 helm_fmt_km 同规则）。 */
                lv_snprintf(buf, sizeof(buf),
                    (rm >= 99950.0) ? "剩余里程 %.0fkm" : "剩余里程 %.1fkm",
                    rm / 1000.0);
            } else if (rm >= 0.0) {
                lv_snprintf(buf, sizeof(buf), "剩余里程 %.0fm", rm);
            } else {
                lv_snprintf(buf, sizeof(buf), "剩余里程 --");
            }
            helm_label_set(s_sh.nav_sum_l1, buf);
            if (tm >= 1000.0) {
                /* ≥100km 不要小数（同上）。 */
                lv_snprintf(buf, sizeof(buf),
                    (tm >= 99950.0) ? "总里程 %.0fkm" : "总里程 %.1fkm",
                    tm / 1000.0);
            } else if (tm >= 0.0) {
                lv_snprintf(buf, sizeof(buf), "总里程 %.0fm", tm);
            } else {
                lv_snprintf(buf, sizeof(buf), "总里程 --");
            }
            helm_label_set(s_sh.nav_sum_l2, buf);
        }
        if (mpp > 0.0 && s_sh.nav_scale_lab) {
            /* 标尺按 40 px 取"好看"的整值：1/2/5 × 10^k。 */
            const double raw = mpp * 40.0;
            double e = 1.0;
            double pick;

            while (e * 10.0 <= raw) {
                e *= 10.0;
            }
            pick = (raw < e * 1.5) ? e : ((raw < e * 3.5) ? e * 2.0 : e * 5.0);
            if (pick >= 1000.0) {
                lv_snprintf(buf, sizeof(buf), "%.0fkm", pick / 1000.0);
            } else {
                lv_snprintf(buf, sizeof(buf), "%.0fm", pick);
            }
            helm_label_set(s_sh.nav_scale_lab, buf);
        }
    }

    if (helm_obj_shown(s_sh.nav_ban)) {
        static const char spin[] = "|/-\\";
        bool arrived = map_page_nav_arrived(s_sh.map);
        bool replanning = map_page_nav_replanning(s_sh.map) && !arrived;
        bool warn = rt && rt->nav_off_route_m > map_page_nav_off_warn_m(s_sh.map)
                    && !arrived && !replanning;
        /* —— 横幅四态自检（HELM_BANNER_DEMO_ON_BOOT）：每 3 秒换一种状态，
         *    常态导航 / 转向 / 偏航 / 到达 轮播一遍，用来现场确认四态的观感。
         *    关掉把那个宏置 0 即可，不留运行时痕迹。 */
        static int s_banner_demo = HELM_BANNER_DEMO_ON_BOOT;
        static uint32_t s_banner_demo_ms;

        if (s_banner_demo != 0 &&
            (uint32_t)(lv_tick_get() - s_banner_demo_ms) > 3000u) {
            s_banner_demo_ms = (uint32_t)lv_tick_get();
            s_banner_demo = (s_banner_demo >= 4) ? 1 : s_banner_demo + 1;
        }
        if (s_banner_demo != 0) {
            arrived = (s_banner_demo == 4);
            replanning = false;
            warn = (s_banner_demo == 3);
        }
        /* 回退到第二张参照图那版（用户 2026-09-25）：淡彩底 + 黑/灰字 + 淡橙徽章反白箭头。 */
        /* 底色：中性深灰半透 + 反白字（用户 2026-09-25："横幅底色白色比较丑"）。
         * 淡彩在这块屏上会洗成白、整卡高饱和又太跳；状态色只留在徽章上，
         * 偏航/到达另靠文案（"偏航 120 m" / "已到达"）区分。与通知弹窗同一套观感。 */
        /* 随状态变的**只有横幅背景**（中浅蓝 / 中浅红 / 中浅绿）；徽章固定淡橙 + 白箭头，
         * 字体固定黑/灰 —— 用户 2026-09-25 对着照片定：徽章与字体颜色始终不变。 */
        uint32_t card = arrived ? HELM_COLOR_BAN_ARR_FILL :
                        ((warn || replanning) ? HELM_COLOR_BAN_OFF_FILL
                                              : HELM_COLOR_BAN_NAV_FILL);
        /* 徽章底色加深一档（用户 2026-09-25："徽章背景颜色太浅"）—— 用饱和橙 + 白箭头。 */
        uint32_t badge = HELM_COLOR_NAV;
        uint32_t badge_fg = HELM_COLOR_PAPER;
        uint32_t pri = HELM_COLOR_INK;
        uint32_t sec = (warn || replanning) ? HELM_COLOR_INK : HELM_COLOR_MUTE;
        char ico[8];
        char dist[40];
        char sub[32];
        char dest[32];

        if (!arrived) {
            s_sh.arrive_dismissed = false;
        }

        if (map_page_nav_turn_info(s_sh.map, ico, sizeof(ico), dist, sizeof(dist),
                                   sub, sizeof(sub))) {
            helm_label_set(s_sh.nav_ban_ico, ico);
            /* 距离与动作**分两个标签**：主字（墨色 22px）+ 次字（灰 15px），
             * 不再把"距离 · 动作"塞成一句（sub 为空时次字自动不占宽度）。 */
            /* 数字必须带语义（用户 2026-09-25："显示 3.2km 不知道是做什么的"）：
             *  ≥1km 且下一步是直行 ⇒ "保持直行 3.2km"（这时骑手要知道"一直骑"）；
             *  其余 ≥1km          ⇒ "3.2km 后左转"；
             *  <1km               ⇒ "300m 后左转"（数字在前，便于临近时扫）；
             *  没有转向点          ⇒ "剩余 12.3km"（明确是剩余，不是距路口）。
             *  动作词由转向箭头映射（← → U ↑），不额外引入数据。 */
            {
                const double td = map_page_nav_turn_dist_m(s_sh.map);
                const double rm = map_page_nav_remain_m(s_sh.map);
                const char *act = "直行";

                if (strcmp(ico, "←") == 0) {
                    act = "左转";
                } else if (strcmp(ico, "→") == 0) {
                    act = "右转";
                } else if (strcmp(ico, "U") == 0) {
                    act = "掉头";
                }

                if (td >= 1000.0) {
                    if (strcmp(act, "直行") == 0) {
                        lv_snprintf(dist, sizeof(dist),
                            (td >= 99950.0) ? "保持直行 %.0fkm" : "保持直行 %.1fkm",
                            td / 1000.0);
                    } else {
                        lv_snprintf(dist, sizeof(dist), "%.1fkm 后%s", td / 1000.0, act);
                    }
                } else if (td >= 0.0) {
                    lv_snprintf(dist, sizeof(dist), "%.0fm 后%s", td, act);
                } else if (rm >= 1000.0) {
                    lv_snprintf(dist, sizeof(dist),
                        (rm >= 99950.0) ? "剩余 %.0fkm" : "剩余 %.1fkm",
                        rm / 1000.0);
                } else if (rm >= 0.0) {
                    lv_snprintf(dist, sizeof(dist), "剩余 %.0fm", rm);
                } else {
                    lv_snprintf(dist, sizeof(dist), "导航中");
                }
            }
            helm_label_set(s_sh.nav_ban_m, dist);
            /* 参照图只有"箭头 + 距离"两样，动作次字不再显示（路牌也窄）。 */
            helm_label_set(s_sh.nav_ban_act, "");
        } else {
            helm_label_set(s_sh.nav_ban_ico, "↑");
            helm_label_set(s_sh.nav_ban_m, "--");
            helm_label_set(s_sh.nav_ban_act, "");
            sub[0] = '\0';
        }

        /* 目的地行回来了（通栏放得下；参照图第二张就是这个样式）。 */
        if (map_page_nav_status_text(s_sh.map, dest, sizeof(dest))
            && dest[0] != '\0') {
            lv_snprintf(line, sizeof(line), "去 %s", dest);
            helm_label_set(s_sh.nav_ban_rd, line);
        } else {
            helm_label_set(s_sh.nav_ban_rd, "");
        }
        /* 自检态的文案（覆盖真实取数，保证四态都能看清样子）。 */
        if (s_banner_demo != 0) {
            static const char *const d_ico[5] = { "↑", "↑", "←", "!", "●" };
            static const char *const d_txt[5] = { "", "12.3km", "228m", "偏航 120m",
                                                  "已到达" };

            helm_label_set(s_sh.nav_ban_ico, d_ico[s_banner_demo]);
            helm_label_set(s_sh.nav_ban_m, d_txt[s_banner_demo]);
            helm_label_set(s_sh.nav_ban_act, "");
            helm_label_set(s_sh.nav_ban_rd,
                           s_banner_demo == 4 ? "终点：滁州大道"
                                              : "去 20260529户外骑行");
        }

        lv_obj_set_style_bg_color(s_sh.nav_ban, helm_color(card), 0);
        lv_obj_set_style_bg_color(s_sh.nav_ban_badge, helm_color(badge), 0);
        if (s_sh.nav_ban_ico) {
            /* 徽章：状态彩底 + 反白图标（回退版配色）。 */
            lv_obj_set_style_text_color(s_sh.nav_ban_ico, helm_color(badge_fg), 0);
            if (arrived) {
                helm_label_set(s_sh.nav_ban_ico, "●");
            } else if (replanning) {
                const uint32_t phase = (lv_tick_get() / HELM_UI_MS) & 3u;

                line[0] = spin[phase];
                line[1] = '\0';
                helm_label_set(s_sh.nav_ban_ico, line);
            } else if (warn) {
                helm_label_set(s_sh.nav_ban_ico, "!");
            }
        }

        if (s_sh.nav_ban_m) {
            lv_obj_set_style_text_color(s_sh.nav_ban_m, helm_color(pri), 0);
            lv_obj_set_style_text_color(s_sh.nav_ban_act, helm_color(sec), 0);
            lv_obj_set_style_text_color(s_sh.nav_ban_rd, helm_color(sec), 0);
            if (arrived) {
                helm_label_set(s_sh.nav_ban_m, "已到达");
                helm_label_set(s_sh.nav_ban_act, "");
            } else if (replanning) {
                helm_label_set(s_sh.nav_ban_m, "正在重新规划路线");
                helm_label_set(s_sh.nav_ban_act, "");
            } else if (warn) {
                lv_snprintf(line, sizeof(line), "偏航 %d m",
                            (int)(rt->nav_off_route_m + 0.5));
                helm_label_set(s_sh.nav_ban_m, line);
                helm_label_set(s_sh.nav_ban_act, "");
            }
        }

        if (s_sh.nav_ban_rd) {
            lv_obj_set_style_text_color(s_sh.nav_ban_rd,
                                        helm_color(HELM_COLOR_INK), 0);
            if (arrived) {
                helm_label_set(s_sh.nav_ban_rd, sub[0] != '\0' ? sub : "终点");
            } else if (warn) {
                helm_label_set(s_sh.nav_ban_rd, "其后途经点保留");
            }
        }
    }
#endif

    if (helm_obj_shown(s_sh.pause_dock)) {
        helm_fmt_hms(buf, sizeof(buf), rt ? rt->session_elapsed_ms : 0);
        helm_label_set(s_sh.pause_time, buf);

        helm_fmt_km(buf, sizeof(buf), dist_km);
        lv_snprintf(line, sizeof(line), "%s km", buf);
        helm_label_set(s_sh.pause_dist, line);

        helm_fmt_speed(buf, sizeof(buf), avg);
        helm_label_set(s_sh.pause_avg, buf);

        if (rt && rt->hr_avg_bpm > 0u) {
            lv_snprintf(buf, sizeof(buf), "%u", (unsigned)rt->hr_avg_bpm);
            helm_label_set(s_sh.pause_hr, buf);
        } else {
            helm_label_set(s_sh.pause_hr, "--");
        }
    }

    if (helm_save_open()) {
        helm_fmt_hms(buf, sizeof(buf), rt ? rt->session_elapsed_ms : 0);
        helm_label_set(s_sh.save_time, buf);

        helm_fmt_km(buf, sizeof(buf), dist_km);
        lv_snprintf(line, sizeof(line), "%s km", buf);
        helm_label_set(s_sh.save_dist, line);

        helm_fmt_speed(buf, sizeof(buf), avg);
        helm_label_set(s_sh.save_avg, buf);

        lv_snprintf(buf, sizeof(buf), "%d m",
                    rt ? (int)(rt->gain_m + 0.5f) : 0);
        helm_label_set(s_sh.save_gain, buf);

        helm_fmt_speed(buf, sizeof(buf), rt ? rt->speed_max_kph : 0);
        helm_label_set(s_sh.save_max, buf);

        if (rt && rt->hr_avg_bpm > 0u) {
            lv_snprintf(buf, sizeof(buf), "%u", (unsigned)rt->hr_avg_bpm);
            helm_label_set(s_sh.save_hr, buf);
        } else {
            helm_label_set(s_sh.save_hr, "--");
        }
    }
}

static bool helm_save_open(void)
{
    return s_sh.save_mask && !lv_obj_has_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
}

static void helm_save_hide(void)
{
    if (s_sh.save_mask) {
        lv_obj_add_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool helm_arrive_open(void)
{
    return s_sh.arrive_dock && !lv_obj_has_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);
}

static void helm_arrive_hide(void)
{
    s_sh.arrive_dismissed = true;
    if (s_sh.arrive_dock) {
        lv_obj_add_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);
    }
}

static void helm_arrive_confirm(void)
{
    helm_arrive_hide();
#if VMAP_ROUTE_ENABLE
    map_page_nav_stop(s_sh.map);
#endif
}

static void helm_on_prev(void * ud)
{
    uint8_t n = helm_page_count();
    uint8_t * idx;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0;
#endif

    LV_UNUSED(ud);
    if (!s_sh.attached) {
        LVGL_STALL("helm_on_prev drop !attached covered=%d",
            (int)s_sh.ui_covered);
        return;
    }

#if MYVENDOR_LVGL_STALL_LOG
    t0 = lv_tick_get();
#endif
    if (helm_save_open()) {
        helm_obj_press(s_sh.save_mask);
        helm_discard_ride();
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        myvendor_sound_back();
        return;
    }

    /* pause dock: KEY1 = 继续骑 (short or long) */
    if (s_sh.paused) {
        helm_obj_press(s_sh.pause_dock);
        helm_pause_resume_by_key();
        return;
    }

    if (helm_flip_busy()) {
        LVGL_STALL("helm_on_prev drop busy");
        return;
    }

    idx = helm_active_idx();
    *idx = (uint8_t)((*idx + n - 1u) % n);
    s_sh.view_dir = -1;
    s_sh.view_anim = true;
    LVGL_STALL("helm_on_prev view=%u/%u riding=%d",
           (unsigned)*idx + 1u, (unsigned)n, helm_riding() ? 1 : 0);
    helm_apply_view();
    helm_flip_arm(helm_riding() ? HELM_FLIP_GUARD_MS : HELM_VIEW_MS);
    helm_nums_defer_begin();
    LVGL_STALL("helm_on_prev done %ums",
        (unsigned)lv_tick_elaps(t0));
}

static void helm_save_clicked(lv_event_t * e)
{
    LV_UNUSED(e);
    helm_finish_ride(true);
    /* 骑行结束 = 记录数变了。菜单那边平时只读缓存（用户 2026-09-26：
     * "开机、骑行结束、进入骑行记录"三处才更新），所以这里显式失效一次。 */
    helm_gpx_count_invalidate();
}

static void helm_prompt_save(void)
{
    if (s_sh.save_mask) {
        helm_refresh_numbers();
        lv_obj_clear_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_sh.save_mask);
        helm_obj_stagger_in(s_sh.save_mask);
        myvendor_sound_prompt();
    }
}

static void helm_on_next(void * ud)
{
    uint8_t n = helm_page_count();
    uint8_t * idx;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0;
#endif

    LV_UNUSED(ud);
    if (!s_sh.attached) {
        LVGL_STALL("helm_on_next drop !attached covered=%d",
            (int)s_sh.ui_covered);
        return;
    }

#if MYVENDOR_LVGL_STALL_LOG
    t0 = lv_tick_get();
#endif
    if (helm_save_open()) {
        helm_obj_press(s_sh.save_mask);
        helm_save_clicked(NULL);
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        return;
    }

    /* pause dock: KEY2 = 结束 → 保存/不保存 */
    if (s_sh.paused) {
        helm_obj_press(s_sh.pause_dock);
        helm_prompt_save();
        return;
    }

    if (helm_flip_busy()) {
        LVGL_STALL("helm_on_next drop busy");
        return;
    }

    idx = helm_active_idx();
    *idx = (uint8_t)((*idx + 1u) % n);
    s_sh.view_dir = 1;
    s_sh.view_anim = true;
    LVGL_STALL("helm_on_next view=%u/%u riding=%d",
           (unsigned)*idx + 1u, (unsigned)n, helm_riding() ? 1 : 0);
    helm_apply_view();
    helm_flip_arm(helm_riding() ? HELM_FLIP_GUARD_MS : HELM_VIEW_MS);
    helm_nums_defer_begin();
    LVGL_STALL("helm_on_next done %ums",
        (unsigned)lv_tick_elaps(t0));
}

static void helm_on_menu(void * ud)
{
    LV_UNUSED(ud);
    if (!s_sh.attached) {
        return;
    }

    if (helm_save_open()) {
        helm_save_hide();
        myvendor_sound_back();
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        myvendor_sound_back();
        return;
    }

    if (s_sh.paused) {
        printf("helm: KEY1 long menu (paused)\n");
        myvendor_sound_long();
        if (helm_menu_open() != 0) {
            printf("helm: menu open failed\n");
        }
        return;
    }

    printf("helm: KEY1 long menu\n");
    myvendor_sound_long();
    if (helm_menu_open() != 0) {
        printf("helm: menu open failed\n");
    }
}

void helm_shell_toggle_ride(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (!s_sh.attached) {
        printf("helm: KEY2 long ignored (not attached)\n");
        return;
    }

    if (helm_save_open()) {
        helm_save_clicked(NULL);
        return;
    }

    if (helm_arrive_open()) {
        helm_arrive_confirm();
        return;
    }

    printf("helm: KEY2 long ride toggle rec=%d paused=%d home=%u ride=%u\n",
           (rt && rt->recording) ? 1 : 0, s_sh.paused ? 1 : 0,
           (unsigned)s_sh.home_idx, (unsigned)s_sh.ride_idx);

    if (s_sh.paused) {
        helm_prompt_save();
        return;
    }

    if (rt && rt->recording) {
        map_page_pause_ride(s_sh.map);
        s_sh.pause_auto = false;
        helm_shell_set_paused(true);
        return;
    }

    helm_enter_ride();
}

static void helm_on_rec(void * ud)
{
    LV_UNUSED(ud);
    helm_shell_toggle_ride();
}

bool helm_shell_nav_ban_shown(void)
{
    return s_sh.nav_ban != NULL && helm_obj_shown(s_sh.nav_ban);
}

void helm_shell_on_nav_started(void)
{
    if (!s_sh.attached) {
        return;
    }

    /* 导航不自动开 REC。切到骑行地图页（骑行态表里地图是 [2]）。 */
    s_sh.ride_idx = 2;
    s_sh.arrive_dismissed = false;
    if (s_sh.ui_covered) {
        return;
    }

    helm_apply_view();
    helm_refresh_numbers();
}

static void helm_rot_cb(lv_timer_t * t)
{
    helm_page_id_t id;

    LV_UNUSED(t);
    if (s_sh.ui_covered || !s_sh.attached) {
        return;
    }

    id = helm_page_at(*helm_active_idx());
    if (id == HELM_PAGE_MAP || id == HELM_PAGE_TURN ||
        id == HELM_PAGE_CLIMB) {
        s_sh.rot_idx = (uint8_t)((s_sh.rot_idx + 1u) % 5u);
    } else {
        return;
    }

    helm_refresh_numbers();
}

static void helm_ui_cb(lv_timer_t * t)
{
    bool added;
    helm_page_id_t id;
    bicycle_gnss_fix_t fix;
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0 = lv_tick_get();
#endif

    LV_UNUSED(t);
    helm_idle_tick();
    helm_pwr_bat_tick();
    /* 主界面自己取 GNSS：不能只靠地图 gpx_timer。启动延迟创建会把
     * 那个定时器 pause，关 splash 若没再 will_appear，就会一直「无数据」。 */
    (void)bicycle_runtime_poll_fix(&fix);
    bicycle_runtime_tick(lv_tick_get());
    bicycle_runtime_update_gnss(&fix);
    helm_shell_autopause_tick();
    if (s_sh.ui_covered) {
        return;
    }

    /* Always ingest ride data; only the visible page is drawn. */
    helm_avg_sample(bicycle_runtime_get());
    added = helm_hist_push();
    helm_rec_ele_push();

    id = s_sh.view_id;
    if (!helm_flip_busy()) {
        helm_refresh_numbers();
        if (helm_page_is_data(id) || helm_obj_shown(s_sh.rotbar)) {
            helm_spark_sync(added);
        }
    }

#if MYVENDOR_LVGL_STALL_LOG
    {
        uint32_t dt = lv_tick_elaps(t0);

        if (dt >= 20u) {
            LVGL_STALL("helm_ui_cb %ums id=%u",
                (unsigned)dt, (unsigned)id);
        }
    }
#endif
}

static lv_obj_t * helm_speed_head(lv_obj_t * parent, const char * title,
                                  lv_obj_t ** title_obj, lv_obj_t ** mark)
{
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_t * t;

    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_SPEED_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(head, 10, 0);
    t = helm_lab(head, s_sh.font_lab, HELM_COLOR_INK, title);
    if (title_obj) {
        *title_obj = t;
    }

    if (mark) {
        *mark = helm_lab(head, s_sh.font_lab, HELM_COLOR_INK, "1/5");
    }
    return head;
}

/** @brief X1「仪表台」右栏的一行：左缘 3px 色条 + 标签（MUTE）+ 数值（num_28）。
 *
 * 标签上 / 数值下：右栏只有 98px 宽，"时间 52:18"并排要 24 + 74 + 边距 > 98。
 * 行间发丝线做在行自己的**底边**上——flex 列里再塞一个 1px 对象会被 20% 分掉高度。
 */
static lv_obj_t * helm_ride_row(lv_obj_t * parent, int idx)
{
    lv_obj_t * r = lv_obj_create(parent);
    lv_obj_t * col;

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    /* 高度走 flex_grow（不是 lv_pct）：
     * 功率计不在时第 5 行隐藏，剩下 4 行要自己长满卡片——百分比不会重算。 */
    helm_grow_y(r);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_set_style_pad_left(r, 6, 0);
    lv_obj_set_style_pad_right(r, 6, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    if (idx < HELM_RIDE_ROWS - 1) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, helm_color(HELM_COLOR_HAIR), 0);
    } else {
        /* 第 5 行（功率）原来默认整行淡底做身份标记——用户 2026-09-26 要求撤掉：
         * "默认都不颜色，只有数据高了才半透红"。底色改由 helm_ride_alert()
         * 按数值刷（帧内带比较，不每拍标脏）。圆角先留着，方便告警底色成形。 */
        lv_obj_set_style_radius(r, HELM_RADIUS - 4, 0);
    }

    s_sh.cell_box[idx] = r;
    s_sh.cell_rail[idx] = lv_obj_create(r);
    lv_obj_remove_style_all(s_sh.cell_rail[idx]);
    lv_obj_set_size(s_sh.cell_rail[idx], 3, HELM_RIDE_RAIL_H);
    lv_obj_set_style_radius(s_sh.cell_rail[idx], 2, 0);
    lv_obj_set_style_bg_opa(s_sh.cell_rail[idx], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_sh.cell_rail[idx],
                              helm_color(HELM_COLOR_MUTE), 0);
    lv_obj_clear_flag(s_sh.cell_rail[idx], LV_OBJ_FLAG_CLICKABLE);

    col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    s_sh.cell_lab[idx] = helm_lab(col, s_sh.font_lab, HELM_COLOR_MUTE, "--");
    /* 数值走 num_28 不是 num_32：num_32 的 "52:18" 实测 86px，加左缘色条后
     * 超出 98px 卡片的内宽（83px）。 */
    s_sh.cell_val[idx] = helm_lab(col, s_sh.font_rot, HELM_COLOR_INK, "--");
    /* 每行挂一根走势，**图占满整行 = 数字背后的背景层**（用户 2026-09-26：
     * "均速 心率 功率，背景都是有一个 chart 的"）。序列由**标签文本**决定
     * （helm_hist_id，见 helm_spark_sync 每拍按标签重读 id），取不到历史的行
     * （时间/里程）不出图。图是 FLOATING + move_background ⇒ 不参与 flex、
     * 也不盖住 col 里的标签与数值。 */
    helm_spark_bind(r, s_sh.cell_lab[idx], HELM_RIDE_ROW_SPARK_H);
    return r;
}

/** @brief 这一行的"数据高了"判据。只看带语义的三项，时间/里程没有高低之分。
 * @return true 则整行半透红告警。阈值见 HELM_ALERT_*。
 */
static bool helm_ride_alert_on(const char * lab, const bicycle_runtime_t * rt)
{
    if (lab == NULL || rt == NULL) {
        return false;
    }

    if (helm_lab_eq(lab, "心率")) {
        return rt->hr_valid && rt->hr_bpm >= HELM_ALERT_HR_BPM;
    }

    if (helm_lab_eq(lab, "功率")) {
        return rt->power_valid && rt->power_w >= HELM_ALERT_PWR_W;
    }

    if (helm_lab_eq(lab, "均速")) {
        /* 判据跟显示值同源：这一行显示的是 chart 点均值（见刷新那一段），
         * 告警就必须按同一个数判，否则会出现"显示没超、底色却红了"。 */
        return helm_spark_avg_kph() >= HELM_ALERT_SPEED_KPH;
    }

    return false;
}

/** @brief 行底告警底色：默认透明，越阈值时整格半透红。
 * @note 带**比较**再设样式：这个块有 98x51，每拍无脑 set 会把它一直标脏。
 */
static void helm_ride_alert(lv_obj_t * row, bool on)
{
    lv_opa_t want = on ? LV_OPA_30 : LV_OPA_TRANSP;

    if (row == NULL || lv_obj_get_style_bg_opa(row, 0) == want) {
        return;
    }

    lv_obj_set_style_bg_opa(row, want, 0);
    if (on) {
        lv_obj_set_style_bg_color(row, helm_color(HELM_COLOR_HR), 0);
    }
}

/** @brief 右栏卡片：5 行常驻数据（时间/里程/均速/心率/功率），不轮转；
 *  每行底部一条该指标的走势。第 5 行（功率）没传感器时隐藏。 */
static lv_obj_t * helm_ride_rows(lv_obj_t * parent)
{
    lv_obj_t * card = lv_obj_create(parent);
    int i;

    lv_obj_remove_style_all(card);
    helm_style_card(card);
    helm_grow_x(card);
    lv_obj_set_height(card, lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < HELM_RIDE_ROWS; i++) {
        helm_ride_row(card, i);
    }

    return card;
}

/** @brief 左栏：时速锁死（最大字号、不参与轮转）+ 量条 + 走势。 */
static lv_obj_t * helm_ride_hero(lv_obj_t * parent)
{
    /* ⚠ 不能是 static const：调色板改成运行期查表后，这两个宏不再是常量表达式。
     * 每次建卡读一次即可（爬升=青 / 坡度=青，与 helm_tone() 那一套身份色一致：
     * 坡度属于海拔族）。 */
    const uint32_t trip_tone[HELM_RIDE_TRIP_N] = {
        HELM_COLOR_CLIMB, HELM_COLOR_CLIMB
    };
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_t * rail;
    lv_obj_t * box;
    int i;

    lv_obj_remove_style_all(card);
    helm_style_card(card);
    lv_obj_set_size(card, HELM_RIDE_HERO_W, lv_pct(100));
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_sh.data_speed = helm_lab(card, s_sh.font_speed_ride, HELM_COLOR_NAV, "0.0");
    lv_obj_set_width(s_sh.data_speed, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.data_speed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_sh.data_speed, -1, 0);
    lv_obj_align(s_sh.data_speed, LV_ALIGN_TOP_MID, 0, 8);
    s_sh.data_unit = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE, "km/h");
    lv_obj_set_width(s_sh.data_unit, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.data_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_sh.data_unit, s_sh.data_speed, LV_ALIGN_OUT_BOTTOM_MID,
                    0, 2);

    /* 量条：0~60 km/h，与时速表盘同一量程。轨道 + 填充两个对象——条高只有
     * 5px，lv_bar 的内边距会把可见高度吃掉。 */
    rail = lv_obj_create(card);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, HELM_RIDE_HERO_W - 24, 5);
    lv_obj_align_to(rail, s_sh.data_unit, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
    lv_obj_set_style_radius(rail, 2, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rail, helm_color(HELM_COLOR_TRACK), 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    s_sh.ride_rail = lv_obj_create(rail);
    lv_obj_remove_style_all(s_sh.ride_rail);
    lv_obj_set_size(s_sh.ride_rail, 0, lv_pct(100));
    lv_obj_align(s_sh.ride_rail, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_sh.ride_rail, 2, 0);
    lv_obj_set_style_bg_opa(s_sh.ride_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_sh.ride_rail, helm_color(HELM_COLOR_CAD), 0);
    lv_obj_clear_flag(s_sh.ride_rail, LV_OBJ_FLAG_CLICKABLE);

    /* 下半段 = "骑行时间 + 综合数据"小结块（用户 2026-09-26："给骑行时间还有
     * 一些综合数据…（不设计为纯数字的东西）"）。
     *
     * 为什么不是走势：速度恒 0 时走势贴在盒子最底边、看起来就是一块空白；
     * 而时间/爬升/坡度永远有值有变化。**这一页不再有速度曲线**。
     *
     * 排版（左卡内 x 从 12 起、宽 100）：
     *   时钟字形 16 + "时间"（12px MUTE）
     *   时间值 num_32（"72:40" 实测 86 ≤ 100）
     *   发丝线
     *   2 项综合：3px 色条 + 标签 MUTE + 数值 num_28 右对齐 + 单位 MUTE
     * 每拍只刷数值文本，"标签/单位/色条"建好就不动。
     */
    s_sh.ride_trip_ico = helm_lab(card, &helm_fa_16, HELM_COLOR_MUTE,
                                  HELM_FA_CLOCK);
    lv_obj_align(s_sh.ride_trip_ico, LV_ALIGN_TOP_LEFT, 12, HELM_RIDE_TRIP_Y);
    s_sh.ride_trip_lab = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE, "时间");
    lv_obj_align(s_sh.ride_trip_lab, LV_ALIGN_TOP_LEFT, 32,
                 HELM_RIDE_TRIP_Y + 2);
    s_sh.ride_trip_val = helm_lab(card, s_sh.font_quad, HELM_COLOR_INK, "0:00");
    lv_obj_align(s_sh.ride_trip_val, LV_ALIGN_TOP_LEFT, 12,
                 HELM_RIDE_TRIP_Y + 18);
    box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, HELM_RIDE_HERO_W - 24, 1);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 12, HELM_RIDE_TRIP_Y + 57);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    for (i = 0; i < HELM_RIDE_TRIP_N; i++)
        {
            lv_coord_t y = HELM_RIDE_TRIP_Y + 62 + i * 30;

            s_sh.ride_trip_rail[i] = lv_obj_create(card);
            lv_obj_remove_style_all(s_sh.ride_trip_rail[i]);
            lv_obj_set_size(s_sh.ride_trip_rail[i], 3, 18);
            lv_obj_align(s_sh.ride_trip_rail[i], LV_ALIGN_TOP_LEFT, 12, y + 6);
            lv_obj_set_style_radius(s_sh.ride_trip_rail[i], 2, 0);
            lv_obj_set_style_bg_opa(s_sh.ride_trip_rail[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_sh.ride_trip_rail[i],
                                      helm_color(trip_tone[i]), 0);
            lv_obj_clear_flag(s_sh.ride_trip_rail[i], LV_OBJ_FLAG_CLICKABLE);

            /* 第 2 项原来放的是"极速"，用户 2026-10-05 要求换成"坡度"
             * （不要小数点）—— 极速在保存小结卡 / 数据页仍有，本页不再重复。 */
            s_sh.ride_trip_k[i] = helm_lab(card, s_sh.font_lab,
                                           HELM_COLOR_MUTE,
                                           (i == 0) ? "爬升" : "坡度");
            lv_obj_align(s_sh.ride_trip_k[i], LV_ALIGN_TOP_LEFT, 20, y + 9);

            /* 值右对齐、单位贴右端：宽度不写死，让"值的右边缘"跟着单位走
             * （状态列那次的教训：写死数字会随数据变而错位）。 */
            s_sh.ride_trip_u[i] = helm_lab(card, s_sh.font_lab,
                                           HELM_COLOR_MUTE,
                                           (i == 0) ? "m" : "%");
            lv_obj_align(s_sh.ride_trip_u[i], LV_ALIGN_TOP_LEFT,
                         HELM_RIDE_TRIP_UNIT_X, y + 14);
            /* 值用**定宽 + 右对齐**的盒子，不用 align_to：文本一变宽 align_to
             * 不会重算（右边缘会跟着跑），定宽盒子天然钉住右边缘。 */
            s_sh.ride_trip_v[i] = helm_lab(card, s_sh.font_rot,
                                           HELM_COLOR_INK, "--");
            lv_obj_set_width(s_sh.ride_trip_v[i], HELM_RIDE_TRIP_VAL_W);
            lv_obj_set_style_text_align(s_sh.ride_trip_v[i],
                                        LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(s_sh.ride_trip_v[i], LV_ALIGN_TOP_LEFT,
                         HELM_RIDE_TRIP_VAL_X, y + 1);
        }

    lv_obj_move_foreground(s_sh.data_speed);
    lv_obj_move_foreground(s_sh.data_unit);
    return card;
}

static const uint8_t helm_poly_nx[HELM_POLY_N] = {
    0, 18, 36, 60, 78, 96, 110, 120
};
static const uint8_t helm_poly_ny[HELM_POLY_N] = {
    18, 8, 14, 0, 11, 6, 16, 12
};

static int16_t helm_dial_span_now(void)
{
    int16_t span;

    if (s_sh.dial_lock == 1) {
        span = (int16_t)s_sh.dial_phase;
    } else {
        span = (int16_t)((HELM_DIAL_SPAN * (int)s_sh.dial_kph10)
                         / (HELM_DIAL_MAX_KPH * 10));
    }

    if (span < 0) {
        return 0;
    }

    if (span > HELM_DIAL_SPAN) {
        return HELM_DIAL_SPAN;
    }

    return span;
}

static int32_t helm_poly_hypot(int32_t dx, int32_t dy)
{
    return (int32_t)lv_sqrt32((uint32_t)(dx * dx + dy * dy));
}

static void helm_poly_build(const lv_area_t * coords, lv_point_t * pts,
                            int32_t * slen, int32_t * total)
{
    lv_coord_t x0 = (lv_coord_t)(coords->x1 + 16);
    lv_coord_t y0 = (lv_coord_t)(coords->y1 + 16);
    lv_coord_t bw = (lv_coord_t)(lv_area_get_width(coords) - 32);
    lv_coord_t bh = 22;
    int i;

    if (bw < 40) {
        bw = 40;
    }

    *total = 0;
    for (i = 0; i < HELM_POLY_N; i++) {
        pts[i].x = (lv_coord_t)(x0 + (bw * helm_poly_nx[i]) / HELM_POLY_NX);
        pts[i].y = (lv_coord_t)(y0 + (bh * helm_poly_ny[i]) / HELM_POLY_NY);
        if (i > 0) {
            slen[i - 1] = helm_poly_hypot(pts[i].x - pts[i - 1].x,
                                          pts[i].y - pts[i - 1].y);
            if (slen[i - 1] < 1) {
                slen[i - 1] = 1;
            }

            *total += slen[i - 1];
        }
    }
}

static void helm_poly_at(const lv_point_t * pts, const int32_t * slen,
                         int32_t total, int32_t dist, int32_t * x, int32_t * y)
{
    int i;
    int32_t acc = 0;

    if (dist <= 0) {
        *x = pts[0].x;
        *y = pts[0].y;
        return;
    }

    if (dist >= total) {
        *x = pts[HELM_POLY_N - 1].x;
        *y = pts[HELM_POLY_N - 1].y;
        return;
    }

    for (i = 0; i < HELM_POLY_N - 1; i++) {
        int32_t L = slen[i];

        if (dist <= acc + L) {
            int32_t t = dist - acc;

            *x = pts[i].x + (pts[i + 1].x - pts[i].x) * t / L;
            *y = pts[i].y + (pts[i + 1].y - pts[i].y) * t / L;
            return;
        }

        acc += L;
    }

    *x = pts[HELM_POLY_N - 1].x;
    *y = pts[HELM_POLY_N - 1].y;
}

static void helm_poly_line(lv_layer_t * layer, int32_t x0, int32_t y0,
                           int32_t x1, int32_t y1, uint16_t w, lv_color_t color)
{
    helm_draw_seg(layer, x0, y0, x1, y1, w, color);
}

static void helm_poly_draw_range(lv_layer_t * layer, const lv_point_t * pts,
                                 const int32_t * slen, int32_t total,
                                 int32_t d0, int32_t d1, uint16_t w,
                                 lv_color_t color)
{
    int i;
    int32_t acc = 0;

    if (d1 <= d0) {
        return;
    }

    if (d0 < 0) {
        d0 = 0;
    }

    if (d1 > total) {
        d1 = total;
    }

    for (i = 0; i < HELM_POLY_N - 1; i++) {
        int32_t L = slen[i];
        int32_t s0 = acc;
        int32_t s1 = acc + L;
        int32_t u0;
        int32_t u1;
        int32_t ax;
        int32_t ay;
        int32_t bx;
        int32_t by;

        acc = s1;
        if (s1 <= d0 || s0 >= d1) {
            continue;
        }

        u0 = (d0 > s0) ? d0 : s0;
        u1 = (d1 < s1) ? d1 : s1;
        helm_poly_at(pts, slen, total, u0, &ax, &ay);
        helm_poly_at(pts, slen, total, u1, &bx, &by);
        helm_poly_line(layer, ax, ay, bx, by, w, color);
    }
}

static void helm_dial_label(lv_layer_t * layer, lv_coord_t x, lv_coord_t y,
                            const char * txt, lv_color_t color)
{
    lv_draw_label_dsc_t d;
    lv_point_t sz;
    lv_area_t a;

    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.text_local = 1;
    d.font = helm_font_lab();
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_text_get_size(&sz, txt, d.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    d.text_size = sz;
    a.x1 = (lv_coord_t)(x - sz.x / 2);
    a.y1 = (lv_coord_t)(y - sz.y / 2);
    a.x2 = (lv_coord_t)(a.x1 + sz.x);
    a.y2 = (lv_coord_t)(a.y1 + sz.y);
    lv_draw_label(layer, &d, &a);
}

static void helm_poly_pip(lv_layer_t * layer, int32_t x, int32_t y, lv_color_t color)
{
    helm_draw_pip(layer, x, y, 5, color);
}

static void helm_hero_spin_exec(void * var, int32_t v)
{
    s_sh.dial_phase = (uint16_t)v;

    if (s_sh.ui_covered) {
        return;
    }

    /* 见 HELM_HERO_SPIN_MS：相位每帧都更新，但只在节流窗口到点时才让
     * LVGL 重绘 hero（整圈折线 + track 弧 + 刻度文字 + 游标点）。 */
    {
        uint32_t now = lv_tick_get();

        if ((uint32_t)(now - s_hero_spin_redraw_ms) < HELM_HERO_SPIN_MS) {
            return;
        }

        s_hero_spin_redraw_ms = now;
    }

    lv_obj_invalidate((lv_obj_t *)var);
}

static void helm_hero_spin_set(bool on)
{
    lv_anim_t a;

    if (s_sh.hero == NULL) {
        s_sh.dial_spin = false;
        return;
    }

    if (on) {
        if (s_sh.ui_covered || s_sh.dial_spin) {
            return;
        }

        s_hero_spin_redraw_ms = 0; /* 首帧立刻重绘，不受节流窗口影响 */
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_sh.hero);
        lv_anim_set_values(&a, 0, HELM_DIAL_SPAN);
        lv_anim_set_time(&a, 1600);
        lv_anim_set_reverse_duration(&a, 1600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, helm_hero_spin_exec);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        s_sh.dial_spin = true;
        return;
    }

    if (s_sh.dial_spin) {
        lv_anim_delete(s_sh.hero, helm_hero_spin_exec);
        s_sh.dial_spin = false;
        s_sh.dial_phase = 0;
        if (!s_sh.ui_covered) {
            lv_obj_invalidate(s_sh.hero);
        }
    }
}

static void helm_hero_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t pts[HELM_POLY_N];
    int32_t slen[HELM_POLY_N - 1];
    int32_t total;
    int32_t dist;
    int32_t win;
    int32_t px;
    int32_t py;
    lv_color_t ink;
    lv_color_t accent;
    lv_color_t track;
    int16_t span;

    if (obj == NULL || layer == NULL) {
        return;
    }

    if (s_sh.dial_poly_defer) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    if (lv_area_get_width(&coords) < 40 || lv_area_get_height(&coords) < 40) {
        return;
    }

    helm_poly_build(&coords, pts, slen, &total);
    if (total < 8) {
        return;
    }

    ink = helm_color(HELM_COLOR_INK);
    accent = helm_color(HELM_COLOR_NAV);
    track = helm_color(HELM_COLOR_NAV_FILL);
    span = helm_dial_span_now();
    dist = (total * (int32_t)span) / HELM_DIAL_SPAN;
    win = (total * HELM_DIAL_SWEEP) / HELM_DIAL_SPAN;
    if (win < 16) {
        win = 16;
    }

    helm_poly_draw_range(layer, pts, slen, total, 0, total, 2, track);

    if (s_sh.dial_lock == 1) {
        int32_t d0 = dist - win / 2;
        int32_t d1 = dist + win / 2;

        if (d0 < 0) {
            d0 = 0;
            d1 = win;
        }

        if (d1 > total) {
            d1 = total;
            d0 = total - win;
        }

        helm_poly_draw_range(layer, pts, slen, total, d0, d1, 5, accent);
    } else if (dist > 8) {
        helm_poly_draw_range(layer, pts, slen, total, 0, dist, 5, accent);
    }

    helm_dial_label(layer, pts[0].x, (lv_coord_t)(pts[0].y + 12), "0", ink);
    helm_dial_label(layer, pts[3].x, (lv_coord_t)(pts[3].y - 9), "30", ink);
    helm_dial_label(layer, pts[HELM_POLY_N - 1].x,
                    (lv_coord_t)(pts[HELM_POLY_N - 1].y + 12), "60", ink);

    helm_poly_at(pts, slen, total, dist, &px, &py);
    helm_poly_pip(layer, px, py, accent);
}

/* ==================== 待机页（主界面）信息卡 ====================
 * 几何与 zcode/analysis/helm_preview/standby.py 的 compose="tight" 一一对应
 * （该脚本是像素级预览器，常量全部由实测字宽反推）。以下坐标都是**卡片内**坐标：
 * 卡片在 pane 内 (6, 162)，尺寸 228x128 ⇒ 屏幕上 186..314。
 * 地图不透明底已由 helm_build_standby 清掉，地图满幅铺到 y=240，跟车点在 y≈132，
 * 所以卡片 (186 起) 盖不到跟车点。
 */
#define HELM_SB_CARD_X    6
/* 卡片纵向：内容 150 − pad 20 = 130
 *   上半 79 + 间隙 6 + 发丝线 1 + 间隙 6 + 动作行 34 = 126 ≤ 130 ✓（余 4）
 * 上半**必须** ≥ 日期 14 + 间隙 6 + 时钟行高 57 = 77，否则日期被挤出卡片
 * （2026-09-26 实测：动作行 24→34 吃掉 10px 后上半只剩 61，日期当场被挤掉）。
 * 卡顶 140（屏 164）仍远低于跟车点 ~132 ⇒ 不挡跟车点。 */
#define HELM_SB_CARD_Y    140
#define HELM_SB_CARD_W    (PAGE_HOR_RES - 12)
#define HELM_SB_CARD_H    150
#define HELM_SB_DIV_X     121    /* 竖直分隔线（离状态列左端 4px） */
#define HELM_SB_ST_X      125    /* 状态列左端 */
#define HELM_SB_ST_Y      8      /* 状态列首行顶端 */
#define HELM_SB_ST_STEP   22     /* 状态列行距 */
#define HELM_SB_DATE_Y    17     /* 日期行顶端 */
#define HELM_SB_TIME_Y    23     /* 40px 时钟**标签顶端**（基线 = +43） */
#define HELM_SB_HAIR_Y    82     /* 信息区/操作区分界 */
#define HELM_SB_BTN_X     12
#define HELM_SB_BTN_Y     94
#define HELM_SB_BTN_W     118
#define HELM_SB_BTN_H     24
#define HELM_SB_RING_CX   200
#define HELM_SB_RING_CY   106
#define HELM_SB_RING_D    22
#define HELM_SB_RING_BOB  4      /* 浮动振幅 px */
/* 环右缘距动作行右缘的距离。目标：环心落在**实体右键正上方**（屏 x≈198，
 * 按用户标注的照片量）。动作行右缘 = 6 + 228 − 10 = 224 ⇒ 224 − 15 − 22/2 = 198。 */
#define HELM_SB_RING_INSET 15
/* 动作行高。不能取 24（= 胶囊高）：环高 22 + 浮动 ±4 ⇒ 需要 30 才装得下，
 * 否则动画那几帧环顶会被行裁掉（用户 2026-09-26："最上方在动画过程中还是又遮挡"）。
 * 取 34 给上下各留 6px。 */
#define HELM_SB_ROW_H     34
/* 状态列**固定宽度**。原来是 LV_SIZE_CONTENT ⇒ "2 颗卫星" 变 "10 颗卫星"、
 * "上次 0.3" 变 "上次 24.7" 都会改变列宽，而分隔线与左列是按剩余空间算的，
 * 于是整条竖线跟着左右晃（用户 2026-09-26："骨架竖线不要动"）。
 * 84 = 图标槽 16 + 间隙 6 + 最宽行 62
 *      （62 要按**三位数里程**算："上次 123.4"；按两位数算 78 会让它被裁，
 *        2026-09-26 实测"上次骑行距离显示不全"就是这个）。
 * 横向总账：时钟 111 + 间隙 4×2 + 竖线 1 + 状态列 84 = 204 ≤ 卡片内容 208 ✓ */
#define HELM_SB_ST_W      84
/* helm_num_40 的 line_height - base_line = 57 - 14 = 43 */
#define HELM_SB_CLOCK_ASC 43

/** 提示环：圆环 + "按下去"（下箭头 + 底座）。
 *  chevron 的顶点必须在两端**下方**才真是向下箭头——预览器第一版把顶点写在
 *  上方，两个箭头都朝上、读作"上滑"，而本机是实体键、没有滑动手势。 */
static void helm_sb_ring_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_draw_arc_dsc_t arc;
    lv_draw_line_dsc_t ln;
    lv_color_t col = lv_obj_get_style_text_color(obj, 0);
    int32_t cx;
    int32_t cy;

    lv_obj_get_coords(obj, &co);
    cx = (co.x1 + co.x2) / 2;
    cy = (co.y1 + co.y2) / 2;

    lv_draw_arc_dsc_init(&arc);
    arc.color = col;
    arc.width = 2;
    arc.opa = LV_OPA_COVER;
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = (uint16_t)(lv_area_get_width(&co) / 2 - 1);
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);

    lv_draw_line_dsc_init(&ln);
    ln.color = col;
    ln.width = 2;
    ln.opa = LV_OPA_COVER;

    ln.p1.x = cx - 4; ln.p1.y = cy - 3;      /* 下箭头左半 */
    ln.p2.x = cx;     ln.p2.y = cy + 2;
    lv_draw_line(layer, &ln);
    ln.p1.x = cx;     ln.p1.y = cy + 2;      /* 下箭头右半 */
    ln.p2.x = cx + 4; ln.p2.y = cy - 3;
    lv_draw_line(layer, &ln);
    ln.p1.x = cx - 5; ln.p1.y = cy + 6;      /* 底座 */
    ln.p2.x = cx + 5; ln.p2.y = cy + 6;
    lv_draw_line(layer, &ln);
}

static void helm_sb_bob_exec(void * var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/** @param parent LiveMap 页的 root（不是待机 pane）。父对象是 root ⇒ 卡片与
 *                 待机 pane 的显隐解耦，可以独立于翻页状态出现。 */
static void helm_build_standby_card(lv_obj_t * parent)
{
    lv_obj_t * card;
    lv_obj_t * top;
    lv_obj_t * lef;
    lv_obj_t * col;
    lv_obj_t * row;
    lv_obj_t * o;
    uint8_t i;

    /* ⚠ 2026-09-26 重写：第一版对子元素用 lv_obj_set_pos() **绝对定位**，上板布局
     * 全错（元素被按创建顺序竖着堆）。本文件既有写法一律用 **flex**（helm_overlay
     * 是 COLUMN、helm_col/helm_ride_row 是 flex 对齐、helm_speed_head 是 ROW
     * SPACE_BETWEEN），混用两种定位方式会被布局重排。
     * 下面全部改成嵌套 flex：外层 COLUMN → 上半 ROW（左列 | 竖线 | 状态列）→
     * 发丝线 → 操作行（CTA 左 / 提示环右，SPACE_BETWEEN 让环贴右）。
     * 唯一保留的绝对定位是卡片自己相对 pane 的位置（pane 是 flex，卡片带
     * FLOATING 被排除在布局外，这是本文件既有做法）。 */
    card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    helm_style_card(card);
    lv_obj_set_size(card, HELM_SB_CARD_W, HELM_SB_CARD_H);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(card, HELM_SB_CARD_X, HELM_SB_CARD_Y);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);   /* 由 helm_apply_view 按状态显隐 */
    s_sh.sb_card = card;
    /* 初值必须不是 0：0 的语义正好是 "00:00"，会让"时分变了才刷新"的判据
     * 在时间无效时永远为假、时钟一直空白。 */
    s_sh.sb_hm = 0xFFFFu;
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_pad_column(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    /* 上半：左列（日期 / 40px 时钟） | 竖直分隔线 | 右列（状态列三行） */
    top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_flex_grow(top, 1);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    /* 间隙 8 -> 6 -> 4：时钟补冒号边距后 111、状态列固定 84，横向只剩 208，
     * 间隙必须收到 4 才放得下（总账见 HELM_SB_ST_W 的注释）。 */
    lv_obj_set_style_pad_column(top, 4, 0);

    lef = lv_obj_create(top);
    lv_obj_remove_style_all(lef);
    /* 左列**内容宽**（= 时钟宽 111，恒宽：时钟永远是 HH:MM 5 个等宽字符），
     * 不再 flex_grow。用户 2026-09-26："时间向左移动，这样左侧空间更大，右侧对应的
     * 空间也能增加"：原先左列吃剩余空间 + 时钟在其中居中 ⇒ 两侧各留 2px 白；
     * 改成内容宽后时钟贴左（cross=START），腾出的余量全给状态列。
     * 副作用是好的：分隔线位置恒等于 111+间隙，**不再随状态列数据变宽而移动**。 */
    lv_obj_set_size(lef, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_flex_flow(lef, LV_FLEX_FLOW_COLUMN);
    /* 横向贴左（cross=START）、整块纵向居中 */
    lv_obj_set_flex_align(lef, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(lef, 6, 0);
    /* 日期+时钟整块**上移**（用户 2026-09-26："时间可以上移一些"）：主轴上原本
     * 只是 CENTER，改成在底部留 12px ⇒ 内容整体上移 6px。 */
    lv_obj_set_style_pad_bottom(lef, 12, 0);
    /* ⚠ 这里**不能**加 pad_right 来"左移几像素"：左列可用宽 107 = 时钟标签宽
     * 107，加 8px 内边距后内容区只剩 99 ⇒ 居中的标签左右各溢出 4px 被裁掉
     * （用户 2026-09-26："现在左侧又显示不全了"）。
     * 用户要的"以中间的 `:` 为锚点、左右对称"本来就成立：时钟是「2 位:2 位」
     * 等宽，冒号恒在字符串正中 ⇒ 只要标签不被裁、居中放置，冒号就是枢轴。 */
    /* 顺序：**时钟在上、日期在下**（用户 2026-09-26："时间在上，日期在下"）。
     * flex COLUMN 里创建顺序 = 视觉顺序，所以这两行的**先后不能随手调换**——
     * 中间那句注释里说的"时钟是枢轴"只跟水平方向有关，与先后无关。 */
    s_sh.sb_time = helm_lab(lef, &helm_num_40, HELM_COLOR_INK, "");
    s_sh.sb_date = helm_lab(lef, s_sh.font_lab, HELM_COLOR_MUTE, "");
    /* 日期**相对时钟水平居中**：左列是 cross=START（时钟贴左，这样左列恒宽 111、
     * 分隔线不随数据动），但日期只有 ~77px，跟着贴左就显得"歪"
     * （用户 2026-09-26："日期歪了"）。把它拉到列宽 100% 再让文字居中，
     * 于是它以时钟的 111px 为基准居中，而列宽不变 ⇒ 不影响分隔线。 */
    lv_obj_set_width(s_sh.sb_date, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.sb_date, LV_TEXT_ALIGN_CENTER, 0);

    o = lv_obj_create(top);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 1, lv_pct(100));
    lv_obj_set_style_bg_color(o, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    col = lv_obj_create(top);
    lv_obj_remove_style_all(col);
    /* 状态列改为吃剩余空间（原来固定 HELM_SB_ST_W=84）。左列收到内容宽后这里从
     * 84 涨到 88；固定宽度那条要求的本意——"竖线不随数据晃动"——由**左列恒宽**
     * 来保证（时钟恒 111），比钉死右列更稳。 */
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_style_min_width(col, HELM_SB_ST_W, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 8, 0);

    /* 状态列三行：卫星 / 定位 / 上次距离。每行 = ROW(图标或圆点 + 文字)，
     * 图标与圆点因此自动对齐同一条竖线（第一版用绝对坐标把它甩到了左边）。 */
    for (i = 0; i < 3u; i++) {
        row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, 16);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);

        /* ⚠ 图标槽必须是**固定 16x16**、圆点在其中居中：原来图标 16px、圆点 7px
         * 各自按内容宽排，于是每行的文字起点都差 9px、左边缘参齐（上板照片可见，
         * 用户 2026-09-26："卫星图标下方图标竖向中心对齐，文字也要对齐"）。 */
        o = lv_obj_create(row);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, 16, 16);
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        if (i == 0u) {
            o = helm_icon_create(o, HELM_ICO_GPS, 16);
            helm_icon_set_color(o, helm_color(HELM_COLOR_GPS));
        } else {
            lv_obj_t * dot = lv_obj_create(o);

            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 7, 7);
            lv_obj_set_style_radius(dot, 3, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, helm_color(i == 1u ? HELM_COLOR_GPS
                                                               : HELM_COLOR_MUTE),
                                      0);
        }
        s_sh.sb_st[i] = helm_lab(row, s_sh.font_lab, HELM_COLOR_MUTE, "");
    }

    /* 信息区 / 操作区 分界 */
    o = lv_obj_create(card);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), 1);
    lv_obj_set_style_bg_color(o, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    /* 操作行：CTA（左） + 提示环（右） */
    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), HELM_SB_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 右侧不再留内边距：环已改成 FLOATING + 显式 align（见下），位置由
     * HELM_SB_RING_INSET 决定；留 pad_right 会把 align 的基准一起推左。 */

    /* 操作条（主）：橙底 + 「长按右键 开始骑行」。
     * 文案里写"右键"（不写 KEY2——用户不知道键位名，但"右键"是位置，能懂），
     * 且不再画键帽：环的位置已经把"哪颗键"指出来了，键帽反而像第二个按钮。 */
    o = lv_obj_create(row);
    lv_obj_remove_style_all(o);
    lv_obj_set_flex_grow(o, 0);
    lv_obj_set_style_pad_hor(o, 14, 0);
    lv_obj_set_style_pad_ver(o, 0, 0);
    /* ⚠ 宽度**必须**显式 LV_SIZE_CONTENT：`remove_style_all()` 之后 lv_obj 的宽度
     * 退回类默认 LV_DPI_DEF = **130px**（helm_widget.h 里记过这个坑）。只设 height
     * 不设 width ⇒ 胶囊恒 130 宽、装不下内容 ⇒ 动作字被截成"开始"。
     * 换句话说上一版"文案太长"是误判：真因是宽度没按内容算。 */
    lv_obj_set_size(o, LV_SIZE_CONTENT, HELM_SB_BTN_H);
    lv_obj_set_style_radius(o, HELM_SB_BTN_H / 2, 0);
    lv_obj_set_style_bg_color(o, helm_color(HELM_COLOR_NAV), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 6, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    /* 两级文字（用户 2026-09-26："「长按右键」这几字放在胶囊里，文字和之前设计
     * 一样小一号，胶囊放不下就加宽"）：前置条件 12px + 动作 15px，沿用之前
     * "键帽小、动作字大"的层级。胶囊宽度由内容决定（pad_hor 14），放不下会自己
     * 撑开——卡片内容宽 208、右侧给环留 37，所以胶囊最大可用约 171。 */
    {
        /* 前置条件放**深红键帽里**（用户照片里圈的就是这片红色区域）：键帽作
         * "这是个按键动作"的形状，文字 12px；动作字 15px 跟在后面。 */
        lv_obj_t * cap = lv_obj_create(o);

        lv_obj_remove_style_all(cap);
        /* 同上：键帽宽度也要显式 LV_SIZE_CONTENT，否则被撑成 130 宽的横条 */
        lv_obj_set_size(cap, LV_SIZE_CONTENT, 16);
        lv_obj_set_style_pad_hor(cap, 6, 0);
        lv_obj_set_style_radius(cap, 4, 0);
        lv_obj_set_style_bg_color(cap, helm_color(HELM_COLOR_INK), 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_60, 0);
        lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        /* 键帽里放**四个字**的前置条件（用户 2026-09-26："只有「长按按键」这四个字
         * 在键帽里，「开始骑行」在外面。总长度需要计算的"）。
         * 总长 = 键帽(4×12 + pad 12 = 60) + 间隙 6 + 动作字(4×15 = 60) + pad 28
         *       = 154 ≤ 可用 171（卡片内容 208 − 环 22 − 环右内缩 15）⇒ 放得下。
         * 上一版把它截断与文案长短无关，是胶囊宽度没设 LV_SIZE_CONTENT（见上）。 */
        helm_lab(cap, s_sh.font_lab, HELM_COLOR_PAPER, "长按按键");

        helm_lab(o, s_sh.font_title, HELM_COLOR_PAPER, "开始骑行");
    }

    /* 浮动指示（辅）：22px 环，面积只有操作条的 1/6 ⇒ 主次分明。
     * ⚠ 环必须 **FLOATING + 显式对齐**，不能当 flex 子元素：它的位置既要落在
     *   实体右键正上方（屏 x≈198），又要做 ±4px 的 translate_y 动画。作为 flex
     *   子元素时"布局算位置 + 动画加位移"两者叠加，环会漂到上方可见区之外
     *   （用户 2026-09-26 报："会移动到上方不可显示区域"）。
     *   FLOATING 把它排除在布局外 ⇒ 位置只由 align 决定，浮动只在这个位置上
     *   叠加 ±4px，完全确定。 */
    s_sh.sb_ring = lv_obj_create(row);
    lv_obj_remove_style_all(s_sh.sb_ring);
    lv_obj_set_size(s_sh.sb_ring, HELM_SB_RING_D, HELM_SB_RING_D);
    lv_obj_add_flag(s_sh.sb_ring, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_sh.sb_ring, LV_ALIGN_RIGHT_MID, -(HELM_SB_RING_INSET), 0);
    lv_obj_set_style_text_color(s_sh.sb_ring, helm_color(HELM_COLOR_NAV), 0);
    lv_obj_set_style_bg_opa(s_sh.sb_ring, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_sh.sb_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sh.sb_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sh.sb_ring, helm_sb_ring_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* 上下浮动：±4px、1400ms、正弦往返。周期远低于帧率门槛，且只有
     * 22x22 区域重画（~1 KB/帧 ⇒ 按 PSRAM 10 MB/s 折算约 0.3% 带宽）。 */
    {
        lv_anim_t a;

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_sh.sb_ring);
        lv_anim_set_exec_cb(&a, helm_sb_bob_exec);
        lv_anim_set_values(&a, -HELM_SB_RING_BOB, HELM_SB_RING_BOB);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_playback_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

/** 待机页信息卡每拍刷新：40px 时钟 + 状态列三行。 */
static void helm_standby_card_tick(const bicycle_runtime_t * rt)
{
    char buf[32];
    time_t now = time(NULL);
    struct tm tmv;
    bool have = false;
    int hour = 0;
    int min = 0;
    int mon = 0;
    int day = 0;
    int wday = 0;

    /* 本地时间：系统时间是 UTC，时区偏移在 devctl 里（tz_hms 同源）。
     * 这里要的不只是时分还要年月日/星期，所以自己做一次偏移 + gmtime_r。 */
    if (now >= 1704067200 && gmtime_r(&(time_t){(time_t)((long)now
            + (long)myvendor_devctl_tz_min_get() * 60)}, &tmv) != NULL) {
        have = true;
        hour = tmv.tm_hour;
        min = tmv.tm_min;
        mon = tmv.tm_mon + 1;
        day = tmv.tm_mday;
        wday = (tmv.tm_wday >= 0 && tmv.tm_wday < 7) ? tmv.tm_wday : 0;
    }

    /* **时间无效也必须显示**（用户 2026-09-26："时间即使不正常也得显示，和静止界面
     * 一样"）：没对上星/没设置时给 00:00，和状态栏同样的兜底。
     * ⚠ 配套：sb_hm 初值必须是 0xFFFF，不能是 0——0 的语义恰好就是 "00:00"，
     * 于是"hm != sb_hm"永远为假、标签一次都不会被设、一直空白（上板实测）。 */
    {
        const uint16_t hm = (uint16_t)(hour * 60 + min);

        if (hm != s_sh.sb_hm) {
            static const char * wd[7] = { "周日", "周一", "周二", "周三",
                                          "周四", "周五", "周六" };

            s_sh.sb_hm = hm;
            if (have) {
                lv_snprintf(buf, sizeof(buf), "%d月%d日 %s", mon, day, wd[wday]);
            } else {
                lv_snprintf(buf, sizeof(buf), "--月--日");
            }
            helm_label_set(s_sh.sb_date, buf);
            lv_snprintf(buf, sizeof(buf), "%02d:%02d", hour, min);
            helm_label_set(s_sh.sb_time, buf);
            /* 半反屏局部刷新会留灰影 / 闪一下（用户 2026-09-26："和静止界面一样，
             * 闪烁，消失时能看到灰色"；旧静止页同样如此 ⇒ 属既有的面板特性，
             * 不是这轮引入的）。缓解：时间一变就把**整个左列**（日期 + 时钟）一起
             * 失效重画，让新数字落在一块干净重绘的区域上，而不是只补字形那一小块。 */
            if (s_sh.sb_date != NULL) {
                lv_obj_invalidate(lv_obj_get_parent(s_sh.sb_date));
            }
        }
    }

    if (rt != NULL) {
        lv_snprintf(buf, sizeof(buf), "%u 颗卫星",
                    (unsigned)rt->gnss_satellites);
        helm_label_set(s_sh.sb_st[0], buf);
        helm_label_set(s_sh.sb_st[1], rt->gnss_valid ? "已定位"
                                                     : "未定位");
        if (s_sh.last_km > 0.05f) {
            /* **取整、不留小数**（用户 2026-09-26："去掉小数点吧"）。
             * 宽度实测（mism4_12）：
             *   "上次 25 km"  ≈ 61px ⇒ 状态列需 83 ≤ 固定宽 84 ✅
             *   "上次 128 km" ≈ 71px ⇒ 需 93 ❌ 三位数带单位放不下
             *   "上次 128"    ≈ 52px ⇒ 需 74 ✅
             * ⇒ 里程 ≥100 km 时**省掉单位**：宁可少个单位，也不要把字裁掉。
             *   （单位不能无条件省：「公里/英里可切」所以数字离开单位就读不出来。） */
            const int kmi = (int)(s_sh.last_km + 0.5f);

            if (kmi < 100) {
                lv_snprintf(buf, sizeof(buf), "上次 %d km", kmi);
            } else {
                lv_snprintf(buf, sizeof(buf), "上次 %d", kmi);
            }
        } else {
            lv_snprintf(buf, sizeof(buf), "上次 -- km");
        }
        helm_label_set(s_sh.sb_st[2], buf);
    }
}

static void helm_build_standby(lv_obj_t * root)
{
    lv_obj_t * hero;
    lv_obj_t * row;
    lv_obj_t * hint;

    s_sh.standby = helm_overlay(root, 0, HELM_PAGE_H);

    /* ⚠ 2026-09-26 回退：这里曾把底色清成透明、想让下面的地图透出来 ⇒ 上板花屏
     * （待机这条路的地图画布从没被绘制过）。地图区共用那件事要靠"待机是地图页的
     * 一个状态"来实现，不是靠透明 pane 硬叠。 */

    /* 待机页仍然是**不透明满页底**，信息卡叠在它上面。 */

    /* Speed sits under a polyline ridge; orange chunk + pip travel the path. */
    hero = lv_obj_create(s_sh.standby);
    lv_obj_remove_style_all(hero);
    helm_grow_y(hero);
    lv_obj_set_style_bg_opa(hero, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(hero, 40, 0);
    lv_obj_set_style_pad_bottom(hero, 8, 0);
    lv_obj_set_style_pad_row(hero, 0, 0);
    lv_obj_add_flag(hero, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hero, helm_hero_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_sh.hero = hero;

    s_sh.speed_big = helm_lab(hero, s_sh.font_speed, HELM_COLOR_NAV, "0.0");
    lv_obj_set_width(s_sh.speed_big, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.speed_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_sh.speed_big, -1, 0);
    lv_obj_add_flag(s_sh.speed_big, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_sh.speed_unit = helm_lab(hero, s_sh.font_lab, HELM_COLOR_NAV, "km/h");
    lv_obj_set_width(s_sh.speed_unit, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.speed_unit, LV_TEXT_ALIGN_CENTER, 0);

    s_sh.hero_mode = helm_lab(hero, s_sh.font_lab, HELM_COLOR_INK, "0Hz");
    lv_obj_set_width(s_sh.hero_mode, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.hero_mode, LV_TEXT_ALIGN_CENTER, 0);

    row = lv_obj_create(s_sh.standby);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), HELM_ROW2_H);
    lv_obj_set_flex_grow(row, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t * lab;

        helm_col(row, &lab, &s_sh.last_dist, s_sh.font_val);
        lv_label_set_text_static(lab, "上次里程");
        lv_label_set_text_static(s_sh.last_dist, "-- km");

        helm_col(row, &lab, &s_sh.gps_val, s_sh.font_val);
        lv_label_set_text_static(lab, "GPS");
        lv_label_set_text_static(s_sh.gps_val, "无定位");
    }

    hint = lv_obj_create(s_sh.standby);
    lv_obj_remove_style_all(hint);
    helm_style_card(hint);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_height(hint, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(hint, 0);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(hint, 8, 0);
    lv_obj_set_style_pad_hor(hint, 10, 0);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_sh.standby_hint = helm_lab(hint, s_sh.font_title, HELM_COLOR_NAV, "待开始");
    lv_obj_set_width(s_sh.standby_hint, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.standby_hint, LV_TEXT_ALIGN_CENTER, 0);

    /* 旧的英雄区（折线脊 + 大速度 + 模式）/两格（上次里程、GPS）/提示条整组隐藏：
     * 刷新逻辑对这些指针仍有非空的判断，隐藏后一行都不用改、也不会 NULL 崩溃
     * （helm_paint_speed_pair 等函数直接调 lv_obj_set_style_* 不做 NULL 判断）。
     * 它们承载的信息已由下面的状态列与操作条接管；物理删除留作后续清理。 */
    lv_obj_add_flag(hero, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);

    /* 信息卡挂在 LiveMap 的 root 上，与待机 pane 解耦 */
    helm_build_standby_card(root);
}

static void helm_build_data(lv_obj_t * root)
{
    lv_obj_t * row;

    s_sh.data = helm_overlay(root, HELM_SUBBAR_H,
                             HELM_PAGE_H - HELM_SUBBAR_H);
    /* 第三参传 NULL = 不建页码标签（用户 2026-09-26：右上角的页面位置指示
     * 不需要显示）。s_sh.data_mark 保持 NULL，下面 refresh 里的
     * helm_label_set(NULL,...) 自带空指针早退。 */
    helm_speed_head(s_sh.data, "骑行", &s_sh.data_title, NULL);

    /* X1 仪表台：左栏时速锁死（字号最大、位置固定、不参与轮转），右栏数据行。
     * 竖向扫读比 2x2 网格快——骑行中眼睛只需要看左栏那一个大数字。 */
    row = lv_obj_create(s_sh.data);
    lv_obj_remove_style_all(row);
    helm_grow_y(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    helm_ride_hero(row);
    helm_ride_rows(row);
}

/** @brief 数据页十项取数（顺序 = 用户清单，两列按 i*2 / i*2+1 排）。 */
static void helm_fill_all(const bicycle_runtime_t * rt, const char ** lab,
                          char val[][16])
{
    char tmp[16];
    float dist_km = rt ? (float)(rt->session_distance_m / 1000.0) : 0.0f;

    lab[0] = "时间";
    helm_fmt_time(val[0], 16, rt ? rt->session_elapsed_ms : 0ull);
    lab[1] = "里程";
    helm_fmt_km(val[1], 16, dist_km);
    lab[2] = "均速";
    helm_fmt_speed(val[2], 16, helm_session_avg_kph(rt));
    lab[3] = "极速";
    helm_fmt_speed(val[3], 16, rt ? rt->speed_max_kph : 0.0f);
    lab[4] = "心率";
    lv_snprintf(val[4], 16, "%s",
                helm_dash(rt && rt->hr_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->hr_bpm : 0));
    lab[5] = "均心率";
    lv_snprintf(val[5], 16, "%s",
                helm_dash(rt && rt->hr_avg_bpm > 0u, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->hr_avg_bpm : 0));
    lab[6] = "最大心率";
    lv_snprintf(val[6], 16, "%s",
                helm_dash(rt && rt->hr_max_bpm > 0u, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->hr_max_bpm : 0));
    lab[7] = "踏频";
    lv_snprintf(val[7], 16, "%s",
                helm_dash(rt && rt->cadence_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->cadence_rpm : 0));
    lab[8] = "功率";
    lv_snprintf(val[8], 16, "%s",
                helm_dash(rt && rt->power_valid, tmp, sizeof(tmp), "%u",
                          rt ? (int)rt->power_w : 0));
    lab[9] = "爬升";
    lv_snprintf(val[9], 16, "%d", rt ? (int)(rt->gain_m + 0.5f) : 0);
}

/** @brief 通用数据格：左缘 3px 身份色条 + 标签（MUTE）+ 数值（num_28，INK）。
 *
 * 与骑行页右栏同一套写法，但**不带走势背景**（用户 2026-09-26 给的数据页/海拔页
 * 图里都没有曲线），也不参与行底告警（那套对象挂在骑行页的 s_sh.cell_box 上）。
 * 数据页（十格）和海拔页（六格）共用它，各自传自己的对象数组。
 */
static lv_obj_t * helm_cell_make(lv_obj_t * parent, lv_obj_t ** rail,
                                 lv_obj_t ** lab, lv_obj_t ** val, int idx)
{
    lv_obj_t * cell = lv_obj_create(parent);
    lv_obj_t * col;

    lv_obj_remove_style_all(cell);
    helm_grow_x(cell);
    lv_obj_set_height(cell, lv_pct(100));
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cell, 6, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    rail[idx] = lv_obj_create(cell);
    lv_obj_remove_style_all(rail[idx]);
    lv_obj_set_size(rail[idx], 3, HELM_RIDE_RAIL_H);
    lv_obj_set_style_radius(rail[idx], 2, 0);
    lv_obj_set_style_bg_opa(rail[idx], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rail[idx], helm_color(HELM_COLOR_MUTE), 0);
    lv_obj_clear_flag(rail[idx], LV_OBJ_FLAG_CLICKABLE);

    col = lv_obj_create(cell);
    lv_obj_remove_style_all(col);
    helm_grow_x(col);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lab[idx] = helm_lab(col, s_sh.font_lab, HELM_COLOR_MUTE, "--");
    val[idx] = helm_lab(col, s_sh.font_rot, HELM_COLOR_INK, "--");
    return cell;
}

static lv_obj_t * helm_all_cell(lv_obj_t * parent, int idx)
{
    return helm_cell_make(parent, s_sh.all_rail, s_sh.all_lab, s_sh.all_val,
                          idx);
}

/** @brief 数据页：2 列 x 5 行十项（用户 2026-09-26 的清单）。
 *
 * @details 行高 flex_grow（5 行均分卡内高 258 ⇒ 51）：行内"标签 14 + 数值 30
 *          = 44"✓。两列之间是**整卡通高的 1px 竖线**（行里留了 12px 间隙给它），
 *          行分隔发丝线做在行自己的底边上（flex 列里塞 1px 对象会参与分高）。
 */
static void helm_build_data_all(lv_obj_t * root)
{
    lv_obj_t * card;
    lv_obj_t * div;
    int i;

    s_sh.data_all = helm_overlay(root, HELM_SUBBAR_H,
                                 HELM_PAGE_H - HELM_SUBBAR_H);
    helm_speed_head(s_sh.data_all, "骑行数据", NULL, NULL);

    card = lv_obj_create(s_sh.data_all);
    lv_obj_remove_style_all(card);
    helm_style_card(card);
    helm_grow_y(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < HELM_ALL_N / 2; i++) {
        lv_obj_t * row = lv_obj_create(card);

        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        helm_grow_y(row);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i < HELM_ALL_N / 2 - 1) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, helm_color(HELM_COLOR_HAIR), 0);
        }

        helm_all_cell(row, i * 2);
        helm_all_cell(row, i * 2 + 1);
    }

    div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 1, lv_pct(100));
    lv_obj_add_flag(div, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(div, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(div, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(div);
}

static void helm_build_turn(lv_obj_t * root)
{
    s_sh.turn = helm_overlay(root, HELM_SUBBAR_H, HELM_MAP_BODY_H);
    lv_obj_set_flex_align(s_sh.turn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_sh.turn_ico = helm_lab(s_sh.turn, s_sh.font_mark, HELM_COLOR_NAV, "↑");
    lv_obj_set_style_transform_pivot_x(s_sh.turn_ico, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_sh.turn_ico, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale(s_sh.turn_ico, 336, 0);
    lv_obj_add_flag(s_sh.turn_ico, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_sh.turn_dist = helm_lab(s_sh.turn, s_sh.font_quad, HELM_COLOR_NAV, "--");
    /* 转向页的副标题也是路名，同样不能走点阵字库（缺字见上面 nav_ban_rd）。 */
    s_sh.turn_sub = helm_lab(s_sh.turn, helm_font_sys(15, s_sh.font_lab),
                             HELM_COLOR_NAV, "转向");
}

/**
 * @brief 图表折线绘制前回调：导航烘焙失败时的现场绘制回退路径。
 * @param e LV_EVENT_DRAW_TASK_ADDED。
 *
 * @details 画布可见时把折线透明并立即返回，切页只 blit 烘焙图。
 *          回退路径隐藏 LVGL 单色折线，只提交与脏区相交的段。
 *          非导航「经过海拔」仍用青绿渐变面积。
 */
static void helm_climb_draw_task(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc;
    lv_draw_line_dsc_t * draw_line_dsc;
    lv_color_t fill;
    lv_area_t coords;
    int32_t i;

    if (obj == NULL || draw_task == NULL) {
        return;
    }

    base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    if (base_dsc == NULL || base_dsc->layer == NULL ||
        base_dsc->part != LV_PART_ITEMS ||
        lv_draw_task_get_type(draw_task) != LV_DRAW_TASK_TYPE_LINE) {
        return;
    }

    draw_line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (draw_line_dsc == NULL || draw_line_dsc->points == NULL ||
        draw_line_dsc->point_cnt < 2) {
        return;
    }

    lv_obj_get_coords(obj, &coords);

    if (s_sh.climb_canvas && !lv_obj_has_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN)) {
        draw_line_dsc->opa = LV_OPA_TRANSP;
        return;
    }

    if (s_sh.climb_nav && s_sh.climb_n >= 2u) {
        int32_t lw = draw_line_dsc->width;
        uint32_t now = s_sh.climb_now;
        const lv_area_t * clip = &base_dsc->layer->_clip_area;

        draw_line_dsc->opa = LV_OPA_TRANSP;
        for (i = 0; i < draw_line_dsc->point_cnt - 1; i++) {
            lv_point_precise_t p1 = draw_line_dsc->points[i];
            lv_point_precise_t p2 = draw_line_dsc->points[i + 1];
            uint8_t z;
            uint32_t fh;
            uint32_t sh;

            if (p1.x == LV_DRAW_LINE_POINT_NONE || p1.y == LV_DRAW_LINE_POINT_NONE ||
                p2.x == LV_DRAW_LINE_POINT_NONE || p2.y == LV_DRAW_LINE_POINT_NONE) {
                continue;
            }
            if (!helm_climb_seg_in_clip(clip, p1, p2, coords.y2, lw)) {
                continue;
            }
            z = ((uint32_t)i + 1u < s_sh.climb_n) ? s_climb_zone[i] : 0u;
            if ((uint32_t)i < now) {
                fh = HELM_COLOR_ELEV_DONE;
                sh = HELM_COLOR_ELEV_DONE_INK;
            } else {
                fh = helm_climb_zone_fill(z);
                sh = helm_climb_zone_ink(z);
            }
            helm_climb_paint_seg(base_dsc->layer, p1, p2, coords.y2,
                helm_color_snap(fh), helm_color(sh), lw);
        }
        return;
    }

    /* 逐行填（2026-09-25 第二版，与主页 spark 同法）：每行一个**纯色**矩形，alpha 由
     * 一条**全局**竖直斜坡给出 —— 越靠图表上方越实、到图底淡成 0，于是山峰处厚实、
     * 谷地处轻薄；比原来"每段一个三角形 + 一块 20% 淡洗"干净得多。
     *
     * 换掉老写法的两个理由：
     *   · 老写法的矩形 x 区间是 [min, max] **两端都含** ⇒ 相邻段在数据点那一列重叠
     *     1 px、混合两次 ⇒ 偏深竖线；三角形的 bbox 又各按自己取整 ⇒ 等 alpha 线是
     *     按段错开的锯齿（观感"脏"）。
     *   · 本板 EPIC 加速单元**没有 TRIANGLE**（只有 FILL/IMAGE/BORDER/LABEL/LAYER），
     *     三角形要回退 SW 逐行生成遮罩；纯色矩形全部走 EPIC 的 FILL。
     *
     * 行、列都只扫**裁剪区内** —— 光标移动时只标脏一个小方块，不能整片重画。 */
    fill = helm_color_snap(HELM_COLOR_CLIMB_FILL);
    {
        const lv_area_t * clip = &base_dsc->layer->_clip_area;
        const int32_t n_pts = (int32_t)draw_line_dsc->point_cnt;
        const int32_t fh = coords.y2 - coords.y1 + 1;
        int32_t x_first = (int32_t)draw_line_dsc->points[0].x;
        int32_t x_last = (int32_t)draw_line_dsc->points[n_pts - 1].x;
        int32_t y_from = coords.y1 > clip->y1 ? coords.y1 : clip->y1;
        int32_t y_to = coords.y2 < clip->y2 ? coords.y2 : clip->y2;
        int32_t x_from = x_first > clip->x1 ? x_first : clip->x1;
        int32_t x_to = x_last < clip->x2 ? x_last : clip->x2;
        int32_t y;

        if (fh > 0 && x_to >= x_from) {
            for (y = y_from; y <= y_to; y++) {
                lv_draw_rect_dsc_t rect_dsc;
                lv_area_t rect_area;
                lv_opa_t opa = (lv_opa_t)(HELM_CLIMB_FILL_TOP_OPA -
                    (uint32_t)(y - coords.y1) * HELM_CLIMB_FILL_TOP_OPA / (uint32_t)fh);
                uint32_t seg = 0;
                int32_t run = 0;
                bool in = false;
                int32_t x;

                if ((int32_t)opa < 6) {
                    continue;       /* 淡到看不见了：这一行不画 */
                }

                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.bg_color = fill;
                rect_dsc.bg_opa = opa;
                rect_area.y1 = y;
                rect_area.y2 = y;

                for (x = x_from; x <= x_to; x++) {
                    bool under = false;

                    while (seg < (uint32_t)n_pts - 2u &&
                           (int32_t)draw_line_dsc->points[seg + 1].x < x) {
                        seg++;
                    }

                    {
                        lv_point_precise_t a = draw_line_dsc->points[seg];
                        lv_point_precise_t b = draw_line_dsc->points[seg + 1];

                        /* 端点无效或该段不向右延伸：这一列当作"曲线不在上方"，不填。 */
                        if (a.x != LV_DRAW_LINE_POINT_NONE &&
                            a.y != LV_DRAW_LINE_POINT_NONE &&
                            b.x != LV_DRAW_LINE_POINT_NONE &&
                            b.y != LV_DRAW_LINE_POINT_NONE &&
                            b.x > a.x) {
                            int32_t yc = (int32_t)a.y +
                                         (int32_t)((b.y - a.y) * (x - a.x) / (b.x - a.x));

                            under = (yc < y);
                        }
                    }

                    if (under && !in) {
                        run = x;
                        in = true;
                    } else if (!under && in) {
                        rect_area.x1 = run;
                        rect_area.x2 = x - 1;
                        if (rect_area.x2 >= rect_area.x1) {
                            lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                        }
                        in = false;
                    }
                }

                if (in) {
                    rect_area.x1 = run;
                    rect_area.x2 = x_to;
                    if (rect_area.x2 >= rect_area.x1) {
                        lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
                    }
                }
            }
        }
    }
}

/* 当前位置指示：橙红倒三角**点阵**（RGB565A8，15×10，边缘抗锯齿）。
 *
 * 为什么是"颜色烧进图里"的点阵，而不是"蒙版 + recolor"：
 *   · recolor 的图会被 EPIC 的 eval 直接拒掉（源码里见 recolor 就 `return 0`）
 *     ⇒ 落到 SW 且颜色不由这里掌控；烧进 RGB565A8 后走的是 EPIC 的 IMAGE 路径。
 *   · 本板 EPIC 加速单元**没有 TRIANGLE**，画三角形会回退 SW 并逐行建 mask
 *     （`lv_draw_sw_triangle.c` 里有 `lv_malloc(area_w)`），而光标每秒要挪好几次。
 *   ⇒ 点阵 + 烘色 = 硬件路径 + 抗锯齿 + 颜色确定。
 *
 * 颜色 = `HELM_COLOR_CLIMB_MARK`（0xFF4500，CSS orangered）⇒ RGB565 = 0xFA20
 * （小端字节 0x20,0xFA）。**要改色就得改这里的数据**，生成方式（复现用）：
 * 15×10 画布，顶点 (0,0)-(15,0)-(7.5,10)，每像素 8×8 超采样取覆盖率 ×255；
 * 布局按 LVGL 的 RGB565A8：色平面 15×10×2 B 在前、alpha 平面 15×10 B 在后。
 * alpha 平面里 60 全透明 / 61 全不透明 / 29 个半透明做抗锯齿。 */
static const uint8_t s_climb_mark_data[HELM_CLIMB_MARK_W * HELM_CLIMB_MARK_H * 3] = {
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,0x20,0xfa,
    0x9f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x9f,0x0c,0xd3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xd3,0x0c,
    0x00,0x2c,0xf3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xf3,0x2c,0x00,0x00,0x00,0x60,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x60,0x00,0x00,
    0x00,0x00,0x00,0x9f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x9f,0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0xd3,0xff,0xff,0xff,0xff,0xff,0xd3,0x0c,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x2c,0xf3,0xff,0xff,0xff,0xf3,0x2c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0xff,0xff,0xff,0x60,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x9f,0xff,0x9f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0xa7,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,
};

static const lv_image_dsc_t s_climb_mark_dsc = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.flags = 0,
    .header.w = HELM_CLIMB_MARK_W,
    .header.h = HELM_CLIMB_MARK_H,
    .header.stride = HELM_CLIMB_MARK_W * 2,
    .data_size = sizeof(s_climb_mark_data),
    .data = s_climb_mark_data,
};

/**
 * @brief 把"当前位置"指示（点阵倒三角）画到图层上。
 * @param layer 目标层。
 * @param cx    三角形水平中心（图层坐标）。
 * @param tip_y 尖端 y（图层坐标）；三角形占据它上方 HELM_CLIMB_MARK_H−1 行。
 *
 * @details 现场模式（图表在画、未烘焙）走这里；烘焙模式（导航）由 `climb_cursor`
 *          这个 `lv_image` 对象自己画 ⇒ 两条路径共用同一份点阵，外观一致。
 */
static void helm_climb_mark_blit(lv_layer_t * layer, int32_t cx, int32_t tip_y)
{
    lv_draw_image_dsc_t d;
    lv_area_t a;

    lv_draw_image_dsc_init(&d);
    d.src = &s_climb_mark_dsc;
    a.x1 = cx - HELM_CLIMB_MARK_W / 2;
    a.x2 = a.x1 + (HELM_CLIMB_MARK_W - 1);
    a.y2 = tip_y;
    a.y1 = a.y2 - (HELM_CLIMB_MARK_H - 1);
    lv_draw_image(layer, &d, &a);
}

/**
 * @brief 绘制当前位置指示（小倒三角）。导航用黑色，经过海拔用青绿。
 * @param e LV_EVENT_DRAW_POST。
 * @details 画布可见时指示由 `climb_cursor` 对象承担，本回调直接返回。
 */
static void helm_climb_draw(lv_event_t * e)
{
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t p;
    uint32_t id;
    uint32_t n;

    if (layer == NULL || s_sh.climb_chart == NULL || s_sh.climb_ser == NULL) {
        return;
    }
    if (s_sh.climb_canvas && !lv_obj_has_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_has_flag(s_sh.climb_chart, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    n = lv_chart_get_point_count(s_sh.climb_chart);
    if (n < 2u || s_sh.climb_n < 2u) {
        return;
    }

    id = s_sh.climb_nav ? (uint32_t)s_sh.climb_now : (n - 1u);
    if (id >= n) {
        id = n - 1u;
    }
    lv_chart_get_point_pos_by_id(s_sh.climb_chart, s_sh.climb_ser, id, &p);
    lv_obj_get_coords(s_sh.climb_chart, &coords);
    /* 尖端贴在剖面点**上方 2 px**，三角整体在折线之上；坐标约定与剖面绘制一致
     * （**不补图表自己的 pad** —— 见 `helm_climb_cursor_place()` 的长注释）。 */
    {
        const int32_t half = HELM_CLIMB_MARK_W / 2;
        int32_t cx = coords.x1 + p.x;
        int32_t tip = coords.y1 + p.y - 2;

        if (cx < coords.x1 + half) {
            cx = coords.x1 + half;
        } else if (cx > coords.x2 - half) {
            cx = coords.x2 - half;
        }
        if (tip < coords.y1 + (HELM_CLIMB_MARK_H - 1)) {
            tip = coords.y1 + (HELM_CLIMB_MARK_H - 1);
        } else if (tip > coords.y2) {
            tip = coords.y2;
        }
        helm_climb_mark_blit(layer, cx, tip);
    }
}

/**
 * @brief 搭建爬升页：海拔 / 累计爬升数字 + 路线（或经过）海拔剖面图。
 * @param root 主界面根对象。
 * @details 导航剖面烘焙到 `climb_canvas`，图表隐藏仅作坐标源；
 *          竖线是独立 2 px 对象。烘焙失败仍走图表 DRAW_TASK。
 */
static void helm_build_climb(lv_obj_t * root)
{
    lv_obj_t * card;
    lv_obj_t * chart;

    /* 用户 2026-09-26 给的图：**剖面卡在上、六格在下**，整页满高。
     * ⇒ 海拔页不再给底部三格数据条让位（`rot_on` 里去掉了 CLIMB），
     *   否则 80px 会把剖面卡从 108 压到 56。 */
    s_sh.climb = helm_overlay(root, HELM_SUBBAR_H,
                              HELM_PAGE_H - HELM_SUBBAR_H);
    helm_speed_head(s_sh.climb, "海拔", NULL, NULL);

    /* 剖面卡（内层 chart/canvas/cursor 机制完全不动，只把"剩余爬升"从页头挪
     * 到卡内右上角——图里它在那儿）。 */
    card = lv_obj_create(s_sh.climb);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), HELM_CLIMB_CARD_H);
    helm_style_card(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    chart = lv_chart_create(card);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_top(chart, 22, 0);
    lv_obj_set_style_pad_bottom(chart, 8, 0);
    lv_obj_set_style_pad_hor(chart, 8, 0);
    lv_obj_set_style_radius(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    /* CIRCULAR：写点不得整图 invalidate（SHIFT 会）。导航烘焙成功后图表隐藏。 */
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_point_count(chart, HELM_CLIMB_PROF_N);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(chart, helm_climb_draw_task, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(chart, helm_climb_draw, LV_EVENT_DRAW_POST, NULL);

    s_sh.climb_chart = chart;
    s_sh.climb_ser = lv_chart_add_series(chart, helm_color_snap(HELM_COLOR_CLIMB),
                                         LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(chart, s_sh.climb_ser, LV_CHART_POINT_NONE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);

    s_sh.climb_canvas = lv_canvas_create(card);
    lv_obj_remove_style_all(s_sh.climb_canvas);
    lv_obj_remove_flag(s_sh.climb_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sh.climb_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_sh.climb_canvas, lv_pct(100), lv_pct(100));
    lv_obj_align(s_sh.climb_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_sh.climb_canvas, LV_OBJ_FLAG_HIDDEN);

    /* 位置指示就是一张 `lv_image`（点阵倒三角，颜色已烧进 RGB565A8 数据）：
     * 不需要自绘回调，尺寸由图片自己给，走的也是 EPIC 的 IMAGE 路径。 */
    s_sh.climb_cursor = lv_image_create(card);
    lv_obj_remove_flag(s_sh.climb_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(s_sh.climb_cursor, &s_climb_mark_dsc);
    lv_obj_align(s_sh.climb_cursor, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_sh.climb_cursor, LV_OBJ_FLAG_HIDDEN);

    s_sh.climb_title = helm_lab(card, s_sh.font_lab, HELM_COLOR_CLIMB, "海拔剖面");
    lv_obj_add_flag(s_sh.climb_title, LV_OBJ_FLAG_HIDDEN);  /* 图里没有这个标题 */
    /* "剩余爬升 280 m"：标签 MUTE + 数值 INK，整体贴卡内右上角 */
    s_sh.climb_mark = helm_lab(card, s_sh.font_lab, HELM_COLOR_MUTE, "");
    lv_obj_align(s_sh.climb_mark, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_move_foreground(s_sh.climb_mark);
    lv_obj_move_foreground(s_sh.climb_cursor);
    /* 六格卡：2 列 x 3 行 = 海拔(m) | 坡度(%) / 爬升(m) | 下降(m) /
     * 最高(m) | 最低(m)。格子沿用数据页那套（helm_cell_make）。 */
    {
        static const char * kname[HELM_CLIMB_N] = {
            "海拔 (m)", "坡度 (%)", "爬升 (m)", "下降 (m)", "最高 (m)",
            "最低 (m)"
        };
        lv_obj_t * grid;
        int i2;

        grid = lv_obj_create(s_sh.climb);
        lv_obj_remove_style_all(grid);
        helm_style_card(grid);
        helm_grow_y(grid);
        lv_obj_set_width(grid, lv_pct(100));
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(grid, 0, 0);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

        for (i2 = 0; i2 < HELM_CLIMB_N / 2; i2++) {
            lv_obj_t * r2 = lv_obj_create(grid);

            lv_obj_remove_style_all(r2);
            lv_obj_set_width(r2, lv_pct(100));
            helm_grow_y(r2);
            lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_hor(r2, 10, 0);
            lv_obj_set_style_pad_column(r2, 12, 0);
            lv_obj_clear_flag(r2, LV_OBJ_FLAG_SCROLLABLE);
            if (i2 < HELM_CLIMB_N / 2 - 1) {
                lv_obj_set_style_border_side(r2, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_width(r2, 1, 0);
                lv_obj_set_style_border_color(r2,
                                              helm_color(HELM_COLOR_HAIR), 0);
            }

            helm_cell_make(r2, s_sh.climb_rail, s_sh.climb_lab,
                           s_sh.climb_val, i2 * 2);
            helm_cell_make(r2, s_sh.climb_rail, s_sh.climb_lab,
                           s_sh.climb_val, i2 * 2 + 1);
            helm_label_set(s_sh.climb_lab[i2 * 2], kname[i2 * 2]);
            helm_label_set(s_sh.climb_lab[i2 * 2 + 1], kname[i2 * 2 + 1]);
        }
    }

    s_sh.climb_n = 0;
    s_sh.climb_now = 0;
    s_sh.climb_nav = false;
}

static void helm_build_chrome(lv_obj_t * root)
{
    lv_obj_t * col;

    s_sh.subbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.subbar);
    helm_style_paper(s_sh.subbar);
    lv_obj_set_size(s_sh.subbar, PAGE_HOR_RES, HELM_SUBBAR_H);
    lv_obj_align(s_sh.subbar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(s_sh.subbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.subbar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_sh.subbar, 10, 0);
    lv_obj_add_flag(s_sh.subbar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(s_sh.subbar, 0, 0);
    lv_obj_add_flag(s_sh.subbar, LV_OBJ_FLAG_HIDDEN);
    s_sh.sub_left = helm_lab(s_sh.subbar, s_sh.font_lab, HELM_COLOR_INK, "DATA");
    s_sh.sub_mid = helm_lab(s_sh.subbar, s_sh.font_lab, HELM_COLOR_INK, "00:00");
    /* 右槽：导航中显示"距下一路口 / 剩余里程"，其余时候留空（LAP 已剔除）。 */
    s_sh.sub_right = helm_lab(s_sh.subbar, s_sh.font_lab, HELM_COLOR_INK, "");

    s_sh.rotbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.rotbar);
    helm_style_scr(s_sh.rotbar);
    lv_obj_set_size(s_sh.rotbar, PAGE_HOR_RES, HELM_ROTBAR_H);
    lv_obj_set_style_bg_opa(s_sh.rotbar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_sh.rotbar, HELM_GAP, 0);
    lv_obj_set_style_pad_column(s_sh.rotbar, HELM_GAP, 0);
    lv_obj_set_flex_flow(s_sh.rotbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.rotbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_sh.rotbar, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_sh.rotbar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_remove_flag(s_sh.rotbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_sh.rotbar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_sh.rot_cell[0] = helm_rot_col(s_sh.rotbar, &s_sh.rot_lab[0],
                                    &s_sh.rot_val[0], s_sh.font_rot);
    lv_label_set_text_static(s_sh.rot_lab[0], "速度");
    lv_obj_set_style_text_color(s_sh.rot_val[0], helm_color(HELM_COLOR_NAV), 0);

    s_sh.rot_cell[1] = helm_rot_col(s_sh.rotbar, &s_sh.rot_lab[1],
                                    &s_sh.rot_val[1], s_sh.font_val);
    s_sh.rot_cell[2] = helm_rot_col(s_sh.rotbar, &s_sh.rot_lab[2],
                                    &s_sh.rot_val[2], s_sh.font_val);
    helm_spark_bind(s_sh.rot_cell[1], s_sh.rot_lab[1], 13);
    helm_spark_bind(s_sh.rot_cell[2], s_sh.rot_lab[2], 13);
    lv_label_set_text_static(s_sh.rot_lab[1], "时间");
    lv_label_set_text_static(s_sh.rot_val[1], "00:00");
    lv_label_set_text_static(s_sh.rot_lab[2], "里程");
    lv_label_set_text_static(s_sh.rot_val[2], "0.0");
    lv_obj_move_foreground(s_sh.rot_lab[0]);
    lv_obj_move_foreground(s_sh.rot_val[0]);
    lv_obj_move_foreground(s_sh.rot_lab[1]);
    lv_obj_move_foreground(s_sh.rot_val[1]);
    lv_obj_move_foreground(s_sh.rot_lab[2]);
    lv_obj_move_foreground(s_sh.rot_val[2]);

    s_sh.nav_ban = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.nav_ban);
    /* 回到通栏横幅（用户 2026-09-25："不需要路牌了，直接蓝色横幅"）。 */
    lv_obj_set_size(s_sh.nav_ban, PAGE_HOR_RES, HELM_NAV_BAN_H);
    lv_obj_add_flag(s_sh.nav_ban, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(s_sh.nav_ban, 0, HELM_SUBBAR_H);
    /* 卡片底色 = **状态语义**（导航蓝 / 偏航红 / 到达绿，由状态块每拍刷，见 helm_palette.h
     * 的 BAN_*）。参照市面码表的深色实心转向牌：字反白、卡片近乎实心；原来的"橙字压橙底"
     * （NAV on NAV_FILL）主次不分的问题不再有。 */
    lv_obj_set_style_bg_color(s_sh.nav_ban, helm_color(HELM_COLOR_BAN_NAV), 0);
    /* 半透强度：200（与全局通知弹窗同档）。白卡时代试过 150 —— 地图是透出来了，但地图
     * 路名会和横幅文字叠在一起；200 时底图只剩两成，够看出"卡是半透的"又不抢字。
     * 性能：EPIC 混合 <1% 屏带宽（240×58 混合 ~0.2 ms vs 同面积上屏 ~35 ms）。 */
    lv_obj_set_style_bg_opa(s_sh.nav_ban, 200, 0);
    /* 参照图是圆角牌：去掉原来通栏时代的底边发丝线，改成圆角。 */
    lv_obj_set_style_radius(s_sh.nav_ban, 8, 0);
    lv_obj_set_style_border_width(s_sh.nav_ban, 0, 0);
    /* 参照图：透明黑 + **圆角矩形**，不描边（底边发丝线是通栏横幅时代的东西）。 */
    lv_obj_set_style_radius(s_sh.nav_ban, HELM_NAV_SIGN_RADIUS, 0);
    lv_obj_set_style_border_width(s_sh.nav_ban, 0, 0);
    lv_obj_set_style_border_side(s_sh.nav_ban, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_sh.nav_ban, helm_color(HELM_COLOR_HAIR), 0);
    lv_obj_set_flex_flow(s_sh.nav_ban, LV_FLEX_FLOW_ROW);
    /* 主轴 START：徽章**贴左**（用户 2026-09-25："箭头可以继续向左侧移动"）。文字块不跟着
     * 一起居中，而是 flex_grow 吃掉徽章右侧的全部宽度、再在自己这块里居中（"文字在它显示
     * 的那个区域居中，不要靠左"）。 */
    lv_obj_set_flex_align(s_sh.nav_ban, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_sh.nav_ban, 8, 0);
    lv_obj_set_style_pad_gap(s_sh.nav_ban, 8, 0);
    lv_obj_add_flag(s_sh.nav_ban, LV_OBJ_FLAG_HIDDEN);

    /* 转向图标 = 实心圆角徽章（34×34），字反白。 */
    /* 参照图：白箭头**直接**画在灰卡上（没有白方块），所以徽章只当容器、背景透明。 */
    s_sh.nav_ban_badge = lv_obj_create(s_sh.nav_ban);
    lv_obj_remove_style_all(s_sh.nav_ban_badge);
    lv_obj_set_size(s_sh.nav_ban_badge, 42, 42);
    lv_obj_set_style_radius(s_sh.nav_ban_badge, 13, 0);
    lv_obj_set_style_bg_color(s_sh.nav_ban_badge, helm_color(HELM_COLOR_PAPER), 0);
    /* 徽章必须**不透明**：路牌那版曾把它设成透明（想让白箭头直接压在卡上），
     * 回退配色时漏了这一处，导致白箭头落在浅色卡上完全看不见（用户 2026-09-25 现场发现）。 */
    lv_obj_set_style_bg_opa(s_sh.nav_ban_badge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_sh.nav_ban_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_sh.nav_ban_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.nav_ban_badge, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 箭头走 FreeType 取 30 px（点阵最大只有 22，配 42 px 徽章太瘦；
     * 缺字时回退点阵，两种都能显示 ↑←→U●）。 */
    s_sh.nav_ban_ico = helm_lab(s_sh.nav_ban_badge,
                                s_sh.font_val,
                                HELM_COLOR_PAPER, "↑");
    lv_obj_set_style_text_align(s_sh.nav_ban_ico, LV_TEXT_ALIGN_CENTER, 0);

    col = lv_obj_create(s_sh.nav_ban);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    /* 吃掉徽章右侧的剩余宽度；列内两个子块（r1 距离行 / rd 目的地行）横向居中。 */
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    /* 深色卡自带对比（地图只透出两成），不再需要白卡时代的"文字底板"。 */
    {
        /* 第一行：距离（主字，墨色 22px）+ 动作（次字，灰 15px）—— 原来把
         * "距离 · 动作"塞进同一个标签，主次不分而且低对比。 */
        lv_obj_t * r1 = lv_obj_create(col);

        lv_obj_remove_style_all(r1);
        lv_obj_set_width(r1, LV_SIZE_CONTENT);
        lv_obj_set_height(r1, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(r1, 6, 0);
        lv_obj_clear_flag(r1, LV_OBJ_FLAG_SCROLLABLE);
        s_sh.nav_ban_m = helm_lab(r1, helm_font_sys(22, s_sh.font_val),
                                  HELM_COLOR_INK, "--");
        s_sh.nav_ban_act = helm_lab(r1, helm_font_sys(15, s_sh.font_title),
                                    HELM_COLOR_MUTE, "");
    }
    /* 路名必须走 FreeType：点阵字库（helm_mism4_*）只收了 UI 文案那 486 个字，
     * 真实路名（「金陵路」的 金/陵 都在缺字之列）会整块显示不出来。
     * 地图上的路名一直用 myvendor_system_font_get() 所以正常，横幅这里漏了。 */
    s_sh.nav_ban_rd = helm_lab(col, helm_font_sys(15, s_sh.font_title),
                               HELM_COLOR_MUTE, "");
    /* 地名/路名长了就省略号收尾，不再换行（用户 2026-09-25："挤出去就不显示全名"）。 */
    lv_label_set_long_mode(s_sh.nav_ban_rd, LV_LABEL_LONG_DOT);
    /* 目的地会被挤成两行：宽度吃满底板、每行居中（否则第二行会左对齐吊在下面）。 */
    lv_obj_set_width(s_sh.nav_ban_rd, lv_pct(100));
    lv_obj_set_style_text_align(s_sh.nav_ban_rd, LV_TEXT_ALIGN_CENTER, 0);

    /* 左下角两行（导航剩余 / 全程）与右下角比例尺（用户 2026-09-25）。都浮在地图下沿之上
     * 一点（地图体到 HELM_SUBBAR_H + HELM_MAP_BODY_H 为止，下面就是数据条）。 */
    s_sh.nav_sum = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.nav_sum);
    lv_obj_set_size(s_sh.nav_sum, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(s_sh.nav_sum, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_sh.nav_sum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_sh.nav_sum, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sh.nav_sum, 1, 0);
    s_sh.nav_sum_l1 = helm_lab(s_sh.nav_sum, s_sh.font_title,
                               HELM_COLOR_INK, "--");
    /* 两行都用墨色（用户 2026-09-25："里程不要用灰色字体"）。 */
    s_sh.nav_sum_l2 = helm_lab(s_sh.nav_sum, s_sh.font_title,
                               HELM_COLOR_INK, "--");
    lv_obj_align(s_sh.nav_sum, LV_ALIGN_BOTTOM_LEFT, 6,
                 -(HELM_PAGE_H - HELM_SUBBAR_H - HELM_MAP_BODY_H) - 4);

    s_sh.nav_scale = lv_obj_create(root);
    lv_obj_remove_style_all(s_sh.nav_scale);
    lv_obj_set_size(s_sh.nav_scale, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(s_sh.nav_scale, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_sh.nav_scale, LV_OBJ_FLAG_SCROLLABLE);
    /* 标尺 = 文字 + 下边框当横线 + 两端小竖线（用户 2026-09-25："比例尺线两端缺少竖线"）。 */
    lv_obj_set_flex_flow(s_sh.nav_scale, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sh.nav_scale, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_border_width(s_sh.nav_scale, 1, 0);
    lv_obj_set_style_border_side(s_sh.nav_scale, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_sh.nav_scale, helm_color(HELM_COLOR_INK), 0);
    lv_obj_set_style_pad_hor(s_sh.nav_scale, 2, 0);
    {
        lv_obj_t * tick_l = lv_obj_create(s_sh.nav_scale);

        lv_obj_remove_style_all(tick_l);
        lv_obj_set_size(tick_l, 1, 6);
        lv_obj_set_style_bg_color(tick_l, helm_color(HELM_COLOR_INK), 0);
        lv_obj_set_style_bg_opa(tick_l, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tick_l, LV_OBJ_FLAG_SCROLLABLE);
        s_sh.nav_scale_lab = helm_lab(s_sh.nav_scale, s_sh.font_title,
                                      HELM_COLOR_INK, "200m");
        {
            lv_obj_t * tick_r = lv_obj_create(s_sh.nav_scale);

            lv_obj_remove_style_all(tick_r);
            lv_obj_set_size(tick_r, 1, 6);
            lv_obj_set_style_bg_color(tick_r, helm_color(HELM_COLOR_INK), 0);
            lv_obj_set_style_bg_opa(tick_r, LV_OPA_COVER, 0);
            lv_obj_clear_flag(tick_r, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
    lv_obj_align(s_sh.nav_scale, LV_ALIGN_BOTTOM_RIGHT, -6,
                 -(HELM_PAGE_H - HELM_SUBBAR_H - HELM_MAP_BODY_H) - 4);

    lv_obj_add_flag(s_sh.nav_sum, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sh.nav_scale, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t * helm_sum_row(lv_obj_t * parent)
{
    lv_obj_t * row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, HELM_DOCK_COL_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void helm_build_pause(lv_obj_t * root)
{
    lv_obj_t * head;
    lv_obj_t * ico;
    lv_obj_t * grid;
    lv_obj_t * row;

    s_sh.pause_dock = helm_dock_create(root, true);
    lv_obj_add_flag(s_sh.pause_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(s_sh.pause_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_PAUSE, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    s_sh.pause_title = helm_lab(head, s_sh.font_title, HELM_COLOR_INK, "手动暂停");

    grid = helm_sum_grid(s_sh.pause_dock);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_TIME, HELM_COLOR_NAV, "时间", &s_sh.pause_time);
    helm_sum_cell(row, HELM_ICO_DIST, HELM_COLOR_INK, "里程", &s_sh.pause_dist);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_AVG, HELM_COLOR_NAV, "均速", &s_sh.pause_avg);
    helm_sum_cell(row, HELM_ICO_HR, HELM_COLOR_HR, "均心率", &s_sh.pause_hr);
    helm_softkeys_create(s_sh.pause_dock, HELM_COLOR_INK);
}

static void helm_build_save(lv_obj_t * root)
{
    lv_obj_t * card;
    lv_obj_t * body;
    lv_obj_t * grid;
    lv_obj_t * row;

    s_sh.save_mask = helm_mask_create(root);
    lv_obj_add_flag(s_sh.save_mask, LV_OBJ_FLAG_HIDDEN);
    card = helm_card_create(s_sh.save_mask, HELM_ICO_SAVE, "保存本次骑行？", false);
    body = helm_card_body(card);
    grid = helm_sum_grid(body);
    lv_obj_set_style_margin_top(grid, 8, 0);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_TIME, HELM_COLOR_NAV, "时间", &s_sh.save_time);
    helm_sum_cell(row, HELM_ICO_DIST, HELM_COLOR_INK, "里程", &s_sh.save_dist);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_AVG, HELM_COLOR_NAV, "均速", &s_sh.save_avg);
    helm_sum_cell(row, HELM_ICO_CLIMB, HELM_COLOR_CLIMB, "爬升", &s_sh.save_gain);
    row = helm_sum_row(grid);
    helm_sum_cell(row, HELM_ICO_MAX, HELM_COLOR_NAV, "极速", &s_sh.save_max);
    helm_sum_cell(row, HELM_ICO_HR, HELM_COLOR_HR, "均心率", &s_sh.save_hr);
    helm_softkeys_create(body, HELM_COLOR_INK);
}

static void helm_build_arrive(lv_obj_t * root)
{
    lv_obj_t * head;
    lv_obj_t * ico;

    s_sh.arrive_dock = helm_dock_create(root, true);
    lv_obj_add_flag(s_sh.arrive_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(s_sh.arrive_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_FLAG, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    helm_lab(head, s_sh.font_title, HELM_COLOR_INK, "导航结束");
}

void helm_shell_bind_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(helm_on_prev, NULL);
    /* 主界面不要 KEY1 双击的"延后单击"：注销掉（菜单页注册过，见 helm_menu.c）。 */
    lv_port_buttons_set_page_scroll_double_cb(NULL, NULL);
    lv_port_buttons_set_page_confirm_cb(helm_on_next, NULL);
    lv_port_buttons_set_page_longpress_cb(helm_on_menu, NULL);
    lv_port_buttons_set_page_longpress2_cb(helm_on_rec, NULL);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
    LVGL_STALL("helm_bind_keys attached=%d covered=%d",
        (int)s_sh.attached, (int)s_sh.ui_covered);
}

void helm_shell_autopause_tick(void)
{
    helm_autopause_tick();
    helm_ride_status_publish();
}

void helm_shell_set_paused(bool paused)
{
    if (s_sh.paused == paused) {
        return;
    }

    if (paused && !s_sh.paused) {
        myvendor_sound_pause();
    }

    s_sh.paused = paused;
    if (s_sh.ui_covered) {
        return;
    }

    /* 只翻暂停条，避免从菜单/保存框回来后整页 apply_view 卡住。 */
    helm_pause_dock_apply();
    helm_nums_defer_begin();
}

bool helm_shell_paused(void)
{
    return s_sh.paused;
}

void helm_shell_raise_chrome(void)
{
    helm_raise_chrome();
}

static void helm_poly_defer_cancel(void)
{
    if (s_sh.poly_timer) {
        lv_timer_delete(s_sh.poly_timer);
        s_sh.poly_timer = NULL;
    }
}

static void helm_nums_defer_cancel(void)
{
    if (s_sh.nums_timer) {
        lv_timer_delete(s_sh.nums_timer);
        s_sh.nums_timer = NULL;
    }
}

static void helm_nums_defer_cb(lv_timer_t * t)
{
    helm_page_id_t id;

    LV_UNUSED(t);
    s_sh.nums_timer = NULL;
    if (s_sh.ui_covered || !s_sh.attached) {
        return;
    }

    helm_refresh_numbers();
    id = s_sh.view_id;
    if (helm_page_is_data(id) || helm_obj_shown(s_sh.rotbar)) {
        helm_spark_sync(false);
    }
}

static void helm_nums_defer_begin(void)
{
    helm_nums_defer_cancel();
    s_sh.nums_timer = lv_timer_create(helm_nums_defer_cb, HELM_NUMS_DEFER_MS,
                                      NULL);
    if (s_sh.nums_timer) {
        lv_timer_set_repeat_count(s_sh.nums_timer, 1);
    } else {
        helm_refresh_numbers();
    }
}

static void helm_poly_defer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    s_sh.poly_timer = NULL;
    s_sh.dial_poly_defer = false;
    if (s_sh.ui_covered || !s_sh.attached) {
        return;
    }

    if (helm_page_at(*helm_active_idx()) != HELM_PAGE_STANDBY) {
        return;
    }

    helm_hero_spin_set(s_sh.dial_lock == 1);
    if (s_sh.hero) {
        lv_obj_invalidate(s_sh.hero);
    }
}

static void helm_poly_defer_begin(void)
{
    helm_poly_defer_cancel();
    s_sh.dial_poly_defer = true;
    s_sh.poly_timer = lv_timer_create(helm_poly_defer_cb, HELM_POLY_DEFER_MS,
                                      NULL);
    if (s_sh.poly_timer) {
        lv_timer_set_repeat_count(s_sh.poly_timer, 1);
    } else {
        s_sh.dial_poly_defer = false;
    }
}

void helm_shell_pause_for_cover(void)
{
    if (!s_sh.attached || s_sh.ui_covered) {
        return;
    }

    LVGL_STALL("helm_pause");
    s_sh.ui_covered = true;
    helm_poly_defer_cancel();
    helm_nums_defer_cancel();
    helm_hero_spin_set(false);
    if (s_sh.ui_timer) {
        lv_timer_pause(s_sh.ui_timer);
    }
    if (s_sh.rot_timer) {
        lv_timer_pause(s_sh.rot_timer);
    }
    helm_pane_reset(s_sh.standby);
    helm_pane_reset(s_sh.data);
    helm_pane_reset(s_sh.data_all);
    helm_pane_reset(s_sh.turn);
    helm_pane_reset(s_sh.climb);
}

void helm_shell_resume_after_cover(void)
{
#if MYVENDOR_LVGL_STALL_LOG
    uint32_t t0;
#endif

    if (!s_sh.attached) {
        LVGL_STALL("helm_resume drop !attached");
        return;
    }

#if MYVENDOR_LVGL_STALL_LOG
    t0 = lv_tick_get();
#endif
    s_sh.ui_covered = false;
    if (s_sh.ui_timer) {
        lv_timer_resume(s_sh.ui_timer);
    }
    if (s_sh.rot_timer) {
        lv_timer_resume(s_sh.rot_timer);
    }
    if (helm_page_at(*helm_active_idx()) == HELM_PAGE_STANDBY) {
        helm_poly_defer_begin();
    } else {
        s_sh.dial_poly_defer = false;
        helm_poly_defer_cancel();
    }
    /* 菜单里可能已恢复记录：这里才收起暂停条。 */
    helm_apply_view();
    helm_refresh_numbers();
    helm_raise_chrome();
    LVGL_STALL("helm_resume done %ums id=%u",
        (unsigned)lv_tick_elaps(t0), (unsigned)s_sh.view_id);
}

void helm_shell_refresh(void)
{
    if (s_sh.ui_covered) {
        return;
    }

    /* 停导航后须收起转向页/横幅，不能只刷数字。 */
    helm_apply_view();
    helm_refresh_numbers();
    helm_raise_chrome();
}

/** @brief attach 之前 root 已有的子对象数（地铁画布等外来件个数，重建时保留）。 */
static uint8_t s_build_keep_n;

/** @brief 换主题后用：本层重建一遍（颜色是建页时烘进样式的）。
 *
 *  @details `helm_color()` 的值写在 `lv_obj_set_style_*` 那一刻 ⇒ 换主题只
 *           `lv_obj_invalidate()` 只是"把旧样子重画一遍"，必须**重建对象**才能重新
 *           取值。菜单那边靠 `helm_menu_paint()` 重建行，本层靠这里。
 *
 *  @note 只删**本层自己建**的对象（`helm_shell_attach()` 里打过 LV_OBJ_FLAG_USER_2
 *        标记的）；地图画布等外来件（vmap 建的兄弟节点）必须留着 —— 待机 pane 的
 *        注释里记着"地图画布从没被绘制过"那次花屏的教训。 */
/** @brief 重入保护：本函数正在重建时，内层再进来直接返回。
 *
 *  @details 2026-09-26 上板实测：把 `helm_shell_rebuild()` 接进换主题通知后，
 *           **一次切换就把 UI 卡死**（log 停住、无 panic/assert = 硬挂）。
 *           机制是重入：重建走 `helm_shell_attach()` → 各 builder →
 *           其中有 `map_page_set_map_ui_visible()` 一类会**再触发一次主题通知**，
 *           于是内层又一次进来删对象 —— **删的正是外层正在遍历的子对象**，
 *           链表当场错乱。所以保护的不是"重复做功"，是**外层的遍历**。
 *  @note 内层返回后外层照常跑完，本轮重建仍然生效（不是"跳过"）。 */
static bool s_rebuilding;

static void helm_shell_rebuild_once(void)
{
    map_page_t * map = s_sh.map;
    lv_obj_t * root;
    uint32_t i;

    if (map == NULL || !s_sh.attached) {
        return;
    }

    root = map_page_root(map);
    if (root == NULL) {
        return;
    }

    /* 换主题才走这里 ⇒ 打点不心疼：真卡住时"最后一行"就是现场（这一族踩过两次）。 */
    syslog(LOG_WARNING, "[theme] delete begin n=%u",
           (unsigned)lv_obj_get_child_count(root));

    for (i = lv_obj_get_child_count(root); i > 0u; i--) {
        lv_obj_t * c = lv_obj_get_child(root, i - 1u);

        if (c != NULL && lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) {
            lv_obj_delete(c);
        }
    }

    syslog(LOG_WARNING, "[theme] delete done");

    s_sh.attached = false;
    helm_shell_attach(map);        /* 按开机顺序重建 */
    syslog(LOG_WARNING, "[theme] attach done");
    helm_apply_view();             /* 恢复当前页 */
    syslog(LOG_WARNING, "[theme] view done");
}

void helm_shell_rebuild(void)
{
    if (s_rebuilding) {
        return;                    /* 内层重入：见 s_rebuilding 的注释，这里是防挂的关键 */
    }

    s_rebuilding = true;
    helm_shell_rebuild_once();
    s_rebuilding = false;

    /*
     * ⚠ 换主题后的"剖面重烘焙"必须登记在**重建之后**（对象是新的）：绘制路径拿到这个标志
     * 会用既有数据 `helm_climb_canvas_bake()` 重烘；若重建把剖面数据也清了（日志里的
     * `[theme] hist_reset`），bake() 返回假 ⇒ 走正常 reload 重新取数再烘 —— 两条路都不会
     * 留下空白，也不会烘在已被删除的对象上（2026-09-27 打桩定位到的顺序问题）。
     */
    s_climb_rebake = true;

    /*
     * ⚠ 还要作废**剖面指纹**：`s_climb_eles[]`（每点海拔）与 `s_climb_zone[]`（坡度分段）
     * 是静态缓存，它们的重算被"点数 / 总里程"指纹挡着 —— 而**重建不等于指纹变化**
     * （点数、总里程都没变）⇒ 重建把数组清掉后重算会被跳过 ⇒ 烘焙出来的折线是空的。
     */
    s_climb_fp_pts = 0xffffffffu;
    s_climb_fp_total_cm = 0xffffffffu;
    s_climb_fp_rec_n = 0xff;
    s_climb_fp_rec_slot = 0xffffffffu;
}

/** @brief 待换主题的"主界面组件重建"（① ② 不受影响，见 `helm_shell_theme_mark()`）。 */
static bool s_theme_rebuild_pending;

/** @brief 换主题后登记"主界面组件要重建"（颜色是建页时烘进样式的）。 */
void helm_shell_theme_mark(void)
{
    s_theme_rebuild_pending = true;

    /*
     * 剖面（海拔折线）是**烘焙**成 RGB565 图的（底色 `HELM_COLOR_PAPER`、线色都烘进去了），
     * 换主题必须重烘焙，否则图上还是旧主题的颜色。
     * ⚠ 但登记点**不能在这里**：此时 shell 还没重建，绘制路径会先跑一拍、把图烘进"马上要
     * 被 `helm_shell_rebuild()` 删掉的那个 canvas 对象"上（2026-09-27 打桩实测：
     * `[climb] theme rebake nav=1 n=60 baked=1` 紧跟着就是 `[theme] delete begin` ⇒
     * 烘了等于没烘、用户看到空白）。真正的登记在 `helm_shell_rebuild()` 末尾（重建之后）。
     */
}

/** @brief 待办的主界面重建：**只在地图页是当前页时**执行，否则继续等。
 *
 *  @details 2026-09-26 上板实测（这是本函数存在的全部理由）：
 *           在**菜单页开着**的时候重建地图页那一层，会把菜单页弄死 —— 表现是
 *           菜单按键回调一律 0ms 返回（= `s_menu == NULL`，菜单页被关掉/状态被释放），
 *           画面停在换色后的那一帧不再更新，而主界面（此时才是真正的当前页）一切正常；
 *           对照实验：只跑 ①②（不动这一层）菜单全程活着，接回 ③ 就复现。
 *           而**地图页可见时**重建是好的（实测：连续切换后菜单仍能正常翻行）。
 *           ⇒ 规则：这一层重建只在该页为当前页时做；不在地图页就挂着，
 *             等主循环下一次问的时候（= 用户回到主界面）再重建 —— 这也正是
 *             "颜色要重新取"的那一刻，用户看到的就是重建后的新配色。 */
void helm_shell_theme_apply(void)
{
    if (!s_theme_rebuild_pending) {
        return;
    }

    if (lvgl_page_current_id() != (int)BICYCLE_PM_ID_MAP ||
        lvgl_page_nav_busy()) {
        return;                    /* 不在主界面/转场还没走完 ⇒ 等（见上） */
    }

    s_theme_rebuild_pending = false;
    helm_shell_rebuild();
}

void helm_shell_attach(map_page_t * map)
{
    lv_obj_t * root;

    if (map == NULL || s_sh.attached) {
        return;
    }

    if (s_hist == NULL) {
        s_hist = vmap_malloc(sizeof(float) * (size_t)HELM_HIST_COUNT * HELM_HIST_N);
        if (s_hist != NULL) {
            memset(s_hist, 0, sizeof(float) * (size_t)HELM_HIST_COUNT * HELM_HIST_N);
        }
    }

    root = map_page_root(map);
    if (root == NULL) {
        return;
    }

    /* ⚠ 临时诊断（换主题卡死定位，定位完删）：每步一条，卡住的**最后一条**就是现场。 */
    syslog(LOG_WARNING, "[theme] attach enter children=%u keep_n=%u",
           (unsigned)lv_obj_get_child_count(root), (unsigned)s_build_keep_n);

    /* attach 之前 root 已有的子对象数 = 外来件（地图画布等）的个数，重建时要留着；
     * 本层建完（见下面 build 之后那段）再把**新增**的子对象打上 LV_OBJ_FLAG_USER_2，
     * helm_shell_rebuild() 只删这些。 */
    s_build_keep_n = (uint8_t)lv_obj_get_child_count(root);

    /* ⚠ 走势图登记表（`s_spark[]`）**必须跟着本层的对象一起重建**：表里存的是本层
     * chart/label/series 的指针，而每次 attach 都会把这些对象删掉重造。
     * 2026-09-26 现场（coredump n623）：表**只增不清** ⇒ 重建后表里留着**已释放**的
     * 指针，主界面左右翻页走 `helm_spark_sync()` 时 `lv_obj_has_flag(野指针)`
     * 直接内存故障重启（判据：pc=`lv_obj_has_flag`、lr=`helm_spark_tree_shown`、
     * 故障地址 0x07e90024 是野值）。放在 builders **之前**：delete 回调会先把旧表项
     * 置 NULL，这里再整表清零，随后 builders 按新对象重新登记。 */
    memset(s_spark, 0, sizeof(s_spark));
    s_spark_n = 0;

    s_sh.map = map;
    s_sh.font_lab = helm_font_lab();
    s_sh.font_title = helm_font_title();
    s_sh.font_val = helm_font_val();
    s_sh.font_quad = helm_font_quad();
    s_sh.font_rot = helm_font_rot_speed();
    s_sh.font_speed = helm_font_speed();
    s_sh.font_speed_ride = helm_font_speed_ride();
    s_sh.font_mark = helm_font_mark();

    syslog(LOG_WARNING, "[theme] chrome");
    helm_build_chrome(root);
    syslog(LOG_WARNING, "[theme] standby");
    helm_build_standby(root);
    syslog(LOG_WARNING, "[theme] data");
    helm_build_data(root);
    syslog(LOG_WARNING, "[theme] data_all");
    helm_build_data_all(root);
    syslog(LOG_WARNING, "[theme] turn");
    helm_build_turn(root);
    syslog(LOG_WARNING, "[theme] climb");
    helm_build_climb(root);
    syslog(LOG_WARNING, "[theme] pause");
    helm_build_pause(root);
    syslog(LOG_WARNING, "[theme] save");
    helm_build_save(root);
    syslog(LOG_WARNING, "[theme] arrive");
    helm_build_arrive(root);
    syslog(LOG_WARNING, "[theme] hist_reset");
    helm_hist_reset();
    syslog(LOG_WARNING, "[theme] flag");

    /* 本层新建的子对象打标记 ⇒ helm_shell_rebuild() 重建时只删它们。 */    {
        uint32_t n = lv_obj_get_child_count(root);
        uint32_t k;

        for (k = s_build_keep_n; k < n; k++) {
            lv_obj_t * c = lv_obj_get_child(root, k);

            if (c != NULL) {
                lv_obj_add_flag(c, LV_OBJ_FLAG_USER_2);
            }
        }
    }

    s_sh.attached = true;
    s_sh.home_idx = 0;
    s_sh.ride_idx = 0;
    {
        int32_t last_m = myvendor_devctl_last_ride_m_get();

        s_sh.last_km = (last_m > 50) ? ((float)last_m / 1000.0f) : 0.0f;
    }
    helm_apply_view();
    helm_refresh_numbers();
    helm_shell_bind_keys();
    s_sh.rot_timer = lv_timer_create(helm_rot_cb, HELM_ROT_MS, NULL);
    s_sh.ui_timer = lv_timer_create(helm_ui_cb, HELM_UI_MS, NULL);
}
