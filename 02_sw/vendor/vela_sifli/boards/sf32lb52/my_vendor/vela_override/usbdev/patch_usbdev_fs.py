#!/usr/bin/env python3
"""构建期补丁：`nuttx/drivers/usbdev/usbdev_fs.c` —— USB 读路径的上界钳位。

## 为什么抽换/为什么用脚本

上游文件 1686 行，我们只改一处（读路径的 `reqlen` 钳位），整份带进 vendor 不划算，
所以按 zblue 那三个脚本的同一套路：**配置期读上游原件、定点改写、写到构建目录**，
再由 CMake 按文件名顶掉 `drivers` target 里的那份。

## 修的是什么（2026-09-19 现场）

崩在 `usbdev_fs_read()`：`pid=14 mtp_simple`、`arm_hardfault.c:186 panic`、
`cfsr=0x8200 (DACCVIOL|MMARVALID)`、`mmfar=bfar=0x20080000`（SRAM 一字节之外）、
`pc=memcpy`、`lr=usbdev_fs_read`。形状是 `memcpy(dst=0x200237cc, src=0x20080000, n)`
—— **从 SRAM 之外读**。复现路径：**弹出 MTP 界面后快速拔线**（主机还排着 READ）。

机制：`reqlen = container->req->xfrd - container->offset;` 是无符号减法且**没有
`offset <= xfrd` 的检查**。请求被中止/重排（`xfrd` 被改小、容器回收后 `offset` 没清）
时 `offset > xfrd`，`reqlen` 下溢成巨大值；后面两处 `memcpy(&buffer[...],
&container->req->buf[container->offset], …)` 就会从 `buf + offset` 一路读到 SRAM 之外。

钳位后：这种"半中止"的容器按 **0 字节**处理（不再 memcpy），只留一行 ERROR 取证，
剩下的收尾流程（出队、回收容器）不变 —— 请求本来就已经死了，少拷 0 字节不影响协议。

## 上游 id

repo : nuttx（Vela 树）
HEAD : 2ce740a0ac1052c5f51083a334ef3093f59ff780
file : drivers/usbdev/usbdev_fs.c
blob : 4e0e5fe6b41417048caa3706ead8002a91cd5d7f

自查：
    git -C nuttx rev-parse HEAD:drivers/usbdev/usbdev_fs.c     # 应等于上面的 blob
"""

import sys
from pathlib import Path

_UP = "nuttx / drivers/usbdev/usbdev_fs.c"
_BLOB = "4e0e5fe6b41417048caa3706ead8002a91cd5d7f"
_WHY = "USB 读路径 reqlen 无符号下溢 → memcpy 读越 SRAM（快速拔线，HardFault）"

# 上游这一行是**六个空格**缩进（不是制表符），照抄别改。
OLD = "      reqlen = container->req->xfrd - container->offset;\n"

NEW = (
    "      reqlen = container->req->xfrd - container->offset;\n"
    "\n"
    "      /* myvendor: 上界钳位。offset > xfrd 时上面那行**无符号下溢**成巨大值，\n"
    "       * 下面两处 memcpy 会从 `buf + offset` 一路读到 SRAM 之外 —— 实机\n"
    "       * 2026-09-19：快速拔线（主机还排着 READ），pid=mtp_simple，\n"
    "       * cfsr=0x8200 DACCVIOL，mmfar=0x20080000（SRAM 一字节之外），\n"
    "       * pc=memcpy / lr=usbdev_fs_read。\n"
    "\t       *\n"
    "       * 这种容器是\"半中止\"的请求：按 0 字节处理（不拷、只记账），\n"
    "       * 出队与回收流程不动 —— 协议侧那一笔本来也就死了。 */\n"
    "      if (container->offset > container->req->xfrd)\n"
    "        {\n"
    "          syslog(LOG_ERR, \"usbdev_fs: stale req off=%u xfrd=%u\\n\",\n"
    "                 (unsigned)container->offset,\n"
    "\t                 (unsigned)container->req->xfrd);\n"
    "          reqlen = 0;\n"
    "        }\n"
).replace("\t", "")   # 上游用空格缩进；插入块里别留制表符混排



def _replace_once(text: str, old: str, new: str) -> str:
    """精确替换一次；已打过就原样返回（幂等）。"""
    if new in text:
        return text
    n = text.count(old)
    if n == 0:
        raise SystemExit("usbdev_fs.c: reqlen pattern not found")
    if n > 1:
        raise SystemExit(f"usbdev_fs.c: reqlen pattern matched {n} times, want 1")
    return text.replace(old, new, 1)


def _myvendor_stamp(path: Path) -> None:
    """在生成的副本里留标记（约定见 vela_override/CMakeLists.txt 顶部）。"""
    _script = "patch_usbdev_fs.py"
    _tag = f"vela_override/{_script} -- 上游 {_UP}@{_BLOB} -- {_WHY}"
    _marker = (
        "\n/* myvendor override (build-time patch): " + _script + "\n"
        " *   上游 " + _UP + " @ " + _BLOB + "\n"
        " *   原因 " + _WHY + " */\n"
        '#pragma message("myvendor override compiled(patch): ' + _tag + ' ")\n'
        "const char myvendor_override_patch_marker_patch_usbdev_fs[]\n"
        '    __attribute__((used, section(".myvendor_marker"))) = "'
        + _tag + '";\n'
    )
    path.write_text(path.read_text(encoding="utf-8") + _marker, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_usbdev_fs.py SRC DST", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    text = _replace_once(src.read_text(encoding="utf-8"), OLD, NEW)

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8")
    _myvendor_stamp(dst)

    print(f"my_vendor: override patch patch_usbdev_fs.py -> {_UP}@{_BLOB} -- {_WHY}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
