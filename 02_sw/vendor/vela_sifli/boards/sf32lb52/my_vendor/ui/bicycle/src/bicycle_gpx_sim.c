/**
 * @file bicycle_gpx_sim.c
 * @brief 自行车 UI — gpx_sim。
 */

#include "bicycle_gpx_sim.h"

#include "vmap/vmap_alloc.h"
#include "vmap/vmap_config.h"
#include "board_malloc.h"
#include "gpx_decode.h"
#include "gpx_port.h"
#include "lvgl/lvgl.h"
#include "myvendor_watchdog.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#ifndef CONFIG_GPX_DECODE_STACKSIZE
#  define CONFIG_GPX_DECODE_STACKSIZE 8192
#endif

#define GPX_SIM_STACK_ALIGN 16u

static void *g_route_sim_stack_raw;
static void *g_gpx_sim_stack_raw;

/** Debug/simulation threads are cold paths; keep their stacks out of SRAM. */
static void *sim_stack_alloc(void **raw_out)
{
    void *raw;
    uintptr_t aligned;

    if (raw_out == NULL) {
        return NULL;
    }

    raw = board_malloc_psram(CONFIG_GPX_DECODE_STACKSIZE +
        GPX_SIM_STACK_ALIGN - 1u);
    if (raw == NULL || !board_ptr_in_psram_pool(raw)) {
        board_mem_free(raw);
        return NULL;
    }

    aligned = ((uintptr_t)raw + GPX_SIM_STACK_ALIGN - 1u) &
        ~((uintptr_t)GPX_SIM_STACK_ALIGN - 1u);
    *raw_out = raw;
    return (void *)aligned;
}

static void sim_stack_free(void **raw)
{
    if (raw != NULL && *raw != NULL) {
        board_free_psram(*raw);
        *raw = NULL;
    }
}

#ifndef GPX_SIM_MAX_PTS
#  define GPX_SIM_MAX_PTS GPX_DECODE_POINT_MAX
#endif

typedef struct {
    float lon;
    float lat;
    float alt;
    gpx_time_t time;
    bool has_alt;
    bool has_time;
} gpx_sim_pt_t;

typedef struct {
    char path[160];
    gpx_sim_pt_t * pts;
    uint32_t pt_count;
    uint32_t idx;
    bool reverse;
    gpx_point_t cur;
    gpx_point_t next;
    bool has_next;
    double seg_len_m;
    double seg_done_m;
    float seg_course;
    float seg_speed_kph;
    float speed_min_kph;
    float speed_max_kph;
    uint32_t sim_elapsed_ms;
    unsigned period_ms;
    uint32_t prefix_n;
    uint32_t prefix_ms;
    double prefix_m;
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_run;
    bool thread_alive;
    bicycle_gnss_fix_t fix;
} gpx_sim_t;

static gpx_sim_t g_sim;

typedef struct {
    float *lon;
    float *lat;
    uint32_t pt_count;
    uint32_t seg_idx;
    double seg_len_m;
    double seg_done_m;
    float seg_course;
    float speed_kph;
    unsigned period_ms;
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_run;
    bool thread_alive;
    bicycle_gnss_fix_t fix;
} route_sim_t;

static route_sim_t g_route;

static uint8_t sim_rx_hz(unsigned period_ms)
{
    unsigned hz;

    if (period_ms == 0) {
        return 1;
    }

    hz = (1000u + period_ms / 2u) / period_ms;
    if (hz < 1u) {
        hz = 1u;
    } else if (hz > 99u) {
        hz = 99u;
    }

    return (uint8_t)hz;
}

/**
 * @brief 路线点放 BoardPSRAM（约 16 KiB），不清掉指针以免重启泄漏。
 */
static bool route_sim_pts_ensure(route_sim_t * sim)
{
    const size_t bytes = (size_t)VMAP_ROUTE_MAX_PTS * sizeof(float);

    if (sim->lon != NULL && sim->lat != NULL) {
        return true;
    }

    if (sim->lon == NULL) {
        sim->lon = vmap_malloc(bytes);
    }

    if (sim->lat == NULL) {
        sim->lat = vmap_malloc(bytes);
    }

    if (sim->lon == NULL || sim->lat == NULL) {
        vmap_free(sim->lon);
        vmap_free(sim->lat);
        sim->lon = NULL;
        sim->lat = NULL;
        return false;
    }

    return true;
}

/**
 * @brief 清状态但保留 lon/lat 缓冲。
 */
static void route_sim_reset_keep_pts(route_sim_t * sim)
{
    float * lon = sim->lon;
    float * lat = sim->lat;

    memset(sim, 0, sizeof(*sim));
    sim->lon = lon;
    sim->lat = lat;
}

static double geo_distance_m_f(float lat1, float lon1, float lat2, float lon2)
{
    const double r = 6371000.0;
    const double dlat = (double)(lat2 - lat1) * M_PI / 180.0;
    const double dlon = (double)(lon2 - lon1) * M_PI / 180.0;
    const double lat1r = (double)lat1 * M_PI / 180.0;
    const double lat2r = (double)lat2 * M_PI / 180.0;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0)
        + cos(lat1r) * cos(lat2r) * sin(dlon / 2.0) * sin(dlon / 2.0);

    return r * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static double geo_distance_m(float lat1, float lon1, float lat2, float lon2)
{
    return geo_distance_m_f(lat1, lon1, lat2, lon2);
}

static float geo_course_deg(float lat1, float lon1, float lat2, float lon2)
{
    const double dlon = (double)(lon2 - lon1) * M_PI / 180.0;
    const double lat1r = (double)lat1 * M_PI / 180.0;
    const double lat2r = (double)lat2 * M_PI / 180.0;
    const double y = sin(dlon) * cos(lat2r);
    const double x = cos(lat1r) * sin(lat2r) - sin(lat1r) * cos(lat2r) * cos(dlon);
    double brng = atan2(y, x) * 180.0 / M_PI;

    if (brng < 0.0) {
        brng += 360.0;
    }
    return (float)brng;
}

