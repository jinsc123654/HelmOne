/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/include/sf32lb52_bd_addr.h
 *
 * Public BD_ADDR from eFuse UID (same algorithm as the BT controller NVDS).
 * HCI byte 0 is LSB. BLE and USB/MTP both use this so the -XXXX suffix
 * matches.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __SF32LB52_BD_ADDR_H
#define __SF32LB52_BD_ADDR_H

#include <stdint.h>

#define SF32LB52_BD_ADDR_LEN 6

#ifdef __cplusplus
extern "C"
{
#endif

/* 0 on success; addr is 6 bytes, byte 0 = LSB (HCI order). */

int sf32lb52_bd_addr_from_efuse(uint8_t addr[SF32LB52_BD_ADDR_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* __SF32LB52_BD_ADDR_H */
