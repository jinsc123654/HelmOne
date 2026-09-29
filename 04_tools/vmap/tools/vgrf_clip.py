#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Clip a monolithic VGRF to a geographic bbox (with margin) for regional packs."""

from __future__ import annotations

import math
import os
import struct
import sys
from pathlib import Path

def _find_osm_dir() -> Path:
    """Directory holding build_vgraph.py, imported for VGRF parsing.

    Standalone layout: the vmap root, one level above tools/. Legacy in-tree
    layout: <fw>/docs/osm. Override with $VMAP_OSM_DIR.
    """
    env = os.environ.get("VMAP_OSM_DIR")
    if env:
        return Path(env).expanduser().resolve()

    here = Path(__file__).resolve()
    for base in (here.parent, *here.parents):
        if (base / "build_vgraph.py").is_file():
            return base
        candidate = base / "docs" / "osm"
        if (candidate / "build_vgraph.py").is_file():
            return candidate
    raise RuntimeError(
        "cannot find build_vgraph.py from vgrf_clip.py; set $VMAP_OSM_DIR")


_OSMDIR = _find_osm_dir()
if str(_OSMDIR) not in sys.path:
    sys.path.insert(0, str(_OSMDIR))

import build_vgraph as vg  # noqa: E402

VGRF_MAGIC = b"VGRF"


def e7_to_deg(lon_e7: int, lat_e7: int) -> tuple[float, float]:
    return lon_e7 / 1e7, lat_e7 / 1e7


def expand_bbox(west: float, south: float, east: float, north: float,
                margin_m: float) -> tuple[float, float, float, float]:
    lat_ref = (south + north) * 0.5
    dlat = margin_m / 111320.0
    dlon = margin_m / (111320.0 * max(0.2, math.cos(math.radians(lat_ref))))
    return west - dlon, south - dlat, east + dlon, north + dlat


def _decode_name(pool: bytes, off: int, none: int) -> str:
    if off == none or off >= len(pool):
        return ""
    end = pool.find(b"\x00", off)
    if end < 0:
        end = len(pool)
    return pool[off:end].decode("utf-8", "replace")


def intern_names(names: list[str], limit: int | None = None) -> tuple[bytes, list[int]]:
    pool = bytearray()
    seen: dict[str, int] = {}
    offs: list[int] = []
    for s in names:
        if not s:
            offs.append(vg.VGRF_NAME_NONE)
            continue
        if s in seen:
            offs.append(seen[s])
            continue
        raw = s.encode("utf-8") + b"\x00"
        if limit is not None and len(pool) + len(raw) > limit:
            raise ValueError(
                "string pool %d + %d exceeds %d bytes (128KiB device cap)"
                % (len(pool), len(raw), limit))
        seen[s] = len(pool)
        pool += raw
        offs.append(seen[s])
    return bytes(pool), offs


def parse_vgrf(data: bytes) -> dict:
    if data[:4] != VGRF_MAGIC:
        raise ValueError("not VGRF")
    ver = data[4]
    if ver < 1 or ver > vg.VGRF_VERSION:
        raise ValueError("unsupported VGRF v%d" % ver)
    n = struct.unpack_from("<I", data, 8)[0]
    ecount = struct.unpack_from("<I", data, 12)[0]
    west, south, east, north = struct.unpack_from("<dddd", data, 16)
    hdr = 52 if ver >= 2 else 48
    snap_off = struct.unpack_from("<I", data, 48)[0] if ver >= 2 else 0
    edge_sz = vg.VGRF_EDGE_SIZE_V3 if ver >= 3 else vg.VGRF_EDGE_SIZE_V2
    none = vg.VGRF_NAME_NONE if ver >= 3 else vg.VGRF_NAME_NONE_V2
    off = hdr
    nodes: list[tuple[int, int]] = []
    for _ in range(n):
        nodes.append(struct.unpack_from("<ii", data, off))
        off += 8
    edges = []
    for _ in range(ecount):
        f, t, length_cm = struct.unpack_from("<III", data, off)
        cls = data[off + 12]
        flags = data[off + 13]
        if ver >= 3:
            name_off = struct.unpack_from("<I", data, off + 14)[0]
        else:
            name_off = struct.unpack_from("<H", data, off + 14)[0]
            if name_off == vg.VGRF_NAME_NONE_V2:
                name_off = vg.VGRF_NAME_NONE
        edges.append({
            "from": f, "to": t, "length_cm": length_cm,
            "cls": cls, "flags": flags, "name_off": name_off,
        })
        off += edge_sz
    adj_off: list[int] = []
    adj_edges: list[int] = []
    if ver < 4:
        adj_off = list(struct.unpack_from(f"<{n + 1}I", data, off))
        off += (n + 1) * 4
        adj_n = adj_off[n] if n else 0
        adj_edges = list(struct.unpack_from(f"<{adj_n}I", data, off))
        off += adj_n * 4
    str_end = snap_off if snap_off > hdr else len(data)
    str_pool = data[off:str_end]
    ele_m = None
    if snap_off > hdr:
        elev_off = vg.snap_section_end(data, snap_off)
        ele_m = vg.parse_elev_section(data, elev_off, n)
    return {
        "version": ver,
        "nodes": nodes, "edges": edges, "adj_off": adj_off,
        "adj_edges": adj_edges, "str_pool": str_pool,
        "west": west, "south": south, "east": east, "north": north,
        "ele_m": ele_m, "name_none": none,
    }


def clip_parsed_vgrf(g: dict, west: float, south: float, east: float, north: float,
                     margin_m: float = 400.0) -> bytes | None:
    w, s, e, n = expand_bbox(west, south, east, north, margin_m)

    src_ele = g.get("ele_m")
    if src_ele is None or len(src_ele) != len(g["nodes"]):
        src_ele = None
    ele_keep: list[int] | None = [] if src_ele is not None else None

    id_map: dict[int, int] = {}
    keep: list[tuple[int, int]] = []
    for i, (lon_e7, lat_e7) in enumerate(g["nodes"]):
        lon, lat = e7_to_deg(lon_e7, lat_e7)
        if w <= lon <= e and s <= lat <= n:
            id_map[i] = len(keep)
            keep.append((lon_e7, lat_e7))
            if ele_keep is not None:
                ele_keep.append(int(src_ele[i]))

    if not keep:
        return None

    new_edges = []
    for edge in g["edges"]:
        if edge["from"] in id_map and edge["to"] in id_map:
            new_edges.append({
                **edge,
                "from": id_map[edge["from"]],
                "to": id_map[edge["to"]],
            })

    if not new_edges:
        return None

    names = [_decode_name(g["str_pool"], e["name_off"], vg.VGRF_NAME_NONE)
             for e in new_edges]
    try:
        pool, offs = intern_names(names, limit=vg.VGRF_STR_POOL_MAX)
    except ValueError as exc:
        raise SystemExit("pack_map: region string pool overflow: %s" % exc) from exc
    for edge, off in zip(new_edges, offs):
        edge["name_off"] = off

    return vg.serialize_vgrf(
        keep, new_edges, pool,
        west, south, east, north,
        ele_m=ele_keep,
        already_undirected=g.get("version", 0) >= 4,
    )


def clip_vgrf(data: bytes, west: float, south: float, east: float, north: float,
              margin_m: float = 400.0) -> bytes | None:
    return clip_parsed_vgrf(parse_vgrf(data), west, south, east, north, margin_m)