static float sim_speed_ramp(const gpx_sim_t * sim)
{
    const double cycle_sec = (double)VMAP_GNSS_SIM_RAMP_SEC * 2.0;
    const double ramp_sec = (double)VMAP_GNSS_SIM_RAMP_SEC;
    const double t = (double)sim->sim_elapsed_ms / 1000.0;
    double phase;
    float u;

    if (cycle_sec < 1.0 || ramp_sec < 1.0) {
        return sim->speed_min_kph;
    }

    phase = fmod(t, cycle_sec);
    if (phase < 0.0) {
        phase += cycle_sec;
    }

    if (phase < ramp_sec) {
        u = (float)(phase / ramp_sec);
        return sim->speed_max_kph - (sim->speed_max_kph - sim->speed_min_kph) * u;
    }

    u = (float)((phase - ramp_sec) / ramp_sec);
    return sim->speed_min_kph + (sim->speed_max_kph - sim->speed_min_kph) * u;
}

static void lerp_point(const gpx_point_t * a, const gpx_point_t * b, double t,
    gpx_point_t * out)
{
    out->latitude = a->latitude + (float)((b->latitude - a->latitude) * t);
    out->longitude = a->longitude + (float)((b->longitude - a->longitude) * t);
    out->altitude = a->altitude + (float)((b->altitude - a->altitude) * t);
    out->has_altitude = a->has_altitude || b->has_altitude;
    out->has_time = a->has_time;
    out->time = a->time;
}

static void sim_pt_to_gpx(const gpx_sim_pt_t * src, gpx_point_t * out)
{
    memset(out, 0, sizeof(*out));
    if (src == NULL || out == NULL) {
        return;
    }

    out->longitude = src->lon;
    out->latitude = src->lat;
    out->altitude = src->alt;
    out->time = src->time;
    out->has_altitude = src->has_alt;
    out->has_time = src->has_time;
}

static int64_t sim_time_sec(const gpx_time_t * t)
{
    if (t == NULL || t->year < 1970 || t->month < 1 || t->month > 12) {
        return -1;
    }

    return (int64_t)t->year * 366LL * 86400LL
        + (int64_t)t->month * 31LL * 86400LL
        + (int64_t)t->day * 86400LL
        + (int64_t)t->hour * 3600LL
        + (int64_t)t->minute * 60LL
        + (int64_t)t->second;
}

static uint32_t sim_prefix_idx(const gpx_sim_t * sim, uint32_t i)
{
    if (sim == NULL || sim->pt_count == 0u || i >= sim->prefix_n) {
        return 0;
    }

    if (sim->reverse) {
        return sim->pt_count - 1u - i;
    }

    return i;
}

static void gpx_sim_seek_distance(gpx_sim_t * sim, double skip_m)
{
    double acc = 0.0;
    uint32_t i;
    int64_t t0;
    int64_t t1;

    sim->prefix_n = 0;
    sim->prefix_m = 0.0;
    sim->prefix_ms = 0;

    if (sim == NULL || sim->pts == NULL || sim->pt_count == 0u) {
        return;
    }

    if (skip_m < 0.5) {
        sim->idx = sim->reverse && sim->pt_count > 0u ? sim->pt_count - 1u : 0u;
        return;
    }

    if (!sim->reverse) {
        sim->idx = 0;
        sim->prefix_n = 1;
        for (i = 1; i < sim->pt_count; i++) {
            acc += geo_distance_m(sim->pts[i - 1u].lat, sim->pts[i - 1u].lon,
                sim->pts[i].lat, sim->pts[i].lon);
            sim->idx = i;
            sim->prefix_n = i + 1u;
            if (acc >= skip_m) {
                break;
            }
        }
    } else {
        sim->idx = sim->pt_count - 1u;
        sim->prefix_n = 1;
        for (i = sim->pt_count - 1u; i > 0u; i--) {
            acc += geo_distance_m(sim->pts[i].lat, sim->pts[i].lon,
                sim->pts[i - 1u].lat, sim->pts[i - 1u].lon);
            sim->idx = i - 1u;
            sim->prefix_n++;
            if (acc >= skip_m) {
                break;
            }
        }
    }

    sim->prefix_m = acc;
    t0 = (sim->pts[sim_prefix_idx(sim, 0)].has_time)
        ? sim_time_sec(&sim->pts[sim_prefix_idx(sim, 0)].time) : -1;
    t1 = (sim->pts[sim->idx].has_time)
        ? sim_time_sec(&sim->pts[sim->idx].time) : -1;
    if (t0 >= 0 && t1 >= 0) {
        int64_t dt = t1 - t0;

        if (dt < 0) {
            dt = -dt;
        }
        if (dt > 0 && dt < 48LL * 3600LL) {
            sim->prefix_ms = (uint32_t)dt * 1000u;
        }
    }

    if (sim->prefix_ms == 0u && acc > 0.5) {
        sim->prefix_ms = (uint32_t)(acc / (22.0 / 3.6) * 1000.0);
    }
}

static uint32_t sim_next_idx(const gpx_sim_t * sim, uint32_t i)
{
    if (sim == NULL || sim->pt_count == 0) {
        return 0;
    }

    if (sim->reverse) {
        return (i == 0) ? i : i - 1u;
    }

    return (i + 1u >= sim->pt_count) ? i : i + 1u;
}

static void gpx_sim_free_pts(gpx_sim_t * sim)
{
    if (sim == NULL || sim->pts == NULL) {
        return;
    }

    gpx_port_free(sim->pts);
    sim->pts = NULL;
    sim->pt_count = 0;
}

