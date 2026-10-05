/****************************************************************************
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sfconfig.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/lcd/memlcd.h>
#include <nuttx/nuttx.h>
#include <nuttx/cache.h>
#include <nuttx/video/fb.h>

#include "chip.h"
#include "arm_internal.h"


#include "drv_io.h"
#include "sf32lb_lcd.h"

/* Force linker to pull in LCD driver objects from static library.
 * LCD_DRIVER_EXPORT places descriptors in the LcdDriverDescTab section,
 * but the linker will not extract unreferenced .o files from .a archives.
 * These extern references ensure the driver objects are linked in.
 */

#ifdef CONFIG_LCD_USING_CO5300
extern const lcd_drv_desc_t __lcddriver_co5300;
const void *_lcd_drv_ref_co5300
  __attribute__((used, section(".rodata"))) = &__lcddriver_co5300;
#endif

#ifdef CONFIG_LCD_USING_NV3031A
extern const lcd_drv_desc_t __lcddriver_nv3031a;
const void *_lcd_drv_ref_nv3031a
  __attribute__((used, section(".rodata"))) = &__lcddriver_nv3031a;
#endif

#ifdef CONFIG_LCD_USING_ILI8688E
extern const lcd_drv_desc_t __lcddriver_ili8688e;
const void *_lcd_drv_ref_ili8688e
  __attribute__((used, section(".rodata"))) = &__lcddriver_ili8688e;
#endif
/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
#define lcd_debug_print lcdinfo

struct sf32lb_lcd_dev_s
{
    struct lcd_dev_s dev;
    LCDC_HandleTypeDef hlcdc;
    lcd_drv_desc_t *p_drv_ops;
    uint16_t buf_format;
    HAL_LCDC_LayerDef select_layer;

    FAR sem_t init_sem;
    FAR sem_t draw_sem;

    int power;
    uint8_t bpp;
};

