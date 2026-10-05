/**
 * @file bicycle_demo.c
 * @brief 自行车 UI — 演示模式。四步流程与横幅见 bicycle_demo.h。
 */

#include <nuttx/config.h>

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "bicycle_demo.h"

#include "bicycle_config.h"
#include "bicycle_gpx_sim.h"
#include "bicycle_page_ids.h"
#include "bicycle_ride_gpx.h"
#include "bicycle_runtime.h"
#include "helm_font.h"
#include "helm_palette.h"
#include "helm_widget.h"
#include "lv_pm_core.h"
#include "lvgl_page.h"
#include "map_page.h"
#include "myvendor_bicycle_ctl.h"
#include "myvendor_gpx.h"

/* ============================ 四步的料 ============================ */

typedef enum {
    STEP_GPX_NAV = 0,   /**< ①模拟 GPX 导航。 */
    STEP_NAV_END,       /**< ①→② 之间：**结束导航**（清干净，别让上一个导航影响下一个）。 */
    STEP_PLAN_NAV,      /**< ②模拟坐标点导航。 */
    STEP_NATION,        /**< ③全国地图解析（跳点）。 */
    STEP_THEME,         /**< ④主题切换。 */
    STEP_COUNT
} demo_step_t;

/** @brief 每一步跑完接哪一步（④ 回到 ① 循环）。 */
static const uint8_t k_demo_next[STEP_COUNT] = {
    STEP_NAV_END, STEP_PLAN_NAV, STEP_NATION, STEP_THEME, STEP_GPX_NAV,
};

/**
 * @brief 第 ③ 步跳的点。
 *
 * @note 三条口径（都在 2026-09-29 上板翻车后定的）：
 *  · **必须落在这张卡真有瓦片的格子里** —— 全国图是按 3 km 格打包的，格与格之间会有洞
 *    （实测：`(100.45,36.85)` 青海湖、`(128.06,42.02)` 长白山天池那两个格子**根本没有 vpk**，
 *    跳过去整屏白）。下面这 5 个坐标是**离线按 `mkfs/fat/map` 逐格核过**的：
 *    所在格的 **3×3 邻格全都有 vpk**（2 km 视野怎么摆都不露白边），用的是网格坐标。
 *  · **"在路上 + 有水 + 有林"是实测挑的**（工具 `zcode/analysis/demo_spot_scan.py`，
 *    直接解 VTIL 的 ROAD/WATER/WATERWAY/FOREST 四层）：最近道路 1~44 m、窗内几百条路迹、
 *    自带水面/水线/林地。**青海湖、洱海、千岛湖、长白山、泸沽湖都不合格**（湖边 1~2 km 内
 *    没有可画的路，或没有林地）—— 换点前先跑一遍那个脚本。
 *  · 顺序就是跳的顺序（引擎按表循环，见 `bicycle_gnss_hop_set_pts`）。
 */
static const bicycle_gnss_hop_pt_t k_nation_pts[] = {
    /* ⚠ 字段顺序是 **lat 在前**（与内置城市表一致）。 */
    { "太湖",       31.4900f, 120.2900f },
    { "阳朔",       24.7800f, 110.4900f },
    { "九寨沟",     33.2690f, 103.9117f },
    { "呼伦贝尔",   49.1960f, 119.7361f },
    { "三亚湾",     18.3121f, 109.4058f },
};

#define DEMO_NATION_N (sizeof(k_nation_pts) / sizeof(k_nation_pts[0]))

/** @brief 每一步的横幅文案 + 跑多久（第 ③ 步 = 每个点 10 s × 点数 + 一拍余量）。 */
static const struct {
    const char * text;
    uint32_t ms;
} k_steps[STEP_COUNT] = {
    { "模拟 GPX 导航",    BICYCLE_DEMO_NAV_MS },
    { "导航结束",          BICYCLE_DEMO_NAV_END_MS },
    { "模拟坐标点导航",    BICYCLE_DEMO_NAV_MS },
    { "全国地图解析",      BICYCLE_DEMO_HOP_PERIOD_MS * (uint32_t)DEMO_NATION_N + 1000u },
    { "主题切换",          BICYCLE_DEMO_THEME_MS },
};

