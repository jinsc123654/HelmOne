/**
 * @file bicycle_runtime.h
 * @brief 自行车 UI — runtime。
 */

#ifndef BICYCLE_RUNTIME_H
#define BICYCLE_RUNTIME_H

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#include "vmap/vmap_config.h"

#if !LV_USE_OBSERVER
#error "bicycle_runtime requires LV_USE_OBSERVER"
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct bicycle_gnss_fix;

typedef struct {
    /** @brief 最新 GNSS 定位。 */
    float longitude;
    float latitude;
    float altitude_m;
    bool has_altitude;
    float gnss_altitude_m;
    bool gnss_has_altitude;
    float gain_m;
    float loss_m;
    /** @name 会话内海拔极值（只在记录中统计，与 gain/loss 同一口径）。
     *  @note `alt_range_valid` 为假时**不要**把 0 当有效值用（0 m 是合法海拔）。
     *  @{
     */
    float altitude_max_m;
    float altitude_min_m;
    bool alt_range_valid;
    /** @} */
    float grade_pct;
    bool gnss_valid;
    bool gnss_alive;
    bool gnss_eph_busy;
    uint8_t gnss_satellites;
    uint8_t gnss_fix_quality;
    uint8_t gnss_hdop_x10;
    uint8_t gnss_rx_hz;

    /** @brief GNSS 航向（度，0–360）。 */
    float course_deg;
    /** @brief 沿轨迹的当前运动方位（度，0–360）。 */
    float motion_deg;
    float speed_kph;
    float speed_max_kph;

    uint16_t hr_bpm;
    uint16_t hr_avg_bpm; /**< 录制中有效心率的会话平均。 */
    uint16_t hr_max_bpm; /**< 录制中有效心率的会话最大。 */
    uint16_t cadence_rpm;
    uint16_t power_w;
    bool hr_valid;
    bool cadence_valid;
    bool power_valid;

    /** @brief 当前圈序号（从 1 开始）。 */
    uint16_t lap_count;
    /** @brief 会话累计距离（米）— 用于公里标记。 */
    double session_distance_m;
    /** @brief 会话累计距离（米）— 圈闭合不重置。 */
    double lap_distance_m;
    /** @brief 当前圈起点时的 session_distance_m（圈闭合守卫）。 */
    double lap_start_session_m;

    uint16_t track_point_count;
    uint16_t km_marker_count;

    /** @brief 自会话/当前圈开始的经过时间（毫秒）。 */
    uint64_t session_elapsed_ms;
    uint64_t lap_elapsed_ms;

    bool recording;

    /** @brief 活跃路线导航指标（同步到 subject）。 */
#if VMAP_ROUTE_ENABLE
    double nav_remain_m;
    double nav_off_route_m;
    float nav_turn_deg;
#endif

    /**
     * LVGL subjects — synced from the fields above; page widgets bind here.
     * Do not write directly; use bicycle_runtime_* update APIs.
     */
    lv_subject_t subj_speed_kph;
    lv_subject_t subj_course_deg;
    lv_subject_t subj_motion_deg;
    lv_subject_t subj_lap;
    /** @brief 会话累计距离（km，一位小数）— 底栏显示。 */
    lv_subject_t subj_lap_km;
    /** @brief 会话累计距离（km）— 总计/公里标记逻辑。 */
    lv_subject_t subj_session_km;
    lv_subject_t subj_track_pts;
    lv_subject_t subj_session_time_s;
    lv_subject_t subj_lap_time_s;
    lv_subject_t subj_sensor;
#if VMAP_ROUTE_ENABLE
    lv_subject_t subj_nav_remain_m;
    lv_subject_t subj_off_route_m;
    lv_subject_t subj_nav_turn_deg;
#endif
} bicycle_runtime_t;

/**
 * @brief 自行车 runtime get。
 * @return 请求的值。
 */
const bicycle_runtime_t * bicycle_runtime_get(void);

