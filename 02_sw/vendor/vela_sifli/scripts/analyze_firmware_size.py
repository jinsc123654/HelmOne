#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Analyze NuttX firmware size (nuttx.bin / nuttx ELF) for my_vendor boards.

Usage (from openvela root):
  python3 vendor/my_vendor/scripts/analyze_firmware_size.py
  python3 vendor/my_vendor/scripts/analyze_firmware_size.py --config nsh-driver
  python3 vendor/my_vendor/scripts/analyze_firmware_size.py --all
  python3 vendor/my_vendor/scripts/analyze_firmware_size.py --elf cmake_out/my_vendor_nsh/nuttx --json report.json

Default: compact colored component table (libc unified, each app on its own row).
--all: sections, top symbols, archives, defconfig hints, etc.

Requires arm-none-eabi-size, arm-none-eabi-nm, arm-none-eabi-readelf on PATH.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from pathlib import Path

SCRIPT = Path(__file__).resolve()
MY_VENDOR = SCRIPT.parent.parent
OPENVELA = MY_VENDOR.parent.parent

DEFAULT_BOARD = "my_vendor"
DEFAULT_CONFIG = "nsh"

FLASH_SECTION_PREFIXES = (".text", ".rodata", ".data", ".ARM.extab", ".ARM.exidx")

MAP_SEC_LINE = re.compile(
    r"^\s+\.([\w.]+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+"
    r"((?:\S+\.a)\([^)]+\)|\S+\.(?:c|cxx)\.o)\s*$"
)
MAP_CONT_LINE = re.compile(
    r"^\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+"
    r"((?:\S+\.a)\([^)]+\)|\S+\.(?:c|cxx)\.o)\s*$"
)
MAP_SECTION_ONLY = re.compile(r"^\s+\.([\w.]+)\s*$")
UNIT_RE = re.compile(r"^(\S+\.a)\([^)]+\)$")
SRAM_SECTION_TOPS = frozenset({".data", ".bss"})
MAP_LOAD_RE = re.compile(r"^LOAD\s+(\S+\.a)\s*$")
ARCHIVE_RE = re.compile(r"(\S+\.a)\([^)]+\)")

DEFAULT_RANK_TOP = 10

RANK_SKIP_KEYS = frozenset({"gap", "sram_gap"})

NSH_ARCHIVES = frozenset({"libapps_nsh.a", "libapps_sh.a", "libapps_builtin.a"})

DEFCONFIG_HINTS: list[tuple[str, str]] = [
    ("CONFIG_GRAPHICS_LVGL=y", "LVGL library"),
    ("CONFIG_MYVENDOR_BICYCLE=y", "bicycle UI (C++)"),
    ("CONFIG_MYVENDOR_LVGL_STACK=y", "LVGL stack master switch"),
    ("CONFIG_DEBUG_NOOPT=y", "no compiler optimization (-O0)"),
    ("CONFIG_DEBUG_SYMBOLS=y", "debug symbols in ELF (not in .bin)"),
    ("CONFIG_ALLSYMS=y", "g_allsyms table (~60 KB in .data)"),
    ("CONFIG_HAVE_CXX=y", "C++ runtime"),
    ("CONFIG_LIBCXX=y", "libc++ (large)"),
    ("CONFIG_LIB_PNG=y", "libpng"),
    ("CONFIG_LIB_ZLIB=y", "zlib"),
    ("CONFIG_ELF=y", "ELF loader"),
    ("CONFIG_BLUETOOTH_FRAMEWORK=y", "Bluetooth framework"),
    ("CONFIG_BT=y", "Zblue BT stack"),
    ("CONFIG_FRAME_POINTER=y", "frame pointers"),
]

# Non-app symbol categories for --all top-symbol listing
SYMBOL_RULES: list[tuple[str, str]] = [
    (r"^(_Z|_ZN)", "C++ runtime"),
    (r"^(lv_|lvgl|glyph_bitmap|style_init)", "LVGL"),
    (r"^(png_|inflate|deflate|adler32|crc32|zlib)", "PNG/ZLIB"),
    (r"^(HAL_|lcd|spi|i2c|gpio|dma|BSP_)", "HAL/drivers"),
    (r"^(nsh_|cmd_)", "NSH"),
    (r"_main$", "app entry"),
]


@dataclass
class SectionInfo:
    name: str
    size: int


@dataclass
class ComponentRow:
    key: str
    label: str
    kind: str
    bytes: int
    percent: float
    sram_bytes: int = 0
    sram_percent: float = 0.0


@dataclass
class MemoryRow:
    label: str
    flash: int | None = None
    sram: int | None = None
    bold: bool = False


@dataclass
class FileInfo:
    path: str
    size: int