/* ============================ 状态 ============================ */

static bool s_on;
static demo_step_t s_step;
/** @brief 本步起点（`lv_tick_get()`；只在 UI 线程读写）。 */
static uint32_t s_step_ms;
/** @brief 本步跑多久（毫秒）。 */
static uint32_t s_step_len_ms;
/** @brief 已经跑完几遍（日志里对进度用）。 */
static uint32_t s_cycle;
/** @brief 本次演示回放哪条轨迹；空 = 没起来。 */
static char s_gpx[192];
/** @brief 开演示之前的主题名（停止时恢复）。 */
static char s_theme0[24];
static bool s_theme_hold;

/** @brief 演示横幅（挂 `lv_layer_top()`；只在演示期间存在，每次露脸 2 秒）。 */
static lv_obj_t * s_ban;
static lv_obj_t * s_ban_lab;
static char s_ban_txt[48];
/** @brief 本次露脸的起点（`lv_tick_get()`）；到期由 `bicycle_demo_poll()` 收掉。 */
static uint32_t s_ban_ms;
/** @brief 本次露脸停留多久（默认 `BICYCLE_DEMO_BAN_MS`；跳点点名要挂久一点）。 */
static uint32_t s_ban_len_ms = BICYCLE_DEMO_BAN_MS;
/** @brief 跳点段上一个点的名字（换点才打一行日志）。 */
static char s_hop_last[24];
/** @brief 第 ③ 步"规划落地"那一行是否已打（每步只打一次）。 */
static bool s_plan_logged;

/** @brief 横幅相对屏幕中心的下移量（px）。0 = 正中间。 */
#define DEMO_BAN_DY   0

/**
 * @brief 每一步的 ASCII 标识（ctl 状态槽只有 16 字节，中文会被截成半个 UTF-8 字）。
 * @note 菜单/横幅上的中文文案是另一套（`bicycle_demo_status()`），别混。
 */
static const char * const k_step_ids[STEP_COUNT] = {
    "gpx_nav", "plan_nav", "nation", "theme",
};

/**
 * @brief 把当前状态写进 ctl 状态槽（`bicycle_nsh demo status` 读它）。
 */
static void demo_publish(void)
{
    myvendor_bicycle_ctl_state_set(MYVENDOR_BICYCLE_CTL_STATE_DEMO,
        s_on ? k_step_ids[s_step] : "off");
}

/* ============================ 横幅 ============================ */

/**
 * @brief 显示/更新演示横幅（**不**重新计时；计时见 `demo_banner_show()`）。
 * @note 文案没变就不动 LVGL（第 ③ 步文案里有当前点名字，每圈都会进这里）。
 */
