#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/conn.c
#   上游 blob: 66a5f24f5d196961207e40f59fd5b3f63f24134a
#   为什么   : (1) get_conn_ready() 里 CONTAINER_OF 反推的 conn 可能是空 → 判空
#              (2) 连接进入 DISCONNECTED 时把 conn_ready 上的节点摘掉（根治）
#   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
#   版本漂移自查:
#     git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/conn.c
#     git -C external/zblue/zblue diff -- subsys/bluetooth/host/conn.c
#   机制：CMake 在本目录下调用本脚本，就地改写 zblue 的构建副本（上游 git 不动）。
# ---------------------------------------------------------------------------
"""两处改动，同一个文件：

(一) 判空（安全网）
    现场（2026-09-20 12:14，快照 n574）：

        kind=assert  file=arm_memfault.c:136  msg=panic
        pid=35 name=ble_companion pri=8 st=3 lock=2
        cfsr=00000082（PRECISERR+BFARVALID）  bfar=00000124

    `cfsr=0x82` 是精确数据总线错误，`bfar` 正好是一个**成员偏移**（0x124）——
    "空指针 + 偏移"，不是野指针（野指针的 bfar 会是垃圾值加偏移）。用当时的 ELF
    解 `pc/lr` 得到 `dont_have_methods()`（conn.c:922），调用点是
    `CHECKIF(dont_have_methods(conn))`（conn.c:965），而 conn 来自
    `CONTAINER_OF(node, struct bt_conn, _conn_ready)`。这条路径上没有任何校验。

(二) 摘链（根治）
    `_conn_ready` 的用法在本树只有四处（结构体字段 / 挂链 conn.c:884 / 消费
    get_conn_ready 两处），而"按节点从链表摘"的助手 `sys_slist_find_and_remove`
    在别处用过、**从没用在这张表上** ⇒ 连接挂上之后、若在 TX 路径消费它之前就断开，
    节点**永久留在表上**；且 `_conn_ready_lock` 只在被消费时才清（conn.c:977），
    那块内存复用时 `!atomic_set(...,1)` 返回假、不会重挂，旧节点却仍指着它 ——
    下一次 peek_head→CONTAINER_OF 就解引用到已清零的 conn（正是 (一) 的现场形状）。
    所以在"进入 DISCONNECTED"那个分支里补一次对称的摘链（atomic_set 返回旧值，
    只有确实还挂着时才摘）。
"""

from __future__ import annotations

import sys
from pathlib import Path

GUARD_MARK = "MYVENDOR_CONN_READY_GUARD"
UNLINK_MARK = "MYVENDOR_CONN_READY_UNLINK"

OLD = "\tstruct bt_conn *conn = CONTAINER_OF(node, struct bt_conn, _conn_ready);\n"

NEW = (
    OLD
    + "\n"
    + "\t/* "
    + GUARD_MARK
    + ": conn 可能是空。\n"
    + "\t * hdev->le.conn_ready 上若出现不属于活连接的节点（被写坏、或连接已释放但\n"
    + "\t * 节点仍挂在表上），CONTAINER_OF 反推出来的指针就是无效的，而下面\n"
    + "\t * dont_have_methods()/cannot_send_to_controller() 都会直接解引用它。\n"
    + "\t * 现场 2026-09-20 12:14：cfsr=0x82 bfar=0x124（空指针+成员偏移），\n"
    + "\t * 任务 ble_companion，panic 重启。这里先判空：这一次不发，下次再试。\n"
    + "\t */\n"
    + "\tif (conn == NULL) {\n"
    + "\t\tLOG_WARN(\"myvendor: conn_ready node is not a live conn, skipped\");\n"
    + "\t\treturn NULL;\n"
    + "\t}\n"
)

OLD2 = (
    "\t\t/* Notify disconnection and queue a dummy buffer to wake\n"
    "\t\t * up and stop the tx thread for states where it was\n"
    "\t\t * running.\n"
    "\t\t */\n"
)

NEW2 = (
    "\t\t/* "
    + UNLINK_MARK
    + ": 把 conn_ready 上的节点摘掉。\n"
    + "\t\t * 挂链只在 conn.c:884 一处，而摘链原先只在 get_conn_ready() 消费时发生\n"
    + "\t\t * ⇒ 连接在 TX 路径消费它之前就断开的话，节点会永久留在表上；那块内存\n"
    + "\t\t * 复用时 _conn_ready_lock 还是 1（不会重挂），旧节点却仍指着它，下一次\n"
    + "\t\t * peek_head→CONTAINER_OF 就解引用到已清零的 conn（现场 bfar=0x124 正是\n"
    + "\t\t * 成员偏移）。atomic_set 返回旧值 ⇒ 只有确实还挂着时才摘。\n"
    + "\t\t */\n"
    + "\t\tif (atomic_set(&conn->_conn_ready_lock, 0)) {\n"
    + "\t\t\tsys_slist_find_and_remove(&conn->hdev->le.conn_ready,\n"
    + "\t\t\t\t\t\t  &conn->_conn_ready);\n"
    + "\t\t}\n"
    + "\n"
    + OLD2
)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_conn_ready_null.py SRC DST", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    text = src.read_text(encoding="utf-8")

    changed = False

    if GUARD_MARK not in text:
        if OLD not in text:
            print("conn.c: CONTAINER_OF(_conn_ready) pattern not found", file=sys.stderr)
            return 1
        text = text.replace(OLD, NEW, 1)
        print("conn.c: conn_ready NULL guard added")
        changed = True

    if UNLINK_MARK not in text:
        if OLD2 not in text:
            print("conn.c: DISCONNECTED-notify pattern not found", file=sys.stderr)
            return 1
        text = text.replace(OLD2, NEW2, 1)
        print("conn.c: conn_ready unlink on disconnect added")
        changed = True

    dst.write_text(text, encoding="utf-8")
    if not changed:
        print("conn.c: already patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
