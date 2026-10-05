/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sifli_spi.c
 *
 * SF32LB SPI driver for NuttX - based on SF32LB SPI HAL layer
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sfconfig.h>

#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/kmalloc.h>
#include <nuttx/power/pm.h>
#include <nuttx/spi/spi.h>
#include <arch/board/board.h>

#include "arm_internal.h"

#include "register.h"
#include "bf0_hal.h"
#include "sf32lb_spi.h"
#include "spi_config.h"
#include "dma_config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SPI_XFER_TIMEOUT    5000  /* Timeout in milliseconds */
#define SPI_DEFAULT_FREQ    1000000  /* 1 MHz default */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* SPI Device hardware configuration */

typedef struct bf0_spi_config
{
    SPI_TypeDef *Instance;
    const char  *bus_name;
    IRQn_Type    irq_type;
    uint8_t      core;
    struct dma_config *dma_rx;
    struct dma_config *dma_tx;
} bf0_spi_config_t;

/* SPI Device Instance */

struct sifli_spi_dev_s
{
    struct spi_dev_s spidev;        /* Generic SPI device */
    const struct spi_ops_s *ops;    /* Standard SPI operations */
    mutex_t lock;                   /* Mutual exclusion mutex */
    sem_t   sem_isr;                /* Interrupt wait semaphore */
#ifdef CONFIG_PM
    struct pm_callback_s pm_cb;     /* PM callbacks */
#endif
    SPI_HandleTypeDef handle;       /* HAL SPI handle */
    bf0_spi_config_t *config;       /* HW configuration */
    struct
    {
        DMA_HandleTypeDef dma_rx;
        DMA_HandleTypeDef dma_tx;
    } dma;
    uint8_t  spi_dma_tx_flag;      /* TX DMA enabled */
    uint8_t  spi_dma_rx_flag;      /* RX DMA enabled */
    uint8_t  port;                  /* SPI port index */
    uint32_t frequency;             /* Current frequency */
    enum spi_mode_e mode;           /* Current SPI mode */
    int      nbits;                 /* Current number of bits */
    RCC_MODULE_TYPE mod;            /* RCC module type */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* SPI ops methods */

static int      sifli_spi_lock(struct spi_dev_s *dev, bool lock);
static void     sifli_spi_select(struct spi_dev_s *dev, uint32_t devid,
                                 bool selected);
static uint32_t sifli_spi_setfrequency(struct spi_dev_s *dev,
                                       uint32_t frequency);
static void     sifli_spi_setmode(struct spi_dev_s *dev,
                                  enum spi_mode_e mode);
static void     sifli_spi_setbits(struct spi_dev_s *dev, int nbits);
static uint8_t  sifli_spi_status(struct spi_dev_s *dev, uint32_t devid);
static uint32_t sifli_spi_send(struct spi_dev_s *dev, uint32_t wd);
#ifdef CONFIG_SPI_EXCHANGE
static void     sifli_spi_exchange(struct spi_dev_s *dev,
                                   const void *txbuffer, void *rxbuffer,
                                   size_t nwords);
#else
static void     sifli_spi_sndblock(struct spi_dev_s *dev,
                                   const void *buffer, size_t nwords);
static void     sifli_spi_recvblock(struct spi_dev_s *dev, void *buffer,
                                    size_t nwords);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct spi_ops_s g_spi_ops =
{
    .lock        = sifli_spi_lock,
    .select      = sifli_spi_select,
    .setfrequency = sifli_spi_setfrequency,
    .setmode     = sifli_spi_setmode,
    .setbits     = sifli_spi_setbits,
    .status      = sifli_spi_status,
    .send        = sifli_spi_send,
#ifdef CONFIG_SPI_EXCHANGE
    .exchange    = sifli_spi_exchange,
#else
    .sndblock    = sifli_spi_sndblock,
    .recvblock   = sifli_spi_recvblock,
#endif
};

/* SPI hardware configuration table */

static bf0_spi_config_t bf0_spi_cfg[] =
{
#ifdef BSP_USING_SPI1
    SPI1_BUS_CONFIG,
#endif
#ifdef BSP_USING_SPI2
    SPI2_BUS_CONFIG,
#endif
};

/* RCC module types for each SPI */

static const RCC_MODULE_TYPE bf0_spi_mod[] =
{
#ifdef BSP_USING_SPI1
    RCC_MOD_SPI1,
#endif
#ifdef BSP_USING_SPI2
    RCC_MOD_SPI2,
#endif
};

/* SPI device instances */

static struct sifli_spi_dev_s spi_obj[SPI_MAX];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: spi_get_dma_info
 *
 * Description:
 *   Initialize DMA configuration for SPI instances.
 *
 ****************************************************************************/

static void spi_get_dma_info(void)
{
#ifdef BSP_SPI1_TX_USING_DMA
    static struct dma_config spi1_tx_dma = SPI1_TX_DMA_CONFIG;
    spi_obj[SPI1_INDEX].spi_dma_tx_flag = 1;
    bf0_spi_cfg[SPI1_INDEX].dma_tx = &spi1_tx_dma;
#endif
#ifdef BSP_SPI1_RX_USING_DMA
    static struct dma_config spi1_rx_dma = SPI1_RX_DMA_CONFIG;
    spi_obj[SPI1_INDEX].spi_dma_rx_flag = 1;
    bf0_spi_cfg[SPI1_INDEX].dma_rx = &spi1_rx_dma;
#endif
#ifdef BSP_SPI2_TX_USING_DMA
    static struct dma_config spi2_tx_dma = SPI2_TX_DMA_CONFIG;
    spi_obj[SPI2_INDEX].spi_dma_tx_flag = 1;
    bf0_spi_cfg[SPI2_INDEX].dma_tx = &spi2_tx_dma;
#endif
#ifdef BSP_SPI2_RX_USING_DMA
    static struct dma_config spi2_rx_dma = SPI2_RX_DMA_CONFIG;
    spi_obj[SPI2_INDEX].spi_dma_rx_flag = 1;
    bf0_spi_cfg[SPI2_INDEX].dma_rx = &spi2_rx_dma;
#endif
}

/****************************************************************************
 * Name: spi_dma_init
 *
 * Description:
 *   Initialize DMA handles and link them to SPI handle.
 *
 ****************************************************************************/

static void spi_dma_init(struct sifli_spi_dev_s *priv)
{
    bf0_spi_config_t *cfg = priv->config;

    if (priv->spi_dma_tx_flag && cfg->dma_tx)
    {
        priv->dma.dma_tx.Instance             = cfg->dma_tx->Instance;
        priv->dma.dma_tx.Init.Request         = cfg->dma_tx->request;
        priv->dma.dma_tx.Init.Direction        = DMA_MEMORY_TO_PERIPH;
        priv->dma.dma_tx.Init.PeriphInc        = DMA_PINC_DISABLE;
        priv->dma.dma_tx.Init.MemInc           = DMA_MINC_ENABLE;
        priv->dma.dma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        priv->dma.dma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        priv->dma.dma_tx.Init.Mode             = DMA_NORMAL;
        priv->dma.dma_tx.Init.Priority         = DMA_PRIORITY_HIGH;
        __HAL_LINKDMA(&priv->handle, hdmatx, priv->dma.dma_tx);
    }

    if (priv->spi_dma_rx_flag && cfg->dma_rx)
    {
        priv->dma.dma_rx.Instance             = cfg->dma_rx->Instance;
        priv->dma.dma_rx.Init.Request         = cfg->dma_rx->request;
        priv->dma.dma_rx.Init.Direction        = DMA_PERIPH_TO_MEMORY;
        priv->dma.dma_rx.Init.PeriphInc        = DMA_PINC_DISABLE;
        priv->dma.dma_rx.Init.MemInc           = DMA_MINC_ENABLE;
        priv->dma.dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        priv->dma.dma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        priv->dma.dma_rx.Init.Mode             = DMA_NORMAL;
        priv->dma.dma_rx.Init.Priority         = DMA_PRIORITY_HIGH;
        __HAL_LINKDMA(&priv->handle, hdmarx, priv->dma.dma_rx);
    }
}

/****************************************************************************
 * Name: sifli_spi_isr
 *
 * Description:
 *   SPI interrupt handler
 *
 ****************************************************************************/

static int sifli_spi_isr(int irq, void *context, void *arg)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)arg;

    HAL_SPI_IRQHandler(&priv->handle);

    /* If SPI transfer is complete, post the semaphore */

    if (priv->handle.State == HAL_SPI_STATE_READY ||
        priv->handle.State == HAL_SPI_STATE_ERROR)
    {
        nxsem_post(&priv->sem_isr);
    }

    return OK;
}

