#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate ftab.c from configs/<profile>/ptab.json + nuttx.bin / bootloader.bin sizes.

SiFli SDK GenFtabCFile() records exact nuttx.bin file size in imgs[].length.
We patch those fields to ptab.json partition max_size so boot copy length matches
ftab[].size (see boot_img_copy_len() in board/main.c). nuttx.bin can then grow
within the partition without rebuilding ftab after every app rebuild.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path


def partition_img_max_sizes(ptab: Path) -> dict[str, int]:
    """Map ptab img name (main, bootloader, ...) -> max_size bytes."""
    mems = json.loads(ptab.read_text(encoding="utf-8"))
    sizes: dict[str, int] = {}
    for mem in mems:
        for region in mem.get("regions", []):
            img = region.get("img")
            if img:
                sizes[img] = int(region["max_size"], 0)
    return sizes


def patch_ftab_copy_lengths(ftab_c: Path, part_sizes: dict[str, int]) -> None:
    """Align imgs[].length with ptab partition caps (bootloader + main/HCPU)."""
    text = ftab_c.read_text(encoding="utf-8")
    main_sz = part_sizes.get("main")
    bl_sz = part_sizes.get("bootloader")

    if main_sz:
        text = re.sub(
            r"(\.imgs\[DFU_FLASH_IMG_IDX\(DFU_FLASH_IMG_HCPU\)\] = \{\.length = )0x[0-9A-Fa-f]+",
            rf"\g<1>0x{main_sz:08X}",
            text,
        )
        text = re.sub(
            r"(\.imgs\[DFU_FLASH_IMG_IDX\(DFU_FLASH_IMG_HCPU2\)\] = \{\.length = )0x[0-9A-Fa-f]+",
            rf"\g<1>0x{main_sz:08X}",
            text,
        )

    if bl_sz:
        text = re.sub(
            r"(\.imgs\[DFU_FLASH_IMG_IDX\(DFU_FLASH_IMG_BL\)\] = \{\.length = )0x[0-9A-Fa-f]+",
            rf"\g<1>0x{bl_sz:08X}",
            text,
        )

    ftab_c.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ptab", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path, help="Output ftab.c path")
    parser.add_argument("--main", required=True, type=Path, help="nuttx.bin")
    parser.add_argument("--bootloader", required=True, type=Path)
    args = parser.parse_args()

    sdk_root = os.environ.get("SIFLI_SDK") or os.environ.get("SIFLI_SDK_PATH")
    if not sdk_root:
        print("ERROR: SIFLI_SDK not set (source SDK export.sh first)", file=sys.stderr)
        return 1

    build_dir = Path(sdk_root) / "tools" / "build"
    sys.path.insert(0, str(build_dir))
    import resource  # noqa: E402

    for path in (args.ptab, args.main, args.bootloader):
        if not path.is_file():
            print(f"ERROR: missing file: {path}", file=sys.stderr)
            return 1

    _scripts = Path(__file__).resolve().parents[2] / "scripts"
    sys.path.insert(0, str(_scripts))
    import flash_args_lib as fal  # noqa: E402

    storage = os.environ.get("BOOT_STORAGE", "nand")
    fal.print_ptab_build_summary(args.ptab.resolve(), storage, stage="gen-ftab")

    part_sizes = partition_img_max_sizes(args.ptab)

    imgs_info = [
        {"name": "bootloader", "binary": [str(args.bootloader.resolve())]},
        {"name": "main", "binary": [str(args.main.resolve())]},
    ]

    args.out.parent.mkdir(parents=True, exist_ok=True)
    resource.GenFtabCFile(str(args.ptab.resolve()), str(args.out.resolve()), imgs_info)
    patch_ftab_copy_lengths(args.out, part_sizes)

    main_size = args.main.stat().st_size
    bl_size = args.bootloader.stat().st_size
    main_copy = part_sizes.get("main", main_size)
    bl_copy = part_sizes.get("bootloader", bl_size)

    print(f"Generated {args.out}")
    print(f"  nuttx.bin file size     = 0x{main_size:X} ({main_size} bytes)")
    print(f"  boot copy size (ptab)   = 0x{main_copy:X} ({main_copy} bytes)  [main]")
    print(f"  bootloader file size    = 0x{bl_size:X} ({bl_size} bytes)")
    print(f"  boot copy size (ptab)   = 0x{bl_copy:X} ({bl_copy} bytes)  [bootloader]")
    if main_size > main_copy:
        print(
            f"ERROR: nuttx.bin ({main_size} B) exceeds main partition "
            f"max_size ({main_copy} B) in {args.ptab}",
            file=sys.stderr,
        )
        return 1
    if bl_size > bl_copy:
        print(
            f"ERROR: bootloader ({bl_size} B) exceeds partition "
            f"max_size ({bl_copy} B) in {args.ptab}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
