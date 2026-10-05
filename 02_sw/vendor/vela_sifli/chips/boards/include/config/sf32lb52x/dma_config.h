/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DMA_CONFIG_H__
#define __DMA_CONFIG_H__

#include <sfconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

/*************************************DMA1 ***************************************/
/*
 * Helm One 固定 DMA1 通道（KeepAlloc 常驻，HAL 不挪 Instance、完成也不还）：
 *   CH3 SDMMC        CH4 USART2 RX GNSS（独占，开机钉死）
 *   CH5 LCDC         CH7 USART1 RX 控制台
 *   CH8 GPADC（若启用；不要占 CH4）
 * USART2 RX 不要用厂商默认 CH6（和 UART1 TX / SPI1 RX 共用）。
 */
#define FLASH1_DMA_REQUEST                     DMA_REQUEST_0
#define FLASH2_DMA_REQUEST                     DMA_REQUEST_1
#define I2C4_DMA_REQUEST                       DMA_REQUEST_3
#define UART1_TX_DMA_REQUEST                   DMA_REQUEST_4
#define UART1_RX_DMA_REQUEST                   DMA_REQUEST_5
#define UART2_TX_DMA_REQUEST                   DMA_REQUEST_6
#define UART2_RX_DMA_REQUEST                   DMA_REQUEST_7
#define GPTIM1_UPDATE_DMA_REQUEST              DMA_REQUEST_8
#define GPTIM1_TRIGGER_DMA_REQUEST             DMA_REQUEST_9
#define GPTIM1_CC1_DMA_REQUEST                 DMA_REQUEST_10
#define GPTIM1_CC2_DMA_REQUEST                 DMA_REQUEST_11
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
#define I2S_TX_DMA_REQUEST                     DMA_REQUEST_32
#define I2S_RX_DMA_REQUEST                     DMA_REQUEST_33
#define PDM1_L_DMA_REQUEST                     DMA_REQUEST_36
#define PDM1_R_DMA_REQUEST                     DMA_REQUEST_37
#define GPADC_DMA_REQUEST                      DMA_REQUEST_38
#define AUDCODEC_ADC0_DMA_REQUEST              DMA_REQUEST_39
#define AUDCODEC_ADC1_DMA_REQUEST              DMA_REQUEST_40
#define AUDCODEC_DAC0_DMA_REQUEST              DMA_REQUEST_41
#define AUDCODEC_DAC1_DMA_REQUEST              DMA_REQUEST_42
#define GPTIM2_UPDATE_DMA_REQUEST              DMA_REQUEST_43
#define GPTIM2_TRIGGER_DMA_REQUEST             DMA_REQUEST_44
#define PWM3_CC1_DMA_REQUEST                   DMA_REQUEST_45//GTIM2_CH1
#define GPTIM2_CC1_DMA_REQUEST                 DMA_REQUEST_45
#define AUDPRC_TX_OUT1_DMA_REQUEST             DMA_REQUEST_46
#define AUDPRC_TX_OUT0_DMA_REQUEST             DMA_REQUEST_47
#define AUDPRC_TX3_DMA_REQUEST                 DMA_REQUEST_48
#define AUDPRC_TX2_DMA_REQUEST                 DMA_REQUEST_49
#define AUDPRC_TX1_DMA_REQUEST                 DMA_REQUEST_50
#define AUDPRC_TX0_DMA_REQUEST                 DMA_REQUEST_51
#define AUDPRC_RX1_DMA_REQUEST                 DMA_REQUEST_52
#define AUDPRC_RX0_DMA_REQUEST                 DMA_REQUEST_53
#define GPTIM2_CC2_DMA_REQUEST                 DMA_REQUEST_54
#define GPTIM2_CC3_DMA_REQUEST                 DMA_REQUEST_55
#define GPTIM2_CC4_DMA_REQUEST                 DMA_REQUEST_56
#define SDMMC1_DMA_REQUEST                     DMA_REQUEST_57

/* DMA1 channel1 */
#define AUDPRC_TX0_DMA_IRQHandler              DMAC1_CH1_IRQHandler
#define AUDPRC_TX0_DMA_IRQ_PRIO                0
#define AUDPRC_TX0_DMA_INSTANCE                DMA1_Channel1
#define AUDPRC_TX0_DMA_IRQ                     DMAC1_CH1_IRQn

#define AUDPRC_RX1_DMA_IRQHandler              DMAC1_CH1_IRQHandler
#define AUDPRC_RX1_DMA_IRQ_PRIO                0
#define AUDPRC_RX1_DMA_INSTANCE                DMA1_Channel1
#define AUDPRC_RX1_DMA_IRQ                     DMAC1_CH1_IRQn

