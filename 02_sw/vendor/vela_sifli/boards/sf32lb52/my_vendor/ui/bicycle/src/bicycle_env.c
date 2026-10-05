/**
 * @file bicycle_env.c
 * @brief 路网+气压融合海拔、累计爬升；BMI270 俯仰坡度。
 *
 * 实时海拔：路网节点钉绝对高，气压跟短时起伏与节点之间的变化。
 * 累计爬升：按"骑过一段距离才结账"记账 —— 记账高度先按**已骑过的距离**做
 * 低通，走满一段（ENV_GAIN_SEG_M）才把这一段的净变化计入，不到门限当平地。
 * 无定位或速度过低不计（过夜静置不会积几千米）；骑行结束由
 * `bicycle_env_flush_gain()` 把最后没走满的一段结一次。
 *
 * 坡度：10 Hz 低通重力向量上算俯仰，再按 1.2 s 时间常数平滑，并限制
 * 每秒变化。路面颠簸不再直接打到数据页。
 */

#include "bicycle_env.h"

#include "bicycle_runtime.h"
#include "myvendor_devctl.h"
#include "myvendor_board_sensor.h"
#include "myvendor_mono.h"

#include <nuttx/config.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define ENV_P0_ISA        1013.25f
#define ENV_ISA_M         44330.77f
#define ENV_GRADE_CLAMP   40.0f
#define ENV_GRADE_TAU_S   1.2f
#define ENV_GRADE_SLEW    8.0f
#define ENV_P0_ALPHA      0.08f
#define ENV_DEM_BIAS_A    0.05f
/** @name 累计爬升的记账口径（用户 2026-10-05 定）
 *
 * 旧写法是"融合海拔 3 m 迟滞"：高度每跨过 ±3 m 就记一笔。问题是高度里
 * 的单点跳变（GNSS 高度解算漂移、路口处路网/气压基准切换，实测一趟 6.5 km
 * 平路有十几处 3~9 m 的跳）每次都被当成一段爬升/下降记下 —— 现场一趟平路
 * 记出 183 m 爬升。新口径两条：
 *   ① 只有**完全骑过**一段距离（SEG）之后才结这一段的账，且只记**净**变化
 *      ⇒ 路口原地/小范围徘徊（距离不推进）永远不结账，往返抵消；
 *   ② 记账高度先按已骑过的距离做一阶低通（TAU）：高度变化要"骑过去之后"
 *      才被承认，单点毛刺在骑过 TAU 米的过程中被平均掉，不再是爬升。
 * 净变化不到 THRESH 认定为平地（参考点不动、也不记账）——噪声不会一点点
 * 攒成大数，长缓坡照常累计。骑行结束再 flush 一次（见 flush_gain）。
 * @{ */
#define ENV_GAIN_SEG_M    20.0f
#define ENV_GAIN_TAU_M    80.0f
#define ENV_GAIN_THRESH_M 3.0f
/** @} */

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

static bool g_imu_ok;
static bool g_baro_ok;
static float g_ax;
static float g_ay;
static float g_az;
static float g_hpa;
static float g_p0 = ENV_P0_ISA;
static float g_alt_m;
static bool g_have_alt;
static float g_dem_m;
static bool g_dem_ok;
static float g_dem_bias;
static bool g_dem_bias_ok;
static float g_gain_ref;          /**< 本段起点的记账海拔。 */
static double g_gain_ref_dist;    /**< 本段起点的会话里程（m）。 */
static bool g_gain_ref_ok;
static float g_gain_acc;          /**< 距离键控低通后的"已认可"海拔。 */
static double g_gain_acc_dist;    /**< g_gain_acc 更新到哪一米的会话里程。 */
static float g_pitch_off;
static float g_raw_grade;
static float g_grade;
static bool g_inited;
static float g_grade_f;
static bool g_grade_have;
static uint32_t g_grade_ms;

static float env_clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static float env_mdeg_to_rad(int32_t mdeg)
{
    return ((float)mdeg / 1000.0f) * ((float)M_PI / 180.0f);
}

static int32_t env_rad_to_mdeg(float rad)
{
    float mdeg = rad * (180.0f / (float)M_PI) * 1000.0f;

    if (mdeg >= 0.0f) {
        mdeg += 0.5f;
    } else {
        mdeg -= 0.5f;
    }

    return (int32_t)mdeg;
}

static float env_pitch_rad(void)
{
    /* Portrait stem mount: +Y along the display, +Z out of the screen.
     * IMU axes are remapped in the BMI270 driver (rear-mount). */
    return atan2f(-g_ay, hypotf(g_ax, g_az));
}

static float env_grade_from_pitch(float pitch)
{
    float g = tanf(pitch) * 100.0f;

    return env_clampf(g, -ENV_GRADE_CLAMP, ENV_GRADE_CLAMP);
}

