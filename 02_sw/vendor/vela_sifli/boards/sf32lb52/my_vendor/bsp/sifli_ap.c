/**
 * @file sifli_ap.c
 * @brief NuttX 板级钩子：晚初始化跑 bringup 表，可选异步拉起 test_app。
 *
 * board_late_initialize() 调用 sf32lb52_devkit_lcd_bringup()；
 * CONFIG_MYVENDOR_TEST_APP 时额外 task_spawn test_app。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <syslog.h>

#include "arm_internal.h"
#include "sf32lb52_devkit_lcd.h"

#include <nuttx/arch.h>
#include <nuttx/board.h>

#ifdef CONFIG_MYVENDOR_TEST_APP
#  include <spawn.h>
#endif

#ifdef CONFIG_MYVENDOR_TEST_APP
extern int test_app_main(int argc, FAR char *argv[]);

/** @brief 异步 task_spawn 启动 test_app。 */
static void board_start_test_app_async(void)
{
  posix_spawnattr_t attr;
  FAR char *argv[] = { "board", NULL };
  int pid;

  posix_spawnattr_init(&attr);
  attr.priority  = CONFIG_MYVENDOR_TEST_APP_PRIORITY;
  attr.stacksize = CONFIG_MYVENDOR_TEST_APP_STACKSIZE;

  pid = task_spawn("test_app", test_app_main, NULL, &attr, argv, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "ERROR: task_spawn test_app failed: %d\n", pid);
    }
  else
    {
      syslog(LOG_INFO, "INFO: test_app async started pid=%d\n", pid);
    }
}
#endif

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
/** @brief 板级极早初始化（本板为空实现）。 */
void board_early_initialize(void)
{
}
#endif

#ifdef CONFIG_BOARD_LATE_INITIALIZE
/** @brief 板级晚初始化：跑 bringup 表，可选拉起 test_app。 */
void board_late_initialize(void)
{
  syslog(LOG_INFO, "BRINGUP: board_late_initialize enter\n");
  sf32lb52_devkit_lcd_bringup();
  syslog(LOG_INFO, "BRINGUP: board_late_initialize bringup returned\n");

#ifdef CONFIG_MYVENDOR_TEST_APP
  syslog(LOG_INFO, "BRINGUP: test_app async start\n");
  board_start_test_app_async();
#endif

  syslog(LOG_INFO, "BRINGUP: board_late_initialize done\n");
}
#endif

/**
 * @brief NuttX 应用初始化钩子。
 * @param arg 未使用。
 * @return 0；未启用 LATE_INITIALIZE 时直接调用 bringup。
 */
int board_app_initialize(uintptr_t arg)
{
  (void)arg;

#ifdef CONFIG_BOARD_LATE_INITIALIZE
  return OK;
#else
  return sf32lb52_devkit_lcd_bringup();
#endif
}

#ifdef CONFIG_BOARDCTL_RESET
/**
 * @brief 板级复位。
 * @param status 未使用。
 * @return 不返回（调用 up_systemreset）。
 */
int board_reset(int status)
{
  (void)status;

  up_systemreset();
  return OK;
}
#endif

#ifdef CONFIG_BOARDCTL_FINALINIT
/** @brief 应用最终初始化钩子（本板为空实现）。 */
int board_app_finalinitialize(uintptr_t arg)
{
  (void)arg;
  return 0;
}
#endif
