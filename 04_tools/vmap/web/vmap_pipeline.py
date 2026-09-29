#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
vmap_pipeline.py — turn a WGS84 bbox into a device-ready vmap bundle.

Stages (each streamed to a log callback):

  1. osmium extract   china-latest.osm.pbf  --bbox W,S,E,N  ->  extract.osm.pbf
  2. build_vmap.py    extract.osm.pbf  --bbox ...  --zoom Z  ->  vmap/<z>/<x>/<y>.vt
  3. build_vgraph.py  extract.osm.pbf              ->  graph.vgrf         (optional)
  4. pack_map.py      vmap/ + graph.vgrf  --grid-global  ->  map/ (lon*/lat*/x*_y*.vpk)

The packed map/ directory is what gets uploaded to the device (via MTP) at
/mnt/lfs/map. The fixed national grid (see pack_map --grid-global) keeps region
cells (and therefore x<ix>/y<iy>.vpk paths) deterministic, so partial builds made
at different times line up. Firmware reverse-geocodes lon/lat → (ix,iy) and opens
that file; there is no map.idx.

Design constraints honoured:
  * one region file per 3 km grid cell
  * directory fan-out is lon<ix//4>/lat<iy//4>/ (≤16 vpk per folder)
  * fixed grid rule (rule 3) via pack_map (origin 0,0, 3 km @ 35°N).
