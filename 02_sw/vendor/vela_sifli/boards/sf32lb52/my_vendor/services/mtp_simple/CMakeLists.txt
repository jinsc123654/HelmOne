# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/services/mtp_simple/CMakeLists.txt
#
# Minimal MTP for LittleFS — replaces external/mtp-responder when enabled.
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_MYVENDOR_MTP_SIMPLE)
  nuttx_add_application(
    NAME
    mtp_simple
    STACKSIZE
    ${CONFIG_MYVENDOR_MTP_SIMPLE_STACKSIZE}
    PRIORITY
    ${CONFIG_MYVENDOR_MTP_SIMPLE_PRIORITY}
    COMPILE_FLAGS
    -I${NUTTX_BOARD_ABS_DIR}/services/mtp_simple
    -I${NUTTX_BOARD_ABS_DIR}/include
    -I${NUTTX_BOARD_ABS_DIR}/ctl
    SRCS
    mtp_simple.c
    mtp_names.c
    mtp_storage.c
    mtp_psram.c
    DEPENDS
    board)
endif()
