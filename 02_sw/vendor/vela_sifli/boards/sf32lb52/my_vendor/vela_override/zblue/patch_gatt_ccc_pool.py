#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/gatt.c
#   上游 blob: 8e31d1cfa353aefeef17adc54d5a22d1d7e1a580
#   为什么   : 断开释放 CCC 槽 + 池满时回收已断开的 peer；CCC 池金丝雀 + 30 s 校验；CCC 写入/复位的原始字节取证；地址对不上时不把还在用的 CCC 打成 0
#   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
#   版本漂移自查:
#     git -C external/zblue/zblue rev-parse HEAD:<上面的上游文件>   # 与对应 blob 比对
#     git -C external/zblue/zblue diff -- <上面的上游文件>
#   机制：CMake 在本目录下调用本脚本，就地改写 zblue 的构建副本（上游 git 不动）。
# ---------------------------------------------------------------------------
"""Rewrite zblue gatt.c so the CCC pool can not be exhausted permanently.

Symptom (Companion 0xFF10): the phone connects, MTU / 0xFF11 / 0xFF1A all
read back fine, and then the FIRST CCC write - the one that enables
notifications on 0xFF12 - is rejected:

    setNotifyValue -> ATT 0x11 GATT_INSUFFICIENT_RESOURCES

The app gives up and the UI stays on "未连接" forever, retrying each
watchdog tick. On the device the only place that error is produced is

    cfg = find_ccc_cfg(NULL, ccc);
    if (!cfg) {
            LOG_WRN("No space to store CCC cfg");
            return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

`ccc->cfg[]` has BT_GATT_CCC_MAX == CONFIG_BT_MAX_PAIRED +
CONFIG_BT_MAX_CONN entries (1 + 4 = 5 on this board; CONFIG_BT_SETTINGS is
off, so there is no lazy-loading variant) and is keyed by peer address.

Entries are only released by disconnected_cb(), which upstream runs from
bt_gatt_disconnected() on ATT teardown. Two ways that leaks:

  * The peer is bonded. Upstream then deliberately keeps the entry so the
    subscription can be restored after reconnect - but the phone rotates
    its resolvable private address every few minutes, so each reconnect
    claims a NEW slot while the bonded one is never freed.
  * ATT teardown never runs for the link at all (HCI disconnect swallowed,
    stale controller handle, adapter cycle skipped) - the failure mode the
    ble_companion hci_disc / gatts_disc diagnostics watch for.

Either way the pool fills after a handful of reconnects and every later
subscription fails, permanently.

Two changes, neither of which costs anything for this service (its CCCDs
carry no ENCRYPT/AUTHEN, so nothing depends on subscriptions surviving a
disconnect, and the app re-subscribes on every connect):

  1. disconnected_cb() releases the slot unconditionally instead of keeping
     it for bonded peers.
  2. bt_gatt_attr_write_ccc() falls back to reclaiming a slot whose peer is
     no longer connected before reporting the pool as full, so a leak that
     already happened heals itself. gatt_ccc_changed() recomputes
     ccc->value from the remaining connected peers, so the summary stays
     consistent.

A second, unrelated defect (2026-09-18) is guarded in the same pass:

  3. The GATT callback list (hdev->gatt_ctx->callback_list, plus the
     gatt_ctx_pool it lives in) is canaried, and every walk of it validates
     each pointer before dereferencing it (myvendor_gatt_cb_list_check()).
     That list is 1-2 statics long and only this file writes it, yet a walk
     from bt_sal_gatt_client_disable() - which the framework runs on every
     adapter cycle ble_companion performs - followed a node whose next word
     had been overwritten with 0xfee7fee7 and panicked the board (MemManage
     DACCVIOL, mmfar=0xfee7fee7, arm_memfault.c:136). The guard turns that
     panic into an ERROR line naming the raw values, and the canaries say
     whether the overwrite came from a neighbouring object or from an
     arbitrary-address writer. See the block itself for the full findings.

CMake runs this at configure time and compiles the copy instead of upstream
gatt.c (same pattern as patch_hci_core.py). Do not copy the whole 7k-line
file into git.
"""

from __future__ import annotations

import sys
from pathlib import Path

