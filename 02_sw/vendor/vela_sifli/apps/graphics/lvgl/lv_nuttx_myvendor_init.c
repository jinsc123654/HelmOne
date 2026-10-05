/**
 * @file lv_nuttx_myvendor_init.c
 *
 * NuttX LVGL init: myvendor_lcd_disp display + standard touch/mouse.
 */

#include "lvgl/src/drivers/nuttx/lv_nuttx_entry.h"

#if LV_USE_NUTTX && LV_USE_NUTTX_CUSTOM_INIT && LV_USE_MYVENDOR_LCD_DISP

#include "lv_myvendor_lcd.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/nuttx/lv_nuttx_mouse.h"
#include "lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h"

void lv_nuttx_init_custom(const lv_nuttx_dsc_t * dsc, lv_nuttx_result_t * result)
{
  lv_display_t * disp = NULL;

  if (dsc != NULL && dsc->fb_path != NULL)
    {
      disp = lv_myvendor_lcd_create(dsc->fb_path);
    }
  else
    {
      disp = lv_myvendor_lcd_create("/dev/lcd0");
    }

  if (result != NULL)
    {
      result->disp = disp;
    }

  if (dsc != NULL)
    {
#if LV_USE_NUTTX_TOUCHSCREEN
      if (dsc->input_path != NULL && result != NULL)
        {
          result->indev = lv_nuttx_touchscreen_create(dsc->input_path);
        }

      if (dsc->utouch_path != NULL && result != NULL)
        {
          result->utouch_indev = lv_nuttx_touchscreen_create(dsc->utouch_path);
        }
#endif

#if LV_USE_NUTTX_MOUSE
      if (dsc->mouse_path != NULL && result != NULL)
        {
          result->mouse_indev = lv_nuttx_mouse_create(dsc->mouse_path);
        }
#endif
    }
}

void lv_nuttx_deinit_custom(lv_nuttx_result_t * result)
{
  if (result != NULL && result->disp != NULL)
    {
      lv_display_delete(result->disp);
      result->disp = NULL;
    }

  if (result != NULL && result->indev != NULL)
    {
      lv_indev_delete(result->indev);
      result->indev = NULL;
    }

  if (result != NULL && result->utouch_indev != NULL)
    {
      lv_indev_delete(result->utouch_indev);
      result->utouch_indev = NULL;
    }

  if (result != NULL && result->mouse_indev != NULL)
    {
      lv_indev_delete(result->mouse_indev);
      result->mouse_indev = NULL;
    }
}

#endif
