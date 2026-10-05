#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : frameworks/connectivity/bluetooth / service/src/adapter_service.c
#   上游 blob: 018189c4cc1e8e11ec671b933d0aba6746dac900
#   为什么   :
#     (1) process_enc_state_change_evt() BLE 分支只 find 不 create →
#         device==NULL 时 device_set_connection_state 读 offset 0xab 崩
#         （现场 2026-09-20，cfsr=0x82 mmfar=0xab）
#     (2) process_ssp_request_evt() 也只 find → device==NULL 时
#         device_set_bond_state 写 offset 0xac 崩
#         （现场 n581 2026-09-21T19:02:19，cfsr=0x82 mmfar=0xac，
#          pc=strb [r0,#0xac] @ device_set_bond_state，
#          lr=process_ssp_request_evt；ELF helmone+nsh+20260921-185939）
#   写入时 HEAD: 43945bc1d13f3494a79e3e1d79ca8312c4c0f6a0
#   版本漂移自查:
#     git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/src/adapter_service.c
#     git -C frameworks/connectivity/bluetooth diff -- service/src/adapter_service.c
#   机制：CMake 配置期调用本脚本 → 生成到构建目录 → 按文件名顶掉 libbluetooth。
# ---------------------------------------------------------------------------
"""修 adapter_service.c 两处"只 find 不 create"的 NULL device 崩 + 断链日志级别。

(1) process_enc_state_change_evt — 见历史注释（mmfar=0xab / connection_state）
(2) process_ssp_request_evt — SSP/配对显示路径（mmfar=0xac / bond_state）
(3) bt_dfx_connection_state_changed — 把两类**日常**断链 reason 的 DFX 上报
    从 ERROR 降为 WARNING（用户 2026-09-25 定调："最多算警告，因为日常使用
    就有掉线、远离的情况"）；**DFX 事件照发**，只改控制台级别。

前两处改成与 bond-state 处理器一致的 find_create，并加 NULL 兜底。
脚本幂等：已打过的补丁跳过，可重复跑。
"""

from __future__ import annotations

import sys
from pathlib import Path

MARK = "MYVENDOR BUILD: patch_adapter_service.py v3"

# ---- (1) enc_state ---------------------------------------------------------

ENC_SIG = "static void process_enc_state_change_evt("
ENC_FIND = "        device = adapter_find_device(addr, BT_TRANSPORT_BLE);"
ENC_CREATE = (
    "        /* my_vendor: 这里以前只 find —— framework 设备表里还没这个地址时\n"
    "         * device 是 NULL（例：落盘密钥恢复后手机用**身份地址**重连并加密），\n"
    "         * 紧随其后的 device_set_connection_state(NULL, …) 会在\n"
    "         * device_is_connected() 里 `ldrb r0,[r0,#0xab]` 直接 MemManage fault\n"
    "         * （现场 cfsr=0x82 mmfar=0xab）。与 BR/EDR 那支和 bond-state\n"
    "         * 处理器一致，改成 find_create。 */\n"
    "        device = adapter_find_create_le_device(addr, BT_LE_ADDR_TYPE_PUBLIC);"
)
ENC_GUARD_ANCHOR = "    if (encrypted) {"
ENC_GUARD = (
    "    if (device == NULL) {\n"
    "        /* my_vendor: 兜底 —— 宁可这次不更新状态，也不要拿 NULL 去解引用。 */\n"
    "        adapter_unlock();\n"
    "        return;\n"
    "    }\n"
)

# ---- (2) ssp_request -------------------------------------------------------

SSP_SIG = "static void process_ssp_request_evt("
SSP_FIND = "    device = adapter_find_device(addr, transport);"
SSP_CREATE = (
    "    /* my_vendor: 以前只 find。手机用身份地址发起 SSP、而 framework\n"
    "     * 设备表还没有该地址时 device==NULL，随后 device_get/set_bond_state\n"
    "     * 写 remote.bond_state（offset 0xac）直接 MemManage fault\n"
    "     * （现场 n581：cfsr=0x82 mmfar=0xac，pc=device_set_bond_state，\n"
    "     * lr=process_ssp_request_evt）。按 transport 走 find_create。 */\n"
    "    if (transport == BT_TRANSPORT_BREDR) {\n"
    "        device = adapter_find_create_classic_device(addr);\n"
    "    } else if (transport == BT_TRANSPORT_BLE) {\n"
    "        device = adapter_find_create_le_device(addr, BT_LE_ADDR_TYPE_PUBLIC);\n"
    "    } else {\n"
    "        adapter_unlock();\n"
    "        return;\n"
    "    }\n"
)
SSP_GUARD_ANCHOR = "    if (device_get_bond_state(device) == BOND_STATE_CANCELING) {"
SSP_GUARD = (
    "    if (device == NULL) {\n"
    "        /* my_vendor: find_create 失败也不要解引用。 */\n"
    "        adapter_unlock();\n"
    "        return;\n"
    "    }\n"
)

