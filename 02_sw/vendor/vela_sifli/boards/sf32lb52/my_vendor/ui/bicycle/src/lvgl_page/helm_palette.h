/**
 * @file helm_palette.h
 * @brief Helm One 半透半反配色（对齐 00_doc/UI/MCU 线稿）。
 *
 * 线稿 hex 已按 RGB565 量化。半透半反色域窄、暗饱和色会塌成黑/灰，
 * helm_color() 在进 LVGL 前做一次提亮/提纯，让屏上观感靠近 HTML。
 *
 * 户外半透半反：不要整屏 #FFFFFF。SCR 是场地底（反光仍够），PAPER 是
 * 卡片/数字面（暖纸）。SCR 用浅暖灰，半反屏中灰会发脏。
 * 卡片不描黑框。NAV 是橙色强调（速度 / 选中），不是导航蓝。
 *
 * @section 用色规则（2026-09-26 按实测对比度定，不是审美偏好）
 * PAPER 底上的文字对比度（已过 helm_color + RGB565 量化）：
 *   INK 0x000000   19.80:1   ← 正文一律用它
 *   MUTE 0x6E655C   5.45:1   ← 次级文字
 *   PWR 0x9C4100    4.33:1
 *   HR 0xC60000     3.77:1
 *   TIME 0x2A6ED0   3.73:1
 *   CLIMB 0x086163  3.57:1
 *   CAD 0x007900    3.07:1
 *   NAV 0xFF6E12    2.66:1   ← 最差
 * 小字门槛是 4.5:1 ⇒ **所有色调色都不能当 12px 文字用**（橙色只有 2.66:1，
 * 基本等于糊的）。所以：
 *   颜色只做**形状与状态**——色条、量条、色块、整格淡底；文字一律 INK/MUTE。
 *   数值需要带语义时（心率区间、时速区间）才整体变色，且用整格淡底配合。
 */

#ifndef HELM_PALETTE_H
#define HELM_PALETTE_H

#include "lvgl/lvgl.h"

/* ===================================================================
 * 菜单/设置类页面的表层 token（2026-09-26 现代化改造）。
 *
 * ⚠ **第一次上板实测：深色表层在这块半透半反屏上不成立** —— 黑底反射成浅灰
 *   （照片里整屏发白）、muted 灰字糊掉、选中条看着像一块藏青。原因是**半反屏把
 *   "黑"渲染成环境光的浅灰**，深色主题的对比层级整个塌掉。所以菜单也走纸白，
 *   只保留 App 的**语法**（分组卡/字号层级/缩进发丝线/中性选中/数值内联单位）。
 *   两个表层都用同一套 token 名，切表层只改这一段。
 *
 * 值来源：App `flutter/lib/app/app_theme.dart`（background/card/hairline/muted/
 * accent）。这里是"App 的语法 + 半反屏的配色"，不是 App 的深色。
 * 设计稿与逐屏对照：zcode/analysis/helm_menu_redesign/README.md（preview/light/）。
 * =================================================================== */
#define HELM_COLOR_MENU_BG     0xE4DCD0 /**< 菜单根底（= SCR，与其它页同场）。 */
#define HELM_COLOR_MENU_CARD   0xFBF6EE /**< 分组卡/行底（= PAPER，INK 19.8:1）。 */
/** @brief 选中行底色：**实心蓝色块**（用户 2026-09-26："光标还是可以橙色或者蓝色系"）。
 *  @note ⚠ **淡色/浅色在这块半透半反屏上活不下来**：先试了"中性抬升 #2C2C2E"（他说像
 *        黑块、丑）和"淡蓝 #C7D4E7"（上板拍照：**完全看不清**，屏把浅色一起洗淡）
 *        ⇒ 选中必须做**高饱和实心块**。用户 2026-09-26 在配色试验页从 5 个候选里
 *        挑了 3 号 = 亮蓝 `0x1D4ED8`（黑字标题在它上面 4.07:1）。
 *        橙色（NAV 0xFF6E12）也可行，但橙已被"速度档位/告警"占用。 */
#define HELM_COLOR_MENU_LIFT   0x1D4ED8
/** @brief 选中块**上**的次级文字（副标题/右值）—— 即"灰色字反色"。
 *  @details 用户 2026-09-26：「灰色字体选中后变得很丑，试试灰色字反色」⇒ 只翻**次级**
 *           文字，标题仍 INK（整行反色是另一件事，用户早先否过："不要吧字体反色"）。
 *           实测在 1D4ED8 上：灰 0x6E655C 只有 1.12:1（糊成一团），白 5.15:1。 */
