#!/usr/bin/env python3
"""DEM sampler for VGRF node elevations.

Sources (first hit wins per point):

  1. Optional SRTM / Skadi ``.hgt`` / ``.hgt.gz`` in ``--dem-dir``
  2. Mapzen / AWS Terrarium PNG (z12, ~30 m) cached under ``docs/osm/cache/dem``,
     fetched only for tiles that cover graph nodes

Unknown samples are ``ELE_UNKNOWN`` (-32768). Sea-level 0 is a valid height.
"""

from __future__ import annotations

import gzip
import math
import os
import re
import struct
import sys
import threading
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from pathlib import Path


@contextmanager
def _exclusive(path: Path):
    """Serialize Terrarium cache writes across parallel 1° workers."""
    import fcntl

    lock = path.with_name(path.name + ".lock")
    lock.parent.mkdir(parents=True, exist_ok=True)
    fp = open(lock, "a+b")
    try:
        fcntl.flock(fp.fileno(), fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(fp.fileno(), fcntl.LOCK_UN)
        finally:
            fp.close()

ELE_UNKNOWN = -32768
HGT_BUF_MAX = 8
TERRARIUM_Z = 12
TERRARIUM_URL = (
    "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
)

_HGT_NAME = re.compile(r"^([NS])(\d+)([EW])(\d+)", re.IGNORECASE)


def dem_jobs(requested: int = 0) -> int:
    """HTTP + PNG decode is I/O bound; use more workers than CPU count."""
    if requested > 0:
        return max(1, int(requested))
    env = os.environ.get("VMAP_DEM_JOBS")
    if env:
        return max(1, int(env))
    cpu = os.cpu_count() or 4
    return max(8, min(32, cpu * 2))


def clamp_ele(v: float) -> int:
    if not math.isfinite(v):
        return ELE_UNKNOWN
    iv = int(round(v))
    if iv <= ELE_UNKNOWN:
        return ELE_UNKNOWN + 1
    if iv > 32767:
        return 32767
    return iv


def _hgt_sw(name: str) -> tuple[int, int] | None:
    m = _HGT_NAME.match(Path(name).stem)
    if not m:
        return None
    ns, lat_s, ew, lon_s = m.groups()
    lat = int(lat_s)
    lon = int(lon_s)
    if ns.upper() == "S":
        lat = -lat
    if ew.upper() == "W":
        lon = -lon
    return lat, lon


def _is_hgt_path(p: Path) -> bool:
    n = p.name.lower()
    return n.endswith(".hgt") or n.endswith(".hgt.gz")


def _bilinear(q11: float, q21: float, q12: float, q22: float,
              tx: float, ty: float) -> float | None:
    vals = (q11, q21, q12, q22)
    if any(v <= ELE_UNKNOWN for v in vals):
        known = [v for v in vals if v > ELE_UNKNOWN]
        if not known:
            return None
        return sum(known) / len(known)
    a = q11 * (1.0 - tx) + q21 * tx
    b = q12 * (1.0 - tx) + q22 * tx
    return a * (1.0 - ty) + b * ty


class DemSampler:
    def __init__(self, dem_dir: str | os.PathLike | None = None,
                 cache_dir: str | os.PathLike | None = None,
                 fetch: bool = True):
        self.fetch = fetch
        self._lock = threading.Lock()
        self.cache_dir = Path(cache_dir) if cache_dir else None
        self._hgt: dict[tuple[int, int], Path] = {}
        self._hgt_buf: OrderedDict[tuple[int, int], tuple[int, bytes]] = OrderedDict()
        self._png: dict[tuple[int, int, int], object] = {}
        self._pillow_ok = True
        if dem_dir:
            self._index_hgt(Path(dem_dir))

    def _index_hgt(self, root: Path) -> None:
        if not root.is_dir():
            print("[dem] --dem-dir missing: %s" % root, file=sys.stderr)
            return
        n = 0
        for p in root.rglob("*"):
            if not _is_hgt_path(p):
                continue
            key = _hgt_sw(p.name)
            if key is None:
                continue
            prev = self._hgt.get(key)
            # Prefer uncompressed .hgt over .hgt.gz.
            if prev is not None and prev.name.lower().endswith(".hgt") \
                    and p.name.lower().endswith(".gz"):
                continue
            self._hgt[key] = p
            n += 1
        print("[dem] indexed %d .hgt under %s" % (n, root), file=sys.stderr)

    def _load_hgt(self, lat0: int, lon0: int):
        key = (lat0, lon0)
        with self._lock:
            cached = self._hgt_buf.get(key)
            if cached is not None:
                self._hgt_buf.move_to_end(key)
                return cached
        path = self._hgt.get(key)
        if path is None:
            return None
        try:
            raw = path.read_bytes()
            if path.name.lower().endswith(".gz"):
                raw = gzip.decompress(raw)
        except Exception as exc:
            print("[dem] read fail %s: %s" % (path, exc), file=sys.stderr)
            return None
        n = int(math.sqrt(len(raw) / 2.0))
        if n not in (1201, 3601) or n * n * 2 != len(raw):
            print("[dem] skip bad hgt %s size=%d" % (path, len(raw)),
                  file=sys.stderr)
            return None
        rec = (n, raw)
        with self._lock:
            self._hgt_buf[key] = rec
            while len(self._hgt_buf) > HGT_BUF_MAX:
                self._hgt_buf.popitem(last=False)
        return rec

    def _sample_hgt(self, lon: float, lat: float) -> int | None:
        lat0 = math.floor(lat)
        lon0 = math.floor(lon)
        rec = self._load_hgt(int(lat0), int(lon0))
        if rec is None:
            return None
        n, data = rec
        # row 0 = north edge of the 1° cell
        row_f = (1.0 - (lat - lat0)) * (n - 1)
        col_f = (lon - lon0) * (n - 1)
        row_f = min(max(row_f, 0.0), float(n - 1))
        col_f = min(max(col_f, 0.0), float(n - 1))
        r0 = int(math.floor(row_f))
        c0 = int(math.floor(col_f))
        r1 = min(r0 + 1, n - 1)
        c1 = min(c0 + 1, n - 1)
        ty = row_f - r0
        tx = col_f - c0

        def at(r, c):
            off = (r * n + c) * 2
            return struct.unpack_from(">h", data, off)[0]

        v = _bilinear(float(at(r0, c0)), float(at(r0, c1)),
                      float(at(r1, c0)), float(at(r1, c1)), tx, ty)
        if v is None:
            return None
        return clamp_ele(v)

    def _png_path(self, z: int, x: int, y: int) -> Path | None:
        if self.cache_dir is None:
            return None
        return self.cache_dir / str(z) / str(x) / ("%d.png" % y)

    def _load_png(self, z: int, x: int, y: int):
        key = (z, x, y)
        with self._lock:
            img = self._png.get(key)
        if img is not None:
            return img if img is not False else None
        if not self._pillow_ok:
            return None
        try:
            from PIL import Image
        except ImportError:
            print("[dem] pillow missing; cannot decode terrarium PNG "
                  "(pip install pillow)", file=sys.stderr)
            self._pillow_ok = False
            return None

        path = self._png_path(z, x, y)
        if path is None:
            return self._fetch_png(z, x, y, key, None)

        with _exclusive(path):
            if path.is_file():
                try:
                    im = Image.open(path).convert("RGB")
                    rec = (im.size[0], im.size[1], im.tobytes())
                    with self._lock:
                        self._png[key] = rec
                    return rec
                except Exception as trans:
                    print("[dem] bad cache %s: %s" % (path, trans), file=sys.stderr)
            return self._fetch_png(z, x, y, key, path)

    def _fetch_png(self, z: int, x: int, y: int, key, path: Path | None):
        if not self.fetch:
            with self._lock:
                self._png[key] = False
            return None

        url = TERRARIUM_URL.format(z=z, x=x, y=y)
        try:
            import requests
        except ImportError:
            print("[dem] requests missing; cannot fetch %s" % url,
                  file=sys.stderr)
            with self._lock:
                self._png[key] = False
            return None
        try:
            r = requests.get(url, timeout=30,
                             headers={"User-Agent": "openvela-vmap/1.0"})
            r.raise_for_status()
            blob = r.content
        except Exception as exc:
            print("[dem] fetch fail %s: %s" % (url, exc), file=sys.stderr)
            with self._lock:
                self._png[key] = False
            return None

        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_name(path.name + ".tmp")
            tmp.write_bytes(blob)
            tmp.replace(path)
        try:
            from io import BytesIO
            from PIL import Image
            im = Image.open(BytesIO(blob)).convert("RGB")
        except Exception as trans:
            print("[dem] decode fail %s: %s" % (url, trans), file=sys.stderr)
            with self._lock:
                self._png[key] = False
            return None
        rec = (im.size[0], im.size[1], im.tobytes())
        with self._lock:
            self._png[key] = rec
        return rec

    def _sample_terrarium(self, lon: float, lat: float) -> int | None:
        import build_vmap as bv

        z = TERRARIUM_Z
        px, py = bv.lonlat_to_pixel(lon, lat, z)
        tx, ty = int(px // 256), int(py // 256)
        fx = px - tx * 256.0
        fy = py - ty * 256.0
        rec = self._load_png(z, tx, ty)
        if rec is None:
            return None
        w, h, blob = rec
        if w < 2 or h < 2:
            return None

        def hgt_at(ix: int, iy: int) -> float:
            ix = min(max(ix, 0), w - 1)
            iy = min(max(iy, 0), h - 1)
            off = (iy * w + ix) * 3
            r, g, b = blob[off], blob[off + 1], blob[off + 2]
            return r * 256.0 + g + b / 256.0 - 32768.0

        x0 = int(math.floor(fx))
        y0 = int(math.floor(fy))
        v = _bilinear(hgt_at(x0, y0), hgt_at(x0 + 1, y0),
                      hgt_at(x0, y0 + 1), hgt_at(x0 + 1, y0 + 1),
                      fx - x0, fy - y0)
        if v is None:
            return None
        return clamp_ele(v)

    def sample(self, lon: float, lat: float) -> int:
        if not math.isfinite(lon) or not math.isfinite(lat):
            return ELE_UNKNOWN
        if lat < -90.0 or lat > 90.0 or lon < -180.0 or lon > 180.0:
            return ELE_UNKNOWN
        v = self._sample_hgt(lon, lat)
        if v is not None:
            return v
        v = self._sample_terrarium(lon, lat)
        if v is not None:
            return v
        return ELE_UNKNOWN

    def sample_nodes(self, nodes, jobs: int = 0) -> list[int]:
        """Sample elevation only at graph nodes (lon_e7, lat_e7).

        Prefetches Terrarium tiles that cover those points (parallel HTTP),
        then samples nodes in parallel. Not a nationwide raster.
        """
        import build_vmap as bv

        jobs = dem_jobs(jobs)
        pts = []
        tiles = set()
        for lon_e7, lat_e7 in nodes:
            lon, lat = lon_e7 / 1e7, lat_e7 / 1e7
            pts.append((lon, lat))
            if self._hgt:
                lat0, lon0 = int(math.floor(lat)), int(math.floor(lon))
                if (lat0, lon0) in self._hgt:
                    continue
            tx, ty = bv.lonlat_to_tile(lon, lat, TERRARIUM_Z)
            tiles.add((TERRARIUM_Z, tx, ty))
        want = sorted(tiles)
        print("[dem] prefetch %d terrarium tiles  jobs=%d"
              % (len(want), jobs), file=sys.stderr, flush=True)
        if want:
            if jobs <= 1 or len(want) <= 1:
                for i, (z, x, y) in enumerate(want, 1):
                    self._load_png(z, x, y)
                    if i == 1 or i == len(want) or i % 10 == 0:
                        print("[dem] tile %d/%d z%d/%d/%d"
                              % (i, len(want), z, x, y), file=sys.stderr)
            else:
                done = 0
                with ThreadPoolExecutor(max_workers=min(jobs, len(want))) as ex:
                    futs = [ex.submit(self._load_png, z, x, y)
                            for z, x, y in want]
                    for fut in as_completed(futs):
                        fut.result()
                        done += 1
                        if done == 1 or done == len(want) or done % 20 == 0:
                            print("[dem] tile %d/%d" % (done, len(want)),
                                  file=sys.stderr, flush=True)

        n = len(pts)
        out = [ELE_UNKNOWN] * n
        known = 0
        if n == 0:
            return out

        def _chunk(lo: int, hi: int):
            local = 0
            buf = []
            for lon, lat in pts[lo:hi]:
                e = self.sample(lon, lat)
                buf.append(e)
                if e != ELE_UNKNOWN:
                    local += 1
            return lo, buf, local

        if jobs <= 1 or n < 4096:
            _, buf, known = _chunk(0, n)
            out = buf
            print("[dem] sample %d/%d nodes" % (n, n), file=sys.stderr)
        else:
            width = max(2048, (n + jobs - 1) // jobs)
            ranges = [(i, min(i + width, n)) for i in range(0, n, width)]
            print("[dem] sample %d nodes in %d chunks jobs=%d"
                  % (n, len(ranges), jobs), file=sys.stderr, flush=True)
            with ThreadPoolExecutor(max_workers=min(jobs, len(ranges))) as ex:
                futs = [ex.submit(_chunk, lo, hi) for lo, hi in ranges]
                finished = 0
                for fut in as_completed(futs):
                    lo, buf, local = fut.result()
                    out[lo:lo + len(buf)] = buf
                    known += local
                    finished += 1
                    print("[dem] sample chunk %d/%d" % (finished, len(ranges)),
                          file=sys.stderr, flush=True)
        print("[dem] sampled %d nodes, %d with elevation"
              % (len(out), known), file=sys.stderr, flush=True)
        return out
