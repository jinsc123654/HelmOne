#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Match cross-region routing portals between clipped regional VGRF blobs."""

from __future__ import annotations

import math
import struct
from typing import Any

from vgrf_clip import e7_to_deg, parse_vgrf

PORTAL_MATCH_M = 120.0
PORTAL_MAX_PER_BORDER = 64
# Keep in sync with pack_map.py GRAPH_CLIP_MARGIN_M
GRAPH_CLIP_MARGIN_M = 600.0


def expand_bbox(bbox: tuple[float, float, float, float],
                margin_m: float) -> tuple[float, float, float, float]:
    w, s, e, n = bbox
    lat_ref = (s + n) * 0.5
    dlat = margin_m / 111320.0
    dlon = margin_m / (111320.0 * max(0.2, math.cos(math.radians(lat_ref))))
    return w - dlon, s - dlat, e + dlon, n + dlat


def rect_intersection(a: tuple[float, float, float, float],
                      b: tuple[float, float, float, float]
                      ) -> tuple[float, float, float, float] | None:
    west = max(a[0], b[0])
    south = max(a[1], b[1])
    east = min(a[2], b[2])
    north = min(a[3], b[3])
    if west < east and south < north:
        return west, south, east, north
    return None


def haversine_m(lon1: float, lat1: float, lon2: float, lat2: float) -> float:
    r = 6371000.0
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(min(1.0, math.sqrt(a)))


def bbox_overlap(a: tuple[float, float, float, float],
                 b: tuple[float, float, float, float],
                 margin_m: float = 500.0) -> tuple[float, float, float, float] | None:
    west = max(a[0], b[0])
    south = max(a[1], b[1])
    east = min(a[2], b[2])
    north = min(a[3], b[3])
    if west < east and south < north:
        return west, south, east, north

    lat_ref = (a[1] + a[3] + b[1] + b[3]) * 0.25
    dlat = margin_m / 111320.0
    dlon = margin_m / (111320.0 * max(0.2, math.cos(math.radians(lat_ref))))

    # Adjacent VREG cells share an edge with zero-width intersection — widen to a stripe.
    if abs(a[2] - b[0]) < 1e-9 or abs(b[2] - a[0]) < 1e-9:
        shared_lon = a[2] if abs(a[2] - b[0]) < 1e-9 else a[0]
        lat_lo = max(a[1], b[1])
        lat_hi = min(a[3], b[3])
        if lat_lo < lat_hi:
            return shared_lon - dlon, lat_lo, shared_lon + dlon, lat_hi

    if abs(a[3] - b[1]) < 1e-9 or abs(b[3] - a[1]) < 1e-9:
        shared_lat = a[3] if abs(a[3] - b[1]) < 1e-9 else a[1]
        lon_lo = max(a[0], b[0])
        lon_hi = min(a[2], b[2])
        if lon_lo < lon_hi:
            return lon_lo, shared_lat - dlat, lon_hi, shared_lat + dlat

    return None


def point_in_bbox(lon: float, lat: float, bbox: tuple[float, float, float, float]) -> bool:
    return bbox[0] <= lon <= bbox[2] and bbox[1] <= lat <= bbox[3]


def match_portals(rid_a: int, graph_a: bytes | None, bbox_a: tuple[float, float, float, float],
                  rid_b: int, graph_b: bytes | None, bbox_b: tuple[float, float, float, float],
                  match_m: float = PORTAL_MATCH_M) -> list[dict[str, Any]]:
    if not graph_a or not graph_b:
        return []

    ea = expand_bbox(bbox_a, GRAPH_CLIP_MARGIN_M)
    eb = expand_bbox(bbox_b, GRAPH_CLIP_MARGIN_M)
    overlap = rect_intersection(ea, eb)
    if overlap is None:
        overlap = bbox_overlap(bbox_a, bbox_b, margin_m=GRAPH_CLIP_MARGIN_M)
    if overlap is None:
        return []

    ga = parse_vgrf(graph_a)
    gb = parse_vgrf(graph_b)
    used_b: set[int] = set()
    portals: list[dict[str, Any]] = []

    cand_b: list[tuple[int, float, float]] = []
    for ib, (lon_e7_b, lat_e7_b) in enumerate(gb["nodes"]):
        lon_b, lat_b = e7_to_deg(lon_e7_b, lat_e7_b)
        if point_in_bbox(lon_b, lat_b, overlap):
            cand_b.append((ib, lon_b, lat_b))
    if not cand_b:
        return []

    for ia, (lon_e7, lat_e7) in enumerate(ga["nodes"]):
        lon_a, lat_a = e7_to_deg(lon_e7, lat_e7)
        if not point_in_bbox(lon_a, lat_a, overlap):
            continue

        best_ib = -1
        best_d = match_m
        for ib, lon_b, lat_b in cand_b:
            if ib in used_b:
                continue
            d = haversine_m(lon_a, lat_a, lon_b, lat_b)
            if d < best_d:
                best_ib = ib
                best_d = d

        if best_ib < 0:
            continue

        used_b.add(best_ib)
        lon_b, lat_b = e7_to_deg(gb["nodes"][best_ib][0], gb["nodes"][best_ib][1])
        portals.append({
            "region_a": rid_a,
            "region_b": rid_b,
            "node_a": ia,
            "node_b": best_ib,
            "lon": (lon_a + lon_b) * 0.5,
            "lat": (lat_a + lat_b) * 0.5,
            "cross_m": best_d,
        })
        if len(portals) >= PORTAL_MAX_PER_BORDER:
            break

    portals.sort(key=lambda p: p["cross_m"])
    return portals


