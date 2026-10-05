#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/buf.c
#   上游 blob: b7ed19303be295dd715d12cec982433d396f2496
#   为什么   : 缓冲区 K_FOREVER 改成有上限的轮询（否则卡死不可杀）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/hci_core.c
#   上游 blob: ab23d63b2da40fe5c909e0c083f7fe34a12d197d
#   为什么   : 同上（控制流侧）
#   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
#   版本漂移自查:
#     git -C external/zblue/zblue rev-parse HEAD:<上面的上游文件>   # 与对应 blob 比对
#     git -C external/zblue/zblue diff -- <上面的上游文件>
#   机制：CMake 在本目录下调用本脚本，就地改写 zblue 的构建副本（上游 git 不动）。
# ---------------------------------------------------------------------------
"""Rewrite zblue port/lib/net_buf/buf.c so a buffer wait can not run forever.

Symptom (2026-09-17 board log): the controller stops answering at t=562 s
(no HCI event at all afterwards, `rxage` grows without bound, the last
command `txop=0x0406` HCI_Disconnect never gets a response).  Nothing
notices, because sf32lb52_bt_hci_rx_stalled() only counts *failed* RX
callbacks and there were none.

Then ble_companion wedges:

    companion_tick -> bt_gatts_notify -> gatt_notify
      -> bt_att_create_pdu -> bt_att_chan_create_pdu
      -> bt_l2cap_create_pdu_timeout(..., K_FOREVER)   (att.c, default branch)
      -> net_buf_alloc -> k_lifo_get -> k_queue_poll -> k_poll
      -> k_sem_take(K_FOREVER) -> nxsem_wait_uninterruptible()

The L2CAP/ATT TX pool only gets buffers back when the controller sends
Number Of Completed Packets, which it never will again, so the pool drains
and the last notify blocks forever.  That wait can NOT be cancelled:

  * `nxsem_wait_uninterruptible()` re-sleeps on every EINTR.
  * `task_delete()` never reaches nxtask_terminate(): nxnotify_cancellation()
    returns true (deferred cancellation, and tl_cpcount == 0 because the
    blocked API is nxsem_wait, not libc sem_wait), so task_delete() returns
    SUCCESS while leaving the task in TSTATE_WAIT_SEM.
  * CONFIG_SIG_DEFAULT is not set in this build, so SIGKILL has no terminate
    action at all.

Net effect: the task is unkillable by construction, board_restart_ble_companion()
returns before its LCPU force reset (which only runs on the success path),
skip_sync stays set, and BLE is dead until reboot.  bt_hci_cmd_create() uses
the same net_buf_alloc(..., K_FOREVER) for the command pool (hci_core.c:386),
so every other BT thread can wedge the same way.

Fix: when the pool is empty and the caller asked for K_FOREVER, poll with
K_NO_WAIT instead, giving up after MYVENDOR_NET_BUF_WAIT_MS, and also give up
immediately while sf32lb52_bt_hci_skip_sync() is set (i.e. while the board is
recovering the LCPU - the same bail-out patch_hci_core.py applies to the HCI
command wait).  Callers already handle NULL: bt_att_create_pdu() propagates it
as -ENOMEM, bt_gatts_notify() returns an error, companion_tick() logs
"status notify failed" and keeps running - so the heartbeat keeps flowing and
the recovery path stays reachable.

Note the finite-timeout path (else branch) is left alone; it depends on a
per-TCB waitdog, and a clipped watchdog list can still drop it.  The polling
loop here needs no watchdog to make progress.

Assumes the pool uses the fixed allocator (net_buf_fixed_cb), so the
`data_alloc()` further down can not block: that is true for the BT pools
(att_pool / acl pools are NET_BUF_POOL_*_DEFINE'd).  A heap-backed pool would
still block in data_alloc() on the timeout computed from `end`.

CMake runs this at configure time and compiles the copy instead of upstream
buf.c (same pattern as patch_hci_core.py).  Do not copy the whole file into
git.
"""

from __future__ import annotations

import sys
from pathlib import Path

# MYVENDOR BUILD: patch_net_buf_timeout.py 补丁 subsys/bluetooth/host/buf.c@b7ed19303be2, subsys/bluetooth/host/hci_core.c@ab23d63b2da4 -- 缓冲区 K_FOREVER 改成有上限的轮询（否则卡死不可杀） | 同上（控制流侧）
print('my_vendor: override patch patch_net_buf_timeout.py -> subsys/bluetooth/host/buf.c@b7ed19303be2, subsys/bluetooth/host/hci_core.c@ab23d63b2da4 -- 缓冲区 K_FOREVER 改成有上限的轮询（否则卡死不可杀） | 同上（控制流侧）')

