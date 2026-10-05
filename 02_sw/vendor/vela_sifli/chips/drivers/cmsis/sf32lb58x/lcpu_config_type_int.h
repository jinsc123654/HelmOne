/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __LCPU_CONFIG_TYPE_INT_H
#define __LCPU_CONFIG_TYPE_INT_H

#if defined(LCPU_CONFIG_V2)

    /*
    typedef struct
    {
    uint32_t lpcycle_curr;
    uint32_t lpcycle_ave;
    uint32_t wdt_time;
    uint32_t wdt_status;
    uint32_t bt_txpwr;
    uint16_t wdt_clk;
    uint8_t is_xtal_enable;
    uint8_t is_rccal_in_L;
    uint32_t is_soft_cvsd;//u32 for magic
    hal_lcpu_bluetooth_em_config_t em_buf;
    hal_lcpu_bluetooth_act_configt_t bt_act_config;
    hal_lcpu_ble_mem_config_t ke_mem_config;
    hal_lcpu_bluetooth_rom_config_t bt_rom_config;
    uint32_t sec_addr;
    uint32_t hcpu_ipc_addr;

    } LCPU_CONFIG_TYPE_T;
    */

    #define LCPU_CONFIG_START_ADDR 0x204E3E00
    #define LPCU_CONFIG_MAGIC_NUM 0x45457878

    #define LCPU_ASSERT_INFO_ADDR  0x204E3FF0
    #define LPCU_ASSERT_OVER_FLAG  0xa5a5a5a5


    #define LCPU_CONFIG_ROM_SIZE 0x1F0


    #define LCPU_CONFIG_LPCYCLE_CURR_ROM_OFFSET 4
    #define LCPU_CONFIG_LPCYCLE_CURR_ROM_LENGTH 4

    #define LCPU_CONFIG_LPCYCLE_AVE_ROM_OFFSET 8
    #define LCPU_CONFIG_LPCYCLE_AVE_ROM_LENGTH 4

    #define LCPU_CONFIG_WDT_TIME_ROM_OFFSET 12
    #define LCPU_CONFIG_WDT_TIME_ROM_LENGTH 4

    #define LCPU_CONFIG_WDT_STATUS_ROM_OFFSET 16
    #define LCPU_CONFIG_WDT_STATUS_ROM_LENGTH 4

    #define LCPU_CONFIG_BT_TXPWR_ROM_OFFSET 20
    #define LCPU_CONFIG_BT_TXPWR_ROM_LENGTH 4

    #define LCPU_CONFIG_WDT_CLK_ROM_OFFSET 24
    #define LCPU_CONFIG_WDT_CLK_ROM_LENGTH 2

    #define LCPU_CONFIG_IS_XTAL_ENABLE_ROM_OFFSET 26
    #define LCPU_CONFIG_IS_XTAL_ENABLE_ROM_LENGTH 1

    #define LCPU_CONFIG_IS_RCCAL_IN_L_ROM_OFFSET 27
    #define LCPU_CONFIG_IS_RCCAL_IN_L_ROM_LENGTH 1

    #define LCPU_CONFIG_IS_SOFT_CVSD_ROM_OFFSET 28
    #define LCPU_CONFIG_IS_SOFT_CVSD_ROM_LENGTH 4

    #define LCPU_CONFIG_EM_BUF_ROM_OFFSET 32
    #define LCPU_CONFIG_EM_BUF_ROM_LENGTH 82

    #define LCPU_CONFIG_BT_ACT_CONFIG_ROM_OFFSET 116
    #define LCPU_CONFIG_BT_ACT_CONFIG_ROM_LENGTH 12

    #define LCPU_CONFIG_KE_MEM_CONFIG_ROM_OFFSET 128
    #define LCPU_CONFIG_KE_MEM_CONFIG_ROM_LENGTH 44

    #define LCPU_CONFIG_BT_ROM_CONFIG_ROM_OFFSET 172
    #define LCPU_CONFIG_BT_ROM_CONFIG_ROM_LENGTH 24

    #define LCPU_CONFIG_SEC_ADDR_ROM_OFFSET 196
    #define LCPU_CONFIG_SEC_ADDR_ROM_LENGTH 4

    #define LCPU_CONFIG_HCPU_IPC_ADDR_OFFSET 200
    #define LCPU_CONFIG_HCPU_IPC_ADDR_LENGTH 4

    #define LCPU_CONFIG_ADC_CALIBRATION_ROM_OFFSET 204
    #define LCPU_CONFIG_ADC_CALIBRATION_ROM_LENGTH 8

    #define LCPU_CONFIG_SDADC_CALIBRATION_ROM_OFFSET 212
    #define LCPU_CONFIG_SDADC_CALIBRATION_ROM_LENGTH 8

    #define LCPU_CONFIG_SN_ROM_OFFSET 220
    #define LCPU_CONFIG_SN_ROM_LENGTH 130

    #define LCPU_CONFIG_CHIP_REV_ROM_OFFSET 350
    #define LCPU_CONFIG_CHIP_REV_ROM_LENGTH 1

    #define LCPU_CONFIG_BATTERY_A_ROM_OFFSET 352
    #define LCPU_CONFIG_BATTERY_A_ROM_LENGTH 4

    #define LCPU_CONFIG_BT_ACTMOVE_ROM_OFFSET 356
    #define LCPU_CONFIG_BT_ACTMOVE_ROM_LENGTH 3

