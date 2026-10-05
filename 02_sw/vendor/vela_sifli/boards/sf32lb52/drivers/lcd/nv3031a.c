/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/nv3031a.c
 *
 * NV3031A-AI 240x320 RGB565, 4-bit QSPI (QAD-SPI) panel driver.
 *
 * Command/pixel opcodes match the vendor TFT/lcd.c demo:
 *   cmd  : 0x02 00 <reg> 00   (1-line)
 *   pixel: 0x32 00 2C   00   (4-line)
 *
 * Init sequence from vendor init_9c01(). TE (0x35) is omitted — this board
 * does not wire LCDC TE.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <sfconfig.h>
#include "string.h"
#include "sf32lb_lcd.h"
#include <syslog.h>

#define RTGRAPHIC_PIXEL_FORMAT_RGB565  LCDC_PIXEL_FORMAT_RGB565
#define RTGRAPHIC_PIXEL_FORMAT_RGB888  LCDC_PIXEL_FORMAT_RGB888

#define LCD_HOR_RES_MAX  CONFIG_LCD_HOR_RES_MAX
#define LCD_VER_RES_MAX  CONFIG_LCD_VER_RES_MAX

#define LCD_ID                  0x3031a
#define LCD_PIXEL_WIDTH         (LCD_HOR_RES_MAX)
#define LCD_PIXEL_HEIGHT        (LCD_VER_RES_MAX)

#define REG_SLEEP_IN            0x10
#define REG_SLEEP_OUT           0x11
#define REG_DISPLAY_OFF         0x28
#define REG_DISPLAY_ON          0x29
#define REG_WRITE_RAM           0x2C
#define REG_CASET               0x2A
#define REG_RASET               0x2B
#define REG_TEARING_EFFECT_OFF  0x34
#define REG_MADCTL              0x36
#define REG_COLOR_MODE          0x3A
#define REG_CONTINUE_WRITE_RAM  0x3C

#define QAD_SPI_ITF             LCDC_INTF_SPI_DCX_4DATA

static LCDC_InitTypeDef lcdc_int_cfg_qadspi =
{
  .lcd_itf = QAD_SPI_ITF,
  /* ⚠ **24 MHz 是实测过的上限，别再往上试。**
   *
   * 面板手册给的名义上限：`NV3031A-AI Datasheet V1.6` §9.3.3
   * 「4-wire QSPI system」第 203 页，`TSCYCW` Serial Clock Cycle (Write) ≥ **27 ns
   * ⇒ 37.04 MHz**（读的 `TSCYCR` ≥ 155 ns 只 6.45 MHz，但我们只写 GRAM）。
   * 注意是 **4 线 QSPI** 那一节；§9.3.1/§9.3.2 的 15.15 ns（66 MHz）是单线 SPI 的。
   *
   * **2026-10-01 实测：改成 36 MHz 后开机花屏、画面不动。** 按 `SetFreq` 的
   * `ceil(HCLK/freq)` 算，36 会给出 240档÷7=34.3 MHz、144档÷4=36.0 MHz，
   * 两个都在 27 ns 之内 —— 但仍然坏了。**⇒ 真实上限低于名义值**，多半是
   * SoC 侧的输出时序（`TSDS` ≥7.575 ns 建立 / `TSDH` ≥1.515 ns 保持 相对 SCL）
   * 在高频下先掉出去，而不是面板本身接不住。所以 24 就这么留着。
   *
   * 24 这个值本身也有讲究：分频必须是整数，而 24 是"能同时整除 144 与 240"
   * 的最大值（gcd(144,240)=48，其 ≤37 的最大约数是 24）⇒ 换档时频率不跳。 */
  .freq = 24000000,
  .color_mode = LCDC_PIXEL_FORMAT_RGB565,
  .cfg =
    {
      .spi =
        {
          .dummy_clock = 0,
          .syn_mode = HAL_LCDC_SYNC_DISABLE,
          .vsyn_polarity = 0,
          .vsyn_delay_us = 0,
          .hsyn_num = 0,
        },
    },
};

static LCDC_InitTypeDef lcdc_int_cfg;

static void LCD_SetRegion(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos0,
                          uint16_t Ypos0, uint16_t Xpos1, uint16_t Ypos1);
static void LCD_WriteReg(LCDC_HandleTypeDef *hlcdc, uint16_t LCD_Reg,
                         uint8_t *Parameters, uint32_t NbParameters)
{
  uint32_t cmd;

  if ((REG_WRITE_RAM == LCD_Reg) || (REG_CONTINUE_WRITE_RAM == LCD_Reg))
    {
      cmd = (0x32u << 24) | ((uint32_t)LCD_Reg << 8);
    }
  else
    {
      cmd = (0x02u << 24) | ((uint32_t)LCD_Reg << 8);
    }

  HAL_LCDC_WriteU32Reg(hlcdc, cmd, Parameters, NbParameters);
}

