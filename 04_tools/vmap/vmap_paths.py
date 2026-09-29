#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Locate the vmap packer tools and the HelmOne firmware tree.

This toolchain used to live inside the firmware repo (``docs/osm``) and found
everything by walking up a fixed number of directories to
``<openvela>/vendor/my_vendor``.  It now ships standalone as
``HelmOne/04_tools/vmap``, so the firmware side is resolved by an explicit
search instead of a fixed relative depth.

Resolution order, for both helpers:

1. the env override (``$VMAP_TOOLS_DIR`` / ``$HELMONE_FW``)
2. the standalone layout — ``./tools`` beside this file, firmware tree found
   above it via ``02_sw/vendor/vela_sifli`` (this repo) or ``vendor/my_vendor``
   (the openvela SDK checkout)
3. the legacy in-firmware layout, so an in-tree copy keeps working unchanged
"""

from __future__ import annotations

import os
from pathlib import Path

HERE = Path(__file__).resolve().parent

_TOOLS_MARKER = "pack_map.py"
_FW_MARKER = Path("boards") / "sf32lb52" / "my_vendor"

#: Firmware checkouts to probe below each ancestor of this file.
_FW_CANDIDATES = (
    "02_sw/vendor/vela_sifli",
    "vendor/vela_sifli",
    "vendor/my_vendor",
)


def tools_dir() -> Path:
    """Directory holding pack_vmap.py / pack_map.py / pack_vgrf_clip.py."""
    env = os.environ.get("VMAP_TOOLS_DIR")
    if env:
        return Path(env).expanduser().resolve()

    for cand in (HERE / "tools", HERE):
        if (cand / _TOOLS_MARKER).is_file():
            return cand

    try:
        return fw_root() / "boards" / "sf32lb52" / "my_vendor" / "ui" / "bicycle" / "tools"
    except RuntimeError:
        pass
    raise RuntimeError(
        "cannot locate the vmap packer tools (pack_map.py); set $VMAP_TOOLS_DIR")


def fw_root() -> Path:
    """Root of the firmware tree — the one holding boards/sf32lb52/my_vendor."""
    env = os.environ.get("HELMONE_FW")
    if env:
        return Path(env).expanduser().resolve()

    for base in (HERE, *HERE.parents):
        if (base / _FW_MARKER).is_dir():
            return base
        for rel in _FW_CANDIDATES:
            cand = base / rel
            if (cand / _FW_MARKER).is_dir():
                return cand

    raise RuntimeError(
        "cannot locate the firmware tree; set $HELMONE_FW to the checkout that "
        "contains boards/sf32lb52/my_vendor")


if __name__ == "__main__":
    print("tools_dir = %s" % tools_dir())
    try:
        print("fw_root   = %s" % fw_root())
    except RuntimeError as exc:
        print("fw_root   = <%s>" % exc)
