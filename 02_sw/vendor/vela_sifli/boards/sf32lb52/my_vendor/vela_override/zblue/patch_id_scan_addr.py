#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/id.c
#   上游 blob: e2e8f4824df（见版本漂移自查）
#   为什么   : `LE Set Random Address` 在外设链路已建立/正广播时会被控制器拒绝
#              （HCI 返回 -EACCES），上游只对"正在扫描/正在发起连接"两个 flag
#              放行 ⇒ 手机上连着的时候要起观测者（扫描）就整条失败。
#              板级策略：把 -EACCES 一律当"保留当前身份地址继续扫描"。
#   写入时 HEAD: 见 git -C external/zblue/zblue rev-parse HEAD
#   版本漂移自查:
#     git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/id.c
#     git -C external/zblue/zblue diff -- subsys/bluetooth/host/id.c   # 应为空
#   机制：CMake 在本目录下调用本脚本，就地改写 zblue 的构建副本（上游 git 不动）。
# ---------------------------------------------------------------------------
"""一处改动：把 -EACCES 的放行条件从"扫描中/发起中"两条 flag 放宽到无条件。

上游原文：

    err = bt_id_set_private_addr(hdev, BT_ID_DEFAULT);
    if (err == -EACCES && (atomic_test_bit(hdev->flags, BT_DEV_SCANNING) ||
                           atomic_test_bit(hdev->flags, BT_DEV_INITIATING))) {
        LOG_WRN("Set random addr failure ignored in scan/init state");
        return 0;
    } else if (err) {
        return err;
    }

现实是手机与本机维持着一条外设链路（`role=peripheral`）时要开扫描：控制器同样
以 -EACCES 拒绝 `LE Set Random Address`，而两个 flag 都不置位 ⇒ 返回错误 ⇒
观测者起不来（日志里表现为 `bt_le_scan_start` 直接失败、传感器一帧都收不到）。
放行之后要显式改回 `own_addr_type`：既然拿不到新的随机地址，就用**当前身份**
（`hdev->id_addr[BT_ID_DEFAULT].type`）去扫，否则会把一个无效的
RPA_OR_RANDOM 类型交给 `LE Set Scan Parameters`，控制器报 Invalid HCI Command
Parameters —— 从"起不来"变成"起不来还更晚"。

保留 `return 0`（而不是直接掉进下面的错误分支）：这就是"降级但成功"的本意。
"""

import sys
from pathlib import Path

MARK = "Set random addr disallowed, scan with identity"

OLD = (
    "\t\tif (err == -EACCES && (atomic_test_bit(hdev->flags, BT_DEV_SCANNING) ||\n"
    "\t\t\t\t       atomic_test_bit(hdev->flags, BT_DEV_INITIATING))) {\n"
    '\t\t\tLOG_WRN("Set random addr failure ignored in scan/init state");\n'
)

NEW = (
    "\t\tif (err == -EACCES) {\n"
    "\t\t\t/* LE Set Random Address is disallowed while advertising,\n"
    "\t\t\t * scanning, initiating, or (on some controllers) while a\n"
    "\t\t\t * peripheral link is up.  Keep the current identity and\n"
    "\t\t\t * still start the observer.\n"
    "\t\t\t */\n"
    '\t\t\tLOG_WRN("Set random addr disallowed, scan with identity");\n'
    "\t\t\tif (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_PUBLIC) {\n"
    "\t\t\t\t*own_addr_type = BT_HCI_OWN_ADDR_PUBLIC;\n"
    "\t\t\t} else {\n"
    "\t\t\t\t*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;\n"
    "\t\t\t}\n"
)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_id_scan_addr.py SRC DST", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    text = src.read_text(encoding="utf-8")

    if MARK in text:
        dst.write_text(text, encoding="utf-8")
        print("id.c: already patched")
        return 0

    n = text.count(OLD)
    if n != 1:
        print(
            f"id.c: -EACCES anchor matched {n} times (expected 1)"
            f" -- upstream drifted, refusing to guess",
            file=sys.stderr,
        )
        return 1

    text = text.replace(OLD, NEW, 1)
    dst.write_text(text, encoding="utf-8")
    print("id.c: -EACCES relaxed to keep broadcast identity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
