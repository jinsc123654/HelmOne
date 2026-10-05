/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DMA_CONFIG_H__
#define __DMA_CONFIG_H__

#include <rtconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

/*************************************DMA1 ***************************************/
#define FLASH1_DMA_REQUEST                     DMA_REQUEST_0
#define I2C4_DMA_REQUEST                       DMA_REQUEST_1 //Reuse with MPI2's request
#define FLASH2_DMA_REQUEST                     DMA_REQUEST_1
#define FLASH3_DMA_REQUEST                     DMA_REQUEST_2
#define FLASH4_DMA_REQUEST                     DMA_REQUEST_3
#define UART1_TX_DMA_REQUEST                   DMA_REQUEST_4
#define UART1_RX_DMA_REQUEST                   DMA_REQUEST_5
#define UART2_TX_DMA_REQUEST                   DMA_REQUEST_6
#define UART2_RX_DMA_REQUEST                   DMA_REQUEST_7
#define GPTIM1_UPDATE_DMA_REQUEST              DMA_REQUEST_8
#define GPTIM1_TRIGGER_DMA_REQUEST             DMA_REQUEST_9
#define GPTIM1_CC1_DMA_REQUEST                 DMA_REQUEST_10
#define GPTIM1_CC2_DMA_REQUEST                 DMA_REQUEST_11
#define PWM2_CC2_DMA_REQUEST                   DMA_REQUEST_11
#define GPTIM1_CC3_DMA_REQUEST                 DMA_REQUEST_12
#define GPTIM1_CC4_DMA_REQUEST                 DMA_REQUEST_13
#define BTIM1_DMA_REQUEST                      DMA_REQUEST_14
#define BTIM2_DMA_REQUEST                      DMA_REQUEST_15
#define ATIM1_UPDATE_DMA_REQUEST               DMA_REQUEST_16
#define ATIM1_TRIGGER_DMA_REQUEST              DMA_REQUEST_17
#define PWMA1_CC1_DMA_REQUEST                  DMA_REQUEST_18//atime1_cc1
#define PWMA1_CC2_DMA_REQUEST                  DMA_REQUEST_19//atime1_cc2
#define PWMA1_CC3_DMA_REQUEST                  DMA_REQUEST_20//atime1_cc3
#define PWMA1_CC4_DMA_REQUEST                  DMA_REQUEST_21//atime1_cc4
#define I2C1_DMA_REQUEST                       DMA_REQUEST_22
#define I2C2_DMA_REQUEST                       DMA_REQUEST_23
#define I2C3_DMA_REQUEST                       DMA_REQUEST_24
#define ATIM1_COM_DMA_REQUEST                  DMA_REQUEST_25
#define UART3_TX_DMA_REQUEST                   DMA_REQUEST_26
#define UART3_RX_DMA_REQUEST                   DMA_REQUEST_27
#define SPI1_TX_DMA_REQUEST                    DMA_REQUEST_28
#define SPI1_RX_DMA_REQUEST                    DMA_REQUEST_29
#define SPI2_TX_DMA_REQUEST                    DMA_REQUEST_30
#define SPI2_RX_DMA_REQUEST                    DMA_REQUEST_31

/* DMA1 channel1 */
#define FLASH1_IRQHandler              DMAC1_CH1_IRQHandler
#define FLASH1_DMA_IRQ_PRIO            0
#define FLASH1_DMA_INSTANCE            DMA1_Channel1
#define FLASH1_DMA_IRQ                 DMAC1_CH1_IRQn

/* DMA1 channel2 */
#define FLASH2_IRQHandler              DMAC1_CH2_IRQHandler
#define FLASH2_DMA_IRQ_PRIO            0
#define FLASH2_DMA_INSTANCE            DMA1_Channel2
#define FLASH2_DMA_IRQ                 DMAC1_CH2_IRQn

#define I2C3_DMA_IRQHandler              DMAC1_CH2_IRQHandler
#define I2C3_DMA_IRQ_PRIO                1
#define I2C3_DMA_INSTANCE                DMA1_Channel2
#define I2C3_DMA_IRQ                     DMAC1_CH2_IRQn

#define I2C4_DMA_IRQHandler              DMAC1_CH2_IRQHandler
#define I2C4_DMA_IRQ_PRIO                1
#define I2C4_DMA_INSTANCE                DMA1_Channel2
#define I2C4_DMA_IRQ                     DMAC1_CH2_IRQn

#define PWM2_CC2_DMA_IRQHandler              DMAC1_CH2_IRQHandler
#define PWM2_CC2_DMA_IRQ_PRIO                1
#define PWM2_CC2_DMA_INSTANCE                DMA1_Channel3
#define PWM2_CC2_DMA_IRQ                     DMAC1_CH2_IRQn
#define PWM2_CC2_DMA_PDATAALIGN                       DMA_PDATAALIGN_HALFWORD
#define PWM2_CC2_DMA_MDATAALIGN                       DMA_MDATAALIGN_HALFWORD

