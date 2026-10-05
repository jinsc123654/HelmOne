/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/lcd_spi2.c
 *
 * Dedicated SPI2 for ST7789 (PA37-PA42). Self-contained; does not use or
 * modify vendor/my_vendor/chips/sf32lb52/sifli_spi.c (SPI1 general bus).
 *
 * 后续剔除：Helm One 已改 NV3031A，不再使用本文件。
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#include <sfconfig.h>

#ifdef CONFIG_LCD_USING_ST7789S
#ifndef CONFIG_LCD_ST7789S_SOFTSPI

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/spi/spi.h>
#include <debug.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "register.h"
#include "bf0_hal.h"
#include "dma_config.h"
#include "spi_config.h"
#include "drv_io.h"
#include "lcd_spi2.h"
#include <syslog.h>

#define LCD_SPI2_XFER_TIMEOUT_MS  5000
#define LCD_SPI2_MAX_XFER         65535

/* SPI/USART/I2C are clocked by clk_peri_hpsys (HRC48 or HXT48 via
 * HPSYS_RCC CSR_SEL_PERI), fixed at 48 MHz and independent of the system /
 * HCLK clock. This is the SPI2 baud source, NOT the 240 MHz HCLK, so the
 * prescaler must be computed against 48 MHz (max bus rate = 48 MHz).
 */
#define LCD_SPI2_PERI_CLK_HZ      48000000u

/* Below this pixel count, use polling instead of DMA. Partial-area updates
 * (st7789_putarea) flush row-by-row; a single narrow row is small enough
 * that DMA setup + IRQ overhead costs more than a quick polled FIFO fill.
 * Full-width / full-screen updates are one large transfer and use DMA.
 */
#define LCD_SPI2_DMA_MIN_PIXELS   512

/* SPI TX FIFO is 16 entries deep (SPI_FIFO_SIZE). With BurstSize=0 the DMA
 * moves one word per service request, starving the FIFO (~8x slower than
 * polling). Burst N fills N words per request; keep <= half FIFO to avoid
 * TX FIFO overflow. Set to 1 to revert to single-word DMA.
 */
#define LCD_SPI2_DMA_BURST        8

/* Set to 1 to re-enable verbose DMA/transfer tracing. */
#define LCD_SPI2_DEBUG 0

