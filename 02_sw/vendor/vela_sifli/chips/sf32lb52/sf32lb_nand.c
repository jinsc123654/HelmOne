/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <nuttx/config.h>

#include <sys/types.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>

#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/mutex.h>

#include "register.h"
#include "mem_map.h"
#include "dma_config.h"
#include "arm_internal.h"

#include "bf0_hal_mpi_ex.h"
#include "flash_table.h"

#include "sf32lb_flash.h"
#include "drv_io.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#if defined(CONFIG_BSP_USING_SPI_NAND) && !defined(CONFIG_BSP_DISABLE_BBM)
#  define BSP_USING_BBM 1
#  include "sifli_bbm.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SF32LB_NAND_TOTAL_SIZE         (128U * 1024U * 1024U)
#define SF32LB_NAND_CLK_DIV            (2)
#define SF32LB_NAND_PARENT_FMT         "/dev/config%d"

#ifndef CONFIG_BSP_QSPI2_MEM_SIZE
#  define CONFIG_BSP_QSPI2_MEM_SIZE    128
#endif

struct sf32lb_nand_dev_s
{
  struct mtd_dev_s mtd;
  uint32_t byte_offset;
  uint32_t byte_limit;
  uint32_t page_size;
  uint32_t erase_size;
  uint32_t neraseblocks;
  FAR FLASH_HandleTypeDef *hflash;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_nand_lock = NXMUTEX_INITIALIZER;

static QSPI_FLASH_CTX_T g_spi_nand_ctx;
static uint8_t *g_nand_page_buf;
static bool g_nand_initialized;
static uint32_t g_nand_pagesize;
static uint32_t g_nand_blksize;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sf32lb_nand_hw_init(void);
static int sf32lb_nand_sync_geometry(FAR struct sf32lb_nand_dev_s *priv);
static int sf32lb_nand_read_bytes(FAR struct sf32lb_nand_dev_s *priv,
                                  uint32_t byte_off, FAR uint8_t *buf,
                                  uint32_t len);
static int sf32lb_nand_write_bytes(FAR struct sf32lb_nand_dev_s *priv,
                                   uint32_t byte_off,
                                   FAR const uint8_t *buf, uint32_t len);
static int sf32lb_nand_erase_bytes(FAR struct sf32lb_nand_dev_s *priv,
                                   uint32_t byte_off, uint32_t len);

/****************************************************************************
 * BBM low-level port (override weak symbols in sifli_bbm.c)
 ****************************************************************************/

#if defined(BSP_USING_BBM)

static void sf32lb_bbm_log(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsyslog(LOG_INFO, fmt, ap);
  va_end(ap);
}

int port_read_page(int blk, int page, int offset, uint8_t *buff,
                   uint32_t size, uint8_t *spare, uint32_t spare_len)
{
  FAR FLASH_HandleTypeDef *hflash = &g_spi_nand_ctx.handle;
  uint32_t addr = (blk * g_nand_blksize) + (page * g_nand_pagesize) + offset;
  int ret;

  if (!g_nand_initialized)
    {
      return RET_ERROR;
    }

  up_invalidate_dcache((uintptr_t)hflash->base,
                       g_nand_pagesize + SPI_NAND_MAXOOB_SIZE);
  if (((addr & g_nand_blksize) != 0) && (hflash->wakeup != 0))
    {
      up_invalidate_dcache((uintptr_t)(hflash->base + (1 << 12)),
                           g_nand_pagesize + SPI_NAND_MAXOOB_SIZE);
    }

  ret = HAL_NAND_READ_WITHOOB(hflash, addr, buff, size, spare, spare_len);
  if (ret <= 0)
    {
      return RET_ERROR;
    }

  return ret;
}

int port_write_page(int blk, int page, uint8_t *data, uint8_t *spare,
                    uint32_t spare_len)
{
  FAR FLASH_HandleTypeDef *hflash = &g_spi_nand_ctx.handle;
  uint32_t addr = (blk * g_nand_blksize) + (page * g_nand_pagesize);
  int ret;

  if (!g_nand_initialized)
    {
      return RET_ERROR;
    }

  ret = HAL_NAND_WRITE_WITHOOB(hflash, addr, data, g_nand_pagesize,
                               spare, spare_len);
  if (ret <= 0)
    {
      if (hflash->ErrorCode == BBM_NAND_P_FAIL)
        {
          return RET_P_FAIL;
        }

      return RET_ERROR;
    }

  return ret;
}

int port_erase_block(int blk)
{
  FAR FLASH_HandleTypeDef *hflash = &g_spi_nand_ctx.handle;
  uint32_t addr = blk * g_nand_blksize;
  int ret;

  if (!g_nand_initialized)
    {
      return RET_ERROR;
    }

  ret = HAL_NAND_ERASE_BLK(hflash, addr);
  if (ret != HAL_OK)
    {
      if (hflash->ErrorCode == BBM_NAND_E_FAIL)
        {
          return RET_E_FAIL;
        }

      return RET_ERROR;
    }

  return RET_NOERROR;
}

int bbm_mark_bb(int blk)
{
  FAR FLASH_HandleTypeDef *hflash = &g_spi_nand_ctx.handle;
  int bad;
  int ret;

  if (!g_nand_initialized)
    {
      return RET_ERROR;
    }

  bad = HAL_NAND_GET_BADBLK(hflash, blk);
  if (bad != 0)
    {
      return RET_NOERROR;
    }

  ret = HAL_NAND_MARK_BADBLK(hflash, blk, 1);
  return (ret == HAL_OK) ? RET_NOERROR : RET_ERROR;
}

int bbm_get_bb(int blk)
{
  FAR FLASH_HandleTypeDef *hflash = &g_spi_nand_ctx.handle;

  if (!g_nand_initialized)
    {
      return 0;
    }

  return HAL_NAND_GET_BADBLK(hflash, blk);
}

#endif /* BSP_USING_BBM */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

extern int nand_read_id(FAR FLASH_HandleTypeDef *handle, uint8_t dummy);
extern int nand_clear_status(FAR FLASH_HandleTypeDef *handle);

/* NAND-only MPI init: do not call ROM/generic HAL_FLASH_Init (it can take the
 * NOR path and fault in spi_flash_get_size_by_id on PSRAM XIP boot). */

static HAL_StatusTypeDef sf32lb_nand_mpi_init(FAR QSPI_FLASH_CTX_T *ctx,
                                              FAR qspi_configure_t *cfg,
                                              FAR DMA_HandleTypeDef *dma,
                                              FAR struct dma_config *dma_cfg,
                                              uint16_t clk_div)
{
  FAR FLASH_HandleTypeDef *hflash = &ctx->handle;
  uint8_t fid;
  uint8_t did;
  uint8_t mtype;
  int size;
  uint32_t sta;

  if (ctx == NULL || cfg == NULL)
    {
      return HAL_ERROR;
    }

  HAL_QSPI_Init(hflash, cfg);

  ctx->flash_mode = cfg->SpiMode;
  ctx->base_addr = cfg->base;
  ctx->total_size = cfg->msize * 0x100000U;
  ctx->cache_flag = 2;

  hflash->dualFlash = 0;
  hflash->isNand = 1;
  hflash->dma = dma;

  if (hflash->dma != NULL && dma_cfg != NULL)
    {
      hflash->dma->Instance                 = dma_cfg->Instance;
      hflash->dma->Init.Request             = dma_cfg->request;
      hflash->dma->Init.Direction           = DMA_MEMORY_TO_PERIPH;
      hflash->dma->Init.PeriphInc           = DMA_PINC_DISABLE;
      hflash->dma->Init.MemInc              = DMA_MINC_ENABLE;
      hflash->dma->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
      hflash->dma->Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
      hflash->dma->Init.Mode                = DMA_NORMAL;
      hflash->dma->Init.Priority            = DMA_PRIORITY_MEDIUM;
      hflash->dma->Init.BurstSize           = 0;
      HAL_FLASH_SET_TXSLOT(hflash, 1);
    }

  hflash->freq = flash_get_freq(RCC_CLK_MOD_FLASH2, clk_div, 1);
  if (hflash->freq > FLASH_CLK_INVERT_THD)
    {
      HAL_QSPI_SET_CLK_INV(hflash, 1, 0);
    }
  else
    {
      HAL_QSPI_SET_CLK_INV(hflash, 0, 0);
    }

  HAL_FLASH_SET_CLK_rom(hflash, clk_div);
  HAL_FLASH_ENABLE_QSPI(hflash, 1);

  /* Bootloader already probed NAND; skip 0xFF chip reset from HAL_FLASH_Init. */

  ctx->dev_id = (uint32_t)nand_read_id(hflash, 8);
  fid = (uint8_t)(ctx->dev_id & 0xff);
  mtype = (uint8_t)((ctx->dev_id >> 8) & 0xff);
  did = (uint8_t)((ctx->dev_id >> 16) & 0xff);

  hflash->ctable = spi_nand_get_cmd_by_id(fid, did, mtype);
  if (hflash->ctable == NULL)
    {
      ctx->dev_id = (uint32_t)nand_read_id(hflash, 0xf);
      fid = (uint8_t)(ctx->dev_id & 0xff);
      mtype = (uint8_t)((ctx->dev_id >> 8) & 0xff);
      did = (uint8_t)((ctx->dev_id >> 16) & 0xff);
      hflash->ctable = spi_nand_get_cmd_by_id(fid, did, mtype);
      if (hflash->ctable == NULL)
        {
          hflash->ctable = spi_nand_get_default_ctable();
        }
    }

  if (hflash->ctable == NULL)
    {
      HAL_FLASH_ENABLE_QSPI(hflash, 0);
      ctx->base_addr = 0;
      ctx->total_size = 0;
      return HAL_ERROR;
    }

  size = spi_nand_get_size_by_id(fid, did, mtype);
  hflash->wakeup = spi_nand_get_plane_select_flag(fid, did, mtype);
  hflash->dualFlash = spi_nand_get_big_page_flag(fid, did, mtype);
  hflash->dualFlash |= spi_nand_get_ecc_mode(fid, did, mtype) << 4;

  if (size != 0)
    {
      ctx->total_size = (uint32_t)size;
      hflash->size = (uint32_t)size;
    }

  MODIFY_REG(hflash->Instance->DCR, MPI_DCR_CSHMIN, 0xf << MPI_DCR_CSHMIN_Pos);

  do
    {
      HAL_FLASH_WRITE_DLEN(hflash, 1);
      HAL_FLASH_ISSUE_CMD(hflash, SPI_FLASH_CMD_RDSR,
                          hflash->ctable->status_reg);
      sta = HAL_FLASH_READ32(hflash);
      HAL_Delay_us(10);
    }
  while ((sta & 0x1) != 0);

  nand_clear_status(hflash);

  if (hflash->Mode == HAL_FLASH_QMODE)
    {
      HAL_NAND_EN_QUAL(hflash, 1);
    }

  return HAL_OK;
}

static int sf32lb_nand_hw_init(void)
{
  HAL_StatusTypeDef status;
  qspi_configure_t flash_cfg;
  FAR FLASH_HandleTypeDef *hflash;

  if (g_nand_initialized)
    {
      return OK;
    }

  memset(&g_spi_nand_ctx, 0, sizeof(g_spi_nand_ctx));
  memset(&flash_cfg, 0, sizeof(flash_cfg));

  flash_cfg.Instance = FLASH2;
  flash_cfg.line = 2;
  flash_cfg.base = HCPU_MPI_SBUS_ADDR(FLASH2_BASE_ADDR);
  flash_cfg.msize = CONFIG_BSP_QSPI2_MEM_SIZE;
  flash_cfg.SpiMode = SPI_MODE_NAND;

  g_spi_nand_ctx.handle.Instance = FLASH2;
  g_spi_nand_ctx.handle.base = flash_cfg.base;
  g_spi_nand_ctx.handle.size = CONFIG_BSP_QSPI2_MEM_SIZE * 1024U * 1024U;
  g_spi_nand_ctx.handle.freq = 24000000;
  g_spi_nand_ctx.handle.buf_mode = 1;

  /* git 204f767: FLASH2 on SYSCLK (~48M), div2 → ~24MHz NAND for init + BBM */
  BSP_Board_EnableNandMpiClocks();

  /* PSRAM XIP + BBM: use polled/FIFO MPI path (no FLASH2 DMA). Dynamic DMA
   * channel alloc + HAL_DMA_DeInit during BBM scan faulted on this board. */
  status = sf32lb_nand_mpi_init(&g_spi_nand_ctx, &flash_cfg,
                                NULL, NULL, SF32LB_NAND_CLK_DIV);

  if (status != HAL_OK)
    {
      syslog(LOG_ERR, "ERROR: NAND mpi_init failed: %d (dev_id=0x%06lx)\n",
             status, (unsigned long)g_spi_nand_ctx.dev_id);
      return -EIO;
    }

  syslog(LOG_INFO, "NAND dev_id=0x%06lx page=%lu blk=%lu\n",
         (unsigned long)g_spi_nand_ctx.dev_id,
         (unsigned long)HAL_NAND_PAGE_SIZE(&g_spi_nand_ctx.handle),
         (unsigned long)HAL_NAND_BLOCK_SIZE(&g_spi_nand_ctx.handle));

  hflash = &g_spi_nand_ctx.handle;
  g_nand_pagesize = HAL_NAND_PAGE_SIZE(hflash);
  g_nand_blksize = HAL_NAND_BLOCK_SIZE(hflash);

  if (g_nand_page_buf == NULL)
    {
      g_nand_page_buf = kmm_malloc(g_nand_pagesize + SPI_NAND_MAXOOB_SIZE);
      if (g_nand_page_buf == NULL)
        {
          return -ENOMEM;
        }
    }

  hflash->data_buf = g_nand_page_buf;
  HAL_NAND_CONF_ECC(hflash, 1);

  /* Mark the NAND ready BEFORE BBM init: sif_bbm_init() scans the device via
   * port_read_page(), which early-returns RET_ERROR while g_nand_initialized
   * is false. If left false here, every BBM probe read "fails", the BBM layer
   * finds no table and cannot build one (detect result -1 -> BBM_ASSERT). */
  g_nand_initialized = true;

#if defined(BSP_USING_BBM)
  syslog(LOG_INFO, "NAND: BBM init (%lu MB)...\n",
         (unsigned long)(g_spi_nand_ctx.total_size >> 20));
  bbm_register_log(sf32lb_bbm_log);
  bbm_set_page_size(g_nand_pagesize);
  bbm_set_blk_size(g_nand_blksize);
  if (sif_bbm_init(g_spi_nand_ctx.total_size, g_nand_page_buf) != 0)
    {
      syslog(LOG_ERR, "ERROR: NAND BBM init failed\n");
      g_nand_initialized = false;
      return -EIO;
    }
#endif

  syslog(LOG_INFO, "INFO: NAND initialized id=0x%x pagesize=%lu blksize=%lu\n",
         (unsigned)g_spi_nand_ctx.dev_id,
         (unsigned long)g_nand_pagesize,
         (unsigned long)g_nand_blksize);
  return OK;
}

static int sf32lb_nand_sync_geometry(FAR struct sf32lb_nand_dev_s *priv)
{
  uint32_t flash_size;
  uint32_t region_size;

  if (priv == NULL || priv->hflash == NULL)
    {
      return -EINVAL;
    }

  flash_size = priv->hflash->size;
  if (flash_size == 0)
    {
      flash_size = SF32LB_NAND_TOTAL_SIZE;
    }

#if defined(BSP_USING_BBM)
  /* bbm_get_total() returns the size of the BBM *backup/reserved* region
   * (bkup_blk * blk_size), NOT the usable size. The user-accessible NAND is
   * the total minus that reserved region (matches the SDK's
   * rt_nand_get_total_size(): total_size - bbm_get_total()). Using the raw
   * bbm_get_total() here made flash_size only ~4MB, so the ~8.6MB FS offset
   * tripped "byte_offset >= flash_size" and NAND init returned NULL. */
  {
    uint32_t bbm_reserved = (uint32_t)bbm_get_total();
    if (bbm_reserved < flash_size)
      {
        flash_size -= bbm_reserved;
      }
  }
#endif

  priv->page_size = g_nand_pagesize;
  priv->erase_size = g_nand_blksize;
  if (priv->page_size == 0 || priv->erase_size == 0)
    {
      return -EIO;
    }

  if (priv->byte_offset >= flash_size)
    {
      return -EINVAL;
    }

  region_size = flash_size - priv->byte_offset;
  if (priv->byte_limit > 0 && priv->byte_limit < region_size)
    {
      region_size = priv->byte_limit;
    }

  priv->neraseblocks = region_size / priv->erase_size;
  return OK;
}

static int sf32lb_nand_read_bytes(FAR struct sf32lb_nand_dev_s *priv,
                                  uint32_t byte_off, FAR uint8_t *buf,
                                  uint32_t len)
{
  FAR FLASH_HandleTypeDef *hflash = priv->hflash;
  uint32_t page_size = priv->page_size;
  uint32_t done = 0;
  int ret;

  if (byte_off >= hflash->base)
    {
      byte_off -= hflash->base;
    }

  while (done < len)
    {
      uint32_t page_off = byte_off & (page_size - 1);
      uint32_t chunk = page_size - page_off;

      if (chunk > (len - done))
        {
          chunk = len - done;
        }

      nxmutex_lock(&g_nand_lock);

#if defined(BSP_USING_BBM)
      {
        int blk = byte_off / g_nand_blksize;
        int page = (byte_off / page_size) &
                   ((g_nand_blksize / page_size) - 1);
        int offset = byte_off & (page_size - 1);

        ret = bbm_read_page(blk, page, offset, buf + done, chunk, NULL, 0);
      }
#else
      ret = HAL_NAND_READ_WITHOOB(hflash, byte_off, buf + done, chunk,
                                  NULL, 0);
#endif

      nxmutex_unlock(&g_nand_lock);
      if (ret <= 0)
        {
          return -EIO;
        }

      done += chunk;
      byte_off += chunk;
    }

  return OK;
}

static int sf32lb_nand_write_bytes(FAR struct sf32lb_nand_dev_s *priv,
                                   uint32_t byte_off,
                                   FAR const uint8_t *buf, uint32_t len)
{
  FAR FLASH_HandleTypeDef *hflash = priv->hflash;
  uint32_t page_size = priv->page_size;
  uint32_t done = 0;
  int ret;

  if (byte_off >= hflash->base)
    {
      byte_off -= hflash->base;
    }

  if ((byte_off & (page_size - 1)) != 0 || (len & (page_size - 1)) != 0)
    {
      return -EINVAL;
    }

  while (done < len)
    {
      nxmutex_lock(&g_nand_lock);

#if defined(BSP_USING_BBM)
      {
        int blk = byte_off / g_nand_blksize;
        int page = (byte_off / page_size) &
                   ((g_nand_blksize / page_size) - 1);

        ret = bbm_write_page(blk, page, (uint8_t *)(buf + done), NULL, 0);
      }
#else
      ret = HAL_NAND_WRITE_WITHOOB(hflash, byte_off + done,
                                   buf + done, page_size, NULL, 0);
#endif

      nxmutex_unlock(&g_nand_lock);
      if (ret <= 0)
        {
          return -EIO;
        }

      done += page_size;
      byte_off += page_size;
    }

  return OK;
}

static int sf32lb_nand_erase_bytes(FAR struct sf32lb_nand_dev_s *priv,
                                   uint32_t byte_off, uint32_t len)
{
  FAR FLASH_HandleTypeDef *hflash = priv->hflash;
  uint32_t erase_size = priv->erase_size;
  int ret;

  if (byte_off >= hflash->base)
    {
      byte_off -= hflash->base;
    }

  if ((byte_off & (erase_size - 1)) != 0 || (len & (erase_size - 1)) != 0)
    {
      return -EINVAL;
    }

  while (len > 0)
    {
      nxmutex_lock(&g_nand_lock);

#if defined(BSP_USING_BBM)
      ret = bbm_erase_block(byte_off / erase_size);
      if (ret == RET_NOERROR)
        {
          ret = OK;
        }
      else
        {
          ret = -EIO;
        }
#else
      ret = HAL_NAND_ERASE_BLK(hflash, byte_off);
      if (ret != HAL_OK)
        {
          ret = -EIO;
        }
      else
        {
          ret = OK;
        }
#endif

      nxmutex_unlock(&g_nand_lock);
      if (ret < 0)
        {
          return ret;
        }

      byte_off += erase_size;
      len -= erase_size;
    }

  return OK;
}

/****************************************************************************
 * MTD Callbacks
 ****************************************************************************/

static ssize_t sf32lb_nand_bread(FAR struct mtd_dev_s *dev, off_t startblock,
                                 size_t nblocks, FAR uint8_t *buffer)
{
  FAR struct sf32lb_nand_dev_s *priv = (FAR struct sf32lb_nand_dev_s *)dev;
  uint32_t byte_off;
  uint32_t nbytes;
  int ret;

  if (startblock < 0)
    {
      return -EINVAL;
    }

  byte_off = priv->byte_offset + (uint32_t)startblock * priv->page_size;
  nbytes = nblocks * priv->page_size;
  ret = sf32lb_nand_read_bytes(priv, byte_off, buffer, nbytes);
  if (ret < 0)
    {
      return ret;
    }

  return (ssize_t)nblocks;
}

static ssize_t sf32lb_nand_bwrite(FAR struct mtd_dev_s *dev, off_t startblock,
                                  size_t nblocks, FAR const uint8_t *buffer)
{
  FAR struct sf32lb_nand_dev_s *priv = (FAR struct sf32lb_nand_dev_s *)dev;
  uint32_t byte_off;
  uint32_t nbytes;
  int ret;

  if (startblock < 0)
    {
      return -EINVAL;
    }

  byte_off = priv->byte_offset + (uint32_t)startblock * priv->page_size;
  nbytes = nblocks * priv->page_size;
  ret = sf32lb_nand_write_bytes(priv, byte_off, buffer, nbytes);
  if (ret < 0)
    {
      return ret;
    }

  return (ssize_t)nblocks;
}

static int sf32lb_nand_erase(FAR struct mtd_dev_s *dev, off_t startblock,
                             size_t nblocks)
{
  FAR struct sf32lb_nand_dev_s *priv = (FAR struct sf32lb_nand_dev_s *)dev;
  uint32_t byte_off;
  uint32_t nbytes;
  int ret;

  if (startblock < 0)
    {
      return -EINVAL;
    }

  byte_off = priv->byte_offset + (uint32_t)startblock * priv->erase_size;
  nbytes = nblocks * priv->erase_size;
  ret = sf32lb_nand_erase_bytes(priv, byte_off, nbytes);
  if (ret < 0)
    {
      return ret;
    }

  return (int)nblocks;
}

static int sf32lb_nand_ioctl(FAR struct mtd_dev_s *dev, int cmd,
                             unsigned long arg)
{
  FAR struct sf32lb_nand_dev_s *priv = (FAR struct sf32lb_nand_dev_s *)dev;
  int ret = -EINVAL;

  switch (cmd)
    {
      case MTDIOC_GEOMETRY:
        {
          FAR struct mtd_geometry_s *geo =
            (FAR struct mtd_geometry_s *)((uintptr_t)arg);

          if (geo != NULL)
            {
              ret = sf32lb_nand_sync_geometry(priv);
              if (ret < 0)
                {
                  break;
                }

              memset(geo, 0, sizeof(*geo));
              geo->blocksize = priv->page_size;
              geo->erasesize = priv->erase_size;
              geo->neraseblocks = priv->neraseblocks;
              ret = OK;
            }
        }
        break;

      case BIOC_PARTINFO:
        {
          FAR struct partition_info_s *info =
            (FAR struct partition_info_s *)arg;

          if (info != NULL)
            {
              ret = sf32lb_nand_sync_geometry(priv);
              if (ret < 0)
                {
                  break;
                }

              info->numsectors = priv->byte_limit > 0 ?
                                 priv->byte_limit / priv->page_size :
                                 (priv->neraseblocks * priv->erase_size /
                                  priv->page_size);
              info->sectorsize = priv->page_size;
              info->startsector = priv->byte_offset / priv->page_size;
              info->parent[0] = '\0';
              ret = OK;
            }
        }
        break;

      case MTDIOC_ERASESTATE:
        {
          FAR uint8_t *result = (FAR uint8_t *)arg;
          *result = 0xff;
          ret = OK;
        }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

static int sf32lb_nand_isbad(FAR struct mtd_dev_s *dev, off_t block)
{
#if defined(BSP_USING_BBM)
  UNUSED(dev);
  return bbm_get_bb((int)block) ? 1 : 0;
#else
  UNUSED(dev);
  UNUSED(block);
  return 0;
#endif
}

static int sf32lb_nand_markbad(FAR struct mtd_dev_s *dev, off_t block)
{
#if defined(BSP_USING_BBM)
  UNUSED(dev);
  return (bbm_mark_bb((int)block) == RET_NOERROR) ? OK : -EIO;
#else
  UNUSED(dev);
  UNUSED(block);
  return -EOPNOTSUPP;
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct mtd_dev_s *sf32lb_nand_initialize(uint32_t byte_offset,
                                             uint32_t byte_size)
{
  FAR struct sf32lb_nand_dev_s *priv;
  int ret;

  priv = kmm_zalloc(sizeof(struct sf32lb_nand_dev_s));
  if (priv == NULL)
    {
      return NULL;
    }

  ret = sf32lb_nand_hw_init();
  if (ret < 0)
    {
      kmm_free(priv);
      return NULL;
    }

  priv->hflash = &g_spi_nand_ctx.handle;
  priv->byte_offset = byte_offset;
  priv->byte_limit = byte_size;

  priv->mtd.erase = sf32lb_nand_erase;
  priv->mtd.bread = sf32lb_nand_bread;
  priv->mtd.bwrite = sf32lb_nand_bwrite;
  priv->mtd.ioctl = sf32lb_nand_ioctl;
  priv->mtd.isbad = sf32lb_nand_isbad;
  priv->mtd.markbad = sf32lb_nand_markbad;
  priv->mtd.name = "config";

  ret = sf32lb_nand_sync_geometry(priv);
  if (ret < 0)
    {
      kmm_free(priv);
      return NULL;
    }

  return (FAR struct mtd_dev_s *)priv;
}

int sf32lb_nand_automount(int minor, uint32_t byte_offset, uint32_t byte_size)
{
  FAR struct mtd_dev_s *mtd;
  static bool initialized;
  char devname[16];
  int ret;

  snprintf(devname, sizeof(devname), SF32LB_NAND_PARENT_FMT, minor);

  if (!initialized)
    {
      mtd = sf32lb_nand_initialize(byte_offset, byte_size);
      if (mtd == NULL)
        {
          syslog(LOG_ERR, "ERROR: Failed to initialize NAND flash\n");
          return -ENODEV;
        }

      ret = register_mtddriver(devname, mtd, 0, mtd);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: register_mtddriver(%s) failed: %d\n",
                 devname, ret);
          return ret;
        }

      initialized = true;
      syslog(LOG_INFO,
             "INFO: NAND MTD registered at %s (off=0x%lx size=0x%lx)\n",
             devname,
             (unsigned long)byte_offset,
             (unsigned long)byte_size);
    }

  return OK;
}

int sf32lb_nand_mount_littlefs(int minor)
{
#ifndef CONFIG_DISABLE_MOUNTPOINT
  char devname[16];
  struct statfs sfs;
  int ret;

  snprintf(devname, sizeof(devname), SF32LB_NAND_PARENT_FMT, minor);

  if (statfs("/mnt/lfs", &sfs) == 0)
    {
      return OK;
    }

  ret = mkdir("/mnt", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(/mnt) failed: %d\n", errno);
      return -errno;
    }

  ret = mkdir("/mnt/lfs", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(/mnt/lfs) failed: %d\n", errno);
      return -errno;
    }

  /* Mount pre-flashed fs_root.bin WITHOUT autoformat (same as sf32lb_sdio.c):
   * do not silently reformat on mount failure — run build-fs + flash-fs instead.
   */

  ret = mount(devname, "/mnt/lfs", "littlefs", 0, NULL);
  if (ret < 0)
    {
      if (errno == EBUSY || errno == ENOTDIR || errno == EEXIST)
        {
          if (statfs("/mnt/lfs", &sfs) == 0)
            {
              return OK;
            }
        }

      syslog(LOG_ERR, "ERROR: mount(%s, /mnt/lfs, littlefs) failed: %d\n",
             devname, errno);
      return -errno;
    }

  syslog(LOG_INFO, "INFO: LittleFS mounted %s -> /mnt/lfs\n", devname);
#endif

  return OK;
}
