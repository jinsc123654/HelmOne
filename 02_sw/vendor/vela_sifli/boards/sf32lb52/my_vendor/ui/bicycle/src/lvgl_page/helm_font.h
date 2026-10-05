/**
 * @file helm_font.h
 * @brief 菜单/标题：MiSans Medium 4bpp 点阵，缺字按 `苹方位图 → 系统 TTF` 回退。
 *        通知/地图长文本（用户文件名、蓝牙设备名、App 通知正文）：量产直接用
 *        helm_font_sys() 的 FreeType；
 *        工厂固件只用 4bpp C 字模（helm_mism4_* + helm_ui_* 回退，没有 TTF）。
 *        大号速度数字仍用码表位图。
 *
 * @note 点阵库只覆盖 582 个码位（2026-10-01 实测），外来文本必然缺字，而本工程
 *       开了 `LV_USE_FONT_PLACEHOLDER` ⇒ 缺字画成**方块**。链尾挂系统 TTF 是
 *       为了让这些字有地方可落，见 helm_font.c。
 */

#ifndef HELM_FONT_H
#define HELM_FONT_H

#include <nuttx/config.h>
#include "lvgl/lvgl.h"
#include "myvendor_system_font.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(helm_ui_12);
LV_FONT_DECLARE(helm_ui_15);
LV_FONT_DECLARE(helm_ui_22);
LV_FONT_DECLARE(helm_mism4_12);
LV_FONT_DECLARE(helm_mism4_15);
LV_FONT_DECLARE(helm_mism4_22);
LV_FONT_DECLARE(helm_ui_64);
LV_FONT_DECLARE(helm_num_28);
LV_FONT_DECLARE(helm_num_32);
/** @brief 40px 时钟字（待机页主时钟用）。
 *  @details 离线生成，只有 `0123456789:.` 十二个字形、2bpp、位图 1824 B
 *           （生成器 `zcode/analysis/helm_preview/genfont.py`，不依赖 lv_font_conv；
 *           已回读校验：与 PingFangSC-Semibold 直渲 40px 逐像素同形，"14:32" 同为 107px）。
 *           为什么单开一档：32px 偏小，56px 的 "14:32" 宽 150px 而待机页左栏约 110px，
 *           中间这一档原来是空的。 */
LV_FONT_DECLARE(helm_num_40);
LV_FONT_DECLARE(helm_num_56);
LV_FONT_DECLARE(helm_num_64);
LV_FONT_DECLARE(helm_fa_16);

#define HELM_FA_CHECK    "\xEF\x80\x8C"
#define HELM_FA_XMARK    "\xEF\x80\x8D"
#define HELM_FA_CLOCK    "\xEF\x80\x97"
#define HELM_FA_PAUSE    "\xEF\x81\x8C"
#define HELM_FA_CHART    "\xEF\x88\x81"
#define HELM_FA_ROUTE    "\xEF\x93\x97"
#define HELM_FA_MOUNTAIN "\xEF\x9B\xBC"

/**
 * @brief 系统 TTF（PSRAM FreeType）。工厂固件返回 @p fallback。
 * @param px       字号。
 * @param fallback 点阵回退，只给调用方用。
 * @return TTF 或 fallback。
 * @warning **不得**把 `fallback` 写进共享 TTF 缓存槽的 `f->fallback`，
 *          否则所有调用方互相污染。
 */
static inline const lv_font_t * helm_font_sys(int32_t px, const lv_font_t * fallback)
{
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
    LV_UNUSED(px);
    return fallback;
#else
    lv_font_t * f = myvendor_system_font_get(px);

    return f ? f : fallback;
#endif
}

/**
 * @brief 把已就绪的系统 TTF 接到点阵链尾（保底字体）。
 * @details 幂等、可重复调；TTF 还没预载完时是空操作，下次调用再补。
 *          `helm_font_lab/title/val/status` 每次都会顺手调一次，所以调用方
 *          通常不需要自己调 —— 只有"想让已经挂在屏上的对象立刻生效"时才显式调。
 */
void helm_font_sync_sys(void);

/** @brief 副标题/行内小字 12px 点阵（MiSans → 苹方 → TTF）。 */
const lv_font_t * helm_font_lab(void);
/** @brief 行标题 15px 点阵（MiSans → 苹方 → TTF）。 */
const lv_font_t * helm_font_title(void);
/** @brief 大号数值 22px 点阵（MiSans → 苹方 → TTF）。 */
const lv_font_t * helm_font_val(void);
/** @brief 状态栏 12px 苹方位图（不含 MiSans 层）+ TTF 保底。 */
const lv_font_t * helm_font_status(void);

static inline const lv_font_t * helm_font_quad(void)
{
    return &helm_num_32;
}

static inline const lv_font_t * helm_font_rot_speed(void)
{
    return &helm_num_28;
}

static inline const lv_font_t * helm_font_speed_ride(void)
{
    return &helm_num_56;
}

static inline const lv_font_t * helm_font_speed(void)
{
    return &helm_num_64;
}

static inline const lv_font_t * helm_font_mark(void)
{
    return &helm_ui_64;
}

#ifdef __cplusplus
}
#endif

#endif /* HELM_FONT_H */
