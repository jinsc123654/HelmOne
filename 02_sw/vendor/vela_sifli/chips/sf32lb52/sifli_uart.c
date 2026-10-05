/**
  ******************************************************************************
  * @file   drv_usart.c
  * @author Sifli software development team
  * @brief USART BSP driver
  * @{
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
#include <nuttx/config.h>
#include "sf32lb_serial.h"
#include <nuttx/power/pm.h>
#include <nuttx/cache.h>
#include <nuttx/mutex.h>
#include <errno.h>
#include <syslog.h>
#include <unistd.h>

#ifndef CONFIG_UART_BUFSZ
#define CONFIG_UART_BUFSZ 512
#endif

int uart_isr(int irq, FAR void *context, FAR void *arg);
int uart_dma_isr(int irq, FAR void *context, FAR void *arg);

#define SIFLI_UART_DMA_CACHE_ALIGN 32

#include "dma_config.h"
#include "uart_config.h"

/** @addtogroup bsp_driver Driver IO
  * @{
  */

/** @defgroup drv_usart UART
  * @brief USART BSP driver
  * @{
  */

enum
{
#ifdef CONFIG_BSP_USING_UART1
    UART1_INDEX,
#endif
#ifdef CONFIG_BSP_USING_UART2
    UART2_INDEX,
#endif
#ifdef CONFIG_BSP_USING_UART3
    UART3_INDEX,
#endif
#ifdef CONFIG_BSP_USING_UART4
    UART4_INDEX,
#endif
#ifdef CONFIG_BSP_USING_UART5
    UART5_INDEX,
#endif
#ifdef CONFIG_BSP_USING_LPUART1
    LPUART1_INDEX,
#endif
#ifdef CONFIG_BSP_USING_UART6
    UART6_INDEX,
#endif
    UART_MAX,
};

#undef CR1
#undef CR2
#undef CR3



/* Force uart_config into .data section by adding attribute.
 * This prevents it from being placed in .bss which gets cleared after
 * arm_earlyserialinit() has already initialized the console UART.
 */
static struct sifli_uart_config uart_config[] __attribute__((section(".data"))) =
{
#ifdef CONFIG_BSP_USING_UART1
    UART1_CONFIG,
#endif
#ifdef CONFIG_BSP_USING_UART2
    UART2_CONFIG,
#endif
#ifdef CONFIG_BSP_USING_UART3
    UART3_CONFIG,
#endif
#ifdef CONFIG_BSP_USING_UART4
    UART4_CONFIG,
#endif
#ifdef CONFIG_BSP_USING_UART5
    UART5_CONFIG,
#endif
#ifdef CONFIG_BSP_USING_UART6
    UART6_CONFIG,
#endif
};

/* Force uart_obj into .data section by adding attribute.
 * This prevents it from being placed in .bss which gets cleared after
 * arm_earlyserialinit() has already initialized the console UART.
 */
struct sifli_uart uart_obj[sizeof(uart_config) / sizeof(uart_config[0])] __attribute__((section(".data"))) =
{
#ifdef CONFIG_BSP_USING_UART1
    [UART1_INDEX] = { .uart_rx_dma_flag = 0 },
#endif
#ifdef CONFIG_BSP_USING_UART2  
    [UART2_INDEX] = { .uart_rx_dma_flag = 0 },
#endif
#ifdef CONFIG_BSP_USING_UART3
    [UART3_INDEX] = { .uart_rx_dma_flag = 0 },
#endif
#ifdef CONFIG_BSP_USING_UART4
    [UART4_INDEX] = { .uart_rx_dma_flag = 0 },
#endif
#ifdef CONFIG_BSP_USING_UART5
    [UART5_INDEX] = { .uart_rx_dma_flag = 0 },
#endif
#ifdef CONFIG_BSP_USING_UART6
    [UART6_INDEX] = { .uart_rx_dma_flag = 0 },
#endif
};

/* 诊断计数（见 sifli_uart_diag）：口径是"首次 open / 最后一次 close"，
 * 也就是上层 uart_open()/uart_close() 调 setup/shutdown 的时机。
 *
 * 存在的理由：现场 GNSS 读线程卡在 poll() 里 50 s 不出来，而 uart_close() 在
 * **最后一次** close 时会调 uart_reset_sem()（nuttx/drivers/serial/serial.c:2274）
 * 把 recv/xmit 两把互斥量强行重置 —— 正卡在 nxmutex_lock 上的等待者从此再也
 * 不会被唤醒。所以"那段时间里有没有人 close 过这个端口、是谁"是判定原因的
 * 直接证据。 */
static uint32_t g_diag_setup_n[UART_MAX];
static uint32_t g_diag_shutdown_n[UART_MAX];
static pid_t    g_diag_shutdown_pid[UART_MAX];

static int sifli_uart_index_of(const struct uart_dev_s *serial)
{
    unsigned int i;

    for (i = 0; i < sizeof(uart_obj) / sizeof(uart_obj[0]); i++)
    {
        if (&uart_obj[i].serial == serial)
        {
            return (int)i;
        }
    }

    return -1;
}

#ifdef CONFIG_PM
// static void sifli_serial_setsuspend(struct uart_dev_s *dev, bool suspend);
static void sifli_serial_pm_setsuspend(bool suspend);
static void sifli_serial_pmnotify(struct pm_callback_s *cb, int domain,
                                   enum pm_state_e pmstate);
// static int  sifli_serial_pmprepare(struct pm_callback_s *cb, int domain,
//                                     enum pm_state_e pmstate);
#endif


#ifdef CONFIG_PM
static struct
{
  struct pm_callback_s pm_cb;
  bool serial_suspended;
} g_serialpm =
  {
    .pm_cb.notify  = sifli_serial_pmnotify,
    .pm_cb.prepare = NULL,
    .serial_suspended = false
  };
#endif