/* Configuration ************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/
static struct sf32lb_lcd_dev_s s_drv_lcd;
static volatile bool s_fb_registering;
static volatile bool s_lcd_hw_ready;

static void sf32lb_lcd_ensure_display_on(FAR struct sf32lb_lcd_dev_s *dev)
{
  if (dev == NULL || dev->p_drv_ops == NULL || dev->p_drv_ops->p_ops == NULL)
    {
      return;
    }

  if (dev->power > 0)
    {
      return;
    }

  if (dev->p_drv_ops->p_ops->DisplayOn != NULL)
    {
      dev->p_drv_ops->p_ops->DisplayOn(&dev->hlcdc);
      dev->power = CONFIG_LCD_MAXPOWER;
    }

  /* Pixels are already in GRAM (caller just finished PUTAREA).
   * 2SFBL handoff already has backlight on — don't pulse it off/on. */

  if (BSP_LCD_BL_GetPct() == 0)
    {
      BSP_LCD_BL_Set(1);
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/
static lcd_drv_desc_t *find_right_driver(void)
{

#ifdef CONFIG_LCD_USING_CO5300
  lcdinfo("Use configured lcd driver: co5300");
  return (lcd_drv_desc_t *)&__lcddriver_co5300;
#endif

#ifdef CONFIG_LCD_USING_NV3031A
  lcdinfo("Use configured lcd driver: nv3031a");
  return (lcd_drv_desc_t *)&__lcddriver_nv3031a;
#endif

#ifdef CONFIG_LCD_USING_ILI8688E
  lcdinfo("Use configured lcd driver: ili8688e");
  return (lcd_drv_desc_t *)&__lcddriver_ili8688e;
#endif

    lcd_drv_desc_t *table_begin = NULL;
    lcd_drv_desc_t *table_end = NULL;
    lcd_drv_desc_t *p_drv_desc = NULL;

#if defined(__CC_ARM) || (defined (__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050))                                 /* ARM C Compiler */
    extern const int LcdDriverDescTab$$Base;
    extern const int LcdDriverDescTab$$Limit;
    table_begin = (lcd_drv_desc_t *) &LcdDriverDescTab$$Base;
    table_end = (lcd_drv_desc_t *)   &LcdDriverDescTab$$Limit;
#elif defined (__ICCARM__) || defined(__ICCRX__)      /* for IAR Compiler */
#error "tobe contribute"
#elif defined (__GNUC__)                              /* for GCC Compiler */
    extern const int LcdDriverDescTab_start;
    extern const int LcdDriverDescTab_end;
    table_begin = (lcd_drv_desc_t *)&LcdDriverDescTab_start;
    table_end = (lcd_drv_desc_t *) &LcdDriverDescTab_end;
#endif /* defined(__CC_ARM) */

    if ((NULL == table_begin) || (NULL == table_end) || (table_begin == table_end))
    {
        lcdwarn("No LCD driver registered!");
        return NULL;

    }

#ifndef LCD_MISSING
    for (p_drv_desc = table_begin; p_drv_desc < table_end; p_drv_desc++)
    {
        if ((p_drv_desc->p_ops != NULL) && (p_drv_desc->p_init_cfg != NULL))
        {
            if (p_drv_desc->p_ops->ReadID != NULL)
            {
                uint32_t id;

                if (p_drv_desc->p_ops->Init != NULL)
                    p_drv_desc->p_ops->Init(&s_drv_lcd.hlcdc);

                id = p_drv_desc->p_ops->ReadID(&s_drv_lcd.hlcdc);

                if (p_drv_desc->id == id)
                {
                    lcdinfo("Found lcd %s id:%lxh", p_drv_desc->name, id);
                    return p_drv_desc;
                }
                else
                {
                    lcdinfo("Try lcd %s, read id:%lxh, expect:%lxh", p_drv_desc->name, id, p_drv_desc->id);
                }
            }
        }
    }
#endif
    lcdwarn("unknow lcd!");
    return NULL;
}

static void sf32lb_lcd_setarea(FAR struct sf32lb_lcd_dev_s *dev,
                           uint16_t x0, uint16_t y0,
                           uint16_t x1, uint16_t y1)
{
	if (dev && dev->p_drv_ops && dev->p_drv_ops->p_ops 
		   && dev->p_drv_ops->p_ops->SetRegion)
    {
        int new_x0, new_x1, new_y0, new_y1;


        //disable_low_power(&drv_lcd);

        lcd_debug_print("set_window [%d,%d,%d,%d]", x0, y0, x1, y1);
        new_x0 = x0;
        new_x1 = x1;
        new_y0 = y0;
        new_y1 = y1;


        DEBUGASSERT((new_x0 <= new_x1) && (new_y0 <= new_y1));
        DEBUGASSERT((new_x1 - new_x0 + 1) <= dev->p_drv_ops->lcd_horizonal_res);
        DEBUGASSERT((new_y1 - new_y0 + 1) <= dev->p_drv_ops->lcd_vertical_res);

        dev->p_drv_ops->p_ops->SetRegion(&dev->hlcdc, new_x0, new_y0, new_x1, new_y1);
        //enable_low_power(&drv_lcd);

    }
}
static void SendLayerDataCpltCbk(LCDC_HandleTypeDef *lcdc)
{

   //lcdinfo("SendLayerDataCpltCbk \r\n");

   if (lcdc->XferCpltCallback != NULL)
   {
       lcdc->XferCpltCallback = NULL;


       struct sf32lb_lcd_dev_s *p_drvlcd = container_of(lcdc, struct sf32lb_lcd_dev_s, hlcdc);
       sem_post(&p_drvlcd->draw_sem);
   }

}

static void SendLayerDataErrCbk(LCDC_HandleTypeDef *lcdc)
{
    lcdinfo("SendLayerDataErrCbk \r\n");
}


static void sf32lb_lcd_wrram(FAR struct sf32lb_lcd_dev_s *dev, FAR const uint8_t *buffer,
                          uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1)
{
   if (dev && dev->p_drv_ops && dev->p_drv_ops->p_ops 
   		  && dev->p_drv_ops->p_ops->WriteMultiplePixels)
   {
       uint16_t new_x0, new_x1, new_y0, new_y1;
  size_t pixels;
  size_t xfer_bytes;

       DEBUGASSERT((x0 <= x1) && (y0 <= y1));

       //disable_low_power(&drv_lcd);

       lcd_debug_print("sf32lb_lcd_wrram [%d,%d,%d,%d]", x0, y0, x1, y1);
       new_x0 = x0;
       new_x1 = x1;
       new_y0 = y0;
       new_y1 = y1;


       DEBUGASSERT((new_x0 <= new_x1) && (new_y0 <= new_y1));
       DEBUGASSERT((new_x1 - new_x0 + 1) <= dev->p_drv_ops->lcd_horizonal_res);
       DEBUGASSERT((new_y1 - new_y0 + 1) <= dev->p_drv_ops->lcd_vertical_res);

        /* Ensure DMA reads the latest pixel data from memory. */

        pixels = (size_t)(new_x1 - new_x0 + 1) * (size_t)(new_y1 - new_y0 + 1);
        xfer_bytes = pixels * ((size_t)dev->bpp >> 3);
        if (buffer != NULL && xfer_bytes > 0)
        {
          up_clean_dcache((uintptr_t)buffer, (uintptr_t)buffer + xfer_bytes);
        }
       
        /* Drain stale completion tokens before starting a new transfer. */
        while (sem_trywait(&(dev->draw_sem)) == 0)
        {
        }

        dev->hlcdc.XferCpltCallback = SendLayerDataCpltCbk;
        dev->hlcdc.XferErrorCallback = SendLayerDataErrCbk;
        dev->hlcdc.debug_cnt0++;


        dev->p_drv_ops->p_ops->WriteMultiplePixels(&dev->hlcdc, buffer, new_x0, new_y0, new_x1, new_y1);
        //enable_low_power(&drv_lcd);
        /* --------- Wait send complete (bounded wait) -----------------*/
        {
          struct timespec ts;
          clock_gettime(CLOCK_REALTIME, &ts);
          ts.tv_nsec += 200 * 1000 * 1000; /* 200ms */
          if (ts.tv_nsec >= 1000000000L)
          {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
          }

          if (sem_timedwait(&(dev->draw_sem), &ts) < 0)
          {
            lcdwarn("lcd xfer wait timeout: %d", errno);
          }
        }

   }
}


                           

/****************************************************************************
 * Name:  sf32lb_lcd_putrun
 *
 * Description:
 *   This method can be used to write a partial raster line to the LCD:
 *
 *   dev     - The lcd device
 *   row     - Starting row to write to (range: 0 <= row < yres)
 *   col     - Starting column to write to (range: 0 <= col <= xres-npixels)
 *   buffer  - The buffer containing the run to be written to the LCD
 *   npixels - The number of pixels to write to the LCD
 *             (range: 0 < npixels <= xres-col)
 *
 ****************************************************************************/

static int sf32lb_lcd_putrun(FAR struct lcd_dev_s *dev,
                         fb_coord_t row, fb_coord_t col,
                         FAR const uint8_t *buffer, size_t npixels)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  if (s_fb_registering || !s_lcd_hw_ready)
    {
      return OK;
    }

  lcd_debug_print("row: %d col: %d npixels: %d\n", row, col, npixels);
  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  if (priv->bpp == 8)
    {
      FAR uint16_t *conv;
      size_t i;

      conv = kmm_malloc(npixels * sizeof(uint16_t));
      if (conv == NULL)
        {
          return -ENOMEM;
        }

      for (i = 0; i < npixels; i++)
        {
          uint8_t v = buffer[i];
          uint8_t r = (v >> 5) & 0x07;
          uint8_t g = (v >> 2) & 0x07;
          uint8_t b = v & 0x03;

          conv[i] = (uint16_t)((((uint16_t)r * 31 / 7) << 11) |
                               (((uint16_t)g * 63 / 7) << 5) |
                               (((uint16_t)b * 31 / 3) << 0));
        }

      sf32lb_lcd_setarea(priv, col, row, col + npixels - 1, row);
      sf32lb_lcd_wrram(priv, (FAR const uint8_t *)conv,
                       col, row, col + npixels - 1, row);
      sf32lb_lcd_ensure_display_on(priv);
      kmm_free(conv);
      return OK;
    }

  sf32lb_lcd_setarea(priv, col, row, col + npixels - 1, row);
  sf32lb_lcd_wrram(priv, buffer, col, row, col + npixels - 1, row);
  sf32lb_lcd_ensure_display_on(priv);

  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_putarea
 *
 * Description:
 *   This method can be used to write a partial area to the LCD:
 *
 *   dev       - The lcd device
 *   row_start - Starting row to write to (range: 0 <= row < yres)
 *   row_end   - Ending row to write to (range: row_start <= row < yres)
 *   col_start - Starting column to write to (range: 0 <= col <= xres)
 *   col_end   - Ending column to write to
 *               (range: col_start <= col_end < xres)
 *   buffer    - The buffer containing the area to be written to the LCD
 *   stride    - Length of a line in bytes. This parameter may be necessary
 *               to allow the LCD driver to calculate the offset for partial
 *               writes when the buffer needs to be splited for row-by-row
 *               writing.
 *
 ****************************************************************************/

static int sf32lb_lcd_putarea(FAR struct lcd_dev_s *dev,
                          fb_coord_t row_start, fb_coord_t row_end,
                          fb_coord_t col_start, fb_coord_t col_end,
                          FAR const uint8_t *buffer, fb_coord_t stride)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;
  size_t bytes_per_pixel;
  size_t row_bytes;

  if (s_fb_registering || !s_lcd_hw_ready)
    {
      return OK;
    }

  bytes_per_pixel = priv->bpp >> 3;
  row_bytes = (size_t)(col_end - col_start + 1) * bytes_per_pixel;

  lcd_debug_print("row_start: %d row_end: %d col_start: %d col_end: %d\n",
         row_start, row_end, col_start, col_end);

  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  if (priv->bpp == 8)
    {
      fb_coord_t y;
      fb_coord_t width = col_end - col_start + 1;
      FAR uint16_t *conv = kmm_malloc(width * sizeof(uint16_t));

      if (conv == NULL)
        {
          return -ENOMEM;
        }

      for (y = row_start; y <= row_end; y++)
        {
          FAR const uint8_t *src = buffer + (y - row_start) * stride;
          fb_coord_t x;

          for (x = 0; x < width; x++)
            {
              uint8_t v = src[x];
              uint8_t r = (v >> 5) & 0x07;
              uint8_t g = (v >> 2) & 0x07;
              uint8_t b = v & 0x03;

              conv[x] = (uint16_t)((((uint16_t)r * 31 / 7) << 11) |
                                   (((uint16_t)g * 63 / 7) << 5) |
                                   (((uint16_t)b * 31 / 3) << 0));
            }

          sf32lb_lcd_setarea(priv, col_start, y, col_end, y);
          sf32lb_lcd_wrram(priv, (FAR const uint8_t *)conv,
                           col_start, y, col_end, y);
        }

      sf32lb_lcd_ensure_display_on(priv);
      kmm_free(conv);
      return OK;
    }

  if ((size_t)stride == row_bytes)
    {
      fb_coord_t y = row_start;
      const fb_coord_t chunk_rows = 24;

      while (y <= row_end)
        {
          fb_coord_t y1 = y + chunk_rows - 1;
          FAR const uint8_t *src;

          if (y1 > row_end)
            {
              y1 = row_end;
            }

          src = buffer + (y - row_start) * stride;
          sf32lb_lcd_setarea(priv, col_start, y, col_end, y1);
          sf32lb_lcd_wrram(priv, src, col_start, y, col_end, y1);
          y = y1 + 1;
        }
    }
  else
    {
      fb_coord_t y;

      /* The source rows are not tightly packed for this area, so send
       * one row at a time using stride to step through the source buffer.
       */

      for (y = row_start; y <= row_end; y++)
        {
          FAR const uint8_t *src = buffer + (y - row_start) * stride;

          sf32lb_lcd_setarea(priv, col_start, y, col_end, y);
          sf32lb_lcd_wrram(priv, src, col_start, y, col_end, y);
        }
    }

  sf32lb_lcd_ensure_display_on(priv);

  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getrun
 *
 * Description:
 *   This method can be used to read a partial raster line from the LCD:
 *
 *  dev     - The lcd device
 *  row     - Starting row to read from (range: 0 <= row < yres)
 *  col     - Starting column to read read (range: 0 <= col <= xres-npixels)
 *  buffer  - The buffer in which to return the run read from the LCD
 *  npixels - The number of pixels to read from the LCD
 *            (range: 0 < npixels <= xres-col)
 *
 ****************************************************************************/

#ifndef CONFIG_LCD_NOGETRUN
static int sf32lb_lcd_getrun(FAR struct lcd_dev_s *dev,
                         fb_coord_t row, fb_coord_t col,
                         FAR uint8_t *buffer, size_t npixels)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;
  //FAR uint16_t *dest = (FAR uint16_t *)buffer;

  lcdinfo("row: %d col: %d npixels: %d\n", row, col, npixels);
  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  sf32lb_lcd_setarea(priv, col, row, col + npixels - 1, row);
  //sf32lb_lcd_rdram(priv, dest, npixels);
  DEBUGASSERT(0);
  return OK;
}
#endif