static float env_baro_alt(float hpa, float p0)
{
    float r;

    if (hpa <= 1.0f || p0 <= 1.0f) {
        return 0.0f;
    }

    r = hpa / p0;
    if (r <= 0.0f) {
        return 0.0f;
    }

    return ENV_ISA_M * (1.0f - powf(r, 0.190263f));
}

static float env_p0_from_alt(float hpa, float alt_m)
{
    float k = 1.0f - alt_m / ENV_ISA_M;

    if (hpa <= 1.0f || k <= 0.05f) {
        return ENV_P0_ISA;
    }

    return hpa / powf(k, 5.255f);
}

#ifdef CONFIG_BOARD_BMI270
static void env_poll_accel(void)
{
    myvendor_sys_vec3_t accel;

    myvendor_board_sensor_accel_filt_get(&accel);
    if (accel.valid) {
        g_ax = accel.x;
        g_ay = accel.y;
        g_az = accel.z;
        g_imu_ok = true;
    } else {
        g_imu_ok = false;
    }
}

static void env_poll_accel_raw(void)
{
    myvendor_sys_vec3_t accel;

    myvendor_board_sensor_accel_get(&accel);
    if (accel.valid) {
        g_ax = accel.x;
        g_ay = accel.y;
        g_az = accel.z;
    }
}
#else
static void env_poll_accel(void)
{
    g_imu_ok = false;
}

static void env_poll_accel_raw(void)
{
}
#endif

#ifdef CONFIG_BOARD_BMP388
static void env_poll_baro(void)
{
    myvendor_sys_baro_t baro;

    myvendor_board_sensor_baro_get(&baro);
    if (baro.valid && baro.hpa > 1.0f) {
        g_hpa = baro.hpa;
        g_baro_ok = true;
    } else {
        g_baro_ok = false;
    }
}
#else
static void env_poll_baro(void)
{
    g_baro_ok = false;
}
#endif

void bicycle_env_init(void)
{
    if (g_inited) {
        return;
    }

    g_pitch_off = env_mdeg_to_rad(myvendor_devctl_grade_offset_get());
    g_inited = true;
}

void bicycle_env_set_dem_m(float m, bool ok)
{
    g_dem_ok = ok;
    if (ok) {
        g_dem_m = m;
    } else {
        g_dem_bias_ok = false;
    }
}

void bicycle_env_hold_gain_ref(void)
{
    g_gain_ref_ok = false;
    g_gain_acc_dist = 0.0;
}

/**
 * @brief 把爬升记账的参考点 / 低通值一起挪到 (alt, dist)。
 *
 * 参考点与低通值必须同时挪：只挪一个会让下一段的第一笔账把低通滞后当成
 * 高度变化记进去。
 */
static void env_gain_ref_set(float alt, double dist_m)
{
    g_gain_ref = alt;
    g_gain_ref_dist = dist_m;
    g_gain_acc = alt;
    g_gain_acc_dist = dist_m;
    g_gain_ref_ok = true;
}

/**
 * @brief 骑行结束：把最后没骑满一段的净高度变化结一次账。
 *
 * 用户 2026-10-05："骑行结束计算一次后方的高度变化"。判据与 tick 里结账
 * 完全一致（只认 ≥ ENV_GAIN_THRESH_M 的净变化），差别只是不再要求走满
 * ENV_GAIN_SEG_M。幂等：结完账参考点就移到当前位置，重复调用不会重复计。
 * 由 `bicycle_runtime_set_recording(false)`（停止录制 = 骑行结束/暂停）
 * 在**清 recording 之前**调用，所以这笔账照样进 gain/loss。
 */
void bicycle_env_flush_gain(void)
{
    const bicycle_runtime_t * rt = bicycle_runtime_get();
    float d;

    if (!g_gain_ref_ok || rt == NULL) {
        return;
    }

    d = g_gain_acc - g_gain_ref;
    if (d >= ENV_GAIN_THRESH_M || d <= -ENV_GAIN_THRESH_M) {
        bicycle_runtime_add_dalt(d);
    }

    g_gain_ref = g_gain_acc;
    g_gain_ref_dist = rt->session_distance_m;
}

static bool env_is_moving(const bicycle_runtime_t * rt)
{
    return rt && rt->gnss_valid && bicycle_runtime_is_moving(false);
}