/** @brief 一次性 LVGL subject 初始化（可重复调用）。 */
void bicycle_runtime_ui_init(void);

/**
 * @brief 自行车 runtime subj speed kph。
 */
lv_subject_t * bicycle_runtime_subj_speed_kph(void);
/**
 * @brief 自行车 runtime subj course deg。
 */
lv_subject_t * bicycle_runtime_subj_course_deg(void);
/**
 * @brief 自行车 runtime subj motion deg。
 */
lv_subject_t * bicycle_runtime_subj_motion_deg(void);
/**
 * @brief 自行车 runtime subj lap。
 */
lv_subject_t * bicycle_runtime_subj_lap(void);
/**
 * @brief 自行车 runtime subj lap km。
 */
lv_subject_t * bicycle_runtime_subj_lap_km(void);
/**
 * @brief 自行车 runtime subj session km。
 */
lv_subject_t * bicycle_runtime_subj_session_km(void);
/**
 * @brief 自行车 runtime subj track pts。
 */
lv_subject_t * bicycle_runtime_subj_track_pts(void);
/**
 * @brief 自行车 runtime subj session time s。
 */
lv_subject_t * bicycle_runtime_subj_session_time_s(void);
/**
 * @brief 自行车 runtime subj lap time s。
 */
lv_subject_t * bicycle_runtime_subj_lap_time_s(void);
/**
 * @brief 自行车 runtime subj sensor。
 */
lv_subject_t * bicycle_runtime_subj_sensor(void);
#if VMAP_ROUTE_ENABLE
/**
 * @brief 自行车 runtime subj nav remain m。
 */
lv_subject_t * bicycle_runtime_subj_nav_remain_m(void);
/**
 * @brief 自行车 runtime subj off route m。
 */
lv_subject_t * bicycle_runtime_subj_off_route_m(void);
/**
 * @brief 自行车 runtime subj nav turn deg。
 */
lv_subject_t * bicycle_runtime_subj_nav_turn_deg(void);
/**
 * @brief 自行车 runtime update nav。
 */
void bicycle_runtime_update_nav(double remain_m, double off_route_m, float turn_deg);
#endif

/**
 * @brief 自行车 runtime reset session。
 */
void bicycle_runtime_reset_session(void);

/**
 * @brief 接续旧骑行：写入已有里程与计时（不重置其它会话状态）。
 */
void bicycle_runtime_seed_session(double dist_m, uint64_t elapsed_ms);
/**
 * @brief 自行车 runtime set recording。
 */
void bicycle_runtime_set_recording(bool recording);

/**
 * @brief 把当前最后一次有效定位写入 KV，供下次开机地图中心使用。
 *        无合法坐标则不改已存值。
 */
void bicycle_runtime_save_last_pos(void);

/** 停表阈值（km/h）；与自动暂停、爬升累计共用。 */
#define BICYCLE_RIDE_MOVE_KPH     1.8f
/** 自动恢复骑行的速度迟滞（km/h）。 */
#define BICYCLE_RIDE_RESUME_KPH   2.5f

/**
 * @brief 当前是否在骑行（GNSS 速度或踏频过阈值）。
 * @param resume true 用恢复阈值（迟滞），false 用停表阈值。
 */
bool bicycle_runtime_is_moving(bool resume);

/**
 * @brief GNSS 解算切换：true 完全信 RMC（位置/速度/航向不过滤）。
 */
void bicycle_runtime_set_gnss_solver_rmc(bool rmc);

/** @brief 当前是否完全信 RMC。 */
bool bicycle_runtime_gnss_solver_rmc(void);

/** @brief 每次 GNSS 轮询时调用，now_ms 为单调时钟（如 lv_tick_get）。 */
void bicycle_runtime_tick(uint32_t now_ms);
/**
 * @brief 自行车 runtime update gnss。
 */
void bicycle_runtime_update_gnss(const struct bicycle_gnss_fix * fix);
void bicycle_runtime_update_sensors(uint16_t hr_bpm, bool hr_valid,
    uint16_t cadence_rpm, bool cadence_valid, uint16_t power_w,
    bool power_valid);