/****************************************************************************
 * Name:  sf32lb_lcd_getvideoinfo
 *
 * Description:
 *   Get information about the LCD video controller configuration.
 *
 ****************************************************************************/

static int sf32lb_lcd_getvideoinfo(FAR struct lcd_dev_s *dev,
                               FAR struct fb_videoinfo_s *vinfo)
{
  DEBUGASSERT(dev && vinfo);

  /* Wait for async lcd_init task to finish before accessing driver state */

  if(!s_drv_lcd.p_drv_ops)
  {
  	sem_wait(&(s_drv_lcd.init_sem));
	sem_post(&s_drv_lcd.init_sem);
  }

 switch(s_drv_lcd.bpp)
 {
    case 8:
      vinfo->fmt     = FB_FMT_RGB8_332;
      break;

    case 16:
      vinfo->fmt     = FB_FMT_RGB16_565;    /* Color format: RGB16-565: RRRR RGGG GGGB BBBB */
      break;

    case 24:
      vinfo->fmt     = FB_FMT_RGB24;    /* Color format: RGB24 */
      break;

    default:
        DEBUGASSERT(0);
        break;
  }

  vinfo->xres    = s_drv_lcd.p_drv_ops->lcd_horizonal_res;        /* Horizontal resolution in pixel columns */
  vinfo->yres    = s_drv_lcd.p_drv_ops->lcd_vertical_res;        /* Vertical resolution in pixel rows */
  vinfo->nplanes = 1;                  /* Number of color planes supported */

  
  lcdinfo("fmt: %d xres: %d yres: %d nplanes: 1\n",
          vinfo->fmt, vinfo->xres, vinfo->yres);
  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getplaneinfo
 *
 * Description:
 *   Get information about the configuration of each LCD color plane.
 *
 ****************************************************************************/

static int sf32lb_lcd_getplaneinfo(FAR struct lcd_dev_s *dev,
                               unsigned int planeno,
                               FAR struct lcd_planeinfo_s *pinfo)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  if(!s_drv_lcd.p_drv_ops)
  {
  	sem_wait(&(s_drv_lcd.init_sem));
	sem_post(&s_drv_lcd.init_sem);
  }

  DEBUGASSERT(dev && pinfo && planeno == 0);
  lcdinfo("planeno: %d bpp: %d\n", planeno, priv->bpp);

  pinfo->putrun = sf32lb_lcd_putrun;                  /* Put a run into LCD memory */
  pinfo->putarea = sf32lb_lcd_putarea;                /* Put an area into LCD */
#ifndef CONFIG_LCD_NOGETRUN
  pinfo->getrun = sf32lb_lcd_getrun;                  /* Get a run from LCD memory */
#endif
  pinfo->buffer = NULL; //(FAR uint8_t *)priv->runbuffer; /* Run scratch buffer */
  pinfo->bpp    = priv->bpp;                      /* Bits-per-pixel */
  pinfo->dev    = dev;                            /* The lcd device */
  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getpower
 ****************************************************************************/

static int sf32lb_lcd_getpower(FAR struct lcd_dev_s *dev)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  lcdinfo("power: %d\n", priv->power);
  return priv->power;
}

