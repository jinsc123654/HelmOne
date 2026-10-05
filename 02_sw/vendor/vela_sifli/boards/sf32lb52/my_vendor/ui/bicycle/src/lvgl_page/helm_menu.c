/**
 * @file helm_menu.c
 * @brief KEY1 长按菜单：对齐 pages/menu.html 及子页。
 */

#include "helm_menu.h"

#include "Version.h"
#include "bicycle_page_anima.h"
#include "bicycle_page_ids.h"
#include "board_malloc.h"
#include "helm_font.h"
#include "helm_font_lab.h"
#include "helm_palette.h"
#include "helm_pwr.h"
#include "helm_shell.h"
#include "helm_toolbox.h"
#include "helm_widget.h"
#include "live_map/map_page.h"
#include "lv_pm_bar.h"
#include "lv_pm_core.h"
#include "lv_pm_overlay.h"
#include "lv_pm_port.h"
#include "lvgl_page.h"
#include "bicycle_env.h"
#include "bicycle_demo.h"
#include "bicycle_gpx_sim.h"
#include "bicycle_runtime.h"
#include "bicycle_status_bar.h"
#include "myvendor_devctl.h"
#include "myvendor_gnss.h"
#include "myvendor_sys.h"
#include "myvendor_identity.h"
#include "myvendor_sound.h"
#include "myvendor_gpx.h"
#include "myvendor_mtp.h"
#include "sf32lb_dvfs.h"
#include "bicycle_ride_gpx.h"
#include "gpx_decode.h"
#include "Vendor/Board/lv_port/lv_port_buttons.h"
#include "vmap/vmap_config.h"
#include "vmap/vmap_geo.h"
#include "lvgl/src/draw/lv_draw_line.h"
#include "lvgl/src/draw/lv_draw_rect.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <syslog.h>
#include "myvendor_board_sensor.h"
/* 打桩要读 lv_draw_task_t 的 type/draw_dsc —— 它在 LVGL 的私有头里
 *（和 helm_shell.c 直接用 lv_draw_buf.h 同一种做法）。 */
#include "lvgl/src/draw/lv_draw_private.h"
#include <malloc.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define HELM_MENU_STACK  8
#define HELM_MENU_ROWS   32
/** @brief GPX 文件名 UTF-8 上限（导入中文名；点阵字库盖不住，列表用 TTF）。 */
#define HELM_GPX_NAME_MAX  64

enum {
    HELM_KIND_GO = HELM_ITEM_GO,
    HELM_KIND_SW = HELM_ITEM_SW,
    HELM_KIND_VAL = HELM_ITEM_VAL,
    HELM_KIND_SLIM = HELM_ITEM_SLIM
};

typedef enum {
    HELM_SCR_ROOT = 0,
    HELM_SCR_NAV,
    HELM_SCR_GPX,
    HELM_SCR_SENSORS,
    HELM_SCR_RIDES,
    HELM_SCR_SETTINGS,
    HELM_SCR_TEST,
    HELM_SCR_EPH,
    HELM_SCR_ABOUT,
    HELM_SCR_FONTLAB,
    HELM_SCR_COLORLAB,
    HELM_SCR_GRADECAL,
    HELM_SCR_INBOX,
    HELM_SCR_NAVPTS,
    HELM_SCR_NAVPT_DETAIL,
    HELM_SCR_NAVPT_ACTIONS,
    HELM_SCR_FAVS,
    HELM_SCR_SCAN,
    HELM_SCR_SCAN_PICK,
    HELM_SCR_PHONE,
    HELM_SCR_GPX_IMPORT,
    HELM_SCR_GPX_RECORD,
    HELM_SCR_RIDE_DETAIL,
    HELM_SCR_TOOLS,
    HELM_SCR_TOOLFACE,
    HELM_SCR_SYSSTAT,
    HELM_SCR_SYS_DISKS,
    HELM_SCR_SYS_MEMORY,
    HELM_SCR_SYS_THREADS,
    /** @brief 系统资源 → 卫星：按星座看"在视 / 锁定 / 参与定位"的颗数。 */
    HELM_SCR_SYS_GNSS
} helm_scr_t;

enum {
    ACT_NOP = 0,
    ACT_ENTER_NAV,
    ACT_ENTER_GPX,
    ACT_ENTER_SENSORS,
    ACT_ENTER_RIDES,
    ACT_ENTER_SETTINGS,
    ACT_ENTER_TEST,
    ACT_ENTER_EPH,
    ACT_ENTER_ABOUT,
    ACT_ENTER_FONTLAB,
    ACT_ENTER_COLORLAB,
    ACT_ENTER_GRADECAL,
    ACT_ENTER_INBOX,
    ACT_ENTER_NAVPTS,
    ACT_ENTER_NAVPT_REC,
    ACT_ENTER_FAVS,
    ACT_ENTER_SCAN_HR,
    ACT_ENTER_SCAN_CAD,
    ACT_ENTER_SCAN_PWR,
    ACT_ENTER_TOOLS,
    ACT_ENTER_SYSSTAT,
    ACT_ENTER_SYS_DISKS,
    ACT_ENTER_SYS_MEMORY,
    ACT_ENTER_SYS_THREADS,
    ACT_ENTER_SYS_GNSS,
    ACT_ENTER_PHONE,
    ACT_PAIR_OPEN,
    ACT_PAIR_UNBIND,
    ACT_TOOL_COMPASS,
    ACT_TOOL_LEVEL,
    ACT_TOOL_GMETER,
    ACT_TOOL_ALT,
    ACT_NAV_COORDS,
    ACT_NAV_FAV,
    ACT_NAV_FAV_START,
    ACT_NAV_STOP,
    ACT_NAV_SKIP,
    ACT_NAV_NEAREST,
    ACT_ENTER_GPX_IMPORT,
    ACT_ENTER_GPX_RECORD,
    ACT_RIDE_OPEN,
    ACT_RIDE_DEL,
    ACT_RIDE_NAV,
    ACT_RIDE_REV,
    ACT_RIDE_CONT,
    ACT_TOGGLE_BT,
    ACT_TOGGLE_SENSOR,
    ACT_CYCLE_BL,
    ACT_CYCLE_THEME,
    ACT_CYCLE_UNIT,
    ACT_CYCLE_TZ,
    ACT_CYCLE_GNSS_SOLVER,
    ACT_TOGGLE_AUTOPAUSE,
    ACT_TOGGLE_NOTIF,
    ACT_TOGGLE_CALLS,
    ACT_TOGGLE_USB,
    ACT_TOGGLE_SOUND,
    ACT_TOGGLE_EPH_AUTO,
    ACT_EPH_WRITE,
    ACT_RIDES_CLEAR_ALL,
    ACT_SCAN_CONNECT,
    ACT_SCAN_AUTO,
    ACT_SCAN_START,
    ACT_INBOX_OPEN,
    ACT_DEMO_TOGGLE,
    ACT_POWEROFF
};

typedef struct {
    char label[HELM_GPX_NAME_MAX];
    char sub[40];
    char value[24];
    uint8_t act;
    uint8_t extra;
    uint8_t kind;
    uint8_t progress;
    uint32_t progress_color;
    helm_ico_id_t ico;
    bool on;
    bool progress_on;
    /** @brief 整行底色覆盖（配色试验页：让候选色**整行铺满**，看选中时的真实观感）。
     *  @note 必须由 helm_row_set 每行清零 —— items 数组跨页复用，残留会漏到别的页。 */
    bool bg_set;
    uint32_t bg_color;
    bool bg_dark;
    /** @brief 分组号（1 起；0 = 未分组）。卡片/标题/发丝线都按它画。 */
    uint8_t grp;
} helm_row_t;

#define HELM_MGRP_MAX 8
/** @brief 分组标题：row = 该组**首行**下标（画卡片和标题都要知道从哪行起）。 */
typedef struct {
    const char * title;
    uint8_t row;
} helm_mgrp_t;

typedef struct {
    helm_scr_t scr;
    uint8_t sel;
} helm_frame_t;

typedef struct {
    lv_obj_t * root;
    lv_obj_t * title;
    lv_obj_t * list;
    lv_obj_t * about;
    lv_obj_t * fontlab;
    lv_obj_t * gradecal;
    lv_obj_t * grade_val;
    lv_obj_t * grade_raw;
    lv_obj_t * grade_off;
    lv_obj_t * grade_imu;
    lv_obj_t * grade_baro;
    lv_obj_t * grade_alt;
    lv_obj_t * grade_track;
    lv_obj_t * grade_target;
    lv_obj_t * grade_bub;
    lv_obj_t * grade_tilt;
    lv_obj_t * empty;
    lv_obj_t * skip_dock;
    lv_obj_t * del_dock;
    lv_obj_t * del_lab;
    lv_obj_t * ride;
    lv_obj_t * ride_track;
    lv_obj_t * ride_dist;
    lv_obj_t * ride_keys;
    lv_obj_t * toolface;
    lv_timer_t * poll;
    lv_timer_t * fav_plan_timer;
    lv_obj_t * plan_mask;
    lv_obj_t * plan_head;
    lv_obj_t * plan_msg;
    lv_obj_t * plan_hint;
    uint8_t fav_plan_phase;
    bool fav_planning;
    bool fav_plan_fail;
    bool fav_plan_kick;
    uint32_t fav_notice_t0;
    uint32_t fav_notice_ms;
    helm_frame_t stack[HELM_MENU_STACK];
    uint8_t sp;
    uint8_t count;
    uint8_t fav_order[MYVENDOR_DEVCTL_FAVORITE_MAX];
    uint8_t fav_order_n;
    helm_row_t items[HELM_MENU_ROWS];
    helm_mgrp_t grps[HELM_MGRP_MAX];
    uint8_t grp_n;
} helm_menu_t;

/* 先声明：分组机制与星历文本在文件前部（星历页）就要用。 */
static void helm_grp_begin(helm_menu_t * m, uint8_t row, const char * title);
static void helm_eph_local(char * out, size_t n, uint32_t when, bool with_time);
static bool helm_eph_times_cached(uint32_t * last, uint32_t * next);

static helm_menu_t * s_menu;
/** @brief 「主题」那一行被点了几下（`[theme] click #N` 的计数；用户要求能看到点击模式）。 */
static uint32_t s_theme_clicks;
static int8_t s_list_enter_dir;
static bool s_unit_imperial;
static uint8_t s_scan_kind;
static uint8_t s_scan_phase;
static uint32_t s_scan_t0;
static uint32_t s_sensor_sig;
static uint8_t s_sensor_del_idx;
static bool s_sensor_del;
/** 确认弹窗（`del_dock`）是**在哪个页面上**弹出来的。
 *
 *  存在的意义（2026-09-25）：`helm_menu_paint()` 会被**背景刷新**反复调用
 *  （`helm_poll_cb` 400 ms 比传感器签名、SCAN 页扫描倒计时…），而它原来
 *  **无条件**把弹窗藏掉 ⇒ 用户刚按出来的"删除此记录？"下一拍就消失
 *  （用户现场："ble sensor 删除配对的确认弹窗总是自己消失，很难删除"）。
 *  弹窗有**三个不同主人**（GPX 记录 / 手机配对解绑 / BLE 传感器记录），
 *  其中 GPX 那条**不用任何 arm 标志**（它的"已装备"就是"可见"本身）⇒
 *  不能靠 arm 标志判断，只能记"弹出来的那一页"：**同一页重画 ⇒ 留着**，
 *  **真的换了页 ⇒ 收起**。 */
static helm_scr_t s_del_scr;
/** 「手机蓝牙」页：长按右键问过一遍之后，等这一下点击确认解绑。 */
static bool s_phone_unbind_arm;
/** 解绑目标槽（0..2）。 */
static unsigned s_phone_unbind_idx;
/** 「手机蓝牙」页：点过「配对新手机」之后，等这一下点击确认开窗口。 */
static bool s_phone_pair_arm;
/** 「手机蓝牙」页上一帧台数（最多 3）与倒计时秒。 */
static unsigned s_phone_shown_n;
/** 上一帧显示的配对窗口剩余秒数（倒计时的重画基准）。 */
static unsigned s_phone_shown_sec;
static lv_timer_t * s_phone_watch;
static bool s_scan_jump;
static bool s_scan_link_wait;
static lv_obj_t * s_squash;
static bool s_squash_restore;
static helm_tool_id_t s_tool_id;

#define HELM_SCAN_OFF   0u
#define HELM_SCAN_WAIT  1u
#define HELM_SCAN_RUN   2u
#define HELM_SCAN_DONE  3u
#define HELM_POLL_MS    400u
#define HELM_GPX_LIST_MAX  HELM_MENU_ROWS
#define HELM_RIDE_PT_MAX   96
#define HELM_SYS_DISK_N    3
#define HELM_SYS_HEAP_N    3
#define HELM_SYS_THREAD_N  HELM_MENU_ROWS

/* 系统页"磁盘"那一栏的刷新周期。
 *
 * **别调小**：这个循环对三个挂载点调 statfs()，其中 /mnt/lfs 和 /mnt/kv
 * 是两个 LittleFS 卷，而 littlefs_statfs() 会 lfs_fs_size() **遍历整棵元数据
 * 树且完全不缓存**（~55 ms/卷）。原先这里写的是 2000 ms，也就是每 2 秒在 UI
 * 线程上白烧约 110 ms —— diag 曾因为同一处 API 占 22% CPU，见 sf32lb_sdio.c
 * 里 sf32lb_sd_fs_ok() 的注释。容量变化本来就慢，30 秒足够；进页面时走的是
 * force=true，所以首屏数字仍然是即时的。 */
#define HELM_SYS_DISK_MS   30000u

#define HELM_STAT_MAGIC    0x31545348u /* HST1 */
#define HELM_STAT_VER      4u
#define HELM_STAT_PATH     "/mnt/lfs/ride_stat"
#define HELM_STAT_TMP      "/mnt/lfs/ride_stat.tmp"

typedef struct {
    uint64_t total;
    uint64_t used;
    uint64_t free;
    bool valid;
} helm_sys_usage_t;

typedef struct {
    pid_t pid;
    char name[32];
    size_t stack_total;
    size_t stack_used;
    uint16_t cpu_permille;
    bool cpu_valid;
} helm_sys_thread_t;

typedef struct {
    helm_sys_thread_t thread[HELM_SYS_THREAD_N];
    uint8_t count;
    uint8_t seen;
} helm_sys_threads_t;

static const char * const s_sys_disk_path[HELM_SYS_DISK_N] = {
    "/mnt/lfs", "/mnt/fat", "/mnt/kv"
};

static const char * const s_sys_disk_name[HELM_SYS_DISK_N] = {
    "记录盘", "地图盘", "数据盘"
};

static const helm_ico_id_t s_sys_disk_icon[HELM_SYS_DISK_N] = {
    HELM_ICO_SAVE, HELM_ICO_NAV, HELM_ICO_CHIP
};

static const char * const s_sys_heap_name[HELM_SYS_HEAP_N] = {
    "片内 SRAM", "系统 PSRAM", "图形 PSRAM"
};

static helm_sys_usage_t s_sys_disks[HELM_SYS_DISK_N];
static helm_sys_usage_t s_sys_heaps[HELM_SYS_HEAP_N];
static helm_sys_threads_t s_sys_threads;
static uint32_t s_sys_disk_tick;
static uint32_t s_sys_heap_tick;
static uint32_t s_sys_thread_tick;
static uint32_t s_sys_stack_tick;
static uint32_t s_sys_count_tick;
static uint32_t s_sys_ui_sig;
static uint8_t s_sys_thread_n;
static bool s_sys_disk_ready;
static bool s_sys_heap_ready;
static bool s_sys_thread_ready;
static bool s_sys_stack_ready;
static bool s_sys_count_ready;

static char s_gpx_names[HELM_GPX_LIST_MAX][HELM_GPX_NAME_MAX];
static uint32_t s_gpx_size[HELM_GPX_LIST_MAX];
static uint32_t s_gpx_mtime[HELM_GPX_LIST_MAX];
static uint8_t s_gpx_n;
static const char * s_gpx_dir;

/** 两个目录字符串是否指向同一个目录。
 *
 * 原来这三处用 `==` 比指针：同一个宏字面量还能被编译器合并而"侥幸成立"，
 * 但和运行期传入的路径比就几乎永远为假（-Waddress 已提示）。改成比字符串。
 */
static bool gpx_same_dir(const char * a, const char * b)
{
  if (a == NULL || b == NULL)
    {
      return a == b;
    }

  return strcmp(a, b) == 0;
}
static char s_ride_name[HELM_GPX_NAME_MAX];
static char s_ride_loaded[HELM_GPX_NAME_MAX];
static const char * s_ride_dir;
static const char * s_ride_loaded_dir;
static char s_navpt_path[160];
static char s_navpt_title[HELM_GPX_NAME_MAX];
static float s_ride_lon[HELM_RIDE_PT_MAX];
static float s_ride_lat[HELM_RIDE_PT_MAX];
static uint16_t s_ride_pt_n;
static double s_ride_km;
static uint32_t s_ride_sec;
static bool s_ride_has_time;

typedef struct {
    char name[HELM_GPX_NAME_MAX];
    const char * dir;
    double km;
    uint32_t sec;
    uint32_t size;
    uint32_t mtime;
    bool have_time;
    bool ok;
} helm_gpx_stat_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t n;
} helm_stat_hdr_t;

typedef struct {
    char name[HELM_GPX_NAME_MAX];
    uint32_t size;
    uint32_t mtime;
    uint32_t dist_m;
    uint32_t sec;
    uint8_t have_time;
    uint8_t pad[3];
} helm_stat_rec_t;

static helm_gpx_stat_t s_gpx_stat[HELM_GPX_LIST_MAX];
static uint8_t s_gpx_stat_n;
static bool s_stat_loaded;
static char s_stat_pend_name[HELM_GPX_NAME_MAX];
static double s_stat_pend_km;
static uint32_t s_stat_pend_sec;
static bool s_stat_pend;

static uint8_t helm_sensor_n(void)
{
    myvendor_sys_sensor_ui_t ui;
    uint8_t n = 0;
    uint8_t i;

    myvendor_sys_sensor_ui_get(&ui);
    for (i = 0; i < MYVENDOR_SYS_SENSOR_KIND_N; i++) {
        if (ui.slot[i].link == MYVENDOR_SYS_SENSOR_LINK_READY) {
            n++;
        }
    }

    return n;
}

static const char * helm_scan_kind_lab(uint8_t kind)
{
    if (kind == MYVENDOR_SYS_SENSOR_KIND_CSC) {
        return "踏频";
    }

    if (kind == MYVENDOR_SYS_SENSOR_KIND_CPS) {
        return "功率";
    }

    return "心率";
}

static const char * helm_scan_anon(uint8_t kind)
{
    if (kind == MYVENDOR_SYS_SENSOR_KIND_CSC) {
        return "踏频器";
    }

    if (kind == MYVENDOR_SYS_SENSOR_KIND_CPS) {
        return "功率计";
    }

    return "心率带";
}

static helm_ico_id_t helm_sensor_ico(uint8_t kind)
{
    if (kind == MYVENDOR_SYS_SENSOR_KIND_CSC) {
        return HELM_ICO_CAD;
    }

    if (kind == MYVENDOR_SYS_SENSOR_KIND_CPS) {
        return HELM_ICO_BOLT;
    }

    return HELM_ICO_HR;
}

static void helm_slot_status(char * val, size_t vn, char * sub, size_t sn,
                             const myvendor_sys_sensor_slot_t * slot,
                             bool radio)
{
    if (val != NULL && vn > 0) {
        val[0] = '\0';
    }

    if (sub != NULL && sn > 0) {
        sub[0] = '\0';
    }

    if (!radio) {
        if (val != NULL) {
            lv_snprintf(val, vn, "外设未开");
        }

        return;
    }

    if (slot == NULL ||
        slot->link == MYVENDOR_SYS_SENSOR_LINK_IDLE) {
        if (val != NULL) {
            lv_snprintf(val, vn, "未连接");
        }

        return;
    }

    if (slot->link == MYVENDOR_SYS_SENSOR_LINK_CONNECTING) {
        if (val != NULL) {
            lv_snprintf(val, vn, "连接中");
        }

        return;
    }

    if (val != NULL) {
        lv_snprintf(val, vn, "已连接");
    }

    if (sub == NULL || sn == 0) {
        return;
    }

    if (slot->bat_pct >= 0 && slot->bat_pct <= 100) {
        lv_snprintf(sub, sn, "电量 %d%%", (int)slot->bat_pct);
    } else {
        lv_snprintf(sub, sn, "电量未知");
    }
}

static void helm_fmt_pct(char * buf, size_t n, float v)
{
    int g10 = (int)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    int a = g10 / 10;
    int b = g10 % 10;

    if (b < 0) {
        b = -b;
    }

    if (g10 < 0 && a == 0) {
        lv_snprintf(buf, n, "-0.%d%%", b);
        return;
    }

    lv_snprintf(buf, n, "%d.%d%%", a, b);
}

static void helm_grade_refresh(helm_menu_t * m)
{
    char buf[24];

    if (m == NULL || m->gradecal == NULL) {
        return;
    }

    bicycle_env_tick();
    if (m->grade_val) {
        if (bicycle_env_imu_valid()) {
            helm_fmt_pct(buf, sizeof(buf), bicycle_env_grade_pct());
        } else {
            lv_snprintf(buf, sizeof(buf), "--");
        }

        lv_label_set_text(m->grade_val, buf);
    }

    if (m->grade_raw) {
        if (bicycle_env_imu_valid()) {
            helm_fmt_pct(buf, sizeof(buf), bicycle_env_raw_grade_pct());
        } else {
            lv_snprintf(buf, sizeof(buf), "--");
        }

        lv_label_set_text(m->grade_raw, buf);
    }

    if (m->grade_off) {
        helm_fmt_pct(buf, sizeof(buf), bicycle_env_offset_pct());
        lv_label_set_text(m->grade_off, buf);
    }

    if (m->grade_imu) {
        lv_label_set_text(m->grade_imu,
                          bicycle_env_imu_valid() ? "已就绪" : "等待中");
    }

    if (m->grade_bub) {
        myvendor_sys_vec3_t a;

        myvendor_board_sensor_accel_get(&a);
        if (a.valid) {
            float roll = atan2f(a.x, a.z) * (180.0f / (float)M_PI);
            bool ok = fabsf(roll) < 1.0f;
            int off = (int)(roll * 12.0f);

            if (off > 60) {
                off = 60;
            } else if (off < -60) {
                off = -60;
            }

            lv_obj_set_pos(m->grade_bub, 74 + off, 14);
            lv_obj_set_style_bg_color(m->grade_bub,
                helm_color(ok ? HELM_COLOR_OK : HELM_COLOR_NAV), 0);
            lv_obj_set_style_bg_color(m->grade_target,
                helm_color(ok ? HELM_COLOR_OK : HELM_COLOR_TRACK), 0);
            lv_snprintf(buf, sizeof(buf), "左右 %+0.1f", (double)roll);
            lv_label_set_text(m->grade_tilt, buf);
        }
    }

    if (m->grade_baro) {
        if (bicycle_env_baro_valid()) {
            int h10 = (int)(bicycle_env_hpa() * 10.0f + 0.5f);

            lv_snprintf(buf, sizeof(buf), "%d.%d hPa", h10 / 10, h10 % 10);
            lv_label_set_text(m->grade_baro, buf);
        } else {
            lv_label_set_text(m->grade_baro, "--");
        }
    }

    if (m->grade_alt) {
        if (bicycle_env_baro_valid()) {
            lv_snprintf(buf, sizeof(buf), "%dm",
                        (int)(bicycle_env_altitude_m() + 0.5f));
            lv_label_set_text(m->grade_alt, buf);
        } else {
            lv_label_set_text(m->grade_alt, "--");
        }
    }
}

static void helm_scan_tick(void)
{
    myvendor_sys_sensor_ui_t ui;

    if (s_scan_phase == HELM_SCAN_OFF) {
        return;
    }

    myvendor_sys_sensor_ui_get(&ui);
    if (ui.scanning) {
        s_scan_phase = HELM_SCAN_RUN;
        return;
    }

    if (s_scan_phase == HELM_SCAN_RUN) {
        s_scan_phase = HELM_SCAN_DONE;
        return;
    }

    if (s_scan_phase == HELM_SCAN_WAIT &&
        lv_tick_elaps(s_scan_t0) >= MYVENDOR_DEVCTL_SENSOR_SCAN_MS_DEFAULT) {
        s_scan_phase = HELM_SCAN_DONE;
    }
}

static void helm_scan_begin(void)
{
    s_scan_t0 = lv_tick_get();
    s_sensor_sig = 0;
    s_scan_jump = true;
    if (!myvendor_devctl_sensor_get()) {
        s_scan_phase = HELM_SCAN_OFF;
        return;
    }

    (void)myvendor_devctl_sensor_scan(MYVENDOR_DEVCTL_SENSOR_SCAN_MS_DEFAULT);
    s_scan_phase = HELM_SCAN_WAIT;
}

static void helm_scan_idle_ui(void)
{
    s_scan_phase = HELM_SCAN_OFF;
    s_scan_jump = false;
}

static void helm_scan_end(void)
{
    if (s_scan_phase != HELM_SCAN_OFF) {
        (void)myvendor_devctl_sensor_scan_stop();
    }

    s_scan_link_wait = false;
    helm_scan_idle_ui();
}

static void helm_scan_connect_idx(uint8_t idx)
{
    myvendor_sys_sensor_ui_t ui;
    uint8_t k;

    if (idx == 0) {
        return;
    }

    /** 一地址一槽位：这台设备已经被别的类型占着，本地就拒掉。
     *
     *  服务侧（ble_sensor_connect_index_kind）用同一判据也会拒，但那边只能打
     *  日志，界面会白等一场「正在连接」。用 UI 已拿到的槽位快照先判，能立刻给
     *  用户一句话。正常情况列表会把 linked 的行藏掉，走到这里的是「已绑定、
     *  当前 IDLE 等重连」的边角。 */
    myvendor_sys_sensor_ui_get(&ui);

    {
        static const uint8_t zaddr[MYVENDOR_SYS_SENSOR_ADDR_LEN];

        for (k = 0; k < MYVENDOR_SYS_SENSOR_KIND_N; k++) {
            if (k == s_scan_kind ||
                ui.slot[k].link == MYVENDOR_SYS_SENSOR_LINK_IDLE) {
                continue;
            }

            if (idx <= ui.found_n &&
                memcmp(ui.found[idx - 1].addr, zaddr, sizeof(zaddr)) != 0 &&
                memcmp(ui.found[idx - 1].addr, ui.slot[k].addr,
                       sizeof(zaddr)) == 0) {
                char msg[32];

                lv_snprintf(msg, sizeof(msg), "已被%s占用",
                            helm_scan_kind_lab(k));
                lv_pm_notify_show("蓝牙设备", msg, 2000);
                myvendor_sound_warn();
                return;
            }
        }
    }

    /* 按**当前页面**的类型绑定：同一台设备既广播 CSC 又广播 CPS（甚至 HR）时，
     * 交给服务侧「取第一种匹配」会连到别的槽位上去。
     *
     * 寻址用 **MAC**，不用扫描表下标：`idx` 是这张**活表**的序号 —— 扫描回调
     * 会并发改写它（"更弱的设备被替换"那条规则），而且**每次用户起扫都会整表清空**。
     * 从"界面显示第 idx 项"到"底层按 idx 查表"之间只要表变过，就会连到别的设备
     * （甚至全零地址）。MAC 在显示那一刻就已确定，之后表怎么变都指向同一台。
     * 上面那段"已被 XX 占用"的检查本来就在用 `ui.found[idx-1].addr`。 */
    (void)myvendor_devctl_sensor_connect_addr(ui.found[idx - 1].addr,
                                              ui.found[idx - 1].addr_type,
                                              s_scan_kind,
                                              ui.found[idx - 1].name);
    /* connect_index 自己停观察者。这里再 post SCAN_STOP 会盖掉 CONNECT。 */
    helm_scan_idle_ui();
    s_scan_link_wait = true;
    lv_pm_notify_show("蓝牙设备", "正在连接", 1500);
}

static bool helm_scr_needs_sensor_poll(helm_scr_t scr)
{
    return scr == HELM_SCR_SCAN || scr == HELM_SCR_SCAN_PICK ||
           scr == HELM_SCR_SENSORS || scr == HELM_SCR_INBOX ||
           scr == HELM_SCR_EPH || scr == HELM_SCR_SYSSTAT ||
           scr == HELM_SCR_SYS_DISKS || scr == HELM_SCR_SYS_MEMORY ||
           scr == HELM_SCR_SYS_THREADS || scr == HELM_SCR_SYS_GNSS;
}

static uint32_t helm_sensor_sig(void)
{
    myvendor_sys_sensor_ui_t ui;
    uint32_t s;
    uint8_t i;

    myvendor_sys_sensor_ui_get(&ui);
    s = (uint32_t)ui.scanning | ((uint32_t)ui.found_n << 1) |
        ((uint32_t)s_scan_phase << 8) |
        (myvendor_devctl_sensor_get() ? 0x1000u : 0);
    {
        uint8_t inbox_n = 0;

        myvendor_sys_inbox_get(NULL, &inbox_n, 0);
        s ^= (uint32_t)inbox_n << 20;
    }
    for (i = 0; i < MYVENDOR_SYS_SENSOR_KIND_N; i++) {
        s ^= (uint32_t)ui.slot[i].link << (12u + i * 2u);
        s ^= (uint32_t)(uint8_t)ui.slot[i].bat_pct << (16u + i);
        s ^= (uint32_t)(uint8_t)ui.slot[i].name[0] << (18u + i);
        s ^= (uint32_t)ui.slot[i].addr[0] << (i & 7u);
        s ^= (uint32_t)ui.slot[i].addr[1] << ((i + 3u) & 7u);
    }

    for (i = 0; i < ui.found_n; i++) {
        s ^= ((uint32_t)ui.found[i].table_idx << (i & 7u)) ^
             (uint32_t)(uint8_t)ui.found[i].name[0];
    }

    s ^= myvendor_devctl_sensor_rec_gen();
    return s;
}

static uint8_t helm_sys_pct(uint64_t part, uint64_t total)
{
    if (total == 0) {
        return 0;
    }

    if (part >= total) {
        return 100;
    }

    return (uint8_t)((part * 100ull + total / 2ull) / total);
}

static void helm_sys_row_bar_ex(helm_menu_t * m, uint8_t row, uint8_t fill,
                                uint8_t used)
{
    if (m == NULL || row >= HELM_MENU_ROWS) {
        return;
    }

    m->items[row].progress_on = true;
    m->items[row].progress = fill > 100u ? 100u : fill;
    if (used >= 90u) {
        m->items[row].progress_color = HELM_COLOR_HR;
    } else if (used >= 70u) {
        m->items[row].progress_color = HELM_COLOR_YEL;
    } else {
        m->items[row].progress_color = HELM_COLOR_CAD;
    }
}

/** @brief 总览条：按已用比例填充。 */
static void helm_sys_row_bar(helm_menu_t * m, uint8_t row, uint8_t used)
{
    helm_sys_row_bar_ex(m, row, used, used);
}