/* DMA1 channel3 */
#define FLASH3_IRQHandler              DMAC1_CH3_IRQHandler
#define FLASH3_DMA_IRQ_PRIO            0
#define FLASH3_DMA_INSTANCE            DMA1_Channel3
#define FLASH3_DMA_IRQ                 DMAC1_CH3_IRQn

/* DMA1 channel3 */
#define FLASH4_IRQHandler              DMAC1_CH4_IRQHandler
#define FLASH4_DMA_IRQ_PRIO            0
#define FLASH4_DMA_INSTANCE            DMA1_Channel4
#define FLASH4_DMA_IRQ                 DMAC1_CH4_IRQn

/* DMA1 channel3 */
//TODO: need to be removed, not used by A0
#define USBD_RX_IRQHandler              DMAC1_CH3_IRQHandler
#define USBD_RX_DMA_IRQ_PRIO            0
#define USBD_RX_DMA_INSTANCE            DMA1_Channel3
#define USBD_RX_DMA_IRQ                 DMAC1_CH3_IRQn

#define SPI1_DMA_RX_IRQHandler         DMAC1_CH3_IRQHandler
#define SPI1_RX_DMA_IRQ_PRIO           0
#define SPI1_RX_DMA_INSTANCE           DMA1_Channel3
#define SPI1_RX_DMA_IRQ                DMAC1_CH3_IRQn

/* DMA1 channel4 */
//TODO: need to be removed, not used by A0
#define USBD_TX_IRQHandler              DMAC1_CH4_IRQHandler
#define USBD_TX_DMA_IRQ_PRIO            0
#define USBD_TX_DMA_INSTANCE            DMA1_Channel4
#define USBD_TX_DMA_IRQ                 DMAC1_CH4_IRQn

#define SPI1_DMA_TX_IRQHandler         DMAC1_CH4_IRQHandler
#define SPI1_TX_DMA_IRQ_PRIO           0
#define SPI1_TX_DMA_INSTANCE           DMA1_Channel4
#define SPI1_TX_DMA_IRQ                DMAC1_CH4_IRQn

/* DMA1 channel5 */
#define UART1_DMA_TX_IRQHandler          DMAC1_CH5_IRQHandler
#define UART1_TX_DMA_IRQ_PRIO            0
#define UART1_TX_DMA_INSTANCE            DMA1_Channel5
#define UART1_TX_DMA_IRQ                 DMAC1_CH5_IRQn

/*UART 2 RX DMA, shared with UART 1 TX DMA*/
#define UART2_DMA_RX_IRQHandler          DMAC1_CH5_IRQHandler
#define UART2_RX_DMA_IRQ_PRIO            0
#define UART2_RX_DMA_INSTANCE            DMA1_Channel5
#define UART2_RX_DMA_IRQ                 DMAC1_CH5_IRQn

/* DMA1 channel6 */
#define UART1_DMA_RX_IRQHandler          DMAC1_CH6_IRQHandler
#define UART1_RX_DMA_IRQ_PRIO            0
#define UART1_RX_DMA_INSTANCE            DMA1_Channel6
#define UART1_RX_DMA_IRQ                 DMAC1_CH6_IRQn

/* DMA1 channel7  */
#define UART3_DMA_TX_IRQHandler         DMAC1_CH7_IRQHandler
#define UART3_TX_DMA_IRQ_PRIO           0
#define UART3_TX_DMA_INSTANCE           DMA1_Channel7
#define UART3_TX_DMA_IRQ                DMAC1_CH7_IRQn

#define SPI2_DMA_RX_IRQHandler         DMAC1_CH7_IRQHandler
#define SPI2_RX_DMA_IRQ_PRIO           0
#define SPI2_RX_DMA_INSTANCE           DMA1_Channel7
#define SPI2_RX_DMA_IRQ                DMAC1_CH7_IRQn

/* DMA1 channel8  */
#define UART3_DMA_RX_IRQHandler         DMAC1_CH8_IRQHandler
#define UART3_RX_DMA_IRQ_PRIO           0
#define UART3_RX_DMA_INSTANCE           DMA1_Channel8
#define UART3_RX_DMA_IRQ                DMAC1_CH8_IRQn

#define SPI2_DMA_TX_IRQHandler         DMAC1_CH8_IRQHandler
#define SPI2_TX_DMA_IRQ_PRIO           0
#define SPI2_TX_DMA_INSTANCE           DMA1_Channel8
#define SPI2_TX_DMA_IRQ                DMAC1_CH8_IRQn