# --- 1. constants + the skip_sync bail-out hook -----------------------------

ANCHOR_WARN_INTERVAL = "#if CONFIG_NET_BUF_WARN_ALLOC_INTERVAL > 0\n"

DECL = (
    "#include <stdbool.h> /* myvendor: for the sf32lb52_bt_hci_skip_sync() decl */\n"
    "\n"
    "/* myvendor: bound the wait for a free buffer (see patch_net_buf_timeout.py) */\n"
    "#define MYVENDOR_NET_BUF_POLL_MS 10u\n"
    "#define MYVENDOR_NET_BUF_WAIT_MS 2000u\n"
    "extern bool sf32lb52_bt_hci_skip_sync(void);\n"
    "\n"
    "/* 诊断走 syslog 而不是 LOG_ERR：本 build 没开 CONFIG_LOG，zblue 的\n"
    " * LOG_* 全被折叠掉（现场验证：打完补丁的固件里连格式串都搜不到），\n"
    " * 而\'pool empty, giving up\'恰恰是排查这次卡死唯一的一手证据。\n"
    " * 不 include <syslog.h>：它的 LOG_* 宏会和 zephyr/logging/log.h 打架；\n"
    " * 数字 3 就是 nuttx 的 LOG_ERR。\n"
    " */\n"
    "extern void syslog(int priority, const char *format, ...);\n"
    "#define MYVENDOR_LOG_ERR(fmt, ...) syslog(3, fmt, ##__VA_ARGS__)\n"
    "\n"
    "/* 池空日志：带上**池容量**（`buf_count`，普通字段，不用 CONFIG_NET_BUF_POOL_USAGE）\n"
    " * —— 拿去对 .config 里的 BT_BUF_CMD_TX_COUNT / BT_L2CAP_TX_BUF_COUNT /\n"
    " * BT_BUF_ACL_TX_COUNT 就能认出是哪个池（保留 pool 指针供回查）。\n"
    " *\n"
    " * 并**节流**：syslog 会阻塞在控制台 UART 上，在 BT 线程里打风暴本身就是风险\n"
    " * （现场 72 MHz + DVFS hop 时每 5 s 一条）。这里按命中次数节流，每\n"
    " * MYVENDOR_NET_BUF_WARN_EVERY 次放一行，其余计入 muted —— 不用时钟，\n"
    " * 也不引 k_uptime_get_32()（那种隐式声明在本 build 里只会在链接期炸）。\n"
    " */\n"
    "#define MYVENDOR_NET_BUF_WARN_EVERY 10u\n"
    "\n"
    "static uint32_t myvendor_buf_warn_n;\n"
    "\n"
    "static void myvendor_buf_warn(struct net_buf_pool *pool, uint32_t waited)\n"
    "{\n"
    "\tuint32_t n = myvendor_buf_warn_n++;\n"
    "\n"
    "\tif (n != 0 && (n % MYVENDOR_NET_BUF_WARN_EVERY) != 0) {\n"
    "\t\treturn;\n"
    "\t}\n"
    "\n"
    '\tMYVENDOR_LOG_ERR("myvendor net_buf: pool %p count=%u empty %u ms, giving up (n=%u)",\n'
    "\t\t\t pool, (unsigned int)pool->buf_count, (unsigned int)waited,\n"
    "\t\t\t (unsigned int)n);\n"
    "}\n"
    "\n"
)

# --- 2. make the module's own LOG_ERR visible -------------------------------

OLD_LEVEL = "#define LOG_LEVEL CONFIG_NET_BUF_LOG_LEVEL\n"
NEW_LEVEL = (
    "/* myvendor: force ERR level so the \"pool empty\" line is not compiled\n"
    " * out together with CONFIG_NET_BUF_LOG (CONFIG_NET_BUF_LOG_LEVEL is not\n"
    " * set on this board, which would evaluate to 0 and drop every LOG_*).\n"
    " */\n"
    "#define LOG_LEVEL 1\n"
)

# --- 3. net_buf_alloc_len(): K_FOREVER -> bounded poll ----------------------

