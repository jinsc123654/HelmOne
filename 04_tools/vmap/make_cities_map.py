#!/usr/bin/env python3
"""Build Chuzhou / Nanjing / Guangzhou into one national-grid pack.

Extract + vmap + graph run in parallel across cities. pack_map --merge is
serial (one writer for lon*/lat*/x*_y*.vpk). Tile clipping inside each city uses
build_vmap.py --jobs.

  ./make_cities_map.sh --only guangzhou --reuse
  ./make_cities_map.sh --only chuzhou guangzhou --reuse --rebuild-graph --clean
  ./make_cities_map.sh --jobs 3
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vmap_paths  # noqa: E402

TOOLS = vmap_paths.tools_dir()
VENV_PY = HERE / ".venv" / "bin" / "python"
PACK_MAP = TOOLS / "pack_map.py"
CHINA_PBF = Path(os.environ.get(
    "VMAP_CHINA_PBF", "/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf"))
DATA_ROOT = Path(os.environ.get(
    "VMAP_CITIES_DATA", "/home/jinsc/SDK/vela/osm_data/cities"))
MAP_OUT = Path(os.environ.get(
    "VMAP_CITIES_OUT", "/home/jinsc/SDK/vela/osm_data/map"))
DEM_CACHE = HERE / "cache" / "dem"

CITY_ORDER = ("chuzhou", "nanjing", "guangzhou")
# Prefecture extents W,S,E,N (slightly padded).
CITY_BBOX = {
    "chuzhou": "117.13,31.83,119.24,33.24",
    "nanjing": "118.35,31.21,119.25,32.64",
    "guangzhou": "112.93,22.41,114.08,23.96",
}


def run(argv, cwd=None) -> int:
    print("[city]", " ".join(str(a) for a in argv), file=sys.stderr, flush=True)
    return subprocess.call([str(a) for a in argv], cwd=cwd)


def build_city(city: str, bbox: str, zh: str, tile_jobs: int) -> dict:
    work = DATA_ROOT / city
    work.mkdir(parents=True, exist_ok=True)
    pbf = work / "extract.osm.pbf"
    vmap = work / "vmap"
    graph = work / "graph.vgrf"
    print("[city] %s extract bbox=%s" % (city, bbox), file=sys.stderr, flush=True)
    osmium = shutil.which("osmium")
    rc = run([osmium, "extract", "--bbox", bbox, "--set-bounds", "--overwrite",
              "-s", "complete_ways", "-o", str(pbf), str(CHINA_PBF)])
    if rc != 0:
        return {"city": city, "ok": False, "err": "extract"}
    print("[city] %s extract %s" % (city, _size(pbf)),
          file=sys.stderr, flush=True)

    if vmap.exists():
        shutil.rmtree(vmap)
    rc = run([str(VENV_PY), str(HERE / "build_vmap.py"),
              "--osm", str(pbf), "--bbox", bbox, "--zoom", "14",
              "--zh", zh, "--jobs", str(tile_jobs), "--out", str(vmap)])
    if rc != 0:
        return {"city": city, "ok": False, "err": "vmap"}

    rc = run([str(VENV_PY), str(HERE / "build_vgraph.py"),
              "--osm", str(pbf), "--zh", zh,
              "--dem-cache", str(DEM_CACHE),
              "--out", str(graph)])
    if rc != 0 or not graph.is_file():
        return {"city": city, "ok": False, "err": "graph"}
    print("[city] %s built" % city, file=sys.stderr, flush=True)
    return {
        "city": city, "ok": True, "vmap": vmap, "graph": graph, "bbox": bbox,
    }


def _size(path: Path) -> str:
    n = path.stat().st_size
    if n >= 1 << 20:
        return "%.0fM" % (n / 1048576.0)
    return "%.0fK" % (n / 1024.0)


def graph_has_v4_elev(city: str) -> bool:
    graph = DATA_ROOT / city / "graph.vgrf"
    if not graph.is_file() or graph.stat().st_size < 64:
        return False
    with graph.open("rb") as fp:
        magic = fp.read(5)
        if magic[:4] != b"VGRF" or magic[4] < 4:
            return False
        fp.seek(0)
        data = fp.read()
    return b"ELEV" in data


def rebuild_city_graph(city: str, zh: str) -> int:
    work = DATA_ROOT / city
    pbf = work / "extract.osm.pbf"
    graph = work / "graph.vgrf"
    tmp = work / "graph.vgrf.tmp"
    print("[city] %s rebuild graph with DEM" % city, file=sys.stderr, flush=True)
    rc = run([str(VENV_PY), str(HERE / "build_vgraph.py"),
              "--osm", str(pbf), "--zh", zh,
              "--dem-cache", str(DEM_CACHE),
              "--out", str(tmp)])
    if rc == 0 and tmp.is_file():
        tmp.replace(graph)
        print("[city] %s graph %s" % (city, _size(graph)),
              file=sys.stderr, flush=True)
        return 0
    tmp.unlink(missing_ok=True)
    return rc if rc != 0 else 1


def city_ready(city: str) -> bool:
    work = DATA_ROOT / city
    vmap = work / "vmap"
    graph = work / "graph.vgrf"
    pbf = work / "extract.osm.pbf"
    if not pbf.is_file() or not graph.is_file() or not vmap.is_dir():
        return False
    return any(vmap.glob("*/tiles.idx")) or any(vmap.rglob("*.vt"))


def pack_city(info: dict, clean: bool) -> int:
    argv = [str(VENV_PY), str(PACK_MAP),
            "-i", str(info["vmap"]), "--graph", str(info["graph"]),
            "-o", str(MAP_OUT), "--region-km", "3", "--grid-global",
            "--subdir", "--skip-empty-graph", "--no-portals", "--max-zoom", "0"]
    argv.append("--clean" if clean else "--merge")
    rc = run(argv)
    if rc == 0:
        print("[city] %s packed" % info["city"], file=sys.stderr, flush=True)
    return rc


def map_out_is_lonlat_grid() -> bool:
    return MAP_OUT.is_dir() and any(MAP_OUT.glob("lon*/lat*/x*_y*.vpk"))


def main():
    cpu = os.cpu_count() or 1
    ap = argparse.ArgumentParser(description="Build city maps into osm_data/map.")
    ap.add_argument("--zh", choices=("hans", "hant"), default="hans")
    ap.add_argument("--only", nargs="+", choices=CITY_ORDER,
                    help="subset of cities (order follows chuzhou, nanjing, guangzhou)")
    ap.add_argument("--jobs", type=int, default=0,
                    help="cities in parallel (0 = all selected, capped by CPU)")
    ap.add_argument("--reuse", action="store_true",
                    help="Reuse existing extract/vmap/graph if present")
    ap.add_argument("--rebuild-graph", action="store_true",
                    help="Rebuild graph.vgrf even with --reuse (keep tiles)")
    ap.add_argument("--clean", action="store_true",
                    help="Wipe osm_data/map before the first city pack")
    args = ap.parse_args()

    if args.only:
        selected = set(args.only)
        cities = [c for c in CITY_ORDER if c in selected]
    else:
        cities = list(CITY_ORDER)
    region_jobs = args.jobs if args.jobs > 0 else min(len(cities), cpu)
    region_jobs = max(1, min(region_jobs, len(cities)))
    tile_jobs = max(1, cpu // region_jobs)
    print("[city] cities=%s  region-jobs=%d tile-jobs=%d zh=%s"
          % (",".join(cities), region_jobs, tile_jobs, args.zh),
          file=sys.stderr, flush=True)

    if not CHINA_PBF.is_file():
        raise SystemExit("missing %s" % CHINA_PBF)
    if not shutil.which("osmium"):
        raise SystemExit("need osmium CLI (apt install osmium-tool)")
    if not VENV_PY.is_file():
        raise SystemExit("need %s" % VENV_PY)
    if not PACK_MAP.is_file():
        raise SystemExit("need %s" % PACK_MAP)

    DATA_ROOT.mkdir(parents=True, exist_ok=True)
    MAP_OUT.mkdir(parents=True, exist_ok=True)

    built: dict[str, dict] = {}
    if region_jobs <= 1:
        for city in cities:
            if args.reuse and city_ready(city):
                print("[city] %s reuse existing extract/vmap%s"
                      % (city, "" if args.rebuild_graph else "/graph"),
                      file=sys.stderr, flush=True)
                if args.rebuild_graph:
                    if graph_has_v4_elev(city):
                        print("[city] %s graph already v4+ELEV, skip rebuild"
                              % city, file=sys.stderr, flush=True)
                    else:
                        rc = rebuild_city_graph(city, args.zh)
                        if rc != 0:
                            raise SystemExit("%s graph rebuild failed rc=%d"
                                             % (city, rc))
                work = DATA_ROOT / city
                info = {
                    "city": city, "ok": True,
                    "vmap": work / "vmap", "graph": work / "graph.vgrf",
                    "bbox": CITY_BBOX[city],
                }
            else:
                info = build_city(city, CITY_BBOX[city], args.zh, tile_jobs)
            built[city] = info
            if not info["ok"]:
                raise SystemExit("%s failed (%s)" % (city, info.get("err")))
    else:
        with ThreadPoolExecutor(max_workers=region_jobs) as ex:
            futs = {}
            for city in cities:
                if args.reuse and city_ready(city):
                    if args.rebuild_graph:
                        if graph_has_v4_elev(city):
                            print("[city] %s graph already v4+ELEV, skip rebuild"
                                  % city, file=sys.stderr, flush=True)
                        else:
                            rc = rebuild_city_graph(city, args.zh)
                            if rc != 0:
                                raise SystemExit("%s graph rebuild failed rc=%d"
                                                 % (city, rc))
                    work = DATA_ROOT / city
                    built[city] = {
                        "city": city, "ok": True,
                        "vmap": work / "vmap", "graph": work / "graph.vgrf",
                        "bbox": CITY_BBOX[city],
                    }
                    print("[city] %s reuse existing extract/vmap%s"
                          % (city, "" if args.rebuild_graph else "/graph"),
                          file=sys.stderr, flush=True)
                    continue
                futs[ex.submit(build_city, city, CITY_BBOX[city], args.zh,
                               tile_jobs)] = city
            for fut in as_completed(futs):
                info = fut.result()
                built[info["city"]] = info
                if not info["ok"]:
                    raise SystemExit("%s failed (%s)"
                                     % (info["city"], info.get("err")))

    first = True
    for city in cities:
        info = built[city]
        wipe = bool(args.clean) and first
        if first and not wipe and not map_out_is_lonlat_grid():
            wipe = True
            print("[city] wiping map/ (old layout or empty) for lon/lat grid",
                  file=sys.stderr, flush=True)
        rc = pack_city(info, clean=wipe)
        if rc != 0:
            raise SystemExit("pack %s failed rc=%d" % (city, rc))
        first = False

    print("[city] rebuild portals -> %s" % MAP_OUT, file=sys.stderr, flush=True)
    rc = run([str(VENV_PY), str(PACK_MAP), "-o", str(MAP_OUT),
              "--rebuild-portals"])
    if rc != 0:
        raise SystemExit("portal rebuild failed rc=%d" % rc)
    print("[done] %s  zh=%s  cities=%s" % (MAP_OUT, args.zh, " ".join(cities)),
          file=sys.stderr)


if __name__ == "__main__":
    main()