#define HELM_COLOR_MENU_SEL_MUTED 0xFFFFFF
#define HELM_COLOR_MENU_HAIR   0xD4C8B8 /**< 卡内发丝线（= HAIR）。 */
#define HELM_COLOR_MENU_FG     0x000000 /**< 主文字（= INK，19.8:1）。 */
/** @brief 次级文字。**不要用 App 的 #8E8E93**：实测在纸白卡上只有 ~3:1，
 *         上板就是"看不清"（用户原话）；用 MUTE 0x6E655C（5.45:1，实测过）。 */
#define HELM_COLOR_MENU_MUTED  0x6E655C
#define HELM_COLOR_MENU_ACCENT 0xFF6E12 /**< 强调色（= NAV，与页面同一支橙）。 */
/** @brief 卡内发丝线的左缩进（App 是 64/390，这里是 240 上的同比例）。 */
#define HELM_MENU_HAIR_INDENT  40

#define HELM_COLOR_SCR       0xE4DCD0
#define HELM_COLOR_PAPER     0xFBF6EE
#define HELM_COLOR_INK       0x000000
/** @brief 次级文字（动作/目的地/指标标签这类不该抢戏的行）。
 *  @note 2026-09-26 由 0x8A8078 加深到 0x6E655C。实测（过 helm_color 与 RGB565
 *        量化后）在 PAPER 上的对比度：旧值 3.54:1、新值 5.45:1。12px 小字在半反
 *        屏户外读 3.5:1 偏吃力；INK 是 19.8:1。 */
#define HELM_COLOR_MUTE      0x6E655C
#define HELM_COLOR_HAIR      0xD4C8B8
/** @brief 轨道/未选中：速度量条底槽、走势条底槽。 */
#define HELM_COLOR_TRACK     0xE2DACE
/** @brief 「时间」色调：蓝。可读性同下表——只做色条/形状，不做小字。 */
#define HELM_COLOR_TIME      0x2A6ED0
#define HELM_COLOR_NAV       0xFF6E12
#define HELM_COLOR_NAV_FILL  0xFFC890
/**
 * @name 导航提示（路牌）状态色 —— 参考市面骑行码表的深色实心转向牌
 * @brief 用户 2026-09-25 给的参照图：深色实心卡 + 大白箭头 + 大距离，没有次要文字。
 *        我们保留"颜色跟状态走"（导航蓝 / 偏航红 / 到达绿），但字改成反白、卡片近乎实心
 *        —— 半透白卡在浅色地图片上会被底图染色，读起来发灰。
 * @{
 */
#define HELM_COLOR_BAN_NAV   0x2F6FD8 /**< 导航中：蓝（徽章用）。 */
#define HELM_COLOR_BAN_OFF   0xC62B22 /**< 偏航 / 重新规划：红（徽章用）。 */
#define HELM_COLOR_BAN_ARR   0x1E9E52 /**< 到达：绿（徽章用）。 */
/** @brief 横幅底色：淡彩（用户 2026-09-25 回退到"常驻横幅 + 彩徽章 + 底色跟状态"这一版）。
 *        深色卡/路牌/宽箭头那条线作废 —— 点阵图标数据过不了写盘门禁，系统字体又没有宽箭头。 */
/** @brief 横幅**背景**（唯一随状态变的颜色）：中浅彩 —— 比淡彩深一档才在这块屏上
 *        看得出是颜色（淡彩会洗成白），又比饱和色浅，压得住黑字。
 *        徽章（淡橙 + 白箭头）与字体（黑/灰）固定不变（用户 2026-09-25 对着照片定）。 */
#define HELM_COLOR_BAN_NAV_FILL 0xA8C8EE
#define HELM_COLOR_BAN_OFF_FILL 0xF0B0A8
#define HELM_COLOR_BAN_ARR_FILL 0xAEE0BC
/** @brief 路牌卡身：**透明黑** + 圆角矩形（用户 2026-09-25，对着参照图定）。
 *        黑 0x000000 + 200/255 透明度 = 深灰半透，和全局通知弹窗同档。 */