static int gpx_sim_load_file(gpx_sim_t * sim)
{
    gpx_decode_t * dec = NULL;
    gpx_decode_cfg_t cfg;
    gpx_point_t batch[16];
    gpx_sim_pt_t * buf;
    unsigned n;
    int ret;
    uint32_t got = 0;

    memset(batch, 0, sizeof(batch));
    buf = gpx_port_calloc(GPX_SIM_MAX_PTS, sizeof(*buf));
    if (buf == NULL) {
        return -ENOMEM;
    }

    gpx_decode_cfg_sparse(&cfg, GPX_SIM_MAX_PTS);
    cfg.batch_max = 16;
    if (gpx_decode_open(sim->path, &cfg, &dec) != 0) {
        gpx_port_free(buf);
        syslog(LOG_WARNING, "gpx_sim: open %s failed\n", sim->path);
        return -ENOENT;
    }

    for (;;) {
        unsigned i;

        ret = gpx_decode_read(dec, batch, 16, &n);
        myvendor_watchdog_ui_beat();
        myvendor_watchdog_work_beat();
        if (ret < 0) {
            gpx_decode_close(&dec);
            gpx_port_free(buf);
            return -EIO;
        }

        for (i = 0; i < n && got < GPX_SIM_MAX_PTS; i++) {
            buf[got].lon = batch[i].longitude;
            buf[got].lat = batch[i].latitude;
            buf[got].alt = batch[i].altitude;
            buf[got].time = batch[i].time;
            buf[got].has_alt = batch[i].has_altitude;
            buf[got].has_time = batch[i].has_time;
            got++;
        }

        if (ret == 1) {
            break;
        }
    }

    gpx_decode_close(&dec);
    if (got < 1u) {
        gpx_port_free(buf);
        syslog(LOG_WARNING, "gpx_sim: no trkpt in %s\n", sim->path);
        return -ENODATA;
    }

    sim->pts = buf;
    sim->pt_count = got;
    syslog(LOG_INFO, "gpx_sim: loaded %u pts from %s\n",
           (unsigned)got, sim->path);
    return 0;
}

/** @brief 往返循环标志（默认关；NSH `gnss sim ... loop` 置开）。 */
static volatile bool g_gpx_loop;

/**
 * @brief  往返循环开关（长测用）：到端点后**掉头**继续，而不是停在端点。
 * @param  on true = 开。
 * @note   用文件级标志、而不是每次 start 的参数：NSH 侧 `gnss sim ... loop` 在"设标志"与
 *         "UI 真正 start"之间隔着一次 ctl 投递（跨任务），而标志只在**到端点**时才被读，
 *         所以这个顺序是安全的。
 * @note   掉头（ping-pong）而不是跳回起点：跳回起点会造成一次上千公里的瞬移 ⇒ 瓦片整批
 *         重载 + 全图重绘（长测里就是周期性尖峰），掉头只是继续沿轨迹往回骑。
 */
void bicycle_gpx_sim_set_loop(bool on)
{
    g_gpx_loop = on;
}

/** @brief 往返循环是否已打开（供日志/调试用）。 */
bool bicycle_gpx_sim_loop_enabled(void)
{
    return g_gpx_loop;
}

/**
 * @brief 取轨迹上的下一个点；到端点时按循环开关决定"掉头"还是"结束"。
 * @param sim 回放状态。
 * @retval true 已把下一个点装进 sim->next；false 到头了（或数据不足）。
 * @note 掉头时打一行 INFO（`gpx_sim: loop turn -> fwd/rev at idx=i/n`）——长测里就是
 *       "它在循环"的唯一证据，一次往返一行。
 */
static bool load_next_point(gpx_sim_t * sim)
{
    uint32_t n;

    if (sim == NULL || sim->pts == NULL || sim->pt_count == 0) {
        return false;
    }

    n = sim_next_idx(sim, sim->idx);
    if (n == sim->idx) {
        if (g_gpx_loop && sim->pt_count >= 2u) {
            sim->reverse = !sim->reverse;
            n = sim_next_idx(sim, sim->idx);
            syslog(LOG_INFO, "gpx_sim: loop turn -> %s at idx=%u/%u\n",
                   sim->reverse ? "rev" : "fwd", (unsigned)sim->idx,
                   (unsigned)sim->pt_count);
        }
        if (n == sim->idx) {
            sim->has_next = false;
            return false;
        }
    }

    sim->idx = n;
    sim_pt_to_gpx(&sim->pts[n], &sim->next);
    sim->has_next = true;
    return true;
}

static void sync_segment(gpx_sim_t * sim)
{
    sim->seg_len_m = geo_distance_m(sim->cur.latitude, sim->cur.longitude,
        sim->next.latitude, sim->next.longitude);
    sim->seg_done_m = 0.0;
    if (sim->seg_len_m > 0.5) {
        sim->seg_course = geo_course_deg(sim->cur.latitude, sim->cur.longitude,
            sim->next.latitude, sim->next.longitude);
    }

    sim->seg_speed_kph = sim_speed_ramp(sim);
}

static void apply_cur(gpx_sim_t * sim)
{
    sim->fix.longitude = sim->cur.longitude;
    sim->fix.latitude = sim->cur.latitude;
    sim->fix.speed_kph = sim->seg_speed_kph;
    sim->fix.course_deg = (sim->has_next && sim->seg_len_m > 0.5)
        ? sim->seg_course : sim->fix.course_deg;
    sim->fix.altitude_m = sim->cur.altitude;
    sim->fix.has_altitude = sim->cur.has_altitude;
    sim->fix.valid = true;
    sim->fix.alive = true;
    sim->fix.satellites = 10;
    sim->fix.fix_quality = 2;
    sim->fix.hdop_x10 = 10;
    sim->fix.rx_hz = sim_rx_hz(sim->period_ms);
}