/**
 * @brief 把 BLE 传感器快照（`myvendor_sys_sensor_get`）同步进 runtime。
 *
 * 主循环每拍调一次：**主骑行页与地图页都要看到 HR/踏频**，别只在某一页的
 * render pump 里更新（否则停在主界面时值永远是 0）。
 */
void bicycle_runtime_sync_companion_sensors(void);

/**
 * @brief 气压/路网融合海拔、IMU 坡度；dalt_m 仅在录制且在骑行中计入爬升/下降。
 */
void bicycle_runtime_update_env(float altitude_m, bool has_altitude,
    float grade_pct, float dalt_m);

/**
 * @brief 计入一段净高度变化（正=爬升、负=下降）；只在录制中生效。
 *
 * 供 env 的段结账与骑行结束的 flush（`bicycle_env_flush_gain`）共用。
 */
void bicycle_runtime_add_dalt(float dalt_m);

/**
 * @brief 取当前定位：路线/GPX 模拟优先，否则板载 GNSS，再否则手机 GNSS。
 * @return 有近期快照则为 true（搜星时 valid=false）。
 */
bool bicycle_runtime_poll_fix(struct bicycle_gnss_fix * out);

/** @brief 追加一段已骑路径；更新会话/圈距离与运动方位。 */
void bicycle_runtime_add_segment(double seg_m, float motion_deg, float speed_kph);

/** @brief 检测到新圈 — 圈数加一、锚定圈起点；距离保持累计。 */
void bicycle_runtime_on_lap_closed(void);

/** @brief 自当前圈开始起骑行的距离（米）。 */
double bicycle_runtime_lap_since_start_m(void);

/**
 * @brief 自行车 runtime set track metrics。
 */
void bicycle_runtime_set_track_metrics(uint16_t point_count, uint16_t km_marker_count);

/** -------------------------------------------------------------------------
 *  Cross-module event bus (lv_subject pointer slots).
 *  Replaces the former lv_pm_msg layer; sport metrics use subj_speed_* above.
 *  ------------------------------------------------------------------------- */

#ifndef BICYCLE_SUBJ_EVT_MAX
#define BICYCLE_SUBJ_EVT_MAX 8
#endif

typedef enum {
    BICYCLE_SUBJ_EVT_PAGE = 0,
    BICYCLE_SUBJ_EVT_MTP = 1,
} bicycle_subj_evt_id_t;

typedef enum {
    BICYCLE_PAGE_EVT_APPEARED = 1,
    BICYCLE_PAGE_EVT_DISAPPEARED,
} bicycle_page_evt_kind_t;

typedef struct {
    uint8_t page_id;
    const char * page_name;
    bicycle_page_evt_kind_t kind;
} bicycle_page_evt_t;

typedef struct {
    bool host_attached;
    bool transfer_active;
} bicycle_mtp_evt_t;

/**
 * @brief 自行车 runtime subj evt。
 */
lv_subject_t * bicycle_runtime_subj_evt(bicycle_subj_evt_id_t id);
/**
 * @brief 自行车 runtime evt send。
 */
void bicycle_runtime_evt_send(bicycle_subj_evt_id_t id, void * payload);
/**
 * @brief 自行车 runtime evt notify。
 */
void bicycle_runtime_evt_notify(bicycle_subj_evt_id_t id);
lv_observer_t * bicycle_runtime_evt_observe(bicycle_subj_evt_id_t id, lv_observer_cb_t cb,
                                            lv_obj_t * obj, void * user_data);

void bicycle_runtime_emit_page_evt(uint8_t page_id, const char * name,
                                   bicycle_page_evt_kind_t kind);
/**
 * @brief 自行车 runtime emit mtp evt。
 */
void bicycle_runtime_emit_mtp_evt(bool host_attached, bool transfer_active);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_RUNTIME_H */