# MYVENDOR BUILD: patch_gatt_ccc_pool.py 补丁 subsys/bluetooth/host/gatt.c@8e31d1cfa353 -- 断开释放 CCC 槽 + 池满时回收已断开的 peer；CCC 池金丝雀 + 30 s 校验；CCC 写入/复位的原始字节取证；地址对不上时不把还在用的 CCC 打成 0
print('my_vendor: override patch patch_gatt_ccc_pool.py -> subsys/bluetooth/host/gatt.c@8e31d1cfa353 -- 断开释放 CCC 槽 + 池满时回收已断开的 peer；CCC 池金丝雀 + 30 s 校验；CCC 写入/复位的原始字节取证；地址对不上时不把还在用的 CCC 打成 0')

# --- 1. disconnected_cb(): always drop the slot -----------------------------

OLD_KEEP = (
    "\t\t} else {\n"
    "\t\t\t/* Clear value if not paired */\n"
    "\t\t\tif (!bt_addr_le_is_bonded(conn->hdev, conn->id, &conn->le.dst)) {\n"
    "\t\t\t\tif (ccc == &sc_ccc) {\n"
    "\t\t\t\t\tsc_clear(conn);\n"
    "\t\t\t\t}\n"
    "\n"
    "\t\t\t\tclear_ccc_cfg(cfg);\n"
    "\t\t\t} else {\n"
    "\t\t\t\t/* Update address in case it has changed */\n"
    "\t\t\t\tbt_addr_le_copy(&cfg->peer, &conn->le.dst);\n"
    "\t\t\t}\n"
    "\t\t}\n"
)

NEW_KEEP = (
    "\t\t} else {\n"
    "\t\t\t/* myvendor: release the slot even when the peer is bonded.\n"
    "\t\t\t *\n"
    "\t\t\t * Upstream keeps it so a bonded peer gets its subscriptions\n"
    "\t\t\t * back after reconnecting, which is only sound while the\n"
    "\t\t\t * peer address is stable. The pool is keyed by peer address\n"
    "\t\t\t * and BT_GATT_CCC_MAX is small, so a peer that rotates its\n"
    "\t\t\t * resolvable private address (Android does) claims a fresh\n"
    "\t\t\t * slot on every reconnect and exhausts the pool for good -\n"
    "\t\t\t * after which every CCC write fails with ATT 0x11. The\n"
    "\t\t\t * Companion CCCDs need no pairing and the app re-subscribes\n"
    "\t\t\t * on connect, so nothing is lost by dropping it here.\n"
    "\t\t\t */\n"
    "\t\t\tif (ccc == &sc_ccc) {\n"
    "\t\t\t\tsc_clear(conn);\n"
    "\t\t\t}\n"
    "\n"
    "\t\t\tclear_ccc_cfg(cfg);\n"
    "\t\t}\n"
)

# --- 2. reclaim helper, inserted right after find_ccc_cfg() -----------------

# The tail of find_ccc_cfg() plus the head of the next function; unique in the
# file and keeps find_ccc_cfg() itself byte-identical.
FIND_CCC_TAIL = "\treturn NULL;\n}\n\n"
READ_CCC_DECL = "ssize_t bt_gatt_attr_read_ccc(struct bt_conn *conn,"

HELPER = (
    "/** myvendor: free a CCC slot held by a peer that is no longer connected.\n"
    " *\n"
    " * The pool is keyed by peer address and normally only released by\n"
    " * disconnected_cb(). A slot left behind by a bonded peer (see NEW_KEEP\n"
    " * above) or by a link whose ATT teardown was missed is unusable forever,\n"
    " * because no later connect can match it. Subscriptions are meaningless\n"
    " * for a peer that is not connected, so such a slot can always be reused.\n"
    " *\n"
    " * @param hdev Bluetooth device the write arrived on.\n"
    " * @param ccc  CCC attribute whose pool is full.\n"
    " * @return A cleared slot, or NULL if every slot belongs to a live peer.\n"
    " */\n"
    "static struct bt_gatt_ccc_cfg *reclaim_ccc_cfg(struct bt_dev *hdev,\n"
    "\t\t\t\t\t       struct _bt_gatt_ccc *ccc)\n"
    "{\n"
    "\tfor (size_t i = 0; i < ARRAY_SIZE(ccc->cfg); i++) {\n"
    "\t\tstruct bt_gatt_ccc_cfg *cfg = &ccc->cfg[i];\n"
    "\t\tstruct bt_conn *conn;\n"
    "\n"
    "\t\tif (bt_addr_le_eq(&cfg->peer, BT_ADDR_LE_ANY)) {\n"
    "\t\t\tcontinue;\n"
    "\t\t}\n"
    "\n"
    "\t\tconn = bt_conn_lookup_addr_le_mc(hdev->dev_id, cfg->id, &cfg->peer);\n"
    "\t\tif (conn != NULL) {\n"
    "\t\t\tbt_conn_unref(conn);\n"
    "\t\t\tcontinue;\n"
    "\t\t}\n"
    "\n"
    "\t\tLOG_WRN(\"Reclaiming CCC cfg of disconnected peer\");\n"
    "\n"
    "\t\tclear_ccc_cfg(cfg);\n"
    "\t\treturn cfg;\n"
    "\t}\n"
    "\n"
    "\treturn NULL;\n"
    "}\n"
    "\n"
)