static void parser_update(gpx_sim_t * sim)
{
    double step_m;
    double remaining;
    int guard = 0;

    sim->sim_elapsed_ms += sim->period_ms;
    sim->seg_speed_kph = sim_speed_ramp(sim);

    step_m = (sim->seg_speed_kph / 3.6) * ((double)sim->period_ms / 1000.0);
    remaining = step_m;

    while (remaining > 1e-4) {
        if (++guard > 4096) {
            break;
        }

        if (!sim->has_next) {
            if (!load_next_point(sim)) {
                break;
            }
            sync_segment(sim);
        }

        if (sim->seg_len_m < 0.5) {
            sim->cur = sim->next;
            sim->has_next = false;
            remaining = 0.0;
            continue;
        }

        {
            const double seg_left = sim->seg_len_m - sim->seg_done_m;
            if (remaining >= seg_left) {
                remaining -= seg_left;
                sim->cur = sim->next;
                sim->has_next = false;
                sim->seg_done_m = 0.0;
            } else {
                sim->seg_done_m += remaining;
                remaining = 0.0;

                if (sim->seg_done_m >= sim->seg_len_m - 1e-9) {
                    sim->cur = sim->next;
                    sim->has_next = false;
                    sim->seg_done_m = 0.0;
                } else {
                    const double t = sim->seg_done_m / sim->seg_len_m;
                    lerp_point(&sim->cur, &sim->next, t, &sim->cur);
                }
            }
        }
    }

    pthread_mutex_lock(&sim->lock);
    apply_cur(sim);
    pthread_mutex_unlock(&sim->lock);
}

static void * gpx_thread(void * arg)
{
    gpx_sim_t * sim = (gpx_sim_t *)arg;

    while (sim->thread_run) {
        parser_update(sim);
        usleep((useconds_t)sim->period_ms * 1000u);
    }
    return NULL;
}

static void route_sim_apply_cur(route_sim_t * sim, float lon, float lat)
{
    sim->fix.longitude = lon;
    sim->fix.latitude = lat;
    sim->fix.speed_kph = sim->speed_kph;
    sim->fix.course_deg = (sim->seg_idx + 1 < sim->pt_count && sim->seg_len_m > 0.5)
        ? sim->seg_course : sim->fix.course_deg;
    sim->fix.valid = sim->pt_count >= 2;
    sim->fix.alive = sim->fix.valid;
    sim->fix.satellites = 10;
    sim->fix.fix_quality = sim->fix.valid ? 2 : 0;
    sim->fix.hdop_x10 = sim->fix.valid ? 10 : 0;
    sim->fix.rx_hz = sim->fix.valid ? sim_rx_hz(sim->period_ms) : 0;
}

static void route_sim_sync_segment(route_sim_t * sim)
{
    if (sim->seg_idx + 1 >= sim->pt_count) {
        sim->seg_len_m = 0.0;
        sim->seg_done_m = 0.0;
        return;
    }

    sim->seg_len_m = geo_distance_m_f(sim->lat[sim->seg_idx], sim->lon[sim->seg_idx],
        sim->lat[sim->seg_idx + 1], sim->lon[sim->seg_idx + 1]);
    if (sim->seg_done_m > sim->seg_len_m) {
        sim->seg_done_m = sim->seg_len_m;
    }
    if (sim->seg_len_m > 0.5) {
        sim->seg_course = geo_course_deg(sim->lat[sim->seg_idx], sim->lon[sim->seg_idx],
            sim->lat[sim->seg_idx + 1], sim->lon[sim->seg_idx + 1]);
    }
}

static void route_sim_copy_pts(route_sim_t * sim, const bicycle_nav_pt_t * pts,
    uint32_t pt_count)
{
    uint32_t i;

    if (pt_count > VMAP_ROUTE_MAX_PTS) {
        pt_count = VMAP_ROUTE_MAX_PTS;
    }
    sim->pt_count = pt_count;
    for (i = 0; i < pt_count; i++) {
        sim->lon[i] = (float)pts[i].lon;
        sim->lat[i] = (float)pts[i].lat;
    }
}

static void route_sim_position_at(const route_sim_t * sim, float * out_lon,
    float * out_lat)
{
    if (!sim || !out_lon || !out_lat || sim->pt_count == 0) {
        return;
    }
    if (sim->seg_idx + 1 >= sim->pt_count) {
        *out_lon = sim->lon[sim->pt_count - 1];
        *out_lat = sim->lat[sim->pt_count - 1];
        return;
    }
    if (sim->seg_len_m < 0.5) {
        *out_lon = sim->lon[sim->seg_idx];
        *out_lat = sim->lat[sim->seg_idx];
        return;
    }
    {
        const double t = sim->seg_done_m / sim->seg_len_m;
        const float lon_a = sim->lon[sim->seg_idx];
        const float lat_a = sim->lat[sim->seg_idx];
        const float lon_b = sim->lon[sim->seg_idx + 1];
        const float lat_b = sim->lat[sim->seg_idx + 1];

        *out_lon = lon_a + (float)((lon_b - lon_a) * t);
        *out_lat = lat_a + (float)((lat_b - lat_a) * t);
    }
}

