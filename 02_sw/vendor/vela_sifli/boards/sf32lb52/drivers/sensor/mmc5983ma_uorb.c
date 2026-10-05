/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/sensor/mmc5983ma_uorb.c
 *
 * Memsic MMC5983MA 3-axis magnetometer — NuttX sensor (uORB) lower-half
 * driver. Register map and scaling follow the MMC5983MA datasheet and the
 * SparkFun MMC5983MA reference driver.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_I2C) && defined(CONFIG_SENSORS) && \
    defined(CONFIG_BOARD_MMC5983MA)

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

#include "mmc5983ma.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MMC5983MA_PROD_ID_VAL     0x30

#define MMC5983MA_REG_XOUT0       0x00
#define MMC5983MA_REG_STATUS      0x08
#define MMC5983MA_REG_CTRL0       0x09
#define MMC5983MA_REG_CTRL1       0x0a
#define MMC5983MA_REG_CTRL2       0x0b
#define MMC5983MA_REG_TOUT        0x07
#define MMC5983MA_REG_PROD_ID     0x2f

#define MMC5983MA_STATUS_MEAS_M_DONE  (1 << 0)
#define MMC5983MA_STATUS_MEAS_T_DONE  (1 << 1)

#define MMC5983MA_CTRL0_TM_M          (1 << 0)
#define MMC5983MA_CTRL0_TM_T          (1 << 1)
#define MMC5983MA_CTRL0_AUTO_SR_EN    (1 << 5)

#define MMC5983MA_CTRL1_BW0           (1 << 0)
#define MMC5983MA_CTRL1_BW1           (1 << 1)
#define MMC5983MA_CTRL1_SW_RST        (1 << 7)

/* Unsigned 18-bit output is centered at 2^17. */

#define MMC5983MA_OFFSET_CENTER       131072
#define MMC5983MA_SCALE_DIVISOR       131072.0f

/* Full-scale range is ±8 Gauss; output in microtesla (1 G = 100 uT). */

#define MMC5983MA_FULLSCALE_GAUSS     8.0f
#define MMC5983MA_GAUSS_TO_UT         100.0f

#define MMC5983MA_FETCH_TIMEOUT_MS    50
#define MMC5983MA_RESET_DELAY_MS      15
#define MMC5983MA_DEFAULT_BW_HZ       400

#ifndef CONFIG_BOARD_MMC5983MA_I2C_ADDR
#  define CONFIG_BOARD_MMC5983MA_I2C_ADDR 0x30
#endif