static int sifli_setup(uart_dev_t *serial)
{
    struct sifli_uart *uart;
    uart = (struct sifli_uart *)serial->priv;
    struct serial_configure *cfg=&uart->ser_cfg;
    int diag_idx = sifli_uart_index_of(serial);

    if (diag_idx >= 0)
    {
        g_diag_setup_n[diag_idx]++;
    }

    uart->handle.Instance          = uart->config->Instance;
    uart->handle.Init.BaudRate     = cfg->baud_rate;
    uart->handle.Init.HwFlowCtl    = cfg->hwfc;
    uart->handle.Init.HwFlowCtl    <<= USART_CR3_RTSE_Pos;
    uart->handle.Init.Mode         = UART_MODE_TX_RX;
    uart->handle.Init.OverSampling = UART_OVERSAMPLING_16;

    if (cfg->parity && cfg->data_bits < DATA_BITS_9)
        cfg->data_bits++;                           // parity is part of data

    switch (cfg->data_bits)
    {
    case DATA_BITS_6:
        uart->handle.Init.WordLength = UART_WORDLENGTH_6B;
        break;
    case DATA_BITS_7:
        uart->handle.Init.WordLength = UART_WORDLENGTH_7B;
        break;
    case DATA_BITS_8:
        uart->handle.Init.WordLength = UART_WORDLENGTH_8B;
        break;
    case DATA_BITS_9:
        uart->handle.Init.WordLength = UART_WORDLENGTH_9B;
        break;
    default:
        uart->handle.Init.WordLength = UART_WORDLENGTH_8B;
        break;
    }
    switch (cfg->stop_bits)
    {
    case STOP_BITS_1:
        uart->handle.Init.StopBits   = UART_STOPBITS_1;
        break;
    case STOP_BITS_2:
        uart->handle.Init.StopBits   = UART_STOPBITS_2;
        break;
    case STOP_BITS_3:
        uart->handle.Init.StopBits   = UART_STOPBITS_0_5;
        break;
    case STOP_BITS_4:
        uart->handle.Init.StopBits   = UART_STOPBITS_1_5;
        break;
    default:
        uart->handle.Init.StopBits   = UART_STOPBITS_1;
        break;
    }
    switch (cfg->parity)
    {
    case PARITY_NONE:
        uart->handle.Init.Parity     = UART_PARITY_NONE;
        break;
    case PARITY_ODD:
        uart->handle.Init.Parity     = UART_PARITY_ODD;
        break;
    case PARITY_EVEN:
        uart->handle.Init.Parity     = UART_PARITY_EVEN;
        break;
    default:
        uart->handle.Init.Parity     = UART_PARITY_NONE;
        break;
    }

    if (HAL_UART_Init(&uart->handle) != HAL_OK)
    {
        return -EINVAL;
    }

    return OK;
}

static int sifli_uart_dma_arm(struct sifli_uart *uart);
static void sifli_uart_dma_poll(struct sifli_uart *uart);

/****************************************************************************
 * Name: nrf53_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method
 *
 ****************************************************************************/

static int sifli_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  struct inode         *inode;
  struct uart_dev_s    *dev;
  struct sifli_uart    *uart;
#ifdef CONFIG_SERIAL_TERMIOS
  struct serial_configure *config;
#endif
  int                   ret    = OK;

  if (filep == NULL || filep->f_inode == NULL)
    {
      return -EINVAL;
    }

  inode = filep->f_inode;
  dev   = inode->i_private;
  uart  = (struct sifli_uart *)dev->priv;
#ifdef CONFIG_SERIAL_TERMIOS
  config = &uart->ser_cfg;
#endif

  switch (cmd)
    {
#ifdef CONFIG_SERIAL_TERMIOS
      case TCGETS:
        {
          struct termios *termiosp = (struct termios *)arg;

          if (!termiosp)
            {
              ret = -EINVAL;
              break;
            }

          termiosp->c_cflag = ((config->parity != 0) ? PARENB : 0)
                              | ((config->parity == 1) ? PARODD : 0)
                              | ((config->stop_bits==STOP_BITS_2) ? CSTOPB : 0) |
#ifdef CONFIG_SERIAL_OFLOWCONTROL
                              ((config->hwfc&RT_SERIAL_HWFC_CTS) ? CCTS_OFLOW : 0) |
#endif
#ifdef CONFIG_SERIAL_IFLOWCONTROL
                              ((config->hwfc&RT_SERIAL_HWFC_RTS) ? CRTS_IFLOW : 0) |
#endif
                              CS8;

          cfsetispeed(termiosp, config->baud_rate);

          break;
        }

      case TCSETS:
        {
          struct termios *termiosp = (struct termios *)arg;

          if (!termiosp)
            {
              ret = -EINVAL;
              break;
            }

          /* Perform some sanity checks before accepting any changes */

          if ((termiosp->c_cflag & CSIZE) != CS8)
            {
              ret = -EINVAL;
              break;
            }

#ifndef HAVE_UART_STOPBITS
          if ((termiosp->c_cflag & CSTOPB) != 0)
            {
              ret = -EINVAL;
              break;
            }
#endif

          if (termiosp->c_cflag & PARODD)
            {
              ret = -EINVAL;
              break;
            }

          /* TODO: CCTS_OFLOW and CRTS_IFLOW */

          /* Parity */

          if (termiosp->c_cflag & PARENB)
            {
              config->parity = (termiosp->c_cflag & PARODD) ? 1 : 2;
            }
          else
            {
              config->parity = 0;
            }

#ifdef HAVE_UART_STOPBITS
          /* Stop bits */

          config->stop_bits = (termiosp->c_cflag & CSTOPB) ? STOP_BITS_2:STOP_BITS_1;
#endif

          /* Note that only cfgetispeed is used because we have knowledge
           * that only one speed is supported.
           */

          config->baud_rate = cfgetispeed(termiosp);

          /* Effect the changes */

          sifli_setup(dev);

          break;
        }
#endif

      default:
        {
          /* FIONREAD / poll look at the SW ring only. Harvest bounce first. */
          sifli_uart_dma_poll(uart);
          ret = -ENOTTY;
          break;
        }
    }

  return ret;
}


static int sifli_receive(struct uart_dev_s *dev, unsigned int *status)
{
    int ch;
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;

    ch = -1;
    if (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_RXNE) != RESET)
        ch = __HAL_UART_GETC(&uart->handle);
    if (status)
      *status = 0x00;

    /** 板级直连通道（GNSS）：**中断式 RX 这条路上也要喂**。
     *
     *  DMA 那条路是在 `sifli_uart_rx_put()` 里喂的；而 SDK 式的纯中断 RX
     *  （`uart_rx_dma_flag == 0`，框架在 RXNE 中断里逐字节调 `ops->receive()`）
     *  走的是这里 —— 不补这一处，关掉 DMA 后 GNSS 的环形就没人喂了。
     *  只在非 DMA 模式喂，避免 DMA 模式下同一字节"从环里来 + 从硬件 FIFO 来"各算一次。 */
    if (ch >= 0 && !uart->uart_rx_dma_flag && uart->handle.Instance == USART2)
      {
        extern void myvendor_gnss_uart_feed(const uint8_t *buf, size_t n) __attribute__((weak));
        uint8_t b = (uint8_t)ch;

        if (myvendor_gnss_uart_feed != NULL)
          {
            myvendor_gnss_uart_feed(&b, 1);
          }
      }

    return ch;
}

static void sifli_rxint(struct uart_dev_s *dev, bool enable)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;

    /* DMA RX: IDLE + EIE must stay enabled. NuttX briefly calls rxint(false)
     * while waiting; masking them leaves circular DMA running with no kick,
     * so GNSS looks dead until the bounce wraps.
     *
     * uart_read() / poll waiters never call rxavailable(), so harvest here
     * when RX is (re)enabled: bounce bytes become visible in the SW ring.
     */
    if (uart->uart_rx_dma_flag)
    {
        if (enable)
        {
            __HAL_UART_ENABLE_IT(&(uart->handle), UART_IT_IDLE);
            if (uart->handle.Instance != NULL)
            {
                SET_BIT(uart->handle.Instance->CR3, USART_CR3_EIE);
            }

            up_enable_irq(uart->config->irq_type+16);
            sifli_uart_dma_poll(uart);
        }
    }
    else
    {
        if (enable)
        {
            /* Enable UART RXNE interrupt */
            __HAL_UART_ENABLE_IT(&(uart->handle), UART_IT_RXNE);
            up_enable_irq(uart->config->irq_type+16);
        }
        else
        {
            /* Disable UART RXNE interrupt */
            __HAL_UART_DISABLE_IT(&(uart->handle), UART_IT_RXNE);
            up_disable_irq(uart->config->irq_type+16);
        }
    }
}