# --- 3. bt_gatt_attr_write_ccc(): reuse a stale slot before failing ---------

OLD_FULL = (
    "\t\tcfg = find_ccc_cfg(NULL, ccc);\n"
    "\t\tif (!cfg) {\n"
    '\t\t\tLOG_WRN("No space to store CCC cfg");\n'
    "\t\t\treturn BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);\n"
    "\t\t}\n"
)

NEW_FULL = (
    "\t\tcfg = find_ccc_cfg(NULL, ccc);\n"
    "\t\tif (!cfg) {\n"
    "\t\t\t/* myvendor: the pool is full. Failing here hands the client\n"
    "\t\t\t * a subscription it can never enable, so try reusing a slot\n"
    "\t\t\t * whose peer is gone first.\n"
    "\t\t\t */\n"
    "\t\t\tcfg = reclaim_ccc_cfg(conn->hdev, ccc);\n"
    "\t\t}\n"
    "\n"
    "\t\tif (!cfg) {\n"
    '\t\t\tLOG_WRN("No space to store CCC cfg");\n'
    "\t\t\treturn BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);\n"
    "\t\t}\n"
)

# --- 4. 取证：CCCD 写入的原始字节，以及"被复位成 0"的归属 --------------------
#
# 现场（2026-09-19）：App 报"0xFF16 订阅没生效 / FS 已收 0 帧"，而设备日志里
# 只有 `ble_companion: fs subscribe -> 0`。**那行 0 有两个来源**：
#   (a) 客户端真的写了 0x0000；
#   (b) zblue 自己把这个 CCC 复位成 0 —— 见下面 `!value_used` 那段
#       （对端已走/未配对时 `ccc->value = 0` 后再调同一个 cfg_changed 回调）。
# 两处各打一行，加上 ble_companion 回调里那行，谁写了什么就一目了然。

CCC_WRITE_OLD = (
    "\tif (len < sizeof(uint16_t)) {\n"
    "\t\tvalue = *(uint8_t *)buf;\n"
    "\t} else {\n"
    "\t\tvalue = sys_get_le16(buf);\n"
    "\t}\n"
)

CCC_WRITE_NEW = (
    "\tif (len < sizeof(uint16_t)) {\n"
    "\t\tvalue = *(uint8_t *)buf;\n"
    "\t} else {\n"
    "\t\tvalue = sys_get_le16(buf);\n"
    "\t}\n"
    "\n"
    "\t/* myvendor: ATT 写进来的原样两字节。和下面 cfg_changed 回调里\n"
    "\t * ble_companion 那行（`xx subscribe -> N`）对起来看，就能分清\n"
    "\t * \"客户端写了 0\" 和 \"设备把 1 处理丢了\"。\n"
    "\t */\n"
    "\tsyslog(MYVENDOR_GATT_LOG_INFO,\n"
    "\t       \"gatt: ccc write h=0x%04x raw=%02x%02x v=%u len=%u\\n\",\n"
    "\t       attr->handle,\n"
    "\t       len >= 1 ? (unsigned)((const uint8_t *)buf)[0] : 0u,\n"
    "\t       len >= 2 ? (unsigned)((const uint8_t *)buf)[1] : 0u,\n"
    "\t       (unsigned)value, (unsigned)len);\n"
    "\n"
    "\t/* myvendor 2026-09-20：**只有客户端真的写了 CCC，才把这一位报给上层**。\n"
    "\t * 聚合重算（gatt_ccc_changed）已经改成不回调 —— 它会因为手机轮换 RPA\n"
    "\t * 后多出一条\"值为 0 但连接查得到\"的槽而报 0，把真实订阅打成\"未订阅\"。\n"
    "\t * 写路径这里拿到的就是客户端写的原值，语义正是上层要的那个。\n"
    "\t */\n"
    "\tif (ccc->cfg_changed) {\n"
    "\t\tccc->cfg_changed(attr, value);\n"
    "\t}\n"
)