/****************************************************************************
 * Name: sifli_spi_dma_tx_isr
 *
 * Description:
 *   SPI TX DMA interrupt handler
 *
 ****************************************************************************/

static int sifli_spi_dma_tx_isr(int irq, void *context, void *arg)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)arg;

    HAL_DMA_IRQHandler(priv->handle.hdmatx);
    return OK;
}

/****************************************************************************
 * Name: sifli_spi_dma_rx_isr
 *
 * Description:
 *   SPI RX DMA interrupt handler
 *
 ****************************************************************************/

static int sifli_spi_dma_rx_isr(int irq, void *context, void *arg)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)arg;

    HAL_DMA_IRQHandler(priv->handle.hdmarx);
    return OK;
}

/****************************************************************************
 * Name: sifli_spi_hw_init
 *
 * Description:
 *   Initialize SPI hardware with default settings.
 *
 ****************************************************************************/

static int sifli_spi_hw_init(struct sifli_spi_dev_s *priv)
{
    bf0_spi_config_t *cfg = priv->config;

    /* Enable the SPI module clock */

    HAL_RCC_EnableModule(bf0_spi_mod[priv->port]);

    /* Set up the HAL handle */

    priv->handle.Instance = cfg->Instance;
    priv->handle.core     = cfg->core;

    /* Default SPI configuration: Master, 8-bit, Mode 0, 1MHz */

    priv->handle.Init.Mode             = SPI_MODE_MASTER;
    priv->handle.Init.Direction        = SPI_DIRECTION_2LINES;
    priv->handle.Init.DataSize         = SPI_DATASIZE_8BIT;
    priv->handle.Init.CLKPolarity      = SPI_POLARITY_LOW;
    priv->handle.Init.CLKPhase         = SPI_PHASE_1EDGE;
    priv->handle.Init.BaudRatePrescaler = 48;  /* Will be recalculated */
    priv->handle.Init.FrameFormat      = SPI_FRAME_FORMAT_SPI;
    priv->handle.Init.SFRMPol          = SPI_SFRMPOL_LOW;

    /* Set up DMA if configured */

    spi_get_dma_info();
    spi_dma_init(priv);

    /* Initialize the HAL SPI handle */

    HAL_SPI_Init(&priv->handle);

    /* Attach SPI interrupt */

    irq_attach(cfg->irq_type + 16, sifli_spi_isr, priv);
    up_enable_irq(cfg->irq_type + 16);

    /* Attach DMA interrupts if configured */

    if (priv->spi_dma_tx_flag && cfg->dma_tx)
    {
        irq_attach(cfg->dma_tx->dma_irq + 16, sifli_spi_dma_tx_isr, priv);
        up_enable_irq(cfg->dma_tx->dma_irq + 16);
    }

    if (priv->spi_dma_rx_flag && cfg->dma_rx)
    {
        irq_attach(cfg->dma_rx->dma_irq + 16, sifli_spi_dma_rx_isr, priv);
        up_enable_irq(cfg->dma_rx->dma_irq + 16);
    }

    return OK;
}

