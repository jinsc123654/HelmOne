#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/scan.c
#   上游 blob: 8d6ff66b5ff（见版本漂移自查）
#   为什么   : `hdev->scan_ctx` 在"适配器正在拆栈 / HCI 复位还没回来"的窗口里是
#              NULL（bt_dev_free→memset 清零，只有 hci_reset_complete→bt_scan_reset
#              或 bt_finalize_init 会重新赋值），而下面四处都会解引用它。
#   写入时 HEAD: 见 git -C external/zblue/zblue rev-parse HEAD
#   版本漂移自查:
#     git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/scan.c
#     git -C external/zblue/zblue diff -- subsys/bluetooth/host/scan.c   # 应为空
#   机制：CMake 在本目录下调用本脚本，就地改写 zblue 的构建副本（上游 git 不动）。
# ---------------------------------------------------------------------------
"""四处判空，同一个文件、同一个根因：

现场（2026-09-18 20:35，快照 n5xx）：

    kind=assert  file=arm_memfault.c  msg=panic
    task=ble_companion
    cfsr=00000082（PRECISERR+BFARVALID）  mmfar=00000004
    pc=bt_scan_softreset        scan.c:111
    lr=bt_le_scan_stop_mc       scan.c:1841

`cfsr=0x82` 是精确数据总线错误、`mmfar` 正好是一个**成员偏移**（0x4 = 结构体
第二个字段），也就是"空指针 + 成员偏移"的签名。这一处当时已经补了判空。

**但同一个调用链上还有第二处、以及两个同族入口没补**：

  (a) bt_le_scan_stop_mc() 尾部（scan.c:1856）又裸写了一次
      `hdev->scan_ctx->scan_dev_found_cb = NULL;` —— softreset 已经返回了，
      接着就撞同一块内存。当时没炸只是运气（走的是同一条路径）。
  (b) bt_le_scan_user_remove()（scan.c:555）入口没有任何校验，函数体
      `atomic_clear_bit(hdev->scan_ctx->scan_state.scan_flags, flag)` 和
      `scan_update()`（scan.c:467 `k_mutex_lock(&hdev->scan_ctx->...)`）
      都直接解引用。它是**从 SAL 层排队的工作项**最容易落到的入口
      （8 个调用点，含 hci_core.c 的三处 conn-complete 收尾）。
  (c) bt_le_scan_user_add() 同族入口：`scan_check_if_state_allowed()`
      （scan.c:517）第一行就 `atomic_test_bit(hdev->scan_ctx->...)`。

返回值口径（不是随意的）：
  * user_remove 判空返回 **0**。语义上"要把扫描停掉"这件事在 scan_ctx 为 NULL
    时已经成立（没有扫描器实例），返回非零会被 SAL/上层当成一次可重试的失败 ——
    本仓的历史教训就是拿错误码去驱动重试会变成风暴。删掉的三个调用点
    （hci_core.c:1106/1609/1707）全是 best-effort，多打一条 LOG_WRN 就够。
  * user_add 判空返回 **-ENODEV**：这里返回 0 反而危险 —— 上层会以为"广播/扫描
    已经置上了 flag"，从而不再重试。宁可报错，也不要一个假的成功。
  * 两处判空都打 LOG_WRN（带各自唯一串），方便 grep/计数这条窗口被踩到的次数；
    该窗口本身很短，不构成刷屏源。
"""

import sys
from pathlib import Path

# --- (b) user_remove 入口 -------------------------------------------------
MARK_B = "scan state remove skipped, scan ctx not ready"
OLD_B = (
    "int bt_le_scan_user_remove(struct bt_dev *hdev, enum bt_le_scan_user flag)\n"
    "{\n"
    "\tif (flag == BT_LE_SCAN_USER_NONE) {\n"
)
NEW_B = (
    "int bt_le_scan_user_remove(struct bt_dev *hdev, enum bt_le_scan_user flag)\n"
    "{\n"
    "\t/* 板级改动（my_vendor）：适配器拆栈 / HCI 复位未回的窗口里 scan_ctx 是\n"
    "\t * NULL，本函数与它调用的 scan_update() 都会解引用。见脚本头注释。 */\n"
    "\tif (hdev == NULL || hdev->scan_ctx == NULL) {\n"
    '\t\tLOG_WRN("scan state remove skipped, scan ctx not ready");\n'
    "\t\treturn 0;\n"
    "\t}\n"
    "\n"
    "\tif (flag == BT_LE_SCAN_USER_NONE) {\n"
)