/*************************************DMA2 ***************************************/
#define I2S1_TX_DMA_REQUEST                 DMA_REQUEST_0
#define I2S1_RX_DMA_REQUEST                 DMA_REQUEST_1
#define I2S2_TX_DMA_REQUEST                 DMA_REQUEST_2
#define I2S2_RX_DMA_REQUEST                 DMA_REQUEST_3
#define PDM1_L_DMA_REQUEST                  DMA_REQUEST_4
#define PDM1_R_DMA_REQUEST                  DMA_REQUEST_5
#define PDM2_L_DMA_REQUEST                  DMA_REQUEST_6
#define PDM2_R_DMA_REQUEST                  DMA_REQUEST_7
#define AUDCODEC_DAC0_DMA_REQUEST           DMA_REQUEST_9
#define AUDCODEC_DAC1_DMA_REQUEST           DMA_REQUEST_10
#define GPTIM2_UPDATE_DMA_REQUEST           DMA_REQUEST_11
#define GPTIM2_TRIGGER_DMA_REQUEST          DMA_REQUEST_12
#define GPTIM2_CC1_DMA_REQUEST              DMA_REQUEST_13
#define AUDPRC_TX_OUT1_DMA_REQUEST          DMA_REQUEST_14
#define AUDPRC_TX_OUT0_DMA_REQUEST          DMA_REQUEST_15
#define AUDPRC_TX3_DMA_REQUEST              DMA_REQUEST_16
#define AUDPRC_TX2_DMA_REQUEST              DMA_REQUEST_17
#define AUDPRC_TX1_DMA_REQUEST              DMA_REQUEST_18
#define AUDPRC_TX0_DMA_REQUEST              DMA_REQUEST_19
#define AUDPRC_RX1_DMA_REQUEST              DMA_REQUEST_20
#define AUDPRC_RX0_DMA_REQUEST              DMA_REQUEST_21
#define GPTIM2_CC2_DMA_REQUEST              DMA_REQUEST_22
#define GPTIM2_CC3_DMA_REQUEST              DMA_REQUEST_23
#define GPTIM2_CC4_DMA_REQUEST              DMA_REQUEST_24
#define ATIM2_UPDATE_DMA_REQUEST            DMA_REQUEST_25
#define ATIM2_TRIGGER_DMA_REQUEST           DMA_REQUEST_26
#define PWMA2_CC1_DMA_REQUEST               DMA_REQUEST_27//atime2_cc1
#define PWMA2_CC2_DMA_REQUEST               DMA_REQUEST_28//atime2_cc2
#define PWMA2_CC3_DMA_REQUEST               DMA_REQUEST_29//atime2_cc3
#define PWMA2_CC4_DMA_REQUEST               DMA_REQUEST_30//atime2_cc4
#define ATIM2_COM_DMA_REQUEST               DMA_REQUEST_31

/* DMA2 channel1  */

//I2S1 TX
#if defined(BSP_ENABLE_I2S_MIC) && !defined(MIC_TX_DMA_INSTANCE)
#define MIC_TX_DMA_IRQHandler              DMAC2_CH1_IRQHandler
#define MIC_TX_DMA_IRQ_PRIO                0
#define MIC_TX_DMA_INSTANCE                DMA2_Channel1

#define MIC_TX_DMA_IRQ                     DMAC2_CH1_IRQn
#endif
//ATIM
#if defined(BSP_PWMA2_CC4_USING_DMA) && !defined(PWMA2_CC4_DMA_INSTANCE)
#define PWMA2_CC4_DMA_IRQHandler              DMAC2_CH1_IRQHandler
#define PWMA2_CC4_DMA_IRQ_PRIO                1
#define PWMA2_CC4_DMA_INSTANCE                DMA2_Channel1
#define PWMA2_CC4_DMA_IRQ                     DMAC2_CH1_IRQn
#define PWMA2_CC4_DMA_PDATAALIGN                        DMA_PDATAALIGN_WORD
#define PWMA2_CC4_DMA_MDATAALIGN                        DMA_MDATAALIGN_WORD
#endif
// AUDPRC TX CH0
#if defined(BSP_AUDPRC_TX0_DMA) && !defined(AUDPRC_TX0_DMA_INSTANCE)
#define AUDPRC_TX0_DMA_IRQHandler              DMAC2_CH1_IRQHandler
#define AUDPRC_TX0_DMA_IRQ_PRIO                0
#define AUDPRC_TX0_DMA_INSTANCE                DMA2_Channel1
#define AUDPRC_TX0_DMA_IRQ                     DMAC2_CH1_IRQn
#endif

