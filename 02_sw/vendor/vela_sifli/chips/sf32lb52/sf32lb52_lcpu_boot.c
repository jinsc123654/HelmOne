/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb52_lcpu_boot.c
 ****************************************************************************/

#include <sfconfig.h>
#include <bf0_hal.h>
#include <bf0_hal_patch.h>
#include <nuttx/cache.h>
#include <string.h>
#include <syslog.h>

#include "mem_map.h"
#include "lcpu_config_type_int.h"

_Static_assert(sizeof(hal_lcpu_bluetooth_em_config_t) ==
               LCPU_CONFIG_EM_BUF_ROM_LENGTH,
               "EM config size must match LCPU ROM slot");
_Static_assert(sizeof(hal_lcpu_bluetooth_act_configt_t) ==
               LCPU_CONFIG_BT_ACT_CONFIG_ROM_LENGTH,
               "ACT config size must match LCPU ROM slot");
_Static_assert(sizeof(hal_lcpu_bluetooth_rom_config_t) ==
               LCPU_CONFIG_BT_ROM_CONFIG_ROM_LENGTH,
               "BT ROM config size must match LCPU ROM slot");
_Static_assert(sizeof(hal_lcpu_ble_mem_config_t) ==
               LCPU_CONFIG_KE_MEM_CONFIG_ROM_LENGTH,
               "KE config size must match LCPU ROM slot");

#if (!defined(SF32LB52X_REV_B)) && !defined(LCPU_RUN_ROM_ONLY)
#  define g_lcpu_bin g_lcpu_bin_legacy
#  include "../../middleware/bluetooth/patch/sf32lb52/sf32lb52_lcpu.h"
#  undef g_lcpu_bin
#  define g_lcpu_patch_list g_lcpu_patch_list_legacy
#  define g_lcpu_patch_bin g_lcpu_patch_bin_legacy
#  include "../../middleware/bluetooth/patch/sf32lb52/sf32lb52_lcpu_patch.h"
#  undef g_lcpu_patch_list
#  undef g_lcpu_patch_bin
#endif

#if defined(APP_BSP_TEST)
#  define bt_rf_cal()
#else
#  if defined(FPGA)
#    define bt_rf_cal() bt_rf_cal_9364()
#  endif
extern void bt_rf_cal(void);
#endif

#if defined(SOC_BF0_HCPU)
extern void lcpu_patch_install_rev_b(void);
extern uint16_t LCPU_CONFIG_get_total_size(void);

static uint8_t g_lcpu_rf_cal_disable;

static void sf32lb52_lcpu_boot_clean_range(uint32_t addr, uint32_t size)
{
  if (size == 0)
    {
      return;
    }

  up_clean_dcache((uintptr_t)addr, (uintptr_t)addr + size);
}

__WEAK void adc_resume(void)
{
}

#if (!defined(SF32LB52X_REV_B)) && !defined(LCPU_RUN_ROM_ONLY)
void lcpu_img_install(void)
{
  if (__HAL_SYSCFG_GET_REVID() < HAL_CHIP_REV_ID_A4)
    {
      memcpy((void *)HCPU_LCPU_CODE_START_ADDR,
             g_lcpu_bin_legacy,
             sizeof(g_lcpu_bin_legacy));
      sf32lb52_lcpu_boot_clean_range(HCPU_LCPU_CODE_START_ADDR,
                                     sizeof(g_lcpu_bin_legacy));
    }
}

static void lcpu_patch_install_legacy(void)
{
  memcpy((void *)LCPU_PATCH_RECORD_ADDR,
         g_lcpu_patch_list_legacy,
         sizeof(g_lcpu_patch_list_legacy));
  sf32lb52_lcpu_boot_clean_range(LCPU_PATCH_RECORD_ADDR,
                                 sizeof(g_lcpu_patch_list_legacy));
  HAL_PATCH_install();
  memset((void *)LCPU_PATCH_START_ADDR_S, 0, LCPU_PATCH_TOTAL_SIZE);
  memcpy((void *)LCPU_PATCH_START_ADDR_S,
         g_lcpu_patch_bin_legacy,
         sizeof(g_lcpu_patch_bin_legacy));
  sf32lb52_lcpu_boot_clean_range(LCPU_PATCH_START_ADDR_S,
                                 LCPU_PATCH_TOTAL_SIZE);
}
#else
#  define lcpu_img_install()
#endif

void lcpu_rom_config_default(void)
{
  uint8_t rev_id = __HAL_SYSCFG_GET_REVID();
  uint8_t is_enable_lxt;
  uint8_t is_lcpu_rccal = 0;
  uint32_t wdt_staus = 0xFF;
  uint32_t wdt_time = 10;
  uint16_t wdt_clk = 32768;

  if (HAL_LXT_DISABLED())
    {
      is_lcpu_rccal = 1;
    }

  is_enable_lxt = 1 - is_lcpu_rccal;
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_XTAL_ENABLED, &is_enable_lxt, 1);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_WDT_STATUS, &wdt_staus, 4);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_WDT_TIME, &wdt_time, 4);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_WDT_CLK_FEQ, &wdt_clk, 2);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_RC_CAL_IN_L, &is_lcpu_rccal, 1);

