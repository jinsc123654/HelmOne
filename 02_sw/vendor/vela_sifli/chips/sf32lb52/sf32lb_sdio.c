/**
 * @file sf32lb_sdio.c
 * @brief SF32LB52 SD/eMMC（SDIO1 / SD1）寄存器级驱动，导出为 NuttX MTD 窗口。
 *
 * 两个独立 LittleFS + 一个 FAT 卷，不共用 superblock：
 * - `/dev/sd0`       → `/mnt/lfs`（FS_REGION，MTP / BLE / GPX）
 * - `/dev/sdkv`      → `/mnt/kv`（KV_REGION，VELA persist）
 * - `/dev/mtdblock0` → `/mnt/fat`（FAT_REGION；产品只读，工厂读写；根下 map/ fonts/）
 *
 * KV 访存优先占用 SD 总线：有 KV 等待者时，非 KV 传输会让出控制器锁，
 * 因此 MTP/BLE 的 EBUSY 不会作用到 `/mnt/kv`。
 *
 * SD1 命令/数据路径（对齐 SiFli `drv_sdio.c`）：
 * - 识别：板级 bootloader（CMD0…ACMD6，ITIMING_SEL）。
 * - DMA：DMAC1 CH3 / REQUEST_57 常驻（KeepAlloc），bounce `g_sd_mblkbuf`。
 *   读写共用这一路，只切 Direction，不释放。FIFO 撑不住多块，CMD18/CMD25
 *   必须走 DMA。
 * - 读：DATA_START 在 CMD 之前，DMA FIFO→内存，CMD17/18，多块后 CMD12。
 * - 出错：先停 DMA、CMD12 停传输、等 DAT0，再用 CMD13 看卡状态。
 *   回到 TRAN 就重试；只有已经 idle 才允许 CMD0/CMD8 全量识别。
 *   卡停在 data 时发 CMD8 会把一次读失败放大成整卡僵死。
 * - 写：DMA 内存→FIFO 先启动，发 CMD24/25，**CMD 应答后再 DATA_START**
 *   （官方 IRQ 里 CMDREND 才开数据相；提前开会把卡留在 data 态）。
 *   写完等 DAT0。DMA 失败再回退 PIO CMD24。
 *
 * 在 `board_late` 把 HCPU 拉到 240 MHz **之前** 跑，时钟与 bootloader 相同（~144 MHz），
 * 分频可沿用。FAT_REGION `max_size=0` 表示卡余量：identify 解析 CSD，
 * 窗口 = `card_bytes - FAT 偏移`，再按 erase 对齐。`/mnt/lfs` 不 autoformat
 * （损坏时拒绝挂载，避免把轨迹格式化掉）；`/mnt/kv` 空白切片仍 autoformat。
 * 卡地址用 64 位字节（SDHC CMD 参数 = byte>>9）。
 *
 * 分区布局见 `docs/sd_partition.md`。
 *
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <nuttx/config.h>

#include <sys/types.h>

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/cache.h>
#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/mutex.h>
#include <nuttx/atomic.h>

#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

/* register.h pulls in sd.h (hwp_sdmmc1 + SD_* register macros), the pin name
 * enums (PAD_PA*, SD1_*) and bf0_hal.h (HAL_PIN_Set / HAL_Delay_us). The NAND
 * driver includes it the same way and compiles cleanly under NuttX.
 */

#include "register.h"
#include "dma_config.h"
#include "sf32lb_sdio.h"
#include "myvendor_watchdog.h"

#ifdef CONFIG_MTD

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SD_SUCCESS                 0
#define SD_TIMEOUT                 1
#define SD_CRCERR                  2

#define SD_HW_BLOCK_SIZE           512U

/* LittleFS geometry — keep identical to scripts/mklfs_disk/mklfs_disk.c.
 *
 * page_size becomes LittleFS read/prog size and erase_size its block size.
 * Unlike NAND these are free parameters (an SD card programs 512 B sectors and
 * needs no erase), and they dominate mount time: LittleFS pads every metadata
 * commit up to prog_size, and fetching a metadata dir scans its whole block
 * forwards (lfs_dir_fetchmatch) and then backwards (lfs_dir_getslice). With
 * 2048/131072 the rootfs mount had to read 5.4 MB; 512/4096 brings the same
 * tree down to ~400 KB and also stops small files from wasting a 128 KiB block.
 */

#define SF32LB_SD_PAGE_SIZE        512U      /* read/prog size  */
#define SF32LB_SD_ERASE_SIZE       4096U     /* lfs block size  */

/* 0 = use the full remaining-card window (CSD capacity - FS offset).
 * Empty volumes allocate fine at this size; the old stall was
 * lfs_alloc() walking a packed tree (vmap/images/...), not block_count.
 * A non-zero cap must match the on-disk superblock or mount returns EINVAL.
 */

#ifndef SF32LB_SD_LFS_MAX_BYTES
#  define SF32LB_SD_LFS_MAX_BYTES  0
#endif

/* Match the bootloader (sd_nand_ops.c): 24 MHz, 4-wire, VOID_FIFO_ERROR.
 * Boot hard-codes CLKCR DIV=5 at 144 MHz HCLK; here the divider is derived
 * from the current HCLK so it stays 24 MHz after bringup raises HCPU.
 */

#define SF32LB_SD_ID_CLOCK_HZ      400000U
#define SF32LB_SD_TRAN_CLOCK_HZ    24000000U

#define SF32LB_SD_PARENT_FMT       "/dev/sd%d"
#define SF32LB_SD_MOUNTPOINT       "/mnt/lfs"
#define SF32LB_SD_KV_DEV           "/dev/sdkv"
#define SF32LB_SD_KV_MOUNTPOINT    "/mnt/kv"
#define SF32LB_SD_FAT_DEV          "/dev/sdfat"
#define SF32LB_SD_FAT_BLK          "/dev/mtdblock0"
#define SF32LB_SD_FAT_MOUNTPOINT   "/mnt/fat"

/* Transient SD CRC / busy can look like LFS_ERR_CORRUPT. Retry mount only;
 * never autoformat /mnt/lfs (that wipes rides).
 */

#define SF32LB_SD_LFS_MOUNT_TRIES  3
#define SF32LB_SD_LFS_RETRY_MS     100u

/* 0: 1-wire, 1: 4-wire (matches bootloader). */

#define SF32LB_SD_WIRE_MODE        1

/* One LittleFS erase (8 x 512 B).  Matches RT-Thread max useful host
 * segment for this geometry; larger MTD_BREAD is split into chunks.
 */

#define SF32LB_SD_CMD18_MAX_BLOCKS  8u

/* Set to 1 to account SD I/O (op counts + microseconds) and run a raw
 * throughput bench at automount. Used to tell apart "littlefs reads too much"
 * from "each SD access is too slow" when profiling the rootfs mount.
 */

#ifndef SF32LB_SD_PROFILE
#  define SF32LB_SD_PROFILE        1
#endif

/**
 * @brief 一张卡上的一个 MTD 窗口（FS 或 KV）。
 */
