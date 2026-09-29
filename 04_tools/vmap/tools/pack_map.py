#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
pack_map.py — Regional map bundle: one self-contained vpk per grid cell.

Device layout (<dst>/, e.g. mkfs/fat/map/):

  lon<ix//4>/lat<iy//4>/x<ix>_y<iy>.vpk   # 3 km cell; folder holds at most 16 files
                                          # trailing VPOR after VGRF (no route.port)

ix = floor(lon / cell_lon), iy = floor(lat / cell_lat)
with the baked 3 km national grid (origin 0,0, anchor 35°N).

Legacy size-sharded VIDX layout: use --legacy-shards.

Usage:
  pack_map.py -i vmap --graph graph.vgrf -o <dst> --clean

<dst> is a device map dir, e.g.
  <fw>/boards/sf32lb52/my_vendor/mkfs/fat/map
"""

from __future__ import annotations

import argparse
import math
import multiprocessing
import os
import struct
import sys
from collections import OrderedDict, defaultdict
from dataclasses import dataclass
from pathlib import Path

_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from pack_vmap import DEFAULT_PACK_BYTES, VMAP_SHARD_EXT, TileBlob, collect_tiles
from vgrf_clip import clip_parsed_vgrf, parse_vgrf
from portal_match import (
    encode_vpk_portals,
    match_portals,
    read_route_port,
    split_portals_by_cell,
)

VREG_MAGIC = b"VREG"
RIDX_MAGIC = b"RIDX"
VIDX_MAGIC = b"VIDX"
VPKG_MAGIC = b"VPKG"

VREG_VERSION = 1
RIDX_VERSION = 2
RIDX_TILE_REC_SIZE = 16
RIDX_TILE_REC_SIZE_V1 = 14
MAX_TILE_BYTES = 128 * 1024
VMAP_PACK_VERSION = 1
VMAP_MAP_INDEX_FILE = "map.idx"
VMAP_ROUTE_PORT_FILE = "route.port"
VMAP_FLAG_HAS_GRAPH = 0x01

REGION_VPK_PREFIX = "r"
DEFAULT_REGION_KM = 3.0
# Overlap band kept in each region's routing graph beyond its cell bbox.
# The device plans a trip within a single region when the destination lies
# within VMAP_ROUTE_REGION_REACH_M (300m) of the FROM region, so this margin
# must stay comfortably larger than that band to guarantee the roads it snaps
# to are backed by connected geometry (no dangling border stubs).
GRAPH_CLIP_MARGIN_M = 600.0

# MTP-friendly subfolder layout: <dst>/d<bucket>/r<id>.vpk, bucket = id // N.
# N is fixed to match vmap_format.h VMAP_REG_FILES_PER_DIR (device side).
REGION_SUBDIR_PREFIX = "d"
REGION_FILES_PER_DIR = 20

# Fixed national grid: with a constant anchor latitude and (0,0) origin the
# region cell (ix,iy) and its bbox are a deterministic function of geography,
# so partial builds done at different times stitch into one nationwide index.
GRID_GLOBAL_ANCHOR_LAT = 35.0
GRID_GLOBAL_ORIGIN_LON = 0.0
GRID_GLOBAL_ORIGIN_LAT = 0.0
# Keep in sync with vmap_format.h VMAP_GRID_CELL_* (3 km @ 35°N).
GRID_CELL_LAT_3KM = 0.026949335249730506
GRID_CELL_LON_3KM = 0.032899063849730509
GRID_BUCKET = 4

VREG_HDR_SIZE = 40
VREG_REC_SIZE = 48
RIDX_HDR_SIZE = 52

_CLIP_G = None
_CLIP_MARGIN = GRAPH_CLIP_MARGIN_M


def _cpu_jobs(n_work: int, cap: int = 12) -> int:
    env = os.environ.get("VMAP_PACK_JOBS")
    if env:
        try:
            return max(1, min(int(env), n_work, cap))
        except ValueError:
            pass
    cpu = os.cpu_count() or 1
    return max(1, min(cpu, n_work, cap))


def _clip_one(item):
    cell, west, south, east, north = item
    blob = clip_parsed_vgrf(_CLIP_G, west, south, east, north, _CLIP_MARGIN)
    return cell, blob


def _match_pair(item):
    path_a, path_b, rid_a, rid_b, bbox_a, bbox_b = item
    ga = graph_from_vpk(Path(path_a))
    gb = graph_from_vpk(Path(path_b))
    return match_portals(rid_a, ga, bbox_a, rid_b, gb, bbox_b)


@dataclass
class Blob:
    data: bytes
    sort_key: tuple


def tile_center_lonlat(z: int, x: int, y: int) -> tuple[float, float]:
    n = 2 ** z
    lon = (x + 0.5) / n * 360.0 - 180.0
    lat_rad = math.atan(math.sinh(math.pi * (1.0 - 2.0 * (y + 0.5) / n)))
    lat = math.degrees(lat_rad)
    return lon, lat


def grid_cell_deg(lat_ref: float, region_km: float) -> tuple[float, float]:
    if (abs(region_km - 3.0) < 1e-9
            and abs(lat_ref - GRID_GLOBAL_ANCHOR_LAT) < 1e-9):
        return GRID_CELL_LON_3KM, GRID_CELL_LAT_3KM
    cell_lat = region_km / 111.32
    cell_lon = region_km / (111.32 * max(0.2, math.cos(math.radians(lat_ref))))
    return cell_lon, cell_lat


def cell_id(ix: int, iy: int) -> int:
    if ix < 0 or iy < 0 or ix > 0xFFFF or iy > 0xFFFF:
        raise SystemExit(f"grid ix/iy out of uint16: {ix},{iy}")
    return (iy << 16) | ix


def vpk_relpath_xy(ix: int, iy: int) -> str:
    bx = ix // GRID_BUCKET
    by = iy // GRID_BUCKET
    return f"lon{bx}/lat{by}/x{ix}_y{iy}.{VMAP_SHARD_EXT}"


def grid_origin(centers: list[tuple[float, float]], cell_lon: float,
                cell_lat: float) -> tuple[float, float]:
    min_lon = min(c[0] for c in centers)
    min_lat = min(c[1] for c in centers)
    ix0 = math.floor(min_lon / cell_lon) if cell_lon > 0 else 0
    iy0 = math.floor(min_lat / cell_lat) if cell_lat > 0 else 0
    return ix0 * cell_lon, iy0 * cell_lat


def cell_for_point(lon: float, lat: float, origin_lon: float, origin_lat: float,
                   cell_lon: float, cell_lat: float) -> tuple[int, int]:
    ix = int(math.floor((lon - origin_lon) / cell_lon))
    iy = int(math.floor((lat - origin_lat) / cell_lat))
    return ix, iy


def cell_bbox(ix: int, iy: int, origin_lon: float, origin_lat: float,
              cell_lon: float, cell_lat: float) -> tuple[float, float, float, float]:
    west = origin_lon + ix * cell_lon
    south = origin_lat + iy * cell_lat
    return west, south, west + cell_lon, south + cell_lat


def neighbor_mask(ix: int, iy: int, cells: set[tuple[int, int]]) -> int:
    mask = 0
    if (ix - 1, iy) in cells:
        mask |= 0x01  # W
    if (ix + 1, iy) in cells:
        mask |= 0x02  # E
    if (ix, iy - 1) in cells:
        mask |= 0x04  # S
    if (ix, iy + 1) in cells:
        mask |= 0x08  # N
    return mask


def write_vpk(path: Path, region_id: int, payload: bytes) -> None:
    header = struct.pack(
        "<4sBBHI I",
        VPKG_MAGIC,
        VMAP_PACK_VERSION,
        0,
        region_id & 0xFFFF,
        len(payload),
        0,
    )
    path.write_bytes(header + payload)


def build_region_payload(region_id: int, tiles: list[TileBlob], graph: bytes | None,
                         bbox: tuple[float, float, float, float],
                         portal_blob: bytes | None = None) -> bytes:
    west, south, east, north = bbox
    flags = VMAP_FLAG_HAS_GRAPH if graph else 0
    tiles = sorted(tiles, key=lambda t: (t.z, t.y, t.x))

    hdr = bytearray()
    hdr.extend(RIDX_MAGIC)
    hdr.extend(struct.pack("<BBH", RIDX_VERSION, flags, region_id & 0xFFFF))
    hdr.extend(struct.pack("<III", len(tiles), 0, len(graph) if graph else 0))
    hdr.extend(struct.pack("<dddd", west, south, east, north))

    tile_area_off = RIDX_HDR_SIZE + len(tiles) * RIDX_TILE_REC_SIZE
    graph_off = tile_area_off
    payload = bytearray()
    payload.extend(hdr)
    for _ in tiles:
        payload.extend(b"\x00" * RIDX_TILE_REC_SIZE)

    cur = tile_area_off
    for i, tile in enumerate(tiles):
        size = len(tile.data)
        if size > MAX_TILE_BYTES:
            raise ValueError(
                "tile z=%d x=%d y=%d is %d B (max %d)"
                % (tile.z, tile.x, tile.y, size, MAX_TILE_BYTES))
        struct.pack_into(
            "<BBHHHII", payload, RIDX_HDR_SIZE + i * RIDX_TILE_REC_SIZE,
            tile.z & 0xFF, 0,
            tile.x & 0xFFFF, tile.y & 0xFFFF,
            0, cur, size,
        )
        payload.extend(tile.data)
        cur += size

    if graph:
        graph_off = len(payload)
        payload.extend(graph)
        struct.pack_into("<I", payload, 12, graph_off)
        struct.pack_into("<I", payload, 16, len(graph))
        if portal_blob:
            payload.extend(portal_blob)

    return bytes(payload)


def write_vreg_index(dst: Path, origin_lon: float, origin_lat: float,
                     cell_lon: float, cell_lat: float,
                     regions: list[dict]) -> None:
    buf = bytearray()
    buf.extend(VREG_MAGIC)
    buf.extend(struct.pack("<BBH", VREG_VERSION, 0, len(regions)))
    buf.extend(struct.pack("<dddd", origin_lon, origin_lat, cell_lon, cell_lat))
    for r in regions:
        w, s, e, n = r["bbox"]
        buf.extend(struct.pack(
            "<HHHHddddII",
            r["id"] & 0xFFFF,
            r["ix"] & 0xFFFF,
            r["iy"] & 0xFFFF,
            r["neighbors"] & 0xFFFF,
            w, s, e, n,
            0, 0,
        ))
    (dst / VMAP_MAP_INDEX_FILE).write_bytes(buf)


def vpk_relpath(rid: int, subdir: bool) -> str:
    name = f"{REGION_VPK_PREFIX}{rid:03d}.{VMAP_SHARD_EXT}"
    if subdir:
        return f"{REGION_SUBDIR_PREFIX}{rid // REGION_FILES_PER_DIR:03d}/{name}"
    return name


def scan_grid_vpks(dst: Path, cell_lon: float, cell_lat: float) -> dict[tuple[int, int], dict]:
    kept: dict[tuple[int, int], dict] = {}
    if not dst.is_dir():
        return kept
    for p in dst.glob("lon*/lat*/x*_y*.vpk"):
        stem = p.stem
        if not stem.startswith("x") or "_y" not in stem:
            continue
        xs, ys = stem.split("_y", 1)
        try:
            ix = int(xs[1:])
            iy = int(ys)
        except ValueError:
            continue
        bbox = cell_bbox(ix, iy, GRID_GLOBAL_ORIGIN_LON, GRID_GLOBAL_ORIGIN_LAT,
                         cell_lon, cell_lat)
        kept[(ix, iy)] = {
            "id": cell_id(ix, iy),
            "ix": ix,
            "iy": iy,
            "neighbors": 0,
            "bbox": bbox,
            "tiles": 0,
            "graph": 0,
            "vpk": str(p.relative_to(dst)),
            "bytes": p.stat().st_size,
        }
    return kept


def read_vreg(dst: Path) -> dict | None:
    path = dst / VMAP_MAP_INDEX_FILE
    if not path.is_file():
        return None
    data = path.read_bytes()
    if data[:4] != VREG_MAGIC or len(data) < VREG_HDR_SIZE:
        return None
    _ver, _flags, n = struct.unpack_from("<BBH", data, 4)
    origin_lon, origin_lat, cell_lon, cell_lat = struct.unpack_from("<dddd", data, 8)
    regions = []
    off = VREG_HDR_SIZE
    for _ in range(n):
        rid, ix, iy, neigh, w, s, e, nlat, _a, _b = struct.unpack_from(
            "<HHHHddddII", data, off)
        off += VREG_REC_SIZE
        rel = vpk_relpath(rid, True)
        if not (dst / rel).is_file():
            rel = vpk_relpath(rid, False)
        regions.append({
            "id": rid, "ix": ix, "iy": iy, "neighbors": neigh,
            "bbox": (w, s, e, nlat), "tiles": 0, "graph": 0,
            "vpk": rel, "bytes": 0,
        })
    return {
        "origin_lon": origin_lon, "origin_lat": origin_lat,
        "cell_lon": cell_lon, "cell_lat": cell_lat,
        "regions": regions,
    }


def graph_from_vpk(path: Path) -> bytes | None:
    raw = path.read_bytes()
    if raw[:4] != VPKG_MAGIC or len(raw) < 16 + RIDX_HDR_SIZE:
        return None
    payload = raw[16:]
    graph_off, graph_sz = struct.unpack_from("<II", payload, 12)
    if graph_sz == 0 or graph_off + graph_sz > len(payload):
        return None
    return payload[graph_off:graph_off + graph_sz]


def vpk_payload_without_portals(raw: bytes) -> bytes | None:
    if raw[:4] != VPKG_MAGIC or len(raw) < 16 + RIDX_HDR_SIZE:
        return None
    payload = raw[16:]
    graph_off, graph_sz = struct.unpack_from("<II", payload, 12)
    if graph_sz == 0:
        return payload
    end = graph_off + graph_sz
    if end > len(payload):
        return None
    return payload[:end]


def patch_vpk_portals(path: Path, region_id: int,
                      nbrs: dict[int, list]) -> int:
    raw = path.read_bytes()
    payload = vpk_payload_without_portals(raw)
    if payload is None:
        return 0
    blob = encode_vpk_portals(nbrs) if nbrs else b""
    write_vpk(path, region_id, payload + blob)
    return len(blob)


def install_portals_into_vpks(dst: Path, regions: list[dict],
                              portals: list[dict]) -> tuple[int, int]:
    by_cell = split_portals_by_cell(portals)
    n_files = 0
    n_bytes = 0
    for r in regions:
        path = dst / r["vpk"]
        if not path.is_file():
            continue
        n_bytes += patch_vpk_portals(path, r["id"], by_cell.get(r["id"], {}))
        n_files += 1
    port_path = dst / VMAP_ROUTE_PORT_FILE
    if port_path.is_file():
        port_path.unlink()
    return n_files, n_bytes


def rebuild_neighbors(regions: list[dict]) -> None:
    cells = {(r["ix"], r["iy"]) for r in regions}
    for r in regions:
        r["neighbors"] = neighbor_mask(r["ix"], r["iy"], cells)


def rebuild_portals(dst: Path, regions: list[dict], cache_size: int = 512,
                    jobs: int = 0) -> int:
    _ = cache_size
    by_xy = {(r["ix"], r["iy"]): r for r in regions}
    seen: set[tuple[int, int]] = set()
    pairs: list[tuple] = []
    for r in regions:
        for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            nb = by_xy.get((r["ix"] + dx, r["iy"] + dy))
            if not nb:
                continue
            pair = (min(r["id"], nb["id"]), max(r["id"], nb["id"]))
            if pair in seen:
                continue
            seen.add(pair)
            pairs.append((
                str(dst / r["vpk"]), str(dst / nb["vpk"]),
                r["id"], nb["id"], r["bbox"], nb["bbox"],
            ))
    nj = jobs if jobs > 0 else _cpu_jobs(len(pairs) or 1)
    print("[pack_map] portal pairs %d jobs=%d" % (len(pairs), nj),
          file=sys.stderr, flush=True)
    portals: list[dict] = []
    if nj <= 1 or len(pairs) <= 1:
        for item in pairs:
            portals.extend(_match_pair(item))
    else:
        ctx = multiprocessing.get_context("fork")
        with ctx.Pool(nj) as pool:
            done = 0
            for recs in pool.imap_unordered(_match_pair, pairs, chunksize=8):
                portals.extend(recs)
                done += 1
                if done == 1 or done == len(pairs) or done % 200 == 0:
                    print("[pack_map] portal %d/%d" % (done, len(pairs)),
                          file=sys.stderr, flush=True)
    n_files, n_bytes = install_portals_into_vpks(dst, regions, portals)
    print(f"[pack_map] VPOR {len(portals)} portals -> {n_files} vpks "
          f"({n_bytes} B)", file=sys.stderr)
    return len(portals)


def parse_bbox(text: str) -> tuple[float, float, float, float]:
    parts = [float(x.strip()) for x in text.split(",")]
    if len(parts) != 4:
        raise SystemExit("bbox must be W,S,E,N")
    w, s, e, n = parts
    if w > e:
        w, e = e, w
    if s > n:
        s, n = n, s
    return w, s, e, n


def cell_center(ix: int, iy: int, origin_lon: float, origin_lat: float,
                cell_lon: float, cell_lat: float) -> tuple[float, float]:
    return (origin_lon + (ix + 0.5) * cell_lon,
            origin_lat + (iy + 0.5) * cell_lat)


def pack_regional(vmap_src: Path, graph_src: Path | None, dst: Path,
                  region_km: float, clean: bool, max_zoom: int,
                  global_grid: bool = False, subdir: bool = False,
                  merge: bool = False, write_portals: bool = True,
                  keep_bbox: tuple[float, float, float, float] | None = None,
                  skip_empty_graph: bool = False, pack_jobs: int = 0) -> None:
    if clean and not merge and dst.exists():
        import shutil
        shutil.rmtree(dst)
    dst.mkdir(parents=True, exist_ok=True)
    _ = (global_grid, subdir)

    tile_list = collect_tiles(vmap_src)
    if not tile_list:
        print(f"[pack_map] no tiles under {vmap_src}", file=sys.stderr)
        return
    if max_zoom > 0:
        before = len(tile_list)
        tile_list = [t for t in tile_list if t.z <= max_zoom]
        dropped = before - len(tile_list)
        if dropped:
            print(
                f"[pack_map] dropped {dropped} tiles with z>{max_zoom}",
                file=sys.stderr,
            )
        if not tile_list:
            print(f"[pack_map] no tiles with z<={max_zoom} under {vmap_src}",
                  file=sys.stderr)
            return

    graph_full: bytes | None = None
    if graph_src is not None:
        if not graph_src.is_file():
            raise SystemExit(f"graph not found: {graph_src}")
        graph_full = graph_src.read_bytes()
        if graph_full[:4] != b"VGRF":
            raise SystemExit(f"not VGRF: {graph_src}")

    centers = [tile_center_lonlat(t.z, t.x, t.y) for t in tile_list]
    # Firmware reverse-geocodes with a baked 3 km national grid.
    if abs(region_km - 3.0) > 1e-6:
        print("[pack_map] warning: --region-km %.1f but MCU grid is 3 km"
              % region_km, file=sys.stderr)
    cell_lon, cell_lat = grid_cell_deg(GRID_GLOBAL_ANCHOR_LAT, region_km)
    origin_lon, origin_lat = GRID_GLOBAL_ORIGIN_LON, GRID_GLOBAL_ORIGIN_LAT

    existing = {}
    if merge and write_portals:
        existing = scan_grid_vpks(dst, cell_lon, cell_lat)
    kept: dict[tuple[int, int], dict] = dict(existing)
    if merge and not existing:
        old_idx = dst / VMAP_MAP_INDEX_FILE
        if old_idx.is_file() and not any(dst.glob("lon*/lat*/x*_y*.vpk")):
            raise SystemExit(
                "pack_map --merge: found old map.idx but no lon*/lat*/x*_y*.vpk; "
                "this layout needs --clean (coordinate-addressed 3km grid)")

    tiles_by_cell: dict[tuple[int, int], list[TileBlob]] = defaultdict(list)
    for tile, (lon, lat) in zip(tile_list, centers):
        ix, iy = cell_for_point(lon, lat, origin_lon, origin_lat, cell_lon, cell_lat)
        tiles_by_cell[(ix, iy)].append(tile)

    cells = sorted(tiles_by_cell.keys())
    if keep_bbox:
        kw, ks, ke, kn = keep_bbox
        owned = []
        for cell in cells:
            cx, cy = cell_center(cell[0], cell[1], origin_lon, origin_lat,
                                 cell_lon, cell_lat)
            if kw <= cx < ke and ks <= cy < kn:
                owned.append(cell)
        print(f"[pack_map] keep-bbox dropped {len(cells) - len(owned)} "
              f"seam cells, kept {len(owned)}", file=sys.stderr)
        cells = owned
    if not cells:
        print("[pack_map] no region cells to write", file=sys.stderr)
        return

    graph_parsed = parse_vgrf(graph_full) if graph_full else None
    if graph_parsed is not None:
        print("[pack_map] graph parsed: %d nodes, %d edges"
              % (len(graph_parsed["nodes"]), len(graph_parsed["edges"])),
              file=sys.stderr)
    pending: list[tuple[tuple[int, int], bytes | None]] = []
    work = []
    for cell in cells:
        ix, iy = cell
        bbox = cell_bbox(ix, iy, origin_lon, origin_lat, cell_lon, cell_lat)
        work.append((cell, bbox[0], bbox[1], bbox[2], bbox[3]))
    nj = pack_jobs if pack_jobs > 0 else _cpu_jobs(len(work) or 1)

    def _keep(cell, region_graph):
        if skip_empty_graph and graph_parsed is not None and not region_graph:
            return
        pending.append((cell, region_graph))

    if graph_parsed is None:
        for cell, *_bbox in work:
            _keep(cell, None)
    elif nj <= 1 or len(work) <= 1:
        for i, (cell, west, south, east, north) in enumerate(work, 1):
            _keep(cell, clip_parsed_vgrf(
                graph_parsed, west, south, east, north, GRAPH_CLIP_MARGIN_M))
            if i % 50 == 0 or i == len(work):
                print("[pack_map] clip %d/%d → %d cells with graph"
                      % (i, len(work), len(pending)), file=sys.stderr)
    else:
        global _CLIP_G
        _CLIP_G = graph_parsed
        print("[pack_map] clip %d cells jobs=%d" % (len(work), nj),
              file=sys.stderr, flush=True)
        ctx = multiprocessing.get_context("fork")
        with ctx.Pool(nj) as pool:
            done = 0
            for cell, region_graph in pool.imap_unordered(
                    _clip_one, work, chunksize=2):
                _keep(cell, region_graph)
                done += 1
                if done == 1 or done == len(work) or done % 50 == 0:
                    print("[pack_map] clip %d/%d → %d cells with graph"
                          % (done, len(work), len(pending)),
                          file=sys.stderr, flush=True)
        _CLIP_G = None
    if not pending:
        print("[pack_map] no region cells to write (empty graph)", file=sys.stderr)
        return

    cell_to_id: dict[tuple[int, int], int] = {}
    for cell, _g in pending:
        cell_to_id[cell] = cell_id(cell[0], cell[1])

    region_graphs: dict[int, bytes | None] = {}
    batch_meta: list[dict] = []
    for cell, region_graph in pending:
        ix, iy = cell
        rid = cell_to_id[cell]
        bbox = cell_bbox(ix, iy, origin_lon, origin_lat, cell_lon, cell_lat)
        region_graphs[rid] = region_graph
        rec = {
            "id": rid,
            "ix": ix,
            "iy": iy,
            "neighbors": 0,
            "bbox": bbox,
            "tiles": len(tiles_by_cell[cell]),
            "graph": len(region_graph) if region_graph else 0,
            "vpk": vpk_relpath_xy(ix, iy),
            "bytes": 0,
        }
        kept[cell] = rec
        batch_meta.append(rec)

    all_portals: list[dict] = []
    by_cell: dict[int, dict[int, list]] = {}
    if write_portals and graph_full and not merge:
        seen_borders: set[tuple[int, int]] = set()
        for r in batch_meta:
            rid = r["id"]
            for _bit, (dx, dy) in ((0x01, (-1, 0)), (0x02, (1, 0)),
                                   (0x04, (0, -1)), (0x08, (0, 1))):
                nb = (r["ix"] + dx, r["iy"] + dy)
                if nb not in cell_to_id:
                    continue
                rid_b = cell_to_id[nb]
                pair = (min(rid, rid_b), max(rid, rid_b))
                if pair in seen_borders:
                    continue
                seen_borders.add(pair)
                bbox_b = next(x["bbox"] for x in batch_meta if x["id"] == rid_b)
                all_portals.extend(match_portals(
                    rid, region_graphs.get(rid), r["bbox"],
                    rid_b, region_graphs.get(rid_b), bbox_b))
        by_cell = split_portals_by_cell(all_portals)

    for rec in batch_meta:
        rid = rec["id"]
        ix, iy = rec["ix"], rec["iy"]
        pblob = encode_vpk_portals(by_cell[rid]) if rid in by_cell else b""
        payload = build_region_payload(
            rid, tiles_by_cell[(ix, iy)], region_graphs.get(rid), rec["bbox"],
            portal_blob=pblob)
        rel = rec["vpk"]
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        write_vpk(dst / rel, rid & 0xFFFF, payload)
        rec["bytes"] = len(payload)

    if write_portals and graph_full and not merge:
        print(f"[pack_map] VPOR {len(all_portals)} portals in {len(by_cell)} cells",
              file=sys.stderr)
        port_path = dst / VMAP_ROUTE_PORT_FILE
        if port_path.is_file():
            port_path.unlink()

    regions_meta = sorted(kept.values(), key=lambda r: (r["iy"], r["ix"]))
    rebuild_neighbors(regions_meta)
    idx_path = dst / VMAP_MAP_INDEX_FILE
    if idx_path.is_file():
        idx_path.unlink()

    if write_portals and merge:
        nport = rebuild_portals(dst, regions_meta, jobs=pack_jobs)
        print(f"[pack_map] merge rebuilt {nport} portals", file=sys.stderr)

    total_tiles = sum(r.get("tiles", 0) for r in batch_meta)
    total_graph = sum(r.get("graph", 0) for r in batch_meta)
    print(
        f"[pack_map] regional {region_km:.0f}km: {len(regions_meta)} regions "
        f"(+{len(batch_meta)} this batch), {total_tiles} tiles, "
        f"graph {total_graph} B",
        file=sys.stderr,
    )
    show = batch_meta if len(batch_meta) <= 24 else (
        batch_meta[:8] + batch_meta[-4:])
    for r in show:
        w, s, e, n = r["bbox"]
        print(
            f"  lon{r['ix'] // GRID_BUCKET}/lat{r['iy'] // GRID_BUCKET}/"
            f"x{r['ix']}_y{r['iy']}: {r['tiles']} tiles, graph {r['graph']} B, "
            f"bbox lon {w:.4f}..{e:.4f} lat {s:.4f}..{n:.4f}",
            file=sys.stderr,
        )
    if len(batch_meta) > len(show):
        print(f"  ... {len(batch_meta) - len(show)} more cells",
              file=sys.stderr)
    print("  (no map.idx — cell vpk path from lon/lat; portals in each vpk)",
          file=sys.stderr)


# --- legacy size-sharded VIDX (unchanged behaviour) ---

def write_vpk_legacy(path: Path, pack_id: int, payload: bytes) -> None:
    write_vpk(path, pack_id, payload)


def pack_shards(blobs: list[Blob], dst: Path, pack_bytes: int):
    shards: list[bytearray] = []

    def new_shard() -> tuple[int, bytearray]:
        shards.append(bytearray())
        return len(shards) - 1, shards[-1]

    pack_id, payload = new_shard()
    placements: list[tuple[int, int, int, int]] = []

    for i, blob in enumerate(blobs):
        data = blob.data
        pos = 0
        while pos < len(data):
            room = pack_bytes - len(payload)
            if room == 0:
                pack_id, payload = new_shard()
                room = pack_bytes
            take = min(len(data) - pos, room)
            if take == 0:
                pack_id, payload = new_shard()
                continue
            offset = len(payload)
            payload.extend(data[pos:pos + take])
            placements.append((i, pack_id, offset, take))
            pos += take

    for pid, payload in enumerate(shards):
        write_vpk_legacy(dst / f"p{pid:03d}.{VMAP_SHARD_EXT}", pid, payload)

    return placements, shards


def write_map_index_legacy(dst: Path, pack_bytes: int, tiles: list,
                           tile_placements: list, graph: bytes | None,
                           graph_placements: list) -> None:
    flags = VMAP_FLAG_HAS_GRAPH if graph else 0
    graph_total = len(graph) if graph else 0
    graph_shards = len(graph_placements)

    buf = bytearray()
    buf.extend(VIDX_MAGIC)
    buf.extend(struct.pack("<BBH", 1, flags, 0))
    buf.extend(struct.pack("<IIIH H", pack_bytes, len(tiles), graph_total, graph_shards, 0))

    for tile, (_, pack_id, offset, size) in zip(tiles, tile_placements):
        buf.extend(struct.pack("<BBHHHIH",
                               tile.z & 0xFF, 0,
                               tile.x & 0xFFFF, tile.y & 0xFFFF,
                               pack_id & 0xFFFF, offset & 0xFFFFFFFF, size & 0xFFFF))

    for pack_id, offset, size in graph_placements:
        buf.extend(struct.pack("<HII", pack_id & 0xFFFF, offset & 0xFFFFFFFF, size & 0xFFFFFFFF))

    (dst / VMAP_MAP_INDEX_FILE).write_bytes(buf)


def pack_legacy(vmap_src: Path, graph_src: Path | None, dst: Path,
                pack_bytes: int, clean: bool) -> None:
    if clean and dst.exists():
        import shutil
        shutil.rmtree(dst)
    dst.mkdir(parents=True, exist_ok=True)

    tile_list = collect_tiles(vmap_src)
    if not tile_list:
        raise SystemExit(f"no tiles under {vmap_src}")

    tile_list.sort(key=lambda t: (t.z, t.y, t.x))
    blobs: list[Blob] = []
    for t in tile_list:
        blobs.append(Blob(t.data, (0, t.z, t.y, t.x)))

    graph: bytes | None = None
    if graph_src is not None:
        graph = graph_src.read_bytes()
        if graph[:4] != b"VGRF":
            raise SystemExit(f"not VGRF: {graph_src}")
        blobs.append(Blob(graph, (1, 0, 0, 0)))

    placements, shards = pack_shards(blobs, dst, pack_bytes)
    n_tiles = len(tile_list)
    tile_placements = placements[:n_tiles]
    graph_placements = [(p, o, s) for (bi, p, o, s) in placements[n_tiles:]]

    write_map_index_legacy(dst, pack_bytes, tile_list, tile_placements, graph, graph_placements)
    print(f"[pack_map] legacy: {len(shards)} shards", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description="Pack tiles + graph for device map/")
    ap.add_argument("-i", "--input", type=Path,
                    help="loose/packed vmap tree (not needed with --rebuild-portals)")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--graph", type=Path)
    ap.add_argument("--region-km", type=float, default=DEFAULT_REGION_KM,
                    help=f"Grid cell size km (default {DEFAULT_REGION_KM}); 0=legacy shards")
    ap.add_argument("--max-zoom", type=int, default=14,
                    help="Regional pack: keep tiles with z<=N (default 14; 0=all zooms)")
    ap.add_argument("--grid-global", action="store_true",
                    help="Fixed national grid (constant anchor lat + 0,0 origin) "
                         "so region cells align across partial builds")
    ap.add_argument("--subdir", action="store_true",
                    help="(ignored) cells are lon*/lat*/x*_y*.vpk")
    ap.add_argument("--merge", action="store_true",
                    help="Union this batch into existing lon*/lat*/x*_y*.vpk "
                         "(same --region-km --grid-global); identity is (ix,iy)")
    ap.add_argument("--no-portals", action="store_true",
                    help="Skip per-cell VPOR (use --rebuild-portals after merges)")
    ap.add_argument("--keep-bbox", metavar="W,S,E,N",
                    help="Only emit cells whose center lies in this bbox "
                         "(so 1deg jobs do not overwrite seam neighbours)")
    ap.add_argument("--skip-empty-graph", action="store_true",
                    help="Drop cells with no clipped VGRF (ocean / no roads)")
    ap.add_argument("--rebuild-portals", action="store_true",
                    help="Rematch portals from cell graphs and write VPOR into each vpk")
    ap.add_argument("--embed-portals", action="store_true",
                    help="Split existing route.port into per-cell VPOR (no rematch)")
    ap.add_argument("--legacy-shards", action="store_true",
                    help="Old p*.vpk size-sharded VIDX layout")
    ap.add_argument("--pack-size", type=int, default=DEFAULT_PACK_BYTES,
                    help="Legacy shard payload limit")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel clip / portal workers (0 = auto from CPU)")
    args = ap.parse_args()

    if args.rebuild_portals or args.embed_portals:
        cell_lon, cell_lat = grid_cell_deg(GRID_GLOBAL_ANCHOR_LAT, DEFAULT_REGION_KM)
        regions = list(scan_grid_vpks(args.output, cell_lon, cell_lat).values())
        if not regions:
            raise SystemExit(f"no cell vpks under {args.output}")
        rebuild_neighbors(regions)
        if args.embed_portals and not args.rebuild_portals:
            port = args.output / VMAP_ROUTE_PORT_FILE
            if not port.is_file():
                raise SystemExit(f"no {VMAP_ROUTE_PORT_FILE} under {args.output}")
            portals = read_route_port(port)
            n_files, n_bytes = install_portals_into_vpks(
                args.output, regions, portals)
            print(f"[pack_map] embedded {len(portals)} portals -> {n_files} vpks "
                  f"({n_bytes} B)", file=sys.stderr)
            return
        n = rebuild_portals(args.output, regions, jobs=args.jobs)
        print(f"[pack_map] rebuilt {n} portals, {len(regions)} cells",
              file=sys.stderr)
        return

    if not args.input or not args.input.is_dir():
        raise SystemExit(f"input not found: {args.input}")

    if args.legacy_shards or args.region_km <= 0:
        pack_legacy(args.input, args.graph, args.output, args.pack_size, args.clean)
    else:
        pack_regional(args.input, args.graph, args.output, args.region_km, args.clean,
                      args.max_zoom, args.grid_global, args.subdir,
                      merge=args.merge, write_portals=not args.no_portals,
                      keep_bbox=parse_bbox(args.keep_bbox) if args.keep_bbox else None,
                      skip_empty_graph=args.skip_empty_graph,
                      pack_jobs=args.jobs)


if __name__ == "__main__":
    main()
