/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/sensor/bmp388.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __MY_VENDOR_BOARDS_SF32LB52_DRIVERS_SENSOR_BMP388_H
#define __MY_VENDOR_BOARDS_SF32LB52_DRIVERS_SENSOR_BMP388_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_BOARD_BMP388

/****************************************************************************
 * Name: bmp388_register
 *
 * Description:
 *   Register BMP388 as a NuttX sensor (uORB baro device).
 *   Device node: /dev/uorb/sensor_baro<devno>
 *
 * Input Parameters:
 *   devno - Sensor instance number (0 -> sensor_baro0)
 *   i2c   - I2C bus already registered (e.g. from sifli_ap bringup)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 ****************************************************************************/

int bmp388_register(int devno, FAR struct i2c_master_s *i2c);

#endif /* CONFIG_BOARD_BMP388 */

#endif /* __MY_VENDOR_BOARDS_SF32LB52_DRIVERS_SENSOR_BMP388_H */
