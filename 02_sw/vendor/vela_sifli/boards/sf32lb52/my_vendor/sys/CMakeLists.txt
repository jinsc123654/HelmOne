# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/sys/CMakeLists.txt
#
# Live snapshot (read-only) + NSH `sys`.
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

target_sources(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_sys.c)
target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/ctl)

nuttx_add_application(
  NAME
  sys
  STACKSIZE
  8192
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  sys_main.c
  DEPENDS
  board)
