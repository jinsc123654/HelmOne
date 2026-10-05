# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/drivers/sensor/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

set(SRCS)

if(CONFIG_BOARD_BMP388)
  list(APPEND SRCS bmp388_uorb.c)
endif()

if(CONFIG_BOARD_MMC5983MA)
  list(APPEND SRCS mmc5983ma_uorb.c)
endif()

if(CONFIG_BOARD_BMI270)
  list(APPEND SRCS
    sf32lb52_bmi270/sf32lb52_bmi270_chip.c
    sf32lb52_bmi270/sf32lb52_bmi270_uorb.c
  )
endif()

target_sources(drivers PRIVATE ${SRCS})
target_include_directories(drivers PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(drivers PRIVATE ${CMAKE_CURRENT_LIST_DIR}/sf32lb52_bmi270)