static void demo_banner_set(const char * text)
{
    lv_obj_t * top;

    if (text == NULL) {
        text = "";
    }

    if (s_ban != NULL && strcmp(s_ban_txt, text) == 0) {
        return;
    }

    strncpy(s_ban_txt, text, sizeof(s_ban_txt) - 1u);
    s_ban_txt[sizeof(s_ban_txt) - 1u] = '\0';

    if (s_ban == NULL) {
        top = lv_layer_top();
        if (top == NULL) {
            return;
        }

        s_ban = lv_obj_create(top);
        lv_obj_remove_style_all(s_ban);
        lv_obj_set_size(s_ban, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        /* 深色实心牌 + 白字：与全局通知横幅同一套观感（半透 200，底下的地图还看得见在动）。 */
        lv_obj_set_style_bg_color(s_ban, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_ban, 200, 0);
        lv_obj_set_style_radius(s_ban, 10, 0);
        lv_obj_set_style_pad_hor(s_ban, 16, 0);
        lv_obj_set_style_pad_ver(s_ban, 9, 0);
        lv_obj_clear_flag(s_ban, LV_OBJ_FLAG_SCROLLABLE);
        /* 不吃按键/滑动：横幅只是看的，底下的菜单与地图照常收事件。 */
        lv_obj_remove_flag(s_ban, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_ban, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(s_ban, LV_ALIGN_CENTER, 0, DEMO_BAN_DY);
        s_ban_lab = helm_label(s_ban, helm_font_sys(15, helm_font_title()),
            0xFFFFFF, s_ban_txt);
    } else {
        lv_label_set_text(s_ban_lab, s_ban_txt);
        lv_obj_align(s_ban, LV_ALIGN_CENTER, 0, DEMO_BAN_DY);
        lv_obj_move_foreground(s_ban);
    }

    /* 半透底色会跟着底下的地图一起重画：换个新文案后要请它重画一次。 */
    lv_obj_invalidate(s_ban);
}

static void demo_banner_clear(void)
{
    if (s_ban != NULL) {
        lv_obj_delete(s_ban);
        s_ban = NULL;
        s_ban_lab = NULL;
    }

    s_ban_txt[0] = '\0';
}

/** @brief 露脸一次（写文案 + 重新计时 `BICYCLE_DEMO_BAN_MS`）。 */
static void demo_banner_show(const char * text)
{
    demo_banner_set(text);
    s_ban_ms = lv_tick_get();
    s_ban_len_ms = BICYCLE_DEMO_BAN_MS;
}

/** @brief 露脸一次，但自己指定停留时长（跳点那步要"切换前报 + 切换后还看得见"）。 */
static void demo_banner_show_for(const char * text, uint32_t ms)
{
    demo_banner_set(text);
    s_ban_ms = lv_tick_get();
    s_ban_len_ms = ms;
}

/**
 * @brief 到点就收横幅。
 */
static void demo_banner_tick(void)
{
    if (s_ban != NULL && lv_tick_elaps(s_ban_ms) >= s_ban_len_ms) {
        demo_banner_clear();
    }
}

/**
 * @brief 第 ③ 步：**切换前**就把下一个点的名字挂出来（用户要求"告诉是什么位置"）。
 *
 * @details 观众面对的是 2 km 窗口里的一块地图，不点名根本不知道跳到哪了。这里的做法是
 *          按**自己的时钟**算"现在该是第几个点、还剩多久换点"：快到切换点时（提前
 *          `BICYCLE_DEMO_ANNOUNCE_MS`）就报**下一个**点的名字，横幅一直挂到切换后
 *          `BICYCLE_DEMO_BAN_MS`。跳点引擎（`bicycle_gnss_hop_*`）按同一周期推进，
 *          两者相差不超过一次定位更新的间隔（~200 ms），提前量足够盖住。
 */
static void demo_nation_announce(void)
{
    uint32_t el = lv_tick_elaps(s_step_ms);
    uint32_t period = BICYCLE_DEMO_HOP_PERIOD_MS;
    uint32_t idx = el / period;
    const char * name;
    char buf[48];

    if (idx >= (uint32_t)DEMO_NATION_N) {
        idx = (uint32_t)DEMO_NATION_N - 1u;
    } else if (((el % period) + BICYCLE_DEMO_ANNOUNCE_MS) >= period
               && idx + 1u < (uint32_t)DEMO_NATION_N) {
        idx += 1u;                       /* 快到换点了：先报下一个 */
    }

    name = k_nation_pts[idx].name;

    /* 换一个点打一行日志：这是"跳到哪了"唯一的复查依据。 */
    if (strcmp(name, s_hop_last) != 0) {
        strncpy(s_hop_last, name, sizeof(s_hop_last) - 1u);
        s_hop_last[sizeof(s_hop_last) - 1u] = '\0';
        syslog(LOG_INFO, "demo: hop -> %s\n", name);

        lv_snprintf(buf, sizeof(buf), "%s · %s", k_steps[STEP_NATION].text, name);
        demo_banner_show_for(buf, BICYCLE_DEMO_ANNOUNCE_MS + BICYCLE_DEMO_BAN_MS);
    }
}

/* ============================ 主题 / 缩放 ============================ */

static void demo_theme_save(void)
{
    const char * n = lvgl_page_theme_name();

    s_theme0[0] = '\0';
    if (n != NULL) {
        strncpy(s_theme0, n, sizeof(s_theme0) - 1u);
        s_theme0[sizeof(s_theme0) - 1u] = '\0';
    }

    s_theme_hold = true;
}

/** @brief 恢复进演示之前那个主题（翻转过才动）。 */
static void demo_theme_restore(void)
{
    const char * n;

    if (!s_theme_hold) {
        return;
    }

    s_theme_hold = false;
    n = lvgl_page_theme_name();
    if (s_theme0[0] == '\0' || (n != NULL && strcmp(n, s_theme0) == 0)) {
        return;
    }

    /* 演示翻出来的主题会被写进 /mnt/kv/ui_theme ⇒ 停止时还原。 */
    (void)lvgl_page_theme_set_by_name(s_theme0);
}

/** @brief 翻主题：判据与动作都跟设置「显示」那一行同源，别自己造第三套。 */
static void demo_theme_flip(void)
{
    (void)lvgl_page_theme_set_by_name(helm_pal_night() ? "classic" : "night");
}

/* ============================ 回放起手 ============================ */

/**
 * @brief 挑一条用来回放的轨迹：用户指定的那条 → 记录/导入目录里最新的那条。
 * @param[out] out 命中的完整路径（失败时空串）。
 * @return 找到为 true。
 * @note 写死的路径**不能是唯一来源**：App 重导一次、换张卡就没了，
 *       演示不该因为一个文件名整个起不来。
 */
static bool demo_pick_gpx(char * out, size_t n)
{
    static const char * const dirs[] = {
        MYVENDOR_GPX_RECORD_DIR,
        MYVENDOR_GPX_IMPORT_DIR,
    };
    char best[192];
    unsigned d;

    best[0] = '\0';

    if (access(BICYCLE_DEMO_GPX, R_OK) == 0) {
        strncpy(best, BICYCLE_DEMO_GPX, sizeof(best) - 1u);
    }

    for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && best[0] == '\0'; d++) {
        DIR * dir = opendir(dirs[d]);
        struct dirent * e;
        time_t best_t = 0;

        if (dir == NULL) {
            continue;
        }

        while ((e = readdir(dir)) != NULL) {
            struct stat st;
            char path[192];
            size_t len = strlen(e->d_name);
            int m;

            if (len < 5u || strcasecmp(e->d_name + len - 4u, ".gpx") != 0) {
                continue;
            }

            m = snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
            if (m < 0 || (size_t)m >= sizeof(path)) {
                continue;
            }

            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }

            if (best[0] == '\0' || st.st_mtime > best_t) {
                best_t = st.st_mtime;
                strncpy(best, path, sizeof(best) - 1u);
                best[sizeof(best) - 1u] = '\0';
            }
        }

        closedir(dir);
    }

    if (best[0] == '\0') {
        out[0] = '\0';
        return false;
    }

    strncpy(out, best, n - 1u);
    out[n - 1u] = '\0';
    return true;
}