#define HELM_COLOR_BAN_CARD  0x000000
/** @brief 路牌圆角半径（参照图的圆角矩形）。 */
#define HELM_NAV_SIGN_RADIUS 8
/** @} */
#define HELM_COLOR_HR        0xC60000
#define HELM_COLOR_HR_FILL   0xFFD7D6
#define HELM_COLOR_CAD       0x007900
#define HELM_COLOR_CAD_FILL  0xCEF3CE
#define HELM_COLOR_OK        0x00B400
#define HELM_COLOR_OK_FILL   0x3CB83C
#define HELM_COLOR_PAUSE_PILL 0xE6B400
#define HELM_COLOR_PWR       0x9C4100
#define HELM_COLOR_PWR_FILL  0xFFE3B5
#define HELM_COLOR_CLIMB     0x086163
#define HELM_COLOR_CLIMB_FILL 0xD6F3F7
/** @brief 海拔剖面「当前位置」倒三角：橙红（CSS orangered）。
 *         ⚠ 运行时**不用这个宏上色**：颜色已烧进 helm_shell 的 `s_climb_mark_data`（RGB565A8 点阵
 *         —— 带 recolor 的图会被 EPIC 的 eval 拒掉、落到 SW，所以烘色以留在硬件路径上）；
 *         改色要同时改那份数据的 RGB565：0xFF4500 ⇒ 0xFA20（小端 0x20,0xFA）—— 见
 *         里的 `s_climb_mark_dsc`。 */
#define HELM_COLOR_CLIMB_MARK 0xFF4500
/* Spark area: mid-luma / high chroma. HTML --*-fill pastels wash to white
 * on this panel (same class as pause-dock #CEF3CE). Skip helm_color(). */
#define HELM_COLOR_NAV_SPARK   0xE88830
#define HELM_COLOR_HR_SPARK    0xF07878
#define HELM_COLOR_CAD_SPARK   0x58C060
#define HELM_COLOR_PWR_SPARK   0xE09038
#define HELM_COLOR_CLIMB_SPARK 0x52BCC4
/**
 * @name GPX 导航海拔剖面分色
 * @brief 按 |坡度| 三档跳色，已骑过路段灰色；填充与描边成对，不做渐变。
 * @{
 */
#define HELM_COLOR_ELEV_FLAT      0x58C060 /**< 平坦填充（绿）。 */
#define HELM_COLOR_ELEV_FLAT_INK  0x007900 /**< 平坦脊线。 */
#define HELM_COLOR_ELEV_MID       0xE6B400 /**< 中等起伏填充（黄）。 */
#define HELM_COLOR_ELEV_MID_INK   0x9C4100 /**< 中等起伏脊线。 */
#define HELM_COLOR_ELEV_STEEP     0xE05050 /**< 陡变填充（红）。 */
#define HELM_COLOR_ELEV_STEEP_INK 0xC60000 /**< 陡变脊线。 */
#define HELM_COLOR_ELEV_DONE      0xB0A898 /**< 已骑过路段填充（灰）。 */
#define HELM_COLOR_ELEV_DONE_INK  0x6E6860 /**< 已骑过路段脊线。 */
/** @} */
/**
 * @name 时速 / 心率三档跳色
 * @brief 时速 / 心率数字三档跳色：低绿、中亮黄、高红。
 * @details 低档复用 `HELM_COLOR_CAD` / `HELM_COLOR_CAD_SPARK`，
 *          高档复用 `HELM_COLOR_HR` / `HELM_COLOR_HR_SPARK`。
 *          时速切点 18 / 32 km/h，心率切点 120 / 150 bpm。
 *          仅「当前时速」和有效心率跳色；均速 / 极速仍用 `HELM_COLOR_NAV`。
 * @{
 */
#define HELM_COLOR_YEL       0xFFDB00 /**< 中档数字/标签（亮黄）。 */
#define HELM_COLOR_YEL_SPARK 0xE8C430 /**< 中档走势填充。 */
/** @} */
#define HELM_COLOR_WARN      0xFFDB00
#define HELM_COLOR_GPS       0x31D36B
#define HELM_COLOR_GPS_DEAD  0xF75152
#define HELM_COLOR_BATT_MID  0xFFDB00
#define HELM_COLOR_BAR       0x000000
/** @brief 卡片圆角。
 *  @note 2026-09-26 由 6 提到 10：6 在大屏上够用，但这块 240x320 上卡片本来就小、
 *        圆角 6 看着偏"方"，与新的间距/字号层级不搭。 */
