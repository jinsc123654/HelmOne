/****************************************************************************
 * vendor/.../drivers/sensor/sf32lb52_bmi270/sf32lb52_bmi270_chip.h
 *
 * BMI270 register map and low-level I2C helpers (board-local, I2C only).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __SF32LB52_BMI270_CHIP_H
#define __SF32LB52_BMI270_CHIP_H

#include <nuttx/config.h>

#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/signal.h>

#ifndef CONFIG_BOARD_BMI270_I2C_FREQUENCY
#  define CONFIG_BOARD_BMI270_I2C_FREQUENCY 400000
#endif

#define DEVID                   0x24

#define BMI270_CHIP_ID          (0x00)
#define BMI270_ERROR            (0x02)
#define BMI270_PMU_STAT         (0x03)
#define BMI270_DATA_0           (0x04)
#define BMI270_DATA_1           (0x05)
#define BMI270_DATA_2           (0x06)
#define BMI270_DATA_3           (0x07)
#define BMI270_DATA_4           (0x08)
#define BMI270_DATA_5           (0x09)
#define BMI270_DATA_6           (0x0a)
#define BMI270_DATA_7           (0x0b)
#define BMI270_DATA_8           (0x0c)
#define BMI270_DATA_9           (0x0d)
#define BMI270_DATA_10          (0x0e)
#define BMI270_DATA_11          (0x0f)
#define BMI270_DATA_12          (0x10)
#define BMI270_DATA_13          (0x11)
#define BMI270_DATA_14          (0x12)
#define BMI270_DATA_15          (0x13)
#define BMI270_DATA_16          (0x14)
#define BMI270_DATA_17          (0x15)
#define BMI270_DATA_18          (0x16)
#define BMI270_DATA_19          (0x17)
#define BMI270_SENSORTIME_0     (0x18)
#define BMI270_SENSORTIME_1     (0x19)
#define BMI270_SENSORTIME_2     (0x1a)
#define BMI270_EVENT            (0x1b)
#define BMI270_INT_STATUS_0     (0x1c)
#define BMI270_INT_STATUS_1     (0x1d)
#define BMI270_INTERNAL_STAT    (0x21)
#define BMI270_TEMPERATURE_0    (0x22)
#define BMI270_TEMPERATURE_1    (0x23)
#define BMI270_FIFO_LENGTH_0    (0x24)
#define BMI270_FIFO_LENGTH_1    (0x25)
#define BMI270_FIFO_DATA        (0x26)
#define BMI270_FEAT_PAGE        (0x2f)
#define BMI270_ACC_CONF         (0x40)
#define BMI270_ACC_RANGE        (0x41)
#define BMI270_GYR_CONF         (0x42)
#define BMI270_GYR_RANGE        (0x43)
#define BMI270_AUX_CONF         (0x44)
#define BMI270_FIFO_DOWNS       (0x45)
#define BMI270_FIFO_WTM_0       (0x46)
#define BMI270_FIFO_WTM_1       (0x47)
#define BMI270_FIFO_CONFIG_0    (0x48)
#define BMI270_FIFO_CONFIG_1    (0x49)
#define BMI270_SATURATION       (0x4a)
#define BMI270_INT1_IO_CTRL     (0x53)
#define BMI270_INT2_IO_CTRL     (0x54)
#define BMI270_INT_LATCH        (0x55)
#define BMI270_INT1_MAP_FEAT    (0x56)
#define BMI270_INT2_MAP_FEAT    (0x57)
#define BMI270_INT_MAP_DATA     (0x58)
#define BMI270_INIT_CTRL        (0x59)
#define BMI270_INIT_ADDR_0      (0x5b)
#define BMI270_INIT_ADDR_1      (0x5c)
#define BMI270_INIT_DATA        (0x5e)
#define BMI270_INTERNAL_ERROR   (0x5f)
#define BMI270_AUX_IF_CONF      (0x6b)
#define BMI270_AUX_RD_ADDR      (0x6c)
#define BMI270_AUX_WR_ADDR      (0x6d)
#define BMI270_AUX_WR_DATA      (0x6e)
#define BMI270_AUX_STATUS       (0x6f)
#define BMI270_PWR_CONF         (0x7c)
#define BMI270_PWR_CTRL         (0x7d)
#define BMI270_CMD              (0x7e)
#define BMI270_CMD_SOFTRESET    (0xb6)

#define INTSTAT_MSG_MASK        (0x01)
#define INTSTAT_MSG_INITOK      (0x01)

#define ACCEL_RANGE_2G          (0x00)
#define ACCEL_RANGE_4G          (0x01)
#define ACCEL_RANGE_8G          (0x02)
#define ACCEL_RANGE_16G         (0x03)

#define ACCEL_BW_OSR4           (0x00)
#define ACCEL_BW_OSR2           (0x01)
#define ACCEL_BW_NORM           (0x02)
#define ACCEL_NORMAL_AVG4       (2 << 4)

#define ACCEL_ODR_0_78HZ        (0x01)
#define ACCEL_ODR_1_56HZ        (0x02)
#define ACCEL_ODR_3_12HZ        (0x03)
#define ACCEL_ODR_6_25HZ        (0x04)
#define ACCEL_ODR_12_5HZ        (0x05)
#define ACCEL_ODR_25HZ          (0x06)
#define ACCEL_ODR_50HZ          (0x07)
#define ACCEL_ODR_100HZ         (0x08)
#define ACCEL_ODR_200HZ         (0x09)
#define ACCEL_ODR_400HZ         (0x0a)
#define ACCEL_ODR_800HZ         (0x0b)
#define ACCEL_ODR_1600HZ        (0x0c)

#define GYRO_RANGE_2000         (0x00)
#define GYRO_RANGE_1000         (0x01)
#define GYRO_RANGE_500          (0x02)
#define GYRO_RANGE_250          (0x03)
#define GYRO_RANGE_125          (0x04)

#define GYRO_ODR_25HZ           (0x06)
#define GYRO_ODR_50HZ           (0x07)
#define GYRO_ODR_100HZ          (0x08)
#define GYRO_ODR_200HZ          (0x09)
#define GYRO_ODR_400HZ          (0x0a)
#define GYRO_ODR_800HZ          (0x0b)
#define GYRO_ODR_1600HZ         (0x0c)
#define GYRO_ODR_3200HZ         (0x0d)

#define GYRO_BW_OSR4            (0x00)
#define GYRO_BW_OSR2            (0x01)
#define GYRO_BW_NORM            (0x02)
#define GYRO_NORMAL_MODE        (2 << 4)
#define GYRO_CIC_MODE           (3 << 4)

#define PWRCONF_FSW_ON          (1 << 1)

#define PWRCTRL_GYR_EN          (1 << 1)
#define PWRCTRL_ACC_EN          (1 << 2)

struct bmi270_dev_s
{
  FAR struct i2c_master_s *i2c;
  uint8_t                  addr;
  int                      freq;
};

extern const uint8_t g_bmi270_config_file[];

uint8_t bmi270_getreg8(FAR struct bmi270_dev_s *priv, uint8_t regaddr);
void bmi270_putreg8(FAR struct bmi270_dev_s *priv, uint8_t regaddr,
                    uint8_t regval);
void bmi270_getregs(FAR struct bmi270_dev_s *priv, uint8_t regaddr,
                    FAR uint8_t *regval, int len);
void bmi270_putregs(FAR struct bmi270_dev_s *priv, uint8_t regaddr,
                    FAR uint8_t *regval, int len);
void bmi270_set_normal_imu(FAR struct bmi270_dev_s *priv);
void bmi270_softreset(FAR struct bmi270_dev_s *priv);
int bmi270_init_seq(FAR struct bmi270_dev_s *priv);
int bmi270_checkid(FAR struct bmi270_dev_s *priv);

#endif /* __SF32LB52_BMI270_CHIP_H */