/****************************************************************************
 * Name:  sf32lb_lcd_setpower
 ****************************************************************************/

static int sf32lb_lcd_setpower(FAR struct lcd_dev_s *dev, int power)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  lcdinfo("power: %d\n", power);
  DEBUGASSERT((unsigned)power <= CONFIG_LCD_MAXPOWER);

  /* Set new power level */

  if (power > 0)
    {
      /* Turn on the display */

      if (priv->p_drv_ops && priv->p_drv_ops->p_ops &&
          priv->p_drv_ops->p_ops->DisplayOn)
        {
          priv->p_drv_ops->p_ops->DisplayOn(&priv->hlcdc);
        }

      /* Save the power */

      priv->power = power;
    }
  else
    {
      /* Turn off the display */

      if (priv->p_drv_ops && priv->p_drv_ops->p_ops &&
          priv->p_drv_ops->p_ops->DisplayOff)
        {
          priv->p_drv_ops->p_ops->DisplayOff(&priv->hlcdc);
        }

      /* Save the power */

      priv->power = 0;
    }

  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getcontrast
 *
 * Description:
 *   Get the current contrast setting (0-CONFIG_LCD_MAXCONTRAST).
 *
 ****************************************************************************/

static int sf32lb_lcd_getcontrast(FAR struct lcd_dev_s *dev)
{
  lcdinfo("Not implemented\n");
  return -ENOSYS;
}