#define HELM_RADIUS          10
#define HELM_INSET           6
#define HELM_GAP             4
#define HELM_LIST_ROW_H      56
#define HELM_STATUS_H        24
#define HELM_PAGE_H          296
/* 状态栏（"NAV / 00:00 / LAP"那条）整条剔除（用户 2026-09-25：没用还占宝贵的 22 px）。
 * 置 0 之后所有依赖它的几何自动重排：地图体高度 +22、数据/转向/爬升三个 pane 上移 22
 * 并长高 22、导航横幅贴到页顶。状态栏对象仍创建但永不显示（helm_apply_view 里改成 false）。 */
#define HELM_SUBBAR_H        0
#define HELM_ROTBAR_H        80
#define HELM_ROW2_H          80
#define HELM_MHEAD_H         32
#define HELM_NAV_BAN_H       58
/** @brief 地图为导航横幅让位下移的距离 —— **归零**：实测非导航时（横幅不显示）地图整体
 *        偏下，箭头不在中心，观感更差（用户 2026-09-25）。导航中想让箭头落在可见区域
 *        中心，需要"有横幅时才下移"，那要能动地图页的 map_area 与 vmap view 的几何，
 *        是运行期重排，先不做。 */
#define HELM_NAV_BAN_SHIFT   0
/** @brief 导航提示改成**路牌**：小牌子挂在左上角，导航空档也一直在（用户 2026-09-25）。
 *         原来 240×58 通栏"一直挂着"会吃掉小屏上半屏，而"只在临近路口才弹"又让人看不到
 *         （截图上就完全没出现）⇒ 折中成一块小牌子：常态在第 30 行起算的位置显示
 *         "距下一路口 / 剩余里程"，临近路口时内容换成转向箭头 + 距离。 */
/** @brief 路牌尺寸：白箭头 + 大距离一行。宽 132 是按最长紧凑距离（如 "12.3km"）留的
 *         —— 用户 2026-09-25 提醒"文本宽度会超过显示区域"，所以距离改用紧凑格式 +
 *         label 走 CLIP，双保险。 */
#define HELM_NAV_SIGN_W      132
#define HELM_NAV_SIGN_H      46
#define HELM_NAV_SIGN_X      6
#define HELM_NAV_SIGN_GAP    4
#define HELM_MAP_BODY_H      (HELM_PAGE_H - HELM_SUBBAR_H - HELM_ROTBAR_H)
#define HELM_HAIR_W          1
/* index.html .dock: left/right/bottom 6; padding 8 10 6; border 2; radius 6.
 * Auto height ≈ head 18 + sumg 8+28+8+28 + keys 8+16 + pad 14 + border 4 = 132. */
#define HELM_DOCK_INSET      6
#define HELM_DOCK_BORDER_W   0
#define HELM_DOCK_H          148
#define HELM_DOCK_HEAD_H     20
#define HELM_DOCK_SUM_MT     8
#define HELM_DOCK_ROW_GAP    8
#define HELM_DOCK_COL_GAP    6
#define HELM_DOCK_CELL_H     32
#define HELM_DOCK_ICO_SUM    22
#define HELM_DOCK_ICO_HEAD   18
#define HELM_DOCK_KEY_MT     8
#define HELM_DOCK_KEY_H      16

/** @brief 当前是否夜间（helm_color 要用，所以声明必须在它前面）。 */
bool helm_pal_night(void);

static inline uint8_t helm_snap565(uint8_t c, uint8_t bits)
{
    unsigned maxv = (1u << bits) - 1u;
    unsigned q = ((unsigned)c * maxv + 127u) / 255u;

    return (uint8_t)((q * 255u + maxv / 2u) / maxv);
}