static void route_sim_seek_position(route_sim_t * sim, float lon, float lat)
{
    uint32_t best_i = 0;
    double best_d = 1e18;
    double best_done = 0.0;

    if (sim->pt_count < 2) {
        return;
    }

    for (uint32_t i = 0; i + 1 < sim->pt_count; i++) {
        const float lat_a = sim->lat[i];
        const float lon_a = sim->lon[i];
        const float lat_b = sim->lat[i + 1];
        const float lon_b = sim->lon[i + 1];
        const double seg_len = geo_distance_m_f(lat_a, lon_a, lat_b, lon_b);
        double done = 0.0;
        double dist;

        if (seg_len < 0.5) {
            dist = geo_distance_m_f(lat, lon, lat_b, lon_b);
        } else {
            const double dlon = (double)(lon_b - lon_a);
            const double dlat = (double)(lat_b - lat_a);
            const double tlon = (double)lon - (double)lon_a;
            const double tlat = (double)lat - (double)lat_a;
            const double dot = tlon * dlon + tlat * dlat;
            const double len2 = dlon * dlon + dlat * dlat;
            double t = len2 > 1e-18 ? dot / len2 : 0.0;

            if (t < 0.0) {
                t = 0.0;
            } else if (t > 1.0) {
                t = 1.0;
            }
            {
                const float mid_lat = lat_a + (float)((lat_b - lat_a) * t);
                const float mid_lon = lon_a + (float)((lon_b - lon_a) * t);

                dist = geo_distance_m_f(lat, lon, mid_lat, mid_lon);
                done = seg_len * t;
            }
        }

        if (dist < best_d) {
            best_d = dist;
            best_i = i;
            best_done = done;
        }
    }

    sim->seg_idx = best_i;
    sim->seg_done_m = best_done;
    route_sim_sync_segment(sim);
}

static void route_sim_update(route_sim_t * sim)
{
    double step_m;
    double remaining;
    float lon = sim->fix.longitude;
    float lat = sim->fix.latitude;
    int guard = 0;

    if (sim->pt_count < 2) {
        return;
    }

    step_m = (sim->speed_kph / 3.6) * ((double)sim->period_ms / 1000.0);
    remaining = step_m;

    while (remaining > 1e-4) {
        if (++guard > 4096) {
            break;
        }

        if (sim->seg_idx + 1 >= sim->pt_count) {
            lon = sim->lon[sim->pt_count - 1];
            lat = sim->lat[sim->pt_count - 1];
            remaining = 0.0;
            break;
        }

        if (sim->seg_len_m < 0.5) {
            sim->seg_idx++;
            sim->seg_done_m = 0.0;
            route_sim_sync_segment(sim);
            continue;
        }

        {
            const double seg_left = sim->seg_len_m - sim->seg_done_m;

            if (remaining >= seg_left) {
                remaining -= seg_left;
                sim->seg_idx++;
                sim->seg_done_m = 0.0;
                lon = sim->lon[sim->seg_idx];
                lat = sim->lat[sim->seg_idx];
                route_sim_sync_segment(sim);
            } else {
                sim->seg_done_m += remaining;
                remaining = 0.0;
                {
                    const double t = sim->seg_done_m / sim->seg_len_m;
                    const float lon_a = sim->lon[sim->seg_idx];
                    const float lat_a = sim->lat[sim->seg_idx];
                    const float lon_b = sim->lon[sim->seg_idx + 1];
                    const float lat_b = sim->lat[sim->seg_idx + 1];

                    lon = lon_a + (float)((lon_b - lon_a) * t);
                    lat = lat_a + (float)((lat_b - lat_a) * t);
                }
            }
        }
    }

    pthread_mutex_lock(&sim->lock);
    route_sim_apply_cur(sim, lon, lat);
    pthread_mutex_unlock(&sim->lock);
}

static void * route_thread(void * arg)
{
    route_sim_t * sim = (route_sim_t *)arg;

    while (sim->thread_run) {
        route_sim_update(sim);
        usleep((useconds_t)sim->period_ms * 1000u);
    }
    return NULL;
}

static int route_sim_start_locked(route_sim_t * sim, const bicycle_nav_pt_t * pts,
    uint32_t pt_count, float speed_kph, unsigned period_ms, bool seek)
{
    pthread_attr_t attr;
    void *stack;
    float start_lon;
    float start_lat;

    if (!pts || pt_count < 2) {
        return -1;
    }

    if (sim->thread_alive) {
        sim->thread_run = false;
        pthread_join(sim->thread, NULL);
        sim->thread_alive = false;
        sim_stack_free(&g_route_sim_stack_raw);
        pthread_mutex_destroy(&sim->lock);
        route_sim_reset_keep_pts(sim);
    }

    if (!route_sim_pts_ensure(sim)) {
        syslog(LOG_ERR, "route_sim: BoardPSRAM pts alloc failed\n");
        return -ENOMEM;
    }

    stack = sim_stack_alloc(&g_route_sim_stack_raw);
    if (stack == NULL) {
        syslog(LOG_ERR, "route_sim: BoardPSRAM stack alloc failed\n");
        return -ENOMEM;
    }

    pthread_mutex_init(&sim->lock, NULL);
    route_sim_copy_pts(sim, pts, pt_count);
    sim->speed_kph = speed_kph > 0.0f ? speed_kph : VMAP_NAV_SIM_SPEED_KPH;
    sim->period_ms = period_ms > 0 ? period_ms : VMAP_GNSS_UPDATE_MS;
    sim->seg_idx = 0;
    sim->seg_done_m = 0.0;
    start_lon = sim->lon[0];
    start_lat = sim->lat[0];
    route_sim_sync_segment(sim);
    route_sim_apply_cur(sim, start_lon, start_lat);

    if (seek) {
        route_sim_seek_position(sim, start_lon, start_lat);
        route_sim_apply_cur(sim, sim->fix.longitude, sim->fix.latitude);
    }

    sim->thread_run = true;
    pthread_attr_init(&attr);
    if (pthread_attr_setstack(&attr, stack,
            CONFIG_GPX_DECODE_STACKSIZE) != 0 ||
        pthread_create(&sim->thread, &attr, route_thread, sim) != 0) {
        sim->thread_run = false;
        pthread_mutex_destroy(&sim->lock);
        route_sim_reset_keep_pts(sim);
        sim_stack_free(&g_route_sim_stack_raw);
        pthread_attr_destroy(&attr);
        return -1;
    }
#ifndef CONFIG_DISABLE_PTHREAD
    pthread_setname_np(sim->thread, "bicycle_nav");
#endif
    sim->thread_alive = true;
    pthread_attr_destroy(&attr);
    return 0;
}

/**
 * @brief 自行车 route sim start。
 */
