/**
 * @file helm_font.c
 * @brief 菜单/标题用 MiSans 4bpp 点阵，链尾统一挂系统 TTF 兜底缺字。
 *
 * 点阵字库只覆盖 582 个码位（`helm_mism4_*` ∪ `helm_ui_*` 回退，2026-10-01 用
 * 生成文件里的 cmap 实测），用户自己的轨迹文件名、蓝牙设备名、任意 App 的通知
 * 正文必然落在库外；而本工程开了 `LV_USE_FONT_PLACEHOLDER` ⇒ 查不到的字画的是
 * **方块**，现场读作"乱码"。
 *
 * 所以每条链都按 `点阵 → 苹方点阵 → 系统 TTF` 排。LVGL 只在前面查不到时才往
 * `fallback` 走（`lv_font.c` 的 `while(f)` 循环），常见字仍走点阵、不增加
 * FreeType 光栅化开销，外来字才落到系统矢量字。
 *
 * 启动顺序：TTF 由 startup 分片预载，菜单第一次构建时**可能还没就绪**，所以链尾
 * 由 helm_font_sync_sys() 后补。补的是本文件自己的 `s_*_fb` 结构体，已经建好的
 * label 持有的正是它们的地址 ⇒ 补完即生效，不需要重建对象。
 *
 * @warning **绝不**能把 `fallback` 写进 `myvendor_system_font_get()` 返回的字体：
 *          那是所有调用方共用的缓存槽，写进去会互相污染（见 helm_font.h）。
 */

#include "helm_font.h"

#include <stdbool.h>

/** @brief 三档字号，各两层：点阵本体 + 苹方点阵（链尾留给 TTF）。 */
static lv_font_t s_lab;
static lv_font_t s_lab_fb;
static lv_font_t s_title;
static lv_font_t s_title_fb;
static lv_font_t s_val;
static lv_font_t s_val_fb;
/** @brief 状态栏用 12px 苹方（不含 MiSans 层，保持原观感）。 */
static lv_font_t s_status;
static bool s_ready;
/** @brief 链尾 TTF 是否已全部接好；没接好时每次取字体都重试。 */
static bool s_sys_linked;

/** @brief 给一档点阵补上行尾 TTF（已接过的跳过，避免重复占引用）。 */
static void helm_font_link_one(lv_font_t * fb, int32_t px)
{
    if (fb->fallback == NULL) {
        fb->fallback = myvendor_system_font_get(px);
    }
}

void helm_font_sync_sys(void)
{
    if (s_sys_linked || !myvendor_system_font_preloaded()) {
        return;
    }

    helm_font_link_one(&s_lab_fb, 12);
    helm_font_link_one(&s_title_fb, 15);
    helm_font_link_one(&s_val_fb, 22);
    helm_font_link_one(&s_status, 12);

    /* 四档只要还剩一档没挂上就继续重试；TTF 缺失 / 工厂固件时这里恒为假，
     * 链保持纯点阵 —— 与改动前同行为。 */
    s_sys_linked = (s_lab_fb.fallback != NULL && s_title_fb.fallback != NULL &&
                    s_val_fb.fallback != NULL && s_status.fallback != NULL);
}

static void helm_font_init(void)
{
    if (s_ready) {
        return;
    }

    s_lab = helm_mism4_12;
    s_lab_fb = helm_ui_12;
    s_lab.fallback = &s_lab_fb;

    s_title = helm_mism4_15;
    s_title_fb = helm_ui_15;
    s_title.fallback = &s_title_fb;

    s_val = helm_mism4_22;
    s_val_fb = helm_ui_22;
    s_val.fallback = &s_val_fb;

    /* 状态栏只要苹方一层（观感与改动前逐像素一致），链尾由 sync 补 TTF。 */
    s_status = helm_ui_12;
    s_status.fallback = NULL;

    s_ready = true;
    helm_font_sync_sys();
}

const lv_font_t * helm_font_lab(void)
{
    helm_font_init();
    helm_font_sync_sys();
    return &s_lab;
}

const lv_font_t * helm_font_title(void)
{
    helm_font_init();
    helm_font_sync_sys();
    return &s_title;
}

const lv_font_t * helm_font_val(void)
{
    helm_font_init();
    helm_font_sync_sys();
    return &s_val;
}

const lv_font_t * helm_font_status(void)
{
    helm_font_init();
    helm_font_sync_sys();
    return &s_status;
}