struct sf32lb_sd_dev_s
{
  struct mtd_dev_s mtd;     /**< NuttX MTD 接口。 */
  uint32_t byte_offset;     /**< 窗口在卡上的字节偏移。 */
  uint64_t window_bytes;    /**< 物理窗口（ptab 或 CSD 余量）。 */
  uint64_t byte_limit;      /**< 逻辑 LittleFS 大小（可被 limit 缩小）。 */
  uint32_t page_size;       /**< `SF32LB_SD_PAGE_SIZE`（512）。 */
  uint32_t erase_size;      /**< `SF32LB_SD_ERASE_SIZE`（4096）。 */
  uint32_t neraseblocks;    /**< `window_bytes / erase_size`。 */
  bool     kv_prio;         /**< true：此窗口占用 SD 锁时优先于 MTP/BLE。 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_sd_lock = NXMUTEX_INITIALIZER;
static bool    g_sd_initialized;
static bool    g_sd_card_ready;
static volatile bool g_sd_baremetal;

static void sdio_syslog(int prio, FAR const IPTR char *fmt, ...)
{
  va_list ap;

  if (g_sd_baremetal)
    {
      return;
    }

  va_start(ap, fmt);
  vsyslog(prio, fmt, ap);
  va_end(ap);
}

#undef syslog
#define syslog sdio_syslog
static atomic_t g_sd_kv_waiters;
static FAR struct mtd_dev_s *g_sd_kv_mtd;
static uint8_t g_sd_sdsc = 1;   /* 1: SDSC byte-addressed, 0: SDHC/SDXC block */
static uint64_t g_sd_card_bytes; /* CSD capacity; 0 if unknown            */
static uint16_t g_sd_rca;
static uint32_t g_sd_bus_hz = SF32LB_SD_TRAN_CLOCK_HZ;
static FAR struct mtd_dev_s *g_sd_mtd;
static FAR struct mtd_dev_s *g_sd_fat_mtd;
static bool    g_sd_dead;       /* 4-try + reinit still failing; fast-EIO */
static bool    g_sd_reinit_used;
static bool    g_sd_dat0_poll;  /* DSR bit0 tracks DAT0 after identify    */
static uint32_t g_sd_last_sr;   /* SR (or R1) at the last cmd/data error  */

/* /mnt/lfs 是否是只读挂上的（读写挂载失败后的降级结果）。见
 * sf32lb_sd_lfs_readonly()：写盘的用户据此跳过，别反复撞 -EACCES。 */
static bool g_lfs_readonly;

/* 判死/复活状态。见 sd1_dead_fail_fast() 与 sd1_hard_reset()。 */
static uint16_t g_sd_dead_mute;      /* 连续多少次重识别失败（mute）      */
static uint64_t g_sd_dead_since_us;  /* 判死时刻                          */
static uint64_t g_sd_dead_try_us;    /* 上次硬复位尝试时刻                */
static uint32_t g_sd_dead_tries;     /* 硬复位尝试次数（日志用，不回绕）  */

/* Staging buffer keeps SD FIFO 32-bit accesses word-aligned regardless of the
 * caller buffer alignment.
 */

static uint32_t g_sd_blkbuf[SD_HW_BLOCK_SIZE / 4];

/* RT-Thread drv_sdio.c uses a cache-aligned bounce so DMA never writes a
 * caller buffer that might be unaligned or share a D-cache line.
 */

static uint32_t g_sd_mblkbuf[SF32LB_SD_CMD18_MAX_BLOCKS *
                             (SD_HW_BLOCK_SIZE / 4)] aligned_data(32);
static DMA_HandleTypeDef g_sd_dmah;
static bool g_sd_dma = true;    /* false if DMA1 init failed (PIO only) */

/* Always-on error diagnostics (rare paths).  These tell whether an MTP read
 * failure lines up with a real SD hardware timeout/CRC, or is purely a
 * filesystem/protocol issue with the card behaving fine.
 */

struct sf32lb_sd_diag_s
{
  uint32_t cmd_timeout;   /* command wait hit the wall-clock backstop */
  uint32_t rd_timeout;    /* read data wait: backstop or DATA_TIMEOUT   */
  uint32_t rd_crc;        /* read data CRC error                        */
  uint32_t wr_timeout;    /* write data wait: backstop or DATA_TIMEOUT  */
  uint32_t wr_crc;        /* write data CRC error                       */
  uint32_t dat0_to;       /* DAT0 (card programming busy) wait timeout  */
  uint32_t rd_retry;      /* single-block read attempts that failed     */
  uint32_t wr_retry;      /* single-block write attempts that failed    */
  uint32_t rd_fail;       /* reads that gave up after all retries       */
  uint32_t wr_fail;       /* writes that gave up after all retries      */
  uint32_t cmd18_ok;      /* CMD18+DMA multi-block reads                  */
  uint32_t cmd18_fail;    /* CMD18 attempts that fell back to PIO         */
  uint32_t cmd25_ok;      /* CMD25+DMA multi-block writes                 */
  uint32_t cmd25_fail;    /* CMD25 attempts that fell back to PIO         */
};

static struct sf32lb_sd_diag_s g_sd_diag;

/**
 * @brief 连续失败次数（饱和，不回绕）。
 *
 * @details 区分"概率性抖动"与"永久性损坏"的关键指标：每次失败递增、任何一次
 * 成功操作（走 sd1_restore_tran_clock）归零。饱和到 0xffff 就不再增 —— 回绕到
 * 0 会被误读成"已恢复"，恰好掩盖永久故障。
 */
static uint16_t g_sd_fail_run;

void sf32lb_sd_diag_dump(const char *tag)
{
  const struct sf32lb_sd_diag_s *d = &g_sd_diag;

  syslog(LOG_INFO,
         "SDDIAG: %s cmd_to=%lu rd_to=%lu rd_crc=%lu wr_to=%lu wr_crc=%lu "
         "dat0_to=%lu rd_retry=%lu wr_retry=%lu rd_fail=%lu wr_fail=%lu "
         "cmd18_ok=%lu cmd18_fail=%lu cmd25_ok=%lu cmd25_fail=%lu\n",
         tag ? tag : "",
         (unsigned long)d->cmd_timeout, (unsigned long)d->rd_timeout,
         (unsigned long)d->rd_crc, (unsigned long)d->wr_timeout,
         (unsigned long)d->wr_crc, (unsigned long)d->dat0_to,
         (unsigned long)d->rd_retry,
         (unsigned long)d->wr_retry, (unsigned long)d->rd_fail,
         (unsigned long)d->wr_fail,
         (unsigned long)d->cmd18_ok, (unsigned long)d->cmd18_fail,
         (unsigned long)d->cmd25_ok, (unsigned long)d->cmd25_fail);
}

/****************************************************************************
 * I/O accounting
 ****************************************************************************/

#if SF32LB_SD_PROFILE

struct sf32lb_sd_stats_s
{
  uint32_t bread;       /* MTD_BREAD calls              */
  uint32_t bread_pages; /* page_size units handed to it */
  uint32_t bwrite;
  uint32_t bwrite_pages;
  uint32_t erase_blocks;
  uint32_t hw_read;     /* CMD17/CMD18 512 B units        */
  uint32_t hw_write;    /* CMD24/CMD25 512 B units         */
  uint64_t hw_read_us;
  uint64_t hw_write_us;
};

static struct sf32lb_sd_stats_s g_sd_stats;

static uint64_t sf32lb_sd_now_us(void)
{
  struct timespec ts;
  uint32_t hclk;
  uint32_t mhz;

  /* 裸机 PRIMASK=1，up_timer_gettime / SysTick 计数会停或抢锁。 */
  if (g_sd_baremetal)
    {
      hclk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
      mhz = (hclk >= 1000000u) ? (hclk / 1000000u) : 240u;
      return (uint64_t)((*(volatile uint32_t *)0xe0001004u) / mhz);
    }

  clock_systime_timespec(&ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static void sf32lb_sd_stats_dump(FAR const char *tag)
{
  FAR const struct sf32lb_sd_stats_s *s = &g_sd_stats;

  syslog(LOG_INFO,
         "SDPROF: %s bread=%lu(%lu pg) hw_rd=%lu blk %llu us "
         "bwrite=%lu(%lu pg) erase=%lu blk hw_wr=%lu blk %llu us\n",
         tag,
         (unsigned long)s->bread, (unsigned long)s->bread_pages,
         (unsigned long)s->hw_read, (unsigned long long)s->hw_read_us,
         (unsigned long)s->bwrite, (unsigned long)s->bwrite_pages,
         (unsigned long)s->erase_blocks,
         (unsigned long)s->hw_write, (unsigned long long)s->hw_write_us);
}

#else
#  define sf32lb_sd_stats_dump(tag)
#endif

/****************************************************************************
 * SD1 controller primitives (ported from bootloader sd_nand_drv.c)
 ****************************************************************************/

/* SD1 is fed by HCLK and divides by (DIV + 1). */

static void sd1_set_clock(uint32_t hz)
{
  uint32_t hclk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
  uint32_t div;

  if (hclk == 0)
    {
      hclk = 144000000u;
    }

  div = (hclk + hz - 1) / hz;
  if (div == 0)
    {
      div = 1;
    }

  if (div > 0x2000u)
    {
      div = 0x2000u;
    }

  /* 改分频前必须先停 SDCLK：运行中直接写 DIV 会产出残脉冲。DVFS 每次跳档
   * 都会调到这里（sf32lb_sd_reclock），而星历注入那种"长时间连续小 read"
   * 正好让卡一直处在传输边缘，撞上一次就锁死。STOP_CLK=1 才是停钟，
   * 见 HAL_SDMMC_CLK_SET()：en=0 时 clk |= SD_CLKCR_STOP_CLK。
   */
  hwp_sdmmc1->CLKCR = SD_CLKCR_STOP_CLK;
  HAL_Delay_us(10);
  hwp_sdmmc1->CLKCR = ((div - 1) << SD_CLKCR_DIV_Pos) | SD_CLKCR_VOID_FIFO_ERROR | SD_CLKCR_STOP_CLK;
  HAL_Delay_us(10);
  hwp_sdmmc1->CLKCR &= ~SD_CLKCR_STOP_CLK;
}

static void sd1_restore_tran_clock(void)
{
  /* "恢复正常"的唯一出口：任何一次成功操作都把连续失败计数清零，
   * 这样日志里的 run= 就能区分抖动与永久故障。 */
  g_sd_fail_run = 0;

  if (g_sd_bus_hz != SF32LB_SD_TRAN_CLOCK_HZ)
    {
      sd1_set_clock(SF32LB_SD_TRAN_CLOCK_HZ);
      g_sd_bus_hz = SF32LB_SD_TRAN_CLOCK_HZ;
    }
}

static void sd1_hw_quiet(void);

static void sd1_init(void)
{
  /* 冷启动时 ENR2 还没使能，寄存器访问无意义，也没有"正在移位"的时钟要收尾。
   * 只有运行期重识别（ENR2 已使能）才需要先按序停机。
   */
  if ((hwp_hpsys_rcc->ENR2 & HPSYS_RCC_ENR2_SDMMC1) != 0)
    {
      sd1_hw_quiet();
    }

  /* ── 控制器模块级复位：对齐官方驱动 ──────────────────────────────────
   * 官方驱动**每次超时**都做这一条：
   *   drv_sdio.c: rt_hw_sdio_timeout_handle() →
   *       dump_sdio_reg();
   *       HAL_RCC_ResetModule(RCC_MOD_SDMMC1);
   *       mdelay(1);
   *       recov_sdio_reg();        ← 复位后重新配置，等价于下面的 sd1_init 本体
   *
   * 我们原来漏了它：早期只在 SD 寄存器块（SR/CCR/CLKCR/…）里找软复位，没找到
   * 就下了"这颗芯片没有控制器复位"的结论 —— **错的**，它在 RCC 的 RSTR2 里
   * （HPSYS_RCC_RSTR2_SDMMC1）。僵住的控制器和哑掉的卡现象完全一样：所有命令
   * 超时、CMD8 无应答、CMD13 不答。所以这条很可能正是"卡锁死后 5 次硬复位都
   * 救不回、只有整机掉电才行"的答案 —— 整机复位会连带复位这个模块。
   *
   * 放在 sd1_init() 里是因为它是唯一的重初始化入口：开机、运行期重识别、
   * 硬复位复活、崩溃裸机路径全部经过这里，一处到位。
   */
  HAL_RCC_ResetModule(RCC_MOD_SDMMC1);
  HAL_Delay_us(1000);

  hwp_hpsys_rcc->ENR2 &= ~HPSYS_RCC_ENR2_SDMMC1;
  HAL_Delay_us(100);
  hwp_hpsys_rcc->ENR2 |= HPSYS_RCC_ENR2_SDMMC1;
  hwp_hpsys_cfg->SYSCR |= HPSYS_CFG_SYSCR_SDNAND;
  hwp_sdmmc1->CLKCR = 0x1 << SD_CLKCR_DIV_Pos;  /* also clears sd_stop_clk */
  hwp_sdmmc1->CDR = 0;                          /* no card detect */
}

/* Wall-clock backstop for the SD PIO status polls below.  Boot waits forever;
 * under NuttX that would hold g_sd_lock and freeze MTP.  A healthy 24 MHz
 * single-block transfer completes in well under 1 ms, so 50 ms is only a
 * stuck-controller backstop.
 */

#define SF32LB_SD_WAIT_US         50000ull

/* CMD24 DATA_DONE may include DAT0 busy, but cheap cards (and LittleFS
 * 512 B COW + 0xFF erase fills) often program longer than 250 ms.  Aborting
 * that wait with DCR=0 wedges the card: the next CMD24 then hits cmd_to /
 * wr_to until retries give up.  Wait up to WRITE_MAX_US for DATA_DONE, then
 * poll DAT0 (DSR[0]) until the card leaves programming.
 */

#define SF32LB_SD_WRITE_WAIT_US   500000ull
#define SF32LB_SD_WRITE_MAX_US    2000000ull

/* 判死与复活。卡完全不应答时，一轮读失败要走 4 次重试 + 丢到 6 MHz +
 * 一次完整重识别，每个块 2~4 秒；所有文件系统调用者都卡在这上面，UI 心跳
 * 一停看门狗就不喂了 —— 这是"卡死后整机跟着死"的真实链路。所以连续几次
 * 重识别都失败后就判死，改为立即 -EIO 快速失败，只按周期做一次硬复位升级。
 * 芯片上没有 SD 供电控制（SYSCR_SDNAND 只是 AHB 地址映射选择 SD2/MPI3，
 * PMUC 里没有任何 SD/NAND 电源位），断电之外只有这一档更接近冷启动的动作。
 */
#define SF32LB_SD_DEAD_AFTER      3u
#define SF32LB_SD_DEAD_RETRY_MS   30000u
#define SF32LB_SD_HARD_DOWN_MS    200u

/* Extra attempts per 512 B block on a CRC/timeout before giving up.  A single
 * block read/write is idempotent (CMD17/CMD24 to the same address), so a
 * transient glitch under heavy I/O recovers transparently instead of bubbling
 * an -EIO up into LittleFS/MTP.
 */

#define SF32LB_SD_IO_RETRIES   3

/* Fallback clock after retries at 24 MHz still fail (sampling CRC).  Always
 * restore TRAN after a successful block — do not stay at 6 MHz.
 */

#define SF32LB_SD_RETRY_CLOCK_HZ   6000000U

#define SF32LB_SD_R1_STATE_SHIFT   9
#define SF32LB_SD_R1_STATE_MASK    (0xfu << SF32LB_SD_R1_STATE_SHIFT)
#define SF32LB_SD_R1_STATE_IDLE    (0u << SF32LB_SD_R1_STATE_SHIFT)
#define SF32LB_SD_R1_STATE_STBY    (3u << SF32LB_SD_R1_STATE_SHIFT)
#define SF32LB_SD_R1_STATE_TRAN    (4u << SF32LB_SD_R1_STATE_SHIFT)
#define SF32LB_SD_R1_STATE_DATA    (5u << SF32LB_SD_R1_STATE_SHIFT)
#define SF32LB_SD_R1_STATE_RCV     (6u << SF32LB_SD_R1_STATE_SHIFT)
#define SF32LB_SD_R1_STATE_PRG     (7u << SF32LB_SD_R1_STATE_SHIFT)

static uint64_t sd1_mono_us(void)
{
  struct timespec ts;
  uint32_t hclk;
  uint32_t mhz;

  if (g_sd_baremetal)
    {
      /* PRIMASK 挡住 SysTick ISR，不能用 clock_systime。DWT 仍在走。 */
      hclk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
      mhz = (hclk >= 1000000u) ? (hclk / 1000000u) : 240u;
      return (uint64_t)((*(volatile uint32_t *)0xe0001004u) / mhz);
    }

  clock_systime_timespec(&ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static uint8_t sd1_wait_card_idle(void);
static int sf32lb_sd_identify(void);
static int sf32lb_sd_cmd12(void);
static bool sd1_dead_fail_fast(void);

/* Clear command flags only.  Boot never writes SR before CCR; HAL_SDMMC_SET_CMD
 * also wipes DATA_* which races sd1_setup_read() (DPSM already running).
 */

static void sd1_clr_cmd_int(void)
{
  hwp_sdmmc1->SR = SD_SR_CMD_DONE | SD_SR_CMD_RSP_CRC | SD_SR_CMD_TIMEOUT |
                   SD_SR_CMD_SENT;
}

static uint8_t sd1_wait_cmd(void)
{
  uint64_t deadline = sd1_mono_us() + SF32LB_SD_WAIT_US;

  while ((hwp_sdmmc1->SR & (SD_SR_CMD_DONE | SD_SR_CMD_TIMEOUT)) == 0)
    {
      if (sd1_mono_us() >= deadline)
        {
          g_sd_last_sr = hwp_sdmmc1->SR;
          hwp_sdmmc1->SR = 0xffffffff;
          g_sd_diag.cmd_timeout++;
          return SD_TIMEOUT;
        }
    }

  hwp_sdmmc1->SR = SD_SR_CMD_DONE;
  if (hwp_sdmmc1->SR & SD_SR_CMD_TIMEOUT)
    {
      g_sd_last_sr = hwp_sdmmc1->SR;
      g_sd_diag.cmd_timeout++;
      return SD_TIMEOUT;
    }

  if (hwp_sdmmc1->SR & SD_SR_CMD_RSP_CRC)
    {
      return SD_CRCERR;
    }

  return SD_SUCCESS;
}

static uint8_t sd1_send_cmd(uint8_t cmd_idx, uint32_t cmd_arg)
{
  uint32_t ccr;
  uint8_t has_rsp;
  uint8_t long_rsp;

  sd1_clr_cmd_int();
  hwp_sdmmc1->CAR = cmd_arg;
  switch (cmd_idx)
    {
      case 2:
      case 9:
      case 10:
        has_rsp = 1;
        long_rsp = 1;
        break;
      case 0:
      case 4:
      case 15:
        has_rsp = 0;
        long_rsp = 0;
        break;
      default:
        has_rsp = 1;
        long_rsp = 0;
        break;
    }

  ccr = (cmd_idx << SD_CCR_CMD_INDEX_Pos);
  if (has_rsp)
    {
      ccr |= SD_CCR_CMD_HAS_RSP;
    }

  if (long_rsp)
    {
      ccr |= SD_CCR_CMD_LONG_RSP;
    }

  ccr |= SD_CCR_CMD_TX_EN | SD_CCR_CMD_START;
  hwp_sdmmc1->CCR = ccr;
  return sd1_wait_cmd();
}

static uint8_t sd1_send_acmd(uint8_t cmd_idx, uint32_t cmd_arg, uint16_t rca)
{
  uint32_t ccr;
  uint8_t cmd_result;

  cmd_result = sd1_send_cmd(55, (uint32_t)rca << 16);
  if (cmd_result != SD_SUCCESS)
    {
      return cmd_result;
    }

  hwp_sdmmc1->CAR = cmd_arg;
  ccr = (cmd_idx << SD_CCR_CMD_INDEX_Pos) | SD_CCR_CMD_HAS_RSP;
  ccr |= SD_CCR_CMD_TX_EN | SD_CCR_CMD_START;
  hwp_sdmmc1->CCR = ccr;
  cmd_result = sd1_wait_cmd();
  if ((cmd_result == SD_CRCERR) && (cmd_idx == 41)) /* no CRC check for R3 */
    {
      hwp_sdmmc1->SR = SD_SR_CMD_RSP_CRC;
      cmd_result = SD_SUCCESS;
    }

  return cmd_result;
}

static void sd1_get_rsp(uint8_t *rsp_idx, uint32_t *a1, uint32_t *a2,
                        uint32_t *a3, uint32_t *a4)
{
  *rsp_idx = hwp_sdmmc1->RIR;
  *a1 = hwp_sdmmc1->RAR1;
  *a2 = hwp_sdmmc1->RAR2;
  *a3 = hwp_sdmmc1->RAR3;
  *a4 = hwp_sdmmc1->RAR4;
}

static void sd1_setup_read(uint8_t wire_mode, uint8_t block_num)
{
  uint32_t dcr;

  hwp_sdmmc1->DLR = (SD_HW_BLOCK_SIZE * block_num) - 1;
  dcr = ((SD_HW_BLOCK_SIZE - 1) << SD_DCR_BLOCK_SIZE_Pos);
  dcr |= (wire_mode << SD_DCR_WIRE_MODE_Pos) | SD_DCR_R_WN;
  dcr |= SD_DCR_TRAN_DATA_EN | SD_DCR_DATA_START;
  hwp_sdmmc1->DCR = dcr;
}

static uint8_t sd1_wait_read(void)
{
  uint32_t mask = SD_SR_DATA_DONE | SD_SR_DATA_TIMEOUT | SD_SR_DATA_CRC |
                  SD_SR_FIFO_OVERRUN;
  uint64_t deadline = sd1_mono_us() + SF32LB_SD_WAIT_US;

  while ((hwp_sdmmc1->SR & mask) == 0)
    {
      if (sd1_mono_us() >= deadline)
        {
          hwp_sdmmc1->SR = 0xffffffff;
          g_sd_diag.rd_timeout++;
          return SD_TIMEOUT;
        }
    }

  hwp_sdmmc1->SR = SD_SR_DATA_DONE;
  if (hwp_sdmmc1->SR & SD_SR_DATA_TIMEOUT)
    {
      g_sd_diag.rd_timeout++;
      return SD_TIMEOUT;
    }

  if (hwp_sdmmc1->SR & (SD_SR_DATA_CRC | SD_SR_FIFO_OVERRUN))
    {
      g_sd_diag.rd_crc++;
      return SD_CRCERR;
    }

  return SD_SUCCESS;
}

static void sd1_setup_write(uint8_t wire_mode, uint8_t block_num)
{
  uint32_t dcr;

  hwp_sdmmc1->DLR = (SD_HW_BLOCK_SIZE * block_num) - 1;
  dcr = ((SD_HW_BLOCK_SIZE - 1) << SD_DCR_BLOCK_SIZE_Pos);
  dcr |= (wire_mode << SD_DCR_WIRE_MODE_Pos);
  dcr |= SD_DCR_TRAN_DATA_EN | SD_DCR_DATA_START;
  hwp_sdmmc1->DCR = dcr;
}

/* DSR[0] is DAT0 last-level.  After a write the card holds DAT0 low while
 * it programs; SD_SR_DATA_BUSY is only the host TX engine.  Identify samples
 * idle DAT0 — if it is not high, skip this poll so a dead DSR cannot stall
 * every block for WRITE_MAX_US.
 */

static bool sd1_dat0_ready(void)
{
  if (!g_sd_dat0_poll)
    {
      return true;
    }

  return (hwp_sdmmc1->DSR & 1u) != 0;
}

/**
 * @brief PIO 忙等循环的“让出 + 喂狗”节拍器，每 1 ms 触发一次。
 *
 * @details
 * 下面几个等待都是**轮询卡的状态位**（等 DAT0 编程忙、等 DATA_DONE、
 * 等 DATA_BUSY），没有中断可用，所以轮询耗掉的时间必然记在发起 I/O 的那个
 * 线程头上 —— 这不是"等"、是实打实的 CPU。
 *
 * 原先只有 `sd1_wait_dat0()` 会 `sched_yield()` + 喂狗，而
 * `sd1_wait_write()` / `sd1_wait_card_idle()` 是**纯自旋**，上界
 * `SF32LB_SD_WRITE_MAX_US` = 2 s。调用者优先级高于被饿住的那些任务时，
 * 这 2 s 里它们一个都跑不到 —— 具体到本板：diag 线程 pri=9 每 500 ms 往
 * SD 上的 LittleFS 刷日志（`myvendor_diaglog_flush()`），卡一慢就会把
 * `ble_companion`(pri=8) 和 IDLE(0) 饿住。三个等待统一走这个节拍器。
 *
 * @param[in,out] next_yield 下次让出的时刻；调用方进入循环前初始化一次。
 */
static void sd1_spin_yield(uint64_t *next_yield)
{
  uint64_t now = sd1_mono_us();

  if (now >= *next_yield)
    {
      myvendor_watchdog_work_beat();
      sched_yield();
      *next_yield = now + 1000u;
    }
}

static uint8_t sd1_wait_dat0(uint64_t timeout_us)
{
  uint64_t deadline;
  uint64_t next_yield;

  if (!g_sd_dat0_poll)
    {
      return SD_SUCCESS;
    }

  deadline = sd1_mono_us() + timeout_us;
  next_yield = sd1_mono_us() + 1000u;
  while (!sd1_dat0_ready())
    {
      uint64_t now = sd1_mono_us();

      if (now >= deadline)
        {
          g_sd_last_sr = hwp_sdmmc1->SR;
          g_sd_diag.dat0_to++;
          return SD_TIMEOUT;
        }

      sd1_spin_yield(&next_yield);
    }

  return SD_SUCCESS;
}

static uint8_t sd1_wait_write(void)
{
  /* Must wait on the error bits too, not just DATA_DONE: on a write CRC or
   * data timeout the controller may never assert DATA_DONE, which previously
   * spun this loop forever and hung the whole SD-backed filesystem.
   */

  uint32_t mask = SD_SR_DATA_DONE | SD_SR_DATA_TIMEOUT | SD_SR_DATA_CRC |
                  SD_SR_FIFO_UNDERRUN;
  uint64_t deadline = sd1_mono_us() + SF32LB_SD_WRITE_MAX_US;
  uint64_t next_yield = sd1_mono_us() + 1000u;

  while ((hwp_sdmmc1->SR & mask) == 0)
    {
      if (sd1_mono_us() >= deadline)
        {
          g_sd_last_sr = hwp_sdmmc1->SR;
          hwp_sdmmc1->DCR = 0;
          hwp_sdmmc1->SR = 0xffffffff;
          g_sd_diag.wr_timeout++;
          return SD_TIMEOUT;
        }

      /* 没有这句就是纯自旋最长 2 s：优先级低于调用者的任务全被饿住。 */
      sd1_spin_yield(&next_yield);
    }

  hwp_sdmmc1->SR = SD_SR_DATA_DONE;
  if (hwp_sdmmc1->SR & SD_SR_DATA_TIMEOUT)
    {
      g_sd_last_sr = hwp_sdmmc1->SR;
      g_sd_diag.wr_timeout++;
      return SD_TIMEOUT;
    }

  if (hwp_sdmmc1->SR & (SD_SR_DATA_CRC | SD_SR_FIFO_UNDERRUN))
    {
      g_sd_last_sr = hwp_sdmmc1->SR;
      g_sd_diag.wr_crc++;
      return SD_CRCERR;
    }

  if (sd1_wait_card_idle() != SD_SUCCESS)
    {
      return SD_TIMEOUT;
    }

  return sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
}

/* SiFli RT-Thread drv_sdio.c waits SD_SR_DATA_BUSY (HW_SDIO_IT_TXACT) after
 * every data transfer.  That bit is the host data engine, not card DAT0.
 * After it clears, sd1_wait_dat0() waits for programming busy.
 */

#define SF32LB_SD_BUSY_US  SF32LB_SD_WRITE_MAX_US

static uint8_t sd1_wait_card_idle(void)
{
  uint64_t deadline = sd1_mono_us() + SF32LB_SD_BUSY_US;
  uint64_t next_yield = sd1_mono_us() + 1000u;

  while ((hwp_sdmmc1->SR & SD_SR_DATA_BUSY) != 0)
    {
      if (sd1_mono_us() >= deadline)
        {
          return SD_TIMEOUT;
        }

      /* 同上：没有这句就是纯自旋最长 2 s。 */
      sd1_spin_yield(&next_yield);
    }

  return SD_SUCCESS;
}

/* 有序停机：先把 SD 时钟停下来、把卡送回 IDLE，然后才允许断外设时钟。
 * 直接拉 ENR2 会把正在移位的 SDCLK 生硬截断，SD-NAND 会锁死到掉电为止
 * （症状：CMD13 无应答 → mute，之后每次重识别 CMD8 都超时、mute_n 一直涨，
 *  只有整机复位才恢复）。顺序照抄 bootloader 的 sd_release()。
 *
 * STOP_CLK=1 才是停钟（HAL_SDMMC_CLK_SET 的 en=0 分支），所以这里绝不能
 * 写 CLKCR=0 —— 那只是把 DIV 清零、时钟照跑。
 */
static void sd1_hw_quiet(void)
{
  hwp_sdmmc1->DCR = 0;
  hwp_sdmmc1->IER = 0;
  hwp_sdmmc1->SR = 0xffffffff;
  (void)sd1_wait_card_idle();
  (void)sd1_send_cmd(0, 0);  /* CMD0 需要时钟，必须放在停钟之前 */
  HAL_Delay_us(1000);
  hwp_sdmmc1->CLKCR = SD_CLKCR_STOP_CLK;  /* 停 SDCLK；此后才允许断外设时钟 */
  HAL_Delay_us(10);
}

static int sf32lb_sd_dma_init(void)
{
  HAL_RCC_EnableModule(RCC_MOD_DMAC1);

  memset(&g_sd_dmah, 0, sizeof(g_sd_dmah));
  g_sd_dmah.Instance                 = SDMMC1_DMA_INSTANCE;
  g_sd_dmah.Init.Request             = SDMMC1_DMA_REQUEST;
  g_sd_dmah.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  g_sd_dmah.Init.PeriphInc           = DMA_PINC_DISABLE;
  g_sd_dmah.Init.MemInc              = DMA_MINC_ENABLE;
  g_sd_dmah.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  g_sd_dmah.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
  g_sd_dmah.Init.Mode                = DMA_NORMAL;
  g_sd_dmah.Init.Priority            = DMA_PRIORITY_LOW;
  g_sd_dmah.Init.BurstSize           = 1;
  g_sd_dmah.Init.IrqPrio             = SDMMC1_DMA_IRQ_PRIO;
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
  g_sd_dmah.KeepAlloc                = 1;
#endif

  if (HAL_DMA_Init(&g_sd_dmah) != HAL_OK)
    {
      return -EIO;
    }

#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
  if (HAL_DMA_AllocChannel(&g_sd_dmah) != HAL_OK)
    {
      return -EIO;
    }
#endif

  return OK;
}

/* Reconfigure DIR on the pinned CH3. Do not DeInit (that drops KeepAlloc). */

static int sf32lb_sd_dma_prepare(bool is_write)
{
  if (g_sd_dmah.State != HAL_DMA_STATE_READY)
    {
      (void)HAL_DMA_Abort(&g_sd_dmah);
    }

  g_sd_dmah.Init.Direction = is_write ? DMA_MEMORY_TO_PERIPH :
                             DMA_PERIPH_TO_MEMORY;
  g_sd_dmah.Init.Priority = is_write ? DMA_PRIORITY_MEDIUM :
                            DMA_PRIORITY_LOW;
#ifdef DMA_SUPPORT_DYN_CHANNEL_ALLOC
  g_sd_dmah.KeepAlloc = 1;
#endif

  if (HAL_DMA_Init(&g_sd_dmah) != HAL_OK)
    {
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Pin mux + card identification (ported from bootloader board.c / sd_nand_ops)
 ****************************************************************************/

static void sf32lb_sd_pinmux(void)
{
  HAL_PIN_Set(PAD_PA15, SD1_CMD, PIN_PULLUP, 1);
  HAL_Delay_us(20);   /* settle before clocking to avoid a spurious command */
  HAL_PIN_Set(PAD_PA14, SD1_CLK,  PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA16, SD1_DIO0, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA17, SD1_DIO1, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA12, SD1_DIO2, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA13, SD1_DIO3, PIN_PULLUP, 1);
}

/* CSD is 128 bits after the CMD9 shuffle: resp[0]=[127:96] … resp[3]=[31:0].
 * lsb is the inclusive low bit of the field (e.g. CSD_STRUCTURE uses lsb=126).
 */

static uint32_t sf32lb_sd_csd_bits(FAR const uint32_t *resp, unsigned lsb,
                                   unsigned nbits)
{
  uint32_t val = 0;
  unsigned i;

  if (nbits == 0 || nbits > 32)
    {
      return 0;
    }

  for (i = 0; i < nbits; i++)
    {
      unsigned bit = lsb + i;
      unsigned word = 3u - (bit / 32u);
      unsigned shft = bit & 31u;

      if ((resp[word] >> shft) & 1u)
        {
          val |= (1u << i);
        }
    }

  return val;
}

static uint8_t sf32lb_sd_mmcsd_csd_struct(FAR uint32_t *resp)
{
  /* CSD_STRUCTURE [127:126]. */

  return (uint8_t)(sf32lb_sd_csd_bits(resp, 126, 2) & 0x3u);
}

static uint64_t sf32lb_sd_csd_capacity(FAR const uint32_t *resp,
                                       uint8_t csd_struct)
{
  uint32_t c_size;
  uint32_t read_bl_len;
  uint32_t c_mult;
  uint64_t blocks;

  if (csd_struct == 0)
    {
      /* SDSC CSD v1: (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN. */

      read_bl_len = sf32lb_sd_csd_bits(resp, 80, 4);
      c_size = sf32lb_sd_csd_bits(resp, 62, 12);
      c_mult = sf32lb_sd_csd_bits(resp, 47, 3);
      blocks = ((uint64_t)c_size + 1ull) << (c_mult + 2);
      return blocks << read_bl_len;
    }

  /* SDHC/SDXC CSD v2: C_SIZE[69:48] (22 bits). SDUC CSD v3: [75:48] (28). */

  c_size = sf32lb_sd_csd_bits(resp, 48, csd_struct >= 2 ? 28 : 22);
  return ((uint64_t)c_size + 1ull) * 512ull * 1024ull;
}

static uint32_t sf32lb_sd_cmd_arg(uint64_t card_byte_addr)
{
  if (g_sd_sdsc)
    {
      return (uint32_t)card_byte_addr;
    }

  return (uint32_t)(card_byte_addr >> 9);
}

static int sf32lb_sd_identify(void)
{
  uint8_t rsp_idx;
  uint32_t rsp[4];
  uint32_t csd[4];
  uint8_t cmd_result;
  uint32_t cmd_arg;
  uint16_t rca = 0;
  uint8_t csd_struct;

  sf32lb_sd_pinmux();

  /* Identification clock: 400 kHz. */

  sd1_init();
  sd1_set_clock(SF32LB_SD_ID_CLOCK_HZ);
  hwp_sdmmc1->IER = 0;
  hwp_sdmmc1->TOR = 0x00100000;
  HAL_Delay_us(500);

  /* CMD0 GO_IDLE */

  sd1_send_cmd(0, 0);
  hwp_sdmmc1->CASR = SD_CASR_SD_REQ;
    {
      uint64_t deadline = sd1_mono_us() + SF32LB_SD_WAIT_US;

      while ((hwp_sdmmc1->CASR & SD_CASR_SD_BUSY) == 0)
        {
          if (sd1_mono_us() >= deadline)
            {
              syslog(LOG_ERR, "ERROR: SD CMD0 busy wait timeout\n");
              break;
            }
        }
    }

  /* CMD8 SEND_IF_COND (VHS=1, check pattern 0xAA) */

  HAL_Delay_us(20);
  cmd_result = sd1_send_cmd(8, 0x000001aa);
  if (cmd_result != SD_SUCCESS)
    {
      syslog(LOG_ERR, "ERROR: SD CMD8 failed: %d\n", cmd_result);
      return -EIO;
    }

  sd1_get_rsp(&rsp_idx, &rsp[0], &rsp[1], &rsp[2], &rsp[3]);
  if ((rsp_idx != 0x8) || (rsp[0] != 0x1aa))
    {
      syslog(LOG_ERR, "ERROR: SD CMD8 bad rsp idx=%u arg=0x%08lx\n",
             rsp_idx, (unsigned long)rsp[0]);
      return -EIO;
    }

  /* ACMD41 with HCS=1; wait for power-up (busy bit 31). */

  cmd_arg = 0x40ff8000;
  while (1)
    {
      HAL_Delay_us(20);
      cmd_result = sd1_send_acmd(41, cmd_arg, rca);
      if (cmd_result == SD_TIMEOUT)
        {
          syslog(LOG_ERR, "ERROR: SD ACMD41 timeout\n");
          return -EIO;
        }

      sd1_get_rsp(&rsp_idx, &rsp[0], &rsp[1], &rsp[2], &rsp[3]);
      if ((rsp[0] & 0x80000000) != 0)
        {
          break;
        }

      HAL_Delay_us(2);
    }

  /* CMD2 ALL_SEND_CID */

  HAL_Delay_us(20);
  cmd_result = sd1_send_cmd(2, 0);
  if (cmd_result != SD_SUCCESS)
    {
      syslog(LOG_ERR, "ERROR: SD CMD2 failed: %d\n", cmd_result);
      return -EIO;
    }

  sd1_get_rsp(&rsp_idx, &csd[3], &csd[2], &csd[1], &csd[0]);

  /* CMD3 SEND_RELATIVE_ADDR */

  HAL_Delay_us(20);
  cmd_result = sd1_send_cmd(3, 0);
  if (cmd_result != SD_SUCCESS)
    {
      syslog(LOG_ERR, "ERROR: SD CMD3 failed: %d\n", cmd_result);
      return -EIO;
    }

  sd1_get_rsp(&rsp_idx, &rsp[0], &rsp[1], &rsp[2], &rsp[3]);
  rca = rsp[0] >> 16;
  g_sd_rca = rca;
  if (rsp_idx != 0x3)
    {
      syslog(LOG_ERR, "ERROR: SD CMD3 bad rsp idx=%u\n", rsp_idx);
      return -EIO;
    }

  /* CMD9 SEND_CSD (R2) to determine SDSC vs SDHC addressing. */

  HAL_Delay_us(20);
  cmd_result = sd1_send_cmd(9, (uint32_t)rca << 16);
  if (cmd_result != SD_SUCCESS)
    {
      syslog(LOG_ERR, "ERROR: SD CMD9 failed: %d\n", cmd_result);
      return -EIO;
    }

  sd1_get_rsp(&rsp_idx, &rsp[0], &rsp[1], &rsp[2], &rsp[3]);
    {
      /* R2 is a 128-bit response with the trailing 8 bits stripped; rebuild
       * a normalized 128-bit value (resp[0] = highest word).
       */

      uint32_t temp;
      temp = rsp[0]; rsp[0] = rsp[3]; rsp[3] = temp;
      temp = rsp[1]; rsp[1] = rsp[2]; rsp[2] = temp;
      rsp[0] = (rsp[0] << 8) | (rsp[1] >> 24);
      rsp[1] = (rsp[1] << 8) | (rsp[2] >> 24);
      rsp[2] = (rsp[2] << 8) | (rsp[3] >> 24);
      rsp[3] = (rsp[3] << 8);
    }

  csd_struct = sf32lb_sd_mmcsd_csd_struct(&rsp[0]);
  if (csd_struct == 0)
    {
      g_sd_sdsc = 1;   /* SDSC: byte addressing */
    }
  else if (csd_struct == 1 || csd_struct == 2)
    {
      g_sd_sdsc = 0;   /* SDHC/SDXC/SDUC: block addressing */
    }
  else
    {
      syslog(LOG_ERR, "ERROR: SD invalid CSD structure %u\n", csd_struct);
      return -EIO;
    }

  g_sd_card_bytes = sf32lb_sd_csd_capacity(&rsp[0], csd_struct);
  if (g_sd_card_bytes < SD_HW_BLOCK_SIZE)
    {
      syslog(LOG_ERR, "ERROR: SD CSD capacity %llu too small\n",
             (unsigned long long)g_sd_card_bytes);
      return -EIO;
    }

  /* Switch to the transfer clock. */

  sd1_set_clock(SF32LB_SD_TRAN_CLOCK_HZ);
  g_sd_bus_hz = SF32LB_SD_TRAN_CLOCK_HZ;
  hwp_sdmmc1->TOR = 0x02000000;

  /* Boot (sd_nand_ops.c after the 24 MHz switch):
   *   CDR = ITIMING_SEL | ITIMING=0
   * That is the sample point that loads the kernel off this same card.
   * HAL_SDMMC_INIT clears ITIMING_SEL; that path is DMA and was not what
   * booted us.
   */

  hwp_sdmmc1->CDR = SD_CDR_ITIMING_SEL | (0u << SD_CDR_ITIMING_Pos);

  /* CMD7 SELECT_CARD */

  HAL_Delay_us(20);
  cmd_result = sd1_send_cmd(7, (uint32_t)rca << 16);
  if (cmd_result != SD_SUCCESS)
    {
      syslog(LOG_ERR, "ERROR: SD CMD7 failed: %d\n", cmd_result);
      return -EIO;
    }

  sd1_get_rsp(&rsp_idx, &rsp[0], &rsp[1], &rsp[2], &rsp[3]);

  /* ACMD6 SET_BUS_WIDTH (4-wire when SF32LB_SD_WIRE_MODE). */

  HAL_Delay_us(20);
  cmd_arg = SF32LB_SD_WIRE_MODE ? 2 : 0;
  cmd_result = sd1_send_acmd(6, cmd_arg, rca);
  if (cmd_result != SD_SUCCESS)
    {
      syslog(LOG_ERR, "ERROR: SD ACMD6 failed: %d\n", cmd_result);
      return -EIO;
    }

  /* Idle DAT0 must read high (pull-up).  If DSR[0] is stuck low, DAT0
   * polling would stall every write for WRITE_MAX_US — disable it.
   */

  g_sd_dat0_poll = (hwp_sdmmc1->DSR & 1u) != 0;
  syslog(LOG_INFO, "INFO: SD card init ok (rca=0x%04x, %s, cap=%llu bytes "
         "(%llu MiB), hclk=%lu Hz, clkdiv=%lu, dsr=0x%02lx dat0_poll=%d)\n",
         rca, g_sd_sdsc ? "SDSC" : "SDHC/SDXC",
         (unsigned long long)g_sd_card_bytes,
         (unsigned long long)(g_sd_card_bytes / (1024ull * 1024ull)),
         (unsigned long)HAL_RCC_GetHCLKFreq(CORE_ID_HCPU),
         (unsigned long)(((hwp_sdmmc1->CLKCR & SD_CLKCR_DIV_Msk) >>
                          SD_CLKCR_DIV_Pos) + 1),
         (unsigned long)(hwp_sdmmc1->DSR & 0xffu),
         g_sd_dat0_poll ? 1 : 0);
  return OK;
}

/****************************************************************************
 * Single-block read / write (programmed I/O via FIFO)
 ****************************************************************************/

static int sf32lb_sd_read_block(uint64_t card_byte_addr, FAR uint8_t *dst)
{
  uint8_t rsp_idx;
  uint32_t cmd_arg;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r4;
  uint8_t cmd_result;
  int i;
#if SF32LB_SD_PROFILE
  uint64_t t0 = sf32lb_sd_now_us();

  g_sd_stats.hw_read++;
#endif

  /* Do not arm DPSM until the previous write has released DAT0. */

  hwp_sdmmc1->DCR = 0;
  (void)sd1_wait_card_idle();
  if (sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US) != SD_SUCCESS)
    {
      return -EIO;
    }

  sd1_setup_read(SF32LB_SD_WIRE_MODE, 1);
  HAL_Delay_us(20);

  cmd_arg = sf32lb_sd_cmd_arg(card_byte_addr);
  cmd_result = sd1_send_cmd(17, cmd_arg);   /* READ_SINGLE_BLOCK */
  if (cmd_result != SD_SUCCESS)
    {
      return -EIO;
    }

  sd1_get_rsp(&rsp_idx, &r1, &r2, &r3, &r4);
  if (rsp_idx != 17)
    {
      return -EIO;
    }

  hwp_sdmmc1->SR = 0xffffffff;
  hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
  cmd_result = sd1_wait_read();
  if (cmd_result != SD_SUCCESS)
    {
      return -EIO;
    }

  for (i = 0; i < (int)(SD_HW_BLOCK_SIZE / 4); i++)
    {
      g_sd_blkbuf[i] = hwp_sdmmc1->FIFO;
    }

  memcpy(dst, g_sd_blkbuf, SD_HW_BLOCK_SIZE);
#if SF32LB_SD_PROFILE
  g_sd_stats.hw_read_us += sf32lb_sd_now_us() - t0;
#endif
  return OK;
}

/* Program the 512 bytes currently staged in g_sd_blkbuf to card_byte_addr.
 * Caller must hold g_sd_lock.
 */

static int sf32lb_sd_program_staged(uint64_t card_byte_addr)
{
  uint8_t rsp_idx;
  uint32_t cmd_arg;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r4;
  uint8_t cmd_result;
  int i;
#if SF32LB_SD_PROFILE
  uint64_t t0 = sf32lb_sd_now_us();

  g_sd_stats.hw_write++;
#endif

  hwp_sdmmc1->DCR = 0;

  if (sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US) != SD_SUCCESS)
    {
      return -EIO;
    }

  cmd_arg = sf32lb_sd_cmd_arg(card_byte_addr);
  cmd_result = sd1_send_cmd(24, cmd_arg);   /* WRITE_BLOCK */
  if (cmd_result != SD_SUCCESS)
    {
      return -EIO;
    }

  sd1_get_rsp(&rsp_idx, &r1, &r2, &r3, &r4);
  if (rsp_idx != 24)
    {
      return -EIO;
    }

  /* CMD24 while the card is still in PROGRAMMING: do not start DATA. */

  if ((r1 & SF32LB_SD_R1_STATE_MASK) == SF32LB_SD_R1_STATE_PRG)
    {
      g_sd_last_sr = r1;
      (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
      return -EIO;
    }

  {
    irqstate_t flags = up_irq_save();

    sd1_setup_write(SF32LB_SD_WIRE_MODE, 1);
    hwp_sdmmc1->SR = 0xffffffff;
    hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;

    for (i = 0; i < (int)(SD_HW_BLOCK_SIZE / 4); i++)
      {
        hwp_sdmmc1->FIFO = g_sd_blkbuf[i];
      }

    up_irq_restore(flags);
  }

  cmd_result = sd1_wait_write();
  if (cmd_result != SD_SUCCESS)
    {
      return -EIO;
    }

#if SF32LB_SD_PROFILE
  g_sd_stats.hw_write_us += sf32lb_sd_now_us() - t0;
#endif
  return OK;
}

static int sf32lb_sd_write_block(uint64_t card_byte_addr,
                                 FAR const uint8_t *src)
{
  memcpy(g_sd_blkbuf, src, SD_HW_BLOCK_SIZE);
  return sf32lb_sd_program_staged(card_byte_addr);
}

static unsigned sd1_r1_state(uint32_t r1)
{
  return (unsigned)((r1 & SF32LB_SD_R1_STATE_MASK) >> SF32LB_SD_R1_STATE_SHIFT);
}

/**
 * @brief CMD13 SEND_STATUS。
 * @param[out] r1_out 卡 R1；可为 NULL。
 * @return 0 成功；负 errno。
 * @note 合法于 tran/data/rcv/prg。不要在非 STBY 时跟 CMD7，曾经卡死 /mnt/lfs。
 */
static int sd1_cmd13(uint32_t *r1_out)
{
  uint8_t idx;
  uint8_t st;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r4;

  if (g_sd_rca == 0)
    {
      return -ENODEV;
    }

  st = sd1_send_cmd(13, (uint32_t)g_sd_rca << 16);
  if (st != SD_SUCCESS)
    {
      return -EIO;
    }

  sd1_get_rsp(&idx, &r1, &r2, &r3, &r4);
  if (idx != 13)
    {
      return -EIO;
    }

  if (r1_out != NULL)
    {
      *r1_out = r1;
    }

  return OK;
}

/**
 * @brief 停 DMA、发 CMD12 退出数据相，再用 CMD13 看到 TRAN。
 * @param[out] r1_out 最后一次 CMD13 的 R1；可为 NULL。
 * @return 0 已在 TRAN；`-ENOTCONN` 卡已 IDLE（这时才允许 CMD0/CMD8）；
 *         其它负 errno 表示仍停在 data/rcv 等，**禁止**全量识别。
 */
static int sd1_unstick_data(uint32_t *r1_out, bool *mute_out)
{
  uint32_t r1 = 0;
  int cmd13_ok = 0;
  int i;
  int ret;
  unsigned st;

  if (mute_out != NULL)
    {
      *mute_out = false;
    }

  if (g_sd_dma && g_sd_dmah.State != HAL_DMA_STATE_READY)
    {
      (void)HAL_DMA_Abort(&g_sd_dmah);
    }

  /* 这里**不再**写 DCR = 0 —— 对齐官方驱动，也去掉一个自伤点。
   *
   * 本文件头部自己就写着："CMD24 DATA_DONE … Aborting that wait with DCR=0
   * wedges the card: the next CMD24 then hits cmd_to / wr_to until retries give
   * up."。而现场已经抓到那一下的后果：首次失败的 sr 是 0x00000001
   * （只有 CMD_BUSY、无任何错误位）= **控制器命令状态机卡在 CMD_BUSY，此后
   * 所有命令都不再发出去**，表现和"卡哑了"完全一样 —— 而第 4 次重试才把它
   * 报出来，说明卡住发生在更早，很可能就在上一次本函数的 DCR=0。
   *
   * 官方超时处理（drv_sdio.c: rt_hw_sdio_timeout_handle）用的是控制器模块复位
   * + 恢复配置，不用 DCR=0。我们的等价物在 sf32lb_sd_identify() 里（sd1_init
   * 开头那次 HAL_RCC_ResetModule）。所以这里只保留规范内的收尾：
   * IER 屏蔽、清状态、停 DMA、CMD12（STOP_TRANSMISSION 本身就是中止数据相位的
   * 标准命令），失败就交给调用方升级到重识别那条路。
   */
  hwp_sdmmc1->IER = 0;
  hwp_sdmmc1->SR = 0xffffffff;
  (void)sd1_wait_card_idle();
  HAL_Delay_us(50);
  (void)sf32lb_sd_cmd12();
  (void)sd1_wait_card_idle();
  (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);

  for (i = 0; i < 3; i++)
    {
      ret = sd1_cmd13(&r1);
      if (ret != OK)
        {
          HAL_Delay_us(200);
          (void)sf32lb_sd_cmd12();
          (void)sd1_wait_card_idle();
          (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
          continue;
        }

      st = r1 & SF32LB_SD_R1_STATE_MASK;
      cmd13_ok = 1;
      if (r1_out != NULL)
        {
          *r1_out = r1;
        }

      if (st == SF32LB_SD_R1_STATE_TRAN)
        {
          return OK;
        }

      if (st == SF32LB_SD_R1_STATE_PRG)
        {
          (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
          continue;
        }

      if (st == SF32LB_SD_R1_STATE_DATA || st == SF32LB_SD_R1_STATE_RCV)
        {
          (void)sf32lb_sd_cmd12();
          (void)sd1_wait_card_idle();
          (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
          continue;
        }

      if (st == SF32LB_SD_R1_STATE_IDLE)
        {
          return -ENOTCONN;
        }

      if (st == SF32LB_SD_R1_STATE_STBY && g_sd_rca != 0)
        {
          (void)sd1_send_cmd(7, (uint32_t)g_sd_rca << 16);
          (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
          continue;
        }

      break;
    }

  /* 走到这里说明没进任何恢复分支：r1 可能仍是初值 0。区分"CMD13 应答了但
   * 状态不是 TRAN/IDLE"与"CMD13 完全无响应"—— 只有后者才允许完整重识别，
   * 因为前者停在 data/rcv 等状态，发 CMD8 会把一次读失败放大成整卡僵死。 */
  if (mute_out != NULL)
    {
      *mute_out = (cmd13_ok == 0);
    }

  if (r1_out != NULL)
    {
      *r1_out = r1;
    }

  return -EIO;
}

/**
 * @brief 出错后停 DMA、CMD12、CMD13，把卡拉回 TRAN 再重试。
 * @details 调用方须已持有 `g_sd_lock`。
 */
static void sd1_recover_after_error(void)
{
  (void)sd1_unstick_data(NULL, NULL);
  HAL_Delay_us(200);
}

/**
 * @brief 重试耗尽后的最后手段：优先 CMD12+CMD13 回 TRAN。
 * @return 0 已可再读写；负 errno。
 * @note 只有卡已经 IDLE 才允许 CMD0/CMD8。DATA 态发 CMD8 会把一次读失败
 *       放大成整卡 1 Hz 僵死。
 */
static int sd1_reinit_card(void)
{
  static uint64_t last_us;
  static uint16_t mute_n;
  uint64_t now = sd1_mono_us();
  uint32_t r1 = 0;
  bool mute = false;
  int ret;

  ret = sd1_unstick_data(&r1, &mute);
  if (ret == OK)
    {
      syslog(LOG_WARNING, "SD: recovered to TRAN (skip re-identify r1=0x%lx)\n",
             (unsigned long)r1);
      mute_n = 0;
      return 0;
    }

  /* 两条路都允许完整重新识别（CMD0/CMD8 + 全量 identify）：
   *
   *   - -ENOTCONN：CMD13 应答且状态就是 IDLE，已**证明**卡在 idle，安全；
   *   - mute：CMD13 三次全无响应，r1 从未被写入。现场日志打的
   *     `r1=0x0 st=0` 属于这种，**不代表"卡在 idle 态"** —— 旧代码把它当
   *     "无法判定"而拒绝重识别，结果是永久不回退（现场：SD 异常后无法恢复，
   *     只能重启，且重启后仍然是坏的）。
   *
   * 为什么 mute 时该重识别：卡彻底不应答，通常是被复位回了 IDLE（掉电/欠压、
   * 卡自身复位、总线被拉低），而重新识别正是唯一能把它拉回来的动作。文件头
   * 担心的"DATA 态发 CMD8 会放大失败"在这里不成立 —— sd1_unstick_data 已经
   * 先发过 CMD12（STOP_TRANSMISSION，就是退出 DATA 态的命令），identify 内部
   * 又以 CMD0 GO_IDLE 开头。
   *
   * 安全性：sf32lb_sd_identify() 本来就在运行时被调用（下面的 -ENOTCONN 分支
   * 和 mount 路径），不需要 baremetal 模式、不需要关中断；只是耗时，所以
   * 统一限频到 1 s 一次，并累计 mute_n 便于从日志判断"重识别到底有没有用"。
   */
  if (ret != -ENOTCONN && !mute)
    {
      /* 停在 data/rcv/prg 等非 TRAN 非 IDLE 态：保持原判定，禁止全量识别。 */
      syslog(LOG_ERR,
             "SD: stuck in non-TRAN state r1=0x%lx st=%u (cmd13=ok), "
             "skip re-identify\n",
             (unsigned long)r1, sd1_r1_state(r1));
      return -EIO;
    }

  if (last_us != 0 && (now - last_us) < 1000000ull)
    {
      return -EAGAIN;
    }

  last_us = now;
  if (mute)
    {
      mute_n++;
    }

  syslog(LOG_WARNING,
         "SD: re-identify (%s) r1=0x%lx cmd13_mute=%d mute_n=%u clk=%ukHz\n",
         mute ? "card mute, CMD13 no response" : "card idle after I/O fail",
         (unsigned long)r1, mute ? 1 : 0, (unsigned)mute_n,
         (unsigned)(g_sd_bus_hz / 1000));

  ret = sf32lb_sd_identify();
  if (ret == 0)
    {
      sd1_set_clock(SF32LB_SD_RETRY_CLOCK_HZ);
      g_sd_bus_hz = SF32LB_SD_RETRY_CLOCK_HZ;
      syslog(LOG_WARNING, "SD: re-identify OK (after %u mute tries)\n",
             (unsigned)mute_n);
      mute_n = 0;
      g_sd_dead_mute = 0;
    }
  else
    {
      /* identify 会先切到 400 kHz 的识别时钟，失败路径上它不会自己切回来。
       * 不补这一下，卡就永远停在 400 kHz：之后每次重试都慢 60 倍，整条
       * 文件系统链路跟着一起卡。日志里的 `clk=` 也会跟着说谎。
       */
      sd1_set_clock(g_sd_bus_hz);

      /* 判死：连续几次重识别都失败就别再每块都试了。一轮 2~4 秒的重试会把
       * 所有文件系统调用者卡住，UI 心跳一停看门狗就不喂 —— 那才是"卡死后
       * 只能关机"的直接原因。判死之后 -EIO 立即返回，只按周期试硬复位。
       */
      if (mute)
        {
          if (g_sd_dead_mute < 0xffffu)
            {
              g_sd_dead_mute++;
            }
        }
      else
        {
          g_sd_dead_mute = 0;
        }

      syslog(LOG_ERR,
             "SD: re-identify FAILED ret=%d mute_n=%u clk=%ukHz dead_run=%u (bus clock restored)\n",
             ret, (unsigned)mute_n, (unsigned)(g_sd_bus_hz / 1000),
             (unsigned)g_sd_dead_mute);

      if (!g_sd_dead && g_sd_dead_mute >= SF32LB_SD_DEAD_AFTER)
        {
          g_sd_dead = true;
          g_sd_dead_since_us = sd1_mono_us();
          g_sd_dead_try_us = g_sd_dead_since_us;
          syslog(LOG_ERR,
                 "SD: declared DEAD after %u failed re-identify (mute_n=%u); fast -EIO from now on, hard reset every %u ms\n",
                 (unsigned)g_sd_dead_mute, (unsigned)mute_n,
                 (unsigned)SF32LB_SD_DEAD_RETRY_MS);
        }
    }

  return ret;
}

/* 把 SR 的错误位解成人能读的名字，对齐官方驱动的分类
 * （drv_sdio.c 按 HW_SDIO_IT_CTIMEOUT / DCRCFAIL / DTIMEOUT 区分）。
 *
 * 为什么需要它：我们原来只在日志里打 g_sd_last_sr 的数值，于是
 * `SD: read addr=... FAILED` 分不出是哪一类故障 —— 而
 *   - cmd_to   = 命令阶段完全无应答 → 卡或控制器不应答；
 *   - data_to  = 命令应答了、数据相位超时 → 传输中途坏；
 *   - data_crc / cmd_crc = 校验错 → 采样或边沿问题。
 * 是完全不同的三种病，混在一起就没法继续查。
 *
 * 按位优先级返回单一主因（不拼串、不格式化）：命令阶段的错先报，因为
 * "命令都没应答"会掩盖后面所有现象。返回常量字符串，无副作用。
 */
static const char *sd1_sr_why(uint32_t sr)
{
  if ((sr & SD_SR_CMD_TIMEOUT) != 0)
    {
      return "cmd_to (command phase: no response)";
    }

  if ((sr & SD_SR_CMD_RSP_CRC) != 0)
    {
      return "cmd_crc (response crc)";
    }

  if ((sr & SD_SR_DATA_TIMEOUT) != 0)
    {
      return "data_to (data phase timeout)";
    }

  if ((sr & SD_SR_DATA_CRC) != 0)
    {
      return "data_crc (data crc)";
    }

  if ((sr & SD_SR_STARTBIT_ERROR) != 0)
    {
      return "startbit (no start bit on data/response)";
    }

  if ((sr & SD_SR_FIFO_UNDERRUN) != 0)
    {
      return "fifo_ur";
    }

  if ((sr & SD_SR_FIFO_OVERRUN) != 0)
    {
      return "fifo_or";
    }

  if ((sr & SD_SR_CACHE_ERR) != 0)
    {
      return "cache_err";
    }

  /* 失败却没有错误位置起：典型是命令压根没发出去，或纯等待超时。
   * 用 busy/done 区分"控制器卡在 busy"和"什么都没发生"。 */
  if ((sr & SD_SR_CMD_BUSY) != 0)
    {
      return "noerr_but_cmd_busy";
    }

  if ((sr & SD_SR_CMD_DONE) != 0)
    {
      return "noerr_cmd_done";
    }

  return "noerr_idle";
}

static int sf32lb_sd_read_block_retry(uint64_t card_byte_addr,
                                      FAR uint8_t *dst)
{
  int ret;
  int t;

  if (sd1_dead_fail_fast())
    {
      return -EIO;
    }

  ret = sf32lb_sd_read_block(card_byte_addr, dst);
  if (ret >= 0)
    {
      sd1_restore_tran_clock();
      return ret;
    }

  /* Retry at the current clock first (official does not downclock on a
   * single glitch).  Drop to 6 MHz only after those retries fail, then
   * restore TRAN on success.
   */

  for (t = 1; t <= SF32LB_SD_IO_RETRIES; t++)
    {
      g_sd_diag.rd_retry++;
      sd1_recover_after_error();
      ret = sf32lb_sd_read_block(card_byte_addr, dst);
      if (ret >= 0)
        {
          sd1_restore_tran_clock();
          syslog(LOG_WARNING, "SD: read addr=0x%llx recovered at %u kHz "
                 "(try %d)\n", (unsigned long long)card_byte_addr,
                 (unsigned)(g_sd_bus_hz / 1000), t);
          return ret;
        }
    }

  sd1_set_clock(SF32LB_SD_RETRY_CLOCK_HZ);
  g_sd_bus_hz = SF32LB_SD_RETRY_CLOCK_HZ;
  sd1_recover_after_error();
  ret = sf32lb_sd_read_block(card_byte_addr, dst);
  if (ret >= 0)
    {
      sd1_restore_tran_clock();
      syslog(LOG_WARNING,
             "SD: read addr=0x%llx recovered at 6 MHz then restored\n",
             (unsigned long long)card_byte_addr);
      return ret;
    }

  g_sd_diag.rd_fail++;
  if (g_sd_fail_run < 0xffffu)
    {
      g_sd_fail_run++;
    }
  /* 把"连续第几次失败 / 当前总线时钟 / 累计重试"一起打出来，用来区分
   * 概率性抖动与永久性损坏：
   *   - run 每次失败都递增、成功即归零 ⇒ 抖动（单次故障）；
   *   - run 单调递增且 clk 一直停在 retry 档 ⇒ 永久性（卡已不应答）。
   * 现场只看单条 FAILED 日志分不出这两种，也看不出时钟是否已经掉档。 */
  syslog(LOG_ERR,
         "SD: read addr=0x%llx FAILED (tries=%d rca=0x%04x run=%u clk=%ukHz sr=0x%08lx why=%s rd_retry=%lu rd_fail=%lu)\n",
         (unsigned long long)card_byte_addr, SF32LB_SD_IO_RETRIES + 1,
         g_sd_rca, (unsigned)g_sd_fail_run, (unsigned)(g_sd_bus_hz / 1000),
         (unsigned long)g_sd_last_sr, sd1_sr_why(g_sd_last_sr),
         (unsigned long)g_sd_diag.rd_retry,
         (unsigned long)g_sd_diag.rd_fail);
  if (sd1_reinit_card() == 0)
    {
      ret = sf32lb_sd_read_block(card_byte_addr, dst);
      if (ret >= 0)
        {
          sd1_restore_tran_clock();
          syslog(LOG_WARNING,
                 "SD: read addr=0x%llx recovered after re-identify\n",
                 (unsigned long long)card_byte_addr);
        }
    }

  return ret;
}

static int sf32lb_sd_write_block_retry(uint64_t card_byte_addr,
                                       FAR const uint8_t *src)
{
  uint32_t crc0 = g_sd_diag.wr_crc;
  int ret;
  int t;

  if (sd1_dead_fail_fast())
    {
      return -EIO;
    }

  ret = sf32lb_sd_write_block(card_byte_addr, src);
  if (ret >= 0)
    {
      sd1_restore_tran_clock();
      return ret;
    }

  /* CRC: sampling glitch → drop the clock.  Timeout: DAT0-busy / aborted
   * program; downclocking does not help and the old "stay at 6 MHz" path
   * made sequential LittleFS flushes crawl.
   */

  if (g_sd_diag.wr_crc > crc0)
    {
      sd1_set_clock(SF32LB_SD_RETRY_CLOCK_HZ);
      g_sd_bus_hz = SF32LB_SD_RETRY_CLOCK_HZ;
    }

  for (t = 1; t <= SF32LB_SD_IO_RETRIES; t++)
    {
      g_sd_diag.wr_retry++;
      sd1_recover_after_error();
      ret = sf32lb_sd_write_block(card_byte_addr, src);
      if (ret >= 0)
        {
          break;
        }

      if (g_sd_diag.wr_crc > crc0)
        {
          sd1_set_clock(SF32LB_SD_RETRY_CLOCK_HZ);
          g_sd_bus_hz = SF32LB_SD_RETRY_CLOCK_HZ;
        }
    }

  if (ret >= 0)
    {
      sd1_restore_tran_clock();
      syslog(LOG_WARNING,
             "SD: write addr=0x%llx recovered clk=%u kHz try=%d sr=0x%lx "
             "cmd_to=%lu wr_to=%lu wr_crc=%lu dat0_to=%lu\n",
             (unsigned long long)card_byte_addr,
             (unsigned)(g_sd_bus_hz / 1000), t,
             (unsigned long)g_sd_last_sr,
             (unsigned long)g_sd_diag.cmd_timeout,
             (unsigned long)g_sd_diag.wr_timeout,
             (unsigned long)g_sd_diag.wr_crc,
             (unsigned long)g_sd_diag.dat0_to);
    }
  else
    {
      g_sd_diag.wr_fail++;
      /* 2026-09-25（口径：可恢复异常→WARN）：后面紧跟重放/重识别，数据不丢、卡片能
       * 自己醒过来 ⇒ 降为 WARNING。真正的终态失败（`SD: declared DEAD`、
       * `LFS: mounted READ-ONLY`）仍是 ERROR。 */
      syslog(LOG_WARNING,
             "SD: write addr=0x%llx FAILED after %d tries "
             "(rca=0x%04x clk=%u kHz sr=0x%lx)\n",
             (unsigned long long)card_byte_addr, SF32LB_SD_IO_RETRIES + 1,
             g_sd_rca, (unsigned)(g_sd_bus_hz / 1000),
             (unsigned long)g_sd_last_sr);
      sf32lb_sd_diag_dump("write-fail");
      if (sd1_reinit_card() == 0)
        {
          ret = sf32lb_sd_write_block(card_byte_addr, src);
          if (ret >= 0)
            {
              sd1_restore_tran_clock();
              syslog(LOG_WARNING,
                     "SD: write addr=0x%llx recovered after re-identify\n",
                     (unsigned long long)card_byte_addr);
            }
        }
    }

  return ret;
}

/**
 * @brief CMD12 STOP_TRANSMISSION（R1b）。
 * @details CMD18/CMD25 之后必须发；CMD17/CMD24 超时若把卡留在 DATA 也要发。
 */
static int sf32lb_sd_cmd12(void)
{
  uint8_t r = sd1_send_cmd(12, 0);

  (void)sd1_wait_card_idle();
  (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
  return (r == SD_SUCCESS) ? OK : -EIO;
}

/* HAL_GetTick()/uwTick is not advanced under NuttX, so HAL_DMA_PollForTransfer
 * cannot time out.  Wait on the TC flag with our own clock, then let HAL
 * do channel-unlock cleanup (Timeout 0 is fine once TC is already set).
 */

static uint8_t sf32lb_sd_dma_wait(FAR DMA_HandleTypeDef *hdma, bool is_write,
                                 uint64_t timeout_us)
{
  uint32_t tc;
  uint32_t te;
  uint32_t err_mask = SD_SR_DATA_TIMEOUT | SD_SR_DATA_CRC |
                      SD_SR_STARTBIT_ERROR |
                      (is_write ? SD_SR_FIFO_UNDERRUN : SD_SR_FIFO_OVERRUN);
  uint64_t deadline = sd1_mono_us() + timeout_us;

  tc = DMA_FLAG_TC1 << (hdma->ChannelIndex & 0x1cU);
  te = DMA_FLAG_TE1 << (hdma->ChannelIndex & 0x1cU);

  while ((hdma->DmaBaseAddress->ISR & tc) == 0)
    {
      if ((hdma->DmaBaseAddress->ISR & te) != 0)
        {
          (void)HAL_DMA_Abort(hdma);
          if (is_write)
            {
              g_sd_diag.wr_crc++;
            }
          else
            {
              g_sd_diag.rd_crc++;
            }

          return SD_CRCERR;
        }

      if ((hwp_sdmmc1->SR & err_mask) != 0)
        {
          (void)HAL_DMA_Abort(hdma);
          if ((hwp_sdmmc1->SR & SD_SR_DATA_TIMEOUT) != 0)
            {
              if (is_write)
                {
                  g_sd_diag.wr_timeout++;
                }
              else
                {
                  g_sd_diag.rd_timeout++;
                }

              return SD_TIMEOUT;
            }

          if (is_write)
            {
              g_sd_diag.wr_crc++;
            }
          else
            {
              g_sd_diag.rd_crc++;
            }

          return SD_CRCERR;
        }

      if (sd1_mono_us() >= deadline)
        {
          (void)HAL_DMA_Abort(hdma);
          if (is_write)
            {
              g_sd_diag.wr_timeout++;
            }
          else
            {
              g_sd_diag.rd_timeout++;
            }

          return SD_TIMEOUT;
        }
    }

  if (HAL_DMA_PollForTransfer(hdma, HAL_DMA_FULL_TRANSFER, 0) != HAL_OK)
    {
      (void)HAL_DMA_Abort(hdma);
      if (is_write)
        {
          g_sd_diag.wr_timeout++;
        }
      else
        {
          g_sd_diag.rd_timeout++;
        }

      return SD_TIMEOUT;
    }

  return SD_SUCCESS;
}

/* Mirror of rthw_sdio_send_command() in drv_sdio.c for one data command:
 *   CLR_DATA_CTRL, SET_DATALEN(bytes), SET_DATA_CTRL,
 *   DATA_START only for reads before CMD; writes start DMA first, send CMD,
 *   DATA_START on CMD response (official CMDREND IRQ), then PollForTransfer,
 *   wait DATA_BUSY (TXACT) and DAT0.  CMD12 when nblocks > 1.
 *
 * cmd: 17/18 read, 24/25 write.  This MCU's SDMMC has no working PIO
 * multi-block path; DMA is mandatory in the SDK (#error without it).
 */

static int sf32lb_sd_rt_xfer(bool is_write, uint64_t card_byte_addr,
                             FAR uint8_t *buf, uint32_t nblocks)
{
  FAR DMA_HandleTypeDef *hdma = &g_sd_dmah;
  uint32_t nbytes = nblocks * SD_HW_BLOCK_SIZE;
  uint32_t cmd_arg;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r4;
  uint32_t dcr;
  uint32_t dir;
  uint8_t cmd;
  uint8_t rsp_idx;
  uint8_t cmd_result;
  uint64_t dma_to;
#if SF32LB_SD_PROFILE
  uint64_t t0 = sf32lb_sd_now_us();
#endif

  if (nblocks == 0 || nblocks > SF32LB_SD_CMD18_MAX_BLOCKS)
    {
      return -EINVAL;
    }

  cmd = is_write ? (nblocks > 1 ? 25 : 24) : (nblocks > 1 ? 18 : 17);
  dir = is_write ? 0 : SD_DCR_R_WN;
  dma_to = is_write ? SF32LB_SD_WRITE_WAIT_US : 200000ull;

  hwp_sdmmc1->DCR = 0;
  (void)sd1_wait_card_idle();
  if (sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US) != SD_SUCCESS)
    {
      return -EIO;
    }

  if (is_write)
    {
      memcpy(g_sd_mblkbuf, buf, nbytes);
      up_clean_dcache((uintptr_t)g_sd_mblkbuf,
                      (uintptr_t)g_sd_mblkbuf + nbytes);
    }
  else
    {
      up_invalidate_dcache((uintptr_t)g_sd_mblkbuf,
                           (uintptr_t)g_sd_mblkbuf + nbytes);
    }

  hwp_sdmmc1->DCR = 0;
  hwp_sdmmc1->SR = 0xffffffff;
  hwp_sdmmc1->DLR = nbytes - 1;
  dcr = ((SD_HW_BLOCK_SIZE - 1) << SD_DCR_BLOCK_SIZE_Pos);
  dcr |= (SF32LB_SD_WIRE_MODE << SD_DCR_WIRE_MODE_Pos) | dir;
  dcr |= SD_DCR_TRAN_DATA_EN;
  hwp_sdmmc1->DCR = dcr;
  if (!is_write)
    {
      hwp_sdmmc1->DCR |= SD_DCR_DATA_START;
    }

  if (sf32lb_sd_dma_prepare(is_write) != OK)
    {
      goto stop;
    }

  if (is_write)
    {
      if (HAL_DMA_Start(hdma, (uint32_t)g_sd_mblkbuf,
                        (uint32_t)&hwp_sdmmc1->FIFO, nbytes / 4) != HAL_OK)
        {
          goto stop;
        }
    }
  else if (HAL_DMA_Start(hdma, (uint32_t)&hwp_sdmmc1->FIFO,
                         (uint32_t)g_sd_mblkbuf, nbytes / 4) != HAL_OK)
    {
      goto stop;
    }

  cmd_arg = sf32lb_sd_cmd_arg(card_byte_addr);
  cmd_result = sd1_send_cmd(cmd, cmd_arg);
  if (cmd_result != SD_SUCCESS)
    {
      (void)HAL_DMA_Abort(hdma);
      goto stop;
    }

  sd1_get_rsp(&rsp_idx, &r1, &r2, &r3, &r4);
  if (rsp_idx != cmd)
    {
      (void)HAL_DMA_Abort(hdma);
      goto stop;
    }

  if (is_write)
    {
      if ((r1 & SF32LB_SD_R1_STATE_MASK) == SF32LB_SD_R1_STATE_PRG)
        {
          (void)HAL_DMA_Abort(hdma);
          g_sd_last_sr = r1;
          (void)sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US);
          goto stop;
        }

      /* Official drv_sdio.c: DATA_START in the CMDREND IRQ, after the
       * card accepted CMD24/CMD25.  Starting the data phase before the
       * response left the card in the data state. */
      hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
      hwp_sdmmc1->DCR |= SD_DCR_DATA_START;
    }

  cmd_result = sf32lb_sd_dma_wait(hdma, is_write, dma_to);
  if (cmd_result != SD_SUCCESS)
    {
      goto stop;
    }

  if (is_write)
    {
      cmd_result = sd1_wait_write();
      if (cmd_result != SD_SUCCESS)
        {
          goto stop;
        }
    }

  (void)sd1_wait_card_idle();
  if (nblocks > 1)
    {
      if (sf32lb_sd_cmd12() < 0)
        {
          sd1_recover_after_error();
          return -EIO;
        }

      (void)sd1_wait_card_idle();
      if (is_write &&
          sd1_wait_dat0(SF32LB_SD_WRITE_MAX_US) != SD_SUCCESS)
        {
          sd1_recover_after_error();
          return -EIO;
        }
    }

  if (!is_write)
    {
      up_invalidate_dcache((uintptr_t)g_sd_mblkbuf,
                           (uintptr_t)g_sd_mblkbuf + nbytes);
      memcpy(buf, g_sd_mblkbuf, nbytes);
      if (nblocks > 1)
        {
          g_sd_diag.cmd18_ok++;
        }
    }
  else if (nblocks > 1)
    {
      g_sd_diag.cmd25_ok++;
    }

#if SF32LB_SD_PROFILE
  if (is_write)
    {
      g_sd_stats.hw_write += nblocks;
      g_sd_stats.hw_write_us += sf32lb_sd_now_us() - t0;
    }
  else
    {
      g_sd_stats.hw_read += nblocks;
      g_sd_stats.hw_read_us += sf32lb_sd_now_us() - t0;
    }
#endif
  return OK;

stop:
  hwp_sdmmc1->DCR = 0;
  (void)HAL_DMA_Abort(hdma);
  (void)sf32lb_sd_cmd12();
  sd1_recover_after_error();
  return -EIO;
}

static int sf32lb_sd_rt_xfer_retry(bool is_write, uint64_t card_byte_addr,
                                   FAR uint8_t *buf, uint32_t nblocks)
{
  int ret;
  int t;
  uint8_t cmd = is_write ? (nblocks > 1 ? 25 : 24) :
                (nblocks > 1 ? 18 : 17);

  /* 判死闸门必须装在**所有**会走"4 次重试 + 重识别"的入口上。LittleFS 的
   * 多块读走的是这条路（日志里的 `SD: CMD17/CMD18 … FAILED after 4 tries`），
   * 只装在 read/write_block_retry 上是不够的 —— 现场就是判死之后照样每块
   * 卡 2~4 秒，因为真正的调用者从这里进来。 */
  if (sd1_dead_fail_fast())
    {
      return -EIO;
    }

  ret = sf32lb_sd_rt_xfer(is_write, card_byte_addr, buf, nblocks);
  if (ret >= 0)
    {
      sd1_restore_tran_clock();
      return ret;
    }

  for (t = 1; t <= SF32LB_SD_IO_RETRIES; t++)
    {
      if (is_write)
        {
          g_sd_diag.wr_retry++;
        }
      else
        {
          g_sd_diag.rd_retry++;
        }

      sd1_recover_after_error();
      ret = sf32lb_sd_rt_xfer(is_write, card_byte_addr, buf, nblocks);
      if (ret >= 0)
        {
          sd1_restore_tran_clock();
          syslog(LOG_WARNING,
                 "SD: CMD%u addr=0x%llx n=%lu recovered at %u kHz "
                 "(try %d)\n",
                 cmd, (unsigned long long)card_byte_addr,
                 (unsigned long)nblocks,
                 (unsigned)(g_sd_bus_hz / 1000), t);
          return ret;
        }
    }

  sd1_set_clock(SF32LB_SD_RETRY_CLOCK_HZ);
  g_sd_bus_hz = SF32LB_SD_RETRY_CLOCK_HZ;
  sd1_recover_after_error();
  ret = sf32lb_sd_rt_xfer(is_write, card_byte_addr, buf, nblocks);
  if (ret >= 0)
    {
      sd1_restore_tran_clock();
      syslog(LOG_WARNING,
             "SD: CMD%u addr=0x%llx n=%lu recovered at 6 MHz then restored\n",
             cmd, (unsigned long long)card_byte_addr,
             (unsigned long)nblocks);
      return ret;
    }

  if (is_write)
    {
      if (nblocks > 1)
        {
          g_sd_diag.cmd25_fail++;
        }
      else
        {
          g_sd_diag.wr_fail++;
        }
    }
  else if (nblocks > 1)
    {
      g_sd_diag.cmd18_fail++;
    }
  else
    {
      g_sd_diag.rd_fail++;
    }

  /* 2026-09-25（口径：可恢复异常→WARN）：下一行就是 `sd1_reinit_card()` + 原样重放，
   * 实测卡片能恢复、数据不丢 ⇒ 降为 WARNING。终态失败仍由 `SD: declared DEAD` /
   * `LFS: mounted READ-ONLY` 以 ERROR 报出。 */
  syslog(LOG_WARNING,
         "SD: CMD%u addr=0x%llx n=%lu FAILED after %d tries (rca=0x%04x sr=0x%08lx why=%s)\n",
         cmd, (unsigned long long)card_byte_addr, (unsigned long)nblocks,
         SF32LB_SD_IO_RETRIES + 1, g_sd_rca,
         (unsigned long)g_sd_last_sr, sd1_sr_why(g_sd_last_sr));
  if (sd1_reinit_card() == 0)
    {
      ret = sf32lb_sd_rt_xfer(is_write, card_byte_addr, buf, nblocks);
      if (ret >= 0)
        {
          sd1_restore_tran_clock();
          syslog(LOG_WARNING,
                 "SD: CMD%u addr=0x%llx n=%lu recovered after re-identify\n",
                 cmd, (unsigned long long)card_byte_addr,
                 (unsigned long)nblocks);
        }
    }

  return ret;
}

static int sf32lb_sd_read_region(uint64_t byte_off, FAR uint8_t *buf,
                                 uint32_t len)
{
  int ret;

  while (len >= SD_HW_BLOCK_SIZE)
    {
      uint32_t n = len / SD_HW_BLOCK_SIZE;
      uint32_t i;

      if (n > SF32LB_SD_CMD18_MAX_BLOCKS)
        {
          n = SF32LB_SD_CMD18_MAX_BLOCKS;
        }

      if (g_sd_dma)
        {
          ret = sf32lb_sd_rt_xfer_retry(false, byte_off, buf, n);
          if (ret < 0)
            {
              /* 判死期间 rt_xfer_retry 是**故意**直接 -EIO 的，不是 DMA 出错，
               * 所以既不该降级重试也不该刷 "fallback" 警告 —— 卡都没了，
               * 每读一个 region 打一行会把日志淹掉（现场就是这样）。 */
              if (g_sd_dead)
                {
                  return ret;
                }

              syslog(LOG_WARNING,
                     "SD: DMA read fallback to PIO CMD17 n=%lu\n",
                     (unsigned long)n);
              for (i = 0; i < n; i++)
                {
                  ret = sf32lb_sd_read_block_retry(
                      byte_off + i * SD_HW_BLOCK_SIZE,
                      buf + i * SD_HW_BLOCK_SIZE);
                  if (ret < 0)
                    {
                      return ret;
                    }
                }
            }
        }
      else
        {
          n = 1;
          ret = sf32lb_sd_read_block_retry(byte_off, buf);
          if (ret < 0)
            {
              return ret;
            }
        }

      byte_off += n * SD_HW_BLOCK_SIZE;
      buf += n * SD_HW_BLOCK_SIZE;
      len -= n * SD_HW_BLOCK_SIZE;
    }

  if (len > 0)
    {
      /* All MTD accesses are page_size (2048) aligned, so this should not be
       * reached; handle it defensively via the staging block.
       */

      ret = sf32lb_sd_read_block_retry(byte_off, (FAR uint8_t *)g_sd_blkbuf);
      if (ret < 0)
        {
          return ret;
        }

      memcpy(buf, g_sd_blkbuf, len);
    }

  return OK;
}

static int sf32lb_sd_write_region(uint64_t byte_off, FAR const uint8_t *buf,
                                  uint32_t len)
{
  int ret;

  /* Official drv_sdio.c: DMAC memory→FIFO, DATA_START after CMD response.
   * PIO CMD24 remains the fallback if DMA leaves the card in a data state. */

  while (len >= SD_HW_BLOCK_SIZE)
    {
      uint32_t n = len / SD_HW_BLOCK_SIZE;
      uint32_t i;

      if (n > SF32LB_SD_CMD18_MAX_BLOCKS)
        {
          n = SF32LB_SD_CMD18_MAX_BLOCKS;
        }

      if (g_sd_dma)
        {
          ret = sf32lb_sd_rt_xfer_retry(true, byte_off,
                                        (FAR uint8_t *)(uintptr_t)buf, n);
          if (ret < 0)
            {
              /* 同读侧：判死是 rt_xfer_retry 故意返回的，不降级也不刷警告。 */
              if (g_sd_dead)
                {
                  return ret;
                }

              syslog(LOG_WARNING,
                     "SD: DMA write fallback to PIO CMD24 n=%lu\n",
                     (unsigned long)n);
              for (i = 0; i < n; i++)
                {
                  ret = sf32lb_sd_write_block_retry(
                      byte_off + i * SD_HW_BLOCK_SIZE,
                      buf + i * SD_HW_BLOCK_SIZE);
                  if (ret < 0)
                    {
                      return ret;
                    }
                }
            }
        }
      else
        {
          n = 1;
          ret = sf32lb_sd_write_block_retry(byte_off, buf);
          if (ret < 0)
            {
              return ret;
            }
        }

      byte_off += n * SD_HW_BLOCK_SIZE;
      buf += n * SD_HW_BLOCK_SIZE;
      len -= n * SD_HW_BLOCK_SIZE;
    }

  return OK;
}

/****************************************************************************
 * MTD callbacks
 ****************************************************************************/

/* Hard guard: an access [rel_off, rel_off+len) must stay fully inside this
 * MTD window. /dev/sd0 is FS_REGION only; /dev/sdkv is KV_REGION only. Use
 * 64-bit math so the end can't wrap onto boot images.
 */

static bool sf32lb_sd_in_window(uint64_t rel_off, uint64_t len,
                                FAR const struct sf32lb_sd_dev_s *priv)
{
  return (len > 0) && (rel_off + len <= priv->byte_limit);
}

static void sf32lb_sd_bus_lock(bool kv_prio)
{
  if (kv_prio)
    {
      atomic_add(&g_sd_kv_waiters, 1);
      nxmutex_lock(&g_sd_lock);
      atomic_sub(&g_sd_kv_waiters, 1);
      return;
    }

  nxmutex_lock(&g_sd_lock);
  while (atomic_read(&g_sd_kv_waiters) > 0)
    {
      nxmutex_unlock(&g_sd_lock);
      sched_yield();
      nxmutex_lock(&g_sd_lock);
    }
}

static void sf32lb_sd_bus_unlock(void)
{
  nxmutex_unlock(&g_sd_lock);
}

/* First mkdir / O_CREAT after mounting a packed volume runs lfs_alloc()
 * and can walk the tree for seconds with no sched_yield. bringup is prio
 * 240 > iwdg_feed 180, so work_beat timestamps never become KEEPALIVE.
 * Pet the IWDT register here (lock already dropped). Do not usleep:
 * that 10 ms/s on every bread also runs during map/MTP and let lcd_hw
 * Init at 48 MHz before hclk240 (garbled panel). */
static void sf32lb_sd_io_progress(void)
{
  myvendor_watchdog_work_beat();
  myvendor_watchdog_hw_pet();
}

static ssize_t sf32lb_sd_bread(FAR struct mtd_dev_s *dev, off_t startblock,
                               size_t nblocks, FAR uint8_t *buffer)
{
  FAR struct sf32lb_sd_dev_s *priv = (FAR struct sf32lb_sd_dev_s *)dev;
  uint64_t byte_off;
  int ret;

  if (startblock < 0 ||
      !sf32lb_sd_in_window((uint64_t)startblock * priv->page_size,
                           (uint64_t)nblocks * priv->page_size, priv))
    {
      return -EINVAL;
    }

#if SF32LB_SD_PROFILE
  g_sd_stats.bread++;
  g_sd_stats.bread_pages += nblocks;
#endif

  byte_off = (uint64_t)priv->byte_offset +
             (uint64_t)startblock * priv->page_size;
  sf32lb_sd_bus_lock(priv->kv_prio);
  ret = sf32lb_sd_read_region(byte_off, buffer,
                              (uint32_t)nblocks * priv->page_size);
  sf32lb_sd_bus_unlock();
  sf32lb_sd_io_progress();
  return ret < 0 ? ret : (ssize_t)nblocks;
}

static ssize_t sf32lb_sd_bwrite(FAR struct mtd_dev_s *dev, off_t startblock,
                                size_t nblocks, FAR const uint8_t *buffer)
{
  FAR struct sf32lb_sd_dev_s *priv = (FAR struct sf32lb_sd_dev_s *)dev;
  uint64_t byte_off;
  int ret;

  if (startblock < 0 ||
      !sf32lb_sd_in_window((uint64_t)startblock * priv->page_size,
                           (uint64_t)nblocks * priv->page_size, priv))
    {
      return -EINVAL;
    }

#if SF32LB_SD_PROFILE
  g_sd_stats.bwrite++;
  g_sd_stats.bwrite_pages += nblocks;
#endif

  byte_off = (uint64_t)priv->byte_offset +
             (uint64_t)startblock * priv->page_size;
  sf32lb_sd_bus_lock(priv->kv_prio);
  ret = sf32lb_sd_write_region(byte_off, buffer,
                               (uint32_t)nblocks * priv->page_size);
  sf32lb_sd_bus_unlock();
  sf32lb_sd_io_progress();
  return ret < 0 ? ret : (ssize_t)nblocks;
}

static int sf32lb_sd_erase(FAR struct mtd_dev_s *dev, off_t startblock,
                           size_t nblocks)
{
  FAR struct sf32lb_sd_dev_s *priv = (FAR struct sf32lb_sd_dev_s *)dev;
  uint64_t byte_off;
  uint32_t nbytes;
  int ret = OK;

  if (startblock < 0 ||
      !sf32lb_sd_in_window((uint64_t)startblock * priv->erase_size,
                           (uint64_t)nblocks * priv->erase_size, priv))
    {
      return -EINVAL;
    }

  /* SD/eMMC has no hardware erase, but LittleFS relies on an erased block
   * reading back as 0xFF (its metadata-log scan stops at the first non-CRC /
   * erased entry). Stale data would let an old commit validate and corrupt the
   * FS, so fill the erase block with 0xFF — matching scripts/mklfs_disk and the
   * NAND erase semantics.
   */

#if SF32LB_SD_PROFILE
  g_sd_stats.erase_blocks += nblocks;
#endif

  byte_off = (uint64_t)priv->byte_offset +
             (uint64_t)startblock * priv->erase_size;
  nbytes = (uint32_t)nblocks * priv->erase_size;

  sf32lb_sd_bus_lock(priv->kv_prio);
  memset(g_sd_blkbuf, 0xff, sizeof(g_sd_blkbuf));
  while (nbytes >= SD_HW_BLOCK_SIZE)
    {
      int t;

      for (t = 0, ret = -EIO; t <= SF32LB_SD_IO_RETRIES; t++)
        {
          ret = sf32lb_sd_program_staged(byte_off);
          if (ret >= 0)
            {
              break;
            }

          sd1_recover_after_error();
        }

      if (ret < 0)
        {
          break;
        }

      byte_off += SD_HW_BLOCK_SIZE;
      nbytes -= SD_HW_BLOCK_SIZE;
    }

  sf32lb_sd_bus_unlock();
  sf32lb_sd_io_progress();
  return ret < 0 ? ret : (int)nblocks;
}

static int sf32lb_sd_ioctl(FAR struct mtd_dev_s *dev, int cmd,
                           unsigned long arg)
{
  FAR struct sf32lb_sd_dev_s *priv = (FAR struct sf32lb_sd_dev_s *)dev;
  int ret = -EINVAL;

  switch (cmd)
    {
      case MTDIOC_GEOMETRY:
        {
          FAR struct mtd_geometry_s *geo =
            (FAR struct mtd_geometry_s *)((uintptr_t)arg);

          if (geo != NULL)
            {
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
              info->numsectors = (size_t)(priv->byte_limit / priv->page_size);
              info->sectorsize = priv->page_size;
              info->startsector = (off_t)(priv->byte_offset / priv->page_size);
              info->parent[0] = '\0';
              ret = OK;
            }
        }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Re-derive the SD1 divider after HCLK changed. Without this the card would
 * keep running at HCLK/old_div — e.g. 40 MHz once bringup raises HCPU to
 * 240 MHz, well past the 25 MHz SD default-speed limit.
 */

/* apply_locked() 用它把 SD 时钟夹住：clk_hold 取总线锁 + 停钟，reclock 按新
 * HCLK 重设分频、放钟并解锁。中间那次 ConfigHCLK 期间卡上无时钟。 */
static bool g_sd_clk_held;
static uint32_t g_sd_div_held_before = 1u;
static uint32_t g_sd_hold_wait_ms;   /* 等总线锁花了多久 */

void sf32lb_sd_clk_hold(void)
{
  uint64_t t0;

  if (!g_sd_initialized)
    {
      return;
    }

  t0 = sd1_mono_us();
  nxmutex_lock(&g_sd_lock);
  g_sd_hold_wait_ms = (uint32_t)((sd1_mono_us() - t0) / 1000ull);
  g_sd_div_held_before =
    ((hwp_sdmmc1->CLKCR & SD_CLKCR_DIV_Msk) >> SD_CLKCR_DIV_Pos) + 1u;
  g_sd_clk_held = true;
  hwp_sdmmc1->CLKCR = SD_CLKCR_STOP_CLK;
}

/* 比 sd1_hw_quiet 更狠一档：按序停钟之后把接口时钟域也关掉并保持一段
 * 时间，让 CMD/DAT 上的持续驱动彻底消失，再重新识别。这是掉电之外我们
 * 唯一能做的、更接近冷启动的动作。调用方须已持有 g_sd_lock。
 */
static int sd1_hard_reset(void)
{
  syslog(LOG_WARNING, "SD: hard reset #%lu (interface down %u ms)\n",
         (unsigned long)g_sd_dead_tries,
         (unsigned)SF32LB_SD_HARD_DOWN_MS);

  sd1_hw_quiet();
  hwp_hpsys_rcc->ENR2 &= ~HPSYS_RCC_ENR2_SDMMC1;
  HAL_Delay_us((uint32_t)SF32LB_SD_HARD_DOWN_MS * 1000u);
  hwp_hpsys_rcc->ENR2 |= HPSYS_RCC_ENR2_SDMMC1;

  return sf32lb_sd_identify();
}

/**
 * @brief 判死后的快速失败闸门，顺带按周期做一次硬复位复活尝试。
 * @return true 表示调用方应立刻返回 -EIO；false 表示可以继续正常 I/O。
 * @note 调用方须已持有 `g_sd_lock`（复活尝试里的 identify 也要用它）。
 */
static bool sd1_dead_fail_fast(void)
{
  uint64_t now;

  if (!g_sd_dead)
    {
      return false;
    }

  now = sd1_mono_us();
  if ((now - g_sd_dead_try_us) < (uint64_t)SF32LB_SD_DEAD_RETRY_MS * 1000ull)
    {
      return true;
    }

  g_sd_dead_try_us = now;
  g_sd_dead_tries++;

  if (sd1_hard_reset() == 0)
    {
      g_sd_dead = false;
      g_sd_dead_mute = 0;
      sd1_set_clock(SF32LB_SD_RETRY_CLOCK_HZ);
      g_sd_bus_hz = SF32LB_SD_RETRY_CLOCK_HZ;
      syslog(LOG_WARNING,
             "SD: RECOVERED by hard reset #%lu (was dead %llu ms)\n",
             (unsigned long)g_sd_dead_tries,
             (unsigned long long)((now - g_sd_dead_since_us) / 1000ull));
      return false;
    }

  syslog(LOG_WARNING, "SD: still dead after hard reset #%lu\n",
         (unsigned long)g_sd_dead_tries);
  return true;
}

/* MPI/SD 那路 mux 的源名。取值见 RCC_CLK_MPI_SD_*：0=SYSCLK 2=DLL2 3=DLL3。
 * 注意这一路上**没有 48M 固定时钟**可选（48M 只进 PERI 域的 I2C/SPI/USART）。
 */
static const char *sd_clk_src_name(uint32_t sel)
{
  switch (sel)
    {
      case RCC_CLK_MPI_SD_SYSCLK:
        return "SYSCLK";
      case RCC_CLK_MPI_SD_DLL2:
        return "DLL2";
      case RCC_CLK_MPI_SD_DLL3:
        return "DLL3";
      default:
        return "?";
    }
}

/**
 * @brief 三个状态判一遍：卡能不能读、两个 LFS 能不能用。
 *
 * @details 判据分两层，**且第二层只在第一层报了异常时才升档**：
 *
 *          **第一层"卡还能不能读"用 FAT 探** —— FAT 和两个 LFS 在同一张卡上，
 *          所以"FAT 还能读"就是"卡没坏"的**独立证据**。这比看 g_sd_dead 强：
 *          g_sd_dead 是 SD 层自己的意见（而且只在 3 次重识别失败后才置起，
 *          中间有窗口），FAT 探测是直接测量。
 *
 *          **第二层"LFS 能不能用"常态用 stat 探，异常才升到 statfs(df) 探** ——
 *          常态（两个挂载点都 stat 得到）直接判健康，**一整个 tick 零遍历**。
 *          只有 stat 已经读不出来时才做 df 口径确认，理由和代价见下面两个
 *          helper 的注释（statfs 在 LFS 上是 O(文件系统)，不能拿来当心跳）。
 *
 *          三种组合对应三种处置，混在一起会做错事：
 *            - FAT 读不到            → 卡的问题，SD 层负责（判死 + 硬复位轮询），
 *                                      别去重挂 LFS，那是白费；
 *            - FAT 能读、LFS 读不到  → **卡是好的，坏的是 LFS 实例** → 该重挂；
 *            - 都读得到              → 健康。
 */
/**
 * @brief 常态探针：只 `stat()` 挂载点根，不做任何全盘遍历。
 *
 * @details `stat` 在 LFS 上命中根 inode（读根元数据对），在 FAT 上读根目录簇，
 *          都是 O(1)，而且都吃块缓存 —— 这正是 mount 之后那次"回读验证"用的
 *          手法（见 sf32lb_sd_mount_littlefs 里的 stat 校验）。它直接回答
 *          "挂载点还在、还读得出来"，而这**正是 diag 要的判据**；
 *          "用了多少块"是 `df` 的问题，不是 diag 的问题。
 *
 *          **为什么不能拿 statfs 当常态探针**：`littlefs_statfs()` 会调
 *          `lfs_fs_size()`（nuttx/fs/littlefs/lfs_vfs.c），它要**遍历整棵元数据
 *          树**才能数出已用块，且**完全不缓存**。diag 每 500 ms 判一次
 *          （DIAG_TICK_MS），一次判两个 LFS 挂载点 —— 实测每 tick 烧掉约 110 ms
 *          （ps 里 22.3% × 500 ms），全记在 diag 头上。
 *          FAT 那路反倒只有挂载后第一次会全表扫描：`fat_nfreeclusters()` 之后
 *          靠 `fs_fsifreecount` 走 RAM，就不是瓶颈了。
 *
 *          **这不是新规矩**：本仓早就把"在 LFS 卷上 statfs"列为禁止项 ——
 *          见 companion_fs.c 的 handle_statfs()（原话："walks every LittleFS
 *          metadata block on this volume and can take minutes. Refuse rather
 *          than stall the radio."）、transfer_backend.h 的卷说明、test_sd.c
 *          的告警。sf32lb_sd_fs_ok() 是这条规矩的**漏网之鱼**，现在补上。
 */
static bool sd_fs_mount_alive(FAR const char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

/**
 * @brief 深探：`df` 口径（statfs），**只在常态探针已经报异常时才跑**。
 *
 * @details 它是异常时的取证手段，不是常态判据：代价是 O(文件系统)
 *          （LFS 遍历元数据树；FAT 在 FSInfo 缺失时全表扫描）。用处是把
 *          "卡坏了"和"LFS 实例坏了"分开 —— 这两者的处置完全不同（见下）。
 */
static bool sd_fs_df_ok(void)
{
  struct statfs st;

  return statfs(SF32LB_SD_MOUNTPOINT, &st) == 0 &&
         statfs(SF32LB_SD_KV_MOUNTPOINT, &st) == 0;
}

bool sf32lb_sd_fs_ok(void)
{
  if (!g_sd_initialized)
    {
      return true;
    }

  if (!sd_fs_mount_alive(SF32LB_SD_FAT_MOUNTPOINT))
    {
      return true;   /* 卡的问题，不归 FS 层管 */
    }

  if (sd_fs_mount_alive(SF32LB_SD_MOUNTPOINT) &&
      sd_fs_mount_alive(SF32LB_SD_KV_MOUNTPOINT))
    {
      return true;   /* 常态出口：本 tick 不做任何全盘遍历 */
    }

  /* 走到这里说明 LFS 的 stat 已经读不出来了。可能只是瞬时（缓存/路径层），
   * 也可能是实例真坏了；用 df 口径确认一次再决定要不要重挂 —— 重挂会断开
   * 正在用的文件，代价远大于这一次深扫，所以宁可多花这一下。 */
  return sd_fs_df_ok();
}

/**
 * @brief 把坏掉的 LFS 挂载点重新挂上（哪个坏挂哪个）。
 *
 * @details 只在"**卡还能读**但 LFS 用不了"时才动手 —— 门就是用 FAT 探的，
 *          理由同上：FAT 与 LFS 同卡，FAT 能读说明卡没坏，那问题就在 LFS
 *          实例上（硬复位救回卡之后 LittleFS 的 RAM 内状态不会自愈，必须重挂）。
 *          卡读不到时直接返回，把舞台留给 SD 层的判死/硬复位。
 *
 *          两个坑，都踩过：
 *          1. /mnt/kv **不能**直接重调 sf32lb_sd_mount_kv()——它开头
 *             `if (g_sd_kv_mtd != NULL) return OK;` 会立刻返回成功却根本不重挂。
 *             所以这里走 umount + 按设备名 mount，不重复注册 MTD。
 *          2. kv 的开机挂载带 "autoformat"；恢复路径上**刻意不传**，而且
 *             按"LFS 永不格式化"的要求，开机那条也要去掉（见 mount_kv）。
 *
 *          不要在持有 SD 总线锁的路径里调用：mount 内部还要走 SD I/O，
 *          g_sd_lock 不可重入，会自锁。只从 diag 线程调。
 *
 * @return 0 全部可用（含本次修好的）；-EAGAIN 卡读不到；负 errno 为失败。
 */
int sf32lb_sd_fs_heal(void)
{
  struct statfs st;
  int ret = 0;
  int one;

  if (statfs(SF32LB_SD_FAT_MOUNTPOINT, &st) != 0)
    {
      syslog(LOG_WARNING, "LFS: heal skipped, card not readable via %s (SD layer owns it)\n",
             SF32LB_SD_FAT_MOUNTPOINT);
      return -EAGAIN;
    }

  if (statfs(SF32LB_SD_MOUNTPOINT, &st) != 0)
    {
      syslog(LOG_WARNING, "LFS: remount %s (card readable, FS is not)\n",
             SF32LB_SD_MOUNTPOINT);
      (void)umount(SF32LB_SD_MOUNTPOINT);
      one = sf32lb_sd_mount_littlefs(0);
      syslog(one == 0 ? LOG_WARNING : LOG_ERR,
             "LFS: remount %s ret=%d\n", SF32LB_SD_MOUNTPOINT, one);
      if (one != 0)
        {
          ret = one;
        }
    }

  if (statfs(SF32LB_SD_KV_MOUNTPOINT, &st) != 0)
    {
      syslog(LOG_WARNING, "LFS: remount %s (card readable, FS is not)\n",
             SF32LB_SD_KV_MOUNTPOINT);
      (void)umount(SF32LB_SD_KV_MOUNTPOINT);
      one = mount(SF32LB_SD_KV_DEV, SF32LB_SD_KV_MOUNTPOINT, "littlefs",
                  0, NULL);
      syslog(one == 0 ? LOG_WARNING : LOG_ERR,
             "LFS: remount %s ret=%d errno=%d\n",
             SF32LB_SD_KV_MOUNTPOINT, one, errno);
      if (one != 0 && ret == 0)
        {
          ret = -errno;
        }
    }

  return ret;
}

/**
 * @brief 只读打印 SD 时钟拓扑，用来确认 SD 到底挂在哪条时钟分支上。
 *
 * @details 背景：52x 的 HPSYS_RCC_CSR 里没有 SEL_SDMMC（只有 SEL_MPI1/2、
 *          SEL_PERI/TICK/USBC），但硬件图上 SD 和 MPI 共用 `CG MPI1/2`
 *          那一路时钟门。所以"SD 的父时钟是谁"要靠寄存器现场判断：
 *            - sel_mpi1/sel_mpi2 现在选的是 SYSCLK 还是 DLL2/DLL3；
 *            - CLKCR 的 DIV 反推出来的 SDCLK 是否等于 bus_hz。
 *          本函数**只读**，不动任何寄存器，也不改任何状态。
 *
 *          注意：这块板子上 HCLK == SYSCLK，所以"挂在 HCLK"和"挂在这条
 *          mux 输出上"从频率上分辨不出来 —— 必须直接读 CSR。
 */
void sf32lb_sd_clk_probe(void)
{
  uint32_t csr = hwp_hpsys_rcc->CSR;
  uint32_t enr2 = hwp_hpsys_rcc->ENR2;
  uint32_t clkcr = hwp_sdmmc1->CLKCR;
  uint32_t div = ((clkcr & SD_CLKCR_DIV_Msk) >> SD_CLKCR_DIV_Pos) + 1u;
  uint32_t hclk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
  uint32_t sc = HAL_RCC_GetSysCLKFreq(CORE_ID_HCPU);

  syslog(LOG_INFO, "SDCLK: CSR=0x%08lx mpi1=%lu(%s) mpi2=%lu(%s) usbc=%lu peri=%lu tick=%lu\n",
         (unsigned long)csr,
         (unsigned long)((csr & HPSYS_RCC_CSR_SEL_MPI1_Msk) >> HPSYS_RCC_CSR_SEL_MPI1_Pos),
         sd_clk_src_name((csr & HPSYS_RCC_CSR_SEL_MPI1_Msk) >> HPSYS_RCC_CSR_SEL_MPI1_Pos),
         (unsigned long)((csr & HPSYS_RCC_CSR_SEL_MPI2_Msk) >> HPSYS_RCC_CSR_SEL_MPI2_Pos),
         sd_clk_src_name((csr & HPSYS_RCC_CSR_SEL_MPI2_Msk) >> HPSYS_RCC_CSR_SEL_MPI2_Pos),
         (unsigned long)((csr & HPSYS_RCC_CSR_SEL_USBC_Msk) >> HPSYS_RCC_CSR_SEL_USBC_Pos),
         (unsigned long)((csr & HPSYS_RCC_CSR_SEL_PERI_Msk) >> HPSYS_RCC_CSR_SEL_PERI_Pos),
         (unsigned long)((csr & HPSYS_RCC_CSR_SEL_TICK_Msk) >> HPSYS_RCC_CSR_SEL_TICK_Pos));

  syslog(LOG_INFO, "SDCLK: dll2=%lu Hz enr2=0x%08lx sdmmc1_gate=%lu hclk=%lu sysclk=%lu\n",
         (unsigned long)HAL_RCC_HCPU_GetDLL2Freq(), (unsigned long)enr2,
         (unsigned long)((enr2 & HPSYS_RCC_ENR2_SDMMC1_Msk) ? 1u : 0u),
         (unsigned long)hclk, (unsigned long)sc);

  syslog(LOG_INFO, "SDCLK: clkcr=0x%08lx stop=%lu voidfifo=%lu tune=%lu div=%lu -> %lu kHz if parent=HCLK\n",
         (unsigned long)clkcr,
         (unsigned long)((clkcr & SD_CLKCR_STOP_CLK_Msk) ? 1u : 0u),
         (unsigned long)((clkcr & SD_CLKCR_VOID_FIFO_ERROR_Msk) ? 1u : 0u),
         (unsigned long)((clkcr & SD_CLKCR_CLK_TUNE_SEL_Msk) >> SD_CLKCR_CLK_TUNE_SEL_Pos),
         (unsigned long)div, (unsigned long)(hclk / div / 1000u));

  syslog(LOG_INFO, "SDCLK: pcr=0x%08lx cdr=0x%08lx bus_hz=%u kHz rca=0x%04x\n",
         (unsigned long)hwp_sdmmc1->PCR, (unsigned long)hwp_sdmmc1->CDR,
         (unsigned)(g_sd_bus_hz / 1000u), g_sd_rca);
}

void sf32lb_sd_reclock(void)
{
  uint32_t div_before;
  uint32_t div_after;
  bool held;

  if (!g_sd_initialized)
    {
      return;
    }

  held = g_sd_clk_held;
  g_sd_clk_held = false;

  if (!held)
    {
      nxmutex_lock(&g_sd_lock);
      div_before = ((hwp_sdmmc1->CLKCR & SD_CLKCR_DIV_Msk) >> SD_CLKCR_DIV_Pos) + 1u;
    }
  else
    {
      /* 停钟时 DIV 字段读到的是 0，用 clk_hold 存下来的真值。 */
      div_before = g_sd_div_held_before;
    }

  sd1_set_clock(g_sd_bus_hz);  /* 结尾清 STOP_CLK，即放钟 */
  div_after = ((hwp_sdmmc1->CLKCR & SD_CLKCR_DIV_Msk) >> SD_CLKCR_DIV_Pos) + 1u;
  nxmutex_unlock(&g_sd_lock);

  /* 每次 DVFS 跳档都会走到这里，DIV 必变。这行是"跳档 ↔ SD 时钟"的
   * 关联证据：以后再出 SD 失败，可以用它对手上的失败时刻。
   *
   * 只报 wait（等 g_sd_lock 花了多久），**不报 SDCLK 停了多久** ——
   * clk_hold 到 reclock 之间夹着 HAL_RCC_HCPU_ConfigHCLK，而
   * sifli_systick_reclock() 会在中间改 SysTick 的时基，sd1_mono_us() 跨过
   * 那一点测出来的差值是无意义的（现场打出过 1271310319 ms）。wait 全程
   * 在切换之前测量，所以是可信的。
   */
  if (held)
    {
      syslog(LOG_INFO,
             "SD: clk div %lu -> %lu (SDCLK %lu kHz) wait=%u ms\n",
             (unsigned long)div_before, (unsigned long)div_after,
             (unsigned long)(g_sd_bus_hz / 1000u),
             (unsigned)g_sd_hold_wait_ms);
    }
  else if (div_before != div_after)
    {
      syslog(LOG_INFO, "SD: clk div %lu -> %lu (SDCLK %lu kHz)\n",
             (unsigned long)div_before, (unsigned long)div_after,
             (unsigned long)(g_sd_bus_hz / 1000u));
    }
}

static int sf32lb_sd_bringup(void)
{
  int ret;

  if (g_sd_card_ready)
    {
      return OK;
    }

  ret = sf32lb_sd_identify();
  if (ret < 0)
    {
      return ret;
    }

  if (sf32lb_sd_dma_init() < 0)
    {
      syslog(LOG_WARNING, "SD: DMA1 init failed, PIO CMD17/CMD24 only\n");
      g_sd_dma = false;
    }
  else
    {
      syslog(LOG_INFO, "INFO: SD DMA pinned (ch=DMA1_CH3 req=57, "
             "CMD17/CMD18 read, CMD24/CMD25 write, max=%u x %u B)\n",
             SF32LB_SD_CMD18_MAX_BLOCKS, SD_HW_BLOCK_SIZE);
    }

  g_sd_card_ready = true;
  g_sd_initialized = true;
  return OK;
}

/**
 * @brief 把 ptab 给出的窗口钳到卡容量，并向下对齐到 erase。
 *
 * @param byte_offset 卡上起始字节。
 * @param byte_size   ptab 大小；0 或 `0xffffffff` 表示 CSD 余量。
 * @param kv_prio     true 时不受 `SF32LB_SD_LFS_MAX_BYTES` 上限约束。
 * @return 对齐后的窗口字节数；过小则由调用方拒绝。
 */
static uint64_t sf32lb_sd_clamp_window(uint32_t byte_offset, uint32_t byte_size,
                                       bool kv_prio)
{
  uint64_t remaining = 0;
  uint64_t want;
  uint64_t max_blocks;

  if (g_sd_card_bytes > byte_offset)
    {
      remaining = g_sd_card_bytes - byte_offset;
    }

  if (byte_size == 0 || byte_size == 0xffffffffu)
    {
      want = remaining;
    }
  else if (remaining != 0 && (uint64_t)byte_size > remaining)
    {
      syslog(LOG_WARNING,
             "WARN: SD window 0x%lx+0x%lx exceeds card %llu, using remainder "
             "%llu\n",
             (unsigned long)byte_offset, (unsigned long)byte_size,
             (unsigned long long)g_sd_card_bytes,
             (unsigned long long)remaining);
      want = remaining;
    }
  else
    {
      want = byte_size;
    }

#if SF32LB_SD_LFS_MAX_BYTES > 0
  if (!kv_prio && want > SF32LB_SD_LFS_MAX_BYTES)
    {
      syslog(LOG_WARNING,
             "WARN: SD LFS window %llu capped to 0x%x (%u MiB)\n",
             (unsigned long long)want, SF32LB_SD_LFS_MAX_BYTES,
             SF32LB_SD_LFS_MAX_BYTES / (1024u * 1024u));
      want = SF32LB_SD_LFS_MAX_BYTES;
    }
#else
  (void)kv_prio;
#endif

  want -= want % SF32LB_SD_ERASE_SIZE;
  max_blocks = (uint64_t)UINT32_MAX;
  if (want / SF32LB_SD_ERASE_SIZE > max_blocks)
    {
      want = max_blocks * (uint64_t)SF32LB_SD_ERASE_SIZE;
    }

  return want;
}

/**
 * @brief 分配并填好一个 MTD 窗口（尚未 `register_mtddriver`）。
 *
 * @param byte_offset 卡上起始字节。
 * @param byte_size   窗口大小（0 = 余量）。
 * @param kv_prio     true 表示为 `/dev/sdkv`（总线优先，不受 LFS 上限约束）。
 * @param win_name    MTD 名（`sd` / `sdkv` / `sdfat`）。
 * @return 成功为 MTD 指针；失败为 NULL。
 */
static FAR struct mtd_dev_s *sf32lb_sd_alloc_window(uint32_t byte_offset,
                                                    uint32_t byte_size,
                                                    bool kv_prio,
                                                    FAR const char *win_name)
{
  FAR struct sf32lb_sd_dev_s *priv;
  uint64_t window;
  int ret;

  ret = sf32lb_sd_bringup();
  if (ret < 0)
    {
      return NULL;
    }

  window = sf32lb_sd_clamp_window(byte_offset, byte_size, kv_prio);
  if (window < 4ull * SF32LB_SD_ERASE_SIZE)
    {
      syslog(LOG_ERR,
             "ERROR: SD window 0x%lx size %llu too small (card %llu)\n",
             (unsigned long)byte_offset, (unsigned long long)window,
             (unsigned long long)g_sd_card_bytes);
      return NULL;
    }

  priv = (FAR struct sf32lb_sd_dev_s *)
         kmm_zalloc(sizeof(struct sf32lb_sd_dev_s));
  if (priv == NULL)
    {
      return NULL;
    }

  priv->mtd.erase = sf32lb_sd_erase;
  priv->mtd.bread = sf32lb_sd_bread;
  priv->mtd.bwrite = sf32lb_sd_bwrite;
  priv->mtd.ioctl = sf32lb_sd_ioctl;
  priv->mtd.name = win_name != NULL ? win_name : (kv_prio ? "sdkv" : "sd");
  priv->kv_prio = kv_prio;
  priv->byte_offset = byte_offset;
  priv->window_bytes = window;
  priv->page_size = SF32LB_SD_PAGE_SIZE;
  priv->erase_size = SF32LB_SD_ERASE_SIZE;
  priv->byte_limit = window;
  priv->neraseblocks = (uint32_t)(window / SF32LB_SD_ERASE_SIZE);

  syslog(LOG_INFO,
         "INFO: SD %s window off=0x%lx size=%llu (%lu x %u, card=%llu)\n",
         priv->mtd.name,
         (unsigned long)byte_offset,
         (unsigned long long)window,
         (unsigned long)priv->neraseblocks,
         SF32LB_SD_ERASE_SIZE,
         (unsigned long long)g_sd_card_bytes);

  return (FAR struct mtd_dev_s *)priv;
}

FAR struct mtd_dev_s *sf32lb_sd_initialize(uint32_t byte_offset,
                                           uint32_t byte_size)
{
  return sf32lb_sd_alloc_window(byte_offset, byte_size, false, "sd");
}

#if SF32LB_SD_PROFILE

/* Raw MTD read bench over the first 128 KiB of the FS window. Separates
 * per-access latency from bulk throughput:
 *   'seq1'  one page per call, consecutive (how littlefs walks metadata)
 *   'seqN'  8 pages per call, consecutive (does a bigger request amortize?)
 *   'seek'  one page per call, 1 MiB apart (pure per-access cost)
 */

#define SDPROF_BENCH_BYTES  131072u
#define SDPROF_BURST_PAGES  8u

static void sf32lb_sd_bench_run(FAR struct mtd_dev_s *mtd, FAR uint8_t *buf,
                                FAR const char *name, uint32_t pages_per_call,
                                uint32_t calls, uint32_t stride_pages)
{
  uint64_t t0 = sf32lb_sd_now_us();
  uint64_t dt;
  ssize_t ret = 0;
  uint32_t bytes = calls * pages_per_call * SF32LB_SD_PAGE_SIZE;
  uint32_t i;

  for (i = 0; i < calls && ret >= 0; i++)
    {
      ret = MTD_BREAD(mtd, (off_t)i * stride_pages, pages_per_call, buf);
    }

  if (ret < 0)
    {
      syslog(LOG_ERR, "SDPROF: %s failed: %d\n", name, (int)ret);
      return;
    }

  dt = sf32lb_sd_now_us() - t0;
  syslog(LOG_INFO,
         "SDPROF: %-5s %lu x %lu B in %llu us -> %lu KB/s, %lu us/call\n",
         name, (unsigned long)calls,
         (unsigned long)(pages_per_call * SF32LB_SD_PAGE_SIZE),
         (unsigned long long)dt,
         (unsigned long)(dt ? ((uint64_t)bytes * 1000ull) / dt : 0),
         (unsigned long)(dt / calls));
}

/* Halve the card clock twice and re-measure. If the per-sector cost tracks the
 * clock we are bus-limited (the divider or its source clock is wrong); if it
 * barely moves, the cost is fixed per-CMD17 latency and only multi-block
 * transfers can help.
 */

static void sf32lb_sd_bench_clocks(FAR struct mtd_dev_s *mtd,
                                   FAR uint8_t *buf)
{
  static const uint32_t hz[] =
  {
    SF32LB_SD_TRAN_CLOCK_HZ,
    SF32LB_SD_TRAN_CLOCK_HZ / 2,
    SF32LB_SD_TRAN_CLOCK_HZ / 4
  };

  uint32_t hclk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
  unsigned int i;

  for (i = 0; i < sizeof(hz) / sizeof(hz[0]); i++)
    {
      uint64_t t0;
      uint64_t dt;
      ssize_t ret = 0;
      uint32_t div;
      int j;

      nxmutex_lock(&g_sd_lock);
      sd1_set_clock(hz[i]);
      div = ((hwp_sdmmc1->CLKCR & SD_CLKCR_DIV_Msk) >>
             SD_CLKCR_DIV_Pos) + 1;
      nxmutex_unlock(&g_sd_lock);

      t0 = sf32lb_sd_now_us();
      for (j = 0; j < 32 && ret >= 0; j++)
        {
          ret = MTD_BREAD(mtd, j, 1, buf);
        }

      dt = sf32lb_sd_now_us() - t0;
      syslog(LOG_INFO,
             "SDPROF: clk  div=%-3lu (~%lu kHz) %lu us/sector%s\n",
             (unsigned long)div,
             (unsigned long)(hclk / div / 1000),
             (unsigned long)(dt / 32),
             ret < 0 ? " READ FAILED" : "");
    }

  nxmutex_lock(&g_sd_lock);
  sd1_set_clock(g_sd_bus_hz);
  nxmutex_unlock(&g_sd_lock);
}

static void sf32lb_sd_bench(FAR struct mtd_dev_s *mtd)
{
  const uint32_t pages = SDPROF_BENCH_BYTES / SF32LB_SD_PAGE_SIZE;
  FAR uint8_t *buf;

  buf = kmm_malloc(SDPROF_BURST_PAGES * SF32LB_SD_PAGE_SIZE);
  if (buf == NULL)
    {
      return;
    }

  sf32lb_sd_bench_run(mtd, buf, "seq1", 1, pages, 1);
  sf32lb_sd_bench_run(mtd, buf, "seqN", SDPROF_BURST_PAGES,
                      pages / SDPROF_BURST_PAGES, SDPROF_BURST_PAGES);
  sf32lb_sd_bench_run(mtd, buf, "seek", 1, 64,
                      1048576u / SF32LB_SD_PAGE_SIZE);
  sf32lb_sd_bench_clocks(mtd, buf);

  kmm_free(buf);
  sf32lb_sd_stats_dump("bench");
  memset(&g_sd_stats, 0, sizeof(g_sd_stats));
}
#endif

/****************************************************************************
 * Name: sf32lb_sd_hwtest
 *
 * Description:
 *   Layer-1 SDIO test: MTD_BREAD/BWRITE only (CMD17/CMD18/CMD24).  Does not go
 *   through LittleFS or POSIX.  Write uses the last erase block with
 *   save/restore so a mounted /mnt/lfs is left intact if the test finishes.
 ****************************************************************************/

int sf32lb_sd_hwtest(unsigned flags, int seq_kib, int rnd_ops)
{
  FAR struct sf32lb_sd_dev_s *priv;
  FAR uint8_t *buf;
  FAR uint8_t *orig;
  uint32_t pages_per_erase;
  uint32_t span;
  uint32_t npages;
  uint32_t i;
  uint64_t t0;
  uint64_t dt;
  ssize_t ret;
  int fail = 0;

  if (!g_sd_initialized || g_sd_mtd == NULL)
    {
      syslog(LOG_ERR, "SDHW: SD MTD not registered\n");
      return -ENODEV;
    }

  if (flags == 0)
    {
      flags = SF32LB_SD_HWTEST_ALL;
    }

  if (seq_kib <= 0)
    {
      seq_kib = 256;
    }

  if (rnd_ops <= 0)
    {
      rnd_ops = 64;
    }

  priv = (FAR struct sf32lb_sd_dev_s *)g_sd_mtd;
  pages_per_erase = priv->erase_size / priv->page_size;
  buf = (FAR uint8_t *)kmm_malloc(priv->erase_size);
  orig = (FAR uint8_t *)kmm_malloc(priv->erase_size);
  if (buf == NULL || orig == NULL)
    {
      kmm_free(buf);
      kmm_free(orig);
      return -ENOMEM;
    }

  sf32lb_sd_diag_dump("sdhw-begin");
  syslog(LOG_INFO,
         "SDHW: geometry page=%lu erase=%lu nerase=%lu window=0x%lx+%llu "
         "bus=%lu kHz\n",
         (unsigned long)priv->page_size, (unsigned long)priv->erase_size,
         (unsigned long)priv->neraseblocks,
         (unsigned long)priv->byte_offset,
         (unsigned long long)priv->byte_limit,
         (unsigned long)(g_sd_bus_hz / 1000));

  if (flags & SF32LB_SD_HWTEST_SEQ)
    {
      npages = ((uint32_t)seq_kib * 1024u) / priv->page_size;
      if (npages == 0)
        {
          npages = 1;
        }

      if ((uint64_t)npages > priv->byte_limit / priv->page_size)
        {
          npages = (uint32_t)(priv->byte_limit / priv->page_size);
        }

      t0 = sd1_mono_us();
      for (i = 0; i < npages; i++)
        {
          ret = MTD_BREAD(g_sd_mtd, (off_t)i, 1, buf);
          if (ret < 0)
            {
              syslog(LOG_ERR, "SDHW: seq read page %lu failed: %d\n",
                     (unsigned long)i, (int)ret);
              fail++;
              break;
            }
        }

      dt = sd1_mono_us() - t0;
      if (dt == 0)
        {
          dt = 1;
        }

      syslog(LOG_INFO,
             "SDHW: seq read %lu x %lu B in %llu us -> %lu KB/s fail=%d\n",
             (unsigned long)i, (unsigned long)priv->page_size,
             (unsigned long long)dt,
             (unsigned long)(((uint64_t)i * priv->page_size * 1000ull) / dt),
             fail);

      if (fail == 0 && npages >= SF32LB_SD_CMD18_MAX_BLOCKS)
        {
          uint32_t burst = SF32LB_SD_CMD18_MAX_BLOCKS;
          uint32_t ncall = npages / burst;

          t0 = sd1_mono_us();
          for (i = 0; i < ncall; i++)
            {
              ret = MTD_BREAD(g_sd_mtd, (off_t)(i * burst), burst, buf);
              if (ret < 0)
                {
                  syslog(LOG_ERR, "SDHW: seq CMD18 page %lu failed: %d\n",
                         (unsigned long)(i * burst), (int)ret);
                  fail++;
                  break;
                }
            }

          dt = sd1_mono_us() - t0;
          if (dt == 0)
            {
              dt = 1;
            }

          syslog(LOG_INFO,
                 "SDHW: seq CMD18 %lu x %lu B in %llu us -> %lu KB/s "
                 "fail=%d cmd18=%s\n",
                 (unsigned long)i,
                 (unsigned long)(burst * priv->page_size),
                 (unsigned long long)dt,
                 (unsigned long)(((uint64_t)i * burst * priv->page_size *
                                  1000ull) / dt),
                 fail, g_sd_dma ? "on" : "off");
        }
    }

  if ((flags & SF32LB_SD_HWTEST_RND) && fail == 0)
    {
      if (priv->byte_limit / priv->page_size > 2048ull)
        {
          span = 2048u;   /* first 1 MiB: stay in likely-programmed area */
        }
      else
        {
          span = (uint32_t)(priv->byte_limit / priv->page_size);
        }

      t0 = sd1_mono_us();
      for (i = 0; i < (uint32_t)rnd_ops; i++)
        {
          off_t page = (off_t)((i * 17u + 3u) % span);

          ret = MTD_BREAD(g_sd_mtd, page, 1, buf);
          if (ret < 0)
            {
              syslog(LOG_ERR, "SDHW: rnd read page %ld failed: %d\n",
                     (long)page, (int)ret);
              fail++;
              break;
            }
        }

      dt = sd1_mono_us() - t0;
      if (dt == 0)
        {
          dt = 1;
        }

      syslog(LOG_INFO,
             "SDHW: rnd read %lu ops x %lu B in %llu us -> %lu KB/s fail=%d\n",
             (unsigned long)i, (unsigned long)priv->page_size,
             (unsigned long long)dt,
             (unsigned long)(((uint64_t)i * priv->page_size * 1000ull) / dt),
             fail);
    }

  if ((flags & SF32LB_SD_HWTEST_WRITE) && fail == 0)
    {
      off_t startpage;
      uint32_t eblk;
      uint32_t p;
      uint64_t card_addr;

      if (priv->neraseblocks < 3)
        {
          fail++;
        }
      else
        {
          /* Last erase block of the LFS window.  Live LittleFS only uses
           * the start of the volume; block 2 is metadata.
           * CMD24 here needs the 500 ms write wait (50 ms aborted and
           * wedged the card).  Skip this during `test layers` (read only).
           */

          eblk = priv->neraseblocks - 1;
          startpage = (off_t)(eblk * pages_per_erase);
          card_addr = (uint64_t)priv->byte_offset +
                      (uint64_t)startpage * priv->page_size;
          syslog(LOG_INFO,
                 "SDHW: write scratch eraseblk=%lu page=%ld card=0x%llx\n",
                 (unsigned long)eblk, (long)startpage,
                 (unsigned long long)card_addr);
          ret = MTD_BREAD(g_sd_mtd, startpage, (size_t)pages_per_erase, orig);
          if (ret < 0)
            {
              syslog(LOG_ERR, "SDHW: save scratch erase block failed: %d\n",
                     (int)ret);
              fail++;
            }
          else
            {
              for (p = 0; p < priv->erase_size; p++)
                {
                  buf[p] = (uint8_t)(0xa5u ^ (uint8_t)p);
                }

              t0 = sd1_mono_us();
              ret = MTD_BWRITE(g_sd_mtd, startpage,
                               (size_t)pages_per_erase, buf);
              dt = sd1_mono_us() - t0;
              if (ret < 0)
                {
                  syslog(LOG_ERR, "SDHW: write scratch erase block failed: %d\n",
                         (int)ret);
                  fail++;
                }
              else
                {
                  if (dt == 0)
                    {
                      dt = 1;
                    }

                  syslog(LOG_INFO,
                         "SDHW: seq write %lu B in %llu us -> %lu KB/s\n",
                         (unsigned long)priv->erase_size,
                         (unsigned long long)dt,
                         (unsigned long)(((uint64_t)priv->erase_size *
                                          1000ull) / dt));

                  ret = MTD_BREAD(g_sd_mtd, startpage,
                                  (size_t)pages_per_erase, buf);
                  if (ret < 0)
                    {
                      fail++;
                    }
                  else
                    {
                      for (p = 0; p < priv->erase_size; p++)
                        {
                          if (buf[p] != (uint8_t)(0xa5u ^ (uint8_t)p))
                            {
                              syslog(LOG_ERR,
                                     "SDHW: write verify mismatch off=%lu\n",
                                     (unsigned long)p);
                              fail++;
                              break;
                            }
                        }
                    }
                }

              ret = MTD_BWRITE(g_sd_mtd, startpage,
                               (size_t)pages_per_erase, orig);
              if (ret < 0)
                {
                  syslog(LOG_ERR, "SDHW: restore scratch erase block failed: %d "
                         "(LittleFS block %lu may be dirty — reboot)\n",
                         (int)ret, (unsigned long)eblk);
                  fail++;
                }
            }
        }
    }

  sf32lb_sd_diag_dump("sdhw-end");
  kmm_free(buf);
  kmm_free(orig);
  return fail ? -EIO : OK;
}

/**
 * @brief 识别 SD 卡并注册 `/dev/sdN`（FS_REGION 窗口）。
 *
 * @param minor       设备号（0 → `/dev/sd0`）。
 * @param byte_offset 卡上偏移（`FS_REGION_OFFSET`）。
 * @param byte_size   窗口大小；0 / `0xffffffff` 为 CSD 余量。
 * @return 成功为 0，失败为负 errno。
 */
int sf32lb_sd_automount(int minor, uint32_t byte_offset, uint32_t byte_size)
{
  FAR struct mtd_dev_s *mtd;
  char devname[16];
  int ret;

  if (g_sd_initialized)
    {
      return OK;
    }

  snprintf(devname, sizeof(devname), SF32LB_SD_PARENT_FMT, minor);

  mtd = sf32lb_sd_initialize(byte_offset, byte_size);
  if (mtd == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SD card\n");
      return -ENODEV;
    }

  ret = register_mtddriver(devname, mtd, 0, mtd);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: register_mtddriver(%s) failed: %d\n",
             devname, ret);
      return ret;
    }

  g_sd_initialized = true;
  g_sd_mtd = mtd;
  {
    FAR struct sf32lb_sd_dev_s *priv = (FAR struct sf32lb_sd_dev_s *)mtd;

    syslog(LOG_INFO,
           "INFO: SD MTD registered at %s (off=0x%lx size=%llu, "
           "page=%u erase=%u nblocks=%lu)\n",
           devname,
           (unsigned long)priv->byte_offset,
           (unsigned long long)priv->byte_limit,
           SF32LB_SD_PAGE_SIZE, SF32LB_SD_ERASE_SIZE,
           (unsigned long)priv->neraseblocks);
  }

#if SF32LB_SD_PROFILE
  sf32lb_sd_bench(mtd);
#endif
  return OK;
}

/**
 * @brief 缩小或恢复 `/mnt/lfs` 逻辑窗口（须先 umount）。
 *
 * @param bytes 目标字节数；0 只打印当前上限；`SF32LB_SD_LFS_WINDOW_FULL` 恢复余量。
 * @return 成功为 0，失败为负 errno。
 */
int sf32lb_sd_limit_fs_bytes(uint32_t bytes)
{
  FAR struct sf32lb_sd_dev_s *priv;
  uint64_t want;
  uint32_t n;

  if (g_sd_mtd == NULL)
    {
      return -ENODEV;
    }

  priv = (FAR struct sf32lb_sd_dev_s *)g_sd_mtd;
  if (bytes == 0)
    {
      syslog(LOG_INFO,
             "INFO: SD LFS window %llu (%lu blocks of %u)\n",
             (unsigned long long)priv->byte_limit,
             (unsigned long)priv->neraseblocks,
             priv->erase_size);
      return OK;
    }

  if (bytes == SF32LB_SD_LFS_WINDOW_FULL)
    {
      want = priv->window_bytes;
    }
  else
    {
      want = bytes;
      if (want > priv->window_bytes)
        {
          want = priv->window_bytes;
        }
    }

  want -= want % priv->erase_size;
  n = (uint32_t)(want / priv->erase_size);
  if (n < 4)
    {
      return -EINVAL;
    }

  priv->byte_limit = want;
  priv->neraseblocks = n;
  syslog(LOG_INFO,
         "INFO: SD LFS window set to %llu (%lu blocks); format required\n",
         (unsigned long long)priv->byte_limit,
         (unsigned long)priv->neraseblocks);
  return OK;
}

/**
 * @brief 把 `/mnt/lfs` 磁盘 superblock 扩到当前 MTD 窗口。
 * @return 见 `sf32lb_lfs_grow_super()`；窗口未注册则为 `-ENODEV`。
 */
int sf32lb_sd_grow_lfs(void)
{
  FAR struct sf32lb_sd_dev_s *priv;

  if (g_sd_mtd == NULL)
    {
      return -ENODEV;
    }

  priv = (FAR struct sf32lb_sd_dev_s *)g_sd_mtd;
  return sf32lb_lfs_grow_super(g_sd_mtd, priv->neraseblocks);
}

/**
 * @brief 尝试一次 LittleFS mount（不 autoformat）。
 *
 * @param devname  `/dev/sdN`
 * @return `mount()` 返回值；失败时 errno 已设置。
 */
static int sd_lfs_mount_once(const char *devname)
{
  int ret;
  int saved;

#if SF32LB_SD_PROFILE
  {
    uint64_t t0 = sf32lb_sd_now_us();

    ret = mount(devname, SF32LB_SD_MOUNTPOINT, "littlefs", 0, NULL);
    saved = errno;
    syslog(LOG_INFO, "SDPROF: lfs_mount took %llu us (ret=%d errno=%d)\n",
           (unsigned long long)(sf32lb_sd_now_us() - t0), ret, saved);
    sf32lb_sd_stats_dump("mount");
    if (ret < 0)
      {
        errno = saved;
      }
  }
#else
  ret = mount(devname, SF32LB_SD_MOUNTPOINT, "littlefs", 0, NULL);
#endif
  return ret;
}

/**
 * @brief 将 `/dev/sdN` 挂到 `/mnt/lfs`。
 *
 * 打包种子小于 CSD 窗口时先 grow superblock。挂载失败会 umount 后再试
 * （瞬时 SD 读错可表现为 CORRUPT）。不 autoformat。
 *
 * @param minor 设备号（与 automount 相同）。
 * @return 成功为 0，失败为负 errno。
 */
int sf32lb_sd_mount_littlefs(int minor)
{
#ifndef CONFIG_DISABLE_MOUNTPOINT
  char devname[16];
  int ret;
  int attempt;
  int last_errno = 0;

  snprintf(devname, sizeof(devname), SF32LB_SD_PARENT_FMT, minor);

  ret = mkdir("/mnt", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(/mnt) failed: %d\n", errno);
      return -errno;
    }

  ret = mkdir(SF32LB_SD_MOUNTPOINT, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(%s) failed: %d\n",
             SF32LB_SD_MOUNTPOINT, errno);
      return -errno;
    }

  /* 开机挂载是"这次开机好不好"的决定点，所以这里必须能自证清不清白：
   *   1. 打起始状态（rca / 时钟 / 容量）—— 挂载前卡是什么样；
   *   2. 每次尝试的**耗时**和这次尝试期间真正落到卡上的 I/O —— 用来区分
   *      "干净挂上"和"靠重试挂上"（后者很可能说明卡已在边缘）；
   *   3. 重试前把卡恢复到已知状态（完整重新识别）—— 不这么做的话三次尝试
   *      会撞同一个坏状态，重试等于没做。这是本次改动最实质的一点；
   *   4. 退避递增（100/300/900 ms），给卡一点安定时间；
   *   5. 全部失败时把"未挂载"这件事说清楚，别让上层以为 FS 是好的。
   */
  syslog(LOG_INFO,
         "LFS: mount begin dev=%s tries=%u rca=0x%04x clk=%ukHz card=%llu B\n",
         devname, (unsigned)SF32LB_SD_LFS_MOUNT_TRIES, g_sd_rca,
         (unsigned)(g_sd_bus_hz / 1000u),
         (unsigned long long)g_sd_card_bytes);

#if SF32LB_SD_PROFILE
  {
    struct sf32lb_sd_stats_s st_prev = g_sd_stats;
#endif
    uint32_t backoff_ms = SF32LB_SD_LFS_RETRY_MS;

    for (attempt = 1; attempt <= SF32LB_SD_LFS_MOUNT_TRIES; attempt++)
      {
        uint32_t t0 = (uint32_t)(sf32lb_sd_now_us() / 1000ull);
        uint32_t dt;

        myvendor_watchdog_work_beat();

        ret = sf32lb_sd_grow_lfs();
        if (ret < 0 && ret != -ENOENT)
          {
            last_errno = -ret;
            syslog(LOG_ERR,
                   "ERROR: littlefs superblock grow failed: %d (try %d/%d)\n",
                   ret, attempt, SF32LB_SD_LFS_MOUNT_TRIES);
          }
        else
          {
            ret = sd_lfs_mount_once(devname);
            if (ret >= 0)
              {
                last_errno = 0;
              }
            else if (errno == EBUSY || errno == ENOTDIR || errno == EEXIST)
              {
                return OK;
              }
            else
              {
                last_errno = errno;
                syslog(LOG_ERR,
                       "ERROR: mount(%s, %s, littlefs) failed: %d (try %d/%d)\n",
                       devname, SF32LB_SD_MOUNTPOINT, last_errno,
                       attempt, SF32LB_SD_LFS_MOUNT_TRIES);
              }
          }

        dt = (uint32_t)(sf32lb_sd_now_us() / 1000ull) - t0;

#if SF32LB_SD_PROFILE
        {
          FAR const struct sf32lb_sd_stats_s *s = &g_sd_stats;

          syslog(last_errno == 0 ? LOG_INFO : LOG_WARNING,
                 "LFS: try %d/%u %s in %lu ms (this try: bread=%lu hw_rd=%lu hw_wr=%lu erase=%lu)\n",
                 attempt, (unsigned)SF32LB_SD_LFS_MOUNT_TRIES,
                 last_errno == 0 ? "ok" : "FAILED", (unsigned long)dt,
                 (unsigned long)(s->bread - st_prev.bread),
                 (unsigned long)(s->hw_read - st_prev.hw_read),
                 (unsigned long)(s->hw_write - st_prev.hw_write),
                 (unsigned long)(s->erase_blocks - st_prev.erase_blocks));
          st_prev = *s;
        }
#else
        syslog(last_errno == 0 ? LOG_INFO : LOG_WARNING,
               "LFS: try %d/%u %s in %lu ms\n",
               attempt, (unsigned)SF32LB_SD_LFS_MOUNT_TRIES,
               last_errno == 0 ? "ok" : "FAILED", (unsigned long)dt);
#endif

        if (last_errno == 0 || attempt >= SF32LB_SD_LFS_MOUNT_TRIES)
          {
            break;
          }

        (void)umount(SF32LB_SD_MOUNTPOINT);
        myvendor_watchdog_work_beat();

        /* 重试前先把卡恢复到已知状态：完整重新识别（含有序停机 + CMD0），
         * 然后放回传输时钟。只 umount + sleep 重试是没用的 —— 坏状态还在。 */
        syslog(LOG_WARNING,
               "LFS: re-init card before retry %d (backoff %lu ms fail_run=%u)\n",
               attempt + 1, (unsigned long)backoff_ms, (unsigned)g_sd_fail_run);

        if (sf32lb_sd_identify() != 0)
          {
            syslog(LOG_ERR, "LFS: card re-init FAILED before retry %d\n",
                   attempt + 1);
            sd1_set_clock(g_sd_bus_hz);
          }
        else
          {
            sd1_set_clock(SF32LB_SD_TRAN_CLOCK_HZ);
            g_sd_bus_hz = SF32LB_SD_TRAN_CLOCK_HZ;
            syslog(LOG_WARNING, "LFS: card re-init ok before retry %d\n",
                   attempt + 1);
          }

        usleep(backoff_ms * 1000u);
        backoff_ms *= 3u;
      }
#if SF32LB_SD_PROFILE
  }
#endif

  if (last_errno != 0)
    {
      /* ── 第二级：只读挂载 ────────────────────────────────────────────
       * 读写挂载失败 → 试只读。NuttX 的 LittleFS 认 `data` 参数里的 "ro"
       * （lfs_vfs.c:1431，在 lfs_mount() 之前就置 fs->readonly，写路径
       * write/truncate/rename 直接 -EACCES）。
       *
       * 刻意**不调 sf32lb_sd_grow_lfs()**：那个函数会写超级块，在只读尝试里
       * 去写就自相矛盾 —— 而且它很可能正是读写挂载失败的原因。
       */
      syslog(LOG_WARNING,
             "LFS: rw mount failed (%d), trying READ-ONLY\n", last_errno);

      ret = mount(devname, SF32LB_SD_MOUNTPOINT, "littlefs", 0, "ro");
      if (ret == 0)
        {
          g_lfs_readonly = true;
          last_errno = 0;
          syslog(LOG_ERR,
                 "LFS: mounted READ-ONLY — rides/settings/eph will NOT persist until reboot\n");
        }
      else
        {
          syslog(LOG_ERR, "LFS: READ-ONLY mount also failed errno=%d\n", errno);
        }
    }

  if (last_errno != 0)
    {
      if (last_errno == EFAULT)
        {
          syslog(LOG_ERR,
                 "ERROR: LittleFS CORRUPT on %s after %d tries; "
                 "refused autoformat (would wipe /mnt/lfs rides). "
                 "nsh> test lfs format -y  only if you accept data loss\n",
                 devname, SF32LB_SD_LFS_MOUNT_TRIES);
        }
      else if (last_errno == EINVAL)
        {
          syslog(LOG_ERR,
                 "ERROR: superblock geometry mismatch; "
                 "nsh> test lfs format -y   (wipes /mnt/lfs)\n");
        }

      /* ── 第三级：重启 ───────────────────────────────────────────────
       * 按明确要求：读写和只读都挂不上就重启，不做循环断路器，无限重启也接受。
       * 选这条路的前提是**救砖通道独立于这一层**：bootloader 的
       * `10x B/b = msh` 和 `KEY1+KEY2 按住 1s = factory` 仍然有效，所以真锁成
       * 无限重启时仍能进 msh / factory 恢复，不会变砖。
       */
      syslog(LOG_ERR,
             "LFS: mount FAILED after %u tries (last_errno=%d) and READ-ONLY also failed; rebooting\n",
             (unsigned)SF32LB_SD_LFS_MOUNT_TRIES, last_errno);
      usleep(200000);
      HAL_PMU_Reboot();

      for (;;)
        {
          /* HAL_PMU_Reboot() 不返回；真回来也停住，绝不带着坏 FS 继续跑。 */
          usleep(1000000);
        }
    }

  if (g_sd_mtd != NULL)
    {
      FAR struct sf32lb_sd_dev_s *fsdev =
          (FAR struct sf32lb_sd_dev_s *)g_sd_mtd;

      syslog(LOG_INFO,
             "INFO: LittleFS mounted %s -> %s%s "
             "(%lu blocks x %lu = %llu bytes, %llu MiB)\n",
             devname, SF32LB_SD_MOUNTPOINT,
             g_lfs_readonly ? " [READ-ONLY]" : "",
             (unsigned long)fsdev->neraseblocks,
             (unsigned long)fsdev->erase_size,
             (unsigned long long)fsdev->window_bytes,
             (unsigned long long)(fsdev->window_bytes / (1024ull * 1024ull)));
    }
  else
    {
      syslog(LOG_INFO, "INFO: LittleFS mounted %s -> %s%s\n",
             devname, SF32LB_SD_MOUNTPOINT,
             g_lfs_readonly ? " [READ-ONLY]" : "");
    }

  /* 挂上之后回读一次，把"挂上了但 I/O 已经坏了"挡在开机阶段 —— 否则这种
   * 状态要等到十几秒后第一次真正读文件才暴露，那时现场已经混进别的东西。
   * stat 命中 LFS 根 inode，代价极小。 */
  {
    struct stat vst;

    if (stat(SF32LB_SD_MOUNTPOINT, &vst) != 0)
      {
        syslog(LOG_ERR,
               "LFS: mounted but stat(%s) failed errno=%d — FS is NOT usable\n",
               SF32LB_SD_MOUNTPOINT, errno);
        return -errno;
      }

    syslog(LOG_INFO, "LFS: mount verified in %u try(ies) mode=%s, stat ok\n",
           (unsigned)attempt, g_lfs_readonly ? "ro" : "rw");
  }

#if SF32LB_SD_PROFILE
  sf32lb_sd_stats_dump("lfs-mount");
#endif

#endif

  return OK;
}

/**
 * @brief /mnt/lfs 是不是只读挂上的（读写挂载失败后的降级结果）。
 *
 * @details 只读期间写操作会拿到 -EACCES。写盘的用户（轨迹记录、设置持久化、
 *          星历落盘）应当据此跳过，而不是反复撞 EACCES 刷日志。
 */
bool sf32lb_sd_lfs_readonly(void)
{
  return g_lfs_readonly;
}

/**
 * @brief 注册 `/dev/sdkv` 并挂到 `/mnt/kv`（独立于 `/mnt/lfs`）。
 *
 * 窗口为 ptab `KV_REGION`（256 MiB @ 256 MiB）。空白切片 autoformat；
 * 已有更小种子则先 grow。不要每轮 boot `forceformat`。
 *
 * @param byte_offset 卡上字节偏移。
 * @param byte_size   窗口字节数，须 ≥ 4 × erase。
 * @return 成功为 0（含已注册），失败为负 errno。
 */
int sf32lb_sd_mount_kv(uint32_t byte_offset, uint32_t byte_size)
{
#ifndef CONFIG_DISABLE_MOUNTPOINT
  FAR struct mtd_dev_s *mtd;
  FAR struct sf32lb_sd_dev_s *priv;
  bool kv_blank;
  int ret;

  if (g_sd_kv_mtd != NULL)
    {
      return OK;
    }

  if (byte_size < 4 * SF32LB_SD_ERASE_SIZE)
    {
      syslog(LOG_ERR, "ERROR: KV_REGION 0x%lx too small for LittleFS\n",
             (unsigned long)byte_size);
      return -EINVAL;
    }

  mtd = sf32lb_sd_alloc_window(byte_offset, byte_size, true, "sdkv");
  if (mtd == NULL)
    {
      syslog(LOG_ERR, "ERROR: KV MTD window failed (off=0x%lx size=0x%lx)\n",
             (unsigned long)byte_offset, (unsigned long)byte_size);
      return -ENODEV;
    }

  ret = register_mtddriver(SF32LB_SD_KV_DEV, mtd, 0, mtd);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: register_mtddriver(%s) failed: %d\n",
             SF32LB_SD_KV_DEV, ret);
      kmm_free(mtd);
      return ret;
    }

  g_sd_kv_mtd = mtd;
  priv = (FAR struct sf32lb_sd_dev_s *)mtd;
  syslog(LOG_INFO,
         "INFO: KV MTD registered at %s (off=0x%lx size=%llu, "
         "page=%u erase=%u nblocks=%lu, bus prio)\n",
         SF32LB_SD_KV_DEV,
         (unsigned long)priv->byte_offset,
         (unsigned long long)priv->byte_limit,
         SF32LB_SD_PAGE_SIZE, SF32LB_SD_ERASE_SIZE,
         (unsigned long)priv->neraseblocks);

  ret = mkdir("/mnt", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(/mnt) failed: %d\n", errno);
      return -errno;
    }

  ret = mkdir(SF32LB_SD_KV_MOUNTPOINT, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(%s) failed: %d\n",
             SF32LB_SD_KV_MOUNTPOINT, errno);
      return -errno;
    }

  /* Packed/old KV seed may be smaller than the 256 MiB window. Patch
   * superblock.block_count (vendor) so stock NuttX littlefs can mount.
   * Blank slice → -ENOENT and autoformat. Shrink is not supported.
   */

  ret = sf32lb_lfs_grow_super(g_sd_kv_mtd, priv->neraseblocks);
  if (ret < 0 && ret != -ENOENT)
    {
      syslog(LOG_ERR, "ERROR: KV littlefs superblock grow failed: %d\n", ret);
      return ret;
    }

  kv_blank = (ret == -ENOENT);

  /* Empty reserved slice: autoformat once. Do not forceformat on every boot. */

  /* `autoformat` 只在 grow 返回 -ENOENT（切片空白，出厂/首次）时才传。
   * 其它情况一律**不格式化** —— 按"LFS 永不格式化"：切片里有内容却挂不上
   * 属于损坏，那要走 只读/重启 的阶梯，绝不能把用户设置清掉。
   * 注意 littlefs 的 autoformat 是绑在 lfs_mount 返回 -EFAULT 上的（见
   * lfs_vfs.c），并不区分"空白"和"损坏"，所以这个判断只能我们自己做。 */
  ret = mount(SF32LB_SD_KV_DEV, SF32LB_SD_KV_MOUNTPOINT, "littlefs", 0,
              kv_blank ? "autoformat" : NULL);
  if (ret < 0)
    {
      if (errno == EBUSY || errno == ENOTDIR || errno == EEXIST)
        {
          return OK;
        }

      syslog(LOG_ERR, "ERROR: mount(%s, %s, littlefs) failed: %d\n",
             SF32LB_SD_KV_DEV, SF32LB_SD_KV_MOUNTPOINT, errno);
      return -errno;
    }

  syslog(LOG_INFO,
         "INFO: LittleFS mounted %s -> %s "
         "(KVDB persist, %lu blocks x %lu = %llu bytes, %llu MiB)\n",
         SF32LB_SD_KV_DEV, SF32LB_SD_KV_MOUNTPOINT,
         (unsigned long)priv->neraseblocks,
         (unsigned long)priv->erase_size,
         (unsigned long long)priv->window_bytes,
         (unsigned long long)(priv->window_bytes / (1024ull * 1024ull)));
#endif

  return OK;
}

/**
 * @brief 注册 FAT 窗口、FTL 块设备，挂 vfat 到 `/mnt/fat`。
 *
 * 产品只读，工厂读写。空白/损坏不 autoformat。
 */
int sf32lb_sd_mount_fat(uint32_t byte_offset, uint32_t byte_size)
{
#ifndef CONFIG_DISABLE_MOUNTPOINT
  FAR struct mtd_dev_s *mtd;
  FAR struct sf32lb_sd_dev_s *priv;
  unsigned long flags;
  int ret;

  if (g_sd_fat_mtd != NULL)
    {
      return OK;
    }

  mtd = sf32lb_sd_alloc_window(byte_offset, byte_size, false, "sdfat");
  if (mtd == NULL)
    {
      syslog(LOG_ERR, "ERROR: FAT MTD window failed (off=0x%lx size=0x%lx)\n",
             (unsigned long)byte_offset, (unsigned long)byte_size);
      return -ENODEV;
    }

  ret = register_mtddriver(SF32LB_SD_FAT_DEV, mtd, 0, mtd);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: register_mtddriver(%s) failed: %d\n",
             SF32LB_SD_FAT_DEV, ret);
      kmm_free(mtd);
      return ret;
    }

  g_sd_fat_mtd = mtd;
  priv = (FAR struct sf32lb_sd_dev_s *)mtd;
  syslog(LOG_INFO,
         "INFO: FAT MTD registered at %s (off=0x%lx size=%llu, "
         "page=%u erase=%u nblocks=%lu)\n",
         SF32LB_SD_FAT_DEV,
         (unsigned long)priv->byte_offset,
         (unsigned long long)priv->byte_limit,
         SF32LB_SD_PAGE_SIZE, SF32LB_SD_ERASE_SIZE,
         (unsigned long)priv->neraseblocks);

  /* SD 按页改写，不需要 FTL 的擦除回读缓存。O_DIRECT 走
   * ftl_flush_direct；上游成功返回 leftover=0，FAT 会当成 ENODEV。
   * 板级抽换 vela_override/mtd/ftl.c 改成返回已写扇区数。
   */
  ret = ftl_initialize_by_path(SF32LB_SD_FAT_BLK, mtd,
                               O_RDWR | O_DIRECT | O_SYNC);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ftl_initialize_by_path(%s) failed: %d\n",
             SF32LB_SD_FAT_BLK, ret);
      return ret;
    }

  ret = mkdir("/mnt", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(/mnt) failed: %d\n", errno);
      return -errno;
    }

  ret = mkdir(SF32LB_SD_FAT_MOUNTPOINT, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: mkdir(%s) failed: %d\n",
             SF32LB_SD_FAT_MOUNTPOINT, errno);
      return -errno;
    }

#ifdef CONFIG_MYVENDOR_FACTORY_MODE
  flags = 0;
#else
  flags = MS_RDONLY;
#endif

  myvendor_watchdog_work_beat();
  ret = mount(SF32LB_SD_FAT_BLK, SF32LB_SD_FAT_MOUNTPOINT, "vfat", flags,
              NULL);
  if (ret < 0)
    {
      if (errno == EBUSY || errno == ENOTDIR || errno == EEXIST)
        {
          return OK;
        }

      syslog(LOG_ERR,
             "ERROR: mount(%s, %s, vfat, %s) failed: %d"
             " (window=%llu MiB; EINVAL often means packed FAT > this CSD)"
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
             " (nsh> mkfatfs %s)"
#endif
             "\n",
             SF32LB_SD_FAT_BLK, SF32LB_SD_FAT_MOUNTPOINT,
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
             "rw", errno,
             (unsigned long long)(priv->window_bytes / (1024ull * 1024ull)),
             SF32LB_SD_FAT_BLK);
#else
             "ro", errno,
             (unsigned long long)(priv->window_bytes / (1024ull * 1024ull)));
#endif
      return -errno;
    }

