/**
 * @file sal_le_scan_interface.c
 * @brief 板级抽换 SAL 扫描：legacy GAP、重试与 ADV/scan HCI 串行化。
 *
 * CMake 去掉上游 sal_le_scan_interface.c。相对上游：bt_le_scan_start 遇 -EALREADY
 * 先 stop 再 start；scan_param.options=0 走 legacy（SF32 扩展扫描 Command Disallowed）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/****************************************************************************
 *  Copyright (C) 2024 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Board override: CMake 从 libbluetooth 去掉上游同名文件，改编本文件。
 * 路径：frameworks/connectivity/bluetooth/service/stacks/zephyr/sal_le_scan_interface.c
 * 上游变更后需对照 rebase。
 *
 * 相对上游的改动：
 * - bt_le_scan_start 遇 -EALREADY 先 stop 再 start（双角色停 ADV 后扫
 *   可能仍占着观察者）。
 * - scan_param.options 保持 0，走 legacy GAP（SF32 扩展扫描会
 *   Command Disallowed）。
 * - 与广播 SAL 共用 ticket 队列，禁止 libuv worker 并行执行 GAP HCI。
 ***************************************************************************/

#include <errno.h>

/* ---- 抽换件标记（2026-09-18）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件 —— 2026-09-18 曾有个抽换件其实根本不在编译库里（已剔除），
 *    改了半天没生效，就靠这种标记一眼看出来；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉），不依赖任何编译选项（有些目标带 -w，会把 #warning 压掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : frameworks/connectivity/bluetooth / service/stacks/zephyr/sal_le_scan_interface.c
 *   写入时 HEAD: 43945bc1d13f3494a79e3e1d79ca8312c4c0f6a0
 *   上游 blob  : 24d8275d5d8bee93096820b717bb263b52097f55     （git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/stacks/zephyr/sal_le_scan_interface.c 应等于它）
 *   为什么抽换 : -EALREADY 重试、options=0；与 adv 共用跨角色 ticket 队列，等待有界 2 s
 *   版本漂移自查:
 *     git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/stacks/zephyr/sal_le_scan_interface.c   # 与上面的 blob 比对
 *     git -C frameworks/connectivity/bluetooth diff -- service/stacks/zephyr/sal_le_scan_interface.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/bluetooth/sal_le_scan_interface.c -- 上游 frameworks/connectivity/bluetooth:service/stacks/zephyr/sal_le_scan_interface.c@24d8275d5d8b -- -EALREADY 重试、options=0；与 adv 共用跨角色 ticket 队列，等待有界 2 s")
const char myvendor_override_marker_bluetooth_sal_le_scan_interface_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/bluetooth/sal_le_scan_interface.c -- 上游 frameworks/connectivity/bluetooth:service/stacks/zephyr/sal_le_scan_interface.c@24d8275d5d8b -- -EALREADY 重试、options=0；与 adv 共用跨角色 ticket 队列，等待有界 2 s";
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>

#include "sal_interface.h"
#include "sal_le_scan_interface.h"
#include "service_loop.h"

#include "utils/log.h"

bool sf32lb52_bt_hci_skip_sync(void);

#ifdef CONFIG_BLUETOOTH_BLE_SCAN
#define STACK_CALL(func) zblue_##func

typedef void (*sal_func_t)(void* args);

typedef struct {
    bt_controller_id_t id;
    sal_func_t func;
    uint32_t radio_ticket;
} sal_scan_req_t;

typedef struct {
    char value[256];
    uint8_t length;
} le_eir_data_t;

static struct bt_le_scan_param scan_param;

/* 广播 SAL 优先提供公共 ticket 队列；scan-only 配置在本文件兜底。 */
#if defined(CONFIG_BLUETOOTH_BLE_ADV)
extern uint32_t myvendor_sal_radio_ticket_issue(void);
extern void myvendor_sal_radio_ticket_wait(uint32_t ticket);
extern void myvendor_sal_radio_ticket_done(void);
#else
static pthread_mutex_t g_radio_order_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_radio_order_cond = PTHREAD_COND_INITIALIZER;
static uint32_t g_radio_ticket_next;
static uint32_t g_radio_ticket_serving;

/* 等待上界：见 sal_le_advertise_interface.c 的同名实现（那边是正式提供者）。 */
#define MYVENDOR_SAL_TICKET_WAIT_MS 2000u

uint32_t myvendor_sal_radio_ticket_issue(void)
{
    uint32_t ticket;

    pthread_mutex_lock(&g_radio_order_lock);
    ticket = g_radio_ticket_next++;
    pthread_mutex_unlock(&g_radio_order_lock);
    syslog(LOG_INFO, "sal scan ticket n=%u serving=%u\n",
           (unsigned)ticket, (unsigned)g_radio_ticket_serving);
    return ticket;
}

void myvendor_sal_radio_ticket_wait(uint32_t ticket)
{
    uint32_t waited = 0;

    pthread_mutex_lock(&g_radio_order_lock);
    while (ticket != g_radio_ticket_serving) {
        struct timespec ts;

        /* 与 sal_le_advertise_interface.c 的同名实现一致：无限等待会让
         * 整条 BLE 栈永久停摆（见那边的注释），超时就强制推进本张 ticket。 */
        if (waited >= MYVENDOR_SAL_TICKET_WAIT_MS) {
            syslog(LOG_ERR,
                   "sal radio ticket %u stuck %u ms (serving=%u next=%u) — forcing\n",
                   (unsigned)ticket, (unsigned)waited,
                   (unsigned)g_radio_ticket_serving,
                   (unsigned)g_radio_ticket_next);
            g_radio_ticket_serving = ticket;
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_radio_order_cond, &g_radio_order_lock, &ts);
        waited += 1000u;
    }
    pthread_mutex_unlock(&g_radio_order_lock);
}

