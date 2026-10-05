/**
 * @file bicycle_ride_gpx.c
 * @brief 骑行 REC 落盘：gpx_record → MYVENDOR_GPX_RECORD_DIR。
 *
 * @note push 无论成败都更新时间戳，避免每个 GNSS 点立刻重试拖垮 UI。
 *       队列暂时满只丢本点；不要把整趟 REC 停掉。
 */

#include "bicycle_ride_gpx.h"

#include "bicycle_env.h"
#include "bicycle_runtime.h"
#include "helm_icon.h"   /* HELM_ICO_GPX：通知横幅左侧图标 */
#include "lvgl/lvgl.h"
#include "gpx_ext_sensor.h"
#include "gpx_record.h"
#include "lv_pm_overlay.h"
#include "myvendor_devctl.h"
#include "myvendor_gpx.h"
#include "myvendor_watchdog.h"

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#define RIDE_GPX_MIN_INTERVAL_MS  1000u /**< 采点最短间隔；push 失败也推进时间戳，避免每帧重试。 */
#define RIDE_GPX_PATH_MAX         128
#define RIDE_GPX_TAIL_MAX         2048
#define RIDE_GPX_TAIL_FOOTER_MAX  256
#define RIDE_GPX_TRKPT_END        "</trkpt>"
#define RIDE_GPX_FOOTER           "</trkseg>\n</trk>\n</gpx>\n"
#define RIDE_GPX_ACTIVE_MARK      MYVENDOR_GPX_RECORD_DIR "/.rec_active"
#define RIDE_GPX_SALVAGE_TICK_MS  40u

static gpx_record_t * s_rec;
static char s_path[RIDE_GPX_PATH_MAX];
static bool s_active;
/**
 * @brief 磁盘已满：置位后本趟不再 `gpx_record_push`。
 * @details 未置位时 push 失败若不更新时间戳，每个 GNSS 点都会立刻重试，
 *          打满 `-ENOSPC` 日志并拖垮 `lv_timer_handler`。
 */
static bool s_push_full;
/** @brief 上次尝试 `gpx_record_push` 的 tick；成败都更新。 */
static uint32_t s_last_push_ms;
static int s_sensor_slot = -1;
static lv_timer_t * s_salvage_tmr;
static lv_timer_t * s_close_tmr;
static DIR * s_salvage_dir;
static uint32_t s_salvage_t0;
static unsigned s_salvage_n;
static unsigned s_salvage_fix;
static bool s_recovered_pending;
static bool s_salvage_ui_ready;
static bool s_salvage_marker_done;
static bool s_salvage_done;
/** @brief 已用 `.rec_active` 那条轨迹的末点写过上次骑行终点。 */
static bool s_last_pos_from_mark;
static bool s_pending_mark_clear;
static bool s_pending_saved_log;
static char s_pending_unlink[RIDE_GPX_PATH_MAX];
/** @brief 下次 begin 追加已复制的文件，而不是新建空 GPX。 */
static bool s_continue_pending;
/** @brief 续录副本已有轨迹：结束时即使本趟没写出新点也要保留。 */
static bool s_seeded;

static void ride_gpx_fill_utc(gpx_time_t * out)
{
    struct timespec ts;
    struct tm tm_buf;
    time_t utc;

    memset(out, 0, sizeof(*out));
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return;
    }

    utc = ts.tv_sec;
    if (gmtime_r(&utc, &tm_buf) == NULL) {
        return;
    }

    out->year = (int16_t)(tm_buf.tm_year + 1900);
    out->month = (uint8_t)(tm_buf.tm_mon + 1);
    out->day = (uint8_t)tm_buf.tm_mday;
    out->hour = (uint8_t)tm_buf.tm_hour;
    out->minute = (uint8_t)tm_buf.tm_min;
    out->second = (uint8_t)tm_buf.tm_sec;
}

