/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __LPTIM_H
#define __LPTIM_H

typedef struct
{
    __IO uint32_t ISR;
    __IO uint32_t ICR;
    __IO uint32_t IER;
    __IO uint32_t CFGR;
    __IO uint32_t CR;
    __IO uint32_t CMP;
    __IO uint32_t ARR;
    __IO uint32_t CNT;
    __IO uint32_t RCR;
} LPTIM_TypeDef;


/******************* Bit definition for LPTIM_ISR register ********************/
#define LPTIM_ISR_UE_Pos                (0U)
#define LPTIM_ISR_UE_Msk                (0x1UL << LPTIM_ISR_UE_Pos)
#define LPTIM_ISR_UE                    LPTIM_ISR_UE_Msk
#define LPTIM_ISR_OF_Pos                (1U)
#define LPTIM_ISR_OF_Msk                (0x1UL << LPTIM_ISR_OF_Pos)
#define LPTIM_ISR_OF                    LPTIM_ISR_OF_Msk
#define LPTIM_ISR_OC_Pos                (2U)
#define LPTIM_ISR_OC_Msk                (0x1UL << LPTIM_ISR_OC_Pos)
#define LPTIM_ISR_OC                    LPTIM_ISR_OC_Msk
#define LPTIM_ISR_ET_Pos                (3U)
#define LPTIM_ISR_ET_Msk                (0x1UL << LPTIM_ISR_ET_Pos)
#define LPTIM_ISR_ET                    LPTIM_ISR_ET_Msk
#define LPTIM_ISR_UEWKUP_Pos            (8U)
#define LPTIM_ISR_UEWKUP_Msk            (0x1UL << LPTIM_ISR_UEWKUP_Pos)
#define LPTIM_ISR_UEWKUP                LPTIM_ISR_UEWKUP_Msk
#define LPTIM_ISR_OFWKUP_Pos            (9U)
#define LPTIM_ISR_OFWKUP_Msk            (0x1UL << LPTIM_ISR_OFWKUP_Pos)
#define LPTIM_ISR_OFWKUP                LPTIM_ISR_OFWKUP_Msk
#define LPTIM_ISR_OCWKUP_Pos            (10U)
#define LPTIM_ISR_OCWKUP_Msk            (0x1UL << LPTIM_ISR_OCWKUP_Pos)
#define LPTIM_ISR_OCWKUP                LPTIM_ISR_OCWKUP_Msk

/******************* Bit definition for LPTIM_ICR register ********************/
#define LPTIM_ICR_UECLR_Pos             (0U)
#define LPTIM_ICR_UECLR_Msk             (0x1UL << LPTIM_ICR_UECLR_Pos)
#define LPTIM_ICR_UECLR                 LPTIM_ICR_UECLR_Msk
#define LPTIM_ICR_OFCLR_Pos             (1U)
#define LPTIM_ICR_OFCLR_Msk             (0x1UL << LPTIM_ICR_OFCLR_Pos)
#define LPTIM_ICR_OFCLR                 LPTIM_ICR_OFCLR_Msk
#define LPTIM_ICR_OCCLR_Pos             (2U)
#define LPTIM_ICR_OCCLR_Msk             (0x1UL << LPTIM_ICR_OCCLR_Pos)
#define LPTIM_ICR_OCCLR                 LPTIM_ICR_OCCLR_Msk
#define LPTIM_ICR_ETCLR_Pos             (3U)
#define LPTIM_ICR_ETCLR_Msk             (0x1UL << LPTIM_ICR_ETCLR_Pos)
#define LPTIM_ICR_ETCLR                 LPTIM_ICR_ETCLR_Msk
#define LPTIM_ICR_WKUPCLR_Pos           (8U)
#define LPTIM_ICR_WKUPCLR_Msk           (0x1UL << LPTIM_ICR_WKUPCLR_Pos)
#define LPTIM_ICR_WKUPCLR               LPTIM_ICR_WKUPCLR_Msk

