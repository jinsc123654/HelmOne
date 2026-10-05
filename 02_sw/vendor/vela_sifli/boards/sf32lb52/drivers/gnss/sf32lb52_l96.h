/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/drivers/gnss/sf32lb52_l96.h
 *
 * u-blox MAX-M10S-00B-01 on USART2
 * (PA31 TX / PA32 RX / PA43 GNSS_PWR).
 *
 * Official low power (MAX-M10S Integration Manual): cut module VCC.
 * This board uses PA43 (GNSS_PWR). Host USART2 stays open.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __SF32LB52_L96_H
#define __SF32LB52_L96_H

#include <nuttx/config.h>
#include <stdbool.h>

#if defined(CONFIG_BOARD_L96_GNSS)

/** Board module ordering part number. */
#define SF32LB52_GNSS_MOD_PN    "MAX-M10S-00B-01"

/* MTP / LittleFS folder for AssistNow and module nav dumps.
 * Phone writes mga_<unix-utc>.ubx (also accept mga_YYYYMMDDTHHMMSSZ.ubx);
 * firmware dump_out uses the same name. Inject picks the newest mga_*.ubx.
 * Keep at most 5 mga_*.ubx files (oldest deleted). dump.ubx is a legacy fallback.
 */

#define SF32LB52_GNSS_EPH_DIR   "/mnt/lfs/eph"
#define SF32LB52_GNSS_EPH_FILE  SF32LB52_GNSS_EPH_DIR "/mga.ubx"
#define SF32LB52_GNSS_EPH_DUMP  SF32LB52_GNSS_EPH_DIR "/dump.ubx"
#define SF32LB52_GNSS_EPH_TMP   SF32LB52_GNSS_EPH_DIR "/mga.tmp"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sf32lb52_l96_init(void);
/** @brief 切 MAX-M10S-00B-01 VCC（PA43）。主机 USART2 保持 open。 */
int sf32lb52_l96_power(bool on);
/** @brief 恢复 USART2 脚：PA31 TX / PA32 RX。iowrite / 其它复用后可能被改掉。 */
void sf32lb52_l96_uart_pins(void);

#endif /* CONFIG_BOARD_L96_GNSS */

#endif /* __SF32LB52_L96_H */