static void ride_gpx_make_path(char * buf, size_t n)
{
    struct tm tm_buf;
    time_t utc;
    time_t wall;
    int16_t tz;

    utc = time(NULL);
    tz = myvendor_devctl_tz_min_get();
    wall = utc + (time_t)tz * 60;
    if (gmtime_r(&wall, &tm_buf) == NULL) {
        snprintf(buf, n, "%s/TRK.gpx", MYVENDOR_GPX_RECORD_DIR);
        return;
    }

    snprintf(buf, n, "%s/TRK_%04d%02d%02d_%02d%02d%02d.gpx",
        MYVENDOR_GPX_RECORD_DIR,
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
}

static bool ride_gpx_path_exists(const char * path)
{
    struct stat st;

    return path != NULL && path[0] != '\0' && stat(path, &st) == 0;
}

static void ride_gpx_make_continue_path(char * buf, size_t n, const char * src)
{
    size_t len;

    ride_gpx_make_path(buf, n);
    if (src != NULL && strcmp(buf, src) == 0) {
        len = strlen(buf);
        if (len >= 4u && n > len + 2u && strcmp(buf + len - 4u, ".gpx") == 0) {
            buf[len - 4u] = '\0';
            snprintf(buf + strlen(buf), n - strlen(buf), "_c.gpx");
        }
    } else if (ride_gpx_path_exists(buf)) {
        len = strlen(buf);
        if (len >= 4u && n > len + 2u && strcmp(buf + len - 4u, ".gpx") == 0) {
            buf[len - 4u] = '\0';
            snprintf(buf + strlen(buf), n - strlen(buf), "_c.gpx");
        }
    }
}

static int ride_gpx_copy_file(const char * src, const char * dst)
{
    char buf[512];
    int in_fd;
    int out_fd;
    ssize_t n;
    ssize_t w;

    if (src == NULL || dst == NULL || src[0] == '\0' || dst[0] == '\0') {
        return -EINVAL;
    }

    if (strcmp(src, dst) == 0) {
        return -EEXIST;
    }

    in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        return -errno;
    }

    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        int err = -errno;

        close(in_fd);
        return err;
    }

    for (;;) {
        n = read(in_fd, buf, sizeof(buf));
        if (n < 0) {
            int err = -errno;

            close(in_fd);
            close(out_fd);
            (void)unlink(dst);
            return err;
        }

        if (n == 0) {
            break;
        }

        w = write(out_fd, buf, (size_t)n);
        if (w != n) {
            int err = (w < 0) ? -errno : -EIO;

            close(in_fd);
            close(out_fd);
            (void)unlink(dst);
            return err;
        }

        myvendor_watchdog_hw_pet();
    }

    (void)fsync(out_fd);
    close(in_fd);
    close(out_fd);
    return 0;
}

