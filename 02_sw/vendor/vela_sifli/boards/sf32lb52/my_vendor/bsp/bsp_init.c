/**
 * @file bsp_init.c
 * @brief HCPU/LCPU 早期时钟、Flash/PSRAM MPI 与 OTP 基址配置。
 *
 * BSP_Board_PreInit 在 HAL 早期路径调用；Flash 时钟与 HCLK 240 分阶段使能，
 * 避免 PSRAM XIP 期间重配引脚导致取指故障。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <syslog.h>

#include "bsp_board.h"
#include "bf0_hal_rtc.h"

#ifndef LXT_LP_CYCLE
    #define LXT_LP_CYCLE 200
#endif

static uint16_t mpi1_div = 1; /**< MPI1（PSRAM/Flash1）软件分频。 */
static uint16_t mpi2_div = 1; /**< MPI2（NAND/Flash2）软件分频。 */

static uint32_t otp_flash_addr = AUTO_FLASH_MAC_ADDRESS; /**< OTP/MAC 基址。 */

#define FUNC_BSP_FLASH_DIV_GET(i) \
uint16_t BSP_GetFlash##i##DIV(void) \
{ \
    return mpi##i##_div; \
}\

#define FUNC_BSP_FLASH_DIV_SET(i) \
void BSP_SetFlash##i##DIV(uint16_t div) \
{ \
    mpi##i##_div = div; \
}\

FUNC_BSP_FLASH_DIV_GET(1);
FUNC_BSP_FLASH_DIV_GET(2);

FUNC_BSP_FLASH_DIV_SET(1)
FUNC_BSP_FLASH_DIV_SET(2)

int rt_psram_init(void);
int rt_hw_flash1_init(uint8_t auto_detect);
int rt_hw_flash2_init(uint8_t auto_detect);
int rt_hw_flash_init(void);

/**
 * @brief 读取 OTP/Flash MAC 区基址。
 * @return 物理地址。
 */
uint32_t BSP_GetOtpBase(void)
{
    return otp_flash_addr;
}

#ifdef SOC_BF0_HCPU
#define HXT_DELAY_EXP_VAL 1000
/** @brief 初始化 LRC 参考时钟与 HXT 就绪延时。 */
static void LRC_init(void)
{
    HAL_PMU_RC10Kconfig();

    HAL_RC_CAL_update_reference_cycle_on_48M(LXT_LP_CYCLE);
    uint32_t ref_cnt = HAL_RC_CAL_get_reference_cycle_on_48M();
    uint32_t cycle_t = (uint32_t)ref_cnt / (48 * LXT_LP_CYCLE);

    HAL_PMU_SET_HXT3_RDY_DELAY((HXT_DELAY_EXP_VAL / cycle_t + 1));
}
#endif

#ifdef BSP_USING_PSRAM
/** @brief 按芯片 PID 选择 PSRAM 模式并 HAL_MPI_PSRAM_Init。 */
static void board_init_psram(void)
{
    qspi_configure_t qspi_cfg =
    {
        .Instance = hwp_qspi1,
        .SpiMode = BSP_QSPI1_MODE,
        .msize = BSP_QSPI1_MEM_SIZE,
        .base = QSPI1_MEM_BASE,
    };
    uint32_t pid = (hwp_hpsys_cfg->IDR & HPSYS_CFG_IDR_PID_Msk) >> HPSYS_CFG_IDR_PID_Pos;
    pid &= 7;

    switch (pid)
    {
    case 5: //BOOT_PSRAM_APS_16P:
        qspi_cfg.SpiMode = SPI_MODE_PSRAM;         // 16Mb APM QSPI PSRAM
        break;
    case 4: //BOOT_PSRAM_APS_32P:
        qspi_cfg.SpiMode = SPI_MODE_LEGPSRAM;    // 32Mb APM LEGACY PSRAM
        break;
    case 6: //BOOT_PSRAM_WINBOND:                // Winbond HYPERBUS PSRAM
        qspi_cfg.SpiMode = SPI_MODE_HBPSRAM;
        break;
    case 3: // BOOT_PSRAM_APS_64P:
    case 2: //BOOT_PSRAM_APS_128P:
        qspi_cfg.SpiMode = SPI_MODE_OPSRAM;      // 64Mb APM XCELLA PSRAM
        break;
    default:
        qspi_cfg.SpiMode = SPI_MODE_NOR;
        break;
    }
    static FLASH_HandleTypeDef f_handle;
    if (PM_STANDBY_BOOT != SystemPowerOnModeGet())
        f_handle.wakeup = 0;
    else
        f_handle.wakeup = 1;
    HAL_MPI_PSRAM_Init(&f_handle, &qspi_cfg, mpi1_div);
}
#endif

