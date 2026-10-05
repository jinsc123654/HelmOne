# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/ctl/CMakeLists.txt
#
# Policy writes + NSH `ctl`. Reads are sys/.
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

target_sources(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_devctl.c)
target_include_directories(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 栈用量取证（2026-09-21）：那 12 份 ctl 快照是"栈写到底"（sp 贴 PSRAM 底、
# pc=lr=0），但**肉眼看不出是谁吃的**（本地最大只有 char buf[256]）。
# 别猜：让编译器把每个函数的栈帧量出来 ——
#   -Wstack-usage=N 对超过 N 字节栈帧的函数逐条报警（构建日志里一眼看到）
#   -fstack-usage   同时生成 .su 文件，给出精确字节数与"动态/有界"标注
# 这两个只在**取证**时开：确认完就没有运行时代价（纯编译期）。
# 注意 myvendor_devctl.c 是 board 目标的源文件（见上一行 target_sources），
# 不在本目录的 `add_compile_options` 覆盖范围内 ⇒ 要单独 set_source_files_properties。
set(MYVENDOR_CTL_STACK_AUDIT 1)
if(MYVENDOR_CTL_STACK_AUDIT)
  add_compile_options(-fstack-usage -Wstack-usage=256)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/myvendor_devctl.c
                              PROPERTIES COMPILE_OPTIONS "-fstack-usage;-Wstack-usage=256")
endif()

nuttx_add_application(
  NAME
  ctl
  # 4096 太小：2026-09-19 02:43~03:00 那 12 份快照（占普查 60%）里 ctl 崩在
  # sp=0x60400078 —— 也就是 PSRAM 堆底（0x60400000）往上 120 字节处：
  # 栈已经写到底，返回地址被破坏后跳到了 0（pc=lr=0）。
  # 而 ctl 自己**没有任何大局部变量**（最大 char buf[256]）⇒ 吃栈的是调用深度，
  # 不是某个缓冲区 ⇒ 正是"这个任务就是需要更多栈"的情形。
  # PSRAM 有 4 MB，翻倍到 8 KB 的成本可忽略；若哪天又在同一处崩，
  # 说明根因是"没有终止条件的递归" —— 那时再往上加没用，要去找递归。
  STACKSIZE
  8192
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  SRCS
  ctl_main.c
  DEPENDS
  board)