/* Circular DMA + IDLE/HT/TC harvest.
 * 1 Mbps USB-UART (CH340) inserts gaps >> 1 character time, so Normal DMA
 * that stops on IDLE then rearms drops the next packet in the FIFO.
 * Keep DMAR running; copy bounce[pos .. NDTR) into the NuttX RX ring.
 *
 * DMA1 channels are pinned at serialinit (KeepAlloc, same as NSH console).
 * Close only stops the engine; the pool slot and NVIC hook stay for life.
 * GNSS USART2 RX stays on CH4.
 */

static IRQn_Type sifli_uart_dma_irqn(DMA_HandleTypeDef *hdma)
{
    IRQn_Type base;

    if (hdma == NULL || hdma->DmaBaseAddress == NULL)
    {
        return (IRQn_Type)0;
    }

    if (hdma->DmaBaseAddress == DMA1)
    {
        base = DMAC1_CH1_IRQn;
    }
#ifdef DMA2
    else if (hdma->DmaBaseAddress == DMA2)
    {
        base = DMAC2_CH1_IRQn;
    }
#endif
    else
    {
        return (IRQn_Type)0;
    }

    return (IRQn_Type)(base + (int)(hdma->ChannelIndex >> 2));
}

static void sifli_uart_dma_irq_sync(struct sifli_uart *uart)
{
    IRQn_Type irq;

    if (!uart->uart_rx_dma_flag || uart->handle.hdmarx == NULL)
    {
        return;
    }

    irq = sifli_uart_dma_irqn(uart->handle.hdmarx);
    if (irq <= 0)
    {
        return;
    }

    if (uart->dma_nvic_irq == (int16_t)irq)
    {
        up_enable_irq((int)irq + 16);
        return;
    }

    if (uart->dma_nvic_irq >= 0)
    {
        up_disable_irq((int)uart->dma_nvic_irq + 16);
        irq_detach((int)uart->dma_nvic_irq + 16);
        uart->dma_nvic_irq = -1;
    }

    if (irq_attach((int)irq + 16, uart_dma_isr, uart) == OK)
    {
        up_enable_irq((int)irq + 16);
        uart->dma_nvic_irq = (int16_t)irq;
    }
}

/* Stop the engine without HAL_DMA_Abort / FreeChannel / irq_detach. */
static void sifli_uart_dma_stop(struct sifli_uart *uart)
{
    DMA_HandleTypeDef *hdma = uart->handle.hdmarx;

    if (uart->handle.Instance != NULL)
    {
        CLEAR_BIT(uart->handle.Instance->CR3, USART_CR3_DMAR);
    }

    if (hdma != NULL && hdma->Instance != NULL)
    {
        __HAL_DMA_DISABLE(hdma);
        if (hdma->DmaBaseAddress != NULL)
        {
            hdma->DmaBaseAddress->IFCR =
                (DMA_ISR_GIF1 << (hdma->ChannelIndex & 0x1cU));
        }

        hdma->State = HAL_DMA_STATE_READY;
        hdma->Lock = HAL_UNLOCKED;
        hdma->ErrorCode = HAL_DMA_ERROR_NONE;
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
        hdma->KeepAlloc = 1;
#endif
    }

    uart->handle.RxState = HAL_UART_STATE_READY;
    uart->handle.Lock = HAL_UNLOCKED;
    uart->handle.ErrorCode = HAL_UART_ERROR_NONE;
}

static void sifli_uart_rx_put(struct sifli_uart *uart,
                              FAR const uint8_t *src, uint16_t n)
{
    FAR char *dst;
    sbuf_size_t size;
    sbuf_size_t head;
    sbuf_size_t tail;
    uint16_t i;
    bool added = false;

    if (n == 0 || src == NULL)
    {
        return;
    }

    dst = uart->serial.recv.buffer;
    size = uart->serial.recv.size;
    head = uart->serial.recv.head;
    tail = uart->serial.recv.tail;

    for (i = 0; i < n; i++)
    {
        sbuf_size_t next = (sbuf_size_t)(head + 1);

        if (next >= size)
        {
            next = 0;
        }

        if (next == tail)
        {
            break;
        }

        dst[head] = (char)src[i];
        head = next;
        added = true;
    }

    uart->serial.recv.head = head;
    if (added)
    {
        uart_datareceived(&uart->serial);
    }

    /* 板级直连通道（GNSS）：USART2 的字节**同时**喂给板级的环形缓冲 ——
     * 那条路不注册 pollfd、不等 tick 超时（见 myvendor_gnss_uart.h 的背景）。
     * 弱符号：没有该模块的构建里这一句自动消失，行为与从前完全一致。
     *
     * 用 `Instance == USART2` 判断而不是索引：本文件本来就按 `uart_config[]`
     * 的 Instance 工作，不依赖索引常量的数值。 */
    {
        extern void myvendor_gnss_uart_feed(const uint8_t *buf, size_t n) __attribute__((weak));

        if (myvendor_gnss_uart_feed != NULL && uart->handle.Instance == USART2)
        {
            myvendor_gnss_uart_feed(src, (size_t)n);
        }
    }
}

/* GNSS 直连通道用的发送口：同一个 HAL 句柄，阻塞发小帧（UBX 命令/NMEA 配置）。
 * 不经过 NuttX serial 的发送环与互斥锁 —— 那条路的锁诊断见本文件 sifli_uart_diag。 */
int sifli_uart_direct_tx(const uint8_t *buf, size_t n)
{
    struct sifli_uart *uart = NULL;
    int i;

    if (buf == NULL || n == 0)
    {
        return -EINVAL;
    }

    for (i = 0; i < (int)(sizeof(uart_obj) / sizeof(uart_obj[0])); i++)
    {
        if (uart_obj[i].config != NULL && uart_obj[i].config->Instance == USART2)
        {
            uart = &uart_obj[i];
            break;
        }
    }

    if (uart == NULL || uart->handle.Instance == NULL)
    {
        return -ENODEV;
    }

    /* 1 字节 ≈ 26 us @38400；给 10 倍余量，上限 2 s。 */
    if (HAL_UART_Transmit(&uart->handle, (uint8_t *)buf, (uint16_t)n, 2000) != HAL_OK)
    {
        return -EIO;
    }

    return 0;
}

static void sifli_uart_rx_drain_fifo(struct sifli_uart *uart)
{
    uint8_t buf[16];
    uint16_t n = 0;

    while (n < (uint16_t)sizeof(buf) &&
           __HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_RXNE) != RESET)
    {
        buf[n++] = (uint8_t)__HAL_UART_GETC(&uart->handle);
    }

    if (n > 0)
    {
        sifli_uart_rx_put(uart, buf, n);
    }
}

