/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/sensor/bmp388_uorb.c
 *
 * Bosch BMP388 barometer — NuttX sensor (uORB) lower-half driver.
 * Compensation adapted from Zephyr bmp388 driver / Bosch BMP3-Sensor-API.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_I2C) && defined(CONFIG_SENSORS) && defined(CONFIG_BOARD_BMP388)

#include <nuttx/nuttx.h>

#include <errno.h>
#include <syslog.h>
#include <debug.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/sensors/sensor.h>
#include <nuttx/signal.h>
#include <nuttx/uorb.h>

#include "bmp388.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BMP388_CHIP_ID            0x50

#define BMP388_REG_CHIPID         0x00
#define BMP388_REG_ERR_REG        0x02
#define BMP388_REG_STATUS         0x03
#define BMP388_REG_DATA0          0x04
#define BMP388_REG_CALIB0         0x31
#define BMP388_REG_PWR_CTRL       0x1b
#define BMP388_REG_OSR            0x1c
#define BMP388_REG_ODR            0x1d
#define BMP388_REG_CONFIG         0x1f
#define BMP388_REG_CMD            0x7e

#define BMP388_CMD_SOFT_RESET     0xb6

#define BMP388_STATUS_CONF_ERR    (1 << 2)
#define BMP388_STATUS_DRDY_PRESS  (1 << 5)
#define BMP388_STATUS_DRDY_TEMP   (1 << 6)
#define BMP388_STATUS_DRDY        (BMP388_STATUS_DRDY_PRESS | \
                                   BMP388_STATUS_DRDY_TEMP)

#define BMP388_PWR_CTRL_PRESS_EN  (1 << 0)
#define BMP388_PWR_CTRL_TEMP_EN   (1 << 1)
#define BMP388_PWR_CTRL_MODE_POS  4
#define BMP388_PWR_CTRL_MODE_MASK (0x03 << BMP388_PWR_CTRL_MODE_POS)
#define BMP388_PWR_CTRL_MODE_SLEEP  (0x00 << BMP388_PWR_CTRL_MODE_POS)
#define BMP388_PWR_CTRL_MODE_FORCED (0x01 << BMP388_PWR_CTRL_MODE_POS)

#define BMP388_PWR_CTRL_FORCED    (BMP388_PWR_CTRL_PRESS_EN | \
                                   BMP388_PWR_CTRL_TEMP_EN | \
                                   BMP388_PWR_CTRL_MODE_FORCED)

#define BMP388_OSR_PRESSURE_POS   0
#define BMP388_OSR_TEMP_POS       3

/* Chip default: x2 oversampling for pressure and temperature. */

#define BMP388_DEFAULT_OSR        0x02

/* Used only if normal mode is enabled later; forced mode ignores ODR. */

#define BMP388_DEFAULT_ODR        0x03

#define BMP388_SAMPLE_SIZE        6
#define BMP388_FETCH_TIMEOUT_MS   100
#define BMP388_FORCED_DELAY_MS    20

#ifndef CONFIG_BOARD_BMP388_I2C_ADDR
#  define CONFIG_BOARD_BMP388_I2C_ADDR 0x76
#endif

#ifndef CONFIG_BOARD_BMP388_I2C_FREQUENCY
#  define CONFIG_BOARD_BMP388_I2C_FREQUENCY 400000
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

begin_packed_struct struct bmp388_calib_s
{
  uint16_t t1;
  uint16_t t2;
  int8_t   t3;
  int16_t  p1;
  int16_t  p2;
  int8_t   p3;
  int8_t   p4;
  uint16_t p5;
  uint16_t p6;
  int8_t   p7;
  int8_t   p8;
  int16_t  p9;
  int8_t   p10;
  int8_t   p11;
} end_packed_struct;

struct bmp388_sample_s
{
  uint32_t press;
  uint32_t raw_temp;
  int64_t  comp_temp;
};