# ---- (3) DFX 断链级别：日常事件 ⇒ WARNING（用户 2026-09-25 定调）------------

DFX_SIG = "static void bt_dfx_connection_state_changed("
DFX_WARN_DEFS = (
    "/* my_vendor（构建期补丁）：**日常断链按 WARNING 报**。\n"
    " *\n"
    " * 本函数只处理两类 reason —— `HCI_ERR_CONNECTION_TIMEOUT`（0x08，监督超时：\n"
    " * 远离 / 对端静默）与 `HCI_ERR_CONNECTION_FAILED_TO_BE_ESTABLISHED`（0x3D，\n"
    " * 建链时对端跑了）—— 两者在日常使用里本来就会发生（用户原话：\"最多算警告，\n"
    " * 因为日常使用就有掉线，或者远离的情况\"）。\n"
    " * 下面两个宏与上游 `BT_DFX_{BR,LE}_GAP_DISCONN_ERROR` **完全同形**，只把\n"
    " * `BT_LOGE` 换成 `BT_LOGW`：**DFX 事件照发**（不丢遥测），控制台级别降一档。\n"
    " * 见 vela_override/bluetooth/patch_adapter_service.py 的 (3)。 */\n"
    "#define BT_DFX_BR_GAP_DISCONN_WARN(reason)                                             \\\n"
    "    do {                                                                               \\\n"
    "        BT_LOGW(\"BT_DFX: brDisconnectError: %s\", reason);                             \\\n"
    "        BT_DFX_SEND_BR_EVENT(BT_DFX_BUILD_CODE(BT_DFXG_BR_GAP, BT_DFXC_BR_GAP_DISCONN), \\\n"
    "            \"%s:%s\", \"brDisconnectError\", reason);                                   \\\n"
    "    } while (0)\n"
    "\n"
    "#define BT_DFX_LE_GAP_DISCONN_WARN(reason)                                             \\\n"
    "    do {                                                                               \\\n"
    "        BT_LOGW(\"BT_DFX: bleDisconnectError: %s\", reason);                            \\\n"
    "        BT_DFX_SEND_LE_GAP_EVENT(                                                      \\\n"
    "            BT_DFX_BUILD_CODE(BT_DFXG_LE_GAP, BT_DFXC_LE_GAP_DISCONN),                 \\\n"
    "            \"%s:%s\", \"bleDisconnectError\", reason);                                  \\\n"
    "    } while (0)\n"
    "\n"
)
DFX_RENAMES = (
    ("BT_DFX_BR_GAP_DISCONN_ERROR(", "BT_DFX_BR_GAP_DISCONN_WARN("),
    ("BT_DFX_LE_GAP_DISCONN_ERROR(", "BT_DFX_LE_GAP_DISCONN_WARN("),
)

MARKER = (
    "\n/* myvendor override (build-time patch): patch_adapter_service.py v3\n"
    " *   上游 frameworks/connectivity/bluetooth / service/src/adapter_service.c\n"
    " *   原因 (1) enc_state BLE find_create+NULL (2) ssp_request find_create+NULL\n"
    " *        (3) 日常断链的 DFX 上报 ERROR -> WARNING（DFX 事件照发） */\n"
    '#pragma message("myvendor override compiled(patch): '
    "vela_override/bluetooth/patch_adapter_service.py v3 -- 上游 "
    'frameworks/connectivity/bluetooth / service/src/adapter_service.c -- '
    'enc_state+ssp_request NULL 设备保护 + 断链 DFX 降为 WARNING")\n'
    "const char myvendor_override_patch_marker_adapter_service[]\n"
    '    __attribute__((used, section(".myvendor_marker"))) = "'
    "vela_override/bluetooth/patch_adapter_service.py v3 -- 上游 "
    "frameworks/connectivity/bluetooth / service/src/adapter_service.c -- "
    'enc_state+ssp_request NULL 设备保护 + 断链 DFX 降为 WARNING";\n'
)