int bicycle_route_sim_start(const bicycle_nav_pt_t * pts, uint32_t pt_count,
    float speed_kph, unsigned period_ms)
{
    return route_sim_start_locked(&g_route, pts, pt_count, speed_kph, period_ms, false);
}

/**
 * @brief 自行车 route sim reload。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_route_sim_reload(const bicycle_nav_pt_t * pts, uint32_t pt_count)
{
    float cur_lon = 0.f;
    float cur_lat = 0.f;
    float pos_lon = 0.f;
    float pos_lat = 0.f;

    if (!g_route.thread_run || !pts || pt_count < 2) {
        return;
    }

    pthread_mutex_lock(&g_route.lock);
    cur_lon = g_route.fix.longitude;
    cur_lat = g_route.fix.latitude;
    pthread_mutex_unlock(&g_route.lock);

    pos_lon = cur_lon;
    pos_lat = cur_lat;
    route_sim_copy_pts(&g_route, pts, pt_count);
    route_sim_seek_position(&g_route, cur_lon, cur_lat);
    route_sim_position_at(&g_route, &pos_lon, &pos_lat);
    pthread_mutex_lock(&g_route.lock);
    route_sim_apply_cur(&g_route, pos_lon, pos_lat);
    pthread_mutex_unlock(&g_route.lock);
}

/**
 * @brief 自行车 route sim stop。
 */
void bicycle_route_sim_stop(void)
{
    if (!g_route.thread_alive) {
        return;
    }
    g_route.thread_run = false;
    pthread_join(g_route.thread, NULL);
    g_route.thread_alive = false;
    sim_stack_free(&g_route_sim_stack_raw);
    pthread_mutex_destroy(&g_route.lock);
    route_sim_reset_keep_pts(&g_route);
}

/**
 * @brief 自行车 route sim active。
 */
bool bicycle_route_sim_active(void)
{
    return g_route.thread_run;
}

/**
 * @brief 自行车 route sim set speed。
 */
void bicycle_route_sim_set_speed(float speed_kph)
{
    if (!g_route.thread_run) {
        return;
    }
    if (speed_kph < 5.0f) {
        speed_kph = 5.0f;
    } else if (speed_kph > 360.0f) {
        speed_kph = 360.0f;
    }
    pthread_mutex_lock(&g_route.lock);
    g_route.speed_kph = speed_kph;
    g_route.fix.speed_kph = speed_kph;
    pthread_mutex_unlock(&g_route.lock);
}

static bool route_sim_get_fix(bicycle_gnss_fix_t * out)
{
    if (!out || !g_route.thread_run) {
        return false;
    }

    pthread_mutex_lock(&g_route.lock);
    *out = g_route.fix;
    pthread_mutex_unlock(&g_route.lock);
    return out->valid;
}

/**
 * @brief 自行车 gpx sim start。
 */
int bicycle_gpx_sim_start(const char * path, float speed_kph, unsigned period_ms,
    bool reverse, double skip_m)
{
    pthread_attr_t attr;
    void *stack;
    int ret;

    if (g_sim.thread_alive) {
        bicycle_gpx_sim_stop();
    }

    memset(&g_sim, 0, sizeof(g_sim));
    pthread_mutex_init(&g_sim.lock, NULL);
    g_sim.reverse = reverse;
    if (speed_kph >= 1.0f) {
        if (speed_kph > 360.0f) {
            speed_kph = 360.0f;
        }

        g_sim.speed_min_kph = speed_kph;
        g_sim.speed_max_kph = speed_kph;
    } else {
        g_sim.speed_min_kph = VMAP_GNSS_SIM_SPEED_KPH;
        g_sim.speed_max_kph = VMAP_GNSS_SIM_SPEED_MAX_KPH;
        if (g_sim.speed_max_kph < g_sim.speed_min_kph + 1.0f) {
            g_sim.speed_max_kph = g_sim.speed_min_kph + 1.0f;
        }
    }

    g_sim.seg_speed_kph = g_sim.speed_min_kph;
    g_sim.sim_elapsed_ms = 0;
    g_sim.period_ms = period_ms > 0 ? period_ms : VMAP_GNSS_UPDATE_MS;
    lv_strlcpy(g_sim.path, path ? path : VMAP_GPX_PATH, sizeof(g_sim.path));

    ret = gpx_sim_load_file(&g_sim);
    if (ret != 0) {
        pthread_mutex_destroy(&g_sim.lock);
        memset(&g_sim, 0, sizeof(g_sim));
        return ret;
    }

    gpx_sim_seek_distance(&g_sim, skip_m);
    sim_pt_to_gpx(&g_sim.pts[g_sim.idx], &g_sim.cur);
    g_sim.has_next = false;
    apply_cur(&g_sim);

    if (load_next_point(&g_sim)) {
        sync_segment(&g_sim);
    }

    stack = sim_stack_alloc(&g_gpx_sim_stack_raw);
    if (stack == NULL) {
        syslog(LOG_ERR, "gpx_sim: BoardPSRAM stack alloc failed\n");
        gpx_sim_free_pts(&g_sim);
        pthread_mutex_destroy(&g_sim.lock);
        memset(&g_sim, 0, sizeof(g_sim));
        return -ENOMEM;
    }

    g_sim.thread_run = true;
    pthread_attr_init(&attr);
    if (pthread_attr_setstack(&attr, stack,
            CONFIG_GPX_DECODE_STACKSIZE) != 0 ||
        pthread_create(&g_sim.thread, &attr, gpx_thread, &g_sim) != 0) {
        g_sim.thread_run = false;
        gpx_sim_free_pts(&g_sim);
        pthread_mutex_destroy(&g_sim.lock);
        sim_stack_free(&g_gpx_sim_stack_raw);
        pthread_attr_destroy(&attr);
        memset(&g_sim, 0, sizeof(g_sim));
        return -1;
    }
#ifndef CONFIG_DISABLE_PTHREAD
    pthread_setname_np(g_sim.thread, "bicycle_gpx");
#endif
    g_sim.thread_alive = true;
    pthread_attr_destroy(&attr);
    syslog(LOG_INFO, "gpx_sim: start %s %s @ %.0f-%.0f km/h skip=%.0fm idx=%u/%u\n",
           g_sim.path, reverse ? "rev" : "fwd",
           (double)g_sim.speed_min_kph, (double)g_sim.speed_max_kph,
           g_sim.prefix_m, (unsigned)g_sim.idx, (unsigned)g_sim.pt_count);
    return 0;
}

