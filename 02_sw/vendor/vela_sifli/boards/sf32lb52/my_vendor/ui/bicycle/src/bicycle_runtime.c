/**
 * @file bicycle_runtime.c
 * @brief 自行车 UI — runtime。
 */

#include "bicycle_runtime.h"

#include "bicycle_env.h"
#include "bicycle_gpx_sim.h"
#include "myvendor_devctl.h"
#include "myvendor_gnss.h"
#include "myvendor_sys.h"
#include "vmap/vmap_geo.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static bicycle_runtime_t g_rt;
static lv_subject_t g_subj_evt[BICYCLE_SUBJ_EVT_MAX];
static uint32_t g_last_tick_ms;
static uint32_t g_hr_sum;
static uint32_t g_hr_n;
static uint32_t g_hr_last_ms;
static bool g_ui_inited;
static char g_lap_km_buf[12];
static char g_lap_km_prev[12];
static char g_sensor_buf[24];
static char g_sensor_prev[24];

#define BICYCLE_RUNTIME_DATA_BYTES offsetof(bicycle_runtime_t, subj_speed_kph)

/*
 * 定位滤波：
 * - 时速：3D 正常且在动时用 RMC 对地速度（多普勒）。旧实现跟 SOG 但又加
 *   2.2 km/h/s slew，100 km/h 时显示被卡在 ~50，不是 RMC 本身不准。
 *   停着或 3D 低：HDOP 差时坐标乱跳，位移窗会算出 0～10 km/h 假速度，此时
 *   信 RMC 静止门限，钉住、显示 0。
 *   3D 低但 RMC 报高速：多普勒常乱跳到上百 km/h（常见 ~160），坐标几乎
 *   不动，GPX 仍正常。SOG 必须和本拍位移或上一拍对得上，否则不信。
 * - 显示层：静止 SOG 1～2 km/h 压成 0；高速时 RMC 短时无效/SOG 掉到 0
 *   不立刻清零，保持上一有效时速（coast），避免信号一晃就变 0 km/h。
 *   已在动时 SOG 单拍暴涨（+40 km/h）当尖峰丢掉，对称于向下 cliff。
 *   仅 3D 低时丢尖峰：3D 高时汽车/下坡 1 Hz 跳 40+ km/h 是真加速，不是飞点。
 *   置信度低（2D / 3D 低）把位移窗拉到 ~15 s，主要跟窗不跟瞬时 SOG；
 *   3D 高仍以多普勒为主。窗长和混合比滑动，避免 3D 高低跳变时数字跳。
 * - 位置门限按「自上次接受」的时间算，去掉 45 m 封顶。旧封顶会让 100 km/h
 *   连续 skip，钉住 5 点再跳 100 m（GPX 里 0 然后 400 km/h 尖峰）。
 * - 3D 低 / HDOP 差且 SOG 近静止：钉住，飞点不进里程，时速显示 0。
 *   3D 高但 SOG≈0、位移却很快：模组动态模型把速度 sanity 掉了，跟位移，
 *   不要强行显示 0（汽车上均速/轨迹对、时速/极速为 0 就是这条）。
 *   此时轨迹均速已经出来：请求 GNSS 线程把动态模型从自行车切到车载，
 *   让后面的 RMC SOG 不再被 sanity 成 0。
 * - 时速窗只在累计位移增加时打点（对齐 1 Hz NMEA）。同一历元的 200 ms
 *   轮询既不新开点也不滑动时间戳；丢星后用位移窗把时速滑到 0。
 */
#define RT_FILT_HOLD_KPH       2.8f
#define RT_FILT_COURSE_KPH     3.5f
#define RT_FILT_MAX_KPH        320.0f
/* 速度路径的门限（rt_trust_speed / rt_disp_note_fix）。 */
#define RT_FILT_TELEPORT_KPH   280.0f
/** @brief 位置门限：只喂 rt_smooth_fix 里"这个坐标能不能进轨迹"那一判。
 *
 *  @details 与上面那条**速度**门限拆开，是用户 2026-10-01 的裁定：
 *           "轨迹点获取改回 2026-09-15 的样子，时速保持现状"。
 *           2026-09-15 这两个位置门限就是 160 km/h / 50 m/s；2026-09-16 的
 *           `818ce499` 把它们和速度门限一起抬到 280 / 90，于是模组那种
 *           "2 秒 151.85 m（273 km/h）"的坏点从**被拦下**变成**写进 GPX**，
 *           现场表现就是轨迹沿原航向冲出去、再飘回正确位置。
 *           ⚠ 改这里只影响轨迹，不动时速显示。 */
#define RT_FILT_POS_TELEPORT_KPH 160.0f
#define RT_FILT_MAX_MPS        50.0f
#define RT_FILT_INVALID_MS     8000u
#define RT_SPEED_HOLD_INV_MS   2500u
#define RT_SPEED_DISP_HYST     0.25f
#define RT_HDOP_LOW_X10        20
#define RT_SPD_N               40
#define RT_SPD_WIN_MS          5000u
#define RT_SPD_WIN_MAX_MS      8000u
#define RT_SPD_MIN_MS          1000u
#define RT_SPD_COAST_MS        1500u
#define RT_DISP_N              48
#define RT_DISP_WIN_HIGH_MS    3000u
#define RT_DISP_WIN_MID_MS     8000u
#define RT_DISP_WIN_LOW_MS     15000u
#define RT_DISP_WIN_SLACK_MS   4000u
#define RT_DISP_WIN_SLEW_MS_S  8000u
#define RT_DISP_BLEND_SLEW_S   1.6f
#define RT_DISP_SLEW_KPH_S     14.0f
#define RT_DISP_CONF_SLEW_KPH_S 10.0f
#define RT_FILT_STILL_M        0.8f
/* 静止门（位移累加用）：必须**大于位置噪声**，否则噪声会被当成位移。
 * 1 Hz 下定位误差 ±2~5 m，0.8 m 的门等于没有 —— 静止时 cum_m 照涨，
 * 位移窗于是算出几 km/h 的假速度（现场"不动也有 10 km/h"，3D 高→低跳变
 * 时最明显，因为那时显示以位移窗为基准）。行业做法是"半径 + 双阈值迟滞"。
 */
#define RT_STILL_ENTER_M       3.0f   /**< 自锚点位移小于它 = 还在原地 */
#define RT_STILL_EXIT_M        8.0f   /**< 自锚点位移超过它 = 真的走了（迟滞） */
#define RT_STILL_HOLD_MS       4000u  /**< 原地持续这么久才判静止 */
#define RT_FILT_HOLD_MAX_M     15.0
#define RT_FILT_POOR_SOG_KPH   8.0f
#define RT_SPD_CUM_EPS_M       0.05f
#define RT_SOG_STILL_OFF       2.8f
#define RT_SOG_STILL_ON        3.6f
#define RT_SOG_MOVE_KPH        8.0f
#define RT_SOG_ZERO_KPH        1.0f
#define RT_SOG_CLIFF_KPH       20.0f
#define RT_SOG_SPIKE_KPH       40.0f
#define RT_SOG_HOLD_MS         4000u
#define RT_DYN_AUTO_KPH        80.0f
#define RT_DYN_SOG_DEAD_KPH    25.0f
#define RT_DYN_UP_MS           1500u
#define RT_DYN_DOWN_MS         60000u

typedef struct {
    uint32_t ms;
    float cum_m;
} rt_spd_pt_t;

typedef struct {
    bool have;
    float lat;
    float lon;
    float course;
    float speed;
    float cum_m;
    uint32_t last_ms;
    uint32_t invalid_ms;
    rt_spd_pt_t spd[RT_SPD_N];
    uint8_t spd_n;
    uint8_t spd_i;
} rt_gnss_filt_t;

typedef enum {
    RT_CONF_HIGH = 0,
    RT_CONF_MID,
    RT_CONF_LOW
} rt_conf_t;

typedef struct {
    rt_spd_pt_t pt[RT_DISP_N];
    uint8_t n;
    uint8_t i;
    float cum_m;
    float lat;
    float lon;
    bool have;
    uint32_t win_ms;
    uint32_t win_at;
    float blend;
    uint32_t blend_at;
    rt_conf_t conf;
    float last_out;
    uint32_t last_ms;
    uint32_t out_ms;
    uint32_t conf_at;
} rt_disp_spd_t;

static rt_gnss_filt_t g_filt;
static rt_disp_spd_t g_disp;
static float g_speed_disp;
static float g_sog_hold;
static uint32_t g_sog_coast_at;
static uint32_t g_last_seg_ms;
static uint8_t g_dyn_ui = MYVENDOR_GNSS_DYN_BIKE;
static uint32_t g_dyn_hi_at;
static uint32_t g_dyn_lo_at;

static float rt_clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static float rt_absf(float v)
{
    return v < 0.0f ? -v : v;
}