CCC_RESET_OLD = (
    "\t/* If all values are now disabled, reset value while disconnected */\n"
    "\tif (!value_used) {\n"
    "\t\tccc->value = 0U;\n"
    "\t\tif (ccc->cfg_changed) {\n"
    "\t\t\tccc->cfg_changed(attr, ccc->value);\n"
    "\t\t}\n"
)

CCC_RESET_NEW = (
    "\t/* If all values are now disabled, reset value while disconnected */\n"
    "\tif (!value_used) {\n"
    "\t\tccc->value = 0U;\n"
    "\t\t/* myvendor: 给\"订阅怎么变成 0 了\"定性用 —— 它下面的回调会带着\n"
    "\t\t * value=0 打到 ble_companion 的 `xx subscribe -> 0`。看到这一行\n"
    "\t\t * 就说明是**本栈自己复位的**（对端已走/未配对），不是客户端写的。\n"
    "\t\t */\n"
    "\t\tsyslog(MYVENDOR_GATT_LOG_INFO,\n"
    "\t\t       \"gatt: ccc reset h=0x%04x (no connected user)\\n\",\n"
    "\t\t       attr->handle);\n"
    "\t\tif (ccc->cfg_changed) {\n"
    "\t\t\tccc->cfg_changed(attr, ccc->value);\n"
    "\t\t}\n"
)

# --- 5. gatt_ccc_changed(): 地址对不上时不要把"还有人用"的 CCC 打成 0 ---------
#
# 现场（2026-09-19，App 报"0xFF16 的通知没订上 / FS 已收 0 帧"）：
#   [147.49] gatt: ccc write h=0x001c raw=0100 v=1 → fs subscribe -> 1   ← 客户端写 1，收到 1
#   （随后一串）              fs subscribe -> 0 ×N   ← 没有任何 ccc write / ccc reset 行
#   fs_disc … sent=0       ccc=0                     ← 于是设备一帧不发
# 那些 0 来自本函数：它把每个 slot 的对端地址拿去查"现在有没有这条连接"
# （`bt_conn_lookup_addr_le_mc`），查不到就不算这个 slot 的贡献 —— 于是聚合值
# 算成 0，回调告诉上层"没人订阅了"。未绑定的对端（我们的 CCCD 特意不加
# AUTHEN，不触发配对 ⇒ 没有 bond ⇒ 轮换的 RPA 无法解析成同一身份）以及重连
# 窗口里地址对不上的槽，都会掉进这个坑。而**客户端真的写 0** 走的是
# `bt_gatt_attr_write_ccc()`（那里 slot 一定匹配得上，我们不动），**对端断开**
# 走下面 `!value_used` 那条 reset 路（那里 `ccc->value` 先被置 0）—— 两条都不
# 受本改动影响。
#
# 判据：这一轮**一个 slot 都没匹配到活连接**、而原值非 0 时，保留原值并留一行
# 取证日志（`gatt: ccc recompute miss`）。真实退订/断开仍会照常把值降下来。

CCC_CHANGED_OLD = (
    "\tint i;\n"
    "\tuint16_t value = 0x0000;\n"
    "\n"
    "\tfor (i = 0; i < ARRAY_SIZE(ccc->cfg); i++) {\n"
    "\t\t/* `ccc->value` shall be a summary of connected peers' CCC values, but\n"
    "\t\t * `ccc->cfg` can contain entries for bonded but not connected peers.\n"
    "\t\t */\n"
    "\t\tstruct bt_conn *conn = bt_conn_lookup_addr_le_mc(hdev->dev_id, ccc->cfg[i].id, &ccc->cfg[i].peer);\n"
    "\n"
    "\t\tif (conn) {\n"
    "\t\t\tif (ccc->cfg[i].value > value) {\n"
    "\t\t\t\tvalue = ccc->cfg[i].value;\n"
    "\t\t\t}\n"
    "\n"
    "\t\t\tbt_conn_unref(conn);\n"
    "\t\t}\n"
    "\t}\n"
    "\n"
    "\tLOG_DBG(\"ccc %p value 0x%04x\", ccc, value);\n"
    "\n"
    "\tif (value != ccc->value) {\n"
    "\t\tccc->value = value;\n"
    "\t\tif (ccc->cfg_changed) {\n"
    "\t\t\tccc->cfg_changed(attr, value);\n"
    "\t\t}\n"
    "\t}\n"
)