// AUDCODEC DAC CH0
#if defined(BSP_AUDCODEC_DAC0_DMA) && !defined(AUDCODEC_DAC0_DMA_INSTANCE)
#define AUDCODEC_DAC0_DMA_IRQHandler              DMAC2_CH1_IRQHandler
#define AUDCODEC_DAC0_DMA_IRQ_PRIO                0
#define AUDCODEC_DAC0_DMA_INSTANCE                DMA2_Channel1
#define AUDCODEC_DAC0_DMA_IRQ                     DMAC2_CH1_IRQn
#endif

/* DMA2 channel2  */

//I2S1 RX
#if defined(BSP_ENABLE_I2S_MIC) && !defined(MIC_DMA_INSTANCE)
#define MIC_DMA_RX_IRQHandler           DMAC2_CH2_IRQHandler
#define MIC_DMA_IRQ_PRIO                0
#define MIC_DMA_INSTANCE                DMA2_Channel2
#define MIC_DMA_IRQ                     DMAC2_CH2_IRQn
#endif

// AUDPRC TX CH1
#if defined(BSP_AUDPRC_TX1_DMA) && !defined(AUDPRC_TX1_DMA_INSTANCE)
#define AUDPRC_TX1_DMA_IRQHandler              DMAC2_CH2_IRQHandler
#define AUDPRC_TX1_DMA_IRQ_PRIO                0
#define AUDPRC_TX1_DMA_INSTANCE                DMA2_Channel2
#define AUDPRC_TX1_DMA_IRQ                     DMAC2_CH2_IRQn
#endif

// AUDCODEC DAC CH1
#if defined(BSP_AUDCODEC_DAC1_DMA) && !defined(AUDCODEC_DAC1_DMA_INSTANCE)
#define AUDCODEC_DAC1_DMA_IRQHandler              DMAC2_CH2_IRQHandler
#define AUDCODEC_DAC1_DMA_IRQ_PRIO                0
#define AUDCODEC_DAC1_DMA_INSTANCE                DMA2_Channel2
#define AUDCODEC_DAC1_DMA_IRQ                     DMAC2_CH2_IRQn
#endif

/* DMA2 channel3  */
//I2S2 TX
#if defined(BSP_ENABLE_I2S_CODEC) && !defined(I2S_TX_DMA_INSTANCE)
#ifdef SOC_BF0_HCPU
#define I2S_TX_DMA_IRQHandler              DMAC2_CH3_IRQHandler
#define I2S_TX_DMA_IRQ                     DMAC2_CH3_IRQn
#else // LCPU
#define I2S_TX_DMA_IRQHandler              Interrupt30_IRQHandler
#define I2S_TX_DMA_IRQ                     Interrupt30_IRQn
#endif  //SOC_BF0_HCPU
#define I2S_TX_DMA_IRQ_PRIO                0
#define I2S_TX_DMA_INSTANCE                DMA2_Channel3
#else
// AUDPRC RX CH1
#if defined(BSP_AUDPRC_RX1_DMA) && !defined(AUDPRC_RX1_DMA_INSTANCE)
#define AUDPRC_RX1_DMA_IRQHandler              DMAC2_CH3_IRQHandler
#define AUDPRC_RX1_DMA_IRQ_PRIO                0
#define AUDPRC_RX1_DMA_INSTANCE                DMA2_Channel3
#define AUDPRC_RX1_DMA_IRQ                     DMAC2_CH3_IRQn
#endif

#endif

/* DMA2 channel4  */
//I2S2 RX
#if defined(BSP_ENABLE_I2S_CODEC) && !defined(I2S_RX_DMA_INSTANCE)
#ifdef SOC_BF0_HCPU
#define I2S_RX_DMA_IRQHandler              DMAC2_CH4_IRQHandler
#define I2S_RX_DMA_IRQ                     DMAC2_CH4_IRQn
#else // LCPU
#define I2S_RX_DMA_IRQHandler              Interrupt31_IRQHandler
#define I2S_RX_DMA_IRQ                     Interrupt31_IRQn
#endif //SOC_BF0_HCPU
#define I2S_RX_DMA_IRQ_PRIO                0
#define I2S_RX_DMA_INSTANCE                DMA2_Channel4
#else
// AUDPRC TX OUT CH1
#if defined(BSP_AUDPRC_TX_OUT1_DMA) && !defined(AUDPRC_TX_OUT1_DMA_INSTANCE)
#define AUDPRC_TX_OUT1_DMA_IRQHandler              DMAC2_CH4_IRQHandler
#define AUDPRC_TX_OUT1_DMA_IRQ_PRIO                0
#define AUDPRC_TX_OUT1_DMA_INSTANCE                DMA2_Channel4
#define AUDPRC_TX_OUT1_DMA_IRQ                     DMAC2_CH4_IRQn
#endif

