# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/drivers/gnss/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_BOARD_L96_GNSS)
  target_sources(drivers PRIVATE sf32lb52_l96.c)
endif()

target_include_directories(drivers PRIVATE ${CMAKE_CURRENT_LIST_DIR})
