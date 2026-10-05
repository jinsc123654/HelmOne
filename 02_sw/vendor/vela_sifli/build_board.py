#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
My Vendor (SF32LB52) 板级构建脚本 — 与 vendor/sifli/build_board.py 同级入口。

用法（openvela 根目录）:
  python3 vendor/my_vendor/build_board.py build
  python3 vendor/my_vendor/build_board.py build-boot
  python3 vendor/my_vendor/build_board.py flash monitor

文档: docs/tools/vela_my_vendor_tools.md
实现: docs/tools/vela_my_vendor_tools.py（本文件为薄封装）。
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

_VENDOR_ROOT = Path(__file__).resolve().parent
_IMPL = _VENDOR_ROOT.parent.parent / "vela_my_vendor_tools.py"


def _load():
    spec = importlib.util.spec_from_file_location("vela_my_vendor_tools", _IMPL)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载 {_IMPL}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    return _load().main()


if __name__ == "__main__":
    sys.exit(main())
