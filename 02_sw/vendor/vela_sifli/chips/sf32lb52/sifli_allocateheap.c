/****************************************************************************
 * vendor/sifli/chip/sf32lb52/sifli_uart.c
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
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>

#include <stdbool.h>

#if defined(CONFIG_MYVENDOR_MTP_SIMPLE) && defined(CONFIG_BSP_USING_PSRAM)
#  ifndef CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB
#    define CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB 256
#  endif
#  define MTP_PSRAM_MTP_RESERVE_BYTES \
    ((size_t)CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB * 1024u)
#endif

#if defined(CONFIG_BSP_USING_PSRAM)
#  if defined(CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB)
#    define BOARD_PSRAM_POOL_KB CONFIG_MYVENDOR_BOARD_PSRAM_POOL_KB
#  else
#    define BOARD_PSRAM_POOL_KB 1024
#  endif
#  define BOARD_PSRAM_POOL_BYTES ((size_t)BOARD_PSRAM_POOL_KB * 1024u)
#endif

#include "chip.h"
#include "arm_internal.h"
#include "bf0_hal.h"
#include "mem_map.h"

extern void BSP_PIN_Init(void);
extern void BSP_Power_Up(bool is_deep_sleep);
extern void BSP_Board_PreInit(void);
extern void BSP_Board_EnableSdkFlashClocks(void);
extern void BSP_Board_EnableHclk240(void);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SRAM memory configuration for SF32LB52 */

#define SRAM_START  0x20000000
#define SRAM_SIZE   0x00080000    /* 512 KB */
#define SRAM_END    (SRAM_START + SRAM_SIZE)

/* HCPU2LCPU mailbox sits at the top of HPSYS SRAM (CH2 then CH1, 2 x 512 B).
 *
 * BSP pitfall (not Vela mm_heap, not OOM): mem_map.h already reserves
 * HPSYS_MBOX_BUF at SRAM top (0x2007FC00, 1 KiB). An earlier port gave
 * Umem [g_idle_topstack, SRAM_END). BLE HCI then wrote HCPU2LCPU_MB_CH1
 * (0x2007FE00) into a live free-chunk header. mallinfo/nsh `free` asserted;
 * malloc itself does not canary-check region overlap.
 *
 * Umem SRAM must end at HPSYS_MBOX_BUF_ADDR. Do not extend kumm to SRAM_END.
 */
#define SRAM_HEAP_END  HPSYS_MBOX_BUF_ADDR

#if SRAM_HEAP_END != (SRAM_END - HPSYS_MBOX_BUF_SIZE)
#  error "HPSYS mailbox is not at the top of SRAM; SRAM_HEAP_END mismatch"
#endif

/* PSRAM: 16MB HyperBus (MPI1 MODE_6). Code uses CBUS 0x10000000 (first 4MB);
 * heap uses PSRAM_DATA @ 0x60400000 (remaining 12MB per SDK ptab.json).
 */

#define PSRAM_HEAP_START  0x60400000
#define PSRAM_HEAP_SIZE   0x00C00000    /* 12 MB */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_BSP_USING_PSRAM
static bool g_psram_ready;

static void sifli_psram_preinit(void)
{
  qspi_configure_t qspi_cfg =
  {
    .Instance = hwp_qspi1,
    .SpiMode  = CONFIG_BSP_QSPI1_MODE,
    .msize    = CONFIG_BSP_QSPI1_MEM_SIZE,
    .base     = QSPI1_MEM_BASE,
  };
  static FLASH_HandleTypeDef psram_handle;

  /* Enable 1.8V LDO required by PSRAM. */

  HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, true, true);

  /* Use SYSCLK for early boot safety. DLL2 path is enabled later by HAL. */

  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_SYSCLK);

  /* Use configured PSRAM mode directly to avoid early-boot PID dependency. */

  if (qspi_cfg.SpiMode == SPI_MODE_NOR)
    {
      g_psram_ready = false;
      return;
    }

  /* Avoid early power-mode query here; HAL_Init will handle PM state later. */

  psram_handle.wakeup = 0;

  /* Keep divider aligned with existing board implementation. */

  g_psram_ready = (HAL_MPI_PSRAM_Init(&psram_handle, &qspi_cfg, 2) == HAL_OK);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void HAL_MspInit(void)
{
  BSP_PIN_Init();
  BSP_Power_Up(true);
}