/****************************************************************************
 * Name:  sf32lb_lcd_setcontrast
 *
 * Description:
 *   Set LCD panel contrast (0-CONFIG_LCD_MAXCONTRAST).
 *
 ****************************************************************************/

static int sf32lb_lcd_setcontrast(FAR struct lcd_dev_s *dev,
                              unsigned int contrast)
{
  lcdinfo("contrast: %d\n", contrast);
  return -ENOSYS;
}

                              
static int sf32lb_lcd_getalignment(FAR struct lcd_dev_s *dev,
                    FAR struct lcddev_area_align_s *align)
{
    if(align)
    {
        align->row_start_align = 2;
        align->height_align    = 2;
        align->col_start_align = 2;
        align->width_align     = 2;
        align->buf_align       = sizeof(uintptr_t);
    }

    return OK;
}

static int lcdc1_isr(int irq, void *context, void *arg)
{
    HAL_LCDC_IRQHandler((LCDC_HandleTypeDef *)arg);

    
    return OK;
}

static int lcd_hw_setup_thread_entry(int argc, FAR char *argv[])
{
    lcd_drv_desc_t *p_drv_ops = s_drv_lcd.p_drv_ops;

#if defined(CONFIG_VIDEO_FB) && defined(CONFIG_LCD_FRAMEBUFFER)
    {
      int ret;
      int retry;

    /* Register /dev/fb0 first so node creation is not blocked by panel init. */
    for (retry = 0; retry < 30; retry++)
      {
        s_fb_registering = true;
        ret = fb_register(0, 0);
        s_fb_registering = false;

        if (ret == OK || ret == -EEXIST)
          {
            lcdinfo("fb_register done.\n");
            break;
          }

        if (ret != -ENOENT && ret != -ENODEV && ret != -EBUSY)
          {
            syslog(LOG_ERR, "ERROR: fb_register() failed: %d\n", ret);
            break;
          }

        usleep(100 * 1000);
      }
    }
#endif

    if (p_drv_ops && p_drv_ops->p_ops && p_drv_ops->p_ops->Init)
    {
        p_drv_ops->p_ops->Init(&s_drv_lcd.hlcdc);
    }

#ifdef SOC_BF0_HCPU     /* gpio1 only work on hcpu */
    irq_attach(NX_IRQ(LCDC1_IRQn), lcdc1_isr, (void *)&s_drv_lcd.hlcdc);
    up_enable_irq(NX_IRQ(LCDC1_IRQn));
#endif /* SOC_BF0_HCPU */

    HAL_LCDC_SetBgColor(&s_drv_lcd.hlcdc, 0, 0, 0);
    HAL_LCDC_LayerReset(&s_drv_lcd.hlcdc, HAL_LCDC_LAYER_DEFAULT);
    HAL_LCDC_LayerSetFormat(&s_drv_lcd.hlcdc, HAL_LCDC_LAYER_DEFAULT,
                            LCDC_PIXEL_FORMAT_RGB565);

    s_lcd_hw_ready = true;
    syslog(LOG_INFO, "INFO: lcd_hw ready (keep 2SFBL panel, LCDC freq=%lu HCLK=%lu)\n",
           (unsigned long)s_drv_lcd.hlcdc.Init.freq,
           (unsigned long)HAL_RCC_GetHCLKFreq(CORE_ID_HCPU));

    return OK;
}

