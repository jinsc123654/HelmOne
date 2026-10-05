/**
 * @file helm_palette.c
 * @brief Helm One 调色板：**日光 / 夜间两套，运行期可切**。
 *
 * 设计稿：zcode/analysis/helm_menu_redesign/preview/{light,dark}/（20 页各一张）。
 * 用法：
 *   · helm_palette_use(true/false) 直接指定；
 *   · helm_palette_follow_theme() 跟随 lv_pm 当前主题（outdoor ⇒ 夜间）；
 *   · 现存控件把颜色**烘进了样式**，所以切换后要重画/重建当前页
 *     （菜单走 helm_menu_paint()，骑行那几屏要它们自己重建时才吃到新色）。
 *
 * 夜间为什么不能复用白天那套提亮：helm_color() 的提亮/提纯是**为了救半透半反屏上的
 * 暗饱和色**（参见 helm_palette.h 的用色规则）；夜间底色本身就是纸黑，再提亮就把
 * 黑拉成灰。所以夜间走"只量化"的通路（helm_color_night）。
 */

#include <stdio.h>
#include <syslog.h>

#include "helm_palette.h"
#include "lv_pm_theme.h"

/* 主界面换主题要重建（颜色烘在样式里）。声明放这里，免得为一行去动 shell 的头。 */
extern void helm_shell_rebuild(void);
extern void helm_shell_theme_mark(void);
extern void helm_shell_theme_apply(void);
/* 菜单页同理：重设页根/列表底/页头 + 屏级底色（见 helm_menu.c 的同名注释）。 */
extern void helm_menu_theme_reapply(void);
/* 开机 splash：底色也要跟主题（用户 2026-09-26 晚："开关机界面也[要]受主题变化影响"）。 */
extern void startup_page_theme_refresh(void);

static const struct helm_pal_t s_day = {
    .menu_bg         = 0xE4DCD0u,
    .menu_card       = 0xFBF6EEu,
    .menu_lift       = 0x1D4ED8u,
    .menu_sel_muted  = 0xFFFFFFu,
    .menu_hair       = 0xD4C8B8u,
    .menu_fg         = 0x000000u,
    .menu_muted      = 0x6E655Cu,
    .menu_accent     = 0xFF6E12u,
    .scr             = 0xE4DCD0u,
    .paper           = 0xFBF6EEu,
    .ink             = 0x000000u,
    .mute            = 0x6E655Cu,
    .hair            = 0xD4C8B8u,
    .track           = 0xE2DACEu,
    .time            = 0x2A6ED0u,
    .nav             = 0xFF6E12u,
    .nav_fill        = 0xFFC890u,
    .ban_nav         = 0x2F6FD8u,
    .ban_off         = 0xC62B22u,
    .ban_arr         = 0x1E9E52u,
    .ban_nav_fill    = 0xA8C8EEu,
    .ban_off_fill    = 0xF0B0A8u,
    .ban_arr_fill    = 0xAEE0BCu,
    .ban_card        = 0x000000u,
    .hr              = 0xC60000u,
    .hr_fill         = 0xFFD7D6u,
    .cad             = 0x007900u,
    .cad_fill        = 0xCEF3CEu,
    .ok              = 0x00B400u,
    .ok_fill         = 0x3CB83Cu,
    .pause_pill      = 0xE6B400u,
    .pwr             = 0x9C4100u,
    .pwr_fill        = 0xFFE3B5u,
    .climb           = 0x086163u,
    .climb_fill      = 0xD6F3F7u,
    .climb_mark      = 0xFF4500u,
    .nav_spark       = 0xE88830u,
    .hr_spark        = 0xF07878u,
    .cad_spark       = 0x58C060u,
    .pwr_spark       = 0xE09038u,
    .climb_spark     = 0x52BCC4u,
    .elev_flat       = 0x58C060u,
    .elev_flat_ink   = 0x007900u,
    .elev_mid        = 0xE6B400u,
    .elev_mid_ink    = 0x9C4100u,
    .elev_steep      = 0xE05050u,
    .elev_steep_ink  = 0xC60000u,
    .elev_done       = 0xB0A898u,
    .elev_done_ink   = 0x6E6860u,
    .yel             = 0xFFDB00u,
    .yel_spark       = 0xE8C430u,
    .warn            = 0xFFDB00u,
    .gps             = 0x31D36Bu,
    .gps_dead        = 0xF75152u,
    .batt_mid        = 0xFFDB00u,
    .bar             = 0x000000u,
};