static void nv_wr(LCDC_HandleTypeDef *hlcdc, uint16_t reg,
                  uint8_t *data, uint32_t n)
{
  LCD_WriteReg(hlcdc, reg, data, n);
}

static void LCD_Drv_Init(LCDC_HandleTypeDef *hlcdc)
{
#ifdef NV3031A_APP_REINIT_PANEL
  uint8_t p[8];
#endif

  memcpy(&hlcdc->Init, &lcdc_int_cfg, sizeof(LCDC_InitTypeDef));
  if (HAL_LCDC_Init(hlcdc) != HAL_OK)
    {
      lcdwarn("[nv3031a] HAL_LCDC_Init failed");
      return;
    }

#ifndef NV3031A_APP_REINIT_PANEL
  /* 沿用 2SFBL 已 init 的面板；完整复位/刷黑见 #ifdef NV3031A_APP_REINIT_PANEL。 */
  HAL_LCDC_LayerReset(hlcdc, HAL_LCDC_LAYER_DEFAULT);
  HAL_LCDC_LayerSetFormat(hlcdc, HAL_LCDC_LAYER_DEFAULT,
                          LCDC_PIXEL_FORMAT_RGB565);
  HAL_LCDC_LayerEnable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
  syslog(LOG_INFO, "[nv3031a] keep 2SFBL panel (no reset)\n");
  return;
#else

  /* Vendor reset: H 20ms, L 220ms, H 120ms. */
  BSP_LCD_Reset(1);
  LCD_DRIVER_DELAY_MS(20);
  BSP_LCD_Reset(0);
  LCD_DRIVER_DELAY_MS(220);
  BSP_LCD_Reset(1);
  LCD_DRIVER_DELAY_MS(120);

  p[0] = 0x06;
  p[1] = 0x08;
  nv_wr(hlcdc, 0xFD, p, 2);
  p[0] = 0x07;
  p[1] = 0x07;
  nv_wr(hlcdc, 0x61, p, 2);
  p[0] = 0x70;
  nv_wr(hlcdc, 0x73, p, 1);
  p[0] = 0x00;
  nv_wr(hlcdc, 0x73, p, 1);
  p[0] = 0x00;
  p[1] = 0x44;
  p[2] = 0x40;
  nv_wr(hlcdc, 0x62, p, 3);
  p[0] = 0x41;
  p[1] = 0x07;
  p[2] = 0x12;
  p[3] = 0x12;
  nv_wr(hlcdc, 0x63, p, 4);
  p[0] = 0x37;
  nv_wr(hlcdc, 0x64, p, 1);
  p[0] = 0x09;
  p[1] = 0x10;
  p[2] = 0x21;
  nv_wr(hlcdc, 0x65, p, 3);
  nv_wr(hlcdc, 0x66, p, 3);
  p[0] = 0x21;
  p[1] = 0x40;
  nv_wr(hlcdc, 0x67, p, 2);
  p[0] = 0x60;
  p[1] = 0x60;
  p[2] = 0x3C;
  p[3] = 0x1C;
  nv_wr(hlcdc, 0x68, p, 4);
  p[0] = 0x0F;
  p[1] = 0x02;
  p[2] = 0x03;
  nv_wr(hlcdc, 0xB1, p, 3);
  p[0] = 0x01;
  nv_wr(hlcdc, 0xB4, p, 1);
  p[0] = 0x02;
  p[1] = 0x02;
  p[2] = 0x0A;
  p[3] = 0x14;
  nv_wr(hlcdc, 0xB5, p, 4);
  p[0] = 0x44;
  p[1] = 0x01;
  p[2] = 0x9F;
  p[3] = 0x00;
  p[4] = 0x02;
  nv_wr(hlcdc, 0xB6, p, 5);
  p[0] = 0x11;
  nv_wr(hlcdc, 0xDF, p, 1);

  p[0] = 0x04;
  p[1] = 0x04;
  p[2] = 0x0C;
  p[3] = 0x0E;
  p[4] = 0x10;
  p[5] = 0x0F;
  p[6] = 0x13;
  p[7] = 0x17;
  nv_wr(hlcdc, 0xE0, p, 8);
  p[0] = 0x17;
  p[1] = 0x13;
  p[2] = 0x0D;
  p[3] = 0x0B;
  p[4] = 0x0F;
  p[5] = 0x0C;
  p[6] = 0x05;
  p[7] = 0x05;
  nv_wr(hlcdc, 0xE3, p, 8);
  p[0] = 0x0A;
  p[1] = 0x68;
  nv_wr(hlcdc, 0xE1, p, 2);
  p[0] = 0x68;
  p[1] = 0x1E;
  nv_wr(hlcdc, 0xE4, p, 2);
  p[0] = 0x05;
  p[1] = 0x06;
  p[2] = 0x05;
  p[3] = 0x33;
  p[4] = 0x34;
  p[5] = 0x3A;
  nv_wr(hlcdc, 0xE2, p, 6);
  p[0] = 0x3A;
  p[1] = 0x35;
  p[2] = 0x32;
  p[3] = 0x05;
  p[4] = 0x06;
  p[5] = 0x05;
  nv_wr(hlcdc, 0xE5, p, 6);
  p[0] = 0x00;
  p[1] = 0xFF;
  nv_wr(hlcdc, 0xE6, p, 2);
  p[0] = 0x01;
  p[1] = 0x04;
  p[2] = 0x03;
  p[3] = 0x03;
  p[4] = 0x00;
  p[5] = 0x12;
  nv_wr(hlcdc, 0xE7, p, 6);
  p[0] = 0x00;
  p[1] = 0x70;
  p[2] = 0x00;
  nv_wr(hlcdc, 0xE8, p, 3);
  p[0] = 0x52;
  nv_wr(hlcdc, 0xEC, p, 1);
  p[0] = 0x01;
  p[1] = 0xAA;
  p[2] = 0xAB;
  nv_wr(hlcdc, 0xF1, p, 3);
  p[0] = 0x01;
  p[1] = 0x30;
  p[2] = 0x00;
  p[3] = 0x00;
  nv_wr(hlcdc, 0xF6, p, 4);
  p[0] = 0xFA;
  p[1] = 0xFC;
  nv_wr(hlcdc, 0xFD, p, 2);

  p[0] = 0x55;
  nv_wr(hlcdc, REG_COLOR_MODE, p, 1);
  LCD_WriteReg(hlcdc, REG_TEARING_EFFECT_OFF, NULL, 0);
  p[0] = 0x00;
  nv_wr(hlcdc, REG_MADCTL, p, 1);

  p[0] = 0x00;
  nv_wr(hlcdc, REG_SLEEP_OUT, p, 1);
  LCD_DRIVER_DELAY_MS(120);
  p[0] = 0x00;
  nv_wr(hlcdc, REG_DISPLAY_ON, p, 1);
  LCD_DRIVER_DELAY_MS(300);

  /* Fill black with LCDC background (no TE, polling). */
  HAL_LCDC_Next_Frame_TE(hlcdc, 0);
  LCD_SetRegion(hlcdc, 0, 0,
                (uint16_t)(LCD_PIXEL_WIDTH - 1),
                (uint16_t)(LCD_PIXEL_HEIGHT - 1));
  HAL_LCDC_LayerSetFormat(hlcdc, HAL_LCDC_LAYER_DEFAULT,
                          LCDC_PIXEL_FORMAT_RGB565);
  HAL_LCDC_LayerDisable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
  HAL_LCDC_SetBgColor(hlcdc, 0, 0, 0);
  HAL_LCDC_SendLayerData2Reg(hlcdc,
                             ((0x32u << 24) | (REG_WRITE_RAM << 8)), 4);
  HAL_LCDC_LayerEnable(hlcdc, HAL_LCDC_LAYER_DEFAULT);

  /* Backlight stays off until the first real PUTAREA (UI frame). */

  lcdinfo("[nv3031a] init done 240x320 QSPI RGB565\n");
#endif
}

