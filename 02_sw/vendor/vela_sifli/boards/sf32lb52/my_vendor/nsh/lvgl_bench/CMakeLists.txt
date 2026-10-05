# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/nsh/lvgl_bench/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# LVGL benchmark demo (lv_demo_benchmark). Toggle: CONFIG_MYVENDOR_LVGL_BENCH.
# ##############################################################################

if(CONFIG_MYVENDOR_LVGL_BENCH)
  nuttx_add_application(
    NAME lvgl_bench
    PRIORITY ${CONFIG_MYVENDOR_LVGL_BENCH_PRIORITY}
    STACKSIZE ${CONFIG_MYVENDOR_LVGL_BENCH_STACKSIZE}
    INCLUDE_DIRECTORIES ${NUTTX_BOARD_ABS_DIR}/include
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/lvgl_bench_main.c
    DEPENDS
    lvgl
    board)
endif()