#endif

/* DMA2 channel5  */
//PDM1 L
#if !defined(PDM1_L_DMA_INSTANCE)
#define PDM1_L_DMA_IRQHandler           DMAC2_CH5_IRQHandler
#define PDM1_L_DMA_IRQ_PRIO             0
#define PDM1_L_DMA_INSTANCE             DMA2_Channel5
#define PDM1_L_DMA_IRQ                  DMAC2_CH5_IRQn
#endif

// AUDPRC RX CH0
#if defined(BSP_AUDPRC_RX0_DMA) && !defined(AUDPRC_RX0_DMA_INSTANCE)
#define AUDPRC_RX0_DMA_IRQHandler              DMAC2_CH5_IRQHandler
#define AUDPRC_RX0_DMA_IRQ_PRIO                0
#define AUDPRC_RX0_DMA_INSTANCE                DMA2_Channel5
#define AUDPRC_RX0_DMA_IRQ                     DMAC2_CH5_IRQn
#endif

/* DMA2 channel6  */
//PDM1 R
#if !defined(PDM1_R_DMA_INSTANCE)
#define PDM1_R_DMA_IRQHandler              DMAC2_CH6_IRQHandler
#define PDM1_R_DMA_IRQ_PRIO                0
#define PDM1_R_DMA_INSTANCE                DMA2_Channel6
#define PDM1_R_DMA_IRQ                     DMAC2_CH6_IRQn
#endif

// AUDPRC TX OUT CH0
#if defined(BSP_AUDPRC_TX_OUT0_DMA) && !defined(AUDPRC_TX_OUT0_DMA_INSTANCE)
#define AUDPRC_TX_OUT0_DMA_IRQHandler              DMAC2_CH6_IRQHandler
#define AUDPRC_TX_OUT0_DMA_IRQ_PRIO                0
#define AUDPRC_TX_OUT0_DMA_INSTANCE                DMA2_Channel6
#define AUDPRC_TX_OUT0_DMA_IRQ                     DMAC2_CH6_IRQn
#endif

/* DMA2 channel7  */
//PDM2 L
#if !defined(PDM2_L_DMA_INSTANCE)
#define PDM2_L_DMA_IRQHandler           DMAC2_CH7_IRQHandler
#define PDM2_L_DMA_IRQ_PRIO             0
#define PDM2_L_DMA_INSTANCE             DMA2_Channel7
#define PDM2_L_DMA_IRQ                  DMAC2_CH7_IRQn
#endif

// AUDPRC TX CH2
#if defined(BSP_AUDPRC_TX2_DMA) && !defined(AUDPRC_TX2_DMA_INSTANCE)
#define AUDPRC_TX2_DMA_IRQHandler              DMAC2_CH7_IRQHandler
#define AUDPRC_TX2_DMA_IRQ_PRIO                0
#define AUDPRC_TX2_DMA_INSTANCE                DMA2_Channel7
#define AUDPRC_TX2_DMA_IRQ                     DMAC2_CH7_IRQn
#endif

/* DMA2 channel8  */
//PDM2 R
#if !defined(PDM2_R_DMA_INSTANCE)
#define PDM2_R_DMA_IRQHandler              DMAC2_CH8_IRQHandler
#define PDM2_R_DMA_IRQ_PRIO                0
#define PDM2_R_DMA_INSTANCE                DMA2_Channel8
#define PDM2_R_DMA_IRQ                     DMAC2_CH8_IRQn
#endif

// AUDPRC TX CH3
#if defined(BSP_AUDPRC_TX3_DMA) && !defined(AUDPRC_TX3_DMA_INSTANCE)
#define AUDPRC_TX3_DMA_IRQHandler              DMAC2_CH8_IRQHandler
#define AUDPRC_TX3_DMA_IRQ_PRIO                0
#define AUDPRC_TX3_DMA_INSTANCE                DMA2_Channel8
#define AUDPRC_TX3_DMA_IRQ                     DMAC2_CH8_IRQn
#endif