/* DMA1 channel2 */
#define FLASH2_IRQHandler              DMAC1_CH2_IRQHandler
#define FLASH2_DMA_IRQ_PRIO            0
#define FLASH2_DMA_INSTANCE            DMA1_Channel2
#define FLASH2_DMA_IRQ                 DMAC1_CH2_IRQn

/* DMA1 channel3 */
#define FLASH1_IRQHandler              DMAC1_CH3_IRQHandler
#define FLASH1_DMA_IRQ_PRIO            0
#define FLASH1_DMA_INSTANCE            DMA1_Channel3
#define FLASH1_DMA_IRQ                 DMAC1_CH3_IRQn

#define SDMMC1_DMA_IRQHandler          DMAC1_CH3_IRQHandler
#define SDMMC1_DMA_IRQ_PRIO            0
#define SDMMC1_DMA_INSTANCE            DMA1_Channel3
#define SDMMC1_DMA_IRQ                 DMAC1_CH3_IRQn

#define I2C2_DMA_IRQHandler            DMAC1_CH3_IRQHandler
#define I2C2_DMA_IRQ_PRIO              1
#define I2C2_DMA_INSTANCE              DMA1_Channel5
#define I2C2_DMA_IRQ                   DMAC1_CH5_IRQn

#define SPI1_DMA_TX_IRQHandler         DMAC1_CH3_IRQHandler
#define SPI1_TX_DMA_IRQ_PRIO           0
#define SPI1_TX_DMA_INSTANCE           DMA1_Channel3
#define SPI1_TX_DMA_IRQ                DMAC1_CH3_IRQn


#define PWM3_UPDATE_DMA_IRQHandler          DMAC1_CH3_IRQHandler
#define PWM3_UPDATE_DMA_IRQ_PRIO          1
#define PWM3_UPDATE_DMA_INSTANCE          DMA1_Channel3
#define PWM3_UPDATE_DMA_IRQ               DMAC1_CH3_IRQn
#define PWM3_UPDATE_DMA_PDATAALIGN        DMA_PDATAALIGN_HALFWORD
#define PWM3_UPDATE_DMA_MDATAALIGN        DMA_MDATAALIGN_HALFWORD

#define PWM3_CC1_DMA_IRQHandler        DMAC1_CH3_IRQHandler
#define PWM3_CC1_DMA_IRQ_PRIO          1
#define PWM3_CC1_DMA_INSTANCE          DMA1_Channel3
#define PWM3_CC1_DMA_IRQ               DMAC1_CH3_IRQn
#define PWM3_CC1_DMA_PDATAALIGN        DMA_PDATAALIGN_HALFWORD
#define PWM3_CC1_DMA_MDATAALIGN        DMA_MDATAALIGN_HALFWORD

/* DMA1 channel4 */
#define AUDPRC_TX1_DMA_IRQHandler              DMAC1_CH4_IRQHandler
#define AUDPRC_TX1_DMA_IRQ_PRIO                0
#define AUDPRC_TX1_DMA_INSTANCE                DMA1_Channel4
#define AUDPRC_TX1_DMA_IRQ                     DMAC1_CH4_IRQn

#define AUDPRC_TX_OUT0_DMA_IRQHandler          DMAC1_CH4_IRQHandler
#define AUDPRC_TX_OUT0_DMA_IRQ_PRIO            0
#define AUDPRC_TX_OUT0_DMA_INSTANCE            DMA1_Channel4
#define AUDPRC_TX_OUT0_DMA_IRQ                 DMAC1_CH4_IRQn

//ATIM1
#define PWMA1_CC1_DMA_IRQHandler               DMAC1_CH4_IRQHandler
#define PWMA1_CC1_DMA_IRQ_PRIO                 1
#define PWMA1_CC1_DMA_INSTANCE                 DMA1_Channel4
#define PWMA1_CC1_DMA_IRQ                      DMAC1_CH4_IRQn
#define PWMA1_CC1_DMA_PDATAALIGN               DMA_PDATAALIGN_WORD
#define PWMA1_CC1_DMA_MDATAALIGN               DMA_MDATAALIGN_WORD

//ATIM2
#define PWMA2_CC1_DMA_IRQHandler               DMAC1_CH4_IRQHandler
#define PWMA2_CC1_DMA_IRQ_PRIO                 1
#define PWMA2_CC1_DMA_INSTANCE                 DMA1_Channel4
#define PWMA2_CC1_DMA_IRQ                      DMAC1_CH4_IRQn
#define PWMA2_CC1_DMA_PDATAALIGN               DMA_PDATAALIGN_WORD
#define PWMA2_CC1_DMA_MDATAALIGN               DMA_MDATAALIGN_WORD

