/**
 * @file myvendor_board_sensor.h
 * @brief 板卡传感器线程：I2C uORB 与 VBATS ADC 乒乓缓存，sys/UI/BLE 只取快照。
 *
 * BMI270 / BMP388 / MMC5983 的 I2C 访问，以及 `/dev/adc0` VBATS 采样，
 * 都集中在线程 `board_sensor`。某一路异常只定时重开那一路，不关整条
 * 总线。ADC 第一次只 TRIGGER，之后先 read 上次结果再 TRIGGER，避免
 * 立刻读写卡在 FIFO 上。结果双槽乒乓发布；其它路径不得再 open ADC。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_BOARD_SENSOR_H
#define MYVENDOR_BOARD_SENSOR_H

#include "myvendor_sys.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动板卡传感器线程（可重复调用）。
 * @return 0 成功；无 I2C 传感器且无 ADC 时为 -ENOTSUP；负 errno 为线程创建失败。
 */
int myvendor_board_sensor_start(void);

/**
 * @brief 拷贝未过期的 BMI270 加速度（m/s²）。
 */
void myvendor_board_sensor_accel_get(myvendor_sys_vec3_t *out);

/**
 * @brief 10 Hz 低通后的加速度，给坡度用；颠簸时 |a| 偏离 1g 则保持上次。
 */
void myvendor_board_sensor_accel_filt_get(myvendor_sys_vec3_t *out);

/**
 * @brief 拷贝未过期的 BMI270 陀螺（rad/s）。
 */
void myvendor_board_sensor_gyro_get(myvendor_sys_vec3_t *out);

/**
 * @brief 拷贝未过期的 BMP388 气压。
 */
void myvendor_board_sensor_baro_get(myvendor_sys_baro_t *out);

/**
 * @brief 拷贝未过期的 MMC5983 磁力计（µT）。
 */
void myvendor_board_sensor_mag_get(myvendor_sys_mag_t *out);

/**
 * @brief 已发布槽的 VBATS 毫伏（乒乓拷贝，不触发 ADC）。
 * @return >=0 毫伏；尚未采到为 -ENODEV；无 ADC 为 -ENOSYS。
 */
int myvendor_board_sensor_bat_mv(void);

/**
 * @brief 已发布槽的电量百分比（乒乓拷贝，不触发 ADC）。
 * @return -1 未知，否则 0～100。
 */
int myvendor_board_sensor_bat_pct(void);

/**
 * @brief 读取线程是否在跑。
 */
bool myvendor_board_sensor_alive(void);

/**
 * @brief 立刻再试已出现过的传感器（工厂页）。不关其它正常的路。
 */
void myvendor_board_sensor_request_reopen(void);

/**
 * @brief 某一路正在 close/open。
 */
bool myvendor_board_sensor_recovering(void);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_BOARD_SENSOR_H */