static void LCD_Init(LCDC_HandleTypeDef *hlcdc)
{
  memcpy(&lcdc_int_cfg, &lcdc_int_cfg_qadspi, sizeof(lcdc_int_cfg));
  LCD_Drv_Init(hlcdc);
}

static uint32_t LCD_ReadID(LCDC_HandleTypeDef *hlcdc)
{
  UNUSED(hlcdc);
  return LCD_ID;
}

static void LCD_DisplayOn(LCDC_HandleTypeDef *hlcdc)
{
  uint8_t z = 0;

  nv_wr(hlcdc, REG_DISPLAY_ON, &z, 1);
}

static void LCD_DisplayOff(LCDC_HandleTypeDef *hlcdc)
{
  BSP_LCD_BL_Set(0);
  LCD_WriteReg(hlcdc, REG_DISPLAY_OFF, NULL, 0);
}

static void LCD_SetRegion(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos0,
                          uint16_t Ypos0, uint16_t Xpos1, uint16_t Ypos1)
{
  uint8_t parameter[4];

  HAL_LCDC_SetROIArea(hlcdc, Xpos0, Ypos0, Xpos1, Ypos1);

  parameter[0] = (uint8_t)(Xpos0 >> 8);
  parameter[1] = (uint8_t)(Xpos0 & 0xFF);
  parameter[2] = (uint8_t)(Xpos1 >> 8);
  parameter[3] = (uint8_t)(Xpos1 & 0xFF);
  LCD_WriteReg(hlcdc, REG_CASET, parameter, 4);

  parameter[0] = (uint8_t)(Ypos0 >> 8);
  parameter[1] = (uint8_t)(Ypos0 & 0xFF);
  parameter[2] = (uint8_t)(Ypos1 >> 8);
  parameter[3] = (uint8_t)(Ypos1 & 0xFF);
  LCD_WriteReg(hlcdc, REG_RASET, parameter, 4);
}

