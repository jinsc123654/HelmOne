/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __BF0_HAL_EFUSE_H
#define __BF0_HAL_EFUSE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "bf0_hal_def.h"

/** @addtogroup EFUSE
  * @ingroup BF0_HAL_Driver
  * @{
  */

/** EFUSE bank size in bytes */
#define HAL_EFUSE_BANK_SIZE         32
/** EFUSE bank number */
#define HAL_EFUSE_BANK_NUM          (4)


/**
 * @brief  Init Efuse controller
 * @retval void
 */
HAL_StatusTypeDef HAL_EFUSE_Init(void);

/**
 * @brief  Configure bypass
 * @param enabled true: enable bypass, false: disable bypass
 * @retval void
 */
void HAL_EFUSE_ConfigBypass(bool enabled);

/**
 * @brief  Write data to efuse starting from bit_offset
 * @param bit_offset bit_offset in efuse, must be 32bits aligned, bank0: 0~255, bank1: 256~511
 * @param data point to the data to be written
 * @param size data size in byte, must be multiple of 4bytes and written data cannot cross bank boundary
 * @retval size successfully written
 */
int32_t HAL_EFUSE_Write(uint16_t bit_offset, uint8_t *data, int32_t size);


/**
 * @brief  Read data to efuse starting from bit_offset
 * @param bit_offset bit_offset in efuse, must be 32bits aligned, bank0: 0~255, bank1: 256~511
 * @param data point to buffer to save read data
 * @param size data size in byte, must be multiple of 4bytes and read data cannot cross bank boundary
 * @retval size successfully read
 */
int32_t HAL_EFUSE_Read(uint16_t bit_offset, uint8_t *data, int size);


#ifdef __cplusplus
}
#endif

///@}   EFUSE

#endif /* __BF0_HAL_EFUSE_H */