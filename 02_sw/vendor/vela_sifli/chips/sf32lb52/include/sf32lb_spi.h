/****************************************************************************
 * vendor/sifli/chips/sf32lb52/include/sf32lb_spi.h
 *
 * SF32LB SPI driver header for NuttX
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __SF32LB_SPI_H__
#define __SF32LB_SPI_H__

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <nuttx/spi/spi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SPI instance index enumeration */

enum
{
#ifdef CONFIG_BSP_USING_SPI1
    SPI1_INDEX,
#endif
#ifdef CONFIG_BSP_USING_SPI2
    SPI2_INDEX,
#endif
    SPI_MAX
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: sifli_spibus_initialize
 *
 * Description:
 *   Initialize one SPI bus
 *
 * Input Parameters:
 *   port - SPI port number (0-based index into SPI_MAX)
 *
 * Returned Value:
 *   A pointer to the SPI device structure on success; NULL on failure.
 *
 ****************************************************************************/

struct spi_dev_s *sifli_spibus_initialize(int port);

/****************************************************************************
 * Name: sifli_spibus_uninitialize
 *
 * Description:
 *   Uninitialize an SPI bus
 *
 * Input Parameters:
 *   dev - SPI device structure to uninitialize
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int sifli_spibus_uninitialize(struct spi_dev_s *dev);

/****************************************************************************
 * Name: sf32lb_spi_select
 * Name: sf32lb_spi_status
 *
 * Description:
 *   Board-specific chip select and status routines. These must be provided
 *   by the board-level logic. They are called from the SPI driver select
 *   and status methods.
 *
 ****************************************************************************/

void sf32lb_spi_select(struct spi_dev_s *dev, uint32_t devid,
                       bool selected);
uint8_t sf32lb_spi_status(struct spi_dev_s *dev, uint32_t devid);

#endif /* __SF32LB_SPI_H__ */
