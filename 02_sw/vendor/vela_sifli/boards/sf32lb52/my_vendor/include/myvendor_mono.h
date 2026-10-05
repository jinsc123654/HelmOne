/**
 * @file myvendor_mono.h
 * @brief CLOCK_MONOTONIC 毫秒，以及回退安全的经过时间。
 *
 * clock_gettime 偶发比上次早数毫秒时，uint32 减法会下溢成 ~49 天，
 * 静默超时就会误关 UART / 拆 BLE / 停喂狗。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_MONO_H
#define MYVENDOR_MONO_H

#include <stdint.h>
#include <time.h>

/**
 * @brief CLOCK_MONOTONIC 毫秒。
 */
static inline uint32_t myvendor_mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                    (uint64_t)ts.tv_nsec / 1000000u);
}

/**
 * @brief now 相对 then 的毫秒数；时钟回退则视为 0。
 *
 * 正向走过 2^32 ms（约 49 天）仍按无符号回绕计算；超过约 24 天的
 * 单段间隔会当成回退，本板超时都在分钟级。
 */
static inline uint32_t myvendor_mono_elapsed_ms(uint32_t now, uint32_t then)
{
  uint32_t dt = now - then;

  if (dt > (UINT32_MAX / 2u))
    {
      return 0;
    }

  return dt;
}

#endif /* MYVENDOR_MONO_H */