/****************************************************************************
 * Name: sifli_spi_hw_deinit
 *
 * Description:
 *   Deinitialize SPI hardware.
 *
 ****************************************************************************/

static int sifli_spi_hw_deinit(struct sifli_spi_dev_s *priv)
{
    bf0_spi_config_t *cfg = priv->config;

    /* Disable SPI */

    __HAL_SPI_DISABLE(&priv->handle);

    /* Disable and detach interrupts */

    up_disable_irq(cfg->irq_type + 16);
    irq_detach(cfg->irq_type + 16);

    if (priv->spi_dma_tx_flag && cfg->dma_tx)
    {
        up_disable_irq(cfg->dma_tx->dma_irq + 16);
        irq_detach(cfg->dma_tx->dma_irq + 16);
    }

    if (priv->spi_dma_rx_flag && cfg->dma_rx)
    {
        up_disable_irq(cfg->dma_rx->dma_irq + 16);
        irq_detach(cfg->dma_rx->dma_irq + 16);
    }

    /* Deinitialize the SPI HAL */

    HAL_SPI_DeInit(&priv->handle);

    /* Disable the SPI module clock */

    HAL_RCC_DisableModule(bf0_spi_mod[priv->port]);

    return OK;
}

/****************************************************************************
 * Name: sifli_spi_wait_complete
 *
 * Description:
 *   Wait for SPI transfer to complete.
 *
 ****************************************************************************/