static void rt_disp_reset(void)
{
    memset(&g_disp, 0, sizeof(g_disp));
    g_disp.win_ms = RT_DISP_WIN_HIGH_MS;
    g_disp.blend = 0.85f;
    g_disp.conf = RT_CONF_HIGH;
}

static void rt_dynmodel_request(uint8_t model)
{
    if (g_dyn_ui == model) {
        return;
    }

    g_dyn_ui = model;
    myvendor_gnss_dynmodel_set(model);
}

/**
 * 轨迹位移窗已经算出均速：若明显高于自行车包络，或 SOG 被 sanity 成 0
 * 而轨迹仍在动，切车载模型。停够一分钟再切回自行车。
 */
static void rt_dynmodel_update(float sog, float path, bool path_ok, uint32_t now)
{
    bool path_ready = path_ok && path >= RT_SOG_MOVE_KPH;
    bool want_auto = path_ready
        && (path >= RT_DYN_AUTO_KPH
            || (sog < RT_SOG_STILL_ON && path >= RT_DYN_SOG_DEAD_KPH));

    if (want_auto) {
        g_dyn_lo_at = 0;
        if (g_dyn_hi_at == 0) {
            g_dyn_hi_at = now ? now : 1u;
        }

        if (lv_tick_elaps(g_dyn_hi_at) >= RT_DYN_UP_MS) {
            rt_dynmodel_request(MYVENDOR_GNSS_DYN_AUTO);
        }

        return;
    }

    g_dyn_hi_at = 0;
    if (!path_ok || path < RT_SOG_MOVE_KPH) {
        if (g_dyn_lo_at == 0) {
            g_dyn_lo_at = now ? now : 1u;
        }

        if (lv_tick_elaps(g_dyn_lo_at) >= RT_DYN_DOWN_MS) {
            rt_dynmodel_request(MYVENDOR_GNSS_DYN_BIKE);
        }
    } else {
        g_dyn_lo_at = 0;
    }
}

static float g_still_anchor_m;   /* 自锚点起的累计位移 */
static uint32_t g_still_at;      /* 进入"疑似静止"的时刻，0 = 未计时 */
static bool g_still;             /* 当前是否判为静止 */

/** @brief 静止判定（半径 + 双阈值迟滞），供位移累加与显示共用。
 *
 *  位置噪声在 1 Hz 下 ±2~5 m，若直接把它当位移累进 cum_m，静止时位移窗就会
 *  算出几 km/h 的假速度 —— 低质量（3D 高→低跳变）时更明显，因为那时显示以
 *  位移窗为基准。这里按行业惯例用"位置停在半径内够久即判停"：
 *
 *    - **进入**：自锚点位移 < RT_STILL_ENTER_M 且持续 RT_STILL_HOLD_MS；
 *    - **离开**：自锚点位移 >= RT_STILL_EXIT_M（双阈值，避免在门附近抖）。
 *
 *  @param step_m 本次定位相对上次的位移（米）。
 *  @param now    当前 tick。
 *  @return true = 判为静止（调用方**不要**把它累进位移）。
 */
static bool rt_still_gate(float step_m, uint32_t now)
{
    if (!(step_m > 0.0f)) {
        return g_still;
    }

    g_still_anchor_m += step_m;

    if (g_still) {
        if (g_still_anchor_m >= RT_STILL_EXIT_M) {
            g_still = false;
            g_still_at = 0;
            g_still_anchor_m = 0.0f;
        }
        return g_still;
    }

    if (g_still_anchor_m < RT_STILL_ENTER_M) {
        if (g_still_at == 0) {
            g_still_at = now ? now : 1u;
        } else if ((now - g_still_at) >= RT_STILL_HOLD_MS) {
            g_still = true;
            g_still_anchor_m = 0.0f;
        }
    } else {
        g_still_at = 0;
        g_still_anchor_m = 0.0f;   /* 真的走了：重新锚定 */
    }

    return g_still;
}

/** @brief 清掉静止判定状态（切换速度源 / 丢星复位时调）。 */
static void rt_still_reset(void)
{
    g_still_anchor_m = 0.0f;
    g_still_at = 0;
    g_still = false;
}

static void rt_filt_reset(void)
{
    memset(&g_filt, 0, sizeof(g_filt));
    g_speed_disp = 0.0f;
    g_sog_hold = 0.0f;
    g_sog_coast_at = 0;
    g_last_seg_ms = 0;
    g_dyn_hi_at = 0;
    g_dyn_lo_at = 0;
    rt_still_reset();
}

static bool rt_trust_rmc(void)
{
    return myvendor_devctl_gnss_rmc_get();
}

static void rt_spd_push(uint32_t now, float cum_m)
{
    if (g_filt.spd_n > 0) {
        unsigned last = (unsigned)((g_filt.spd_i + RT_SPD_N - 1u) % RT_SPD_N);
        float dc = cum_m - g_filt.spd[last].cum_m;
        uint32_t dt = now - g_filt.spd[last].ms;

        if (dc < RT_SPD_CUM_EPS_M) {
            return;
        }

        if (dt < 150u) {
            g_filt.spd[last].ms = now;
            g_filt.spd[last].cum_m = cum_m;
            return;
        }
    }

    g_filt.spd[g_filt.spd_i].ms = now;
    g_filt.spd[g_filt.spd_i].cum_m = cum_m;
    g_filt.spd_i = (uint8_t)((g_filt.spd_i + 1u) % RT_SPD_N);
    if (g_filt.spd_n < RT_SPD_N) {
        g_filt.spd_n++;
    }
}

/** 最近约 5 s 行程位移 / 时间 → km/h。窗口两端用位移采样点，不用 UI 轮询时刻。 */
static float rt_path_speed_kph(uint32_t now)
{
    unsigned n = g_filt.spd_n;
    unsigned i;
    unsigned idx;
    unsigned newest;
    unsigned best = 0xffffu;
    int32_t best_err = 0x7fffffff;
    uint32_t t1;
    uint32_t dt_ms;
    int32_t err;
    float c1;
    float d;
    float dt_s;
    float kph;

    if (n < 2) {
        return 0.0f;
    }

    newest = (unsigned)((g_filt.spd_i + RT_SPD_N - 1u) % RT_SPD_N);
    t1 = g_filt.spd[newest].ms;
    c1 = g_filt.spd[newest].cum_m;

    /* 停车后位移不再增加：用墙钟把 5 s 窗滑过停车点，时速掉到 0。 */
    if ((now - t1) >= RT_SPD_COAST_MS) {
        t1 = now;
        c1 = g_filt.cum_m;
    }

    for (i = 0; i < n; i++) {
        idx = (unsigned)((g_filt.spd_i + RT_SPD_N - n + i) % RT_SPD_N);
        dt_ms = t1 - g_filt.spd[idx].ms;
        if (dt_ms < RT_SPD_MIN_MS || dt_ms > RT_SPD_WIN_MAX_MS) {
            continue;
        }

        err = (int32_t)dt_ms - (int32_t)RT_SPD_WIN_MS;
        if (err < 0) {
            err = -err;
        }

        /* 同等误差取较新点，避免 1 Hz 阶跃多算一跳。 */
        if (err <= best_err) {
            best_err = err;
            best = idx;
        }
    }

    if (best == 0xffffu) {
        return g_filt.speed;
    }

    dt_s = (float)(t1 - g_filt.spd[best].ms) / 1000.0f;
    if (dt_s < 0.2f) {
        return g_filt.speed;
    }

    d = c1 - g_filt.spd[best].cum_m;
    if (d < 0.0f) {
        d = 0.0f;
    }

    kph = (d / dt_s) * 3.6f;
    if (kph < 0.4f) {
        return 0.0f;
    }

    return rt_clampf(kph, 0.0f, RT_FILT_MAX_KPH);
}

/** @brief sAcc 门的两个常数（见 docs/gnss_speed_filter.md 步骤 2）。
 *
 * @details
 * 判据是**相对**的：sAcc 是 1σ 速度误差，读数只有在"误差远小于速度本身"时才可信，
 * 绝对阈值换个速度就不成立。取
 *     sAcc > max(RT_SACC_MIN_M_S, RT_SACC_FRAC × v)  → 这一帧多普勒判为不可信
 * 下限 0.6 m/s 兜住低速（相对误差在低速必然发散），30% 即"误差不超过读数三成"。
 *
 * 现场实测（2026-09-17，无定位 / 2D、hAcc≈680 m）：gSpeed 在 2~31 km/h 之间乱跳而
 * sAcc 是 2.7~9.9 m/s —— 误差比读数还大。这类帧正是要挡掉的。
 *
 * PVT 不可用时 sAcc 传 0，门恒不触发，行为与改动前完全一致。
 */
#define RT_SACC_MIN_M_S   0.6f
#define RT_SACC_FRAC      0.30f