void HAL_PreInit(void)
{
  BSP_Board_PreInit();

#ifdef CONFIG_BSP_USING_PSRAM
#if !defined(CONFIG_BSP_USING_SPI_NAND)
  BSP_Board_EnableSdkFlashClocks();
  BSP_Board_EnableHclk240();
#endif

  HAL_MspInit();

#if defined(CONFIG_BSP_USING_SPI_NAND)
  /* ★ 把 PSRAM 总线钉到 DLL2/2 = 144 MHz（官方设计）。
   *
   * 出处：`SDK/2.4/docs/source/app_development/startup_flow_sf32lb52x.md` 的时钟表 ——
   * 内置 PSRAM 走 **DLL2**、固定 **144 MHz**，与大核系统时钟（走 DLL1）解耦；SDK 里
   * 每一块板也都 `ClockSelect(FLASH1, RCC_CLK_FLASH_DLL2)`。
   *
   * 为什么必须由这里补：本配置下 App 的代码**就在 PSRAM 里**（`ld.script` 的
   * `flash` 区 = `0x10000000` = `HCPU_PSRAM_CODE`），所以只能切**时钟源与分频**，
   * **绝不能重跑 `HAL_MPI_PSRAM_Init()`** —— 那会 reset 器件、把正在执行的代码抹掉。
   * 但 bootloader 把 FLASH1 留在了 SYSCLK 上（它的 `bootloader_switch_clock()` 明确
   * 不选 DLL2：那时 HXT48 还没起），于是 PSRAM 一直跟着 HCLK 从 144 荡到 240，
   * 而读写延迟码只在开机按低时钟算过一次。
   *
   * 延迟码**故意不动**：实测它在 240 MHz 下已经跑得住（代码就是从 PSRAM 取指的，
   * 读延迟不够会立刻取指错），144 MHz 只会更宽松；而重写 MR0/MR4 的那几行本身
   * 也在 PSRAM 里（`bf0_hal_mpi_psram.c` 没有 `.ramfunc`），"改读时序的同时从它取指"
   * 才是真正的风险。**频率一旦固定，"延迟一次算准、全档通用"这个性质就成立了。**
   *
   * ★ 为什么 DLL2 取 **240 MHz** 而不是官方的 288：
   *   OPI 模式下分频恒为 1（`HAL_OPI_PSRAM_Init()` 里写的是常量 1，校准函数
   *   `HAL_MPI_OPSRAM_CAL_DELAY()` 也只是临时改成 2 再恢复），所以
   *   `HAL_QSPI_GET_CLK` = DLL2 频率本身。而**读写延迟码只在开机按低时钟算过一次
   *   （w_lat=3），没人重算** —— 厂商表在等效 144（DLL2 288）那一行要 w_lat=6。
   *   实测：**DLL2=288 起不来**（板子卡在加载后重启，见下），而 **DLL2=240 与
   *   改之前 SYSCLK 240/1 的 `HAL_QSPI_GET_CLK` 是同一个数**，等于速度不变、
   *   只是把时钟源从这个会跟着 HCLK 荡的域换到了一个不动的域。240 也是厂商
   *   用过的值（`bsp_init.c` 的 USB 分支就 `EnableDLL2(240000000)`）。
   *
   * ⚠ **不要碰 `PSCLR`**：曾经在这里写过 `PSCLR = 2`（等效降到 72），那比现在还慢。
   *   OPI 下它是常量 1，改它等于背离厂商的配置形状。
   *
   * 权衡记在这里：想要官方那个等效 144（DLL2 288/1），前提是**把延迟码
   * 重算成 w_lat=6/r_lat=12**；而重写 MR0/MR4 的那几行本身也在 PSRAM 里
   * （`bf0_hal_mpi_psram.c` 没有 `.ramfunc`），"改读时序的同时从它取指"是
   * 另一个量级的风险，没做。
   *
   * XT48 由上面的 `BSP_Board_PreInit()` 打开（DLL2 靠它）。失败就维持 bootloader
   * 留下的 SYSCLK 路径 —— 也就是现状，不会更糟。 */
  if (HAL_RCC_HCPU_EnableDLL2(240000000) == HAL_OK)
    {
      HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_DLL2);
    }

  g_psram_ready = true;
