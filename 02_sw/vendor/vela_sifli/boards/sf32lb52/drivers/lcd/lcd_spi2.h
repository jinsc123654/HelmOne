/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/lcd/lcd_spi2.h
 *
 * Dedicated SPI2 for ST7789 (PA37-PA42). Not part of sifli_spi.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __LCD_SPI2_H__
#define __LCD_SPI2_H__

#include <nuttx/spi/spi.h>
#include <stddef.h>
#include <stdint.h>

struct spi_dev_s *lcd_spi2_dev_initialize(void);

/* Push RGB565 pixels (16-bit SPI). CS selected, DC=data, RAMWR already sent. */

int lcd_spi2_tx_pixels(FAR struct spi_dev_s *dev,
                       FAR const void *buffer, size_t npixels);

/* Full-screen fb flush: 16-bit SPI TX with optional DMA (window preset). */

int lcd_spi2_fb_flush(FAR struct spi_dev_s *dev,
                      FAR const uint8_t *fbmem,
                      uint16_t xres, uint16_t yres,
                      size_t stride);

/** Unblock ui_flush worker stuck in PUTAREA/DMA wait (recover, worker alive). */
void lcd_spi2_abort_active_xfer(void);

/** Tear down and re-init SPI2+DMA after worker stopped (recover, pipe restart). */
void lcd_spi2_restart_xfer_path(void);

#endif /* __LCD_SPI2_H__ */