void bicycle_env_tick(void)
{
    const bicycle_runtime_t * rt;
    float pitch;
    float dalt = 0.0f;
    bool has_alt = false;
    float alt = 0.0f;

    bicycle_env_init();
    g_pitch_off = env_mdeg_to_rad(myvendor_devctl_grade_offset_get());
    env_poll_accel();
    env_poll_baro();

    if (g_imu_ok) {
        float inst;
        float dt_s;
        uint32_t now = myvendor_mono_ms();

        pitch = env_pitch_rad();
        inst = env_grade_from_pitch(pitch - g_pitch_off);
        env_poll_accel_raw();
        g_raw_grade = env_grade_from_pitch(env_pitch_rad());
        if (!g_grade_have) {
            g_grade_f = inst;
            g_grade_have = true;
            g_grade_ms = now;
        } else {
            float a;
            float step;
            float lim;

            dt_s = (float)myvendor_mono_elapsed_ms(now, g_grade_ms) / 1000.0f;
            if (dt_s < 0.02f) {
                dt_s = 0.02f;
            } else if (dt_s > 1.0f) {
                dt_s = 1.0f;
            }

            a = dt_s / (ENV_GRADE_TAU_S + dt_s);
            g_grade_f += (inst - g_grade_f) * a;
            step = g_grade_f - g_grade;
            lim = ENV_GRADE_SLEW * dt_s;
            if (step > lim) {
                g_grade_f = g_grade + lim;
            } else if (step < -lim) {
                g_grade_f = g_grade - lim;
            }

            g_grade_ms = now;
        }

        g_grade = g_grade_f;
    }

    rt = bicycle_runtime_get();

    if (g_baro_ok) {
        float ref_alt;
        bool have_ref = false;

        if (g_dem_ok) {
            ref_alt = g_dem_m;
            have_ref = true;
        } else if (rt && rt->gnss_has_altitude && rt->gnss_valid) {
            ref_alt = rt->gnss_altitude_m;
            have_ref = true;
        }

        if (have_ref) {
            float p0 = env_p0_from_alt(g_hpa, ref_alt);

            g_p0 = g_p0 * (1.0f - ENV_P0_ALPHA) + p0 * ENV_P0_ALPHA;
        }

        alt = env_baro_alt(g_hpa, g_p0);
        if (g_dem_ok) {
            float err = alt - g_dem_m;

            if (!g_dem_bias_ok) {
                g_dem_bias = err;
                g_dem_bias_ok = true;
            } else {
                g_dem_bias = g_dem_bias * (1.0f - ENV_DEM_BIAS_A) +
                             err * ENV_DEM_BIAS_A;
            }

            alt = alt - g_dem_bias;
        }

        has_alt = true;
    } else if (g_dem_ok) {
        alt = g_dem_m;
        has_alt = true;
    } else if (rt && rt->has_altitude) {
        alt = rt->altitude_m;
        has_alt = true;
    }

    if (has_alt) {
        /* 爬升/下降记账（口径见 ENV_GAIN_* 注释）：只在"在骑 + 在记录"时记账；
         * 停车/暂停时参考点跟住当前值（这段不算，也避免停放期间的气压漂移在
         * 起步后被当成爬升）。 */
        const bool tracking = (rt != NULL) && rt->recording && env_is_moving(rt);

        if (!tracking) {
            env_gain_ref_set(alt, rt != NULL ? rt->session_distance_m : 0.0);
        } else if (!g_gain_ref_ok) {
            env_gain_ref_set(alt, rt->session_distance_m);
        } else {
            const double dist_m = rt->session_distance_m;
            const float moved = (float)(dist_m - g_gain_acc_dist);

            if (moved > 0.0f) {
                const float a = moved / (moved + ENV_GAIN_TAU_M);

                g_gain_acc += (alt - g_gain_acc) * a;
                g_gain_acc_dist = dist_m;
            }

            if (dist_m - g_gain_ref_dist >= (double)ENV_GAIN_SEG_M) {
                const float d = g_gain_acc - g_gain_ref;

                if (d >= ENV_GAIN_THRESH_M || d <= -ENV_GAIN_THRESH_M) {
                    dalt = d;
                    env_gain_ref_set(g_gain_acc, dist_m);
                }
            }
        }

        g_alt_m = alt;
        g_have_alt = true;
    }

    if (!g_imu_ok && rt) {
        g_grade = rt->grade_pct;
        g_raw_grade = rt->grade_pct;
    }

    bicycle_runtime_update_env(g_have_alt ? g_alt_m : 0.0f, g_have_alt,
                               g_imu_ok ? g_grade : (rt ? rt->grade_pct : 0.0f),
                               dalt);
}

bool bicycle_env_imu_valid(void)
{
    return g_imu_ok;
}

bool bicycle_env_baro_valid(void)
{
    return g_baro_ok;
}

float bicycle_env_raw_grade_pct(void)
{
    return g_raw_grade;
}

float bicycle_env_grade_pct(void)
{
    return g_grade;
}

float bicycle_env_offset_pct(void)
{
    return env_grade_from_pitch(g_pitch_off);
}

float bicycle_env_altitude_m(void)
{
    return g_alt_m;
}

float bicycle_env_hpa(void)
{
    return g_hpa;
}

bool bicycle_env_calibrate(void)
{
    bicycle_env_init();
    env_poll_accel();
    if (!g_imu_ok) {
        return false;
    }

    g_pitch_off = env_pitch_rad();
    (void)myvendor_devctl_grade_offset_set(env_rad_to_mdeg(g_pitch_off));
    g_grade = 0.0f;
    g_grade_f = 0.0f;
    g_grade_have = true;
    g_grade_ms = myvendor_mono_ms();
    g_raw_grade = env_grade_from_pitch(g_pitch_off);
    return true;
}
