#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate production MiSans Medium 4bpp bitmaps for menu/title/val."""

from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BICYCLE_DIR = os.path.normpath(os.path.join(HERE, ".."))
OUT_DIR = os.path.join(BICYCLE_DIR, "src", "lvgl_page", "fonts")
SYM_PATH = os.path.join(OUT_DIR, "helm_mism4_symbols.txt")
MISANS = "/usr/share/fonts/truetype/misans/MiSans-Medium.ttf"

SIZES = (12, 15, 22)


def symbols_for(size: int) -> tuple[str, str]:
    """取这一档的符号集：**优先按档文件** `helm_mism4_symbols_<size>.txt`。

    为什么要按档：三档的用字其实不同（菜单标题 15px / 副标题 12px / 数值 22px），
    2026-09-26 按"调用点归档"裁过一轮（mism4_12 417 / 15 398 / 22 383 个汉字），
    比共用一份 510 字的大表省 ~56 KB flash。按档文件不在时才退回那份共用表。
    """
    per = os.path.join(OUT_DIR, f"helm_mism4_symbols_{size}.txt")
    if os.path.isfile(per):
        return open(per, encoding="utf-8").read().replace("\n", ""), per
    return open(SYM_PATH, encoding="utf-8").read().replace("\n", ""), SYM_PATH


def main() -> int:
    if not os.path.isfile(MISANS):
        print(f"[error] missing {MISANS}", file=sys.stderr)
        return 1
    if not os.path.isfile(SYM_PATH):
        print(f"[error] missing {SYM_PATH}", file=sys.stderr)
        return 1

    for size in SIZES:
        symbols, src = symbols_for(size)
        cjk = sum(1 for c in symbols if "\u4e00" <= c <= "\u9fff")
        print(f"[charset] size={size} total={len(symbols)} CJK={cjk}"
              f" <- {os.path.basename(src)}", file=sys.stderr)
        name = f"helm_mism4_{size}"
        out = os.path.join(OUT_DIR, f"{name}.c")
        print("[lv_font_conv]", name, file=sys.stderr)
        subprocess.check_call([
            "npx", "--yes", "lv_font_conv",
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--no-compress",
            "--no-prefilter",
            "--no-kerning",
            "--lv-include", "lvgl/lvgl.h",
            "--lv-font-name", name,
            "-o", out,
            "--font", MISANS,
            "--autohint-strong",
            "-r", "0x20-0x7E",
            "--symbols", symbols,
        ])
        print("[wrote]", out, os.path.getsize(out), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