# --- (c) user_add 入口 ----------------------------------------------------
MARK_C = "scan state add skipped, scan ctx not ready"
OLD_C = (
    "int bt_le_scan_user_add(struct bt_dev *hdev, enum bt_le_scan_user flag)\n"
    "{\n"
    "\tuint32_t err;\n"
    "\n"
    "\tif (flag == BT_LE_SCAN_USER_NONE) {\n"
)
NEW_C = (
    "int bt_le_scan_user_add(struct bt_dev *hdev, enum bt_le_scan_user flag)\n"
    "{\n"
    "\tuint32_t err;\n"
    "\n"
    "\t/* 板级改动（my_vendor）：同 user_remove —— scan_check_if_state_allowed()\n"
    "\t * 第一行就解引用 scan_ctx。这里返回 -ENODEV 而不是 0：返回 0 会让上层\n"
    "\t * 以为 flag 已经置上、不再重试。 */\n"
    "\tif (hdev == NULL || hdev->scan_ctx == NULL) {\n"
    '\t\tLOG_WRN("scan state add skipped, scan ctx not ready");\n'
    "\t\treturn -ENODEV;\n"
    "\t}\n"
    "\n"
    "\tif (flag == BT_LE_SCAN_USER_NONE) {\n"
)

# --- (原) softreset 判空，2026-09-18 就地改过，现收进脚本 -------------------
MARK_A = "scan softreset skipped, scan ctx not ready"
OLD_A = (
    "void bt_scan_softreset(struct bt_dev *hdev)\n"
    "{\n"
    "\thdev->scan_ctx->scan_dev_found_cb = NULL;\n"
)
NEW_A = (
    "void bt_scan_softreset(struct bt_dev *hdev)\n"
    "{\n"
    "\t/* 板级改动（my_vendor，见 docs/pitch/zblue-scan-stop-null-ctx.md）：\n"
    "\t * `hdev->scan_ctx` 只在 HCI_RESET 完成（hci_reset_complete → bt_scan_reset）\n"
    "\t * 或 bt_finalize_init 里被赋值，设备结构在初始化路径上会被 memset 清零。\n"
    "\t * 于是\"适配器已拆掉、复位还没回来\"这个窗口里再来一次 scan-stop，就是\n"
    "\t * hdev->scan_ctx == NULL 解引用 —— 实机 2026-09-18 20:35 的 panic 正是它\n"
    "\t * （MemManage cfsr=0x82 mmfar=0x4，pc=bt_scan_softreset scan.c:111，\n"
    "\t *  lr=bt_le_scan_stop_mc scan.c:1841，task=ble_companion）。\n"
    "\t *\n"
    "\t * 调用方可能来自 SAL 里排队的工作项（适配器 cycle 拆栈时队列还没清空），\n"
    "\t * 从上层堵不干净，所以在这里直接变 no-op。 */\n"
    "\tif (hdev == NULL || hdev->scan_ctx == NULL) {\n"
    '\t\tLOG_WRN("scan softreset skipped, scan ctx not ready");\n'
    "\t\treturn;\n"
    "\t}\n"
    "\n"
    "\thdev->scan_ctx->scan_dev_found_cb = NULL;\n"
)

# --- (a) stop_mc 尾部第二处解引用 -----------------------------------------
MARK_D = "同一窗口的第二处解引用"
OLD_D = (
    "\tbt_scan_softreset(hdev);\n"
    "\thdev->scan_ctx->scan_dev_found_cb = NULL;\n"
)
NEW_D = (
    "\tbt_scan_softreset(hdev);\n"
    "\n"
    "\t/* 同一窗口的第二处解引用（scan.c:1856）：softreset 已经判过 NULL，\n"
    "\t * 那次没 panic 只是因为同一路径，但这里同样不能裸写。 */\n"
    "\tif (hdev->scan_ctx != NULL) {\n"
    "\t\thdev->scan_ctx->scan_dev_found_cb = NULL;\n"
    "\t}\n"
)

EDITS = (
    ("user_remove NULL guard", MARK_B, OLD_B, NEW_B),
    ("user_add NULL guard", MARK_C, OLD_C, NEW_C),
    ("softreset NULL guard", MARK_A, OLD_A, NEW_A),
    ("stop_mc second deref guard", MARK_D, OLD_D, NEW_D),
)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_scan_stop_null.py SRC DST", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    text = src.read_text(encoding="utf-8")

    changed = False

    for desc, mark, old, new in EDITS:
        if mark in text:
            continue
        n = text.count(old)
        if n != 1:
            print(
                f"scan.c: anchor for {desc} matched {n} times (expected 1)"
                f" -- upstream drifted, refusing to guess",
                file=sys.stderr,
            )
            return 1
        text = text.replace(old, new, 1)
        print(f"scan.c: {desc} added")
        changed = True

    dst.write_text(text, encoding="utf-8")
    if not changed:
        print("scan.c: already patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
