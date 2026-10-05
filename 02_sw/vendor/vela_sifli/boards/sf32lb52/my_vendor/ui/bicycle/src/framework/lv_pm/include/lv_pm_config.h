/**
 * @file lv_pm_config.h
 * @brief lv_pm 编译期配置。
 */

#ifndef LV_PM_CONFIG_H
#define LV_PM_CONFIG_H

/* Bicycle: LiveMap uses pm pull-down status bar (collapsed strip always black). */
#define LV_PM_USE_STA_BAR   1
#define LV_PM_USE_BACK_BAR  0
#define LV_PM_USE_OVERLAY   1
#define LV_PM_USE_ANMI_STA  0
#define LV_PM_USE_ANMI_BACK 0

/* Default page pool capacity (lv_pm_init may use a smaller value). */
#ifndef LV_PM_MAX_PAGES
#define LV_PM_MAX_PAGES 8
#endif

#endif /* LV_PM_CONFIG_H */