@dataclass
class Report:
    elf: str
    bin: str | None
    config: str
    elf_file_bytes: int | None
    bin_file_bytes: int | None
    sections: list[SectionInfo] = field(default_factory=list)
    flash_bytes: int | None = None
    sram_bytes: int | None = None
    size_summary: dict[str, int] = field(default_factory=dict)
    components: list[ComponentRow] = field(default_factory=list)
    apps: list[ComponentRow] = field(default_factory=list)
    largest_symbols: list[dict[str, str | int]] = field(default_factory=list)
    largest_archives: list[FileInfo] = field(default_factory=list)
    map_archive_refs: list[dict[str, str | int]] = field(default_factory=list)
    defconfig: str | None = None
    defconfig_flags: list[str] = field(default_factory=list)


class Color:
    def __init__(self, enabled: bool) -> None:
        self.enabled = enabled

    def wrap(self, text: str, code: str) -> str:
        if not self.enabled:
            return text
        return f"\033[{code}m{text}\033[0m"

    def bold(self, text: str) -> str:
        return self.wrap(text, "1")

    def dim(self, text: str) -> str:
        return self.wrap(text, "2")

    def cyan(self, text: str) -> str:
        return self.wrap(text, "36")

    def green(self, text: str) -> str:
        return self.wrap(text, "32")

    def yellow(self, text: str) -> str:
        return self.wrap(text, "33")

    def blue(self, text: str) -> str:
        return self.wrap(text, "34")

    def magenta(self, text: str) -> str:
        return self.wrap(text, "35")

    def red(self, text: str) -> str:
        return self.wrap(text, "31")


def find_openvela_root(start: Path | None = None) -> Path:
    cur = (start or Path.cwd()).resolve()
    for directory in (cur, *cur.parents):
        if (directory / "build.sh").is_file() and (directory / "nuttx").is_dir():
            return directory
    raise RuntimeError(
        "Cannot find openvela root (need build.sh and nuttx/). "
        "Run from the openvela tree or pass --root."
    )


def cmake_out_dir(root: Path, board: str, config: str) -> Path:
    return root / "cmake_out" / f"{board}_{config}"


def defconfig_path(root: Path, board: str, config: str) -> Path:
    return (
        root
        / "vendor/my_vendor/boards/sf32lb52/my_vendor/configs"
        / config
        / "defconfig"
    )


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"Required tool not found on PATH: {name}")
    return path


def run_tool(args: list[str]) -> str:
    try:
        proc = subprocess.run(
            args,
            check=True,
            capture_output=True,
            text=True,
            errors="replace",
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"Command failed: {' '.join(args)}\n{exc.stderr or exc.stdout}"
        ) from exc
    return proc.stdout


def parse_hex_size(token: str) -> int:
    token = token.strip()
    if not token:
        return 0
    return int(token, 16) if not token.isdecimal() else int(token)


def section_top(name: str) -> str:
    return "." + name.split(".")[0]


def flash_section(name: str) -> bool:
    return any(section_top(name).startswith(prefix) for prefix in FLASH_SECTION_PREFIXES)


def sram_section(name: str) -> bool:
    return section_top(name) in SRAM_SECTION_TOPS


def normalize_link_unit(token: str) -> str:
    """Strip archive member suffix: libfoo.a(bar.o) -> libfoo.a path."""
    m = UNIT_RE.match(token)
    if m:
        return m.group(1)
    return token


def parse_map_by_unit(
    map_path: Path,
    section_filter,
) -> dict[str, int]:
    """Sum section bytes per link unit (archive or standalone .o) from nuttx.map."""
    if not map_path.is_file():
        return {}

    sizes: dict[str, int] = defaultdict(int)
    current = ""

    for line in map_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = MAP_SEC_LINE.match(line)
        if m:
            current = m.group(1)
            if section_filter(current):
                sizes[normalize_link_unit(m.group(3))] += int(m.group(2), 16)
            continue
        m = MAP_SECTION_ONLY.match(line)
        if m:
            current = m.group(1)
            continue
        m = MAP_CONT_LINE.match(line)
        if m and current and section_filter(current):
            sizes[normalize_link_unit(m.group(2))] += int(m.group(1), 16)

    return dict(sizes)


def parse_map_flash_by_unit(map_path: Path) -> dict[str, int]:
    return parse_map_by_unit(map_path, flash_section)


def parse_map_sram_by_unit(map_path: Path) -> dict[str, int]:
    return parse_map_by_unit(map_path, sram_section)


def parse_map_loaded_archives(map_path: Path) -> list[str]:
    if not map_path.is_file():
        return []
    loaded: list[str] = []
    for line in map_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = MAP_LOAD_RE.match(line.strip())
        if m:
            loaded.append(m.group(1))
    return loaded