static inline lv_color_t helm_color(uint32_t hex)
{
    int r = (int)((hex >> 16) & 0xff);
    int g = (int)((hex >> 8) & 0xff);
    int b = (int)(hex & 0xff);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int chroma = mx - mn;
    int lum = (r * 3 + g * 6 + b) / 10;

    /* 夜间不做提亮：那套变换是为了救半透半反屏上的暗饱和色，纸黑底上提亮会把黑拉灰。 */
    if (helm_pal_night()) {
        return lv_color_make(helm_snap565((uint8_t)r, 5),
                             helm_snap565((uint8_t)g, 6),
                             helm_snap565((uint8_t)b, 5));
    }

    if (r > 180 && g > 140 && b < 40) {
        /* Yellow #FFDB00 washes to white on the panel — drop luma. */
        r = (r * 82) / 100;
        g = (g * 75) / 100;
    } else if (chroma >= 40 && lum < 150 && mx > 0) {
        /* Dark navy/red/green collapse on transflective — lift same hue. */
        int add = ((150 - lum) * 6) / 10;

        r += (r * add) / mx;
        g += (g * add) / mx;
        b += (b * add) / mx;
    } else if (chroma >= 16 && lum > 165) {
        /* Pale fills wash to white — punch greens harder. */
        int k = (g > r && g > b) ? 4 : 2;

        r += ((r - lum) * k) / 5;
        g += ((g - lum) * k) / 5;
        b += ((b - lum) * k) / 5;
    }

    if (r < 0) {
        r = 0;
    } else if (r > 255) {
        r = 255;
    }

    if (g < 0) {
        g = 0;
    } else if (g > 255) {
        g = 255;
    }

    if (b < 0) {
        b = 0;
    } else if (b > 255) {
        b = 255;
    }

    return lv_color_make(helm_snap565((uint8_t)r, 5),
                         helm_snap565((uint8_t)g, 6),
                         helm_snap565((uint8_t)b, 5));
}

/* RGB565 only — no pale-punch / dark-lift. Spark strokes stay dark on fill. */
static inline lv_color_t helm_color_snap(uint32_t hex)
{
    return lv_color_make(helm_snap565((uint8_t)((hex >> 16) & 0xff), 5),
                         helm_snap565((uint8_t)((hex >> 8) & 0xff), 6),
                         helm_snap565((uint8_t)(hex & 0xff), 5));
}

/* =====================================================================================
 * 运行期调色板（日光 / 夜间）
 *
 * 上面那些 `#define HELM_COLOR_x 0x…` 是**日光值**，这里把它们改成"查当前表"，
 * 于是同一个宏在日光/夜间自动取不同值 —— 所有调用点一行都不用改。
 * 表在 helm_palette.c（夜间值来自设计稿 preview/dark，20 页各一张）。
 * 切换后要重画/重建当前页：现存控件把颜色烘进了样式。
 * =================================================================================== */
struct helm_pal_t {
    uint32_t menu_bg;
    uint32_t menu_card;
    uint32_t menu_lift;
    uint32_t menu_sel_muted;
    uint32_t menu_hair;
    uint32_t menu_fg;
    uint32_t menu_muted;
    uint32_t menu_accent;
    uint32_t scr;
    uint32_t paper;
    uint32_t ink;
    uint32_t mute;
    uint32_t hair;
    uint32_t track;
    uint32_t time;
    uint32_t nav;
    uint32_t nav_fill;
    uint32_t ban_nav;
    uint32_t ban_off;
    uint32_t ban_arr;
    uint32_t ban_nav_fill;
    uint32_t ban_off_fill;
    uint32_t ban_arr_fill;
    uint32_t ban_card;
    uint32_t hr;
    uint32_t hr_fill;
    uint32_t cad;
    uint32_t cad_fill;
    uint32_t ok;
    uint32_t ok_fill;
    uint32_t pause_pill;
    uint32_t pwr;
    uint32_t pwr_fill;
    uint32_t climb;
    uint32_t climb_fill;
    uint32_t climb_mark;
    uint32_t nav_spark;
    uint32_t hr_spark;
    uint32_t cad_spark;
    uint32_t pwr_spark;
    uint32_t climb_spark;
    uint32_t elev_flat;
    uint32_t elev_flat_ink;
    uint32_t elev_mid;
    uint32_t elev_mid_ink;
    uint32_t elev_steep;
    uint32_t elev_steep_ink;
    uint32_t elev_done;
    uint32_t elev_done_ink;
    uint32_t yel;
    uint32_t yel_spark;
    uint32_t warn;
    uint32_t gps;
    uint32_t gps_dead;
    uint32_t batt_mid;
    uint32_t bar;
};