/** @brief 板级预初始化：系统/外设时钟、LCPU、RTC32K、WDT 等。 */
void BSP_Board_PreInit(void)
{
#ifdef SOC_BF0_HCPU

    /* not switch back to XT48 if other clock source has been selected already */
    if (RCC_SYSCLK_HRC48 == HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_SYS))
    {
        HAL_HPAON_EnableXT48();
        HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_HXT48);
    }

    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_HP_PERI, RCC_CLK_PERI_HXT48);

    if (PM_STANDBY_BOOT != SystemPowerOnModeGet())
    {
        HAL_HPAON_WakeCore(CORE_ID_LCPU);
        HAL_RCC_Reset_and_Halt_LCPU(1);
#ifndef USE_ATE_MODE
        BSP_System_Config();
#endif
        HAL_HPAON_StartGTimer();
        HAL_PMU_EnableRC32K(1);
        HAL_PMU_LpCLockSelect(PMU_LPCLK_RC32);

        HAL_PMU_EnableDLL(1);

#ifndef LXT_DISABLE
        HAL_PMU_EnableXTAL32();
        if (HAL_PMU_LXTReady() == HAL_OK)
        {
            HAL_RTC_ENABLE_LXT();
        }
#endif

#ifndef CFG_BOOTLOADER
        {
            HAL_PMU_SetWdt((uint32_t)hwp_wdt2);
        }
#endif

        HAL_RCC_LCPU_ClockSelect(RCC_CLK_MOD_LP_PERI, RCC_CLK_PERI_HXT48);

        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
        if (HAL_LXT_DISABLED())
            LRC_init();
    }

    /* Flash/PSRAM MPI clocks and HCLK 240 are applied later; see
     * BSP_Board_EnableSdkFlashClocks() / BSP_Board_EnableHclk240(). */

#elif defined(SOC_BF0_LCPU)
    HAL_LPAON_EnableXT48();
    HAL_RCC_LCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_HXT48);
    HAL_RCC_LCPU_ClockSelect(RCC_CLK_MOD_LP_PERI, RCC_CLK_PERI_HXT48);
    HAL_RCC_LCPU_SetDiv(2, 1, 3);
    HAL_MspInit();
#endif
}

/**
 * @brief 使能 DLL2 + MPI 分频 + FLASH1/2 路由（PSRAM 144MHz，NAND 72MHz）。
 *
 * 不提升 HCLK 至 240MHz；PSRAM XIP 启动时在 pinmux 之后调用更安全。
 */
void BSP_Board_EnableSdkFlashClocks(void)
{
#ifdef SOC_BF0_HCPU
#if defined(CONFIG_BSP_USING_SPI_NAND)
  /* Bootloader already configured MPI1/PSRAM (FLASH1<-DLL2, div=2, ~144MHz)
   * for XIP. Do not touch FLASH1/DLL2 here. NAND FLASH2 clocks are raised in
   * BSP_Board_EnableNandMpiClocks() before HAL_FLASH_Init. */
  mpi1_div = 2;
  mpi2_div = 4;
  return;
#endif

#if defined(BSP_USING_USBD) || defined(BSP_USING_USBH)
    HAL_RCC_HCPU_EnableDLL2(240000000);
    hwp_hpsys_rcc->USBCR = 4;
    hwp_hpsys_rcc->CSR |= HPSYS_RCC_CSR_SEL_USBC;
#else
    HAL_RCC_HCPU_EnableDLL2(288000000);
#endif

    HAL_Delay_us(0);

    mpi1_div = 2;
    mpi2_div = 4;

    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_DLL2);
#if defined(BSP_USING_PSRAM)
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, true, true);
#if !defined(CONFIG_BSP_USING_SPI_NAND)
    board_init_psram();
#endif
#endif

    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH2, RCC_CLK_FLASH_DLL2);
