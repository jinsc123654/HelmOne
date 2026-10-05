# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/nsh/CMakeLists.txt
#
# Layer: NSH applets and probes.
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################
#
# ★ 栈大小口径（2026-09-21 统一定到 8192，别再单独把某个改小）
#
# 起因：2026-09-19 02:43~03:00 的 12 份 `ctl` 崩溃快照（占崩溃普查 60%）是
# **栈写到底**：sp=0x60400078（PSRAM 堆底 0x60400000 往上只剩 120 B）、
# pc=lr=0（返回地址被破坏后跳到 0）。用 `-fstack-usage` 全量量过之后，
# ctl 自己的最大栈帧只有 288 B、整条调用链里没有任何 ≥512 B 的局部缓冲区
# ⇒ **吃栈的是调用深度**，不是某个大变量：
#   - NSH applet 的栈是**逐次从 PSRAM 堆分配、退出即释放**（崩溃时 sp 落在
#     PSRAM 堆底正是这个证据）⇒ 调大只是"这次跑的时候多占几 KB"，不常驻；
#   - 这些 applet 都会走 printf/日志、板级服务、部分还进 LVGL，深度和 ctl 同类；
#   - 对照：`sys`/`ble_companion` 一直是 8192，上游样例 app 都是 16096。
# 所以这里统一到 8192（= 兄弟命令的量级），**不是为了消掉某个已测出的水位**，
# 而是先把这一类"静默踩栈底"的风险拉到与兄弟一致。
# 真正的证据在 ctl：它退出时会打一行 `ctl: stack used X/Y B (Z%)`
# （见 ctl/ctl_main.c 的 ctl_stack_report()）—— 想再调大小，先看那一行。
# ##############################################################################

add_subdirectory(test)
add_subdirectory(test_app)
add_subdirectory(ble_test_app)
add_subdirectory(lvgl_bench)
add_subdirectory(transfer_ui)

if(CONFIG_CRYPTO)
  nuttx_add_application(
    NAME
    md5_test
    STACKSIZE
    8192
    PRIORITY
    100
    SRCS
    md5_test.c)
endif()

nuttx_add_application(
  NAME
  ptab
  STACKSIZE
  8192
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  ptab_main.c)

nuttx_add_application(
  NAME
  boardmem
  STACKSIZE
  8192
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  boardmem_main.c
  DEPENDS
  board)

nuttx_add_application(
  NAME
  btnpeek
  STACKSIZE
  8192
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  btnpeek_main.c
  DEPENDS
  board)

nuttx_add_application(
  NAME
  mtpstat
  STACKSIZE
  8192
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  mtpstat_main.c
  DEPENDS
  board)

if(CONFIG_MYVENDOR_LCD_DISP)
  nuttx_add_application(
    NAME
    lcd_recover
    STACKSIZE
    8192
    PRIORITY
    100
    INCLUDE_DIRECTORIES
    ${NUTTX_BOARD_ABS_DIR}/include
    SRCS
    lcd_recover_main.c
    DEPENDS
    lvgl
    board)
endif()
