/**
 * @file test_mmc5983ma.c
 * @brief test mmc5983ma：MMC5983MA 磁力计 uORB 读取。
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

#define MMC5983MA_READ_DEFAULT_COUNT  10

/**
 * @brief test mmc5983ma 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_mmc5983ma_main(int argc, FAR char *argv[])
{
  int fd;
  ssize_t n;
  int count;
  int i;
  struct sensor_mag mag;

  count = MMC5983MA_READ_DEFAULT_COUNT;
  if (argc > 1)
    {
      count = atoi(argv[1]);
      if (count < 1)
        {
          printf("test mmc5983ma: invalid count '%s'\n", argv[1]);
          return EXIT_FAILURE;
        }
    }

  fd = open("/dev/uorb/sensor_mag0", O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("test mmc5983ma: open sensor_mag0 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  for (i = 0; i < count; i++)
    {
      n = read(fd, &mag, sizeof(mag));
      if (n != (ssize_t)sizeof(mag))
        {
          printf("test mmc5983ma: read %d/%d failed n=%zd errno=%d\n",
                 i + 1, count, n, errno);
          close(fd);
          return EXIT_FAILURE;
        }

      printf("[%d/%d] MMC5983MA: x=%.2f uT  y=%.2f uT  z=%.2f uT  temp=%.1f C\n",
             i + 1, count, mag.x, mag.y, mag.z, mag.temperature);
    }

  close(fd);
  return EXIT_SUCCESS;
}
