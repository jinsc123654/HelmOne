/**
 * @file bicycle_gpx_sim.h
 * @brief 自行车 UI — gpx_sim。
 */

#ifndef BICYCLE_GPX_SIM_H
#define BICYCLE_GPX_SIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bicycle_gnss_fix {
    float longitude;
    float latitude;
    float altitude_m;
    float course_deg;
    float speed_kph;
    uint8_t satellites;
    uint8_t fix_quality; /**< 0 无、1 2D、2 3D。 */
    uint8_t hdop_x10;    /**< HDOP×10；0 未知。 */
    uint8_t rx_hz;       /**< NMEA / 模拟更新率。 */
    /**
     * @brief UBX-NAV-PVT 带来的速度量（NMEA 没有）。见 docs/gnss_speed_filter.md 步骤 2。
     *
     * @details
     * `speed_kph` 仍是 RMC 那份**未滤波**多普勒；`speed_pvt_kph` 是模块**滤波过**的
     * `gSpeed`，`speed_acc_m_s` 是它自己的精度估计（1σ）。`pvt_valid` 为假时这两项
     * 无意义。选源与门控都在 bicycle_runtime.c（`rt_dop_kph()` / `rt_conf_of()`）。
     */
    float speed_pvt_kph;
    float speed_acc_m_s;
    float pos_acc_m; /**< PVT hAcc：位置精度 1σ（米）。位移窗可不可信的判据。 */
    bool pvt_valid;
    bool pvt_gnss_ok; /**< 模组自认定位可用（NAV-PVT flags.gnssFixOK）。 */
    bool valid;
    bool alive;
    bool has_altitude;

    /**
     * @brief 快照原样带过来的三个星数，不要合成一个。
     *
     * @details
     * satellites 为了兼容旧逻辑仍是 "已定位用锁定量、搜星用有信号量" 的合成值
     * （见 rt_sys_gnss_to_fix）。下面三个保留原始口径，任何想区分
     * "锁定 / 有信号 / 在视" 的地方都应该用它们，而不是再猜 satellites 是哪个。
     */
    uint8_t sats_locked;   /**< GGA 解算用星。 */
    uint8_t sats_heard;    /**< GSV 中 SNR>0 的星；搜星进度看这个。 */
    uint8_t sats_in_view;  /**< GSV "in view" 累加值，不是颗数。 */

    /** @brief 本份快照的采样时刻（CLOCK_MONOTONIC ms）；0 未知。 */
    uint32_t stamp_ms;
} bicycle_gnss_fix_t;

typedef struct {
    double lon;
    double lat;
} bicycle_nav_pt_t;

/**
 * @brief 用 GPX 轨迹点回放模拟 GNSS（覆盖板载定位）。
 * @param path GPX 路径；NULL 则用 VMAP_GPX_PATH。
 * @param speed_kph 回放速度；≤0 用默认加减速。
 * @param period_ms 更新周期；0 用 VMAP_GNSS_UPDATE_MS。
 * @param reverse true 从终点向起点。
 * @param skip_m 先走到这条距离再开始回放；≤0 从头。前缀视为已骑过。
 * @return 0 成功，负值失败。
 */
int bicycle_gpx_sim_start(const char * path, float speed_kph, unsigned period_ms,
    bool reverse, double skip_m);
/** @brief 跳过前缀的实际里程（米）。 */
double bicycle_gpx_sim_prefix_m(void);
/** @brief 跳过前缀的轨迹时间（毫秒）；无时间戳则为按 22 km/h 估算。 */
uint32_t bicycle_gpx_sim_prefix_ms(void);
/** @brief 前缀点数（含起点和跳到的点）。 */
uint32_t bicycle_gpx_sim_prefix_count(void);
/** @brief 取前缀第 i 点（回放方向）。 */
bool bicycle_gpx_sim_prefix_point(uint32_t i, float * lon, float * lat,
    float * speed_kph);
/**
 * @brief 自行车 gpx sim stop。
 */
void bicycle_gpx_sim_stop(void);
/**
 * @brief 自行车 gpx sim active。
 */