static int lcd_init_thread_entry(int argc, FAR char *argv[])
{
	lcd_drv_desc_t *p_drv_ops;
  int ret;
  int hw_pid;
  bool lcd_registered = false;
  int retry;

	BSP_LCD_PowerUp();
	
	p_drv_ops = find_right_driver();

#ifdef CONFIG_LCD_USING_CO5300
  if (!p_drv_ops)
  {
    p_drv_ops = (lcd_drv_desc_t *)&__lcddriver_co5300;
  }
#endif

#ifdef CONFIG_LCD_USING_NV3031A
  if (!p_drv_ops)
  {
    p_drv_ops = (lcd_drv_desc_t *)&__lcddriver_nv3031a;
  }
#endif

#ifdef CONFIG_LCD_USING_ILI8688E
  if (!p_drv_ops)
  {
    p_drv_ops = (lcd_drv_desc_t *)&__lcddriver_ili8688e;
  }
#endif

	if (p_drv_ops)
	{
    lcdinfo("Init LCD %s", p_drv_ops->name);

		switch(p_drv_ops->p_init_cfg->color_mode)
		{
		   case LCDC_PIXEL_FORMAT_RGB565:
			 s_drv_lcd.bpp = 16;
			 break;
		
		   case LCDC_PIXEL_FORMAT_RGB888:
			 s_drv_lcd.bpp = 24;
			 break;
			 
         default:
       s_drv_lcd.bpp = 16;
       lcdwarn("Unknown color mode %d, fallback to RGB565",
               p_drv_ops->p_init_cfg->color_mode);
       break;
		 }

  /* Keep framebuffer format aligned with panel color mode (typically RGB565)
   * so fb writes are sent without intermediate color conversion.
   */

	}

	s_drv_lcd.p_drv_ops = p_drv_ops; 
	sem_post(&s_drv_lcd.init_sem);

	if (!p_drv_ops)
	{
		syslog(LOG_ERR, "ERROR: No LCD driver found, skip device register\n");
		return -ENODEV;
	}


#ifdef CONFIG_LCD_DEV
    lcd_registered = false;
#endif
    /* Retry registration to tolerate early-boot timing races. */
    for (retry = 0; retry < 30; retry++)
    {
#ifdef CONFIG_LCD_DEV
      if (!lcd_registered)
      {
        ret = lcddev_register(0);
        if (ret == OK || ret == -EEXIST)
        {
          lcd_registered = true;
          lcdinfo("lcddev_register done.");
        }
        else if (ret != -ENOENT && ret != -ENODEV)
        {
          syslog(LOG_ERR, "ERROR: lcddev_register() failed: %d\n", ret);
          lcd_registered = true; /* stop retrying on hard errors */
        }
      }
#endif

#ifdef CONFIG_LCD_DEV
      if (lcd_registered)
      {
        break;
      }
#endif

      usleep(100 * 1000);
    }

    hw_pid = task_create("lcd_hw",
                         SCHED_PRIORITY_DEFAULT,
                         8192,
                         lcd_hw_setup_thread_entry,
                         NULL);

    if (hw_pid < 0)
    {
      syslog(LOG_ERR, "ERROR: lcd_hw task_create failed: %d\n", errno);
    }

	return 0;
}
/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name:  board_lcd_initialize
 *
 * Description:
 *   Initialize the LCD video hardware.  The initial state of the LCD is
 *   fully initialized, display memory cleared, and the LCD ready to use, but
 *   with the power setting at 0 (full off).
 *
 ****************************************************************************/