static struct helm_pal_t s_night;
static bool s_night_ready;
static const struct helm_pal_t * s_pal = &s_day;

/** @brief lv_color_t → 0xRRGGBB（本表内部用 24 位值，helm_color() 再量化）。 */
static uint32_t pal_hex(lv_color_t c)
{
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | (uint32_t)c.blue;
}

/** @brief 夜间 = 日光的底色/文字整体翻面。
 *  @details **底色/文字/描边/强调一律取 lv_pm 的当前主题**（"night" 那份主题就是
 *           设计稿 preview/dark 的取值）—— 主题是单一真源，这里只做"槽位 → helm token"
 *           的映射；主题没定义到的 helm 专有身份色（心率/踏频/功率/爬升那几支）沿用日光。
 *           次级文字若主题里与正文同色（classic 就是），保留日光的 MUTE，
 *           否则 MUTE 会跟正文一样黑、层级塌掉。 */
static void night_build(void)
{
    const lv_pm_theme_def_t * t = lv_pm_theme_current();

    s_night = s_day;

    if (t != NULL) {
        uint32_t page_bg = pal_hex(t->colors.page_bg);
        uint32_t panel_bg = pal_hex(t->colors.panel_bg);
        uint32_t fg = pal_hex(t->colors.text_primary);
        uint32_t sec = pal_hex(t->colors.text_secondary);
        uint32_t border = pal_hex(t->colors.panel_border);

        if (sec == fg) {
            sec = s_day.mute;               /* 主题没区分主次 ⇒ 保留日光的次级灰 */
        }

        s_night.scr = page_bg;
        s_night.menu_bg = page_bg;
        s_night.paper = panel_bg;
        s_night.menu_card = panel_bg;
        s_night.ink = fg;
        s_night.menu_fg = fg;
        s_night.mute = sec;
        s_night.menu_muted = sec;
        s_night.hair = border;
        s_night.menu_hair = border;
        s_night.track = border;
        s_night.menu_accent = pal_hex(t->colors.accent);
        /* 用户 2026-09-26 的定调（三条一起）：
         *   ① `#181C21` 是**每个 list 行的底色**（不是聚焦色）；
         *   ② 聚焦色**保留日光主题那支**（0x1D4ED8），夜间也用蓝；
         *   ③ 别的深色抬升别拿 #2C2C2E 去冒充聚焦 —— 那是发丝线/轨道色。 */
        s_night.paper = 0x181C21u;
        s_night.menu_card = 0x181C21u;
        s_night.menu_lift = 0x1D4ED8u;
        s_night.menu_sel_muted = 0xFFFFFFu;
        s_night.bar = fg;
    }

    /* ⚠ 指标身份色也照**设计稿 dark 的取值**来（不是把日光值搬过来）：
     * preview/dark 用的是 App 那套 tones —— nav #2563EB / dev #0EA5E9 / sys #64748B /
     * danger #EF4444 / ride #0D9488 / gpx #7C3AED，外加 accent #FF5C33。
     * 日光那几支（橙 FF6E12 / 红 C60000 / 青 086163 / 棕 9C4100）在黑底上偏暗，
     * 这正是"夜间不如设计稿好看"的原因。 */
    s_night.nav = 0xFF5C33u;          /* accent */
    s_night.time = 0x2563EBu;         /* nav 蓝 */
    s_night.hr = 0xEF4444u;           /* danger 红 */
    s_night.hr_fill = 0x3A1414u;
    s_night.cad = 0x0D9488u;          /* ride 青 */
    s_night.cad_fill = 0x103033u;
    s_night.climb = 0x0EA5E9u;        /* dev 天蓝 */
    s_night.climb_fill = 0x102A3Au;
    s_night.pwr = 0x64748Bu;          /* sys 石板灰（也是"其它图标"的默认色调） */
    s_night.pwr_fill = 0x22262Eu;
    s_night.nav_spark = 0xF07A4Cu;
    s_night.hr_spark = 0xEF6A6Au;
    s_night.cad_spark = 0x2AA79Au;
    s_night.pwr_spark = 0x7C8AA3u;
    s_night.climb_spark = 0x4AAEE8u;

    /* 浅色填充（白天靠"提纯"活下来）在夜间反过来要压暗，否则一片白斑。
     * ⚠ 这一族**必须逐个覆盖**：上一版漏了 nav_fill（pale 橙 0xFFC890），
     *   结果设置页上凡是用到 NAV_FILL 的面（曲线填充/speed 相关块）在夜里
     *   还是一片淡橙 —— 用户照片里那块"发白/发橙的底"就是它。 */
    s_night.nav_fill = 0x3A2A12u;
    s_night.ok_fill = 0x14401Au;
    s_night.elev_done = 0x4A4640u;
    s_night.elev_done_ink = 0x6E6860u;
    s_night.ban_nav_fill = 0x24384Fu;
    s_night.ban_off_fill = 0x4A2420u;
    s_night.ban_arr_fill = 0x1E3A26u;
    s_night_ready = true;
}