CCC_CHANGED_NEW = (
    "\tint i;\n"
    "\tuint16_t value = 0x0000;\n"
    "\tbool matched = false; /* myvendor: 有没有 slot 匹配到活连接 */\n"
    "\n"
    "\tfor (i = 0; i < ARRAY_SIZE(ccc->cfg); i++) {\n"
    "\t\t/* `ccc->value` shall be a summary of connected peers' CCC values, but\n"
    "\t\t * `ccc->cfg` can contain entries for bonded but not connected peers.\n"
    "\t\t */\n"
    "\t\tstruct bt_conn *conn = bt_conn_lookup_addr_le_mc(hdev->dev_id, ccc->cfg[i].id, &ccc->cfg[i].peer);\n"
    "\n"
    "\t\tif (conn) {\n"
    "\t\t\tmatched = true;\n"
    "\n"
    "\t\t\tif (ccc->cfg[i].value > value) {\n"
    "\t\t\t\tvalue = ccc->cfg[i].value;\n"
    "\t\t\t}\n"
    "\n"
    "\t\t\tbt_conn_unref(conn);\n"
    "\t\t}\n"
    "\t}\n"
    "\n"
    "\t/* myvendor: 一个 slot 都没匹配到、而原值非 0 —— 说明地址簿记跟丢了这条\n"
    "\t * 连接（未绑定对端的 RPA 轮换、重连窗口都算），不是客户端退订。保留\n"
    "\t * 原值，否则 notify 会在还有人听的时候自己停掉（2026-09-19 现场）。\n"
    "\t * 真退订走写路径、断开走 reset 路径，都不经过这里。\n"
    "\t */\n"
    "\tif (!matched && ccc->value != 0) {\n"
    "\t\tsyslog(MYVENDOR_GATT_LOG_INFO,\n"
    "\t\t       \"gatt: ccc recompute miss h=0x%04x keep=0x%04x\\n\",\n"
    "\t\t       attr->handle, (unsigned)ccc->value);\n"
    "\t\treturn;\n"
    "\t}\n"
    "\n"
    "\t/* myvendor 2026-09-20：**这里不再回调上层**。\n"
    "\t *\n"
    "\t * 现场（新日志一次抓齐）：客户端写 1 到 FS 的 CCC（`gatt: ccc write h=0x001c\n"
    "\t * raw=0100 v=1`），同一刻上层却收到 `fs ccc h=0x0009 raw=0000 -> 0` ——\n"
    "\t * 写路径和 reset 路径都没有日志，唯一可能就是这条聚合重算：手机换 RPA\n"
    "\t * 重连后 `ccc->cfg` 里多了一条**值为 0 但连接查得到**的新槽（Android 对\n"
    "\t * 已缓存的 CCC 不会重写），于是 matched=true、value=0 ⇒ 回调报\"没人订阅\"\n"
    "\t * ⇒ FS 一帧不发、事务被收掉，用户看到\"0xFF16 莫名奇妙掉订阅\"。\n"
    "\t *\n"
    "\t * `ccc->value` 只是**本栈内部**的汇总，不该拿它当\"客户端刚写了什么\"。\n"
    "\t * 真正代表客户端动作的只有两处，都已各自回调：写路径（本次补上，见\n"
    "\t * CCC_WRITE_NEW）、断开复位（reset 路径）。这里只维护汇总值。\n"
    "\t */\n"
    "\tif (value != ccc->value) {\n"
    "\t\tsyslog(MYVENDOR_GATT_LOG_INFO,\n"
    "\t\t       \"gatt: ccc recompute h=0x%04x %04x->%04x (no callback)\\n\",\n"
    "\t\t       attr->handle, (unsigned)ccc->value, (unsigned)value);\n"
    "\t\tccc->value = value;\n"
    "\t}\n"
)

# --- 3. canary the GATT context pool, validate the callback list -------------

POOL_DECL = "} gatt_ctx_pool[CONFIG_BT_NUM_CTLRS];\n"

