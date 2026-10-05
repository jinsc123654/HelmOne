/**
 * @file helm_splash.h
 * @brief 开机进入 / 关机退出共用的白底 splash。
 *
 * 同一套 logo、品名、滚动事项行和细进度条。事项行格式为
 * `左边名称  ·  右边状态`（中间间隔点，两侧各两空格），与关机三项、
 * 开机三项共用。进行中的右侧会由 `helm_splash_pump()` 追加 `.` / `..` / `...`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HELM_SPLASH_H
#define HELM_SPLASH_H

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 黑→白（或灰阶）背景阶数。 */
#define HELM_SPLASH_BG_STEPS 6

/** @brief 事项行滚入 / 滚出时长（毫秒）。 */
#define HELM_SPLASH_SLIDE_MS 420u

/**
 * @brief 白底 splash 控件组。
 *
 * 由 `helm_splash_build()` 填入。调用方持有本结构，不要把指针存进
 * 会在 `helm_splash_clear()` 之后仍被动画回调解引用的地方。
 */
typedef struct helm_splash {
    lv_obj_t *root;       /**< 页面根（开机页或关机全屏层）。 */
    lv_obj_t *logo;       /**< 与 2SFBL 相同的 logo 图。 */
    lv_obj_t *name;       /**< 品名（如 Helm One）。 */
    lv_obj_t *clip;       /**< 事项行裁剪窗，滚入滚出在这里完成。 */
    lv_obj_t *lab;        /**< 事项行文本。 */
    lv_obj_t *bar;        /**< 进度条轨道。 */
    lv_obj_t *bar_fill;   /**< 进度条填充。 */
    char pending[48];     /**< `helm_splash_show()` 排队的下一行。 */
    char shown[48];       /**< 当前行（不含动态省略点）。 */
    uint16_t stride;      /**< 滚入/滚出位移（约一行高）。 */
    uint8_t gray;         /**< 背景灰阶 0=黑 255=白。 */
    uint8_t bar_pct;      /**< 进度 0–100。 */
    uint16_t slide_ms;    /**< 覆盖默认 `HELM_SPLASH_SLIDE_MS`；0 则用默认。 */
    bool tick_on;         /**< 已有一行在显示（含滚入中）。 */
    bool out_busy;        /**< 正在滚出，结束后再吃 pending。 */
    bool busy;            /**< true：右侧跟省略点；结果行必须 false。 */
} helm_splash_t;

/**
 * @brief 在 @p root 上创建 logo、品名、裁剪事项行和进度条。
 * @param s    调用方提供的结构，会被清零后填入。
 * @param root 父对象，须已铺满 240×320。
 */
void helm_splash_build(helm_splash_t *s, lv_obj_t *root);

/**
 * @brief 背景与文字/进度条按灰阶反相（白底黑字 ↔ 黑底白字）。
 * @param s splash。
 * @param g 0=黑，255=白。
 */
void helm_splash_set_gray(helm_splash_t *s, uint8_t g);

/**
 * @brief 按 `HELM_SPLASH_BG_STEPS` 把背景设到某一阶。
 * @param s    splash。
 * @param step 0…`HELM_SPLASH_BG_STEPS`。
 */
void helm_splash_set_step(helm_splash_t *s, uint8_t step);

/**
 * @brief 事项行改用指定字体（开机 TTF 就绪后切到系统字）。
 * @param s    splash。
 * @param font 不得为 NULL；不要把共享 TTF 的 `fallback` 写回缓存槽。
 */
void helm_splash_set_tick_font(helm_splash_t *s, const lv_font_t *font);

/**
 * @brief 隐藏或显示事项行裁剪窗与进度条（品牌 logo/品名不动）。
 * @param s    splash。
 * @param hide true 隐藏。
 */
void helm_splash_set_chrome_hidden(helm_splash_t *s, bool hide);

/**
 * @brief 事项行与进度条整体不透明度。
 * @param s splash。
 * @param o `LV_OPA_*`。
 */
void helm_splash_set_chrome_opa(helm_splash_t *s, lv_opa_t o);

/**
 * @brief 关机刷黑前去掉 logo、品名和 chrome，避免反相灰影。
 * @param s splash。
 */
void helm_splash_hide_brand(helm_splash_t *s);

/**
 * @brief 立刻改整行文字，不滚动、不加省略点。
 * @param s   splash。
 * @param txt 可为 NULL（空行）。
 */
void helm_splash_set_text(helm_splash_t *s, const char *txt);

/**
 * @brief 立刻改整行文字。
 * @param s    splash。
 * @param txt  整行内容。
 * @param busy true 时 `helm_splash_pump()` 会在末尾追加省略点。
 */
void helm_splash_set_line(helm_splash_t *s, const char *txt, bool busy);

/**
 * @brief 立刻改成关机/开机风格的一行：`key  ·  val`。
 * @param s    splash。
 * @param key  左侧事项名，如「骑行记录」。
 * @param val  右侧状态，如「恢复中」/「已保存」。
 * @param busy true 表示进行中（跟省略点）；结果行传 false。
 */
void helm_splash_set_row(helm_splash_t *s, const char *key, const char *val,
                         bool busy);

/**
 * @brief 切到新文案：若已有一行则先滚出再滚入，否则直接滚入。
 * @param s   splash。
 * @param txt 新整行；与当前相同则忽略。
 * @note 开机/关机事项切换请用 `set_row` + `slide_in`/`slide_out`，以便
 *       同一事项内把「进行中」改成「已完成」时不滚动。
 */
void helm_splash_show(helm_splash_t *s, const char *txt);

/**
 * @brief 刷新进行中的省略点。开机 poll / 关机 tick 里调用。
 * @param s splash。
 */
void helm_splash_pump(helm_splash_t *s);

/**
 * @brief 当前行从裁剪窗下方滚入。
 * @param s splash。
 */
void helm_splash_slide_in(helm_splash_t *s);

/**
 * @brief 当前行向上滚出。结束后若有 pending 会自动滚入。
 * @param s splash。
 */
void helm_splash_slide_out(helm_splash_t *s);

/**
 * @brief 进度条立刻跳到 @p pct（0–100）。
 * @param s   splash。
 * @param pct 百分比。
 */
void helm_splash_bar_set(helm_splash_t *s, uint8_t pct);

/**
 * @brief 进度条在 @p ms 内线性从 @p from 走到 @p to。
 * @param s    splash。
 * @param from 起点百分比。
 * @param to   终点百分比。
 * @param ms   时长；0 当作 1 ms。
 * @note 关机条走完整段时序，不要在事项边界把进度条跳到 20/50/80。
 */
void helm_splash_bar_linear(helm_splash_t *s, uint8_t from, uint8_t to,
                            uint32_t ms);

/**
 * @brief 停掉本 splash 上的动画并清零结构（对象由页面析构）。
 * @param s splash。
 */
void helm_splash_clear(helm_splash_t *s);

#ifdef __cplusplus
}
#endif

#endif /* HELM_SPLASH_H */
