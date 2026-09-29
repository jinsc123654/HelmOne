#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Scan a nationwide OSM PBF and write the simplified name charset.

This is the font-subset foundation: every printable character that appears in
OSM name tags, after traditional→simplified, with convertible 繁体 dropped.

  .venv/bin/python extract_osm_chars.py
  .venv/bin/python extract_osm_chars.py --pbf /path/china-latest.osm.pbf

Default source: $VMAP_CHINA_PBF or /home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf
Default output: data/china_osm_chars.txt (next to this script)
"""

from __future__ import annotations

import argparse
import os
import sys

import zh_simplify

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PBF = os.environ.get(
    "VMAP_CHINA_PBF", "/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf"
)
DEFAULT_OUT = os.path.join(HERE, "data", "china_osm_chars.txt")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Extract simplified OSM name charset for font subsetting."
    )
    ap.add_argument("--pbf", default=DEFAULT_PBF, help="OSM XML or PBF")
    ap.add_argument("--out", default=DEFAULT_OUT, help="charset text file")
    ap.add_argument(
        "--all-scripts",
        action="store_true",
        help="keep every script (Arabic/Thai/emoji/…). Default: CJK+Latin+punct",
    )
    args = ap.parse_args()
    if not os.path.isfile(args.pbf):
        print("[error] OSM not found: %s" % args.pbf, file=sys.stderr)
        return 1
    scan = zh_simplify.scan_osm_file(args.pbf, device_font=not args.all_scripts)
    zh_simplify.write_chars_file(args.out, scan)
    return 0


if __name__ == "__main__":
    sys.exit(main())