/**
 * @brief 起回放（当作模拟 GNSS）。
 * @param[out] path 实际回放的那条（失败时空串）。
 * @return 0 成功。
 */
static int demo_replay_start(map_page_t * page, char * path, size_t n)
{
    float kph = bicycle_config_nav_sim_speed_kph();

    path[0] = '\0';

    if (demo_pick_gpx(path, n)
        && map_page_gnss_sim(page, path, false, kph, 0.0f) == 0) {
        return 0;
    }

    path[0] = '\0';
    return -1;
}

/* ============================ 四步 ============================ */

/** @brief ①：开这段路的 GPX 跟线导航 + 回放当模拟定位。 */
static bool demo_enter_gpx_nav(void)
{
    map_page_t * page = lvgl_page_map();
    char path[192];

    if (page == NULL) {
        syslog(LOG_WARNING, "demo: no map page\n");
        return false;
    }

    /* 每一步都从干净状态起：清掉上一轮/用户留下的导航（含到达锁存）。 */
    map_page_nav_stop(page);

    if (!demo_pick_gpx(path, sizeof(path))) {
        syslog(LOG_WARNING, "demo: no gpx to play\n");
        return false;
    }

    /* ⚠ 顺序不能反：`map_page_nav_from_gpx()` 开头会 `bicycle_gpx_sim_stop()`
     * （它默认"跟线用真实定位"）⇒ 先起回放的话会被它一脚踢掉。反过来先开导航、
     * 再起回放，两者才同时在线。 */
    if (!map_page_nav_from_gpx(page, path, false)) {
        syslog(LOG_WARNING, "demo: gpx nav failed (%s)\n", path);
        return false;
    }

    if (map_page_gnss_sim(page, path, false,
            bicycle_config_nav_sim_speed_kph(), 0.0f) != 0) {
        syslog(LOG_WARNING, "demo: replay failed (%s)\n", path);
        map_page_nav_stop(page);
        return false;
    }

    strncpy(s_gpx, path, sizeof(s_gpx) - 1u);
    s_gpx[sizeof(s_gpx) - 1u] = '\0';
    syslog(LOG_INFO, "demo: step1 gpx nav %s @%.0f km/h\n", path,
        (double)bicycle_config_nav_sim_speed_kph());
    return true;
}