def archive_basename(unit: str) -> str:
    return Path(unit).name


def app_name_from_archive(archive: str) -> str:
    base = archive_basename(archive)
    if base.startswith("libapps_") and base.endswith(".a"):
        return base[len("libapps_") : -len(".a")]
    return base


def is_app_archive(archive: str) -> bool:
    base = archive_basename(archive)
    return base.startswith("libapps_") and base not in NSH_ARCHIVES


def classify_unit(unit: str) -> tuple[str, str, str]:
    """
    Return (key, label, kind).
    kind: libc | platform | lib | nsh | app | other
    """
    base = archive_basename(unit)

    if base in ("libc.a", "libm.a", "libgcc.a"):
        return ("libc", "libc (+ libm, libgcc)", "libc")

    platform_map = {
        "libarch.a": ("arch", "arch", "platform"),
        "libsched.a": ("sched", "scheduler", "platform"),
        "libmm.a": ("mm", "memory manager", "platform"),
        "libfs.a": ("fs", "filesystems", "platform"),
        "libdrivers.a": ("drivers", "drivers", "platform"),
        "libboard.a": ("board", "board", "platform"),
    }
    if base in platform_map:
        return platform_map[base]

    if base in NSH_ARCHIVES:
        return ("nsh", "NSH shell", "nsh")

    lib_map = {
        "liblvgl.a": ("lvgl", "LVGL", "lib"),
        "liblibcxx.a": ("libcxx", "libc++", "lib"),
        "liblibcxxabi.a": ("libcxxabi", "libc++abi", "lib"),
        "libpng_static.a": ("libpng", "libpng", "lib"),
        "libzlib.a": ("zlib", "zlib", "lib"),
        "libapps_bicycle.a": ("bicycle", "bicycle (UI)", "lib"),
        "libmbedtls.a": ("mbedtls", "mbedTLS", "lib"),
        "libzblue.a": ("zblue", "Bluetooth (zblue)", "lib"),
        "liblibbluetooth.a": ("bt_framework", "Bluetooth framework", "lib"),
        "liblibuv.a": ("libuv", "libuv", "lib"),
        "libunqlite.a": ("unqlite", "unqlite", "lib"),
        "libtinycrypt.a": ("tinycrypt", "tinycrypt", "lib"),
        "libapps.a": ("apps_common", "apps (common)", "lib"),
        "libbinfmt.a": ("binfmt", "binfmt", "lib"),
        "libandroid.a": ("android", "android HAL", "lib"),
    }
    if base in lib_map:
        return lib_map[base]

    if is_app_archive(unit):
        name = app_name_from_archive(base)
        return (f"app:{name}", f"app: {name}", "app")

    if unit.endswith(".o"):
        short = Path(unit).name
        if "allsyms" in short:
            return ("allsyms", "allsyms", "other")
        return (f"obj:{short}", short, "other")

    return (f"unit:{base}", base, "other")


