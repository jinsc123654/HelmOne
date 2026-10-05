/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef IPC_HW_PORT_H
#define IPC_HW_PORT_H

#include "register.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_HW_CH_NUM (1)
#define IPC_HW_QUEUE_NUM (8)

#define IPC_LH_HW_QUEUE_0 (0)
#define IPC_LH_HW_QUEUE_1 (1)
#define IPC_LH_HW_QUEUE_2 (2)
#define IPC_LH_HW_QUEUE_3 (3)
#define IPC_LH_HW_QUEUE_4 (4)
#define IPC_LH_HW_QUEUE_5 (5)
#define IPC_LH_HW_QUEUE_6 (6)
#define IPC_LH_HW_QUEUE_7 (7)

int HCPU2LCPU_IRQHandler(int irq, FAR void *context, FAR void *arg);

#ifdef __cplusplus
}
#endif

#endif