#if LCD_SPI2_DEBUG
#  define lcd_spi2_dbg(fmt, ...) \
     syslog(LOG_INFO, "DBG lcd_spi2: " fmt, ##__VA_ARGS__)
#else
#  define lcd_spi2_dbg(fmt, ...) do { } while (0)
#endif

struct lcd_spi2_dev_s
{
  struct spi_dev_s dev;
  mutex_t lock;
  sem_t sem_done;
  SPI_HandleTypeDef hspi;
  DMA_HandleTypeDef hdmatx;
#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  struct dma_config tx_dma_cfg;
  bool dma_enabled;
  volatile bool dma_inflight;
  volatile uint32_t dbg_dma_isr;
  volatile uint32_t dbg_spi_isr;
  volatile uint32_t dbg_done_post;
  volatile uint32_t dbg_done_skip;
#endif
  int nbits;
  uint32_t frequency;
  enum spi_mode_e mode;
};

static struct lcd_spi2_dev_s g_lcd_spi2;

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
static int lcd_spi2_dma_isr(int irq, FAR void *context, FAR void *arg);
#endif

static int lcd_spi2_lock(FAR struct spi_dev_s *dev, bool lock);
static void lcd_spi2_select(FAR struct spi_dev_s *dev, uint32_t devid,
                            bool selected);
static uint32_t lcd_spi2_setfrequency(FAR struct spi_dev_s *dev,
                                      uint32_t frequency);
static void lcd_spi2_setmode(FAR struct spi_dev_s *dev, enum spi_mode_e mode);
static void lcd_spi2_setbits(FAR struct spi_dev_s *dev, int nbits);
static uint8_t lcd_spi2_status(FAR struct spi_dev_s *dev, uint32_t devid);
static int lcd_spi2_cmddata(FAR struct spi_dev_s *dev, uint32_t devid,
                            bool cmd);
static uint32_t lcd_spi2_send(FAR struct spi_dev_s *dev, uint32_t wd);
#ifdef CONFIG_SPI_EXCHANGE
static void lcd_spi2_exchange(FAR struct spi_dev_s *dev,
                              FAR const void *txbuffer, FAR void *rxbuffer,
                              size_t nwords);
#else
static void lcd_spi2_sndblock(FAR struct spi_dev_s *dev,
                              FAR const void *buffer, size_t nwords);
static void lcd_spi2_recvblock(FAR struct spi_dev_s *dev,
                               FAR void *buffer, size_t nwords);
#endif

static const struct spi_ops_s g_lcd_spi2_ops =
{
  .lock         = lcd_spi2_lock,
  .select       = lcd_spi2_select,
  .setfrequency = lcd_spi2_setfrequency,
  .setmode      = lcd_spi2_setmode,
  .setbits      = lcd_spi2_setbits,
  .status       = lcd_spi2_status,
#ifdef CONFIG_SPI_CMDDATA
  .cmddata      = lcd_spi2_cmddata,
#endif
  .send         = lcd_spi2_send,
#ifdef CONFIG_SPI_EXCHANGE
  .exchange     = lcd_spi2_exchange,
#else
  .sndblock     = lcd_spi2_sndblock,
  .recvblock    = lcd_spi2_recvblock,
#endif
};

static inline FAR struct lcd_spi2_dev_s *lcd_spi2_dev(FAR struct spi_dev_s *dev)
{
  return (FAR struct lcd_spi2_dev_s *)dev;
}

static void lcd_spi2_dcache_clean(FAR const void *buffer, size_t nbytes)
{
#ifdef CONFIG_ARCH_DCACHE
  if (buffer != NULL && nbytes > 0)
    {
      up_clean_dcache((uintptr_t)buffer, (uintptr_t)buffer + nbytes);
    }
#endif
}

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
static void lcd_spi2_dma_apply_align(FAR struct lcd_spi2_dev_s *priv)
{
  if (priv->nbits > 8)
    {
      priv->hdmatx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
      priv->hdmatx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    }
  else
    {
      priv->hdmatx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
      priv->hdmatx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    }
}

static void lcd_spi2_dma_setup(FAR struct lcd_spi2_dev_s *priv)
{
  priv->tx_dma_cfg.Instance    = SPI2_TX_DMA_INSTANCE;
  priv->tx_dma_cfg.dma_irq_prio = SPI2_TX_DMA_IRQ_PRIO;
  priv->tx_dma_cfg.dma_irq     = SPI2_TX_DMA_IRQ;
  priv->tx_dma_cfg.request     = SPI2_TX_DMA_REQUEST;

  priv->hdmatx.Instance             = priv->tx_dma_cfg.Instance;
  priv->hdmatx.Init.Request         = priv->tx_dma_cfg.request;
  priv->hdmatx.Init.Direction       = DMA_MEMORY_TO_PERIPH;
  priv->hdmatx.Init.PeriphInc       = DMA_PINC_DISABLE;
  priv->hdmatx.Init.MemInc          = DMA_MINC_ENABLE;
  priv->hdmatx.Init.Mode            = DMA_NORMAL;
  priv->hdmatx.Init.Priority        = DMA_PRIORITY_HIGH;
  priv->hdmatx.Init.IrqPrio         = priv->tx_dma_cfg.dma_irq_prio;
  priv->hdmatx.Init.BurstSize       = LCD_SPI2_DMA_BURST;
  lcd_spi2_dma_apply_align(priv);

  HAL_RCC_EnableModule(RCC_MOD_DMAC1);

  __HAL_LINKDMA(&priv->hspi, hdmatx, priv->hdmatx);

  if (HAL_DMA_Init(&priv->hdmatx) != HAL_OK)
    {
      syslog(LOG_ERR, "ERR lcd_spi2: HAL_DMA_Init failed in setup\n");
      priv->dma_enabled = false;
      return;
    }

  irq_attach(priv->tx_dma_cfg.dma_irq + 16, lcd_spi2_dma_isr, priv);
  up_enable_irq(priv->tx_dma_cfg.dma_irq + 16);

  priv->dma_enabled = true;
  lcd_spi2_dbg("dma_setup ch=%p irq=%d req=%lu\n",
               priv->hdmatx.Instance,
               (int)priv->tx_dma_cfg.dma_irq,
               (unsigned long)priv->tx_dma_cfg.request);
}

static void lcd_spi2_drain_done(FAR struct lcd_spi2_dev_s *priv)
{
  while (nxsem_trywait(&priv->sem_done) == OK)
    {
      /* Drop stale completion tokens before starting a new DMA chunk. */
    }
}

static void lcd_spi2_signal_done(FAR struct lcd_spi2_dev_s *priv, int from)
{
  if (priv->dma_inflight)
    {
      priv->dma_inflight = false;
      priv->dbg_done_post++;
      nxsem_post(&priv->sem_done);
      if (priv->dbg_done_post <= 4)
        {
          lcd_spi2_dbg("signal_done from=%d post=%lu state=%d err=0x%08lx\n",
                       from, (unsigned long)priv->dbg_done_post,
                       priv->hspi.State, priv->hspi.ErrorCode);
        }
    }
  else
    {
      priv->dbg_done_skip++;
      if (priv->dbg_done_skip <= 4)
        {
          lcd_spi2_dbg("signal_done skip from=%d state=%d inflight=0\n",
                       from, priv->hspi.State);
        }
    }
}

static int lcd_spi2_dma_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct lcd_spi2_dev_s *priv = arg;

  (void)irq;
  (void)context;

  HAL_DMA_IRQHandler(priv->hspi.hdmatx);
  priv->dbg_dma_isr++;

  /* HAL SPI DMA completion runs in this IRQ (SPI_DMATransmitCplt). */
  if (priv->dbg_dma_isr <= 4)
    {
      lcd_spi2_dbg("dma_isr #%lu state=%d err=0x%08lx dma_state=%d\n",
                   (unsigned long)priv->dbg_dma_isr,
                   priv->hspi.State, priv->hspi.ErrorCode,
                   priv->hspi.hdmatx != NULL ?
                   priv->hspi.hdmatx->State : -1);
    }

  if (priv->hspi.State == HAL_SPI_STATE_READY ||
      priv->hspi.State == HAL_SPI_STATE_ERROR ||
      priv->hspi.ErrorCode != HAL_SPI_ERROR_NONE)
    {
      lcd_spi2_signal_done(priv, 1);
    }

  return OK;
}