def build_components(
    unit_flash: dict[str, int],
    unit_sram: dict[str, int],
    loaded_archives: list[str],
    flash_total: int,
    sram_total: int,
) -> tuple[list[ComponentRow], list[ComponentRow]]:
    """Build summary rows; apps are always listed (loaded list), even at 0 B."""
    grouped_flash: dict[str, int] = defaultdict(int)
    grouped_sram: dict[str, int] = defaultdict(int)
    labels: dict[str, str] = {}
    kinds: dict[str, str] = {}

    for unit, size in unit_flash.items():
        key, label, kind = classify_unit(unit)
        grouped_flash[key] += size
        labels[key] = label
        kinds[key] = kind

    for unit, size in unit_sram.items():
        key, label, kind = classify_unit(unit)
        grouped_sram[key] += size
        labels.setdefault(key, label)
        kinds.setdefault(key, kind)

    app_keys: dict[str, str] = {}
    for archive in loaded_archives:
        if is_app_archive(archive):
            key, label, kind = classify_unit(archive)
            app_keys[key] = label
            labels.setdefault(key, label)
            kinds.setdefault(key, kind)

    def flash_pct(nbytes: int) -> float:
        return (100.0 * nbytes / flash_total) if flash_total else 0.0

    def sram_pct(nbytes: int) -> float:
        return (100.0 * nbytes / sram_total) if sram_total else 0.0

    order = [
        "libc",
        "arch",
        "sched",
        "mm",
        "fs",
        "drivers",
        "board",
        "nsh",
        "lvgl",
        "bicycle",
        "libcxx",
        "libcxxabi",
        "libpng",
        "zlib",
        "mbedtls",
        "zblue",
        "bt_framework",
        "libuv",
        "unqlite",
        "tinycrypt",
        "binfmt",
        "android",
        "apps_common",
        "allsyms",
    ]

    rows: list[ComponentRow] = []
    used: set[str] = set()

    for key in order:
        if key not in grouped_flash and key not in grouped_sram and key not in labels:
            continue
        flash_n = grouped_flash.get(key, 0)
        sram_n = grouped_sram.get(key, 0)
        rows.append(
            ComponentRow(
                key=key,
                label=labels[key],
                kind=kinds.get(key, "platform"),
                bytes=flash_n,
                percent=flash_pct(flash_n),
                sram_bytes=sram_n,
                sram_percent=sram_pct(sram_n),
            )
        )
        used.add(key)

    other_flash = 0
    other_sram = 0
    for key in set(grouped_flash) | set(grouped_sram):
        if key in used or key.startswith("app:"):
            continue
        other_flash += grouped_flash.get(key, 0)
        other_sram += grouped_sram.get(key, 0)
    if other_flash or other_sram:
        rows.append(
            ComponentRow(
                key="other",
                label="other / link units",
                kind="other",
                bytes=other_flash,
                percent=flash_pct(other_flash),
                sram_bytes=other_sram,
                sram_percent=sram_pct(other_sram),
            )
        )

    app_rows: list[ComponentRow] = []
    for key in sorted(app_keys, key=lambda k: app_keys[k]):
        flash_n = grouped_flash.get(key, 0)
        sram_n = grouped_sram.get(key, 0)
        app_rows.append(
            ComponentRow(
                key=key,
                label=app_keys[key],
                kind="app",
                bytes=flash_n,
                percent=flash_pct(flash_n),
                sram_bytes=sram_n,
                sram_percent=sram_pct(sram_n),
            )
        )

    accounted_flash = (
        sum(r.bytes for r in rows)
        + sum(r.bytes for r in app_rows)
    )
    accounted_sram = (
        sum(r.sram_bytes for r in rows)
        + sum(r.sram_bytes for r in app_rows)
    )
    flash_gap = flash_total - accounted_flash
    if flash_gap > 1024:
        rows.append(
            ComponentRow(
                key="gap",
                label="padding / map remainder",
                kind="other",
                bytes=flash_gap,
                percent=flash_pct(flash_gap),
            )
        )
    sram_gap = sram_total - accounted_sram
    if sram_gap > 1024:
        rows.append(
            ComponentRow(
                key="sram_gap",
                label="SRAM map remainder",
                kind="other",
                bytes=0,
                percent=0.0,
                sram_bytes=sram_gap,
                sram_percent=sram_pct(sram_gap),
            )
        )

    return rows, app_rows


def parse_size_summary(elf: Path) -> dict[str, int]:
    require_tool("arm-none-eabi-size")
    out = run_tool(["arm-none-eabi-size", str(elf)])
    lines = [ln for ln in out.splitlines() if ln.strip()]
    if len(lines) < 2:
        return {}
    parts = lines[-1].split()
    if len(parts) < 4:
        return {}
    return {
        "text": int(parts[0]),
        "data": int(parts[1]),
        "bss": int(parts[2]),
        "total": int(parts[3]),
    }


def parse_sections(elf: Path) -> list[SectionInfo]:
    out = run_tool(["arm-none-eabi-readelf", "-S", str(elf)])
    sections: list[SectionInfo] = []
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+\S+\s+\S+\s+([0-9a-fA-F]+)\s+", line)
        if not m:
            continue
        name, size_hex = m.group(1), m.group(2)
        if name in (
            ".text",
            ".data",
            ".bss",
            ".rodata",
            ".ARM.extab",
            ".ARM.exidx",
        ):
            sections.append(SectionInfo(name=name, size=int(size_hex, 16)))
    return sections


def flash_image_bytes(sections: list[SectionInfo], bin_path: Path | None) -> int | None:
    if bin_path and bin_path.is_file():
        return bin_path.stat().st_size
    flash_sections = {".text", ".data", ".rodata", ".ARM.extab", ".ARM.exidx"}
    total = sum(s.size for s in sections if s.name in flash_sections)
    return total or None


def parse_nm_symbols(elf: Path, max_symbol_size: int = 500_000) -> list[tuple[int, str, str]]:
    require_tool("arm-none-eabi-nm")
    out = run_tool(["arm-none-eabi-nm", "--print-size", "-S", str(elf)])
    symbols: list[tuple[int, str, str]] = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        size = parse_hex_size(parts[1])
        if size <= 0 or size > max_symbol_size:
            continue
        sym_type = parts[2]
        name = " ".join(parts[3:])
        symbols.append((size, sym_type, name))
    return symbols


