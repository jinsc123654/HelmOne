#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate compact on-device font-lab bitmaps (settings → 字体试验).

Lab charset is a short UI sample, not the production 337-glyph set.
Do not copy Apple TTF into git; PingFang is read from the local SDK path.
"""

from __future__ import annotations

import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
BICYCLE_DIR = os.path.normpath(os.path.join(HERE, ".."))
FONTS_DIR = os.path.join(BICYCLE_DIR, "fonts")
OUT_DIR = os.path.join(BICYCLE_DIR, "src", "lvgl_page", "fonts", "ftest")

PF_HEAVY = "/home/jinsc/SDK/SGl/PingFangSC/PingFangSC-Semibold.ttf"
SHS_BOLD = os.path.join(FONTS_DIR, "SourceHanSansCN-Bold.ttf")
NOTO_BOLD = "/home/jinsc/SDK/SGl/NotoSansSC/NotoSansSC-Bold.otf"
NOTO_XBOLD = "/home/jinsc/SDK/SGl/NotoSansSC/NotoSansSC-ExtraBold.ttf"
PUHUI_MED = os.path.join(FONTS_DIR, "Alibaba_PuHuiTi_2.0_65_Medium.ttf")
MISANS_MED = "/usr/share/fonts/truetype/misans/MiSans-Medium.ttf"

# Compact latin for labels: 2bpp / KEY1 / km/h / 17.1 / 3/8
LATIN = " 0123456789.:/-%+BbEhikmnptKEYxso★"

CJK = (
    "字体试验推荐当前量产传感器已连接骑行记录暂无"
    "下一套上一套长按返回适合小字菜单半透半反最大对比"
    "线稿同族小屏中等思源黑体粗小米谷歌普惠苹方特无强"
    "导航坐标点常用亮度中日光设置蓝牙开启关闭固件码表"
    "速度时间里程均速海拔对照像素"
)

NUM_SYM = "0123456789.:/-%+"

UI_SIZES = (12, 15)
NUM_SIZE = 32

# (id, ttf, bpp, hint)  hint: "strong" | "off"
VARIANTS = (
    ("shsb2", SHS_BOLD, 2, "strong"),
    ("mism2", MISANS_MED, 2, "strong"),
    ("notob2", NOTO_BOLD, 2, "strong"),
    ("puhm2", PUHUI_MED, 2, "strong"),
    ("pfh2", PF_HEAVY, 2, "strong"),
    ("shsb1", SHS_BOLD, 1, "strong"),
    ("notoe2", NOTO_XBOLD, 2, "strong"),
    ("pfh2n", PF_HEAVY, 2, "off"),
)


def unique_symbols(text: str) -> str:
    seen: set[str] = set()
    out: list[str] = []
    for ch in text:
        if ch in seen or ch == "\n":
            continue
        seen.add(ch)
        out.append(ch)
    return "".join(out)


def run_lv_font_conv(args: list[str]) -> None:
    cmd = ["npx", "--yes", "lv_font_conv"] + args
    print("[lv_font_conv]", args[args.index("--lv-font-name") + 1],
          file=sys.stderr)
    subprocess.check_call(cmd)


def generate(ttf: str, size: int, name: str, symbols: str, bpp: int,
             hint: str, ascii_range: bool) -> str:
    out = os.path.join(OUT_DIR, f"{name}.c")
    args = [
        "--size", str(size),
        "--bpp", str(bpp),
        "--format", "lvgl",
        "--no-compress",
        "--no-prefilter",
        "--no-kerning",
        "--lv-include", "lvgl/lvgl.h",
        "--lv-font-name", name,
        "-o", out,
        "--font", ttf,
    ]
    if hint == "strong":
        args.append("--autohint-strong")
    elif hint == "off":
        args.append("--autohint-off")
    if ascii_range:
        args.extend(["-r", "0x20-0x7E"])
    if symbols:
        args.extend(["--symbols", symbols])
    run_lv_font_conv(args)
    return out


def write_header(names: list[str]) -> None:
    path = os.path.join(OUT_DIR, "ftest_fonts.h")
    lines = [
        "/**",
        " * @file ftest_fonts.h",
        " * @brief 字体试验页位图（由 tools/gen_font_lab.py 生成）。",
        " */",
        "",
        "#ifndef FTEST_FONTS_H",
        "#define FTEST_FONTS_H",
        "",
        '#include "lvgl/lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for name in names:
        lines.append(f"LV_FONT_DECLARE({name});")
    lines.extend([
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif /* FTEST_FONTS_H */",
        "",
    ])
    with open(path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    print(f"[header] {path}", file=sys.stderr)


def main() -> int:
    missing = [p for _, p, _, _ in VARIANTS if not os.path.isfile(p)]
    if missing:
        for p in missing:
            print(f"[error] missing font: {p}", file=sys.stderr)
        return 1

    ui_sym = unique_symbols(LATIN + CJK)
    print(f"[charset] ui={len(ui_sym)} num={len(NUM_SYM)}", file=sys.stderr)
    os.makedirs(OUT_DIR, exist_ok=True)

    jobs: list[tuple] = []
    names: list[str] = []
    for vid, ttf, bpp, hint in VARIANTS:
        for size in UI_SIZES:
            name = f"ftest_{vid}_{size}"
            names.append(name)
            jobs.append((ttf, size, name, ui_sym, bpp, hint, False))
        name = f"ftest_{vid}_{NUM_SIZE}"
        names.append(name)
        jobs.append((ttf, NUM_SIZE, name, NUM_SYM, bpp, hint, False))

    write_header(names)

    nerr = 0
    with ThreadPoolExecutor(max_workers=4) as pool:
        futs = [pool.submit(generate, *job) for job in jobs]
        for fut in as_completed(futs):
            try:
                out = fut.result()
                print(f"[ok] {os.path.basename(out)}", file=sys.stderr)
            except subprocess.CalledProcessError as exc:
                nerr += 1
                print(f"[fail] {exc}", file=sys.stderr)

    if nerr:
        print(f"[done] {nerr} failed", file=sys.stderr)
        return 1
    print(f"[done] {OUT_DIR} files={len(jobs)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