double bicycle_gpx_sim_prefix_m(void)
{
    return g_sim.thread_run ? g_sim.prefix_m : 0.0;
}

uint32_t bicycle_gpx_sim_prefix_ms(void)
{
    return g_sim.thread_run ? g_sim.prefix_ms : 0u;
}

uint32_t bicycle_gpx_sim_prefix_count(void)
{
    return g_sim.thread_run ? g_sim.prefix_n : 0u;
}

bool bicycle_gpx_sim_prefix_point(uint32_t i, float * lon, float * lat,
    float * speed_kph)
{
    uint32_t a;
    uint32_t b;
    float kph = 22.0f;

    if (!g_sim.thread_run || g_sim.pts == NULL || i >= g_sim.prefix_n
        || !lon || !lat) {
        return false;
    }

    a = sim_prefix_idx(&g_sim, i);
    *lon = g_sim.pts[a].lon;
    *lat = g_sim.pts[a].lat;

    if (i + 1u < g_sim.prefix_n) {
        double dm;
        int64_t t0;
        int64_t t1;

        b = sim_prefix_idx(&g_sim, i + 1u);
        dm = geo_distance_m(g_sim.pts[a].lat, g_sim.pts[a].lon,
            g_sim.pts[b].lat, g_sim.pts[b].lon);
        t0 = g_sim.pts[a].has_time ? sim_time_sec(&g_sim.pts[a].time) : -1;
        t1 = g_sim.pts[b].has_time ? sim_time_sec(&g_sim.pts[b].time) : -1;
        if (t0 >= 0 && t1 >= 0 && t0 != t1 && dm > 0.5) {
            int64_t dt = t1 - t0;

            if (dt < 0) {
                dt = -dt;
            }
            if (dt > 0 && dt < 3600) {
                kph = (float)((dm / (double)dt) * 3.6);
            }
        }
    }

    if (kph < 1.0f) {
        kph = 1.0f;
    } else if (kph > 80.0f) {
        kph = 80.0f;
    }

    if (speed_kph) {
        *speed_kph = kph;
    }
    return true;
}

/**
 * @brief 自行车 gpx sim stop。
 */
void bicycle_gpx_sim_stop(void)
{
    if (!g_sim.thread_alive) {
        return;
    }
    g_sim.thread_run = false;
    pthread_join(g_sim.thread, NULL);
    g_sim.thread_alive = false;
    sim_stack_free(&g_gpx_sim_stack_raw);
    gpx_sim_free_pts(&g_sim);
    pthread_mutex_destroy(&g_sim.lock);
    memset(&g_sim, 0, sizeof(g_sim));
}

/**
 * @brief 自行车 gpx sim active。
 */
bool bicycle_gpx_sim_active(void)
{
    return g_sim.thread_run;
}

/* ======================= 全国跳（调试用合成定位） =======================
 * @brief  伪造 GNSS 定位在固定城市序列间大跳，造可重复的渲染负载。
 *
 * @details 为什么有它：瓦片是**按需重绘**的 —— 小幅平移常常一个新瓦片都不需要，
 *          `[vperf] frame` 就不出帧（实测：15-40 km/h 的 GPX 回放跑 45 s 只有 1 帧）。
 *          要可重复地压满"瓦片整批作废 + 重新加载 + 全屏重绘"这条路径，就得让定位
 *          **大跳**（上千公里）。
 *          用法（NSH）：`bicycle_nsh gnss hop [period_ms] [count]` 开始（count=0 不限次）、
 *          `bicycle_nsh gnss hop stop` 停止（随后回到真实 GNSS）。
 *
 * @note    实现取舍：**不建线程、不建定时器** —— getter 每次被调用时按"距上次换点的时间"
 *          决定要不要跳到下一个城市（调用方是 `bicycle_runtime.c` 的定位更新，≥1 Hz）。
 *          城市表是固定序列 ⇒ 两次跑的顺序一致（A/B 需要的可重复性）。
 *
 * @warning 跨任务只共享几个标量，竞态后果最多是"某个点多/少停留一档"，调试命令可接受；
 *          **不要**把它当产品功能用。另：跳的时候定位是真的在瞬移，若此时在录轨，
 *          那段轨迹是脏的。
 * ====================================================================== */
#include <nuttx/clock.h>

static const bicycle_gnss_hop_pt_t g_hop_pts[] = {
    { "北京",     39.9042f, 116.4074f },
    { "上海",     31.2304f, 121.4737f },
    { "广州",     23.1291f, 113.2644f },
    { "成都",     30.5728f, 104.0668f },
    { "乌鲁木齐", 43.8256f,  87.6168f },
    { "哈尔滨",   45.8038f, 126.5350f },
    { "拉萨",     29.6520f,  91.1721f },
    { "三亚",     18.2528f, 109.5119f },
    { "昆明",     25.0389f, 102.7183f },
    { "西安",     34.3416f, 108.9398f },
};

#define BICYCLE_GNSS_HOP_N (sizeof(g_hop_pts) / sizeof(g_hop_pts[0]))

/**
 * @brief 自定义点表（`bicycle_gnss_hop_set_pts()`；演示模式用它跳风景点）。
 * @note 只在**一次**跳点期间有效：`hop_stop()` 与"跳够次数自动停"两处都清掉 ⇒
 *       `bicycle_nsh gnss hop`（渲染压测）永远还是那串固定城市，口径不被演示改掉。
 */
