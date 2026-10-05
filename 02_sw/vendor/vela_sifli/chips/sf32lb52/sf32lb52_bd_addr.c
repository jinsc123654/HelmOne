/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/sf32lb52_bd_addr.c
 *
 * Chip-unique public BD_ADDR from eFuse UID. NAND/SD boards have no NOR
 * factory OTP MAC (CFG_SUPPORT_NON_OTP). Follow SiFli: UID v2 then v1.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "sf32lb52_bd_addr.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>

#include "bf0_hal.h"

#define SF32LB52_EFUSE_UID_LEN 16

static bool mac_is_valid(const uint8_t *mac)
{
  uint8_t any = 0;
  uint8_t all = 0xff;
  int i;

  for (i = 0; i < SF32LB52_BD_ADDR_LEN; i++)
    {
      any |= mac[i];
      all &= mac[i];
    }

  return any != 0 && all != 0xff;
}

int sf32lb52_bd_addr_from_efuse(uint8_t addr[SF32LB52_BD_ADDR_LEN])
{
  uint8_t uid[SF32LB52_EFUSE_UID_LEN];
  int32_t nread;
  uint32_t i;

  if (addr == NULL)
    {
      return -EINVAL;
    }

  (void)HAL_EFUSE_Init();
  nread = HAL_EFUSE_Read(0, uid, SF32LB52_EFUSE_UID_LEN);
  if (nread != SF32LB52_EFUSE_UID_LEN)
    {
      syslog(LOG_ERR, "sf32lb52 eFuse UID read failed: %ld\n",
             (long)nread);
      return -ENODEV;
    }

  for (i = 0; i < SF32LB52_EFUSE_UID_LEN; i++)
    {
      if (uid[i] != 0)
        {
          break;
        }
    }

  if (i >= SF32LB52_EFUSE_UID_LEN)
    {
      syslog(LOG_ERR, "sf32lb52 eFuse UID is empty\n");
      return -ENODATA;
    }

  if (uid[7] == 0xa5)
    {
      uint8_t sum = (uint8_t)(uid[0] + uid[1] + uid[2] +
                              uid[3] + uid[4] + uid[5]);
      if (sum == uid[6])
        {
          memcpy(addr, uid, SF32LB52_BD_ADDR_LEN);
          addr[0] &= (uint8_t)~0x01;
          if (mac_is_valid(addr))
            {
              syslog(LOG_INFO,
                     "sf32lb52 MAC eFuse v2 "
                     "%02X:%02X:%02X:%02X:%02X:%02X\n",
                     addr[5], addr[4], addr[3],
                     addr[2], addr[1], addr[0]);
              return 0;
            }
        }
    }

  {
    uint32_t lower_part;
    uint32_t lot_part;
    uint32_t higher_part = 0;
    uint32_t base = 36;
    uint64_t mac;

    lower_part = uid[8] | ((uint32_t)uid[9] << 8) |
                 ((uint32_t)(uid[10] & 0x07) << 16);
    lot_part = ((uid[10] & 0xf8) >> 3) |
               ((uint32_t)(uid[11] & 0x7f) << 5) |
               ((uint32_t)uid[12] << 12) |
               ((uint32_t)uid[13] << 20) |
               ((uint32_t)(uid[14] & 0x03) << 28);

    for (i = 0; i < 5; i++)
      {
        if (i == 0)
          {
            higher_part += (lot_part & 0x3f) % 36;
          }
        else
          {
            higher_part += ((lot_part & 0x3f) % 36) * base;
            base *= 36;
          }

        lot_part >>= 6;
      }

    mac = (uint64_t)(lower_part & 0xfffff) |
          (((uint64_t)higher_part & 0xfffffff) << 20);
    memcpy(addr, &mac, SF32LB52_BD_ADDR_LEN);
    addr[0] &= (uint8_t)~0x01;
    if (!mac_is_valid(addr))
      {
        return -EINVAL;
      }

    syslog(LOG_INFO,
           "sf32lb52 MAC eFuse UID "
           "%02X:%02X:%02X:%02X:%02X:%02X\n",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return 0;
  }
}
