/* source/widgets/sgl_spectrum.h
 *
 * MIT License
 *
 * Copyright(c) 2023-present All contributors of SGL  
 * Document reference link: docs directory
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

#ifndef __SGL_SPECTRUM_H__
#define __SGL_SPECTRUM_H__

#include <sgl_core.h>
#include <sgl_draw.h>
#include <sgl_math.h>
#include <sgl_log.h>
#include <sgl_mm.h>
#include <sgl_cfgfix.h>
#include <string.h>

#define SGL_SPECTRUM_MODE_HAT_FLAG                 (1 << 2)
#define SGL_SPECTRUM_MODE_BAR                      (1 << 0)
#define SGL_SPECTRUM_MODE_BLOCK                    (1 << 1)
#define SGL_SPECTRUM_MODE_BAR_HAT                  (SGL_SPECTRUM_MODE_HAT_FLAG | SGL_SPECTRUM_MODE_BAR)
#define SGL_SPECTRUM_MODE_BLOCK_HAT                (SGL_SPECTRUM_MODE_HAT_FLAG | SGL_SPECTRUM_MODE_BLOCK)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief sgl spectrum struct
 * @obj: sgl general object
 */
typedef struct sgl_spectrum {
    sgl_obj_t   obj;
    sgl_color_t bar_color;
    sgl_color_t bar_hat_color;
    uint16_t    bar_num;
    uint8_t     bar_width;
    uint8_t     bar_mode;
    uint8_t     alpha;
    uint8_t     bar_hat_height;
    uint16_t    *bar_value;
    uint16_t    *bar_hat;
} sgl_spectrum_t;

/**
 * @brief create a spectrum object
 * @param parent parent of the spectrum
 * @return spectrum object
 */
sgl_obj_t* sgl_spectrum_create(sgl_obj_t* parent);

/**
 * @brief set spectrum bar number
 * @param obj spectrum object
 * @param number bar number
 * @return none
 */
void sgl_spectrum_set_bar_number(sgl_obj_t *obj, uint16_t number);

/**
 * @brief set spectrum bar value
 * @param obj spectrum object
 * @param index bar index
 * @param value bar value
 * @return none
 */
void sgl_spectrum_set_bar_value(sgl_obj_t *obj, uint16_t index, uint16_t value);

/**
 * @brief set spectrum bar mode
 * @param obj spectrum object
 * @param mode bar mode
 * @return none
 * @note mode value:
 *       SGL_SPECTRUM_MODE_BAR: bar mode
 *       SGL_SPECTRUM_MODE_BLOCK: block mode
 *       SGL_SPECTRUM_MODE_BAR_HAT: bar mode with hat
 *       SGL_SPECTRUM_MODE_BLOCK_HAT: block mode with hat
 */
void sgl_spectrum_set_bar_mode(sgl_obj_t *obj, uint8_t mode);

/**
 * @brief set spectrum bar color
 * @param obj spectrum object
 * @param color bar color
 * @return none
 */
void sgl_spectrum_set_bar_color(sgl_obj_t *obj, sgl_color_t color);

/**
 * @brief set spectrum bar hat color
 * @param obj spectrum object
 * @param color bar hat color
 * @return none
 */
void sgl_spectrum_set_bar_hat_color(sgl_obj_t *obj, sgl_color_t color);

/**
 * @brief set spectrum bar hat height
 * @param obj spectrum object
 * @param height bar hat height
 * @return none
 */
void sgl_spectrum_set_bar_hat_height(sgl_obj_t *obj, uint8_t height);

/**
 * @brief set spectrum alpha
 * @param obj spectrum object
 * @param alpha alpha value
 * @return none
 */
void sgl_spectrum_set_alpha(sgl_obj_t *obj, uint8_t alpha);

#ifdef __cplusplus
}
#endif

#endif // !__SGL_SPECTRUM_H__
