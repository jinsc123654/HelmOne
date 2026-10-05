/**
 * @file helm_shell.h
 * @brief 主界面翻页：待机 / 概览 / 心率 / 速度 / 地图 / 转向 / 爬升。
 */

#ifndef HELM_SHELL_H
#define HELM_SHELL_H

#include "live_map/map_page.h"

#ifdef __cplusplus
extern "C" {
#endif

void helm_shell_attach(map_page_t * map);
void helm_shell_bind_keys(void);
void helm_shell_refresh(void);
/** @brief 地图重绘后把底栏/导航条提到画布前面。 */
void helm_shell_raise_chrome(void);
/** @brief 被菜单等盖住：停仪表绘制/搜星动画，数据仍由 map_page 接收。 */
void helm_shell_pause_for_cover(void);
/** @brief 回到前台：先恢复数字，搜星页顶部折线延到转场后再画。 */
void helm_shell_resume_after_cover(void);
/** @brief 自动暂停节拍：菜单盖住时仍可恢复/暂停记录，暂停条回主界面再收。 */
void helm_shell_autopause_tick(void);
void helm_shell_set_paused(bool paused);
bool helm_shell_paused(void);
void helm_shell_toggle_ride(void);
/** @brief 续录：接上旧里程/计时，并画出旧轨迹。须先 prepare_continue。 */
void helm_shell_continue_ride(float km, uint32_t sec, const float * lon,
                              const float * lat, uint16_t n);
/** @brief GNSS 回放跳里程：未开录则开录；已暂停则继续。 */
void helm_shell_ensure_ride(void);
/** @brief 导航开始：切到地图页。不自动开始录像。 */
void helm_shell_on_nav_started(void);
/** @brief 通栏导航横幅此刻是否可见（地图据此决定是否下移半个横幅高）。 */
bool helm_shell_nav_ban_shown(void);

#ifdef __cplusplus
}
#endif

#endif /* HELM_SHELL_H */
