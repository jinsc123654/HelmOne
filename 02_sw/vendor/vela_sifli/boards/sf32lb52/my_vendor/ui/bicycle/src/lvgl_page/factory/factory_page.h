/**
 * @file factory_page.h
 * @brief 工厂固件首页：KEY1 列表 / KEY2 确认，编译期字模，不加载 TTF。
 */

#ifndef FACTORY_PAGE_H
#define FACTORY_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 向 lv_pm 注册工厂页（仅工厂镜像由 lvgl_page_init 调用）。 */
void factory_page_register(void);

/** @brief 刷新 MTP / BLE 状态（主循环调用）。 */
void factory_page_poll_active(void);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_PAGE_H */