static bool sifli_uart_dma_hw_alive(struct sifli_uart *uart)
{
    DMA_HandleTypeDef *hdma;

    if (!uart->uart_rx_dma_flag || uart->dma_rx.xfer_size == 0 ||
        uart->handle.Instance == NULL)
    {
        return false;
    }

    if ((uart->handle.Instance->CR3 & USART_CR3_DMAR) == 0)
    {
        return false;
    }

    hdma = uart->handle.hdmarx;
    if (hdma == NULL || hdma->Instance == NULL ||
        hdma->State != HAL_DMA_STATE_BUSY)
    {
        return false;
    }

    return (hdma->Instance->CCR & DMAC_CCR1_EN) != 0;
}

static void sifli_uart_dma_harvest(struct sifli_uart *uart)
{
    irqstate_t flags;
    uint16_t size;
    uint16_t ndtr;
    uint16_t now;
    uint16_t pos;
    FAR const uint8_t *src;

    flags = enter_critical_section();
    if (!uart->uart_rx_dma_flag || uart->dma_rx.bounce == NULL)
    {
        leave_critical_section(flags);
        return;
    }

    size = uart->dma_rx.xfer_size;
    if (size == 0 || uart->handle.hdmarx == NULL ||
        uart->handle.hdmarx->Instance == NULL)
    {
        leave_critical_section(flags);
        return;
    }

    ndtr = (uint16_t)__HAL_DMA_GET_COUNTER(uart->handle.hdmarx);
    now = (ndtr > size) ? uart->dma_rx.pos : (uint16_t)(size - ndtr);
    pos = uart->dma_rx.pos;
    src = uart->dma_rx.bounce;

    if (now != pos)
    {
        if (now > pos)
        {
            up_invalidate_dcache((uintptr_t)(src + pos),
                                 (uintptr_t)(src + now));
            sifli_uart_rx_put(uart, src + pos, (uint16_t)(now - pos));
        }
        else
        {
            up_invalidate_dcache((uintptr_t)(src + pos),
                                 (uintptr_t)(src + size));
            sifli_uart_rx_put(uart, src + pos, (uint16_t)(size - pos));
            if (now > 0)
            {
                up_invalidate_dcache((uintptr_t)src,
                                     (uintptr_t)(src + now));
                sifli_uart_rx_put(uart, src, now);
            }
        }

        uart->dma_rx.pos = now;
    }

    leave_critical_section(flags);
}

static void sifli_uart_dma_recover_log(uint32_t n, int inplace)
{
    if (up_interrupt_context())
    {
        return;
    }

    if (n <= 3u || (n & 0x1fu) == 0u)
    {
        syslog(LOG_WARNING, "uart: RX DMA recover n=%u inplace=%d\n",
               (unsigned)n, inplace);
    }
}

/** Restart circular RX on the pinned channel; do not HAL_DMA_Abort (frees pool). */
static int sifli_uart_dma_restart_circ(struct sifli_uart *uart)
{
    DMA_HandleTypeDef *hdma = uart->handle.hdmarx;
    USART_TypeDef *usart = uart->handle.Instance;
    uint16_t size = uart->dma_rx.xfer_size;

    if (hdma == NULL || hdma->Instance == NULL ||
        hdma->DmaBaseAddress == NULL || size == 0 ||
        uart->dma_rx.bounce == NULL || usart == NULL)
    {
        return -ENODEV;
    }

    CLEAR_BIT(usart->CR3, USART_CR3_DMAR);

    __HAL_DMA_DISABLE(hdma);
    hdma->DmaBaseAddress->IFCR = (DMA_ISR_GIF1 << (hdma->ChannelIndex & 0x1cU));

    /* TE recover clears TCIE/HTIE/TEIE and may drop CIRC. Reload the
     * bounce target so GNSS can start again after an error.
     */
    hdma->Init.Mode = DMA_CIRCULAR;
    SET_BIT(hdma->Instance->CCR, DMA_CIRCULAR);
    hdma->Instance->CPAR = (uint32_t)&usart->RDR;
    hdma->Instance->CM0AR = (uint32_t)uart->dma_rx.bounce;
    __HAL_DMA_SET_COUNTER(hdma, size);
    __HAL_DMA_ENABLE_IT(hdma, (DMA_IT_TC | DMA_IT_HT | DMA_IT_TE));

    uart->dma_rx.pos = 0;

    /* **重新使能 DMA 之前先冲掉硬件 RX FIFO** —— 与官方驱动对齐
     * （官方在 HAL_UART_Receive_DMA 里就是 `RQR |= USART_RQR_RXFRQ`，
     *  见官方 drv_usart.c:275）。
     *
     * 为什么这里必须做：本函数把 bounce 的目标位置重置为 0 再重新使能，而调用方
     * （recover 路径 `sifli_uart_dma_recover()`）紧接着还会用 CPU 抢读 RDR 去
     * drain FIFO。FIFO 里残留的半截/陈旧字节既会被 DMA 写进 bounce 开头，又可能
     * 被 CPU 再读走一遍 —— 两条路径争同一个 RDR，结果是**乱序或重复**。
     * 冲掉它，DMA 从下一个真实到达的字节开始，语义唯一。
     *
     * 顺序要求：必须在 `CR3 |= DMAR` **之前**，否则就是跟已经在跑的 DMA 抢。 */
    SET_BIT(usart->RQR, USART_RQR_RXFRQ);

    __HAL_DMA_ENABLE(hdma);
    SET_BIT(usart->CR3, USART_CR3_DMAR);
    SET_BIT(usart->CR3, USART_CR3_EIE);

    hdma->State = HAL_DMA_STATE_BUSY;
    hdma->Lock = HAL_UNLOCKED;
    hdma->ErrorCode = HAL_DMA_ERROR_NONE;
    uart->handle.RxState = HAL_UART_STATE_BUSY_RX;
    uart->handle.Lock = HAL_UNLOCKED;
    uart->handle.ErrorCode = HAL_UART_ERROR_NONE;
    uart->handle.pRxBuffPtr = uart->dma_rx.bounce;
    uart->handle.RxXferSize = size;
    uart->dma_rx.need_recover = 0;
    sifli_uart_dma_irq_sync(uart);
    __HAL_UART_ENABLE_IT(&(uart->handle), UART_IT_IDLE);
    return sifli_uart_dma_hw_alive(uart) ? OK : -EIO;
}

static void sifli_uart_dma_recover(struct sifli_uart *uart)
{
    static uint32_t recover_n;
    irqstate_t flags;
    int inplace = 0;

    flags = enter_critical_section();
    if (uart->dma_rx.recovering)
    {
        leave_critical_section(flags);
        return;
    }

    uart->dma_rx.recovering = 1;
    sifli_uart_dma_harvest(uart);
    if (sifli_uart_dma_restart_circ(uart) == OK)
    {
        sifli_uart_rx_drain_fifo(uart);
        inplace = 1;
    }
    else if (!up_interrupt_context())
    {
        sifli_uart_dma_stop(uart);
        sifli_uart_rx_drain_fifo(uart);
        uart->dma_rx.xfer_size = 0;
        uart->dma_rx.pos = 0;
        uart->dma_rx.need_recover = 0;
        (void)sifli_uart_dma_arm(uart);
    }
    else
    {
        /* Abort+Init from UART ISR re-enters HAL_DMA and can hang GNSS. */
        uart->dma_rx.need_recover = 1;
    }

    uart->dma_rx.recovering = 0;
    recover_n++;
    leave_critical_section(flags);
    sifli_uart_dma_recover_log(recover_n, inplace);
}

