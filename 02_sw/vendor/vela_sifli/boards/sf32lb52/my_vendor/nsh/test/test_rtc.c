/**
 * @file test_rtc.c
 * @brief test rtc：读取或强制设置硬件 RTC。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <nuttx/timers/rtc.h>

#include "test_demos.h"

#define LOGI(fmt, ...)  printf("test rtc: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf("test rtc: ERROR " fmt "\n", ##__VA_ARGS__)

#define RTC_DEVPATH  "/dev/rtc0"

/** @brief 经 ioctl 读取并打印 RTC 时间。 */
static int rtc_show(int fd)
{
  struct rtc_time rt;
  bool trusted = false;

  memset(&rt, 0, sizeof(rt));
  if (ioctl(fd, RTC_RD_TIME, (unsigned long)(uintptr_t)&rt) < 0)
    {
      LOGE("RTC_RD_TIME failed errno=%d", errno);
      return EXIT_FAILURE;
    }

  if (ioctl(fd, RTC_HAVE_SET_TIME, (unsigned long)(uintptr_t)&trusted) < 0)
    {
      LOGE("RTC_HAVE_SET_TIME failed errno=%d", errno);
    }

  LOGI("%04d-%02d-%02d %02d:%02d:%02d trusted=%d",
       rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
       rt.tm_hour, rt.tm_min, rt.tm_sec,
       trusted ? 1 : 0);
  return EXIT_SUCCESS;
}

/** @brief 打印 CLOCK_REALTIME 对照。 */
static void rtc_show_realtime(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
      LOGE("clock_gettime REALTIME failed errno=%d", errno);
      return;
    }

  LOGI("CLOCK_REALTIME=%lu.%06lu (what 'date' shows)",
       (unsigned long)ts.tv_sec,
       (unsigned long)(ts.tv_nsec / 1000L));
}

/**
 * @brief test rtc 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_rtc_main(int argc, FAR char *argv[])
{
  int fd;
  int ret;

  fd = open(RTC_DEVPATH, O_RDWR);
  if (fd < 0)
    {
      fd = open(RTC_DEVPATH, O_RDONLY);
    }

  if (fd < 0)
    {
      LOGE("open %s failed errno=%d", RTC_DEVPATH, errno);
      return EXIT_FAILURE;
    }

  if (argc < 2)
    {
      ret = rtc_show(fd);
      if (ret == EXIT_SUCCESS)
        {
          rtc_show_realtime();
        }
      close(fd);
      return ret;
    }

  {
    struct rtc_time rt;
    int year = atoi(argv[1]);

    if (year < 1970 || year > 2099)
      {
        LOGE("year '%s' out of range [1970..2099]", argv[1]);
        printf("Usage: test rtc <year> [mon] [day] [hour] [min] [sec]\n");
        close(fd);
        return EXIT_FAILURE;
      }

    memset(&rt, 0, sizeof(rt));
    rt.tm_year = year - 1900;
    rt.tm_mon  = (argc >= 3 ? atoi(argv[2]) : 1) - 1;   /* 1..12 -> 0..11 */
    rt.tm_mday = (argc >= 4 ? atoi(argv[3]) : 1);
    rt.tm_hour = (argc >= 5 ? atoi(argv[4]) : 0);
    rt.tm_min  = (argc >= 6 ? atoi(argv[5]) : 0);
    rt.tm_sec  = (argc >= 7 ? atoi(argv[6]) : 0);

    if (ioctl(fd, RTC_SET_TIME, (unsigned long)(uintptr_t)&rt) < 0)
      {
        LOGE("RTC_SET_TIME failed errno=%d", errno);
        close(fd);
        return EXIT_FAILURE;
      }

    LOGI("set ok");
    ret = rtc_show(fd);
    rtc_show_realtime();
    close(fd);
    return ret;
  }
}
