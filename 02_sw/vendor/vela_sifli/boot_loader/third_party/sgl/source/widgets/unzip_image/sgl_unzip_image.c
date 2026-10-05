/* source/widgets/img/sgl_unzip_img.c
 *
 * MIT License
 *
 * Copyright(c) 2023-present All contributors of SGL  
 * Document reference link: https://sgl-docs.readthedocs.io
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * This module is ported from the unzipping code of the open-source project 
 * SC_GUI (Project URL: https://gitee.com/li_yucheng/scgui, Author Contact: QQ 617622104). 
 * We sincerely appreciate the author's open-source spirit and selfless sharing.
 */

#include <sgl_core.h>
#include <sgl_draw.h>
#include <sgl_log.h>
#include <sgl_mm.h>
#include <sgl_cfgfix.h>
#include <string.h>
#include "sgl_unzip_image.h"

/**
 * @brief Compressed image decoder structure
 */
typedef struct {
    uint32_t n;         /* Current decode position */
    int16_t x;          /* Current X coordinate */
    int16_t y;          /* Current Y coordinate */
    uint16_t rep_cnt;   /* Repeat count */
    sgl_color_t out;    /* Output color value */
    sgl_color_t unzip;  /* Unzip buffer */
    const uint8_t *p;   /* Image data pointer */
} sgl_unzip_img_dec_t;

/**
 * @brief Initialize compressed image decoder
 * @param dec Decoder structure pointer
 * @param unzip_img Compressed image data
 */
static void sgl_unzip_img_dec_init(sgl_unzip_img_dec_t *dec, const sgl_unzip_img_pixmap_t *unzip_img)
{
    SGL_ASSERT(dec != NULL);
    SGL_ASSERT(unzip_img != NULL);
    
    dec->n = 0;
    dec->x = 0;
    dec->y = 0;
    dec->rep_cnt = 0;
    dec->out = sgl_color_hex(0);
    dec->unzip = sgl_color_hex(0);
    dec->p = unzip_img->map;
}

/**
 * @brief Incrementally decompress compressed image data
 * @param dec Decoder pointer
 */
static void sgl_unzip_img_incremental(sgl_unzip_img_dec_t *dec)
{
    if (dec->rep_cnt == 0) {
        dec->rep_cnt = 1;
        if (dec->p[dec->n] & 0x20) {
            sgl_color_t dat16;
            dat16 = sgl_color_hex(dec->p[dec->n] | (dec->p[dec->n + 1] << 8));
            if (sgl_color_value(dec->unzip) == sgl_color_value(dat16)) {
                dec->n += 2;
                dec->rep_cnt = (dec->p[dec->n] << 8) | dec->p[dec->n + 1];
            } else {
                dec->unzip = dat16;
                dec->out = dat16;
            }
            dec->n += 2;
        } else {
            uint8_t b = dec->p[dec->n];
            uint16_t r = (b << 5) & 0x1800;
            uint16_t g = (b << 3) & 0x00e3;
            dec->out =  sgl_color_hex((sgl_color_value(dec->unzip) ^ (r + g + (b & 0x03))));
            dec->n++;
        }
    }
}


/**
 * @brief Draw compressed image with transparency
 * @param surf Drawing surface
 * @param xs Starting X coordinate
 * @param ys Starting Y coordinate
 * @param unzip_img Compressed image data
 * @param color Color (for color replacement)
 * @param alpha Transparency
 */
static void sgl_draw_unzip_img_with_alpha(sgl_surf_t *surf, int16_t xs, int16_t ys, 
                                       const sgl_unzip_img_pixmap_t *unzip_img, sgl_color_t color, uint8_t alpha)
{
    SGL_ASSERT(surf != NULL);
    SGL_ASSERT(unzip_img != NULL);
    
    SGL_UNUSED(color);

    sgl_area_t img_rect;
    img_rect.x1 = xs;
    img_rect.y1 = ys;
    img_rect.x2 = xs + unzip_img->width - 1;
    img_rect.y2 = ys + unzip_img->height - 1;
    
    sgl_area_t intersection;
    if (!sgl_surf_clip(surf, &img_rect, &intersection)) {
        return;
    }
    
    sgl_unzip_img_dec_t dec;
    sgl_unzip_img_dec_init(&dec, unzip_img);
    
    // int16_t xe = xs + unzip_img->width - 1;
    int16_t ye = ys + unzip_img->height - 1;
    
    while (dec.y <= ye && dec.y <= intersection.y2) {
        sgl_unzip_img_incremental(&dec);
        while (dec.rep_cnt > 0) {
            dec.rep_cnt--;
            int16_t x = xs + dec.x;
            int16_t y = ys + dec.y;
            
            if (x >= intersection.x1 && x <= intersection.x2 && 
                y >= intersection.y1 && y <= intersection.y2) {
                sgl_color_t *buf = sgl_surf_get_buf(surf, x - surf->x1, y - surf->y1);
                *buf = sgl_color_mixer(dec.out, *buf, alpha);
                // For monochrome images, can be used to change image color
                // if (buf != NULL) {
                //     if (sgl_color2int(color) != 0) {
                //         *buf = sgl_color_mixer(color, *buf, alpha);
                //     } else {
                //         *buf = sgl_color_mixer(dec.out, *buf, alpha);
                //     }
                // }
            }
            
            if (++dec.x >= unzip_img->width) {
                dec.x = 0;
                if (++dec.y > ye) {
                    break;
                }
            }
        }
    }
}

