#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Offline vmap packaging entry point (alias for pack_vmap.py).

Example:
  python3 tools/build_vmap.py -i /path/to/vmap_loose -o <dst> --clean --pack-size 512000

<dst> is a device map dir, e.g.
  <fw>/boards/sf32lb52/my_vendor/mkfs/fat/map
"""

if __name__ == "__main__":
    from pathlib import Path
    import runpy

    runpy.run_path(str(Path(__file__).with_name("pack_vmap.py")), run_name="__main__")