def categorize_symbol(name: str) -> str:
    for pattern, category in SYMBOL_RULES:
        if re.search(pattern, name):
            return category
    return "kernel/libc/misc"


def largest_non_app_symbols(
    symbols: list[tuple[int, str, str]], top: int
) -> list[dict[str, str | int]]:
    filtered = [
        (size, typ, name)
        for size, typ, name in symbols
        if categorize_symbol(name) != "app entry"
    ]
    ranked = sorted(filtered, key=lambda x: -x[0])[:top]
    return [
        {
            "size": size,
            "size_kb": round(size / 1024, 1),
            "type": typ,
            "name": name,
            "category": categorize_symbol(name),
        }
        for size, typ, name in ranked
    ]


def scan_archives(build_dir: Path, top: int) -> list[FileInfo]:
    if not build_dir.is_dir():
        return []
    require_tool("arm-none-eabi-size")
    rows: list[FileInfo] = []
    for archive in build_dir.rglob("*.a"):
        try:
            out = run_tool(["arm-none-eabi-size", "-t", str(archive)])
        except RuntimeError:
            continue
        lines = [ln for ln in out.splitlines() if ln.strip()]
        if len(lines) < 2:
            continue
        parts = lines[-1].split()
        if len(parts) < 2:
            continue
        text_data = int(parts[0]) + int(parts[1])
        rows.append(FileInfo(path=str(archive.relative_to(build_dir)), size=text_data))
    rows.sort(key=lambda r: -r.size)
    return rows[:top]


def parse_map_archive_refs(map_path: Path, top: int) -> list[dict[str, str | int]]:
    if not map_path.is_file():
        return []
    counts: dict[str, int] = defaultdict(int)
    in_text = False
    for line in map_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(".text"):
            in_text = True
            continue
        if in_text and line.startswith("."):
            if line.startswith(".data") or line.startswith(".bss") or line.startswith(".ARM"):
                in_text = False
            continue
        if in_text:
            m = ARCHIVE_RE.search(line)
            if m:
                counts[Path(m.group(1)).name] += 1
    ranked = sorted(counts.items(), key=lambda kv: -kv[1])[:top]
    return [{"archive": name, "text_refs": count} for name, count in ranked]


def read_defconfig_flags(defconfig: Path) -> list[str]:
    if not defconfig.is_file():
        return []
    text = defconfig.read_text(encoding="utf-8", errors="replace")
    flags: list[str] = []
    for needle, label in DEFCONFIG_HINTS:
        if needle in text:
            flags.append(f"{label} ({needle})")
    if "CONFIG_DEBUG_NOOPT=y" not in text:
        flags.append("Compiler optimizations likely enabled (CONFIG_DEBUG_NOOPT off)")
    return flags


def build_memory_rows(report: Report) -> tuple[list[MemoryRow], int, int]:
    """Flash/SRAM breakdown from arm-none-eabi-size and nuttx.bin."""
    s = report.size_summary
    text = int(s.get("text") or 0)
    data = int(s.get("data") or 0)
    bss = int(s.get("bss") or 0)
    flash_total = report.bin_file_bytes or (text + data) or report.flash_bytes or 0
    sram_total = data + bss
    rows = [
        MemoryRow("text", flash=text),
        MemoryRow("initialized (.data)", flash=data, sram=data),
        MemoryRow("uninit (.bss)", sram=bss),
        MemoryRow("total", flash=flash_total, sram=sram_total, bold=True),
    ]
    return rows, flash_total, sram_total


def _memory_cell(n: int | None) -> str:
    if n is None:
        return "—"
    return format_bytes(n, compact=True)


def _memory_share(n: int | None, total: int) -> str:
    if n is None or not total:
        return "      —"
    return f"{100.0 * n / total:6.1f}%"


def print_memory_summary_table(
    title: str,
    rows: list[MemoryRow],
    *,
    flash_total: int,
    sram_total: int,
    c: Color,
) -> None:
    if not rows:
        return

    label_w = max(len(r.label) for r in rows)
    label_w = max(label_w, len("Region"))
    label_w = min(label_w, 24)

    print(c.bold(title))
    header = (
        f"{'Region':<{label_w}}  "
        f"{'Flash':>10}  {'Share':>7}  "
        f"{'SRAM':>10}  {'Share':>7}"
    )
    print(c.dim(header))
    print(c.dim("-" * (label_w + 42)))

    for row in rows:
        if row.bold:
            flash_share = "      —"
            sram_share = "      —"
        else:
            flash_share = _memory_share(row.flash, flash_total)
            sram_share = _memory_share(row.sram, sram_total)
        line = (
            f"{row.label:<{label_w}}  "
            f"{_memory_cell(row.flash):>10}  {flash_share}  "
            f"{_memory_cell(row.sram):>10}  {sram_share}"
        )
        print(c.bold(line) if row.bold else line)
    print()


