#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : frameworks/connectivity/bluetooth / service/src/adapter_state.c
#   上游 blob: 6a5a1d8f0c67f4c2c9c6b0e1d1c9a2c5b6cdd6c9 (写入时 HEAD 见下)
#   为什么   :
#     adapter 状态机在「turn on 失败」这条路上**没有失败出口** ——
#     ble_turning_on_enter() 里 bt_sal_le_enable() 失败时只记一个 dfx，
#     **不做任何状态迁移**，于是 HSM 永久停在 ble_turning_on_state，而对外
#     记录的 adapter_state 仍是 OFF（因为 notify 只在成功分支调）。
#
#     而该状态只认 BLE_ENABLED / BLE_PROFILE_ENABLED 两个事件，它们都要求
#     zblue 的 ready 回调 —— bt_enable() 已经失败了，回调永远不会来；
#     两个 *_TIMEOUT 事件在 process_event 里是空的 `break;`；状态表又是扁平的
#     （state_t 无 parent，hsm_transition_to 是显式跳转）⇒ 新来的 SYS_TURN_ON
#     会被 process_event 判为不处理。
#
#     ⇒ **一次瞬时的 bt_enable() 失败 = 该次开机 BLE 彻底死亡**，重复
#     bt_adapter_enable() 也回不去。上层那套恢复阶梯（adapter cycle →
#     LCPU force reset → companion task restart）全都不碰这台 HSM，所以
#     现场是每 ~76 s 一轮的 companion respawn 死循环：
#       adapter not ready, state=0 → escalate → force reset →
#       task restart → 起来后原样复现（76 s ≈ HCI_WAIT 10 s + ADAPTER_WAIT 60 s）
#
#     修法：enable 失败时把状态机**退回 off_state**。hsm_transition_to() 会调
#     off_enter()，后者对 prev 非 NULL 的情况会
#     adapter_notify_state_change(prev, BT_ADAPTER_STATE_OFF) —— 对外状态
#     变回 OFF（真实），同时 HSM 回到可接受 TURN_ON_BLE 的状态 ⇒ 上层重试
#     才有意义。
#
#     安全性：hsm_transition_to() 是先把 current_state 指过去、再调 enter 的
#     （framework/common/state_machine.c），所以在 enter 里再迁移是安全的
#     （嵌套调用最终把 current 落在 off_state）。本板的
#     `on_adapter_state_changed` 回调只打日志（ble_companion.c:1055），
#     不会同步回调进状态机，没有重入风险。
#   写入时 HEAD: (见 git -C frameworks/connectivity/bluetooth log -1)
#   版本漂移自查:
#     git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/src/adapter_state.c
#     git -C frameworks/connectivity/bluetooth diff -- service/src/adapter_state.c
#   机制：CMake 配置期调用本脚本 → 生成到构建目录 → 按文件名顶掉 libbluetooth。
# ---------------------------------------------------------------------------
"""给 adapter 状态机的 turn-on 失败路径补一个失败出口（退回 off_state）。

症状：`ble_companion: adapter not ready, state=0 (expect ON or BLE_ON)` 永久
不消失，LCPU 复位与 companion 重启都救不回来。

脚本幂等：已打过的补丁跳过，可重复跑。
"""

from __future__ import annotations

import sys
from pathlib import Path

MARK = "MYVENDOR BUILD: patch_adapter_state.py v1"

# ---- 定位与替换 -------------------------------------------------------------
#
# 只碰 ble_turning_on_enter 这一个函数体：不动文件里任何其它状态，
# 改动面小到可以逐字 review。

SIG = "static void ble_turning_on_enter(state_machine_t* sm)"

# 锚点必须**连 `else` 一起吃掉**：只换 BT_DFX_OPEN_ERROR 那一行的话，原来的
# 裸 `else` 会留下来，和替换文本开头的大括号拼成 `else\n    else {` —— 语法错误。
# （第一版就是这么写的，standalone diff 抓到了。）
OLD_ELSE = (
    "    else\n"
    "        BT_DFX_OPEN_ERROR(BT_DFXE_LE_ENABLE_FAIL);\n"
)

NEW_ELSE = (
    "    else {\n"
    "        /* my_vendor: **enable 失败必须退出 turning-on 态。**\n"
    "         *\n"
    "         * 原来这里只记一个 dfx，不做任何迁移 —— 于是 HSM 永久停在\n"
    "         * ble_turning_on_state，而该状态只认 BLE_ENABLED /\n"
    "         * BLE_PROFILE_ENABLED（都要 zblue 的 ready 回调，而 bt_enable()\n"
    "         * 已经失败了），两个 *_TIMEOUT 又是空的 `break;`，状态表还是扁平的\n"
    "         * ⇒ **再调 bt_adapter_enable() 也回不去**，一次瞬时失败变成\n"
    "         * 整次开机 BLE 死亡（现场每 76 s 一轮 companion respawn）。\n"
    "         *\n"
    "         * 退回 off_state：off_enter() 会把对外状态改成 BT_ADAPTER_STATE_OFF\n"
    "         * （prev 非 NULL 时它自己会 notify），同时 HSM 重新可接受\n"
    "         * TURN_ON_BLE ⇒ 上层那套 cycle / 复位阶梯才有意义。\n"
    "         *\n"
    "         * hsm_transition_to() 先改 current_state 再调 enter，因此\n"
    "         * 在 enter 里嵌套迁移是安全的。 */\n"
    "        BT_DFX_OPEN_ERROR(BT_DFXE_LE_ENABLE_FAIL);\n"
    "        hsm_transition_to(sm, &off_state);\n"
    "    }\n"
)