const struct helm_pal_t * helm_pal_active(void)
{
    return s_pal;
}

bool helm_pal_night(void)
{
    return s_pal != &s_day;
}

void helm_palette_use(bool night)
{
    if (night && !s_night_ready) {
        night_build();
    }

    s_pal = night ? &s_night : &s_day;
}

void helm_palette_follow_theme(void)
{
    const char * n = lv_pm_theme_name(lv_pm_theme_get());

    /* 主题名决定明暗：classic = 日光纸白；outdoor / night = 深色。 */
    helm_palette_use(!(n != NULL && n[0] == 'c'));
}

/** @brief 主题名持久化：/mnt/kv/ui_theme（一行名字，如 "night"）。
 *  @details 用户 2026-09-26：这个设置要进 KV、开机读回来。放 /mnt/kv 是因为它就是
 *           参数区（`/mnt/kv/bt_phone.tsv` 那种），不牵动 devctl 的表结构。 */
#define HELM_THEME_KV_PATH "/mnt/kv/ui_theme"

static void helm_theme_save(void)
{
    const char * n = lv_pm_theme_name(lv_pm_theme_get());
    FILE * f;

    if (n == NULL) {
        return;
    }

    f = fopen(HELM_THEME_KV_PATH, "w");
    if (f == NULL) {
        return;                      /* 读失败不影响使用，只是下次开机回到默认 */
    }

    (void)fprintf(f, "%s\n", n);
    (void)fclose(f);
}

/** @brief 主题名是否已落盘（`/mnt/kv/ui_theme` 存在）。
 *  @details 给 `bicycle_config_apply()` 用：**主题名比地图样式（classic/outdoor）更细**
 *           —— 夜间是它才表达得出的取值，开机再拿 map_style 去覆盖，会把用户选的
 *           夜间在重启后丢掉（实测：选 outdoor→重启后主题变 classic，KV 还被改写成
 *           classic）。所以只要有这个文件，就让它说了算。 */
bool helm_theme_kv_present(void)
{
    FILE * f = fopen(HELM_THEME_KV_PATH, "r");

    if (f == NULL) {
        return false;
    }

    (void)fclose(f);
    return true;
}

/** @brief 开机读回主题名并应用（失败就保持默认）。 */
static void helm_theme_load(void)
{
    char buf[24];
    FILE * f = fopen(HELM_THEME_KV_PATH, "r");

    if (f == NULL) {
        return;
    }

    buf[0] = '\0';
    if (fgets(buf, sizeof(buf), f) != NULL) {
        size_t i;

        for (i = 0; buf[i] != '\0'; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                buf[i] = '\0';
                break;
            }
        }

        if (buf[0] != '\0') {
            (void)lv_pm_theme_set_by_name(buf);
        }
    }

    (void)fclose(f);
}