bool bicycle_gpx_sim_active(void);

int bicycle_route_sim_start(const bicycle_nav_pt_t * pts, uint32_t pt_count,
    float speed_kph, unsigned period_ms);
/**
 * @brief 自行车 route sim reload。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_route_sim_reload(const bicycle_nav_pt_t * pts, uint32_t pt_count);
/**
 * @brief 自行车 route sim stop。
 */
void bicycle_route_sim_stop(void);
/**
 * @brief 自行车 route sim active。
 */
bool bicycle_route_sim_active(void);
/** @brief 活跃路线 sim 上更新速度（km/h）。 */
void bicycle_route_sim_set_speed(float speed_kph);

/** @brief 路线 sim 与 GPX 回放同时存在时，优先使用路线 sim 定位。 */
bool bicycle_gpx_sim_get_fix(bicycle_gnss_fix_t * out);

/**
 * @brief 往返循环开关（长测用）。
 *
 * @details 打开后，回放到**端点**时不再停住（默认行为）而是**掉头**继续骑 —— 长测里
 *          让它一直有定位更新 ⇒ 地图一直有平移/重绘负载。掉头（而不是跳回起点）是为了
 *          避免一次上千公里的瞬移把瓦片整批打掉、造成周期性尖峰。
 *          NSH：`bicycle_nsh gnss sim <gpx> ... loop`（不带 loop 就回到默认的一次性回放）。
 *
 * @note 掉头时日志会打 `gpx_sim: loop turn -> fwd/rev at idx=i/n`：长测里"它在循环"的
 *       唯一证据，一次往返一行。
 */
void bicycle_gpx_sim_set_loop(bool on);
/** @brief 往返循环是否已打开。 */
bool bicycle_gpx_sim_loop_enabled(void);

/** @brief 跳点表的一行（`bicycle_gnss_hop_set_pts()` 用；字段顺序与内置表一致）。 */
typedef struct {
    const char * name;
    float lat;
    float lon;
} bicycle_gnss_hop_pt_t;

/**
 * @brief 全国跳（调试用合成定位）。
 *
 * @details 每 period_ms 在固定的城市序列里换一个点（经度/纬度都是上千公里的大跳），
 * 用来**可重复地**逼出"瓦片整批作废 + 重载 + 全屏重绘"这条路径 —— 小幅平移常常
 * 一个新瓦片都不需要，`[vperf] frame` 因此不出帧。用于渲染性能 A/B（WT vs WB）。
 *
 * @param period_ms 换点周期（下限 200 ms）。
 * @param count     总次数；0 = 不限次（用 bicycle_gnss_hop_stop() 停）。
 */
void bicycle_gnss_hop_start(unsigned period_ms, unsigned count);

/**
 * @brief 换一组跳点（演示模式用来跳太湖/青海湖这类风景点）。
 * @param pts 点表；**调用方持有**，跳点期间必须一直有效（演示里是静态表）。
 * @param n   点数；传 NULL 或 0 回到内置城市序列。
 * @note 只对**接下来这一次**跳点有效：跳点停掉（手动或跳够 count 自动停）就失效
 *       ⇒ `bicycle_nsh gnss hop` 的城市序列口径不变。
 */
void bicycle_gnss_hop_set_pts(const bicycle_gnss_hop_pt_t * pts, uint32_t n);
/** @brief 停止全国跳；随后回到真实 GNSS。 */
void bicycle_gnss_hop_stop(void);
/** @brief 是否在跳。 */
bool bicycle_gnss_hop_active(void);
/** @brief 当前所在城市名（未跳时为 "-"）；仅日志用。 */
const char * bicycle_gnss_hop_cur_name(void);
/** @brief 取当前跳到的定位（在 bicycle_gpx_sim_get_fix() 里最优先）。 */
bool bicycle_gnss_hop_get_fix(bicycle_gnss_fix_t * out);

/** @brief GPX 点上的墙钟（回放轨迹时间）。无则 false. */
bool bicycle_gpx_sim_get_wall_hms(int * hour, int * min);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_GPX_SIM_H */