static void sifli_uart_dma_poll(struct sifli_uart *uart)
{
    if (!uart->uart_rx_dma_flag)
    {
        return;
    }

    sifli_uart_dma_harvest(uart);
    if (uart->dma_rx.need_recover || !sifli_uart_dma_hw_alive(uart))
    {
        sifli_uart_dma_recover(uart);
    }
}

static void sifli_uart_dma_bind(struct sifli_uart *uart)
{
    HAL_RCC_EnableModule(RCC_MOD_DMAC1);
    __HAL_LINKDMA(&(uart->handle), hdmarx, uart->dma_rx.handle);
    uart->handle.hdmarx->Instance = uart->config->dma_rx->Instance;
    uart->handle.hdmarx->Init.Request = uart->config->dma_rx->request;
    uart->handle.hdmarx->Init.Direction = DMA_PERIPH_TO_MEMORY;
    uart->handle.hdmarx->Init.PeriphInc = DMA_PINC_DISABLE;
    uart->handle.hdmarx->Init.MemInc = DMA_MINC_ENABLE;
    uart->handle.hdmarx->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    uart->handle.hdmarx->Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    uart->handle.hdmarx->Init.Mode = DMA_CIRCULAR;
    uart->handle.hdmarx->Init.Priority = DMA_PRIORITY_HIGH;
    uart->handle.hdmarx->Init.BurstSize = 1;
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
    uart->handle.hdmarx->Init.IrqPrio = uart->config->dma_rx->dma_irq_prio;
    uart->handle.hdmarx->KeepAlloc = 1;
#endif
}

/* Reserve the UART RX channel at serialinit, before LCD/SD come up.
 * Same as NSH console: occupy one KeepAlloc slot and DMA IRQ for life.
 */

static void sifli_uart_dma_pin(struct sifli_uart *uart)
{
    if (!uart->uart_rx_dma_flag || uart->config == NULL ||
        uart->config->dma_rx == NULL || uart->config->dma_rx->Instance == NULL)
    {
        return;
    }

    sifli_uart_dma_bind(uart);
    (void)HAL_DMA_Init(uart->handle.hdmarx);
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
    uart->handle.hdmarx->KeepAlloc = 1;
#endif
    sifli_uart_dma_irq_sync(uart);
}

static int sifli_uart_dma_arm(struct sifli_uart *uart)
{
    HAL_StatusTypeDef st;
    irqstate_t flags;
    int tries;
    uint16_t size;

    if (!uart->uart_rx_dma_flag)
    {
        return OK;
    }

    if (uart->config->dma_rx == NULL || uart->config->dma_rx->Instance == NULL ||
        uart->dma_rx.bounce == NULL)
    {
        return -EINVAL;
    }

    /* Handle.State==BUSY is not enough: PM/HAL_UART_Init can drop DMAR
     * while the DMA handle still looks busy, and we would never re-arm.
     */
    if (sifli_uart_dma_hw_alive(uart))
    {
        return OK;
    }

    sifli_uart_dma_bind(uart);

    size = (uint16_t)uart->serial.recv.size;
    if (size == 0 || size > (uint16_t)CONFIG_UART_BUFSZ)
    {
        size = (uint16_t)CONFIG_UART_BUFSZ;
    }

    up_invalidate_dcache((uintptr_t)uart->dma_rx.bounce,
                         (uintptr_t)uart->dma_rx.bounce + size);

    flags = enter_critical_section();
    sifli_uart_dma_stop(uart);
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
    uart->handle.hdmarx->KeepAlloc = 1;
#endif

    /* Reopen after GNSS close: callbacks already live on the pinned
     * handle. Restart in place; HAL_UART_Receive_DMA is first-open only.
     */
    if (uart->handle.hdmarx != NULL &&
        uart->handle.hdmarx->XferCpltCallback != NULL &&
        uart->handle.Instance != NULL)
    {
        uart->dma_rx.xfer_size = size;
        if (sifli_uart_dma_restart_circ(uart) == OK)
        {
            leave_critical_section(flags);
            return OK;
        }
    }

    for (tries = 0; tries < 2; tries++)
    {
        sifli_uart_dma_stop(uart);
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
        uart->handle.hdmarx->KeepAlloc = 1;
#endif

        st = HAL_DMA_Init(uart->handle.hdmarx);
        if (st != HAL_OK)
        {
            uart->handle.hdmarx->State = HAL_DMA_STATE_READY;
            uart->handle.hdmarx->Lock = HAL_UNLOCKED;
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
            uart->handle.hdmarx->KeepAlloc = 1;
#endif
            st = HAL_DMA_Init(uart->handle.hdmarx);
        }

#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
        uart->handle.hdmarx->KeepAlloc = 1;
#endif
        uart->handle.RxState = HAL_UART_STATE_READY;
        uart->handle.Lock = HAL_UNLOCKED;
        uart->dma_rx.pos = 0;
        st = HAL_UART_Receive_DMA(&(uart->handle),
                                  uart->dma_rx.bounce, size);
        if (st == HAL_OK &&
            uart->handle.hdmarx->State == HAL_DMA_STATE_BUSY)
        {
            uart->dma_rx.xfer_size = size;
            sifli_uart_dma_irq_sync(uart);
            __HAL_UART_ENABLE_IT(&(uart->handle), UART_IT_IDLE);
            if (uart->handle.Instance != NULL)
            {
                SET_BIT(uart->handle.Instance->CR3, USART_CR3_EIE);
            }

            leave_critical_section(flags);
            return OK;
        }
    }

    uart->dma_rx.xfer_size = 0;
    leave_critical_section(flags);
    if (!up_interrupt_context())
    {
        syslog(LOG_ERR,
               "uart: RX DMA start failed st=%d dma_st=%d rx_st=%d inst=%p\n",
               (int)st,
               (int)uart->handle.hdmarx->State,
               (int)uart->handle.RxState,
               uart->handle.hdmarx->Instance);
    }

    return -EIO;
}

static int sifli_dma_receive(struct uart_dev_s *dev)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;

    return sifli_uart_dma_arm(uart);
}


/****************************************************************************
 * Name: nrf53_rxavailable
 *
 * Description:
 *   Return true if the receive register is not empty
 *
 ****************************************************************************/

static bool sifli_rxavailable(struct uart_dev_s *dev)
{
  struct sifli_uart *uart = (struct sifli_uart *)dev->priv;

  if (uart->uart_rx_dma_flag)
    {
      sifli_uart_dma_poll(uart);
      return uart->serial.recv.head != uart->serial.recv.tail;
    }

  /* Return true if the receive buffer/fifo is not "empty." */

  return __HAL_UART_GET_FLAG(&uart->handle, UART_FLAG_RXNE)? true:false;
}


