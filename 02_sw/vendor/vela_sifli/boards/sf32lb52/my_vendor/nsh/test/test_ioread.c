/**
 * @file test_ioread.c
 * @brief test ioread：按键 GPIO 输入电平轮询探针。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#ifdef UNUSED
#  undef UNUSED
#endif
#include "bf0_hal.h"
#include "drv_io.h"

#define IOREAD_POLL_US      50000u
#define IOREAD_HEARTBEAT_N  20     /* 20 * 50 ms = 1 s */
#define IOREAD_COLS         4
#define IOREAD_COLW         16

struct ioread_pin_s
{
  FAR const char *name;
  int pad;
  pin_function gpio_func;
  int gpio_pin;
  uint32_t pull;
  bool active_high;
};

static const struct ioread_pin_s g_ioread_pins[] =
{
  { "KEY1",         PAD_PA30, GPIO_A30, 30, PIN_PULLUP,   false },
  { "KEY2",         PAD_PA33, GPIO_A33, 33, PIN_PULLUP,   false },
  { "PWR_KEY_READ", PAD_PA34, GPIO_A34, 34, PIN_PULLDOWN, true  },
};

#define IOREAD_COUNT  (sizeof(g_ioread_pins) / sizeof(g_ioread_pins[0]))

/** @brief 将引脚配置为输入并设置上下拉。 */
static void ioread_cfg_in(FAR const struct ioread_pin_s *p)
{
  GPIO_InitTypeDef gpio;

  HAL_PIN_Set(p->pad, p->gpio_func, p->pull, 1);

  gpio.Pin  = (uint32_t)p->gpio_pin;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(hwp_gpio1, &gpio);
}

/** @brief 读取引脚逻辑电平。 */
static bool ioread_level(FAR const struct ioread_pin_s *p)
{
  return HAL_GPIO_ReadPin(hwp_gpio1, (uint16_t)p->gpio_pin) != GPIO_PIN_RESET;
}

static void ioread_print(FAR const bool *level)
{
  size_t i;
  size_t j;
  size_t n;
  bool pressed;

  for (i = 0; i < IOREAD_COUNT; i += IOREAD_COLS)
    {
      n = IOREAD_COUNT - i;
      if (n > IOREAD_COLS)
        {
          n = IOREAD_COLS;
        }

      for (j = 0; j < n; j++)
        {
          printf("%-*s", IOREAD_COLW, g_ioread_pins[i + j].name);
        }

      printf("\n");

      for (j = 0; j < n; j++)
        {
          pressed = g_ioread_pins[i + j].active_high ? level[i + j]
                                                     : !level[i + j];
          printf("%-*s", IOREAD_COLW, pressed ? "pressed" : "idle");
        }

      printf("\n");
    }

  printf("\n");
  fflush(stdout);
}

/**
 * @brief test ioread 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_ioread_main(int argc, FAR char *argv[])
{
  bool level[IOREAD_COUNT];
  bool last[IOREAD_COUNT];
  size_t i;
  int beat;
  bool changed;

  UNUSED(argc);
  UNUSED(argv);

  for (i = 0; i < IOREAD_COUNT; i++)
    {
      ioread_cfg_in(&g_ioread_pins[i]);
    }

  printf("test ioread: KEY1/KEY2 pull-up (press=LOW), "
         "PWR_KEY_READ pull-down (press=HIGH)\n");
  fflush(stdout);

  for (i = 0; i < IOREAD_COUNT; i++)
    {
      last[i] = ioread_level(&g_ioread_pins[i]);
    }

  ioread_print(last);
  beat = 0;

  for (; ; )
    {
      changed = false;
      for (i = 0; i < IOREAD_COUNT; i++)
        {
          level[i] = ioread_level(&g_ioread_pins[i]);
          if (level[i] != last[i])
            {
              changed = true;
            }
        }

      beat++;
      if (changed || beat >= IOREAD_HEARTBEAT_N)
        {
          ioread_print(level);
          for (i = 0; i < IOREAD_COUNT; i++)
            {
              last[i] = level[i];
            }

          beat = 0;
        }

      usleep(IOREAD_POLL_US);
    }

  return 0;
}
