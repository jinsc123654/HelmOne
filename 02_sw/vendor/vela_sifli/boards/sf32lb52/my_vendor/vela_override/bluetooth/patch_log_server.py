#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : frameworks/connectivity/bluetooth / service/utils/log_server.c
#   上游 blob: (见 git -C frameworks/connectivity/bluetooth log -1)
#   为什么   :
#     `log_server.c` 里凡是碰 `property_monitor_*` 的地方，只在
#     `CONFIG_KVDB` + `__NuttX__` 下编译 —— **漏判了 `CONFIG_KVDB_DIRECT`**。
#     而 KVDB 的 CMake 是这么分的：
#       frameworks/system/utils/CMakeLists.txt:51-57
#         if(CONFIG_KVDB_DIRECT)  → kvdb/direct.c     （**没有** monitor 实现）
#         else()                  → kvdb/client.c     （property_monitor_* 在这里）
#     本板是 `CONFIG_KVDB_DIRECT=y` ⇒ **`property_monitor_open/read/close`
#     三个符号在整棵树里根本没有实现**（声明在
#     frameworks/system/utils/include/kvdb.h:82-101，实现在 client.c，不参与编译）。
#
#     平时看不出来，是因为 framework 的 `BT_LOG*` 宏是条件编译的：不开
#     `CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL` 时它们全是空宏，于是谁都不引用
#     `bt_log_print_check()`，链接器就不从 `liblibbluetooth.a` 里拉
#     `log_server.o` —— 那三个未定义符号自然没人管。
#     **一旦打开 framework 日志**（CONFIG_BLUETOOTH_LOG=y + 等级），
#     `bt_log_print_check` 被引用 ⇒ `log_server.o` 被拉进来 ⇒ 链接期
#     三个 undefined reference，整个固件编不出来。
#
#     ⇒ 这是个**真缺陷**：本树里 framework 日志"能开"和"能链"是矛盾的。
#     本补丁把 monitor 那三块按 `!CONFIG_KVDB_DIRECT` 关掉，让 DIRECT 模式下
#     日志能正常开。
#
#     代价：DIRECT 模式下**等级不再热加载**，改 `persist.bluetooth.log.level`
#     要下次开机才生效（`bt_log_server_init()` 里的 `property_get_int32()`
#     还在，启动时照样读）。`ctl btlog` 的注释里写明了这一点。
#     哪天 DIRECT 模式补上 monitor 实现（或本板改走 client 模式），
#     把这三处 guard 去掉即可 —— 判据就是链接能不能过。
#   写入时 HEAD: (见 git -C frameworks/connectivity/bluetooth log -1)
#   版本漂移自查:
#     git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/utils/log_server.c
#     git -C frameworks/connectivity/bluetooth diff -- service/utils/log_server.c
#   机制：CMake 配置期调用本脚本 → 生成到构建目录 → 按文件名顶掉 libbluetooth。
# ---------------------------------------------------------------------------
"""DIRECT 模式下关掉 log_server.c 的 property monitor（那三个符号不存在）。

不加这个补丁，`CONFIG_BLUETOOTH_LOG=y` 会让整棵固件链接失败：
  undefined reference to `property_monitor_close' / `property_monitor_read'
                       / `property_monitor_open'

脚本幂等：已打过的补丁跳过，可重复跑。
"""

from __future__ import annotations

import sys
from pathlib import Path

MARK = "MYVENDOR BUILD: patch_log_server.py v1"

GUARD = "!defined(CONFIG_KVDB_DIRECT)"

# ---- (1) property_monitor_cb 整个函数 --------------------------------------
#
# 用两行组合定位，单看 guard 行会在文件里命中好几处（其它块只用得到
# property_get/set_int32，那些在 direct.c 里**是有的**，不能一起关掉）。

CB_OLD = "#if defined(CONFIG_KVDB) && defined(__NuttX__)\nstatic void property_monitor_cb("
CB_NEW = (
    "/* my_vendor: 本块用 property_monitor_*（KVDB client 模式才有实现），\n"
    " * DIRECT 模式下必须一起关掉，否则链接期 undefined。见文件头说明。 */\n"
    "#if defined(CONFIG_KVDB) && defined(__NuttX__) && " + GUARD + "\n"
    "static void property_monitor_cb("
)

# ---- (2) bt_log_server_init 里的 monitor 启动段 -----------------------------

INIT_OLD = (
    "    /** start log property monitor */\n"
    "    property_set_int32(PERSIST_BT_LOG_CHANGED, 0);\n"
    "    g_logger.monitor_fd = property_monitor_open(PERSIST_BT_LOG_CHANGED);\n"
    "    if (g_logger.monitor_fd < 0) {\n"
    '        syslog(LOG_ERR, "propert monitor open fail: %d\\n", errno);\n'
    "        return;\n"
    "    }\n"
    "\n"
    "    g_logger.poll = service_loop_poll_fd(g_logger.monitor_fd, POLL_READABLE, property_monitor_cb, NULL);\n"
    "    if (g_logger.poll == NULL)\n"
    '        syslog(LOG_ERR, "%s\\n", "propert monitor poll error");\n'
    "\n"
)

