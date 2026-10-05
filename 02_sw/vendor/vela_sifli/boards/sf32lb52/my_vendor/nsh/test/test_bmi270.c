/**
 * @file test_bmi270.c
 * @brief test bmi270：BMI270 加速度/陀螺 uORB 读取。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <nuttx/nuttx.h>

#include <nuttx/uorb.h>

#define BMI270_READ_DEFAULT_COUNT  10

/**
 * @brief test bmi270 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_bmi270_main(int argc, FAR char *argv[])
{
  int fd_accel;
  int fd_gyro;
  ssize_t n;
  int count;
  int i;
  struct sensor_accel accel;
  struct sensor_gyro gyro;

  count = BMI270_READ_DEFAULT_COUNT;
  if (argc > 1)
    {
      count = atoi(argv[1]);
      if (count < 1)
        {
          printf("test bmi270: invalid count '%s'\n", argv[1]);
          return EXIT_FAILURE;
        }
    }

  fd_accel = open("/dev/uorb/sensor_accel0", O_RDONLY | O_NONBLOCK);
  if (fd_accel < 0)
    {
      printf("test bmi270: open sensor_accel0 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  fd_gyro = open("/dev/uorb/sensor_gyro0", O_RDONLY | O_NONBLOCK);
  if (fd_gyro < 0)
    {
      printf("test bmi270: open sensor_gyro0 failed: %d\n", errno);
      close(fd_accel);
      return EXIT_FAILURE;
    }

  /* Allow first ODR period after activate (100 Hz). */

  usleep(20000);

  for (i = 0; i < count; i++)
    {
      n = read(fd_accel, &accel, sizeof(accel));
      if (n != (ssize_t)sizeof(accel))
        {
          printf("test bmi270: accel read %d/%d failed n=%zd errno=%d\n",
                 i + 1, count, n, errno);
          break;
        }

      n = read(fd_gyro, &gyro, sizeof(gyro));
      if (n != (ssize_t)sizeof(gyro))
        {
          printf("test bmi270: gyro read %d/%d failed n=%zd errno=%d\n",
                 i + 1, count, n, errno);
          break;
        }

      printf("[%d/%d] BMI270 accel: x=%.3f y=%.3f z=%.3f m/s^2\n",
             i + 1, count, accel.x, accel.y, accel.z);
      printf("         gyro:  x=%.3f y=%.3f z=%.3f rad/s\n",
             gyro.x, gyro.y, gyro.z);
    }

  close(fd_gyro);
  close(fd_accel);
  return EXIT_SUCCESS;
}
