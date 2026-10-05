/**
 * @file test_led.c
 * @brief test led：经 /dev/gpioN 闪烁 LED。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include "test_demos.h"

#define LOGI(fmt, ...)   printf("test led: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)   printf("test led: ERROR " fmt "\n", ##__VA_ARGS__)

#if defined(CONFIG_DEV_GPIO)

#include <nuttx/ioexpander/gpio.h>

#define LED_DEFAULT_DEV     "/dev/gpio0"
#define LED_DEFAULT_COUNT   5
#define LED_DEFAULT_PERIOD  200

/**
 * @brief test led 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_led_main(int argc, FAR char *argv[])
{
  FAR const char *devpath = LED_DEFAULT_DEV;
  int count = LED_DEFAULT_COUNT;
  int period = LED_DEFAULT_PERIOD;
  int fd;
  int i;
  int ret;

  if (argc > 1)
    {
      devpath = argv[1];
    }

  if (argc > 2)
    {
      count = atoi(argv[2]);
    }

  if (argc > 3)
    {
      period = atoi(argv[3]);
    }

  if (count <= 0 || period <= 0)
    {
      LOGE("invalid count/period");
      return EXIT_FAILURE;
    }

  LOGI("blink %s x%d (period=%dms)", devpath, count, period);

  fd = open(devpath, O_RDWR);
  if (fd < 0)
    {
      LOGE("open(%s) failed: %d", devpath, errno);
      return EXIT_FAILURE;
    }

  /* Best-effort: force the pin to push-pull output. Ignore failure for
   * drivers that expose a fixed-direction output pin.
   */

  ret = ioctl(fd, GPIOC_SETPINTYPE, (unsigned long)GPIO_OUTPUT_PIN);
  if (ret < 0)
    {
      LOGI("GPIOC_SETPINTYPE not supported (%d), assuming output", errno);
    }

  for (i = 0; i < count; i++)
    {
      ret = ioctl(fd, GPIOC_WRITE, (unsigned long)true);
      if (ret < 0)
        {
          LOGE("GPIOC_WRITE(on) failed: %d", errno);
          close(fd);
          return EXIT_FAILURE;
        }

      LOGI("  [%d/%d] ON", i + 1, count);
      usleep(period * 1000);

      ret = ioctl(fd, GPIOC_WRITE, (unsigned long)false);
      if (ret < 0)
        {
          LOGE("GPIOC_WRITE(off) failed: %d", errno);
          close(fd);
          return EXIT_FAILURE;
        }

      LOGI("  [%d/%d] OFF", i + 1, count);
      usleep(period * 1000);
    }

  close(fd);
  LOGI("==== LED test: PASS ====");
  return EXIT_SUCCESS;
}

#else /* CONFIG_DEV_GPIO */

int test_led_main(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);
  printf("test led: requires CONFIG_DEV_GPIO\n");
  return EXIT_FAILURE;
}

#endif /* CONFIG_DEV_GPIO */