def safe_path(raw: str) -> Path:
    if ".." in Path(raw).parts:
        raise SystemExit("refusing path containing '..': %s" % raw)
    p = Path(raw).resolve()
    if p.is_dir():
        raise SystemExit("expected a file path, got a directory: %s" % p)
    return p


def _replace_fn(text: str, sig: str, replacer) -> tuple[str, bool]:
    i = text.find(sig)
    if i < 0:
        raise SystemExit("%s not found" % sig.strip())
    j = text.find("\n}", i)
    if j < 0:
        raise SystemExit("%s body end not found" % sig.strip())
    body = text[i:j]
    new_body, changed = replacer(body)
    return text[:i] + new_body + text[j:], changed


def _patch_enc(body: str) -> tuple[str, bool]:
    changed = False
    if ENC_FIND in body:
        body = body.replace(ENC_FIND, ENC_CREATE, 1)
        changed = True
    elif "adapter_find_create_le_device(addr, BT_LE_ADDR_TYPE_PUBLIC);" not in body:
        raise SystemExit("enc_state: neither find nor find_create present")
    if ENC_GUARD not in body:
        if ENC_GUARD_ANCHOR not in body:
            raise SystemExit("enc_state: guard anchor missing")
        body = body.replace(ENC_GUARD_ANCHOR, ENC_GUARD + ENC_GUARD_ANCHOR, 1)
        changed = True
    return body, changed


def _patch_ssp(body: str) -> tuple[str, bool]:
    changed = False
    if SSP_FIND in body:
        body = body.replace(SSP_FIND, SSP_CREATE, 1)
        changed = True
    elif "adapter_find_create_classic_device(addr)" not in body:
        raise SystemExit("ssp_request: neither find nor find_create present")
    if SSP_GUARD not in body:
        if SSP_GUARD_ANCHOR not in body:
            raise SystemExit("ssp_request: guard anchor missing")
        body = body.replace(SSP_GUARD_ANCHOR, SSP_GUARD + SSP_GUARD_ANCHOR, 1)
        changed = True
    return body, changed


def _patch_dfx(body: str) -> tuple[str, bool]:
    """(3) 两类日常断链 reason 的 DFX 上报：ERROR ⇒ WARNING（DFX 事件照发）。"""
    changed = False
    if "BT_DFX_LE_GAP_DISCONN_WARN(" not in body:
        for old, new in DFX_RENAMES:
            if old not in body:
                raise SystemExit("dfx: %s not found" % old)
            body = body.replace(old, new)
            changed = True
        body = DFX_WARN_DEFS + body
    return body, changed


def patch(text: str) -> tuple[str, list[str]]:
    applied: list[str] = []
    text, c = _replace_fn(text, ENC_SIG, _patch_enc)
    if c:
        applied.append("enc_state")
    text, c = _replace_fn(text, SSP_SIG, _patch_ssp)
    if c:
        applied.append("ssp_request")
    text, c = _replace_fn(text, DFX_SIG, _patch_dfx)
    if c:
        applied.append("dfx_disconn_warn")
    return text, applied


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_adapter_service.py SRC DST", file=sys.stderr)
        return 2

    src_path = safe_path(sys.argv[1])
    dst_path = safe_path(sys.argv[2])
    text = src_path.read_text(encoding="utf-8")

    # Strip any previous marker so re-patch from a generated copy still works.
    for old in (
        MARK,
        "MYVENDOR BUILD: patch_adapter_service.py",
        'myvendor_override_patch_marker_adapter_service[]',
    ):
        pass  # content checks below are idempotent on the function bodies

    if MARK in text and "process_ssp_request_evt" in text:
        # Fast path: already v2. Still verify ssp create is present.
        if "adapter_find_create_classic_device(addr)" in text[
            text.find(SSP_SIG) : text.find(SSP_SIG) + 800
        ]:
            dst_path.write_text(text, encoding="utf-8")
            print("my_vendor: adapter_service.c already patched (v3)")
            return 0

    out, applied = patch(text)
    # Drop old marker block if re-patching a v1 output, then append v2.
    marker_start = out.find("\n/* myvendor override (build-time patch): patch_adapter_service.py")
    if marker_start >= 0:
        out = out[:marker_start]
    if MARK not in out:
        # MARK must stay a C comment — bare text breaks the compile.
        out = out + MARKER + "/* " + MARK + " */\n"

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(out, encoding="utf-8")
    what = "+".join(applied) if applied else "idempotent"
    print(
        "my_vendor: override patch patch_adapter_service.py v3 -> "
        "service/src/adapter_service.c -- %s" % what
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
