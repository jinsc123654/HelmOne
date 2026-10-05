# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/nsh/ble_test_app/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_MYVENDOR_BLE_TEST_APP)
  set(OPENVELA_ROOT ${NUTTX_BOARD_ABS_DIR}/../../../../..)
  set(BLUETOOTH_DIR ${OPENVELA_ROOT}/frameworks/connectivity/bluetooth)

  nuttx_add_application(
    NAME
    ble_test
    STACKSIZE
    ${CONFIG_MYVENDOR_BLE_TEST_APP_STACKSIZE}
    PRIORITY
    ${CONFIG_MYVENDOR_BLE_TEST_APP_PRIORITY}
    INCLUDE_DIRECTORIES
    ${BLUETOOTH_DIR}/framework/include
    SRCS
    ble_test_app.c
    DEPENDS
    libbluetooth)
endif()