/** @brief 位置精度差到这个程度，位移窗就不能当速度源（1σ，米）。
 *
 * @details
 * 位移窗的分辨力 = hAcc / 窗口长度：hAcc 20 m 配 5 s 窗就是 ~14 km/h 的噪声。
 * 现场（2026-09-17）残余的"跳 4 km/h"正是这么来的 —— sAcc 门把档位压到 LOW 后
 * 显示以位移窗为主，而那一程的 hAcc 是 22~47 m。
 *
 * 取 10 m：比它差的一律认为位置不可用；好定位（hAcc 1~3 m）正常通过。
 */
#define RT_POS_BAD_M      10.0f

static rt_conf_t rt_conf_of(rt_conf_t prev, uint8_t q, uint8_t hdop_x10, bool valid,
    float sacc_m_s, float dop_kph, bool *dop_bad)
{
    float lim;

    if (dop_bad != NULL) {
        *dop_bad = false;
    }

    if (!valid || q < 2) {
        return RT_CONF_LOW;
    }

    /* sAcc 门只降不升：精度差就按最低档处理（混合权重 0.10，基本交给位移窗）。
     * 放在 HDOP 分档之前 —— 它说的是"速度这个量本身可不可信"，与几何精度正交。
     * `dop_bad` 单独回给调用方：那是"多普勒不可信"这件事本身，与档位不是一回事
     * （位置也不可信时，调用方要据此改成保持上一次输出，见 rt_shape_speed）。 */
    lim = dop_kph * RT_SACC_FRAC / 3.6f;
    if (lim < RT_SACC_MIN_M_S) {
        lim = RT_SACC_MIN_M_S;
    }

    if (sacc_m_s > lim) {
        if (dop_bad != NULL) {
            *dop_bad = true;
        }

        return RT_CONF_LOW;
    }

    if (hdop_x10 == 0) {
        return (prev == RT_CONF_HIGH) ? RT_CONF_HIGH : RT_CONF_MID;
    }

    if (hdop_x10 <= 9) {
        return RT_CONF_HIGH;
    }

    if (hdop_x10 <= 12 && prev == RT_CONF_HIGH) {
        return RT_CONF_HIGH;
    }

    if (hdop_x10 <= 18) {
        return RT_CONF_MID;
    }

    if (hdop_x10 <= 22 && prev == RT_CONF_MID) {
        return RT_CONF_MID;
    }

    return RT_CONF_LOW;
}

static uint32_t rt_disp_win_target(rt_conf_t conf)
{
    if (conf == RT_CONF_HIGH) {
        return RT_DISP_WIN_HIGH_MS;
    }

    if (conf == RT_CONF_MID) {
        return RT_DISP_WIN_MID_MS;
    }

    return RT_DISP_WIN_LOW_MS;
}

static float rt_disp_blend_target(rt_conf_t conf)
{
    if (conf == RT_CONF_HIGH) {
        return 0.85f;
    }

    if (conf == RT_CONF_MID) {
        return 0.40f;
    }

    return 0.10f;
}

static void rt_disp_slew_u32(uint32_t * v, uint32_t target, uint32_t * at,
    uint32_t now, uint32_t per_s)
{
    uint32_t dt;
    uint32_t step;

    if (*v == 0) {
        *v = target;
        *at = now;
        return;
    }

    dt = now - *at;
    if (dt < 40u) {
        return;
    }

    *at = now;
    step = (per_s * dt) / 1000u;
    if (step < 20u) {
        step = 20u;
    }

    if (*v < target) {
        *v = (*v + step > target) ? target : (*v + step);
    } else if (*v > target) {
        *v = (*v - step < target) ? target : (*v - step);
    }
}

static void rt_disp_slew_f(float * v, float target, uint32_t * at, uint32_t now,
    float per_s)
{
    uint32_t dt;
    float step;

    dt = now - *at;
    if (*at == 0 || dt > 2000u) {
        *v = target;
        *at = now;
        return;
    }

    if (dt < 40u) {
        return;
    }

    *at = now;
    step = per_s * ((float)dt / 1000.0f);
    if (*v < target) {
        *v = (*v + step > target) ? target : (*v + step);
    } else if (*v > target) {
        *v = (*v - step < target) ? target : (*v - step);
    }
}

static void rt_disp_push(uint32_t now, float cum_m)
{
    if (g_disp.n > 0) {
        unsigned last = (unsigned)((g_disp.i + RT_DISP_N - 1u) % RT_DISP_N);
        float dc = cum_m - g_disp.pt[last].cum_m;
        uint32_t dt = now - g_disp.pt[last].ms;

        if (dc < RT_SPD_CUM_EPS_M) {
            return;
        }

        if (dt < 150u) {
            g_disp.pt[last].ms = now;
            g_disp.pt[last].cum_m = cum_m;
            return;
        }
    }

    g_disp.pt[g_disp.i].ms = now;
    g_disp.pt[g_disp.i].cum_m = cum_m;
    g_disp.i = (uint8_t)((g_disp.i + 1u) % RT_DISP_N);
    if (g_disp.n < RT_DISP_N) {
        g_disp.n++;
    }
}

static float rt_disp_path_kph(uint32_t now)
{
    unsigned n = g_disp.n;
    unsigned i;
    unsigned idx;
    unsigned newest;
    unsigned best = 0xffffu;
    int32_t best_err = 0x7fffffff;
    uint32_t t1;
    uint32_t dt_ms;
    uint32_t win = g_disp.win_ms;
    uint32_t win_max;
    uint32_t win_min;
    int32_t err;
    float c1;
    float d;
    float dt_s;
    float kph;

    if (win < RT_DISP_WIN_HIGH_MS) {
        win = RT_DISP_WIN_HIGH_MS;
    }

    win_max = win + RT_DISP_WIN_SLACK_MS;
    win_min = win / 3u;
    if (win_min < 800u) {
        win_min = 800u;
    }

    if (n < 2) {
        return 0.0f;
    }

    newest = (unsigned)((g_disp.i + RT_DISP_N - 1u) % RT_DISP_N);
    t1 = g_disp.pt[newest].ms;
    c1 = g_disp.pt[newest].cum_m;

    if ((now - t1) >= RT_SPD_COAST_MS) {
        t1 = now;
        c1 = g_disp.cum_m;
    }

    for (i = 0; i < n; i++) {
        idx = (unsigned)((g_disp.i + RT_DISP_N - n + i) % RT_DISP_N);
        dt_ms = t1 - g_disp.pt[idx].ms;
        if (dt_ms < win_min || dt_ms > win_max) {
            continue;
        }

        err = (int32_t)dt_ms - (int32_t)win;
        if (err < 0) {
            err = -err;
        }

        if (err <= best_err) {
            best_err = err;
            best = idx;
        }
    }

    if (best == 0xffffu) {
        return g_disp.last_out;
    }

    dt_s = (float)(t1 - g_disp.pt[best].ms) / 1000.0f;
    if (dt_s < 0.2f) {
        return g_disp.last_out;
    }

    d = c1 - g_disp.pt[best].cum_m;
    if (d < 0.0f) {
        d = 0.0f;
    }

    kph = (d / dt_s) * 3.6f;
    if (kph < 0.4f) {
        return 0.0f;
    }

    return rt_clampf(kph, 0.0f, RT_FILT_MAX_KPH);
}

static void rt_disp_note_fix(const bicycle_gnss_fix_t * fix, bool valid,
    float sog, uint32_t now)
{
    float dist;
    float dt_s;
    float implied;
    bool moving;

    if (!valid || fix == NULL) {
        return;
    }

    if (!g_disp.have) {
        g_disp.have = true;
        g_disp.lat = fix->latitude;
        g_disp.lon = fix->longitude;
        g_disp.cum_m = 0.0f;
        g_disp.last_ms = now;
        rt_disp_push(now, 0.0f);
        return;
    }

    dt_s = (float)(now - g_disp.last_ms) / 1000.0f;
    if (g_disp.last_ms == 0 || dt_s < 0.05f) {
        dt_s = 0.05f;
    } else if (dt_s > 8.0f) {
        dt_s = 8.0f;
    }

    dist = (float)vmap_geo_haversine_m((double)g_disp.lon, (double)g_disp.lat,
        (double)fix->longitude, (double)fix->latitude);
    implied = (dt_s > 0.0f) ? (dist / dt_s) * 3.6f : 0.0f;
    if (implied > RT_FILT_TELEPORT_KPH) {
        g_disp.last_ms = now;
        return;
    }

    moving = (sog >= RT_FILT_HOLD_KPH) || (g_sog_hold >= RT_SOG_MOVE_KPH)
        || (implied >= RT_SOG_MOVE_KPH);
    g_disp.lat = fix->latitude;
    g_disp.lon = fix->longitude;
    g_disp.last_ms = now;
    if (!moving || dist < RT_STILL_ENTER_M || rt_still_gate(0.0f, now)) {
        return;                 /* 静止判定与 g_filt 共用（见 rt_still_gate） */
    }

    g_disp.cum_m += dist;
    rt_disp_push(now, g_disp.cum_m);
}