static void ride_gpx_mark_write(const char * path)
{
    int fd;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    fd = open(RIDE_GPX_ACTIVE_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
    (void)write(fd, path, strlen(path));
    (void)fsync(fd);
    close(fd);
}

static void ride_gpx_mark_clear(void)
{
    (void)unlink(RIDE_GPX_ACTIVE_MARK);
}

static void ride_gpx_notify_recovered(void)
{
    s_recovered_pending = true;
    if (!s_salvage_ui_ready) {
        return;
    }

    if (lv_pm_notify_show_icon("骑行", "已恢复未结束的记录", HELM_ICO_GPX, 3000)) {
        s_recovered_pending = false;
    }
}

static void ride_gpx_notify_flush(void)
{
    if (s_recovered_pending) {
        ride_gpx_notify_recovered();
    }
}

static int ride_gpx_ensure_rec(void)
{
    gpx_record_cfg_t cfg;
    int ret;

    if (s_sensor_slot < 0) {
        s_sensor_slot = gpx_ext_sensor_register();
    }

    if (s_rec != NULL) {
        return 0;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.batch_size = 8;
    /* KV 写 persist 会占 SD；深度太浅会 -ENOSPC，旧逻辑把整趟 REC 判死。 */
    cfg.queue_depth = 128;
    ret = gpx_record_create(&cfg, &s_rec);
    if (ret != 0) {
        printf("ride gpx: create failed %d\n", ret);
        s_rec = NULL;
        return ret;
    }

    return 0;
}

static ssize_t ride_gpx_find_last(const char * buf, size_t n, const char * needle)
{
    size_t needle_n;
    ssize_t last = -1;
    size_t i;

    if (buf == NULL || needle == NULL) {
        return -1;
    }

    needle_n = strlen(needle);
    if (needle_n == 0 || n < needle_n) {
        return -1;
    }

    for (i = 0; i + needle_n <= n; i++) {
        if (memcmp(buf + i, needle, needle_n) == 0) {
            last = (ssize_t)i;
        }
    }

    return last;
}

/**
 * @brief 从文件尾往前找最后一个完整 </trkpt>，得到应保留的长度。
 *
 * @return 1 找到，0 无轨迹点，负 errno 失败
 */
static int ride_gpx_last_trkpt_end(int fd, off_t size, off_t * keep_out)
{
    char buf[RIDE_GPX_TAIL_MAX];
    const char * tag = RIDE_GPX_TRKPT_END;
    const size_t tag_n = sizeof(RIDE_GPX_TRKPT_END) - 1u;
    const off_t overlap = (off_t)tag_n - 1;
    off_t pos = size;

    if (fd < 0 || keep_out == NULL || size <= 0) {
        return 0;
    }

    while (pos > 0) {
        off_t start;
        ssize_t n;
        ssize_t last;

        start = (pos > (off_t)sizeof(buf)) ? pos - (off_t)sizeof(buf) : 0;
        if (lseek(fd, start, SEEK_SET) < 0) {
            return -errno;
        }

        n = read(fd, buf, (size_t)(pos - start));
        if (n <= 0) {
            return (n < 0) ? -errno : 0;
        }

        myvendor_watchdog_ui_beat();

        last = ride_gpx_find_last(buf, (size_t)n, tag);
        if (last >= 0) {
            off_t keep = start + (off_t)last + (off_t)tag_n;

            if ((size_t)last + tag_n < (size_t)n &&
                buf[last + (ssize_t)tag_n] == '\n') {
                keep++;
            }

            *keep_out = keep;
            return 1;
        }

        if (start == 0) {
            return 0;
        }

        pos = start + overlap;
        if (pos >= size) {
            pos = start;
        }
    }

    return 0;
}

static void ride_gpx_apply_last_pos(double lon, double lat, bool from_mark)
{
    int32_t lon_e7;
    int32_t lat_e7;

    if (!from_mark && s_last_pos_from_mark) {
        return;
    }

    if (!isfinite(lon) || !isfinite(lat) ||
        lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0) {
        return;
    }

    if (fabs(lon) < 0.001 && fabs(lat) < 0.001) {
        return;
    }

    lon_e7 = (int32_t)(lon * 10000000.0 + (lon >= 0.0 ? 0.5 : -0.5));
    lat_e7 = (int32_t)(lat * 10000000.0 + (lat >= 0.0 ? 0.5 : -0.5));
    myvendor_devctl_last_pos_set(lon_e7, lat_e7);
    if (from_mark) {
        s_last_pos_from_mark = true;
    }

    syslog(LOG_NOTICE, "ride gpx: last pos lon=%.6f lat=%.6f%s",
        lon, lat, from_mark ? " (rec_active)" : "");
}

/**
 * @brief 从最后一个完整 </trkpt> 之前读开标签，解析 lat/lon。
 */
static bool ride_gpx_last_trkpt_ll(int fd, off_t keep, double * lon, double * lat)
{
    char buf[RIDE_GPX_TAIL_MAX];
    char open[192];
    off_t start;
    ssize_t n;
    ssize_t tag;
    ssize_t gt;
    ssize_t i;
    size_t open_n;
    const char * lp;
    const char * op;
    char * end_lat;
    char * end_lon;
    double la;
    double lo;

    if (fd < 0 || keep <= 0 || lon == NULL || lat == NULL) {
        return false;
    }

    start = (keep > (off_t)sizeof(buf)) ? keep - (off_t)sizeof(buf) : 0;
    if (lseek(fd, start, SEEK_SET) < 0) {
        return false;
    }

    n = read(fd, buf, (size_t)(keep - start));
    if (n <= 0) {
        return false;
    }

    tag = ride_gpx_find_last(buf, (size_t)n, "<trkpt");
    if (tag < 0) {
        return false;
    }

    gt = -1;
    for (i = tag; i < n; i++) {
        if (buf[i] == '>') {
            gt = i;
            break;
        }
    }

    if (gt < 0) {
        return false;
    }

    open_n = (size_t)(gt - tag + 1);
    if (open_n >= sizeof(open)) {
        open_n = sizeof(open) - 1u;
    }

    memcpy(open, buf + tag, open_n);
    open[open_n] = '\0';
    lp = strstr(open, "lat=\"");
    op = strstr(open, "lon=\"");
    if (lp == NULL || op == NULL) {
        return false;
    }

    la = strtod(lp + 5, &end_lat);
    lo = strtod(op + 5, &end_lon);
    if (end_lat == lp + 5 || end_lon == op + 5) {
        return false;
    }

    if (!isfinite(la) || !isfinite(lo)) {
        return false;
    }

    *lat = la;
    *lon = lo;
    return true;
}

/**
 * @brief 截到最后一个完整 </trkpt>，去掉 footer，供追加新点。
 */
static int ride_gpx_strip_for_append(const char * path)
{
    struct stat st;
    int fd;
    int found;
    off_t keep = 0;

    if (path == NULL || stat(path, &st) != 0) {
        return -ENOENT;
    }

    if (st.st_size <= 0) {
        return -ENODATA;
    }

    fd = open(path, O_RDWR);
    if (fd < 0) {
        return -errno;
    }

    found = ride_gpx_last_trkpt_end(fd, st.st_size, &keep);
    if (found <= 0) {
        close(fd);
        return (found < 0) ? found : -ENODATA;
    }

    if (ftruncate(fd, keep) != 0) {
        int err = -errno;

        close(fd);
        return err;
    }

    (void)fsync(fd);
    close(fd);
    return 0;
}

static bool ride_gpx_tail_has_footer(int fd, off_t size)
{
    char buf[RIDE_GPX_TAIL_FOOTER_MAX];
    off_t start;
    ssize_t n;
    ssize_t i;

    if (fd < 0 || size < 6) {
        return false;
    }

    start = (size > (off_t)sizeof(buf)) ? size - (off_t)sizeof(buf) : 0;
    if (lseek(fd, start, SEEK_SET) < 0) {
        return false;
    }

    n = read(fd, buf, (size_t)(size - start));
    if (n < 6) {
        return false;
    }

    for (i = 0; i + 6 <= n; i++) {
        if (memcmp(buf + i, "</gpx>", 6) == 0) {
            return true;
        }
    }
    return false;
}

static void ride_gpx_salvage_one(const char * path, bool from_mark)
{
    struct stat st;
    int fd;
    int found;
    off_t keep = 0;
    char endbuf[64];
    ssize_t n;
    bool has_footer;
    double lon = 0.0;
    double lat = 0.0;
    bool have_ll = false;

    if (path == NULL || stat(path, &st) != 0) {
        return;
    }

    if (st.st_size <= 0) {
        if (unlink(path) == 0) {
            syslog(LOG_NOTICE, "ride gpx: drop 0-byte %s", path);
            s_salvage_fix++;
        }
        return;
    }

    fd = open(path, O_RDWR);
    if (fd < 0) {
        return;
    }

    has_footer = ride_gpx_tail_has_footer(fd, st.st_size);
    if (has_footer && !from_mark) {
        close(fd);
        return;
    }

    found = ride_gpx_last_trkpt_end(fd, st.st_size, &keep);
    if (found <= 0) {
        close(fd);
        if (!has_footer && found == 0 && unlink(path) == 0) {
            syslog(LOG_NOTICE, "ride gpx: drop no-trkpt %s", path);
            s_salvage_fix++;
        }
        return;
    }

    have_ll = ride_gpx_last_trkpt_ll(fd, keep, &lon, &lat);

    if (has_footer) {
        close(fd);
        if (have_ll) {
            ride_gpx_apply_last_pos(lon, lat, from_mark);
        }
        return;
    }

    if (lseek(fd, keep, SEEK_SET) < 0) {
        close(fd);
        return;
    }

    n = read(fd, endbuf, sizeof(endbuf) - 1u);
    if (n > 0) {
        endbuf[n] = '\0';
        if (strstr(endbuf, "</gpx>") != NULL) {
            close(fd);
            if (have_ll) {
                ride_gpx_apply_last_pos(lon, lat, from_mark);
            }
            return;
        }
    }

    if (ftruncate(fd, keep) != 0 || lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        return;
    }

    if (write(fd, "\n" RIDE_GPX_FOOTER, sizeof("\n" RIDE_GPX_FOOTER) - 1u) < 0) {
        close(fd);
        return;
    }

    (void)fsync(fd);
    close(fd);
    syslog(LOG_NOTICE, "ride gpx: closed %s", path);
    s_salvage_fix++;
    if (have_ll) {
        ride_gpx_apply_last_pos(lon, lat, from_mark);
    }
    ride_gpx_notify_recovered();
}

static void ride_gpx_salvage_marker(void)
{
    char path[RIDE_GPX_PATH_MAX];
    int fd;
    ssize_t n;

    fd = open(RIDE_GPX_ACTIVE_MARK, O_RDONLY);
    if (fd < 0) {
        return;
    }

    n = read(fd, path, sizeof(path) - 1u);
    close(fd);
    if (n <= 0) {
        ride_gpx_mark_clear();
        return;
    }

    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r' ||
                     path[n - 1] == ' ')) {
        n--;
    }
    path[n] = '\0';
    ride_gpx_salvage_one(path, true);
    ride_gpx_mark_clear();
}