#else //!LCPU_CONFIG_V2, Will delete when A2 is ready

    /**
    typedef struct
    {
    uint32_t adc_cal[2];
    uint32_t sdadc_cal[2];
    uint8_t sn[128];
    uint16_t sn_len;
    uint8_t chip_rev;
    uint8_t reserved;
    uint32_t battery_a;
    uint32_t battery_b;
    } LCPU_CONFIG_TYPE_T;
    **/


    #define LCPU_CONFIG_START_ADDR 0x2041FF00
    #define LPCU_CONFIG_MAGIC_NUM 0x45457878

    #define LCPU_ASSERT_INFO_ADDR  0x2041FFF0
    #define LPCU_ASSERT_OVER_FLAG  0xa5a5a5a5

    #define LCPU_CONFIG_ROM_SIZE 0xA0


    #define HAL_LCPU_CONFIG_ADC_CALIBRATION_ROM 0
    #define LCPU_CONFIG_ADC_CALIBRATION_ROM_OFFSET 4
    #define LCPU_CONFIG_ADC_CALIBRATION_ROM_LENGTH 8

    #define HAL_LCPU_CONFIG_SDADC_CALIBRATION_ROM 1
    #define LCPU_CONFIG_SDADC_CALIBRATION_ROM_OFFSET 12
    #define LCPU_CONFIG_SDADC_CALIBRATION_ROM_LENGTH 8

    #define HAL_LCPU_CONFIG_SN_ROM 2
    #define LCPU_CONFIG_SN_ROM_OFFSET 20
    #define LCPU_CONFIG_SN_ROM_LENGTH 128
    #define LCPU_CONFIG_SN_LEN_ROM_OFFSET 148
    #define LCPU_CONFIG_SN_LEN_ROM_LENGTH 2


    #define HAL_LCPU_CONFIG_CHIP_REV_ROM 3
    #define LCPU_CONFIG_CHIP_REV_ROM_OFFSET 150
    #define LCPU_CONFIG_CHIP_REV_ROM_LENGTH 1

    #define HAL_LCPU_CONFIG_BATTERY_CALIBRATION_ROM 4
    #define LCPU_CONFIG_BATTERY_A_ROM_OFFSET 152
    #define LCPU_CONFIG_BATTERY_A_ROM_LENGTH 4
    #define LCPU_CONFIG_BATTERY_B_ROM_OFFSET 156
    #define LCPU_CONFIG_BATTERY_B_ROM_LENGTH 4



#endif // LCPU_CONFIG_V2



#endif // __LCPU_CONFIG_TYPE_INT_H