int board_lcd_initialize(void)
{
    static bool initialized = false;
  int pid;

    if (initialized)
      return OK;
    initialized = true;

    lcdinfo("board_lcd_initialize\n");
    
    memset(&s_drv_lcd, 0, sizeof(s_drv_lcd));

    s_drv_lcd.hlcdc.Instance = LCDC1;

    s_drv_lcd.select_layer = HAL_LCDC_LAYER_DEFAULT;
    s_lcd_hw_ready = false;

    sem_init(&(s_drv_lcd.init_sem), 0, 0);
    sem_init(&(s_drv_lcd.draw_sem), 0, 0);

    /* Keep bringup non-blocking; init/register devices in a worker task. */

    pid = task_create("lcd_init",
                      SCHED_PRIORITY_DEFAULT,
                      4096,
                      lcd_init_thread_entry,
                      NULL);

    if (pid < 0)
      {
        lcdwarn("lcd_init task_create failed: %d", errno);
        return -errno;
      }

    return OK;
}

/****************************************************************************
 * Name:  board_lcd_getdev
 *
 * Description:
 *   Return a a reference to the LCD object for the specified LCD.  This
 *   allows support for multiple LCD devices.
 *
 ****************************************************************************/

struct lcd_dev_s *board_lcd_getdev(int devno)
{
    lcdinfo("board_lcd_getdev\n");

    
    struct lcd_dev_s *g_lcd = NULL;
    g_lcd = &s_drv_lcd.dev;

