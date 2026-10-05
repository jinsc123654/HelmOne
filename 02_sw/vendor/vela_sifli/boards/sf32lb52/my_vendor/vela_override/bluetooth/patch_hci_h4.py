#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : frameworks/connectivity/bluetooth / service/stacks/zephyr/hci_h4.c
#   上游 blob: (见 git -C frameworks/connectivity/bluetooth log -1)
#   为什么   :
#     (1) `get_rx()` 对**非可丢**事件与 ACL 一律传 `K_FOREVER`。本函数跑在
#         framework 的 service loop 里（字符设备 poll 回调 →
#         `bt_sal_hci_transport_recv`），在这里等就是**把整条 BT 服务线程钉死**：
#         所有 SAL 回调、GATT、连接管理都在这条线程上。`K_FOREVER` 虽然被
#         `vela_override/zblue/patch_net_buf_timeout.py` 兜成 2 s 有界轮询，
#         但那仍是"服务线程停 2 s"，而且拿不到就是**丢包**——对
#         `bt_buf_get_evt()` 路由到 sync_evt_pool 的高优先级事件
#         （NUM_COMPLETED_PACKETS / CMD_STATUS / CMD_COMPLETE）来说，
#         丢一条会让上层流控和状态永久错位。
#     (2) 可丢集合漏了**扩展广播报告**（`BT_HCI_EVT_LE_EXT_ADVERTISING_REPORT`
#         = 0x0d）。SDK 2.4 的 zephyr 通路有这条修复（commit `8a1103b`
#         "Make ext adv packet to be discardable"，见
#         `zephyr_bt/sf_port/zbt_hci_sf.c:310-316`）。本板 `CONFIG_BT_EXT_ADV=y`
#         是开着的，真开扩展扫描时缺这条会去抢有限的普通事件池。
#         （现场实测未触发：抓到的 LE Meta 事件最大 90 字节，没有 ~250 字节的
#         扩展广播报告 ⇒ 属于补潜在问题。）
#
#     改法：`get_rx()` 一律 `K_NO_WAIT`，并用出参告诉调用方"这一包能不能丢"；
#     调用方对**不可丢**的包**退帧重试**（不消费、直接返回，下一轮 poll 再来）。
#     返回本身就是让主机去处理已入队的包、把 net_buf 还回池子 ⇒ 重试有进展，
#     不是活锁。这与 SDK 2.4 的 `zbt_hci_sf.c` 同思路：它的 `read_payload()`
#     用 `K_NO_WAIT`，失败就 defer 给 rx 线程，**从不在解析上下文里阻塞**。
#
#     ⚠ 本文件在写入前**已经被人在原地改过**（`h4_rx_reopen()` 那一段：读失败时
#     有界重开字符设备 + 重注册 poll，而不是直接摘回调）。本补丁**只碰上面两处**，
#     不动那段。之所以走 patch 脚本而不是原地改：上游重新同步时原地改动会丢，
#     而脚本会在配置期重新落到新版本上（同 patch_adapter_service.py 的取舍）。
#   写入时 HEAD: (见 git -C frameworks/connectivity/bluetooth log -1)
#   版本漂移自查:
#     git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/stacks/zephyr/hci_h4.c
#     git -C frameworks/connectivity/bluetooth diff -- service/stacks/zephyr/hci_h4.c
#   机制：CMake 配置期调用本脚本 → 生成到构建目录 → 按文件名顶掉 libbluetooth。
# ---------------------------------------------------------------------------
"""hci_h4.c：RX 取包不再阻塞服务线程；不可丢的包退帧重试；补 ext adv 可丢。

脚本幂等：已打过的补丁跳过，可重复跑。
"""

from __future__ import annotations

import sys
from pathlib import Path

MARK = "MYVENDOR BUILD: patch_hci_h4.py v1"

# ---- (1) get_rx：改成 K_NO_WAIT + 出参 droppable ---------------------------

GETRX_SIG = "static struct net_buf* get_rx(const uint8_t* buf)"