static int lcd_spi2_wait_done(FAR struct lcd_spi2_dev_s *priv)
{
  int ret;
  struct timespec t0;
  struct timespec t1;
  uint32_t wait_ms;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  /* Block until the DMA ISR posts sem_done (wakes within microseconds).
   * Safe now: stale tokens are drained before each DMA start, and the SPI
   * NVIC line is masked during DMA, so only the DMA ISR posts.
   */
  ret = nxsem_tickwait_uninterruptible(&priv->sem_done,
                                       MSEC2TICK(LCD_SPI2_XFER_TIMEOUT_MS));
  clock_gettime(CLOCK_MONOTONIC, &t1);
  {
    time_t sec = t1.tv_sec - t0.tv_sec;
    long nsec = t1.tv_nsec - t0.tv_nsec;

    if (nsec < 0)
      {
        sec--;
        nsec += 1000000000L;
      }

    wait_ms = (sec < 0) ? 0u :
              (uint32_t)((uint64_t)sec * 1000u + (uint64_t)nsec / 1000000u);
  }

  if (wait_ms >= 200u)
    {
      syslog(LOG_WARNING,
             "[stab] lcd_spi2_wait %ums state=%d inflight=%d dma_isr=%lu\n",
             (unsigned)wait_ms, priv->hspi.State, priv->dma_inflight,
             (unsigned long)priv->dbg_dma_isr);
    }

  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERR lcd_spi2: wait timeout state=%d inflight=%d "
             "dma_isr=%lu post=%lu skip=%lu err=0x%08lx\n",
             priv->hspi.State, priv->dma_inflight,
             (unsigned long)priv->dbg_dma_isr,
             (unsigned long)priv->dbg_done_post,
             (unsigned long)priv->dbg_done_skip,
             priv->hspi.ErrorCode);
      priv->dma_inflight = false;
      HAL_SPI_Abort(&priv->hspi);
      return -ETIMEDOUT;
    }

  if (priv->hspi.ErrorCode != HAL_SPI_ERROR_NONE)
    {
      syslog(LOG_ERR, "ERR lcd_spi2: xfer error 0x%08lx\n",
             priv->hspi.ErrorCode);
      return -EIO;
    }

  return OK;
}
#endif