#define PWMA2_CC4_DMA_IRQHandler               DMAC2_CH1_IRQHandler
#define PWMA2_CC4_DMA_IRQ_PRIO                 1
#define PWMA2_CC4_DMA_INSTANCE                 DMA2_Channel1
#define PWMA2_CC4_DMA_IRQ                      DMAC2_CH1_IRQn
#define PWMA2_CC4_DMA_PDATAALIGN               DMA_PDATAALIGN_WORD
#define PWMA2_CC4_DMA_MDATAALIGN               DMA_MDATAALIGN_WORD

// AUDPRC RX CH0
#define AUDPRC_RX0_DMA_IRQHandler              DMAC1_CH4_IRQHandler
#define AUDPRC_RX0_DMA_IRQ_PRIO                0
#define AUDPRC_RX0_DMA_INSTANCE                DMA1_Channel4
#define AUDPRC_RX0_DMA_IRQ                     DMAC1_CH4_IRQn

/* GPADC 离开 CH4：USART2 RX GNSS 开机钉死该通道，KeepAlloc 不释放。 */
#define GPADC_DMA_IRQHandler                   DMAC1_CH8_IRQHandler
#define GPADC_DMA_IRQ_PRIO                     0
#define GPADC_DMA_INSTANCE                     DMA1_Channel8
#define GPADC_DMA_IRQ                          DMAC1_CH8_IRQn

/* CODEC ADC 也不要和 GNSS 抢 CH4。 */
#define AUDCODEC_ADC0_DMA_IRQHandler           DMAC1_CH8_IRQHandler
#define AUDCODEC_ADC0_DMA_IRQ                  DMAC1_CH8_IRQn
#define AUDCODEC_ADC0_DMA_IRQ_PRIO             0
#define AUDCODEC_ADC0_DMA_INSTANCE             DMA1_Channel8

/* UART2 RX GNSS：独占 CH4，开机 Alloc，关串口只停引擎。 */
#define UART2_DMA_RX_IRQHandler                DMAC1_CH4_IRQHandler
#define UART2_RX_DMA_IRQ_PRIO                  0
#define UART2_RX_DMA_INSTANCE                  DMA1_Channel4
#define UART2_RX_DMA_IRQ                       DMAC1_CH4_IRQn

/* DMA1 channel5 */
//PDM1 L
#define PDM1_L_DMA_IRQHandler                  DMAC1_CH5_IRQHandler
#define PDM1_L_DMA_IRQ_PRIO                    0
#define PDM1_L_DMA_INSTANCE                    DMA1_Channel5
#define PDM1_L_DMA_IRQ                         DMAC1_CH5_IRQn

#define I2S_TX_DMA_IRQHandler                  DMAC1_CH5_IRQHandler
#define I2S_TX_DMA_IRQ_PRIO                    0
#define I2S_TX_DMA_INSTANCE                    DMA1_Channel5
#define I2S_TX_DMA_IRQ                         DMAC1_CH5_IRQn

#define SPI2_DMA_RX_IRQHandler                 DMAC1_CH5_IRQHandler
#define SPI2_RX_DMA_IRQ_PRIO                   0
#define SPI2_RX_DMA_INSTANCE                   DMA1_Channel5
#define SPI2_RX_DMA_IRQ                        DMAC1_CH5_IRQn

#define SPI1_DMA_RX_IRQHandler                 DMAC1_CH6_IRQHandler
#define SPI1_RX_DMA_IRQ_PRIO                   0
#define SPI1_RX_DMA_INSTANCE                   DMA1_Channel6
#define SPI1_RX_DMA_IRQ                        DMAC1_CH6_IRQn

#define UART2_DMA_TX_IRQHandler                DMAC1_CH5_IRQHandler
#define UART2_TX_DMA_IRQ_PRIO                  0
#define UART2_TX_DMA_INSTANCE                  DMA1_Channel5
#define UART2_TX_DMA_IRQ                       DMAC1_CH5_IRQn

/* DMA1 channel6 */
#define UART1_DMA_TX_IRQHandler                DMAC1_CH6_IRQHandler
#define UART1_TX_DMA_IRQ_PRIO                  0
#define UART1_TX_DMA_INSTANCE                  DMA1_Channel6
#define UART1_TX_DMA_IRQ                       DMAC1_CH6_IRQn

#define AUDPRC_TX3_DMA_IRQHandler              DMAC1_CH6_IRQHandler
#define AUDPRC_TX3_DMA_IRQ_PRIO                0
#define AUDPRC_TX3_DMA_INSTANCE                DMA1_Channel6
#define AUDPRC_TX3_DMA_IRQ                     DMAC1_CH6_IRQn

