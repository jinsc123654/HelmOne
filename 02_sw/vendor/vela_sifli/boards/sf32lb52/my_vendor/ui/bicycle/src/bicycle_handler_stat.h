/**
 * @file bicycle_handler_stat.h
 * @brief 自行车 UI 处理器统计。
 */

#ifndef BICYCLE_HANDLER_STAT_H
#define BICYCLE_HANDLER_STAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BICYCLE_LOOP_SLEEP_MS
#define BICYCLE_LOOP_SLEEP_MS 10u
#endif

/**
 * @brief 自行车 handler stat init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_handler_stat_init(void);
/**
 * @brief 自行车 handler stat begin。
 */
void bicycle_handler_stat_begin(void);
/**
 * @brief 自行车 handler stat after handler。
 */
void bicycle_handler_stat_after_handler(uint32_t idle_ms, uint32_t sleep_ms);
/**
 * @brief 自行车 handler stat usleep begin。
 */
void bicycle_handler_stat_usleep_begin(void);
/**
 * @brief 自行车 handler stat usleep end。
 */
void bicycle_handler_stat_usleep_end(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_HANDLER_STAT_H */