static bool ride_gpx_name_is_trk(const char * name)
{
    size_t n;

    if (name == NULL || name[0] == '.') {
        return false;
    }

    n = strlen(name);
    if (n < 5u || n >= 64u) {
        return false;
    }

    return strcasecmp(name + n - 4u, ".gpx") == 0;
}

static void ride_gpx_salvage_finish(void)
{
    if (s_salvage_done) {
        return;
    }

    s_salvage_done = true;
    s_salvage_marker_done = true;
    if (s_salvage_dir != NULL) {
        closedir(s_salvage_dir);
        s_salvage_dir = NULL;
    }
    if (s_salvage_tmr != NULL) {
        lv_timer_del(s_salvage_tmr);
        s_salvage_tmr = NULL;
    }
    syslog(LOG_NOTICE, "ride gpx: salvage done files=%u closed=%u %ums",
        s_salvage_n, s_salvage_fix, (unsigned)lv_tick_elaps(s_salvage_t0));
    if (ride_gpx_ensure_rec() == 0 && s_rec != NULL) {
        int pret = gpx_record_prepare(s_rec);

        if (pret != 0) {
            printf("ride gpx: prepare failed %d\n", pret);
        }
    }
    ride_gpx_notify_flush();
}

/**
 * @brief 每拍最多闭合 1 个未写完的 GPX；完成返回 true。
 */
