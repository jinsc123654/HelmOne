# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/nsh/test_app/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# 逐项说明见 vendor/my_vendor/docs/test_app_guide.md 第 2 节
# ##############################################################################

if(CONFIG_MYVENDOR_TEST_APP)                              # Kconfig/defconfig 总开关
  nuttx_add_application(
    NAME                                                  # NSH 命令名；入口符号 test_app_main（main 自动改名）
    test_app
    STACKSIZE                                             # 任务栈（字节）
    ${CONFIG_MYVENDOR_TEST_APP_STACKSIZE}
    PRIORITY                                              # 调度优先级
    ${CONFIG_MYVENDOR_TEST_APP_PRIORITY}
    SRCS                                                  # 源文件列表（首个含 main）
    test_app.c)
endif()
# 默认: bringup 异步 test_app；rcS 仅 echo（见 bsp/bringup.c）
# 代码启动见 docs/test_app_guide.md 方式 B
