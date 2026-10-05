/**
 * @file bicycle_status_bar.h
 * @brief 自行车 UI — status_bar。
 */

#ifndef BICYCLE_STATUS_BAR_H
#define BICYCLE_STATUS_BAR_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 在 lv_pm_status_bar_cont() 上构建 GNSS/时钟/电量；lv_pm init 后调用。 */
void bicycle_status_bar_init(void);

/** @brief 立即刷新标签（如 GNSS 轮询后）。 */
void bicycle_status_bar_refresh(void);

/** @brief 板级电量计可用时的可选钩子（-1 = 未知）。 */
void bicycle_status_bar_set_battery_pct(int pct);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_STATUS_BAR_H */