    g_lcd->getvideoinfo = sf32lb_lcd_getvideoinfo;
    g_lcd->getplaneinfo = sf32lb_lcd_getplaneinfo;
    g_lcd->getpower     = sf32lb_lcd_getpower;
    g_lcd->setpower     = sf32lb_lcd_setpower;
    g_lcd->getcontrast  = sf32lb_lcd_getcontrast;
    g_lcd->setcontrast  = sf32lb_lcd_setcontrast;
    g_lcd->getareaalign = sf32lb_lcd_getalignment;
  #if 0
  g_lcd = st7789_lcdinitialize(g_spidev);
  if (!g_lcd)
    {
      lcderr("ERROR: Failed to bind SPI port %d to LCD %d\n", LCD_SPI_PORTNO,
             devno);
    }
  else
    {
      lcdinfo("SPI port %d bound to LCD %d\n", LCD_SPI_PORTNO, devno);
      return g_lcd;
    }
  #endif /* 0 */

  return g_lcd;
}

/****************************************************************************
 * Name:  board_lcd_uninitialize
 *
 * Description:
 *   Uninitialize the LCD support
 *
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
    lcdinfo("board_lcd_uninitialize\n");
    BSP_LCD_PowerDown();
    sem_destroy(&(s_drv_lcd.draw_sem));

}

void sf32lb_lcd_reclock(void)
{
  uint32_t freq;
  uint32_t hclk;
  HAL_StatusTypeDef st;

  if (!s_lcd_hw_ready)
    {
      syslog(LOG_INFO, "INFO: lcd reclock skipped (hw not ready)\n");
      return;
    }

  freq = s_drv_lcd.hlcdc.Init.freq;
  if (freq == 0)
    {
      freq = 24000000u;
    }

  hclk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
  st = HAL_LCDC_SetFreq(&s_drv_lcd.hlcdc, freq);
  syslog(LOG_INFO, "INFO: lcd reclock HCLK=%lu target=%lu st=%d\n",
         (unsigned long)hclk, (unsigned long)freq, (int)st);
}

