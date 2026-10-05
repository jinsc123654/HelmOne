#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate Helm UI bitmap fonts from PingFang SC (苹方_特粗).

Small CJK on the 240×320 RGB565 panel needs ink more than hairline
cleanliness. 2bpp + strong hint keeps strokes 1–2px solid; Medium+1bpp
threshold ate the strokes and made menus unreadable.

Usage (from openvela root):
  python3 vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/tools/gen_helm_fonts.py
"""

from __future__ import annotations

import ast
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BICYCLE_DIR = os.path.normpath(os.path.join(HERE, ".."))
FONTS_DIR = os.path.join(BICYCLE_DIR, "fonts")
OUT_DIR = os.path.join(BICYCLE_DIR, "src", "lvgl_page", "fonts")

HEAVY_TTF = "/home/jinsc/SDK/SGl/PingFangSC/PingFangSC-Semibold.ttf"
FALLBACK_TTF = os.path.join(FONTS_DIR, "SourceHanSansCN-Bold.ttf")

BASE_ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
EXTRA_GLYPHS = "，。！？、；：·—…°℃×÷±←→↑↓●✓"

HELM_COPY = (
    "上次里程搜索中待开始导航中定位速度时间里程均速极速心率踏频功率圈速"
    "概览均心率最大心率恢复看清"
    "海拔坡度爬升下降累计剖面转向已暂停保存骑行本次轨迹并结束菜单坐标点"
    "常用点传感器记录设置请用导入历史停止跳过本点当前路线前往下一途经"
    "等待下发暂无已连接未连接扫描结束后会出现在这里蓝牙开启关闭亮度低"
    "中高显示夜间日光单位英制公制公里英里自动暂停手机通知仅来电最近查看推送"
    "传输固件版本与设备名关于离线码表请勿断电请勿关机写入来源当前可写入"
    "可导出可传文件正在更新未发现设备扫描踏频功率就绪已到达结束导航关机"
    "电量维护地图轨迹建目录删除重命名移动复制导出更新"
    "校准水平放置归零原始气压偏置启动中"
    "无定位无数据搜索时区"
    "星历同步过期有效建议尽快约上传"
    "蜂鸣器"
    "工厂模式"
    "条路书规划延伸衔接途经站折线"
    "直行稍向左右急转掉头出发到达"
    "逆行严重偏航打开失败保存"
    "回放删除返航沿轨迹模拟定位从本机移除按记录路线骑行从终点骑回起点已移除"
    "工具箱指南针水平仪加速度计高度计系统状态地磁罗盘十字圆环剩余空闲磁北倾斜峰值加载中气压定位"
    "已休眠背光已关定位已暂停按键或拿起即可唤醒"
)

# UI 位图字：菜单 / 通知 overlay / 转向文案。只抽 C 字符串，避免注释把字库撑爆。
SCAN_GLOBS = (
    os.path.join(BICYCLE_DIR, "src", "lvgl_page", "helm_*.c"),
    os.path.join(BICYCLE_DIR, "src", "lvgl_page", "live_map", "*.c"),
    os.path.join(BICYCLE_DIR, "src", "lvgl_page", "startup", "*.c"),
    os.path.join(BICYCLE_DIR, "src", "lvgl_page", "usb_transfer", "*.c"),
    os.path.join(BICYCLE_DIR, "src", "lvgl_page", "factory", "*.c"),
    os.path.join(BICYCLE_DIR, "src", "bicycle_status_bar.c"),
    os.path.join(BICYCLE_DIR, "src", "bicycle_demo.c"),
    os.path.join(BICYCLE_DIR, "src", "bicycle_ui_ctl.c"),
    os.path.join(BICYCLE_DIR, "src", "bicycle_mtp_ui.c"),
    os.path.join(BICYCLE_DIR, "src", "vmap", "vmap_route.c"),
    os.path.join(BICYCLE_DIR, "src", "vmap", "vmap_route_nav.c"),
    os.path.join(BICYCLE_DIR, "src", "framework", "lv_pm", "core", "lv_pm_overlay.c"),
)

C_STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"')
C_COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.S)
C_COMMENT_LINE = re.compile(r"//.*?$", re.M)

UI_SIZES = (12, 15, 22)
NUM_SIZES = (28, 32, 56, 64)
FALLBACK_SYMS = "✓"


def _c_string_chars(src: str) -> set[str]:
    chars: set[str] = set()
    stripped = C_COMMENT_LINE.sub("", C_COMMENT_BLOCK.sub("", src))
    for match in C_STRING_RE.finditer(stripped):
        try:
            text = ast.literal_eval(match.group(0))
        except (SyntaxError, ValueError):
            continue
        for ch in text:
            if ch.isprintable() and not ch.isspace():
                chars.add(ch)
    return chars


def collect_symbols() -> str:
    chars: set[str] = set(BASE_ASCII + EXTRA_GLYPHS + HELM_COPY)
    for pattern in SCAN_GLOBS:
        for path in glob.glob(pattern):
            with open(path, "r", encoding="utf-8", errors="replace") as fp:
                chars.update(_c_string_chars(fp.read()))
    return "".join(sorted(chars, key=ord))


def run_lv_font_conv(args: list[str]) -> None:
    cmd = ["npx", "--yes", "lv_font_conv"] + args
    print("[lv_font_conv]", " ".join(args[:8]), "...", file=sys.stderr)
    subprocess.check_call(cmd)


def generate(
    ttf: str,
    size: int,
    name: str,
    symbols: str,
    ascii_range: bool,
    *,
    bpp: int,
    fallback_syms: str = "",
) -> None:
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
        "--autohint-strong",
    ]
    if ascii_range:
        args.extend(["-r", "0x20-0x7E"])
    if symbols:
        args.extend(["--symbols", symbols])
    if fallback_syms and os.path.isfile(FALLBACK_TTF):
        args.extend([
            "--font", FALLBACK_TTF, "--autohint-strong", "--symbols", fallback_syms,
        ])
    run_lv_font_conv(args)


def collect_factory_symbols() -> str:
    """工厂页 + 板卡测试：从 factory/*.c 抽取全部可见字符，并含 ASCII。"""
    chars: set[str] = set(BASE_ASCII + EXTRA_GLYPHS)
    factory_glob = os.path.join(BICYCLE_DIR, "src", "lvgl_page", "factory", "*.c")
    for path in glob.glob(factory_glob):
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            chars.update(_c_string_chars(fp.read()))
    return "".join(sorted(chars, key=ord))


def generate_factory_title() -> None:
    """工厂固件专用位图字模（无 TTF 预加载）。"""
    os.makedirs(OUT_DIR, exist_ok=True)
    syms = collect_factory_symbols()
    cjk = sum(1 for c in syms if "\u4e00" <= c <= "\u9fff")
    print(f"[factory] glyphs={len(syms)} CJK={cjk}", file=sys.stderr)
    generate(
        HEAVY_TTF, 22, "helm_factory_22", syms, True,
        bpp=2,
    )


def main() -> int:
    if not os.path.isfile(HEAVY_TTF):
        print(f"[error] missing font: {HEAVY_TTF}", file=sys.stderr)
        return 1

    if len(sys.argv) > 1 and sys.argv[1] == "--factory":
        generate_factory_title()
        print(f"[done] helm_factory_22 {OUT_DIR}", file=sys.stderr)
        return 0

    symbols = collect_symbols()
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        cjk_s = "".join(c for c in symbols if "\u4e00" <= c <= "\u9fff")
        print(f"[check] total={len(symbols)} CJK={len(cjk_s)}", file=sys.stderr)
        print(cjk_s)
        return 0
    cjk = sum(1 for c in symbols if "\u4e00" <= c <= "\u9fff")
    print(f"[charset] total={len(symbols)} CJK={cjk}", file=sys.stderr)
    print(f"[font] {HEAVY_TTF} 2bpp autohint-strong", file=sys.stderr)
    os.makedirs(OUT_DIR, exist_ok=True)

    for size in UI_SIZES:
        generate(
            HEAVY_TTF, size, f"helm_ui_{size}", symbols, True,
            bpp=2, fallback_syms=FALLBACK_SYMS,
        )

    generate(
        HEAVY_TTF, 64, "helm_ui_64", "U←↑→↓●", False,
        bpp=2,
    )

    num_sym = "0123456789.:-/+%"
    for size in NUM_SIZES:
        generate(
            HEAVY_TTF, size, f"helm_num_{size}", num_sym, True,
            bpp=2,
        )

    print(f"[done] {OUT_DIR}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
