/* source/include/sgl_theme.h
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

#ifndef __SGL_THEME_H__
#define __SGL_THEME_H__

#include <sgl_cfgfix.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (CONFIG_SGL_THEME_DEFAULT)
#define SGL_THEME_DESKTOP                    sgl_rgb(255, 255, 255)
#define SGL_THEME_COLOR                      sgl_rgb(255, 255, 255)
#define SGL_THEME_BG_COLOR                   sgl_rgb(0, 0, 0)
#define SGL_THEME_BORDER_WIDTH               2
#define SGL_THEME_BORDER_COLOR               sgl_rgb(0, 0, 0)
#define SGL_THEME_ALPHA                      255
#define SGL_THEME_TEXT_COLOR                 sgl_rgb(0, 0, 0)
#define SGL_THEME_RADIUS                     0
#define SGL_THEME_SCROLL_RADIUS              3
#define SGL_THEME_SCROLL_BG_COLOR            sgl_rgb(100, 100, 100)
#define SGL_THEME_SCROLL_FG_COLOR            sgl_rgb(200, 200, 200)

#elif (CONFIG_SGL_THEME_DARK)
#define SGL_THEME_DESKTOP                    sgl_rgb(10, 10, 10)
#define SGL_THEME_COLOR                      sgl_rgb(50, 50, 50)
#define SGL_THEME_BG_COLOR                   sgl_rgb(20, 20, 20)
#define SGL_THEME_BORDER_WIDTH               2
#define SGL_THEME_BORDER_COLOR               sgl_rgb(200, 200, 200)
#define SGL_THEME_ALPHA                      255
#define SGL_THEME_TEXT_COLOR                 sgl_rgb(200, 200, 200)
#define SGL_THEME_RADIUS                     0
#define SGL_THEME_SCROLL_BG_COLOR            sgl_rgb(100, 100, 100)
#define SGL_THEME_SCROLL_FG_COLOR            sgl_rgb(200, 200, 200)

#elif (CONFIG_SGL_THEME_LIGHT)
#define SGL_THEME_COLOR                      sgl_rgb(189, 212, 243)
#define SGL_THEME_BG_COLOR                   sgl_rgb(255, 255, 255)
#define SGL_THEME_BORDER_WIDTH               2
#define SGL_THEME_BORDER_COLOR               sgl_rgb(0, 0, 0)
#define SGL_THEME_ALPHA                      255
#define SGL_THEME_TEXT_COLOR                 sgl_rgb(0, 0, 0)
#define SGL_THEME_RADIUS                     0
#define SGL_THEME_SCROLL_BG_COLOR            sgl_rgb(100, 100, 100)
#define SGL_THEME_SCROLL_FG_COLOR            sgl_rgb(200, 200, 200)

#endif

#ifdef __cplusplus
}
#endif

#endif