static void sifli_send(struct uart_dev_s *dev, int c)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;

    /* 这里**刻意不**计"中断里打印"（2026-09-24 更正）：
     * `sifli_send()` 是驱动逐字节写硬件的那一层，而 NuttX 的发送完成中断
     * （`nuttx/drivers/serial/serial.c` 的 `uart_xmitchars()`）**本来就**从 ISR
     * 里调它推下一个字节 —— 那属于设计如此，计数在这里会把正常发包全记成
     * "中断里打印"（现场 delta=9 的误报就是这么来的）。
     * 真正的哨兵放在**轮询式控制台出口** `arm_lowputc()`（见 sifli_lowput.c）。 */

    /* Do not wait for TC: uart_xmitchars holds a spinlock. Busy-waiting
     * here masked RX DMA IRQs long enough to drop 1 Mbps USB-UART packets.
     */
    __HAL_UART_PUTC(&uart->handle, c);
}

/* 供 diag 线程读取"中断里打印"的哨兵计数（见 sifli_lowput.c 的说明）。 */
uint32_t sifli_isr_console_chars(void)
{
    extern volatile uint32_t g_isr_console_chars;

    return g_isr_console_chars;
}


/****************************************************************************
 * Name: sifli_txint
 *
 * Description:
 *   Call to enable or disable TX interrupts
 *
 ****************************************************************************/

static void sifli_txint(struct uart_dev_s *dev, bool enable)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;

    if (uart->uart_tx_dma_flag)
    {
        if (enable)
        {
            uart_xmitchars(dev);
        }
        return;
    }

    if (enable)
    {
        __HAL_UART_ENABLE_IT(&(uart->handle), UART_IT_TXE);
        up_enable_irq(uart->config->irq_type + 16);
        if (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_TXE) != RESET)
        {
            uart_xmitchars(dev);
        }
    }
    else
    {
        __HAL_UART_DISABLE_IT(&(uart->handle), UART_IT_TXE);
    }
}



/****************************************************************************
 * Name: sifli_txready
 *
 * Description:
 *   Return true if the tranmsit data register is empty
 *
 ****************************************************************************/

static bool sifli_txready(struct uart_dev_s *dev)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;
    
    /* Return true if the receive buffer/fifo is not "empty." */
    
    return __HAL_UART_GET_FLAG(&uart->handle, UART_FLAG_TXE)? true:false;
}

/****************************************************************************
 * Name: sifli_txempty
 *
 * Description:
 *   Return true if the transmit data register is empty
 *
 ****************************************************************************/

static bool sifli_txempty(struct uart_dev_s *dev)
{
    return sifli_txready(dev);
}



/****************************************************************************
 * Name: nrf53_attach
 *
 * Description:
 *   Configure the UART to operation in interrupt driven mode.  This method
 *   is called when the serial port is opened.  Normally, this is just after
 *   the the setup() method is called, however, the serial console may
 *   operate in a non-interrupt driven mode during the boot phase.
 *
 *   RX and TX interrupts are not enabled when by the attach method (unless
 *   the hardware supports multiple levels of interrupt enabling).
 *   The RX and TX interrupts are not enabled until the txint() and rxint()
 *   methods are called.
 *
 ****************************************************************************/

static int sifli_attach(struct uart_dev_s *dev)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;
    int ret;

    /* sifli_attach called */
    
    //_info("recv %p\n", &(dev->recv));
    // Start to receive data.
    sifli_dma_receive(dev);

    /* Attach and enable the IRQ(s).  The interrupts are (probably) still
    * disabled in the C2 register.
    */
    ret = irq_attach(uart->config->irq_type+16, uart_isr, uart);
    if (ret == OK) {
        up_enable_irq(uart->config->irq_type+16);
    }

    sifli_uart_dma_irq_sync(uart);
    return ret;
}

/****************************************************************************
 * Name: nrf53_detach
 *
 * Description:
 *   Detach UART interrupts.  This method is called when the serial port is
 *   closed normally just before the shutdown method is called.
 *   The exception is the serial console which is never shutdown.
 *
 ****************************************************************************/

static void sifli_detach(struct uart_dev_s *dev)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;

    /* UART IRQ only. DMA channel + NVIC stay pinned from serialinit. */
    up_disable_irq(uart->config->irq_type+16);
    irq_detach(uart->config->irq_type+16);
}


/****************************************************************************
 * Name: sifli_shutdown
 *
 * Description:
 *   Disable the UART.  This method is called when the serial
 *   port is closed
 *
 ****************************************************************************/

static void sifli_shutdown(struct uart_dev_s *dev)
{
    struct sifli_uart *uart = (struct sifli_uart *)dev->priv;
    int diag_idx = sifli_uart_index_of(dev);

    if (diag_idx >= 0)
    {
        g_diag_shutdown_n[diag_idx]++;
        g_diag_shutdown_pid[diag_idx] = gettid();
    }

    /* Stop the engine; KeepAlloc slot and DMA IRQ stay for the next open. */
    if (uart->uart_rx_dma_flag)
    {
        sifli_uart_dma_stop(uart);
    }

    HAL_UART_DeInit(&uart->handle);
}


static const struct uart_ops_s g_uart_ops =
{
  .setup          = sifli_setup,
  .shutdown       = sifli_shutdown,
  .attach         = sifli_attach,
  .detach         = sifli_detach,
  .ioctl          = sifli_ioctl,
  .receive        = sifli_receive,
  .rxint          = sifli_rxint,
  .rxavailable    = sifli_rxavailable,
  .send           = sifli_send,
  .txint          = sifli_txint,
  .txready        = sifli_txready,
  .txempty        = sifli_txempty,
};



#ifdef CONFIG_PM

static void sifli_serial_pm_setsuspend(bool suspend)
{
  int n;
  int obj_num = sizeof(uart_obj) / sizeof(uart_obj[0]);

  /* Already in desired state? */

  if (suspend == g_serialpm.serial_suspended)
    return;

  g_serialpm.serial_suspended = suspend;


  if (!suspend)
  {
    for (n = 0; n < obj_num; n++)
    {
        /* Open ports keep their DMA; HAL_UART_Init here would drop DMAR
         * while the handle still looks BUSY, and GNSS goes silent.
         */
        if (uart_obj[n].serial.open_count != 0 ||
            uart_obj[n].serial.isconsole)
        {
            continue;
        }

        sifli_setup(&uart_obj[n].serial);
        sifli_attach(&uart_obj[n].serial);
    }
  }
}

static void sifli_serial_pmnotify(struct pm_callback_s *cb, int domain,
                                   enum pm_state_e pmstate)
{
  switch (pmstate)
    {
      case PM_NORMAL:
        {
          sifli_serial_pm_setsuspend(false);
        }
        break;

      case PM_IDLE:
        {
          sifli_serial_pm_setsuspend(false);
        }
        break;

      case PM_STANDBY:
        {
          sifli_serial_pm_setsuspend(true);
        }
        break;

      case PM_SLEEP:
        {
          sifli_serial_pm_setsuspend(true);
        }
        break;

      default:

        /* Should not get here */

        break;
    }
}
#endif


