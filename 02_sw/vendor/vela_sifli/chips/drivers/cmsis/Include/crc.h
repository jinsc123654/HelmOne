/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CRC_H
#define __CRC_H

typedef struct
{
    __IO uint32_t DR;
    __IO uint32_t SR;
    __IO uint32_t CR;
    __IO uint32_t RSVD1;
    __IO uint32_t INIT;
    __IO uint32_t POL;
} CRC_TypeDef;


/********************* Bit definition for CRC_DR register *********************/
#define CRC_DR_DR_Pos                   (0U)
#define CRC_DR_DR_Msk                   (0xFFFFFFFFUL << CRC_DR_DR_Pos)
#define CRC_DR_DR                       CRC_DR_DR_Msk

/********************* Bit definition for CRC_SR register *********************/
#define CRC_SR_DONE_Pos                 (0U)
#define CRC_SR_DONE_Msk                 (0x1UL << CRC_SR_DONE_Pos)
#define CRC_SR_DONE                     CRC_SR_DONE_Msk
#define CRC_SR_OVERFLOW_Pos             (1U)
#define CRC_SR_OVERFLOW_Msk             (0x1UL << CRC_SR_OVERFLOW_Pos)
#define CRC_SR_OVERFLOW                 CRC_SR_OVERFLOW_Msk

/********************* Bit definition for CRC_CR register *********************/
#define CRC_CR_RESET_Pos                (0U)
#define CRC_CR_RESET_Msk                (0x1UL << CRC_CR_RESET_Pos)
#define CRC_CR_RESET                    CRC_CR_RESET_Msk
#define CRC_CR_DATASIZE_Pos             (1U)
#define CRC_CR_DATASIZE_Msk             (0x3UL << CRC_CR_DATASIZE_Pos)
#define CRC_CR_DATASIZE                 CRC_CR_DATASIZE_Msk
#define CRC_CR_POLYSIZE_Pos             (3U)
#define CRC_CR_POLYSIZE_Msk             (0x3UL << CRC_CR_POLYSIZE_Pos)
#define CRC_CR_POLYSIZE                 CRC_CR_POLYSIZE_Msk
#define CRC_CR_REV_IN_Pos               (5U)
#define CRC_CR_REV_IN_Msk               (0x3UL << CRC_CR_REV_IN_Pos)
#define CRC_CR_REV_IN                   CRC_CR_REV_IN_Msk
#define CRC_CR_REV_OUT_Pos              (7U)
#define CRC_CR_REV_OUT_Msk              (0x1UL << CRC_CR_REV_OUT_Pos)
#define CRC_CR_REV_OUT                  CRC_CR_REV_OUT_Msk

/******************** Bit definition for CRC_INIT register ********************/
#define CRC_INIT_INIT_Pos               (0U)
#define CRC_INIT_INIT_Msk               (0xFFFFFFFFUL << CRC_INIT_INIT_Pos)
#define CRC_INIT_INIT                   CRC_INIT_INIT_Msk

/******************** Bit definition for CRC_POL register *********************/
#define CRC_POL_POL_Pos                 (0U)
#define CRC_POL_POL_Msk                 (0xFFFFFFFFUL << CRC_POL_POL_Pos)
#define CRC_POL_POL                     CRC_POL_POL_Msk

#endif
