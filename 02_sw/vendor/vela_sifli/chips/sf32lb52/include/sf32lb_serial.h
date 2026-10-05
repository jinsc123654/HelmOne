/**
  ******************************************************************************
  * @file   drv_usart.h
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

#ifndef __SF32LB_USART_H__
#define __SF32LB_USART_H__

#include <nuttx/config.h>
#include "sfconfig.h"  /* Chip-specific config that defines BSP_USING_UARTx */

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include "bf0_hal.h"
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/serial/serial.h>

#ifdef CONFIG_SERIAL_TERMIOS
#  include <termios.h>
#endif

#include <arch/board/board.h>
#include "arm_internal.h"

/* Include UART and DMA configuration after HAL definitions */
#include "dma_config.h"
#include "uart_config.h"


#define CONSOLE_UART 1


#define BAUD_RATE_2400                  2400
#define BAUD_RATE_4800                  4800
#define BAUD_RATE_9600                  9600
#define BAUD_RATE_19200                 19200
#define BAUD_RATE_38400                 38400
#define BAUD_RATE_57600                 57600
#define BAUD_RATE_115200                115200
#define BAUD_RATE_230400                230400
#define BAUD_RATE_460800                460800
#define BAUD_RATE_921600                921600
#define BAUD_RATE_1000000               1000000
#define BAUD_RATE_2000000               2000000
#define BAUD_RATE_3000000               3000000

#ifndef CONFIG_UART_BAUD
#  define CONFIG_UART_BAUD              BAUD_RATE_115200
#endif

#define DATA_BITS_5                     5
#define DATA_BITS_6                     6
#define DATA_BITS_7                     7
#define DATA_BITS_8                     8
#define DATA_BITS_9                     9

#define STOP_BITS_1                     0
#define STOP_BITS_2                     1
#define STOP_BITS_3                     2
#define STOP_BITS_4                     3

#define PARITY_NONE                     0
#define PARITY_ODD                      1
#define PARITY_EVEN                     2

#define BIT_ORDER_LSB                   0
#define BIT_ORDER_MSB                   1

#define NRZ_NORMAL                      0       /* Non Return to Zero : normal mode */
#define NRZ_INVERTED                    1       /* Non Return to Zero : inverted mode */

#ifndef RT_SERIAL_RB_BUFSZ
    #define RT_SERIAL_RB_BUFSZ              64
#endif

#define RT_SERIAL_EVENT_RX_IND          0x01    /* Rx indication */
#define RT_SERIAL_EVENT_TX_DONE         0x02    /* Tx complete   */
#define RT_SERIAL_EVENT_RX_DMADONE      0x03    /* Rx DMA transfer done */
#define RT_SERIAL_EVENT_TX_DMADONE      0x04    /* Tx DMA transfer done */
#define RT_SERIAL_EVENT_RX_TIMEOUT      0x05    /* Rx timeout    */

#define RT_SERIAL_DMA_RX                0x01
#define RT_SERIAL_DMA_TX                0x02

#define RT_SERIAL_RX_INT                0x01
#define RT_SERIAL_TX_INT                0x02

#define RT_SERIAL_ERR_OVERRUN           0x01
#define RT_SERIAL_ERR_FRAMING           0x02
#define RT_SERIAL_ERR_PARITY            0x03

#define RT_SERIAL_TX_DATAQUEUE_SIZE     2048
#define RT_SERIAL_TX_DATAQUEUE_LWM      30

#define RT_SERIAL_HWFC_NONE             0x00000000U                                    /*!< No hardware control       */
#define RT_SERIAL_HWFC_RTS              1                                              /*!< Request To Send           */
#define RT_SERIAL_HWFC_CTS              2                                              /*!< Clear To Send             */
#define RT_SERIAL_HWFC_RTS_CTS          (RT_SERIAL_HWFC_RTS | RT_SERIAL_HWFC_CTS)      /*!< Request and Clear To Send */

/* Default config for serial_configure structure */
#define RT_SERIAL_CONFIG_DEFAULT           \
{                                          \
    CONFIG_UART_BAUD,             \
    DATA_BITS_8,      /* 8 databits */     \
    STOP_BITS_1,      /* 1 stopbit */      \
    PARITY_NONE,      /* No parity  */     \
    BIT_ORDER_LSB,    /* LSB first sent */ \
    NRZ_NORMAL,       /* Normal mode */    \
    RT_SERIAL_RB_BUFSZ, /* Buffer size */  \
    RT_SERIAL_HWFC_NONE, /* No Hardware flow control */ \
    0                                      \
}

void arm_serialinit(void);

/* Set default baud before the matching UART device is first opened (1-based). */

void sifli_uart_set_default_baud(unsigned int uart_num, uint32_t baud);

#define DMA_INSTANCE_TYPE              DMA_Channel_TypeDef

#define UART_INSTANCE_CLEAR_FUNCTION    __HAL_UART_CLEAR_FLAG

/* sifli uart config class */
struct sifli_uart_config
{
    const char         *name;
    USART_TypeDef      *Instance;
    IRQn_Type           irq_type;
    struct dma_config *dma_rx;
    struct dma_config *dma_tx;
};


struct serial_configure
{
    uint32_t baud_rate;

    uint32_t data_bits               : 4;
    uint32_t stop_bits               : 2;
    uint32_t parity                  : 2;
    uint32_t bit_order               : 1;
    uint32_t invert                  : 1;
    uint32_t bufsz                   : 16;
    uint32_t hwfc                    : 2;
    uint32_t reserved                : 4;
};

/* sifli uart dirver class */
struct sifli_uart
{
    UART_HandleTypeDef handle;
    struct sifli_uart_config *config;

    struct
    {
        DMA_HandleTypeDef handle;
        uint8_t *bounce;        /* Circular DMA target; NuttX recv is a SW ring */
        uint16_t xfer_size;
        uint16_t pos;           /* Last harvested index in bounce */
        uint8_t need_recover;   /* DMA ISR asked for re-arm; do it outside HAL */
        uint8_t recovering;
    } dma_rx;
    struct
    {
        DMA_HandleTypeDef handle;
        uint32_t last_index;
    } dma_tx;
    uint8_t uart_rx_dma_flag;
    uint8_t uart_tx_dma_flag;
    int16_t dma_nvic_irq;       /* Actual DMAC IRQ after dyn channel alloc */
    struct serial_configure ser_cfg;
    uart_dev_t serial;
};


#endif  /* __DRV_USART_H__ */
/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/