static void lcd_spi2_tx_poll(FAR struct lcd_spi2_dev_s *priv,
                             FAR const void *buffer, size_t nwords)
{
  HAL_StatusTypeDef status;
  uint16_t size;
  size_t bytes_per = (priv->nbits > 8) ? 2 : 1;

  if (nwords == 0)
    {
      return;
    }

  if (nwords >= LCD_SPI2_DMA_MIN_PIXELS)
    {
      lcd_spi2_dbg("tx_poll nwords=%zu nbits=%d\n", nwords, priv->nbits);
    }

  if (priv->hspi.State != HAL_SPI_STATE_READY)
    {
      HAL_SPI_Abort(&priv->hspi);
    }

  __HAL_SPI_ENABLE(&priv->hspi);

  while (nwords > 0)
    {
      size = (nwords > LCD_SPI2_MAX_XFER) ? LCD_SPI2_MAX_XFER : (uint16_t)nwords;

      status = HAL_SPI_Transmit(&priv->hspi, (uint8_t *)buffer, size,
                                LCD_SPI2_XFER_TIMEOUT_MS);
      if (status != HAL_OK)
        {
          spierr("LCD SPI2 poll TX failed: %d state=%d\n", status,
                 priv->hspi.State);
          break;
        }

      nwords -= size;
      buffer = (FAR const uint8_t *)buffer + size * bytes_per;
    }
}

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
static void lcd_spi2_spi_irq_restore(FAR struct lcd_spi2_dev_s *priv)
{
  /* HAL may leave TXE enabled in INTE; clear SPI IT sources so re-enabling
   * the NVIC line does not fire into a NULL TxISR (DMA mode).
   */
  __HAL_SPI_DISABLE_IT(&priv->hspi, (SPI_IT_TXE | SPI_IT_RXNE | SPI_IT_ERR));
  up_enable_irq(SPI2_IRQn + 16);
}

static void lcd_spi2_dma_prepare(FAR struct lcd_spi2_dev_s *priv)
{
  if (priv->hdmatx.State != HAL_DMA_STATE_READY)
    {
      HAL_DMA_Abort(&priv->hdmatx);
      priv->hdmatx.State = HAL_DMA_STATE_READY;
    }

  priv->hdmatx.Lock = HAL_UNLOCKED;
  lcd_spi2_dma_apply_align(priv);
}

static int lcd_spi2_tx_dma(FAR struct lcd_spi2_dev_s *priv,
                           FAR const void *buffer, size_t nwords)
{
  HAL_StatusTypeDef status;
  size_t bytes_per = (priv->nbits > 8) ? 2 : 1;

  if (priv->hspi.State != HAL_SPI_STATE_READY)
    {
      HAL_SPI_Abort(&priv->hspi);
    }

  lcd_spi2_dcache_clean(buffer, nwords * bytes_per);
  lcd_spi2_dma_apply_align(priv);
  __HAL_SPI_ENABLE(&priv->hspi);

  /* HAL_SPI_Transmit_DMA enables SPI_IT_TXE which drives DMA via TSRE but
   * also raises the SPI NVIC line continuously, starving this thread so the
   * function never returns. DMA completion arrives on the DMA channel IRQ
   * (DMAC1_CH8), so mask the SPI NVIC line for the whole transfer.
   */
  up_disable_irq(SPI2_IRQn + 16);

  lcd_spi2_dbg("tx_dma start nwords=%zu nbits=%d\n", nwords, priv->nbits);

  while (nwords > 0)
    {
      uint16_t chunk = (nwords > LCD_SPI2_MAX_XFER) ?
                       LCD_SPI2_MAX_XFER : (uint16_t)nwords;
      int ret;

      lcd_spi2_dbg("tx_dma chunk=%u left=%zu buf=%p\n",
                   chunk, nwords, buffer);

      lcd_spi2_drain_done(priv);
      lcd_spi2_dma_prepare(priv);

      lcd_spi2_dbg("tx_dma pre-init dma_st=%d spi_st=%d\n",
                   priv->hdmatx.State, priv->hspi.State);

      if (HAL_DMA_Init(&priv->hdmatx) != HAL_OK)
        {
          syslog(LOG_ERR, "ERR lcd_spi2: HAL_DMA_Init failed chunk=%u\n", chunk);
          lcd_spi2_spi_irq_restore(priv);
          return -EIO;
        }

      lcd_spi2_dbg("tx_dma post-init dma_st=%d\n", priv->hdmatx.State);

      {
        static bool diag_once;
        if (!diag_once)
          {
            diag_once = true;
            syslog(LOG_INFO,
                   "DIAG lcd_spi2: freq=%lu presc=%lu hclk=%lu pclk1=%lu "
                   "pclk2=%lu CBSR=%lu burst=%d nbits=%d\n",
                   (unsigned long)priv->frequency,
                   (unsigned long)priv->hspi.Init.BaudRatePrescaler,
                   (unsigned long)HAL_RCC_GetHCLKFreq(SPI2_CORE),
                   (unsigned long)HAL_RCC_GetPCLKFreq(SPI2_CORE, 1),
                   (unsigned long)HAL_RCC_GetPCLKFreq(SPI2_CORE, 0),
                   (unsigned long)priv->hdmatx.Instance->CBSR,
                   LCD_SPI2_DMA_BURST, priv->nbits);
          }
      }

      priv->dma_inflight = true;

      status = HAL_SPI_Transmit_DMA(&priv->hspi, (uint8_t *)buffer, chunk);

      lcd_spi2_dbg("tx_dma post-xmit status=%d spi_st=%d dma_st=%d\n",
                   status, priv->hspi.State, priv->hdmatx.State);

      if (status != HAL_OK)
        {
          priv->dma_inflight = false;
          syslog(LOG_ERR, "ERR lcd_spi2: DMA start failed %d spi_st=%d dma_st=%d\n",
                 status, priv->hspi.State, priv->hdmatx.State);
          lcd_spi2_spi_irq_restore(priv);
          return -EIO;
        }

      ret = lcd_spi2_wait_done(priv);
      if (ret < 0)
        {
          syslog(LOG_WARNING,
                 "WRN lcd_spi2: DMA chunk wait failed, poll fallback "
                 "chunk=%u\n", chunk);
          priv->dma_inflight = false;
          HAL_SPI_Abort(&priv->hspi);
          lcd_spi2_tx_poll(priv, buffer, chunk);
        }
      else
        {
          lcd_spi2_dbg("tx_dma chunk done chunk=%u elapsed_ok\n", chunk);
        }

      nwords -= chunk;
      buffer = (FAR const uint8_t *)buffer + chunk * bytes_per;
    }

  lcd_spi2_spi_irq_restore(priv);

  lcd_spi2_dbg("tx_dma complete\n");
  return OK;
}
#endif

