/**
 * @file img_src_boot_logo.h
 * @brief 80x80 boot/app splash (RGB565A8). Same art as 2SFBL.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMG_SRC_BOOT_LOGO_H
#define IMG_SRC_BOOT_LOGO_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_LOGO_W  80
#define BOOT_LOGO_H  80
#define BOOT_LOGO_X  80
#define BOOT_LOGO_Y  64

/* Keep in sync with boot_sgl.c (progress-bar slot, even coords). */
#define BOOT_PRODUCT_NAME  "Helm One"
#define BOOT_NAME_X        10
#define BOOT_NAME_Y        212
#define BOOT_NAME_W        220
#define BOOT_NAME_H        24
#define BOOT_TICK_X        10
#define BOOT_TICK_Y        244
#define BOOT_TICK_W        220
#define BOOT_TICK_H        24
#define BOOT_BAR_W         128
#define BOOT_BAR_H         4
#define BOOT_BAR_X         56
#define BOOT_BAR_Y         280

extern const lv_image_dsc_t img_src_boot_logo;

#ifdef __cplusplus
}
#endif

#endif /* IMG_SRC_BOOT_LOGO_H */
