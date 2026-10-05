# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/CMakeLists.txt
#
# Board layers (product tree). Chip HAL, boot_loader, and boards/.../drivers
# stay at vendor/chip paths.
#
#   vela_override/  Vela 抽换
#   bsp/            BSP + 启动编排
#   ctl/            系统控制（NSH `ctl`，只写 persist）
#   sys/            整机只读快照（NSH `sys`）
#   services/       连接 / 存储
#   domain/         领域库
#   ui/             产品 UI
#   nsh/            NSH / 探针
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

add_subdirectory(bsp)
add_subdirectory(ctl)
add_subdirectory(sys)
add_subdirectory(vela_override)
add_subdirectory(services)
add_subdirectory(domain)
add_subdirectory(ui)
add_subdirectory(nsh)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../apps/graphics/lvgl
               myvendor_lvgl)

file(GLOB EXTRA_LIBS
     ${NUTTX_BOARD_ABS_DIR}/libs/${CONFIG_ARCH_BOARD_CUSTOM_NAME}/*.a)

set_property(GLOBAL APPEND PROPERTY NUTTX_EXTRA_LIBRARIES ${EXTRA_LIBS})

set_property(GLOBAL PROPERTY LD_SCRIPT ${NUTTX_BOARD_ABS_DIR}/scripts/ld.script)