static int sifli_spi_wait_complete(struct sifli_spi_dev_s *priv)
{
    int ret;

    /* Wait on the semaphore with timeout */

    ret = nxsem_tickwait_uninterruptible(&priv->sem_isr,
                                          MSEC2TICK(SPI_XFER_TIMEOUT));
    if (ret < 0)
    {
        spierr("SPI transfer timeout\n");
        HAL_SPI_Abort(&priv->handle);
        return -ETIMEDOUT;
    }

    /* Check for errors */

    if (priv->handle.ErrorCode != HAL_SPI_ERROR_NONE)
    {
        spierr("SPI error: 0x%08lx\n", priv->handle.ErrorCode);
        return -EIO;
    }

    return OK;
}

/****************************************************************************
 * Name: sifli_spi_lock
 *
 * Description:
 *   Lock/unlock the SPI bus.
 *
 ****************************************************************************/

static int sifli_spi_lock(struct spi_dev_s *dev, bool lock)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;
    int ret;

    if (lock)
    {
        ret = nxmutex_lock(&priv->lock);
    }
    else
    {
        ret = nxmutex_unlock(&priv->lock);
    }

    return ret;
}

/****************************************************************************
 * Name: sifli_spi_select
 *
 * Description:
 *   Enable/disable the SPI chip select. This calls the board-specific
 *   implementation.
 *
 ****************************************************************************/

static void sifli_spi_select(struct spi_dev_s *dev, uint32_t devid,
                             bool selected)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;

    /* Use the HAL CS control for hardware CS management */

    if (selected)
    {
        __HAL_SPI_ENABLE(&priv->handle);
        __HAL_SPI_TAKE_CS(&priv->handle);
    }
    else
    {
        __HAL_SPI_RELEASE_CS(&priv->handle);
        __HAL_SPI_DISABLE(&priv->handle);
    }

    /* Also call board-specific select if available */

    sf32lb_spi_select(dev, devid, selected);
}

/****************************************************************************
 * Name: sifli_spi_setfrequency
 *
 * Description:
 *   Set the SPI bus frequency.
 *
 ****************************************************************************/