static bool ride_gpx_salvage_step(void)
{
    struct dirent * de;
    char path[RIDE_GPX_PATH_MAX];
    unsigned skip;

    myvendor_watchdog_ui_beat();
    myvendor_watchdog_hw_pet();

    if (s_salvage_done) {
        return true;
    }

    if (!s_salvage_marker_done) {
        if (s_salvage_t0 == 0) {
            s_salvage_t0 = lv_tick_get();
        }
        printf("恢复未保存gpx...\n");
        syslog(LOG_NOTICE, "恢复未保存gpx...");
        ride_gpx_salvage_marker();
        s_salvage_marker_done = true;
        return false;
    }

    if (s_salvage_dir == NULL) {
        s_salvage_dir = opendir(MYVENDOR_GPX_RECORD_DIR);
        if (s_salvage_dir == NULL) {
            ride_gpx_salvage_finish();
            return true;
        }
        syslog(LOG_NOTICE, "ride gpx: salvage scan %s", MYVENDOR_GPX_RECORD_DIR);
        return false;
    }

    for (skip = 0; skip < 32u; skip++) {
        de = readdir(s_salvage_dir);
        if (de == NULL) {
            ride_gpx_salvage_finish();
            return true;
        }

        if (!ride_gpx_name_is_trk(de->d_name)) {
            continue;
        }

        if (snprintf(path, sizeof(path), "%s/%s", MYVENDOR_GPX_RECORD_DIR,
                     de->d_name) >= (int)sizeof(path)) {
            continue;
        }

        s_salvage_n++;
        ride_gpx_salvage_one(path, false);
        return false;
    }

    return false;
}

static void ride_gpx_salvage_tick(lv_timer_t * tmr)
{
    (void)tmr;
    (void)ride_gpx_salvage_step();
}

/**
 * @brief 只处理上次未关的那条记录（工厂页无 splash）。
 */
void bicycle_ride_gpx_salvage_dir(void)
{
    ride_gpx_salvage_marker();
    s_salvage_marker_done = true;
}

/**
 * @brief 开机白屏泵一次；完成返回 true。
 */
bool bicycle_ride_gpx_salvage_boot_pump(void)
{
    return ride_gpx_salvage_step();
}

/** @brief 开机扫过的轨迹文件数。 */
unsigned bicycle_ride_gpx_salvage_scanned(void)
{
    return s_salvage_n;
}