/**
 * Uart common interrupt process. This need add to uart ISR.
 *
 * @param serial serial device
 */
int uart_isr(int irq, FAR void *context, FAR void *arg)
{
    struct sifli_uart *uart;


    uart = (struct sifli_uart *) arg;

    /* Clear error flags first to prevent data loss */
    if (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(&uart->handle);
        if (uart->uart_rx_dma_flag)
        {
            /* FIFO overflowed. If circular DMA is still running, just
             * harvest; aborting frees the dyn channel and races SPI/SD.
             */
            if (sifli_uart_dma_hw_alive(uart))
            {
                sifli_uart_dma_harvest(uart);
                sifli_uart_rx_drain_fifo(uart);
            }
            else
            {
                sifli_uart_dma_recover(uart);
            }
        }
    }
    if (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_NE) != RESET)
    {
        __HAL_UART_CLEAR_NEFLAG(&uart->handle);
    }
    if (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_FE) != RESET)
    {
        __HAL_UART_CLEAR_FEFLAG(&uart->handle);
    }
    if (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_PE) != RESET)
    {
        __HAL_UART_CLEAR_PEFLAG(&uart->handle);
    }

    /* UART in mode Receiver -------------------------------------------------*/
    if (!uart->uart_rx_dma_flag &&
        __HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_RXNE) != RESET)
    {
        uart_recvchars(&(uart->serial));
    }

    /* DMA RX: copy new bounce bytes; leave circular DMA running. */
    if ((uart->uart_rx_dma_flag) && (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_IDLE) != RESET))
    {
        __HAL_UART_CLEAR_IDLEFLAG(&uart->handle);
        sifli_uart_dma_poll(uart);
    }
    else if (uart->uart_rx_dma_flag && uart->dma_rx.need_recover)
    {
        sifli_uart_dma_recover(uart);
    }

    if (!uart->uart_tx_dma_flag &&
        __HAL_UART_GET_IT_SOURCE(&(uart->handle), UART_IT_TXE) &&
        __HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_TXE) != RESET)
    {
        uart_xmitchars(&uart->serial);
    }

    /* DMA TX complete interrupt */
    if ((uart->uart_tx_dma_flag) && (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_TC) != RESET))
    {
        __HAL_UART_CLEAR_FLAG(&uart->handle, UART_CLEAR_TCF);
        uart->handle.gState = HAL_UART_STATE_READY;
        uart_xmitchars(&uart->serial);
    }

    /* Clear remaining flags */
    if (__HAL_UART_GET_FLAG(&(uart->handle), UART_FLAG_CTS) != RESET)
    {
        UART_INSTANCE_CLEAR_FUNCTION(&(uart->handle), UART_FLAG_CTS);
    }

    return OK;
}

/**
 * Uart common interrupt process. This need add to uart ISR.
 *
 * @param serial serial device
 */
int uart_dma_isr(int irq, FAR void *context, FAR void *arg)
{
    struct sifli_uart *uart =(struct sifli_uart *) arg;

    HAL_DMA_IRQHandler(&(uart->dma_rx.handle));
    return OK;
}

/**
  * @brief  Rx Half Transfer completed callback.
  * @param huart UART handle.
  * @retval None
  */
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    struct sifli_uart *uart = (struct sifli_uart *)huart;

    sifli_uart_dma_harvest(uart);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    struct sifli_uart *uart = (struct sifli_uart *)huart;

    sifli_uart_dma_harvest(uart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    struct sifli_uart *uart = (struct sifli_uart *)huart;

    /* HAL_DMA_IRQHandler is still on the stack. Re-arming here re-enters
     * HAL_DMA_Init/Start and can hang the DMA IRQ (GNSS poll never wakes).
     */
    if (uart->uart_rx_dma_flag)
    {
        uart->dma_rx.need_recover = 1;
    }
}

static void sifli_uart_get_dma_config(void)
{
#ifdef CONFIG_BSP_UART1_RX_USING_DMA
    uart_obj[UART1_INDEX].uart_rx_dma_flag = 1;
    /* Force into .data section by using non-zero initialization. */
    static struct dma_config uart1_dma_rx = { .Instance = (void *)-1 };
    uart1_dma_rx = (struct dma_config)UART1_RX_DMA_CONFIG;
    uart_config[UART1_INDEX].dma_rx = &uart1_dma_rx;
#endif

#ifdef CONFIG_BSP_UART2_RX_USING_DMA
    /** A1 实验（分支 `retire-eval`，见 docs/retire_eval.md）：**UART2 关掉 DMA RX**，
     *  退到 SDK 式纯中断 RX（`uart_rx_dma_flag == 0` ⇒ setup 走 RXNE 分支、
     *  `sifli_receive()` 逐字节喂框架 + 我们那份 feed 钩子）。
     *
     *  目的：判"我们给 UART 写的那 ~950 行 DMA/IDLE/harvest/recover 是否还需要"。
     *  判据（观察窗 ≥30 min）：`gnss_uart` 的 `drop=0`、`feed` 持续增长、
     *  `gnss: cnt link` 的 `bad=junk=0`、`[ ERROR]` 不新增。
     *  ⚠ UART1（控制台，1 Mbaud）**保持 DMA**，不动。 */
    uart_obj[UART2_INDEX].uart_rx_dma_flag = 0;   /* ← 原来是 1 */
    static struct dma_config uart2_dma_rx = { .Instance = (void *)-1 };
    uart2_dma_rx = (struct dma_config)UART2_RX_DMA_CONFIG;
    uart_config[UART2_INDEX].dma_rx = &uart2_dma_rx;
#endif

#ifdef CONFIG_BSP_UART3_RX_USING_DMA
    uart_obj[UART3_INDEX].uart_rx_dma_flag = 1;
    static struct dma_config uart3_dma_rx = UART3_RX_DMA_CONFIG;
    uart_config[UART3_INDEX].dma_rx = &uart3_dma_rx;
#endif

#ifdef CONFIG_BSP_UART4_RX_USING_DMA
    uart_obj[UART4_INDEX].uart_rx_dma_flag = 1;
    static struct dma_config uart4_dma_rx = UART4_RX_DMA_CONFIG;
    uart_config[UART4_INDEX].dma_rx = &uart4_dma_rx;
#endif

#ifdef CONFIG_BSP_UART5_RX_USING_DMA
    uart_obj[UART5_INDEX].uart_rx_dma_flag = 1;
    static struct dma_config uart5_dma_rx = UART5_RX_DMA_CONFIG;
    uart_config[UART5_INDEX].dma_rx = &uart5_dma_rx;
#endif
#ifdef CONFIG_BSP_UART6_RX_USING_DMA
    uart_obj[UART6_INDEX].uart_rx_dma_flag = 1;
    static struct dma_config uart6_dma_rx = UART6_RX_DMA_CONFIG;
    uart_config[UART6_INDEX].dma_rx = &uart6_dma_rx;
#endif


}




/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef USE_SERIALDRIVER

/****************************************************************************
 * Name: arm_serialinit
 *
 * Description:
 *   Register serial console and serial ports.  This assumes
 *   that arm_earlyserialinit was called previously.
 *
 ****************************************************************************/
