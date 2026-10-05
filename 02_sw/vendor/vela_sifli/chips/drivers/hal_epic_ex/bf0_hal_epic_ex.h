/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BF0_HAL_EPIC_EX_H
#define BF0_HAL_EPIC_EX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bf0_hal.h"

void HAL_EPIC_Adv_Log(uint32_t level);

HAL_StatusTypeDef HAL_EPIC_Adv(EPIC_HandleTypeDef *hepic,
                               EPIC_LayerConfigTypeDef *input_layers,
                               uint8_t input_layer_num,
                               EPIC_LayerConfigTypeDef *output_layer);

#ifdef __cplusplus
}
#endif

#endif /* BF0_HAL_EPIC_EX_H */



