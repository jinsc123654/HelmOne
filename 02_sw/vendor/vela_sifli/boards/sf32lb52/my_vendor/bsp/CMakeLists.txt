# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/bsp/CMakeLists.txt
#
# Layer: BSP + bring-up (g_bringup[] in bringup.c).
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

set(SRCS sifli_ap.c bringup.c sifli_gpio.c sf32lb52_buttons.c bsp_init.c
         bsp_pinmux.c bsp_power.c board_malloc.c ptab_table.c eta9184.c
         myvendor_schedmon.c)

if(CONFIG_MYVENDOR_WATCHDOG)
  list(APPEND SRCS myvendor_watchdog.c)
endif()

if(CONFIG_MYVENDOR_COREDUMP)
  list(APPEND SRCS myvendor_coredump.c myvendor_crash_lfs.c)
endif()

nuttx_add_romfs(
  NAME
  etc
  MOUNTPOINT
  etc
  RCSRCS
  etc/init.d/rc.sysinit
  RCRAWS
  etc/init.d/rcS
  etc/group
  etc/1.txt
  etc/img
  PATH
  ${CMAKE_CURRENT_BINARY_DIR}/etc)

target_link_libraries(board PRIVATE romfs_etc)

target_sources(board PRIVATE ${SRCS})

target_compile_definitions(board PRIVATE SOC_BF0_HCPU
                                         SF32LB52X
                                         HAL_TICK_PER_SECOND=1000
                                         USE_HAL_DRIVER)

target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR}/include)
target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR}/../drivers/Include)
target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/Include)
target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x)
target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR}/../external/CMSIS/Include)
target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR}/../boards/include/config/sf32lb52x)
target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/include)
target_include_directories(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/../drivers/input)
target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/../drivers/sensor)
target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/../drivers/sensor/sf32lb52_bmi270)
target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/../drivers/gnss)
target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/../../../../boot_loader/include)
target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR}/../../../../nuttx/mm)
if(CONFIG_MYVENDOR_COREDUMP)
  target_include_directories(board PRIVATE
    ${NUTTX_CHIP_ABS_DIR}/../../.cache/littlefs-nuttx)
endif()

target_compile_options(board PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-w>
    $<$<COMPILE_LANGUAGE:CXX>:-w>)