static void rt_publish_held(bicycle_gnss_fix_t * fix, uint32_t now,
    bool advance_clock)
{
    rt_spd_push(now, g_filt.cum_m);
    g_filt.speed = rt_path_speed_kph(now);
    if (advance_clock) {
        g_filt.last_ms = now;
    }
    fix->latitude = g_filt.lat;
    fix->longitude = g_filt.lon;
    fix->course_deg = g_filt.course;
    fix->speed_kph = g_filt.speed;
}

static void rt_speed_hold_zero(bicycle_gnss_fix_t * fix)
{
    g_filt.spd_n = 0;
    g_filt.spd_i = 0;
    g_filt.speed = 0.0f;
    if (fix != NULL) {
        fix->speed_kph = 0.0f;
    }
}

static float rt_speed_for_display(float kph)
{
    int tenths;

    if (kph < 0.4f) {
        g_speed_disp = 0.0f;
        return 0.0f;
    }

    tenths = (int)(kph * 10.0f + 0.5f);
    kph = (float)tenths / 10.0f;
    if (g_speed_disp < 0.05f || rt_absf(kph - g_speed_disp) >= RT_SPEED_DISP_HYST) {
        g_speed_disp = kph;
    }
    return g_speed_disp;
}

/**
 * 3D/HDOP 差时不把离谱多普勒当时速。位移像样则用位移，否则保持上一拍。
 */
static float rt_trust_speed(float sog, float implied_kph, float prev_kph, bool poor)
{
    float phys;

    if (!poor) {
        if (sog < RT_FILT_HOLD_KPH && implied_kph >= RT_SOG_MOVE_KPH
            && implied_kph <= RT_FILT_TELEPORT_KPH) {
            return implied_kph;
        }

        return sog;
    }

    phys = implied_kph;
    if (phys > RT_FILT_TELEPORT_KPH) {
        phys = prev_kph;
    }

    if (sog < RT_FILT_POOR_SOG_KPH) {
        return 0.0f;
    }

    if (sog > phys + RT_SOG_CLIFF_KPH && sog > prev_kph + RT_SOG_CLIFF_KPH) {
        if (phys >= RT_FILT_POOR_SOG_KPH && phys <= RT_FILT_TELEPORT_KPH) {
            return phys;
        }

        return prev_kph;
    }

    return sog;
}

static float rt_shape_sog(float sog, bool valid, bool spike_guard)
{
    uint32_t now = lv_tick_get();
    float gate;
    bool dropout;

    sog = rt_clampf(sog, 0.0f, RT_FILT_MAX_KPH);
    gate = (g_sog_hold < 0.05f) ? RT_SOG_STILL_ON : RT_SOG_STILL_OFF;

    if (valid && sog >= gate) {
        if (spike_guard && g_sog_hold >= RT_SOG_MOVE_KPH
            && sog > g_sog_hold + RT_SOG_SPIKE_KPH) {
            return g_sog_hold;
        }

        g_sog_hold = sog;
        g_sog_coast_at = 0;
        return sog;
    }

    dropout = (g_sog_hold >= RT_SOG_MOVE_KPH)
        && (!valid || sog < RT_SOG_ZERO_KPH
            || (g_sog_hold - sog) >= RT_SOG_CLIFF_KPH);
    if (dropout) {
        if (g_sog_coast_at == 0) {
            g_sog_coast_at = now ? now : 1u;
        }
        if (lv_tick_elaps(g_sog_coast_at) < RT_SOG_HOLD_MS) {
            return g_sog_hold;
        }
    }

    g_sog_hold = 0.0f;
    g_sog_coast_at = 0;
    return 0.0f;
}

/**
 * @brief 多普勒源：PVT 可用时用模块**滤波过**的 gSpeed，否则回退 NMEA 的未滤波 SOG。
 *
 * @details
 * 现场（2026-09-17）：弱信号下 NMEA 的 SOG 在 20 km/h 处读数 17↔25 来回跳，而
 * NMEA 给的就是**未滤波**的多普勒；u-blox 的 `UBX-NAV-PVT.gSpeed` 是模组自己滤过的
 * 同一个量。没开 PVT / 没收到帧时回退，行为与改动前一致。
 */
static float rt_dop_kph(const bicycle_gnss_fix_t * fix)
{
    if (fix->pvt_valid) {
        return rt_clampf(fix->speed_pvt_kph, 0.0f, RT_FILT_MAX_KPH);
    }

    return fix->speed_kph;
}

static float rt_shape_speed(const bicycle_gnss_fix_t * fix, bool valid)
{
    uint32_t now = lv_tick_get();
    rt_conf_t conf;
    float sog;
    float shaped;
    float path;
    float hold_prev;
    float out;
    float dt_s;
    float step;
    float cap;
    bool conf_jump;
    bool path_ok;
    bool dop_bad;

    sog = (fix && valid) ? rt_dop_kph(fix) : 0.0f;
    hold_prev = g_sog_hold;
    conf = rt_conf_of(g_disp.conf,
        (fix && valid) ? fix->fix_quality : 0,
        (fix && valid) ? fix->hdop_x10 : 0, valid,
        (fix && valid) ? fix->speed_acc_m_s : 0.0f, sog, &dop_bad);
    conf_jump = (conf != g_disp.conf);
    if (conf_jump) {
        g_disp.conf = conf;
        g_disp.conf_at = now;
    }

    shaped = rt_shape_sog(sog, valid, conf != RT_CONF_HIGH);

    rt_disp_slew_u32(&g_disp.win_ms, rt_disp_win_target(conf), &g_disp.win_at,
        now, RT_DISP_WIN_SLEW_MS_S);
    rt_disp_slew_f(&g_disp.blend, rt_disp_blend_target(conf), &g_disp.blend_at,
        now, RT_DISP_BLEND_SLEW_S);
    rt_disp_note_fix(fix, valid, sog, now);
    if (!valid && g_disp.last_ms != 0
        && (now - g_disp.last_ms) >= RT_FILT_INVALID_MS) {
        rt_disp_reset();
    }

    path = rt_disp_path_kph(now);
    path_ok = (g_disp.n >= 2u);
    /*
     * SOG 被模组打成 0 时仍可能在高速挪点。位移窗说在动就跟窗，
     * 不要把 path 一并清掉，否则时速/极速会一直钉在 0。
     */
    if (shaped < 0.4f && g_sog_hold < 0.05f && path < RT_FILT_HOLD_KPH) {
        path = 0.0f;
    }

    /*
     * 3D 低常见：坐标几乎不动，RMC 多普勒单次或连爬到 ~160。
     * 位移窗说没这么快时，不把 SOG 写进 hold，也不让它进显示。
     */
    if (conf != RT_CONF_HIGH) {
        float ref = path_ok ? path : g_disp.last_out;
        bool parked_path = !path_ok || path < RT_FILT_POOR_SOG_KPH;
        bool bogus = (shaped > ref + RT_SOG_CLIFF_KPH)
            && (shaped > RT_SOG_MOVE_KPH)
            && parked_path;

        if (bogus) {
            g_sog_hold = (hold_prev >= RT_SOG_MOVE_KPH) ? hold_prev : 0.0f;
            shaped = (ref > 0.4f) ? ref : 0.0f;
        }
    }

    if (shaped < 0.4f && path >= RT_SOG_MOVE_KPH) {
        out = path;
    } else {
        out = shaped * g_disp.blend + path * (1.0f - g_disp.blend);
    }

    /*
     * 两个源都不可信时**保持上一次输出**（计划里的"质量冻结"），而不是把
     * 90% 权重交给位移窗：sAcc 说多普勒不可信、hAcc 又说位置不可信（几十米）
     * 时，位移窗自己的噪声就有几 km/h —— 现场残余的"跳 4 km/h"正是它贡献的
     * （见 RT_POS_BAD_M）。位置尚可（hAcc 小）时不动：那种情况下位移窗是更好的
     * 源，就是 sAcc 门把档位压低的本意。
     *
     * 只保留"保持"、不改档位：静止时下面的 still 门照旧归零，位置重新变好或
     * 多普勒恢复都会立刻退出这条分支。
     */
    if (dop_bad && (!fix || !fix->pvt_valid || fix->pos_acc_m > RT_POS_BAD_M)) {
        out = g_disp.last_out;
    }

    if (g_disp.out_ms != 0 && now > g_disp.out_ms) {
        dt_s = (float)(now - g_disp.out_ms) / 1000.0f;
    } else {
        dt_s = 0.05f;
    }
    if (dt_s > 0.5f) {
        dt_s = 0.5f;
    }

    if (g_disp.conf != RT_CONF_HIGH || g_disp.blend < 0.80f || conf_jump
        || (g_disp.conf_at != 0 && lv_tick_elaps(g_disp.conf_at) < 800u)) {
        cap = (conf_jump || (g_disp.conf_at != 0 && lv_tick_elaps(g_disp.conf_at) < 800u))
            ? RT_DISP_CONF_SLEW_KPH_S : RT_DISP_SLEW_KPH_S;
        step = cap * dt_s;
        if (rt_absf(out - g_disp.last_out) > step) {
            if (out > g_disp.last_out) {
                out = g_disp.last_out + step;
            } else {
                out = g_disp.last_out - step;
            }
        }
    }

    /* 静止判定压过平滑结果：位置没离开半径就别报速度 —— 治"3D 高→低
     * 跳变时车不动也有几 km/h"（那时显示以位移窗为基准，而位移窗被噪声喂饱）。
     * 离开静止时从 0 起按 slew 爬升，不会有阶跃。
     * ⚠ 这条能生效的前提是**静止门自己解得开** —— 见 rt_smooth_fix 钉住分支里的
     *   `rt_still_gate(dist)`：门锁存后若没人喂它，这里会把时速永远压成 0。 */
    if (rt_still_gate(0.0f, now)) {
        out = 0.0f;
    }

    g_disp.last_out = out;
    g_disp.out_ms = now;
    rt_dynmodel_update(sog, path, path_ok, now);
    return rt_speed_for_display(out);
}