static uint32_t sifli_spi_setfrequency(struct spi_dev_s *dev,
                                       uint32_t frequency)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;
    uint32_t clk_src;
    uint32_t prescaler;
    uint32_t actual_freq;

    if (priv->frequency == frequency)
    {
        return priv->frequency;
    }

    /* Get the source clock frequency */

    clk_src = HAL_RCC_GetHCLKFreq(priv->config->core);

    /* Calculate prescaler: SPI_clk = clk_src / prescaler */

    if (frequency >= clk_src)
    {
        prescaler = 1;
    }
    else
    {
        prescaler = (clk_src + frequency - 1) / frequency;
        if (prescaler > SPI_BAUDRATE_PRESCALER_MAX)
        {
            prescaler = SPI_BAUDRATE_PRESCALER_MAX;
        }

        if (prescaler < 1)
        {
            prescaler = 1;
        }
    }

    actual_freq = clk_src / prescaler;

    /* Disable SPI before reconfiguring */

    __HAL_SPI_DISABLE(&priv->handle);

    priv->handle.Init.BaudRatePrescaler = prescaler;
    HAL_SPI_Init(&priv->handle);

    priv->frequency = actual_freq;

    spiinfo("frequency=%lu actual=%lu prescaler=%lu\n",
            frequency, actual_freq, prescaler);

    return actual_freq;
}

/****************************************************************************
 * Name: sifli_spi_setmode
 *
 * Description:
 *   Set the SPI mode (CPOL, CPHA).
 *
 ****************************************************************************/

static void sifli_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;

    if (priv->mode == mode)
    {
        return;
    }

    __HAL_SPI_DISABLE(&priv->handle);

    switch (mode)
    {
        case SPIDEV_MODE0:  /* CPOL=0, CPHA=0 */
            priv->handle.Init.CLKPolarity = SPI_POLARITY_LOW;
            priv->handle.Init.CLKPhase    = SPI_PHASE_1EDGE;
            break;

        case SPIDEV_MODE1:  /* CPOL=0, CPHA=1 */
            priv->handle.Init.CLKPolarity = SPI_POLARITY_LOW;
            priv->handle.Init.CLKPhase    = SPI_PHASE_2EDGE;
            break;

        case SPIDEV_MODE2:  /* CPOL=1, CPHA=0 */
            priv->handle.Init.CLKPolarity = SPI_POLARITY_HIGH;
            priv->handle.Init.CLKPhase    = SPI_PHASE_1EDGE;
            break;

        case SPIDEV_MODE3:  /* CPOL=1, CPHA=1 */
            priv->handle.Init.CLKPolarity = SPI_POLARITY_HIGH;
            priv->handle.Init.CLKPhase    = SPI_PHASE_2EDGE;
            break;

        default:
            spierr("Unsupported SPI mode: %d\n", mode);
            return;
    }

    HAL_SPI_Init(&priv->handle);
    priv->mode = mode;

    spiinfo("mode=%d\n", mode);
}

/****************************************************************************
 * Name: sifli_spi_setbits
 *
 * Description:
 *   Set the number of bits per word.
 *
 ****************************************************************************/

static void sifli_spi_setbits(struct spi_dev_s *dev, int nbits)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;

    if (priv->nbits == nbits)
    {
        return;
    }

    __HAL_SPI_DISABLE(&priv->handle);

    /* Map NuttX bit width to HAL DataSize.
     * The HAL uses SPI_DATASIZE_xBIT macros (4-16 bits).
     */

    switch (nbits)
    {
        case 4:
            priv->handle.Init.DataSize = SPI_DATASIZE_4BIT;
            break;
        case 5:
            priv->handle.Init.DataSize = SPI_DATASIZE_5BIT;
            break;
        case 6:
            priv->handle.Init.DataSize = SPI_DATASIZE_6BIT;
            break;
        case 7:
            priv->handle.Init.DataSize = SPI_DATASIZE_7BIT;
            break;
        case 8:
            priv->handle.Init.DataSize = SPI_DATASIZE_8BIT;
            break;
        case 9:
            priv->handle.Init.DataSize = SPI_DATASIZE_9BIT;
            break;
        case 10:
            priv->handle.Init.DataSize = SPI_DATASIZE_10BIT;
            break;
        case 11:
            priv->handle.Init.DataSize = SPI_DATASIZE_11BIT;
            break;
        case 12:
            priv->handle.Init.DataSize = SPI_DATASIZE_12BIT;
            break;
        case 13:
            priv->handle.Init.DataSize = SPI_DATASIZE_13BIT;
            break;
        case 14:
            priv->handle.Init.DataSize = SPI_DATASIZE_14BIT;
            break;
        case 15:
            priv->handle.Init.DataSize = SPI_DATASIZE_15BIT;
            break;
        case 16:
            priv->handle.Init.DataSize = SPI_DATASIZE_16BIT;
            break;
        default:
            spierr("Unsupported bit width: %d, defaulting to 8\n", nbits);
            priv->handle.Init.DataSize = SPI_DATASIZE_8BIT;
            nbits = 8;
            break;
    }

    HAL_SPI_Init(&priv->handle);
    priv->nbits = nbits;

    spiinfo("nbits=%d\n", nbits);
}

