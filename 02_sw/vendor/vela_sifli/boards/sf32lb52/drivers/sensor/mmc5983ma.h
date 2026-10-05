/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/sensor/mmc5983ma.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __MY_VENDOR_BOARDS_SF32LB52_DRIVERS_SENSOR_MMC5983MA_H
#define __MY_VENDOR_BOARDS_SF32LB52_DRIVERS_SENSOR_MMC5983MA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_BOARD_MMC5983MA

/****************************************************************************
 * Name: mmc5983ma_register
 *
 * Description:
 *   Register MMC5983MA as a NuttX sensor (uORB magnetometer device).
 *
 * Input Parameters:
 *   devno - Sensor instance number (e.g. 0 -> /dev/uorb/sensor_mag0)
 *   i2c   - I2C master for the board sensor bus (/dev/i2c0)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int mmc5983ma_register(int devno, FAR struct i2c_master_s *i2c);

#endif /* CONFIG_BOARD_MMC5983MA */

#endif /* __MY_VENDOR_BOARDS_SF32LB52_DRIVERS_SENSOR_MMC5983MA_H */
