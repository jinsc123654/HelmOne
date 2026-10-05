#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate cmake_out/.../flasher_args.json from ptab + sftool_param."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve()
MYV = SCRIPT.parent.parent
OPENVELA = MYV.parent.parent

sys.path.insert(0, str(SCRIPT.parent))
import flash_args_lib as fal  # noqa: E402

# 相对**本树根**（MYV = <vendor>）—— 这棵树在 vendor/ 下叫什么都行
BOARD_CONFIG = "boards/sf32lb52/my_vendor/configs/nsh"
BOOT_CONFIG = "boot_loader/config/nsh"
BOOT_BIN_DIR = "boot_loader/bin"


def cmake_out_dir(root: Path) -> Path:
    cfg = Path(BOARD_CONFIG.strip("/"))
    return root / "cmake_out" / f"{MYV.name}_{cfg.name}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate flasher_args.json")
    parser.add_argument(
        "--root",
        type=Path,
        default=OPENVELA,
        help="openvela root (default: auto from script location)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="CMake output dir (default: cmake_out/<本树目录名>_nsh)",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    out = (args.out or cmake_out_dir(root)).resolve()
    out.mkdir(parents=True, exist_ok=True)

    dest = fal.write_flasher_args(
        root=root,
        out=out,
        boot_config=BOOT_CONFIG,
        boot_bin_dir_spec=BOOT_BIN_DIR,
    )
    print(f"Wrote {dest}")
    for line in json_preview(dest):
        print(f"  {line}")
    return 0


def json_preview(path: Path) -> list[str]:
    import json

    data = json.loads(path.read_text(encoding="utf-8"))
    lines = [f"chip={data['chip']} memory={data['memory']}"]
    for spec in data.get("write_flash", []):
        lines.append(spec)
    return lines


if __name__ == "__main__":
    raise SystemExit(main())