/** @brief 闭合或丢掉的条数。 */
unsigned bicycle_ride_gpx_salvage_fixed(void)
{
    return s_salvage_fix;
}

/**
 * @brief splash 结束：弹出「已恢复」；白屏没扫完则后台继续。
 */
void bicycle_ride_gpx_salvage_scan(void)
{
    s_salvage_ui_ready = true;
    ride_gpx_notify_flush();

    if (s_salvage_done || s_salvage_tmr != NULL) {
        return;
    }

    s_salvage_tmr = lv_timer_create(ride_gpx_salvage_tick, RIDE_GPX_SALVAGE_TICK_MS,
        NULL);
    if (s_salvage_tmr == NULL) {
        return;
    }
}

static void ride_gpx_close_tick(lv_timer_t * t)
{
    LV_UNUSED(t);
    if (s_rec != NULL && gpx_record_is_closing(s_rec)) {
        return;
    }

    if (s_pending_mark_clear) {
        ride_gpx_mark_clear();
        s_pending_mark_clear = false;
    }

    if (s_pending_unlink[0] != '\0') {
        if (unlink(s_pending_unlink) != 0 && errno != ENOENT) {
            printf("ride gpx: unlink %s errno=%d\n", s_pending_unlink, errno);
        }

        s_pending_unlink[0] = '\0';
    }

    if (s_pending_saved_log) {
        printf("ride gpx: closed %s\n", s_path);
        s_pending_saved_log = false;
    }

    if (s_close_tmr != NULL) {
        lv_timer_del(s_close_tmr);
        s_close_tmr = NULL;
    }
}

static void ride_gpx_arm_close_tmr(void)
{
    if (s_close_tmr != NULL) {
        return;
    }

    s_close_tmr = lv_timer_create(ride_gpx_close_tick, RIDE_GPX_SALVAGE_TICK_MS,
                                  NULL);
}

/**
 * @brief 关当前记录文件。
 * @param keep true 保留文件；false 删除。
 * @details 结束时清 `s_push_full`，下一趟可再写。STOP 立即返回，close 在工作线程。
 */
static int ride_gpx_stop_file(bool keep)
{
    int ret = 0;
    uint32_t points = 0;
    bool has_data;

    if (s_rec != NULL) {
        gpx_record_stats_t st;

        if (gpx_record_get_stats(s_rec, &st) == 0) {
            points = st.points_written;
        }
    }

    has_data = (s_last_push_ms != 0) || (points > 0) || s_seeded;
    s_seeded = false;
    s_continue_pending = false;

    if (s_rec != NULL &&
        (gpx_record_is_recording(s_rec) || gpx_record_is_closing(s_rec))) {
        ret = gpx_record_stop(s_rec);
        if (ret != 0) {
            printf("ride gpx: stop failed %d\n", ret);
        }
    }

    s_active = false;
    s_push_full = false;
    s_last_push_ms = 0;
    s_pending_mark_clear = true;

    if (!keep || (ret == 0 && s_path[0] != '\0' && !has_data)) {
        if (s_path[0] != '\0') {
            snprintf(s_pending_unlink, sizeof(s_pending_unlink), "%s", s_path);
        }

        if (!keep) {
            s_path[0] = '\0';
        } else {
            printf("ride gpx: drop empty %s\n", s_path);
            s_path[0] = '\0';
            ret = (ret == 0) ? -ENODATA : ret;
        }
    } else if (keep && ret == 0) {
        s_pending_saved_log = true;
    }

    ride_gpx_arm_close_tmr();
    return ret;
}

/**
 * @brief 打开本机记录文件。
 * @details 新开一趟时清 `s_push_full`，磁盘满停写只作用于当前文件。
 */
