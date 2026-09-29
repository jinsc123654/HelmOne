#!/usr/bin/env python3
"""Optional nationwide Skadi DEM download.

Packing does **not** need this: build_vgraph.py samples elevation only at
road-graph nodes via Terrarium tiles (a few MB per city).

Use this script only if you want a local 1° .hgt.gz archive.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

SKADI_URL = "https://s3.amazonaws.com/elevation-tiles-prod/skadi/{folder}/{name}.hgt.gz"
UA = "openvela-vmap/1.0"
DEFAULT_DEST = "/home/jinsc/SDK/vela/osm_data/dem"
# Inclusive SW corners: lat 18..53, lon 73..134 covers [18,54) x [73,135).
CHINA_WEST, CHINA_SOUTH, CHINA_EAST, CHINA_NORTH = 73, 18, 135, 54
MIN_BYTES = 2048


def skadi_id(lat: int, lon: int) -> tuple[str, str]:
    ns = "N" if lat >= 0 else "S"
    ew = "E" if lon >= 0 else "W"
    folder = "%s%02d" % (ns, abs(lat))
    name = "%s%s%03d" % (folder, ew, abs(lon))
    return folder, name


def china_cells(west=CHINA_WEST, south=CHINA_SOUTH,
                east=CHINA_EAST, north=CHINA_NORTH):
    for lat in range(int(south), int(north)):
        for lon in range(int(west), int(east)):
            yield lat, lon


def gzip_ok(path: Path) -> bool:
    if not path.is_file() or path.stat().st_size < MIN_BYTES:
        return False
    with path.open("rb") as fp:
        return fp.read(2) == b"\x1f\x8b"


def fetch_one(url: str, dest: Path, timeout: int, retries: int) -> str:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if gzip_ok(dest):
        return "skip"
    tmp = dest.with_suffix(dest.suffix + ".part")
    last_err = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = resp.read()
            if len(data) < MIN_BYTES or data[:2] != b"\x1f\x8b":
                raise RuntimeError("not gzip (%d bytes)" % len(data))
            tmp.write_bytes(data)
            tmp.replace(dest)
            return "ok"
        except urllib.error.HTTPError as exc:
            last_err = exc
            if exc.code == 404:
                return "missing"
            time.sleep(1.5 * (attempt + 1))
        except Exception as exc:
            last_err = exc
            time.sleep(1.5 * (attempt + 1))
    return "fail:%s" % last_err


def main():
    ap = argparse.ArgumentParser(description="Download China Skadi DEM.")
    ap.add_argument("--dest", default=os.environ.get("VMAP_CHINA_DEM", DEFAULT_DEST))
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--retries", type=int, default=4)
    args = ap.parse_args()

    dest_root = Path(args.dest)
    dest_root.mkdir(parents=True, exist_ok=True)
    (dest_root / "SOURCE.txt").write_text(
        "Mapzen/AWS Skadi SRTM1 (1 arc-sec, ~30 m)\n"
        "bbox 73E-135E, 18N-54N\n"
        "url https://s3.amazonaws.com/elevation-tiles-prod/skadi/{Nxx}/{NxxExxx}.hgt.gz\n"
        "keep gzipped; build_vgraph.py --dem-dir this folder\n",
        encoding="utf-8",
    )

    jobs = []
    for lat, lon in china_cells():
        folder, name = skadi_id(lat, lon)
        url = SKADI_URL.format(folder=folder, name=name)
        path = dest_root / folder / (name + ".hgt.gz")
        jobs.append((url, path))

    total = len(jobs)
    print("[dem-cn] dest=%s cells=%d workers=%d" % (dest_root, total, args.workers),
          file=sys.stderr)

    counts = {"ok": 0, "skip": 0, "missing": 0, "fail": 0}
    bytes_ok = 0
    t0 = time.time()
    done = 0

    def run(job):
        url, path = job
        status = fetch_one(url, path, args.timeout, args.retries)
        sz = path.stat().st_size if path.is_file() and status in ("ok", "skip") else 0
        return status, sz, path.name

    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futs = [pool.submit(run, j) for j in jobs]
        for fut in as_completed(futs):
            status, sz, name = fut.result()
            key = status.split(":", 1)[0]
            counts[key] = counts.get(key, 0) + 1
            if status in ("ok", "skip"):
                bytes_ok += sz
            done += 1
            if status.startswith("fail"):
                print("[dem-cn] %s %s" % (name, status), file=sys.stderr)
            if done % 20 == 0 or done == total:
                dt = max(0.001, time.time() - t0)
                print("[dem-cn] %d/%d ok=%d skip=%d miss=%d fail=%d %.1f GB  %.0fs"
                      % (done, total, counts["ok"], counts["skip"],
                         counts["missing"], counts["fail"],
                         bytes_ok / 1e9, dt),
                      file=sys.stderr, flush=True)

    dt = time.time() - t0
    print("[dem-cn] done ok=%d skip=%d missing=%d fail=%d  %.2f GB in %.0fs"
          % (counts["ok"], counts["skip"], counts["missing"], counts["fail"],
             bytes_ok / 1e9, dt),
          file=sys.stderr)
    if counts["fail"]:
        sys.exit(2)


if __name__ == "__main__":
    main()