/*************************************DMA3 ***************************************/
#define UART4_TX_DMA_REQUEST                   DMA_REQUEST_0
#define UART4_RX_DMA_REQUEST                   DMA_REQUEST_1
#define UART5_TX_DMA_REQUEST                   DMA_REQUEST_2
#define UART5_RX_DMA_REQUEST                   DMA_REQUEST_3
#define UART6_TX_DMA_REQUEST                   DMA_REQUEST_4
#define UART6_RX_DMA_REQUEST                   DMA_REQUEST_5
#define BTIM3_DMA_REQUEST                      DMA_REQUEST_6
#define BTIM4_DMA_REQUEST                      DMA_REQUEST_7
#define GPTIM3_UPDATE_DMA_REQUEST              DMA_REQUEST_8
#define GPTIM3_TRIGGER_DMA_REQUEST             DMA_REQUEST_9
#define GPTIM3_CC1_DMA_REQUEST                 DMA_REQUEST_10
#define GPTIM3_CC2_DMA_REQUEST                 DMA_REQUEST_11
#define GPTIM3_CC3_DMA_REQUEST                 DMA_REQUEST_12
#define GPTIM3_CC4_DMA_REQUEST                 DMA_REQUEST_13
#define PWM4_CC4_DMA_REQUEST                   DMA_REQUEST_13//gtim3_cc4
#define GPTIM5_UPDATE_DMA_REQUEST              DMA_REQUEST_14
#define GPTIM5_TRIGGER_DMA_REQUEST             DMA_REQUEST_15
#define SPI3_TX_DMA_REQUEST                    DMA_REQUEST_16
#define SPI3_RX_DMA_REQUEST                    DMA_REQUEST_17
#define SPI4_TX_DMA_REQUEST                    DMA_REQUEST_18
#define SPI4_RX_DMA_REQUEST                    DMA_REQUEST_19
#define FLASH5_DMA_REQUEST                     DMA_REQUEST_20
#define I2C5_DMA_REQUEST                       DMA_REQUEST_21
#define I2C6_DMA_REQUEST                       DMA_REQUEST_22
#define I2C7_DMA_REQUEST                       DMA_REQUEST_23
#define GPTIM4_UPDATE_DMA_REQUEST              DMA_REQUEST_24
#define GPTIM4_TRIGGER_DMA_REQUEST             DMA_REQUEST_25
#define I2S3_RX_DMA_REQUEST                    DMA_REQUEST_26
#define I2S3_TX_DMA_REQUEST                    DMA_REQUEST_27
#define AUDCODEC_ADC0_DMA_REQUEST              DMA_REQUEST_28
#define AUDCODEC_ADC1_DMA_REQUEST              DMA_REQUEST_29
#define GPTIM4_CC1_DMA_REQUEST                 DMA_REQUEST_26
#define GPTIM4_CC2_DMA_REQUEST                 DMA_REQUEST_27
#define GPTIM4_CC3_DMA_REQUEST                 DMA_REQUEST_28
#define GPTIM4_CC4_DMA_REQUEST                 DMA_REQUEST_29
#define GPADC_DMA_REQUEST                      DMA_REQUEST_30
#define SDADC_DMA_REQUEST                      DMA_REQUEST_31

/* DMA3 channel1  */
#if defined(BSP_UART4_TX_USING_DMA) && !defined(UART4_TX_DMA_INSTANCE)
#define UART4_DMA_TX_IRQHandler         DMAC3_CH1_IRQHandler
#define UART4_TX_DMA_IRQ_PRIO           0
#define UART4_TX_DMA_INSTANCE           DMA3_Channel1
#define UART4_TX_DMA_IRQ                DMAC3_CH1_IRQn
#endif

/* DMA3 channel2  */
#if defined(BSP_UART4_RX_USING_DMA) && !defined(UART4_RX_DMA_INSTANCE)
#define UART4_DMA_RX_IRQHandler         DMAC3_CH2_IRQHandler
#define UART4_RX_DMA_IRQ_PRIO           0
#define UART4_RX_DMA_INSTANCE           DMA3_Channel2
#define UART4_RX_DMA_IRQ                DMAC3_CH2_IRQn
#endif

/* DMA3 channel3  */
#if defined(BSP_SPI3_RX_USING_DMA) && !defined(SPI3_RX_DMA_INSTANCE)
#define SPI3_DMA_RX_IRQHandler         DMAC3_CH3_IRQHandler
#define SPI3_RX_DMA_IRQ_PRIO           0
#define SPI3_RX_DMA_INSTANCE           DMA3_Channel3
#define SPI3_RX_DMA_IRQ                DMAC3_CH3_IRQn
#endif

#if defined(BSP_PWM4_UPDATE_USING_DMA) && !defined(PWM4_UPDATE_DMA_INSTANCE)
#define PWM4_UPDATE_DMA_IRQHandler              DMAC3_CH3_IRQHandler
#define PWM4_UPDATE_DMA_IRQ_PRIO                1
#define PWM4_UPDATE_DMA_INSTANCE                DMA3_Channel3
#define PWM4_UPDATE_DMA_IRQ                     DMAC3_CH3_IRQn
#define PWM4_UPDATE_DMA_PDATAALIGN                       DMA_PDATAALIGN_HALFWORD
#define PWM4_UPDATE_DMA_MDATAALIGN                       DMA_MDATAALIGN_HALFWORD
#endif