static int lcd_spi2_do_tx(FAR struct lcd_spi2_dev_s *priv,
                          FAR const void *buffer, size_t nwords,
                          bool allow_dma)
{
#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  if (allow_dma && priv->dma_enabled && nwords >= LCD_SPI2_DMA_MIN_PIXELS)
    {
      lcd_spi2_dbg("do_tx DMA path nwords=%zu nbits=%d\n",
                   nwords, priv->nbits);
      int ret = lcd_spi2_tx_dma(priv, buffer, nwords);

      if (ret >= 0)
        {
          return ret;
        }

      spiwarn("LCD SPI2 DMA failed, fallback to poll\n");
    }
#else
  (void)allow_dma;
#endif

  lcd_spi2_dbg("do_tx poll path nwords=%zu nbits=%d\n", nwords, priv->nbits);
  lcd_spi2_tx_poll(priv, buffer, nwords);
  return OK;
}

static int lcd_spi2_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct lcd_spi2_dev_s *priv = arg;

  (void)irq;
  (void)context;

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  /* During DMA pixel TX, ignore SPI TXE IRQ storms; SPI_DMATransmitCplt
   * runs in the DMA ISR and posts sem_done.
   */
  if (priv->dma_inflight)
    {
      priv->dbg_spi_isr++;
      if (priv->dbg_spi_isr <= 4)
        {
          lcd_spi2_dbg("spi_isr #%lu ignored inflight=1 state=%d\n",
                       (unsigned long)priv->dbg_spi_isr, priv->hspi.State);
        }

      return OK;
    }
#endif

  HAL_SPI_IRQHandler(&priv->hspi);

  return OK;
}

static int lcd_spi2_lock(FAR struct spi_dev_s *dev, bool lock)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);

  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

static void lcd_spi2_select(FAR struct spi_dev_s *dev, uint32_t devid,
                            bool selected)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);

  if (selected)
    {
      __HAL_SPI_ENABLE(&priv->hspi);
      __HAL_SPI_TAKE_CS(&priv->hspi);
    }
  else
    {
      if (priv->hspi.State != HAL_SPI_STATE_READY)
        {
          HAL_SPI_Abort(&priv->hspi);
        }

      __HAL_SPI_RELEASE_CS(&priv->hspi);
      __HAL_SPI_DISABLE(&priv->hspi);
    }

  (void)devid;
}

