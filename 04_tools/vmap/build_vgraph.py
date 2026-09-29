#!/usr/bin/env python3
"""
build_vgraph.py — OSM highway network -> VGRF offline routing graph.

Companion to build_vmap.py: same OSM extract, but builds a bicycle routing
graph with junction topology (OSM node IDs), oneway flags, and undirected
on-disk edges (VGRF v4). The device expands forward-star adjacency on load.

Output: graph.vgrf (see ROUTING.md / vmap_format.h).
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys

# Reuse GCJ/WGS helpers and road classification from the tile builder.
import build_vmap as bv

VGRF_MAGIC = b"VGRF"
VGRF_VERSION = 4
VGRF_PROFILE_BICYCLE = 0
VGRF_SNAP_CELL_M = 200
VGRF_ELE_UNKNOWN = -32768

VGRF_EDGE_ONEWAY = 0x01
VGRF_EDGE_NO_BIKE = 0x02

VGRF_NAME_NONE = 0xFFFFFFFF
VGRF_NAME_NONE_V2 = 0xFFFF
VGRF_EDGE_SIZE_V2 = 16
VGRF_EDGE_SIZE_V3 = 18
VGRF_STR_POOL_MAX = 128 * 1024

# Dijkstra cost = length_cm * weight / 100
# China OSM tags many urban arterials as trunk; those are rideable.
# True motorway/motorway_link never enter the graph (see bicycle_allowed).
ROAD_WEIGHT = {
    bv.ROAD_MOTORWAY: 120,    # trunk / trunk_link (urban arterial)
    bv.ROAD_PRIMARY: 120,
    bv.ROAD_SECONDARY: 100,
    bv.ROAD_TERTIARY: 100,
    bv.ROAD_RESIDENTIAL: 110,
    bv.ROAD_SERVICE: 130,
    bv.ROAD_PATH: 90,
}


def haversine_m(lon1, lat1, lon2, lat2):
    r = 6371000.0
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    lat1r = math.radians(lat1)
    lat2r = math.radians(lat2)
    a = (math.sin(dlat / 2) ** 2
         + math.cos(lat1r) * math.cos(lat2r) * math.sin(dlon / 2) ** 2)
    return 2.0 * r * math.asin(min(1.0, math.sqrt(a)))


def lonlat_to_e7(lon, lat):
    return int(round(lon * 1e7)), int(round(lat * 1e7))


def e7_to_lonlat(lon_e7, lat_e7):
    return lon_e7 / 1e7, lat_e7 / 1e7


def parse_oneway(tags):
    ow = tags.get("oneway")
    if ow in ("yes", "true", "1"):
        return 1
    if ow == "-1":
        return -1
    if tags.get("junction") == "roundabout":
        return 1
    return 0


# Display tiles still stroke these. Routing skips pedestrian-only and
# compound internals. Keep cycleway (PATH class, dedicated bike infra)
# and generic highway=service (park access, 便桥). Drop parking aisles
# and driveways — those are the bulk of 小区内部 service ways.
_GRAPH_SKIP_HW = {
    "living_street",
    "footway", "path", "steps", "pedestrian",
    "corridor", "platform",
}
_SERVICE_SKIP = {"parking_aisle", "driveway", "parking"}
# Arterials that are 高架 / 立交 when layer>=1 or tagged viaduct.
# Ground-level residential/tertiary river bridges stay (cyclists need them).
_ELEVATED_HW = {
    "trunk", "trunk_link",
    "primary", "primary_link",
    "secondary", "secondary_link",
}


def _osm_layer(tags):
    v = tags.get("layer")
    if not v:
        return 0
    try:
        return int(v.split(";", 1)[0].strip())
    except ValueError:
        return 0


def is_elevated_unrideable(tags):
    """Urban viaduct / 高架: bikes cannot enter. Tiles still draw them."""
    hw = tags.get("highway")
    bridge = (tags.get("bridge") or "").lower()
    loc = (tags.get("location") or "").lower()
    layer = _osm_layer(tags)

    if bridge in ("viaduct",):
        return True
    if loc in ("bridge", "overhead", "elevated") and hw in _ELEVATED_HW:
        return True
    if layer >= 1 and hw in _ELEVATED_HW:
        return True
    if bridge in ("yes", "viaduct") and hw in ("trunk", "trunk_link"):
        return True
    return False


def bicycle_allowed(tags, road_cls):
    """Keep rideable public streets; drop freeways, 高架, pedestrian internals.

    OSM `trunk` in China is usually a wide city arterial (龙兴路 / 滁州大道).
    Ground-level trunks stay unless tagged bicycle=no. Elevated
    trunk/primary/secondary (layer>=1, viaduct) are dropped — bikes cannot
    get onto 内环/快速高架. Display tiles are unchanged.
    Generic `highway=service` stays (park roads / 明湖便桥); only
    parking_aisle / driveway / parking are dropped.
    """
    hw = tags.get("highway")
    if hw in ("motorway", "motorway_link"):
        return False
    if hw in _GRAPH_SKIP_HW:
        return False
    if hw == "service" and tags.get("service") in _SERVICE_SKIP:
        return False
    if is_elevated_unrideable(tags):
        return False
    if tags.get("bicycle") == "no":
        return False
    access = tags.get("access")
    if access in ("private", "no"):
        return False
    return True


def pick_name(tags):
    return bv.pick_name(tags, allow_ref=False)


def build_graph(osm_path):
    import osmium

    node_ll = {}          # osm_id -> (lon, lat)
    edges = []            # dicts: from,to,length_cm,cls,flags,name
    edge_keys = set()

    class Handler(osmium.SimpleHandler):
        def node(self, n):
            if n.location.valid():
                node_ll[n.id] = (n.location.lon, n.location.lat)

        def way(self, w):
            tags = {t.k: t.v for t in w.tags}
            cls = bv.classify_road(tags)
            if cls is None:
                return
            if not bicycle_allowed(tags, cls):
                return

            refs = []
            for n in w.nodes:
                if not n.location.valid():
                    ll = node_ll.get(n.ref)
                    if ll is None:
                        return
                    refs.append((n.ref, ll[0], ll[1]))
                else:
                    refs.append((n.ref, n.location.lon, n.location.lat))
                    node_ll[n.ref] = (n.location.lon, n.location.lat)

            if len(refs) < 2:
                return

            oneway = parse_oneway(tags)
            name = pick_name(tags)
            flags_base = 0
            if tags.get("bicycle") == "no":
                flags_base |= VGRF_EDGE_NO_BIKE

            for i in range(len(refs) - 1):
                id_a, lon_a, lat_a = refs[i]
                id_b, lon_b, lat_b = refs[i + 1]
                length_cm = int(haversine_m(lon_a, lat_a, lon_b, lat_b) * 100.0)
                if length_cm < 1:
                    length_cm = 1

                def add_edge(frm, to, ow_flag):
                    key = (frm, to)
                    if key in edge_keys:
                        return
                    edge_keys.add(key)
                    edges.append({
                        "from": frm,
                        "to": to,
                        "length_cm": length_cm,
                        "cls": cls,
                        "flags": flags_base | ow_flag,
                        "name": name,
                    })

                # Bicycle profile: car oneway does not block reverse travel
                # unless oneway:bicycle=yes (common in CN urban grids).
                bike_oneway = tags.get("oneway:bicycle") == "yes"
                if oneway == 1 and bike_oneway:
                    add_edge(id_a, id_b, VGRF_EDGE_ONEWAY)
                elif oneway == -1 and bike_oneway:
                    add_edge(id_b, id_a, VGRF_EDGE_ONEWAY)
                else:
                    add_edge(id_a, id_b, 0)
                    add_edge(id_b, id_a, 0)

    h = Handler()
    h.apply_file(osm_path, locations=True)

    # dense node index
    osm_ids = set()
    for e in edges:
        osm_ids.add(e["from"])
        osm_ids.add(e["to"])
    osm_ids = sorted(osm_ids)
    # Stitch micro-gaps: merge OSM nodes within ~12 m (split endpoints).
    MERGE_M = 12.0
    CELL_DEG = 0.00012
    parent = {oid: oid for oid in osm_ids}
    buckets = {}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def unite(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    for oid in osm_ids:
        lon, lat = node_ll[oid]
        key = (int(lat / CELL_DEG), int(lon / CELL_DEG))
        buckets.setdefault(key, []).append(oid)

    for (cy, cx), members in buckets.items():
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nb = buckets.get((cy + dy, cx + dx))
                if not nb:
                    continue
                for a in members:
                    lon_a, lat_a = node_ll[a]
                    for b in nb:
                        if a >= b:
                            continue
                        lon_b, lat_b = node_ll[b]
                        if haversine_m(lon_a, lat_a, lon_b, lat_b) <= MERGE_M:
                            unite(a, b)

    id_map = {}
    dense_ids = []
    for oid in osm_ids:
        root = find(oid)
        if root not in id_map:
            id_map[root] = len(dense_ids)
            dense_ids.append(root)
    for oid in osm_ids:
        id_map[oid] = id_map[find(oid)]

    nodes = []
    west = south = 180.0
    east = north = -180.0
    for oid in dense_ids:
        lon, lat = node_ll[oid]
        west = min(west, lon)
        east = max(east, lon)
        south = min(south, lat)
        north = max(north, lat)
        lon_e7, lat_e7 = lonlat_to_e7(lon, lat)
        nodes.append((lon_e7, lat_e7))

    # string pool
    str_pool = bytearray()
    name_off_map = {}

    def intern_name(s):
        nonlocal str_pool
        if not s:
            return VGRF_NAME_NONE
        if s in name_off_map:
            return name_off_map[s]
        off = len(str_pool)
        str_pool += s.encode("utf-8") + b"\x00"
        name_off_map[s] = off
        return off

    dense_edges = []
    for e in edges:
        dense_edges.append({
            "from": id_map[e["from"]],
            "to": id_map[e["to"]],
            "length_cm": e["length_cm"],
            "cls": e["cls"],
            "flags": e["flags"],
            "name_off": intern_name(e["name"]),
        })

    n = len(nodes)
    print("[vgraph] nodes=%d directed-edges=%d strings=%d"
          % (n, len(dense_edges), len(str_pool)), file=sys.stderr)
    if len(str_pool) > VGRF_STR_POOL_MAX:
        print("[vgraph] city string pool %d B > 128KiB (host file; "
              "pack_map re-interns each 15km region)"
              % len(str_pool), file=sys.stderr)
    return nodes, dense_edges, bytes(str_pool), west, south, east, north


def _edge_bike_ok(e) -> bool:
    return (int(e["flags"]) & VGRF_EDGE_NO_BIKE) == 0


def fold_undirected_edges(edges, *, already_undirected=False):
    """Collapse A→B / B→A into one record. ONEWAY if travel is only from→to.

    Directed v2/v3 input uses presence of both orientations. v4 input already
    stores one record per pair: a clear ONEWAY bit means bidirectional.
    """
    groups: dict[tuple[int, int], list] = {}
    for e in edges:
        a = int(e["from"])
        b = int(e["to"])
        if a == b:
            continue
        key = (a, b) if a < b else (b, a)
        groups.setdefault(key, []).append(e)

    out = []
    for (lo, hi), group in groups.items():
        bike = [e for e in group if _edge_bike_ok(e)]
        if already_undirected:
            bi = any((int(e["flags"]) & VGRF_EDGE_ONEWAY) == 0 for e in group)
            base = (bike or group)[0]
            if bi and bike:
                rec_from, rec_to = lo, hi
                flags = int(base["flags"]) & ~(VGRF_EDGE_ONEWAY | VGRF_EDGE_NO_BIKE)
            elif bike:
                rec_from, rec_to = int(base["from"]), int(base["to"])
                flags = (int(base["flags"]) & ~VGRF_EDGE_NO_BIKE) | VGRF_EDGE_ONEWAY
            else:
                rec_from, rec_to = int(group[0]["from"]), int(group[0]["to"])
                flags = (int(group[0]["flags"]) | VGRF_EDGE_NO_BIKE
                         | VGRF_EDGE_ONEWAY)
                base = group[0]
        else:
            fwd_ok = any(int(e["from"]) == lo and int(e["to"]) == hi
                         for e in bike)
            rev_ok = any(int(e["from"]) == hi and int(e["to"]) == lo
                         for e in bike)
            if fwd_ok and rev_ok:
                base = next(e for e in bike
                            if int(e["from"]) == lo and int(e["to"]) == hi)
                rec_from, rec_to = lo, hi
                flags = int(base["flags"]) & ~(VGRF_EDGE_ONEWAY | VGRF_EDGE_NO_BIKE)
            elif fwd_ok:
                base = next(e for e in bike
                            if int(e["from"]) == lo and int(e["to"]) == hi)
                rec_from, rec_to = lo, hi
                flags = (int(base["flags"]) & ~VGRF_EDGE_NO_BIKE) | VGRF_EDGE_ONEWAY
            elif rev_ok:
                base = next(e for e in bike
                            if int(e["from"]) == hi and int(e["to"]) == lo)
                rec_from, rec_to = hi, lo
                flags = (int(base["flags"]) & ~VGRF_EDGE_NO_BIKE) | VGRF_EDGE_ONEWAY
            else:
                base = group[0]
                rec_from, rec_to = int(base["from"]), int(base["to"])
                flags = (int(base["flags"]) | VGRF_EDGE_NO_BIKE
                         | VGRF_EDGE_ONEWAY)
        rec = {
            "from": rec_from,
            "to": rec_to,
            "length_cm": min(int(e["length_cm"]) for e in (bike or group)),
            "cls": int(base["cls"]),
            "flags": flags,
            "name_off": int(base.get("name_off", VGRF_NAME_NONE)),
        }
        if base.get("name") is not None:
            rec["name"] = base["name"]
        out.append(rec)
    return out


def build_snap_grid(nodes, west, south, east, north, cell_m=VGRF_SNAP_CELL_M):
    """Uniform grid index (~200 m cells) for O(1) local snap queries."""
    cell_e7 = max(1000, int(cell_m * 1e7 / 111320.0))
    olon = int(round(west * 1e7))
    olat = int(round(south * 1e7))
    elon = int(round(east * 1e7))
    nlat = int(round(north * 1e7))
    ncol = max(1, (elon - olon) // cell_e7 + 1)
    nrow = max(1, (nlat - olat) // cell_e7 + 1)
    buckets = [[] for _ in range(ncol * nrow)]

    for i, (lon_e7, lat_e7) in enumerate(nodes):
        cx = (lon_e7 - olon) // cell_e7
        cy = (lat_e7 - olat) // cell_e7
        if 0 <= cx < ncol and 0 <= cy < nrow:
            buckets[cy * ncol + cx].append(i)

    flat = []
    off = [0]
    for b in buckets:
        flat.extend(b)
        off.append(len(flat))
    return olon, olat, cell_e7, ncol, nrow, off, flat


def serialize_snap(olon, olat, cell_e7, ncol, nrow, off, flat):
    out = bytearray()
    out += b"SNAP"
    out += struct.pack("<HH", 1, VGRF_SNAP_CELL_M)
    out += struct.pack("<ii", olon, olat)
    out += struct.pack("<HH", ncol, nrow)
    for o in off:
        out += struct.pack("<I", o)
    for node in flat:
        out += struct.pack("<I", node)
    return bytes(out)


def snap_section_end(data: bytes, snap_off: int) -> int:
    """Byte offset immediately after a SNAP blob, or snap_off if invalid."""
    if snap_off < 0 or snap_off + 20 > len(data):
        return snap_off
    if data[snap_off:snap_off + 4] != b"SNAP":
        return snap_off
    ncol, nrow = struct.unpack_from("<HH", data, snap_off + 16)
    ncells = int(ncol) * int(nrow)
    off = snap_off + 20
    if off + (ncells + 1) * 4 > len(data):
        return snap_off
    n_list = struct.unpack_from("<I", data, off + ncells * 4)[0]
    end = off + (ncells + 1) * 4 + n_list * 4
    return end if end <= len(data) else snap_off


def serialize_elev(ele_m) -> bytes:
    out = bytearray()
    out += b"ELEV"
    out += struct.pack("<HHI", 1, 0, len(ele_m))
    for v in ele_m:
        iv = int(v)
        if iv < VGRF_ELE_UNKNOWN:
            iv = VGRF_ELE_UNKNOWN
        elif iv > 32767:
            iv = 32767
        out += struct.pack("<h", iv)
    return bytes(out)


def parse_elev_section(data: bytes, off: int, node_count: int):
    if off < 0 or off + 12 > len(data):
        return None
    if data[off:off + 4] != b"ELEV":
        return None
    ver, _flags, count = struct.unpack_from("<HHI", data, off + 4)
    if ver != 1 or count != node_count:
        return None
    need = off + 12 + count * 2
    if need > len(data):
        return None
    return list(struct.unpack_from("<%dh" % count, data, off + 12))


def serialize_vgrf(nodes, edges, str_pool, west, south, east, north,
                   ele_m=None, already_undirected=False, verbose=False):
    edges = fold_undirected_edges(edges, already_undirected=already_undirected)
    if verbose:
        print("[vgraph] write v%d nodes=%d undirected-edges=%d"
              % (VGRF_VERSION, len(nodes), len(edges)), file=sys.stderr)
    olon, olat, cell_e7, ncol, nrow, snap_off_list, snap_nodes = build_snap_grid(
        nodes, west, south, east, north)
    snap_blob = serialize_snap(olon, olat, cell_e7, ncol, nrow, snap_off_list, snap_nodes)

    out = bytearray()
    out += VGRF_MAGIC
    out += struct.pack("<BBHII", VGRF_VERSION, VGRF_PROFILE_BICYCLE, 0,
                       len(nodes), len(edges))
    out += struct.pack("<dddd", west, south, east, north)
    out += struct.pack("<I", 0)

    for lon_e7, lat_e7 in nodes:
        out += struct.pack("<ii", lon_e7, lat_e7)

    for e in edges:
        out += struct.pack("<III", e["from"], e["to"], e["length_cm"])
        out += struct.pack("<BBI", e["cls"] & 0xFF, e["flags"] & 0xFF,
                           e["name_off"] & 0xFFFFFFFF)

    out += str_pool
    snap_at = len(out)
    out += snap_blob
    struct.pack_into("<I", out, 48, snap_at)

    if ele_m is not None and len(ele_m) == len(nodes):
        known = sum(1 for v in ele_m if int(v) != VGRF_ELE_UNKNOWN)
        if known > 0:
            out += serialize_elev(ele_m)
            if verbose:
                print("[vgraph] ELEV %d/%d nodes have height"
                      % (known, len(ele_m)), file=sys.stderr)

    if verbose:
        print("[vgraph] snap grid %dx%d cell_e7=%d nodes=%d"
              % (ncol, nrow, cell_e7, len(snap_nodes)), file=sys.stderr)
    return bytes(out)


def sample_node_elev(nodes, dem_dir=None, dem_cache=None, fetch=True, jobs=0):
    from dem import DemSampler, ELE_UNKNOWN

    sampler = DemSampler(dem_dir=dem_dir, cache_dir=dem_cache, fetch=fetch)
    ele = sampler.sample_nodes(nodes, jobs=jobs)
    if not any(v != ELE_UNKNOWN for v in ele):
        print("[vgraph] no DEM samples; omitting ELEV section", file=sys.stderr)
        return None
    return ele


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.normpath(os.path.join(here, "graph.vgrf"))
    default_cache = os.path.join(here, "cache", "dem")

    ap = argparse.ArgumentParser(description="Build VGRF routing graph from OSM.")
    ap.add_argument("--osm", required=True, help="local .osm XML from Overpass")
    ap.add_argument("--out", default=default_out, help="output graph.vgrf path")
    ap.add_argument("--dem-dir", default=os.environ.get("VMAP_DEM_DIR"),
                    help="optional SRTM/Skadi .hgt dir; default is Terrarium PNG cache")
    ap.add_argument("--dem-cache", default=default_cache,
                    help="Terrarium PNG cache directory")
    ap.add_argument("--no-dem", action="store_true",
                    help="do not sample elevation (no ELEV section)")
    ap.add_argument("--no-dem-fetch", action="store_true",
                    help="do not download Terrarium tiles; cache/.hgt only")
    ap.add_argument("--dem-jobs", type=int, default=0,
                    help="parallel Terrarium fetch/sample workers (0 = auto)")
    bv.zh_simplify.add_zh_argument(ap)
    args = ap.parse_args()
    bv.zh_simplify.set_script(args.zh)

    nodes, edges, str_pool, w, s, e, n = build_graph(args.osm)
    if not edges:
        print("[vgraph] ERROR: no routable edges", file=sys.stderr)
        sys.exit(1)

    ele_m = None
    if not args.no_dem:
        ele_m = sample_node_elev(
            nodes, dem_dir=args.dem_dir, dem_cache=args.dem_cache,
            fetch=not args.no_dem_fetch, jobs=args.dem_jobs)

    data = serialize_vgrf(nodes, edges, str_pool, w, s, e, n,
                          ele_m=ele_m, verbose=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "wb") as fp:
        fp.write(data)
    print("[vgraph] wrote %s (%d bytes)" % (args.out, len(data)), file=sys.stderr)


if __name__ == "__main__":
    main()