/****************************************************************************
 * Name: sifli_spi_status
 *
 * Description:
 *   Return SPI status. Calls board-specific implementation.
 *
 ****************************************************************************/

static uint8_t sifli_spi_status(struct spi_dev_s *dev, uint32_t devid)
{
    return sf32lb_spi_status(dev, devid);
}

/****************************************************************************
 * Name: sifli_spi_send
 *
 * Description:
 *   Exchange one word on SPI.
 *
 ****************************************************************************/

static uint32_t sifli_spi_send(struct spi_dev_s *dev, uint32_t wd)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;
    uint32_t rxval = 0;
    HAL_StatusTypeDef status;
    uint16_t size;

    /* Determine the transfer size based on the data width */

    if (priv->nbits <= 8)
    {
        uint8_t txbyte = (uint8_t)wd;
        uint8_t rxbyte = 0;
        size = 1;

        __HAL_SPI_ENABLE(&priv->handle);
        status = HAL_SPI_TransmitReceive(&priv->handle, &txbyte, &rxbyte,
                                          size, SPI_XFER_TIMEOUT);
        rxval = (uint32_t)rxbyte;
    }
    else
    {
        uint16_t txword = (uint16_t)wd;
        uint16_t rxword = 0;
        size = 1;

        __HAL_SPI_ENABLE(&priv->handle);
        status = HAL_SPI_TransmitReceive(&priv->handle,
                                          (uint8_t *)&txword,
                                          (uint8_t *)&rxword,
                                          size, SPI_XFER_TIMEOUT);
        rxval = (uint32_t)rxword;
    }

    if (status != HAL_OK)
    {
        spierr("SPI send failed: %d\n", status);
    }

    return rxval;
}

/****************************************************************************
 * Name: sifli_spi_do_exchange
 *
 * Description:
 *   Internal exchange function that handles both DMA and polling modes.
 *
 ****************************************************************************/

