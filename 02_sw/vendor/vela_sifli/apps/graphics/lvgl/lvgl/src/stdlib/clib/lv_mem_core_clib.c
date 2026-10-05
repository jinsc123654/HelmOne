/**
 * @file lv_mem_core_clib.c
 */

/*********************
 *      INCLUDES
 *********************/
#include "../lv_mem.h"
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CLIB
#include "../../stdlib/lv_mem.h"
#include <stdlib.h>

#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

void * board_malloc_psram(size_t size);
void * board_realloc_psram(void * ptr, size_t size);
void board_mem_free(void * ptr);
int board_psram_heap_init(void);

#ifdef __cplusplus
}
#endif
#endif /* CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC */

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/
/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_mem_init(void)
{
#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
    (void)board_psram_heap_init();
#endif
    return; /*Nothing to init*/
}

void lv_mem_deinit(void)
{
    return; /*Nothing to deinit*/

}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    /*Not supported*/
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    /*Not supported*/
    LV_UNUSED(pool);
    return;
}

void * lv_malloc_core(size_t size)
{
#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
    return board_malloc_psram(size);
#else
    return malloc(size);
#endif
}

void * lv_realloc_core(void * p, size_t new_size)
{
#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
    return board_realloc_psram(p, new_size);
#else
    return realloc(p, new_size);
#endif
}

void lv_free_core(void * p)
{
#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
    board_mem_free(p);
#else
    free(p);
#endif
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    /*Not supported*/
    LV_UNUSED(mon_p);
    return;
}

lv_result_t lv_mem_test_core(void)
{
    /*Not supported*/
    return LV_RESULT_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_STDLIB_CLIB*/
