/**
 * @file test_app.c
 * @brief NSH 命令 test_app：启动路径演示（rcs/entry/board/manual）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

#include <nuttx/video/fb.h>

#define TEST_APP_TICK_COUNT  2

/* Full-screen stress test duration (seconds). */
#define TEST_APP_SCREEN_SECS 5

#define LOGI(fmt, ...)  printf("test_app: " fmt "\n", ##__VA_ARGS__)

static const char *resolve_launch_mode(int argc, FAR char *argv[])
{
  if (argc >= 2 && argv[1][0] != '\0')
    {
      return argv[1];
    }

  /* task_spawn(CONFIG_INIT_ENTRYPOINT) with no extra args */
  return "entry";
}

/** @brief 校验启动模式是否合法。 */
static int validate_mode(FAR const char *mode)
{
  return (strcmp(mode, "rcs") == 0 || strcmp(mode, "entry") == 0 ||
          strcmp(mode, "board") == 0 || strcmp(mode, "manual") == 0) ? 0 : -1;
}

/** @brief 打印用法说明。 */
static void usage(void)
{
  printf("Usage: test_app <rcs|entry|board|manual>\n");
  printf("  rcs     started from /etc/init.d/rcS (test_app rcs &)\n");
  printf("  entry   started as CONFIG_INIT_ENTRYPOINT (test_app_main)\n");
  printf("  board   started from board bringup (task_spawn)\n");
  printf("  manual  started from NSH by hand\n");
}

/**
 * @brief NSH 命令 test_app：启动路径演示（rcs/entry/board/manual）。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, FAR char *argv[])
{
  FAR const char *mode = resolve_launch_mode(argc, argv);
  int tick = 0;

  if (validate_mode(mode) != 0)
    {
      usage();
      return EXIT_FAILURE;
    }

  LOGI("started pid=%d tid=%d launch=%s", (int)getpid(), (int)gettid(), mode);

  for (tick = 0; tick < TEST_APP_TICK_COUNT; tick++)
    {
      LOGI("tick=%d/%d pid=%d tid=%d launch=%s",
           tick, TEST_APP_TICK_COUNT, (int)getpid(), (int)gettid(), mode);
      sleep(2);
    }

  LOGI("done, exiting");
  return EXIT_SUCCESS;
}
