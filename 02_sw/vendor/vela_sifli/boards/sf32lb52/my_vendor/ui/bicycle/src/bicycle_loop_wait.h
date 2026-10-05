/**
 * @file bicycle_loop_wait.h
 * @brief 主循环等待/节拍辅助。
 */

#ifndef BICYCLE_LOOP_WAIT_H
#define BICYCLE_LOOP_WAIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 自行车 loop wait init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_loop_wait_init(void);
/**
 * @brief 自行车 loop wait ms。
 */
void bicycle_loop_wait_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_LOOP_WAIT_H */
