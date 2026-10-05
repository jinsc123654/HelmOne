# ##############################################################################
# vendor/sifli/chip/sf32lb52/src/CMakeLists.txt
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

set(SRCS sifli_start.c sifli_irq.c sifli_uart.c sifli_lowput.c
         sifli_oneshot.c sifli_allocateheap.c sifli_gpio.c sifli_i2c.c
         sifli_spi.c sf32lb52_bd_addr.c)

if(CONFIG_ADC)
    list(APPEND SRCS sf32lb_adc.c)
endif()

if(CONFIG_RTC)
    list(APPEND SRCS sf32lb_rtc.c)
endif()

if(CONFIG_WATCHDOG)
    list(APPEND SRCS sf32lb_iwdg.c)
endif()

if(CONFIG_MTD)
    list(APPEND SRCS sf32lb_nand.c)
    # SDIO/SD1 block driver (MTD) for SD/eMMC boot rootfs (/mnt/lfs). The
    # automount entry is only called from bringup when MY_VENDOR_BOOT_FROM_SD/
    # EMMC is set (see boards/.../bsp/bringup.c); unused otherwise.
    list(APPEND SRCS sf32lb_sdio.c sf32lb_lfs_super.c)
endif()

if(CONFIG_TIMER)
    list(APPEND SRCS sf32lb_timer.c)
endif()

if(CONFIG_PWM)
    list(APPEND SRCS sf32lb_pwm.c)
endif()

if(CONFIG_SF32LB52_USBDEV)
    list(APPEND SRCS sf32lb_usbdev.c)
endif()

if(CONFIG_MYVENDOR_IDLE_STAT)
    list(APPEND SRCS sf32lb52_idle.c myvendor_idle_stat.c)
endif()

if(CONFIG_MYVENDOR_DVFS)
    list(APPEND SRCS sf32lb_dvfs.c)
endif()

if(CONFIG_MYVENDOR_CPULOAD_EXTCLK)
    list(APPEND SRCS myvendor_cpuload.c)
endif()

set(IPC_QUEUE_SRCS
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common/circular_buf.c
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common/ipc_hw.c
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common/ipc_queue.c
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/porting/sf32lb52x/hcpu/ipc_hw_port.c)

set(LCPU_BT_SRCS)

if(CONFIG_UART_BTH4)
    list(APPEND LCPU_BT_SRCS
         ${NUTTX_CHIP_ABS_DIR}/sf32lb52_bt_adapter.c
         ${NUTTX_CHIP_ABS_DIR}/sf32lb52_bth4.c
         ${NUTTX_CHIP_ABS_DIR}/sf32lb52_lcpu_boot.c
         ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x/lcpu_patch.c
         ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x/lcpu_patch_rev_b.c
         ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x/bt_rf_fulcal.c)
    target_compile_definitions(arch PRIVATE SF32LB52X_REV_AUTO)
endif()

# Add all HAL source files
file(GLOB HAL_SRCS ${NUTTX_CHIP_ABS_DIR}/../drivers/hal/*.c)

# For SF32LB52X, exclude certain files based on SConscript rules
list(REMOVE_ITEM HAL_SRCS 
    ${NUTTX_CHIP_ABS_DIR}/../drivers/hal/bf0_hal_audcodec.c
    ${NUTTX_CHIP_ABS_DIR}/../drivers/hal/bf0_hal_qspi.c
    ${NUTTX_CHIP_ABS_DIR}/../drivers/hal/bf0_hal_qspi_ex.c)

# Add system initialization file
set(SYSTEM_SRCS ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x/Templates/system_bf0_ap.c
                ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x/bf0_pin_const.c
                ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x/lcpu_config_type.c)

target_sources(arch PRIVATE ${SRCS} ${HAL_SRCS} ${SYSTEM_SRCS} ${IPC_QUEUE_SRCS} ${LCPU_BT_SRCS})


target_compile_definitions(arch PRIVATE SOC_BF0_HCPU 
                                        SF32LB52X
                                        HAL_TICK_PER_SECOND=1000
                                        USE_HAL_DRIVER
                                        USE_SERIALDRIVER
                                        HAL_USE_NAND
                                        # NAND-only board has no NOR-flash OTP region; BSP_System_Config()
                                        # would hang in HAL_FLASH_PreInit()/HAL_ASSERT reading OTP. This
                                        # makes it return right after EFUSE + HAL_PMU_LoadCalData(),
                                        # skipping the (optional) factory OTP calibration read.
                                        CFG_SUPPORT_NON_OTP)

if(CONFIG_BSP_USING_SPI_NAND)
  target_compile_definitions(arch PRIVATE BSP_USING_SPI_NAND)
  if(NOT CONFIG_BSP_DISABLE_BBM)
    target_compile_definitions(arch PRIVATE BSP_USING_BBM)
  endif()
endif()

# 全局 PSRAM 写回（WIP，默认关）：让 HAL/arch 看到 SDK 的 PSRAM_CACHE_WB。
# 代价是非 CPU master 碰 PSRAM 都要做缓存维护 —— 审计、缺口清单与验证计划见
# docs/psram_cache_wb_audit.md，别在 `test coh` 探针通过之前打开。
if(CONFIG_MYVENDOR_PSRAM_CACHE_WB)
  target_compile_definitions(arch PRIVATE PSRAM_CACHE_WB)
endif()

target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/include)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/../drivers/Include)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/Include)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/../external/CMSIS/Include)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/../boards/include/config/sf32lb52x)
target_include_directories(arch PRIVATE ${NUTTX_BOARD_ABS_DIR}/include)
target_include_directories(arch PRIVATE ${NUTTX_BOARD_ABS_DIR}/bsp)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue/porting/os/vela)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue/porting/sf32lb52x/hcpu)

# Suppress warnings for HAL driver files
target_compile_options(arch PRIVATE 
    $<$<COMPILE_LANGUAGE:C>:-w>
    $<$<COMPILE_LANGUAGE:CXX>:-w>)