int bicycle_ride_gpx_begin(void)
{
    gpx_meta_t meta;
    const char * base;
    int ret;
    bool append = s_continue_pending;

    if (s_active && s_rec != NULL && gpx_record_is_recording(s_rec)) {
        return 0;
    }

    ret = ride_gpx_ensure_rec();
    if (ret != 0) {
        if (append && s_path[0] != '\0') {
            (void)unlink(s_path);
            s_path[0] = '\0';
        }

        s_continue_pending = false;
        s_seeded = false;
        return ret;
    }

    if (gpx_record_is_recording(s_rec)) {
        s_active = true;
        return 0;
    }

    if (!append) {
        ride_gpx_make_path(s_path, sizeof(s_path));
    }

    s_continue_pending = false;
    base = strrchr(s_path, '/');
    base = (base != NULL && base[1] != '\0') ? base + 1 : s_path;

    memset(&meta, 0, sizeof(meta));
    meta.creator = CONFIG_MYVENDOR_PRODUCT_NAME;
    meta.meta_name = CONFIG_MYVENDOR_PRODUCT_NAME;
    meta.track_name = base;

    ret = append ? gpx_record_start_append(s_rec, s_path, &meta)
                 : gpx_record_start(s_rec, s_path, &meta);
    if (ret != 0) {
        printf("ride gpx: start %s failed %d\n", s_path, ret);
        if (append && s_path[0] != '\0') {
            (void)unlink(s_path);
        }

        s_path[0] = '\0';
        s_active = false;
        s_seeded = false;
        return ret;
    }

    s_active = true;
    s_push_full = false;
    s_last_push_ms = 0;
    s_seeded = append;
    ride_gpx_mark_write(s_path);
    printf("ride gpx: start %s%s\n", s_path, append ? " (append)" : "");
    return 0;
}

/**
 * @brief 复制 src 到新的 TRK_*.gpx，去掉 footer，供下次 begin 追加。
 */
int bicycle_ride_gpx_prepare_continue(const char * src)
{
    char dest[RIDE_GPX_PATH_MAX];
    int ret;

    if (src == NULL || src[0] == '\0') {
        return -EINVAL;
    }

    if (s_active || bicycle_ride_gpx_active()) {
        return -EBUSY;
    }

    bicycle_ride_gpx_wait_idle();

    if (s_continue_pending && s_path[0] != '\0') {
        (void)unlink(s_path);
        s_path[0] = '\0';
        s_continue_pending = false;
    }

    ride_gpx_make_continue_path(dest, sizeof(dest), src);
    if (dest[0] == '\0' || strcmp(dest, src) == 0) {
        return -EEXIST;
    }

    ret = ride_gpx_copy_file(src, dest);
    if (ret != 0) {
        printf("ride gpx: copy %s -> %s failed %d\n", src, dest, ret);
        return ret;
    }

    ret = ride_gpx_strip_for_append(dest);
    if (ret != 0) {
        printf("ride gpx: strip %s failed %d\n", dest, ret);
        (void)unlink(dest);
        return ret;
    }

    snprintf(s_path, sizeof(s_path), "%s", dest);
    s_continue_pending = true;
    s_seeded = true;
    printf("ride gpx: continue copy %s -> %s\n", src, dest);
    return 0;
}

/**
 * @brief 放弃尚未 begin 的续录副本。
 */
void bicycle_ride_gpx_cancel_continue(void)
{
    if (s_continue_pending && s_path[0] != '\0') {
        (void)unlink(s_path);
        s_path[0] = '\0';
    }

    s_continue_pending = false;
    s_seeded = false;
}

/**
 * @brief 有效定位且未暂停时采点（约 1 Hz）。
 * @param fix 当前 GNSS 定位。
 * @details 无论成败都更新 `s_last_push_ms`，避免失败时每帧重试。
 *          队列满只丢掉本点；开始 / 结束录制时清 `s_push_full`。
 */
