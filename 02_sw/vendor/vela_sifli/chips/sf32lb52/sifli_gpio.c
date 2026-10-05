/****************************************************************************
 * arch/arm/src/stm32/stm32_gpio.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>
#include <assert.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "arm_internal.h"
#include "chip.h"
#include "bf0_hal.h"
#include "sifli_gpio.h"
#include "irq.h"

#define MAX_PIN_INPUT_CNT           (32)
#define PBR_PIN_OFFSET              (GPIO1_PIN_NUM + GPIO2_PIN_NUM)

struct sifli_pin_irq_hdr
{
    int16_t                pin;
    uint16_t               mode: 14;
    uint16_t               en: 1;
    uint16_t               state: 1;
    sifli_gpio_event_func_t func;
    void                   *args;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef hwp_pbr
    #define PIN_TOTAL_NUM               (GPIO1_PIN_NUM + GPIO2_PIN_NUM + HAL_PBR_MAX + 1)
#else
    #define PIN_TOTAL_NUM               (GPIO1_PIN_NUM + GPIO2_PIN_NUM)
#endif /* hwp_pbr */

static uint16_t pin_irq_hdr_num;
#ifdef hwp_pbr
    static uint16_t pbr_pin_irq_hdr_num;
#endif /* hwp_pbr */
static struct sifli_pin_irq_hdr pin_irq_hdr_tab[MAX_PIN_INPUT_CNT] =
{
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
    {-1, 0, 0, 0, NULL, NULL},
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void pin_irq_hdr(GPIO_TypeDef *hgpio, int pin)
{
    int i;

    if (hgpio == (GPIO_TypeDef *)hwp_gpio1)
    {
      // Do nothing
    }
    else if (hgpio == (GPIO_TypeDef *)hwp_gpio2)
      pin += GPIO1_PIN_NUM;
#ifdef hwp_pbr
    else if (hgpio == (GPIO_TypeDef *)hwp_pbr)
      pin = pin + GPIO1_PIN_NUM + GPIO2_PIN_NUM;
#endif /* hwp_pbr */
    else
      DEBUGASSERT(0);

    for (i = 0; i < pin_irq_hdr_num; i++)
      if (pin_irq_hdr_tab[i].pin == pin)
          break;

    if (i < pin_irq_hdr_num)
    {
#ifdef hwp_pbr
      if (hgpio == (GPIO_TypeDef *)hwp_pbr)
      {
        int8_t val = HAL_PBR_ReadPin(pin);
        DEBUGASSERT(val >= 0);
        pin_irq_hdr_tab[i].state = val;
      }
#endif /* hwp_pbr */
      pin_irq_hdr_tab[i].func(pin_irq_hdr_tab[i].args);
    }
}

static void GPIO1_IRQHandler(void)
{
    HAL_GPIO_IRQHandler((GPIO_TypeDef *)hwp_gpio1);
}

static void GPIO2_IRQHandler(void)
{
    uint32_t i;
    uint32_t pin;

#ifdef SOC_BF0_HCPU
    for (i = 0; i < pin_irq_hdr_num; i++)
    {
        /* LPSYS should be active when HPSYS is active,
           so HCPU only needs to handle interrupt source registered by its own */
        if ((pin_irq_hdr_tab[i].pin >= GPIO1_PIN_NUM) && (pin_irq_hdr_tab[i].pin < PBR_PIN_OFFSET))
        {
            pin = pin_irq_hdr_tab[i].pin - GPIO1_PIN_NUM;
            HAL_GPIO_EXTI_IRQHandler((GPIO_TypeDef *)hwp_gpio2, pin);
        }
    }
#else
    if (HAL_LPAON_IS_HP_ACTIVE())
    {
        /* If HPSYS is active, only handle interrupt source registered by LCPU,
           others should be handled by HCPU */
        for (i = 0; i < pin_irq_hdr_num; i++)
        {
            if ((pin_irq_hdr_tab[i].pin >= GPIO1_PIN_NUM) && (pin_irq_hdr_tab[i].pin < PBR_PIN_OFFSET))
            {
                pin = pin_irq_hdr_tab[i].pin - GPIO1_PIN_NUM;
                HAL_GPIO_EXTI_IRQHandler((GPIO_TypeDef *)hwp_gpio2, pin);
            }
        }
    }
    else
    {
        /* If HPSYS is inactive, clear all interrupt source even if it's not registered by LCPU side */
        HAL_GPIO_IRQHandler((GPIO_TypeDef *)hwp_gpio2);
    }
#endif /* SOC_BF0_HCPU */
}

static int gpio_isr(int irq, void *context, void *arg)
{
  if ((GPIO_TypeDef *)hwp_gpio1 == (GPIO_TypeDef *)arg)
  {
    GPIO1_IRQHandler();
  }
  else if ((GPIO_TypeDef *)hwp_gpio2 == (GPIO_TypeDef *)arg)
  {
    GPIO2_IRQHandler();
  }
  else
  {
    DEBUGASSERT(0);
  }

  return OK;
}

static int sifli_gpio_irq_enable(uint32_t pin, bool enabled, sifli_gpio_event_mode_t mode)
{
  irqstate_t flags;
  GPIO_InitTypeDef GPIO_InitStruct;
  GPIO_TypeDef *gphandle = NULL;
  int i;
  int8_t pin_state;

  /* Configure GPIO_InitStructure */
  if (pin < GPIO1_PIN_NUM)
  {
    GPIO_InitStruct.Pin = pin;
    gphandle = (GPIO_TypeDef *)hwp_gpio1;
  }
  else if (pin < (GPIO1_PIN_NUM + GPIO2_PIN_NUM))
  {
    GPIO_InitStruct.Pin = pin - GPIO1_PIN_NUM;
    gphandle = (GPIO_TypeDef *)hwp_gpio2;
  }

  if (enabled)
  {
    switch (mode)
    {
    case GPIO_EVENT_MODE_RISING:
      GPIO_InitStruct.Pull = GPIO_PULLDOWN;
      GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
      break;
    case GPIO_EVENT_MODE_FALLING:
      GPIO_InitStruct.Pull = GPIO_PULLUP;
      GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
      break;
    case GPIO_EVENT_MODE_RISING_FALLING:
      GPIO_InitStruct.Pull = GPIO_NOPULL;
      GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
      break;
    case GPIO_EVENT_MODE_HIGH_LEVEL:
      GPIO_InitStruct.Pull = GPIO_NOPULL;
      GPIO_InitStruct.Mode = GPIO_MODE_IT_HIGH_LEVEL;
      break;
    case GPIO_EVENT_MODE_LOW_LEVEL:
      GPIO_InitStruct.Pull = GPIO_NOPULL;
      GPIO_InitStruct.Mode = GPIO_MODE_IT_LOW_LEVEL;
      break;
    }

    if (gphandle)
    {
      HAL_GPIO_Init(gphandle, &GPIO_InitStruct);

      if (pin < GPIO1_PIN_NUM)   // PA
      {
#ifdef SOC_BF0_HCPU     // gpio1 only work on hcpu
          irq_attach(GPIO1_IRQn + NVIC_IRQ_FIRST, gpio_isr, (void *)hwp_gpio1);
          up_enable_irq(GPIO1_IRQn + NVIC_IRQ_FIRST);
#endif // SOC_BF0_HCPU
      }
      else // PB
      {
          irq_attach(GPIO2_IRQn + NVIC_IRQ_FIRST, gpio_isr, (void *)hwp_gpio2);
          up_enable_irq(GPIO2_IRQn + NVIC_IRQ_FIRST);          
      }
    }
    else
    {
#ifdef hwp_pbr
      pin_state = HAL_PBR_ReadPin(pin - PBR_PIN_OFFSET);
      DEBUGASSERT(pin_state >= 0);

      flags = enter_critical_section();
      for (i = 0; i < pin_irq_hdr_num; i++)
          if (pin_irq_hdr_tab[i].pin == pin)
              break;
      if (i < pin_irq_hdr_num)
      {
        pin_irq_hdr_tab[i].state = pin_state & 1;
        pin_irq_hdr_tab[i].en = 1;
      }
      leave_critical_section(flags);
#else
      DEBUGASSERT(0);
#endif /* hwp_pbr */
    }
}
else
{
    if (gphandle)
    {
        HAL_GPIO_DeInit(gphandle, GPIO_InitStruct.Pin);
    }
    else
    {
#ifdef hwp_pbr
      flags = enter_critical_section();
      for (i = 0; i < pin_irq_hdr_num; i++)
          if (pin_irq_hdr_tab[i].pin == pin)
              break;
      if (i < pin_irq_hdr_num)
      {
          pin_irq_hdr_tab[i].en = 0;
      }
    leave_critical_section(flags);
#else
        DEBUGASSERT(0);
#endif /* hwp_pbr */
    }
    //pin_irq_enable_mask &= ~(1 << pin);
    //HAL_NVIC_DisableIRQ(GPIO_IRQn);

  }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void HAL_GPIO_EXTI_Callback(GPIO_TypeDef *hgpio, uint16_t GPIO_Pin)
{
    irqstate_t flags;
    
    flags = enter_critical_section();//Prevent re-entry from systick_IRQ/GPIO_IRQ/AON_IRQ

    pin_irq_hdr(hgpio, GPIO_Pin);

    leave_critical_section(flags);
}



/****************************************************************************
 * Function:  sifli_gpioinit
 *
 * Description:
 *   Based on configuration within the .config file, it does:
 *    - Remaps positions of alternative functions.
 *
 *   Typically called from stm32_start().
 *
 * Assumptions:
 *   This function is called early in the initialization sequence so that
 *   no mutual exclusion is necessary.
 *
 ****************************************************************************/

void sifli_gpioinit(void)
{
  /*TODO: Remap according to the configuration within .config file */

  //sifli_gpioremap();
}

/****************************************************************************
 * Name: sifli_gpio_config
 *
 * Description:
 *   Configure a GPIO pin based on bit-encoded description of the pin.
 *   Once it is configured as Alternative (GPIO_ALT|GPIO_CNF_AFPP|...)
 *   function, it must be unconfigured with stm32_unconfiggpio() with
 *   the same cfgset first before it can be set to non-alternative function.
 *
 * Returned Value:
 *   OK on success
 *   A negated errno value on invalid port, or when pin is locked as ALT
 *   function.
 *
 * To-Do: Auto Power Enable
 ****************************************************************************/
int sifli_gpio_config(uint32_t pin, sifli_gpio_mode_t mode)
{
  GPIO_InitTypeDef GPIO_InitStruct;
  GPIO_TypeDef *gphandle = NULL;
  bool output_en = false;
  HAL_StatusTypeDef status;

  /* Configure GPIO_InitStructure */
  if (pin < GPIO1_PIN_NUM)
  {
      GPIO_InitStruct.Pin = pin;
      gphandle = (GPIO_TypeDef *)hwp_gpio1;
  }
  else
  {
      GPIO_InitStruct.Pin = pin - GPIO1_PIN_NUM;
      gphandle = (GPIO_TypeDef *)hwp_gpio2;
  }
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;

  if (GPIO_OUTPUT == mode)
  {
      /* output setting */
      GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT;
      GPIO_InitStruct.Pull = GPIO_NOPULL;
      output_en = true;
  }
  else if (GPIO_INPUT == mode)
  {
      /* input setting: not pull. */
      GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
      GPIO_InitStruct.Pull = GPIO_NOPULL;
  }
  else if (mode == GPIO_OUTPUT_OD)
  {
      /* output setting: od. */
      GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
      GPIO_InitStruct.Pull = GPIO_NOPULL;
      output_en = true;
  }

  if (pin < (GPIO1_PIN_NUM + GPIO2_PIN_NUM))
  {
      HAL_GPIO_Init(gphandle, &GPIO_InitStruct);
  }
  else
  {
#ifdef hwp_pbr
      /* PBR pin doesn't support open-drain output */
      DEBUGASSERT(mode != GPIO_OUTPUT_OD);
      status = HAL_PBR_ConfigMode(pin - PBR_PIN_OFFSET, output_en);
      DEBUGASSERT(HAL_OK == status);
#else
      DEBUGASSERT(0);
#endif /* hwp_pbr */
  }
  return OK;
}

/****************************************************************************
 * Name: sifli_unconfig_gpio
 *
 * Description:
 *   Unconfigure a GPIO pin based on bit-encoded description of the pin, set
 *   it into default HiZ state (and possibly mark it's unused) and unlock it
 *   whether it was previously selected as an alternative function
 *   (GPIO_ALT | GPIO_CNF_AFPP | ...).
 *
 *   This is a safety function and prevents hardware from shocks, as
 *   unexpected write to the Timer Channel Output GPIO to fixed '1' or '0'
 *   while it should operate in PWM mode could produce excessive on-board
 *   currents and trigger over-current/alarm function.
 *
 * Returned Value:
 *  OK on success
 *  A negated errno value on invalid port
 *
 * To-Do: Auto Power Disable
 ****************************************************************************/

int sifli_gpio_unconfig(uint32_t pin)
{
  /* To-Do: Mark its unuse for automatic power saving options */

  return sifli_gpio_config(pin, GPIO_INPUT);
}

/****************************************************************************
 * Name: sifli_gpio_write
 *
 * Description:
 *   Write one or zero to the selected GPIO pin
 *
 ****************************************************************************/

void sifli_gpio_write(uint32_t pin, bool value)
{
  GPIO_PinState state;

  state = value ? GPIO_PIN_SET : GPIO_PIN_RESET;
  if (pin < GPIO1_PIN_NUM)
      HAL_GPIO_WritePin((GPIO_TypeDef *)hwp_gpio1, pin, state);
  else if (pin < (GPIO1_PIN_NUM + GPIO2_PIN_NUM))
      HAL_GPIO_WritePin((GPIO_TypeDef *)hwp_gpio2, pin - GPIO1_PIN_NUM, state);
  else
#ifdef hwp_pbr
      HAL_PBR_WritePin(pin - (GPIO1_PIN_NUM + GPIO2_PIN_NUM), (uint8_t)state);
#else
      DEBUGASSERT(0);
#endif /* hwp_pbr */
}

/****************************************************************************
 * Name: sifli_gpio_read
 *
 * Description:
 *   Read one or zero from the selected GPIO pin
 *
 ****************************************************************************/

bool sifli_gpio_read(uint32_t pin)
{
  int value;

  if (pin < GPIO1_PIN_NUM)
      value = HAL_GPIO_ReadPin((GPIO_TypeDef *)hwp_gpio1, pin);
  else if (pin < (GPIO1_PIN_NUM + GPIO2_PIN_NUM))
      value = HAL_GPIO_ReadPin((GPIO_TypeDef *)hwp_gpio2, pin - GPIO1_PIN_NUM);
  else
  {
#ifdef hwp_pbr
      value = HAL_PBR_ReadPin(pin - (GPIO1_PIN_NUM + GPIO2_PIN_NUM));
      DEBUGASSERT(value >= 0);
#else
      DEBUGASSERT(0);
#endif /* hwp_pbr */
  }

  return (value != 0);
}

int sifli_gpio_set_event(uint32_t pin, bool risingedge, bool fallingedge,
                       sifli_gpio_event_func_t func, void *arg)
{
    irqstate_t flags;
    int i;
    struct sifli_pin_irq_hdr *item;
    sifli_gpio_event_mode_t mode = GPIO_EVENT_MODE_RISING;
    int ret = 0;

    if (func)
    {
      if (risingedge && fallingedge)
      {
        mode = GPIO_EVENT_MODE_RISING_FALLING;
      }
      else if (risingedge)
      {
        mode = GPIO_EVENT_MODE_RISING;
      }
      else if (fallingedge)
      {
        mode = GPIO_EVENT_MODE_FALLING;
      }
      else
      {
        return ERROR;
      }
      
      flags = enter_critical_section();

      for (i = 0; i < pin_irq_hdr_num; i++)
          if (pin_irq_hdr_tab[i].pin == pin)
              break;
      if (i < pin_irq_hdr_num)
      {
          if (pin_irq_hdr_tab[i].pin == pin &&
                  pin_irq_hdr_tab[i].func == func &&
                  pin_irq_hdr_tab[i].mode == mode &&
                  pin_irq_hdr_tab[i].args == arg)    // attached before
          {
              // Do Nothing
          }
          else // found but not correct parameters, quit ? or recover ?
          {
              leave_critical_section(flags);
              return ERROR;
          }
      }
      else // not found
      {
          DEBUGASSERT(pin_irq_hdr_num < MAX_PIN_INPUT_CNT);
          item = &pin_irq_hdr_tab[pin_irq_hdr_num];
          DEBUGASSERT(item->pin == -1);
          item->pin = pin;
          item->en = 0;
          item->func = func;
          item->mode = mode;
          item->args = arg;
          pin_irq_hdr_num++;
  #ifdef hwp_pbr
          if (pin >= PBR_PIN_OFFSET)
          {
              pbr_pin_irq_hdr_num++;
          }
  #endif /* hwp_pbr */

          leave_critical_section(flags);
      }

      ret = sifli_gpio_irq_enable(pin, true, mode);
    }
    else
    {
      ret = sifli_gpio_irq_enable(pin, false, mode);
    }

    return ret;
}

