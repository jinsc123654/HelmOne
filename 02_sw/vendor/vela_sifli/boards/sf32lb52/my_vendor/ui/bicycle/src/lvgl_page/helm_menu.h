/**
 * @file helm_menu.h
 * @brief KEY1 长按菜单：导航 / 传感器 / 记录 / 设置。
 */

#ifndef HELM_MENU_H
#define HELM_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void helm_menu_page_register(void);
int helm_menu_open(void);

/** @brief 菜单已打开时重绘（开机键改亮度后刷新设置行）。 */
void helm_menu_refresh(void);

/** @brief 保存骑行后写入距离/时间缓存，列表不再打开 GPX。 */
void helm_ride_stat_remember(const char * gpx_path, double km, uint32_t sec);

#ifdef __cplusplus
}
#endif

#endif /* HELM_MENU_H */