/**
 * @brief 读 GPX 里**第一个轨迹点**（演示规划要一个确定的"从哪儿出发"）。
 * @return 取到为 true。
 *
 * @note 为什么不问 `bicycle_runtime_poll_fix()`（2026-09-29 上板翻车的原因）：
 *       回放引擎是"全国跳 → 路线模拟 → GPX 回放"三级优先，**回放不是永远说了算**。
 *       实测日志里那一次，规划起点取到的是**上一条路线的终点**（= 上一次规划的目的地），
 *       于是"从目的地规划到目的地"→ 21 m 路线 → 一开就报到达。
 *       轨迹起点是这条回放文件本身的事实，直接读文件最稳；读不到才退回"当前定位"。
 * @note 文件是**一整行**的超长 XML（Fly/小米导出都这样），所以按块读、在块里找首点。
 */
static bool demo_gpx_first_pt(const char * path, double * lon, double * lat)
{
    char buf[4096];
    FILE * f = fopen(path, "r");
    size_t n;
    const char * p;
    bool ok = false;

    if (f == NULL) {
        return false;
    }

    n = fread(buf, 1u, sizeof(buf) - 1u, f);
    buf[n] = '\0';
    (void)fclose(f);

    p = strstr(buf, "<trkpt");
    if (p != NULL) {
        const char * la = strstr(p, "lat=\"");
        const char * lo = strstr(p, "lon=\"");

        if (la != NULL && lo != NULL && la < p + 256 && lo < p + 256) {
            double vlat = strtod(la + 5, NULL);
            double vlon = strtod(lo + 5, NULL);

            if (vlat >= -90.0 && vlat <= 90.0 && vlon >= -180.0 && vlon <= 180.0) {
                *lat = vlat;
                *lon = vlon;
                ok = true;
            }
        }
    }

    return ok;
}

/**
 * @brief ③：**假 GPS 按 GPX 从头骑** + 规划到固定坐标点（导航叠在上面）。
 *
 * @details 用户 2026-09-29：**"坐标点导航的轨迹是错的，不应该重头开始那条 GPX 吗"**。
 *          原因是我先前用了 `map_page_nav_plan_sim()` —— sim 版会
 *          `map_page_nav_gnss_for_sim()`：**停掉 GPX 回放、把定位源切成路线模拟**，
 *          于是屏上记下来的"骑行轨迹"是那条规划路线，而不是回放的 GPX。
 *          现在改用 `map_page_nav_plan()`（非 sim 版）：它只停**路线模拟**
 *          （`map_page_nav_gnss_for_gps()`），GPX 回放照跑 ⇒ 假 GPS 从轨迹起点一路骑，
 *          导航线/转向/里程是叠在上面的一层。回放先起，规划起点用文件里第一个
 *          `<trkpt>`，两者落在同一个地方。
 */