#if defined(BSP_USING_NOR_FLASH1) || defined(BSP_USING_NOR_FLASH2)
    // TODO: move select FLASH1 clock to here if MPI1 used as FLASH.
#ifdef BSP_USING_NOR_FLASH1
    mpi1_div = 3;
#endif
    if (PM_STANDBY_BOOT == SystemPowerOnModeGet())
    {
        //TODO: pin device is not restored
        HAL_HPAON_ENABLE_PAD();
        /* rt_hw_flash_init cannot be called as data has not been restored at this moment,
           so rt_sem_init cannot be called */
#if defined(BSP_USING_NOR_FLASH1)
        BSP_Flash_hw1_init();
#endif
#if defined(BSP_USING_NOR_FLASH2)
        BSP_Flash_hw2_init();
#endif
    }
    else
    {
#ifdef BSP_USING_RTTHREAD
        rt_hw_flash_init();
#else
        BSP_Flash_Init();
#endif
    }
#endif /* BSP_USING_NOR_FLASH1 || BSP_USING_NOR_FLASH2 */
#endif /* SOC_BF0_HCPU */
}

/**
 * @brief 使能 MPI2，FLASH2 走 SYSCLK（约 48MHz，NAND 启动路径）。
 *
 * 不使用 DLL2，避免与 PSRAM XIP 的 FLASH1 争用。
 */
void BSP_Board_EnableNandMpiClocks(void)
{
#ifdef SOC_BF0_HCPU
#if defined(CONFIG_BSP_USING_SPI_NAND)
  /* Bootloader XIP: MPI1 stays on DLL2/div2; only publish div for reporting. */
  mpi1_div = 2;
  mpi2_div = 2;
  BSP_SetFlash2DIV(2);
  HAL_RCC_EnableModule(RCC_MOD_DMAC1);
  HAL_RCC_EnableModule(RCC_MOD_MPI2);
  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH2, RCC_CLK_FLASH_SYSCLK);
  HAL_Delay_us(0);
#endif
#endif
}

/**
 * @brief 根据 RCC 源与 BSP 分频计算 MPI 时钟频率。
 * @param clk_module RCC 时钟模块（FLASH1/FLASH2）。
 * @param clk_div 软件分频系数。
 * @param hcpu 非 0 为 HCPU，0 为 LCPU。
 * @return Hz；clk_div&lt;=0 返回 0。
 */
uint32_t flash_get_freq(int clk_module, uint16_t clk_div, uint8_t hcpu)
{
  int src;
  uint32_t freq;

  if (clk_div <= 0)
    {
      return 0;
    }

  if (hcpu == 0)
    {
      freq = HAL_RCC_GetSysCLKFreq(CORE_ID_LCPU);
      return freq / clk_div;
    }

  src = HAL_RCC_HCPU_GetClockSrc(clk_module);
#ifdef SOC_SF32LB52X
  if (RCC_CLK_FLASH_DLL2 == src)
    {
      freq = HAL_RCC_HCPU_GetDLL2Freq();
    }
  else if (RCC_CLK_SRC_DLL1 == src)
    {
      freq = HAL_RCC_HCPU_GetDLL1Freq();
    }
  else if (3 == src)
    {
      freq = 96000000;
    }
  else
    {
      freq = HAL_RCC_GetSysCLKFreq(CORE_ID_HCPU);
    }
#else
  if (RCC_CLK_FLASH_DLL2 == src)
    {
      freq = HAL_RCC_HCPU_GetDLL2Freq();
    }
  else
    {
      freq = HAL_RCC_GetSysCLKFreq(CORE_ID_HCPU);
    }
#endif

  return freq / clk_div;
}

#ifdef SOC_BF0_HCPU

/** @brief 系统时钟源名称（日志用）。 */
static const char *bsp_clk_sys_src_name(int src)
{
  switch (src)
    {
    case RCC_SYSCLK_HRC48:
      return "HRC48";
    case RCC_SYSCLK_HXT48:
      return "HXT48";
    case RCC_SYSCLK_DLL1:
      return "DLL1";
    default:
      return "?";
    }
}

