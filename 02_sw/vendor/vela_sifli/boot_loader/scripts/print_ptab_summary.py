#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Print ptab.*.json boot-chain addresses for build.sh / gen_ftab / gen_ptab_table."""

from __future__ import annotations

import sys
from pathlib import Path

# openvela/vendor/my_vendor/scripts
_SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(_SCRIPTS))
import flash_args_lib as fal  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print(
            f"Usage: {Path(sys.argv[0]).name} <storage> <ptab.json> [stage]",
            file=sys.stderr,
        )
        return 1
    storage = sys.argv[1]
    ptab = Path(sys.argv[2])
    stage = sys.argv[3] if len(sys.argv) > 3 else "build"
    if not ptab.is_file():
        print(f"ERROR: ptab not found: {ptab}", file=sys.stderr)
        return 1
    fal.print_ptab_build_summary(ptab, storage, stage=stage)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