POOL_GUARDED = """};

/* myvendor: canary the GATT context pool.
 *
 * 2026-09-18 07:53, crash note n004: MemManage DACCVIOL (cfsr=0x82 =
 * DACCVIOL|MMARVALID, mmfar=0xfee7fee7) in bt_gatt_cb_unregister_mc(), taken
 * from bt_sal_gatt_client_disable() <- if_gattc_shutdown() <- the adapter
 * cycle ble_companion had just requested for adv failures. The faulting
 * instruction was the list walk's "ldr r3, [r3]" with r3 = 0xfee7fee7, so
 * either callback_list.head or a node's next word held that value.
 *
 * Nothing in this file can produce it: sys_slist_append() and
 * sys_slist_find_and_remove() write node addresses or NULL, and 0xfee7fee7
 * (two ARM "b ." trap instructions) is not a constant anywhere in the image.
 * A 4-byte field of the list was therefore overwritten from outside. The pool
 * and the two zblue_gatt_callbacks statics sit in .bss/.data wedged between
 * the ATT/L2CAP net_buf slabs and the HCI pools, which is where an overflow
 * from a neighbour would land.
 *
 * These two words tell those cases apart: a broken canary means a neighbour
 * overran into the pool (and which side), an intact pair means the word came
 * from an arbitrary-address writer (wild pointer, stale DMA descriptor, ...).
 * Detecting without repairing would be pointless though - the walk itself is
 * what panics - so see myvendor_gatt_cb_list_check() below.
 */
#define MYVENDOR_GATT_CTX_MAGIC 0x47415454u /* "GATT" */

/* The port's logging/log.h #undef's LOG_ERR (it becomes the logging macro),
 * so the syslog severity has to be spelled out. 3 is LOG_ERR, which
 * nuttx/drivers/syslog/vsyslog.c renders as "[ ERROR]" - the token
 * myvendor_diaglog keeps, so these lines reach /mnt/kv/diag.
 */
#define MYVENDOR_GATT_LOG_ERR 3

/* 6 is LOG_INFO (nuttx/drivers/syslog). Used for the CCC wire trace below:
 * it is normal traffic, not an error, and it has to be visible on the
 * console (the /mnt/kv diag file only keeps WARNING and above).
 */
#define MYVENDOR_GATT_LOG_INFO 6

static struct {
	uint32_t pre;
	struct bt_dev_gatt_ctx body[CONFIG_BT_NUM_CTLRS];
	uint32_t post;
} gatt_ctx_guard = {
	.pre = MYVENDOR_GATT_CTX_MAGIC,
	.post = MYVENDOR_GATT_CTX_MAGIC,
};

#define gatt_ctx_pool gatt_ctx_guard.body

/**
 * myvendor: is @p node a plausible callback-list node?
 *
 * Every legitimate node is a static inside the firmware image, so a pointer
 * outside the RAM the firmware owns means the field was overwritten.
 */
static bool myvendor_gatt_node_sane(const sys_snode_t *node)
{
	uintptr_t a = (uintptr_t)node;

	return (a >= 0x20000000u && a < 0x20080000u) || /* HCPU SRAM */
	       (a >= 0x60000000u && a < 0x68000000u);   /* PSRAM / heap */
}

/**
 * myvendor: validate the GATT callback list, repairing it when corrupt.
 *
 * Called before every walk of callback_list. A corrupt list used to be fatal:
 * bt_sal_gatt_client_disable() runs on every adapter cycle, and
 * sys_slist_find_and_remove() dereferences whatever next pointer it finds.
 *
 * @param ctx   GATT context to check; NULL is reported and refused.
 * @param where caller tag, so a log line names the path that found it.
 * @return true when the list is walkable; false when it was corrupt and has
 *         just been reinitialised (the caller must not walk it then).
 */
static bool myvendor_gatt_cb_list_check(struct bt_dev_gatt_ctx *ctx,
					const char *where)
{
	sys_snode_t *node;
	unsigned int hops = 0;

	if (ctx == NULL) {
		syslog(MYVENDOR_GATT_LOG_ERR,
		       "gatt: cb list %s with null ctx\\n", where);
		return false;
	}

	/* The pool is a static, so a legitimate ctx is always one of its
	 * elements. A wild pointer would fault inside this very check - i.e.
	 * turn the guard into the crash it exists to prevent - so refuse to
	 * dereference it and say what it was instead.
	 */
	if ((uintptr_t)ctx < (uintptr_t)&gatt_ctx_guard.body[0] ||
	    (uintptr_t)ctx >=
		(uintptr_t)&gatt_ctx_guard.body[CONFIG_BT_NUM_CTLRS]) {
		syslog(MYVENDOR_GATT_LOG_ERR,
		       "gatt: cb list %s with wild ctx=%p (pool %p..%p)\\n",
		       where, (void *)ctx, (void *)&gatt_ctx_guard.body[0],
		       (void *)&gatt_ctx_guard.body[CONFIG_BT_NUM_CTLRS]);
		return false;
	}

	if (gatt_ctx_guard.pre != MYVENDOR_GATT_CTX_MAGIC ||
	    gatt_ctx_guard.post != MYVENDOR_GATT_CTX_MAGIC) {
		syslog(MYVENDOR_GATT_LOG_ERR,
		       "gatt: ctx pool canary broken %s pre=%08x post=%08x\\n",
		       where, (unsigned int)gatt_ctx_guard.pre,
		       (unsigned int)gatt_ctx_guard.post);
		gatt_ctx_guard.pre = MYVENDOR_GATT_CTX_MAGIC;
		gatt_ctx_guard.post = MYVENDOR_GATT_CTX_MAGIC;
	}

	for (node = ctx->callback_list.head; node != NULL; node = node->next) {
		if (!myvendor_gatt_node_sane(node) || ++hops > 8u) {
			syslog(MYVENDOR_GATT_LOG_ERR,
			       "gatt: cb list corrupt %s head=%p tail=%p node=%p hops=%u\\n",
			       where, (void *)ctx->callback_list.head,
			       (void *)ctx->callback_list.tail, (void *)node,
			       hops);
			sys_slist_init(&ctx->callback_list);
			return false;
		}
	}

	return true;
}

/**
 * myvendor: periodic checkpoint for the pool canaries.
 *
 * myvendor_gatt_cb_list_check() only reports a broken canary when something
 * happens to register/unregister a callback or walk the list; this lets a
 * caller that ticks on its own date an overflow to a window. Deliberately
 * reads the two canary words only - walking the list from a foreign thread
 * would race the Bluetooth stack's own edits.
 */
void myvendor_gatt_ctx_checkpoint(void)
{
	if (gatt_ctx_guard.pre != MYVENDOR_GATT_CTX_MAGIC ||
	    gatt_ctx_guard.post != MYVENDOR_GATT_CTX_MAGIC) {
		syslog(MYVENDOR_GATT_LOG_ERR,
		       "gatt: ctx pool canary broken (checkpoint) pre=%08x post=%08x\\n",
		       (unsigned int)gatt_ctx_guard.pre,
		       (unsigned int)gatt_ctx_guard.post);
		gatt_ctx_guard.pre = MYVENDOR_GATT_CTX_MAGIC;
		gatt_ctx_guard.post = MYVENDOR_GATT_CTX_MAGIC;
	}
}
"""

