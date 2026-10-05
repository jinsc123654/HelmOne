/**
 * @file transfer_ui_main.c
 * @brief MTP 文件传输全屏 LVGL UI（独立应用入口）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "myvendor_mtp_lfs.h"
#include "myvendor_system_font.h"

#ifndef TRANSFER_UI_FBDEV_PATH
#define TRANSFER_UI_FBDEV_PATH "/dev/fb0"
#endif

#ifndef TRANSFER_UI_FBDEV_WAIT_MS
#define TRANSFER_UI_FBDEV_WAIT_MS 30000
#endif

#define TRANSFER_UI_ICON_PATH "/etc/img/usb.png"
#define TRANSFER_UI_TITLE     "文件传输模式"

/** @brief 轮询等待 framebuffer 设备就绪。 */
static bool wait_for_fbdev(const char *path, int timeout_ms)
{
  const int step_ms = 100;
  int elapsed = 0;

  while (elapsed < timeout_ms)
    {
      int fd = open(path, O_RDWR);

      if (fd >= 0)
        {
          close(fd);
          LV_LOG_INFO("fbdev ready: %s (%d ms)", path, elapsed);
          return true;
        }

      usleep(step_ms * 1000);
      elapsed += step_ms;
    }

  LV_LOG_ERROR("fbdev not ready: %s (waited %d ms, errno=%d)",
               path, timeout_ms, errno);
  return false;
}

/** @brief 构建全屏 USB 传输 UI 界面。 */
static void transfer_ui_build(lv_obj_t *scr)
{
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *icon = lv_img_create(scr);
  lv_img_set_src(icon, TRANSFER_UI_ICON_PATH);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -24);

  lv_obj_t *text = lv_label_create(scr);
  lv_label_set_text(text, TRANSFER_UI_TITLE);
  lv_obj_set_style_text_color(text, lv_color_white(), 0);

  lv_font_t *font = myvendor_system_font_get(16);
  if (font != NULL)
    {
      lv_obj_set_style_text_font(text, font, 0);
    }
  else
    {
      lv_obj_set_style_text_font(text, lv_theme_get_font_normal(text), 0);
    }

  lv_obj_align_to(text, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
}

/**
 * @brief MTP 传输 UI 主循环入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int transfer_ui_main(int argc, char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

  (void)argc;
  (void)argv;

  if (lv_is_initialized())
    {
      LV_LOG_ERROR("LVGL already initialized");
      return 1;
    }

  if (!myvendor_mtp_lfs_is_mtp())
    {
      LV_LOG_WARN("transfer_ui: MTP not active, exit");
      return 0;
    }

  lv_init();
  myvendor_system_font_lv_bind();

  if (!wait_for_fbdev(TRANSFER_UI_FBDEV_PATH, TRANSFER_UI_FBDEV_WAIT_MS))
    {
      return 1;
    }

  lv_nuttx_dsc_init(&info);
  info.fb_path = TRANSFER_UI_FBDEV_PATH;

  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      LV_LOG_ERROR("lv_nuttx_init failed");
      return 1;
    }

  myvendor_mtp_lfs_ui_register(getpid());
  transfer_ui_build(lv_screen_active());

  LV_LOG_INFO("transfer_ui running (pid=%ld)", (long)getpid());

  while (myvendor_mtp_lfs_is_mtp())
    {
      uint32_t idle = lv_timer_handler();
      usleep(idle ? idle * 1000 : 5000);
    }

  LV_LOG_INFO("transfer_ui MTP ended, exit");
  lv_nuttx_deinit(&result);
  lv_deinit();
  return 0;
}