  syslog(LOG_INFO,
         "INFO: FAT mounted %s -> %s (%s, %llu bytes, %llu MiB)\n",
         SF32LB_SD_FAT_BLK, SF32LB_SD_FAT_MOUNTPOINT,
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
         "rw",
#else
         "ro",
#endif
         (unsigned long long)priv->window_bytes,
         (unsigned long long)(priv->window_bytes / (1024ull * 1024ull)));
#endif

  return OK;
}

int sf32lb_sd_crash_reinit(void)
{
  /* DEMCR.TRCENA + DWT CYCCNT：裸机超时用。 */
  *(volatile uint32_t *)0xe000edfcu |= (1u << 24);
  *(volatile uint32_t *)0xe0001000u |= 1u;

  g_sd_baremetal = true;
  g_sd_dma = false;
  g_sd_card_ready = false;
  g_sd_dead = false;
  g_sd_reinit_used = false;

  hwp_sdmmc1->IER = 0;
  hwp_sdmmc1->DCR = 0;
  up_disable_irq(SDMMC1_IRQn + NVIC_IRQ_FIRST);
  up_disable_irq(DMAC1_CH3_IRQn + NVIC_IRQ_FIRST);

  /* 停掉可能还在跑的 SD DMA（CH3），identify 里 sd1_init 会再关时钟。 */
  DMA1_Channel3->CCR = 0;

  return sf32lb_sd_identify();
}