def format_bytes(n: int | None, *, compact: bool = False) -> str:
    if n is None:
        return "n/a"
    if compact:
        if n >= 1024 * 1024:
            return f"{n / (1024 * 1024):.2f} MB"
        if n >= 1024:
            return f"{n / 1024:.1f} KB"
        return f"{n} B"
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.2f} MB ({n:,} B)"
    if n >= 1024:
        return f"{n / 1024:.1f} KB ({n:,} B)"
    return f"{n} B"


def kind_color(c: Color, kind: str) -> str:
    return {
        "libc": c.yellow,
        "platform": c.blue,
        "nsh": c.cyan,
        "lib": c.magenta,
        "app": c.green,
        "other": c.dim,
    }.get(kind, lambda x: x)


def print_table(
    title: str,
    rows: list[ComponentRow],
    *,
    c: Color,
    flash_total: int,
    sram_total: int,
) -> None:
    if not rows:
        return

    label_w = max(len(r.label) for r in rows)
    label_w = max(label_w, len("Component"))
    label_w = min(label_w, 36)

    print(c.bold(title))
    header = (
        f"{'Component':<{label_w}}  "
        f"{'Flash':>10}  {'F%':>6}  "
        f"{'SRAM':>10}  {'S%':>6}"
    )
    print(c.dim(header))
    print(c.dim("-" * (label_w + 38)))

    for row in rows:
        paint = kind_color(c, row.kind)
        label = row.label if len(row.label) <= label_w else row.label[: label_w - 1] + "…"
        sram_cell = (
            format_bytes(row.sram_bytes, compact=True)
            if row.sram_bytes
            else "—"
        )
        sram_pct_cell = f"{row.sram_percent:5.1f}%" if row.sram_bytes else "     —"
        line = (
            f"{label:<{label_w}}  "
            f"{format_bytes(row.bytes, compact=True):>10}  {row.percent:5.1f}%  "
            f"{sram_cell:>10}  {sram_pct_cell}"
        )
        print(paint(line))

    flash_sub = sum(r.bytes for r in rows)
    sram_sub = sum(r.sram_bytes for r in rows)
    flash_pct = (100.0 * flash_sub / flash_total) if flash_total else 0.0
    sram_pct = (100.0 * sram_sub / sram_total) if sram_sub and sram_total else 0.0
    sram_sub_cell = format_bytes(sram_sub, compact=True) if sram_sub else "—"
    sram_pct_cell = f"{sram_pct:5.1f}%" if sram_sub else "     —"
    print(c.dim("-" * (label_w + 38)))
    print(
        c.bold(
            f"{'subtotal':<{label_w}}  "
            f"{format_bytes(flash_sub, compact=True):>10}  {flash_pct:5.1f}%  "
            f"{sram_sub_cell:>10}  {sram_pct_cell}"
        )
    )
    print()


def merged_component_rows(report: Report) -> list[ComponentRow]:
    """Platform + apps for unified rankings (exclude map padding rows)."""
    rows = [r for r in report.components if r.key not in RANK_SKIP_KEYS]
    rows.extend(report.apps)
    return rows


def print_ranking_table(
    title: str,
    ranked: list[ComponentRow],
    *,
    metric: str,
    c: Color,
) -> None:
    if not ranked:
        return

    label_w = max(len(r.label) for r in ranked)
    label_w = max(label_w, len("Component"))
    label_w = min(label_w, 36)

    print(c.bold(title))
    header = f"{'#':>2}  {'Component':<{label_w}}  {'Size':>10}  {'Share':>7}"
    print(c.dim(header))
    print(c.dim("-" * (label_w + 26)))

    for rank, row in enumerate(ranked, start=1):
        paint = kind_color(c, row.kind)
        label = row.label if len(row.label) <= label_w else row.label[: label_w - 1] + "…"
        if metric == "flash":
            size = row.bytes
            share = row.percent
        else:
            size = row.sram_bytes
            share = row.sram_percent
        line = (
            f"{rank:2d}  {label:<{label_w}}  "
            f"{format_bytes(size, compact=True):>10}  {share:6.1f}%"
        )
        print(paint(line))
    print()