void myvendor_sal_radio_ticket_done(void)
{
    pthread_mutex_lock(&g_radio_order_lock);
    g_radio_ticket_serving++;
    pthread_cond_broadcast(&g_radio_order_cond);
    pthread_mutex_unlock(&g_radio_order_lock);
}
#endif

static sal_scan_req_t* sal_scan_req(bt_controller_id_t id, sal_func_t func)
{
    sal_scan_req_t* req = calloc(1, sizeof(sal_scan_req_t));

    if (!req) {
        BT_LOGE("%s, req malloc fail", __func__);
        return NULL;
    }

    req->id = id;
    req->func = func;

    return req;
}

static void sal_invoke_async(service_work_t* work, void* userdata)
{
    sal_scan_req_t* req = userdata;

    SAL_ASSERT(req);
    myvendor_sal_radio_ticket_wait(req->radio_ticket);
    req->func(req);
    myvendor_sal_radio_ticket_done();
    free(userdata);
}

static bt_status_t sal_send_req(sal_scan_req_t* req)
{
    if (!req) {
        BT_LOGE("%s, req null", __func__);
        return BT_STATUS_PARM_INVALID;
    }

    req->radio_ticket = myvendor_sal_radio_ticket_issue();
    if (!service_loop_work((void*)req, sal_invoke_async, NULL)) {
        BT_LOGE("%s, service_loop_work fail", __func__);
        myvendor_sal_radio_ticket_wait(req->radio_ticket);
        myvendor_sal_radio_ticket_done();
        free(req);
        return BT_STATUS_FAIL;
    }

    return BT_STATUS_SUCCESS;
}

static bool zblue_on_eir_found(struct bt_data* data, void* user_data)
{
    le_eir_data_t* eir = user_data;

    eir->value[eir->length++] = data->data_len + 1;
    eir->value[eir->length++] = data->type;
    memcpy(&eir->value[eir->length], data->data, data->data_len);
    eir->length += data->data_len;
    return true;
}

static void zblue_on_device_found(const bt_addr_le_t* addr, int8_t rssi, uint8_t type, struct net_buf_simple* ad)
{
    ble_scan_result_t result_info = { 0 };
    le_eir_data_t eir = { 0 };

    bt_data_parse(ad, zblue_on_eir_found, &eir);

    result_info.length = eir.length;
    result_info.adv_type = type;
    result_info.rssi = rssi;
    result_info.dev_type = BT_DEVICE_DEVTYPE_BLE;
    result_info.addr_type = addr->type;
    memcpy(&result_info.addr, &addr->a, sizeof(result_info.addr));

    scan_on_result_data_update(&result_info, eir.value);
}

static void STACK_CALL(start_scan)(void* args)
{
    int err;

    (void)args;
    if (sf32lb52_bt_hci_skip_sync()) {
        BT_LOGE("%s, skip HCI (recovering)", __func__);
        return;
    }

    scan_param.options = 0;
    err = bt_le_scan_start(&scan_param, zblue_on_device_found);
    if (err == -EALREADY) {
        (void)bt_le_scan_stop();
        err = bt_le_scan_start(&scan_param, zblue_on_device_found);
    }

    if (err) {
        BT_LOGE("%s, bt_le_scan_start fail, err:%d", __func__, err);
    }
}

static void STACK_CALL(stop_scan)(void* args)
{
    int err;

    (void)args;
    if (sf32lb52_bt_hci_skip_sync()) {
        return;
    }

    err = bt_le_scan_stop();
    if (err && err != -EALREADY) {
        BT_LOGE("%s, bt_le_scan_stop fail, err:%d", __func__, err);
    }
}

bt_status_t bt_sal_le_set_scan_parameters(bt_controller_id_t id, ble_scan_params_t* params)
{
    (void)id;
    memset(&scan_param, 0, sizeof(scan_param));
    scan_param.type = params->scan_type;
    scan_param.interval = params->scan_interval;
    scan_param.window = params->scan_window;
    scan_param.options = 0;

    return BT_STATUS_SUCCESS;
}

bt_status_t bt_sal_le_start_scan(bt_controller_id_t id)
{
    sal_scan_req_t* req;

    req = sal_scan_req(id, STACK_CALL(start_scan));
    if (!req) {
        BT_LOGE("%s, sal req fail", __func__);
        return BT_STATUS_NOMEM;
    }

    return sal_send_req(req);
}

bt_status_t bt_sal_le_stop_scan(bt_controller_id_t id)
{
    sal_scan_req_t* req;

    req = sal_scan_req(id, STACK_CALL(stop_scan));
    if (!req) {
        BT_LOGE("%s, sal req fail", __func__);
        return BT_STATUS_NOMEM;
    }

    return sal_send_req(req);
}
#endif /* CONFIG_BLUETOOTH_BLE_SCAN */