/** @brief 子页条：按剩余比例填充，未使用时满条绿色。 */
static void helm_sys_row_free_bar(helm_menu_t * m, uint8_t row, uint8_t used)
{
    uint8_t fill = (used >= 100u) ? 0u : (uint8_t)(100u - used);

    helm_sys_row_bar_ex(m, row, fill, used);
}

static void helm_sys_fmt_size(char * buf, size_t n, uint64_t bytes)
{
    if (bytes >= 1024ull * 1024ull) {
        uint64_t mb10 = (bytes * 10ull) / (1024ull * 1024ull);

        lv_snprintf(buf, n, "%lu.%luM", (unsigned long)(mb10 / 10ull),
                    (unsigned long)(mb10 % 10ull));
    } else {
        lv_snprintf(buf, n, "%luK",
                    (unsigned long)((bytes + 512ull) / 1024ull));
    }
}

static void helm_sys_fmt_pair(char * buf, size_t n, uint64_t used,
                              uint64_t total)
{
    char a[16];
    char b[16];

    helm_sys_fmt_size(a, sizeof(a), used);
    helm_sys_fmt_size(b, sizeof(b), total);
    lv_snprintf(buf, n, "%s / %s", a, b);
}

static void helm_sys_refresh_disks(bool force)
{
    uint32_t now = lv_tick_get();
    unsigned i;

    if (!force && s_sys_disk_ready &&
        lv_tick_elaps(s_sys_disk_tick) < HELM_SYS_DISK_MS) {
        return;
    }

    for (i = 0; i < HELM_SYS_DISK_N; i++) {
        struct statfs fs;
        uint64_t total;
        uint64_t free;

        if (i == 0u && myvendor_mtp_lfs_quiesce()) {
            continue;
        }

        memset(&fs, 0, sizeof(fs));
        if (statfs(s_sys_disk_path[i], &fs) != 0 || fs.f_bsize == 0 ||
            fs.f_blocks == 0) {
            continue;
        }

        total = (uint64_t)fs.f_bsize * (uint64_t)fs.f_blocks;
        free = (uint64_t)fs.f_bsize * (uint64_t)fs.f_bavail;
        s_sys_disks[i].total = total;
        s_sys_disks[i].free = free;
        s_sys_disks[i].used = total > free ? total - free : 0;
        s_sys_disks[i].valid = true;
    }

    s_sys_disk_tick = now;
    s_sys_disk_ready = true;
}

static void helm_sys_usage_from_mallinfo(helm_sys_usage_t * usage,
                                         const struct mallinfo * info)
{
    if (usage == NULL || info == NULL || info->arena <= 0) {
        return;
    }

    usage->total = (uint64_t)info->arena;
    usage->used = (uint64_t)info->uordblks;
    usage->free = (uint64_t)info->fordblks;
    usage->valid = true;
}

static void helm_sys_refresh_heaps(bool force)
{
    uint32_t now = lv_tick_get();
    struct mallinfo info;

    if (!force && s_sys_heap_ready &&
        lv_tick_elaps(s_sys_heap_tick) < 2000u) {
        return;
    }

    memset(s_sys_heaps, 0, sizeof(s_sys_heaps));
    if (board_umem_region_mallinfo(0, &info)) {
        helm_sys_usage_from_mallinfo(&s_sys_heaps[0], &info);
    }

#if CONFIG_MM_REGIONS > 1
    if (board_umem_region_mallinfo(1, &info)) {
        helm_sys_usage_from_mallinfo(&s_sys_heaps[1], &info);
    }
#endif

    if (board_psram_heap_ready()) {
        info = board_psram_mallinfo();
        helm_sys_usage_from_mallinfo(&s_sys_heaps[2], &info);
    }

    s_sys_heap_tick = now;
    s_sys_heap_ready = true;
}

static bool s_sys_collect_stack;

static void helm_sys_collect_thread(FAR struct tcb_s * tcb, FAR void * arg)
{
    helm_sys_threads_t * out = (helm_sys_threads_t *)arg;
    helm_sys_thread_t * item;

    if (tcb == NULL || out == NULL) {
        return;
    }

    if (out->seen < UINT8_MAX) {
        out->seen++;
    }

    if (out->count >= HELM_SYS_THREAD_N) {
        return;
    }

    item = &out->thread[out->count++];
    memset(item, 0, sizeof(*item));
    item->pid = tcb->pid;
    item->stack_total = tcb->adj_stack_size;
#ifdef CONFIG_STACK_COLORATION
    if (s_sys_collect_stack) {
        item->stack_used = up_check_tcbstack(tcb, tcb->adj_stack_size);
    }
#endif
    lv_snprintf(item->name, sizeof(item->name), "%s", get_task_name(tcb));
}

static void helm_sys_count_one(FAR struct tcb_s * tcb, FAR void * arg)
{
    uint8_t * n = (uint8_t *)arg;

    if (tcb != NULL && n != NULL && *n < UINT8_MAX) {
        (*n)++;
    }
}

static void helm_sys_refresh_thread_count(bool force)
{
    uint32_t now = lv_tick_get();

    if (!force && s_sys_count_ready &&
        lv_tick_elaps(s_sys_count_tick) < 2000u) {
        return;
    }

    s_sys_thread_n = 0;
    nxsched_foreach(helm_sys_count_one, &s_sys_thread_n);
    s_sys_count_tick = now;
    s_sys_count_ready = true;
}

static void helm_sys_refresh_threads(bool force)
{
    pid_t old_pid[HELM_SYS_THREAD_N];
    size_t old_used[HELM_SYS_THREAD_N];
    uint32_t now = lv_tick_get();
    uint8_t old_n;
    uint8_t i;
    uint8_t j;
    bool want_stack;

    if (!force && s_sys_thread_ready &&
        lv_tick_elaps(s_sys_thread_tick) < 1200u) {
        return;
    }

    want_stack = force || !s_sys_stack_ready ||
                 lv_tick_elaps(s_sys_stack_tick) >= 4000u;
    old_n = s_sys_threads.count;
    for (i = 0; i < old_n; i++) {
        old_pid[i] = s_sys_threads.thread[i].pid;
        old_used[i] = s_sys_threads.thread[i].stack_used;
    }

    memset(&s_sys_threads, 0, sizeof(s_sys_threads));
    s_sys_collect_stack = want_stack;
    nxsched_foreach(helm_sys_collect_thread, &s_sys_threads);
    s_sys_collect_stack = false;

    for (i = 0; i < s_sys_threads.count; i++) {
        helm_sys_thread_t * item = &s_sys_threads.thread[i];

#ifdef CONFIG_STACK_COLORATION
        if (!want_stack) {
            uint8_t k;

            for (k = 0; k < old_n; k++) {
                if (old_pid[k] == item->pid) {
                    item->stack_used = old_used[k];
                    break;
                }
            }
        }
#else
        LV_UNUSED(old_n);
        LV_UNUSED(old_pid);
        LV_UNUSED(old_used);
        LV_UNUSED(want_stack);
#endif
#ifndef CONFIG_SCHED_CPULOAD_NONE
        {
            struct cpuload_s load;

            if (clock_cpuload(item->pid, &load) == 0 && load.total > 0) {
                uint64_t p = (uint64_t)load.active * 1000ull /
                             (uint64_t)load.total;

                item->cpu_permille = (uint16_t)(p > 1000ull ? 1000ull : p);
                item->cpu_valid = true;
            }
        }
#endif
    }

    /* PID 顺序不会随负载跳动，按键浏览时选中项保持稳定。 */
    for (i = 1; i < s_sys_threads.count; i++) {
        helm_sys_thread_t cur = s_sys_threads.thread[i];

        j = i;
        while (j > 0 && s_sys_threads.thread[j - 1u].pid > cur.pid) {
            s_sys_threads.thread[j] = s_sys_threads.thread[j - 1u];
            j--;
        }

        s_sys_threads.thread[j] = cur;
    }

    s_sys_thread_n = s_sys_threads.seen;
    s_sys_thread_tick = now;
    s_sys_thread_ready = true;
    s_sys_count_tick = now;
    s_sys_count_ready = true;
    if (want_stack) {
        s_sys_stack_tick = now;
        s_sys_stack_ready = true;
    }
}

static uint8_t helm_sys_cpu_used(void)
{
    uint32_t mhz = sf32lb_dvfs_hclk_mhz();
    uint8_t idle;

    if (mhz != 0) {
        idle = sf32lb_dvfs_idle_pct();
        if (idle > 100u) {
            idle = 100u;
        }

        return (uint8_t)(100u - idle);
    }

#ifndef CONFIG_SCHED_CPULOAD_NONE
    {
        struct cpuload_s load;

        if (clock_cpuload(0, &load) == 0 && load.total > 0) {
            uint64_t idle_pct = (uint64_t)load.active * 100ull /
                                (uint64_t)load.total;

            return (uint8_t)(idle_pct < 100ull ? 100ull - idle_pct : 0ull);
        }
    }
#endif

    return 0;
}

static uint32_t helm_sys_cpu_mhz(void)
{
    return sf32lb_dvfs_hclk_mhz();
}

static uint32_t helm_sys_items_sig(const helm_menu_t * m);

static void helm_row_set(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                         const char * lab, const char * sub, const char * val,
                         uint8_t act, uint8_t kind);
static void helm_row_sw(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                        const char * lab, bool on, uint8_t act);

static bool helm_addr_nz(const uint8_t *addr)
{
    static const uint8_t z[MYVENDOR_SYS_SENSOR_ADDR_LEN];

    return addr != NULL &&
           memcmp(addr, z, MYVENDOR_SYS_SENSOR_ADDR_LEN) != 0;
}

static bool helm_addr_eq(const uint8_t *a, const uint8_t *b)
{
    return a != NULL && b != NULL &&
           memcmp(a, b, MYVENDOR_SYS_SENSOR_ADDR_LEN) == 0;
}

static bool helm_fill_scan_recs(helm_menu_t * m, uint8_t * n)
{
    myvendor_sys_sensor_ui_t ui;
    myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
    size_t rec_n = 0;
    uint8_t i;
    char sub[24];
    bool radio;

    myvendor_sys_sensor_ui_get(&ui);
    radio = myvendor_devctl_sensor_get();
    (void)myvendor_devctl_sensor_recs_get(recs, MYVENDOR_DEVCTL_SENSOR_REC_MAX,
                                          &rec_n);
    lv_snprintf(sub, sizeof(sub), "%s · BLE", helm_scan_kind_lab(s_scan_kind));

    if (*n < HELM_MENU_ROWS) {
        helm_row_set(m, *n, HELM_ICO_BLE, "扫描设备", "查找附近设备",
                     radio ? "" : "外设未开",
                     ACT_SCAN_START, HELM_KIND_GO);
        (*n)++;
    }

    for (i = 0; i < rec_n && *n < HELM_MENU_ROWS; i++) {
        const char * nm;
        const char * val = "";
        char row_sub[24];
        helm_ico_id_t ico = helm_sensor_ico(recs[i].kind);
        bool linked = false;

        if (recs[i].kind != s_scan_kind) {
            continue;
        }

        nm = recs[i].name[0] != '\0' ? recs[i].name : helm_scan_anon(s_scan_kind);
        lv_snprintf(row_sub, sizeof(row_sub), "%s", sub);
        if (s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
            helm_addr_nz(ui.slot[s_scan_kind].addr) &&
            helm_addr_eq(ui.slot[s_scan_kind].addr, recs[i].addr) &&
            ui.slot[s_scan_kind].link != MYVENDOR_SYS_SENSOR_LINK_IDLE) {
            linked = true;
        }

        if (recs[i].autorc) {
            ico = HELM_ICO_STAR;
        }

        if (linked &&
            ui.slot[s_scan_kind].link == MYVENDOR_SYS_SENSOR_LINK_READY) {
            val = "已连接";
            if (recs[i].autorc) {
                lv_snprintf(row_sub, sizeof(row_sub), "回连");
            }
        } else if (linked) {
            val = "连接中";
        } else if (recs[i].autorc) {
            val = "回连";
        }

        helm_row_set(m, *n, ico, nm, row_sub, val, ACT_SCAN_AUTO, HELM_KIND_GO);
        m->items[*n].extra = (uint8_t)i;
        (*n)++;
    }

    if (radio && s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
        ui.slot[s_scan_kind].link != MYVENDOR_SYS_SENSOR_LINK_IDLE) {
        bool have = false;
        uint8_t r;

        if (helm_addr_nz(ui.slot[s_scan_kind].addr)) {
            for (r = 0; r < rec_n; r++) {
                if (recs[r].kind == s_scan_kind &&
                    helm_addr_eq(recs[r].addr, ui.slot[s_scan_kind].addr)) {
                    have = true;
                    break;
                }
            }
        }

        if (!have && *n < HELM_MENU_ROWS) {
            const char * nm = ui.slot[s_scan_kind].name[0] != '\0' ?
                              ui.slot[s_scan_kind].name :
                              helm_scan_anon(s_scan_kind);
            const char * val = (ui.slot[s_scan_kind].link ==
                                MYVENDOR_SYS_SENSOR_LINK_READY) ?
                               "已连接" : "连接中";

            helm_row_set(m, (*n)++, HELM_ICO_BLE, nm, sub, val, ACT_NOP,
                         HELM_KIND_GO);
        }
    }

    return *n == 0;
}

static bool helm_fill_scan_pick(helm_menu_t * m, uint8_t * n)
{
    myvendor_sys_sensor_ui_t ui;
    uint8_t i;
    uint8_t bit;
    char sub[24];
    bool radio;
    bool wait;

    helm_scan_tick();
    myvendor_sys_sensor_ui_get(&ui);
    radio = myvendor_devctl_sensor_get();
    bit = (uint8_t)(1u << s_scan_kind);
    lv_snprintf(sub, sizeof(sub), "%s · BLE", helm_scan_kind_lab(s_scan_kind));
    wait = s_scan_link_wait ||
           (s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
            ui.slot[s_scan_kind].link == MYVENDOR_SYS_SENSOR_LINK_CONNECTING);

    if (wait && s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
        *n < HELM_MENU_ROWS) {
        const char * nm = ui.slot[s_scan_kind].name[0] != '\0' ?
                          ui.slot[s_scan_kind].name :
                          helm_scan_anon(s_scan_kind);
        const char * val = (ui.slot[s_scan_kind].link ==
                            MYVENDOR_SYS_SENSOR_LINK_READY) ?
                           "已连接" : "连接中";

        helm_row_set(m, (*n)++, helm_sensor_ico(s_scan_kind), nm, sub, val,
                     ACT_NOP, HELM_KIND_GO);
        return *n == 0;
    }

    if (radio) {
        for (i = 0; i < ui.found_n && *n < HELM_MENU_ROWS; i++) {
            const char * nm;

            if ((ui.found[i].kind_mask & bit) == 0 || ui.found[i].linked) {
                continue;
            }

            nm = ui.found[i].name[0] != '\0' ? ui.found[i].name :
                 helm_scan_anon(s_scan_kind);
            helm_row_set(m, *n, helm_sensor_ico(s_scan_kind), nm, sub, "",
                         ACT_SCAN_CONNECT, HELM_KIND_GO);
            m->items[*n].extra = ui.found[i].table_idx;
            (*n)++;
        }
    }

    return *n == 0;
}

static const char * helm_bl_name(uint8_t pwm)
{
    static char s[8];

    LV_UNUSED(pwm);
    lv_snprintf(s, sizeof(s), "%u%%", (unsigned)helm_pwr_bl_ui());
    return s;
}

static void helm_tz_fmt(char * buf, size_t n)
{
    int16_t m = myvendor_devctl_tz_min_get();
    unsigned mag;
    unsigned hh;
    unsigned mm;
    char sign;

    if (m < 0) {
        sign = '-';
        mag = (unsigned)(-m);
    } else {
        sign = '+';
        mag = (unsigned)m;
    }

    hh = mag / 60u;
    mm = mag % 60u;
    if (mm == 0) {
        lv_snprintf(buf, n, "UTC%c%u", sign, hh);
    } else {
        lv_snprintf(buf, n, "UTC%c%u:%02u", sign, hh, mm);
    }
}

#define HELM_EPH_MIN_UNIX  1704067200u

