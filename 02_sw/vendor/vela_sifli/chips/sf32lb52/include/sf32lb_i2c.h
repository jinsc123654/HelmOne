/**
  ******************************************************************************
  * @file   sf32lb_i2c.h
  * @author Sifli software development team
  ******************************************************************************
*/
/**
 * @attention
 * Copyright (c) 2019 - 2022,  Sifli Technology
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Sifli integrated circuit
 *    in a product or a software update for such product, must reproduce the above
 *    copyright notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Sifli nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Sifli integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY SIFLI TECHNOLOGY "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SIFLI TECHNOLOGY OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef __SF32LB_I2C_H__
#define __SF32LB_I2C_H__

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <nuttx/i2c/i2c_master.h>

enum
{
#ifdef CONFIG_BSP_USING_I2C1
    I2C1_INDEX,
#endif
#ifdef CONFIG_BSP_USING_I2C2
    I2C2_INDEX,
#endif
#ifdef CONFIG_BSP_USING_I2C3
    I2C3_INDEX,
#endif
#ifdef CONFIG_BSP_USING_I2C4
    I2C4_INDEX,
#endif
#ifdef CONFIG_BSP_USING_I2C5
    I2C5_INDEX,
#endif
#ifdef CONFIG_BSP_USING_I2C6
    I2C6_INDEX,
#endif
#ifdef CONFIG_BSP_USING_I2C7
    I2C7_INDEX,
#endif
    I2C_MAX
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: sifli_i2cbus_initialize
 *
 * Description:
 *   Initialize one I2C bus
 *
 * Input Parameters:
 *   port - I2C port number (0-based index into I2C_MAX)
 *
 * Returned Value:
 *   A pointer to the I2C master structure on success; NULL on failure.
 *
 ****************************************************************************/

struct i2c_master_s *sifli_i2cbus_initialize(int port);

/****************************************************************************
 * Name: sifli_i2cbus_uninitialize
 *
 * Description:
 *   Uninitialize an I2C bus
 *
 * Input Parameters:
 *   dev - I2C master structure to uninitialize
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int sifli_i2cbus_uninitialize(struct i2c_master_s *dev);

#endif
/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/