def print_rankings(
    report: Report,
    *,
    c: Color,
    top: int = DEFAULT_RANK_TOP,
) -> None:
    """Top-N components by Flash and by SRAM (platform + apps)."""
    pool = merged_component_rows(report)
    flash_ranked = sorted(
        [r for r in pool if r.bytes > 0],
        key=lambda r: -r.bytes,
    )[:top]
    sram_ranked = sorted(
        [r for r in pool if r.sram_bytes > 0],
        key=lambda r: -r.sram_bytes,
    )[:top]

    n_flash = min(top, len([r for r in pool if r.bytes > 0]))
    n_sram = min(top, len([r for r in pool if r.sram_bytes > 0]))
    print_ranking_table(
        f"Top {n_flash} by Flash",
        flash_ranked,
        metric="flash",
        c=c,
    )
    print_ranking_table(
        f"Top {n_sram} by SRAM",
        sram_ranked,
        metric="sram",
        c=c,
    )


def sram_runtime_bytes(size_summary: dict[str, int]) -> int:
    return int(size_summary.get("data") or 0) + int(size_summary.get("bss") or 0)


def print_brief(report: Report, *, c: Color, top: int = DEFAULT_RANK_TOP) -> None:
    """Post-build concise summary (used by build_board.py / vela_my_vendor_tools)."""
    flash = report.flash_bytes or 0
    sram = report.sram_bytes or 0
    mem_rows, flash_total, sram_total = build_memory_rows(report)
    print()
    print(c.bold(f"=== Firmware size ({report.config}) ==="))
    print_memory_summary_table(
        "Memory usage",
        mem_rows,
        flash_total=flash_total,
        sram_total=sram_total,
        c=c,
    )

    platform = [
        r for r in report.components if r.key not in RANK_SKIP_KEYS
    ]
    print_table(
        "Platform & libraries",
        platform,
        c=c,
        flash_total=flash,
        sram_total=sram,
    )

    active_apps = [a for a in report.apps if a.bytes > 0 or a.sram_bytes > 0]
    zero_apps = len(report.apps) - len(active_apps)
    if active_apps:
        print_table(
            "Applications (linked)",
            active_apps,
            c=c,
            flash_total=flash,
            sram_total=sram,
        )
    if zero_apps:
        print(
            c.dim(
                f"  ({zero_apps} apps in image with 0 B flash and 0 B SRAM — "
                "run without --brief to list all)"
            )
        )

    print_rankings(report, c=c, top=top)

    print_memory_summary_table(
        "Memory usage",
        mem_rows,
        flash_total=flash_total,
        sram_total=sram_total,
        c=c,
    )


def print_summary(report: Report, *, c: Color, top: int = DEFAULT_RANK_TOP) -> None:
    flash = report.flash_bytes or 0
    sram = report.sram_bytes or 0
    mem_rows, flash_total, sram_total = build_memory_rows(report)
    print(c.bold("=" * 72))
    print(c.bold("Firmware size report"))
    print(c.bold("=" * 72))
    print(f"Config : {report.config}")
    print(f"ELF    : {report.elf}")
    if report.elf_file_bytes is not None:
        print(f"         file {format_bytes(report.elf_file_bytes)}")
    if report.bin:
        print(f"BIN    : {report.bin}")
        if report.bin_file_bytes is not None:
            print(f"         flash {c.bold(format_bytes(report.bin_file_bytes))}")
    print()
    print_memory_summary_table(
        "Memory usage",
        mem_rows,
        flash_total=flash_total,
        sram_total=sram_total,
        c=c,
    )

    print_table(
        "Platform & libraries (from nuttx.map)",
        report.components,
        c=c,
        flash_total=flash,
        sram_total=sram,
    )
    print_table(
        "Applications (each app, always listed)",
        report.apps,
        c=c,
        flash_total=flash,
        sram_total=sram,
    )

    if not report.apps:
        print(c.dim("  (no libapps_* applications in link map LOAD list)"))

    print_rankings(report, c=c, top=top)

    print_memory_summary_table(
        "Memory usage",
        mem_rows,
        flash_total=flash_total,
        sram_total=sram_total,
        c=c,
    )


def print_verbose(report: Report, *, c: Color, top: int) -> None:
    if report.sections:
        print(c.bold("Sections (readelf)"))
        for sec in report.sections:
            print(f"  {sec.name:14} {format_bytes(sec.size)}")
        print()

    if report.largest_symbols:
        print(c.bold(f"Top {min(top, len(report.largest_symbols))} symbols (excluding app entry)"))
        for row in report.largest_symbols[:top]:
            print(
                f"  {row['size_kb']:7.1f} KB  [{row['category']}]  {row['name']}"
            )
        print()

    if report.largest_archives:
        print(c.bold("Largest static archives in build tree (upper bound)"))
        for arc in report.largest_archives:
            print(f"  {arc.size / 1024:7.1f} KB  {arc.path}")
        print()

    if report.map_archive_refs:
        print(c.bold("Archive references in .text (nuttx.map)"))
        for row in report.map_archive_refs:
            print(f"  {row['text_refs']:5d} refs  {row['archive']}")
        print()

    if report.defconfig:
        print(f"defconfig: {report.defconfig}")
        if report.defconfig_flags:
            print("Size-related options:")
            for flag in report.defconfig_flags:
                print(f"  - {flag}")
        print()

    print(c.dim("Tips:"))
    print(c.dim("  - nuttx.bin ≈ flash; ELF grows with CONFIG_DEBUG_SYMBOLS"))
    print(c.dim("  - Disable MYVENDOR_LVGL_STACK or use configs/nsh-driver for driver builds"))
    print(c.dim("  - Turn off CONFIG_DEBUG_NOOPT for release (-Os saves ~30-50%)"))


