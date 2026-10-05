/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/gnss/sf32lb52_l96.c
 *
 * u-blox MAX-M10S-00B-01 board support: PA43 GNSS_PWR (module VCC) and
 * UART2 baud preset. Official low power is cutting VCC, not closing
 * host USART2.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_BOARD_L96_GNSS)

#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include <debug.h>

#include "bf0_hal.h"
#include "sf32lb_serial.h"

#include "sf32lb52_l96.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PA43 / GPIO_A43 — GNSS_PWR. */

#define GNSS_PWR_GPIO_PIN     43

#ifdef CONFIG_BOARD_L96_GNSS_PWR_ACTIVE_HIGH
#  define GNSS_PWR_ON_LEVEL   GPIO_PIN_SET
#  define GNSS_PWR_OFF_LEVEL  GPIO_PIN_RESET
#  define GNSS_PWR_PIN_PULL   PIN_PULLDOWN
#else
#  define GNSS_PWR_ON_LEVEL   GPIO_PIN_RESET
#  define GNSS_PWR_OFF_LEVEL  GPIO_PIN_SET
#  define GNSS_PWR_PIN_PULL   PIN_PULLUP
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void gnss_pwr_gpio_write(bool power_on)
{
  GPIO_InitTypeDef gpio =
  {
    .Pin  = GNSS_PWR_GPIO_PIN,
    .Mode = GPIO_MODE_OUTPUT,
    .Pull = GPIO_NOPULL,
  };

  HAL_GPIO_Init(hwp_gpio1, &gpio);
  HAL_GPIO_WritePin(hwp_gpio1, GNSS_PWR_GPIO_PIN,
                    power_on ? GNSS_PWR_ON_LEVEL : GNSS_PWR_OFF_LEVEL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sf32lb52_l96_power(bool on)
{
  /* MAX-M10S-00B-01 VCC via PA43. Host UART stays open.
   * Re-apply GPIO mux/pull; test iowrite leaves PA43 as NOPULL output. */

  HAL_PIN_Set(PAD_PA43, GPIO_A43, GNSS_PWR_PIN_PULL, 1);
  gnss_pwr_gpio_write(on);
  return OK;
}

void sf32lb52_l96_uart_pins(void)
{
  /* USART2: PA31 = MCU TX (GNSS_RX), PA32 = MCU RX (GNSS_TX). */

  HAL_PIN_Set(PAD_PA31, USART2_TXD, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA32, USART2_RXD, PIN_PULLUP, 1);
}

int sf32lb52_l96_init(void)
{
  int ret;

  sf32lb52_l96_uart_pins();

  /* PA43 GNSS_PWR: idle = off (pull matches polarity). */

  HAL_PIN_Set(PAD_PA43, GPIO_A43, GNSS_PWR_PIN_PULL, 1);

  ret = sf32lb52_l96_power(false);
  if (ret < 0)
    {
      return ret;
    }

  usleep(100 * 1000);

  ret = sf32lb52_l96_power(true);
  if (ret < 0)
    {
      return ret;
    }

  usleep(CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS * 1000);

#ifdef CONFIG_BSP_USING_UART2
  sifli_uart_set_default_baud(2, CONFIG_BOARD_L96_GNSS_BAUD);
#endif

  sninfo(SF32LB52_GNSS_MOD_PN
         " powered on, UART2 baud=%d PWR=PA43 %s\n",
         CONFIG_BOARD_L96_GNSS_BAUD,
#ifdef CONFIG_BOARD_L96_GNSS_PWR_ACTIVE_HIGH
         "active-high"
#else
         "active-low"
#endif
        );
  return OK;
}

#endif /* CONFIG_BOARD_L96_GNSS */
