#ifndef __SGL_CONFIG_H__
#define __SGL_CONFIG_H__

/* 2SFBL-trimmed SGL. Include path must list this directory before sgl/source. */

#define CONFIG_SGL_FBDEV_PIXEL_DEPTH            16
#define CONFIG_SGL_FBDEV_ROTATION               0
#define CONFIG_SGL_FBDEV_EVEN_COORDS            1
#define CONFIG_SGL_USE_FBDEV_VRAM               0
#define CONFIG_SGL_SYSTICK_MS                   10
#define CONFIG_SGL_EVENT_QUEUE_SIZE             16
#define CONFIG_SGL_DIRTY_AREA_TRACE             0
#define CONFIG_SGL_DIRTY_AREA_NUM_MAX           16
#define CONFIG_SGL_COLOR16_SWAP                 0
#define CONFIG_SGL_ANIMATION                    0
#define CONFIG_SGL_DEBUG                        0
#define CONFIG_SGL_LOG_COLOR                    0
#define CONFIG_SGL_LOG_LEVEL                    5
#define CONFIG_SGL_OBJ_USE_NAME                 0
#define CONFIG_SGL_FONT_COMPRESSED              0
#define CONFIG_SGL_BOOT_LOGO                    0
#define CONFIG_SGL_THEME_DARK                   0
#define CONFIG_SGL_THEME_LIGHT                  0
#define CONFIG_SGL_THEME_DEFAULT                1
#define CONFIG_SGL_HEAP_ALGO                    bump
#define CONFIG_SGL_HEAP_MEMORY_SIZE             6144
#define CONFIG_SGL_FONT_SONG23                  0
#define CONFIG_SGL_FONT_CONSOLAS14              1
#define CONFIG_SGL_FONT_CONSOLAS23              0
#define CONFIG_SGL_FONT_CONSOLAS24              0
#define CONFIG_SGL_FONT_CONSOLAS32              0
#define CONFIG_SGL_FONT_CONSOLAS24_COMPRESS     0
#define CONFIG_SGL_MONITOR_TRACE                0
#define CONFIG_SGL_PIXMAP_BILINEAR_INTERP       0
#define CONFIG_SGL_FOCUSED_WIDTH                0
#define CONFIG_SGL_FLASH_FONT                   0

#endif