/**
 * @brief Draw compressed image
 * @param surf Drawing surface
 * @param area Clipping area
 * @param coords Object coordinates
 * @param desc Drawing description
 */
void sgl_draw_unzip_img(sgl_surf_t *surf, sgl_area_t *area, sgl_area_t *coords, sgl_draw_unzip_img_t *desc)
{
    SGL_ASSERT(surf != NULL);
    SGL_ASSERT(desc != NULL);
    SGL_ASSERT(desc->unzip_img != NULL);

    SGL_UNUSED(area);

    int16_t xs = coords->x1;
    int16_t ys = coords->y1;
    
    if (SGL_ALPHA_MIN == desc->alpha) {
        return;  
    } else {
        sgl_draw_unzip_img_with_alpha(surf, xs, ys, desc->unzip_img, desc->color, desc->alpha);
    }
}


/**
 * @brief Compressed image construction callback
 * @param surf Drawing surface
 * @param obj Object pointer
 * @param evt Event pointer
 */
static void sgl_unzip_img_construct_cb(sgl_surf_t *surf, sgl_obj_t* obj, sgl_event_t *evt)
{
    sgl_unzip_img_t *unzip_img = (sgl_unzip_img_t*)obj;

    if (evt->type == SGL_EVENT_DRAW_MAIN) {
        if (unzip_img->desc.unzip_img != NULL) {
            sgl_draw_unzip_img(surf, &obj->area, &obj->coords, &unzip_img->desc);
        }
    }
    else if (evt->type == SGL_EVENT_PRESSED || evt->type == SGL_EVENT_RELEASED) {
        if (obj->event_fn) {
            obj->event_fn(evt);
        }
    }
}

/**
 * @brief Create compressed image object
 * @param parent Parent object pointer
 * @return Object pointer
 */
sgl_obj_t* sgl_unzip_img_create(sgl_obj_t* parent)
{
    sgl_unzip_img_t *unzip_img = sgl_malloc(sizeof(sgl_unzip_img_t));
    if (unzip_img == NULL) {
        SGL_LOG_ERROR("sgl_unzip_img_create: malloc failed");
        return NULL;
    }

    memset(unzip_img, 0, sizeof(sgl_unzip_img_t));
    
    sgl_obj_t *obj = &unzip_img->obj;
    sgl_obj_init(obj, parent);
    obj->construct_fn = sgl_unzip_img_construct_cb;

    unzip_img->desc.alpha = 255;
    unzip_img->desc.unzip_img = NULL;
    unzip_img->desc.color = sgl_color_hex(0);
    unzip_img->desc.align = SGL_ALIGN_CENTER;

    return obj;
}

/**
 * @brief Set image color
 * @param obj Image object pointer
 * @param color Image color
 * @return none
 */
void sgl_unzip_img_set_color(sgl_obj_t *obj, sgl_color_t color)
{
    sgl_unzip_img_t *img = sgl_container_of(obj, sgl_unzip_img_t, obj);
    img->desc.color = color;
    sgl_obj_set_dirty(obj);
}

/**
 * @brief Set image alpha value
 * @param obj Image object pointer
 * @param alpha Image alpha value
 * @return none
 */
void sgl_unzip_img_set_alpha(sgl_obj_t *obj, uint8_t alpha)
{
    sgl_unzip_img_t *img = sgl_container_of(obj, sgl_unzip_img_t, obj);
    img->desc.alpha = alpha;
    sgl_obj_set_dirty(obj);
}

/**
 * @brief Set image alignment type
 * @param obj Image object pointer
 * @param align Image alignment type
 * @return none
 */
void sgl_unzip_img_set_align(sgl_obj_t *obj, sgl_align_type_t align)
{
    sgl_unzip_img_t *img = sgl_container_of(obj, sgl_unzip_img_t, obj);
    img->desc.align = align;
    sgl_obj_set_dirty(obj);
}

/**
 * @brief Set image compressed image data
 * @param obj Image object pointer
 * @param unzip_img Compressed image data
 * @return none
 */
void sgl_unzip_img_set_img(sgl_obj_t *obj, const sgl_unzip_img_pixmap_t *unzip_img)
{
    sgl_unzip_img_t *img = sgl_container_of(obj, sgl_unzip_img_t, obj);
    img->desc.unzip_img = unzip_img;
    if (img->desc.unzip_img != NULL) {
        sgl_obj_set_size(obj, img->desc.unzip_img->width, img->desc.unzip_img->height);
    }
    sgl_obj_set_dirty(obj);
}