static void sifli_spi_do_exchange(struct sifli_spi_dev_s *priv,
                                  const void *txbuffer, void *rxbuffer,
                                  size_t nwords)
{
    HAL_StatusTypeDef status;
    uint16_t size;
    int ret;

    /* Calculate number of bytes to transfer */

    if (priv->nbits <= 8)
    {
        size = (uint16_t)nwords;
    }
    else
    {
        size = (uint16_t)(nwords);
    }

    __HAL_SPI_ENABLE(&priv->handle);

    if (txbuffer && rxbuffer)
    {
        /* Full duplex exchange */

        if (priv->spi_dma_tx_flag && priv->spi_dma_rx_flag)
        {
            HAL_DMA_Init(&priv->dma.dma_tx);
            HAL_DMA_Init(&priv->dma.dma_rx);
            status = HAL_SPI_TransmitReceive_DMA(&priv->handle,
                                                  (uint8_t *)txbuffer,
                                                  (uint8_t *)rxbuffer,
                                                  size);
            if (status == HAL_OK)
            {
                ret = sifli_spi_wait_complete(priv);
                if (ret < 0)
                {
                    spierr("SPI DMA exchange timeout\n");
                }
            }
        }
        else
        {
            status = HAL_SPI_TransmitReceive_IT(&priv->handle,
                                                 (uint8_t *)txbuffer,
                                                 (uint8_t *)rxbuffer,
                                                 size);
            if (status == HAL_OK)
            {
                ret = sifli_spi_wait_complete(priv);
                if (ret < 0)
                {
                    spierr("SPI IT exchange timeout\n");
                }
            }
        }
    }
    else if (txbuffer)
    {
        /* Transmit only */

        if (priv->spi_dma_tx_flag)
        {
            HAL_DMA_Init(&priv->dma.dma_tx);
            status = HAL_SPI_Transmit_DMA(&priv->handle,
                                           (uint8_t *)txbuffer, size);
            if (status == HAL_OK)
            {
                ret = sifli_spi_wait_complete(priv);
                if (ret < 0)
                {
                    spierr("SPI DMA TX timeout\n");
                }
            }
        }
        else
        {
            status = HAL_SPI_Transmit_IT(&priv->handle,
                                          (uint8_t *)txbuffer, size);
            if (status == HAL_OK)
            {
                ret = sifli_spi_wait_complete(priv);
                if (ret < 0)
                {
                    spierr("SPI IT TX timeout\n");
                }
            }
        }
    }
    else if (rxbuffer)
    {
        /* Receive only */

        if (priv->spi_dma_rx_flag)
        {
            HAL_DMA_Init(&priv->dma.dma_rx);
            status = HAL_SPI_Receive_DMA(&priv->handle,
                                          (uint8_t *)rxbuffer, size);
            if (status == HAL_OK)
            {
                ret = sifli_spi_wait_complete(priv);
                if (ret < 0)
                {
                    spierr("SPI DMA RX timeout\n");
                }
            }
        }
        else
        {
            status = HAL_SPI_Receive_IT(&priv->handle,
                                         (uint8_t *)rxbuffer, size);
            if (status == HAL_OK)
            {
                ret = sifli_spi_wait_complete(priv);
                if (ret < 0)
                {
                    spierr("SPI IT RX timeout\n");
                }
            }
        }
    }

    UNUSED(status);
    UNUSED(ret);
}

#ifdef CONFIG_SPI_EXCHANGE

/****************************************************************************
 * Name: sifli_spi_exchange
 *
 * Description:
 *   Exchange a block of data on SPI.
 *
 ****************************************************************************/

static void sifli_spi_exchange(struct spi_dev_s *dev,
                               const void *txbuffer, void *rxbuffer,
                               size_t nwords)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;
    sifli_spi_do_exchange(priv, txbuffer, rxbuffer, nwords);
}

#else /* CONFIG_SPI_EXCHANGE */

/****************************************************************************
 * Name: sifli_spi_sndblock
 *
 * Description:
 *   Send a block of data on SPI.
 *
 ****************************************************************************/

static void sifli_spi_sndblock(struct spi_dev_s *dev, const void *buffer,
                               size_t nwords)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;
    sifli_spi_do_exchange(priv, buffer, NULL, nwords);
}

/****************************************************************************
 * Name: sifli_spi_recvblock
 *
 * Description:
 *   Receive a block of data from SPI.
 *
 ****************************************************************************/

static void sifli_spi_recvblock(struct spi_dev_s *dev, void *buffer,
                                size_t nwords)
{
    struct sifli_spi_dev_s *priv = (struct sifli_spi_dev_s *)dev;
    sifli_spi_do_exchange(priv, NULL, buffer, nwords);
}

#endif /* CONFIG_SPI_EXCHANGE */

/****************************************************************************
 * Name: sifli_spi_pm_prepare
 *
 * Description:
 *   PM state change preparation callback.
 *
 ****************************************************************************/

#ifdef CONFIG_PM
static int sifli_spi_pm_prepare(struct pm_callback_s *cb, int domain,
                                enum pm_state_e pmstate)
{
    struct sifli_spi_dev_s *priv =
        (struct sifli_spi_dev_s *)((char *)cb -
         offsetof(struct sifli_spi_dev_s, pm_cb));