/* DMA1 channel7 */
#define UART1_DMA_RX_IRQHandler                DMAC1_CH7_IRQHandler
#define UART1_RX_DMA_IRQ_PRIO                  0
#define UART1_RX_DMA_INSTANCE                  DMA1_Channel7
#define UART1_RX_DMA_IRQ                       DMAC1_CH7_IRQn

#define UART3_DMA_TX_IRQHandler                DMAC1_CH7_IRQHandler
#define UART3_TX_DMA_IRQ_PRIO                  0
#define UART3_TX_DMA_INSTANCE                  DMA1_Channel7
#define UART3_TX_DMA_IRQ                       DMAC1_CH7_IRQn

/* DMA1 channel8  */
//PDM1 R
#define PDM1_R_DMA_IRQHandler                  DMAC1_CH8_IRQHandler
#define PDM1_R_DMA_IRQ_PRIO                    0
#define PDM1_R_DMA_INSTANCE                    DMA1_Channel8
#define PDM1_R_DMA_IRQ                         DMAC1_CH8_IRQn

#define I2S_RX_DMA_IRQHandler                  DMAC1_CH8_IRQHandler
#define I2S_RX_DMA_IRQ_PRIO                    0
#define I2S_RX_DMA_INSTANCE                    DMA1_Channel8
#define I2S_RX_DMA_IRQ                         DMAC1_CH8_IRQn

#define SPI2_DMA_TX_IRQHandler                 DMAC1_CH8_IRQHandler
#define SPI2_TX_DMA_IRQ_PRIO                   0
#define SPI2_TX_DMA_INSTANCE                   DMA1_Channel8
#define SPI2_TX_DMA_IRQ                        DMAC1_CH8_IRQn

#define UART3_DMA_RX_IRQHandler                DMAC1_CH8_IRQHandler
#define UART3_RX_DMA_IRQ_PRIO                  0
#define UART3_RX_DMA_INSTANCE                  DMA1_Channel8
#define UART3_RX_DMA_IRQ                       DMAC1_CH8_IRQn

#define AUDPRC_TX2_DMA_IRQHandler              DMAC1_CH8_IRQHandler
#define AUDPRC_TX2_DMA_IRQ_PRIO                0
#define AUDPRC_TX2_DMA_INSTANCE                DMA1_Channel8
#define AUDPRC_TX2_DMA_IRQ                     DMAC1_CH8_IRQn

/*************************************DMA2 ***************************************/
/* DMA2 channel1  */
#define UART4_TX_DMA_REQUEST            DMA_REQUEST_0
#define UART4_RX_DMA_REQUEST            DMA_REQUEST_1
#define UART5_TX_DMA_REQUEST            DMA_REQUEST_2
#define UART5_RX_DMA_REQUEST            DMA_REQUEST_3
#define BTIM3_DMA_REQUEST               DMA_REQUEST_6
#define BTIM4_DMA_REQUEST               DMA_REQUEST_7

#if defined(BSP_UART4_TX_USING_DMA) && !defined(UART4_TX_DMA_INSTANCE)
#define UART4_DMA_TX_IRQHandler         DMAC2_CH1_IRQHandler
#define UART4_TX_DMA_IRQ_PRIO           0
#define UART4_TX_DMA_INSTANCE           DMA2_Channel1
#define UART4_TX_DMA_IRQ                DMAC2_CH1_IRQn
#endif

/* DMA2 channel2  */
#if defined(BSP_UART4_RX_USING_DMA) && !defined(UART4_RX_DMA_INSTANCE)
#define UART4_DMA_RX_IRQHandler         DMAC2_CH2_IRQHandler
#define UART4_RX_DMA_IRQ_PRIO           0
#define UART4_RX_DMA_INSTANCE           DMA2_Channel2
#define UART4_RX_DMA_IRQ                DMAC2_CH2_IRQn
#endif

/* DMA2 channel5 */
#if defined(BSP_UART5_TX_USING_DMA) && !defined(UART5_TX_DMA_INSTANCE)
#define UART5_DMA_TX_IRQHandler          DMAC2_CH5_IRQHandler
#define UART5_TX_DMA_IRQ_PRIO            0
#define UART5_TX_DMA_INSTANCE            DMA2_Channel5
#define UART5_TX_DMA_IRQ                 DMAC2_CH5_IRQn
#endif

/* DMA2 channel6 */
#if defined(BSP_UART5_RX_USING_DMA) && !defined(UART5_RX_DMA_INSTANCE)
#define UART5_DMA_RX_IRQHandler          DMAC2_CH6_IRQHandler
#define UART5_RX_DMA_IRQ_PRIO            0
#define UART5_RX_DMA_INSTANCE            DMA2_Channel6
#define UART5_RX_DMA_IRQ                 DMAC2_CH6_IRQn
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DMA_CONFIG_H__ */

