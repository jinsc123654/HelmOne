/**
 * @file test_bmp388.c
 * @brief test bmp388：BMP388 气压计 uORB 读取。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <nuttx/uorb.h>

#define BMP388_READ_DEFAULT_COUNT  10

/**
 * @brief test bmp388 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_bmp388_main(int argc, FAR char *argv[])
{
  int fd;
  ssize_t n;
  int count;
  int i;
  struct sensor_baro baro;

  count = BMP388_READ_DEFAULT_COUNT;
  if (argc > 1)
    {
      count = atoi(argv[1]);
      if (count < 1)
        {
          printf("test bmp388: invalid count '%s'\n", argv[1]);
          return EXIT_FAILURE;
        }
    }

  fd = open("/dev/uorb/sensor_baro0", O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("test bmp388: open sensor_baro0 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  for (i = 0; i < count; i++)
    {
      n = read(fd, &baro, sizeof(baro));
      if (n != (ssize_t)sizeof(baro))
        {
          printf("test bmp388: read %d/%d failed n=%zd errno=%d\n",
                 i + 1, count, n, errno);
          close(fd);
          return EXIT_FAILURE;
        }

      printf("[%d/%d] BMP388: pressure=%.2f hPa  temperature=%.2f C\n",
             i + 1, count, baro.pressure, baro.temperature);
    }

  close(fd);
  return EXIT_SUCCESS;
}