GETRX_NEW_BODY = """static struct net_buf* get_rx(const uint8_t* buf, bool* droppable)
{
    *droppable = false;

    /* my_vendor: 一律 K_NO_WAIT，不在这里等（见文件头说明 (1)）。
     * "拿不到怎么办"交给调用方：可丢的丢，不可丢的退帧重试。 */
    switch (buf[0]) {
    case BT_HCI_H4_EVT:
        /* buf[2] 是事件参数总长度，>=1 才保证 buf[3]（LE 子事件号）在帧内。
         * hci_packet_complete() 已保证整包完整，这里只是把前提写出来。 */
        if (buf[1] == BT_HCI_EVT_LE_META_EVENT && buf[2] >= 1
            && (buf[3] == BT_HCI_EVT_LE_ADVERTISING_REPORT
                || buf[3] == BT_HCI_EVT_LE_EXT_ADVERTISING_REPORT)) {
            *droppable = true;
            return bt_buf_get_evt(buf[1], true, K_NO_WAIT);
        }

        return bt_buf_get_evt(buf[1], false, K_NO_WAIT);
    case BT_HCI_H4_ACL:
        return bt_buf_get_rx(BT_BUF_ACL_IN, K_NO_WAIT);
    case BT_HCI_H4_ISO:
        if (IS_ENABLED(CONFIG_BT_ISO)) {
            return bt_buf_get_rx(BT_BUF_ISO_IN, K_NO_WAIT);
        }
        break;
    default:
        BT_LOGE("RX unknown packet type: %u", buf[0]);
        break;
    }

    return NULL;
}"""

# ---- (2) 调用点：退帧重试 ---------------------------------------------------

CALL_OLD = (
    "        buf_add = frame_start + sizeof(packet_type);\n"
    "        buf_add_len = decoded_len - sizeof(packet_type);\n"
    "\n"
    "        buf = get_rx(frame_start);\n"
    "\n"
    "        frame_size -= decoded_len;\n"
    "        frame_start += decoded_len;\n"
    "\n"
    "        if (!buf) {\n"
    '            BT_LOGD("Discard adv report due to insufficient buf");\n'
    "            continue;\n"
    "        }\n"
)

CALL_NEW = (
    "        buf_add = frame_start + sizeof(packet_type);\n"
    "        buf_add_len = decoded_len - sizeof(packet_type);\n"
    "\n"
    "        {\n"
    "            bool droppable = false;\n"
    "\n"
    "            buf = get_rx(frame_start, &droppable);\n"
    "\n"
    "            if (!buf) {\n"
    "                if (droppable) {\n"
    "                    /* 广播报告：丢掉即可，下一拍还有。 */\n"
    '                    BT_LOGD("Discard adv report due to insufficient buf");\n'
    "                    frame_size -= decoded_len;\n"
    "                    frame_start += decoded_len;\n"
    "                    continue;\n"
    "                }\n"
    "\n"
    "                /* my_vendor: **不可丢的包退帧重试** —— 丢一条会让上层状态\n"
    "                 * 永久错位，而在这里等会把整条 service loop 钉死。\n"
    "                 * frame_start 不推进、残留挪回帧首，直接返回。\n"
    "                 *\n"
    "                 * `usleep(1000)` 不是“等 buffer”，是**防热转**：字符设备上的\n"
    "                 * poll 是电平触发的，不消费就还是可读，立刻返回会让服务\n"
    "                 * 线程空转。让出 1 ms，同时给主机把已入队事件处理掉、把\n"
    "                 * net_buf 还回池子的时间（RX 处理在 zblue 自己的 RX 工作\n"
    "                 * 队列线程上，不在这条线程上）。 */\n"
    '                BT_LOGW("H4 rx no buf (type %u len %d), retry after 1ms",\n'
    "                    packet_type, (int)decoded_len);\n"
    "                usleep(1000);\n"
    "\n"
    "                if (frame_start != frame) {\n"
    "                    memmove(frame, frame_start, frame_size);\n"
    "                }\n"
    "\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "\n"
    "        frame_size -= decoded_len;\n"
    "        frame_start += decoded_len;\n"
)

# ---- (3) h4_open 的例行信息别占 ERROR 档 ------------------------------------
#
# `BT_LOGE("H4: %s opened as fd:%d", …)` 是**每次打开 H4 传输都会打**的例行信息，
# 不是错误。开着 framework 日志（等级 4=WARN）之后它会以 `[ ERROR]` 出到控制台
# —— 而本项目的错误计数纪律是"真错误只认 `[ ERROR]` 标签行"
# （见 DEBUG_PLAYBOOK），这么一条会把那个判据稀释掉。
# 降到 BT_LOGI：默认等级 4 不再打，要看时 `ctl btlog info`。

