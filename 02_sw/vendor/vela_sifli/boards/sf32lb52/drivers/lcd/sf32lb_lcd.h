

#ifndef __INCLUDE_SF32LB_LCD_H
#define __INCLUDE_SF32LB_LCD_H

#include <stdint.h>
#include <nuttx/arch.h>
#include "bf0_hal.h"
#include <bf0_hal_lcdc.h>
#include "drv_io.h"
#include <debug.h>

/****************************************    Adapter for LCD driver      **********************************************/
typedef enum
{
    LCD_ROTATE_0_DEGREE = 0,
    LCD_ROTATE_90_DEGREE = 90,   /*Clock wise 90 degrees*/
    LCD_ROTATE_180_DEGREE = 180, /*Clock wise 180 degrees*/
    LCD_ROTATE_270_DEGREE = 270, /*Clock wise 270 degrees*/
} LCD_DrvRotateTypeDef;

typedef struct
{
    void (*Init)(LCDC_HandleTypeDef *hlcdc);
    uint32_t (*ReadID)(LCDC_HandleTypeDef *hlcdc);
    void (*DisplayOn)(LCDC_HandleTypeDef *hlcdc);
    void (*DisplayOff)(LCDC_HandleTypeDef *hlcdc);

    void (*SetRegion)(LCDC_HandleTypeDef *hlcdc, uint16_t, uint16_t, uint16_t, uint16_t);
    void (*WritePixel)(LCDC_HandleTypeDef *hlcdc, uint16_t, uint16_t, const uint8_t *);
    void (*WriteMultiplePixels)(LCDC_HandleTypeDef *hlcdc, const uint8_t *, uint16_t, uint16_t, uint16_t, uint16_t);
    uint32_t (*ReadPixel)(LCDC_HandleTypeDef *hlcdc, uint16_t, uint16_t);

    /*optional operation*/
    void (*SetColorMode)(LCDC_HandleTypeDef *hlcdc, uint16_t);
    void (*SetBrightness)(LCDC_HandleTypeDef *hlcdc, uint8_t);       // in percentage
    void (*IdleModeOn)(LCDC_HandleTypeDef *hlcdc);
    void (*IdleModeOff)(LCDC_HandleTypeDef *hlcdc);
    void (*Rotate)(LCDC_HandleTypeDef *hlcdc, LCD_DrvRotateTypeDef);
} LCD_DrvOpsDef;


#ifndef locate_code
#define locate_code(sec)  __attribute__((section(sec)))
#endif

#define LCD_DRV_NAME_MAX_LEN 16

typedef struct
{
    char name[LCD_DRV_NAME_MAX_LEN];
    uint32_t id;
    const LCDC_InitTypeDef *p_init_cfg;
    const LCD_DrvOpsDef *p_ops;
    uint16_t lcd_horizonal_res;
    uint16_t lcd_vertical_res;
    uint16_t ic_max_horizonal_res;
    uint16_t ic_max_vertical_res;
    uint16_t pixel_align;
} lcd_drv_desc_t;

#define LCD_DRIVER_EXPORT(name, id, init_cfg, dev_ops, ic_max_hor_res, ic_max_ver_res, pixel_align)         \
    __attribute__((used)) const lcd_drv_desc_t __lcddriver_##name                       \
    locate_code("LcdDriverDescTab")  =                                                \
    {                                                                            \
        #name,        \
        id,           \
        init_cfg,     \
        dev_ops,      \
        CONFIG_LCD_HOR_RES_MAX,       \
        CONFIG_LCD_VER_RES_MAX,       \
        ic_max_hor_res,        \
        ic_max_ver_res,        \
        pixel_align           \
    }

#define LCD_DRIVER_DELAY_MS(ms) up_mdelay(ms)
typedef enum
{
    LCD_STATUS_NONE = 0,
    LCD_STATUS_NOT_FIND_LCD,
    LCD_STATUS_INITIALIZED,     /*Finded LCD*/
    LCD_STATUS_DISPLAY_ON,
    LCD_STATUS_DISPLAY_OFF,
    LCD_STATUS_DISPLAY_TIMEOUT,
    LCD_STATUS_IDLE_MODE,
} LCD_DrvStatusTypeDef;

/** HCLK 变化后按 Init.freq 重算 LCDC SPI 分频（不复位面板）。 */
void sf32lb_lcd_reclock(void);

#endif /* __INCLUDE_SF32LB_LCD_H */

