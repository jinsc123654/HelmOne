/**
 * @file test_sound.c
 * @brief test sound：投递 PA40 PWM 蜂鸣片提示（走声音线程函数表）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myvendor_sound.h"
#include "test_demos.h"

/**
 * @brief test sound 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 退出码。
 */
int test_sound_main(int argc, FAR char *argv[])
{
  myvendor_sound_id_t i;
  int ret;

  ret = myvendor_sound_start();
  if (ret < 0)
    {
      printf("test sound: start failed %d\n", ret);
      return EXIT_FAILURE;
    }

  if (argc < 2)
    {
      printf("Usage: test sound <name>|all|4k|8k|hz <n>|duty <pct>|dc|sweep\n");
      printf("Diag:\n");
      printf("  4k / 8k   4000 / 8000 Hz 800 ms\n");
      printf("  hz 7500   指定频率 500-12000，找最响点\n");
      printf("  duty 50   5000 Hz 占空比 10-90\n");
      printf("  sweep     4000~10000 Hz 步进 500\n");
      printf("  dc        PA40 拉高（陶瓷片应几乎无声）\n");
      printf("Names:\n");
      for (i = 0; i < MYVENDOR_SOUND_COUNT; i++)
        {
          printf("  %s\n", myvendor_sound_id_name(i));
        }

      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "4k") == 0)
    {
      printf("test sound: 4000 Hz PWM 800 ms\n");
      ret = myvendor_sound_diag_pwm(4000, 800);
      return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "8k") == 0)
    {
      printf("test sound: 8000 Hz PWM 800 ms\n");
      ret = myvendor_sound_diag_pwm(8000, 800);
      return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "dc") == 0)
    {
      printf("test sound: PA40 DC high 400 ms\n");
      ret = myvendor_sound_diag_dc(400);
      return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "hz") == 0)
    {
      unsigned long hz;

      if (argc < 3)
        {
          printf("test sound: hz <500-12000>\n");
          return EXIT_FAILURE;
        }

      hz = strtoul(argv[2], NULL, 10);
      printf("test sound: %lu Hz PWM 800 ms\n", hz);
      ret = myvendor_sound_diag_pwm((uint16_t)hz, 800);
      return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "duty") == 0)
    {
      unsigned long duty;

      if (argc < 3)
        {
          printf("test sound: duty <10-90>\n");
          return EXIT_FAILURE;
        }

      duty = strtoul(argv[2], NULL, 10);
      printf("test sound: 5000 Hz duty %lu%% 800 ms\n", duty);
      ret = myvendor_sound_diag_pwm_duty(5000, 800, (uint8_t)duty);
      return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "sweep") == 0)
    {
      printf("test sound: sweep 4000..10000 Hz step 500\n");
      ret = myvendor_sound_diag_sweep();
      return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "all") == 0)
    {
      for (i = 0; i < MYVENDOR_SOUND_COUNT; i++)
        {
          printf("test sound: %s\n", myvendor_sound_id_name(i));
          myvendor_sound_play(i);
          usleep(400000);
        }

      return EXIT_SUCCESS;
    }

  ret = myvendor_sound_play_name(argv[1]);
  if (ret < 0)
    {
      printf("test sound: unknown '%s'\n", argv[1]);
      return EXIT_FAILURE;
    }

  printf("test sound: queued %s\n", argv[1]);
  usleep(500000);
  return EXIT_SUCCESS;
}