static void helm_eph_local(char * buf, size_t n, uint32_t utc, bool with_date)
{
    struct tm tm_buf;
    time_t wall;
    int16_t tz;

    if (buf == NULL || n == 0) {
        return;
    }

    if (utc < HELM_EPH_MIN_UNIX) {
        lv_snprintf(buf, n, "--");
        return;
    }

    tz = myvendor_devctl_tz_min_get();
    wall = (time_t)utc + (time_t)tz * 60;
    if (gmtime_r(&wall, &tm_buf) == NULL) {
        lv_snprintf(buf, n, "--");
        return;
    }

    if (with_date) {
        lv_snprintf(buf, n, "%04d-%02d-%02d %02d:%02d",
                    tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                    tm_buf.tm_hour, tm_buf.tm_min);
    } else {
        lv_snprintf(buf, n, "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
    }
}

static void helm_eph_fill_page(helm_menu_t * m, uint8_t * n)
{
    uint32_t last = 0;
    uint32_t next = 0;
    char last_s[24];
    char next_s[24];
    time_t now;
    bool have;

    /* 顺序照设计稿 08 页：星历（日期）一组、操作一组。 */
    helm_grp_begin(m, *n, "星历");
    have = helm_eph_times_cached(&last, &next);
    now = time(NULL);
    if (!have) {
        helm_row_set(m, (*n)++, HELM_ICO_TIME, "上次同步",
                     "未同步", "--", ACT_NOP, HELM_KIND_GO);
        helm_row_set(m, (*n)++, HELM_ICO_TIME, "下次同步",
                     "请用 App 同步", "现在", ACT_NOP, HELM_KIND_GO);
    } else {
        helm_eph_local(last_s, sizeof(last_s), last, true);
        helm_eph_local(next_s, sizeof(next_s), next, true);
        helm_row_set(m, (*n)++, HELM_ICO_TIME, "上次同步", last_s, "",
                     ACT_NOP, HELM_KIND_GO);
        if (next != 0 && now >= (time_t)next) {
            helm_row_set(m, (*n)++, HELM_ICO_TIME, "下次同步", next_s, "已过期",
                         ACT_NOP, HELM_KIND_GO);
        } else {
            helm_row_set(m, (*n)++, HELM_ICO_TIME, "下次同步", next_s, "",
                         ACT_NOP, HELM_KIND_GO);
        }
    }

    helm_grp_begin(m, *n, "操作");
    helm_row_set(m, (*n)++, HELM_ICO_GPS, "写入星历",
                 myvendor_gnss_eph_busy() ? "写入中" : "写进 GPS 模块", "",
                 ACT_EPH_WRITE, HELM_KIND_GO);
    helm_row_set(m, (*n)++, HELM_ICO_GPS, "自动同步",
                 myvendor_devctl_eph_auto_get() ? "已开启" : "已关闭", "",
                 ACT_TOGGLE_EPH_AUTO, HELM_KIND_SW);
    m->items[*n - 1u].on = myvendor_devctl_eph_auto_get();
}

static const char * helm_notif_kind(uint8_t type)
{
    switch (type) {
    case MYVENDOR_SYS_NOTIF_TYPE_CALL:
        return "来电";
    case MYVENDOR_SYS_NOTIF_TYPE_SMS:
        return "短信";
    case MYVENDOR_SYS_NOTIF_TYPE_CALENDAR:
        return "日历";
    default:
        return "应用";
    }
}

static bool helm_fill_inbox(helm_menu_t * m, uint8_t * n)
{
    myvendor_sys_notif_t box[MYVENDOR_SYS_INBOX_MAX];
    uint8_t nn = 0;
    uint8_t i;

    myvendor_sys_inbox_get(box, &nn, MYVENDOR_SYS_INBOX_MAX);
    for (i = 0; i < nn && *n < HELM_MENU_ROWS; i++) {
        const char * lab = box[i].title[0] ? box[i].title : box[i].body;

        helm_row_set(m, *n,
                     (box[i].type == MYVENDOR_SYS_NOTIF_TYPE_CALL) ?
                         HELM_ICO_CALL : HELM_ICO_BELL,
                     (lab && lab[0]) ? lab : "通知",
                     helm_notif_kind(box[i].type), "",
                     ACT_INBOX_OPEN, HELM_KIND_GO);
        m->items[*n].extra = i;
        (*n)++;
    }

    return *n == 0;
}

static void helm_row_set(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                         const char * lab, const char * sub, const char * val,
                         uint8_t act, uint8_t kind)
{
    if (i >= HELM_MENU_ROWS) {
        return;
    }

    /* 底色覆盖默认关 —— items 数组跨页复用，残留会漏到别的页。 */
    m->items[i].bg_set = false;
    m->items[i].bg_dark = false;
    m->items[i].grp = 0;

    lv_snprintf(m->items[i].label, sizeof(m->items[i].label), "%s", lab);
    m->items[i].sub[0] = '\0';
    m->items[i].value[0] = '\0';
    if (sub && sub[0] != '\0') {
        lv_snprintf(m->items[i].sub, sizeof(m->items[i].sub), "%s", sub);
    }

    if (val && val[0] != '\0') {
        lv_snprintf(m->items[i].value, sizeof(m->items[i].value), "%s", val);
    }

    m->items[i].act = act;
    m->items[i].kind = kind;
    m->items[i].ico = ico;
    m->items[i].on = false;
}

static void helm_row_sw(helm_menu_t * m, uint8_t i, helm_ico_id_t ico,
                        const char * lab, bool on, uint8_t act)
{
    helm_row_set(m, i, ico, lab, on ? "已开启" : "已关闭", "", act, HELM_KIND_SW);
    m->items[i].on = on;
}

static helm_scr_t helm_cur_scr(const helm_menu_t * m)
{
    if (m == NULL || m->sp == 0) {
        return HELM_SCR_ROOT;
    }

    return m->stack[m->sp - 1u].scr;
}

static uint8_t * helm_cur_sel(helm_menu_t * m)
{
    if (m == NULL || m->sp == 0) {
        return NULL;
    }

    return &m->stack[m->sp - 1u].sel;
}

static bool helm_name_is_gpx(const char * name)
{
    size_t n;

    if (name == NULL) {
        return false;
    }

    n = strlen(name);
    if (n < 5u) {
        return false;
    }

    return strcasecmp(name + n - 4u, ".gpx") == 0;
}

static bool helm_name_is_tsv(const char * name)
{
    size_t n;

    if (name == NULL) {
        return false;
    }

    n = strlen(name);
    if (n < 5u) {
        return false;
    }

    return strcasecmp(name + n - 4u, ".tsv") == 0;
}

static void helm_navpt_clear_bind(void)
{
    s_navpt_path[0] = '\0';
    s_navpt_title[0] = '\0';
}

static uint8_t helm_navpt_scan(void)
{
    DIR * d;
    struct dirent * de;

    if (myvendor_mtp_lfs_quiesce()) {
        return gpx_same_dir(s_gpx_dir, MYVENDOR_NAVPTS_DIR) ? s_gpx_n : 0;
    }

    s_gpx_n = 0;
    s_gpx_dir = MYVENDOR_NAVPTS_DIR;
    d = opendir(MYVENDOR_NAVPTS_DIR);
    if (d == NULL) {
        return 0;
    }

    while ((de = readdir(d)) != NULL && s_gpx_n < HELM_GPX_LIST_MAX) {
        char path[160];
        struct stat st;

        if (de->d_name[0] == '.' || !helm_name_is_tsv(de->d_name)) {
            continue;
        }

        lv_snprintf(path, sizeof(path), "%s/%s", MYVENDOR_NAVPTS_DIR,
                    de->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size < 8) {
            continue;
        }

        lv_snprintf(s_gpx_names[s_gpx_n], HELM_GPX_NAME_MAX, "%s",
                    de->d_name);
        s_gpx_size[s_gpx_n] = (uint32_t)st.st_size;
        s_gpx_mtime[s_gpx_n] = (uint32_t)st.st_mtime;
        s_gpx_n++;
    }

    closedir(d);

    {
        uint8_t i;
        uint8_t j;
        char tmp[HELM_GPX_NAME_MAX];
        uint32_t u;

        for (i = 0; i < s_gpx_n; i++) {
            for (j = (uint8_t)(i + 1u); j < s_gpx_n; j++) {
                if (strcmp(s_gpx_names[j], s_gpx_names[i]) > 0) {
                    lv_snprintf(tmp, sizeof(tmp), "%s", s_gpx_names[i]);
                    lv_snprintf(s_gpx_names[i], HELM_GPX_NAME_MAX, "%s",
                                s_gpx_names[j]);
                    lv_snprintf(s_gpx_names[j], HELM_GPX_NAME_MAX, "%s", tmp);
                    u = s_gpx_size[i];
                    s_gpx_size[i] = s_gpx_size[j];
                    s_gpx_size[j] = u;
                    u = s_gpx_mtime[i];
                    s_gpx_mtime[i] = s_gpx_mtime[j];
                    s_gpx_mtime[j] = u;
                }
            }
        }
    }

    return s_gpx_n;
}

static bool helm_fill_waypoint_rows(helm_menu_t * m, uint8_t * n,
                                    uint8_t ico,
                                    const myvendor_devctl_favorite_t * pts,
                                    size_t pt_n, bool pick)
{
    size_t i;

    for (i = 0; i < pt_n && *n < HELM_MENU_ROWS; i++) {
        char sub[40];
        char fallback[24];
        const char * lab;
        uint8_t order = 0u;
        uint8_t j;

        lv_snprintf(fallback, sizeof(fallback), "第%u站",
                    (unsigned)(i + 1u));
        lab = (pts[i].name[0] != '\0') ? pts[i].name : fallback;
        lv_snprintf(sub, sizeof(sub), "%.6f, %.6f",
                    pts[i].latitude, pts[i].longitude);

        if (!pick) {
            helm_row_set(m, (*n)++, ico, lab, sub, fallback,
                         ACT_NOP, HELM_KIND_GO);
            m->items[*n - 1u].extra = (uint8_t)i;
            continue;
        }

        for (j = 0u; j < m->fav_order_n; j++) {
            if (m->fav_order[j] == (uint8_t)i) {
                order = (uint8_t)(j + 1u);
                break;
            }
        }

        helm_row_set(m, (*n)++, ico, lab, sub,
                     order > 0u ? "" : "选择", ACT_NAV_FAV, HELM_KIND_GO);
        m->items[*n - 1u].extra = (uint8_t)i;
        if (order > 0u) {
            lv_snprintf(m->items[*n - 1u].value,
                        sizeof(m->items[*n - 1u].value), "第%u站",
                        (unsigned)order);
        }
    }

    return *n == 0u;
}

/** @brief 目录扫描的 2 秒缓存 —— `helm_menu_paint` 每帧都会问记录数。
 *  @details 现场实测（[vperf] ui slow menu）稳态一次 paint 116 ms、rebuild=0，
 *           把每次都扫目录这一项去掉是最大的单笔。记录数只会被"录一段/写入"改变，
 *           2 秒过期足够，改动不会看不到。 */
static uint8_t helm_gpx_scan(const char * dir);

static int s_rc_scr = -1;
static bool s_rc_dirty = true;
static char s_rc_dir[64];
static uint8_t s_rc_n;

/** @brief 让记录数缓存失效：下次问的时候真扫一次目录。
 *  @details 用户 2026-09-26 定："这个就开机、骑行结束、进入时更新一次"，
 *           且"进入"指的是**进入骑行记录页**（不是进菜单）。
 *           开机 = 初值脏；进入 = helm_fill_items 里进 RIDES 屏时置脏；
 *           骑行结束 = 保存路径上显式调这个函数。 */
void helm_gpx_count_invalidate(void)
{
    s_rc_dirty = true;
}

/** @brief 记录数：**只在脏的时候扫目录**，其余一律读缓存。
 *  @details 原来是 2 秒 TTL —— 而菜单开着时后台刷新约 0.5 Hz（探针里 paints 自己
 *           在涨），TTL 几乎每次都过期 ⇒ 每 2 秒一次完整 readdir（实测 ~300 ms，
 *           就是主菜单 fill=332/347ms 的来头）。改成事件驱动之后，菜单静置时一次都不扫。 */
static uint8_t helm_gpx_scan_cached(const char * dir)
{
    if (!s_rc_dirty && dir != NULL && strcmp(s_rc_dir, dir) == 0) {
        return s_rc_n;
    }

    s_rc_n = 0;
    if (dir != NULL) {
        s_rc_n = helm_gpx_scan(dir);
        lv_snprintf(s_rc_dir, sizeof(s_rc_dir), "%s", dir);
    }

    s_rc_dirty = false;
    return s_rc_n;
}

static uint8_t helm_gpx_scan(const char * dir)
{
    DIR * d;
    struct dirent * de;

    if (myvendor_mtp_lfs_quiesce()) {
        return gpx_same_dir(s_gpx_dir, dir) ? s_gpx_n : 0;
    }

    s_gpx_n = 0;
    s_gpx_dir = dir;
    if (dir == NULL) {
        return 0;
    }

    d = opendir(dir);
    if (d == NULL) {
        return 0;
    }

    while ((de = readdir(d)) != NULL && s_gpx_n < HELM_GPX_LIST_MAX) {
        if (de->d_name[0] == '.') {
            continue;
        }

        if (!helm_name_is_gpx(de->d_name)) {
            continue;
        }

        {
            char path[160];
            struct stat st;

            lv_snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            /* 空壳约 381 字节、0 字节文件不进列表。 */
            if (stat(path, &st) != 0 || st.st_size < 400) {
                continue;
            }

            lv_snprintf(s_gpx_names[s_gpx_n], HELM_GPX_NAME_MAX, "%s",
                        de->d_name);
            s_gpx_size[s_gpx_n] = (uint32_t)st.st_size;
            s_gpx_mtime[s_gpx_n] = (uint32_t)st.st_mtime;
            s_gpx_n++;
        }
    }

    closedir(d);

    /* TRK_YYYYMMDD_HHMMSS：名字倒序即最新在前。 */
    {
        uint8_t i;
        uint8_t j;
        char tmp[HELM_GPX_NAME_MAX];
        uint32_t u;

        for (i = 0; i < s_gpx_n; i++) {
            for (j = (uint8_t)(i + 1u); j < s_gpx_n; j++) {
                if (strcmp(s_gpx_names[j], s_gpx_names[i]) > 0) {
                    lv_snprintf(tmp, sizeof(tmp), "%s", s_gpx_names[i]);
                    lv_snprintf(s_gpx_names[i], HELM_GPX_NAME_MAX, "%s",
                                s_gpx_names[j]);
                    lv_snprintf(s_gpx_names[j], HELM_GPX_NAME_MAX, "%s", tmp);
                    u = s_gpx_size[i];
                    s_gpx_size[i] = s_gpx_size[j];
                    s_gpx_size[j] = u;
                    u = s_gpx_mtime[i];
                    s_gpx_mtime[i] = s_gpx_mtime[j];
                    s_gpx_mtime[j] = u;
                }
            }
        }
    }

    return s_gpx_n;
}

static int helm_gpx_leap(int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
}

static int64_t helm_gpx_epoch(const gpx_time_t * t)
{
    static const uint8_t dim[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int y;
    int m;
    int i;
    int64_t days = 0;

    if (t == NULL || t->year < 1970 || t->month < 1 || t->month > 12
        || t->day < 1) {
        return -1;
    }

    y = t->year;
    m = t->month;
    for (i = 1970; i < y; i++) {
        days += 365 + helm_gpx_leap(i);
    }

    for (i = 1; i < m; i++) {
        days += dim[i - 1];
        if (i == 2) {
            days += helm_gpx_leap(y);
        }
    }

    days += (int)t->day - 1;
    return days * 86400 + (int64_t)t->hour * 3600
        + (int64_t)t->minute * 60 + (int64_t)t->second;
}

static void helm_fmt_ride_dur(char * buf, size_t n, uint32_t sec)
{
    unsigned h = sec / 3600u;
    unsigned m = (sec / 60u) % 60u;
    unsigned s = sec % 60u;

    if (buf == NULL || n == 0u) {
        return;
    }

    /* 始终 H:MM:SS，避免 MM:SS 被看成钟点。 */
    lv_snprintf(buf, n, "%u:%02u:%02u", h, m, s);
}

static void helm_fmt_ride_sub(char * buf, size_t n, double km, uint32_t sec,
                             bool have_time)
{
    char dist[16];
    char dur[16];

    if (buf == NULL || n == 0u) {
        return;
    }

    if (s_unit_imperial) {
        const double mi = km * 0.621371;

        lv_snprintf(dist, sizeof(dist),
            (mi >= 99.95) ? "%.0f mi" : "%.1f mi", mi);
    } else {
        /* ≥100km 不要小数（用户 2026-09-27，与 helm_fmt_km 同规则）。 */
        lv_snprintf(dist, sizeof(dist),
            (km >= 99.95) ? "%.0f km" : "%.1f km", km);
    }

    if (!have_time) {
        lv_snprintf(buf, n, "%s", dist);
        return;
    }

    helm_fmt_ride_dur(dur, sizeof(dur), sec);
    lv_snprintf(buf, n, "%s  用时%s", dist, dur);
}

static helm_gpx_stat_t * helm_gpx_stat_find(const char * name)
{
    uint8_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; i < s_gpx_stat_n; i++) {
        if (strcmp(s_gpx_stat[i].name, name) == 0) {
            return &s_gpx_stat[i];
        }
    }

    return NULL;
}

static bool helm_gpx_stat_flush(void)
{
    helm_stat_hdr_t hdr;
    int fd;
    uint8_t i;
    uint16_t n = 0;

    for (i = 0; i < s_gpx_stat_n; i++) {
        if (s_gpx_stat[i].ok) {
            n++;
        }
    }

    fd = open(HELM_STAT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = HELM_STAT_MAGIC;
    hdr.ver = HELM_STAT_VER;
    hdr.n = n;
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        close(fd);
        (void)unlink(HELM_STAT_TMP);
        return false;
    }

    for (i = 0; i < s_gpx_stat_n; i++) {
        helm_stat_rec_t rec;

        if (!s_gpx_stat[i].ok) {
            continue;
        }

        memset(&rec, 0, sizeof(rec));
        lv_snprintf(rec.name, sizeof(rec.name), "%s", s_gpx_stat[i].name);
        rec.size = s_gpx_stat[i].size;
        rec.mtime = s_gpx_stat[i].mtime;
        rec.dist_m = (uint32_t)(s_gpx_stat[i].km * 1000.0 + 0.5);
        rec.sec = s_gpx_stat[i].sec;
        rec.have_time = s_gpx_stat[i].have_time ? 1u : 0u;
        if (write(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
            close(fd);
            (void)unlink(HELM_STAT_TMP);
            return false;
        }
    }

    close(fd);
    (void)unlink(HELM_STAT_PATH);
    if (rename(HELM_STAT_TMP, HELM_STAT_PATH) != 0) {
        (void)unlink(HELM_STAT_TMP);
        return false;
    }

    return true;
}

static void helm_gpx_stat_load(void)
{
    helm_stat_hdr_t hdr;
    int fd;
    uint16_t i;

    if (s_stat_loaded) {
        return;
    }

    s_stat_loaded = true;
    s_gpx_stat_n = 0;
    fd = open(HELM_STAT_PATH, O_RDONLY);
    if (fd < 0) {
        return;
    }

    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)
        || hdr.magic != HELM_STAT_MAGIC || hdr.ver != HELM_STAT_VER
        || hdr.n > HELM_GPX_LIST_MAX) {
        close(fd);
        return;
    }

    for (i = 0; i < hdr.n && s_gpx_stat_n < HELM_GPX_LIST_MAX; i++) {
        helm_stat_rec_t rec;

        if (read(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
            break;
        }

        rec.name[HELM_GPX_NAME_MAX - 1u] = '\0';
        if (rec.name[0] == '\0') {
            continue;
        }

        lv_snprintf(s_gpx_stat[s_gpx_stat_n].name,
                    sizeof(s_gpx_stat[s_gpx_stat_n].name), "%s", rec.name);
        s_gpx_stat[s_gpx_stat_n].km = (double)rec.dist_m / 1000.0;
        s_gpx_stat[s_gpx_stat_n].sec = rec.sec;
        s_gpx_stat[s_gpx_stat_n].size = rec.size;
        s_gpx_stat[s_gpx_stat_n].mtime = rec.mtime;
        s_gpx_stat[s_gpx_stat_n].have_time = (rec.have_time != 0u);
        s_gpx_stat[s_gpx_stat_n].ok = true;
        s_gpx_stat[s_gpx_stat_n].dir = MYVENDOR_GPX_RECORD_DIR;
        s_gpx_stat_n++;
    }

    close(fd);
}

static void helm_gpx_stat_put(const char * name, double km, uint32_t sec,
                             bool have_time, uint32_t size, uint32_t mtime,
                             const char * dir)
{
    helm_gpx_stat_t * s;

    if (name == NULL || name[0] == '\0') {
        return;
    }

    helm_gpx_stat_load();
    s = helm_gpx_stat_find(name);
    if (s == NULL) {
        if (s_gpx_stat_n >= HELM_GPX_LIST_MAX) {
            return;
        }

        s = &s_gpx_stat[s_gpx_stat_n++];
        memset(s, 0, sizeof(*s));
        lv_snprintf(s->name, sizeof(s->name), "%s", name);
    }

    s->km = km;
    s->sec = sec;
    s->have_time = have_time;
    s->size = size;
    s->mtime = mtime;
    s->dir = (dir != NULL) ? dir : MYVENDOR_GPX_RECORD_DIR;
    s->ok = true;
    (void)helm_gpx_stat_flush();
}

static void helm_gpx_stat_forget(const char * name)
{
    helm_gpx_stat_t * s;

    helm_gpx_stat_load();
    s = helm_gpx_stat_find(name);
    if (s == NULL) {
        return;
    }

    {
        uint8_t i = (uint8_t)(s - s_gpx_stat);

        if (i + 1u < s_gpx_stat_n) {
            s_gpx_stat[i] = s_gpx_stat[s_gpx_stat_n - 1u];
        }

        s_gpx_stat_n--;
    }

    (void)helm_gpx_stat_flush();
}

static bool helm_gpx_name_live(const char * name)
{
    uint8_t i;

    if (name == NULL) {
        return false;
    }

    for (i = 0; i < s_gpx_n; i++) {
        if (strcmp(s_gpx_names[i], name) == 0) {
            return true;
        }
    }

    return false;
}

static void helm_gpx_stat_prune(void)
{
    uint8_t i = 0;
    bool dirty = false;

    helm_gpx_stat_load();
    while (i < s_gpx_stat_n) {
        /* 只按当前目录的内存列表淘汰，避免扫盘 stat 和 BLE 落盘抢 LFS。 */
        if (gpx_same_dir(s_gpx_stat[i].dir, s_gpx_dir) &&
            !helm_gpx_name_live(s_gpx_stat[i].name)) {
            if (i + 1u < s_gpx_stat_n) {
                s_gpx_stat[i] = s_gpx_stat[s_gpx_stat_n - 1u];
            }

            s_gpx_stat_n--;
            dirty = true;
            continue;
        }

        i++;
    }

    if (dirty) {
        (void)helm_gpx_stat_flush();
    }
}

static bool helm_gpx_stat_hit(const helm_gpx_stat_t * s, uint32_t size,
                              uint32_t mtime)
{
    if (s == NULL || !s->ok) {
        return false;
    }

    if (size != 0u && s->size != 0u && s->size != size) {
        return false;
    }

    if (mtime != 0u && s->mtime != 0u && s->mtime != mtime) {
        return false;
    }

    return true;
}

void helm_ride_stat_remember(const char * gpx_path, double km, uint32_t sec)
{
    const char * base;

    if (gpx_path == NULL || gpx_path[0] == '\0') {
        return;
    }

    base = strrchr(gpx_path, '/');
    base = (base != NULL && base[1] != '\0') ? base + 1 : gpx_path;
    lv_snprintf(s_stat_pend_name, sizeof(s_stat_pend_name), "%s", base);
    s_stat_pend_km = km;
    s_stat_pend_sec = sec;
    s_stat_pend = true;

    /* 按键路径不碰 LittleFS。已加载的 RAM 表立刻更新，落盘推迟到打开记录列表。 */
    if (s_stat_loaded) {
        helm_gpx_stat_t * s = helm_gpx_stat_find(s_stat_pend_name);

        if (s == NULL && s_gpx_stat_n < HELM_GPX_LIST_MAX) {
            s = &s_gpx_stat[s_gpx_stat_n++];
            memset(s, 0, sizeof(*s));
            lv_snprintf(s->name, sizeof(s->name), "%s", s_stat_pend_name);
        }

        if (s != NULL) {
            s->km = km;
            s->sec = sec;
            s->have_time = true;
            s->dir = MYVENDOR_GPX_RECORD_DIR;
            s->ok = true;
        }
    }
}

static void helm_gpx_stat_commit_pend(void)
{
    if (!s_stat_pend || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    helm_gpx_stat_put(s_stat_pend_name, s_stat_pend_km, s_stat_pend_sec, true,
                      0, 0, MYVENDOR_GPX_RECORD_DIR);
    s_stat_pend = false;
}

static bool helm_fill_gpx_dir(helm_menu_t * m, uint8_t * n, const char * dir,
                              uint8_t act)
{
    uint8_t i;

    /* BLE/MTP 写 /mnt/lfs 时不要在按键路径上 opendir/stat。
     * 否则第一次 lfs_alloc 能把 KEY2 confirm 卡住数秒，列表也打不开。 */
    if (myvendor_mtp_lfs_quiesce()) {
        helm_row_set(m, (*n)++, HELM_ICO_GPX, "写入中",
                     "稍后再打开", "", ACT_NOP, HELM_KIND_GO);
        return false;
    }

    helm_gpx_stat_commit_pend();

    /* ⚠ 列表内容必须用**真扫**（helm_gpx_scan），不能用带缓存的计数：
     * 缓存只存了条数，而名字数组 `s_gpx_names[]`/`s_gpx_n` 是扫描器共用的 ——
     * 一旦中间扫过别的目录（比如坐标点/常用点），缓存命中就会变成"计数>0 但
     * 名字数组是别的目录（或空）"，列表于是显示为空。用户 2026-09-26 报的
     * 「GPX 记录进去什么都没有」就是这个。计数缓存只给显示条数的地方用
     * （主菜单「骑行记录」行、GPX 页「记录」行）。 */
    if (helm_gpx_scan(dir) == 0) {
        helm_gpx_stat_prune();
        return true;
    }

    helm_gpx_stat_prune();
    for (i = 0; i < s_gpx_n && *n < HELM_MENU_ROWS; i++) {
        char sub[40];
        helm_gpx_stat_t * st = helm_gpx_stat_find(s_gpx_names[i]);

        if (helm_gpx_stat_hit(st, s_gpx_size[i], s_gpx_mtime[i])) {
            helm_fmt_ride_sub(sub, sizeof(sub), st->km, st->sec,
                              st->have_time);
        } else {
            lv_snprintf(sub, sizeof(sub), "--");
        }

        helm_row_set(m, (*n)++, HELM_ICO_GPX, s_gpx_names[i], sub, "",
                     act, HELM_KIND_GO);
        m->items[*n - 1u].extra = i;
    }

    return false;
}

static const char * helm_ride_dir_path(void)
{
    return (s_ride_dir != NULL) ? s_ride_dir : MYVENDOR_GPX_RECORD_DIR;
}

static bool helm_ride_bind(const char * name, const char * dir)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    lv_snprintf(s_ride_name, sizeof(s_ride_name), "%s", name);
    s_ride_dir = (dir != NULL) ? dir : MYVENDOR_GPX_RECORD_DIR;
    return true;
}

static bool helm_ride_path(char * buf, size_t n)
{
    if (buf == NULL || n == 0u || s_ride_name[0] == '\0') {
        return false;
    }

    lv_snprintf(buf, n, "%s/%s", helm_ride_dir_path(), s_ride_name);
    return true;
}

static bool helm_ride_load(void)
{
    char path[160];
    gpx_decode_t * dec = NULL;
    gpx_decode_cfg_t cfg;
    gpx_decode_stats_t st;
    gpx_point_t batch[16];
    unsigned n;
    int ret;
    float last_lon = 0.0f;
    float last_lat = 0.0f;
    bool have_last = false;
    bool have_t0 = false;
    gpx_time_t t0;
    gpx_time_t t1;
    double len = 0.0;

    memset(&t0, 0, sizeof(t0));
    memset(&t1, 0, sizeof(t1));
    memset(&st, 0, sizeof(st));
    memset(batch, 0, sizeof(batch));

    if (s_ride_name[0] == '\0') {
        s_ride_pt_n = 0;
        s_ride_km = 0.0;
        s_ride_sec = 0;
        s_ride_has_time = false;
        s_ride_loaded[0] = '\0';
        s_ride_loaded_dir = NULL;
        return false;
    }

    if (strcmp(s_ride_loaded, s_ride_name) == 0 &&
        s_ride_loaded_dir == helm_ride_dir_path() && s_ride_pt_n >= 2u) {
        return true;
    }

    s_ride_pt_n = 0;
    s_ride_km = 0.0;
    s_ride_sec = 0;
    s_ride_has_time = false;
    s_ride_loaded[0] = '\0';
    s_ride_loaded_dir = NULL;
    if (!helm_ride_path(path, sizeof(path))) {
        return false;
    }

    gpx_decode_cfg_sparse(&cfg, HELM_RIDE_PT_MAX);
    cfg.batch_max = 16;
    if (gpx_decode_open(path, &cfg, &dec) != 0) {
        return false;
    }

    for (;;) {
        unsigned i;

        memset(batch, 0, sizeof(batch));
        ret = gpx_decode_read(dec, batch, 16, &n);
        if (ret < 0) {
            /* 扩展/尾部损坏时仍用已读到的点画预览。 */
            break;
        }

        for (i = 0; i < n; i++) {
            if (batch[i].latitude == 0.0f && batch[i].longitude == 0.0f) {
                continue;
            }
            if (have_last) {
                const double d = vmap_geo_haversine_m((double)last_lon,
                    (double)last_lat, (double)batch[i].longitude,
                    (double)batch[i].latitude);

                if (d > 100000.0) {
                    continue;
                }
                len += d;
                if (d < 0.5) {
                    last_lon = batch[i].longitude;
                    last_lat = batch[i].latitude;
                    if (batch[i].has_time) {
                        t1 = batch[i].time;
                    }
                    continue;
                }
            }
            last_lon = batch[i].longitude;
            last_lat = batch[i].latitude;
            have_last = true;
            if (batch[i].has_time) {
                if (!have_t0) {
                    t0 = batch[i].time;
                    have_t0 = true;
                }

                t1 = batch[i].time;
            }

            if (s_ride_pt_n < HELM_RIDE_PT_MAX) {
                s_ride_lon[s_ride_pt_n] = last_lon;
                s_ride_lat[s_ride_pt_n] = last_lat;
                s_ride_pt_n++;
            } else {
                s_ride_lon[HELM_RIDE_PT_MAX - 1u] = last_lon;
                s_ride_lat[HELM_RIDE_PT_MAX - 1u] = last_lat;
            }
        }

        if (ret == 1) {
            break;
        }
    }

    (void)gpx_decode_get_stats(dec, &st);
    gpx_decode_close(&dec);

    if (have_last && s_ride_pt_n < HELM_RIDE_PT_MAX) {
        if (s_ride_pt_n == 0u
            || s_ride_lon[s_ride_pt_n - 1u] != last_lon
            || s_ride_lat[s_ride_pt_n - 1u] != last_lat) {
            s_ride_lon[s_ride_pt_n] = last_lon;
            s_ride_lat[s_ride_pt_n] = last_lat;
            s_ride_pt_n++;
        }
    }

    if (s_ride_pt_n < 2u) {
        return false;
    }

    if (st.length_m > len) {
        len = st.length_m;
    }

    s_ride_km = len / 1000.0;
    {
        helm_gpx_stat_t * cache;
        uint32_t gpx_sec = 0;
        bool gpx_has = false;

        if (have_t0) {
            const int64_t a = helm_gpx_epoch(&t0);
            const int64_t b = helm_gpx_epoch(&t1);

            if (a >= 0 && b >= a) {
                gpx_sec = (uint32_t)(b - a);
                gpx_has = true;
            }
        }

        /* 保存时写入的 session 用时优先于 GPX 首末时间差（墙钟跨度）。 */
        helm_gpx_stat_load();
        cache = helm_gpx_stat_find(s_ride_name);
        if (cache != NULL && cache->ok && cache->have_time) {
            s_ride_sec = cache->sec;
            s_ride_has_time = true;
        } else {
            s_ride_sec = gpx_sec;
            s_ride_has_time = gpx_has;
        }
    }

    lv_snprintf(s_ride_loaded, sizeof(s_ride_loaded), "%s", s_ride_name);
    s_ride_loaded_dir = helm_ride_dir_path();
    {
        struct stat fst;

        if (stat(path, &fst) == 0) {
            helm_gpx_stat_put(s_ride_name, s_ride_km, s_ride_sec,
                              s_ride_has_time, (uint32_t)fst.st_size,
                              (uint32_t)fst.st_mtime, helm_ride_dir_path());
        }
    }

    return true;
}

static lv_obj_t * s_colorlab_list;

/** @brief 配色试验页：把候选色**整行铺满**（用户要求看"选中时整行是什么样"）。
 *
 *  @details 为什么不用行自己的底色：菜单行的 bg_opa 恒为 TRANSP
 *           （helm_mitem_apply_mix：选中块画在列表的 DRAW_MAIN_END、比子对象早，
 *            行要是实底就把选中块盖住了）。所以在列表的 DRAW_MAIN 里自己铺：
 *            DRAW_MAIN 早于子对象 ⇒ 标题/副标题仍压在底色上；
 *            也早于 MAIN_END ⇒ 选中块照样压在整行底色上 —— 就是真实观感。
 */
static void helm_colorlab_draw(lv_event_t * e)
{
    helm_menu_t * m = s_menu;
    lv_obj_t * list = lv_event_get_target(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    uint32_t i;

    if (m == NULL || list == NULL || layer == NULL) {
        return;
    }

    for (i = 0; i < m->count && i < HELM_MENU_ROWS; i++) {
        lv_draw_rect_dsc_t rd;
        lv_obj_t * row;
        lv_area_t a;

        if (!m->items[i].bg_set) {
            continue;
        }

        row = lv_obj_get_child(list, i);
        if (row == NULL || lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }

        lv_obj_get_coords(row, &a);
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = helm_color(m->items[i].bg_color);
        rd.bg_opa = LV_OPA_COVER;
        rd.radius = HELM_RADIUS;
        lv_draw_rect(layer, &rd, &a);
    }
}

static void helm_colorlab_attach(lv_obj_t * list)
{
    if (list == NULL || list == s_colorlab_list) {
        return;
    }

    s_colorlab_list = list;
    lv_obj_add_event_cb(list, helm_colorlab_draw, LV_EVENT_DRAW_MAIN, NULL);
}

/** @brief 分组标题行高（给首行加 margin，标题就画在这段空白里）。 */
#define HELM_GRP_HEAD_H 20

static lv_obj_t * s_grp_list;

/** @brief 画分组卡片 + 卡内发丝线。**在列表的 DRAW_MAIN 里自绘**。
 *
 *  @details 为什么不用对象做卡片：菜单行的 bg_opa 恒为 TRANSP
 *           （helm_mitem_apply_mix：选中块画在列表 DRAW_MAIN_END、比子对象早，
 *            行要是有实底就把选中块盖住）⇒ 卡片只能自己画。
 *           在 DRAW_MAIN 里按**当前行坐标**现画还有个好处：滚动、光标、换页都不用
 *           跟着重排；LVGL 会把绘制裁到脏区，所以光标移动不会重画整张卡。
 */
static void helm_grp_draw(lv_event_t * e)
{
    helm_menu_t * m = s_menu;
    lv_obj_t * list = lv_event_get_target(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    uint8_t i;

    if (m == NULL || list == NULL || layer == NULL || m->count == 0) {
        return;
    }

    if (lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    for (i = 0; i < m->count; ) {
        uint8_t g = m->items[i].grp;
        uint8_t j = i;
        uint8_t k;
        lv_obj_t * first;
        lv_obj_t * last;
        lv_area_t a1;
        lv_area_t a2;
        lv_area_t card;
        bool ok_title;
        lv_draw_rect_dsc_t rd;

        while (j + 1u < m->count && m->items[j + 1u].grp == g) {
            j++;
        }

        first = lv_obj_get_child(list, i);
        last = lv_obj_get_child(list, j);
        if (first == NULL || last == NULL) {
            i = (uint8_t)(j + 1u);
            continue;
        }

        lv_obj_get_coords(first, &a1);
        lv_obj_get_coords(last, &a2);
        ok_title = (g > 0 && (uint8_t)(g - 1u) < m->grp_n &&
                    m->grps[g - 1u].title != NULL);
        card.x1 = a1.x1 - 2;
        card.x2 = a1.x2 + 2;
        /* 卡片顶在首行上方 4px：标题那段空白留在**卡外**（App 的分组标题是压在
         * 页面底上的，不在卡里）。 */
        card.y1 = a1.y1 - 4;
        card.y2 = a2.y2 + 3;

                lv_draw_rect_dsc_init(&rd);
        rd.bg_color = helm_color(HELM_COLOR_MENU_CARD);
        rd.bg_opa = LV_OPA_COVER;
        rd.radius = HELM_RADIUS;
        lv_draw_rect(layer, &rd, &card);

        if (ok_title) {
            /* 分组标题**自绘**，不建标签对象：列表的复用判定是
             * `lv_obj_get_child_count(list) == m->count`，多一个子对象就让
             * 每次重画都走"整表重建"⇒ 光标不动画、卡、焦点还可能被重建带走
             * （用户 2026-09-26 报的正是这三条）。标题落在首行上方那段空白里
             * （首行有 margin_top 腾的位置），画在卡片上沿之上。 */
            lv_draw_label_dsc_t td;
            lv_area_t ta;

            lv_draw_label_dsc_init(&td);
            td.text = m->grps[g - 1u].title;
            td.text_local = 1;
            /* 自绘也要带链：分组标题里的字同样可能落在点阵库外（见 helm_font.c）。 */
            td.font = helm_font_lab();
            td.color = helm_color(HELM_COLOR_MENU_MUTED);
            ta.x1 = card.x1 + 10;
            ta.x2 = card.x2 - 10;
            ta.y1 = a1.y1 - HELM_GRP_HEAD_H + 3;
            ta.y2 = a1.y1 - 2;
            lv_draw_label(layer, &td, &ta);
        }

        /* 卡内发丝线：缩进到文字列（HELM_MENU_HAIR_INDENT），两端留内边距。 */
        for (k = i; k < j; k++) {
            lv_obj_t * r1 = lv_obj_get_child(list, k);
            lv_obj_t * r2 = lv_obj_get_child(list, (uint8_t)(k + 1u));
            lv_area_t b1;
            lv_area_t b2;
            lv_area_t ln;
            lv_draw_rect_dsc_t ld;

            if (r1 == NULL || r2 == NULL) {
                continue;
            }

            lv_obj_get_coords(r1, &b1);
            lv_obj_get_coords(r2, &b2);
            ln.x1 = card.x1 + HELM_MENU_HAIR_INDENT;
            ln.x2 = card.x2 - 10;
            ln.y1 = (b1.y2 + b2.y1) / 2;
            ln.y2 = ln.y1;
            lv_draw_rect_dsc_init(&ld);
            ld.bg_color = helm_color(HELM_COLOR_MENU_HAIR);
            ld.bg_opa = LV_OPA_COVER;
            ld.radius = 0;
            lv_draw_rect(layer, &ld, &ln);
        }

        i = (uint8_t)(j + 1u);
    }
}

/** @brief 开一个分组：从**下一行**（下标 row）起算。没开组的页 = 一整张无标题卡。 */
typedef enum {
    VAL_NONE = 0,
    VAL_SENSOR_N,     /**< 主菜单「蓝牙设备」副标题：N 已连接 */
    VAL_RIDES_N,      /**< 主菜单「骑行记录」副标题：N 条 · 最近 9月26日 */
    VAL_SET_SUB,      /**< 主菜单「设置」副标题：日光 · 亮度 78% */
    VAL_EPH_LAST,     /**< 主菜单「星历」：右值=上次同步时间，副标题=还有多久过期 */
    VAL_TOOLS_SUB,    /**< 主菜单「工具箱」副标题（固定文案，留位） */
    VAL_DEV_BT,       /**< 设置「手机蓝牙」：开关 + 副标题 已连接/未连接 */
    VAL_DEV_SENSOR,   /**< 设置「外设蓝牙」：开关 + 副标题 N 已连接 */
    VAL_BL,           /**< 设置「亮度」右值 */
    VAL_THEME,        /**< 设置「显示」右值：日光/夜间 */
    VAL_UNIT,         /**< 设置「单位」右值：公里/英里 */
    VAL_TZ,           /**< 设置「时区」右值 */
    VAL_SOUND,        /**< 设置「蜂鸣器」开关 */
    VAL_AUTOPAUSE,    /**< 设置「自动暂停」开关 */
    VAL_NOTIF,        /**< 设置「手机通知」开关 */
    VAL_CALLS,        /**< 设置「仅来电」开关 */
    VAL_INBOX_SUB,    /**< 设置「最近通知」副标题：收件箱 N 条 */
    VAL_MTP,          /**< 设置「USB 传输」开关 */
    VAL_FW,           /**< 设置「固件」右值=构建日期、副标题=版本 */
    VAL_REC_N,        /**< GPX「记录」副标题：本机 N 条 */
    VAL_TRIP,         /**< 导航中「途经点」右值 */
    VAL_EPH_AUTO,     /**< 星历「自动维护」开关 */
    VAL_SKIP_SUB,     /**< 导航中「跳过本点」副标题：当前 N/M 未到 */
    VAL_DEMO,         /**< 设置「演示模式」开关 + 副标题=当前那一步 */
} helm_val_t;

/** @brief 一行：group 非 NULL ⇒ 从这一行开新组并作为分组标题。 */
typedef struct {
    const char * grp;
    uint8_t ico;
    const char * title;
    const char * sub;      /**< 静态副标题；NULL = 无（或被取值器覆盖） */
    uint8_t val;           /**< helm_val_t */
    uint8_t act;
    uint8_t kind;
} helm_prow_t;

/** @brief 取值结果：副标题 / 右值 / 开关状态。空指针 = 用表里的静态值。 */
typedef struct {
    const char * sub;
    const char * val;
    bool on;
} helm_pval_out_t;

/** @brief 导航中：主菜单最上面那一组（设计稿 13 页）。 */
/** 「清空记录」是否已装填（确认 dock 靠它区分"删单条"与"清空全部"）。 */
static bool s_clear_all_armed;

static const helm_prow_t P_ROOT_NAV[] = {
    /* 设计稿 13 页：导航中这一组。**原来第一行是「关闭轨迹」，已删掉**（用户 2026-09-27）——
     * 它和本组最后一行的「停止导航」是同一个动作（都是 ACT_NAV_STOP），而且"关闭轨迹"这个
     * 说法容易被读成"删骑行记录"，歧义大。组标题「本次导航」随之挪到新的第一行上。 */
    { "本次导航", HELM_ICO_PIN, "就近规划", "从最近点向后", VAL_NONE,
      ACT_NAV_NEAREST, HELM_KIND_GO },
    { NULL, HELM_ICO_LIST, "跳过本点", NULL, VAL_SKIP_SUB, ACT_NAV_SKIP,
      HELM_KIND_GO },
    { NULL, HELM_ICO_PIN, "途经点", NULL, VAL_TRIP, ACT_NOP, HELM_KIND_GO },
    { NULL, HELM_ICO_XMARK, "停止导航", "结束本次导航", VAL_NONE,
      ACT_NAV_STOP, HELM_KIND_GO },
};

/** @brief 回放中：主菜单最上面那一组（设计稿 14 页）。 */
static const helm_prow_t P_ROOT_GPX[] = {
    /* 这一行是**回放唯一的出口**，所以留着，但名字从同样有歧义的「关闭轨迹」改成
     * 「结束回放」（用户 2026-09-27）。 */
    { "回放", HELM_ICO_XMARK, "结束回放", NULL, VAL_NONE, ACT_NAV_STOP, HELM_KIND_GO },
};

/** @brief 主菜单（设计稿 01 页）。 */
static const helm_prow_t P_ROOT[] = {
    { "路线", HELM_ICO_NAV, "导航", "GPX / 坐标点 / 常用点", VAL_NONE,
      ACT_ENTER_NAV, HELM_KIND_GO },
    { "设备", HELM_ICO_BLE, "蓝牙设备", NULL, VAL_SENSOR_N,
      ACT_ENTER_SENSORS, HELM_KIND_GO },
    { NULL, HELM_ICO_NAV, "工具箱", "指南针 / 水平仪", VAL_NONE,
      ACT_ENTER_TOOLS, HELM_KIND_GO },
    /* 星历不在设计稿的主菜单里，但它是唯一入口 ⇒ 留在「系统」组第一位（不能让功能失去入口） */
    { "系统", HELM_ICO_GPS, "星历", NULL, VAL_EPH_LAST,
      ACT_ENTER_EPH, HELM_KIND_GO },
    { NULL, HELM_ICO_LIST, "骑行记录", NULL, VAL_RIDES_N,
      ACT_ENTER_RIDES, HELM_KIND_GO },
    { NULL, HELM_ICO_CHIP, "系统状态", "磁盘 · 内存 · 线程", VAL_NONE,
      ACT_ENTER_SYSSTAT, HELM_KIND_GO },
    { NULL, HELM_ICO_GEAR, "设置", NULL, VAL_SET_SUB,
      ACT_ENTER_SETTINGS, HELM_KIND_GO },
    { NULL, HELM_ICO_POWER, "关机", "断电", VAL_NONE,
      ACT_POWEROFF, HELM_KIND_GO },
};

/** @brief 导航（设计稿 05 页）：只放"要导航去哪"，进行中的动作在主菜单。 */
static const helm_prow_t P_NAV[] = {
    { "路线", HELM_ICO_GPX, "GPX导航", "导入 / 记录", VAL_NONE,
      ACT_ENTER_GPX, HELM_KIND_GO },
    { NULL, HELM_ICO_NAV, "坐标点导航", "App 下发后点选", VAL_NONE,
      ACT_ENTER_NAVPTS, HELM_KIND_GO },
    { NULL, HELM_ICO_STAR, "常用点导航", "App 同步后点选", VAL_NONE,
      ACT_ENTER_FAVS, HELM_KIND_GO },
};

/** @brief 工具箱（设计稿 04 页）。 */
static const helm_prow_t P_TOOLS[] = {
    { "工具", HELM_ICO_NAV, "指南针", "地磁罗盘", VAL_NONE,
      ACT_TOOL_COMPASS, HELM_KIND_GO },
    { NULL, HELM_ICO_CLIMB, "水平仪", "十字圆环", VAL_NONE,
      ACT_TOOL_LEVEL, HELM_KIND_GO },
    { NULL, HELM_ICO_BOLT, "加速度计", "G 值", VAL_NONE,
      ACT_TOOL_GMETER, HELM_KIND_GO },
    { NULL, HELM_ICO_ALT, "高度计", "气压 / 高度", VAL_NONE,
      ACT_TOOL_ALT, HELM_KIND_GO },
};

/** @brief GPX（设计稿 09 页）。 */
static const helm_prow_t P_GPX[] = {
    { "文件", HELM_ICO_INBOX, "导入", "USB / App 导入", VAL_NONE,
      ACT_ENTER_GPX_IMPORT, HELM_KIND_GO },
    { NULL, HELM_ICO_LIST, "记录", NULL, VAL_REC_N,
      ACT_ENTER_GPX_RECORD, HELM_KIND_GO },
};

/** @brief 设置（设计稿 02 页）。「显示」不在稿里，但它是切日光/夜间的唯一入口，保留。 */
static const helm_prow_t P_SETTINGS[] = {
    { "连接", HELM_ICO_BLE, "手机蓝牙", NULL, VAL_DEV_BT,
      ACT_TOGGLE_BT, HELM_KIND_SW },
    { NULL, HELM_ICO_CHIP, "外设蓝牙", NULL, VAL_DEV_SENSOR,
      ACT_TOGGLE_SENSOR, HELM_KIND_SW },
    { "显示", HELM_ICO_SUN, "亮度", "夜间 / 日光", VAL_BL,
      ACT_CYCLE_BL, HELM_KIND_VAL },
    { NULL, HELM_ICO_SOUND, "蜂鸣器", NULL, VAL_SOUND,
      ACT_TOGGLE_SOUND, HELM_KIND_SW },
    { NULL, HELM_ICO_EYE, "显示", "日光 / 夜间", VAL_THEME,
      ACT_CYCLE_THEME, HELM_KIND_VAL },
    { NULL, HELM_ICO_UNIT, "单位", NULL, VAL_UNIT,
      ACT_CYCLE_UNIT, HELM_KIND_VAL },
    { NULL, HELM_ICO_TIME, "时区", NULL, VAL_TZ,
      ACT_CYCLE_TZ, HELM_KIND_VAL },
    { "骑行", HELM_ICO_PAUSE, "自动暂停", "按速度 / 踏频", VAL_AUTOPAUSE,
      ACT_TOGGLE_AUTOPAUSE, HELM_KIND_SW },
    { NULL, HELM_ICO_CLIMB, "坡度校准", "水平放置后归零", VAL_NONE,
      ACT_ENTER_GRADECAL, HELM_KIND_GO },
    { "通知", HELM_ICO_BELL, "手机通知", "横幅 + 收件箱", VAL_NOTIF,
      ACT_TOGGLE_NOTIF, HELM_KIND_SW },
    { NULL, HELM_ICO_CALL, "仅来电", "只弹来电", VAL_CALLS,
      ACT_TOGGLE_CALLS, HELM_KIND_SW },
    { NULL, HELM_ICO_INBOX, "最近通知", NULL, VAL_INBOX_SUB,
      ACT_ENTER_INBOX, HELM_KIND_GO },
    { "系统", HELM_ICO_USB, "USB 传输", "MTP 模式", VAL_MTP,
      ACT_TOGGLE_USB, HELM_KIND_SW },
    /* 演示模式：四步循环（GPX 导航 → 坐标点导航 → 全国跳点 → 换主题），
     * 给展会/拍摄用。开关状态与当前那一步都来自 bicycle_demo_status()。 */
    { NULL, HELM_ICO_PLAY, "演示模式", "导航 / 跳点 / 换主题", VAL_DEMO,
      ACT_DEMO_TOGGLE, HELM_KIND_SW },
    { NULL, HELM_ICO_GEAR, "测试配置", "字体 / GNSS 解算", VAL_NONE,
      ACT_ENTER_TEST, HELM_KIND_GO },
    { NULL, HELM_ICO_CHIP, "固件", NULL, VAL_FW,
      ACT_ENTER_ABOUT, HELM_KIND_GO },
};


/** @brief 取一行的动态内容（开关状态 / 右值 / 随状态变的副标题）。
 *  @note 返回的字符串是**函数内静态缓冲**：helm_row_set 会立刻拷进 items，
 *        单线程 UI 够用；调用方不要留着指针跨行用。 */
static bool s_eph_dirty = true;
static bool s_eph_have;
static uint32_t s_eph_last;
static uint32_t s_eph_next;

/** @brief 星历时间（上次/下次同步）—— 带事件驱动缓存。
 *  @details 现场实测（[vperf] ui slow menu 的 `max src=4`）：**单次 221 ms**，
 *           而主菜单「星历」行的右值每次填页都要问一遍，是现在 fill 里最贵的一项。
 *           这个时间只会因为"收到/写入星历"而变，所以按屏失效即可（进星历页、
 *           手动写入之后），平时一律读缓存。 */
static bool helm_eph_times_cached(uint32_t * last, uint32_t * next)
{
    if (s_eph_dirty) {
        s_eph_have = myvendor_gnss_eph_times(&s_eph_last, &s_eph_next);
        s_eph_dirty = false;
    }

    if (last != NULL) {
        *last = s_eph_last;
    }

    if (next != NULL) {
        *next = s_eph_next;
    }

    return s_eph_have;
}

static void helm_eph_times_invalidate(void)
{
    s_eph_dirty = true;
}

/* 打桩计数：必须在 helm_pval 之前（那里就在用），上报在 helm_menu_paint。 */
static uint32_t s_ui_val_max_ms;
static uint32_t s_ui_sysref_ms;
static uint8_t s_ui_val_max_src;

static void helm_pval(const helm_prow_t * r, helm_pval_out_t * o)
{
    static char buf[40];
    static char buf2[24];
    uint32_t t_pv = lv_tick_get();

    o->sub = NULL;
    o->val = NULL;
    o->on = false;
    buf[0] = '\0';
    buf2[0] = '\0';

    switch (r->val) {
    case VAL_SENSOR_N:
        lv_snprintf(buf, sizeof(buf), "%u 已连接", (unsigned)helm_sensor_n());
        o->sub = buf;
        break;

    case VAL_RIDES_N:
    case VAL_REC_N:
        {
            uint8_t nrec = helm_gpx_scan_cached(MYVENDOR_GPX_RECORD_DIR);

            if (nrec == 0) {
                o->sub = "暂无记录";
            } else {
                lv_snprintf(buf, sizeof(buf), "%u 条", (unsigned)nrec);
                o->sub = buf;
            }
        }
        break;

    case VAL_SET_SUB:
        {
            /* ⚠ 判据必须跟**调色板**同源（`helm_pal_night()`）：原来这里按主题名首字母
             * 猜（'o'=夜间、其余=日光），而调色板是按 'c'=日光、其余=夜间 —— 两边对
             * **"night"** 的结论正好相反 ⇒ 选了夜间，这一行却写"日光"。
             * 用户现场："显示是日光，实际效果是夜间"。 */
            lv_snprintf(buf, sizeof(buf), "%s · 亮度%s",
                        helm_pal_night() ? "夜间" : "日光",
                        helm_bl_name(myvendor_devctl_bl_get()));
            o->sub = buf;
        }
        break;

    case VAL_EPH_LAST:
        {
            uint32_t last = 0;
            uint32_t next = 0;

            if (helm_eph_times_cached(&last, &next)) {
                helm_eph_local(buf2, sizeof(buf2), last, false);
                o->val = buf2;
                o->sub = (next != 0 &&
                          time(NULL) >= (time_t)next) ? "已过期" : "已同步";
            } else {
                o->sub = "未同步";
            }
        }
        break;

    case VAL_DEV_BT:
        o->on = myvendor_devctl_radio_get();
        o->sub = myvendor_sys_phone_ble_connected() ? "已连接" : "未连接";
        break;

    case VAL_DEV_SENSOR:
        o->on = myvendor_devctl_sensor_get();
        lv_snprintf(buf, sizeof(buf), "%u 已连接", (unsigned)helm_sensor_n());
        o->sub = buf;
        break;

    case VAL_BL:
        o->val = helm_bl_name(myvendor_devctl_bl_get());
        break;

    case VAL_THEME:
        /* ⚠ 同 `VAL_SET_SUB`：判据必须和调色板一致（`helm_pal_night()`）。
         * 原来按主题名首字母（'o'→夜间，其余→日光），而 "night" 的名字是 'n' ⇒
         * 选了夜间这一行却写"日光"—— 用户现场"显示是日光，实际效果是夜间"。 */
        o->val = helm_pal_night() ? "夜间" : "日光";
        break;

    case VAL_UNIT:
        o->val = s_unit_imperial ? "英里" : "公里";
        break;

    case VAL_TZ:
        helm_tz_fmt(buf2, sizeof(buf2));
        o->val = buf2;
        break;

    case VAL_SOUND:
        o->on = myvendor_devctl_sound_get();
        break;

    case VAL_AUTOPAUSE:
        o->on = myvendor_devctl_autopause_get();
        break;

    case VAL_NOTIF:
        o->on = myvendor_devctl_notif_get();
        break;

    case VAL_CALLS:
        o->on = myvendor_devctl_notif_calls_only_get();
        break;

    case VAL_MTP:
        o->on = myvendor_devctl_mtp_get();
        break;

    case VAL_EPH_AUTO:
        o->on = myvendor_devctl_eph_auto_get();
        break;

    case VAL_DEMO:
        /* 开关状态与副标题同源（都问 bicycle_demo_*）：开着的时候副标题就是
         * 当前那一步（"模拟 GPX 导航"/"全国地图解析 · 太湖"/…）。 */
        o->on = bicycle_demo_active();
        if (o->on) {
            o->sub = bicycle_demo_status();
        }
        break;

    case VAL_INBOX_SUB:
        {
            uint8_t nn = 0;

            myvendor_sys_inbox_get(NULL, &nn, 0);
            if (nn > 0) {
                lv_snprintf(buf, sizeof(buf), "收件箱 %u 条", (unsigned)nn);
                o->sub = buf;
            }
        }
        break;

    case VAL_FW:
        {
            const char * ver = myvendor_sw_version();
            const char * bld = myvendor_build_date();

            o->sub = (ver && ver[0]) ? ver : VERSION_SOFTWARE;
            o->val = (bld && bld[0]) ? bld : "";
        }
        break;

    case VAL_SKIP_SUB:
        {
            uint32_t idx = 0;
            uint32_t total = 0;

            if (map_page_nav_trip_progress(lvgl_page_map(), &idx, &total) &&
                total > 0u) {
                lv_snprintf(buf, sizeof(buf), "当前 %u/%u 未到",
                            (unsigned)idx, (unsigned)total);
                o->sub = buf;
            }
        }
        break;

    case VAL_TRIP:
        {
            uint32_t idx = 0;
            uint32_t total = 0;

            if (map_page_nav_trip_progress(lvgl_page_map(), &idx, &total)) {
                lv_snprintf(buf2, sizeof(buf2), "%u/%u",
                            (unsigned)idx, (unsigned)total);
            } else {
                lv_snprintf(buf2, sizeof(buf2), "未到");
            }
            o->val = buf2;
        }
        break;

    default:
        break;
    }

    {
        uint32_t dt = lv_tick_elaps(t_pv);

        if (dt > s_ui_val_max_ms) {
            s_ui_val_max_ms = dt;
            s_ui_val_max_src = (uint8_t)r->val;
        }
    }
}

#define HELM_PTAB(t) t, (uint8_t)(sizeof(t) / sizeof((t)[0]))

/** @brief 按页表填行：组标题、静态文案、动态右值、开关状态一次到位。 */
static uint8_t helm_fill_table(helm_menu_t * m, uint8_t n, const helm_prow_t * t,
                              uint8_t cnt)
{
    uint8_t i;

    for (i = 0; i < cnt && n < HELM_MENU_ROWS; i++) {
        helm_pval_out_t o;

        if (t[i].grp != NULL) {
            helm_grp_begin(m, n, t[i].grp);
        }

        helm_pval(&t[i], &o);
        helm_row_set(m, n, t[i].ico, t[i].title,
                     o.sub ? o.sub : t[i].sub,
                     o.val ? o.val : "", t[i].act, t[i].kind);
        if (t[i].kind == HELM_KIND_SW) {
            m->items[n].on = o.on;
        }

        n++;
    }

    return n;
}

/* =====================================================================================
 * 菜单页表 —— **唯一数据源是设计稿**
 *   zcode/analysis/helm_menu_redesign/preview/{light,dark}/01..20_*.png
 *   生成脚本 zcode/analysis/helm_preview/menu_preview.py 的 P_* 表 + py/build_all.py
 * 页 = 若干组；组 = 标题 + 行。表里只写"长什么样"，**动态内容一律走 helm_pval()**
 * （开关状态、右值、随状态变的副标题），所以不用为了改文案去动逻辑。
 * ===================================================================================== */
static void helm_grp_begin(helm_menu_t * m, uint8_t row, const char * title)
{
    if (m == NULL || m->grp_n >= HELM_MGRP_MAX) {
        return;
    }

    m->grps[m->grp_n].title = title;
    m->grps[m->grp_n].row = row;
    m->grp_n++;
}

/** @brief 行填完：把组号盖到行上，并把自绘回调挂到列表上（只挂一次）。 */
static void helm_grp_finish(helm_menu_t * m, uint8_t rows)
{
    uint8_t g;

    if (m == NULL) {
        return;
    }

    if (m->grp_n == 0) {
        helm_grp_begin(m, 0, NULL);
    }

    for (g = 0; g < m->grp_n; g++) {
        uint8_t from = m->grps[g].row;
        uint8_t to = (uint8_t)((uint8_t)(g + 1u) < m->grp_n ?
                               m->grps[g + 1u].row : rows);
        uint8_t i;

        for (i = from; i < to && i < HELM_MENU_ROWS; i++) {
            m->items[i].grp = (uint8_t)(g + 1u);
        }
    }

    if (m->list != NULL && m->list != s_grp_list) {
        s_grp_list = m->list;
        lv_obj_add_event_cb(m->list, helm_grp_draw, LV_EVENT_DRAW_MAIN, NULL);
    }
}

/** @brief 给带标题的组腾出标题行：首行加 margin_top（标题由 helm_grp_draw 自绘）。
 *  @note 这里**不建对象、不额外 layout** —— 标题要是标签，列表复用就永远命中不了。 */
static void helm_grp_layout(helm_menu_t * m)
{
    uint8_t g;

    if (m == NULL || m->list == NULL) {
        return;
    }

    for (g = 0; g < m->grp_n; g++) {
        lv_obj_t * row;

        if (m->grps[g].title == NULL || m->grps[g].row >= m->count) {
            continue;
        }

        row = lv_obj_get_child(m->list, m->grps[g].row);
        if (row != NULL) {
            lv_obj_set_style_margin_top(row, HELM_GRP_HEAD_H, 0);
        }
    }
}

static void helm_ride_track_draw(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_area_t coords;
    int32_t w;
    int32_t h;
    const int32_t pad = 14;
    float min_lon;
    float max_lon;
    float min_lat;
    float max_lat;
    float sx;
    float sy;
    uint16_t i;
    lv_draw_line_dsc_t ld;
    lv_draw_rect_dsc_t rd;

    if (obj == NULL || layer == NULL || s_ride_pt_n < 2u) {
        return;
    }

    lv_obj_get_coords(obj, &coords);
    w = lv_area_get_width(&coords) - pad * 2;
    h = lv_area_get_height(&coords) - pad * 2;
    if (w < 8 || h < 8) {
        return;
    }

    min_lon = max_lon = s_ride_lon[0];
    min_lat = max_lat = s_ride_lat[0];
    for (i = 1u; i < s_ride_pt_n; i++) {
        if (s_ride_lon[i] < min_lon) {
            min_lon = s_ride_lon[i];
        }
        if (s_ride_lon[i] > max_lon) {
            max_lon = s_ride_lon[i];
        }
        if (s_ride_lat[i] < min_lat) {
            min_lat = s_ride_lat[i];
        }
        if (s_ride_lat[i] > max_lat) {
            max_lat = s_ride_lat[i];
        }
    }

    sx = (max_lon > min_lon) ? ((float)w / (max_lon - min_lon)) : 0.0f;
    sy = (max_lat > min_lat) ? ((float)h / (max_lat - min_lat)) : 0.0f;
    if (sx <= 0.0f && sy <= 0.0f) {
        sx = sy = 1.0f;
    } else if (sx <= 0.0f) {
        sx = sy;
    } else if (sy <= 0.0f) {
        sy = sx;
    } else if (sx < sy) {
        sy = sx;
    } else {
        sx = sy;
    }

    {
        const float used_w = (max_lon - min_lon) * sx;
        const float used_h = (max_lat - min_lat) * sy;
        const float ox = (float)coords.x1 + (float)pad
            + ((float)w - used_w) * 0.5f;
        const float oy = (float)coords.y1 + (float)pad
            + ((float)h - used_h) * 0.5f;

        lv_point_precise_t pts[HELM_RIDE_PT_MAX];
        uint16_t n = 0;

        lv_draw_line_dsc_init(&ld);
        ld.color = helm_color(HELM_COLOR_NAV);
        ld.width = 3;
        ld.opa = LV_OPA_COVER;
        ld.round_start = 1;
        ld.round_end = 1;

        for (i = 0u; i < s_ride_pt_n; i++) {
            const lv_coord_t x = (lv_coord_t)(ox
                + (s_ride_lon[i] - min_lon) * sx);
            const lv_coord_t y = (lv_coord_t)(oy
                + (max_lat - s_ride_lat[i]) * sy);

            if (n > 0u && pts[n - 1u].x == x && pts[n - 1u].y == y) {
                continue;
            }

            pts[n].x = x;
            pts[n].y = y;
            n++;
        }

        if (n >= 2u) {
            ld.points = pts;
            ld.point_cnt = n;
            lv_draw_line(layer, &ld);
        }

        lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_COVER;
        rd.border_width = 0;
        rd.radius = LV_RADIUS_CIRCLE;

        {
            lv_area_t a;
            const lv_coord_t x = (lv_coord_t)(ox
                + (s_ride_lon[0] - min_lon) * sx);
            const lv_coord_t y = (lv_coord_t)(oy
                + (max_lat - s_ride_lat[0]) * sy);

            rd.bg_color = helm_color(HELM_COLOR_GPS);
            a.x1 = x - 5;
            a.y1 = y - 5;
            a.x2 = x + 5;
            a.y2 = y + 5;
            lv_draw_rect(layer, &rd, &a);

            rd.bg_color = helm_color(HELM_COLOR_INK);
            {
                const lv_coord_t ex = (lv_coord_t)(ox
                    + (s_ride_lon[s_ride_pt_n - 1u] - min_lon) * sx);
                const lv_coord_t ey = (lv_coord_t)(oy
                    + (max_lat - s_ride_lat[s_ride_pt_n - 1u]) * sy);

                a.x1 = ex - 5;
                a.y1 = ey - 5;
                a.x2 = ex + 5;
                a.y2 = ey + 5;
                lv_draw_rect(layer, &rd, &a);
                rd.bg_color = helm_color(HELM_COLOR_PAPER);
                a.x1 = ex - 2;
                a.y1 = ey - 2;
                a.x2 = ex + 2;
                a.y2 = ey + 2;
                lv_draw_rect(layer, &rd, &a);
            }
        }
    }
}

static void helm_ride_refresh(helm_menu_t * m)
{
    uint8_t * sel;
    uint8_t i;
    char buf[40];

    if (m == NULL || m->ride == NULL || m->ride_keys == NULL) {
        return;
    }

    (void)helm_ride_load();
    if (m->ride_dist) {
        if (s_ride_pt_n >= 2u) {
            helm_fmt_ride_sub(buf, sizeof(buf), s_ride_km, s_ride_sec,
                              s_ride_has_time);
        } else {
            lv_snprintf(buf, sizeof(buf), "无轨迹");
        }

        lv_label_set_text(m->ride_dist, buf);
    }

    if (m->ride_track) {
        lv_obj_invalidate(m->ride_track);
    }

    sel = helm_cur_sel(m);
    {
        bool reuse = false;

        if (m->count > 0 &&
            lv_obj_get_child_count(m->ride_keys) == m->count) {
            reuse = true;
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;
                lv_obj_t * row = lv_obj_get_child(m->ride_keys, i);

                memset(&spec, 0, sizeof(spec));
                spec.ico = m->items[i].ico;
                spec.title = m->items[i].label;
                spec.kind = HELM_KIND_SLIM;
                spec.sel = (sel && *sel == i);
                if (!helm_mitem_refresh(row, &spec)) {
                    reuse = false;
                    break;
                }
            }
        }

        if (!reuse) {
            lv_obj_clean(m->ride_keys);
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;

                memset(&spec, 0, sizeof(spec));
                spec.ico = m->items[i].ico;
                spec.title = m->items[i].label;
                spec.kind = HELM_KIND_SLIM;
                spec.sel = (sel && *sel == i);
                helm_mitem_create(m->ride_keys, &spec);
            }
        }

        if (sel && m->count > 0) {
            uint8_t idx = *sel;

            if (idx >= m->count) {
                idx = 0;
                *sel = 0;
            }
            lv_obj_update_layout(m->ride);
            lv_obj_update_layout(m->ride_keys);
            helm_mlist_sel_snap(m->ride_keys, idx);
        }
    }
}

static void helm_ride_close_map(const char * title, const char * sub)
{
    myvendor_sound_ok();
    lv_pm_notify_show(title, sub, 1500);
    (void)lv_pm_close_page_msg(NULL);
}

static bool helm_fill_items(helm_menu_t * m)
{
    myvendor_sys_sensor_ui_t ui;
    helm_scr_t scr = helm_cur_scr(m);
    uint8_t n = 0;
    bool empty = false;

    memset(m->items, 0, sizeof(m->items));
    m->grp_n = 0;
    /* **只在"进入骑行记录"时**让记录数重扫一次（用户 2026-09-26 明确：
     * 进菜单不扫）。开机靠初值脏；骑行结束在保存那条路径上显式失效。
     * 同一屏的反复重画（含 0.5 Hz 后台刷新）一律读缓存。 */
    if ((int)scr != s_rc_scr) {
        s_rc_scr = (int)scr;
        if (scr == HELM_SCR_RIDES || scr == HELM_SCR_GPX_RECORD) {
            helm_gpx_count_invalidate();
        }

        if (scr == HELM_SCR_EPH) {
            helm_eph_times_invalidate();
        }
    }
    myvendor_sys_sensor_ui_get(&ui);

    switch (scr) {
    case HELM_SCR_ROOT:
        /* 导航中 / 回放中：动作组提到最上面（设计稿 13/14 页），别再钻进「导航」去找。 */
        if (map_page_review_active(lvgl_page_map())) {
            n = helm_fill_table(m, n, HELM_PTAB(P_ROOT_GPX));
        } else if (map_page_nav_active(lvgl_page_map())
                   || map_page_nav_planning(lvgl_page_map())) {
            n = helm_fill_table(m, n, HELM_PTAB(P_ROOT_NAV));
        }

        n = helm_fill_table(m, n, HELM_PTAB(P_ROOT));
        break;

    case HELM_SCR_NAV:
        n = helm_fill_table(m, n, HELM_PTAB(P_NAV));
        break;

    case HELM_SCR_GPX:
        n = helm_fill_table(m, n, HELM_PTAB(P_GPX));
        break;

    case HELM_SCR_NAVPTS:
        if (myvendor_mtp_lfs_quiesce()) {
            helm_row_set(m, n++, HELM_ICO_NAV, "写入中",
                         "稍后再打开", "", ACT_NOP, HELM_KIND_GO);
            empty = false;
            break;
        }
        if (helm_navpt_scan() == 0) {
            empty = true;
            break;
        }
        {
            uint8_t i;

            for (i = 0; i < s_gpx_n && n < HELM_MENU_ROWS; i++) {
                char title[HELM_GPX_NAME_MAX];
                char * dot;
                char sub[24];

                lv_snprintf(title, sizeof(title), "%s", s_gpx_names[i]);
                dot = strrchr(title, '.');
                if (dot != NULL) {
                    *dot = '\0';
                }

                lv_snprintf(sub, sizeof(sub), "%lu B",
                            (unsigned long)s_gpx_size[i]);
                helm_row_set(m, n++, HELM_ICO_NAV, title, sub, "",
                             ACT_ENTER_NAVPT_REC, HELM_KIND_GO);
                m->items[n - 1u].extra = i;
            }
        }
        empty = n == 0u;
        break;

    case HELM_SCR_NAVPT_DETAIL:
        if (s_navpt_path[0] == '\0') {
            empty = true;
            break;
        }
        {
            myvendor_devctl_favorite_t pts[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t pt_n = 0;

            if (myvendor_devctl_waypoints_load(s_navpt_path, pts,
                    MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0 ||
                pt_n == 0) {
                empty = true;
                break;
            }

            empty = helm_fill_waypoint_rows(m, &n, HELM_ICO_NAV, pts, pt_n,
                                            false);
        }
        break;

    case HELM_SCR_NAVPT_ACTIONS:
        if (s_navpt_path[0] == '\0') {
            empty = true;
            break;
        }
        helm_row_set(m, n++, HELM_ICO_NAV, "导航", "按文件顺序", "",
                     ACT_RIDE_NAV, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_REV, "返航", "反向途经点", "",
                     ACT_RIDE_REV, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_XMARK, "删除", "删除本条", "",
                     ACT_RIDE_DEL, HELM_KIND_GO);
        break;

    case HELM_SCR_FAVS:
        {
            myvendor_devctl_favorite_t favs[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t fav_n = 0;

            if (myvendor_devctl_favorites_load(favs,
                    MYVENDOR_DEVCTL_FAVORITE_MAX, &fav_n) != 0 ||
                fav_n == 0) {
                empty = true;
                break;
            }

            empty = helm_fill_waypoint_rows(m, &n, HELM_ICO_STAR, favs, fav_n,
                                            true);
        }
        break;

    case HELM_SCR_SENSORS:
        {
            char st[16];
            char bat[24];

            helm_slot_status(st, sizeof(st), bat, sizeof(bat),
                             &ui.slot[MYVENDOR_SYS_SENSOR_KIND_HR],
                             myvendor_devctl_sensor_get());
            helm_grp_begin(m, n, "配对记录");
        helm_row_set(m, n++, HELM_ICO_HR, "心率带", bat, st,
                         ACT_ENTER_SCAN_HR, HELM_KIND_GO);
            helm_slot_status(st, sizeof(st), bat, sizeof(bat),
                             &ui.slot[MYVENDOR_SYS_SENSOR_KIND_CSC],
                             myvendor_devctl_sensor_get());
            helm_row_set(m, n++, HELM_ICO_CAD, "踏频器", bat, st,
                         ACT_ENTER_SCAN_CAD, HELM_KIND_GO);
            helm_slot_status(st, sizeof(st), bat, sizeof(bat),
                             &ui.slot[MYVENDOR_SYS_SENSOR_KIND_CPS],
                             myvendor_devctl_sensor_get());
            helm_row_set(m, n++, HELM_ICO_BOLT, "功率计", bat, st,
                         ACT_ENTER_SCAN_PWR, HELM_KIND_GO);
            helm_grp_begin(m, n, "手机");
        helm_row_set(m, n++, HELM_ICO_BLE, "手机蓝牙", "",
                         myvendor_sys_phone_ble_connected() ? "已连接" : "未连接",
                         ACT_ENTER_PHONE, HELM_KIND_GO);
        }
        break;

    /* 「手机蓝牙」：最多 3 台已绑定；未满可再开配对窗；长按某台 → 解绑该台。 */
    case HELM_SCR_PHONE:
        {
            unsigned i;
            unsigned nphone = myvendor_devctl_pair_phone_count();
            const uint32_t left_ms = myvendor_devctl_pair_window_left_ms();
            const unsigned left_s  = (left_ms + 999u) / 1000u;
            char title[12];
            char addr[24];

            s_phone_shown_n   = nphone;
            s_phone_shown_sec = left_s;

            for (i = 0; i < nphone && i < 3u; i++)
              {
                lv_snprintf(title, sizeof(title), "手机%u", i + 1u);
                if (myvendor_devctl_pair_phone_at(i, addr, sizeof(addr)) != 1)
                  {
                    addr[0] = '?';
                    addr[1] = '\0';
                  }

                helm_row_set(m, n++, HELM_ICO_BLE, title, addr,
                             myvendor_sys_phone_ble_connected() ? "已连接" : "已绑定",
                             ACT_NOP, HELM_KIND_GO);
              }

            if (nphone < 3u)
              {
                if (left_s > 0u)
                  {
                    char secs[12];

                    lv_snprintf(secs, sizeof(secs), "%us", left_s);
                    helm_row_set(m, n++, HELM_ICO_BLE, "配对中",
                                 "手机可搜索到码表", secs, ACT_NOP,
                                 HELM_KIND_GO);
                  }
                else
                  {
                    char cap[16];

                    lv_snprintf(cap, sizeof(cap), "%u/3", nphone);
                    helm_row_set(m, n++, HELM_ICO_BLE, "配对新手机",
                                 "按勾号开始配对", cap, ACT_PAIR_OPEN,
                                 HELM_KIND_SLIM);
                  }
              }
            else
              {
                helm_row_set(m, n++, HELM_ICO_BLE, "已满", "手机已满 3",
                             "3/3", ACT_NOP, HELM_KIND_GO);
              }
        }
        break;

    case HELM_SCR_SCAN:
        empty = helm_fill_scan_recs(m, &n);
        break;

    case HELM_SCR_SCAN_PICK:
        empty = helm_fill_scan_pick(m, &n);
        break;

    case HELM_SCR_RIDES:
        helm_grp_begin(m, n, "最近");
        empty = helm_fill_gpx_dir(m, &n, MYVENDOR_GPX_RECORD_DIR, ACT_RIDE_OPEN);
        if (!empty) {
            /* 设计稿 07 页底部那一组：清除（不可恢复）—— 走确认 dock。 */
            helm_grp_begin(m, n, "清除");
            helm_row_set(m, n++, HELM_ICO_NONE, "清空记录", "不可恢复", "",
                         ACT_RIDES_CLEAR_ALL, HELM_KIND_GO);
        }
        break;

    case HELM_SCR_GPX_IMPORT:
        /* 导入列表：列 /mnt/lfs/mtp/import 下的文件。
         * ⚠ 这两个 case 是**我在 8f76db85（页表重写）里误删的**：那次我用正则
         * "从 case HELM_SCR_GPX 一直吞到 case HELM_SCR_NAVPTS" 替换，而 GPX 与
         * NAVPTS 之间正好夹着 GPX_IMPORT / GPX_RECORD 两个屏 ⇒ 它们的填充分支被
         * 一起吞掉，页面只剩页头与空态 ⇒ 进去永远空白（用户 2026-09-26 报）。
         * 现在按原样补回（动作与原版一致：都是 ACT_RIDE_OPEN）。 */
        empty = helm_fill_gpx_dir(m, &n, MYVENDOR_GPX_IMPORT_DIR, ACT_RIDE_OPEN);
        break;

    case HELM_SCR_GPX_RECORD:
        /* 记录列表：与「骑行记录」同源（同一目录、同一个打开动作）。 */
        empty = helm_fill_gpx_dir(m, &n, MYVENDOR_GPX_RECORD_DIR, ACT_RIDE_OPEN);
        break;

    case HELM_SCR_RIDE_DETAIL:
        if (s_list_enter_dir > 0) {
            uint8_t * rsel = helm_cur_sel(m);

            if (rsel) {
                *rsel = 0;
            }
        }
        /* 顺序：导航放"继续骑行"前面（用户 2026-09-25 定）—— 进这一屏默认停在第一行，
         * 于是回车默认动作也跟着变成"导航"。 */
        helm_row_set(m, n++, HELM_ICO_NAV, "导航", "", "",
                     ACT_RIDE_NAV, HELM_KIND_SLIM);
        helm_row_set(m, n++, HELM_ICO_PLAY, "继续骑行", "", "",
                     ACT_RIDE_CONT, HELM_KIND_SLIM);
        helm_row_set(m, n++, HELM_ICO_REV, "返航", "", "",
                     ACT_RIDE_REV, HELM_KIND_SLIM);
        helm_row_set(m, n++, HELM_ICO_XMARK, "删除", "", "",
                     ACT_RIDE_DEL, HELM_KIND_SLIM);
        break;

    case HELM_SCR_INBOX:
        empty = helm_fill_inbox(m, &n);
        break;

    case HELM_SCR_SETTINGS:
        n = helm_fill_table(m, n, HELM_PTAB(P_SETTINGS));
        break;

    case HELM_SCR_COLORLAB:
        {
            /* 候选配色**直接画在这块屏上**（用户 2026-09-26："实在不行，在设置界面
             * 增加一个颜色预览页面"）—— 半反屏的洗色很特别，靠照片来回猜代价太高。
             * 色块用行内进度条承载（100% 填满 = 一整条实色带），右侧印 RRGGBB。
             * 选色口径：翻到这页记下编号，改 helm_palette.h 里对应的那个宏。 */
            static const struct {
                const char * name;
                uint32_t color;
            } sw[] = {
                /* 名字只用点阵字库有的字：橙/青/深/浅/墨/色/级/场/纸/层 都不在
                 * 覆盖里（实测，见 zcode/analysis/font_glyph_scan.py）—— 缺字是
                 * **整块不画**，行首那个色字会变成空白。所以候选色只用编号，
                 * 颜色本身整行铺出来看，右值给 hex。 */
                { "选中 1",   0x2A6ED0 },   /* 现用（TIME 蓝） */
                { "选中 2",   0xFF6E12 },   /* NAV 橙 */
                { "选中 3",   0x1D4ED8 },   /* 亮蓝 */
                { "选中 4",   0x086163 },   /* 暗青 */
                { "选中 5",   0x000000 },   /* 黑 */
                { "灰 1",     0x6E655C },   /* 现用（MUTE，5.45:1） */
                { "灰 2",     0x8E8E93 },   /* App 原值，板上偏淡 */
                { "灰 3",     0x4A443C },
                { "卡 PAPER", 0xFBF6EE },
                { "底 SCR",   0xE4DCD0 },
                { "线 HAIR",  0xD4C8B8 },
                { "轨道 TRACK", 0xE2DACE }
            };
            uint8_t i;

            for (i = 0; i < (uint8_t)(sizeof(sw) / sizeof(sw[0])); i++) {
                char hexbuf[8];

                lv_snprintf(hexbuf, sizeof(hexbuf), "%06lX",
                            (unsigned long)sw[i].color);
                helm_row_set(m, n, HELM_ICO_BOLT, sw[i].name,
                             (i < 4) ? "就是选中时的样子" :
                             ((i == 4) ? "黑字看不出，不用" : ""),
                             hexbuf, ACT_NOP, HELM_KIND_VAL);
                /* 直接给这一行的进度条上色（不走 helm_sys_row_bar 的档位阈值，
                 * 否则红/绿会被"占用率"改写，色卡就不是原色了）。 */
                if (i < 5) {
                    /* 前 5 条 = 选中候选：**整行铺满该色**，标题/副标题照常压在上面
                     * （就是选中时的真实观感；光画一条色带看不出效果 —— 用户指出过）。 */
                    m->items[n].bg_set = true;
                    m->items[n].bg_color = sw[i].color;
                    /* 深底 ⇒ 副标题/右值用浅色（就是选中时的样子：灰字反色）。 */
                    m->items[n].bg_dark = true;
                } else {
                    m->items[n].progress_on = true;
                    m->items[n].progress = 100u;
                    m->items[n].progress_color = sw[i].color;
                }
                n++;
            }

            /* 整行铺色由这个回调画（见 helm_colorlab_draw 的说明）。 */
            helm_colorlab_attach(m->list);
        }
        break;

    case HELM_SCR_TEST:
        helm_row_set(m, n++, HELM_ICO_STAR, "字体试验", "对比", "",
                     ACT_ENTER_FONTLAB, HELM_KIND_GO);
        helm_row_set(m, n++, HELM_ICO_BOLT, "COLOR 对比", "屏上直接看", "",
                     ACT_ENTER_COLORLAB, HELM_KIND_GO);
        {
            bool rmc = bicycle_runtime_gnss_solver_rmc();

            helm_row_set(m, n++, HELM_ICO_GPS, "GPS解算",
                         rmc ? "完全信 RMC，不过滤" : "自定义滤波",
                         rmc ? "RMC" : "自定义",
                         ACT_CYCLE_GNSS_SOLVER, HELM_KIND_VAL);
        }
        break;

    case HELM_SCR_EPH:
        helm_eph_fill_page(m, &n);
        break;

    case HELM_SCR_TOOLS:
        n = helm_fill_table(m, n, HELM_PTAB(P_TOOLS));
        break;

    case HELM_SCR_SYSSTAT:
        {
            uint64_t disk_free = 0;
            uint64_t disk_total = 0;
            uint64_t heap_used = 0;
            uint64_t heap_total = 0;
            uint8_t disk_n = 0;
            uint8_t heap_n = 0;
            uint8_t i;
            uint8_t pct;
            char size[16];
            char sub[40];
            char val[16];

            {
                uint32_t t_sr = lv_tick_get();

                helm_sys_refresh_disks(false);
                helm_sys_refresh_heaps(false);
                helm_sys_refresh_thread_count(false);
                s_ui_sysref_ms = lv_tick_elaps(t_sr);
            }

            for (i = 0; i < HELM_SYS_DISK_N; i++) {
                if (s_sys_disks[i].valid) {
                    disk_free += s_sys_disks[i].free;
                    disk_total += s_sys_disks[i].total;
                    disk_n++;
                }
            }

            helm_sys_fmt_size(size, sizeof(size), disk_free);
            lv_snprintf(sub, sizeof(sub), "%u 个卷 · 可用 %s",
                        (unsigned)disk_n, size);
            pct = helm_sys_pct(disk_total - disk_free, disk_total);
            lv_snprintf(val, sizeof(val), "%u%%", (unsigned)pct);
            helm_grp_begin(m, n, "存储");
        helm_row_set(m, n++, HELM_ICO_SAVE, "磁盘", sub, val,
                         ACT_ENTER_SYS_DISKS, HELM_KIND_GO);
            helm_sys_row_bar(m, (uint8_t)(n - 1u), pct);

            for (i = 0; i < HELM_SYS_HEAP_N; i++) {
                if (s_sys_heaps[i].valid) {
                    heap_used += s_sys_heaps[i].used;
                    heap_total += s_sys_heaps[i].total;
                    heap_n++;
                }
            }

            helm_sys_fmt_size(size, sizeof(size),
                              heap_total > heap_used ?
                              heap_total - heap_used : 0);
            lv_snprintf(sub, sizeof(sub), "%u 个堆 · 可用 %s",
                        (unsigned)heap_n, size);
            pct = helm_sys_pct(heap_used, heap_total);
            lv_snprintf(val, sizeof(val), "%u%%", (unsigned)pct);
            helm_row_set(m, n++, HELM_ICO_CHIP, "内存", sub, val,
                         ACT_ENTER_SYS_MEMORY, HELM_KIND_GO);
            helm_sys_row_bar(m, (uint8_t)(n - 1u), pct);

            lv_snprintf(sub, sizeof(sub), "%u 个运行线程",
                        (unsigned)s_sys_thread_n);
            pct = helm_sys_cpu_used();
            {
                uint32_t mhz = helm_sys_cpu_mhz();

                if (mhz > 0) {
                    lv_snprintf(val, sizeof(val), "%uMHz %u%%",
                                (unsigned)mhz, (unsigned)pct);
                } else {
                    lv_snprintf(val, sizeof(val), "CPU %u%%", (unsigned)pct);
                }
            }
            helm_grp_begin(m, n, "运行");
        helm_row_set(m, n++, HELM_ICO_BOLT, "线程", sub, val,
                         ACT_ENTER_SYS_THREADS, HELM_KIND_GO);
            helm_sys_row_bar(m, (uint8_t)(n - 1u), pct);
        }

        {
            /* 系统资源 → 卫星：概览只给总数，按星座的细分在子页。 */
            myvendor_sys_gnss_t g;
            char sub_g[40];
            char val_g[16];
            unsigned view = 0;
            unsigned used = 0;
            unsigned path = 0;
            uint8_t ci;

            if (!myvendor_sys_onboard_gnss_get(&g)) {
                memset(&g, 0, sizeof(g));
            }

            for (ci = 0; ci < MYVENDOR_SYS_GNSS_CONST_N; ci++) {
                view += (unsigned)g.sats_in_view_c[ci];
                used += (unsigned)g.sats_locked_c[ci];
                if (g.sats_in_view_c[ci] > 0u || g.sats_locked_c[ci] > 0u) {
                    path++;
                }
            }

            lv_snprintf(sub_g, sizeof(sub_g), "在视 %u · 参与定位 %u 颗",
                        view, used);
            lv_snprintf(val_g, sizeof(val_g), "%u 路", path);
            helm_row_set(m, n++, HELM_ICO_BOLT, "卫星", sub_g, val_g,
                         ACT_ENTER_SYS_GNSS, HELM_KIND_GO);
        }
        break;

    case HELM_SCR_SYS_DISKS:
        helm_grp_begin(m, n, "存储卷");
        {
            uint8_t i;

            helm_sys_refresh_disks(false);
            for (i = 0; i < HELM_SYS_DISK_N; i++) {
                char sub[40];
                char val[16];

                if (s_sys_disks[i].valid) {
                    char free[16];
                    char total[16];

                    helm_sys_fmt_size(free, sizeof(free), s_sys_disks[i].free);
                    helm_sys_fmt_size(total, sizeof(total),
                                      s_sys_disks[i].total);
                    lv_snprintf(sub, sizeof(sub), "可用 %s · 共 %s",
                                free, total);
                    lv_snprintf(val, sizeof(val), "%u%%",
                                (unsigned)helm_sys_pct(s_sys_disks[i].used,
                                                       s_sys_disks[i].total));
                } else {
                    lv_snprintf(sub, sizeof(sub), "%s", "未挂载");
                    lv_snprintf(val, sizeof(val), "--");
                }

                helm_row_set(m, n++, s_sys_disk_icon[i], s_sys_disk_name[i],
                             sub, val, ACT_NOP, HELM_KIND_VAL);
                if (s_sys_disks[i].valid) {
                    helm_sys_row_free_bar(
                        m, (uint8_t)(n - 1u),
                        helm_sys_pct(s_sys_disks[i].used,
                                     s_sys_disks[i].total));
                } else {
                    helm_sys_row_free_bar(m, (uint8_t)(n - 1u), 0);
                }
            }
        }
        break;

    case HELM_SCR_SYS_MEMORY:
        helm_grp_begin(m, n, "内存");
        {
            uint8_t i;

            helm_sys_refresh_heaps(false);
            for (i = 0; i < HELM_SYS_HEAP_N; i++) {
                char sub[40];
                char val[16];

                if (!s_sys_heaps[i].valid) {
                    continue;
                }

                helm_sys_fmt_pair(sub, sizeof(sub), s_sys_heaps[i].used,
                                  s_sys_heaps[i].total);
                lv_snprintf(val, sizeof(val), "%u%%",
                            (unsigned)helm_sys_pct(s_sys_heaps[i].used,
                                                   s_sys_heaps[i].total));
                helm_row_set(m, n++, HELM_ICO_CHIP, s_sys_heap_name[i],
                             sub, val, ACT_NOP, HELM_KIND_VAL);
                helm_sys_row_free_bar(m, (uint8_t)(n - 1u),
                                      helm_sys_pct(s_sys_heaps[i].used,
                                                   s_sys_heaps[i].total));
            }
        }
        empty = n == 0u;
        break;

    case HELM_SCR_SYS_THREADS:
        helm_grp_begin(m, n, "线程");
        {
            uint8_t i;

            helm_sys_refresh_threads(false);
            for (i = 0; i < s_sys_threads.count && n < HELM_MENU_ROWS; i++) {
                const helm_sys_thread_t * th = &s_sys_threads.thread[i];
                char sub[40];
                char val[24];
                char used[12];
                char total[12];

                helm_sys_fmt_size(used, sizeof(used), th->stack_used);
                helm_sys_fmt_size(total, sizeof(total), th->stack_total);
#ifdef CONFIG_STACK_COLORATION
                lv_snprintf(sub, sizeof(sub), "PID %d · 栈 %s/%s",
                            (int)th->pid, used, total);
#else
                lv_snprintf(sub, sizeof(sub), "PID %d · 栈 --/%s",
                            (int)th->pid, total);
#endif
#ifndef CONFIG_SCHED_CPULOAD_NONE
                if (th->cpu_valid) {
                    lv_snprintf(val, sizeof(val), "CPU %u.%u%%",
                                (unsigned)(th->cpu_permille / 10u),
                                (unsigned)(th->cpu_permille % 10u));
                } else {
                    lv_snprintf(val, sizeof(val), "CPU --");
                }
#else
                lv_snprintf(val, sizeof(val), "CPU --");
#endif
                helm_row_set(m, n++, HELM_ICO_BOLT,
                             th->name[0] ? th->name : "无名线程",
                             sub, val, ACT_NOP, HELM_KIND_VAL);
                helm_sys_row_free_bar(
                    m, (uint8_t)(n - 1u),
                    th->stack_total > 0u ?
                    helm_sys_pct(th->stack_used, th->stack_total) : 0);
            }
        }
        empty = n == 0u;
        break;

    case HELM_SCR_SYS_GNSS:
        {
            /* 按星座分组：在视（GSV 总数）/ 锁定（GSV 里 SNR>0）/ 参与定位（GSA）。
             * 一行一个星座 —— 哪几路真在跑，看这一页比看 GSV 原文直观。 */
            static const char * const names[MYVENDOR_SYS_GNSS_CONST_N] = {
                "GPS", "GLONASS", "Galileo", "北斗", "QZSS"
            };
            /* 与 GPS/QZSS 同图标：这里是"卫星"语义，不放星座专属图标。 */
            static const uint8_t icons[MYVENDOR_SYS_GNSS_CONST_N] = {
                HELM_ICO_BOLT, HELM_ICO_BOLT, HELM_ICO_BOLT,
                HELM_ICO_BOLT, HELM_ICO_BOLT
            };
            myvendor_sys_gnss_t g;
            uint8_t i;

            if (!myvendor_sys_onboard_gnss_get(&g)) {
                memset(&g, 0, sizeof(g));
            }

            for (i = 0; i < MYVENDOR_SYS_GNSS_CONST_N; i++) {
                char sub[40];
                char val[16];
                unsigned view = (unsigned)g.sats_in_view_c[i];
                unsigned heard = (unsigned)g.sats_heard_c[i];
                unsigned used = (unsigned)g.sats_locked_c[i];
                bool supported = (g.mod_const_mask == 0u) ||
                                 ((g.mod_const_mask & (1u << i)) != 0u);

                if (!supported) {
                    /* 模组自己（MON-VER）就没列这一路：换天线也没用，
                     * 这条提示是给现场看的人一个结论，而不是"0 颗"。 */
                    lv_snprintf(sub, sizeof(sub), "模组不支持（ROM 未列）");
                    lv_snprintf(val, sizeof(val), "%s", "--");
                } else if (view == 0u && heard == 0u && used == 0u) {
                    /* 支持但没数据：可能是当前环境/未启用，标出来别当成 0 颗。 */
                    lv_snprintf(sub, sizeof(sub), "%s", "无数据（未启用/未收到）");
                    lv_snprintf(val, sizeof(val), "%s", "--");
                } else {
                    lv_snprintf(sub, sizeof(sub), "在视 %u · 锁定 %u",
                                view, heard);
                    lv_snprintf(val, sizeof(val), "%u颗", used);
                }

                helm_row_set(m, n++, icons[i], names[i], sub, val,
                             ACT_NOP, HELM_KIND_VAL);
                helm_sys_row_free_bar(
                    m, (uint8_t)(n - 1u),
                    view > 0u ? (heard * 100u) / view : 0u);
            }

            /* 模组身份一行：固件串 + ROM 里声明的星座列表。上一条"模组不支持"
             * 就是拿它判的 —— 现场不用再翻日志。 */
            if (n < HELM_MENU_ROWS) {
                char sub[48];

                if (g.mod_fw[0] != '\0' || g.mod_gnss[0] != '\0') {
                    lv_snprintf(sub, sizeof(sub), "%s",
                                g.mod_fw[0] ? g.mod_fw : "固件未知");
                    helm_row_set(m, n++, HELM_ICO_CHIP, "模组", sub,
                                 g.mod_gnss[0] ? g.mod_gnss : "--",
                                 ACT_NOP, HELM_KIND_VAL);
                }
            }
        }
        empty = n == 0u;
        break;

    default:
        break;
    }

    helm_grp_finish(m, n);
    m->count = n;
    return empty;
}

static const char * helm_scr_title(helm_scr_t scr)
{
    switch (scr) {
    case HELM_SCR_NAV:
        return "导航";
    case HELM_SCR_GPX:
        return "GPX导航";
    case HELM_SCR_GPX_IMPORT:
        return "导入";
    case HELM_SCR_GPX_RECORD:
        return "记录";
    case HELM_SCR_SENSORS:
        return "蓝牙设备";
    case HELM_SCR_RIDES:
        return "骑行记录";
    case HELM_SCR_RIDE_DETAIL:
        {
            static char title[HELM_GPX_NAME_MAX];
            char * dot;

            lv_snprintf(title, sizeof(title), "%s", s_ride_name);
            dot = strrchr(title, '.');
            if (dot != NULL) {
                *dot = '\0';
            }

            return title[0] != '\0' ? title : "记录";
        }
    case HELM_SCR_SETTINGS:
        return "设置";
    case HELM_SCR_TEST:
        return "测试配置";
    case HELM_SCR_EPH:
        return "星历";
    case HELM_SCR_GRADECAL:
        return "坡度校准";
    case HELM_SCR_ABOUT:
        return "固件";
    case HELM_SCR_FONTLAB:
        return "字体试验";
    case HELM_SCR_COLORLAB:
        return "COLOR 对比";
    case HELM_SCR_INBOX:
        return "最近通知";
    case HELM_SCR_NAVPTS:
        return "坐标点导航";
    case HELM_SCR_NAVPT_DETAIL:
        {
            static char title[80];

            if (s_navpt_title[0] == '\0') {
                return "途经点：长按右键";
            }

            lv_snprintf(title, sizeof(title), "%s：长按右键", s_navpt_title);
            return title;
        }
    case HELM_SCR_NAVPT_ACTIONS:
        return s_navpt_title[0] != '\0' ? s_navpt_title : "操作";
    case HELM_SCR_FAVS:
        return "常用点导航：长按右键";
    case HELM_SCR_SCAN:
        return helm_scan_anon(s_scan_kind);
    case HELM_SCR_PHONE:
        return "手机蓝牙";
    case HELM_SCR_SCAN_PICK:
        if (s_scan_link_wait) {
            return "正在连接";
        }

        if (s_scan_phase == HELM_SCAN_WAIT || s_scan_phase == HELM_SCAN_RUN) {
            return "扫描中";
        }

        return "选择设备";
    case HELM_SCR_TOOLS:
        return "工具箱";
    case HELM_SCR_TOOLFACE:
        return helm_toolbox_title_for(s_tool_id);
    case HELM_SCR_SYSSTAT:
        return "系统资源";
    case HELM_SCR_SYS_DISKS:
        return "磁盘";
    case HELM_SCR_SYS_MEMORY:
        return "内存堆";
    case HELM_SCR_SYS_THREADS:
        return "线程";
    case HELM_SCR_SYS_GNSS:
        return "卫星";
    default:
        return "菜单";
    }
}

static void helm_empty_for(helm_menu_t * m, helm_scr_t scr)
{
    if (m->empty == NULL) {
        return;
    }

    switch (scr) {
    case HELM_SCR_INBOX:
        helm_empty_set(m->empty, HELM_ICO_BELL, "没有通知",
                        "手机推送会出现在这里");
        break;
    case HELM_SCR_RIDES:
        helm_empty_set(m->empty, HELM_ICO_LIST, "暂无记录",
                        "骑行结束后会出现在这里");
        break;
    case HELM_SCR_GPX_IMPORT:
        /* 空列表直接告诉用户怎么放文件（用户 2026-09-26：别显示路径，提示用 App）。 */
        helm_empty_set(m->empty, HELM_ICO_GPX, "暂无导入",
                        "请用 App 导入");
        break;
    case HELM_SCR_GPX_RECORD:
        helm_empty_set(m->empty, HELM_ICO_LIST, "暂无记录",
                        "结束后会出现在这里");
        break;
    case HELM_SCR_NAVPTS:
        helm_empty_set(m->empty, HELM_ICO_NAV, "请用 App 同步",
                        "坐标点由手机下发");
        break;
    case HELM_SCR_NAVPT_DETAIL:
    case HELM_SCR_NAVPT_ACTIONS:
        helm_empty_set(m->empty, HELM_ICO_NAV, "没有途经点",
                        "请在 App 编辑后重新同步");
        break;
    case HELM_SCR_FAVS:
        helm_empty_set(m->empty, HELM_ICO_STAR, "暂无常用点",
                        "请用 App 同步");
        break;
    case HELM_SCR_SCAN:
        helm_empty_set(m->empty, HELM_ICO_BLE, "暂无设备",
                        "点扫描查找附近设备");
        break;
    case HELM_SCR_SCAN_PICK:
        helm_scan_tick();
        if (!myvendor_devctl_sensor_get()) {
            helm_empty_set(m->empty, HELM_ICO_BLE, "外设蓝牙未开",
                          "请在设置中打开外设蓝牙");
        } else if (s_scan_link_wait) {
            helm_empty_set(m->empty, HELM_ICO_BLE, "正在连接",
                          "连接成功后返回");
        } else if (s_scan_phase == HELM_SCAN_WAIT ||
                   s_scan_phase == HELM_SCAN_RUN) {
            helm_empty_set(m->empty, HELM_ICO_BLE, "正在扫描",
                          "发现后右键连接");
        } else {
            helm_empty_set(m->empty, HELM_ICO_BLE, "未发现设备",
                          "返回后重新扫描");
        }
        break;
    default:
        helm_empty_set(m->empty, HELM_ICO_LIST, "空", "");
        break;
    }
}

static bool helm_fav_plan_overlay_open(const helm_menu_t * m);

static void helm_menu_enter_vis(helm_menu_t * m)
{
    lv_obj_t * vis = NULL;

    if (m == NULL || s_list_enter_dir == 0) {
        return;
    }

    if (m->list && !lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->list;
    } else if (m->empty && !lv_obj_has_flag(m->empty, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->empty;
    } else if (m->about && !lv_obj_has_flag(m->about, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->about;
    } else if (m->fontlab && !lv_obj_has_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->fontlab;
    } else if (m->gradecal && !lv_obj_has_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->gradecal;
    } else if (m->ride && !lv_obj_has_flag(m->ride, LV_OBJ_FLAG_HIDDEN)) {
        vis = m->ride;
    }

    if (vis) {
        helm_obj_enter(vis, s_list_enter_dir);
    }

    s_list_enter_dir = 0;
}

static void helm_menu_clear_motion(lv_obj_t * obj)
{
    if (obj == NULL) {
        return;
    }

    lv_anim_delete(obj, NULL);
    lv_obj_set_style_translate_x(obj, 0, 0);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
}

static bool helm_row_stays(const helm_row_t * r)
{
    if (r == NULL) {
        return false;
    }

    if (r->kind == HELM_KIND_SW || r->kind == HELM_KIND_VAL) {
        return true;
    }

    switch (r->act) {
    case ACT_SCAN_AUTO:
    case ACT_SCAN_CONNECT:
    case ACT_NAV_STOP:
    case ACT_NAV_SKIP:
    case ACT_NAV_NEAREST:
    case ACT_RIDES_CLEAR_ALL:

    case ACT_RIDE_DEL:
    case ACT_INBOX_OPEN:
        return true;
    default:
        return false;
    }
}

static lv_obj_t * helm_menu_active_list(helm_menu_t * m)
{
    if (m == NULL) {
        return NULL;
    }

    if (m->ride_keys && m->ride &&
        !lv_obj_has_flag(m->ride, LV_OBJ_FLAG_HIDDEN)) {
        return m->ride_keys;
    }

    if (m->list && !lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN)) {
        return m->list;
    }

    return NULL;
}

static lv_obj_t * helm_menu_sel_row(helm_menu_t * m)
{
    uint8_t * sel;
    lv_obj_t * list;

    sel = helm_cur_sel(m);
    list = helm_menu_active_list(m);
    if (sel == NULL || list == NULL || *sel >= lv_obj_get_child_count(list)) {
        return NULL;
    }

    return lv_obj_get_child(list, *sel);
}

static void helm_menu_squash_drop_obj(void)
{
    if (s_squash) {
        helm_obj_squash_set(s_squash, false);
        s_squash = NULL;
    }
}

static void helm_menu_squash_abort(void)
{
    helm_menu_squash_drop_obj();
    s_squash_restore = false;
}

static void helm_menu_spec_from_row(const helm_menu_t * m, uint8_t i,
                                    const uint8_t * sel,
                                    const lv_font_t * notif_font,
                                    helm_item_t * spec)
{
    memset(spec, 0, sizeof(*spec));
    spec->ico = m->items[i].ico;
    spec->title = m->items[i].label;
    spec->sub = m->items[i].sub;
    spec->value = m->items[i].value;
    spec->kind = m->items[i].kind;
    spec->on = m->items[i].on;
    spec->sel = (sel && *sel == i);
    spec->progress_on = m->items[i].progress_on;
    spec->progress = m->items[i].progress;
    spec->progress_color = m->items[i].progress_color;
    spec->bg_set = m->items[i].bg_set;
    spec->bg_color = m->items[i].bg_color;
    spec->bg_dark = m->items[i].bg_dark;
    /* 外来文字（手机通知正文 / 轨迹文件名 / 蓝牙设备名）必须走 FreeType：
     * 点阵字库只覆盖 582 个码位（mism4 ∪ 苹方回退，2026-10-01 实测），用户自己的
     * 文件名、任意 App 的通知正文必然缺字，而本工程开了 LV_USE_FONT_PLACEHOLDER
     * ⇒ 缺字画的是**方块**（现场读作"乱码"）。这里按"这行的数据是不是外来"判定，
     * 而不是只看屏幕 —— 因为实时重绘那条路径（helm_sys_live_paint）不带屏幕信息，
     * 原先直接把 notif_font 传成 NULL，通知一更新、GPX 统计一落盘，字体就退回点阵、
     * 缺字又出现。
     * 用 helm_font_sys 而不是裸 TTF：TTF 还没预载完 / 工厂固件时自动退回点阵，
     * 而点阵链尾现在也挂着 TTF（见 helm_font.c），两条路都不会再出方块。 */
    if (notif_font == NULL &&
        (m->items[i].act == ACT_INBOX_OPEN || m->items[i].act == ACT_RIDE_OPEN ||
         m->items[i].act == ACT_SCAN_CONNECT ||
         m->items[i].act == ACT_SCAN_AUTO)) {
        notif_font = helm_font_sys(15, helm_font_title());
    }
    spec->title_font = notif_font;
}

static void helm_menu_squash_after_paint(helm_menu_t * m)
{
    lv_obj_t * row;

    s_squash = NULL;
    if (!s_squash_restore) {
        return;
    }

    s_squash_restore = false;
    row = helm_menu_sel_row(m);
    if (row) {
        helm_obj_squash_set(row, true);
        helm_obj_squash(row, false);
    }
}

/* ============================ UI 慢帧探针 ============================
 * 用户 2026-09-26："很卡"。菜单侧原先没有耗时数据（`[vperf] frame` 只管地图渲染），
 * 所以加这条**只在超阈值时打印**的探针（这个仓库对降噪的既定做法：超长即报）：
 *
 *   [vperf] ui slow menu 31ms rows=8 rebuild=1 paints=412 rebuilds=3
 *
 * 判读：
 *   · rebuild=1        ⇒ 这一帧又整表重建了（列表复用失效，见 helm_menu_paint）
 *   · paints 涨、rebuilds 不涨 ⇒ 慢在**绘制**，与重建无关 ——
 *                        那就该去算 PSRAM 写的账（帧缓存 write-through ≈10 MB/s）
 *   · 基线：一帧 60Hz=16ms / 30Hz=33ms，所以阈值取 25ms。
 */
#define HELM_UI_SLOW_MS 25
static uint32_t s_ui_paints;
static uint32_t s_ui_rebuilds;
static uint32_t s_ui_reb_clean;      /* 重建：lv_obj_clean */
static uint32_t s_ui_reb_rows;       /* 重建：建行循环 */
static uint32_t s_ui_reb_grp;        /* 重建：分组布局（margin/标题） */
static uint32_t s_ui_reb_sel;        /* 重建：建完后的滚动 + 落光标 */
static uint32_t s_ui_fill_ms;        /* helm_fill_items（每行的动态取值） */
static uint8_t s_ui_reuse_bust;      /* 复用在第几行断掉（1 起；0 = 没断） */
static bool s_ui_last_rebuild;
static uint32_t s_ui_slow_gate;

static void helm_ui_slow_report(uint32_t ms, bool rebuild)
{
    uint32_t now = lv_tick_get();

    if (ms < HELM_UI_SLOW_MS) {
        return;
    }

    if (s_ui_slow_gate != 0 && lv_tick_elaps(s_ui_slow_gate) < 1000u) {
        return;                        /* 限频：最多 1 条/秒，别把日志刷满 */
    }

    s_ui_slow_gate = now;
    syslog(LOG_WARNING,
           "[vperf] ui slow menu %ums rows=%u rebuild=%u paints=%u rebuilds=%u | "
           "fill=%ums (max src=%u %ums sysref=%ums) clean=%ums build=%ums grp=%ums sel=%ums bust=%u\n",
           (unsigned)ms, (unsigned)(s_menu ? s_menu->count : 0),
           rebuild ? 1u : 0u, (unsigned)s_ui_paints, (unsigned)s_ui_rebuilds,
           (unsigned)s_ui_fill_ms, (unsigned)s_ui_val_max_src,
           (unsigned)s_ui_val_max_ms, (unsigned)s_ui_sysref_ms,
           (unsigned)s_ui_reb_clean, (unsigned)s_ui_reb_rows,
           (unsigned)s_ui_reb_grp, (unsigned)s_ui_reb_sel,
           (unsigned)s_ui_reuse_bust);
}

/* ===================== LVGL 打桩（一次性挂在 display 上） =====================
 * 用户 2026-09-26："对 lvgl 怎么各种打桩验证吧"。这一套覆盖四件事，全部
 * **只在有活动时每 5 秒汇总一行**（不逐帧打，免得把日志刷满）：
 *
 *   [vperf] ui 5s renders=42 slow=7 render=310ms flush=180ms
 *                 | fill_sq=96 fill_round=58 label=210 img=4 other=12
 *
 *  · renders/slow/render  —— 每帧渲染耗时（含慢帧数）。慢帧多 ⇒ 绘制本身贵。
 *  · flush               —— **屏刷写的耗时**（LCDC/DMA 那一段）。它大而 render 小
 *                           ⇒ 卡在屏，不在绘制，该去查帧缓存内存与总线。
 *  · fill_sq / fill_round —— 填充任务按"方角 / 圆角"分开数。这条最值钱：
 *                            lv_draw_sifli_epic.c 里 radius != 0 会被 EPIC **拒绝**
 *                            （原文 "EPIC doesn't support rounded corners"），
 *                            所以 fill_round 就是"被踢回 CPU 光栅化"的量；
 *                            这个数一涨、render 跟着涨，就是 GPU 没吃上。
 *  · label/img/other      —— 文字/图片任务数（EPIC 有 label 与 img 通道）。
 *
 * 另外开机打一行帧缓存身份（在哪块内存、多大），见 helm_lvgl_probe_attach。
 * 判读地址：0x2000_xxxx 片上 SRAM（非缓存但快）；0x6000_xxxx PSRAM SBUS
 * （**write-through**，写惩罚实测 ~10 MB/s）；0x1000_xxxx PSRAM CBUS（可缓存）。
 */
typedef struct {
    uint32_t renders;
    uint32_t slow;
    uint32_t render_ms;
    uint32_t flush_ms;
    uint32_t fill_sq;
    uint32_t fill_round;
    uint32_t label;
    uint32_t img;
    uint32_t other;
    uint32_t t_render;
    uint32_t t_flush;
    uint32_t t_window;
} helm_lvgl_probe_t;

static helm_lvgl_probe_t s_probe;

void helm_ui_task_cb(lv_event_t * e)
{
    lv_draw_task_t * t = (lv_draw_task_t *)lv_event_get_param(e);

    if (t == NULL) {
        return;
    }

    switch (t->type) {
    case LV_DRAW_TASK_TYPE_FILL:
        {
            const lv_draw_fill_dsc_t * d = (const lv_draw_fill_dsc_t *)t->draw_dsc;

            if (d != NULL && d->radius != 0) {
                s_probe.fill_round++;     /* EPIC 不吃 ⇒ 落 CPU */
            } else {
                s_probe.fill_sq++;        /* 方角 ⇒ 能走 GPU */
            }
        }
        break;

    case LV_DRAW_TASK_TYPE_LABEL:
        s_probe.label++;
        break;

    case LV_DRAW_TASK_TYPE_IMAGE:
        s_probe.img++;
        break;

    default:
        s_probe.other++;
        break;
    }
}

static void helm_lvgl_probe_cb(lv_event_t * e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:
        s_probe.t_render = lv_tick_get();
        break;

    case LV_EVENT_RENDER_READY:
        {
            uint32_t ms = lv_tick_elaps(s_probe.t_render);

            s_probe.renders++;
            s_probe.render_ms += ms;
            if (ms >= HELM_UI_SLOW_MS) {
                s_probe.slow++;
            }
        }
        break;

    case LV_EVENT_FLUSH_START:
        s_probe.t_flush = lv_tick_get();
        break;

    case LV_EVENT_FLUSH_FINISH:
        s_probe.flush_ms += lv_tick_elaps(s_probe.t_flush);
        break;

    default:
        break;
    }
}

/** @brief 挂桩：display 四件事 + 菜单列表的绘制任务事件 + 帧缓存身份那一行。 */
static void helm_lvgl_probe_attach(helm_menu_t * m)
{
    static bool done;
    lv_display_t * d;
    lv_draw_buf_t * b;
    uintptr_t p;

    if (done) {
        return;
    }

    d = lv_display_get_default();
    if (d == NULL) {
        return;
    }

    done = true;
    lv_display_add_event_cb(d, helm_lvgl_probe_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(d, helm_lvgl_probe_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(d, helm_lvgl_probe_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(d, helm_lvgl_probe_cb, LV_EVENT_FLUSH_FINISH, NULL);
    if (m != NULL && m->list != NULL) {
        /* 只有带这个标志的对象才会发 DRAW_TASK_ADDED（菜单的填充/文字都在这个列表里）。 */
        lv_obj_add_flag(m->list, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(m->list, helm_ui_task_cb,
                            LV_EVENT_DRAW_TASK_ADDED, NULL);
    }

    s_probe.t_window = lv_tick_get();

    b = lv_display_get_buf_active(d);
    if (b != NULL && b->data != NULL) {
        p = (uintptr_t)b->data;
        syslog(LOG_WARNING, "[vperf] ui buf %p size=%u %s\n", (void *)b->data,
               (unsigned)lv_display_get_draw_buf_size(d),
               (p >= 0x60000000u && p < 0x70000000u) ? "PSRAM-WT(慢)" :
               ((p >= 0x20000000u && p < 0x30000000u) ? "SRAM(快)" :
                ((p >= 0x10000000u && p < 0x20000000u) ? "PSRAM-CBUS" : "其它")));
    } else {
        syslog(LOG_WARNING, "[vperf] ui buf: 取不到\n");
    }
}

/** @brief 每 5 秒汇总一行（这一窗口没活动就不打），然后清零窗口计数。 */
static void helm_lvgl_probe_summary(void)
{
    if (s_probe.t_window != 0 &&
        lv_tick_elaps(s_probe.t_window) < 5000u) {
        return;
    }

    s_probe.t_window = lv_tick_get();
    if (s_probe.renders == 0 && s_probe.fill_sq == 0 &&
        s_probe.fill_round == 0) {
        return;
    }

    syslog(LOG_WARNING,
           "[vperf] ui 5s renders=%u slow=%u render=%ums flush=%ums | "
           "fill_sq=%u fill_round=%u label=%u img=%u other=%u\n",
           (unsigned)s_probe.renders, (unsigned)s_probe.slow,
           (unsigned)s_probe.render_ms, (unsigned)s_probe.flush_ms,
           (unsigned)s_probe.fill_sq, (unsigned)s_probe.fill_round,
           (unsigned)s_probe.label, (unsigned)s_probe.img,
           (unsigned)s_probe.other);

    s_probe.renders = 0;
    s_probe.slow = 0;
    s_probe.render_ms = 0;
    s_probe.flush_ms = 0;
    s_probe.fill_sq = 0;
    s_probe.fill_round = 0;
    s_probe.label = 0;
    s_probe.img = 0;
    s_probe.other = 0;
}

static void helm_menu_paint(helm_menu_t * m)
{
    uint32_t t0 = lv_tick_get();

    helm_lvgl_probe_attach(m);
    helm_lvgl_probe_summary();
    helm_scr_t scr;
    uint8_t * sel;
    uint8_t i;
    bool about;
    bool empty;

    if (m == NULL || m->list == NULL || m->title == NULL) {
        return;
    }

    helm_menu_squash_drop_obj();
    helm_menu_clear_motion(m->list);
    helm_menu_clear_motion(m->empty);
    helm_menu_clear_motion(m->about);
    helm_menu_clear_motion(m->fontlab);
    helm_menu_clear_motion(m->gradecal);
    helm_menu_clear_motion(m->ride);
    helm_menu_clear_motion(m->toolface);

    scr = helm_cur_scr(m);
    about = (scr == HELM_SCR_ABOUT);
    if (m->fav_planning) {
        helm_mhead_set(m->title, "正在规划…");
    } else if (m->fav_plan_fail) {
        helm_mhead_set(m->title, "规划失败");
    } else {
        helm_mhead_set(m->title, helm_scr_title(scr));
        /* 这三页的页头就是用户文件名的本身（`helm_scr_title()` 里去掉扩展名，
         * 见 NAVPT_DETAIL 的拼法），按外来文字走系统 TTF；`helm_mhead_set()`
         * 每次都会把字体压回点阵，所以这句必须排在它后面。 */
        if (scr == HELM_SCR_RIDE_DETAIL || scr == HELM_SCR_NAVPT_DETAIL ||
            scr == HELM_SCR_NAVPT_ACTIONS) {
            helm_mhead_set_font(m->title, helm_font_sys(15, helm_font_title()));
        }
    }
    {
        /* 四段只覆盖"重建"，这一帧的总时长减去四段还有一大截 —— 那段就在取值里。 */
        uint32_t tf = lv_tick_get();

        s_ui_val_max_ms = 0;
        s_ui_val_max_src = 0;
        s_ui_sysref_ms = 0;
        empty = helm_fill_items(m);
        s_ui_fill_ms = lv_tick_elaps(tf);
    }
    if (scr == HELM_SCR_SYSSTAT || scr == HELM_SCR_SYS_DISKS ||
        scr == HELM_SCR_SYS_MEMORY || scr == HELM_SCR_SYS_THREADS ||
        scr == HELM_SCR_SYS_GNSS) {
        s_sys_ui_sig = helm_sys_items_sig(m);
    }

    if (m->skip_dock) {
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
    }

    /* 确认弹窗（`del_dock`）：**背景重画不许把它吃掉**（2026-09-25 修）。
     *
     * 用户现场："BLE 传感器『删除配对』的确认弹窗总是自己消失，很难删除"。
     * 机制：本函数被**背景刷新**反复调用 —— `helm_poll_cb`（400 ms）一发现
     * `helm_sensor_sig()` 变了（RSSI / 链路状态 / last-seen，传感器一有 notify
     * 就变）就整页重画；SCAN 页的扫描倒计时（`helm_scan_tick`）也走这里。
     * 而原来这句是**无条件**藏的 ⇒ 弹窗下一拍（≤400 ms）就消失。
     *
     * 判据是"弹出来时在哪一页"（`s_del_scr`，见 `helm_show_del()`）：
     *   · 同一页 ⇒ **保持可见**并抬到最前（重画会改变子对象顺序，不抬会被压住）；
     *   · 真的换了页 ⇒ 收起（那是 `helm_push`/`helm_back`，本来也会先清 arm）。
     * 三个弹窗主人（GPX 记录 / 手机配对解绑 / BLE 传感器记录）一并生效 ——
     * 尤其 GPX 那条**不使用 arm 标志**（"可见"本身就是它的"已装备"），
     * 所以这里不能按 arm 判、只能按页判。 */
    if (m->del_dock) {
        if (!lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN) && s_del_scr == scr) {
            lv_obj_move_foreground(m->del_dock);
        } else {
            lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (about || scr == HELM_SCR_FONTLAB || scr == HELM_SCR_GRADECAL
        || scr == HELM_SCR_RIDE_DETAIL || scr == HELM_SCR_TOOLFACE) {
        lv_obj_add_flag(m->list, LV_OBJ_FLAG_HIDDEN);
        if (m->empty) {
            lv_obj_add_flag(m->empty, LV_OBJ_FLAG_HIDDEN);
        }

        if (m->about) {
            if (about) {
                lv_obj_clear_flag(m->about, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(m->about, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (m->fontlab) {
            if (scr == HELM_SCR_FONTLAB) {
                lv_obj_clear_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN);
                helm_font_lab_refresh();
            } else {
                lv_obj_add_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (m->gradecal) {
            if (scr == HELM_SCR_GRADECAL) {
                lv_obj_clear_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
                helm_grade_refresh(m);
            } else {
                lv_obj_add_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (m->ride) {
            if (scr == HELM_SCR_RIDE_DETAIL) {
                uint8_t * rsel = helm_cur_sel(m);

                if (rsel && m->count > 0 && *rsel >= m->count) {
                    *rsel = 0;
                }
                lv_obj_clear_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
                helm_ride_refresh(m);
            } else {
                lv_obj_add_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
                helm_mlist_cursor_off(m->ride_keys);
            }
        }

        if (m->toolface) {
            if (scr == HELM_SCR_TOOLFACE) {
                helm_toolbox_open(s_tool_id);
                helm_mhead_set(m->title, helm_toolbox_title());
            } else {
                helm_toolbox_hide();
            }
        }

        s_sensor_sig = helm_sensor_sig();
        if (m->poll) {
            if (scr == HELM_SCR_GRADECAL) {
                lv_timer_resume(m->poll);
            } else {
                lv_timer_pause(m->poll);
            }
        }

        helm_menu_squash_after_paint(m);
        helm_menu_enter_vis(m);
        return;
    }

    if (m->about) {
        lv_obj_add_flag(m->about, LV_OBJ_FLAG_HIDDEN);
    }

    if (m->fontlab) {
        lv_obj_add_flag(m->fontlab, LV_OBJ_FLAG_HIDDEN);
    }

    if (m->gradecal) {
        lv_obj_add_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
    }

    if (m->ride) {
        lv_obj_add_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
        helm_mlist_cursor_off(m->ride_keys);
    }

    helm_toolbox_hide();

    if (empty) {
        lv_obj_add_flag(m->list, LV_OBJ_FLAG_HIDDEN);
        if (m->empty) {
            helm_empty_for(m, scr);
            lv_obj_clear_flag(m->empty, LV_OBJ_FLAG_HIDDEN);
        }

        s_sensor_sig = helm_sensor_sig();
        if (m->poll) {
            if (helm_scr_needs_sensor_poll(scr)) {
                lv_timer_resume(m->poll);
            } else {
                lv_timer_pause(m->poll);
            }
        }

        helm_menu_squash_after_paint(m);
        helm_menu_enter_vis(m);
        return;
    }

    if (m->empty) {
        lv_obj_add_flag(m->empty, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(m->list, LV_OBJ_FLAG_HIDDEN);
    sel = helm_cur_sel(m);
    if (sel && m->count > 0 && *sel >= m->count) {
        *sel = 0;
    }

    if (scr == HELM_SCR_SCAN_PICK && s_scan_jump && sel != NULL) {
        uint8_t pick;

        for (pick = 0; pick < m->count; pick++) {
            if (m->items[pick].act == ACT_SCAN_CONNECT &&
                m->items[pick].extra != 0) {
                *sel = pick;
                s_scan_jump = false;
                break;
            }
        }
    }

    {
        const lv_font_t * notif_font = NULL;
        bool reuse = false;

        /* 这些页的行文字全是**外来串**：轨迹文件名（导入/记录/最近）、
         * 手机通知正文、坐标点与收藏的名称、以及蓝牙设备名（扫描/配对）。
         * 点阵库只有 582 个字，用户自己的名字必然落在库外，而缺字在
         * LV_USE_FONT_PLACEHOLDER 下画的是方块 ⇒ 整行走系统 TTF
         * （未就绪/工厂固件时 helm_font_sys 自动退回点阵，链尾还有一层保底）。 */
        if (scr == HELM_SCR_INBOX || scr == HELM_SCR_GPX_IMPORT ||
            scr == HELM_SCR_GPX_RECORD || scr == HELM_SCR_RIDES ||
            scr == HELM_SCR_NAVPTS ||
            scr == HELM_SCR_NAVPT_DETAIL ||
            scr == HELM_SCR_FAVS ||
            scr == HELM_SCR_SCAN || scr == HELM_SCR_SCAN_PICK ||
            scr == HELM_SCR_PHONE) {
            notif_font = helm_font_sys(15, helm_font_title());
        }

        if (m->count > 0 && lv_obj_get_child_count(m->list) == m->count) {
            reuse = true;
            s_ui_reuse_bust = 0;
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;
                lv_obj_t * row = lv_obj_get_child(m->list, i);

                helm_menu_spec_from_row(m, i, sel, notif_font, &spec);
                if (!helm_mitem_refresh(row, &spec)) {
                    reuse = false;
                    s_ui_reuse_bust = (uint8_t)(i + 1u);
                    break;
                }
            }
        }

        s_ui_last_rebuild = !reuse;
        if (!reuse) {
            /* 重建 200~650 ms 太贵，分段量出来看是谁吃掉的（现场数据见 [vperf] ui slow menu）。 */
            uint32_t tb = lv_tick_get();
            uint32_t t_clean;
            uint32_t t_rows;

            s_ui_rebuilds++;
            lv_obj_clean(m->list);
            t_clean = lv_tick_elaps(tb);
            for (i = 0; i < m->count; i++) {
                helm_item_t spec;

                helm_menu_spec_from_row(m, i, sel, notif_font, &spec);
                helm_mitem_create(m->list, &spec);
            }

            uint32_t t_grp;

            t_rows = lv_tick_elaps(tb) - t_clean;
            helm_grp_layout(m);
            t_grp = lv_tick_elaps(tb) - t_clean - t_rows;
            helm_mitem_list_built(m->list, (sel != NULL) ? (int)*sel : -1);
            s_ui_reb_clean = t_clean;
            s_ui_reb_rows = t_rows;
            s_ui_reb_grp = t_grp;
            s_ui_reb_sel = lv_tick_elaps(tb) - t_clean - t_rows - t_grp;
        }

        if (sel && m->count > 0) {
            uint8_t idx = *sel;

            if (idx >= m->count) {
                idx = 0;
                *sel = 0;
            }
            lv_obj_update_layout(m->list);
            helm_mlist_sel_snap(m->list, idx);
        }
    }

    s_sensor_sig = helm_sensor_sig();
    if (m->poll) {
        if (helm_scr_needs_sensor_poll(scr)) {
            lv_timer_resume(m->poll);
        } else {
            lv_timer_pause(m->poll);
        }
    }

    helm_menu_squash_after_paint(m);
    helm_menu_enter_vis(m);

    s_ui_paints++;
    helm_ui_slow_report(lv_tick_elaps(t0), s_ui_last_rebuild);
}

/** @brief 换主题后用：重设"建页时烘进样式"的那几处，再重建行。
 *
 *  @details 菜单页的**行**是 `helm_menu_paint()` 每帧重建的 ⇒ 换主题后行会自己变色，
 *           但下面这几个对象只在 `helm_ui_ensure()` 里建一次，颜色烘死了：
 *           页根（`helm_style_scr` + MENU_BG）、列表底（`helm_style_scr`）、
 *           页头（`helm_mhead_create`）、屏级底色。所以要把这几处**再跑一遍**。
 *
 *  @note 骑行那几屏不在菜单体系里，走 `helm_shell_rebuild()`（见 helm_palette.c）。
 *  @note ⚠ 只处理"菜单自己的壳"。子页（关于/字体/海拔校准/工具箱）的颜色也是建页时
 *        烘的，这里**没管** —— 那几屏要各自重建才吃得上新色，属已知遗留。 */
/** @brief 重设"建页时烘进样式"的那几处**样式**（不动行、不重画）。
 *  @details 抽出来是为了让**按键回调能在同一帧里**把背景和行一起换掉：用户 2026-09-27
 *           现场"切换有些慢，特别是菜单的背景色" —— 因为背景原来只在帧后的补跑里换，
 *           于是"行先变、底后变"。 */
void helm_menu_theme_restyle(void)
{
    helm_menu_t * m = s_menu;
    lv_obj_t * scr;

    if (m == NULL) {
        return;
    }

    if (m->root != NULL) {
        helm_style_scr(m->root);
        /* 与 helm_ui_ensure() 同序：SCR 之后才盖 MENU_BG（菜单类页面统一深色表层）。 */
        lv_obj_set_style_bg_color(m->root, helm_color(HELM_COLOR_MENU_BG), 0);
    }

    if (m->list != NULL) {
        /* ⚠ 只改颜色，**不要**再跑一遍 `helm_style_scr()`：那个函数还会把 pad 归零，
         * 而 `helm_mlist_create()` 是在它**之后**才设 pad_all=6 / pad_row=4 的 ⇒
         * 重跑一遍会把行贴到屏幕边上（布局被改，不只是颜色）。 */
        lv_obj_set_style_bg_color(m->list, helm_color(HELM_COLOR_SCR), 0);
    }

    if (m->empty != NULL) {
        /* 空态卡片（`helm_empty_create` 建在 m->empty 下）的底色/图标色/文字色也都是
         * 建时烘的，而 `helm_empty_set()` 的复用分支只改文案和图标 ⇒ 换主题后它会一直
         * 停在旧配色（夜间"暂无记录"还是米白卡）。交给既有重建路径最省事：清掉子对象，
         * 由 `helm_menu_paint()` 在空态时重建。 */
        lv_obj_clean(m->empty);
    }

    if (m->title != NULL) {
        helm_mhead_reapply(m->title);
    }

    scr = lv_screen_active();
    if (scr != NULL) {
        lv_obj_set_style_bg_color(scr, helm_color(HELM_COLOR_SCR), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    }
}

void helm_menu_theme_reapply(void)
{
    if (s_menu == NULL) {
        return;                      /* 菜单页还没建过：开机头几帧，无处可施 */
    }

    helm_menu_theme_restyle();
    helm_menu_paint(s_menu);         /* 行/空态/子页显隐都按新表重建 */
}

static void helm_scan_pick_finish(helm_menu_t * m)
{
    s_scan_link_wait = false;
    helm_scan_idle_ui();
    myvendor_sound_ok();
    lv_pm_notify_show("蓝牙设备", "已连接", 1500);
    if (m != NULL && m->sp > 1 && helm_cur_scr(m) == HELM_SCR_SCAN_PICK) {
        m->sp--;
        s_list_enter_dir = -1;
    }

    helm_menu_paint(m);
}

static uint32_t helm_sys_items_sig(const helm_menu_t * m)
{
    uint32_t s = 2166136261u;
    uint8_t i;

    if (m == NULL) {
        return 0;
    }

    s ^= m->count;
    s *= 16777619u;
    for (i = 0; i < m->count; i++) {
        const helm_row_t * r = &m->items[i];
        const char * p;

        s ^= r->progress;
        s *= 16777619u;
        s ^= r->progress_color;
        s *= 16777619u;
        s ^= r->bg_color;
        s *= 16777619u;
        for (p = r->value; *p != '\0'; p++) {
            s ^= (uint8_t)*p;
            s *= 16777619u;
        }

        for (p = r->sub; *p != '\0'; p++) {
            s ^= (uint8_t)*p;
            s *= 16777619u;
        }
    }

    return s;
}

static bool helm_sys_live_paint(helm_menu_t * m)
{
    uint8_t * sel;
    uint8_t i;
    uint32_t sig;

    if (m == NULL || m->list == NULL ||
        lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_child_count(m->list) != m->count) {
        return false;
    }

    sig = helm_sys_items_sig(m);
    if (sig == s_sys_ui_sig) {
        return true;
    }

    sel = helm_cur_sel(m);
    for (i = 0; i < m->count; i++) {
        helm_item_t spec;
        lv_obj_t * row = lv_obj_get_child(m->list, i);

        helm_menu_spec_from_row(m, i, sel, NULL, &spec);
        spec.live = true;
        if (!helm_mitem_refresh(row, &spec)) {
            return false;
        }
    }

    s_sys_ui_sig = sig;
    return true;
}

static bool helm_sys_poll_paint(helm_menu_t * m)
{
    if (m == NULL) {
        return false;
    }

    (void)helm_fill_items(m);
    return helm_sys_live_paint(m);
}

static void helm_poll_cb(lv_timer_t * t)
{
    helm_menu_t * m = s_menu;
    myvendor_sys_sensor_ui_t ui;
    static uint32_t sys_ui_tick;
    helm_scr_t scr;

    LV_UNUSED(t);
    if (m == NULL) {
        return;
    }

    scr = helm_cur_scr(m);
    helm_scan_tick();
    if (s_scan_link_wait && scr == HELM_SCR_SCAN_PICK &&
        s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N) {
        myvendor_sys_sensor_ui_get(&ui);
        if (ui.slot[s_scan_kind].link == MYVENDOR_SYS_SENSOR_LINK_READY) {
            helm_scan_pick_finish(m);
            return;
        }
    }
    if (scr == HELM_SCR_GRADECAL) {
        helm_grade_refresh(m);
        return;
    }

    if (scr == HELM_SCR_SYSSTAT || scr == HELM_SCR_SYS_DISKS ||
        scr == HELM_SCR_SYS_MEMORY || scr == HELM_SCR_SYS_THREADS ||
        scr == HELM_SCR_SYS_GNSS) {
        uint32_t wait = 1000u;

        if (scr == HELM_SCR_SYS_DISKS) {
            wait = 2000u;
        } else if (scr == HELM_SCR_SYS_MEMORY) {
            wait = 1500u;
        } else if (scr == HELM_SCR_SYS_THREADS) {
            wait = 1200u;
        }

        if (lv_tick_elaps(sys_ui_tick) >= wait) {
            sys_ui_tick = lv_tick_get();
            if (!helm_sys_poll_paint(m)) {
                helm_menu_paint(m);
            }
        }

        return;
    }

    if (scr == HELM_SCR_EPH) {
        static uint32_t s_eph_sig;
        uint32_t last = 0;
        uint32_t next = 0;
        uint32_t sig;

        (void)myvendor_gnss_eph_times(&last, &next);
        sig = last ^ next ^ (uint32_t)(time(NULL) / 60) ^
              (myvendor_devctl_eph_auto_get() ? 1u : 2u);
        if (sig == s_eph_sig) {
            return;
        }

        s_eph_sig = sig;
        helm_menu_paint(m);
        return;
    }

    if (helm_sensor_sig() == s_sensor_sig) {
        return;
    }

    helm_menu_paint(m);
}

static void helm_fav_plan_timer_cb(lv_timer_t * timer);
static void helm_fav_plan_on_input(helm_menu_t * m);

static bool helm_fav_plan_overlay_open(const helm_menu_t * m)
{
    return m != NULL && (m->fav_planning || m->fav_plan_fail);
}

static void helm_fav_plan_raise_chrome(void)
{
    lv_obj_t * chrome = lv_pm_status_bar_cont();

    if (chrome != NULL && !lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(chrome);
    }
}

static void helm_fav_plan_place_mask(helm_menu_t * m)
{
    if (!m || !m->plan_mask) {
        return;
    }
    lv_obj_set_size(m->plan_mask, PAGE_HOR_RES, HELM_PAGE_H - HELM_MHEAD_H);
    lv_obj_align(m->plan_mask, LV_ALIGN_TOP_LEFT, 0, HELM_MHEAD_H);
}

static void helm_fav_plan_mask_clicked(lv_event_t * e)
{
    helm_fav_plan_on_input((helm_menu_t *)lv_event_get_user_data(e));
}

static void helm_fav_plan_hide(helm_menu_t * m)
{
    if (!m) {
        return;
    }
    if (m->fav_plan_timer) {
        lv_timer_delete(m->fav_plan_timer);
        m->fav_plan_timer = NULL;
    }
    m->fav_planning = false;
    m->fav_plan_fail = false;
    m->fav_plan_kick = false;
    m->fav_plan_phase = 0u;
    m->fav_notice_t0 = 0u;
    m->fav_notice_ms = 0u;
    if (m->plan_mask) {
        lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    }
    if (m->title && (helm_cur_scr(m) == HELM_SCR_FAVS ||
                     helm_cur_scr(m) == HELM_SCR_NAVPT_DETAIL ||
                     helm_cur_scr(m) == HELM_SCR_NAVPT_ACTIONS)) {
        helm_mhead_set(m->title, helm_scr_title(helm_cur_scr(m)));
    }
}

static void helm_fav_plan_present(helm_menu_t * m, const char * msg,
    const char * hint)
{
    if (!m || !m->plan_mask) {
        return;
    }
    if (m->plan_msg) {
        lv_label_set_text(m->plan_msg, msg ? msg : "");
    }
    if (m->plan_hint) {
        lv_label_set_text(m->plan_hint, hint ? hint : "");
    }
    if (m->plan_head) {
        lv_label_set_text(m->plan_head,
            m->fav_planning ? "正在规划" : "规划失败");
    }
    lv_obj_clear_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    helm_fav_plan_place_mask(m);
    lv_obj_move_foreground(m->plan_mask);
    if (m->title) {
        helm_mhead_set(m->title, m->fav_planning ? "正在规划…" :
            (msg ? msg : "规划失败"));
    }
    if (m->root) {
        lv_obj_update_layout(m->root);
    }
    helm_fav_plan_place_mask(m);
    if (m->plan_mask) {
        lv_obj_update_layout(m->plan_mask);
        lv_obj_move_foreground(m->plan_mask);
    }
    helm_fav_plan_raise_chrome();
}

static void helm_fav_plan_update(helm_menu_t * m)
{
    static const char spin[] = "|/-\\";
    char text[40];

    if (!m || !m->plan_msg) {
        return;
    }
    lv_snprintf(text, sizeof(text), "正在规划路线 %c",
        spin[m->fav_plan_phase & 3u]);
    m->fav_plan_phase++;
    lv_label_set_text(m->plan_msg, text);
}

static void helm_fav_notice(helm_menu_t * m, const char * text, uint32_t ms)
{
    if (!m || !m->title || !text) {
        return;
    }
    m->fav_planning = false;
    m->fav_plan_fail = false;
    m->fav_plan_kick = false;
    m->fav_notice_t0 = lv_tick_get();
    m->fav_notice_ms = ms;
    if (m->plan_mask) {
        lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    }
    helm_mhead_set(m->title, text);
    if (!m->fav_plan_timer) {
        m->fav_plan_timer = lv_timer_create(helm_fav_plan_timer_cb, 250u, m);
    }
}

static void helm_fav_plan_fail(helm_menu_t * m, const char * msg)
{
    if (!m) {
        return;
    }
    if (m->fav_plan_timer) {
        lv_timer_delete(m->fav_plan_timer);
        m->fav_plan_timer = NULL;
    }
    m->fav_planning = false;
    m->fav_plan_fail = true;
    m->fav_plan_kick = false;
    m->fav_notice_t0 = 0u;
    m->fav_notice_ms = 0u;
    helm_fav_plan_present(m, msg ? msg : "路线规划失败", "按键关闭");
    myvendor_sound_warn();
    lv_refr_now(NULL);
}

static void helm_fav_plan_do_submit(helm_menu_t * m)
{
    myvendor_devctl_favorite_t favs[MYVENDOR_DEVCTL_FAVORITE_MAX];
    vmap_route_waypoint_t wps[MYVENDOR_DEVCTL_FAVORITE_MAX];
    const char * names[MYVENDOR_DEVCTL_FAVORITE_MAX];
    size_t fav_n = 0;
    uint8_t i;

    bicycle_gnss_fix_t fix;

    if (!m || !m->fav_planning) {
        return;
    }
    if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
        helm_fav_plan_fail(m, "无定位，无法规划");
        return;
    }
    if (myvendor_devctl_waypoints_load(
            (s_navpt_path[0] != '\0') ? s_navpt_path :
            MYVENDOR_DEVCTL_FAVORITES_PATH,
            favs, MYVENDOR_DEVCTL_FAVORITE_MAX, &fav_n) != 0) {
        helm_fav_plan_fail(m, s_navpt_path[0] != '\0' ?
            "坐标点读取失败" : "常用点读取失败");
        return;
    }
    for (i = 0u; i < m->fav_order_n; i++) {
        const uint8_t idx = m->fav_order[i];

        if ((size_t)idx >= fav_n) {
            m->fav_order_n = 0u;
            helm_menu_paint(m);
            helm_fav_plan_fail(m, s_navpt_path[0] != '\0' ?
                "坐标点已变化，请重试" : "常用点已变化，请重试");
            return;
        }
        wps[i].lon = favs[idx].longitude;
        wps[i].lat = favs[idx].latitude;
        names[i] = favs[idx].name;
    }
    /* 站在某站上＝那一站已经过掉（trip 的语义是「当前坐标不算」），规划时会
     * 自动跳过它（见 map_page_nav_trip_plan 里的 skip）。所以要报「已在终点
     * 附近」，判据必须是**整条行程一站都还没到**，而不是只看第 1 个点 ——
     * 以前盯 wps[0]，于是站在第 1 个途经点上就误报「已在终点附近」。 */
    if (vmap_route_trip_first_unreached(wps, 0u, m->fav_order_n,
            (double)fix.longitude, (double)fix.latitude) >= m->fav_order_n) {
        helm_fav_plan_fail(m, "已在终点附近");
        return;
    }
    if (!map_page_nav_trip_plan(lvgl_page_map(), wps, m->fav_order_n)) {
        helm_fav_plan_fail(m, "无法开始规划");
        return;
    }
    map_page_nav_trip_set_names(lvgl_page_map(), names, m->fav_order_n);
    myvendor_sound_ok();
}

static void helm_fav_plan_timer_cb(lv_timer_t * timer)
{
    helm_menu_t * m = (helm_menu_t *)lv_timer_get_user_data(timer);
    map_page_t * map = lvgl_page_map();

    if (!m) {
        return;
    }
    if (m->fav_plan_kick) {
        m->fav_plan_kick = false;
        lv_timer_set_period(timer, 250u);
        helm_fav_plan_do_submit(m);
        return;
    }
    if (!m->fav_planning) {
        if (m->fav_notice_ms != 0u
            && lv_tick_elaps(m->fav_notice_t0) >= m->fav_notice_ms) {
            helm_fav_plan_hide(m);
        }
        return;
    }
    if (map_page_nav_active(map)) {
        helm_fav_plan_hide(m);
        (void)lv_pm_close_page_msg(NULL);
        return;
    }
    if (!map_page_nav_planning(map)) {
        helm_fav_plan_fail(m, "路线规划失败");
        return;
    }
    helm_fav_plan_update(m);
}

static void helm_fav_plan_show(helm_menu_t * m)
{
    bicycle_gnss_fix_t fix;

    if (!m) {
        return;
    }
    if (!bicycle_runtime_poll_fix(&fix) || !fix.valid) {
        helm_fav_plan_fail(m, "无定位，无法规划");
        return;
    }
    m->fav_planning = true;
    m->fav_plan_fail = false;
    m->fav_plan_kick = true;
    m->fav_plan_phase = 0u;
    m->fav_notice_t0 = 0u;
    m->fav_notice_ms = 0u;
    helm_fav_plan_present(m, "正在规划路线", "单击停止");
    helm_fav_plan_update(m);
    if (m->fav_plan_timer) {
        lv_timer_delete(m->fav_plan_timer);
        m->fav_plan_timer = NULL;
    }
    m->fav_plan_timer = lv_timer_create(helm_fav_plan_timer_cb, 30u, m);
    lv_refr_now(NULL);
}

static bool helm_navpt_prepare_order(helm_menu_t * m, bool reverse)
{
    myvendor_devctl_favorite_t pts[MYVENDOR_DEVCTL_FAVORITE_MAX];
    size_t pt_n = 0;
    uint8_t i;

    if (m == NULL || s_navpt_path[0] == '\0') {
        return false;
    }

    if (myvendor_devctl_waypoints_load(s_navpt_path, pts,
            MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0 ||
        pt_n == 0) {
        return false;
    }

    m->fav_order_n = (uint8_t)pt_n;
    for (i = 0u; i < m->fav_order_n; i++) {
        m->fav_order[i] = reverse ? (uint8_t)(m->fav_order_n - 1u - i) : i;
    }

    return true;
}

static void helm_navpt_start(helm_menu_t * m, bool reverse)
{
    if (helm_fav_plan_overlay_open(m)) {
        return;
    }

    if (!helm_navpt_prepare_order(m, reverse)) {
        myvendor_sound_warn();
        helm_fav_notice(m, "坐标点读取失败", 2000u);
        return;
    }

    helm_fav_plan_show(m);
}

static void helm_fav_plan_on_input(helm_menu_t * m)
{
    if (!m) {
        return;
    }
    if (m->fav_planning) {
        m->fav_planning = false;
        map_page_nav_stop(lvgl_page_map());
        helm_fav_plan_hide(m);
        myvendor_sound_back();
        return;
    }
    if (m->fav_plan_fail) {
        helm_fav_plan_hide(m);
        myvendor_sound_back();
    }
}

static void helm_push(helm_menu_t * m, helm_scr_t scr)
{
    if (m->sp >= HELM_MENU_STACK) {
        return;
    }

    m->stack[m->sp].scr = scr;
    m->stack[m->sp].sel = 0;
    m->sp++;
    s_list_enter_dir = 1;
    helm_menu_paint(m);
}

static void helm_back(helm_menu_t * m)
{
    if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
        s_sensor_del = false;
        return;
    }

    if (m->skip_dock && !lv_obj_has_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_SCAN_PICK) {
        helm_scan_end();
    }

    if (m->sp <= 1) {
        (void)lv_pm_close_page_msg(NULL);
        return;
    }

    m->sp--;
    s_list_enter_dir = -1;
    helm_menu_paint(m);
}

static void helm_wait_app(const char * title)
{
    myvendor_sound_warn();
    lv_pm_notify_show(title, "等待 App 下发", 2000);
}

static void helm_show_skip(helm_menu_t * m)
{
    if (m->skip_dock) {
        lv_obj_clear_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(m->skip_dock);
    }
}

static void helm_show_del(helm_menu_t * m)
{
    if (m == NULL || m->del_dock == NULL) {
        return;
    }

    myvendor_sound_warn();
    if (m->del_lab) {
        lv_label_set_text(m->del_lab, "删除此记录？");
    }

    /* 记下"哪一页弹的"：`helm_menu_paint()` 靠它区分"同页背景重画（不许吃弹窗）"
     * 与"真的换了页（该收起）"。见 `s_del_scr` 的注释。 */
    s_del_scr = helm_cur_scr(m);

    lv_obj_clear_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(m->del_dock);
}

/**
 * @brief 「手机蓝牙」页看门狗（1 s）：状态翻了才重画。
 *
 * 配对（记录被 companion 写出来）和解绑（记录被清掉）都是**异步**的：UI 只投命令，
 * 真正的读写发生在 companion 线程下一拍 —— 所以确认之后立刻重画读到的还是旧状态
 * （实测"解绑后仍显示已配对"）。这里只在**当前就在这一页、且状态真的和上一帧不同**
 * 时才重画：不每秒都画（免得干扰选中/开关动画），配对成功后也能自动变成"已配对"。
 */
void helm_phone_watch_cb(lv_timer_t * t)
{
    helm_menu_t * m = s_menu;

    LV_UNUSED(t);

    if (m == NULL || helm_cur_scr(m) != HELM_SCR_PHONE) {
        return;
    }

    {
        const unsigned left_s =
            (myvendor_devctl_pair_window_left_ms() + 999u) / 1000u;
        const unsigned n_now = myvendor_devctl_pair_phone_count();

        if (n_now != s_phone_shown_n || left_s != s_phone_shown_sec) {
            helm_menu_paint(s_menu);
        }
    }
}

static bool helm_ride_sel_bind(helm_menu_t * m)
{
    uint8_t * sel;
    uint8_t idx;

    if (m == NULL) {
        return false;
    }

    sel = helm_cur_sel(m);
    if (sel == NULL || *sel >= m->count) {
        return false;
    }

    if (m->items[*sel].act != ACT_RIDE_OPEN) {
        return false;
    }

    idx = m->items[*sel].extra;
    if (idx >= s_gpx_n) {
        return false;
    }

    return helm_ride_bind(s_gpx_names[idx], s_gpx_dir);
}

/** @brief 「清空记录」的执行体：把记录目录里的文件逐个删掉。
 *  @details 设计稿 07 页的「清除」组就这一行，且标着"不可恢复" ⇒ 必须走确认 dock
 *           （与删单条共用同一个 dock，用 s_clear_all_armed 区分是哪种删除）。 */
static void helm_rides_clear_all(helm_menu_t * m)
{
    uint8_t n;
    uint8_t i;

    if (m != NULL && m->del_dock != NULL) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    }

    n = helm_gpx_scan(MYVENDOR_GPX_RECORD_DIR);
    for (i = 0; i < n && i < HELM_GPX_LIST_MAX; i++) {
        char path[160];

        lv_snprintf(path, sizeof(path), "%s/%s", MYVENDOR_GPX_RECORD_DIR,
                    s_gpx_names[i]);
        (void)unlink(path);
    }

    helm_gpx_count_invalidate();
    myvendor_sound_ok();
    lv_pm_notify_show("清除", "记录已清空", 1500);

    if (m != NULL) {
        helm_menu_paint(m);
    }
}

static void helm_ride_do_del(helm_menu_t * m)
{
    char path[160];
    helm_scr_t scr;

    scr = (m != NULL) ? helm_cur_scr(m) : HELM_SCR_ROOT;
    if (m != NULL && m->del_dock != NULL) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    }

    if (scr == HELM_SCR_NAVPT_DETAIL || scr == HELM_SCR_NAVPT_ACTIONS) {
        if (s_navpt_path[0] == '\0' || unlink(s_navpt_path) != 0) {
            myvendor_sound_warn();
            lv_pm_notify_show("删除", "失败", 2000);
            return;
        }

        helm_navpt_clear_bind();
        myvendor_sound_ok();
        lv_pm_notify_show("删除", "已移除", 1500);
        if (m != NULL) {
            while (m->sp > 1) {
                helm_scr_t cur = m->stack[m->sp - 1u].scr;

                if (cur != HELM_SCR_NAVPT_DETAIL &&
                    cur != HELM_SCR_NAVPT_ACTIONS) {
                    break;
                }

                m->sp--;
            }

            s_list_enter_dir = -1;
            helm_menu_paint(m);
        }

        return;
    }

    if (!helm_ride_path(path, sizeof(path))) {
        return;
    }

    if (unlink(path) != 0) {
        myvendor_sound_warn();
        lv_pm_notify_show("删除", "失败", 2000);
        return;
    }

    helm_gpx_stat_forget(s_ride_name);
    s_ride_name[0] = '\0';
    s_ride_loaded[0] = '\0';
    s_ride_dir = NULL;
    s_ride_loaded_dir = NULL;
    s_ride_pt_n = 0;
    s_ride_km = 0.0;
    s_ride_sec = 0;
    s_ride_has_time = false;
    myvendor_sound_ok();
    lv_pm_notify_show("删除", "已移除", 1500);
    if (m == NULL) {
        return;
    }

    if (scr == HELM_SCR_RIDE_DETAIL) {
        helm_back(m);
    } else {
        helm_menu_paint(m);
    }
}

static void helm_sensor_do_del(helm_menu_t * m)
{
    if (m != NULL && m->del_dock != NULL) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_sensor_del) {
        return;
    }

    s_sensor_del = false;
    /* 与 arm 那条配对读：arm 与 confirm 的间隔 = 弹窗实际存活时间。 */
    printf("helm: sensor del confirmed idx=%u\n", (unsigned)s_sensor_del_idx);
    if (myvendor_devctl_sensor_delete(s_sensor_del_idx) != 0) {
        myvendor_sound_warn();
        lv_pm_notify_show("删除", "失败", 2000);
        return;
    }

    myvendor_sound_ok();
    lv_pm_notify_show("删除", "已移除", 1500);
    if (m != NULL) {
        helm_menu_paint(m);
    }
}

static void helm_do_act(helm_menu_t * m, uint8_t act)
{
    switch (act) {
    case ACT_ENTER_PHONE:
        /* 只进页面，**不开窗**：开配对必须由用户在页内点击 + 弹窗确认
         * （`开始配对？` → 确认；见点击路径里 HELM_SCR_PHONE 分支）。
         * 顺带清掉上次留下的"待确认"，免得一进来第一下点击就把旧确认吃掉了。 */
        s_phone_pair_arm = false;
        s_phone_unbind_arm = false;
        printf("helm: phone page enter\n");
        helm_push(m, HELM_SCR_PHONE);
        /* 看门狗：配对/解绑是异步的，状态翻了就重画（只在这一页生效）。 */
        if (s_phone_watch == NULL) {
            extern void helm_phone_watch_cb(lv_timer_t * t);

            s_phone_watch = lv_timer_create(helm_phone_watch_cb, 1000, NULL);
        }
        break;
    /* 「手机蓝牙」子界面里的动作（2026-09-20）。都在 companion 线程执行：
     * 开窗会把广播从"不广播/定向"切到可发现；解绑清密钥 + 清手机记录。 */
    case ACT_PAIR_OPEN:
        (void)myvendor_devctl_pair_open(30);
        lv_pm_notify_show("手机蓝牙", "已开配对 30 秒", 1500);
        break;
    case ACT_PAIR_UNBIND:
        (void)myvendor_devctl_pair_unbind();
        lv_pm_notify_show("手机蓝牙", "已解除绑定", 1500);
        break;
    case ACT_ENTER_NAV:
        helm_push(m, HELM_SCR_NAV);
        break;
    case ACT_ENTER_GPX:
        helm_push(m, HELM_SCR_GPX);
        break;
    case ACT_ENTER_SENSORS:
        helm_push(m, HELM_SCR_SENSORS);
        break;
    case ACT_ENTER_RIDES:
        helm_push(m, HELM_SCR_RIDES);
        break;
    case ACT_ENTER_SETTINGS:
        helm_push(m, HELM_SCR_SETTINGS);
        break;
    case ACT_ENTER_TEST:
        helm_push(m, HELM_SCR_TEST);
        break;
    case ACT_ENTER_EPH:
        helm_push(m, HELM_SCR_EPH);
        break;
    case ACT_ENTER_ABOUT:
        helm_push(m, HELM_SCR_ABOUT);
        break;
    case ACT_ENTER_COLORLAB:
        helm_push(m, HELM_SCR_COLORLAB);
        break;
    case ACT_ENTER_FONTLAB:
        helm_push(m, HELM_SCR_FONTLAB);
        break;
    case ACT_ENTER_GRADECAL:
        helm_push(m, HELM_SCR_GRADECAL);
        break;
    case ACT_ENTER_INBOX:
        helm_push(m, HELM_SCR_INBOX);
        break;
    case ACT_ENTER_NAVPTS:
        helm_navpt_clear_bind();
        helm_push(m, HELM_SCR_NAVPTS);
        break;
    case ACT_ENTER_NAVPT_REC:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;
            myvendor_devctl_favorite_t pts[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t pt_n = 0;
            char * dot;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (idx >= s_gpx_n) {
                break;
            }

            lv_snprintf(s_navpt_path, sizeof(s_navpt_path), "%s/%s",
                        MYVENDOR_NAVPTS_DIR, s_gpx_names[idx]);
            lv_snprintf(s_navpt_title, sizeof(s_navpt_title), "%s",
                        s_gpx_names[idx]);
            dot = strrchr(s_navpt_title, '.');
            if (dot != NULL) {
                *dot = '\0';
            }

            if (myvendor_devctl_waypoints_load(s_navpt_path, pts,
                    MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0 ||
                pt_n == 0) {
                myvendor_sound_warn();
                helm_navpt_clear_bind();
                break;
            }

            helm_push(m, HELM_SCR_NAVPT_DETAIL);
        }
        break;
    case ACT_ENTER_FAVS:
        helm_navpt_clear_bind();
        m->fav_order_n = 0u;
        helm_push(m, HELM_SCR_FAVS);
        break;
    case ACT_ENTER_TOOLS:
        helm_push(m, HELM_SCR_TOOLS);
        break;
    case ACT_ENTER_SYSSTAT:
        helm_push(m, HELM_SCR_SYSSTAT);
        break;
    case ACT_ENTER_SYS_DISKS:
        helm_sys_refresh_disks(true);
        helm_push(m, HELM_SCR_SYS_DISKS);
        break;
    case ACT_ENTER_SYS_MEMORY:
        helm_sys_refresh_heaps(true);
        helm_push(m, HELM_SCR_SYS_MEMORY);
        break;
    case ACT_ENTER_SYS_THREADS:
        helm_sys_refresh_threads(true);
        helm_push(m, HELM_SCR_SYS_THREADS);
        break;
    case ACT_ENTER_SYS_GNSS:
        helm_push(m, HELM_SCR_SYS_GNSS);
        break;
    case ACT_TOOL_COMPASS:
        s_tool_id = HELM_TOOL_COMPASS;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_TOOL_LEVEL:
        s_tool_id = HELM_TOOL_LEVEL;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_TOOL_GMETER:
        s_tool_id = HELM_TOOL_GMETER;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_TOOL_ALT:
        s_tool_id = HELM_TOOL_ALTIMETER;
        helm_push(m, HELM_SCR_TOOLFACE);
        break;
    case ACT_ENTER_SCAN_HR:
        s_scan_kind = 0;
        helm_push(m, HELM_SCR_SCAN);
        break;
    case ACT_ENTER_SCAN_CAD:
        s_scan_kind = 1;
        helm_push(m, HELM_SCR_SCAN);
        break;
    case ACT_ENTER_SCAN_PWR:
        s_scan_kind = 2;
        helm_push(m, HELM_SCR_SCAN);
        break;
    case ACT_NAV_COORDS:
        helm_wait_app("导航");
        break;
    case ACT_NAV_FAV:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;
            uint8_t i;

            if (sel == NULL || *sel >= m->count) {
                break;
            }
            idx = m->items[*sel].extra;
            for (i = 0u; i < m->fav_order_n; i++) {
                if (m->fav_order[i] == idx) {
                    memmove(&m->fav_order[i], &m->fav_order[i + 1u],
                        (size_t)(m->fav_order_n - i - 1u)
                            * sizeof(m->fav_order[0]));
                    m->fav_order_n--;
                    myvendor_sound_back();
                    helm_menu_paint(m);
                    return;
                }
            }
            if (m->fav_order_n >= MYVENDOR_DEVCTL_FAVORITE_MAX) {
                myvendor_sound_warn();
                break;
            }
            m->fav_order[m->fav_order_n++] = idx;
            myvendor_sound_ok();
            helm_menu_paint(m);
        }
        break;
    case ACT_NAV_FAV_START:
        {
            myvendor_devctl_favorite_t favs[MYVENDOR_DEVCTL_FAVORITE_MAX];
            size_t fav_n = 0;
            uint8_t i;

            if (helm_fav_plan_overlay_open(m)) {
                break;
            }
            if (m->fav_order_n == 0u) {
                myvendor_sound_warn();
                helm_fav_notice(m, s_navpt_path[0] != '\0' ?
                    "请先选择途经点" : "请先选择常用点", 1500u);
                break;
            }
            {
                const char * src = (s_navpt_path[0] != '\0') ?
                    s_navpt_path : MYVENDOR_DEVCTL_FAVORITES_PATH;
                size_t pt_n = 0;

                if (myvendor_devctl_waypoints_load(src, favs,
                        MYVENDOR_DEVCTL_FAVORITE_MAX, &pt_n) != 0) {
                    myvendor_sound_warn();
                    helm_fav_notice(m, s_navpt_path[0] != '\0' ?
                        "坐标点读取失败" : "常用点读取失败", 2000u);
                    break;
                }

                fav_n = pt_n;
            }
            for (i = 0u; i < m->fav_order_n; i++) {
                if ((size_t)m->fav_order[i] >= fav_n) {
                    myvendor_sound_warn();
                    m->fav_order_n = 0u;
                    helm_menu_paint(m);
                    helm_fav_notice(m, s_navpt_path[0] != '\0' ?
                        "坐标点已变化，请重试" : "常用点已变化，请重试", 2000u);
                    return;
                }
            }
            helm_fav_plan_show(m);
        }
        break;
    case ACT_ENTER_GPX_IMPORT:
        helm_push(m, HELM_SCR_GPX_IMPORT);
        break;
    case ACT_ENTER_GPX_RECORD:
        helm_push(m, HELM_SCR_GPX_RECORD);
        break;
    case ACT_RIDE_OPEN:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (idx >= s_gpx_n) {
                break;
            }

            if (!helm_ride_bind(s_gpx_names[idx], s_gpx_dir)) {
                break;
            }

            helm_push(m, HELM_SCR_RIDE_DETAIL);
        }
        break;
    case ACT_RIDE_DEL:
        helm_show_del(m);
        break;
    case ACT_RIDE_CONT:
        {
            char path[160];
            const bicycle_runtime_t * rt = bicycle_runtime_get();

            if (helm_shell_paused() || (rt && rt->recording) ||
                bicycle_ride_gpx_active()) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "请先结束当前骑行", 2000);
                break;
            }

            if (myvendor_mtp_lfs_quiesce()) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "USB 传输中", 2000);
                break;
            }

            if (!helm_ride_load() || s_ride_pt_n < 2u) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "无轨迹", 2000);
                break;
            }

            if (!helm_ride_path(path, sizeof(path))) {
                break;
            }

            if (bicycle_ride_gpx_prepare_continue(path) != 0) {
                myvendor_sound_warn();
                lv_pm_notify_show("骑行", "复制轨迹失败", 2000);
                break;
            }

            helm_shell_continue_ride((float)s_ride_km, s_ride_sec,
                                     s_ride_lon, s_ride_lat, s_ride_pt_n);
            helm_ride_close_map("骑行", "继续记录");
        }
        break;
    case ACT_RIDE_NAV:
    case ACT_RIDE_REV:
        if (helm_cur_scr(m) == HELM_SCR_NAVPT_DETAIL ||
            helm_cur_scr(m) == HELM_SCR_NAVPT_ACTIONS) {
            helm_navpt_start(m, act == ACT_RIDE_REV);
            break;
        }
        {
            char path[160];
            bool reverse = (act == ACT_RIDE_REV);

            if (!helm_ride_path(path, sizeof(path))) {
                break;
            }

            if (!map_page_nav_from_gpx(lvgl_page_map(), path, reverse)) {
                myvendor_sound_warn();
                lv_pm_notify_show(reverse ? "返航" : "导航", "打开失败", 2000);
                break;
            }

            helm_ride_close_map(reverse ? "返航" : "导航", s_ride_name);
        }
        break;
    case ACT_NAV_STOP:
#if VMAP_ROUTE_ENABLE
        {
            const bool review = map_page_review_active(lvgl_page_map());

            map_page_nav_stop(lvgl_page_map());
            myvendor_sound_back();
            lv_pm_notify_show(review ? "回放" : "导航",
                              review ? "已关闭" : "已停止", 1500);
            {
                uint8_t * nsel = helm_cur_sel(m);

                if (nsel) {
                    *nsel = 0;
                }
            }
            helm_menu_paint(m);
        }
#endif
        break;
    case ACT_NAV_SKIP:
        helm_show_skip(m);
        break;
    case ACT_NAV_NEAREST:
#if VMAP_ROUTE_ENABLE
        if (!map_page_nav_plan_nearest(lvgl_page_map())) {
            myvendor_sound_warn();
        } else {
            myvendor_sound_ok();
        }
#endif
        break;
    case ACT_TOGGLE_BT:
        (void)myvendor_devctl_radio_set(!myvendor_devctl_radio_get());
        helm_menu_paint(m);
        break;
    case ACT_DEMO_TOGGLE:
        if (bicycle_demo_active()) {
            bicycle_demo_stop();
            myvendor_sound_back();
            lv_pm_notify_show("演示模式", "已停止", 1500);
            helm_menu_paint(m);
        } else {
            int rc = bicycle_demo_start();

            if (rc != BICYCLE_DEMO_OK) {
                myvendor_sound_warn();
                lv_pm_notify_show("演示模式", bicycle_demo_error_text(rc), 2500);
                helm_menu_paint(m);
            } else {
                /* 演示要看地图 ⇒ 起完就收菜单（与"骑行记录 → 导航"同一个姿势）。
                 * ⚠ 收页之后**不能再** helm_menu_paint(m)：m 会跟着 page 一起释放。 */
                myvendor_sound_ok();
                lv_pm_notify_show("演示模式", "开始", 1500);
                (void)lv_pm_close_page_msg(NULL);
            }
        }
        break;
    case ACT_TOGGLE_SENSOR:
        (void)myvendor_devctl_sensor_set(!myvendor_devctl_sensor_get());
        helm_menu_paint(m);
        break;
    case ACT_CYCLE_BL:
        (void)helm_pwr_bl_cycle();
        helm_menu_paint(m);
        break;
    case ACT_CYCLE_THEME:
        {
            /* 用户 2026-09-27 要求："在这个 btn 上增加回调，打印我点下的模式"。
             * 每点一下打**一行摘要**：第几下、切前切后的主题、以及各段耗时 ——
             * 手感（"点一下像没反应"）在 `cycle=` 这一段里一眼可见。 */
            uint32_t t0 = lv_tick_get();
            const char * n0 = lv_pm_theme_name(lv_pm_theme_get());
            uint32_t id0 = (uint32_t)lv_pm_theme_get();
            uint32_t t_cycle;
            uint32_t t_all;

            /* 用户 2026-09-27："最开始设计是多种主题，但是现在只有两个" —— 说对了：
             * 注册表里有**三档**（classic / outdoor / night），而 `lvgl_page_theme_cycle()`
             * 走的是"下一档"三档循环；outdoor 和 night **都是深色**（同一套深色外观）
             * ⇒ 从夜间点一下只会走到另一个深色档（看着像没反应），点第二下才回日光。
             * 现在按用户实际的两套外观做**两档切换**：判据用 `helm_pal_night()`
             * （跟调色板同一个函数，不会再出现"文字/外观各说一套"）。 */
            (void)lvgl_page_theme_set_by_name(helm_pal_night() ? "classic" : "night");
            t_cycle = lv_tick_elaps(t0);
            /* 主题名变了 ⇒ 调色板跟着切（outdoor=夜间），再整屏失效让当前页重建。
             * 现存控件把颜色烘进了样式，所以必须重画而不是只 invalidate。 */
            helm_palette_follow_theme();
            /* 背景/页头/屏底**在同一帧里**跟着换（用户现场："特别是菜单的背景色"慢 ——
             * 就是因为它原来只在帧后的补跑里换）。然后再画行。 */
            helm_menu_theme_restyle();
            if (s_menu != NULL && s_menu->root != NULL) {
                lv_obj_invalidate(s_menu->root);
            }
            helm_menu_paint(m);
            t_all = lv_tick_elaps(t0);
            s_theme_clicks++;
            syslog(LOG_WARNING,
                   "[theme] click #%u %s(%u)->%s(%u) night=%d | cycle=%ums paint=%ums total=%ums",
                   (unsigned)s_theme_clicks, n0 ? n0 : "?", (unsigned)id0,
                   lv_pm_theme_name(lv_pm_theme_get()) ? lv_pm_theme_name(lv_pm_theme_get()) : "?",
                   (unsigned)lv_pm_theme_get(), (int)helm_pal_night(),
                   (unsigned)t_cycle, (unsigned)(t_all - t_cycle), (unsigned)t_all);
        }
        break;
    case ACT_CYCLE_UNIT:
        s_unit_imperial = !s_unit_imperial;
        helm_menu_paint(m);
        break;
    case ACT_CYCLE_TZ:
        {
            int16_t tzm = myvendor_devctl_tz_min_get();

            tzm = (int16_t)(tzm + 60);
            if (tzm > MYVENDOR_DEVCTL_TZ_MAX) {
                tzm = (int16_t)MYVENDOR_DEVCTL_TZ_MIN;
            }

            (void)myvendor_devctl_tz_min_set(tzm);
            bicycle_status_bar_refresh();
            helm_menu_paint(m);
        }
        break;
    case ACT_CYCLE_GNSS_SOLVER:
        bicycle_runtime_set_gnss_solver_rmc(!bicycle_runtime_gnss_solver_rmc());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_AUTOPAUSE:
        (void)myvendor_devctl_autopause_set(!myvendor_devctl_autopause_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_NOTIF:
        (void)myvendor_devctl_notif_set(!myvendor_devctl_notif_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_CALLS:
        (void)myvendor_devctl_notif_calls_only_set(
            !myvendor_devctl_notif_calls_only_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_USB:
        (void)myvendor_devctl_mtp_set(!myvendor_devctl_mtp_get());
        helm_menu_paint(m);
        break;
    case ACT_TOGGLE_SOUND:
        {
            bool on = !myvendor_devctl_sound_get();

            (void)myvendor_devctl_sound_set(on);
            if (on) {
                myvendor_sound_ok();
            }

            helm_menu_paint(m);
        }
        break;
    case ACT_EPH_WRITE:
        /* 手动写一次星历（自动那条在 ACT_TOGGLE_EPH_AUTO）；耗时长，异步在服务里做。 */
        helm_eph_times_invalidate();
        myvendor_gnss_eph_maintain();
        break;

    case ACT_TOGGLE_EPH_AUTO:
        {
            bool on = !myvendor_devctl_eph_auto_get();

            (void)myvendor_devctl_eph_auto_set(on);
            if (on) {
                myvendor_gnss_eph_reload();
            }

            helm_menu_paint(m);
        }
        break;
    case ACT_INBOX_OPEN:
        {
            myvendor_sys_notif_t box[MYVENDOR_SYS_INBOX_MAX];
            uint8_t nn = 0;
            uint8_t idx;
            uint8_t * sel = helm_cur_sel(m);

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            myvendor_sys_inbox_get(box, &nn, MYVENDOR_SYS_INBOX_MAX);
            if (idx >= nn) {
                break;
            }

            lv_pm_notify_show_ex(box[idx].title[0] ? box[idx].title : "通知",
                                 box[idx].body[0] ? box[idx].body :
                                 (box[idx].title[0] ? box[idx].title : "通知"),
                                 box[idx].icon[0] ? box[idx].icon : NULL,
                                 3000);
        }
        break;
    case ACT_SCAN_CONNECT:
        {
            uint8_t * sel = helm_cur_sel(m);
            uint8_t idx;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (s_scan_link_wait) {
                break;
            }

            if (!myvendor_devctl_sensor_get()) {
                myvendor_sound_warn();
                lv_pm_notify_show("蓝牙设备", "外设蓝牙未开", 1500);
                break;
            }

            helm_scan_connect_idx(idx);
            helm_menu_paint(m);
        }
        break;
    case ACT_SCAN_START:
        {
            myvendor_sys_sensor_ui_t ui;

            if (!myvendor_devctl_sensor_get()) {
                myvendor_sound_warn();
                lv_pm_notify_show("蓝牙设备", "外设蓝牙未开", 1500);
                break;
            }

            myvendor_sys_sensor_ui_get(&ui);
            if (s_scan_link_wait ||
                (s_scan_kind < MYVENDOR_SYS_SENSOR_KIND_N &&
                 ui.slot[s_scan_kind].link ==
                 MYVENDOR_SYS_SENSOR_LINK_CONNECTING)) {
                lv_pm_notify_show("蓝牙设备", "正在连接", 1500);
                break;
            }

            helm_scan_begin();
            helm_push(m, HELM_SCR_SCAN_PICK);
        }
        break;
    case ACT_SCAN_AUTO:
        {
            uint8_t * sel = helm_cur_sel(m);
            myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
            size_t rec_n = 0;
            uint8_t idx;
            bool on;

            if (sel == NULL || *sel >= m->count) {
                break;
            }

            idx = m->items[*sel].extra;
            if (myvendor_devctl_sensor_recs_get(recs,
                    MYVENDOR_DEVCTL_SENSOR_REC_MAX, &rec_n) != 0 ||
                idx >= rec_n) {
                break;
            }

            on = !recs[idx].autorc;
            if (myvendor_devctl_sensor_auto_set(idx, on) != 0) {
                myvendor_sound_warn();
                lv_pm_notify_show("蓝牙设备", "保存失败", 1500);
                break;
            }

            myvendor_sound_ok();
            lv_pm_notify_show("蓝牙设备", on ? "开机回连" : "已取消回连", 1500);
            helm_menu_paint(m);
        }
        break;
    case ACT_RIDES_CLEAR_ALL:
        /* 清空记录：先弹确认 dock（复用删单条那一个），确认后 helm_rides_clear_all。 */
        s_clear_all_armed = true;
        helm_show_del(m);
        if (m->del_lab != NULL) {
            lv_label_set_text(m->del_lab, "清空记录？");
        }
        break;

    case ACT_POWEROFF:
        helm_pwr_exec();
        break;
    default:
        break;
    }
}

static void helm_menu_step_sel(helm_menu_t * m, int8_t dir)
{
    uint8_t * sel;
    uint8_t old;
    helm_scr_t scr;

    if (m == NULL || dir == 0) {
        return;
    }

    if (helm_fill_items(m) || m->count == 0) {
        return;
    }

    sel = helm_cur_sel(m);
    if (sel == NULL || m->count <= 1u) {
        return;
    }

    old = *sel;
    if (dir > 0) {
        *sel = (uint8_t)((*sel + 1u) % m->count);
    } else {
        *sel = (*sel == 0u) ? (uint8_t)(m->count - 1u) :
               (uint8_t)(*sel - 1u);
    }

    scr = helm_cur_scr(m);
    if (scr == HELM_SCR_RIDE_DETAIL && m->ride_keys &&
        lv_obj_get_child_count(m->ride_keys) == m->count) {
        helm_mlist_move_sel(m->ride_keys, old, *sel);
        return;
    }

    if (m->list && !lv_obj_has_flag(m->list, LV_OBJ_FLAG_HIDDEN) &&
        lv_obj_get_child_count(m->list) == m->count) {
        helm_mlist_move_sel(m->list, old, *sel);
        return;
    }

    helm_menu_paint(m);
}

static void helm_on_step_item(void * ud, int dir)
{
    helm_menu_t * m = s_menu;

    LV_UNUSED(ud);
    helm_menu_squash_abort();
    if (m == NULL) {
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_TOOLFACE) {
        if (helm_toolbox_key1()) {
            helm_mhead_set(m->title, helm_toolbox_title());
        }

        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_ABOUT ||
        helm_cur_scr(m) == HELM_SCR_GRADECAL) {
        return;
    }
    if (helm_fav_plan_overlay_open(m)) {
        helm_fav_plan_on_input(m);
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_FONTLAB) {
        helm_font_lab_next();
        return;
    }

    if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
        s_sensor_del = false;
        myvendor_sound_back();
        return;
    }

    if (m->skip_dock && !lv_obj_has_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        myvendor_sound_back();
        return;
    }

    helm_menu_step_sel(m, dir);
}

/** @brief KEY1 短按：列表下一个聚焦。 */
static void helm_on_next_item(void * ud)
{
    helm_on_step_item(ud, 1);
}

/** @brief KEY1 双击：列表**向上**一个聚焦（用户 2026-09-27）。 */
static void helm_on_prev_item(void * ud)
{
    helm_on_step_item(ud, -1);
}

static void helm_on_confirm(void * ud)
{
    helm_menu_t * m = s_menu;
    uint8_t * sel;
    bool stays = false;

    LV_UNUSED(ud);
    if (m == NULL || helm_cur_scr(m) == HELM_SCR_ABOUT) {
        helm_menu_squash_abort();
        return;
    }

    /* **「手机蓝牙」页的确认**（KEY2 短按 = 这一步；"询问"在 helm_on_key2_long）。
     *
     * ⚠️ 必须放在**最前**：下面紧跟着的 `del_dock` 守卫会在弹窗可见时
     * `helm_ride_do_del()` / `helm_sensor_do_del()` 一嗓子就 `return` ——
     * 之前这段写在守卫后面，于是**弹窗上的 ✓ 被当成"删记录"的确认**，
     * 配对/解绑永远不生效（用户现场："长按弹窗后按 ✓ 没能启动广播"）。
     */
    if (helm_cur_scr(m) == HELM_SCR_PHONE) {
        if (s_phone_pair_arm) {
            s_phone_pair_arm = false;
            if (m->del_dock != NULL) {
                lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
            }

            printf("helm: phone pair confirmed -> open window\n");
            (void)myvendor_devctl_pair_open(30);
            myvendor_sound_ok();
            lv_pm_notify_show("手机蓝牙", "已开配对 30 秒", 1500);
            return;
        }

        if (s_phone_unbind_arm) {
            s_phone_unbind_arm = false;
            if (m->del_dock != NULL) {
                lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);
            }

            printf("helm: phone unbind confirmed slot=%u\n", s_phone_unbind_idx);
            (void)myvendor_devctl_pair_unbind_at(s_phone_unbind_idx);
            myvendor_sound_ok();
            lv_pm_notify_show("手机蓝牙", "已解除绑定", 1500);
            helm_menu_paint(m);
            return;
        }

        /* 选中已绑定行时短按：提示长按解绑。 */
        {
            uint8_t *sel = helm_cur_sel(m);
            unsigned nphone = myvendor_devctl_pair_phone_count();

            if (sel != NULL && *sel < nphone) {
                printf("helm: phone press while paired (long-press to unbind)\n");
                myvendor_sound_warn();
                lv_pm_notify_show("手机蓝牙", "长按右键可解绑", 1200);
                return;
            }
        }

        if (myvendor_devctl_pair_phone_count() >= 3u) {
            myvendor_sound_warn();
            lv_pm_notify_show("手机蓝牙", "手机已满 3", 1200);
            return;
        }

        /* 没满：按确认 = 想配对。 */
        printf("helm: phone pair ask\n");
        s_phone_pair_arm = true;
        helm_show_del(m);
        if (m->del_lab != NULL) {
            lv_label_set_text(m->del_lab, "开始配对？");
        }
        return;
    }
    if (helm_fav_plan_overlay_open(m)) {
        helm_menu_squash_abort();
        helm_fav_plan_on_input(m);
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_TOOLFACE) {
        helm_menu_squash_abort();
        if (helm_toolbox_key2()) {
            helm_mhead_set(m->title, helm_toolbox_title());
        }

        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_GRADECAL) {
        helm_menu_squash_abort();
        if (bicycle_env_calibrate()) {
            helm_grade_refresh(m);
            lv_pm_notify_show("坡度", "已归零", 1500);
        } else {
            myvendor_sound_warn();
            lv_pm_notify_show("坡度", "IMU 未就绪", 2000);
        }

        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_FONTLAB) {
        helm_menu_squash_abort();
        helm_font_lab_prev();
        return;
    }

    if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
        helm_menu_squash_abort();
        if (helm_cur_scr(m) == HELM_SCR_SCAN) {
            helm_sensor_do_del(m);
        } else if (s_clear_all_armed) {
            s_clear_all_armed = false;
            helm_rides_clear_all(m);
        } else {
            helm_ride_do_del(m);
        }

        return;
    }

    if (m->skip_dock && !lv_obj_has_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN)) {
        helm_menu_squash_abort();
        lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);
        lv_pm_notify_show("导航", "已跳过", 1500);
        return;
    }

    /* 坐标点详情：左右键都只翻点位，长按右键才出操作。 */
    if (helm_cur_scr(m) == HELM_SCR_NAVPT_DETAIL) {
        helm_menu_squash_abort();
        helm_menu_step_sel(m, -1);
        return;
    }

    if (helm_fill_items(m)) {
        helm_menu_squash_abort();
        return;
    }

    sel = helm_cur_sel(m);
    if (sel != NULL && m->count > 0 && *sel >= m->count) {
        *sel = 0;
    }

    if (sel == NULL || *sel >= m->count) {
        helm_menu_squash_abort();
        return;
    }

    if (helm_cur_scr(m) == HELM_SCR_SCAN_PICK && s_scan_jump) {
        uint8_t i;

        for (i = 0; i < m->count; i++) {
            if (m->items[i].act == ACT_SCAN_CONNECT &&
                m->items[i].extra != 0) {
                *sel = i;
                s_scan_jump = false;
                break;
            }
        }
    }

    stays = helm_row_stays(&m->items[*sel]);
    if (stays) {
        s_squash_restore = true;
        s_squash = helm_menu_sel_row(m);
    } else {
        helm_menu_squash_drop_obj();
        helm_mlist_press_sel(helm_menu_active_list(m), *sel);
    }

    helm_do_act(m, m->items[*sel].act);
    if (s_squash_restore) {
        if (s_squash) {
            helm_obj_squash_pulse(s_squash);
        }

        s_squash_restore = false;
        s_squash = NULL;
    }
}

static void helm_on_back(void * ud)
{
    LV_UNUSED(ud);
    helm_menu_squash_abort();
    if (s_menu) {
        if (helm_fav_plan_overlay_open(s_menu)) {
            helm_fav_plan_on_input(s_menu);
            return;
        }
        myvendor_sound_back();
        helm_back(s_menu);
    }
}

static void helm_on_key2_long(void * ud)
{
    helm_menu_t * m = s_menu;
    helm_scr_t scr;
    uint8_t * sel;

    LV_UNUSED(ud);
    helm_menu_squash_abort();
    if (m == NULL) {
        return;
    }
    if (helm_fav_plan_overlay_open(m)) {
        return;
    }

    /* 菜单里 KEY2 长按不得开停 REC。记录页长按询问删除；
     * 连接页长按删除已记设备，其它行仍断开当前类型。 */
    scr = helm_cur_scr(m);

    /* 「手机蓝牙」页：长按右键 = 提示解绑（已配对时）；没配对就提示还没配对。
     * 确认那一下走点击路径（同一个确认 dock，避免和别页的删记录确认打架）。 */
    if (scr == HELM_SCR_PHONE) {
        uint8_t *sel;
        unsigned nphone;

        if (s_phone_unbind_arm) {
            return;                 /* 已经在问"解除绑定？"了 */
        }

        sel = helm_cur_sel(m);
        nphone = myvendor_devctl_pair_phone_count();
        if (sel != NULL && *sel < nphone) {
            s_phone_unbind_idx = (unsigned)*sel;
            printf("helm: phone unbind ask slot=%u\n", s_phone_unbind_idx);
            s_phone_unbind_arm = true;
            helm_show_del(m);
            if (m->del_lab != NULL) {
                lv_label_set_text(m->del_lab, "解除绑定？");
            }
        } else if (nphone == 0u) {
            myvendor_sound_warn();
            lv_pm_notify_show("手机蓝牙", "还没配对", 1200);
        } else {
            myvendor_sound_warn();
            lv_pm_notify_show("手机蓝牙", "选中手机再长按", 1200);
        }
        return;
    }
    if (scr == HELM_SCR_RIDE_DETAIL || scr == HELM_SCR_RIDES
        || scr == HELM_SCR_GPX_RECORD || scr == HELM_SCR_GPX_IMPORT) {
        if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }

        if (scr != HELM_SCR_RIDE_DETAIL && !helm_ride_sel_bind(m)) {
            return;
        }

        if (s_ride_name[0] == '\0') {
            return;
        }

        s_sensor_del = false;
        helm_show_del(m);
        return;
    }

    if (scr == HELM_SCR_SENSORS) {
        sel = helm_cur_sel(m);
        if (sel != NULL && *sel < MYVENDOR_SYS_SENSOR_KIND_N) {
            (void)myvendor_devctl_sensor_disconnect_kind(*sel);
            myvendor_sound_warn();
            lv_pm_notify_show("蓝牙设备", "已断开", 1500);
            helm_menu_paint(m);
        } else if (sel != NULL) {
            /* 非传感器行（就是「手机蓝牙」那行）走它自己的动作：进子界面。 */
            helm_do_act(m, m->items[*sel].act);
        }

        return;
    }

    if (scr == HELM_SCR_FAVS) {
        helm_do_act(m, ACT_NAV_FAV_START);
        return;
    }

    if (scr == HELM_SCR_NAVPT_DETAIL) {
        if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }

        if (s_navpt_path[0] == '\0') {
            return;
        }

        myvendor_sound_ok();
        helm_push(m, HELM_SCR_NAVPT_ACTIONS);
        return;
    }

    if (scr == HELM_SCR_SCAN_PICK) {
        if (s_scan_link_wait) {
            s_scan_link_wait = false;
            (void)myvendor_devctl_sensor_disconnect_kind(s_scan_kind);
            myvendor_sound_warn();
            lv_pm_notify_show("蓝牙设备", "已取消", 1500);
            helm_menu_paint(m);
        }

        return;
    }

    if (scr == HELM_SCR_SCAN) {
        (void)helm_fill_items(m);
        sel = helm_cur_sel(m);
        if (sel != NULL && *sel < m->count &&
            m->items[*sel].act == ACT_SCAN_AUTO) {
            if (m->del_dock && !lv_obj_has_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN)) {
                return;
            }

            s_sensor_del = true;
            s_sensor_del_idx = m->items[*sel].extra;
            helm_show_del(m);
            /** 面包屑（2026-09-25）：用户报"确认弹窗自己消失"时，
             *  日志里要能对上"什么时候按出来的、隔多久按的 ✓" ——
             *  与 `helm_sensor_do_del()` 里那条配对读。 */
            printf("helm: sensor del dialog arm idx=%u (waiting confirm)\n",
                   (unsigned)s_sensor_del_idx);
            return;
        }

        (void)myvendor_devctl_sensor_disconnect_kind(s_scan_kind);
        myvendor_sound_warn();
        lv_pm_notify_show("蓝牙设备", "已断开", 1500);
        helm_menu_paint(m);
    }
}

static void helm_bind_keys(void)
{
    lv_port_buttons_set_page_scroll_cb(helm_on_next_item, NULL);
    /* KEY1 双击 = 列表**向上**一个聚焦（用户 2026-09-27）。注册它会同时把本页的单击
     * 延后一个双击窗口（否则单击+双击互相抵消）—— 所以离开菜单页时别忘注销。 */
    lv_port_buttons_set_page_scroll_double_cb(helm_on_prev_item, NULL);
    lv_port_buttons_set_page_confirm_cb(helm_on_confirm, NULL);
    lv_port_buttons_set_page_longpress_cb(helm_on_back, NULL);
    lv_port_buttons_set_page_longpress2_cb(helm_on_key2_long, NULL);
    lv_port_buttons_set_page_longpress2_up_cb(NULL, NULL);
    printf("helm: menu keys bound KEY1=next/back KEY2=ok\n");
}

static void helm_build_about(helm_menu_t * m)
{
    lv_obj_t * hero;
    lv_obj_t * kv;
    char line[48];
    const char * id = myvendor_identity_name();
    const char * ver = myvendor_sw_version();
    const char * hwv = myvendor_hw_version();
    const char * bld = myvendor_build_date();

    m->about = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->about);
    helm_grow_y(m->about);
    lv_obj_set_flex_flow(m->about, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(m->about, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->about, LV_OBJ_FLAG_SCROLLABLE);

    hero = helm_sheet_hero(m->about, HELM_ICO_CHIP, true);
    /* 设计稿 19 页：**版本号是这页的主角**（原来放产品名，版本挤在 KV 行里）。
     * 34px 走 TTF（版本串带字母，点阵大号只有数字），工厂固件回退 32px 点阵。 */
    helm_label(hero, helm_font_lab(), HELM_COLOR_MUTE, "本机");
    helm_label(hero, helm_font_val(), HELM_COLOR_INK,
               (ver && ver[0]) ? ver : VERSION_SOFTWARE);

    kv = helm_kvbox_create(m->about);
    lv_snprintf(line, sizeof(line), "%s",
                (ver && ver[0]) ? ver : VERSION_SOFTWARE);
    helm_kv_add(kv, HELM_ICO_CHIP, "固件", line);
    lv_snprintf(line, sizeof(line), "%s",
                (bld && bld[0]) ? bld : "-");
    helm_kv_add(kv, HELM_ICO_CHIP, "Build", line);
    lv_snprintf(line, sizeof(line), "%s",
                (hwv && hwv[0]) ? hwv : VERSION_HARDWARE);
    helm_kv_add(kv, HELM_ICO_CHIP, "硬件", line);
    helm_kv_add(kv, HELM_ICO_BLE, "蓝牙名",
                (id && id[0]) ? id : VERSION_FIRMWARE_NAME);
    helm_kvbox_seal(kv);
}

static void helm_kv_set_val(lv_obj_t * row, lv_obj_t ** out)
{
    uint32_t n;

    if (row == NULL || out == NULL) {
        return;
    }

    n = lv_obj_get_child_count(row);
    if (n == 0) {
        return;
    }

    *out = lv_obj_get_child(row, (int32_t)n - 1);
}

static void helm_build_gradecal(helm_menu_t * m)
{
    lv_obj_t * hero;
    lv_obj_t * kv;
    lv_obj_t * hint;

    m->gradecal = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->gradecal);
    helm_grow_y(m->gradecal);
    lv_obj_set_flex_flow(m->gradecal, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(m->gradecal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->gradecal, LV_OBJ_FLAG_SCROLLABLE);

    hero = helm_sheet_hero(m->gradecal, HELM_ICO_CLIMB, true);
    m->grade_val = helm_label(hero, helm_font_quad(), HELM_COLOR_CLIMB, "--");
    helm_label(hero, helm_font_lab(), HELM_COLOR_INK, "当前坡度");

    /* 水平气泡条：**放平了才知道什么时候能归零**（设计稿 20 页点名的缺失件）。
     * 中央 24px 靶区 = ±1°，气泡 = 左右倾角（加速度计 atan2(x,z)）。 */
    {
        lv_obj_t * card = lv_obj_create(m->gradecal);

        lv_obj_remove_style_all(card);
        helm_style_card(card);
        lv_obj_set_size(card, lv_pct(100), 44);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        m->grade_track = lv_obj_create(card);
        lv_obj_remove_style_all(m->grade_track);
        lv_obj_set_style_bg_color(m->grade_track, helm_color(HELM_COLOR_HAIR), 0);
        lv_obj_set_style_bg_opa(m->grade_track, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(m->grade_track, 2, 0);
        lv_obj_set_size(m->grade_track, 140, 4);
        lv_obj_set_pos(m->grade_track, 12, 20);

        m->grade_target = lv_obj_create(card);
        lv_obj_remove_style_all(m->grade_target);
        lv_obj_set_style_bg_color(m->grade_target, helm_color(HELM_COLOR_TRACK), 0);
        lv_obj_set_style_bg_opa(m->grade_target, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(m->grade_target, 4, 0);
        lv_obj_set_size(m->grade_target, 24, 8);
        lv_obj_set_pos(m->grade_target, 70, 18);

        m->grade_bub = lv_obj_create(card);
        lv_obj_remove_style_all(m->grade_bub);
        lv_obj_set_style_bg_color(m->grade_bub, helm_color(HELM_COLOR_NAV), 0);
        lv_obj_set_style_bg_opa(m->grade_bub, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(m->grade_bub, 8, 0);
        lv_obj_set_size(m->grade_bub, 16, 16);
        lv_obj_set_pos(m->grade_bub, 74, 14);

        m->grade_tilt = helm_label(card, helm_font_title(), HELM_COLOR_INK,
                                   "左右 --");
        lv_obj_align(m->grade_tilt, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    kv = helm_kvbox_create(m->gradecal);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_CLIMB, "原始", "--"), &m->grade_raw);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_GEAR, "偏置", "0.0%"), &m->grade_off);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_CHIP, "IMU", "等待中"), &m->grade_imu);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_ALT, "气压", "--"), &m->grade_baro);
    helm_kv_set_val(helm_kv_add(kv, HELM_ICO_ALT, "海拔", "--"), &m->grade_alt);
    helm_kvbox_seal(kv);

    hint = helm_label(m->gradecal, helm_font_lab(), HELM_COLOR_MUTE,
                      "水平放置后归零");
    lv_obj_set_style_pad_hor(hint, 12, 0);
    lv_obj_set_style_pad_top(hint, 8, 0);
}