static void rt_smooth_on_invalid(void)
{
    uint32_t now;

    if (!g_filt.have) {
        return;
    }

    now = lv_tick_get();
    if (g_filt.invalid_ms == 0) {
        g_filt.invalid_ms = now;
        return;
    }

    if (lv_tick_elaps(g_filt.invalid_ms) >= RT_SPEED_HOLD_INV_MS) {
        rt_spd_push(now, g_filt.cum_m);
        g_filt.speed = rt_path_speed_kph(now);
    }

    if (lv_tick_elaps(g_filt.invalid_ms) >= RT_FILT_INVALID_MS) {
        rt_filt_reset();
    }
}

static void rt_smooth_fix(bicycle_gnss_fix_t * fix)
{
    uint32_t now = lv_tick_get();
    float sog;
    float meas_lat;
    float meas_lon;
    float prev_lat;
    float prev_lon;
    float dt_s;
    double dist;
    float implied_kph;
    float step;
    bool poor;
    bool still;
    bool implied_move;
    bool parked;

    if (fix == NULL || !fix->valid) {
        rt_smooth_on_invalid();
        return;
    }

    g_filt.invalid_ms = 0;
    sog = rt_clampf(fix->speed_kph, 0.0f, RT_FILT_MAX_KPH);
    meas_lat = fix->latitude;
    meas_lon = fix->longitude;
    poor = (fix->fix_quality < 2) || (fix->hdop_x10 > RT_HDOP_LOW_X10);
    still = (sog < RT_FILT_HOLD_KPH);

    if (!g_filt.have) {
        g_filt.have = true;
        g_filt.lat = meas_lat;
        g_filt.lon = meas_lon;
        g_filt.course = fix->course_deg;
        g_filt.last_ms = now;
        g_filt.cum_m = 0.0f;
        rt_spd_push(now, 0.0f);
        g_filt.speed = still ? 0.0f : rt_trust_speed(sog, 0.0f, 0.0f, poor);
        fix->speed_kph = g_filt.speed;
        return;
    }

    dt_s = (float)(now - g_filt.last_ms) / 1000.0f;
    if (dt_s < 0.05f) {
        dt_s = 0.05f;
    } else if (dt_s > 8.0f) {
        dt_s = 8.0f;
    }

    dist = vmap_geo_haversine_m((double)g_filt.lon, (double)g_filt.lat,
        (double)meas_lon, (double)meas_lat);
    implied_kph = (dt_s > 0.0f) ? (float)(dist / (double)dt_s * 3.6) : 0.0f;
    /* ⚠ 只喂**速度**判定（下面 dist < 0.8 那条分支里"要不要把时速清零"），
     * **不参与"钉不钉住坐标"** —— 位置那一侧按 2026-09-15 的算法，
     * 见再下面那条"钉住坐标"的分支。别把这两个又接回位置判定。 */
    implied_move = (implied_kph >= RT_SOG_MOVE_KPH)
        && (implied_kph <= RT_FILT_TELEPORT_KPH);
    parked = still && !implied_move;

    /*
     * 200 ms 轮询常拿到同一 NMEA。若每次都刷新 last_ms，下一秒 28 m
     * 会被当成 500 km/h 跳点丢掉。无位移时只更新时速窗。
     */
    if (dist < (double)RT_FILT_STILL_M) {
        if ((parked || (poor && sog < RT_FILT_POOR_SOG_KPH))
            && g_filt.speed < RT_SOG_MOVE_KPH) {
            rt_speed_hold_zero(fix);
            fix->latitude = g_filt.lat;
            fix->longitude = g_filt.lon;
            fix->course_deg = g_filt.course;
            return;
        }

        rt_spd_push(now, g_filt.cum_m);
        /* 3D 低时坐标几乎不动、SOG 却到 160：保持上一拍，不信多普勒。 */
        g_filt.speed = rt_trust_speed(sog, 0.0f, g_filt.speed, poor);
        fix->latitude = g_filt.lat;
        fix->longitude = g_filt.lon;
        fix->course_deg = g_filt.course;
        fix->speed_kph = g_filt.speed;
        return;
    }

    /* 静止或 3D 低且 SOG 近静止：**钉住坐标** —— 位置这一侧完全按 2026-09-15
     * 的算法（`still || (poor && sog < 8)`，不带 `!implied_move`）。
     * 下面那句 `g_filt.speed < RT_SOG_MOVE_KPH` 才是 09-16 加的，只用于
     * "位移窗还在动时不要把时速清零"，属速度侧，按用户要求保留。 */
    if (still || (poor && sog < RT_FILT_POOR_SOG_KPH)) {
        rt_publish_held(fix, now, dist < RT_FILT_HOLD_MAX_M);

        /* ★ 别让这个分支变成静止门的牢笼（2026-10-01 修"汽车急加速时速归 0"）。
         *
         * 本分支是**唯一**会命中后提前返回、不喂 `rt_still_gate(step)` 的路径。
         * 汽车急加速时模组多普勒跟不上、SOG 还压在低位 ⇒ 本分支连续命中 ⇒
         * `g_still` 锁存后再没人喂它，**永远解不开**。而锁存会同时掐死两处：
         *   · `rt_shape_speed()` 末尾 `if (rt_still_gate(0)) out = 0` ⇒ 时速钉死 0；
         *   · `rt_disp_note_fix()` 里那句 `|| rt_still_gate(0)` ⇒ **位移窗也停止累计**
         *     ⇒ `path` 恒为 0，连"SOG 塌了就改跟位移"那条补救都拿不到数。
         * 这就是"位置在以 40~126 km/h 走、时速却连续 6 个点是 0"的成因
         * （TRK_20260930_074235.gpx 43:13~43:25）。
         *
         * 判据用**自钉住点的累计位移** `dist`，且只在已超过 `RT_STILL_EXIT_M`
         * （"真的走了"的迟滞阈值）时才喂：真停车时位置噪声累加不到 8 m，
         * "停稳就报 0"照旧生效。 */
        if (dist >= RT_STILL_EXIT_M) {
            rt_still_gate((float)dist, now);
        }

        if (g_filt.speed < RT_SOG_MOVE_KPH) {
            rt_speed_hold_zero(fix);
        }
        return;
    }

    /* 单点超**位置**门限：丢弃坐标。3D 高时保留 SOG，别用空位移窗写成 0。
     * 用 160 而不是速度那条 280 —— 这条判的是"进不进轨迹"。 */
    if (implied_kph > RT_FILT_POS_TELEPORT_KPH) {
        if ((now - g_filt.last_ms) >= RT_FILT_INVALID_MS) {
            g_filt.lat = meas_lat;
            g_filt.lon = meas_lon;
            g_filt.course = fix->course_deg;
            g_filt.last_ms = now;
            rt_spd_push(now, g_filt.cum_m);
            g_filt.speed = rt_trust_speed(sog, implied_kph, g_filt.speed, poor);
            fix->speed_kph = g_filt.speed;
            return;
        }

        if (!poor && sog >= RT_SOG_MOVE_KPH) {
            g_filt.speed = sog;
            fix->latitude = g_filt.lat;
            fix->longitude = g_filt.lon;
            fix->course_deg = g_filt.course;
            fix->speed_kph = sog;
            return;
        }

        rt_publish_held(fix, now, false);
        return;
    }

    prev_lat = g_filt.lat;
    prev_lon = g_filt.lon;
    g_filt.lat = meas_lat;
    g_filt.lon = meas_lon;

    step = (float)vmap_geo_haversine_m((double)prev_lon, (double)prev_lat,
        (double)g_filt.lon, (double)g_filt.lat);
    if (step > RT_FILT_MAX_MPS * dt_s) {
        g_filt.lat = prev_lat;
        g_filt.lon = prev_lon;
        if (!poor && sog >= RT_SOG_MOVE_KPH) {
            g_filt.speed = sog;
            fix->latitude = g_filt.lat;
            fix->longitude = g_filt.lon;
            fix->course_deg = g_filt.course;
            fix->speed_kph = sog;
            return;
        }

        rt_publish_held(fix, now, false);
        return;
    }

    if (!rt_still_gate(step, now)) {
        g_filt.cum_m += step;   /* 静止时噪声不进里程/速度窗，见 rt_still_gate */
    }

    g_filt.last_ms = now;
    if (g_filt.speed >= RT_FILT_COURSE_KPH || sog >= RT_FILT_COURSE_KPH) {
        float d = vmap_geo_angle_delta_deg(g_filt.course, fix->course_deg);

        g_filt.course += d;
        if (g_filt.course < 0.0f) {
            g_filt.course += 360.0f;
        } else if (g_filt.course >= 360.0f) {
            g_filt.course -= 360.0f;
        }
    }

    rt_spd_push(now, g_filt.cum_m);
    g_filt.speed = rt_trust_speed(sog, implied_kph, g_filt.speed, poor);
    fix->latitude = g_filt.lat;
    fix->longitude = g_filt.lon;
    fix->course_deg = g_filt.course;
    fix->speed_kph = g_filt.speed;
}