/** @brief 有待生效的主题（重活等循环顶来做，见 `helm_theme_notify()`）。 */
static bool s_apply_pending;

/** @brief 主题变化回调：**只**换调色板 + 写 KV，重活交给 `helm_theme_poll_apply()`。
 *
 *  @details 为什么要把重活挪出去（2026-09-26 上板实测的教训）：
 *           主题通知是从 `lv_pm_theme_set()` 里**同步**回调的，而它的触发点不确定 ——
 *           用户路径是"设置行按键 → LVGL timer → 菜单处理函数"，控制台路径是
 *           `bicycle_ui_ctl_poll()`。在这两个点上重建菜单行/删建对象，等于在渲染
 *           管线中间改对象树：实测结果是 UI 看着还活着（定时器在跑、按键回调也在跑，
 *           有一次 confirm 回调跑了 5.36 s），但**菜单页从此不再重画**（停在换色后
 *           那一帧、按键看不到变化），主界面反而正常。
 *           现在这里只做"换表 + 存 KV"，把 ①屏底 ②页头 ③主界面组件 的执行挪到
 *           `helm_theme_poll_apply()`，由 UI 主循环在**循环顶**调用 —— 那是本工程
 *           既有的"可以整屏 invalidate + refr"的安全点（见 myvendor_lcd_disp.c 的
 *           `myvendor_lcd_disp_poll_recover_ui()` 注释原文："仅在 LVGL refresh 外做
 *           全 invalidate + refr（loop 顶）"）。 */
static void helm_theme_notify(lv_pm_theme_id_t id, void * user_data)
{
    LV_UNUSED(id);
    LV_UNUSED(user_data);

    s_night_ready = false;               /* 主题变了 ⇒ 夜间表要按新主题重建 */
    syslog(LOG_WARNING, "[theme] notify enter id=%d", (int)id);
    helm_palette_follow_theme();
    syslog(LOG_WARNING, "[theme] notify palette");
    /* ⚠ **KV 写盘也挪到循环顶**（2026-09-27）：它是文件 I/O（/mnt/kv 是 LFS），
     * 实测 12 ms 到 **1.6 s** 不定（`notify enter → notify done` 的间隔就是它，
     * 撞上 FTL/GC 时特别久）。挂在换主题的同步路径里 ⇒ 点一下要等它写完屏幕才动。 */
    s_apply_pending = true;              /* 重活（写 KV + reapply + 重建）等循环顶来做 */
    syslog(LOG_WARNING, "[theme] notify done");
}

/** @brief 主题生效：①屏级底色 ②菜单壳（页根/列表底/页头）③主界面/骑行各组件。
 *  @note 由 UI 主循环在循环顶调用（见 `helm_theme_notify()` 的注释）；没有待办时是空操作。 */
void helm_theme_poll_apply(void)
{
    if (s_apply_pending) {
        s_apply_pending = false;
        syslog(LOG_WARNING, "[theme] apply enter");
        helm_menu_theme_reapply();       /* ①屏级底色 ②页头 + 菜单壳重设样式 */
        syslog(LOG_WARNING, "[theme] apply menu done");
        startup_page_theme_refresh();    /* 开机 splash 的底色（正在放才有效） */
        helm_shell_theme_mark();         /* ③主界面组件：登记重建 */
        lv_obj_invalidate(lv_screen_active());
        helm_theme_save();               /* KV 写盘（慢，但已经不影响可见变化了） */
        syslog(LOG_WARNING, "[theme] apply done");
    }

    /* ③ 每圈都问一次：挂起时它会等"主界面成为当前页"（换主题时正开着菜单就是这种）。
     * 放在 `s_apply_pending` 之外 —— 否则重活挂起后就再也没人问了。 */
    helm_shell_theme_apply();
}

static bool s_notify_hooked;

void helm_palette_follow_theme_hook(void)
{
    if (s_notify_hooked) {
        return;
    }

    s_notify_hooked = true;
    lv_pm_set_theme_notify_cb(helm_theme_notify, NULL);
    helm_theme_load();                   /* 开机先读 KV，再对齐调色板 */
    helm_palette_follow_theme();
}
