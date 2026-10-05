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
# 工具实现的两处候选（按序）：
#   1) 本树自带的 docs/tools/vela_my_vendor_tools.py —— **优先**。它是本树的真身
#      （实现里的 VENDOR_ROOT 就是按它的位置算的）⇒ 本树改名 / 多份并存时各编各的。
#   2) <openvela 根>/vela_my_vendor_tools.py —— SDK 里的旧入口（通常是指向本树的
#      软链接），仅当本树没有自带实现时兜底。
# ⚠ 顺序不能反：SDK 根那份一旦存在（指向开发树），先选它就会**去编开发树**。
_IMPL_CANDIDATES = (
    _VENDOR_ROOT / "docs" / "tools" / "vela_my_vendor_tools.py",
    _VENDOR_ROOT.parent.parent / "vela_my_vendor_tools.py",
)
_IMPL = next((p for p in _IMPL_CANDIDATES if p.is_file()), _IMPL_CANDIDATES[-1])


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