static void LCD_WritePixel(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos,
                           uint16_t Ypos, const uint8_t *RGBCode)
{
  LCD_SetRegion(hlcdc, Xpos, Ypos, Xpos, Ypos);
  LCD_WriteReg(hlcdc, REG_WRITE_RAM, (uint8_t *)RGBCode, 2);
}

static void LCD_WriteMultiplePixels(LCDC_HandleTypeDef *hlcdc,
                                    const uint8_t *RGBCode,
                                    uint16_t Xpos0, uint16_t Ypos0,
                                    uint16_t Xpos1, uint16_t Ypos1)
{
  HAL_LCDC_LayerSetData(hlcdc, HAL_LCDC_LAYER_DEFAULT, (uint8_t *)RGBCode,
                        Xpos0, Ypos0, Xpos1, Ypos1);
  HAL_LCDC_Next_Frame_TE(hlcdc, 0);
  HAL_LCDC_SendLayerData2Reg_IT(hlcdc,
                                ((0x32u << 24) | (REG_WRITE_RAM << 8)), 4);
}

static uint32_t LCD_ReadPixel(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos,
                              uint16_t Ypos)
{
  UNUSED(hlcdc);
  UNUSED(Xpos);
  UNUSED(Ypos);
  return 0;
}

static void LCD_SetColorMode(LCDC_HandleTypeDef *hlcdc, uint16_t color_mode)
{
  uint8_t parameter[1];

  if (color_mode != RTGRAPHIC_PIXEL_FORMAT_RGB565)
    {
      return;
    }

  parameter[0] = 0x55;
  lcdc_int_cfg.color_mode = LCDC_PIXEL_FORMAT_RGB565;
  LCD_WriteReg(hlcdc, REG_COLOR_MODE, parameter, 1);
  HAL_LCDC_SetOutFormat(hlcdc, lcdc_int_cfg.color_mode);
}

static void LCD_SetBrightness(LCDC_HandleTypeDef *hlcdc, uint8_t br)
{
  UNUSED(hlcdc);
  BSP_LCD_BL_SetPct(br);
}

static void LCD_IdleModeOn(LCDC_HandleTypeDef *hlcdc)
{
  LCD_WriteReg(hlcdc, REG_DISPLAY_OFF, NULL, 0);
  LCD_DRIVER_DELAY_MS(20);
  LCD_WriteReg(hlcdc, REG_SLEEP_IN, NULL, 0);
}

static void LCD_IdleModeOff(LCDC_HandleTypeDef *hlcdc)
{
  uint8_t z = 0;

  nv_wr(hlcdc, REG_SLEEP_OUT, &z, 1);
  LCD_DRIVER_DELAY_MS(120);
  nv_wr(hlcdc, REG_DISPLAY_ON, &z, 1);
}

static const LCD_DrvOpsDef NV3031A_drv =
{
  LCD_Init,
  LCD_ReadID,
  LCD_DisplayOn,
  LCD_DisplayOff,
  LCD_SetRegion,
  LCD_WritePixel,
  LCD_WriteMultiplePixels,
  LCD_ReadPixel,
  LCD_SetColorMode,
  LCD_SetBrightness,
  LCD_IdleModeOn,
  LCD_IdleModeOff,
};

LCD_DRIVER_EXPORT(nv3031a, LCD_ID, &lcdc_int_cfg_qadspi,
                  &NV3031A_drv,
                  LCD_PIXEL_WIDTH,
                  LCD_PIXEL_HEIGHT,
                  2);
