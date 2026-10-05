/**
 * @file bicycle_page_ids.h
 * @brief 自行车 lv_pm 页面 ID 枚举。
 */

#ifndef BICYCLE_PAGE_IDS_H
#define BICYCLE_PAGE_IDS_H

#include <stdint.h>

typedef enum {
    BICYCLE_PM_ID_MAP = 0,
    BICYCLE_PM_ID_USB_TRANSFER,
    BICYCLE_PM_ID_STARTUP,
    BICYCLE_PM_ID_MENU,
    BICYCLE_PM_ID_FACTORY,
    BICYCLE_PM_ID_COUNT
} bicycle_pm_id_t;

#endif /* BICYCLE_PAGE_IDS_H */