#if defined(BSP_PWM4_CC4_USING_DMA) && !defined(PWM4_CC4_DMA_INSTANCE)
#define PWM4_CC4_DMA_IRQHandler              DMAC3_CH3_IRQHandler
#define PWM4_CC4_DMA_IRQ_PRIO                1
#define PWM4_CC4_DMA_INSTANCE                DMA3_Channel3
#define PWM4_CC4_DMA_IRQ                     DMAC3_CH3_IRQn
#define PWM4_CC4_DMA_PDATAALIGN                       DMA_PDATAALIGN_HALFWORD
#define PWM4_CC4_DMA_MDATAALIGN                       DMA_MDATAALIGN_HALFWORD
#endif

/* DMA3 channel4  */
#if defined(BSP_SPI3_TX_USING_DMA) && !defined(SPI3_TX_DMA_INSTANCE)
#define SPI3_DMA_TX_IRQHandler         DMAC3_CH4_IRQHandler
#define SPI3_TX_DMA_IRQ_PRIO           0
#define SPI3_TX_DMA_INSTANCE           DMA3_Channel4
#define SPI3_TX_DMA_IRQ                DMAC3_CH4_IRQn
#endif

/* DMA3 channel3 */
#if defined(BSP_I2C5_USING_DMA) && !defined(I2C5_DMA_INSTANCE)
#define I2C5_DMA_IRQHandler              DMAC3_CH3_IRQHandler
#define I2C5_DMA_IRQ_PRIO                1
#define I2C5_DMA_INSTANCE                DMA3_Channel3
#define I2C5_DMA_IRQ                     DMAC3_CH3_IRQn
#endif

/* DMA3 channel4 */
#if defined(BSP_I2C6_USING_DMA) && !defined(I2C6_DMA_INSTANCE)
#define I2C6_DMA_IRQHandler              DMAC3_CH4_IRQHandler
#define I2C6_DMA_IRQ_PRIO                1
#define I2C6_DMA_INSTANCE                DMA3_Channel4
#define I2C6_DMA_IRQ                     DMAC3_CH4_IRQn
#endif

//I2S3 RX
#if defined(BSP_ENABLE_I2S3) && !defined(I2S3_RX_DMA_INSTANCE)
#define I2S3_RX_DMA_IRQHandler              DMAC3_CH4_IRQHandler
#define I2S3_RX_DMA_IRQ                     DMAC3_CH4_IRQn
#define I2S3_RX_DMA_IRQ_PRIO                0
#define I2S3_RX_DMA_INSTANCE                DMA3_Channel4
#endif
//I2S3 TX
#if defined(BSP_ENABLE_I2S3) && !defined(I2S3_TX_DMA_INSTANCE)
#define I2S3_TX_DMA_IRQHandler              DMAC3_CH5_IRQHandler
#define I2S3_TX_DMA_IRQ                     DMAC3_CH5_IRQn
#define I2S3_TX_DMA_IRQ_PRIO                0
#define I2S3_TX_DMA_INSTANCE                DMA3_Channel5
#endif

//CODEC ADC CH0
#if defined(BSP_AUDCODEC_ADC0_DMA) && !defined(AUDCODEC_ADC0_DMA_INSTANCE)
#define AUDCODEC_ADC0_DMA_IRQHandler              DMAC3_CH4_IRQHandler
#define AUDCODEC_ADC0_DMA_IRQ                     DMAC3_CH4_IRQn
#define AUDCODEC_ADC0_DMA_IRQ_PRIO                0
#define AUDCODEC_ADC0_DMA_INSTANCE                DMA3_Channel4
#endif
//CODEC ADC CH1
#if defined(BSP_AUDCODEC_ADC1_DMA) && !defined(AUDCODEC_ADC1_DMA_INSTANCE)
#define AUDCODEC_ADC1_DMA_IRQHandler              DMAC3_CH5_IRQHandler
#define AUDCODEC_ADC1_DMA_IRQ                     DMAC3_CH5_IRQn
#define AUDCODEC_ADC1_DMA_IRQ_PRIO                0
#define AUDCODEC_ADC1_DMA_INSTANCE                DMA3_Channel5
#endif

/* DMA3 channel5 */
#if defined(BSP_UART5_TX_USING_DMA) && !defined(UART5_TX_DMA_INSTANCE)
#define UART5_DMA_TX_IRQHandler          DMAC3_CH5_IRQHandler
#define UART5_TX_DMA_IRQ_PRIO            0
#define UART5_TX_DMA_INSTANCE            DMA3_Channel5
#define UART5_TX_DMA_IRQ                 DMAC3_CH5_IRQn
#endif

