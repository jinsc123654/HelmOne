# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/nsh/transfer_ui/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_MYVENDOR_TRANSFER_UI)
  nuttx_add_application(
    NAME transfer_ui
    PRIORITY ${CONFIG_MYVENDOR_TRANSFER_UI_PRIORITY}
    STACKSIZE ${CONFIG_MYVENDOR_TRANSFER_UI_STACKSIZE}
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/transfer_ui_main.c
    DEPENDS lvgl board)
endif()
