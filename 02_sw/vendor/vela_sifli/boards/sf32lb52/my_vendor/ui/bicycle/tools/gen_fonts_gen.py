#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_fonts_gen.py — regenerate bicycle UI bitmap fonts (fonts_gen/*.c) via lv_font_conv.

Theme fonts (ResourcePool regular/bold) are embedded LVGL bitmaps, NOT the LFS TTF subset.
After adding i18n strings (e.g. 文件传输模式), rerun this script and rebuild firmware.

Usage (from openvela root or bicycle/tools):
  python3 vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/tools/gen_fonts_gen.py
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BICYCLE_DIR = os.path.normpath(os.path.join(HERE, ".."))
FONTS_DIR = os.path.join(BICYCLE_DIR, "fonts")
OUT_DIR = os.path.join(BICYCLE_DIR, "src", "App", "UI", "Resource", "fonts_gen")
LV_PM_FONT_DIR = os.path.join(BICYCLE_DIR, "src", "framework", "lv_pm", "font")
I18N_DIR = os.path.join(BICYCLE_DIR, "src", "App", "Service", "i18n")

REGULAR_TTF = os.path.join(FONTS_DIR, "SourceHanSansCN-Regular.ttf")
BOLD_TTF = os.path.join(FONTS_DIR, "SourceHanSansCN-Bold.ttf")
AWESOME_TTF = os.path.join(FONTS_DIR, "fa-solid-900.ttf")

BASE_ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
EXTRA_PUNCT = (
    "，。！？、；：""''（）【】《》…—·"
    "℃°×÷±"
    "←→↑↓"
)

# Legacy symbols baked into older fonts_gen (keep unless i18n scan covers them).
LEGACY_SYMBOLS = (
    "%()-./0123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "中于件低作使停充关功加动卡卫历压名向否器图均失始定就已平度开录形态总成"
    "打拔操放文无日时星是最期未机止池海状用电确离称程纬经绪编者航记译败距路"
    "载运速里量闭间高？"
)

REGULAR_SIZES = (14, 16, 20, 32, 36, 48)
AWESOME_SIZES = (12, 16, 20, 40)
AWESOME_RANGES = "0xf011,0xf017,0xf018,0xf0c9,0xf206,0xf241,0xf244,0xf2bb,0xf3c5,0xf7bf"


def collect_text_paths(extra: list[str]) -> list[str]:
    paths: list[str] = []
    for pattern in (
        os.path.join(I18N_DIR, "lv_i18n.c"),
        os.path.join(I18N_DIR, "translations", "*.yml"),
    ):
        paths.extend(glob.glob(pattern))
    for p in extra:
        if os.path.isfile(p):
            paths.append(p)
    return sorted(set(paths))


def collect_symbols(extra_paths: list[str]) -> str:
    chars: set[str] = set(BASE_ASCII + EXTRA_PUNCT + LEGACY_SYMBOLS)
    for path in collect_text_paths(extra_paths):
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            text = fp.read()
        for ch in text:
            if ch.isprintable() and not ch.isspace():
                chars.add(ch)
            elif ch in " \t":
                chars.add(" ")
    return "".join(sorted(chars, key=lambda c: ord(c)))


def run_lv_font_conv(args: list[str]) -> None:
    cmd = ["npx", "--yes", "lv_font_conv"] + args
    print("[lv_font_conv]", " ".join(args), file=sys.stderr)
    subprocess.check_call(cmd)


def generate_regular(size: int, symbols: str) -> None:
    out = os.path.join(OUT_DIR, f"font_regular_{size}.c")
    run_lv_font_conv(
        [
            "--font",
            REGULAR_TTF,
            "--size",
            str(size),
            "--bpp",
            "4",
            "--format",
            "lvgl",
            "--no-compress",
            "-r",
            "0x20-0x7E",
            "--symbols",
            symbols,
            "--lv-font-name",
            f"font_regular_{size}",
            "-o",
            out,
        ]
    )


def generate_bold(size: int, symbols: str) -> None:
    out = os.path.join(OUT_DIR, f"font_bold_{size}.c")
    run_lv_font_conv(
        [
            "--font",
            BOLD_TTF,
            "--size",
            str(size),
            "--bpp",
            "4",
            "--format",
            "lvgl",
            "--no-compress",
            "-r",
            "0x20-0x7E",
            "--symbols",
            symbols,
            "--lv-font-name",
            f"font_bold_{size}",
            "-o",
            out,
        ]
    )


def generate_awesome(size: int) -> None:
    out = os.path.join(LV_PM_FONT_DIR, f"font_awesome_{size}.c")
    run_lv_font_conv(
        [
            "--font",
            AWESOME_TTF,
            "--size",
            str(size),
            "--bpp",
            "4",
            "--format",
            "lvgl",
            "--no-compress",
            "-r",
            AWESOME_RANGES,
            "--lv-font-name",
            f"font_awesome_{size}",
            "-o",
            out,
        ]
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Regenerate bicycle fonts_gen bitmap fonts.")
    ap.add_argument("--extra", action="append", default=[], help="extra UTF-8 file to scan")
    ap.add_argument("--list-chars", action="store_true", help="print symbol set and exit")
    ap.add_argument("--regular-only", action="store_true", help="skip bold/awesome")
    args = ap.parse_args()

    for path in (REGULAR_TTF, BOLD_TTF, AWESOME_TTF):
        if not os.path.isfile(path):
            print(f"[error] missing font: {path}", file=sys.stderr)
            return 1

    symbols = collect_symbols(args.extra)
    cjk = sum(1 for c in symbols if "\u4e00" <= c <= "\u9fff")
    print(f"[charset] total={len(symbols)} CJK={cjk}", file=sys.stderr)
    for ch in "文件传输模式":
        print(f"  {ch}: {'OK' if ch in symbols else 'MISSING'}", file=sys.stderr)

    if args.list_chars:
        print(symbols)
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(LV_PM_FONT_DIR, exist_ok=True)
    for size in REGULAR_SIZES:
        generate_regular(size, symbols)
    if not args.regular_only:
        generate_bold(16, symbols)
        for size in AWESOME_SIZES:
            generate_awesome(size)

    print(f"[done] wrote fonts under {OUT_DIR}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