/******************* Bit definition for LPTIM_IER register ********************/
#define LPTIM_IER_UEIE_Pos              (0U)
#define LPTIM_IER_UEIE_Msk              (0x1UL << LPTIM_IER_UEIE_Pos)
#define LPTIM_IER_UEIE                  LPTIM_IER_UEIE_Msk
#define LPTIM_IER_OFIE_Pos              (1U)
#define LPTIM_IER_OFIE_Msk              (0x1UL << LPTIM_IER_OFIE_Pos)
#define LPTIM_IER_OFIE                  LPTIM_IER_OFIE_Msk
#define LPTIM_IER_OCIE_Pos              (2U)
#define LPTIM_IER_OCIE_Msk              (0x1UL << LPTIM_IER_OCIE_Pos)
#define LPTIM_IER_OCIE                  LPTIM_IER_OCIE_Msk
#define LPTIM_IER_ETIE_Pos              (3U)
#define LPTIM_IER_ETIE_Msk              (0x1UL << LPTIM_IER_ETIE_Pos)
#define LPTIM_IER_ETIE                  LPTIM_IER_ETIE_Msk
#define LPTIM_IER_UEWE_Pos              (8U)
#define LPTIM_IER_UEWE_Msk              (0x1UL << LPTIM_IER_UEWE_Pos)
#define LPTIM_IER_UEWE                  LPTIM_IER_UEWE_Msk
#define LPTIM_IER_OFWE_Pos              (9U)
#define LPTIM_IER_OFWE_Msk              (0x1UL << LPTIM_IER_OFWE_Pos)
#define LPTIM_IER_OFWE                  LPTIM_IER_OFWE_Msk
#define LPTIM_IER_OCWE_Pos              (10U)
#define LPTIM_IER_OCWE_Msk              (0x1UL << LPTIM_IER_OCWE_Pos)
#define LPTIM_IER_OCWE                  LPTIM_IER_OCWE_Msk

/******************* Bit definition for LPTIM_CFGR register *******************/
#define LPTIM_CFGR_CKSEL_Pos            (0U)
#define LPTIM_CFGR_CKSEL_Msk            (0x1UL << LPTIM_CFGR_CKSEL_Pos)
#define LPTIM_CFGR_CKSEL                LPTIM_CFGR_CKSEL_Msk
#define LPTIM_CFGR_CKPOL_Pos            (1U)
#define LPTIM_CFGR_CKPOL_Msk            (0x3UL << LPTIM_CFGR_CKPOL_Pos)
#define LPTIM_CFGR_CKPOL                LPTIM_CFGR_CKPOL_Msk
#define LPTIM_CFGR_CKPOL_0              (0x1U << LPTIM_CFGR_CKPOL_Pos)
#define LPTIM_CFGR_CKPOL_1              (0x2U << LPTIM_CFGR_CKPOL_Pos)

#define LPTIM_CFGR_CKFLT_Pos            (3U)
#define LPTIM_CFGR_CKFLT_Msk            (0x3UL << LPTIM_CFGR_CKFLT_Pos)
#define LPTIM_CFGR_CKFLT                LPTIM_CFGR_CKFLT_Msk
#define LPTIM_CFGR_CKFLT_0              (0x1U << LPTIM_CFGR_CKFLT_Pos)
#define LPTIM_CFGR_CKFLT_1              (0x2U << LPTIM_CFGR_CKFLT_Pos)


#define LPTIM_CFGR_INTCKSEL_Pos         (5U)
#define LPTIM_CFGR_INTCKSEL_Msk         (0x1UL << LPTIM_CFGR_INTCKSEL_Pos)
#define LPTIM_CFGR_INTCKSEL             LPTIM_CFGR_INTCKSEL_Msk
#define LPTIM_CFGR_TRGFLT_Pos           (6U)
#define LPTIM_CFGR_TRGFLT_Msk           (0x3UL << LPTIM_CFGR_TRGFLT_Pos)
#define LPTIM_CFGR_TRGFLT               LPTIM_CFGR_TRGFLT_Msk
#define LPTIM_CFGR_TRGFLT_0             (0x1U << LPTIM_CFGR_TRGFLT_Pos)
#define LPTIM_CFGR_TRGFLT_1             (0x2U << LPTIM_CFGR_TRGFLT_Pos)

#define LPTIM_CFGR_EXTCKSEL_Pos         (8U)
#define LPTIM_CFGR_EXTCKSEL_Msk         (0x1UL << LPTIM_CFGR_EXTCKSEL_Pos)
#define LPTIM_CFGR_EXTCKSEL             LPTIM_CFGR_EXTCKSEL_Msk
#define LPTIM_CFGR_PRESC_Pos            (9U)
#define LPTIM_CFGR_PRESC_Msk            (0x7UL << LPTIM_CFGR_PRESC_Pos)
#define LPTIM_CFGR_PRESC                LPTIM_CFGR_PRESC_Msk
#define LPTIM_CFGR_PRESC_0              (0x1U << LPTIM_CFGR_PRESC_Pos)
#define LPTIM_CFGR_PRESC_1              (0x2U << LPTIM_CFGR_PRESC_Pos)
#define LPTIM_CFGR_PRESC_2              (0x4U << LPTIM_CFGR_PRESC_Pos)

