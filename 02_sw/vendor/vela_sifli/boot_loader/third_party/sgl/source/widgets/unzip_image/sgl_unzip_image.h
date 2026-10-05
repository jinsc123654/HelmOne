/* source/widgets/img/sgl_unzip_img.h
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

#ifndef __SGL_UNZIP_IMAGE_H__
#define __SGL_UNZIP_IMAGE_H__

#include <sgl_core.h>
#include <sgl_draw.h>

/**
 * @brief Compressed image data structure
 */
typedef struct {
    uint16_t width;          // Image width
    uint16_t height;         // Image height
    const uint8_t *map;      // Compressed image data
} sgl_unzip_img_pixmap_t;

/**
 * @brief Compressed image drawing description
 */
typedef struct {
    const sgl_unzip_img_pixmap_t *unzip_img;  // Compressed image data
    sgl_color_t color;                    // Image color
    uint8_t alpha;                        // Transparency
    sgl_align_type_t align;               // Alignment type
} sgl_draw_unzip_img_t;

/**
 * @brief Compressed image object
 */
typedef struct {
    sgl_obj_t obj;                // Base object
    sgl_draw_unzip_img_t desc;    // Drawing description
} sgl_unzip_img_t;

/**
 * @brief Create compressed image object
 * @param parent Parent object
 * @return Created image object pointer
 */
sgl_obj_t* sgl_unzip_img_create(sgl_obj_t *parent);

/**
 * @brief Set image color
 * @param obj Image object pointer
 * @param color Image color
 * @return none
 */
void sgl_unzip_img_set_color(sgl_obj_t *obj, sgl_color_t color);

/**
 * @brief Set image alpha value
 * @param obj Image object pointer
 * @param alpha Image alpha value
 * @return none
 */
void sgl_unzip_img_set_alpha(sgl_obj_t *obj, uint8_t alpha);

/**
 * @brief Set image alignment type
 * @param obj Image object pointer
 * @param align Image alignment type
 * @return none
 */
void sgl_unzip_img_set_align(sgl_obj_t *obj, sgl_align_type_t align);

/**
 * @brief Set image compressed image data
 * @param obj Image object pointer
 * @param unzip_img Compressed image data
 * @return none
 */
void sgl_unzip_img_set_img(sgl_obj_t *obj, const sgl_unzip_img_pixmap_t *unzip_img);

#endif /* __SGL_UNZIP_IMAGE_H__ */
