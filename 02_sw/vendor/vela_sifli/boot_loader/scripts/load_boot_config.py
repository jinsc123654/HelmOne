#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Load boot.json from a board config profile directory."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def load_boot_json(config_dir: Path) -> dict:
    path = config_dir / "boot.json"
    if not path.is_file():
        raise SystemExit(f"ERROR: missing {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <config_dir> <key>", file=sys.stderr)
        return 2
    cfg = load_boot_json(Path(sys.argv[1]))
    key = sys.argv[2]
    if key not in cfg:
        raise SystemExit(f"ERROR: boot.json missing key: {key}")
    val = cfg[key]
    if isinstance(val, (dict, list)):
        print(json.dumps(val))
    else:
        print(val)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
