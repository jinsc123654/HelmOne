/****************************************************************************
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

#ifndef __ARCH_ARM_SRC_SF32LB_SF32LB_GPIO_H
#define __ARCH_ARM_SRC_SF32LB_SF32LB_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifndef __ASSEMBLY__
#  include <stdint.h>
#  include <stdbool.h>
#endif

#include "chip.h"
#include "bf0_hal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/


/* Helper macros */

#define __GPIO_INSTANCE(GPIOx)  (GPIO##GPIOx##_BASE)

/** get driver pin id by instance id and its pin id
 *
 * e.g. GET_PIN(1, 0) for GPIO1 pin0 -->  driver pin id is 0
 *      GET_PIN(2, 0) for GPIO2 pin0 -->  driver pin id is 64
 *
 * @param  GPIOx GPIO instance id, starting from 0, 0 is for PBR is present, 1 is for GPIO1, 2 is for GPIO2, etc.
 * @param  PIN GPIO instance pin id, starting from 0
 * @return driver pin id, counting from 0
 */
#define GET_PIN(GPIOx,PIN)  ((GPIOx != 0) ? ((__GPIO_INSTANCE(GPIOx) == GPIO2_BASE) ? (GPIO1_PIN_NUM + (PIN)) : (PIN)) : (GPIO1_PIN_NUM + GPIO2_PIN_NUM + (PIN)))

#ifdef hwp_pbr
    /** get driver pin id by instance id and its pin id
    *
    * e.g. GET_PIN(1, 0) for GPIO1 pin0 -->  driver pin id is 0
    *      GET_PIN(2, 0) for GPIO2 pin0 -->  driver pin id is 64
    *
    * @param  GPIOx GPIO instance id, starting from 0, 0 is for PBR is present, 1 is for GPIO1, 2 is for GPIO2, etc.
    * @param  PIN GPIO instance pin id, starting from 0
    * @return driver pin id, counting from 0
    */
    #define GET_PIN_2(GPIOx,PIN)  ((GPIOx != hwp_pbr) ? (((GPIOx) == hwp_gpio2) ? (GPIO1_PIN_NUM + (PIN)) : (PIN)) : (GPIO1_PIN_NUM + GPIO2_PIN_NUM + (PIN)))

    /** get GPIO instance according to driver pin  */
    #define GET_GPIO_INSTANCE(PIN)  ((PIN) >= GPIO1_PIN_NUM ? (((PIN) < (GPIO1_PIN_NUM + GPIO2_PIN_NUM)) ? hwp_gpio2 : hwp_pbr) : hwp_gpio1)

    /** get GPIO instance pin id according to driver pin id */
    #define GET_GPIOx_PIN(PIN) ((PIN) >= GPIO1_PIN_NUM ? (((PIN) < (GPIO1_PIN_NUM + GPIO2_PIN_NUM)) ? ((PIN) - GPIO1_PIN_NUM) : ((PIN) - GPIO1_PIN_NUM - GPIO2_PIN_NUM)) : (PIN))

#else
    #define GET_PIN_2(GPIOx,PIN)  (((GPIOx) == hwp_gpio2) ? (GPIO1_PIN_NUM + (PIN)) : (PIN))

    #define GET_GPIO_INSTANCE(PIN)  ((PIN) >= GPIO1_PIN_NUM ? hwp_gpio2 : hwp_gpio1)

    #define GET_GPIOx_PIN(PIN) ((PIN) >= GPIO1_PIN_NUM ? (PIN) - GPIO1_PIN_NUM : (PIN))
#endif /* hwp_pbr */

#define GPIO_PIN_DECODE(p)  (((p) & GPIO_PIN_MASK)  >> GPIO_PIN_SHIFT)
#define GPIO_PORT_DECODE(p) (((p) & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT)

/****************************************************************************
 * Public Types
 ****************************************************************************/
typedef uint32_t sifli_pinset_t;

typedef enum
{
  GPIO_OUTPUT     =    0x00,
  GPIO_INPUT      =    0x01,
  GPIO_OUTPUT_OD  =    0x02,
} sifli_gpio_mode_t;


typedef enum
{
  GPIO_EVENT_MODE_RISING           =  0x00,
  GPIO_EVENT_MODE_FALLING          =  0x01,
  GPIO_EVENT_MODE_RISING_FALLING   =  0x02,
  GPIO_EVENT_MODE_HIGH_LEVEL       =  0x03,
  GPIO_EVENT_MODE_LOW_LEVEL        =  0x04,
} sifli_gpio_event_mode_t;


typedef void (*sifli_gpio_event_func_t)(void *args);


/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__
#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: sifli_gpio_config
 *
 * Description:
 *   Configure a GPIO pin based on bit-encoded description of the pin.
 *
 ****************************************************************************/

int sifli_gpio_config(uint32_t pin, sifli_gpio_mode_t mode);

/****************************************************************************
 * Name: sifli_gpio_unconfig
 *
 * Description:
 *   Unconfigure a GPIO pin based on bit-encoded description of the pin.
 *
 ****************************************************************************/

int sifli_gpio_unconfig(uint32_t pin);

/****************************************************************************
 * Name: sifli_gpio_write
 *
 * Description:
 *   Write one or zero to the selected GPIO pin
 *
 ****************************************************************************/

void sifli_gpio_write(uint32_t pin, bool value);

/****************************************************************************
 * Name: sifli_gpio_read
 *
 * Description:
 *   Read one or zero from the selected GPIO pin
 *
 ****************************************************************************/

bool sifli_gpio_read(uint32_t pin);


/****************************************************************************
 * Name: sifli_gpio_set_event
 *
 * Description:
 *   Sets/clears GPIO based event and interrupt triggers.
 *
 * Input Parameters:
 *  - pinset: gpio pin configuration
 *  - rising/falling edge: enables
 *  - event:  generate event when set
 *  - func:   when non-NULL, generate interrupt
 *  - arg:    Argument passed to the interrupt callback
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure indicating the
 *   nature of the failure.
 *
 ****************************************************************************/

int sifli_gpio_set_event(uint32_t pin, bool risingedge, bool fallingedge,
                       sifli_gpio_event_func_t func, void *arg);


#ifdef __cplusplus
}
#endif
#endif /* __ASSEMBLY__ */

#endif /* __ARCH_ARM_SRC_SF32LB_SF32LB_GPIO_H */
