/**
 * @file test_iowrite.c
 * @brief test iowrite：PWR_MD（PA27）与 GNSS_PWR（PA43，MAX-M10S-00B-01 VCC）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef UNUSED
#  undef UNUSED
#endif
#include "bf0_hal.h"
#include "drv_io.h"

#define IOWRITE_TOGGLE_US  2000000u
#define IOWRITE_COLS       4
#define IOWRITE_COLW       16

#define IOWRITE_PIN_PWR    0
#define IOWRITE_PIN_GNSS   1

struct iowrite_pin_s
{
  FAR const char *name;
  int pad;
  pin_function gpio_func;
  int gpio_pin;
};

static const struct iowrite_pin_s g_iowrite_pins[] =
{
  { "PWR_MD",   PAD_PA27, GPIO_A27, 27 },
  { "GNSS_PWR", PAD_PA43, GPIO_A43, 43 },
};

#define IOWRITE_COUNT  (sizeof(g_iowrite_pins) / sizeof(g_iowrite_pins[0]))

/** @brief 打印用法。 */
static void iowrite_usage(void)
{
  printf("test iowrite: usage: test iowrite [pwr|gnss|all] [0|1|loop]\n"
         "  pwr  PA27 PWR_MD   HIGH=FPWM LOW=PFM (default pin)\n"
         "  gnss PA43 GNSS_PWR GPIO level"
#ifdef CONFIG_BOARD_L96_GNSS_PWR_ACTIVE_HIGH
         " (HIGH=module on)\n"
#else
         " (LOW=module on, active-low)\n"
#endif
         "  omitted pin name keeps old PWR_MD-only command\n");
}

/** @brief 配置引脚为输出并设电平。 */
static void iowrite_cfg_out(FAR const struct iowrite_pin_s *p, bool high)
{
  HAL_PIN_Set(p->pad, p->gpio_func, PIN_NOPULL, 1);
  BSP_GPIO_Set(p->gpio_pin, high ? 1 : 0, 1);
}

/** @brief 打印 mask 内引脚的输出电平。 */
static void iowrite_print(FAR const bool *level, unsigned mask)
{
  size_t i;
  size_t j;
  size_t n;
  size_t idx[IOWRITE_COUNT];
  size_t shown = 0;

  for (i = 0; i < IOWRITE_COUNT; i++)
    {
      if ((mask & (1u << i)) != 0)
        {
          idx[shown++] = i;
        }
    }

  for (i = 0; i < shown; i += IOWRITE_COLS)
    {
      n = shown - i;
      if (n > IOWRITE_COLS)
        {
          n = IOWRITE_COLS;
        }

      for (j = 0; j < n; j++)
        {
          printf("%-*s", IOWRITE_COLW, g_iowrite_pins[idx[i + j]].name);
        }

      printf("\n");

      for (j = 0; j < n; j++)
        {
          printf("%-*s", IOWRITE_COLW, level[idx[i + j]] ? "HIGH" : "LOW");
        }

      printf("\n");
    }

  printf("\n");
  fflush(stdout);
}

/** @brief 把 mask 内引脚写到指定电平。 */
static void iowrite_apply(FAR bool *level, unsigned mask)
{
  size_t i;

  for (i = 0; i < IOWRITE_COUNT; i++)
    {
      if ((mask & (1u << i)) != 0)
        {
          iowrite_cfg_out(&g_iowrite_pins[i], level[i]);
        }
    }

  iowrite_print(level, mask);
}

/** @brief 解析 pwr / gnss / all；失败返回 -1。 */
static int iowrite_parse_pin(FAR const char *s, unsigned *mask)
{
  if (s == NULL || mask == NULL)
    {
      return -1;
    }

  if (strcmp(s, "pwr") == 0 || strcmp(s, "pa27") == 0 ||
      strcmp(s, "pwr_md") == 0)
    {
      *mask = (1u << IOWRITE_PIN_PWR);
      return 0;
    }

  if (strcmp(s, "gnss") == 0 || strcmp(s, "pa43") == 0 ||
      strcmp(s, "gnss_pwr") == 0)
    {
      *mask = (1u << IOWRITE_PIN_GNSS);
      return 0;
    }

  if (strcmp(s, "all") == 0)
    {
      *mask = (1u << IOWRITE_PIN_PWR) | (1u << IOWRITE_PIN_GNSS);
      return 0;
    }

  return -1;
}

/** @brief 解析 0/1/loop；失败返回 -1。 */
static int iowrite_parse_level(FAR const char *s, bool *high, bool *loop)
{
  if (s == NULL || high == NULL || loop == NULL)
    {
      return -1;
    }

  if (strcmp(s, "loop") == 0)
    {
      *loop = true;
      return 0;
    }

  if (strcmp(s, "0") == 0 || strcmp(s, "low") == 0)
    {
      *high = false;
      *loop = false;
      return 0;
    }

  if (strcmp(s, "1") == 0 || strcmp(s, "high") == 0)
    {
      *high = true;
      *loop = false;
      return 0;
    }

  return -1;
}

/**
 * @brief test iowrite 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_iowrite_main(int argc, FAR char *argv[])
{
  bool level[IOWRITE_COUNT];
  size_t i;
  unsigned mask = (1u << IOWRITE_PIN_PWR);
  bool high = true;
  bool loop = false;
  FAR const char *level_arg = NULL;

  for (i = 0; i < IOWRITE_COUNT; i++)
    {
      level[i] = true;
    }

  if (argc > 1)
    {
      if (iowrite_parse_pin(argv[1], &mask) == 0)
        {
          level_arg = (argc > 2) ? argv[2] : NULL;
        }
      else
        {
          level_arg = argv[1];
        }

      if (level_arg != NULL &&
          iowrite_parse_level(level_arg, &high, &loop) != 0)
        {
          iowrite_usage();
          return EXIT_FAILURE;
        }
    }

  for (i = 0; i < IOWRITE_COUNT; i++)
    {
      if ((mask & (1u << i)) != 0)
        {
          level[i] = high;
        }
    }

  printf("test iowrite: PWR_MD PA27 (HIGH=FPWM, LOW=PFM); "
         "GNSS_PWR PA43"
#ifdef CONFIG_BOARD_L96_GNSS_PWR_ACTIVE_HIGH
         " (HIGH=module on).\n");
#else
         " (LOW=module on, active-low).\n");
#endif
  fflush(stdout);

  iowrite_apply(level, mask);

  if (!loop)
    {
      return EXIT_SUCCESS;
    }

  for (; ; )
    {
      usleep(IOWRITE_TOGGLE_US);
      for (i = 0; i < IOWRITE_COUNT; i++)
        {
          if ((mask & (1u << i)) != 0)
            {
              level[i] = !level[i];
            }
        }

      iowrite_apply(level, mask);
    }

  return 0;
}
