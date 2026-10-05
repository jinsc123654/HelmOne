/* source/component/sgl_3dvortex.h
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

#ifndef __SGL_3DVORTEX_H__
#define __SGL_3DVORTEX_H__

#include <sgl_core.h>
#include <sgl_draw.h>
#include <sgl_math.h>
#include <sgl_log.h>
#include <sgl_mm.h>
#include <sgl_cfgfix.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SGL_VORTEX_MAX_PARTICLES    150
#define SGL_VORTEX_INIT_PARTICLES   150
#define SGL_VORTEX_TRAIL_ALPHA      38

typedef struct sgl_3dvortex {
    sgl_obj_t       obj;
    void           *state;
    bool            running;
    uint8_t         trail_alpha;
    bool            update;
} sgl_3dvortex_t;

sgl_obj_t* sgl_3dvortex_create(sgl_obj_t* parent);
void sgl_3dvortex_set_running(sgl_obj_t *obj, bool running);
void sgl_3dvortex_set_trail_alpha(sgl_obj_t *obj, uint8_t alpha);
void sgl_3dvortex_reset(sgl_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /* __SGL_3DVORTEX_H__ */
