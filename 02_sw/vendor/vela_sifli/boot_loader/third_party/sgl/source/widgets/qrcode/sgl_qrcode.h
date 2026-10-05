/* source/widget/qrcode/sgl_qrcode.h
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

/**
 * For example:
 *   sgl_obj_t *qrcode = sgl_qrcode_create(NULL);
 *   sgl_obj_set_pos(qrcode, 0, 0);
 *   sgl_obj_set_size(qrcode, 300, 300);
 *   sgl_qrcode_set_logo(qrcode, &qrcode_logo);
 *   sgl_qrcode_set_ecc(qrcode, 2);
 *   sgl_qrcode_set_version(qrcode, 7);
 *   sgl_qrcode_set_text(qrcode, "https://github.com/sgl-org/sgl");
 */

#ifndef __SGL_QRCODE_H__
#define __SGL_QRCODE_H__

#include <sgl_core.h>
#include <sgl_draw.h>
#include <sgl_math.h>
#include <sgl_log.h>
#include <sgl_mm.h>
#include <sgl_cfgfix.h>
#include <string.h>
#include "qrcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_SGL_QRCODE_QR_VERSION_MAX
// #warning "CONFIG_SGL_QRCODE_QR_VERSION_MAX is not defined, using default value 5. Define it in CMakeLists.txt to override."
#define CONFIG_SGL_QRCODE_QR_VERSION_MAX    5
#endif

#ifndef SGL_QRCODE_ZONE_DEFAULT
#define SGL_QRCODE_ZONE_DEFAULT             1
#endif

#ifndef SGL_QRCODE_SCALE_DEFAULT
#define SGL_QRCODE_SCALE_DEFAULT            4
#endif

typedef struct sgl_qrcode {
    sgl_obj_t          obj;
    QRCode             qrcode;
    const sgl_pixmap_t *pixmap;
    uint8_t            *data;
    sgl_color_t        bg_color;
    sgl_color_t        cell_color;
    uint8_t            cell_radius;
    uint8_t            zone : 4;
    uint8_t            scale : 4;
    uint8_t            alpha;
} sgl_qrcode_t;


/**
 * @brief create a qrcode object
 * @param parent parent of the qrcode
 * @return qrcode object
 */
sgl_obj_t* sgl_qrcode_create(sgl_obj_t* parent);

/**
 * @brief set qrcode version
 * @param obj qrcode object
 * @param version qrcode version
 * @return none
 */
void sgl_qrcode_set_version(sgl_obj_t *obj, uint8_t version);

/**
 * @brief set qrcode ecc level
 * @param obj qrcode object
 * @param ecc qrcode ecc level
 * @return none
 * @note the ecc level must be in range [0, 3]
 */
void sgl_qrcode_set_ecc(sgl_obj_t *obj, uint8_t ecc);

/**
 * @brief set qrcode text
 * @param obj qrcode object
 * @param text qrcode text
 * @return none
 */
void sgl_qrcode_set_text(sgl_obj_t *obj, const char *text);

/**
 * @brief set qrcode alpha
 * @param obj qrcode object
 * @param alpha qrcode alpha
 * @return none
 */
void sgl_qrcode_set_alpha(sgl_obj_t *obj, uint8_t alpha);

/**
 * @brief set qrcode background color
 * @param obj qrcode object
 * @param color qrcode background color
 * @return none
 */
void sgl_qrcode_set_bg_color(sgl_obj_t *obj, sgl_color_t color);

/**
 * @brief set qrcode cell color
 * @param obj qrcode object
 * @param color qrcode cell color
 * @return none
 */
void sgl_qrcode_set_cell_color(sgl_obj_t *obj, sgl_color_t color);

/**
 * @brief set qrcode cell radius
 * @param obj qrcode object
 * @param radius qrcode cell radius
 * @return none
 */
void sgl_qrcode_set_cell_radius(sgl_obj_t *obj, uint8_t radius);

/**
 * @brief set qrcode zone
 * @param obj qrcode object
 * @param zone qrcode zone
 * @return none
 */
void sgl_qrcode_set_zone(sgl_obj_t *obj, uint8_t zone);

/**
 * @brief set qrcode scale
 * @param obj qrcode object
 * @param scale qrcode scale
 * @return none
 */
void sgl_qrcode_set_scale(sgl_obj_t *obj, uint8_t scale);

/**
 * @brief set qrcode logo
 * @param obj qrcode object
 * @param pixmap qrcode logo
 * @return none
 */
void sgl_qrcode_set_logo(sgl_obj_t *obj, const sgl_pixmap_t *pixmap);

/**
 * @brief set qrcode logo radius
 * @param obj qrcode object
 * @param radius qrcode logo radius
 * @return none
 */
void sgl_qrcode_set_logo_radius(sgl_obj_t *obj, int16_t radius);

#ifdef __cplusplus
}
#endif

#endif // __SGL_QRCODE_H__
