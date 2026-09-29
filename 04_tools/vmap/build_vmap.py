#!/usr/bin/env python3
"""
build_vmap.py — Offline OSM -> ".vt" vector-tile builder for the bicycle app.

Pipeline:
  1. Fetch a small bbox of OSM data from the Overpass API (WGS84).
  2. Filter to roads (highway), waterways, water bodies and forest.
  3. Slice each feature into slippy-map z/x/y tiles with an overlap buffer
     so strokes/fills meet across tile edges (MCU has no per-tile scissor),
     and write a compact binary ".vt" file (see VectorMapFormat.h).

The default center is the user's start point in Nanjing, given in GCJ02
(高德/腾讯). It is converted to WGS84 here because OSM and GPS are both WGS84;
no GCJ02 offset is applied on-device.

Output goes to <repo>/.../bicycle/etc/vmap/<z>/<x>/<y>.vt so it is baked into
ROMFS and mounted at /etc/vmap on the device.

Requirements: python3, requests, pyosmium (osmium), shapely.
    pip install requests osmium shapely
"""

import argparse
import math
import multiprocessing
import os
import re
import struct
import sys
import tempfile
import time

import zh_simplify

# ---------------------------------------------------------------------------
# Format constants — keep in sync with VectorMapFormat.h
# ---------------------------------------------------------------------------
VMAP_MAGIC = b"VTIL"
VMAP_VERSION = 2
VMAP_EXTENT = 4096

LAYER_WATER = 0
LAYER_FOREST = 1
LAYER_WATERWAY = 2
LAYER_ROAD = 3
LAYER_LABEL = 4
LAYER_LAND = 5
LAYER_MAX = 6

FLAG_CLOSED = 0x01
FLAG_NAMED = 0x02

ROAD_MOTORWAY = 1
ROAD_PRIMARY = 2
ROAD_SECONDARY = 3
ROAD_TERTIARY = 4
ROAD_RESIDENTIAL = 5
ROAD_SERVICE = 6
ROAD_PATH = 7

LAND_RESIDENTIAL = 1
LAND_COMMERCIAL = 2
LAND_INDUSTRIAL = 3
LAND_EDU = 4
LAND_PARK = 5

LABEL_PLACE = 1
LABEL_WATER = 2
LABEL_ROAD = 3
LABEL_POI = 4
LABEL_MAXLEN = 60

# Default center: Chuzhou (滁州) urban core (琅琊区/南谯区主城), GCJ02.
DEFAULT_GCJ_LON = 118.3175
DEFAULT_GCJ_LAT = 32.3025

# ---------------------------------------------------------------------------
# GCJ02 -> WGS84 (China coordinate de-shift). Iterative inverse of the
# well-known "eviltransform" algorithm. Accurate to a few cm.
# ---------------------------------------------------------------------------
_A = 6378245.0
_EE = 0.00669342162296594323


def _out_of_china(lon, lat):
    return not (73.66 < lon < 135.05 and 3.86 < lat < 53.55)


def _transform_lat(x, y):
    ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * math.sqrt(abs(x))
    ret += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    ret += (20.0 * math.sin(y * math.pi) + 40.0 * math.sin(y / 3.0 * math.pi)) * 2.0 / 3.0
    ret += (160.0 * math.sin(y / 12.0 * math.pi) + 320 * math.sin(y * math.pi / 30.0)) * 2.0 / 3.0
    return ret


def _transform_lon(x, y):
    ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * math.sqrt(abs(x))
    ret += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    ret += (20.0 * math.sin(x * math.pi) + 40.0 * math.sin(x / 3.0 * math.pi)) * 2.0 / 3.0
    ret += (150.0 * math.sin(x / 12.0 * math.pi) + 300.0 * math.sin(x / 30.0 * math.pi)) * 2.0 / 3.0
    return ret


def _gcj_delta(lon, lat):
    dlat = _transform_lat(lon - 105.0, lat - 35.0)
    dlon = _transform_lon(lon - 105.0, lat - 35.0)
    radlat = lat / 180.0 * math.pi
    magic = math.sin(radlat)
    magic = 1 - _EE * magic * magic
    sqrtmagic = math.sqrt(magic)
    dlat = (dlat * 180.0) / ((_A * (1 - _EE)) / (magic * sqrtmagic) * math.pi)
    dlon = (dlon * 180.0) / (_A / sqrtmagic * math.cos(radlat) * math.pi)
    return dlon, dlat


def gcj02_to_wgs84(lon, lat):
    if _out_of_china(lon, lat):
        return lon, lat
    dlon, dlat = _gcj_delta(lon, lat)
    return lon - dlon, lat - dlat


# ---------------------------------------------------------------------------
# Slippy-map projection (Web Mercator), tileSize = 256. Matches TileSystem.
# ---------------------------------------------------------------------------
def lonlat_to_pixel(lon, lat, z):
    n = 256.0 * (2 ** z)
    x = (lon + 180.0) / 360.0 * n
    siny = math.sin(math.radians(lat))
    siny = min(max(siny, -0.9999), 0.9999)
    y = (0.5 - math.log((1 + siny) / (1 - siny)) / (4 * math.pi)) * n
    return x, y


def pixel_to_lonlat(px, py, z):
    n = 256.0 * (2 ** z)
    lon = px / n * 360.0 - 180.0
    t = math.pi * (1.0 - 2.0 * py / n)
    lat = math.degrees(math.atan(math.sinh(t)))
    return lon, lat