CB_REGISTER_OLD = "\tsys_slist_append(&hdev->gatt_ctx->callback_list, &cb->node);\n"
CB_REGISTER_NEW = (
    "\t/* myvendor: never walk a corrupted list - see the check. */\n"
    '\tif (myvendor_gatt_cb_list_check(hdev->gatt_ctx, "register")) {\n'
    "\t\tsys_slist_append(&hdev->gatt_ctx->callback_list, &cb->node);\n"
    "\t}\n"
)

CB_UNREGISTER_OLD = (
    "\tsys_slist_find_and_remove(&hdev->gatt_ctx->callback_list, &cb->node);\n"
)
CB_UNREGISTER_NEW = (
    "\t/* myvendor: never walk a corrupted list - see the check. */\n"
    '\tif (myvendor_gatt_cb_list_check(hdev->gatt_ctx, "unregister")) {\n'
    "\t\tsys_slist_find_and_remove(&hdev->gatt_ctx->callback_list, &cb->node);\n"
    "\t}\n"
)

# The MTU walk runs per connection (att_mtu_updated), the same exposure.
CB_MTU_OLD = (
    "\tSYS_SLIST_FOR_EACH_CONTAINER(&conn->hdev->gatt_ctx->callback_list, cb, node) {\n"
)
CB_MTU_NEW = (
    "\t/* myvendor: same exposure as register/unregister; skip when broken. */\n"
    '\tif (!myvendor_gatt_cb_list_check(conn->hdev->gatt_ctx, "mtu changed")) {\n'
    "\t\treturn;\n"
    "\t}\n"
    "\n" + CB_MTU_OLD
)