OLD_NOLE = '    BT_LOGE("Not supported");'

NEW_NOLE = (
    "    /* my_vendor: 同上的理由 —— 没有 BLE 支持时也要退出 turning-on 态，\n"
    "     * 否则这台状态机一样会卡死在同一处。 */\n"
    '    BT_LOGE("Not supported");\n'
    "    hsm_transition_to(sm, &off_state);"
)

MARKER = (
    "\n/* myvendor override (build-time patch): patch_adapter_state.py v1\n"
    " *   上游 frameworks/connectivity/bluetooth / service/src/adapter_state.c\n"
    " *   原因 ble_turning_on_enter() 失败路径没有失败出口 → HSM 永久死态 */\n"
    '#pragma message("myvendor override compiled(patch): '
    "vela_override/bluetooth/patch_adapter_state.py v1 -- 上游 "
    "frameworks/connectivity/bluetooth / service/src/adapter_state.c -- "
    'adapter turn-on 失败退回 off_state")\n'
    "const char myvendor_override_patch_marker_adapter_state[]\n"
    '    __attribute__((used, section(".myvendor_marker"))) = "'
    "vela_override/bluetooth/patch_adapter_state.py v1 -- 上游 "
    "frameworks/connectivity/bluetooth / service/src/adapter_state.c -- "
    'adapter turn-on 失败退回 off_state";\n'
)


def safe_path(raw: str) -> Path:
    if ".." in Path(raw).parts:
        raise SystemExit("refusing path containing '..': %s" % raw)
    p = Path(raw).resolve()
    if p.is_dir():
        raise SystemExit("expected a file path, got a directory: %s" % p)
    return p


def patch_body(body: str) -> tuple[str, bool]:
    """给 ble_turning_on_enter 的两个失败分支各加一次 off_state 迁移。"""
    changed = False

    if "hsm_transition_to(sm, &off_state);" in body:
        return body, False          # 已经打过

    if OLD_ELSE in body:
        body = body.replace(OLD_ELSE, NEW_ELSE, 1)
        changed = True
    else:
        raise SystemExit(
            "adapter_state.c: BT_DFX_OPEN_ERROR(BT_DFXE_LE_ENABLE_FAIL) not found "
            "-- upstream drifted, re-check the patch"
        )

    # `#else` 分支（未开 BLE 支持）是可选锚点：找不到只说明上游改过写法，
    # 不影响主分支的修复，所以这里不 fail。
    if OLD_NOLE in body:
        body = body.replace(OLD_NOLE, NEW_NOLE, 1)
        changed = True

    return body, changed


def patch(text: str) -> tuple[str, bool]:
    # **必须找定义、不能找声明**：本文件开头有一整块前置声明
    # （`static void ble_turning_on_enter(state_machine_t* sm);`），
    # 用裸 SIG 去 find 会命中那一行，取到的"函数体"是后面一堆声明
    # ⇒ 锚点必然找不到（第一版就是这么挂的）。加上 "\n{" 才是定义。
    i = text.find(SIG + "\n{")
    if i < 0:
        raise SystemExit("adapter_state.c: ble_turning_on_enter definition not found")
    j = text.find("\n}", i)
    if j < 0:
        raise SystemExit("adapter_state.c: ble_turning_on_enter body end not found")

    body = text[i:j]
    new_body, changed = patch_body(body)
    return text[:i] + new_body + text[j:], changed


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_adapter_state.py SRC DST", file=sys.stderr)
        return 2

    src_path = safe_path(sys.argv[1])
    dst_path = safe_path(sys.argv[2])
    text = src_path.read_text(encoding="utf-8")

    out, changed = patch(text)

    marker_start = out.find(
        "\n/* myvendor override (build-time patch): patch_adapter_state.py"
    )
    if marker_start >= 0:
        out = out[:marker_start]
    if MARK not in out:
        # MARK 必须待在 C 注释里 —— 裸文本会破坏编译。
        out = out + MARKER + "/* " + MARK + " */\n"

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(out, encoding="utf-8")
    print(
        "my_vendor: override patch patch_adapter_state.py v1 -> "
        "service/src/adapter_state.c -- %s" % ("applied" if changed else "idempotent")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
