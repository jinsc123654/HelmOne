# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/services/CMakeLists.txt
#
# Layer: connectivity / storage / board sensors (BLE, MTP, identity, LCD,
# board_sensor thread, transfer).
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

set(MYVENDOR_BUILD_STAMP_H ${CMAKE_CURRENT_BINARY_DIR}/myvendor_build_stamp.h)
add_custom_target(myvendor_gen_build_stamp
  COMMAND ${CMAKE_COMMAND}
    -DOUT=${MYVENDOR_BUILD_STAMP_H}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_build_stamp.cmake
  BYPRODUCTS ${MYVENDOR_BUILD_STAMP_H}
  COMMENT "Generate firmware build date (local, to the second)")
add_dependencies(board myvendor_gen_build_stamp)
target_include_directories(board PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/myvendor_identity.c
  PROPERTIES OBJECT_DEPENDS ${MYVENDOR_BUILD_STAMP_H})

target_sources(board PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_mtp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_usbdev.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_identity.c
  ${CMAKE_CURRENT_SOURCE_DIR}/board_mtp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_gnss.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_gnss_uart.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_fw_slot.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_board_sensor.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_sound.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_diaglog.c
  ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_diag.c)

if(CONFIG_MYVENDOR_BLE_COMPANION OR CONFIG_MYVENDOR_BICYCLE)
  target_sources(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/companion_bridge.c)
endif()

# EPIC 2D 搬运封装：应用（bicycle）侧看不见芯片层头文件，所以硬件那一半放这儿。
if(CONFIG_LV_USE_SIFLI_EPIC AND CONFIG_MYVENDOR_BICYCLE)
  target_sources(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_epic_blit.c)
endif()

add_subdirectory(ble_companion)
add_subdirectory(mtp_simple)