def _replace_once(
    text: str, old: str, new: str, what: str, marker: str | None = None
) -> str:
    """Replace @p old exactly once; already-patched text is left alone.

    @p marker is what distinguishes a patched file from an unpatched one and
    defaults to @p new. Pass one explicitly when @p new does not contain
    @p old, or re-running the script would not find @p old at all.
    """
    if (marker or new) in text:
        # Already patched. Checked before counting: NEW_FULL contains the
        # whole OLD_FULL block, so counting first would re-apply it.
        return text

    n = text.count(old)
    if n == 0:
        raise SystemExit(f"gatt.c: {what} pattern not found")
    if n > 1:
        raise SystemExit(f"gatt.c: {what} pattern matched {n} times, want 1")

    return text.replace(old, new, 1)

def _myvendor_stamp(path):
    """在被改写出来的副本里留三行标记（约定见 vela_override/CMakeLists.txt 顶部）。

    为什么要写进**生成的文件**：真正编译的是这份副本（build/myvendor_zblue/myvendor_*），
    只有它自己带标记，构建输出里才会出现 `note: '#pragma message: myvendor override compiled(patch): …'`，
    一眼看出"这份 .o 是补丁产物、基于哪个上游版本"。用 `#pragma message`（note/info）而不是
    `#pragma GCC warning`：量产构建会关警告（`-w` / 全局压制），note 关不掉；裸 `#warning` 更不行
    —— 抽换件 target 带 `-Wno-cpp`，会被静默掉（2026-09-19 实测）。
    """
    _script = 'patch_gatt_ccc_pool.py'
    _up = 'external/zblue/zblue / subsys/bluetooth/host/gatt.c'
    _blob = '8e31d1cfa353'
    _why = '断开释放 CCC 槽 + 池满时回收已断开的 peer；CCC 池金丝雀 + 30 s 校验；CCC 写入/复位的原始字节取证；地址对不上时不把还在用的 CCC 打成 0'
    _sym = 'patch_gatt_ccc_pool'
    _tag = "vela_override/" + _script + " -- 上游 " + _up + "@" + _blob + " -- " + _why
    _marker = (
        "\n/* myvendor override (build-time patch): " + _script + "\n"
        " *   上游 " + _up + " @ " + _blob + "\n"
        " *   原因 " + _why + " */\n"
        '#pragma message("myvendor override compiled(patch): ' + _tag + ' ")\n'
        "const char myvendor_override_patch_marker_" + _sym + "[]\n"
        '    __attribute__((used, section(".myvendor_marker"))) = "'
        + _tag + '";\n'
    )
    path.write_text(path.read_text(encoding="utf-8") + _marker, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_gatt_ccc_pool.py SRC DST", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    text = src.read_text(encoding="utf-8")

    text = _replace_once(text, OLD_KEEP, NEW_KEEP, "disconnected_cb")
    text = _replace_once(
        text,
        FIND_CCC_TAIL + READ_CCC_DECL,
        FIND_CCC_TAIL + HELPER + READ_CCC_DECL,
        "find_ccc_cfg",
    )
    text = _replace_once(text, OLD_FULL, NEW_FULL, "CCC pool full")
    text = _replace_once(text, CCC_WRITE_OLD, CCC_WRITE_NEW, "CCC wire trace")
    text = _replace_once(text, CCC_RESET_OLD, CCC_RESET_NEW, "CCC reset trace")
    text = _replace_once(
        text, CCC_CHANGED_OLD, CCC_CHANGED_NEW, "gatt_ccc_changed keep-on-miss"
    )

    # The marker has to be part of the *new* pool declaration: POOL_GUARDED
    # does not contain POOL_DECL, so a second run would otherwise look for a
    # line that is already gone and abort the whole patch.
    text = _replace_once(
        text,
        POOL_DECL,
        POOL_GUARDED,
        "gatt_ctx_pool",
        marker="MYVENDOR_GATT_CTX_MAGIC",
    )
    text = _replace_once(text, CB_REGISTER_OLD, CB_REGISTER_NEW, "cb register")
    text = _replace_once(
        text, CB_UNREGISTER_OLD, CB_UNREGISTER_NEW, "cb unregister"
    )
    text = _replace_once(text, CB_MTU_OLD, CB_MTU_NEW, "cb mtu walk")

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8")
    _myvendor_stamp(dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