int sf32lb_sd_crash_read512(uint64_t byte_off, FAR void *buf)
{
  if (buf == NULL)
    {
      return -EINVAL;
    }

  return sf32lb_sd_read_block(byte_off, (FAR uint8_t *)buf);
}

/**
 * @brief 裸读连续 512 B 块并逐块报时，给 `ctl sd read` 做卡自检。
 *
 * @details 三个刻意的设计：
 *          1. **绕过判死闸门** —— 现场 4/4 次首次失败都落在 LFS 块 194
 *             （偏移 0xC2000），要验证"是不是这个块"，就必须在卡可能已经判死
 *             的状态下也真的去读它一次，而不是被闸门直接 -EIO 挡掉；
 *          2. **按正常 24 MHz 读** —— 用识别时钟（400 kHz）读会掩盖边缘问题，
 *             取到的"能读"是假的；所以先把总线时钟设回 TRAN；
 *          3. **不进 LFS、不改任何文件系统状态** —— 纯读，可随时重复跑。
 *
 *          逐块打点：坏页/磨损页的典型特征是**某一块明显慢或直接失败**，只
 *          报总结果看不出是哪块。
 *
 * @return 0 全部读到；负 errno 表示首个失败的块。
 */
int sf32lb_sd_raw_read(uint64_t byte_off, uint32_t nblocks)
{
  FAR uint8_t *buf;
  uint32_t i;
  int ret = 0;

  if (nblocks == 0)
    {
      return -EINVAL;
    }

  buf = kmm_malloc(SD_HW_BLOCK_SIZE);
  if (buf == NULL)
    {
      syslog(LOG_ERR, "SDTEST: no mem for %u B\n", (unsigned)SD_HW_BLOCK_SIZE);
      return -ENOMEM;
    }

  nxmutex_lock(&g_sd_lock);
  sd1_set_clock(SF32LB_SD_TRAN_CLOCK_HZ);
  g_sd_bus_hz = SF32LB_SD_TRAN_CLOCK_HZ;

  syslog(LOG_WARNING, "SDTEST: raw read off=0x%llx n=%lu @24MHz (bypasses dead gate)\n",
         (unsigned long long)byte_off, (unsigned long)nblocks);

  for (i = 0; i < nblocks; i++)
    {
      uint64_t off = byte_off + (uint64_t)i * SD_HW_BLOCK_SIZE;
      uint64_t t0 = sd1_mono_us();

      ret = sf32lb_sd_read_block(off, buf);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "SDTEST: blk %lu off=0x%llx FAIL ret=%d (%llu us)\n",
                 (unsigned long)i, (unsigned long long)off, ret,
                 (unsigned long long)(sd1_mono_us() - t0));
          break;
        }

      syslog(LOG_WARNING,
             "SDTEST: blk %lu off=0x%llx ok %llu us first=%02x%02x%02x%02x\n",
             (unsigned long)i, (unsigned long long)off,
             (unsigned long long)(sd1_mono_us() - t0),
             buf[0], buf[1], buf[2], buf[3]);
    }

  nxmutex_unlock(&g_sd_lock);
  kmm_free(buf);

  syslog(LOG_WARNING, "SDTEST: done n=%lu ret=%d\n", (unsigned long)nblocks, ret);
  return ret;
}

