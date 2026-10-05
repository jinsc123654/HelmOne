# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/services/ble_companion/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_MYVENDOR_BLE_COMPANION)
  if(NOT CONFIG_BLUETOOTH_BLE_SCAN)
    message(
      WARNING
        "MYVENDOR_BLE_COMPANION needs CONFIG_BLUETOOTH_BLE_SCAN=y. "
        "cmake_out/.config is stale versus configs/nsh/defconfig. "
        "Run: cmake --build cmake_out/my_vendor_nsh --target resetconfig")
  endif()

  set(OPENVELA_ROOT ${NUTTX_BOARD_ABS_DIR}/../../../../..)
  set(BLUETOOTH_DIR ${OPENVELA_ROOT}/frameworks/connectivity/bluetooth)
  set(ZBLUE_DIR ${OPENVELA_ROOT}/apps/external/zblue/zblue)

  nuttx_add_application(
    NAME
    ble_companion
    STACKSIZE
    ${CONFIG_MYVENDOR_BLE_COMPANION_STACKSIZE}
    PRIORITY
    ${CONFIG_MYVENDOR_BLE_COMPANION_PRIORITY}
    INCLUDE_DIRECTORIES
    ${BLUETOOTH_DIR}/framework/include
    ${NUTTX_BOARD_ABS_DIR}/include
    ${NUTTX_BOARD_ABS_DIR}/ctl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ZBLUE_DIR}/port/include
    ${ZBLUE_DIR}/port/kernel/include
    ${ZBLUE_DIR}/include
    ${CMAKE_BINARY_DIR}/apps/external/zblue/include
    SRCS
    ble_companion.c
    myvendor_bt_pair.c
    companion_fs.c
    companion_rle.c
    companion_downlink.c
    ble_supervisor.c
    ble_sensor.c
    ${NUTTX_BOARD_ABS_DIR}/services/transfer_backend/transfer_backend.c
    DEPENDS
    libbluetooth
    board)

  if(TARGET apps_ble_companion)
    target_link_libraries(apps_ble_companion PRIVATE zblue_headers)
    # Download RLE encodes on the MCU; keep this file at -O2 even if the
    # rest of the tree is -Os.
    set_source_files_properties(
      ${CMAKE_CURRENT_SOURCE_DIR}/companion_rle.c PROPERTIES COMPILE_FLAGS -O2)
  endif()
endif()