VPOR_MAGIC = b"VPOR"
VPOR_VERSION = 1
VPOR_HDR_SIZE = 8
VPOR_NBR_SIZE = 8
VPOR_REC_SIZE = 24


def split_portals_by_cell(portals: list[dict[str, Any]]
                          ) -> dict[int, dict[int, list[dict[str, Any]]]]:
    """Each cell stores outgoing portals to its 4-neighbors (duplicated)."""
    by: dict[int, dict[int, list[dict[str, Any]]]] = {}
    for p in portals:
        a = p["region_a"] & 0xFFFFFFFF
        b = p["region_b"] & 0xFFFFFFFF
        rec_ab = {
            "node_self": p["node_a"] & 0xFFFFFFFF,
            "node_nbr": p["node_b"] & 0xFFFFFFFF,
            "lon": p["lon"],
            "lat": p["lat"],
            "cross_m": p.get("cross_m", 0.0),
        }
        rec_ba = {
            "node_self": p["node_b"] & 0xFFFFFFFF,
            "node_nbr": p["node_a"] & 0xFFFFFFFF,
            "lon": p["lon"],
            "lat": p["lat"],
            "cross_m": p.get("cross_m", 0.0),
        }
        by.setdefault(a, {}).setdefault(b, []).append(rec_ab)
        by.setdefault(b, {}).setdefault(a, []).append(rec_ba)
    for nbrs in by.values():
        for nb, recs in nbrs.items():
            recs.sort(key=lambda r: r.get("cross_m", 0.0))
            if len(recs) > PORTAL_MAX_PER_BORDER:
                nbrs[nb] = recs[:PORTAL_MAX_PER_BORDER]
    return by


def encode_vpk_portals(nbrs: dict[int, list[dict[str, Any]]]) -> bytes:
    keys = sorted(nbrs.keys())
    buf = bytearray()
    buf.extend(VPOR_MAGIC)
    buf.extend(struct.pack("<BBH", VPOR_VERSION, 0, len(keys)))
    recs_blob = bytearray()
    first = 0
    for nb in keys:
        recs = nbrs[nb]
        if len(recs) > 0xFFFF or first > 0xFFFF:
            raise ValueError("VPOR neighbor table overflow")
        buf.extend(struct.pack("<IHH", nb & 0xFFFFFFFF, first, len(recs)))
        first += len(recs)
        for r in recs:
            recs_blob.extend(struct.pack(
                "<IIdd",
                r["node_self"] & 0xFFFFFFFF,
                r["node_nbr"] & 0xFFFFFFFF,
                r["lon"],
                r["lat"],
            ))
    buf.extend(recs_blob)
    return bytes(buf)


def read_route_port(path) -> list[dict[str, Any]]:
    data = path.read_bytes()
    if data[:4] != b"PORT" or len(data) < 14:
        return []
    ver = data[4]
    rec_count, pair_count = struct.unpack_from("<II", data, 6)
    recs: list[dict[str, Any]] = []
    if ver >= 3:
        o = 14 + pair_count * 16
        for _ in range(rec_count):
            if o + 32 > len(data):
                break
            a, b, na, nb, lon, lat = struct.unpack_from("<IIIIdd", data, o)
            recs.append({
                "region_a": a, "region_b": b, "node_a": na, "node_b": nb,
                "lon": lon, "lat": lat, "cross_m": 0.0,
            })
            o += 32
        return recs
    if ver == 2:
        o = 14 + pair_count * 12
        rec_sz = 28
    else:
        o = 10
        rec_sz = 28
    for _ in range(rec_count):
        if o + rec_sz > len(data):
            break
        a, b = struct.unpack_from("<HH", data, o)
        na, nb = struct.unpack_from("<II", data, o + 4)
        lon, lat = struct.unpack_from("<dd", data, o + 12)
        recs.append({
            "region_a": a, "region_b": b, "node_a": na, "node_b": nb,
            "lon": lon, "lat": lat, "cross_m": 0.0,
        })
        o += rec_sz
    return recs


def write_route_port(path, portals: list[dict[str, Any]]) -> None:
    """route.port v3: uint32 cell ids. Header + sorted pair index + records."""
    from collections import OrderedDict

    groups: "OrderedDict[tuple[int, int], list]" = OrderedDict()
    for p in portals:
        a = p["region_a"] & 0xFFFFFFFF
        b = p["region_b"] & 0xFFFFFFFF
        key = (a, b) if a <= b else (b, a)
        groups.setdefault(key, []).append(p)

    pairs = sorted(groups.keys())
    rec_count = sum(len(groups[k]) for k in pairs)

    buf = bytearray()
    buf.extend(b"PORT")
    buf.extend(struct.pack("<BBII", 3, 0, rec_count, len(pairs)))

    first = 0
    for key in pairs:
        num = len(groups[key])
        buf.extend(struct.pack("<IIII", key[0], key[1], first, num))
        first += num

    for key in pairs:
        for p in groups[key]:
            buf.extend(struct.pack(
                "<IIIIdd",
                p["region_a"] & 0xFFFFFFFF,
                p["region_b"] & 0xFFFFFFFF,
                p["node_a"] & 0xFFFFFFFF,
                p["node_b"] & 0xFFFFFFFF,
                p["lon"],
                p["lat"],
            ))
    path.write_bytes(bytes(buf))