static uint32_t lcd_spi2_setfrequency(FAR struct spi_dev_s *dev,
                                      uint32_t frequency)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);
  uint32_t clk_src;
  uint32_t prescaler;

  if (priv->frequency == frequency)
    {
      return priv->frequency;
    }

  /* The SPI/USART/I2C baud divider is fed by clk_peri_hpsys, which is selected
   * (HPSYS_RCC CSR_SEL_PERI) between clk_hrc48 and clk_hxt48 - both 48 MHz - and
   * is independent of the 240 MHz HCLK / system clock.  Deriving the prescaler
   * from HCLK made it 5x too small in clock terms (presc=4 -> 48/4=12 MHz real
   * bus instead of the intended 60 MHz).  Use the true 48 MHz source so a
   * prescaler of 1 selects the full 48 MHz (the hardware maximum for SPI2).
   */
  clk_src = LCD_SPI2_PERI_CLK_HZ;

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

  __HAL_SPI_DISABLE(&priv->hspi);
  priv->hspi.Init.BaudRatePrescaler = prescaler;
  HAL_SPI_Init(&priv->hspi);
  priv->frequency = clk_src / prescaler;

  return priv->frequency;
}

static void lcd_spi2_setmode(FAR struct spi_dev_s *dev, enum spi_mode_e mode)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);

  if (priv->mode == mode)
    {
      return;
    }

  __HAL_SPI_DISABLE(&priv->hspi);

  switch (mode)
    {
      case SPIDEV_MODE0:
        priv->hspi.Init.CLKPolarity = SPI_POLARITY_LOW;
        priv->hspi.Init.CLKPhase    = SPI_PHASE_1EDGE;
        break;
      case SPIDEV_MODE1:
        priv->hspi.Init.CLKPolarity = SPI_POLARITY_LOW;
        priv->hspi.Init.CLKPhase    = SPI_PHASE_2EDGE;
        break;
      case SPIDEV_MODE2:
        priv->hspi.Init.CLKPolarity = SPI_POLARITY_HIGH;
        priv->hspi.Init.CLKPhase    = SPI_PHASE_1EDGE;
        break;
      case SPIDEV_MODE3:
      default:
        priv->hspi.Init.CLKPolarity = SPI_POLARITY_HIGH;
        priv->hspi.Init.CLKPhase    = SPI_PHASE_2EDGE;
        break;
    }

  HAL_SPI_Init(&priv->hspi);
  priv->mode = mode;
}

static void lcd_spi2_setbits(FAR struct spi_dev_s *dev, int nbits)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);

  if (priv->nbits == nbits)
    {
      return;
    }

  __HAL_SPI_DISABLE(&priv->hspi);

  switch (nbits)
    {
      case 16:
        priv->hspi.Init.DataSize = SPI_DATASIZE_16BIT;
        break;
      case 8:
      default:
        priv->hspi.Init.DataSize = SPI_DATASIZE_8BIT;
        nbits = 8;
        break;
    }

  HAL_SPI_Init(&priv->hspi);
  priv->nbits = nbits;
#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  lcd_spi2_dma_apply_align(priv);
#endif
}

static uint8_t lcd_spi2_status(FAR struct spi_dev_s *dev, uint32_t devid)
{
  (void)dev;
  (void)devid;
  return SPI_STATUS_PRESENT;
}

static int lcd_spi2_cmddata(FAR struct spi_dev_s *dev, uint32_t devid,
                            bool cmd)
{
  if (devid == SPIDEV_DISPLAY(0))
    {
      BSP_LCD_DC_Set(cmd);
      return OK;
    }

  return -ENODEV;
}

static uint32_t lcd_spi2_send(FAR struct spi_dev_s *dev, uint32_t wd)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);
  HAL_StatusTypeDef status;

  if (priv->hspi.State != HAL_SPI_STATE_READY)
    {
      HAL_SPI_Abort(&priv->hspi);
    }

  __HAL_SPI_ENABLE(&priv->hspi);

  if (priv->nbits <= 8)
    {
      uint8_t tx = (uint8_t)wd;

      status = HAL_SPI_Transmit(&priv->hspi, &tx, 1,
                                LCD_SPI2_XFER_TIMEOUT_MS);
    }
  else
    {
      uint16_t tx = (uint16_t)wd;

      status = HAL_SPI_Transmit(&priv->hspi, (uint8_t *)&tx, 1,
                                LCD_SPI2_XFER_TIMEOUT_MS);
    }

  if (status != HAL_OK)
    {
      spierr("LCD SPI2 send failed: %d\n", status);
    }

  return 0;
}