static void rt_subj_set_int(lv_subject_t * subj, int32_t v)
{
    if (lv_subject_get_int(subj) != v) {
        lv_subject_set_int(subj, v);
    }
}

static int32_t rt_round_course(float deg)
{
    int32_t v = (int32_t)(deg + 0.5f) % 360;

    if (v < 0) {
        v += 360;
    }
    return v;
}

static void bicycle_runtime_ui_sync(void)
{
    if (!g_ui_inited) {
        return;
    }

    rt_subj_set_int(&g_rt.subj_speed_kph, (int32_t)(g_rt.speed_kph + 0.5f));
    rt_subj_set_int(&g_rt.subj_course_deg, rt_round_course(g_rt.course_deg));
    rt_subj_set_int(&g_rt.subj_motion_deg, rt_round_course(g_rt.motion_deg));
    rt_subj_set_int(&g_rt.subj_lap, (int32_t)g_rt.lap_count);
    lv_subject_snprintf(&g_rt.subj_lap_km, "%.1f", g_rt.session_distance_m / 1000.0);
    rt_subj_set_int(&g_rt.subj_session_km,
        (int32_t)(g_rt.session_distance_m / 1000.0 + 0.5));
    rt_subj_set_int(&g_rt.subj_track_pts, (int32_t)g_rt.track_point_count);
    rt_subj_set_int(&g_rt.subj_session_time_s,
        (int32_t)(g_rt.session_elapsed_ms / 1000u));
    rt_subj_set_int(&g_rt.subj_lap_time_s, (int32_t)(g_rt.lap_elapsed_ms / 1000u));
    {
        char line[24];
        int n = 0;

        line[0] = '\0';
        if (g_rt.hr_valid) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "H%u", g_rt.hr_bpm);
        }
        if (g_rt.cadence_valid) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%sC%u",
                n > 0 ? " " : "", g_rt.cadence_rpm);
        }
        if (g_rt.power_valid) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%sP%u",
                n > 0 ? " " : "", g_rt.power_w);
        }
        if (n == 0) {
            snprintf(line, sizeof(line), "--");
        }
        lv_subject_snprintf(&g_rt.subj_sensor, "%s", line);
    }
#if VMAP_ROUTE_ENABLE
    rt_subj_set_int(&g_rt.subj_nav_remain_m, (int32_t)(g_rt.nav_remain_m + 0.5));
    rt_subj_set_int(&g_rt.subj_off_route_m, (int32_t)(g_rt.nav_off_route_m + 0.5));
    rt_subj_set_int(&g_rt.subj_nav_turn_deg, (int32_t)(g_rt.nav_turn_deg + 0.5f));
#endif
}

/**
 * @brief 自行车 runtime ui init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_runtime_ui_init(void)
{
    if (g_ui_inited) {
        bicycle_runtime_ui_sync();
        return;
    }

    lv_subject_init_int(&g_rt.subj_speed_kph, 0);
    lv_subject_init_int(&g_rt.subj_course_deg, 0);
    lv_subject_init_int(&g_rt.subj_motion_deg, 0);
    lv_subject_init_int(&g_rt.subj_lap, 1);
    lv_subject_init_string(&g_rt.subj_lap_km, g_lap_km_buf, g_lap_km_prev,
        sizeof(g_lap_km_buf), "0.0");
    lv_subject_init_int(&g_rt.subj_session_km, 0);
    lv_subject_init_int(&g_rt.subj_track_pts, 0);
    lv_subject_init_int(&g_rt.subj_session_time_s, 0);
    lv_subject_init_int(&g_rt.subj_lap_time_s, 0);
    lv_subject_init_string(&g_rt.subj_sensor, g_sensor_buf, g_sensor_prev,
        sizeof(g_sensor_buf), "--");
#if VMAP_ROUTE_ENABLE
    lv_subject_init_int(&g_rt.subj_nav_remain_m, 0);
    lv_subject_init_int(&g_rt.subj_off_route_m, 0);
    lv_subject_init_int(&g_rt.subj_nav_turn_deg, 0);
#endif
    for (int i = 0; i < BICYCLE_SUBJ_EVT_MAX; i++) {
        lv_subject_init_pointer(&g_subj_evt[i], NULL);
    }
    g_ui_inited = true;
    rt_disp_reset();
    bicycle_runtime_ui_sync();
}

/**
 * @brief 自行车 runtime subj evt。
 */
lv_subject_t * bicycle_runtime_subj_evt(bicycle_subj_evt_id_t id)
{
    if (id >= BICYCLE_SUBJ_EVT_MAX) {
        return NULL;
    }
    return &g_subj_evt[id];
}

/**
 * @brief 自行车 runtime evt send。
 */
void bicycle_runtime_evt_send(bicycle_subj_evt_id_t id, void * payload)
{
    lv_subject_t * subj = bicycle_runtime_subj_evt(id);

    if (subj == NULL) {
        return;
    }
    lv_subject_set_pointer(subj, payload);
}

/**
 * @brief 自行车 runtime evt notify。
 */
void bicycle_runtime_evt_notify(bicycle_subj_evt_id_t id)
{
    lv_subject_t * subj = bicycle_runtime_subj_evt(id);

    if (subj == NULL) {
        return;
    }
    lv_subject_notify(subj);
}

/**
 * @brief 自行车 runtime evt observe。
 */
lv_observer_t * bicycle_runtime_evt_observe(bicycle_subj_evt_id_t id, lv_observer_cb_t cb,
                                            lv_obj_t * obj, void * user_data)
{
    lv_subject_t * subj = bicycle_runtime_subj_evt(id);

    if (subj == NULL || cb == NULL) {
        return NULL;
    }
    return lv_subject_add_observer_obj(subj, cb, obj, user_data);
}

/**
 * @brief 自行车 runtime emit page evt。
 */
void bicycle_runtime_emit_page_evt(uint8_t page_id, const char * name,
                                   bicycle_page_evt_kind_t kind)
{
    static bicycle_page_evt_t evt;

    evt.page_id = page_id;
    evt.page_name = name;
    evt.kind = kind;
    bicycle_runtime_evt_send(BICYCLE_SUBJ_EVT_PAGE, &evt);
}

/**
 * @brief 自行车 runtime emit mtp evt。
 */
void bicycle_runtime_emit_mtp_evt(bool host_attached, bool transfer_active)
{
    static bicycle_mtp_evt_t evt;

    evt.host_attached = host_attached;
    evt.transfer_active = transfer_active;
    bicycle_runtime_evt_send(BICYCLE_SUBJ_EVT_MTP, &evt);
}

/**
 * @brief 自行车 runtime get。
 * @return 请求的值。
 */
const bicycle_runtime_t * bicycle_runtime_get(void)
{
    return &g_rt;
}

/**
 * @brief 自行车 runtime subj speed kph。
 */
lv_subject_t * bicycle_runtime_subj_speed_kph(void)
{
    return &g_rt.subj_speed_kph;
}

/**
 * @brief 自行车 runtime subj course deg。
 */
lv_subject_t * bicycle_runtime_subj_course_deg(void)
{
    return &g_rt.subj_course_deg;
}

/**
 * @brief 自行车 runtime subj motion deg。
 */
lv_subject_t * bicycle_runtime_subj_motion_deg(void)
{
    return &g_rt.subj_motion_deg;
}

/**
 * @brief 自行车 runtime subj lap。
 */
lv_subject_t * bicycle_runtime_subj_lap(void)
{
    return &g_rt.subj_lap;
}

/**
 * @brief 自行车 runtime subj lap km。
 */
lv_subject_t * bicycle_runtime_subj_lap_km(void)
{
    return &g_rt.subj_lap_km;
}

/**
 * @brief 自行车 runtime subj session km。
 */
lv_subject_t * bicycle_runtime_subj_session_km(void)
{
    return &g_rt.subj_session_km;
}

/**
 * @brief 自行车 runtime subj track pts。
 */
