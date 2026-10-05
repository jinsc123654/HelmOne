/* source/component/sgl_timer.h
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

#ifndef __SGL_TIMER_H__
#define __SGL_TIMER_H__

#include <stdint.h>
#include <stdbool.h>
#include <sgl_core.h>

#define SGL_TIMER_SHORT_SLOT                  (128U)
#define SGL_TIMER_LONG_SLOT                   (16U)
#define SGL_TIMER_LONG_STEP                   (128U)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief declare timer structure
 */
typedef struct sgl_timer sgl_timer_t;

/**
 * @brief declare timer callback function
 */
typedef void (*sgl_timer_callback_t)(const sgl_timer_t *timer, void *user_data);

/**
 * @brief timer structure
 */
struct sgl_timer {
    sgl_list_node_t node;
    sgl_timer_callback_t callback;
    void     *user_data;
    int32_t  count;
    uint16_t interval;
    uint8_t  destroyed;
    uint8_t  wheel_type;
    uint32_t last_tick;
    uint32_t wheel_pos;
};

/**
 * @brief create a timer by dynamically allocating memory
 * @return Pointer to the timer structure
 */
sgl_timer_t* sgl_timer_create(void);

/**
 * @brief delete a timer if your time is created dynamically
 * @param timer Pointer to the timer structure to be removed
 * @return none
 */
void sgl_timer_delete(sgl_timer_t *timer);

/**
 * @brief setup a timer
 * @param timer Pointer to the timer structure
 * @param callback Callback function to be called when timer expires
 * @param interval Timer interval in ticks, ms
 * @param repeat_cnt Repeat count, -1 for infinite
 * @param user_data User data passed to callback function
 * @return true if successful, false if failed
 */
bool sgl_timer_setup(sgl_timer_t *timer, sgl_timer_callback_t callback, uint16_t interval, int32_t repeat_cnt, void *user_data);

/**
 * @brief Timer handler function, should be called periodically
 * @note This function checks all timers and executes callbacks if expired
 * @warning Must be called frequently enough to not miss timer events
 */
void sgl_timer_handler(void);

#ifdef __cplusplus
}
#endif

#endif // __SGL_TIMER_H__