#ifdef CONFIG_SPI_EXCHANGE
static void lcd_spi2_exchange(FAR struct spi_dev_s *dev,
                              FAR const void *txbuffer, FAR void *rxbuffer,
                              size_t nwords)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);

  if (rxbuffer != NULL)
    {
      spierr("LCD SPI2 RX not supported\n");
      return;
    }

  if (txbuffer == NULL)
    {
      return;
    }

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  (void)lcd_spi2_do_tx(priv, txbuffer, nwords, true);
#else
  (void)lcd_spi2_do_tx(priv, txbuffer, nwords, false);
#endif
}
#else
static void lcd_spi2_sndblock(FAR struct spi_dev_s *dev,
                              FAR const void *buffer, size_t nwords)
{
  FAR struct lcd_spi2_dev_s *priv = lcd_spi2_dev(dev);

  lcd_spi2_dbg("sndblock nwords=%zu nbits=%d\n", nwords, priv->nbits);

  /* FBIO_UPDATE -> st7789_putarea -> sndblock: DMA TX, thread waits on sem. */

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  (void)lcd_spi2_do_tx(priv, buffer, nwords, true);
#else
  (void)lcd_spi2_do_tx(priv, buffer, nwords, false);
#endif
}

static void lcd_spi2_recvblock(FAR struct spi_dev_s *dev,
                               FAR void *buffer, size_t nwords)
{
  (void)dev;
  (void)buffer;
  (void)nwords;
}
#endif

int lcd_spi2_tx_pixels(FAR struct spi_dev_s *dev,
                       FAR const void *buffer, size_t npixels)
{
  FAR struct lcd_spi2_dev_s *priv;

  if (dev == NULL || buffer == NULL || npixels == 0)
    {
      return -EINVAL;
    }

  priv = lcd_spi2_dev(dev);
#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  return lcd_spi2_do_tx(priv, buffer, npixels, true);
#else
  return lcd_spi2_do_tx(priv, buffer, npixels, false);
#endif
}

