/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/sensor/sf32lb52_bmi270/sf32lb52_bmi270.h
 *
 * SF32LB52 board BMI270 IMU — registers /dev/uorb/sensor_accelN and gyroN.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __SF32LB52_BMI270_H
#define __SF32LB52_BMI270_H

#include <nuttx/config.h>
#include <nuttx/compiler.h>

struct i2c_master_s;

#if defined(CONFIG_BOARD_BMI270)

int sf32lb52_bmi270_register(int devno, FAR struct i2c_master_s *i2c,
                             uint8_t addr);

/**
 * @brief 软复位并重载配置；若开机 register 失败则补注册 uORB。
 * @return 0 成功，负 errno。
 */
int sf32lb52_bmi270_recover(void);

#endif /* CONFIG_BOARD_BMI270 */

#endif /* __SF32LB52_BMI270_H */