/** @brief 当前生效的调色板（默认日光）。 */
const struct helm_pal_t * helm_pal_active(void);
/** @brief 直接指定日光/夜间。 */
void helm_palette_use(bool night);
/** @brief 跟随 lv_pm 当前主题（"outdoor" 视作夜间）。 */
void helm_palette_follow_theme(void);
/** @brief 把调色板挂到 lv_pm 主题通知上（开机调一次即可）。 */
void helm_palette_follow_theme_hook(void);
/** @brief 主题名（`/mnt/kv/ui_theme`）是否已落盘。
 *  @note 用途见 `bicycle_config_apply()`：有它就别再用地图样式覆盖主题。 */
bool helm_theme_kv_present(void);
/** @brief 让待办的主题生效（重设样式/重建对象）。UI 主循环**循环顶**调，空操作无开销。
 *  @note 为什么不在主题通知里直接做，见 `helm_palette.c` 的 `helm_theme_notify()`。 */
void helm_theme_poll_apply(void);

#undef  HELM_COLOR_MENU_BG
#define HELM_COLOR_MENU_BG (helm_pal_active()->menu_bg)
#undef  HELM_COLOR_MENU_CARD
#define HELM_COLOR_MENU_CARD (helm_pal_active()->menu_card)
#undef  HELM_COLOR_MENU_LIFT
#define HELM_COLOR_MENU_LIFT (helm_pal_active()->menu_lift)
#undef  HELM_COLOR_MENU_SEL_MUTED
#define HELM_COLOR_MENU_SEL_MUTED (helm_pal_active()->menu_sel_muted)
#undef  HELM_COLOR_MENU_HAIR
#define HELM_COLOR_MENU_HAIR (helm_pal_active()->menu_hair)
#undef  HELM_COLOR_MENU_FG
#define HELM_COLOR_MENU_FG (helm_pal_active()->menu_fg)
#undef  HELM_COLOR_MENU_MUTED
#define HELM_COLOR_MENU_MUTED (helm_pal_active()->menu_muted)
#undef  HELM_COLOR_MENU_ACCENT
#define HELM_COLOR_MENU_ACCENT (helm_pal_active()->menu_accent)
#undef  HELM_COLOR_SCR
#define HELM_COLOR_SCR (helm_pal_active()->scr)
#undef  HELM_COLOR_PAPER
#define HELM_COLOR_PAPER (helm_pal_active()->paper)
#undef  HELM_COLOR_INK
#define HELM_COLOR_INK (helm_pal_active()->ink)
#undef  HELM_COLOR_MUTE
#define HELM_COLOR_MUTE (helm_pal_active()->mute)
#undef  HELM_COLOR_HAIR
#define HELM_COLOR_HAIR (helm_pal_active()->hair)
#undef  HELM_COLOR_TRACK
#define HELM_COLOR_TRACK (helm_pal_active()->track)
#undef  HELM_COLOR_TIME
#define HELM_COLOR_TIME (helm_pal_active()->time)
#undef  HELM_COLOR_NAV
#define HELM_COLOR_NAV (helm_pal_active()->nav)
#undef  HELM_COLOR_NAV_FILL
#define HELM_COLOR_NAV_FILL (helm_pal_active()->nav_fill)
#undef  HELM_COLOR_BAN_NAV
#define HELM_COLOR_BAN_NAV (helm_pal_active()->ban_nav)
#undef  HELM_COLOR_BAN_OFF
#define HELM_COLOR_BAN_OFF (helm_pal_active()->ban_off)
#undef  HELM_COLOR_BAN_ARR
#define HELM_COLOR_BAN_ARR (helm_pal_active()->ban_arr)
#undef  HELM_COLOR_BAN_NAV_FILL
#define HELM_COLOR_BAN_NAV_FILL (helm_pal_active()->ban_nav_fill)
#undef  HELM_COLOR_BAN_OFF_FILL
#define HELM_COLOR_BAN_OFF_FILL (helm_pal_active()->ban_off_fill)
#undef  HELM_COLOR_BAN_ARR_FILL
#define HELM_COLOR_BAN_ARR_FILL (helm_pal_active()->ban_arr_fill)
#undef  HELM_COLOR_BAN_CARD
#define HELM_COLOR_BAN_CARD (helm_pal_active()->ban_card)
#undef  HELM_COLOR_HR
#define HELM_COLOR_HR (helm_pal_active()->hr)
#undef  HELM_COLOR_HR_FILL
#define HELM_COLOR_HR_FILL (helm_pal_active()->hr_fill)
#undef  HELM_COLOR_CAD
#define HELM_COLOR_CAD (helm_pal_active()->cad)
#undef  HELM_COLOR_CAD_FILL
#define HELM_COLOR_CAD_FILL (helm_pal_active()->cad_fill)
#undef  HELM_COLOR_OK
#define HELM_COLOR_OK (helm_pal_active()->ok)
#undef  HELM_COLOR_OK_FILL
#define HELM_COLOR_OK_FILL (helm_pal_active()->ok_fill)
#undef  HELM_COLOR_PAUSE_PILL
#define HELM_COLOR_PAUSE_PILL (helm_pal_active()->pause_pill)
#undef  HELM_COLOR_PWR
#define HELM_COLOR_PWR (helm_pal_active()->pwr)
#undef  HELM_COLOR_PWR_FILL
#define HELM_COLOR_PWR_FILL (helm_pal_active()->pwr_fill)
#undef  HELM_COLOR_CLIMB
#define HELM_COLOR_CLIMB (helm_pal_active()->climb)
#undef  HELM_COLOR_CLIMB_FILL
#define HELM_COLOR_CLIMB_FILL (helm_pal_active()->climb_fill)
#undef  HELM_COLOR_CLIMB_MARK
#define HELM_COLOR_CLIMB_MARK (helm_pal_active()->climb_mark)
#undef  HELM_COLOR_NAV_SPARK
#define HELM_COLOR_NAV_SPARK (helm_pal_active()->nav_spark)
#undef  HELM_COLOR_HR_SPARK
#define HELM_COLOR_HR_SPARK (helm_pal_active()->hr_spark)
#undef  HELM_COLOR_CAD_SPARK
#define HELM_COLOR_CAD_SPARK (helm_pal_active()->cad_spark)
#undef  HELM_COLOR_PWR_SPARK
#define HELM_COLOR_PWR_SPARK (helm_pal_active()->pwr_spark)
#undef  HELM_COLOR_CLIMB_SPARK
#define HELM_COLOR_CLIMB_SPARK (helm_pal_active()->climb_spark)
#undef  HELM_COLOR_ELEV_FLAT
#define HELM_COLOR_ELEV_FLAT (helm_pal_active()->elev_flat)
#undef  HELM_COLOR_ELEV_FLAT_INK
#define HELM_COLOR_ELEV_FLAT_INK (helm_pal_active()->elev_flat_ink)
#undef  HELM_COLOR_ELEV_MID
#define HELM_COLOR_ELEV_MID (helm_pal_active()->elev_mid)
#undef  HELM_COLOR_ELEV_MID_INK
#define HELM_COLOR_ELEV_MID_INK (helm_pal_active()->elev_mid_ink)
#undef  HELM_COLOR_ELEV_STEEP
#define HELM_COLOR_ELEV_STEEP (helm_pal_active()->elev_steep)
#undef  HELM_COLOR_ELEV_STEEP_INK
#define HELM_COLOR_ELEV_STEEP_INK (helm_pal_active()->elev_steep_ink)
#undef  HELM_COLOR_ELEV_DONE
#define HELM_COLOR_ELEV_DONE (helm_pal_active()->elev_done)
#undef  HELM_COLOR_ELEV_DONE_INK
#define HELM_COLOR_ELEV_DONE_INK (helm_pal_active()->elev_done_ink)
#undef  HELM_COLOR_YEL
#define HELM_COLOR_YEL (helm_pal_active()->yel)
#undef  HELM_COLOR_YEL_SPARK
#define HELM_COLOR_YEL_SPARK (helm_pal_active()->yel_spark)
#undef  HELM_COLOR_WARN
#define HELM_COLOR_WARN (helm_pal_active()->warn)
#undef  HELM_COLOR_GPS
#define HELM_COLOR_GPS (helm_pal_active()->gps)
#undef  HELM_COLOR_GPS_DEAD
#define HELM_COLOR_GPS_DEAD (helm_pal_active()->gps_dead)
#undef  HELM_COLOR_BATT_MID
#define HELM_COLOR_BATT_MID (helm_pal_active()->batt_mid)
#undef  HELM_COLOR_BAR
#define HELM_COLOR_BAR (helm_pal_active()->bar)

#endif /* HELM_PALETTE_H */
