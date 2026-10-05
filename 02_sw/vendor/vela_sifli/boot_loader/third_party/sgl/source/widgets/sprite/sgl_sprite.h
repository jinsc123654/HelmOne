/* source/widgets/sgl_sprite.h
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

#ifndef __SGL_SPRITE_H__
#define __SGL_SPRITE_H__

#include <sgl_core.h>
#include <sgl_draw.h>
#include <sgl_mm.h>

/**
 * @WARNING!!!!!!
 * 
 * This module provides a lightweight Sprite object, primarily used for
 * character rendering and background drawing in games. It supports alpha
 * blending and global opacity control with ARGB4444 textures, suitable for
 * rendering characters, NPCs, map tiles, UI icons, and other elements in
 * 2D game scenes.
 * 
 * sgl sprite is only support ARGB4444 pixmap format
 * sgl sprite is only support ARGB4444 pixmap format
 * sgl sprite is only support ARGB4444 pixmap format
 * please ensure your pixmap is ARGB4444 format
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief sgl sprite struct
 * @obj: sgl general object
 */
typedef struct sgl_sprite {
    sgl_obj_t obj;
    const sgl_pixmap_t *pixmap;
    uint8_t alpha;
} sgl_sprite_t;

/**
 * @brief create a sprite object
 * @param parent parent of the sprite
 * @return sprite object
 */
sgl_obj_t* sgl_sprite_create(sgl_obj_t* parent);

/**
 * @brief set the alpha of the sprite
 * @param obj sprite object
 * @param alpha alpha of the sprite
 * @return none
 */
void sgl_sprite_set_alpha(sgl_obj_t *obj, uint8_t alpha);

/**
 * @brief set the pixmap of the sprite
 * @param obj sprite object
 * @param pixmap pixmap of the sprite
 * @return none
 */
void sgl_sprite_set_pixmap(sgl_obj_t *obj, const sgl_pixmap_t *pixmap);

#ifdef __cplusplus
}
#endif

#endif // !__SGL_SPRITE_H__
