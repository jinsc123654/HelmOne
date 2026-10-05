/**
 * @file lv_myvendor_lcd.c
 *
 * LVGL display port: two SRAM strip draw buffers (A/B), flush_cb passes the
 * active SRAM pointer to the ui_flush worker for kernel DMA (no PSRAM copy).
 *
 * UI thread submits and waits; ui_flush transfers the previous strip.
 */

#include "lv_myvendor_lcd.h"

#if LV_USE_MYVENDOR_LCD_DISP

#include "myvendor_coredump.h"
#include "myvendor_lcd_disp.h"
#include "lvgl/lvgl.h"
#include "src/core/lv_refr.h"

#if LV_USE_SIFLI_EPIC
#include "src/draw/sifli/epic/lv_sifli_epic_cfg.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <syslog.h>

#ifndef CONFIG_LCD_HOR_RES_MAX
#  define CONFIG_LCD_HOR_RES_MAX 240
#endif

#ifndef CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS
#  define CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS 60
#endif

#define MYVENDOR_LCD_SRAM_BUF_BYTES \
  (CONFIG_LCD_HOR_RES_MAX * CONFIG_MYVENDOR_LCD_DISP_STRIP_ROWS * 2u)

/* LCDC/EPIC/coredump 共用的固定 HCPU SRAM 双条带。显式放在 .bss，避免
 * CONFIG_MM_REGIONS>1 时 malloc 把所谓 SRAM 缓冲分到 PSRAM kumm。 */
static uint8_t g_lcd_sram_buf_a[MYVENDOR_LCD_SRAM_BUF_BYTES]
    __attribute__((aligned(LV_DRAW_BUF_ALIGN)));
static uint8_t g_lcd_sram_buf_b[MYVENDOR_LCD_SRAM_BUF_BYTES]
    __attribute__((aligned(LV_DRAW_BUF_ALIGN)));

typedef struct
{
  lv_draw_buf_t draw1;
  lv_draw_buf_t draw2;
  void         *sram_px1;
  void         *sram_px2;
  uint32_t      sram_bytes;
} lv_myvendor_lcd_dsc_t;

static void flush_cb(lv_display_t * disp, const lv_area_t * area,
                     uint8_t * color_p);
static void flush_wait_cb(lv_display_t * disp);
static void flush_done_cb(void * user_data);
static void flush_refr_cb(void * user_data);
static void flush_unblock_cb(void * user_data);
static void display_release_cb(lv_event_t * e);

lv_display_t * lv_myvendor_lcd_create(const char * lcd_path)
{
  myvendor_lcd_disp_info_t info;
  lv_myvendor_lcd_dsc_t * dsc;
  lv_display_t * disp;
  uint32_t stride;
  int ret;

  ret = myvendor_lcd_disp_init(lcd_path != NULL ? lcd_path : "/dev/lcd0");
  if (ret < 0)
    {
      syslog(LOG_ERR, "lv_myvendor_lcd: myvendor_lcd_disp_init failed %d", ret);
      LV_LOG_ERROR("myvendor_lcd_disp_init failed: %d", ret);
      return NULL;
    }

  ret = myvendor_lcd_disp_get_info(&info);
  if (ret < 0 || info.hor_res == 0 || info.ver_res == 0 || info.strip_rows == 0)
    {
      syslog(LOG_ERR,
             "lv_myvendor_lcd: bad geometry %dx%d strip=%u (ret=%d)",
             info.hor_res, info.ver_res, info.strip_rows, ret);
      LV_LOG_ERROR("myvendor_lcd_disp_get_info failed or bad geometry: %dx%d strip=%u",
                   info.hor_res, info.ver_res, info.strip_rows);
      myvendor_lcd_disp_deinit();
      return NULL;
    }

  dsc = lv_malloc_zeroed(sizeof(*dsc));
  if (dsc == NULL)
    {
      syslog(LOG_ERR, "lv_myvendor_lcd: dsc alloc failed");
      myvendor_lcd_disp_deinit();
      return NULL;
    }

  disp = lv_display_create(info.hor_res, info.ver_res);
  if (disp == NULL)
    {
      syslog(LOG_ERR, "lv_myvendor_lcd: lv_display_create failed");
      lv_free(dsc);
      myvendor_lcd_disp_deinit();
      return NULL;
    }

  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

  stride = lv_draw_buf_width_to_stride(info.hor_res, LV_COLOR_FORMAT_RGB565);
  dsc->sram_bytes = stride * info.strip_rows;

  if (dsc->sram_bytes > sizeof(g_lcd_sram_buf_a))
    {
      syslog(LOG_ERR,
             "lv_myvendor_lcd: strip %u exceeds static SRAM buffer %u",
             (unsigned)dsc->sram_bytes,
             (unsigned)sizeof(g_lcd_sram_buf_a));
      LV_LOG_ERROR("static SRAM strip buffer too small");
      lv_free(dsc);
      lv_display_delete(disp);
      myvendor_lcd_disp_deinit();
      return NULL;
    }

  dsc->sram_px1 = g_lcd_sram_buf_a;
  dsc->sram_px2 = g_lcd_sram_buf_b;

  if (lv_draw_buf_init(&dsc->draw1, info.hor_res, info.strip_rows,
          LV_COLOR_FORMAT_RGB565, stride, dsc->sram_px1,
          dsc->sram_bytes) != LV_RESULT_OK ||
      lv_draw_buf_init(&dsc->draw2, info.hor_res, info.strip_rows,
          LV_COLOR_FORMAT_RGB565, stride, dsc->sram_px2,
          dsc->sram_bytes) != LV_RESULT_OK)
    {
      syslog(LOG_ERR, "lv_myvendor_lcd: lv_draw_buf_init failed");
      lv_free(dsc);
      lv_display_delete(disp);
      myvendor_lcd_disp_deinit();
      return NULL;
    }

  lv_display_set_driver_data(disp, dsc);
  myvendor_coredump_bind_fb(dsc->sram_px1, dsc->sram_bytes,
                            dsc->sram_px2, dsc->sram_bytes);
  lv_display_set_draw_buffers(disp, &dsc->draw1, &dsc->draw2);
  lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_flush_wait_cb(disp, flush_wait_cb);
  lv_display_add_event_cb(disp, display_release_cb, LV_EVENT_DELETE, disp);

  myvendor_lcd_disp_register_done_cb(flush_done_cb, disp);
  myvendor_lcd_disp_register_recover_cbs(flush_refr_cb, flush_unblock_cb, disp);

  syslog(LOG_INFO,
         "lv_myvendor_lcd: %ux%u strip=%u SRAM A/B ~%u KiB x2 ui_flush DMA epic=%d",
         info.hor_res, info.ver_res, info.strip_rows,
         (unsigned)(dsc->sram_bytes / 1024u),
         (int)LV_USE_SIFLI_EPIC);
  LV_LOG_INFO("lv_myvendor_lcd %ux%u strip=%u SRAM direct epic=%d",
              info.hor_res, info.ver_res, info.strip_rows,
              (int)LV_USE_SIFLI_EPIC);

  return disp;
}