struct bmp388_dev_s
{
  struct sensor_lowerhalf_s sensor_lower;
  FAR struct i2c_master_s *i2c;
  uint8_t addr;
  int freq;
  bool activated;
  struct bmp388_calib_s calib;
  struct bmp388_sample_s sample;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bmp388_getreg8(FAR struct bmp388_dev_s *priv, uint8_t regaddr,
                          FAR uint8_t *regval);
static int bmp388_putreg8(FAR struct bmp388_dev_s *priv, uint8_t regaddr,
                          uint8_t regval);
static int bmp388_getregs(FAR struct bmp388_dev_s *priv, uint8_t regaddr,
                          FAR uint8_t *buffer, uint8_t length);
static int bmp388_reg_update(FAR struct bmp388_dev_s *priv, uint8_t reg,
                             uint8_t mask, uint8_t val);

static int bmp388_set_interval(FAR struct sensor_lowerhalf_s *lower,
                               FAR struct file *filep,
                               FAR uint32_t *period_us);
static int bmp388_activate(FAR struct sensor_lowerhalf_s *lower,
                           FAR struct file *filep, bool enable);
static int bmp388_fetch(FAR struct sensor_lowerhalf_s *lower,
                        FAR struct file *filep,
                        FAR char *buffer, size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct sensor_ops_s g_bmp388_sensor_ops =
{
  .activate     = bmp388_activate,
  .fetch        = bmp388_fetch,
  .set_interval = bmp388_set_interval,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t bmp388_le16(FAR const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t bmp388_le16s(FAR const uint8_t *p)
{
  return (int16_t)bmp388_le16(p);
}

static uint32_t bmp388_le24(FAR const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static int bmp388_transfer(FAR struct bmp388_dev_s *priv,
                           FAR struct i2c_msg_s *msgs, int msgc)
{
  int ret = I2C_TRANSFER(priv->i2c, msgs, msgc);

  if (ret < 0)
    {
      snerr("I2C_TRANSFER failed: %d\n", ret);
    }

  return ret;
}

static int bmp388_getreg8(FAR struct bmp388_dev_s *priv, uint8_t regaddr,
                          FAR uint8_t *regval)
{
  struct i2c_msg_s msgs[2];

  msgs[0].frequency = priv->freq;
  msgs[0].addr      = priv->addr;
  msgs[0].flags     = 0;
  msgs[0].buffer    = &regaddr;
  msgs[0].length    = 1;

  msgs[1].frequency = priv->freq;
  msgs[1].addr      = priv->addr;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = regval;
  msgs[1].length    = 1;

  return bmp388_transfer(priv, msgs, 2);
}

static int bmp388_putreg8(FAR struct bmp388_dev_s *priv, uint8_t regaddr,
                          uint8_t regval)
{
  struct i2c_msg_s msg;
  uint8_t txbuf[2];

  txbuf[0] = regaddr;
  txbuf[1] = regval;

  msg.frequency = priv->freq;
  msg.addr      = priv->addr;
  msg.flags     = 0;
  msg.buffer    = txbuf;
  msg.length    = 2;

  return bmp388_transfer(priv, &msg, 1);
}

static int bmp388_getregs(FAR struct bmp388_dev_s *priv, uint8_t regaddr,
                          FAR uint8_t *buffer, uint8_t length)
{
  struct i2c_msg_s msgs[2];

  msgs[0].frequency = priv->freq;
  msgs[0].addr      = priv->addr;
  msgs[0].flags     = 0;
  msgs[0].buffer    = &regaddr;
  msgs[0].length    = 1;

  msgs[1].frequency = priv->freq;
  msgs[1].addr      = priv->addr;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = buffer;
  msgs[1].length    = length;

  return bmp388_transfer(priv, msgs, 2);
}

static int bmp388_reg_update(FAR struct bmp388_dev_s *priv, uint8_t reg,
                             uint8_t mask, uint8_t val)
{
  uint8_t oldv;
  uint8_t newv;
  int ret;

  ret = bmp388_getreg8(priv, reg, &oldv);
  if (ret < 0)
    {
      return ret;
    }

  newv = (oldv & ~mask) | (val & mask);
  if (newv == oldv)
    {
      return OK;
    }

  return bmp388_putreg8(priv, reg, newv);
}

static int bmp388_checkid(FAR struct bmp388_dev_s *priv)
{
  uint8_t chipid = 0;
  int ret;

  ret = bmp388_getreg8(priv, BMP388_REG_CHIPID, &chipid);
  if (ret < 0)
    {
      return ret;
    }

  sninfo("BMP388 chip id: 0x%02x\n", chipid);

  if (chipid != BMP388_CHIP_ID)
    {
      snerr("Unexpected BMP388 chip id 0x%02x\n", chipid);
      return -ENODEV;
    }

  return OK;
}

static int bmp388_load_calib(FAR struct bmp388_dev_s *priv)
{
  uint8_t buf[sizeof(struct bmp388_calib_s)];
  int ret;

  ret = bmp388_getregs(priv, BMP388_REG_CALIB0, buf, sizeof(buf));
  if (ret < 0)
    {
      return ret;
    }

  priv->calib.t1  = bmp388_le16(&buf[0]);
  priv->calib.t2  = bmp388_le16(&buf[2]);
  priv->calib.t3  = (int8_t)buf[4];
  priv->calib.p1  = bmp388_le16s(&buf[5]);
  priv->calib.p2  = bmp388_le16s(&buf[7]);
  priv->calib.p3  = (int8_t)buf[9];
  priv->calib.p4  = (int8_t)buf[10];
  priv->calib.p5  = bmp388_le16(&buf[11]);
  priv->calib.p6  = bmp388_le16(&buf[13]);
  priv->calib.p7  = (int8_t)buf[15];
  priv->calib.p8  = (int8_t)buf[16];
  priv->calib.p9  = bmp388_le16s(&buf[17]);
  priv->calib.p10 = (int8_t)buf[19];
  priv->calib.p11 = (int8_t)buf[20];

  return OK;
}

static int bmp388_hw_init(FAR struct bmp388_dev_s *priv)
{
  uint8_t err;
  int ret;

  syslog(LOG_INFO, "BRINGUP: BMP388 hw_init enter\n");

  ret = bmp388_putreg8(priv, BMP388_REG_CMD, BMP388_CMD_SOFT_RESET);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(10);

  ret = bmp388_checkid(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = bmp388_load_calib(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = bmp388_putreg8(priv, BMP388_REG_ODR, BMP388_DEFAULT_ODR);
  if (ret < 0)
    {
      return ret;
    }

  ret = bmp388_putreg8(priv, BMP388_REG_OSR, BMP388_DEFAULT_OSR);
  if (ret < 0)
    {
      return ret;
    }

  ret = bmp388_putreg8(priv, BMP388_REG_CONFIG, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = bmp388_putreg8(priv, BMP388_REG_PWR_CTRL, BMP388_PWR_CTRL_MODE_SLEEP);
  if (ret < 0)
    {
      return ret;
    }

  ret = bmp388_getreg8(priv, BMP388_REG_ERR_REG, &err);
  if (ret < 0)
    {
      return ret;
    }

  if (err & BMP388_STATUS_CONF_ERR)
    {
      snerr("BMP388 OSR/ODR configuration error\n");
      return -EINVAL;
    }

  syslog(LOG_INFO, "BRINGUP: BMP388 hw_init ok\n");
  return OK;
}

static void bmp388_compensate_temp(FAR struct bmp388_dev_s *priv)
{
  int64_t partial_data1;
  int64_t partial_data2;
  int64_t partial_data3;
  int64_t partial_data4;
  int64_t partial_data5;
  FAR struct bmp388_calib_s *cal = &priv->calib;
  FAR struct bmp388_sample_s *sample = &priv->sample;

  partial_data1 = ((int64_t)sample->raw_temp - ((int64_t)256 * cal->t1));
  partial_data2 = (int64_t)cal->t2 * partial_data1;
  partial_data3 = partial_data1 * partial_data1;
  partial_data4 = partial_data3 * (int64_t)cal->t3;
  partial_data5 = ((partial_data2 * 262144) + partial_data4);

  sample->comp_temp = partial_data5 / 4294967296;
}

static uint64_t bmp388_compensate_press(FAR struct bmp388_dev_s *priv)
{
  int64_t partial_data1;
  int64_t partial_data2;
  int64_t partial_data3;
  int64_t partial_data4;
  int64_t partial_data5;
  int64_t partial_data6;
  int64_t offset;
  int64_t sensitivity;
  uint64_t comp_press;
  FAR struct bmp388_calib_s *cal = &priv->calib;
  FAR struct bmp388_sample_s *sample = &priv->sample;
  int64_t t_lin = sample->comp_temp;
  uint32_t raw_pressure = sample->press;

  if (sample->comp_temp == 0)
    {
      bmp388_compensate_temp(priv);
      t_lin = sample->comp_temp;
    }

  partial_data1 = t_lin * t_lin;
  partial_data2 = partial_data1 / 64;
  partial_data3 = (partial_data2 * t_lin) / 256;
  partial_data4 = ((int64_t)cal->p8 * partial_data3) / 32;
  partial_data5 = ((int64_t)cal->p7 * partial_data1) * 16;
  partial_data6 = ((int64_t)cal->p6 * t_lin) * 4194304;
  offset = ((int64_t)cal->p5 * 140737488355328LL) + partial_data4 +
           partial_data5 + partial_data6;

  partial_data2 = ((int64_t)cal->p4 * partial_data3) / 32;
  partial_data4 = ((int64_t)cal->p3 * partial_data1) * 4;
  partial_data5 = ((int64_t)cal->p2 - 16384) * t_lin * 2097152;
  sensitivity = ((int64_t)cal->p1 - 16384) * 70368744177664LL +
                partial_data2 + partial_data4 + partial_data5;

  partial_data1 = (sensitivity / 16777216) * raw_pressure;
  partial_data2 = (int64_t)cal->p10 * t_lin;
  partial_data3 = partial_data2 + ((int64_t)65536 * cal->p9);
  partial_data4 = (partial_data3 * raw_pressure) / 8192;
  partial_data5 = (raw_pressure * (partial_data4 / 10)) / 512;
  partial_data5 = partial_data5 * 10;
  partial_data6 = ((int64_t)raw_pressure * (int64_t)raw_pressure);
  partial_data2 = ((int64_t)cal->p11 * partial_data6) / 65536;
  partial_data3 = (partial_data2 * raw_pressure) / 128;
  partial_data4 = (offset / 4) + partial_data1 + partial_data5 +
                  partial_data3;

  comp_press = (((uint64_t)partial_data4 * 25) / (uint64_t)1099511627776ULL);

  return comp_press;
}

static int bmp388_wait_drdy(FAR struct bmp388_dev_s *priv)
{
  uint8_t status = 0;
  int elapsed = 0;

  while (elapsed < BMP388_FETCH_TIMEOUT_MS)
    {
      int ret = bmp388_getreg8(priv, BMP388_REG_STATUS, &status);

      if (ret < 0)
        {
          return ret;
        }

      if ((status & BMP388_STATUS_DRDY) == BMP388_STATUS_DRDY)
        {
          return OK;
        }

      nxsig_usleep(5000);
      elapsed += 5;
    }

  return -ETIMEDOUT;
}

static int bmp388_read_sample(FAR struct bmp388_dev_s *priv)
{
  uint8_t raw[BMP388_SAMPLE_SIZE];
  int ret;

  /* Forced mode: one shot, then chip returns to sleep (BMP388 datasheet 3.3.2). */

  ret = bmp388_putreg8(priv, BMP388_REG_PWR_CTRL, BMP388_PWR_CTRL_FORCED);
  if (ret < 0)
    {
      return ret;
    }

  ret = bmp388_wait_drdy(priv);
  if (ret < 0)
    {
      /* Fall back to typical conversion time if status bits stay cleared. */

      nxsig_usleep(BMP388_FORCED_DELAY_MS * 1000);
    }

  ret = bmp388_getregs(priv, BMP388_REG_DATA0, raw, sizeof(raw));
  if (ret < 0)
    {
      return ret;
    }

  priv->sample.press = bmp388_le24(&raw[0]);
  priv->sample.raw_temp = bmp388_le24(&raw[3]);
  priv->sample.comp_temp = 0;

  return OK;
}

static int bmp388_set_interval(FAR struct sensor_lowerhalf_s *lower,
                               FAR struct file *filep,
                               FAR uint32_t *period_us)
{
  UNUSED(lower);
  UNUSED(filep);
  UNUSED(period_us);
  return OK;
}

static int bmp388_activate(FAR struct sensor_lowerhalf_s *lower,
                           FAR struct file *filep, bool enable)
{
  FAR struct bmp388_dev_s *priv = container_of(lower,
                                               struct bmp388_dev_s,
                                               sensor_lower);
  int ret;

  if (enable)
    {
      /* Subscriber open: keep chip in sleep; fetch triggers forced mode. */

      priv->activated = true;
      return OK;
    }

  ret = bmp388_reg_update(priv, BMP388_REG_PWR_CTRL,
                          BMP388_PWR_CTRL_MODE_MASK,
                          BMP388_PWR_CTRL_MODE_SLEEP);
  if (ret >= 0)
    {
      priv->activated = false;
    }

  return ret;
}

static int bmp388_fetch(FAR struct sensor_lowerhalf_s *lower,
                        FAR struct file *filep,
                        FAR char *buffer, size_t buflen)
{
  FAR struct bmp388_dev_s *priv = container_of(lower,
                                               struct bmp388_dev_s,
                                               sensor_lower);
  struct sensor_baro baro;
  struct timespec ts;
  uint64_t comp_press;
  int64_t temp_q;
  int ret;

  UNUSED(filep);

  if (buflen != sizeof(baro))
    {
      return -EINVAL;
    }

  ret = bmp388_read_sample(priv);
  if (ret < 0)
    {
      return ret;
    }

  bmp388_compensate_temp(priv);
  comp_press = bmp388_compensate_press(priv);

  temp_q = (priv->sample.comp_temp * 250000) / 16384;

  clock_systime_timespec(&ts);
  baro.timestamp = 1000000ull * ts.tv_sec + ts.tv_nsec / 1000;
  baro.pressure = (float)comp_press / 10000.0f;
  baro.temperature = (float)temp_q / 1000000.0f;

  memcpy(buffer, &baro, sizeof(baro));
  return (int)sizeof(baro);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bmp388_register(int devno, FAR struct i2c_master_s *i2c)
{
  FAR struct bmp388_dev_s *priv;
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->i2c = i2c;
  priv->addr = CONFIG_BOARD_BMP388_I2C_ADDR;
  priv->freq = CONFIG_BOARD_BMP388_I2C_FREQUENCY;
  priv->sensor_lower.ops = &g_bmp388_sensor_ops;
  priv->sensor_lower.type = SENSOR_TYPE_BAROMETER;

  ret = bmp388_hw_init(priv);
  if (ret < 0)
    {
      kmm_free(priv);
      return ret;
    }

  ret = sensor_register(&priv->sensor_lower, devno);
  if (ret < 0)
    {
      snerr("sensor_register failed: %d\n", ret);
      kmm_free(priv);
      return ret;
    }

  sninfo("BMP388 registered as /dev/uorb/sensor_baro%d\n", devno);
  return OK;
}

#endif /* CONFIG_I2C && CONFIG_SENSORS && CONFIG_BOARD_BMP388 */