#define LPTIM_CFGR_TRIGSEL_Pos          (13U)
#define LPTIM_CFGR_TRIGSEL_Msk          (0x7UL << LPTIM_CFGR_TRIGSEL_Pos)
#define LPTIM_CFGR_TRIGSEL              LPTIM_CFGR_TRIGSEL_Msk
#define LPTIM_CFGR_TRIGSEL_0            (0x1U << LPTIM_CFGR_TRIGSEL_Pos)
#define LPTIM_CFGR_TRIGSEL_1            (0x2U << LPTIM_CFGR_TRIGSEL_Pos)
#define LPTIM_CFGR_TRIGSEL_2            (0x4U << LPTIM_CFGR_TRIGSEL_Pos)

#define LPTIM_CFGR_TRIGEN_Pos           (17U)
#define LPTIM_CFGR_TRIGEN_Msk           (0x3UL << LPTIM_CFGR_TRIGEN_Pos)
#define LPTIM_CFGR_TRIGEN               LPTIM_CFGR_TRIGEN_Msk
#define LPTIM_CFGR_TRIGEN_0           (0x1U << LPTIM_CFGR_TRIGEN_Pos)
#define LPTIM_CFGR_TRIGEN_1           (0x2U << LPTIM_CFGR_TRIGEN_Pos)


#define LPTIM_CFGR_TIMOUT_Pos           (19U)
#define LPTIM_CFGR_TIMOUT_Msk           (0x1UL << LPTIM_CFGR_TIMOUT_Pos)
#define LPTIM_CFGR_TIMOUT               LPTIM_CFGR_TIMOUT_Msk
#define LPTIM_CFGR_WAVE_Pos             (20U)
#define LPTIM_CFGR_WAVE_Msk             (0x1UL << LPTIM_CFGR_WAVE_Pos)
#define LPTIM_CFGR_WAVE                 LPTIM_CFGR_WAVE_Msk
#define LPTIM_CFGR_WAVPOL_Pos           (21U)
#define LPTIM_CFGR_WAVPOL_Msk           (0x1UL << LPTIM_CFGR_WAVPOL_Pos)
#define LPTIM_CFGR_WAVPOL               LPTIM_CFGR_WAVPOL_Msk
#define LPTIM_CFGR_COUNTMODE_Pos        (23U)
#define LPTIM_CFGR_COUNTMODE_Msk        (0x1UL << LPTIM_CFGR_COUNTMODE_Pos)
#define LPTIM_CFGR_COUNTMODE            LPTIM_CFGR_COUNTMODE_Msk

/******************** Bit definition for LPTIM_CR register ********************/
#define LPTIM_CR_ENABLE_Pos             (0U)
#define LPTIM_CR_ENABLE_Msk             (0x1UL << LPTIM_CR_ENABLE_Pos)
#define LPTIM_CR_ENABLE                 LPTIM_CR_ENABLE_Msk
#define LPTIM_CR_SNGSTRT_Pos            (1U)
#define LPTIM_CR_SNGSTRT_Msk            (0x1UL << LPTIM_CR_SNGSTRT_Pos)
#define LPTIM_CR_SNGSTRT                LPTIM_CR_SNGSTRT_Msk
#define LPTIM_CR_CNTSTRT_Pos            (2U)
#define LPTIM_CR_CNTSTRT_Msk            (0x1UL << LPTIM_CR_CNTSTRT_Pos)
#define LPTIM_CR_CNTSTRT                LPTIM_CR_CNTSTRT_Msk
#define LPTIM_CR_COUNTRST_Pos           (3U)
#define LPTIM_CR_COUNTRST_Msk           (0x1UL << LPTIM_CR_COUNTRST_Pos)
#define LPTIM_CR_COUNTRST               LPTIM_CR_COUNTRST_Msk

/******************* Bit definition for LPTIM_CMP register ********************/
#define LPTIM_CMP_CMP_Pos               (0U)
#define LPTIM_CMP_CMP_Msk               (0xFFFFFFUL << LPTIM_CMP_CMP_Pos)
#define LPTIM_CMP_CMP                   LPTIM_CMP_CMP_Msk

/******************* Bit definition for LPTIM_ARR register ********************/
#define LPTIM_ARR_ARR_Pos               (0U)
#define LPTIM_ARR_ARR_Msk               (0xFFFFFFUL << LPTIM_ARR_ARR_Pos)
#define LPTIM_ARR_ARR                   LPTIM_ARR_ARR_Msk

/******************* Bit definition for LPTIM_CNT register ********************/
#define LPTIM_CNT_CNT_Pos               (0U)
#define LPTIM_CNT_CNT_Msk               (0xFFFFFFUL << LPTIM_CNT_CNT_Pos)
#define LPTIM_CNT_CNT                   LPTIM_CNT_CNT_Msk

/******************* Bit definition for LPTIM_RCR register ********************/
#define LPTIM_RCR_REP_Pos               (0U)
#define LPTIM_RCR_REP_Msk               (0xFFUL << LPTIM_RCR_REP_Pos)
#define LPTIM_RCR_REP                   LPTIM_RCR_REP_Msk

#endif