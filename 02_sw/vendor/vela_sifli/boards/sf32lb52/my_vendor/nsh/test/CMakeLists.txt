# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/nsh/test/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Board self-test demos, exposed as a single NSH builtin "test" that
# dispatches by sub-command (e.g. "test lfs", "test kv", "test led"). Add a
# new demo by dropping test_<name>.c here, declaring it in test_demos.h,
# registering it in g_test_demos[] (test_main.c), and appending it to SRCS.
# ##############################################################################

if(CONFIG_MYVENDOR_SELFTEST)
  set(TEST_SRCS test_main.c test_lfs.c test_fat.c test_sd.c test_sdio.c test_led.c test_iopin.c test_ioread.c test_iowrite.c test_sound.c test_eta.c test_screen.c test_cpu.c test_task.c test_idle.c test_time.c test_tick.c test_wdt.c test_rtc.c test_fault.c test_diag.c)
  if(CONFIG_KVDB)
    list(APPEND TEST_SRCS test_kv.c)
  endif()
  if(CONFIG_BOARD_BMP388)
    list(APPEND TEST_SRCS test_bmp388.c)
  endif()
  if(CONFIG_BOARD_MMC5983MA)
    list(APPEND TEST_SRCS test_mmc5983ma.c)
  endif()
  if(CONFIG_BOARD_BMI270)
    list(APPEND TEST_SRCS test_bmi270.c)
  endif()
  if(CONFIG_BOARD_L96_GNSS)
    list(APPEND TEST_SRCS test_gnss.c)
  endif()
  if(CONFIG_BSP_USING_UART2)
    list(APPEND TEST_SRCS test_uart2loop.c)
  endif()
  if(CONFIG_MYVENDOR_BLE_COMPANION)
    list(APPEND TEST_SRCS test_companion.c)
  endif()
  nuttx_add_application(
    NAME
    test
    STACKSIZE
    6144
    PRIORITY
    100
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}
    ${NUTTX_CHIP_ABS_DIR}/include
    ${NUTTX_CHIP_ABS_DIR}/../drivers/Include
    ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/Include
    ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x
    ${NUTTX_CHIP_ABS_DIR}/../external/CMSIS/Include
    ${NUTTX_BOARD_ABS_DIR}/include
    ${NUTTX_BOARD_ABS_DIR}/bsp
    ${NUTTX_APPS_DIR}/frameworks/system/utils/include
    COMPILE_FLAGS
    -DSOC_BF0_HCPU
    -DSF32LB52X
    -DUSE_HAL_DRIVER
    -Wno-undef
    -Wno-strict-prototypes
    SRCS
    ${TEST_SRCS})
endif()