static void helm_build_ride(helm_menu_t * m)
{
    m->ride = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->ride);
    helm_grow_y(m->ride);
    lv_obj_set_flex_flow(m->ride, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(m->ride, 6, 0);
    lv_obj_set_style_pad_row(m->ride, 4, 0);
    lv_obj_add_flag(m->ride, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->ride, LV_OBJ_FLAG_SCROLLABLE);

    m->ride_dist = helm_label(m->ride, helm_font_title(), HELM_COLOR_NAV, "--");
    lv_obj_set_width(m->ride_dist, lv_pct(100));
    lv_obj_set_style_pad_hor(m->ride_dist, 4, 0);
    lv_obj_set_style_pad_bottom(m->ride_dist, 2, 0);

    m->ride_track = lv_obj_create(m->ride);
    lv_obj_remove_style_all(m->ride_track);
    helm_grow_y(m->ride_track);
    lv_obj_set_style_bg_color(m->ride_track, helm_color(HELM_COLOR_NAV_FILL), 0);
    lv_obj_set_style_bg_opa(m->ride_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m->ride_track, HELM_RADIUS, 0);
    lv_obj_clear_flag(m->ride_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m->ride_track, helm_ride_track_draw, LV_EVENT_DRAW_MAIN,
                        NULL);

    m->ride_keys = lv_obj_create(m->ride);
    lv_obj_remove_style_all(m->ride_keys);
    lv_obj_set_width(m->ride_keys, lv_pct(100));
    lv_obj_set_height(m->ride_keys, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(m->ride_keys, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m->ride_keys, 4, 0);
    lv_obj_clear_flag(m->ride_keys, LV_OBJ_FLAG_SCROLLABLE);
    helm_mlist_sel_snap(m->ride_keys, 0);
}

static void helm_build_skip(helm_menu_t * m)
{
    lv_obj_t * head;
    lv_obj_t * ico;
    lv_obj_t * nav;

    m->skip_dock = helm_dock_create(m->root, false);
    lv_obj_add_flag(m->skip_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(m->skip_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_SKIP, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    helm_label(head, helm_font_title(), HELM_COLOR_INK, "跳过本点？");

    nav = lv_obj_create(m->skip_dock);
    lv_obj_remove_style_all(nav);
    lv_obj_set_width(nav, lv_pct(100));
    lv_obj_set_height(nav, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, 5, 0);
    lv_obj_set_style_margin_top(nav, 6, 0);
    ico = helm_icon_create(nav, HELM_ICO_PIN, 14);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    helm_label(nav, helm_font_lab(), HELM_COLOR_INK, "下一未到：终点");
}

static void helm_build_del(helm_menu_t * m)
{
    lv_obj_t * head;
    lv_obj_t * ico;

    m->del_dock = helm_dock_create(m->root, false);
    lv_obj_add_flag(m->del_dock, LV_OBJ_FLAG_HIDDEN);

    head = lv_obj_create(m->del_dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), HELM_DOCK_HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, HELM_DOCK_COL_GAP, 0);
    ico = helm_icon_create(head, HELM_ICO_XMARK, HELM_DOCK_ICO_HEAD);
    helm_icon_set_color(ico, helm_color(HELM_COLOR_INK));
    m->del_lab = helm_label(head, helm_font_title(), HELM_COLOR_INK, "删除此记录？");
    helm_softkeys_create(m->del_dock, HELM_COLOR_INK);
}

static void helm_build_plan(helm_menu_t * m)
{
    lv_obj_t * card;
    lv_obj_t * body;

    m->plan_mask = helm_mask_create(m->root);
    lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_IGNORE_LAYOUT);
    helm_fav_plan_place_mask(m);
    lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m->plan_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->plan_mask, helm_fav_plan_mask_clicked,
        LV_EVENT_CLICKED, m);

    card = helm_card_create(m->plan_mask, HELM_ICO_NAV, "正在规划", false);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, helm_fav_plan_mask_clicked, LV_EVENT_CLICKED, m);
    {
        lv_obj_t * head = lv_obj_get_child(card, 0);
        uint32_t n = head ? lv_obj_get_child_count(head) : 0u;

        m->plan_head = (n > 0u) ? lv_obj_get_child(head, n - 1u) : NULL;
    }
    body = helm_card_body(card);
    m->plan_msg = helm_label(body, helm_font_title(), HELM_COLOR_INK,
        "正在规划路线");
    lv_obj_set_width(m->plan_msg, lv_pct(100));
    lv_label_set_long_mode(m->plan_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(m->plan_msg, LV_TEXT_ALIGN_CENTER, 0);
    m->plan_hint = helm_label(body, helm_font_lab(), HELM_COLOR_HAIR,
        "单击停止");
    lv_obj_set_width(m->plan_hint, lv_pct(100));
    lv_obj_set_style_text_align(m->plan_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(m->plan_hint, 6, 0);
    helm_softkeys_create(body, HELM_COLOR_INK);
}

static helm_menu_t * helm_ui_ensure(lv_pm_page_t page)
{
    helm_menu_t * m;

    if (page == NULL || page->page == NULL) {
        return NULL;
    }

    if (page->user_data) {
        s_menu = (helm_menu_t *)page->user_data;
        return s_menu;
    }

    m = (helm_menu_t *)lv_pm_malloc(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }

    memset(m, 0, sizeof(*m));

    m->root = lv_obj_create(page->page);
    lv_obj_remove_style_all(m->root);
    lv_obj_set_size(m->root, PAGE_HOR_RES, HELM_PAGE_H);
    lv_obj_align(m->root, LV_ALIGN_TOP_LEFT, 0, 0);
    /* 菜单根底 = 深色（App background #000000）—— 菜单类页面统一深色表层，
     * 数据/骑行页保持半反屏纸白（见 helm_palette.h 的 MENU_* 一段）。 */
    helm_style_scr(m->root);
    lv_obj_set_style_bg_color(m->root, helm_color(HELM_COLOR_MENU_BG), 0);
    lv_obj_set_flex_flow(m->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(m->root, LV_OBJ_FLAG_SCROLLABLE);

    m->title = helm_mhead_create(m->root, "菜单");
    m->list = helm_mlist_create(m->root);

    m->empty = lv_obj_create(m->root);
    lv_obj_remove_style_all(m->empty);
    helm_grow_y(m->empty);
    lv_obj_set_flex_flow(m->empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(m->empty, LV_OBJ_FLAG_HIDDEN);

    helm_build_about(m);
    helm_build_gradecal(m);
    helm_build_ride(m);
    m->toolface = helm_toolbox_build(m->root);
    m->fontlab = helm_font_lab_build(m->root);
    helm_build_skip(m);
    helm_build_del(m);
    helm_build_plan(m);

    m->poll = lv_timer_create(helm_poll_cb, HELM_POLL_MS, NULL);
    lv_timer_pause(m->poll);

    m->stack[0].scr = HELM_SCR_ROOT;
    m->stack[0].sel = 0;
    m->sp = 1;
    page->user_data = m;
    s_menu = m;
    helm_menu_paint(m);
    return m;
}

static void helm_on_load(void * pm_page)
{
    (void)helm_ui_ensure(lv_pm_get_pm_page(pm_page));
}

static void helm_will_appear(void * pm_page)
{
    helm_menu_t * m = helm_ui_ensure(lv_pm_get_pm_page(pm_page));

    syslog(LOG_WARNING, "[theme] menu appear m=%p", (void *)m);   /* 临时诊断 */

    if (m) {
        helm_bind_keys();
        helm_menu_paint(m);
    }
}

static void helm_will_disappear(void * pm_page)
{
    LV_UNUSED(pm_page);
    syslog(LOG_WARNING, "[theme] menu will-disappear");   /* 临时诊断 */
    helm_scan_end();
    helm_menu_squash_abort();
    if (s_menu) {
        helm_mlist_cursor_off(s_menu->list);
        helm_mlist_cursor_off(s_menu->ride_keys);
        helm_fav_plan_hide(s_menu);
        helm_toolbox_hide();
    }
    if (s_menu && s_menu->poll) {
        lv_timer_pause(s_menu->poll);
    }
}

static void helm_did_disappear(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    syslog(LOG_WARNING, "[theme] menu did-disappear");   /* 临时诊断 */

    /* Fade 结束 opa=0 但仍会因子对象 invalidate 打到 LCD。 */
    if (page && page->page) {
        lv_obj_add_flag(page->page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void helm_on_unload(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);
    helm_menu_t * m;

    syslog(LOG_WARNING, "[theme] menu UNLOAD (s_menu=%p)", (void *)s_menu);  /* 临时诊断 */

    if (page == NULL) {
        return;
    }

    m = (helm_menu_t *)page->user_data;
    helm_scan_end();
    helm_menu_squash_abort();
    if (m) {
        helm_mlist_cursor_off(m->list);
        helm_mlist_cursor_off(m->ride_keys);
        helm_fav_plan_hide(m);
        helm_toolbox_unload();
        if (m->poll) {
            lv_timer_delete(m->poll);
            m->poll = NULL;
        }

        lv_pm_free(m);
        page->user_data = NULL;
    }

    if (s_menu == m) {
        s_menu = NULL;
    }
}

void helm_menu_page_register(void)
{
    lv_pm_page_t page = lv_pm_create_page((lv_pm_id)BICYCLE_PM_ID_MENU, "Menu");

    if (page == NULL) {
        LV_LOG_ERROR("helm_menu: register failed");
        return;
    }

    lv_pm_set_open(page, helm_on_load);
    lv_pm_set_will_appear(page, helm_will_appear);
    lv_pm_set_will_disappear(page, helm_will_disappear);
    lv_pm_set_dis_disappear(page, helm_did_disappear);
    lv_pm_set_close(page, helm_on_unload);
    bicycle_page_anima_apply(page, BICYCLE_PM_ID_MENU);
}

int helm_menu_open(void)
{
    int rc;

    if (lvgl_page_current_id() == BICYCLE_PM_ID_MENU) {
        return 0;
    }

    /* lv_pm_open_page_msg preempts an in-flight transition; do not refuse. */
    rc = lv_pm_open_page_msg((lv_pm_id)BICYCLE_PM_ID_MENU, NULL);
    printf("helm: menu open rc=%d\n", rc);
    return rc;
}

void helm_menu_refresh(void)
{
    if (s_menu != NULL) {
        helm_menu_paint(s_menu);
    }
}