static bool demo_enter_plan_nav(void)
{
    map_page_t * page = lvgl_page_map();
    char path[192];
    double from_lon = NAN;
    double from_lat = NAN;

    if (page == NULL) {
        return false;
    }

    if (demo_replay_start(page, path, sizeof(path)) != 0) {
        syslog(LOG_WARNING, "demo: replay for plan failed\n");
        return false;
    }

    strncpy(s_gpx, path, sizeof(s_gpx) - 1u);
    s_gpx[sizeof(s_gpx) - 1u] = '\0';

    /* 起点显式用**轨迹起点**（= 这段假 GPS 的出发点）；读不到才交给"当前定位"。 */
    if (!demo_gpx_first_pt(path, &from_lon, &from_lat)) {
        from_lon = NAN;
        from_lat = NAN;
        syslog(LOG_WARNING, "demo: no trkpt in %s, plan from current fix\n", path);
    }

    /* 方向箭头兜底：万一之前进过"回放 / 只看轨迹"，箭头是关着的，而**规划导航不会
     * 自己把它打开** ⇒ 这一步就会看不到方向箭头（用户 2026-09-29 点名要）。 */
    map_page_set_arrow_visible(page, true);

    if (!map_page_nav_plan(page, from_lon, from_lat,
            BICYCLE_DEMO_PLAN_LON, BICYCLE_DEMO_PLAN_LAT)) {
        syslog(LOG_WARNING, "demo: plan failed\n");
        return false;
    }

    s_plan_logged = false;          /* 等规划落地后再打"路线就绪"那一行 */
    syslog(LOG_INFO, "demo: step2 plan %.7f,%.7f -> %.7f,%.7f (replay keeps driving)\n",
        from_lon, from_lat, BICYCLE_DEMO_PLAN_LON, BICYCLE_DEMO_PLAN_LAT);
    return true;
}

/**
 * @brief ①→② 之间：**结束导航**。
 *
 * @details 用户现场（2026-09-29）："我建议中间增加上导航结束吧，不然上一个导航可能对
 *          下一个有影响"。GPX 跟线导航留下的不只是叠层 —— 还有 `nav_gpx_course`、
 *          GPX 窗口、"到达锁存"、以及**仍然在跑的回放/路线模拟**；直接在上面叠一次
 *          `nav_plan_sim`，规划起点和到达判定都可能被上一条路线带跑。
 *          所以单独留一步，把导航/回放/演示骑行**全部收干净**再进下一步。
 */
static void demo_enter_nav_end(void)
{
    map_page_t * page = lvgl_page_map();

    if (page != NULL) {
        map_page_nav_stop(page);
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
        map_page_end_ride(page);       /* 演示骑行不落盘 */
    } else {
        bicycle_gpx_sim_stop();
        bicycle_route_sim_stop();
    }

    syslog(LOG_INFO, "demo: nav end (clean slate for step2)\n");
}

/** @brief ③：结束导航与回放，收掉演示骑行，开始跳点。 */
static void demo_enter_nation(void)
{
    map_page_t * page = lvgl_page_map();

    if (page != NULL) {
        map_page_nav_stop(page);                                /* 结束导航 */
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);  /* 停回放/路线模拟 */
        /* 收掉这次演示骑行：未保存的 GPX 丢弃、轨迹线清掉。跳点动辄上千公里，
         * 不收就会往轨迹里灌一条横跨全国的长线、里程也涨到几千公里。 */
        map_page_end_ride(page);
        /* ⚠ **不要动瓦片缩放**（2026-09-29 上板翻车）：全国图是**只按 z14 打包**的
         * （逐个 vpk 核过，里面全是 z14 瓦片），把视图拉到 z11 就是请求一批不存在的
         * 瓦片 ⇒ 整段跳点全白屏。要"看宽一点"只用 `s,F` 缩放，别改 z。 */
    } else {
        bicycle_gpx_sim_stop();
        bicycle_route_sim_stop();
    }

    bicycle_gnss_hop_set_pts(k_nation_pts, (uint32_t)DEMO_NATION_N);
    bicycle_gnss_hop_start(BICYCLE_DEMO_HOP_PERIOD_MS, (unsigned)DEMO_NATION_N);
    s_hop_last[0] = '\0';

    syslog(LOG_INFO, "demo: step3 nation %u pts x %u ms\n",
        (unsigned)DEMO_NATION_N, (unsigned)BICYCLE_DEMO_HOP_PERIOD_MS);
}