"""

from __future__ import annotations

import math
import os
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths — resolved relative to this file so the tool is location independent.
# ---------------------------------------------------------------------------
WEB_DIR = Path(__file__).resolve().parent
OSM_DIR = WEB_DIR.parent                        # 04_tools/vmap
sys.path.insert(0, str(OSM_DIR))
import vmap_paths  # noqa: E402

VENV_PY = OSM_DIR / ".venv" / "bin" / "python"
BUILD_VMAP = OSM_DIR / "build_vmap.py"
BUILD_VGRAPH = OSM_DIR / "build_vgraph.py"
PACK_MAP = vmap_paths.tools_dir() / "pack_map.py"

# Source planet extract (China). Override with $VMAP_CHINA_PBF.
DEFAULT_CHINA_PBF = Path(
    os.environ.get("VMAP_CHINA_PBF", "/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf")
)

OUT_ROOT = WEB_DIR / "out"
# Shared, persistent tile cache keyed by the globally-fixed slippy z/x/y.
# Overlapping selections reuse tiles here instead of re-slicing every time.
CACHE_TILES = WEB_DIR / "cache" / "tiles"
CACHE_SIG_FILE = CACHE_TILES / "_source.sig"

# Keep the same fixed-grid constants the packer uses, so the UI can preview the
# exact cell boundaries before generating.
GRID_ANCHOR_LAT = 35.0
GRID_ORIGIN_LON = 0.0
GRID_ORIGIN_LAT = 0.0
# 15 km cells: a typical city is a handful of r*.vpk instead of dozens of
# tiny shards. Raise further for sparse countryside; lower for dense cores.
DEFAULT_REGION_KM = 3.0
GRID_BUCKET = 4
# Keep in sync with vmap_format.h (3 km @ 35°N).
GRID_CELL_LAT_3KM = 0.026949335249730506
GRID_CELL_LON_3KM = 0.032899063849730509

# Soft guard so an accidental "select the whole country" doesn't churn for hours.
MAX_BBOX_DEG = float(os.environ.get("VMAP_MAX_BBOX_DEG", "3.0"))


def grid_cell_deg(region_km: float) -> tuple[float, float]:
    if abs(region_km - 3.0) < 1e-9:
        return GRID_CELL_LON_3KM, GRID_CELL_LAT_3KM
    cell_lat = region_km / 111.32
    cell_lon = region_km / (111.32 * max(0.2, math.cos(math.radians(GRID_ANCHOR_LAT))))
    return cell_lon, cell_lat


def _bv():
    """Import the tile builder lazily to reuse its slippy-map math."""
    import sys
    if str(OSM_DIR) not in sys.path:
        sys.path.insert(0, str(OSM_DIR))
    import build_vmap as bv
    return bv


def _pbf_sig(pbf: Path) -> str:
    st = pbf.stat()
    return f"{pbf}|{st.st_size}|{int(st.st_mtime)}"


def _required_tiles(z: int, west, south, east, north) -> list[tuple[int, int]]:
    """(x,y) tiles a bbox covers at zoom z — same range build_vmap would tile."""
    bv = _bv()
    tx_min, ty_max = bv.lonlat_to_tile(west, south, z)
    tx_max, ty_min = bv.lonlat_to_tile(east, north, z)
    if tx_min > tx_max:
        tx_min, tx_max = tx_max, tx_min
    if ty_min > ty_max:
        ty_min, ty_max = ty_max, ty_min
    return [(x, y) for x in range(tx_min, tx_max + 1)
            for y in range(ty_min, ty_max + 1)]


def _tiles_geo_bbox(z: int, tiles: list[tuple[int, int]]) -> tuple:
    """Lon/lat bbox covering a set of z/x/y tiles."""
    bv = _bv()
    w = s = 180.0
    e = n = -180.0
    for (x, y) in tiles:
        tw, ts, te, tn = bv.tile_bbox_lonlat(x, y, z)
        w, s, e, n = min(w, tw), min(s, ts), max(e, te), max(n, tn)
    return w, s, e, n


def cells_for_bbox(west: float, south: float, east: float, north: float,
                   region_km: float) -> list[dict]:
    """List fixed-grid region cells that a bbox overlaps (for UI preview)."""
    cell_lon, cell_lat = grid_cell_deg(region_km)
    ix0 = math.floor((west - GRID_ORIGIN_LON) / cell_lon)
    ix1 = math.floor((east - GRID_ORIGIN_LON) / cell_lon)
    iy0 = math.floor((south - GRID_ORIGIN_LAT) / cell_lat)
    iy1 = math.floor((north - GRID_ORIGIN_LAT) / cell_lat)
    cells = []
    for iy in range(iy0, iy1 + 1):
        for ix in range(ix0, ix1 + 1):
            w = GRID_ORIGIN_LON + ix * cell_lon
            s = GRID_ORIGIN_LAT + iy * cell_lat
            cells.append({
                "ix": ix, "iy": iy,
                "west": w, "south": s,
                "east": w + cell_lon, "north": s + cell_lat,
                "vpk": f"lon{ix // GRID_BUCKET}/lat{iy // GRID_BUCKET}/x{ix}_y{iy}.vpk",
            })
    return cells


@dataclass
class Job:
    job_id: str
    params: dict
    status: str = "pending"          # pending|running|done|error
    log: list = field(default_factory=list)
    result: dict = field(default_factory=dict)
    progress: int = 0                # 0..100
    stage: str = ""                  # human label of the current step
    started: float = 0.0
    finished: float = 0.0

    def emit(self, line: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        self.log.append(f"[{stamp}] {line}")

    def set_progress(self, pct: int, stage: str) -> None:
        self.progress = max(self.progress, min(100, int(pct)))
        self.stage = stage


def _run(job: Job, argv: list[str], cwd: Path | None = None) -> int:
    """Run a subprocess, streaming stdout+stderr into the job log."""
    job.emit("$ " + " ".join(str(a) for a in argv))
    proc = subprocess.Popen(
        [str(a) for a in argv],
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        job.emit(line.rstrip("\n"))
    proc.wait()
    return proc.returncode


def _dir_stats(root: Path, pattern: str) -> tuple[int, int, int]:
    """(file count, total bytes, max file bytes) for root/**/pattern."""
    files = list(root.rglob(pattern))
    total = sum(f.stat().st_size for f in files)
    biggest = max((f.stat().st_size for f in files), default=0)
    return len(files), total, biggest


def run_pipeline(job: Job) -> None:
    p = job.params
    job.status = "running"
    job.started = time.time()
    try:
        _do_run(job)
        job.status = "done"
        job.set_progress(100, "完成")
        job.emit("✅ 完成")
    except Exception as exc:  # noqa: BLE001 - surface any failure to the UI
        job.status = "error"
        job.stage = "失败"
        job.result["error"] = str(exc)
        job.emit(f"❌ 失败: {exc}")
    finally:
        job.finished = time.time()


def _do_run(job: Job) -> None:
    p = job.params
    west = float(p["west"]); south = float(p["south"])
    east = float(p["east"]); north = float(p["north"])
    if west > east:
        west, east = east, west
    if south > north:
        south, north = north, south

    if east - west <= 0 or north - south <= 0:
        raise ValueError("bbox 面积为 0，请重新框选")
    if (east - west) > MAX_BBOX_DEG or (north - south) > MAX_BBOX_DEG:
        raise ValueError(
            f"框选范围过大（> {MAX_BBOX_DEG}°），请缩小或提高 VMAP_MAX_BBOX_DEG")

    zooms = p.get("zooms") or [14]
    region_km = float(p.get("region_km") or DEFAULT_REGION_KM)
    build_graph = bool(p.get("build_graph", True))
    china_pbf = Path(p.get("china_pbf") or DEFAULT_CHINA_PBF)

    if not china_pbf.is_file():
        raise FileNotFoundError(f"找不到源数据: {china_pbf}")
    if not VENV_PY.exists():
        raise FileNotFoundError(f"缺少虚拟环境 python: {VENV_PY}")

    name = p["name"]
    out_dir = OUT_ROOT / name
    if out_dir.exists():
        shutil.rmtree(out_dir)
    osm_dir = out_dir / "osm"
    vmap_dir = out_dir / "vmap"
    map_dir = out_dir / "map"
    osm_dir.mkdir(parents=True, exist_ok=True)

    extract_pbf = osm_dir / "extract.osm.pbf"
    graph_vgrf = out_dir / "graph.vgrf"

    job.emit(f"区域 {name}: W={west:.5f} S={south:.5f} E={east:.5f} N={north:.5f}")
    job.emit(f"zoom={zooms} region_km={region_km} graph={build_graph}")
    job.set_progress(3, "准备")

    # --- tile cache bookkeeping ----------------------------------------
    # Slippy z/x/y is globally fixed, so a tile's content depends only on the
    # source PBF. Cache built tiles under cache/tiles/<z>/<x>/<y>.vt and only
    # (re)build the ones missing for this bbox. Invalidate on PBF change.
    CACHE_TILES.mkdir(parents=True, exist_ok=True)
    sig = _pbf_sig(china_pbf)
    if CACHE_SIG_FILE.exists() and CACHE_SIG_FILE.read_text() != sig:
        job.emit("源数据已变化 → 清空瓦片缓存")
        for child in CACHE_TILES.iterdir():
            if child.is_dir():
                shutil.rmtree(child)
            elif child.name != CACHE_SIG_FILE.name:
                child.unlink()
    CACHE_SIG_FILE.write_text(sig)

    required = {z: _required_tiles(z, west, south, east, north) for z in zooms}
    missing: dict[int, list] = {}
    n_req = n_cached = 0
    for z, tl in required.items():
        miss = []
        for (x, y) in tl:
            n_req += 1
            if (CACHE_TILES / str(z) / str(x) / f"{y}.vt").exists():
                n_cached += 1
            else:
                miss.append((x, y))
        if miss:
            missing[z] = miss
    n_missing = n_req - n_cached
    job.emit(f"瓦片缓存: 需要 {n_req}，命中 {n_cached}，需新建 {n_missing}")

    # 1. osmium extract (only when something must be built) --------------
    osmium = shutil.which("osmium") or "osmium"
    if missing or build_graph:
        job.set_progress(6, "裁剪 OSM (osmium extract)")
        job.emit("── 步骤 1/4: osmium extract")
        if build_graph:
            ex = (west, south, east, north)          # graph needs full bbox
        else:
            # union bbox of just the missing tiles (across zooms)
            ws = [_tiles_geo_bbox(z, missing[z]) for z in missing]
            ex = (min(b[0] for b in ws), min(b[1] for b in ws),
                  max(b[2] for b in ws), max(b[3] for b in ws))
        pad = 0.01
        exb = (ex[0] - pad, ex[1] - pad, ex[2] + pad, ex[3] + pad)
        rc = _run(job, [
            osmium, "extract",
            "--bbox", f"{exb[0]},{exb[1]},{exb[2]},{exb[3]}",
            "--set-bounds", "--overwrite",
            "-s", "complete_ways",
            "-o", extract_pbf,
            china_pbf,
        ])
        if rc != 0:
            raise RuntimeError(f"osmium extract 失败 (rc={rc})")
        ex_mb = extract_pbf.stat().st_size / 1e6
        job.emit(f"extract.osm.pbf = {ex_mb:.1f} MB")
    else:
        ex_mb = 0.0
        job.emit("── 步骤 1/4: 完全命中缓存，跳过 OSM 裁剪")
    job.set_progress(35, "OSM 裁剪完成")

    # 2. build_vmap for missing tiles -> cache --------------------------
    job.emit("── 步骤 2/4: build_vmap（只切缺失瓦片，写入缓存）")
    vmap_lo, vmap_hi = 35, 70
    miss_zooms = list(missing.keys())
    for i, z in enumerate(miss_zooms):
        job.set_progress(vmap_lo + (vmap_hi - vmap_lo) * i // max(1, len(miss_zooms)),
                         f"切片 z{z} ({i + 1}/{len(miss_zooms)})")
        bw, bs, be, bn = _tiles_geo_bbox(z, missing[z])
        rc = _run(job, [
            VENV_PY, BUILD_VMAP,
            "--osm", extract_pbf,
            "--bbox", f"{bw},{bs},{be},{bn}",
            "--zoom", str(z),
            "--zh", "hans",
            "--out", CACHE_TILES,
            "--loose",
        ], cwd=OSM_DIR)
        if rc != 0:
            raise RuntimeError(f"build_vmap z{z} 失败 (rc={rc})")
    if not miss_zooms:
        job.emit("无缺失瓦片，直接从缓存拼装")
    job.set_progress(vmap_hi, "瓦片切片完成")

    # assemble this job's vmap/ from cache (only the required tiles).
    # Empty (sea / no-data) tiles won't exist in cache; that's expected.
    copied = 0
    for z, tl in required.items():
        for (x, y) in tl:
            src = CACHE_TILES / str(z) / str(x) / f"{y}.vt"
            if src.exists():
                dst = vmap_dir / str(z) / str(x) / f"{y}.vt"
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
                copied += 1
    n_vt, vt_bytes, _ = _dir_stats(vmap_dir, "*.vt")
    if n_vt == 0:
        raise RuntimeError("没有生成任何瓦片：该区域可能无 OSM 数据")
    job.emit(f"loose 瓦片: {n_vt} 个 .vt（缓存命中 {n_cached}，"
             f"新建 {n_missing}），合计 {vt_bytes / 1e6:.2f} MB")

    # 3. build_vgraph (optional) ----------------------------------------
    if build_graph:
        job.set_progress(72, "建路网导航图 (build_vgraph)")
        job.emit("── 步骤 3/4: build_vgraph（路网导航图 VGRF）")
        graph_argv = [
            VENV_PY, BUILD_VGRAPH,
            "--osm", extract_pbf,
            "--out", graph_vgrf,
            "--zh", "hans",
        ]
        rc = _run(job, graph_argv, cwd=OSM_DIR)
        if rc != 0:
            job.emit(f"⚠️ build_vgraph 失败 (rc={rc})，继续打包但不含导航图")
            build_graph = False
    else:
        job.emit("── 步骤 3/4: 跳过路网导航图")
    job.set_progress(88, "导航图完成")

    # 4. pack_map (fixed global grid) -----------------------------------
    job.set_progress(90, "打包 (pack_map)")
    job.emit("── 步骤 4/4: pack_map（3km 全国网格 → lon*/lat*/x*_y*.vpk）")
    argv = [
        VENV_PY, PACK_MAP,
        "-i", vmap_dir,
        "-o", map_dir,
        "--region-km", str(region_km),
        "--grid-global",
        # 0 = keep every zoom we actually built (no silent drop of z15);
        # the vmap/ tree already contains exactly the selected zooms.
        "--max-zoom", "0",
        # lonN/latN folders, at most 16 x*_y*.vpk each.
        "--subdir",
        "--clean",
    ]
    if build_graph and graph_vgrf.is_file():
        argv += ["--graph", graph_vgrf]
    rc = _run(job, argv, cwd=OSM_DIR)
    if rc != 0:
        raise RuntimeError(f"pack_map 失败 (rc={rc})")
    job.set_progress(98, "汇总结果")

    # 5. summary ---------------------------------------------------------
    n_vpk, vpk_bytes, vpk_max = _dir_stats(map_dir, "*.vpk")
    idx = map_dir / "map.idx"
    idx_bytes = idx.stat().st_size if idx.is_file() else 0
    top_files = [f for f in map_dir.iterdir() if f.is_file()]
    subdirs = [d for d in map_dir.iterdir() if d.is_dir()]
    max_files_per_dir = max(
        [len([f for f in d.iterdir() if f.is_file()]) for d in subdirs]
        + [len(top_files)], default=0)

    job.result.update({
        "name": name,
        "bbox": [west, south, east, north],
        "zooms": zooms,
        "region_km": region_km,
        "graph": build_graph,
        "out_dir": str(out_dir),
        "map_dir": str(map_dir),
        "extract_mb": round(ex_mb, 2),
        "vt_count": n_vt,
        "vt_mb": round(vt_bytes / 1e6, 2),
        "tiles_required": n_req,
        "tiles_cached": n_cached,
        "tiles_built": n_missing,
        "vpk_count": n_vpk,
        "vpk_total_kb": round(vpk_bytes / 1024, 1),
        "vpk_max_kb": round(vpk_max / 1024, 1),
        "idx_bytes": idx_bytes,
        "subdir_count": len(subdirs),
        "max_files_per_dir": max_files_per_dir,
        "top_files": sorted(f.name for f in top_files),
    })

    job.emit(f"打包完成: {n_vpk} 个 x*_y*.vpk 分布在 {len(subdirs)} 个 lon 目录，"
             f"顶层 {len(top_files)} 个文件（无 map.idx）")
    job.emit(f"单文件夹最多 {max_files_per_dir} 个文件")
    job.emit(f"最大分片 {vpk_max / 1024:.1f} KB，合计 {vpk_bytes / 1024:.1f} KB")
    if vpk_max > 2 * 1024 * 1024:
        job.emit("⚠️ 有分片 > 2MB，可调小 region_km 或只保留 z14")
    job.emit(f"上传目录（MTP → /mnt/lfs/map）: {map_dir}")