/* 写自检只允许落在卡末尾这一段。它是唯一不属于任何文件系统的区域
 * （FAT32 数据区是 1765693 x 16 x 512 字节，而 sdfat 窗口一直到卡尾，
 * 差出来的约 96 MB 在末尾），而且 /mnt/fat 是只读挂载、本系统从不读写那里。
 * 闸门放在驱动内部是为了让"写坏数据"只有一个不可绕过的入口 —— 两个 LFS
 * 都在卡的前部，任何落在里面的偏移都必须被拒。
 */
#define SF32LB_SD_RAWWRITE_TAIL_BYTES  (1024u * 1024u)

/**
 * @brief 往指定块写 0xFF（与 LittleFS 的"擦除"完全同一种操作）并回读校验。
 *
 * @details 为什么需要它：/mnt/fat 是只读挂载，所以**整个系统里所有的写和
 *          擦除都只发生在两个 LFS 上**。只读测试永远测不到写路径 —— 而驱动
 *          文件头自己就点名了这条路径的危险（"LittleFS 512 B COW + 0xFF
 *          erase fills often program longer than 250 ms. Aborting that wait
 *          with DCR=0 wedges the card"）。本函数就是对那条路径的直接复现。
 *
 *          逐块报 prog=（编程）与 rb=（回读校验）耗时；坏块/边缘卡的典型
 *          特征是某一块编程时间明显拉长或校验不过。
 *
 * @return 0 全部通过；负 errno（首个失败的块）。
 */