/** @brief ④：停跳点（跳的是合成定位），换另一套主题。 */
static void demo_enter_theme(void)
{
    bicycle_gnss_hop_stop();
    demo_theme_flip();
    syslog(LOG_INFO, "demo: step4 theme -> %s\n", helm_pal_night() ? "night" : "classic");
}

/** @brief 进某一步：设时长 + 干这一步的活 + 换横幅。 */
static void demo_goto(demo_step_t step)
{
    bool ok = true;

    s_step = step;
    s_step_ms = lv_tick_get();
    s_step_len_ms = k_steps[step].ms;

    switch (step) {
    case STEP_GPX_NAV:
        ok = demo_enter_gpx_nav();
        break;

    case STEP_NAV_END:
        demo_enter_nav_end();
        break;

    case STEP_PLAN_NAV:
        ok = demo_enter_plan_nav();
        break;

    case STEP_NATION:
        demo_enter_nation();
        break;

    case STEP_THEME:
    default:
        demo_enter_theme();
        break;
    }

    if (!ok) {
        /* 这一步起不来也照常提示这一步的横幅（看得见"到哪一步了"）。 */
        syslog(LOG_WARNING, "demo: step %d did not start\n", (int)step);
    }

    demo_banner_show(k_steps[step].text);
    demo_publish();
}

/* ============================ 开关 ============================ */

int bicycle_demo_start(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();

    if (s_on) {
        return BICYCLE_DEMO_OK;
    }

    if (lvgl_page_map() == NULL) {
        return BICYCLE_DEMO_ERR_NO_MAP;
    }

    /* 真实骑行中不开：演示会改里程/轨迹/主题，用户这一趟就废了。 */
    if ((rt != NULL && rt->recording) || bicycle_ride_gpx_active()) {
        return BICYCLE_DEMO_ERR_RIDING;
    }

    demo_theme_save();
    s_cycle = 0;
    s_gpx[0] = '\0';
    s_on = true;

    demo_goto(STEP_GPX_NAV);
    if (s_gpx[0] == '\0') {
        bicycle_demo_stop();
        return BICYCLE_DEMO_ERR_NAV;
    }

    /* 开演示的唯一入口在菜单里，而演示要看地图 ⇒ 让菜单让位
     * （与"骑行记录 → 导航"同一个姿势：起完导航再收页）。 */
    if (lvgl_page_current_id() == BICYCLE_PM_ID_MENU) {
        (void)lv_pm_close_page_msg(NULL);
    }

    syslog(LOG_NOTICE, "demo: start (gpx=%s)\n", s_gpx);
    return BICYCLE_DEMO_OK;
}

void bicycle_demo_stop(void)
{
    map_page_t * page;

    if (!s_on) {
        return;
    }

    s_on = false;

    /* 跳点在任何一步都可能还开着（比如刚进第 ③ 步就被关）。 */
    bicycle_gnss_hop_stop();

    page = lvgl_page_map();
    if (page != NULL) {
        if (s_step <= STEP_PLAN_NAV) {
            /* 演示骑行不落盘：每次演示都留一条 TRK 会把骑行记录灌满。 */
            map_page_end_ride(page);
        }

        map_page_nav_stop(page);
        (void)map_page_gnss_sim(page, NULL, false, 0.0f, 0.0f);
    } else {
        bicycle_gpx_sim_stop();
        bicycle_route_sim_stop();
    }

    demo_banner_clear();
    demo_theme_restore();
    demo_publish();
    syslog(LOG_NOTICE, "demo: stop (%u cycle(s) done)\n", (unsigned)s_cycle);
}

void bicycle_demo_toggle(void)
{
    if (bicycle_demo_active()) {
        bicycle_demo_stop();
    } else {
        (void)bicycle_demo_start();
    }
}

bool bicycle_demo_active(void)
{
    return s_on;
}