lv_subject_t * bicycle_runtime_subj_track_pts(void)
{
    return &g_rt.subj_track_pts;
}

/**
 * @brief 自行车 runtime subj session time s。
 */
lv_subject_t * bicycle_runtime_subj_session_time_s(void)
{
    return &g_rt.subj_session_time_s;
}

/**
 * @brief 自行车 runtime subj lap time s。
 */
lv_subject_t * bicycle_runtime_subj_lap_time_s(void)
{
    return &g_rt.subj_lap_time_s;
}

/**
 * @brief 自行车 runtime subj sensor。
 */
lv_subject_t * bicycle_runtime_subj_sensor(void)
{
    return &g_rt.subj_sensor;
}

#if VMAP_ROUTE_ENABLE
/**
 * @brief 自行车 runtime subj nav remain m。
 */
lv_subject_t * bicycle_runtime_subj_nav_remain_m(void)
{
    return &g_rt.subj_nav_remain_m;
}

/**
 * @brief 自行车 runtime subj off route m。
 */
lv_subject_t * bicycle_runtime_subj_off_route_m(void)
{
    return &g_rt.subj_off_route_m;
}

/**
 * @brief 自行车 runtime subj nav turn deg。
 */
lv_subject_t * bicycle_runtime_subj_nav_turn_deg(void)
{
    return &g_rt.subj_nav_turn_deg;
}

/**
 * @brief 自行车 runtime update nav。
 */
void bicycle_runtime_update_nav(double remain_m, double off_route_m, float turn_deg)
{
    g_rt.nav_remain_m = remain_m;
    g_rt.nav_off_route_m = off_route_m;
    g_rt.nav_turn_deg = turn_deg;
    bicycle_runtime_ui_sync();
}
#endif

/**
 * @brief 自行车 runtime reset session。
 */
void bicycle_runtime_reset_session(void)
{
    const bool recording = g_rt.recording;

    memset(&g_rt, 0, BICYCLE_RUNTIME_DATA_BYTES);
    g_rt.lap_count = 1;
    g_rt.recording = recording;
    g_last_tick_ms = 0;
    g_hr_sum = 0;
    g_hr_n = 0;
    g_hr_last_ms = 0;
    g_rt.has_altitude = false;
    rt_filt_reset();
    rt_disp_reset();
    rt_dynmodel_request(MYVENDOR_GNSS_DYN_BIKE);
    bicycle_env_hold_gain_ref();
    bicycle_runtime_ui_sync();
}

/**
 * @brief 接续旧骑行：写入已有里程与计时。
 */
void bicycle_runtime_seed_session(double dist_m, uint64_t elapsed_ms)
{
    if (dist_m < 0.0) {
        dist_m = 0.0;
    }

    g_rt.session_distance_m = dist_m;
    g_rt.lap_distance_m = dist_m;
    g_rt.lap_start_session_m = 0.0;
    g_rt.session_elapsed_ms = elapsed_ms;
    g_rt.lap_elapsed_ms = elapsed_ms;
    bicycle_runtime_ui_sync();
}

/**
 * @brief 自行车 runtime set recording。
 */
void bicycle_runtime_set_recording(bool recording)
{
    if (!recording && g_rt.recording) {
        /* 骑行结束（含暂停）：先把最后没骑满一段的净高度变化结掉，
         * 再停录 —— 见 bicycle_env_flush_gain()。放在清 recording 之前，
         * 这笔账才过得了 add_dalt 里的录制判据。 */
        bicycle_env_flush_gain();
    }

    g_rt.recording = recording;
    if (!recording) {
        g_last_tick_ms = 0;
    } else if (g_last_tick_ms == 0) {
        g_last_tick_ms = lv_tick_get();
        bicycle_env_hold_gain_ref();
    }
}

void bicycle_runtime_save_last_pos(void)
{
    int32_t lon_e7;
    int32_t lat_e7;

    if (!isfinite(g_rt.longitude) || !isfinite(g_rt.latitude)) {
        return;
    }

    if (fabsf(g_rt.longitude) < 0.001f && fabsf(g_rt.latitude) < 0.001f) {
        return;
    }

    if (g_rt.longitude < -180.0f || g_rt.longitude > 180.0f ||
        g_rt.latitude < -90.0f || g_rt.latitude > 90.0f) {
        return;
    }

    lon_e7 = (int32_t)((double)g_rt.longitude * 10000000.0 +
                       (g_rt.longitude >= 0.0f ? 0.5 : -0.5));
    lat_e7 = (int32_t)((double)g_rt.latitude * 10000000.0 +
                       (g_rt.latitude >= 0.0f ? 0.5 : -0.5));
    myvendor_devctl_last_pos_set(lon_e7, lat_e7);
}

bool bicycle_runtime_is_moving(bool resume)
{
    float lim = resume ? BICYCLE_RIDE_RESUME_KPH : BICYCLE_RIDE_MOVE_KPH;
    uint16_t cad = resume ? 40u : 30u;

    if (g_rt.speed_kph >= lim) {
        return true;
    }

    return g_rt.cadence_valid && g_rt.cadence_rpm >= cad;
}

void bicycle_runtime_set_gnss_solver_rmc(bool rmc)
{
    (void)myvendor_devctl_gnss_rmc_set(rmc);
    rt_filt_reset();
}

bool bicycle_runtime_gnss_solver_rmc(void)
{
    return myvendor_devctl_gnss_rmc_get();
}

/**
 * @brief 自行车 runtime tick。
 *
 * REC 后按墙钟累加；不依赖 GNSS / 速度。暂停时 recording=false，间隙不计入。
 */
void bicycle_runtime_tick(uint32_t now_ms)
{
    uint32_t dt;

    if (!g_rt.recording || g_last_tick_ms == 0) {
        g_last_tick_ms = now_ms;
        return;
    }

    if (now_ms >= g_last_tick_ms) {
        dt = now_ms - g_last_tick_ms;
        g_rt.session_elapsed_ms += dt;
        g_rt.lap_elapsed_ms += dt;
        bicycle_runtime_ui_sync();
    }

    g_last_tick_ms = now_ms;
}

/**
 * @brief 自行车 runtime update gnss。
 */
void bicycle_runtime_update_gnss(const bicycle_gnss_fix_t * fix)
{
    if (!fix) {
        return;
    }

    g_rt.gnss_eph_busy = myvendor_sys_gnss_eph_busy();
    g_rt.gnss_alive = fix->alive || fix->valid || g_rt.gnss_eph_busy;
    g_rt.gnss_valid = fix->valid;
    g_rt.gnss_satellites = fix->satellites;
    g_rt.gnss_fix_quality = fix->fix_quality;
    g_rt.gnss_hdop_x10 = fix->hdop_x10;
    g_rt.gnss_rx_hz = fix->rx_hz;
    if (!fix->valid) {
        g_rt.gnss_has_altitude = false;
        g_rt.speed_kph = rt_shape_speed(fix, false);
        bicycle_runtime_ui_sync();
        return;
    }

    g_rt.longitude = fix->longitude;
    g_rt.latitude = fix->latitude;
    g_rt.course_deg = fix->course_deg;
    g_rt.speed_kph = rt_shape_speed(fix, true);
    if (g_rt.recording && g_rt.speed_kph > g_rt.speed_max_kph) {
        g_rt.speed_max_kph = g_rt.speed_kph;
    }

    if (fix->has_altitude) {
        g_rt.altitude_m = fix->altitude_m;
        g_rt.has_altitude = true;
        g_rt.gnss_altitude_m = fix->altitude_m;
        g_rt.gnss_has_altitude = true;
    }

    bicycle_runtime_ui_sync();
}

/**
 * @brief 计入一段净高度变化（正=爬升、负=下降）。
 *
 * 与 update_env 同一口径：只在录制中累计。env 的段结账与骑行结束的
 * `bicycle_env_flush_gain()` 都走这里。
 */
void bicycle_runtime_add_dalt(float dalt_m)
{
    if (!g_rt.recording) {
        return;
    }

    if (dalt_m > 0.0f) {
        g_rt.gain_m += dalt_m;
    } else if (dalt_m < 0.0f) {
        g_rt.loss_m += -dalt_m;
    }
}

void bicycle_runtime_update_env(float altitude_m, bool has_altitude,
    float grade_pct, float dalt_m)
{
    if (has_altitude) {
        g_rt.altitude_m = altitude_m;
        g_rt.has_altitude = true;
    }

    g_rt.grade_pct = grade_pct;
    if (g_rt.recording) {
        bicycle_runtime_add_dalt(dalt_m);

        /* 海拔极值：和 gain/loss 同一个数据源、同一个"只在记录中统计"口径
         * （数据页/海拔页的"最高/最低"用它；没记录过就 alt_range_valid=false，
         * 界面显示 "--" 而不是 0 —— 0 m 是合法海拔，不能当"没数据"）。 */
        if (has_altitude) {
            if (!g_rt.alt_range_valid) {
                g_rt.alt_range_valid = true;
                g_rt.altitude_max_m = altitude_m;
                g_rt.altitude_min_m = altitude_m;
            } else {
                if (altitude_m > g_rt.altitude_max_m) {
                    g_rt.altitude_max_m = altitude_m;
                }
                if (altitude_m < g_rt.altitude_min_m) {
                    g_rt.altitude_min_m = altitude_m;
                }
            }
        }
    }

    bicycle_runtime_ui_sync();
}