OPEN_OLD = '    BT_LOGE("H4: %s opened as fd:%d", CONFIG_BT_UART_ON_DEV_NAME, h4->fd);'

OPEN_NEW = (
    "    /* my_vendor: 例行信息，不是错误 —— 别占 `[ ERROR]` 这一档，\n"
    "     * 否则会稀释\"真错误只认 ERROR 标签行\"那条判据。 */\n"
    '    BT_LOGI("H4: %s opened as fd:%d", CONFIG_BT_UART_ON_DEV_NAME, h4->fd);'
)

MARKER = (
    "\n/* myvendor override (build-time patch): patch_hci_h4.py v1\n"
    " *   上游 frameworks/connectivity/bluetooth / service/stacks/zephyr/hci_h4.c\n"
    " *   原因 RX 取包 K_FOREVER → K_NO_WAIT + 不可丢包退帧重试；补 ext adv 到可丢集合 */\n"
    '#pragma message("myvendor override compiled(patch): '
    "vela_override/bluetooth/patch_hci_h4.py v1 -- 上游 "
    "frameworks/connectivity/bluetooth / service/stacks/zephyr/hci_h4.c -- "
    'RX 不再阻塞服务线程（K_NO_WAIT + 退帧重试）")\n'
    "const char myvendor_override_patch_marker_hci_h4[]\n"
    '    __attribute__((used, section(".myvendor_marker"))) = "'
    "vela_override/bluetooth/patch_hci_h4.py v1 -- 上游 "
    "frameworks/connectivity/bluetooth / service/stacks/zephyr/hci_h4.c -- "
    'RX 不再阻塞服务线程"'
    ";\n"
)


def safe_path(raw: str) -> Path:
    if ".." in Path(raw).parts:
        raise SystemExit("refusing path containing '..': %s" % raw)
    p = Path(raw).resolve()
    if p.is_dir():
        raise SystemExit("expected a file path, got a directory: %s" % p)
    return p


def _replace_once(text: str, old: str, new: str, what: str) -> tuple[str, bool]:
    if new in text:
        return text, False
    n = text.count(old)
    if n == 0:
        raise SystemExit("%s: pattern not found -- upstream drifted?" % what)
    if n > 1:
        raise SystemExit("%s: pattern matched %d times, want 1" % (what, n))
    return text.replace(old, new, 1), True


def patch(text: str) -> tuple[str, list[str]]:
    applied: list[str] = []

    # (1) get_rx：按"签名 + 函数体到 \n}" 整块换掉，比逐行锚点稳
    i = text.find(GETRX_SIG + "\n{")
    if i < 0:
        raise SystemExit("hci_h4.c: get_rx definition not found")
    if "bool* droppable" in text[i:i + 200]:
        applied.append("get_rx(already)")
    else:
        j = text.find("\n}", i)
        if j < 0:
            raise SystemExit("hci_h4.c: get_rx body end not found")
        text = text[:i] + GETRX_NEW_BODY + text[j + 2:]
        applied.append("get_rx")

    # (2) 调用点
    text, c = _replace_once(text, CALL_OLD, CALL_NEW, "transport_recv call site")
    if c:
        applied.append("call_site")

    # (3) h4_open 的例行信息降级到 INFO
    text, c = _replace_once(text, OPEN_OLD, OPEN_NEW, "h4_open routine log level")
    if c:
        applied.append("open_log_level")

    return text, applied


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_hci_h4.py SRC DST", file=sys.stderr)
        return 2

    src_path = safe_path(sys.argv[1])
    dst_path = safe_path(sys.argv[2])
    text = src_path.read_text(encoding="utf-8")

    out, applied = patch(text)

    marker_start = out.find(
        "\n/* myvendor override (build-time patch): patch_hci_h4.py"
    )
    if marker_start >= 0:
        out = out[:marker_start]
    if MARK not in out:
        out = out + MARKER + "/* " + MARK + " */\n"

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(out, encoding="utf-8")
    print(
        "my_vendor: override patch patch_hci_h4.py v1 -> "
        "service/stacks/zephyr/hci_h4.c -- %s" % "+".join(applied)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