static void flush_done_cb(void * user_data)
{
  lv_display_t * disp = (lv_display_t *)user_data;

  myvendor_lcd_disp_stats_note_flush_ready();
  lv_display_flush_ready(disp);
}

/** Step 1 (bicycle_ui only): full-screen invalidate + redraw. */
static void flush_refr_cb(void * user_data)
{
  lv_display_t * disp = (lv_display_t *)user_data;
  lv_obj_t * scr = lv_display_get_screen_active(disp);
  lv_area_t full;

  if (scr != NULL)
    {
      full.x1 = 0;
      full.y1 = 0;
      full.x2 = (int32_t)lv_display_get_horizontal_resolution(disp) - 1;
      full.y2 = (int32_t)lv_display_get_vertical_resolution(disp) - 1;
      lv_obj_invalidate_area(scr, &full);
    }

  lv_refr_now(disp);
}

/** Step 2 (bicycle_ui only): release flush_wait after DMA recover. */
static void flush_unblock_cb(void * user_data)
{
  lv_display_t * disp = (lv_display_t *)user_data;

  myvendor_lcd_disp_stats_note_flush_ready();
  lv_display_flush_ready(disp);
}

/** Wait until ui_flush finishes the submitted strip (other buffer can draw). */
static void flush_wait_cb(lv_display_t * disp)
{
  uint32_t waited_ms = 0;

  (void)disp;
  myvendor_lcd_disp_wait_idle(0, &waited_ms);
  /* Recover may finish while we block — flush_ready only (no invalidate here). */
  myvendor_lcd_disp_poll_recover_unblock();
  myvendor_lcd_disp_stats_note_wait(waited_ms, false, false);
}

static void flush_cb(lv_display_t * disp, const lv_area_t * area,
                     uint8_t * color_p)
{
  myvendor_lcd_disp_area_t a;
  lv_draw_buf_t * draw_buf;
  uint32_t stride;
  int ret;

#if LV_USE_SIFLI_EPIC
  if(lv_epic_is_initialized()) {
      lv_epic_cont_blend_reset();
      if(lv_epic_is_hardware_active()) {
          lv_epic_poll_pending_completion();
      }
  }
#endif

  if (myvendor_lcd_disp_recover_hold_active())
    {
      myvendor_lcd_disp_stats_note_flush_ready();
      lv_display_flush_ready(disp);
      return;
    }

  a.x1 = area->x1;
  a.y1 = area->y1;
  a.x2 = area->x2;
  a.y2 = area->y2;

  draw_buf = lv_display_get_buf_active(disp);
  if (draw_buf != NULL)
    {
      lv_draw_buf_invalidate_cache(draw_buf, area);
      stride = draw_buf->header.stride;
    }
  else
    {
      stride = lv_draw_buf_width_to_stride(
          lv_display_get_horizontal_resolution(disp),
          lv_display_get_color_format(disp));
    }

  ret = myvendor_lcd_disp_submit_flush(&a, color_p, stride,
      lv_display_flush_is_last(disp));
  if (ret < 0)
    {
      myvendor_lcd_disp_stats_note_submit_err();
      syslog(LOG_ERR, "[lcd_flush] submit_flush failed %d", ret);
      lv_display_flush_ready(disp);
    }
}

static void display_release_cb(lv_event_t * e)
{
  lv_display_t * disp = lv_event_get_user_data(e);
  lv_myvendor_lcd_dsc_t * dsc = lv_display_get_driver_data(disp);

  myvendor_lcd_disp_unregister_recover_cbs();
  myvendor_lcd_disp_unregister_done_cb();
  myvendor_lcd_disp_deinit();

  if (dsc != NULL)
    {
      myvendor_coredump_unbind_fb();
      lv_free(dsc);
      lv_display_set_driver_data(disp, NULL);
    }

  LV_LOG_INFO("lv_myvendor_lcd released");
}

#endif /* LV_USE_MYVENDOR_LCD_DISP */
