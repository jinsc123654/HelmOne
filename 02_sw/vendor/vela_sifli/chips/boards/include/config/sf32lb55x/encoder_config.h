/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ENCODER_CONFIG_H__
#define __ENCODER_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BSP_USING_ENCODER_GPTIM1
#define ENCODER_GPTIM1_CONFIG                             \
    {                                               \
        .tim_handle.Instance     = GPTIM1,          \
        .tim_handle.core         = CORE_ID_HCPU,   \
        .name                    = "encoder1",      \
        .encoder_init.EncoderMode         = GPT_ENCODERMODE_TI3, \
        .encoder_init.IC1Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC1Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC1Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC1Filter           = 0xF,               \
        .encoder_init.IC2Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC2Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC2Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC2Filter           = 0xF                \
                                               \
    }
#endif /* BSP_USING_ENCODER_GPTIM1 */

#ifdef BSP_USING_ENCODER_GPTIM2
#define ENCODER_GPTIM2_CONFIG                             \
    {                                               \
        .tim_handle.Instance     = GPTIM2,          \
        .tim_handle.core         = CORE_ID_HCPU,   \
        .name                    = "encoder2",      \
        .encoder_init.EncoderMode         = GPT_ENCODERMODE_TI3, \
        .encoder_init.IC1Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC1Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC1Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC1Filter           = 0xF,               \
        .encoder_init.IC2Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC2Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC2Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC2Filter           = 0xF                \
                                               \
    }
#endif /* BSP_USING_ENCODER_GPTIM2 */

#ifdef BSP_USING_ENCODER_GPTIM3
#define ENCODER_GPTIM3_CONFIG                             \
    {                                               \
        .tim_handle.Instance     = GPTIM3,          \
        .tim_handle.core         = CORE_ID_LCPU,   \
        .name                    = "encoder3",      \
        .encoder_init.EncoderMode         = GPT_ENCODERMODE_TI3, \
        .encoder_init.IC1Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC1Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC1Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC1Filter           = 0xF,               \
        .encoder_init.IC2Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC2Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC2Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC2Filter           = 0xF                \
                                               \
    }
#endif /* BSP_USING_ENCODER_GPTIM3 */

#ifdef BSP_USING_ENCODER_GPTIM4
#define ENCODER_GPTIM4_CONFIG                             \
    {                                               \
        .tim_handle.Instance     = GPTIM4,          \
        .tim_handle.core         = CORE_ID_LCPU,   \
        .name                    = "encoder4",      \
        .encoder_init.EncoderMode         = GPT_ENCODERMODE_TI3, \
        .encoder_init.IC1Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC1Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC1Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC1Filter           = 0xF,               \
        .encoder_init.IC2Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC2Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC2Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC2Filter           = 0xF                \
                                               \
    }
#endif /* BSP_USING_ENCODER_GPTIM4 */

#ifdef BSP_USING_ENCODER_GPTIM5
#define ENCODER_GPTIM5_CONFIG                             \
    {                                               \
        .tim_handle.Instance     = GPTIM5,          \
        .tim_handle.core         = CORE_ID_LCPU,   \
        .name                    = "encoder5",      \
        .encoder_init.EncoderMode         = GPT_ENCODERMODE_TI3, \
        .encoder_init.IC1Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC1Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC1Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC1Filter           = 0xF,               \
        .encoder_init.IC2Polarity         = GPT_ICPOLARITY_RISING, \
        .encoder_init.IC2Selection        = GPT_ICSELECTION_DIRECTTI, \
        .encoder_init.IC2Prescaler        = GPT_ICPSC_DIV1,  \
        .encoder_init.IC2Filter           = 0xF                \
                                               \
    }
#endif /* BSP_USING_ENCODER_GPTIM5 */

#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_CONFIG_H__ */