static void rt_sys_gnss_to_fix(const myvendor_sys_gnss_t * src,
    bicycle_gnss_fix_t * out)
{
    uint8_t sats;

    if (src == NULL || out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->alive = src->alive || src->valid;
    out->valid = src->valid;
    out->fix_quality = src->fix_quality;
    out->hdop_x10 = src->hdop_x10;
    out->rx_hz = src->rx_hz;
    /* satellites 保持旧语义（已定位给锁定量、搜星给有信号量），不改既有判定；
     * 三个原始口径加采样时刻另带一份，想区分的地方不要再猜 satellites 是谁。 */
    sats = src->valid ? src->satellites : src->sats_heard;

    out->satellites = sats;
    out->sats_locked = src->satellites;
    out->sats_heard = src->sats_heard;
    out->sats_in_view = src->sats_in_view;
    out->stamp_ms = src->stamp_ms;
    /* PVT 的三个量在 `!valid` 时也要带过来：无定位/2D 恰恰是"速度不可信、
     * 需要 sAcc 说话"的场景，不能因为没定位就把证据丢了。 */
    out->pvt_valid = src->pvt_valid;
    out->pvt_gnss_ok = src->pvt_gnss_ok;
    out->speed_pvt_kph = (float)src->speed_pvt_centi_kmh / 100.0f;
    out->speed_acc_m_s = (float)src->speed_acc_mm_s / 1000.0f;
    out->pos_acc_m = (float)src->pos_acc_mm / 1000.0f;
    if (!src->valid) {
        return;
    }

    out->latitude = (float)((double)src->lat_e7 / 10000000.0);
    out->longitude = (float)((double)src->lon_e7 / 10000000.0);
    out->course_deg = (float)src->course_deg;
    out->speed_kph = (float)src->speed_centi_kmh / 100.0f;
    if (src->fix_quality >= 2) {
        out->has_altitude = true;
        out->altitude_m = (float)src->alt_mm / 1000.0f;
    }
}

bool bicycle_runtime_poll_fix(bicycle_gnss_fix_t * out)
{
    myvendor_sys_gnss_t onboard;
    myvendor_sys_gnss_t phone;
    bool have_onboard;
    bool have_phone;

    if (out == NULL) {
        return false;
    }

    if (bicycle_gpx_sim_get_fix(out) && out->valid) {
        if (g_filt.have) {
            rt_filt_reset();
        }
        out->alive = true;
        if (out->fix_quality < 1) {
            out->fix_quality = 2;
        }

        if (out->hdop_x10 == 0) {
            out->hdop_x10 = 10;
        }

        return true;
    }

    have_onboard = myvendor_sys_onboard_gnss_get(&onboard);
    have_phone = myvendor_sys_phone_gnss_get(&phone);

    if (have_onboard && onboard.valid) {
        rt_sys_gnss_to_fix(&onboard, out);
        if (rt_trust_rmc()) {
            if (g_filt.have) {
                rt_filt_reset();
            }
        } else {
            rt_smooth_fix(out);
        }
        return true;
    }

    if (have_phone && phone.valid) {
        rt_sys_gnss_to_fix(&phone, out);
        if (rt_trust_rmc()) {
            if (g_filt.have) {
                rt_filt_reset();
            }
        } else {
            rt_smooth_fix(out);
        }
        return true;
    }

    if (have_onboard) {
        rt_sys_gnss_to_fix(&onboard, out);
        if (!rt_trust_rmc()) {
            rt_smooth_on_invalid();
            out->speed_kph = g_filt.speed;
        }
        return true;
    }

    if (have_phone) {
        rt_sys_gnss_to_fix(&phone, out);
        if (!rt_trust_rmc()) {
            rt_smooth_on_invalid();
            out->speed_kph = g_filt.speed;
        }
        return true;
    }

    memset(out, 0, sizeof(*out));
    rt_smooth_on_invalid();
    out->speed_kph = g_filt.speed;
    return false;
}

/**
 * @brief 把 BLE 传感器快照同步进 runtime（主界面/地图页共用的那一条路）。
 *
 * 以前只有**地图页**的 render pump 调这个逻辑（`map_page_apply_companion_sensors()`），
 * 于是会话停在主骑行页时 `g_rt.hr_bpm/cadence_rpm` 没人更新 —— 传感器明明连上、
 * `sys hr` 也有值（H77 C63），主界面却还是 0/空。这里提到 runtime，主循环每拍调一次。
 *
 * 取的是 `myvendor_sys_sensor_get()` 那份**已加锁的桥快照**（sensor 线程发布），
 * 不在 UI 线程直接摸 ble_sensor 的内部表。
 */
void bicycle_runtime_sync_companion_sensors(void)
{
    myvendor_sys_sensor_t telem;

    /* **不阻塞**：BLE 侧（companion/FS 写盘）持 bridge 锁时，UI 线程这一拍直接
     * 跳过 —— 否则 LVGL、静止覆盖层心跳、按键与拿起唤醒会一起被冻住
     * （现场表现就是"静止界面卡住"）。下一拍（约 10 ms 后）自然补上。 */
    if (!myvendor_sys_sensor_get_try(&telem)) {
        return;
    }

    bicycle_runtime_update_sensors(telem.hr_bpm, telem.hr_valid,
        telem.cadence_rpm, telem.cadence_valid, telem.power_w,
        telem.power_valid);
}

/**
 * @brief 自行车 runtime update sensors。
 */
void bicycle_runtime_update_sensors(uint16_t hr_bpm, bool hr_valid,
    uint16_t cadence_rpm, bool cadence_valid, uint16_t power_w,
    bool power_valid)
{
    g_rt.hr_bpm = hr_bpm;
    g_rt.hr_valid = hr_valid;
    g_rt.cadence_rpm = cadence_rpm;
    g_rt.cadence_valid = cadence_valid;
    g_rt.power_w = power_w;
    g_rt.power_valid = power_valid;
    if (g_rt.recording && hr_valid && hr_bpm > 0u) {
        uint32_t now = lv_tick_get();

        if (g_hr_last_ms == 0u || (now - g_hr_last_ms) >= 1000u) {
            g_hr_sum += hr_bpm;
            g_hr_n++;
            g_rt.hr_avg_bpm = (uint16_t)((g_hr_sum + g_hr_n / 2u) / g_hr_n);
            if (hr_bpm > g_rt.hr_max_bpm) {
                g_rt.hr_max_bpm = hr_bpm;
            }

            g_hr_last_ms = now;
        }
    }

    bicycle_runtime_ui_sync();
}

/**
 * @brief 自行车 runtime add segment。
 */
void bicycle_runtime_add_segment(double seg_m, float motion_deg, float speed_kph)
{
    uint32_t now;
    float dt_s;
    double max_m;

    if (seg_m <= 0.0) {
        return;
    }

    now = lv_tick_get();
    if (g_last_seg_ms != 0) {
        dt_s = (float)(now - g_last_seg_ms) / 1000.0f;
        if (dt_s < 0.2f) {
            dt_s = 0.2f;
        } else if (dt_s > 8.0f) {
            dt_s = 8.0f;
        }

        max_m = (double)RT_FILT_MAX_MPS * (double)dt_s;
        if (seg_m > max_m) {
            g_last_seg_ms = now;
            g_rt.motion_deg = motion_deg;
            bicycle_runtime_ui_sync();
            return;
        }
    }

    g_last_seg_ms = now;
    g_rt.session_distance_m += seg_m;
    g_rt.lap_distance_m += seg_m;
    g_rt.motion_deg = motion_deg;
    (void)speed_kph;
    bicycle_runtime_ui_sync();
}

/**
 * @brief 自行车 runtime on lap closed。
 */
void bicycle_runtime_on_lap_closed(void)
{
    g_rt.lap_count++;
    g_rt.lap_start_session_m = g_rt.session_distance_m;
    g_rt.lap_elapsed_ms = 0;
    bicycle_runtime_ui_sync();
}

/**
 * @brief 自行车 runtime lap since start m。
 */
double bicycle_runtime_lap_since_start_m(void)
{
    const double d = g_rt.session_distance_m - g_rt.lap_start_session_m;

    return d > 0.0 ? d : 0.0;
}

/**
 * @brief 自行车 runtime set track metrics。
 */
void bicycle_runtime_set_track_metrics(uint16_t point_count, uint16_t km_marker_count)
{
    g_rt.track_point_count = point_count;
    g_rt.km_marker_count = km_marker_count;
    bicycle_runtime_ui_sync();
}