int lcd_spi2_fb_flush(FAR struct spi_dev_s *dev,
                      FAR const uint8_t *fbmem,
                      uint16_t xres, uint16_t yres,
                      size_t stride)
{
  size_t row_bytes;
  int y;
  int ret = OK;

  if (dev == NULL || fbmem == NULL || xres == 0 || yres == 0)
    {
      return -EINVAL;
    }

  row_bytes = (size_t)xres * sizeof(uint16_t);

  SPI_LOCK(dev, true);
  SPI_SETMODE(dev, CONFIG_LCD_ST7789_SPIMODE);
  SPI_SETFREQUENCY(dev, CONFIG_LCD_ST7789_FREQUENCY);
  SPI_SETBITS(dev, 16);
  SPI_SELECT(dev, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(dev, SPIDEV_DISPLAY(0), false);

  if (stride == row_bytes)
    {
      ret = lcd_spi2_tx_pixels(dev, fbmem, (size_t)xres * yres);
    }
  else
    {
      for (y = 0; y < yres && ret >= 0; y++)
        {
          ret = lcd_spi2_tx_pixels(dev, fbmem + y * stride, xres);
        }
    }

  SPI_SELECT(dev, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(dev, false);

  return ret;
}

struct spi_dev_s *lcd_spi2_dev_initialize(void)
{
  FAR struct lcd_spi2_dev_s *priv = &g_lcd_spi2;

  nxmutex_init(&priv->lock);
  nxsem_init(&priv->sem_done, 0, 0);

  priv->dev.ops = &g_lcd_spi2_ops;
  priv->nbits = 8;
  priv->mode = SPIDEV_MODE0;
  priv->frequency = 0;
#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  priv->dma_enabled = false;
  priv->dma_inflight = false;
  priv->dbg_dma_isr = 0;
  priv->dbg_spi_isr = 0;
  priv->dbg_done_post = 0;
  priv->dbg_done_skip = 0;
#endif

  HAL_RCC_EnableModule(RCC_MOD_SPI2);

  priv->hspi.Instance = SPI2;
  priv->hspi.core     = SPI2_CORE;
  priv->hspi.Init.Mode              = SPI_MODE_MASTER;
  priv->hspi.Init.Direction         = SPI_DIRECTION_2LINES;
  priv->hspi.Init.DataSize          = SPI_DATASIZE_8BIT;
  priv->hspi.Init.CLKPolarity       = SPI_POLARITY_LOW;
  priv->hspi.Init.CLKPhase          = SPI_PHASE_1EDGE;
  priv->hspi.Init.BaudRatePrescaler = 48;
  priv->hspi.Init.FrameFormat       = SPI_FRAME_FORMAT_SPI;
  priv->hspi.Init.SFRMPol           = SPI_SFRMPOL_LOW;

  HAL_SPI_Init(&priv->hspi);

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  lcd_spi2_dma_setup(priv);
#endif

  irq_attach(SPI2_IRQn + 16, lcd_spi2_isr, priv);
  up_enable_irq(SPI2_IRQn + 16);

  lcd_spi2_dbg("init done dma=%d spi_irq=%d\n",
#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
               priv->dma_enabled,
#else
               0,
#endif
               (int)SPI2_IRQn);

  return &priv->dev;
}

void lcd_spi2_abort_active_xfer(void)
{
  FAR struct lcd_spi2_dev_s *priv = &g_lcd_spi2;

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  syslog(LOG_ERR,
         "[lcd_spi2] force abort inflight=%d state=%d err=0x%08lx\n",
         priv->dma_inflight, priv->hspi.State, priv->hspi.ErrorCode);
  HAL_DMA_Abort(&priv->hdmatx);
  HAL_SPI_Abort(&priv->hspi);
  priv->hdmatx.State = HAL_DMA_STATE_READY;
  priv->hdmatx.Lock = HAL_UNLOCKED;
  priv->hspi.ErrorCode = HAL_SPI_ERROR_NONE;
  priv->dma_inflight = false;
  lcd_spi2_spi_irq_restore(priv);
  __HAL_SPI_RELEASE_CS(&priv->hspi);
  __HAL_SPI_DISABLE(&priv->hspi);
  nxsem_post(&priv->sem_done);
  /* st7789_select() may hold SPI_LOCK if ui_flush was aborted mid-PUTAREA. */
  nxmutex_reset(&priv->lock);
#else
  HAL_SPI_Abort(&priv->hspi);
  priv->hspi.ErrorCode = HAL_SPI_ERROR_NONE;
  __HAL_SPI_RELEASE_CS(&priv->hspi);
  __HAL_SPI_DISABLE(&priv->hspi);
  nxmutex_reset(&priv->lock);
#endif
}

void lcd_spi2_restart_xfer_path(void)
{
  FAR struct lcd_spi2_dev_s *priv = &g_lcd_spi2;

  syslog(LOG_ERR, "[lcd_spi2] restart xfer path (SPI2+DMA)");

  lcd_spi2_drain_done(priv);
  nxsem_reset(&priv->sem_done, 0);
  nxmutex_reset(&priv->lock);

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  if (priv->dma_inflight ||
      priv->hspi.State != HAL_SPI_STATE_READY ||
      priv->hspi.ErrorCode != HAL_SPI_ERROR_NONE)
    {
      HAL_DMA_Abort(&priv->hdmatx);
      HAL_SPI_Abort(&priv->hspi);
      priv->dma_inflight = false;
    }

  lcd_spi2_spi_irq_restore(priv);

  if (priv->dma_enabled)
    {
      HAL_DMA_DeInit(&priv->hdmatx);
      priv->dma_enabled = false;
    }

  priv->hdmatx.State = HAL_DMA_STATE_READY;
  priv->hdmatx.Lock = HAL_UNLOCKED;
#else
  if (priv->hspi.State != HAL_SPI_STATE_READY)
    {
      HAL_SPI_Abort(&priv->hspi);
    }
#endif

  priv->hspi.ErrorCode = HAL_SPI_ERROR_NONE;
  __HAL_SPI_DISABLE(&priv->hspi);
  HAL_SPI_Init(&priv->hspi);

#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
  lcd_spi2_dma_setup(priv);
#endif

  syslog(LOG_ERR, "[lcd_spi2] xfer path ready dma=%d spi_state=%d",
#ifdef CONFIG_LCD_ST7789S_SPI2_TX_DMA
         priv->dma_enabled,
#else
         0,
#endif
         priv->hspi.State);
}

#endif /* !CONFIG_LCD_ST7789S_SOFTSPI */
#endif /* CONFIG_LCD_USING_ST7789S */