static char g_uart_buffer[sizeof(uart_config)*2/sizeof(uart_config[0])][CONFIG_UART_BUFSZ]
    __attribute__((aligned(SIFLI_UART_DMA_CACHE_ALIGN)));
static uint8_t g_uart_dma_rx[sizeof(uart_config) / sizeof(uart_config[0])][CONFIG_UART_BUFSZ]
    __attribute__((aligned(SIFLI_UART_DMA_CACHE_ALIGN)));

int sifli_usart_init(void)
{
    int obj_num = sizeof(uart_obj) / sizeof(struct sifli_uart);
    if (obj_num > 0)
    {
        sifli_uart_get_dma_config();
    }
    return OK;
}

void sifli_uart_set_default_baud(unsigned int uart_num, uint32_t baud)
{
    unsigned int i;

    if (uart_num < 1 || uart_num > (sizeof(uart_obj) / sizeof(uart_obj[0])))
    {
        return;
    }

    i = uart_num - 1;
    uart_obj[i].ser_cfg.baud_rate = baud;
}

/****************************************************************************
 * Name: sifli_uart_diag
 *
 * Description:
 *   诊断：报出某个 UART 端口的"生命周期计数"和驱动器互斥量的**持有者**。
 *
 *   背景（现场：GNSS 读线程 50 s 卡在 poll() 里出不来）：`uart_poll()` 取
 *   `dev->xmit.lock` / `dev->recv.lock` 用的是**无超时、且不可被信号打断**的
 *   `nxmutex_lock()`（nuttx/drivers/serial/serial.c:1847/1869；驱动原注释
 *   "we do not let this wait be interrupted by a signal"）。所以只要有人一直
 *   握着其中一把，或者这两把被 `uart_reset_sem()` 重置过，读线程就会永久停在
 *   poll() 里：poll 的超时根本没机会生效、SIGUSR1 不起作用、reap 只能放弃。
 *   现场要指认"是谁握着"就只能读 mutex 里记的 holder pid（本 NuttX 即使没开
 *   CONFIG_PRIORITY_INHERITANCE 也会把持有者写进 sem.val.mholder）。
 *
 *   holder 字段本身是 racy 的（驱动文档原话），这里只作诊断读数。
 *
 * Input Parameters:
 *   uart_num          - USART 序号（1 起，与 sifli_uart_set_default_baud 同约定）
 *   open_count        - 上层 open_count；读线程独占时是 1
 *   rx_holder         - recv.lock 持有者 pid（-1 = 没人持有）
 *   tx_holder         - xmit.lock 持有者 pid（-1 = 没人持有）
 *   setup_n           - sifli_setup() 累计次数（= 首次 open 的次数）
 *   shutdown_n        - sifli_shutdown() 累计次数（= 最后一次 close 的次数）
 *   last_shutdown_pid - 最后一次 shutdown 是哪个线程干的（0 = 还没有过）
 *
 *   任一输出参数可为 NULL。
 *
 * Returned Value:
 *   0 成功；-EINVAL 序号越界。
 *
 ****************************************************************************/

int sifli_uart_diag(int uart_num, int *open_count, int *rx_holder, int *tx_holder,
                    unsigned int *setup_n, unsigned int *shutdown_n,
                    int *last_shutdown_pid)
{
    uart_dev_t *dev;
    int idx;

    if (uart_num < 1 || uart_num > (int)(sizeof(uart_obj) / sizeof(uart_obj[0])))
    {
        return -EINVAL;
    }

    idx = uart_num - 1;
    dev = &uart_obj[idx].serial;

    if (open_count != NULL)
    {
        *open_count = (int)dev->open_count;
    }

    if (rx_holder != NULL)
    {
        *rx_holder = (int)nxmutex_get_holder(&dev->recv.lock);
    }

    if (tx_holder != NULL)
    {
        *tx_holder = (int)nxmutex_get_holder(&dev->xmit.lock);
    }

    if (setup_n != NULL)
    {
        *setup_n = g_diag_setup_n[idx];
    }

    if (shutdown_n != NULL)
    {
        *shutdown_n = g_diag_shutdown_n[idx];
    }

    if (last_shutdown_pid != NULL)
    {
        *last_shutdown_pid = (int)g_diag_shutdown_pid[idx];
    }

    return 0;
}


void arm_serialinit(void)
{
    unsigned minor = 0;
    unsigned i     = 0;
    char devname[16];
    int obj_num = sizeof(uart_obj) / sizeof(struct sifli_uart);
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

#ifdef CONFIG_PM
  int ret;
#endif

#ifdef CONFIG_PM
    ret = pm_register(&g_serialpm.pm_cb);
    DEBUGASSERT(ret == OK);
    UNUSED(ret);
#endif

    sifli_usart_init();

    for (i = 0; i < obj_num; i++)
    {
        uart_obj[i].config = &(uart_config[i]);
        uart_obj[i].serial.ops = &g_uart_ops;
        uart_obj[i].serial.priv = &uart_obj[i];
        uart_obj[i].serial.xmit.size = CONFIG_UART_BUFSZ;
        uart_obj[i].serial.xmit.buffer = g_uart_buffer[2*i];
        uart_obj[i].serial.recv.size = CONFIG_UART_BUFSZ;
        uart_obj[i].serial.recv.buffer = g_uart_buffer[2*i+1];
        uart_obj[i].dma_rx.bounce = g_uart_dma_rx[i];
        uart_obj[i].dma_rx.xfer_size = 0;
        uart_obj[i].dma_rx.pos = 0;
        uart_obj[i].dma_rx.need_recover = 0;
        uart_obj[i].dma_rx.recovering = 0;
        uart_obj[i].ser_cfg=config;
        uart_obj[i].dma_nvic_irq = -1;
        sifli_uart_dma_pin(&uart_obj[i]);
#ifdef CONSOLE_UART
        if (i == (CONSOLE_UART -1))
        {
            // uart_obj[i].serial.isconsole = true;
        }
#endif /* CONSOLE_UART */        
    }
    
#ifdef CONSOLE_UART
    /* Register the serial console */
    /* isconsole was already set in arm_earlyserialinit */
    
    uart_register("/dev/console", &(uart_obj[CONSOLE_UART-1].serial));
#endif

    /* Register all remaining UARTs */
    strcpy(devname, "/dev/ttySx");
    for (i = 1; i <= obj_num; i++)
    {
        /* Register USARTs as devices in increasing order */
#ifdef CONSOLE_UART
        if (i==CONSOLE_UART)
            continue;
#endif        
        
        devname[9] = '0' + minor++;
        uart_register(devname, &(uart_obj[i-1].serial));
    }
}

void up_putc(int ch)
{
#if CONSOLE_UART > 0
  /* Check for LF */

  if (ch == '\n')
    {
      /* Add CR */

      arm_lowputc('\r');
    }

  arm_lowputc(ch);
#endif
  return ch;
}

#endif /* USE_SERIALDRIVER */

/// @} drv_usart
/// @} bsp_driver
/// @} file


/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/