#else
  sifli_psram_preinit();
#endif
#endif
}

/****************************************************************************
 * Name: up_allocate_heap/up_allocate_kheap
 *
 * Description:
 *   This function will be called to dynamically set aside the heap region.
 *
 *   - For the normal "flat" build, this function returns the size of the
 *     single heap.
 *   - For the protected build (CONFIG_BUILD_PROTECTED=y) with both kernel-
 *     and user-space heaps (CONFIG_MM_KERNEL_HEAP=y), this function
 *     provides the size of the unprotected, user-space heap.
 *   - For the kernel build (CONFIG_BUILD_KERNEL=y), this function provides
 *     the size of the protected, kernel-space heap.
 *
 *   If a protected kernel-space heap is provided, the kernel heap must be
 *   allocated by an analogous up_allocate_kheap(). A custom version of this
 *   file is needed if memory protection of the kernel heap is required.
 *
 *   The following memory map is assumed for the flat build:
 *
 *     .data region.  Size determined at link time.
 *     .bss  region  Size determined at link time.
 *     IDLE thread stack.  Size determined by CONFIG_IDLETHREAD_STACKSIZE.
 *     Heap.  Extends to SRAM_HEAP_END (HCPU2LCPU mailbox reserved at top).
 *
 *   The following memory map is assumed for the kernel build:
 *
 *     Kernel .data region.  Size determined at link time.
 *     Kernel .bss  region  Size determined at link time.
 *     Kernel IDLE thread stack.  Size determined by
 *       CONFIG_IDLETHREAD_STACKSIZE.
 *     Padding for alignment
 *     User .data region.  Size determined at link time.
 *     User .bss region  Size determined at link time.
 *     Kernel heap.  Size determined by CONFIG_MM_KERNEL_HEAPSIZE.
 *     User heap.  Extends to SRAM_HEAP_END (below the HCPU2LCPU mailbox).
 *
 ****************************************************************************/

void up_allocate_heap(FAR void **heap_start, size_t *heap_size)
{
  /* Heap starts at g_idle_topstack and stops before the BT mailbox. */

  *heap_start = (FAR void *)g_idle_topstack;
  *heap_size  = SRAM_HEAP_END - g_idle_topstack;
}

/******************************************************************************
 * Name: arm_addregion
 *
 * Description:
 *   Memory may be added in non-contiguous chunks.  Additional chunks are
 *   added by calling this function.
 *
 ******************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm_addregion(void)
{
#ifdef CONFIG_BSP_USING_PSRAM
  if (g_psram_ready)
    {
      size_t psram_heap = PSRAM_HEAP_SIZE;

#if defined(CONFIG_BSP_USING_PSRAM) && defined(BOARD_PSRAM_POOL_BYTES)
      if (BOARD_PSRAM_POOL_KB > 0 && psram_heap > BOARD_PSRAM_POOL_BYTES)
        {
          psram_heap -= BOARD_PSRAM_POOL_BYTES;
        }
#endif

#if defined(CONFIG_MYVENDOR_MTP_SIMPLE)
      if (psram_heap > MTP_PSRAM_MTP_RESERVE_BYTES)
        {
          psram_heap -= MTP_PSRAM_MTP_RESERVE_BYTES;
        }
#endif

      kumm_addregion((void *)PSRAM_HEAP_START, psram_heap);
    }
#endif
}
#endif
