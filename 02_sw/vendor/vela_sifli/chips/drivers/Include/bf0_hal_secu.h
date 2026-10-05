/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __BF0_HAL_SECU_H
#define __BF0_HAL_SECU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "bf0_hal_def.h"

/** @addtogroup BF0_HAL_Driver
  * @{
  */

/** @defgroup SECU SECU
  * @brief SECU HAL module driver
  * @{
  */

/* Exported types ------------------------------------------------------------*/
/** @defgroup SECU_Exported_Types SECU Exported Types
  * @{
  */

/** Security supported SECU groups definition  */
typedef enum
{
    SECU_GROUP_INVALID = 0,

    /**SECU1 groups*/
    SECU_GROUP_HPMST,   /*!< HPSYS master group*/
    SECU_GROUP_HPSLV,   /*!<HPSYS slave  group*/
    SECU_GROUP_HPMPI1,  /*!<HPSYS MPI1   group*/
    SECU_GROUP_HPMPI2,  /*!<HPSYS MPI2   group*/
    SECU_GROUP_HPRAM,   /*!<HPSYS SRAM   group*/

    /**SECU2  groups*/
    SECU_GROUP_LPMST,   /*!<LPSYS master group*/
    SECU_GROUP_LPSLV,   /*!<LPSYS slave  group*/
    SECU_GROUP_LPRAM,   /*!<LPSYS SRAM   group*/
} SECU_GROUP_TYPE;


/** Security supported modules definition */
typedef enum
{
    SECU_MOD_INVALID = 0,

    /**SECU1 modules*/
    SECU_MOD_PTC1,    /*!<Master & Slave*/
    SECU_MOD_DMAC1,   /*!<Master & Slave */
    SECU_MOD_USBC,    /*!<Master */
    SECU_MOD_AES,     /*!<Master  & Slave*/
    SECU_MOD_LCDC1,   /*!<Master */
    SECU_MOD_EPIC,    /*!<Master */
    SECU_MOD_EXTDMA,  /*!<Master */
    SECU_MOD_HCPU,    /*!<Master */

    SECU_MOD_TRNG,    /*!<Slave */
    SECU_MOD_EFUSE,  /*!<Slave */


    /**SECU2 modules*/
    SECU_MOD_PTC2,   /*!<Master & Slave*/
    SECU_MOD_DMAC2,  /*!<Master & Slave */

    SECU_MOD_LCPU,  /*!<Master */

} SECU_MODULE_TYPE;


/** Security supported memory definition */
typedef enum
{
    SECU_MEM_INVALID = 0,
    /*SECU1*/
    SECU_MEM_MPI1,
    SECU_MEM_MPI2,
    SECU_MEM_HPSYS_RAM0,
    SECU_MEM_HPSYS_RAM1,
    SECU_MEM_HPSYS_RAM2,

    /*SECU2*/
    SECU_MEM_LPSYS_RAM0,
    SECU_MEM_LPSYS_RAM1,

} SECU_MEM_TYPE;


/** Memory attribution rang:[start,end] */
typedef struct
{
    uint32_t start; /*!< start address which must align to #SECU_MEM_MIN_BLOCK, */
    uint32_t end;   /*!< end address align to #SECU_MEM_MIN_BLOCK*/
    uint32_t flag;  /*!< Can be value of @ref SECU_Flag */
} SECU_MemAttr_Type;




/**
  * @}
  */

/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/** @defgroup SECU_Exported_Macros SECU Exported Macros
  * @{
  */

/** @defgroup SECU_Flag  Security setting flags
  * @{
  */
#define SECU_FLAG_NONE  (0x00UL)           /* !< None security mode and none privilege mode*/
#define SECU_FLAG_PRIV  (0x01UL)           /* !< Privilege mode */
#define SECU_FLAG_SECU  (0x02UL)           /* !< Security  mode */
/**
  * @}
  */


/** @defgroup SECU_Role  Security role
  * @{
  */
#define SECU_ROLE_MASTER    (0x01UL)         /*!< Master role */
#define SECU_ROLE_SLAVE     (0x02UL)         /*!< Slave role */
/**
  * @}
  */


#define SECU_MEM_MIN_BLOCK (1024)           /*!< Minimal security block size*/
#define SECU_MEMRange_ALIGN(v) ((v)&0xFFFFFC00)
#define SECU_MEMRange_ALIGN_UP(v) SECU_MEMRange_ALIGN((v) + SECU_MEM_MIN_BLOCK - 1)


/**
  * @}
  */

/* Exported functions --------------------------------------------------------*/
/** @defgroup SECU_Exported_Functions SECU Exported Functions
  * @{
  */


/**
  * @brief  Config a module's security.
  * @param  module  - module control by SECU, the value is in type of #SECU_MODULE_TYPE
  * @param  role   - apply security on module while it act as this role @ref SECU_Role
  * @param  flag - module's security attribute, @ref SECU_Flag
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_SECU_SetAttr(SECU_MODULE_TYPE module, uint32_t role, uint32_t flag);

/**
  * @brief  Config memory's security.
  * @param  memory_type  - memory control by SECU, the value can be one of #SECU_MEM_TYPE
  * @param  attrs        - memory's security attributes
  * @param  attrs_cnt     - length of attrs
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_SECU_SetMemoryAttr(SECU_MEM_TYPE memory_type, const SECU_MemAttr_Type *attrs, uint32_t attrs_cnt);


/**
  * @brief  Security groups lock
  * @param  group  - group of SECU, the value can be one of @ref SECU_GROUP_TYPE
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_SECU_Lock(SECU_GROUP_TYPE group);

/**
  * @brief  Security groups apply
  * @param  group  - group of SECU, the value can be one of @ref SECU_GROUP_TYPE
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_SECU_Apply(SECU_GROUP_TYPE group);

/**
  * @brief  Security groups lock and apply
  * @param  group  - group of SECU, the value can be one of @ref SECU_GROUP_TYPE
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_SECU_ApplyAndLock(SECU_GROUP_TYPE group);




/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __BF0_HAL_SECU_H */