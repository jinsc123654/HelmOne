/* source/widgets/sgl_img_ext/sgl_img_ext.h
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
 */

#ifndef __SGL_IMG_EXT_H__
#define __SGL_IMG_EXT_H__

#include <sgl_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transformable image extension widget.
 *
 * Supports arbitrary rotation, independent X/Y scaling,
 * global alpha blending, and optional external flash read callback.
 */
typedef struct sgl_img_ext {
    sgl_obj_t obj;                  /**< Base widget object */
    const sgl_pixmap_t *pixmap;     /**< Source pixmap to render */
    uint8_t alpha;                  /**< Global opacity (0-255) */
    bool scale_uniform;             /**< True when X/Y scales are equal */
    int16_t rotation;               /**< Rotation angle in degrees (0-360) */
    int32_t scale_x;                /**< X-axis scale factor (fixed-point) */
    int32_t scale_y;                /**< Y-axis scale factor (fixed-point) */
    int32_t pivot_x;                /**< Rotation/scale center X (image coords, default 0 = top-left) */
    int32_t pivot_y;                /**< Rotation/scale center Y (image coords, default 0 = top-left) */
    int32_t orig_x1;                /**< Widget original top-left X (for pivot) */
    int32_t orig_y1;                /**< Widget original top-left Y (for pivot) */
    void (*read)(const size_t addr, uint8_t *out, uint32_t len_bytes); /**< Flash read callback */
    uint8_t *flash_buffer;          /**< Temporary row buffer for flash reads */
} sgl_img_ext_t;

/**
 * @brief Create a transformable image widget.
 * @param parent Parent object
 * @return New image extension object, or NULL on allocation failure
 */
sgl_obj_t* sgl_img_ext_create(sgl_obj_t* parent);

/**
 * @brief Set the pixmap source for the image widget.
 * @param obj Image extension object
 * @param pixmap Pixmap data to render
 */
void sgl_img_ext_set_pixmap(sgl_obj_t *obj, const sgl_pixmap_t *pixmap);

/**
 * @brief Set global opacity of the image widget.
 * @param obj Image extension object
 * @param alpha Opacity value (0 = transparent, 255 = opaque)
 */
void sgl_img_ext_set_alpha(sgl_obj_t *obj, uint8_t alpha);

/**
 * @brief Set rotation angle of the image widget.
 * @param obj Image extension object
 * @param rotation Angle in degrees (normalized to 0-360)
 */
void sgl_img_ext_set_rotation(sgl_obj_t *obj, int16_t rotation);

/**
 * @brief Set independent X/Y scale factors (fixed-point).
 * @param obj Image extension object
 * @param scale_x X-axis scale
 * @param scale_y Y-axis scale
 */
void sgl_img_ext_set_scale(sgl_obj_t *obj, float scale_x, float scale_y);

/**
 * @brief Set uniform scale from a signed byte.
 * @param obj Image extension object
 * @param scale Scale value: 0 = 1.0x (no scale),
 *              positive = zoom in (e.g. 64 ~ 1.5x, 128 = 2.0x),
 *              negative = zoom out (e.g. -64 ~ 0.5x)
 */
void sgl_img_ext_set_scale_uniform(sgl_obj_t *obj, int8_t scale);

/**
 * @brief Set external flash read callback for memory-mapped pixmap access.
 * @param obj Image extension object
 * @param read Read function: (flash_addr, out_buffer, length_bytes)
 */
void sgl_img_ext_set_pivot(sgl_obj_t *obj, int32_t pivot_x, int32_t pivot_y);

/**
 * @brief Set external flash read callback for memory-mapped pixmap access.
 * @param obj Image extension object
 * @param read Read function: (flash_addr, out_buffer, length_bytes)
 */
void sgl_img_ext_set_read_ops(sgl_obj_t *obj, void (*read)(const size_t addr, uint8_t *out, uint32_t len_bytes));

#ifdef __cplusplus
}
#endif

#endif /* __SGL_IMG_EXT_H__ */