def lonlat_to_tile(lon, lat, z):
    px, py = lonlat_to_pixel(lon, lat, z)
    return int(px // 256), int(py // 256)


def tile_bbox_lonlat(tx, ty, z):
    n = 2.0 ** z

    def x2lon(x):
        return x / n * 360.0 - 180.0

    def y2lat(y):
        t = math.pi * (1 - 2 * y / n)
        return math.degrees(math.atan(math.sinh(t)))

    west = x2lon(tx)
    east = x2lon(tx + 1)
    north = y2lat(ty)
    south = y2lat(ty + 1)
    return west, south, east, north


# ---------------------------------------------------------------------------
# OSM tag classification
# ---------------------------------------------------------------------------
# Skip these highway=* values (not drawable centerlines).
_ROAD_SKIP = {
    "construction", "proposed", "abandoned", "disused", "razed", "dismantled",
    "raceway", "bus_guideway", "corridor", "elevator", "services", "rest_area",
    "emergency_bay", "platform", "bus_stop", "via_ferrata", "no",
}

_ROAD_CLASS = {
    "motorway": ROAD_MOTORWAY, "motorway_link": ROAD_MOTORWAY,
    "trunk": ROAD_MOTORWAY, "trunk_link": ROAD_MOTORWAY,
    "primary": ROAD_PRIMARY, "primary_link": ROAD_PRIMARY,
    "secondary": ROAD_SECONDARY, "secondary_link": ROAD_SECONDARY,
    "tertiary": ROAD_TERTIARY, "tertiary_link": ROAD_TERTIARY,
    "residential": ROAD_RESIDENTIAL, "unclassified": ROAD_RESIDENTIAL,
    "living_street": ROAD_RESIDENTIAL, "road": ROAD_RESIDENTIAL,
    "service": ROAD_SERVICE,
    "track": ROAD_PATH, "path": ROAD_PATH, "footway": ROAD_PATH,
    "cycleway": ROAD_PATH, "pedestrian": ROAD_PATH, "steps": ROAD_PATH,
}

# Geometry cleanup for schematic (not physical) rendering.
_LINE_SIMPLIFY_DEG = 0.000035   # ~3–4 m
_MIN_WAY_DEG = 0.000035         # drop stubs shorter than ~4 m
_DUAL_MAX_DEG = 0.00032         # collapse parallel one-ways within ~35 m
_DUAL_SAMPLE_DEG = 0.00008
# Overlap into neighbouring tiles so road casings / water fills meet.
# 24 px at 256 covers motorway case at HTML z16–17; MCU has no per-tile scissor.
_TILE_CLIP_PAD_PX = 24


def classify_road(tags):
    hw = tags.get("highway")
    if hw is None or hw in _ROAD_SKIP:
        return None
    return _ROAD_CLASS.get(hw)


def _is_oneway(tags):
    v = (tags.get("oneway") or "").lower()
    if v in ("yes", "true", "1", "-1"):
        return True
    if tags.get("junction") == "roundabout":
        return True
    return tags.get("highway") in ("motorway", "motorway_link")


def _is_road_area(tags, coords):
    """True for plaza / area:highway rings — do not stroke as a fat outline."""
    if (tags.get("area") or "").lower() == "yes":
        return True
    if tags.get("area:highway"):
        return True
    hw = tags.get("highway")
    if hw in ("pedestrian", "footway", "platform") and len(coords) >= 4:
        if coords[0] == coords[-1]:
            return True
    return False


def _road_merge_key(tags, name):
    if name:
        return name
    ref = tags.get("ref")
    if ref:
        return "ref:" + ref
    return None


def is_water_area(tags):
    return tags.get("natural") == "water" or tags.get("landuse") == "reservoir" \
        or tags.get("water") is not None


def is_forest_area(tags):
    return tags.get("natural") == "wood" or tags.get("landuse") == "forest"


def is_park_area(tags):
    """Named greens the rider should see: 公园 / 湿地 / 花博园. Not 小区花园."""
    if tags.get("leisure") in ("park", "garden", "nature_reserve",
                               "recreation_ground"):
        return True
    if tags.get("natural") in ("wetland",):
        return True
    return False


def is_waterway_line(tags):
    return tags.get("waterway") in ("river", "stream", "canal", "drain", "ditch")


def classify_land(tags):
    """Area fills: 小区 / 商业 / 学校 / 公园. Not buildings (too dense)."""
    if is_forest_area(tags) or is_water_area(tags):
        return None
    lu = tags.get("landuse")
    if lu in ("residential", "apartments"):
        return LAND_RESIDENTIAL
    if tags.get("place") in ("neighbourhood", "suburb", "quarter", "city_block"):
        return LAND_RESIDENTIAL
    if lu in ("commercial", "retail"):
        return LAND_COMMERCIAL
    if lu in ("industrial",):
        return LAND_INDUSTRIAL
    if lu in ("education", "civic_admin", "governmental", "institutional"):
        return LAND_EDU
    if tags.get("amenity") in ("school", "university", "college", "hospital",
                               "kindergarten"):
        return LAND_EDU
    if tags.get("amenity") in ("police", "courthouse", "library", "townhall",
                               "community_centre", "marketplace", "bus_station"):
        return LAND_COMMERCIAL
    if lu in ("recreation_ground",) or tags.get("leisure") in (
            "park", "garden", "recreation_ground", "pitch", "sports_centre",
            "stadium", "nature_reserve") or tags.get("natural") == "wetland":
        return LAND_PARK
    return None


def _closed_ring(coords):
    if len(coords) < 3:
        return None
    ring = list(coords)
    if ring[0] != ring[-1]:
        ring.append(ring[0])
    if len(ring) < 4:
        return None
    return ring


def _poly_area_deg2(coords):
    """Shoelace area in deg² (absolute). ~1e-7 ≈ 1000 m² near 32°N."""
    ring = _closed_ring(coords)
    if not ring:
        return 0.0
    a = 0.0
    for i in range(len(ring) - 1):
        a += ring[i][0] * ring[i + 1][1] - ring[i + 1][0] * ring[i][1]
    return abs(a) * 0.5


def _land_label_prio(attr, area):
    base = {LAND_PARK: 96}.get(attr, 70)
    if area >= 4e-6:
        return min(255, base + 15)
    return base


def geo_bounds(features, labels, pad=0.002):
    """WGS84 W,S,E,N covering feature coords and labels."""
    xs = []
    ys = []
    for feat in features:
        for lon, lat in feat.get("coords") or ():
            xs.append(lon)
            ys.append(lat)
        for ring in feat.get("holes") or ():
            for lon, lat in ring:
                xs.append(lon)
                ys.append(lat)
    for lab in labels:
        xs.append(lab[0])
        ys.append(lab[1])
    if not xs:
        return None
    return (min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad)


def pick_name(tags, allow_ref=True):
    """Pick a map label. Default script is simplified Chinese.

    Traditional names are converted to simplified unless `--zh hant`.
    """
    for k in zh_simplify.name_keys(allow_ref=allow_ref):
        v = tags.get(k)
        if v:
            return zh_simplify.convert_name(v)[:LABEL_MAXLEN]
    return None


def _park_forest_name(tags):
    """Park / forest / wetland names only. 小区名 (阳光花园、吾悦华府) stay off."""
    name = pick_name(tags)
    if not name:
        return None
    if is_park_area(tags) or is_forest_area(tags):
        return name
    if re.search(r"公园|湿地|花博|森林|林场|植物园|风景名胜|自然保护区", name):
        return name
    return None


def road_label_priority(cls):
    return {
        ROAD_MOTORWAY: 130, ROAD_PRIMARY: 110, ROAD_SECONDARY: 95,
        ROAD_TERTIARY: 80, ROAD_RESIDENTIAL: 65, ROAD_SERVICE: 55,
        ROAD_PATH: 45,
    }.get(cls, 60)


# ---------------------------------------------------------------------------
# Overpass fetch
# ---------------------------------------------------------------------------
def overpass_query_body(bbox, timeout):
    """Overpass QL body (bbox = south,west,north,east)."""
    return f"""
[out:xml][timeout:{timeout}];
(
  way["highway"]({bbox});
  way["waterway"~"river|stream|canal|drain|ditch"]({bbox});
  way["natural"="water"]({bbox});
  way["water"]({bbox});
  way["landuse"="reservoir"]({bbox});
  way["natural"="wood"]({bbox});
  way["landuse"="forest"]({bbox});
  way["landuse"~"residential|apartments|commercial|retail|industrial|education|civic_admin|governmental"]({bbox});
  way["place"~"neighbourhood|suburb|quarter|city_block"]({bbox});
  way["amenity"~"school|university|college|hospital|kindergarten|police|library|townhall|marketplace|bus_station"]({bbox});
  way["leisure"~"park|garden|recreation_ground|pitch|sports_centre|stadium"]({bbox});
  relation["type"="multipolygon"]["natural"="water"]({bbox});
  relation["type"="multipolygon"]["water"]({bbox});
  relation["type"="multipolygon"]["landuse"="reservoir"]({bbox});
  relation["type"="multipolygon"]["natural"="wood"]({bbox});
  relation["type"="multipolygon"]["landuse"="forest"]({bbox});
  relation["type"="multipolygon"]["landuse"~"residential|apartments|commercial|retail|industrial|education|civic_admin|governmental"]({bbox});
  relation["type"="multipolygon"]["place"~"neighbourhood|suburb|quarter"]({bbox});
  relation["type"="multipolygon"]["amenity"~"school|university|college|hospital|kindergarten|police|library|townhall|marketplace|bus_station"]({bbox});
  relation["type"="multipolygon"]["leisure"~"park|garden|recreation_ground|pitch|sports_centre|stadium"]({bbox});
  node["place"~"city|town|suburb|village|neighbourhood|hamlet|quarter|locality"]({bbox});
  node["natural"="peak"]["name"]({bbox});
  node["tourism"]["name"]({bbox});
  node["historic"]["name"]({bbox});
  node["amenity"~"school|university|college|hospital|kindergarten|townhall|library|police|bus_station"]["name"]({bbox});
  node["railway"~"station|halt"]["name"]({bbox});
  node["shop"~"mall|department_store"]["name"]({bbox});
);
(._;>;);
out body;
"""


def fetch_overpass(west, south, east, north, endpoint, timeout):
    import requests

    bbox = "%f,%f,%f,%f" % (south, west, north, east)
    query = overpass_query_body(bbox, timeout)
    print("[overpass] querying %s" % endpoint, file=sys.stderr)
    print("[overpass] bbox(S,W,N,E) = %s" % bbox, file=sys.stderr)
    headers = {
        "User-Agent": "bicycle-vmap/1.0 (openvela vector map tiler)",
        "Accept": "*/*",
    }
    resp = requests.post(endpoint, data={"data": query},
        headers=headers, timeout=timeout + 30)
    resp.raise_for_status()
    return resp.content


# ---------------------------------------------------------------------------
# Multipolygon relations (common for lakes / large reservoirs in OSM)
# ---------------------------------------------------------------------------
def _polygonize_way_ids(way_ids, ways):
    """Join member ways into polygon rings."""
    from shapely.geometry import LineString
    from shapely.ops import polygonize, unary_union

    lines = []
    for wid in way_ids:
        coords = ways.get(wid)
        if coords and len(coords) >= 2:
            lines.append(LineString(coords))
    if not lines:
        return []
    try:
        return list(polygonize(unary_union(lines)))
    except Exception:
        return []


def _polygons_from_members(members, ways):
    """Assemble copied relation members into shapely Polygons (with holes)."""
    from shapely.geometry import Polygon

    outer_ids = []
    inner_ids = []
    for mtype, ref, role in members:
        if mtype != "w":
            continue
        if role == "inner":
            inner_ids.append(ref)
        else:
            outer_ids.append(ref)

    outers = _polygonize_way_ids(outer_ids, ways)
    inners = _polygonize_way_ids(inner_ids, ways)
    if not outers:
        return []

    polys = []
    for outer in outers:
        if outer.is_empty:
            continue
        holes = []
        for inner in inners:
            try:
                if outer.contains(inner.representative_point()):
                    holes.append(list(inner.exterior.coords))
            except Exception:
                continue
        try:
            poly = Polygon(outer.exterior.coords, holes) if holes else outer
            if not poly.is_valid:
                poly = poly.buffer(0)
            if poly.is_empty:
                continue
            if poly.geom_type == "MultiPolygon":
                polys.extend(p for p in poly.geoms if not p.is_empty)
            elif poly.geom_type == "Polygon":
                polys.append(poly)
        except Exception:
            continue
    return polys


def _feature_from_polygon(layer, poly, attr=0):
    """Feature dict from a shapely Polygon (lon/lat)."""
    if poly.is_empty or len(poly.exterior.coords) < 4:
        return None
    feat = {"layer": layer, "attr": attr, "closed": True,
            "coords": list(poly.exterior.coords)}
    if poly.interiors:
        feat["holes"] = [list(r.coords) for r in poly.interiors]
    return feat


def _water_label_for_poly(poly, name):
    """Place a water label at the polygon representative point."""
    try:
        p = poly.representative_point()
        lon, lat = p.x, p.y
    except Exception:
        c = poly.centroid
        lon, lat = c.x, c.y
    npts = len(poly.exterior.coords)
    prio = 140 if npts >= 30 else 110
    return (lon, lat, LABEL_WATER, prio, name)


# ---------------------------------------------------------------------------
# Parse OSM XML into classified features (geometry in lon/lat).
# Returns list of dicts: {layer, attr, closed, coords:[(lon,lat),...]}
# ---------------------------------------------------------------------------
def parse_osm(osm_path):
    import osmium

    features = []
    labels = []  # (lon, lat, kind, priority, name)
    ways = {}          # id -> [(lon, lat), ...]
    way_tags = {}      # id -> {tag: val}
    mp_relations = []  # (layer, relation)
    name_scan = zh_simplify.NameScan()

    def centroid(coords):
        return (sum(c[0] for c in coords) / len(coords),
                sum(c[1] for c in coords) / len(coords))

    class Handler(osmium.SimpleHandler):
        def node(self, n):
            tags = {t.k: t.v for t in n.tags}
            name_scan.add_tags(tags)
            name = _park_forest_name(tags)
            if name:
                labels.append((n.location.lon, n.location.lat,
                               LABEL_POI, 90, name))

        def way(self, w):
            try:
                coords = [(n.lon, n.lat) for n in w.nodes if n.location.valid()]
            except Exception:
                coords = []
            if len(coords) < 2:
                return
            ways[w.id] = coords
            way_tags[w.id] = {t.k: t.v for t in w.tags}
            name_scan.add_tags(way_tags[w.id])

        def relation(self, r):
            tags = {t.k: t.v for t in r.tags}
            name_scan.add_tags(tags)
            if tags.get("type") != "multipolygon":
                return
            members = [(m.type, m.ref, m.role) for m in r.members]
            if is_water_area(tags):
                mp_relations.append((LAYER_WATER, members, tags))
            elif is_forest_area(tags):
                mp_relations.append((LAYER_FOREST, members, tags))
            elif classify_land(tags) is not None:
                mp_relations.append((LAYER_LAND, members, tags))

    h = Handler()
    h.apply_file(osm_path, locations=True)

    rel_way_ids = set()
    for layer, members, tags in mp_relations:
        for mtype, ref, role in members:
            if mtype == "w":
                rel_way_ids.add(ref)
        for poly in _polygons_from_members(members, ways):
            attr = classify_land(tags) if layer == LAYER_LAND else 0
            feat = _feature_from_polygon(layer, poly, attr=attr or 0)
            if feat:
                features.append(feat)
            name = pick_name(tags, allow_ref=False) if layer == LAYER_WATER \
                else _park_forest_name(tags)
            if name:
                try:
                    p = poly.representative_point()
                except Exception:
                    continue
                if layer == LAYER_WATER:
                    labels.append(_water_label_for_poly(poly, name))
                elif layer in (LAYER_FOREST, LAYER_LAND):
                    attr = classify_land(tags) if layer == LAYER_LAND else LAND_PARK
                    labels.append((p.x, p.y, LABEL_POI,
                                   _land_label_prio(attr or LAND_PARK, poly.area),
                                   name))

    n_mp = len(mp_relations)
    for wid, coords in ways.items():
        if wid in rel_way_ids:
            continue
        tags = way_tags.get(wid, {})

        name = pick_name(tags)

        road = classify_road(tags)
        if road is not None:
            if _is_road_area(tags, coords):
                continue
            feat = {"layer": LAYER_ROAD, "attr": road,
                    "closed": False, "coords": coords,
                    "oneway": _is_oneway(tags)}
            merge_key = _road_merge_key(tags, name)
            if merge_key:
                feat["merge_key"] = merge_key
            if name:
                feat["name"] = name
                feat["label_prio"] = road_label_priority(road)
            features.append(feat)
            continue
        if is_water_area(tags):
            features.append({"layer": LAYER_WATER, "attr": 0,
                             "closed": True, "coords": coords})
            wname = pick_name(tags, allow_ref=False)
            if wname:
                c = centroid(coords)
                prio = 140 if len(coords) >= 30 else 110
                labels.append((c[0], c[1], LABEL_WATER, prio, wname))
            continue
        if is_forest_area(tags):
            features.append({"layer": LAYER_FOREST, "attr": 0,
                             "closed": True, "coords": coords})
            nforest = _park_forest_name(tags)
            if nforest:
                c = centroid(coords)
                labels.append((c[0], c[1], LABEL_POI, 88, nforest))
            continue
        if is_waterway_line(tags):
            features.append({"layer": LAYER_WATERWAY, "attr": 0,
                             "closed": False, "coords": coords})
            continue

        land = classify_land(tags)
        if land is not None:
            ring = _closed_ring(coords)
            if not ring:
                continue
            area = _poly_area_deg2(ring)
            # Drop tiny unnamed scraps; keep named 小区 even if small.
            if not name and area < 1.5e-7:
                continue
            features.append({"layer": LAYER_LAND, "attr": land,
                             "closed": True, "coords": ring})
            npark = _park_forest_name(tags)
            if npark:
                c = centroid(ring)
                labels.append((c[0], c[1], LABEL_POI,
                               _land_label_prio(land, area), npark))
            continue

    n_road = sum(1 for f in features if f["layer"] == LAYER_ROAD)
    n_land = sum(1 for f in features if f["layer"] == LAYER_LAND)
    features = schematize_roads(features)
    n_road_s = sum(1 for f in features if f["layer"] == LAYER_ROAD)
    print("[parse] %d features (%d land, %d mp), %d labels"
          % (len(features), n_land, n_mp, len(labels)), file=sys.stderr)
    print("[schematic] roads %d -> %d (dual collapse + simplify)"
          % (n_road, n_road_s), file=sys.stderr)
    name_scan.finish()
    return features, labels, name_scan


# ---------------------------------------------------------------------------
# Schematic road geometry: class centerlines, not physical carriageways.
# ---------------------------------------------------------------------------
def _pt_eq(a, b, eps=2.0e-5):
    """~2 m — OSM ways at a node sometimes differ by millimetres after simplify."""
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def _bearing(coords):
    if len(coords) < 2:
        return None
    dx = coords[-1][0] - coords[0][0]
    dy = coords[-1][1] - coords[0][1]
    if abs(dx) < 1e-12 and abs(dy) < 1e-12:
        return None
    return math.atan2(dy, dx)


def _ang_diff(a, b):
    d = abs(a - b) % (2.0 * math.pi)
    if d > math.pi:
        d = 2.0 * math.pi - d
    return d


def _same_dir(b0, b1, lim=math.radians(38)):
    if b0 is None or b1 is None:
        return True
    return _ang_diff(b0, b1) < lim


def _parallel(b0, b1, lim=math.radians(32)):
    if b0 is None or b1 is None:
        return False
    d = _ang_diff(b0, b1)
    return d < lim or abs(d - math.pi) < lim


def _seg_bearing(coords, at_head):
    if len(coords) < 2:
        return None
    if at_head:
        return math.atan2(coords[1][1] - coords[0][1],
                          coords[1][0] - coords[0][0])
    return math.atan2(coords[-1][1] - coords[-2][1],
                      coords[-1][0] - coords[-2][0])


def _chain_same_dir(coord_lists):
    """Join pieces that share an endpoint and keep heading (no U-turn merge)."""
    items = [list(c) for c in coord_lists if c and len(c) >= 2]
    used = [False] * len(items)
    chains = []
    for i in range(len(items)):
        if used[i]:
            continue
        used[i] = True
        coords = list(items[i])
        grew = True
        while grew:
            grew = False
            head, tail = coords[0], coords[-1]
            hb = _seg_bearing(coords, True)
            tb = _seg_bearing(coords, False)
            for j in range(len(items)):
                if used[j]:
                    continue
                c = items[j]
                c0, c1 = c[0], c[-1]
                if _pt_eq(tail, c0) and _same_dir(tb, _seg_bearing(c, True)):
                    coords.extend(c[1:])
                    used[j] = True
                    grew = True
                    break
                if _pt_eq(tail, c1) and _same_dir(tb, math.atan2(
                        c[-2][1] - c[-1][1], c[-2][0] - c[-1][0])):
                    coords.extend(reversed(c[:-1]))
                    used[j] = True
                    grew = True
                    break
                if _pt_eq(head, c1) and _same_dir(hb, _seg_bearing(c, False)):
                    coords = c + coords[1:]
                    used[j] = True
                    grew = True
                    break
                if _pt_eq(head, c0) and _same_dir(hb, math.atan2(
                        c[0][1] - c[1][1], c[0][0] - c[1][0])):
                    coords = list(reversed(c)) + coords[1:]
                    used[j] = True
                    grew = True
                    break
        chains.append(coords)
    return chains


def _dual_centerline(a, b):
    """Midline between two parallel one-way carriageways."""
    from shapely.geometry import LineString

    longer, shorter = (a, b) if a.length >= b.length else (b, a)
    n = max(6, int(longer.length / _DUAL_SAMPLE_DEG))
    n = min(n, 400)
    pts = []
    last = None
    for i in range(n + 1):
        pa = longer.interpolate(float(i) / n, normalized=True)
        pb = shorter.interpolate(shorter.project(pa))
        p = ((pa.x + pb.x) * 0.5, (pa.y + pb.y) * 0.5)
        if last is None or p != last:
            pts.append(p)
            last = p
    if len(pts) < 2:
        return longer
    try:
        return LineString(pts).simplify(_LINE_SIMPLIFY_DEG, preserve_topology=False)
    except Exception:
        return LineString(pts)


def _collapse_dual(geoms):
    from shapely.geometry import LineString

    remaining = list(geoms)
    changed = True
    while changed:
        changed = False
        n = len(remaining)
        drop = set()
        added = []
        for i in range(n):
            if i in drop:
                continue
            for j in range(i + 1, n):
                if j in drop:
                    continue
                gi, gj = remaining[i], remaining[j]
                try:
                    dist = gi.distance(gj)
                except Exception:
                    continue
                if dist < 1e-8 or dist > _DUAL_MAX_DEG:
                    continue
                if not _parallel(_bearing(list(gi.coords)), _bearing(list(gj.coords))):
                    continue
                try:
                    h = gi.hausdorff_distance(gj)
                except Exception:
                    h = dist
                if h > _DUAL_MAX_DEG * 3.5:
                    continue
                mid = _dual_centerline(gi, gj)
                if mid is None or mid.is_empty:
                    continue
                drop.add(i)
                drop.add(j)
                added.append(mid)
                changed = True
                break
            if changed:
                break
        if changed:
            remaining = [remaining[k] for k in range(n) if k not in drop] + added
    return remaining


def _simplify_line_coords(coords):
    from shapely.geometry import LineString

    if len(coords) < 2:
        return None
    try:
        g = LineString(coords)
        if g.is_empty or g.length < _MIN_WAY_DEG:
            return None
        g = g.simplify(_LINE_SIMPLIFY_DEG, preserve_topology=False)
        if g.is_empty or g.length < _MIN_WAY_DEG:
            return None
        out = list(g.coords)
        return out if len(out) >= 2 else None
    except Exception:
        return coords if len(coords) >= 2 else None


def schematize_roads(features):
    """Replace physical dual carriageways with one class-width centerline."""
    from collections import defaultdict
    from shapely.geometry import LineString

    roads = []
    rest = []
    for f in features:
        if f.get("layer") == LAYER_ROAD:
            roads.append(f)
        else:
            rest.append(f)

    groups = defaultdict(list)
    leftover = []
    for f in roads:
        key = f.get("merge_key")
        if key:
            groups[(key, f["attr"])].append(f)
        else:
            leftover.append(f)

    out = rest
    for _key, ways in groups.items():
        by_ow = defaultdict(list)
        for w in ways:
            coords = w.get("coords")
            if not coords or len(coords) < 2:
                continue
            by_ow[bool(w.get("oneway"))].append((list(coords), w))

        for is_ow, items in by_ow.items():
            if not items:
                continue
            # Chain on raw endpoints, then simplify — RDP first would move
            # joins and leave gaps in 辅道 / duals.
            chains = _chain_same_dir([c for c, _t in items])
            geoms = []
            for ch in chains:
                coords = _simplify_line_coords(ch)
                if coords is None:
                    continue
                try:
                    g = LineString(coords)
                except Exception:
                    continue
                if g.is_empty or g.length < _MIN_WAY_DEG:
                    continue
                geoms.append(g)
            if is_ow and len(geoms) >= 2:
                geoms = _collapse_dual(geoms)
            tmpl = items[0][1]
            for g in geoms:
                nf = dict(tmpl)
                nf["coords"] = list(g.coords)
                nf["closed"] = False
                out.append(nf)

    for f in leftover:
        coords = _simplify_line_coords(f["coords"])
        if coords is None:
            continue
        nf = dict(f)
        nf["coords"] = coords
        out.append(nf)
    return out


def _insert_unpadded_edge_vertices(pts, extent):
    """Add vertices where a polyline crosses x/y = 0 or extent.

    Neighbour tiles share those edges; inserting the crossing in local
    mercator space (then rounding) makes the stored endpoints match.
    """
    if len(pts) < 2:
        return pts
    edges = (0.0, float(extent))
    out = [pts[0]]
    for i in range(1, len(pts)):
        x0, y0 = pts[i - 1]
        x1, y1 = pts[i]
        dx = x1 - x0
        dy = y1 - y0
        hits = []
        if abs(dx) > 1e-9:
            for xv in edges:
                t = (xv - x0) / dx
                if 1e-8 < t < 1.0 - 1e-8:
                    hits.append((t, (xv, y0 + t * dy)))
        if abs(dy) > 1e-9:
            for yv in edges:
                t = (yv - y0) / dy
                if 1e-8 < t < 1.0 - 1e-8:
                    hits.append((t, (x0 + t * dx, yv)))
        hits.sort(key=lambda h: h[0])
        for _t, p in hits:
            px, py = out[-1]
            if abs(p[0] - px) > 1e-6 or abs(p[1] - py) > 1e-6:
                out.append(p)
        out.append((x1, y1))
    return out


# ---------------------------------------------------------------------------
# Tiling + clipping with shapely (STRtree + optional fork workers)
# ---------------------------------------------------------------------------
_TILE_WORKER = None  # (geoms, tree, z, extent, pad_px, pad_u)


def _to_local(tx, ty, lon, lat, z, extent):
    px, py = lonlat_to_pixel(lon, lat, z)
    lx = (px - tx * 256.0) * (extent / 256.0)
    ly = (py - ty * 256.0) * (extent / 256.0)
    return lx, ly


def _emit_ring(tiles, tx, ty, layer, attr, closed, xy_ring, extent, name=None):
    ring = list(xy_ring)
    if not closed:
        ring = _insert_unpadded_edge_vertices(ring, extent)
    pts = []
    last = None
    for (x, y) in ring:
        p = (int(round(x)), int(round(y)))
        p = (max(0, min(65535, p[0])), max(0, min(65535, p[1])))
        if p != last:
            pts.append(p)
            last = p
    if closed and len(pts) < 3:
        return
    if not closed and len(pts) < 2:
        return
    if len(pts) > 0xFFFF:
        pts = pts[:0xFFFF]
    if name and layer == LAYER_ROAD:
        tiles.setdefault((tx, ty), {}).setdefault(layer, []).append(
            (attr, closed, pts, name))
    else:
        tiles.setdefault((tx, ty), {}).setdefault(layer, []).append(
            (attr, closed, pts))


def _add_clipped(tiles, tx, ty, f, clipped, extent):
    from shapely.geometry.base import BaseGeometry
    gt = clipped.geom_type
    rname = f.get("name") if f["layer"] == LAYER_ROAD else None
    if gt == "Polygon":
        _emit_ring(tiles, tx, ty, f["layer"], f["attr"], True,
                   list(clipped.exterior.coords), extent)
    elif gt == "MultiPolygon":
        for poly in clipped.geoms:
            _emit_ring(tiles, tx, ty, f["layer"], f["attr"], True,
                       list(poly.exterior.coords), extent)
    elif gt == "LineString":
        _emit_ring(tiles, tx, ty, f["layer"], f["attr"], False,
                   list(clipped.coords), extent, rname)
    elif gt == "MultiLineString":
        for ls in clipped.geoms:
            _emit_ring(tiles, tx, ty, f["layer"], f["attr"], False,
                       list(ls.coords), extent, rname)
    elif gt == "GeometryCollection":
        for sub in clipped.geoms:
            if isinstance(sub, BaseGeometry) and not sub.is_empty:
                _add_clipped(tiles, tx, ty, f, sub, extent)


def _clip_tiles_range(tx0, tx1, ty_min, ty_max):
    from shapely.geometry import box
    from shapely.ops import transform

    geoms, tree, z, extent, pad_px, pad_u = _TILE_WORKER
    tiles = {}
    local_rect = box(0, 0, extent + pad_u, extent + pad_u)
    nxy = (tx1 - tx0 + 1) * (ty_max - ty_min + 1)
    done = 0
    t0 = time.monotonic()
    for tx in range(tx0, tx1 + 1):
        for ty in range(ty_min, ty_max + 1):
            x0 = tx * 256.0 - pad_px
            y0 = ty * 256.0 - pad_px
            x1 = (tx + 1) * 256.0 + pad_px
            y1 = (ty + 1) * 256.0 + pad_px
            w, nlat = pixel_to_lonlat(x0, y0, z)
            e, slat = pixel_to_lonlat(x1, y1, z)
            tbox = box(w, slat, e, nlat)

            def _project(lon, lat, _tx=tx, _ty=ty):
                return _to_local(_tx, _ty, lon, lat, z, extent)

            if tree is None:
                pairs = geoms
            else:
                hits = tree.query(tbox, predicate="intersects")
                pairs = (geoms[int(i)] for i in hits)
            for f, g in pairs:
                try:
                    geo_c = g.intersection(tbox)
                    if geo_c.is_empty:
                        continue
                    loc = transform(_project, geo_c)
                    clipped = loc.intersection(local_rect)
                except Exception:
                    continue
                if clipped.is_empty:
                    continue
                _add_clipped(tiles, tx, ty, f, clipped, extent)
            done += 1
            if done == nxy or (done % 400 == 0):
                dt = max(0.001, time.monotonic() - t0)
                print("[tile] cols %d..%d  %d/%d  %.0f tiles/s"
                      % (tx0, tx1, done, nxy, done / dt),
                      file=sys.stderr, flush=True)
    return tiles


def default_tile_jobs():
    env = os.environ.get("VMAP_JOBS")
    if env:
        try:
            return max(1, int(env))
        except ValueError:
            pass
    return max(1, os.cpu_count() or 1)


def build_tiles(features, labels, z, west, south, east, north, extent, jobs=1):
    from shapely.geometry import LineString, Polygon
    from shapely.strtree import STRtree

    tx_min, ty_max = lonlat_to_tile(west, south, z)
    tx_max, ty_min = lonlat_to_tile(east, north, z)
    if tx_min > tx_max:
        tx_min, tx_max = tx_max, tx_min
    if ty_min > ty_max:
        ty_min, ty_max = ty_max, ty_min

    pad_px = _TILE_CLIP_PAD_PX
    pad_u = int(round(pad_px * extent / 256.0))

    geoms = []
    for f in features:
        try:
            if f["closed"]:
                if len(f["coords"]) < 3:
                    continue
                holes = f.get("holes") or []
                g = Polygon(f["coords"], holes) if holes else Polygon(f["coords"])
                if not g.is_valid:
                    g = g.buffer(0)
            else:
                g = LineString(f["coords"])
        except Exception:
            continue
        if g.is_empty:
            continue
        geoms.append((f, g))

    total = (tx_max - tx_min + 1) * (ty_max - ty_min + 1)
    ncols = tx_max - tx_min + 1
    jobs = max(1, int(jobs))
    if jobs > ncols:
        jobs = ncols
    try:
        ctx = multiprocessing.get_context("fork")
    except ValueError:
        ctx = None
        jobs = 1
    tree = STRtree([g for _f, g in geoms]) if geoms else None
    print("[tile] z=%d  x:[%d..%d] y:[%d..%d]  (%d tiles)  pad=%dpx/%du  jobs=%d"
          % (z, tx_min, tx_max, ty_min, ty_max, total, pad_px, pad_u, jobs),
          file=sys.stderr, flush=True)

    global _TILE_WORKER
    _TILE_WORKER = (geoms, tree, z, extent, pad_px, pad_u)
    t0 = time.monotonic()
    if jobs <= 1 or ctx is None or ncols <= 1:
        tiles = _clip_tiles_range(tx_min, tx_max, ty_min, ty_max)
    else:
        chunk = max(1, (ncols + jobs - 1) // jobs)
        ranges = []
        for t0x in range(tx_min, tx_max + 1, chunk):
            ranges.append((t0x, min(tx_max, t0x + chunk - 1), ty_min, ty_max))
        with ctx.Pool(processes=min(jobs, len(ranges))) as pool:
            parts = pool.starmap(_clip_tiles_range, ranges)
        tiles = {}
        for part in parts:
            tiles.update(part)
    print("[tile] clip done in %.1fs" % (time.monotonic() - t0),
          file=sys.stderr, flush=True)

    nlab = 0
    for (lon, lat, kind, prio, name) in labels:
        tx, ty = lonlat_to_tile(lon, lat, z)
        if not (tx_min <= tx <= tx_max and ty_min <= ty <= ty_max):
            continue
        lx, ly = _to_local(tx, ty, lon, lat, z, extent)
        lx = min(extent - 1, max(0, int(round(lx))))
        ly = min(extent - 1, max(0, int(round(ly))))
        nb = name.encode("utf-8")[:255]
        tiles.setdefault((tx, ty), {}).setdefault("labels", []).append(
            (kind & 0xFF, prio & 0xFF, lx, ly, nb))
        nlab += 1
    print("[tile] %d labels placed" % nlab, file=sys.stderr)

    return tiles, z


# ---------------------------------------------------------------------------
# Serialize one tile to ".vt" bytes
# ---------------------------------------------------------------------------
def serialize_tile(tx, ty, z, layers, extent):
    # layers: {layerId(int): [(attr, closed, [(x,y),...]), ...],
    #          "labels": [(kind, prio, x, y, name_bytes), ...]}
    geom_ids = sorted(k for k in layers.keys() if isinstance(k, int))
    label_list = layers.get("labels")
    total_layers = len(geom_ids) + (1 if label_list else 0)

    out = bytearray()
    out += VMAP_MAGIC
    out += struct.pack("<BBH", VMAP_VERSION, z & 0xFF, extent)
    out += struct.pack("<IIHH", tx, ty, total_layers, 0)

    for lid in geom_ids:
        feats = layers[lid]
        blob = bytearray()
        for feat in feats:
            if len(feat) == 4:
                attr, closed, pts, rname = feat
            else:
                attr, closed, pts = feat
                rname = None
            flags = FLAG_CLOSED if closed else 0
            nb = b""
            if rname:
                flags |= FLAG_NAMED
                nb = rname.encode("utf-8")[:LABEL_MAXLEN]
            blob += struct.pack("<BBH", attr & 0xFF, flags, len(pts))
            for (x, y) in pts:
                blob += struct.pack("<HH", x & 0xFFFF, y & 0xFFFF)
            if nb:
                blob += struct.pack("<B", len(nb)) + nb
        out += struct.pack("<BBHI", lid & 0xFF, 0, len(feats) & 0xFFFF, len(blob))
        out += blob

    if label_list:
        blob = bytearray()
        for (kind, prio, x, y, nb) in label_list:
            blob += struct.pack("<BBHHBB", kind, prio, x & 0xFFFF, y & 0xFFFF,
                                len(nb), 0)
            blob += nb
        out += struct.pack("<BBHI", LAYER_LABEL, 0,
                           len(label_list) & 0xFFFF, len(blob))
        out += blob

    return bytes(out)


MAX_VT_BYTES = 128 * 1024  # RIDX v2 tile-size field is uint32; MCU cap
OLD_U16_MAX = 65535


def _drop_layer(layers, lid):
    return {k: v for k, v in layers.items() if k != lid}


def _drop_minor_roads(layers):
    roads = layers.get(LAYER_ROAD)
    if not roads:
        return layers
    kept = [f for f in roads if f[0] < ROAD_SERVICE]
    out = dict(layers)
    if kept:
        out[LAYER_ROAD] = kept
    else:
        out.pop(LAYER_ROAD, None)
    return out


def _strip_road_names(layers):
    roads = layers.get(LAYER_ROAD)
    if not roads:
        return layers
    out = dict(layers)
    out[LAYER_ROAD] = [
        (f[0], f[1], f[2]) if len(f) == 4 else f for f in roads
    ]
    return out


def _roads_primary_only(layers):
    roads = layers.get(LAYER_ROAD)
    if not roads:
        return layers
    kept = [f for f in roads if f[0] <= ROAD_SECONDARY]
    out = dict(layers)
    if kept:
        out[LAYER_ROAD] = kept
    else:
        out.pop(LAYER_ROAD, None)
    return out


def serialize_tile_fit(tx, ty, z, layers, extent, max_bytes=MAX_VT_BYTES):
    """Keep a tile within the on-device size cap (128 KiB)."""
    data = serialize_tile(tx, ty, z, layers, extent)
    if len(data) <= max_bytes:
        if len(data) > OLD_U16_MAX:
            return data, "over-64k"
        return data, None
    cur = layers
    for name, fn in (
        ("land", lambda L: _drop_layer(L, LAYER_LAND)),
        ("forest", lambda L: _drop_layer(L, LAYER_FOREST)),
        ("service-roads", _drop_minor_roads),
        ("road-names", _strip_road_names),
        ("primary-roads", _roads_primary_only),
        ("labels", lambda L: {k: v for k, v in L.items() if k != "labels"}),
    ):
        cur = fn(cur)
        data = serialize_tile(tx, ty, z, cur, extent)
        if len(data) <= max_bytes:
            return data, name
    water = {}
    if LAYER_WATER in cur:
        water[LAYER_WATER] = cur[LAYER_WATER]
    data = serialize_tile(tx, ty, z, water, extent)
    if len(data) <= max_bytes:
        return data, "water-only"
    data = serialize_tile(tx, ty, z, {}, extent)
    if len(data) > max_bytes:
        raise ValueError(
            "tile z=%d x=%d y=%d still %d B after emptying (max %d)"
            % (z, tx, ty, len(data), max_bytes))
    return data, "empty"


def write_tiles_loose(tiles, z, out_dir, extent):
    count = 0
    shrunk = 0
    over64 = 0
    for (tx, ty), layers in tiles.items():
        data, how = serialize_tile_fit(tx, ty, z, layers, extent)
        if how == "over-64k":
            over64 += 1
        elif how:
            shrunk += 1
        d = os.path.join(out_dir, str(z), str(tx))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "%d.vt" % ty), "wb") as fp:
            fp.write(data)
        count += 1
    print("[write] %d loose .vt -> %s/%d/" % (count, out_dir, z), file=sys.stderr)
    if shrunk:
        print("[write] %d tiles reduced to fit %d B cap" % (shrunk, MAX_VT_BYTES),
              file=sys.stderr)
    if over64:
        print("[write] %d tiles between 64KiB and 128KiB" % over64, file=sys.stderr)
    return count