    switch (pmstate)
    {
        case PM_NORMAL:
        case PM_IDLE:
            break;

        case PM_STANDBY:
        case PM_SLEEP:
            /* Check if the SPI bus is locked. */

            if (nxmutex_is_locked(&priv->lock))
            {
                return -EBUSY;
            }
            break;

        default:
            break;
    }

    return OK;
}
#endif

/****************************************************************************
 * HAL Callbacks - Called by HAL SPI layer
 ****************************************************************************/

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    /* Transfer complete - the ISR handler will post the semaphore */
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    /* Receive complete - the ISR handler will post the semaphore */
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    /* Transfer/Receive complete - the ISR handler will post the semaphore */
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    spierr("SPI HAL error callback: 0x%08lx\n", hspi->ErrorCode);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sifli_spibus_initialize
 *
 * Description:
 *   Initialize one SPI bus.
 *
 * Input Parameters:
 *   port - SPI port number (0-based index)
 *
 * Returned Value:
 *   A pointer to the SPI device structure on success; NULL on failure.
 *
 ****************************************************************************/

struct spi_dev_s *sifli_spibus_initialize(int port)
{
    struct sifli_spi_dev_s *priv;

    DEBUGASSERT(port >= 0 && port < SPI_MAX);

    priv = &spi_obj[port];

    /* Initialize synchronization primitives */

    nxmutex_init(&priv->lock);
    nxsem_init(&priv->sem_isr, 0, 0);

    nxmutex_lock(&priv->lock);

    /* Set up device state */

    priv->spidev.ops = &g_spi_ops;
    priv->ops        = &g_spi_ops;
    priv->port       = port;
    priv->config     = &bf0_spi_cfg[port];
    priv->mod        = bf0_spi_mod[port];
    priv->frequency  = 0;
    priv->mode       = SPIDEV_MODE0;
    priv->nbits      = 8;

    /* Initialize hardware */

    sifli_spi_hw_init(priv);

#ifdef CONFIG_PM
    /* Register power management callbacks */

    priv->pm_cb.prepare = sifli_spi_pm_prepare;
    DEBUGVERIFY(pm_register(&priv->pm_cb));
#endif

    nxmutex_unlock(&priv->lock);

    spiinfo("SPI%d initialized\n", port + 1);

    return &priv->spidev;
}

/****************************************************************************
 * Name: sifli_spibus_uninitialize
 *
 * Description:
 *   Uninitialize an SPI bus.
 *
 ****************************************************************************/

int sifli_spibus_uninitialize(struct spi_dev_s *dev)
{
    struct sifli_spi_dev_s *priv;

    DEBUGASSERT(dev != NULL);
    priv = (struct sifli_spi_dev_s *)dev;

    nxmutex_lock(&priv->lock);

#ifdef CONFIG_PM
    pm_unregister(&priv->pm_cb);
#endif

    sifli_spi_hw_deinit(priv);

    nxmutex_unlock(&priv->lock);

    spiinfo("SPI%d uninitialized\n", priv->port + 1);

    return OK;
}

/****************************************************************************
 * Name: sf32lb_spi_select (weak)
 *
 * Description:
 *   Board-specific SPI chip select callback default (weak) implementation.
 *   Boards should override this for their specific CS GPIO management.
 *
 ****************************************************************************/

__attribute__((weak)) void sf32lb_spi_select(struct spi_dev_s *dev,
                                              uint32_t devid, bool selected)
{
    spiinfo("devid=%lu, selected=%d\n", devid, selected);
}

/****************************************************************************
 * Name: sf32lb_spi_status (weak)
 *
 * Description:
 *   Board-specific SPI status callback default (weak) implementation.
 *
 ****************************************************************************/

__attribute__((weak)) uint8_t sf32lb_spi_status(struct spi_dev_s *dev,
                                                 uint32_t devid)
{
    return SPI_STATUS_PRESENT;
}