OLD_ALLOC = (
    "\tbuf = k_lifo_get(&pool->free, timeout);\n"
    "#endif\n"
    "\tif (!buf) {\n"
    '\t\tNET_BUF_ERR("%s():%d: Failed to get free buffer", func, line);\n'
    "\t\treturn NULL;\n"
    "\t}\n"
)

NEW_ALLOC = (
    "\t/* myvendor: never wait forever for a buffer.\n"
    "\t *\n"
    "\t * K_FOREVER here becomes an uninterruptible wait that SIGTERM,\n"
    "\t * SIGKILL and task_delete() can not break, so whatever thread runs\n"
    "\t * out of buffers is stuck for good. Poll instead and give up, so the\n"
    "\t * caller sees -ENOMEM and stays alive; bail out early while the LCPU\n"
    "\t * is being recovered.\n"
    "\t */\n"
    "\tif (K_TIMEOUT_EQ(timeout, K_FOREVER)) {\n"
    "\t\tuint32_t waited = 0;\n"
    "\n"
    "\t\tfor (;;) {\n"
    "\t\t\tbuf = k_lifo_get(&pool->free, K_NO_WAIT);\n"
    "\t\t\tif (buf != NULL) {\n"
    "\t\t\t\tbreak;\n"
    "\t\t\t}\n"
    "\n"
    "\t\t\tif (sf32lb52_bt_hci_skip_sync()) {\n"
    '\t\t\t\tMYVENDOR_LOG_ERR("myvendor net_buf: pool %p abandoned (recovering)", pool);\n'
    "\t\t\t\treturn NULL;\n"
    "\t\t\t}\n"
    "\n"
    "\t\t\tif (waited >= MYVENDOR_NET_BUF_WAIT_MS) {\n"
    "\t\t\t\tmyvendor_buf_warn(pool, waited);\n"
    "\t\t\t\treturn NULL;\n"
    "\t\t\t}\n"
    "\n"
    "\t\t\tk_msleep(MYVENDOR_NET_BUF_POLL_MS);\n"
    "\t\t\twaited += MYVENDOR_NET_BUF_POLL_MS;\n"
    "\t\t}\n"
    "\t} else {\n"
    "\t\tbuf = k_lifo_get(&pool->free, timeout);\n"
    "\t}\n"
    "#endif\n"
    "\tif (!buf) {\n"
    '\t\tNET_BUF_ERR("%s():%d: Failed to get free buffer", func, line);\n'
    "\t\treturn NULL;\n"
    "\t}\n"
)


def _replace_once(text: str, old: str, new: str, what: str) -> str:
    """Replace @p old exactly once; already-patched text is left alone."""
    if new in text:
        return text

    n = text.count(old)
    if n == 0:
        raise SystemExit(f"port/lib/net_buf/buf.c: {what} pattern not found")
    if n > 1:
        raise SystemExit(
            f"port/lib/net_buf/buf.c: {what} pattern matched {n} times, want 1"
        )

    return text.replace(old, new, 1)

def _myvendor_stamp(path):
    """在被改写出来的副本里留三行标记（约定见 vela_override/CMakeLists.txt 顶部）。

    为什么要写进**生成的文件**：真正编译的是这份副本（build/myvendor_zblue/myvendor_*），
    只有它自己带标记，构建输出里才会出现 `note: '#pragma message: myvendor override compiled(patch): …'`，
    一眼看出"这份 .o 是补丁产物、基于哪个上游版本"。用 `#pragma message`（note/info）而不是
    `#pragma GCC warning`：量产构建会关警告（`-w` / 全局压制），note 关不掉；裸 `#warning` 更不行
    —— 抽换件 target 带 `-Wno-cpp`，会被静默掉（2026-09-19 实测）。
    """
    _script = 'patch_net_buf_timeout.py'
    _up = 'external/zblue/zblue / subsys/bluetooth/host/buf.c'
    _blob = 'b7ed19303be2'
    _why = '缓冲区 K_FOREVER 改成有上限的轮询（否则卡死不可杀）'
    _sym = 'patch_net_buf_timeout'
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
        print("usage: patch_net_buf_timeout.py SRC DST", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    text = src.read_text(encoding="utf-8")

    text = _replace_once(text, ANCHOR_WARN_INTERVAL,
                         DECL + ANCHOR_WARN_INTERVAL, "declaration block")
    text = _replace_once(text, OLD_LEVEL, NEW_LEVEL, "LOG_LEVEL")
    text = _replace_once(text, OLD_ALLOC, NEW_ALLOC, "net_buf_alloc_len")

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8")
    _myvendor_stamp(dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