def write_tiles_packed(tiles, z, out_dir, extent, pack_bytes):
    """One (or few) .vpk per zoom — avoids hundreds of tiny files on copy."""
    from pathlib import Path

    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import vmap_paths

    tools = str(vmap_paths.tools_dir())
    if tools not in sys.path:
        sys.path.insert(0, tools)
    from pack_vmap import TileBlob, pack_zoom

    blobs = []
    total = 0
    shrunk = 0
    over64 = 0
    for (tx, ty), layers in tiles.items():
        data, how = serialize_tile_fit(tx, ty, z, layers, extent)
        if how == "over-64k":
            over64 += 1
        elif how:
            shrunk += 1
        blobs.append(TileBlob(z=z, x=tx, y=ty, data=data))
        total += len(data)
    if shrunk:
        print("[write] %d/%d tiles reduced to fit %d B cap"
              % (shrunk, len(blobs), MAX_VT_BYTES), file=sys.stderr)
    if over64:
        print("[write] %d/%d tiles between 64KiB and 128KiB"
              % (over64, len(blobs)), file=sys.stderr)
    if not blobs:
        print("[write] z=%d empty, skip pack" % z, file=sys.stderr)
        return 0
    dst = Path(out_dir) / str(z)
    if dst.exists():
        import shutil
        shutil.rmtree(dst)
    pack_zoom(blobs, dst, pack_bytes)
    print("[write] %d tiles (%d KB) packed -> %s/"
          % (len(blobs), (total + 1023) // 1024, dst), file=sys.stderr)
    return len(blobs)


def write_tiles(tiles, z, out_dir, extent, packed=True, pack_bytes=8 * 1024 * 1024):
    if packed:
        return write_tiles_packed(tiles, z, out_dir, extent, pack_bytes)
    return write_tiles_loose(tiles, z, out_dir, extent)


# ---------------------------------------------------------------------------
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.normpath(os.path.join(here, "vmap"))

    ap = argparse.ArgumentParser(description="Build .vt vector tiles from OSM.")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--center-gcj", metavar="LON,LAT",
                   help="center in GCJ02 (高德/腾讯), converted to WGS84")
    g.add_argument("--center-wgs", metavar="LON,LAT",
                   help="center in WGS84")
    g.add_argument("--bbox", metavar="W,S,E,N",
                   help="explicit WGS84 bbox west,south,east,north "
                        "(overrides --center-*/--radius-deg)")
    ap.add_argument("--zoom", type=int, default=15)
    ap.add_argument("--radius-deg", type=float, default=0.02,
                    help="half-size of the bbox in degrees (default 0.02 ~ 2km)")
    ap.add_argument("--out", default=default_out, help="output dir for vmap/")
    ap.add_argument("--loose", action="store_true",
                    help="write one tiny .vt per tile (web tile cache only). "
                         "Default is packed <z>/tiles.idx + p000.vpk")
    ap.add_argument("--pack-bytes", type=int, default=8 * 1024 * 1024,
                    help="max payload per packed .vpk (default 8 MiB)")
    ap.add_argument("--osm", help="use a local .osm/.pbf instead of Overpass")
    ap.add_argument("--endpoint",
                    default="https://overpass-api.de/api/interpreter")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--keep-osm", help="save fetched OSM XML to this path")
    ap.add_argument(
        "--chars-out",
        help="write OSM name charset after --zh conversion (default: <out>/osm_chars.txt)",
    )
    ap.add_argument(
        "--chars-only",
        action="store_true",
        help="scan OSM names / write charset, skip tile output",
    )
    ap.add_argument("--jobs", type=int, default=default_tile_jobs(),
                    help="tile worker processes (default: CPU count or $VMAP_JOBS)")
    zh_simplify.add_zh_argument(ap)
    args = ap.parse_args()
    zh_simplify.set_script(args.zh)

    osm_bounds = False
    if args.bbox:
        west, south, east, north = (float(v) for v in args.bbox.split(","))
        if west > east:
            west, east = east, west
        if south > north:
            south, north = north, south
        print("[bbox] WGS84 W=%.6f S=%.6f E=%.6f N=%.6f"
              % (west, south, east, north), file=sys.stderr)
    elif args.osm and not args.center_wgs and not args.center_gcj:
        west = south = east = north = None
        osm_bounds = True
    else:
        if args.center_wgs:
            lon, lat = (float(v) for v in args.center_wgs.split(","))
        elif args.center_gcj:
            glon, glat = (float(v) for v in args.center_gcj.split(","))
            lon, lat = gcj02_to_wgs84(glon, glat)
        else:
            lon, lat = gcj02_to_wgs84(DEFAULT_GCJ_LON, DEFAULT_GCJ_LAT)

        print("[center] WGS84 lon=%.6f lat=%.6f" % (lon, lat), file=sys.stderr)
        r = args.radius_deg
        west, east = lon - r, lon + r
        # latitude bbox a bit smaller since the view is taller than wide is fine
        south, north = lat - r * 0.8, lat + r * 0.8

    if args.osm:
        osm_path = args.osm
        tmp = None
    else:
        data = fetch_overpass(west, south, east, north, args.endpoint, args.timeout)
        if args.keep_osm:
            with open(args.keep_osm, "wb") as fp:
                fp.write(data)
            osm_path = args.keep_osm
            tmp = None
        else:
            tmp = tempfile.NamedTemporaryFile(suffix=".osm", delete=False)
            tmp.write(data)
            tmp.close()
            osm_path = tmp.name

    try:
        features, labels, name_scan = parse_osm(osm_path)
        if osm_bounds:
            b = geo_bounds(features, labels)
            if not b:
                print("[error] OSM has no drawable geometry", file=sys.stderr)
                return 1
            west, south, east, north = b
            print("[bbox] from OSM W=%.6f S=%.6f E=%.6f N=%.6f"
                  % (west, south, east, north), file=sys.stderr)
        chars_path = args.chars_out or os.path.join(args.out, "osm_chars.txt")
        zh_simplify.write_chars_file(chars_path, name_scan)
        if args.chars_only:
            return
        tiles, z = build_tiles(features, labels, args.zoom,
            west, south, east, north, VMAP_EXTENT, jobs=max(1, args.jobs))
        n = write_tiles(tiles, z, args.out, VMAP_EXTENT,
            packed=not args.loose, pack_bytes=args.pack_bytes)
        if n == 0:
            print("[warn] no tiles written — empty area or bad bbox?", file=sys.stderr)
    finally:
        if not args.osm and not args.keep_osm and tmp:
            os.unlink(tmp.name)


if __name__ == "__main__":
    main()