static const bicycle_gnss_hop_pt_t * g_hop_use;
static uint32_t g_hop_use_n;

#define HOP_N()  (g_hop_use != NULL ? g_hop_use_n : (uint32_t)BICYCLE_GNSS_HOP_N)
#define HOP_PT(i) (&(g_hop_use != NULL ? g_hop_use : g_hop_pts)[(i) % HOP_N()])

static void hop_override_clear(void)
{
    g_hop_use = NULL;
    g_hop_use_n = 0;
}

/** @brief 换一组跳点（演示用：太湖 / 青海湖 这类风景点）。 */
void bicycle_gnss_hop_set_pts(const bicycle_gnss_hop_pt_t * pts, uint32_t n)
{
    if (pts == NULL || n == 0u) {
        hop_override_clear();
        return;
    }

    g_hop_use = pts;
    g_hop_use_n = n;
}

static volatile bool     g_hop_run;
static volatile unsigned g_hop_period_ms;
static volatile unsigned g_hop_count;   /* 0 = 不限次 */
static volatile unsigned g_hop_idx;
static volatile unsigned g_hop_sent;
static uint32_t          g_hop_last_ms;

/**
 * @brief 开始全国跳。
 * @param period_ms 换点周期（下限 200 ms）。
 * @param count     总次数；0 = 不限次（用 bicycle_gnss_hop_stop() 停）。
 */
void bicycle_gnss_hop_start(unsigned period_ms, unsigned count)
{
    g_hop_period_ms = (period_ms < 200u) ? 200u : period_ms;
    g_hop_count     = count;
    g_hop_idx       = 0;
    g_hop_sent      = 0;
    g_hop_last_ms   = 0;   /* 0 表示"下一次调用立刻出点" */
    g_hop_run       = true;
}

/**
 * @brief 停止全国跳；随后回到真实 GNSS（或 GPX 回放，若有）。
 */
void bicycle_gnss_hop_stop(void)
{
    g_hop_run = false;
    hop_override_clear();
}

/** @brief 是否在跳。 */
bool bicycle_gnss_hop_active(void)
{
    return g_hop_run;
}

/** @brief 当前所在城市名（未跳时为 "-"）；仅日志用。 */
const char * bicycle_gnss_hop_cur_name(void)
{
    return g_hop_run ? HOP_PT(g_hop_idx)->name : "-";
}

/**
 * @brief 取当前跳到的定位（在 bicycle_gpx_sim_get_fix() 里最优先）。
 * @param out 输出的定位快照；填一个"干净"的 3D 定位（星数/精度都合理，免得上层当过
 *            异常过滤掉）。
 * @retval true 已填入；false 未在跳（或在本次达到 count 已经自动停）。
 * @note 时间基准用 `clock_systime_ticks()` + `TICK2MSEC`（与 vmap_route_nav.c 一致）。
 */
bool bicycle_gnss_hop_get_fix(bicycle_gnss_fix_t * out)
{
    const bicycle_gnss_hop_pt_t * p;
    uint32_t now;

    if (!g_hop_run || out == NULL) {
        return false;
    }

    now = (uint32_t)TICK2MSEC(clock_systime_ticks());
    if (g_hop_last_ms == 0U || (now - g_hop_last_ms) >= g_hop_period_ms) {
        if (g_hop_last_ms != 0U) {
            g_hop_idx = (g_hop_idx + 1U) % HOP_N();
            g_hop_sent++;
            if (g_hop_count != 0U && g_hop_sent >= g_hop_count) {
                g_hop_run = false;
                hop_override_clear();
                return false;
            }
        }

        g_hop_last_ms = (now == 0U) ? 1U : now;
    }

    p = HOP_PT(g_hop_idx);

    /* 一个"干净"的 3D 定位：星数/精度都填合理值，免得上层把这次定位当异常过滤掉。 */
    memset(out, 0, sizeof(*out));
    out->longitude    = p->lon;
    out->latitude     = p->lat;
    out->speed_kph    = 20.0f;
    out->speed_pvt_kph = 20.0f;
    out->pos_acc_m    = 5.0f;
    out->satellites   = 9;
    out->sats_locked  = 9;
    out->sats_heard   = 9;
    out->fix_quality  = 2;
    out->hdop_x10     = 10;
    out->rx_hz        = 1;
    out->valid        = true;
    out->alive        = true;
    out->pvt_valid    = true;
    out->pvt_gnss_ok  = true;
    out->stamp_ms     = now;
    return true;
}

/**
 * @brief 自行车 gpx sim get fix。
 */
bool bicycle_gpx_sim_get_fix(bicycle_gnss_fix_t * out)
{
    /* 全国跳优先（见上面那段）：它是"最大渲染负载"的定位源，专供 A/B 用。 */
    if (bicycle_gnss_hop_get_fix(out)) {
        return true;
    }

    if (route_sim_get_fix(out)) {
        return true;
    }
    if (!out) {
        return false;
    }
    if (g_sim.thread_run) {
        pthread_mutex_lock(&g_sim.lock);
        *out = g_sim.fix;
        pthread_mutex_unlock(&g_sim.lock);
        return out->valid;
    }

    return false;
}

bool bicycle_gpx_sim_get_wall_hms(int * hour, int * min)
{
    bool ok = false;

    if (hour == NULL || min == NULL) {
        return false;
    }

    if (!g_sim.thread_run) {
        return false;
    }

    pthread_mutex_lock(&g_sim.lock);
    if (g_sim.cur.has_time && g_sim.cur.time.year >= 2020) {
        *hour = (int)g_sim.cur.time.hour;
        *min = (int)g_sim.cur.time.minute;
        ok = true;
    }
    pthread_mutex_unlock(&g_sim.lock);
    return ok;
}
