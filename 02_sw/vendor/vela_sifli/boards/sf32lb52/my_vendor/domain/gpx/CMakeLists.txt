# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/domain/gpx/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# gpx_main.c MUST be the first SRC (nuttx_add_application aliases main=gpx_main
# on the first source file only).
# ##############################################################################

if(CONFIG_MYVENDOR_GPX)
  nuttx_add_application(
    NAME
    gpx
    STACKSIZE
    ${CONFIG_MYVENDOR_GPX_STACKSIZE}
    PRIORITY
    ${CONFIG_MYVENDOR_GPX_PRIORITY}
    COMPILE_FLAGS
    -I${NUTTX_BOARD_ABS_DIR}/domain/gpx
    -I${NUTTX_BOARD_ABS_DIR}/domain/gpx/example
    -I${NUTTX_BOARD_ABS_DIR}/include
    SRCS
    gpx_main.c
    example/gpx_example.c
    example/gpx_example_record.c
    gpx_port.c
    gpx_ext.c
    gpx_ext_sensor.c
    gpx_writer.c
    gpx_reader.c
    gpx_decode.c
    gpx_record.c
    DEPENDS
    board)
endif()
