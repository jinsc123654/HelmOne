/**
 * @file usb_transfer_page.h
 * @brief usb_transfer_page 模块。
 */

#ifndef USB_TRANSFER_PAGE_H
#define USB_TRANSFER_PAGE_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct page_usb_transfer page_usb_transfer_t;

/** @brief 向 lv_pm 注册 UsbTransfer 页面（由 lvgl_page_init 调用）。 */
void usb_transfer_page_register(void);

/**
 * @brief usb_transfer_page_poll 接口。
 */
void usb_transfer_page_poll(page_usb_transfer_t * page);
/**
 * @brief usb_transfer_page_poll_active 接口。
 */
void usb_transfer_page_poll_active(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_TRANSFER_PAGE_H */