/** @brief Flash 时钟源名称（日志用）。 */
static const char *bsp_clk_flash_src_name(int src)
{
  switch (src)
    {
    case RCC_CLK_FLASH_SYSCLK:
      return "SYSCLK";
    case RCC_CLK_FLASH_DLL2:
      return "DLL2";
    case RCC_CLK_SRC_DLL1:
      return "DLL1";
    default:
      return "?";
    }
}

/** @brief 读 MPI 硬件 PSCLR 分频。 */
static uint8_t bsp_mpi_hw_psclr_div(MPI_TypeDef *inst)
{
  if (inst == NULL)
    {
      return 0;
    }

  return (uint8_t)GET_REG_VAL(inst->PSCLR, MPI_PSCLR_DIV_Msk, MPI_PSCLR_DIV_Pos);
}

/** @brief 打印 HCPU sysclk/hclk 与 FLASH1/2 MPI 频率（NAND XIP 诊断）。 */
void BSP_Board_PrintClocks(void)
{
  uint32_t sysclk;
  uint32_t hclk;
  int sys_src;
  int flash1_src;
  int flash2_src;
  uint32_t flash1_hz;
  uint32_t flash2_hz;
  uint32_t dll2_hz;
  uint8_t mpi1_psclr;
  uint8_t mpi2_psclr;
  bool dll2_en;

  sysclk = HAL_RCC_GetSysCLKFreq(CORE_ID_HCPU);
  hclk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
  sys_src = HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_SYS);
  flash1_src = HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_FLASH1);
  flash2_src = HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_FLASH2);

  flash1_hz = flash_get_freq(RCC_CLK_MOD_FLASH1, BSP_GetFlash1DIV(), 1);
  flash2_hz = flash_get_freq(RCC_CLK_MOD_FLASH2, BSP_GetFlash2DIV(), 1);

  mpi1_psclr = bsp_mpi_hw_psclr_div(hwp_qspi1);
  mpi2_psclr = bsp_mpi_hw_psclr_div(hwp_qspi2);

  dll2_en = (hwp_hpsys_rcc->DLL2CR & HPSYS_RCC_DLL2CR_EN) != 0;
  dll2_hz = dll2_en ? HAL_RCC_HCPU_GetDLL2Freq() : 0;

  syslog(LOG_INFO, "CLK: HCPU sysclk=%lu Hz (%s) hclk=%lu Hz\n",
         (unsigned long)sysclk, bsp_clk_sys_src_name(sys_src),
         (unsigned long)hclk);
  syslog(LOG_INFO,
         "CLK: DLL2 %s (%lu Hz) FLASH1=%s sw_div=%u hw_psclr=%u ~%lu Hz (PSRAM XIP)\n",
         dll2_en ? "on" : "off", (unsigned long)dll2_hz,
         bsp_clk_flash_src_name(flash1_src),
         (unsigned)BSP_GetFlash1DIV(), (unsigned)mpi1_psclr,
         (unsigned long)flash1_hz);
  syslog(LOG_INFO,
         "CLK: FLASH2=%s sw_div=%u hw_psclr=%u ~%lu Hz (NAND)\n",
         bsp_clk_flash_src_name(flash2_src),
         (unsigned)BSP_GetFlash2DIV(), (unsigned)mpi2_psclr,
         (unsigned long)flash2_hz);
}

#endif /* SOC_BF0_HCPU */

/**
 * @brief 将 HCPU HCLK 提至 240 MHz。
 *
 * 须在 HAL_Init 早期路径完成且 PSRAM XIP 稳定后调用（NAND 启动）。
 */
void BSP_Board_EnableHclk240(void)
{
#ifdef SOC_BF0_HCPU
#ifndef USE_ATE_MODE
    SCB_InvalidateICache();
    HAL_RCC_HCPU_ConfigHCLK(240);
#else
    HAL_RCC_HCPU_SetDiv(1, 1, 6);
    HAL_RCC_HCPU_EnableDLL1(240000000);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_DLL1);
#endif
    HAL_Delay_us(0);
#endif /* SOC_BF0_HCPU */
}

extern void BSP_PIN_Init(void);

/** @brief HAL_MspInit 调用：pinmux + 板级上电。 */
void BSP_IO_Init(void)
{
    BSP_PIN_Init();
    BSP_Power_Up(true);
}

/** @brief 弱符号：应用可覆盖 SystemClock_Config。 */
__WEAK void SystemClock_Config(void)
{

}

