/**
 * @file sifli_gpio.c
 * @brief SiFli GPIO NuttX 上层驱动：输入/输出/中断引脚注册到 /dev/gpio。
 *
 * 引脚表来自 sf32lb52_devkit_lcd.h（BOARD_NGPIO*、GPIO_IN1 等）。
 * 供 /apps/examples/gpio 与板级代码通过 gpio_pin_register 访问。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/clock.h>
#include <nuttx/wdog.h>
#include <nuttx/ioexpander/gpio.h>

#include <arch/board/board.h>

#include "sifli_gpio.h"
#include "sf32lb52_devkit_lcd.h"



#if defined(CONFIG_DEV_GPIO) && !defined(CONFIG_GPIO_LOWER_HALF)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/** @brief NuttX gpio_dev_s 包装，带板级引脚索引。 */
struct sifli_gpio_dev_s
{
  struct gpio_dev_s gpio;
  uint8_t id; /**< g_gpioinputs/g_gpiooutputs 数组下标。 */
};

/** @brief 带中断回调的 GPIO 设备。 */
struct sifli_gpint_dev_s
{
  struct sifli_gpio_dev_s sifli_gpio;
  pin_interrupt_t callback; /**< NuttX 中断回调。 */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int gpin_read(struct gpio_dev_s *dev, bool *value);
static int gpin_write(struct gpio_dev_s *dev, bool value);
static int gpin_setpintype(struct gpio_dev_s *dev, enum gpio_pintype_e pintype);
static int gpout_read(struct gpio_dev_s *dev, bool *value);
static int gpout_write(struct gpio_dev_s *dev, bool value);
static int gpout_setpintype(struct gpio_dev_s *dev, enum gpio_pintype_e pintype);
static int gpint_read(struct gpio_dev_s *dev, bool *value);
static int gpint_write(struct gpio_dev_s *dev, bool value);
static int gpint_attach(struct gpio_dev_s *dev,
                        pin_interrupt_t callback);
static int gpint_enable(struct gpio_dev_s *dev, bool enable);
static int gpint_setpintype(struct gpio_dev_s *dev, enum gpio_pintype_e pintype);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gpio_operations_s gpin_ops =
{
  .go_read   = gpin_read,
  .go_write  = gpin_write,
  .go_attach = NULL,
  .go_enable = NULL,
  .go_setpintype = gpin_setpintype,  
}; /**< 输入引脚操作表。 */

static const struct gpio_operations_s gpout_ops =
{
  .go_read   = gpout_read,
  .go_write  = gpout_write,
  .go_attach = NULL,
  .go_enable = NULL,
  .go_setpintype = gpout_setpintype,  
}; /**< 输出引脚操作表。 */

static const struct gpio_operations_s gpint_ops =
{
  .go_read   = gpint_read,
  .go_write  = gpint_write,
  .go_attach = gpint_attach,
  .go_enable = gpint_enable,
  .go_setpintype = gpint_setpintype,
}; /**< 中断引脚操作表。 */

#if BOARD_NGPIOIN > 0
/** 输入引脚 HAL 编码表。 */
static const uint32_t g_gpioinputs[BOARD_NGPIOIN] =
{
  GPIO_IN1,
};

static struct sifli_gpio_dev_s g_gpin[BOARD_NGPIOIN]; /**< 输入引脚设备实例。 */
#endif

#if BOARD_NGPIOOUT
/** 输出引脚 HAL 编码表。 */
static const uint32_t g_gpiooutputs[BOARD_NGPIOOUT] =
{
  GPIO_OUT1,
};

static struct sifli_gpio_dev_s g_gpout[BOARD_NGPIOOUT]; /**< 输出引脚设备实例。 */
#endif

#if BOARD_NGPIOINT > 0
/** 中断输入引脚 HAL 编码表。 */
static const uint32_t g_gpiointinputs[BOARD_NGPIOINT] =
{
  GPIO_INT1,
};

static struct sifli_gpint_dev_s g_gpint[BOARD_NGPIOINT]; /**< 中断引脚设备实例。 */
#endif

/** @brief GPIO 中断底半部，转发至 NuttX callback。 */
static void sifli_gpio_interrupt(void *arg)
{
  struct sifli_gpint_dev_s *gpint_dev =
                        (struct sifli_gpint_dev_s *)arg;

  DEBUGASSERT(gpint_dev != NULL && gpint_dev->callback != NULL);
  gpioinfo("Interrupt! callback=%p\n", gpint_dev->callback);

  gpint_dev->callback(&gpint_dev->sifli_gpio.gpio,
                       gpint_dev->sifli_gpio.id);
  return;
}

/** @brief 读输入引脚电平。 */
static int gpin_read(struct gpio_dev_s *dev, bool *value)
{
  struct sifli_gpio_dev_s *gpio_dev =
                        (struct sifli_gpio_dev_s *)dev;

  DEBUGASSERT(gpio_dev != NULL && value != NULL);
  DEBUGASSERT(gpio_dev->id < BOARD_NGPIOIN);
  gpioinfo("Reading...\n");

  *value = sifli_gpio_read(g_gpioinputs[gpio_dev->id]);
  return OK;
}

/** @brief 写输入引脚（动态切输出时）。 */
static int gpin_write(struct gpio_dev_s *dev, bool value)
{
  struct sifli_gpio_dev_s *gpio_dev =
                             (struct sifli_gpio_dev_s *)dev;

  DEBUGASSERT(gpio_dev != NULL);
  DEBUGASSERT(gpio_dev->id < BOARD_NGPIOIN);
  gpioinfo("Writing %d\n", (int)value);

  sifli_gpio_write(g_gpioinputs[gpio_dev->id], value);
  return OK;
}

/** @brief 设置输入引脚类型（输入/推挽或开漏输出）。 */
static int gpin_setpintype(struct gpio_dev_s *dev, enum gpio_pintype_e pintype)
{
  struct sifli_gpio_dev_s *gpio_dev =
                              (struct sifli_gpio_dev_s *)dev;

  DEBUGASSERT(gpio_dev != NULL);
  DEBUGASSERT(gpio_dev->id < BOARD_NGPIOIN);

  dev->gp_pintype = pintype;

  if ((pintype == GPIO_OUTPUT_PIN) || (pintype == GPIO_OUTPUT_PIN_OPENDRAIN))
  {
    sifli_gpio_config(g_gpioinputs[gpio_dev->id], GPIO_OUTPUT);
  }
  else
  {
    sifli_gpio_config(g_gpioinputs[gpio_dev->id], GPIO_INPUT);
  }
 
  return OK;
}


/** @brief 读输出引脚电平。 */
static int gpout_read(struct gpio_dev_s *dev, bool *value)
{
  struct sifli_gpio_dev_s *gpio_dev =
                        (struct sifli_gpio_dev_s *)dev;

  DEBUGASSERT(gpio_dev != NULL && value != NULL);
  DEBUGASSERT(gpio_dev->id < BOARD_NGPIOOUT);
  gpioinfo("Reading...\n");

  *value = sifli_gpio_read(g_gpiooutputs[gpio_dev->id]);
  return OK;
}

/** @brief 写输出引脚电平。 */
static int gpout_write(struct gpio_dev_s *dev, bool value)
{
  struct sifli_gpio_dev_s *gpio_dev =
                             (struct sifli_gpio_dev_s *)dev;

  DEBUGASSERT(gpio_dev != NULL);
  DEBUGASSERT(gpio_dev->id < BOARD_NGPIOOUT);
  gpioinfo("Writing %d\n", (int)value);

  sifli_gpio_write(g_gpiooutputs[gpio_dev->id], value);
  return OK;
}

/** @brief 设置输出引脚类型。 */
static int gpout_setpintype(struct gpio_dev_s *dev, enum gpio_pintype_e pintype)
{
  struct sifli_gpio_dev_s *gpio_dev =
                              (struct sifli_gpio_dev_s *)dev;

  DEBUGASSERT(gpio_dev != NULL);
  DEBUGASSERT(gpio_dev->id < BOARD_NGPIOOUT);

  dev->gp_pintype = pintype;

  if ((pintype == GPIO_OUTPUT_PIN) || (pintype == GPIO_OUTPUT_PIN_OPENDRAIN))
  {
    sifli_gpio_config(g_gpiooutputs[gpio_dev->id], GPIO_OUTPUT);
  }
  else
  {
    sifli_gpio_config(g_gpiooutputs[gpio_dev->id], GPIO_INPUT);
  }
 
  return OK;
}

/** @brief 读中断引脚电平。 */
static int gpint_read(struct gpio_dev_s *dev, bool *value)
{
  struct sifli_gpint_dev_s *gpint_dev =
                              (struct sifli_gpint_dev_s *)dev;

  DEBUGASSERT(gpint_dev != NULL && value != NULL);
  DEBUGASSERT(gpint_dev->sifli_gpio.id < BOARD_NGPIOINT);
  gpioinfo("Reading int pin...\n");

  *value = sifli_gpio_read(g_gpiointinputs[gpint_dev->sifli_gpio.id]);
  return OK;
}

/** @brief 写中断引脚（动态切输出时）。 */
static int gpint_write(struct gpio_dev_s *dev, bool value)
{
  struct sifli_gpint_dev_s *gpint_dev =
                             (struct sifli_gpint_dev_s *)dev;

  DEBUGASSERT(gpint_dev != NULL);
  DEBUGASSERT(gpint_dev->sifli_gpio.id < BOARD_NGPIOINT);
  gpioinfo("Writing %d\n", (int)value);

  sifli_gpio_write(g_gpiointinputs[gpint_dev->sifli_gpio.id], value);
  return OK;
}


/** @brief 挂接 NuttX 中断回调。 */
static int gpint_attach(struct gpio_dev_s *dev,
                        pin_interrupt_t callback)
{
  struct sifli_gpint_dev_s *gpint_dev =
                             (struct sifli_gpint_dev_s *)dev;

  gpioinfo("Attaching the callback\n");

  /* Make sure the interrupt is disabled */

  sifli_gpio_set_event(g_gpiointinputs[gpint_dev->sifli_gpio.id], false,
                     false, NULL, NULL);

  gpioinfo("Attach %p\n", callback);
  gpint_dev->callback = callback;
  return OK;
}

/** @brief 使能或禁用上升沿中断。 */
static int gpint_enable(struct gpio_dev_s *dev, bool enable)
{
  struct sifli_gpint_dev_s *gpint_dev =
                              (struct sifli_gpint_dev_s *)dev;

  if (enable)
    {
      if (gpint_dev->callback != NULL)
        {
          gpioinfo("Enabling the interrupt\n");

          /* Configure the interrupt for rising edge */

          sifli_gpio_set_event(g_gpiointinputs[gpint_dev->sifli_gpio.id],
                             true, true, sifli_gpio_interrupt,
                             &g_gpint[gpint_dev->sifli_gpio.id]);
        }
    }
  else
    {
      gpioinfo("Disable the interrupt\n");
      sifli_gpio_set_event(g_gpiointinputs[gpint_dev->sifli_gpio.id],
                         false, false, NULL, NULL);
    }

  return OK;
}

/** @brief 设置中断引脚类型。 */
static int gpint_setpintype(struct gpio_dev_s *dev, enum gpio_pintype_e pintype)
{
  struct sifli_gpint_dev_s *gpint_dev =
                              (struct sifli_gpint_dev_s *)dev;

  DEBUGASSERT(gpint_dev != NULL);
  DEBUGASSERT(gpint_dev->sifli_gpio.id < BOARD_NGPIOINT);

  dev->gp_pintype = pintype;

  if ((pintype == GPIO_OUTPUT_PIN) || (pintype == GPIO_OUTPUT_PIN_OPENDRAIN))
  {
    sifli_gpio_config(g_gpiointinputs[gpint_dev->sifli_gpio.id], GPIO_OUTPUT);
  }
  else
  {
    sifli_gpio_config(g_gpiointinputs[gpint_dev->sifli_gpio.id], GPIO_INPUT);
  }
 
  return OK;
}



/**
 * @brief 初始化并注册板载 GPIO 引脚到 NuttX。
 * @return 0 成功。
 */
int sifli_gpio_initialize(void)
{
  int i;
  int pincount = 0;
#if BOARD_NGPIOIN > 0
  for (i = 0; i < BOARD_NGPIOIN; i++)
    {
      /* Setup and register the GPIO pin */

      g_gpin[i].gpio.gp_pintype = GPIO_INPUT_PIN;
      g_gpin[i].gpio.gp_ops     = &gpin_ops;
      g_gpin[i].id              = i;
      gpio_pin_register(&g_gpin[i].gpio, pincount);

      /* Configure the pin that will be used as input */

      sifli_gpio_config(g_gpioinputs[i], GPIO_INPUT);

      pincount++;
    }
#endif

#if BOARD_NGPIOOUT > 0
  for (i = 0; i < BOARD_NGPIOOUT; i++)
    {
      /* Setup and register the GPIO pin */

      g_gpout[i].gpio.gp_pintype = GPIO_OUTPUT_PIN;
      g_gpout[i].gpio.gp_ops     = &gpout_ops;
      g_gpout[i].id              = i;
      gpio_pin_register(&g_gpout[i].gpio, pincount);

      /* Configure the pin that will be used as output */

      sifli_gpio_config(g_gpiooutputs[i], GPIO_OUTPUT);
      sifli_gpio_write(g_gpiooutputs[i], 0);

      pincount++;
    }
#endif

#if BOARD_NGPIOINT > 0
  for (i = 0; i < BOARD_NGPIOINT; i++)
    {
      /* Setup and register the GPIO pin */

      g_gpint[i].sifli_gpio.gpio.gp_pintype = GPIO_INTERRUPT_BOTH_PIN;
      g_gpint[i].sifli_gpio.gpio.gp_ops     = &gpint_ops;
      g_gpint[i].sifli_gpio.id              = i;
      gpio_pin_register(&g_gpint[i].sifli_gpio.gpio, pincount);

      /* Configure the pin that will be used as interrupt input */

      sifli_gpio_config(g_gpiointinputs[i], GPIO_INPUT);

      pincount++;
    }
#endif

  return 0;
}
#endif /* CONFIG_DEV_GPIO && !CONFIG_GPIO_LOWER_HALF */
