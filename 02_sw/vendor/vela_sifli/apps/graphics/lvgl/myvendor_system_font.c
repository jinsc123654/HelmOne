/**
 * @file myvendor_system_font.c
 * @brief 系统 TTF 字体（PSRAM 常驻 + FreeType 渲染），与 `liblvgl` 链接、不在 bicycle 里。
 *
 * 读取方式对齐 SiFli SDK：整份 TTF 读进 RAM 后 `FT_New_Memory_Face()`，
 * 避免每条文字都去 FAT 卷上随机读。日志一律走 LVGL 日志（`LV_LOG_*`），
 * 不直接调 `syslog()` —— 界面层与系统日志的边界见 docs/diag_log.md。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_system_font.h"
#include "board_malloc.h"
#include "myvendor_watchdog.h"

#include <nuttx/config.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef CONFIG_MYVENDOR_SYSTEM_FONT_DIR
#  define CONFIG_MYVENDOR_SYSTEM_FONT_DIR "/mnt/fat/fonts"
#endif

#ifndef CONFIG_MYVENDOR_SYSTEM_FONT_FILE
#  define CONFIG_MYVENDOR_SYSTEM_FONT_FILE "MiSans-Medium.subset.ttf"
#endif

#define MYVENDOR_FONT_PATH CONFIG_MYVENDOR_SYSTEM_FONT_DIR "/" CONFIG_MYVENDOR_SYSTEM_FONT_FILE
#define MYVENDOR_FONT_CACHE_SLOTS 6
#ifndef MYVENDOR_FONT_PRELOAD_CHUNK
#define MYVENDOR_FONT_PRELOAD_CHUNK 32768u
#endif

#if defined(CONFIG_MYVENDOR_FACTORY_MODE)
#  define MYVENDOR_USE_VECTOR_FONT 0
#elif LV_USE_FREETYPE
#  define MYVENDOR_USE_VECTOR_FONT 1
#else
#  define MYVENDOR_USE_VECTOR_FONT 0
#endif

static uint8_t *s_ttf_psram;
static size_t s_ttf_psram_size;
static size_t s_preload_got;
static int s_preload_fd = -1;
static bool s_preload_done;
static bool s_preload_ok;
static bool s_lv_bound;
#if MYVENDOR_USE_VECTOR_FONT && LV_USE_FS_MEMFS
static lv_fs_path_ex_t s_mempath;
#endif

typedef struct {
    int32_t px;
    lv_font_t *font;
    int refs;
} myvendor_font_slot_t;

static myvendor_font_slot_t s_cache[MYVENDOR_FONT_CACHE_SLOTS];

static myvendor_font_slot_t *myvendor_font_find_px(int32_t px)
{
    for (int i = 0; i < MYVENDOR_FONT_CACHE_SLOTS; i++) {
        if (s_cache[i].font != NULL && s_cache[i].px == px) {
            return &s_cache[i];
        }
    }

    return NULL;
}

static myvendor_font_slot_t *myvendor_font_find_ptr(lv_font_t *font)
{
    for (int i = 0; i < MYVENDOR_FONT_CACHE_SLOTS; i++) {
        if (s_cache[i].font == font) {
            return &s_cache[i];
        }
    }

    return NULL;
}

static myvendor_font_slot_t *myvendor_font_alloc_slot(void)
{
    for (int i = 0; i < MYVENDOR_FONT_CACHE_SLOTS; i++) {
        if (s_cache[i].font == NULL) {
            return &s_cache[i];
        }
    }

    return NULL;
}

bool myvendor_system_font_preloaded(void)
{
    return s_preload_ok && s_ttf_psram != NULL && s_ttf_psram_size > 0;
}

static void myvendor_system_font_preload_fail(const char * why)
{
    if (s_preload_fd >= 0) {
        close(s_preload_fd);
        s_preload_fd = -1;
    }

    if (s_ttf_psram != NULL) {
        board_free_psram(s_ttf_psram);
        s_ttf_psram = NULL;
    }

    s_ttf_psram_size = 0;
    s_preload_got = 0;
    s_preload_done = true;
    s_preload_ok = false;
    LV_LOG_ERROR("system_font: %s %s", why ? why : "fail", MYVENDOR_FONT_PATH);
}

bool myvendor_system_font_preload_pump(void)
{
#if !MYVENDOR_USE_VECTOR_FONT
    s_preload_done = true;
    s_preload_ok = false;
    return true;
#else
    off_t file_size;
    ssize_t rd;
    size_t chunk;
    size_t remain;

    if (s_preload_done) {
        return true;
    }

    if (s_preload_fd < 0) {
        if (access(MYVENDOR_FONT_PATH, R_OK) != 0) {
            myvendor_system_font_preload_fail("missing");
            return true;
        }

        s_preload_fd = open(MYVENDOR_FONT_PATH, O_RDONLY);
        if (s_preload_fd < 0) {
            myvendor_system_font_preload_fail("open failed");
            return true;
        }

        file_size = lseek(s_preload_fd, 0, SEEK_END);
        if (file_size <= 0 || lseek(s_preload_fd, 0, SEEK_SET) < 0) {
            myvendor_system_font_preload_fail("size failed");
            return true;
        }

        s_ttf_psram = (uint8_t *)board_malloc_psram((size_t)file_size);
        if (s_ttf_psram == NULL) {
            myvendor_system_font_preload_fail("PSRAM alloc failed");
            return true;
        }

        s_ttf_psram_size = (size_t)file_size;
        s_preload_got = 0;
        return false;
    }

    remain = s_ttf_psram_size - s_preload_got;
    chunk = remain > MYVENDOR_FONT_PRELOAD_CHUNK ? MYVENDOR_FONT_PRELOAD_CHUNK : remain;
    rd = read(s_preload_fd, s_ttf_psram + s_preload_got, chunk);
    if (rd <= 0) {
        myvendor_system_font_preload_fail("read failed");
        return true;
    }

    s_preload_got += (size_t)rd;
    myvendor_watchdog_busy_pump();
    if (s_preload_got < s_ttf_psram_size) {
        return false;
    }

    close(s_preload_fd);
    s_preload_fd = -1;
    s_preload_done = true;
    s_preload_ok = true;
    LV_LOG_INFO(
        "system_font: PSRAM preload %u bytes @ %s",
        (unsigned)s_ttf_psram_size,
        MYVENDOR_FONT_PATH);
    return true;
#endif
}

bool myvendor_system_font_preload(void)
{
    while (!myvendor_system_font_preload_pump()) {
    }

    return s_preload_ok;
}

bool myvendor_system_font_lv_bind(void)
{
#if !MYVENDOR_USE_VECTOR_FONT
    return false;
#else
    if (s_lv_bound) {
        return myvendor_system_font_preloaded();
    }

    if (!lv_is_initialized()) {
        return false;
    }

    if (!myvendor_system_font_preload()) {
        return false;
    }

#if LV_USE_FS_MEMFS
    lv_fs_make_path_from_buffer(&s_mempath, LV_FS_MEMFS_LETTER, s_ttf_psram,
                                (uint32_t)s_ttf_psram_size, "ttf");
#endif
    s_lv_bound = true;
    LV_LOG_INFO("system_font: LVGL FreeType bind OK");
    return true;
#endif
}

static lv_font_t *myvendor_system_font_create(int32_t px)
{
#if !MYVENDOR_USE_VECTOR_FONT
    LV_UNUSED(px);
    return NULL;
#elif LV_USE_FS_MEMFS
    return lv_freetype_font_create((const char *)&s_mempath,
                                   LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   (uint32_t)px,
                                   LV_FREETYPE_FONT_STYLE_NORMAL);
#else
    return lv_freetype_font_create(MYVENDOR_FONT_PATH,
                                   LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   (uint32_t)px,
                                   LV_FREETYPE_FONT_STYLE_NORMAL);
#endif
}

lv_font_t *myvendor_system_font_get(int32_t px)
{
#if !MYVENDOR_USE_VECTOR_FONT
    LV_UNUSED(px);
    return NULL;
#else
    myvendor_font_slot_t *slot;
    lv_font_t *font;

    if (px <= 0) {
        return NULL;
    }

    if (!myvendor_system_font_lv_bind()) {
        return NULL;
    }

    slot = myvendor_font_find_px(px);
    if (slot != NULL) {
        slot->refs++;
        return slot->font;
    }

    slot = myvendor_font_alloc_slot();
    if (slot == NULL) {
        LV_LOG_ERROR("system_font: cache full");
        return NULL;
    }

    font = myvendor_system_font_create(px);
    if (font == NULL) {
        LV_LOG_ERROR("system_font: freetype create failed px=%d", (int)px);
        return NULL;
    }

    slot->px = px;
    slot->font = font;
    slot->refs = 1;
    LV_LOG_INFO("system_font: ready px=%d", (int)px);
    return font;
#endif
}

void myvendor_system_font_put(lv_font_t *font)
{
#if MYVENDOR_USE_VECTOR_FONT
    myvendor_font_slot_t *slot = myvendor_font_find_ptr(font);

    if (slot != NULL && slot->refs > 0) {
        slot->refs--;
    }
#else
    LV_UNUSED(font);
#endif
}
