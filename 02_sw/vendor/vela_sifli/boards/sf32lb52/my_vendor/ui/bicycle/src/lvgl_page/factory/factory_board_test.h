/**
 * @file factory_board_test.h
 * @brief 工厂模式板卡测试向导（分页外设校验）。
 */

#ifndef FACTORY_BOARD_TEST_H
#define FACTORY_BOARD_TEST_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 从工厂首页进入板卡测试。 */
void factory_board_test_begin(lv_obj_t * root);

/** @brief 退出板卡测试回到工厂首页。 */
void factory_board_test_end(void);

/** @brief 测试向导是否在运行。 */
bool factory_board_test_active(void);

/** @brief 主循环 / 定时器泵（传感器页采样、RGB 轮播等）。 */
void factory_board_test_poll(void);

/** @brief KEY1 短按：上一页；汇总页上翻结果。 */
void factory_board_test_prev(void);

/** @brief KEY2 短按：下一页；汇总页下翻结果。 */
void factory_board_test_next(void);

/** @brief KEY1 长按：退出测试。 */
void factory_board_test_exit(void);

/** @brief KEY2 长按：重测本页；汇总页则从头再测一轮。 */
void factory_board_test_retest(void);

/** @brief 当前是否在汇总页。 */
bool factory_board_test_on_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_BOARD_TEST_H */
