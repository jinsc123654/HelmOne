/**
 * @file bicycle_env.h
 * @brief 码表环境：路网海拔钉绝对高度，气压补短时变化；IMU 坡度（低通+限速）与零点校准。
 */

#ifndef BICYCLE_ENV_H
#define BICYCLE_ENV_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void bicycle_env_init(void);

/** @brief 采一帧 IMU/气压并写入 runtime 海拔/坡度。 */
void bicycle_env_tick(void);

/**
 * @brief 当前吸附路网节点海拔（米）。无图或未吸附时 ok=false。
 *
 * 由地图定时器在 bicycle_env_tick 之前写入；实时海拔 = 路网绝对高 + 气压残差。
 */
void bicycle_env_set_dem_m(float m, bool ok);

/** @brief 开始 REC 时锚定爬升参考，避免把停放期间的气压漂记进去。 */
void bicycle_env_hold_gain_ref(void);

/**
 * @brief 骑行结束：把最后没骑满一段的净高度变化结一次账。
 *
 * 由 `bicycle_runtime_set_recording(false)` 在清 recording 之前调用；
 * 幂等，重复调用不会重复计入。
 */
void bicycle_env_flush_gain(void);

bool bicycle_env_imu_valid(void);
bool bicycle_env_baro_valid(void);
float bicycle_env_raw_grade_pct(void);
float bicycle_env_grade_pct(void);
float bicycle_env_offset_pct(void);
float bicycle_env_altitude_m(void);
float bicycle_env_hpa(void);

/**
 * @brief 把当前 IMU 俯仰当作 0% 坡度并写入 persist。
 * @return 校准成功则为 true。
 */
bool bicycle_env_calibrate(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_ENV_H */