#if defined(SF32LB52X_REV_B) || defined(SF32LB52X_REV_AUTO)
  if (rev_id >= HAL_CHIP_REV_ID_A4)
    {
      uint32_t tx_queue = HCPU2LCPU_MB_CH1_BUF_START_ADDR;
      hal_lcpu_bluetooth_rom_config_t config = {0};
      hal_lcpu_ble_mem_config_t ble_config = {0};
      hal_lcpu_bluetooth_em_config_t em_cfg = {0};
      hal_lcpu_bluetooth_act_configt_t act_cfg = {0};
      /* Dual-mode EM map from SiFli printer/peripheral examples
       * (SF32LB52 Rev B, 24 KB).  Trailing slots stay 0.
       */
      static const uint16_t em_offset[HAL_LCPU_CONFIG_EM_BUF_MAX_NUM] =
        {
          0x0178, 0x0178, 0x0740, 0x07a0, 0x0810, 0x0880, 0x0a00, 0x0bb0,
          0x0d48, 0x133c, 0x13a4, 0x19bc, 0x21bc, 0x21bc, 0x21bc, 0x21bc,
          0x21bc, 0x21bc, 0x21bc, 0x21bc, 0x263c, 0x265c, 0x2734, 0x2784,
          0x28d4, 0x28e8, 0x28fc, 0x29ec, 0x29fc, 0x2bbc, 0x2bd8, 0x3be8,
          0x5804, 0x5804, 0x5804
        };

      /* bit0 BLE, bit1 Classic.  File transfer is GATT-only. */
      config.bit_valid |= (1u << 10) | (1u << 6) | (1u << 2) | (1u << 1);
      config.controller_enable_bit = 0x01;
      config.lld_prog_delay = 3;
      config.is_fpga = 0;
      config.default_xtal_enabled = is_enable_lxt;
      HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_HCPU_TX_QUEUE, &tx_queue, 4);
      HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_CONFIG, &config, sizeof(config));

      memcpy(em_cfg.em_buf, em_offset, sizeof(em_offset));
      em_cfg.is_valid = 1;
      HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_EM_BUF, &em_cfg, sizeof(em_cfg));

      act_cfg.bt_max_acl = 7;
      act_cfg.bt_max_sco = 0;
      act_cfg.ble_max_act = 6;
      act_cfg.ble_max_ral = 3;
      act_cfg.ble_max_iso = 0;
      act_cfg.bit_valid = (1u << 0) | (1u << 1) | (1u << 2) |
                          (1u << 3) | (1u << 4);
      HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_ACT_CFG,
                          &act_cfg, sizeof(act_cfg));

      ble_config.max_nb_of_hci_completed = 6;
      ble_config.bit_valid = 1 << 6;
      HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_KE_BUF,
                          &ble_config,
                          sizeof(ble_config));
      syslog(LOG_INFO,
             "sf32lb52 lcpu BLE-only enable=0x%02x em_valid=%u\n",
             config.controller_enable_bit, em_cfg.is_valid);
    }
#else
  (void)rev_id;
#endif
}

__WEAK void lcpu_rom_config(void)
{
  lcpu_rom_config_default();
}

static void lcpu_ble_patch_install(void)
{
  uint8_t rev_id = __HAL_SYSCFG_GET_REVID();

  if (rev_id < HAL_CHIP_REV_ID_A4)
    {
#if !defined(SF32LB52X_REV_B)
      lcpu_patch_install_legacy();
#else
      HAL_ASSERT(0 && "Wrongly config");
#endif
    }
  else
    {
#if !defined(SF32LB52X_REV_A)
      memset((void *)0x20400000, 0, 0x500);
      lcpu_patch_install_rev_b();
#else
      HAL_ASSERT(0 && "Wrongly config");
#endif
    }

  if (g_lcpu_rf_cal_disable == 0)
    {
      bt_rf_cal();
    }

  adc_resume();
  /* RF cal uses EM.  Zero the full 24 KB so Classic page-scan
   * descriptors at the high end are not leftover cal junk.
   */
  memset((void *)LPSYS_EM_BASE, 0, LPSYS_EM_SIZE);
  sf32lb52_lcpu_boot_clean_range(LPSYS_EM_BASE, LPSYS_EM_SIZE);
}

void lcpu_disable_rf_cal(uint8_t is_disable)
{
  g_lcpu_rf_cal_disable = is_disable;
}

__WEAK __NOINLINE void lcpu_nvds_config(void)
{
}

uint8_t lcpu_power_on(void)
{
  HAL_HPAON_WakeCore(CORE_ID_LCPU);
  HAL_RCC_Reset_and_Halt_LCPU(0);

  lcpu_nvds_config();
  lcpu_rom_config();
  sf32lb52_lcpu_boot_clean_range(LCPU_CONFIG_START_ADDR,
                                 LCPU_CONFIG_get_total_size());

  if (HAL_RCC_GetHCLKFreq(CORE_ID_LCPU) > 24000000)
    {
      HAL_RCC_LCPU_SetDiv(2, 1, 5);
      HAL_ASSERT(HAL_RCC_GetHCLKFreq(CORE_ID_LCPU) <= 24000000);
    }

  if (__HAL_SYSCFG_GET_REVID() < HAL_CHIP_REV_ID_A4)
    {
      lcpu_img_install();
    }

  HAL_LPAON_ConfigStartAddr((uint32_t *)HCPU_LCPU_CODE_START_ADDR);
  lcpu_ble_patch_install();
  HAL_RCC_ReleaseLCPU();
  HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
#ifdef USING_SEC_ENV
  HAL_SECU_SetAttr(SECU_MOD_HCPU, SECU_ROLE_MASTER, SECU_FLAG_NONE);
  HAL_SECU_Apply(SECU_GROUP_HPMST);
#endif
  HAL_Delay_us(5000);
  return 0;
}

uint8_t lcpu_power_off(void)
{
  HAL_RCC_Reset_and_Halt_LCPU(0);
  return 0;
}

#else
uint8_t lcpu_power_on(void)
{
  return 0;
}
#endif