/* DMA3 channel5 */
#if defined(BSP_GPADC_USING_DMA) && !defined(GPADC_DMA_INSTANCE)
#define GPADC_DMA_IRQHandler              DMAC3_CH5_IRQHandler
#define GPADC_DMA_IRQ_PRIO            0
#define GPADC_DMA_INSTANCE            DMA3_Channel5
#define GPADC_DMA_IRQ                 DMAC3_CH5_IRQn
#endif

/* DMA3 channel5 */
#if defined(BSP_SDADC_USING_DMA) && !defined(SDADC_DMA_INSTANCE)
#define SDADC_DMA_IRQHandler              DMAC3_CH5_IRQHandler
#define SDADC_DMA_IRQ_PRIO            0
#define SDADC_DMA_INSTANCE            DMA3_Channel5
#define SDADC_DMA_IRQ                 DMAC3_CH5_IRQn
#endif

/* DMA3 channel6 */
#if defined(BSP_UART5_RX_USING_DMA) && !defined(UART5_RX_DMA_INSTANCE)
#define UART5_DMA_RX_IRQHandler          DMAC3_CH6_IRQHandler
#define UART5_RX_DMA_IRQ_PRIO            0
#define UART5_RX_DMA_INSTANCE            DMA3_Channel6
#define UART5_RX_DMA_IRQ                 DMAC3_CH6_IRQn
#endif

/* DMA3 channel6, UART6 shared with UART5 RX, only one of UART5/6 is used in most cases. Comment this out if want to use DMA3_CH8*/
#if defined(BSP_UART6_RX_USING_DMA) && !defined(UART6_RX_DMA_INSTANCE)
#define UART6_DMA_RX_IRQHandler          DMAC3_CH6_IRQHandler
#define UART6_RX_DMA_IRQ_PRIO            0
#define UART6_RX_DMA_INSTANCE            DMA3_Channel6
#define UART6_RX_DMA_IRQ                 DMAC3_CH6_IRQn
#endif

/* DMA3 channel7 */
#if defined(BSP_SPI4_RX_USING_DMA) && !defined(SPI4_RX_DMA_INSTANCE)
#define SPI4_DMA_RX_IRQHandler         DMAC3_CH7_IRQHandler
#define SPI4_RX_DMA_IRQ_PRIO           0
#define SPI4_RX_DMA_INSTANCE           DMA3_Channel7
#define SPI4_RX_DMA_IRQ                DMAC3_CH7_IRQn
#endif

/* DMA3 channel8 */
#if defined(BSP_I2C7_USING_DMA) && !defined(I2C7_DMA_INSTANCE)
#define I2C7_DMA_IRQHandler              DMAC3_CH8_IRQHandler
#define I2C7_DMA_IRQ_PRIO                1
#define I2C7_DMA_INSTANCE                DMA3_Channel8
#define I2C7_DMA_IRQ                     DMAC3_CH8_IRQn
#endif

/* DMA3 channel8 */
#if defined(BSP_SPI4_TX_USING_DMA) && !defined(SPI4_TX_DMA_INSTANCE)
#define SPI4_DMA_TX_IRQHandler         DMAC3_CH8_IRQHandler
#define SPI4_TX_DMA_IRQ_PRIO           0
#define SPI4_TX_DMA_INSTANCE           DMA3_Channel8
#define SPI4_TX_DMA_IRQ                DMAC3_CH8_IRQn
#endif

/* DMA3 channel7 */
#if defined(BSP_UART6_TX_USING_DMA) && !defined(UART6_TX_DMA_INSTANCE)
#define UART6_DMA_TX_IRQHandler          DMAC3_CH7_IRQHandler
#define UART6_TX_DMA_IRQ_PRIO            0
#define UART6_TX_DMA_INSTANCE            DMA3_Channel7
#define UART6_TX_DMA_IRQ                 DMAC3_CH7_IRQn
#endif

/* DMA3 channel8 */
#if defined(BSP_UART6_RX_USING_DMA) && !defined(UART6_RX_DMA_INSTANCE)
#define UART6_DMA_RX_IRQHandler          DMAC3_CH8_IRQHandler
#define UART6_RX_DMA_IRQ_PRIO            0
#define UART6_RX_DMA_INSTANCE            DMA3_Channel8
#define UART6_RX_DMA_IRQ                 DMAC3_CH8_IRQn
#endif

/* DMA3 channel8 */
#ifndef FLASH5_DMA_INSTANCE
#define FLASH5_IRQHandler              DMAC3_CH8_IRQHandler
#define FLASH5_DMA_IRQ_PRIO            0
#define FLASH5_DMA_INSTANCE            DMA3_Channel8
#define FLASH5_DMA_IRQ                 DMAC3_CH8_IRQn
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DMA_CONFIG_H__ */