int sf32lb_sd_raw_write(uint64_t byte_off, uint32_t nblocks)
{
  FAR uint8_t *wbuf;
  FAR uint8_t *rbuf;
  uint32_t i;
  int ret = 0;

  if (nblocks == 0)
    {
      return -EINVAL;
    }

  if (g_sd_card_bytes == 0 ||
      byte_off < (g_sd_card_bytes - SF32LB_SD_RAWWRITE_TAIL_BYTES) ||
      (byte_off + (uint64_t)nblocks * SD_HW_BLOCK_SIZE) > g_sd_card_bytes)
    {
      syslog(LOG_ERR, "SDTEST: write refused off=0x%llx n=%lu; only the last %lu KiB of the card is allowed\n",
             (unsigned long long)byte_off, (unsigned long)nblocks,
             (unsigned long)(SF32LB_SD_RAWWRITE_TAIL_BYTES / 1024u));
      return -EINVAL;
    }

  wbuf = kmm_malloc(SD_HW_BLOCK_SIZE);
  rbuf = kmm_malloc(SD_HW_BLOCK_SIZE);
  if (wbuf == NULL || rbuf == NULL)
    {
      kmm_free(wbuf);
      kmm_free(rbuf);
      syslog(LOG_ERR, "SDTEST: no mem for write test\n");
      return -ENOMEM;
    }

  memset(wbuf, 0xff, SD_HW_BLOCK_SIZE);

  nxmutex_lock(&g_sd_lock);
  sd1_set_clock(SF32LB_SD_TRAN_CLOCK_HZ);
  g_sd_bus_hz = SF32LB_SD_TRAN_CLOCK_HZ;

  syslog(LOG_WARNING, "SDTEST: 0xFF program off=0x%llx n=%lu @24MHz (bypasses dead gate)\n",
         (unsigned long long)byte_off, (unsigned long)nblocks);

  for (i = 0; i < nblocks; i++)
    {
      uint64_t off = byte_off + (uint64_t)i * SD_HW_BLOCK_SIZE;
      uint64_t t0 = sd1_mono_us();
      uint64_t prog_us;

      ret = sf32lb_sd_write_block(off, wbuf);
      prog_us = sd1_mono_us() - t0;
      if (ret < 0)
        {
          syslog(LOG_ERR, "SDTEST: wr blk %lu off=0x%llx FAIL ret=%d (%llu us)\n",
                 (unsigned long)i, (unsigned long long)off, ret,
                 (unsigned long long)prog_us);
          break;
        }

      memset(rbuf, 0, SD_HW_BLOCK_SIZE);
      t0 = sd1_mono_us();
      ret = sf32lb_sd_read_block(off, rbuf);
      if (ret < 0 || rbuf[0] != 0xff || rbuf[SD_HW_BLOCK_SIZE - 1] != 0xff)
        {
          syslog(LOG_ERR, "SDTEST: verify blk %lu off=0x%llx FAIL ret=%d head=%02x tail=%02x\n",
                 (unsigned long)i, (unsigned long long)off, ret,
                 rbuf[0], rbuf[SD_HW_BLOCK_SIZE - 1]);
          if (ret >= 0)
            {
              ret = -EIO;
            }

          break;
        }

      syslog(LOG_WARNING, "SDTEST: wr blk %lu off=0x%llx ok prog=%llu us rb=%llu us\n",
             (unsigned long)i, (unsigned long long)off,
             (unsigned long long)prog_us,
             (unsigned long long)(sd1_mono_us() - t0));
    }

  nxmutex_unlock(&g_sd_lock);
  kmm_free(wbuf);
  kmm_free(rbuf);

  syslog(LOG_WARNING, "SDTEST: wr done n=%lu ret=%d\n", (unsigned long)nblocks, ret);
  return ret;
}

int sf32lb_sd_crash_write512(uint64_t byte_off, FAR const void *buf)
{
  if (buf == NULL)
    {
      return -EINVAL;
    }

  return sf32lb_sd_write_block(byte_off, (FAR const uint8_t *)buf);
}

#endif /* CONFIG_MTD */