void bicycle_ride_gpx_push_fix(const bicycle_gnss_fix_t * fix)
{
    const bicycle_runtime_t * rt;
    gpx_point_t pt;
    gpx_sensor_ext_t sensor;
    uint32_t now_ms;
    int ret;

    if (!s_active || s_push_full || s_rec == NULL || fix == NULL || !fix->valid) {
        return;
    }

    now_ms = lv_tick_get();
    if (s_last_push_ms != 0 &&
        lv_tick_elaps(s_last_push_ms) < RIDE_GPX_MIN_INTERVAL_MS) {
        return;
    }

    rt = bicycle_runtime_get();
    memset(&pt, 0, sizeof(pt));
    pt.latitude = fix->latitude;
    pt.longitude = fix->longitude;
    ride_gpx_fill_utc(&pt.time);
    pt.has_time = (pt.time.year > 0);

    if (fix->has_altitude) {
        pt.altitude = fix->altitude_m;
        pt.has_altitude = true;
    } else if (rt && rt->has_altitude) {
        pt.altitude = rt->altitude_m;
        pt.has_altitude = true;
    }

    /* 本机能给出的四个骑行读数全部落盘：心率 / 踏频 / 速度 / 功率。
     *
     * 速度存 m/s（gpxtpx:speed 的单位，见 gpx_ext_sensor.h），来源是 runtime
     * 里那个**已经发布出去**的速度，不是 fix->speed_kph 的原始多普勒 —— 屏幕上
     * 显示的是哪个数，文件里就该是哪个数。
     *
     * 门控只看 rt 与 slot，**不再看有没有传感器**：速度是 GNSS 给的，没插心率带
     * 时文件里也该有 speed（之前的写法把整个扩展块挂在 hr_valid||cadence_valid
     * 上，于是无传感器的那趟骑行连速度都没写）。没数据的字段由各自的 has_* 关掉。
     *
     * 速度为 0 也照样写：能走到这里说明 fix->valid（上面刚判过），停车的 0 是真实
     * 信息；跳过不写反而让下游分不清"缺字段"和"停在原地"。 */
    if (rt && s_sensor_slot >= 0) {
        memset(&sensor, 0, sizeof(sensor));
        sensor.hr = rt->hr_bpm;
        sensor.cad = rt->cadence_rpm;
        sensor.power_w = rt->power_w;
        sensor.speed_mps = rt->speed_kph / 3.6f;
        sensor.has_hr = rt->hr_valid;
        sensor.has_cad = rt->cadence_valid;
        sensor.has_power = rt->power_valid;
        sensor.has_speed = true;
        pt.ext_data[s_sensor_slot] = &sensor;
        pt.ext_mask = (uint8_t)(1u << s_sensor_slot);
    }

    ret = gpx_record_push(s_rec, &pt);
    s_last_push_ms = now_ms;
    if (ret != 0) {
        if (ret == -ENOSPC) {
            /* 队列暂时满（SD 被 KV/地图占着），丢掉本点，下一秒再写。
             * 不要当成磁盘满把整趟 REC 停掉。 */
            syslog(LOG_WARNING, "ride gpx: drop point (queue busy)\n");
        } else {
            printf("ride gpx: push failed %d\n", ret);
        }
        return;
    }
}

/**
 * @brief 刷盘关文件并保留。
 */
int bicycle_ride_gpx_commit(char * path, size_t path_n)
{
    int ret;

    /* 骑行结束的"最后一段结账"（用户 2026-10-05）。正常走 KEY2 结束/暂停时
     * `bicycle_runtime_set_recording(false)` 已经结过一次，这里是给**没经过
     * 停录的收尾路径**兜底（关机前的 auto-save：helm_pwr_do_gpx 直接 commit）
     * ——flush 幂等，结过账再调只挪参考点，不会重复计。 */
    bicycle_env_flush_gain();

    if (!s_active || s_rec == NULL || !gpx_record_is_recording(s_rec)) {
        if (s_path[0] == '\0') {
            ret = bicycle_ride_gpx_begin();
            if (ret != 0) {
                return ret;
            }
        }
    }

    ret = ride_gpx_stop_file(true);
    if (path != NULL && path_n > 0) {
        snprintf(path, path_n, "%s", s_path);
    }

    printf("ride gpx: saved %s ret=%d\n", s_path, ret);
    return ret;
}

/**
 * @brief 等到当前 GPX close/fsync 结束（关机前调用）。
 */
void bicycle_ride_gpx_wait_idle(void)
{
    while (s_rec != NULL && gpx_record_is_closing(s_rec)) {
        myvendor_watchdog_hw_pet();
        usleep(20000);
    }

    if (s_pending_mark_clear || s_pending_unlink[0] != '\0' ||
        s_pending_saved_log) {
        ride_gpx_close_tick(NULL);
    }
}

/**
 * @brief 关文件并删除。
 *
 * 已 commit 的会话 s_active=false，此处不得 unlink，否则结束保存会被 end_ride 删掉。
 */
int bicycle_ride_gpx_discard(void)
{
    if (!s_active) {
        return 0;
    }

    printf("ride gpx: discard %s\n", s_path);
    return ride_gpx_stop_file(false);
}

/**
 * @brief 本趟是否已占住一条 TRK_*.gpx 正在录。
 */
bool bicycle_ride_gpx_active(void)
{
    return s_active && s_rec != NULL && gpx_record_is_recording(s_rec);
}