#ifndef CONFIG_BOARD_MMC5983MA_I2C_FREQUENCY
#  define CONFIG_BOARD_MMC5983MA_I2C_FREQUENCY 400000
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct mmc5983ma_dev_s
{
  struct sensor_lowerhalf_s sensor_lower;
  FAR struct i2c_master_s *i2c;
  uint8_t addr;
  int freq;
  bool activated;
  uint8_t ctrl0_shadow;
  uint8_t ctrl1_shadow;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int mmc5983ma_getreg8(FAR struct mmc5983ma_dev_s *priv, uint8_t regaddr,
                             FAR uint8_t *regval);
static int mmc5983ma_putreg8(FAR struct mmc5983ma_dev_s *priv, uint8_t regaddr,
                             uint8_t regval);
static int mmc5983ma_getregs(FAR struct mmc5983ma_dev_s *priv, uint8_t regaddr,
                             FAR uint8_t *buffer, uint8_t length);

static int mmc5983ma_set_interval(FAR struct sensor_lowerhalf_s *lower,
                                  FAR struct file *filep,
                                  FAR uint32_t *period_us);
static int mmc5983ma_activate(FAR struct sensor_lowerhalf_s *lower,
                              FAR struct file *filep, bool enable);
static int mmc5983ma_fetch(FAR struct sensor_lowerhalf_s *lower,
                           FAR struct file *filep,
                           FAR char *buffer, size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct sensor_ops_s g_mmc5983ma_sensor_ops =
{
  .activate     = mmc5983ma_activate,
  .fetch        = mmc5983ma_fetch,
  .set_interval = mmc5983ma_set_interval,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Board mount: MMC5983MA on PCB rear. Displayed heading needed 270°
 * clockwise from the previous 180° map: (x, y) -> (-y, x). Z inverted
 * because the package faces away from the screen.
 */

static void mmc5983ma_board_axes(FAR float *x, FAR float *y, FAR float *z)
{
  float cx = *x;
  float cy = *y;

  *x = -cy;
  *y = cx;
  *z = -(*z);
}

static int mmc5983ma_transfer(FAR struct mmc5983ma_dev_s *priv,
                              FAR struct i2c_msg_s *msgs, int msgc)
{
  return I2C_TRANSFER(priv->i2c, msgs, msgc);
}

static int mmc5983ma_getreg8(FAR struct mmc5983ma_dev_s *priv, uint8_t regaddr,
                             FAR uint8_t *regval)
{
  struct i2c_msg_s msgs[2];
  int ret;

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

  ret = mmc5983ma_transfer(priv, msgs, 2);
  return ret < 0 ? ret : OK;
}

static int mmc5983ma_putreg8(FAR struct mmc5983ma_dev_s *priv, uint8_t regaddr,
                             uint8_t regval)
{
  struct i2c_msg_s msg;
  uint8_t buf[2];
  int ret;

  buf[0] = regaddr;
  buf[1] = regval;

  msg.frequency = priv->freq;
  msg.addr      = priv->addr;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  ret = mmc5983ma_transfer(priv, &msg, 1);
  return ret < 0 ? ret : OK;
}

static int mmc5983ma_getregs(FAR struct mmc5983ma_dev_s *priv, uint8_t regaddr,
                             FAR uint8_t *buffer, uint8_t length)
{
  struct i2c_msg_s msgs[2];
  int ret;

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

  ret = mmc5983ma_transfer(priv, msgs, 2);
  return ret < 0 ? ret : OK;
}

static int mmc5983ma_checkid(FAR struct mmc5983ma_dev_s *priv)
{
  uint8_t prodid;
  int ret;

  ret = mmc5983ma_getreg8(priv, MMC5983MA_REG_PROD_ID, &prodid);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO, "BRINGUP: MMC5983MA product id: 0x%02x\n", prodid);

  if (prodid != MMC5983MA_PROD_ID_VAL)
    {
      syslog(LOG_ERR,
             "BRINGUP: MMC5983MA unexpected product id 0x%02x\n", prodid);
      return -ENODEV;
    }

  return OK;
}

static void mmc5983ma_ctrl1_bandwidth(FAR struct mmc5983ma_dev_s *priv,
                                      int bandwidth_hz)
{
  priv->ctrl1_shadow &= ~(MMC5983MA_CTRL1_BW0 | MMC5983MA_CTRL1_BW1);

  switch (bandwidth_hz)
    {
      case 800:
        priv->ctrl1_shadow |= MMC5983MA_CTRL1_BW1;
        break;

      case 400:
        priv->ctrl1_shadow |= MMC5983MA_CTRL1_BW0 | MMC5983MA_CTRL1_BW1;
        break;

      case 200:
        priv->ctrl1_shadow |= MMC5983MA_CTRL1_BW0;
        break;

      default:
        /* 100 Hz */
        break;
    }
}

static int mmc5983ma_hw_init(FAR struct mmc5983ma_dev_s *priv)
{
  int ret;

  syslog(LOG_INFO, "BRINGUP: MMC5983MA hw_init enter\n");

  ret = mmc5983ma_checkid(priv);
  if (ret < 0)
    {
      return ret;
    }

  priv->ctrl0_shadow = MMC5983MA_CTRL0_AUTO_SR_EN;
  priv->ctrl1_shadow = 0;

  mmc5983ma_ctrl1_bandwidth(priv, MMC5983MA_DEFAULT_BW_HZ);

  /* Software reset via shadow register (avoid read-modify-write quirks). */

  ret = mmc5983ma_putreg8(priv, MMC5983MA_REG_CTRL1,
                          priv->ctrl1_shadow | MMC5983MA_CTRL1_SW_RST);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(MMC5983MA_RESET_DELAY_MS);

  ret = mmc5983ma_putreg8(priv, MMC5983MA_REG_CTRL1, priv->ctrl1_shadow);
  if (ret < 0)
    {
      return ret;
    }

  ret = mmc5983ma_putreg8(priv, MMC5983MA_REG_CTRL0, priv->ctrl0_shadow);
  if (ret >= 0)
    {
      syslog(LOG_INFO, "BRINGUP: MMC5983MA hw_init ok\n");
    }

  return ret;
}

static uint32_t mmc5983ma_parse_axis(FAR const uint8_t *raw, int shift)
{
  uint32_t value;

  value  = ((uint32_t)raw[0] << 10);
  value |= ((uint32_t)raw[1] << 2);
  value |= ((uint32_t)raw[2] >> shift) & 0x03;

  return value;
}

static float mmc5983ma_raw_to_ut(int32_t raw)
{
  float centered;

  centered = (float)(raw - MMC5983MA_OFFSET_CENTER);
  return centered * MMC5983MA_FULLSCALE_GAUSS * MMC5983MA_GAUSS_TO_UT /
         MMC5983MA_SCALE_DIVISOR;
}

static int mmc5983ma_wait_meas_done(FAR struct mmc5983ma_dev_s *priv)
{
  int elapsed = 0;
  uint8_t status;
  int ret;

  while (elapsed < MMC5983MA_FETCH_TIMEOUT_MS)
    {
      ret = mmc5983ma_getreg8(priv, MMC5983MA_REG_STATUS, &status);
      if (ret < 0)
        {
          return ret;
        }

      if ((status & MMC5983MA_STATUS_MEAS_M_DONE) != 0)
        {
          return OK;
        }

      nxsig_usleep(1000);
      elapsed++;
    }

  return -ETIMEDOUT;
}

static int mmc5983ma_read_temp(FAR struct mmc5983ma_dev_s *priv,
                               FAR float *temp_c)
{
  uint8_t status;
  uint8_t tout;
  int elapsed = 0;
  int ret;

  if (temp_c == NULL)
    {
      return OK;
    }

  priv->ctrl0_shadow |= MMC5983MA_CTRL0_TM_T;

  ret = mmc5983ma_putreg8(priv, MMC5983MA_REG_CTRL0, priv->ctrl0_shadow);
  if (ret < 0)
    {
      priv->ctrl0_shadow &= ~MMC5983MA_CTRL0_TM_T;
      *temp_c = 0.0f;
      return ret;
    }

  while (elapsed < MMC5983MA_FETCH_TIMEOUT_MS)
    {
      ret = mmc5983ma_getreg8(priv, MMC5983MA_REG_STATUS, &status);
      if (ret < 0)
        {
          break;
        }

      if ((status & MMC5983MA_STATUS_MEAS_T_DONE) != 0)
        {
          break;
        }

      nxsig_usleep(1000);
      elapsed++;
    }

  priv->ctrl0_shadow &= ~MMC5983MA_CTRL0_TM_T;

  ret = mmc5983ma_getreg8(priv, MMC5983MA_REG_TOUT, &tout);
  if (ret < 0)
    {
      *temp_c = 0.0f;
      return ret;
    }

  *temp_c = -75.0f + ((float)tout * 200.0f / 255.0f);
  return OK;
}

static int mmc5983ma_read_sample(FAR struct mmc5983ma_dev_s *priv,
                                 FAR int32_t *x, FAR int32_t *y,
                                 FAR int32_t *z, FAR float *temp_c)
{
  uint8_t raw[7];
  int ret;

  priv->ctrl0_shadow |= MMC5983MA_CTRL0_TM_M;

  ret = mmc5983ma_putreg8(priv, MMC5983MA_REG_CTRL0, priv->ctrl0_shadow);
  if (ret < 0)
    {
      priv->ctrl0_shadow &= ~MMC5983MA_CTRL0_TM_M;
      return ret;
    }

  ret = mmc5983ma_wait_meas_done(priv);
  priv->ctrl0_shadow &= ~MMC5983MA_CTRL0_TM_M;

  if (ret < 0)
    {
      /* Fall through and return the last latched field values. */
    }

  ret = mmc5983ma_getregs(priv, MMC5983MA_REG_XOUT0, raw, sizeof(raw));
  if (ret < 0)
    {
      return ret;
    }

  *x = (int32_t)mmc5983ma_parse_axis(&raw[0], 6);
  *y = (int32_t)mmc5983ma_parse_axis(&raw[2], 4);
  *z = (int32_t)mmc5983ma_parse_axis(&raw[4], 2);

  if (temp_c != NULL)
    {
      ret = mmc5983ma_read_temp(priv, temp_c);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

static int mmc5983ma_set_interval(FAR struct sensor_lowerhalf_s *lower,
                                  FAR struct file *filep,
                                  FAR uint32_t *period_us)
{
  UNUSED(lower);
  UNUSED(filep);
  UNUSED(period_us);
  return OK;
}

static int mmc5983ma_activate(FAR struct sensor_lowerhalf_s *lower,
                              FAR struct file *filep, bool enable)
{
  FAR struct mmc5983ma_dev_s *priv = container_of(lower,
                                                  struct mmc5983ma_dev_s,
                                                  sensor_lower);

  UNUSED(filep);

  priv->activated = enable;
  return OK;
}

static int mmc5983ma_fetch(FAR struct sensor_lowerhalf_s *lower,
                           FAR struct file *filep,
                           FAR char *buffer, size_t buflen)
{
  FAR struct mmc5983ma_dev_s *priv = container_of(lower,
                                                  struct mmc5983ma_dev_s,
                                                  sensor_lower);
  struct sensor_mag mag;
  int32_t raw_x;
  int32_t raw_y;
  int32_t raw_z;
  int ret;

  UNUSED(filep);

  if (buflen != sizeof(mag))
    {
      return -EINVAL;
    }

  ret = mmc5983ma_read_sample(priv, &raw_x, &raw_y, &raw_z, &mag.temperature);
  if (ret < 0)
    {
      return ret;
    }

  mag.timestamp = sensor_get_timestamp();
  mag.x = mmc5983ma_raw_to_ut(raw_x);
  mag.y = mmc5983ma_raw_to_ut(raw_y);
  mag.z = mmc5983ma_raw_to_ut(raw_z);
  mmc5983ma_board_axes(&mag.x, &mag.y, &mag.z);
  mag.status = 0;

  memcpy(buffer, &mag, sizeof(mag));
  return (int)sizeof(mag);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int mmc5983ma_register(int devno, FAR struct i2c_master_s *i2c)
{
  FAR struct mmc5983ma_dev_s *priv;
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
  priv->addr = CONFIG_BOARD_MMC5983MA_I2C_ADDR;
  priv->freq = CONFIG_BOARD_MMC5983MA_I2C_FREQUENCY;
  priv->sensor_lower.ops = &g_mmc5983ma_sensor_ops;
  priv->sensor_lower.type = SENSOR_TYPE_MAGNETIC_FIELD;

  ret = mmc5983ma_hw_init(priv);
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

  sninfo("MMC5983MA registered as /dev/uorb/sensor_mag%d\n", devno);
  return OK;
}

#endif /* CONFIG_I2C && CONFIG_SENSORS && CONFIG_BOARD_MMC5983MA */