const char * bicycle_demo_status(void)
{
    static char buf[48];

    if (!s_on) {
        return "自动演示：导航 / 跳点 / 主题";
    }

    switch (s_step) {
    case STEP_NATION:
        lv_snprintf(buf, sizeof(buf), "%s · %s", k_steps[STEP_NATION].text,
            bicycle_gnss_hop_cur_name());
        return buf;

    case STEP_GPX_NAV:
    case STEP_PLAN_NAV:
    case STEP_THEME:
    default:
        return k_steps[s_step].text;
    }
}

const char * bicycle_demo_error_text(int rc)
{
    switch (rc) {
    case BICYCLE_DEMO_ERR_NO_MAP:
        return "地图还没准备好";
    case BICYCLE_DEMO_ERR_NAV:
        return "起不了导航（先导入 GPX？）";
    case BICYCLE_DEMO_ERR_RIDING:
        return "请先结束当前骑行";
    default:
        return "无法开始";
    }
}

void bicycle_demo_poll(void)
{
    map_page_t * page;
    uint32_t elapsed;

    if (!s_on) {
        return;
    }

    demo_banner_tick();

    if (s_step == STEP_NATION) {
        demo_nation_announce();       /* 切换前点名（自己带横幅计时） */
    }

    /* 第 ③ 步：**规划完毕才开始计时**（用户要求）。规划的 route worker 是异步的，
     * 期间把本步起点一直往后挪 = 计时不走；规划一落地就开始数那 30 秒。
     * 上限 `BICYCLE_DEMO_PLAN_WAIT_MAX_MS`：规划卡住时别把演示挂在这儿。 */
    if (s_step == STEP_PLAN_NAV) {
        page = lvgl_page_map();
        if (page != NULL && map_page_nav_planning(page)
            && lv_tick_elaps(s_step_ms) < BICYCLE_DEMO_PLAN_WAIT_MAX_MS) {
            s_step_ms = lv_tick_get();
        } else if (page != NULL && !s_plan_logged && map_page_nav_active(page)) {
            /* 规划落地：打一行"路线就绪 + 点数"，配 `[nav] paint: seen=1 seg=…/N` 就是
             * "规划线真的画出来了"的判据（用户点名要"规划轨迹要绘制"）。 */
            uint32_t pts = 0;
            uint32_t total_cm = 0;
            uint32_t now_idx = 0;

            (void)map_page_nav_progress(page, &pts, &total_cm, &now_idx, 0);
            s_plan_logged = true;
            map_page_set_arrow_visible(page, true);
            syslog(LOG_INFO, "demo: step2 route ready pts=%u len=%.2fkm arrow=on\n",
                (unsigned)pts, (double)total_cm / 100000.0);
        }
    }

    elapsed = lv_tick_elaps(s_step_ms);
    if (elapsed < s_step_len_ms) {
        /* 导航段提前收工：到终点了、规划失败了、被手动停了 —— 都没必要把
         * 剩下的时间空耗掉（但给个下限，免得刚起来那两拍规划还没落地就翻页）。 */
        if ((s_step == STEP_GPX_NAV || s_step == STEP_PLAN_NAV)
            && elapsed >= 5000u) {
            page = lvgl_page_map();
            if (page != NULL && !map_page_nav_active(page)
                && !map_page_nav_planning(page)) {
                syslog(LOG_INFO, "demo: nav step %d ended early at %us\n",
                    (int)s_step, (unsigned)(elapsed / 1000u));
            } else {
                return;
            }
        } else {
            return;
        }
    }

    if (s_step == STEP_THEME) {
#if BICYCLE_DEMO_LOOP
        s_cycle++;
        syslog(LOG_INFO, "demo: cycle %u done, back to step1\n", (unsigned)s_cycle);
        demo_goto((demo_step_t)k_demo_next[STEP_THEME]);
#else
        syslog(LOG_NOTICE, "demo: cycle done, stop\n");
        bicycle_demo_stop();
#endif
        return;
    }

    demo_goto((demo_step_t)k_demo_next[s_step]);
}