def build_report(
    *,
    elf: Path,
    bin_path: Path | None,
    defconfig: Path | None,
    build_dir: Path | None,
    config_name: str,
    top: int,
) -> Report:
    elf = elf.resolve()
    if not elf.is_file():
        raise RuntimeError(f"ELF not found: {elf}")

    bin_resolved = bin_path.resolve() if bin_path else elf.with_name("nuttx.bin")
    if not bin_resolved.is_file():
        bin_resolved = None

    map_path = elf.parent / "nuttx.map"
    sections = parse_sections(elf)
    symbols = parse_nm_symbols(elf)
    size_summary = parse_size_summary(elf)
    flash_total = flash_image_bytes(sections, bin_resolved) or 0
    sram_total = sram_runtime_bytes(size_summary)
    unit_flash = parse_map_flash_by_unit(map_path)
    unit_sram = parse_map_sram_by_unit(map_path)
    loaded = parse_map_loaded_archives(map_path)
    components, apps = build_components(
        unit_flash, unit_sram, loaded, flash_total, sram_total
    )

    return Report(
        elf=str(elf),
        bin=str(bin_resolved) if bin_resolved else None,
        config=config_name,
        elf_file_bytes=elf.stat().st_size,
        bin_file_bytes=bin_resolved.stat().st_size if bin_resolved else None,
        sections=sections,
        flash_bytes=flash_total,
        sram_bytes=sram_total,
        size_summary=size_summary,
        components=components,
        apps=apps,
        largest_symbols=largest_non_app_symbols(symbols, top),
        largest_archives=scan_archives(build_dir or elf.parent, top),
        map_archive_refs=parse_map_archive_refs(map_path, top),
        defconfig=str(defconfig) if defconfig and defconfig.is_file() else None,
        defconfig_flags=read_defconfig_flags(defconfig) if defconfig else [],
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze NuttX firmware size (nuttx / nuttx.bin).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--root", type=Path, default=None, help="openvela root")
    parser.add_argument("--board", default=DEFAULT_BOARD, help="board name")
    parser.add_argument(
        "--config",
        default=DEFAULT_CONFIG,
        help="config name (default: nsh)",
    )
    parser.add_argument("--out", type=Path, default=None, help="CMake output dir")
    parser.add_argument("--elf", type=Path, default=None, help="nuttx ELF path")
    parser.add_argument("--bin", type=Path, default=None, help="nuttx.bin path")
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="Top N for rankings and --all symbol/archive listings (default: 20)",
    )
    parser.add_argument(
        "--brief",
        action="store_true",
        help="Concise summary (post-build default; apps with 0 B omitted)",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Full report: sections, top symbols, archives, defconfig",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="Disable ANSI colors",
    )
    parser.add_argument("--json", type=Path, default=None, help="Write JSON report")
    args = parser.parse_args()

    use_color = not args.no_color and sys.stdout.isatty() and os.environ.get("NO_COLOR") is None
    c = Color(use_color)

    try:
        root = (args.root or find_openvela_root(SCRIPT)).resolve()
        out_dir = (args.out or cmake_out_dir(root, args.board, args.config)).resolve()
        elf = (args.elf or out_dir / "nuttx").resolve()
        bin_path = args.bin or (elf.parent / "nuttx.bin")
        defconfig = defconfig_path(root, args.board, args.config)
        config_label = f"{args.board}/{args.config}"

        report = build_report(
            elf=elf,
            bin_path=bin_path,
            defconfig=defconfig,
            build_dir=elf.parent,
            config_name=config_label,
            top=args.top,
        )

        if args.brief:
            print_brief(report, c=c, top=args.top)
        else:
            print_summary(report, c=c, top=args.top)
            if args.all:
                print_verbose(report, c=c, top=args.top)

        if args.json:
            args.json.write_text(
                json.dumps(asdict(report), indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            print(c.dim(f"Wrote JSON: {args.json}"))

    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