INIT_NEW = (
    "    /** my_vendor: 等级**热加载**用的 monitor —— DIRECT 模式没有\n"
    "     * property_monitor_*，整段关掉；下面的启动期读数与最终那行\n"
    "     * \"Framework log level:\" 保留，所以日志照样能用，只是改等级\n"
    "     * 要下次开机生效。 */\n"
    "#if !defined(CONFIG_KVDB_DIRECT)\n"
    "    /** start log property monitor */\n"
    "    property_set_int32(PERSIST_BT_LOG_CHANGED, 0);\n"
    "    g_logger.monitor_fd = property_monitor_open(PERSIST_BT_LOG_CHANGED);\n"
    "    if (g_logger.monitor_fd < 0) {\n"
    '        syslog(LOG_ERR, "propert monitor open fail: %d\\n", errno);\n'
    "        return;\n"
    "    }\n"
    "\n"
    "    g_logger.poll = service_loop_poll_fd(g_logger.monitor_fd, POLL_READABLE, property_monitor_cb, NULL);\n"
    "    if (g_logger.poll == NULL)\n"
    '        syslog(LOG_ERR, "%s\\n", "propert monitor poll error");\n'
    "#endif /* !CONFIG_KVDB_DIRECT */\n"
    "\n"
)

# ---- (3) bt_log_server_cleanup 里的 monitor 拆除段 --------------------------

CLEAN_OLD = (
    "    /** stop log property monitor */\n"
    "    service_loop_remove_poll(g_logger.poll);\n"
    "    g_logger.poll = NULL;\n"
    "    if (g_logger.monitor_fd)\n"
    "        property_monitor_close(g_logger.monitor_fd);\n"
    "\n"
    "    g_logger.monitor_fd = -1;\n"
)

CLEAN_NEW = (
    "    /** my_vendor: 同 init —— DIRECT 模式下 monitor 根本没开过，\n"
    "     * 这三个调用也会链接失败。 */\n"
    "#if !defined(CONFIG_KVDB_DIRECT)\n"
    "    /** stop log property monitor */\n"
    "    service_loop_remove_poll(g_logger.poll);\n"
    "    g_logger.poll = NULL;\n"
    "    if (g_logger.monitor_fd)\n"
    "        property_monitor_close(g_logger.monitor_fd);\n"
    "\n"
    "    g_logger.monitor_fd = -1;\n"
    "#endif /* !CONFIG_KVDB_DIRECT */\n"
)

MARKER = (
    "\n/* myvendor override (build-time patch): patch_log_server.py v1\n"
    " *   上游 frameworks/connectivity/bluetooth / service/utils/log_server.c\n"
    " *   原因 DIRECT 模式没有 property_monitor_* ⇒ monitor 三处按 "
    "!CONFIG_KVDB_DIRECT 关掉 */\n"
    '#pragma message("myvendor override compiled(patch): '
    "vela_override/bluetooth/patch_log_server.py v1 -- 上游 "
    "frameworks/connectivity/bluetooth / service/utils/log_server.c -- "
    'DIRECT 模式关掉 property monitor（符号不存在，否则链接失败）")\n'
    "const char myvendor_override_patch_marker_log_server[]\n"
    '    __attribute__((used, section(".myvendor_marker"))) = "'
    "vela_override/bluetooth/patch_log_server.py v1 -- 上游 "
    "frameworks/connectivity/bluetooth / service/utils/log_server.c -- "
    'DIRECT 模式关掉 property monitor"'
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
    """整体替换一次；已经打过的（new 已在）原样返回。"""
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

    text, c = _replace_once(text, CB_OLD, CB_NEW, "property_monitor_cb guard")
    if c:
        applied.append("monitor_cb")

    text, c = _replace_once(text, INIT_OLD, INIT_NEW, "bt_log_server_init monitor block")
    if c:
        applied.append("init")

    text, c = _replace_once(text, CLEAN_OLD, CLEAN_NEW, "bt_log_server_cleanup monitor block")
    if c:
        applied.append("cleanup")

    return text, applied


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_log_server.py SRC DST", file=sys.stderr)
        return 2

    src_path = safe_path(sys.argv[1])
    dst_path = safe_path(sys.argv[2])
    text = src_path.read_text(encoding="utf-8")

    out, applied = patch(text)

    marker_start = out.find(
        "\n/* myvendor override (build-time patch): patch_log_server.py"
    )
    if marker_start >= 0:
        out = out[:marker_start]
    if MARK not in out:
        out = out + MARKER + "/* " + MARK + " */\n"

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(out, encoding="utf-8")
    what = "+".join(applied) if applied else "idempotent"
    print(
        "my_vendor: override patch patch_log_server.py v1 -> "
        "service/utils/log_server.c -- %s" % what
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
