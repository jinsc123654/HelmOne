#!/usr/bin/env python3
"""Nationwide China map, same pipeline as Chuzhou / Guangzhou.

  china-latest.osm.pbf
      │  osmium extract  (1° cells, padded; resumable)
      ▼
  china_1deg/NxxEyyy.osm.pbf
      │  build_vmap  +  build_vgraph(--dem-cache)  +  pack_map
      │  3 km national grid → lon*/lat*/x*_y*.vpk
      ▼
  map_china/   (or VMAP_CHINA_MAP)

Default is a streaming pipeline: split / vmap / graph+DEM / pack overlap
with bounded queues (backpressure). Always leave ~20% RAM (or 3 GiB).
build_vmap cannot load the national PBF.

  ./make_china_map.sh                 # stream: cut || tile || graph || pack
  ./make_china_map.sh --split-only
  ./make_china_map.sh --pack-only --jobs 2
  ./make_china_map.sh --only N32E118
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from collections import deque
from pathlib import Path
from queue import Queue

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vmap_paths  # noqa: E402

TOOLS = vmap_paths.tools_dir()

# Keep in sync with pack_map.py / make_cities_map.py (do not import pack_map:
# tools/ also has a build_vmap.py that would shadow this dir's build_vmap.py).
DEFAULT_REGION_KM = 3.0
GRID_GLOBAL_ANCHOR_LAT = 35.0
GRID_GLOBAL_ORIGIN_LON = 0.0
GRID_GLOBAL_ORIGIN_LAT = 0.0
# Firmware 3 km cell at 35°N (vmap_format.h / pack_map.py).
GRID_CELL_LAT_3KM = 0.026949335249730506
GRID_CELL_LON_3KM = 0.032899063849730509

CHINA_PBF = Path(os.environ.get(
    "VMAP_CHINA_PBF", "/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf"))
# Core land bbox. Extracts include a 1° ring so 3 km cells whose center sits
# just outside an integer-degree line still have a complete owning extract.
CHINA_WEST, CHINA_SOUTH, CHINA_EAST, CHINA_NORTH = 73.0, 18.0, 135.0, 54.0
SPLIT_WEST, SPLIT_SOUTH = int(CHINA_WEST) - 1, int(CHINA_SOUTH) - 1
SPLIT_EAST, SPLIT_NORTH = int(CHINA_EAST), int(CHINA_NORTH)
SPLIT_DIR = Path(os.environ.get(
    "VMAP_CHINA_1DEG", "/home/jinsc/SDK/vela/osm_data/china_1deg"))
OUT_MAP = Path(os.environ.get(
    "VMAP_CHINA_MAP", "/home/jinsc/SDK/vela/osm_data/map_china"))
DEM_CACHE = Path(os.environ.get(
    "VMAP_DEM_CACHE", str(HERE / "cache" / "dem")))
VENV_PY = HERE / ".venv" / "bin" / "python"
PACK_MAP = TOOLS / "pack_map.py"
# osmium complete_ways holds an index per extract; wide batches OOM
# (last nationwide run died at batch=80; 40 still blows mid-range boxes).
EXTRACT_BATCH = 16
# ~3 km cell is ~0.03°; pad past one cell so --keep-bbox owners are complete.
EXTRACT_PAD = 0.22
MIN_PBF_BYTES = 2048
# Dense urban 1° (vmap ProcessPool + graph + pack). 1.3 GiB was far too low.
WORKER_GIB = 4.0
SPLIT_GIB = 2.5
RESERVE_GIB = 3.0
RESERVE_FRAC = 0.20
# Soft ceiling so a 64-core box does not start 32 packers (disk thrash).
# Real limit is MemAvailable after reserve. Override with VMAP_CHINA_MAX_JOBS.
AUTO_JOBS_HARD_MAX = 12
MEM_WAIT_S = 8.0
MEM_WAIT_TIMEOUT_S = 90.0

PROVINCES = {
    "anhui": (114.88, 29.40, 119.65, 34.66),
    "jiangsu": (116.30, 30.75, 121.96, 35.14),
    "guangdong": (109.65, 20.13, 117.32, 25.52),
}
PROVINCE_ALIAS = {
    "安徽": "anhui",
    "安徽省": "anhui",
    "江苏": "jiangsu",
    "江苏省": "jiangsu",
    "广东": "guangdong",
    "广东省": "guangdong",
}


_QUIET = False


def run(argv, cwd=None) -> int:
    if not _QUIET:
        print("[china]", " ".join(str(a) for a in argv), file=sys.stderr, flush=True)
    return subprocess.call([str(a) for a in argv], cwd=cwd)


def run_logged(argv, log_path: Path) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as fp:
        fp.write(" ".join(str(a) for a in argv) + "\n")
        fp.flush()
        return subprocess.call(
            [str(a) for a in argv], stdout=fp, stderr=subprocess.STDOUT)


def _meminfo_gib(key: str) -> float:
    try:
        with open("/proc/meminfo", encoding="utf-8") as fp:
            for line in fp:
                if line.startswith(key):
                    return int(line.split()[1]) / (1024.0 * 1024.0)
    except OSError:
        return 0.0
    return 0.0


def avail_mem_gib() -> float:
    return _meminfo_gib("MemAvailable:")


def total_mem_gib() -> float:
    return _meminfo_gib("MemTotal:")


def reserve_gib() -> float:
    total = total_mem_gib()
    return max(RESERVE_GIB, total * RESERVE_FRAC if total else 0.0)


def free_for_work_gib() -> float:
    return avail_mem_gib() - reserve_gib()


def wait_for_work_mem(need_work_gib: float, what: str,
                      timeout_s: float = MEM_WAIT_TIMEOUT_S) -> float:
    """Wait until avail-reserve >= need. After timeout, continue anyway."""
    t0 = time.time()
    while True:
        avail = avail_mem_gib()
        leave = reserve_gib()
        free = avail - leave
        if free >= need_work_gib or time.time() - t0 >= timeout_s:
            if free < need_work_gib and not _QUIET:
                print("[china] mem tight (avail=%.1f leave=%.1f need=%.1f) "
                      "continuing %s with jobs=1 / smaller batch"
                      % (avail, leave, need_work_gib, what),
                      file=sys.stderr, flush=True)
            return avail
        if not _QUIET:
            print("[china] wait mem for %s: avail=%.1fGiB leave=%.1f need=%.1f"
                  % (what, avail, leave, need_work_gib),
                  file=sys.stderr, flush=True)
        time.sleep(MEM_WAIT_S)


def auto_jobs_cap() -> int:
    env = os.environ.get("VMAP_CHINA_MAX_JOBS")
    if env:
        return max(1, int(env))
    return max(1, min(os.cpu_count() or 1, AUTO_JOBS_HARD_MAX))


def extract_batch_now(wanted: int) -> int:
    """Pick extract width from free RAM; `wanted` is the max this run."""
    free = free_for_work_gib()
    if free >= 16.0:
        n = 16
    elif free >= 10.0:
        n = 12
    elif free >= 6.0:
        n = 8
    elif free >= 3.5:
        n = 6
    else:
        n = 4
    return max(1, min(wanted, n))


def pack_jobs_now(n_cells: int, jobs_cap: int) -> int:
    """jobs = min(CPU, cap, floor((MemAvailable - reserve) / 4GiB))."""
    cap = max(1, jobs_cap)
    free = free_for_work_gib()
    by_mem = int(free / WORKER_GIB) if free >= WORKER_GIB else 1
    return max(1, min(cap, by_mem, n_cells, os.cpu_count() or 1))


def default_region_jobs(n_cells: int = 0) -> int:
    jobs = pack_jobs_now(n_cells or 1, auto_jobs_cap())
    if n_cells > 0:
        jobs = min(jobs, n_cells)
    return jobs


def default_tile_jobs(region_jobs: int) -> int:
    """Share cores among concurrent 1° cells. Cap forks of the OSM tree."""
    cpu = os.cpu_count() or 1
    share = max(1, cpu // max(1, region_jobs))
    cap = 6 if region_jobs <= 2 else 3
    return max(1, min(cap, share))


def grid_cell_deg(lat_ref: float, region_km: float) -> tuple[float, float]:
    if (abs(region_km - 3.0) < 1e-9
            and abs(lat_ref - GRID_GLOBAL_ANCHOR_LAT) < 1e-9):
        return GRID_CELL_LON_3KM, GRID_CELL_LAT_3KM
    cell_lat = region_km / 111.32
    cell_lon = region_km / (111.32 * max(0.2, math.cos(math.radians(lat_ref))))
    return cell_lon, cell_lat


def deg_cells():
    for lat in range(SPLIT_SOUTH, SPLIT_NORTH):
        for lon in range(SPLIT_WEST, SPLIT_EAST):
            yield lat, lon


def deg_name(lat: int, lon: int) -> str:
    ns = "N" if lat >= 0 else "S"
    ew = "E" if lon >= 0 else "W"
    return "%s%02d%s%03d" % (ns, abs(lat), ew, abs(lon))


def parse_only(text: str) -> tuple[int, int]:
    name = text.strip().upper().replace("_", "")
    if len(name) != 7 or name[0] not in "NS" or name[3] not in "EW":
        raise SystemExit(" --only expects like N32E118")
    lat = int(name[1:3])
    lon = int(name[4:7])
    if name[0] == "S":
        lat = -lat
    if name[3] == "W":
        lon = -lon
    return lat, lon


def parse_bbox(text: str) -> tuple[float, float, float, float]:
    parts = [p.strip() for p in text.split(",")]
    if len(parts) != 4:
        raise SystemExit(" --bbox expects W,S,E,N")
    west, south, east, north = (float(p) for p in parts)
    if west >= east or south >= north:
        raise SystemExit(" --bbox empty: %s" % text)
    return west, south, east, north


def resolve_province(name: str) -> str:
    key = PROVINCE_ALIAS.get(name.strip(), name.strip().lower())
    if key not in PROVINCES:
        known = ", ".join(sorted(PROVINCES))
        raise SystemExit(" unknown province %r (have %s)" % (name, known))
    return key


def cell_hits_bbox(lat: int, lon: int,
                   bbox: tuple[float, float, float, float]) -> bool:
    west, south, east, north = bbox
    return lon < east and (lon + 1) > west and lat < north and (lat + 1) > south


def cells_for_bboxes(bboxes: list[tuple[float, float, float, float]]):
    seen = set()
    out = []
    for lat, lon in deg_cells():
        if any(cell_hits_bbox(lat, lon, b) for b in bboxes):
            if (lat, lon) not in seen:
                seen.add((lat, lon))
                out.append((lat, lon))
    return out


def pbf_complete(path: Path) -> bool:
    """True if extract finished. 0-byte leftovers from a killed osmium are not."""
    try:
        return path.is_file() and path.stat().st_size > 0
    except OSError:
        return False


def split_1deg(pbf: Path, dest: Path, osmium: str, jobs: list[tuple[int, int]],
               batch_size: int) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    pending = []
    for lat, lon in jobs:
        out = dest / (deg_name(lat, lon) + ".osm.pbf")
        if pbf_complete(out):
            continue
        if out.is_file():
            out.unlink()
        part = out.with_suffix(out.suffix + ".part")
        if part.is_file():
            part.unlink()
        pending.append((lat, lon, out, part))
    if not _QUIET:
        print("[china] 1deg split: %d already, %d to extract"
              % (len(jobs) - len(pending), len(pending)),
              file=sys.stderr, flush=True)
    if not pending:
        return

    batch_size = max(1, batch_size)
    total = len(pending)
    done_n = 0
    while pending:
        wait_for_work_mem(SPLIT_GIB, "split")
        n = extract_batch_now(batch_size)
        chunk = pending[:n]
        pending = pending[n:]
        extracts = []
        for lat, lon, _out, part in chunk:
            extracts.append({
                "output": str(part),
                "output_format": "pbf",
                "bbox": [
                    lon - EXTRACT_PAD, lat - EXTRACT_PAD,
                    lon + 1 + EXTRACT_PAD, lat + 1 + EXTRACT_PAD,
                ],
            })
        cfg = {"extracts": extracts}
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fp:
            json.dump(cfg, fp)
            cfg_path = fp.name
        try:
            rc = run([osmium, "extract", "-c", cfg_path, "-s", "complete_ways",
                      "--overwrite", str(pbf)])
            if rc != 0:
                for _lat, _lon, _out, part in chunk:
                    if part.is_file():
                        part.unlink()
                raise SystemExit("osmium extract failed rc=%d" % rc)
            for _lat, _lon, out, part in chunk:
                if not pbf_complete(part):
                    if part.is_file():
                        part.unlink()
                    raise SystemExit("osmium produced empty %s" % part)
                part.replace(out)
        finally:
            os.unlink(cfg_path)
        done_n += len(chunk)
        if not _QUIET:
            print("[china] split %d/%d (batch %d, avail=%.1fGiB leave=%.1f)"
                  % (done_n, total, len(chunk), avail_mem_gib(), reserve_gib()),
                  file=sys.stderr, flush=True)


def has_tiles(vmap_dir: Path) -> bool:
    if not vmap_dir.is_dir():
        return False
    return any(vmap_dir.rglob("tiles.idx")) or any(vmap_dir.rglob("*.vt"))


def _cell_box(lat: int, lon: int) -> tuple[str, str]:
    keep = "%d,%d,%d,%d" % (lon, lat, lon + 1, lat + 1)
    bbox = "%f,%f,%f,%f" % (
        lon - EXTRACT_PAD, lat - EXTRACT_PAD,
        lon + 1 + EXTRACT_PAD, lat + 1 + EXTRACT_PAD)
    return keep, bbox


def _mark(pbf: Path, text: str) -> None:
    pbf.with_suffix(pbf.suffix + ".done").write_text(text, encoding="utf-8")


def stage_vmap(item: dict, zoom: int, zh: str, tile_jobs: int) -> str:
    work = Path(tempfile.mkdtemp(prefix="vmap_cn_"))
    item["work"] = work
    item["vmap"] = work / "vmap"
    item["graph"] = work / "graph.vgrf"
    item["keep"], item["bbox"] = _cell_box(item["lat"], item["lon"])
    rc = run_logged(
        [VENV_PY, HERE / "build_vmap.py",
         "--osm", item["pbf"], "--bbox", item["bbox"], "--zoom", str(zoom),
         "--zh", zh, "--jobs", str(tile_jobs), "--out", item["vmap"]],
        work / "vmap.log")
    if rc != 0:
        return "fail-vmap"
    if not has_tiles(item["vmap"]):
        _mark(item["pbf"], "empty-tiles\n")
        return "empty"
    return "ok"


def stage_graph(item: dict, zh: str, dem_jobs: int) -> str:
    DEM_CACHE.mkdir(parents=True, exist_ok=True)
    rc = run_logged(
        [VENV_PY, HERE / "build_vgraph.py",
         "--osm", item["pbf"], "--out", item["graph"], "--zh", zh,
         "--dem-cache", str(DEM_CACHE),
         "--dem-jobs", str(max(1, dem_jobs))],
        item["work"] / "graph.log")
    if rc != 0 or not item["graph"].is_file():
        _mark(item["pbf"], "empty-graph\n")
        return "empty"
    return "ok"


def stage_pack(item: dict, out_map: Path, region_km: float, pack_jobs: int) -> str:
    rc = run_logged(
        [VENV_PY, PACK_MAP, "-i", item["vmap"], "-o", out_map,
         "--graph", item["graph"],
         "--region-km", str(region_km),
         "--grid-global", "--subdir", "--merge", "--no-portals",
         "--skip-empty-graph",
         "--keep-bbox", item["keep"],
         "--max-zoom", "0",
         "--jobs", str(max(1, pack_jobs))],
        item["work"] / "pack.log")
    if rc != 0:
        return "fail-pack"
    _mark(item["pbf"], "ok\n")
    return "ok"


class Hud:
    LINES = 8

    def __init__(self):
        self.lock = threading.Lock()
        self.n_goal = 0
        self.split_todo = 0
        self.counts: dict[str, int] = {}
        self.run = {"split": [], "vmap": [], "graph": [], "pack": []}
        self.q = {"pbf": 0, "vmap": 0, "graph": 0}
        self.t0 = time.time()
        self._drawn = False
        self._tty = None
        self._last_log = 0.0
        try:
            self._tty = open("/dev/tty", "w", encoding="utf-8", buffering=1)
        except OSError:
            self._tty = None

    def close(self):
        if self._tty and self._drawn:
            self._tty.write("\033[?25h")
            self._tty.flush()
            self._tty.close()

    def begin(self, stage: str, name: str) -> None:
        with self.lock:
            self.run[stage].append(name)

    def end(self, stage: str, name: str) -> None:
        with self.lock:
            try:
                self.run[stage].remove(name)
            except ValueError:
                pass

    def set_split_todo(self, n: int) -> None:
        with self.lock:
            self.split_todo = n

    def add(self, key: str, n: int = 1) -> None:
        with self.lock:
            self.counts[key] = self.counts.get(key, 0) + n

    def set_q(self, **kw) -> None:
        with self.lock:
            self.q.update(kw)

    def snapshot(self) -> dict:
        with self.lock:
            return {
                "n_goal": self.n_goal,
                "split_todo": self.split_todo,
                "counts": dict(self.counts),
                "run": {k: list(v) for k, v in self.run.items()},
                "q": dict(self.q),
                "elapsed": time.time() - self.t0,
            }

    def render(self, force_log: bool = False) -> None:
        s = self.snapshot()
        done = sum(s["counts"].get(k, 0) for k in (
            "ok", "empty", "fail-vmap", "fail-pack", "fail-graph",
            "missing", "skip"))
        ok = s["counts"].get("ok", 0)
        empty = s["counts"].get("empty", 0)
        fail = sum(s["counts"].get(k, 0) for k in (
            "fail-vmap", "fail-pack", "fail-graph"))
        goal = max(1, s["n_goal"])
        rate = ok / (s["elapsed"] / 3600.0) if s["elapsed"] > 30 and ok else 0
        left = max(0, s["n_goal"] - done)
        eta = (left / rate) if rate > 0 else 0
        lines = [
            "全国流水线  %s  内存可用 %.1fGiB 留 %.1fGiB" % (
                _fmt_hms(s["elapsed"]), avail_mem_gib(), reserve_gib()),
            "切块  待抽 %d  队列PBF %d  %s" % (
                s["split_todo"], s["q"]["pbf"], _fmt_run(s["run"]["split"])),
            "切片  队列 %d  %s" % (s["q"]["vmap"], _fmt_run(s["run"]["vmap"])),
            "路网  队列 %d  %s" % (s["q"]["graph"], _fmt_run(s["run"]["graph"])),
            "打包  %s" % _fmt_run(s["run"]["pack"]),
            "进度  %s %d/%d" % (_bar(done, goal, 24), done, s["n_goal"]),
            "结果  ok=%d empty=%d fail=%d skip=%d  %.1f格/时  剩余 %s" % (
                ok, empty, fail, s["counts"].get("skip", 0),
                rate, _fmt_hms(eta * 3600) if eta else "--"),
            "协同  切块∥切片∥路网∥打包 同时转（队列满则反压）",
        ]
        if self._tty:
            if not self._drawn:
                self._tty.write("\033[?25l")
            else:
                self._tty.write("\033[%dA" % self.LINES)
            for line in lines:
                self._tty.write("\033[2K\r%s\n" % line[:120])
            self._tty.flush()
            self._drawn = True
        now = time.time()
        if force_log or now - self._last_log >= 15:
            self._last_log = now
            print("[hud] %s  ok=%d/%d  q=%s  run=%s" % (
                _fmt_hms(s["elapsed"]), ok, s["n_goal"], s["q"], s["run"]),
                  file=sys.stderr, flush=True)


def _fmt_run(names) -> str:
    if not names:
        return "idle"
    return ",".join(str(x) for x in names)


def _bar(n: int, d: int, w: int) -> str:
    if d <= 0:
        return "░" * w
    k = max(0, min(w, int(round(w * n / d))))
    return "█" * k + "░" * (w - k)


def _fmt_hms(sec: float) -> str:
    sec = int(max(0, sec))
    h, r = divmod(sec, 3600)
    m, s = divmod(r, 60)
    if h:
        return "%dh%02dm" % (h, m)
    return "%02d:%02d" % (m, s)


def select_jobs(args) -> tuple[list[tuple[int, int]], list[str]]:
    jobs = list(deg_cells())
    wanted: set[tuple[int, int]] = set()
    if args.only:
        wanted |= {parse_only(x) for x in args.only}
    bboxes = [parse_bbox(x) for x in args.bbox]
    provs = []
    for item in args.province:
        provs.extend(x.strip() for x in item.split(",") if x.strip())
    provs = [resolve_province(x) for x in provs]
    for key in provs:
        bboxes.append(PROVINCES[key])
    if bboxes:
        wanted |= set(cells_for_bboxes(bboxes))
    if wanted:
        jobs = [j for j in jobs if j in wanted]
        missing = wanted - set(jobs)
        if missing:
            raise SystemExit(" selection not in split grid: %s" % (missing,))
        if provs:
            def _rank(cell):
                lat, lon = cell
                for i, key in enumerate(provs):
                    if cell_hits_bbox(lat, lon, PROVINCES[key]):
                        return i
                return len(provs)
            jobs.sort(key=lambda c: (_rank(c), c[0], c[1]))
    return jobs, provs


def print_status(jobs: list[tuple[int, int]], split_dir: Path) -> None:
    n = len(jobs)
    have = done = empty = zero = missing = 0
    for lat, lon in jobs:
        pbf = split_dir / (deg_name(lat, lon) + ".osm.pbf")
        marker = pbf.with_suffix(pbf.suffix + ".done")
        if marker.is_file():
            done += 1
            try:
                body = marker.read_text(encoding="utf-8")
            except OSError:
                body = ""
            if body.startswith("empty"):
                empty += 1
            continue
        if pbf_complete(pbf):
            have += 1
        elif pbf.is_file():
            zero += 1
        else:
            missing += 1
    print("[china] status cells=%d  pbf-ready=%d  done=%d (empty-marked=%d)  "
          "zero-byte=%d  missing=%d"
          % (n, have + done, done, empty, zero, missing),
          file=sys.stderr, flush=True)


def classify_cells(jobs, split_dir: Path, force: bool):
    missing: list[tuple[int, int]] = []
    ready: list[tuple[int, int, Path]] = []
    skipped = 0
    for lat, lon in jobs:
        pbf = split_dir / (deg_name(lat, lon) + ".osm.pbf")
        marker = pbf.with_suffix(pbf.suffix + ".done")
        if marker.is_file() and not force:
            skipped += 1
            continue
        if pbf_complete(pbf):
            ready.append((lat, lon, pbf))
        else:
            missing.append((lat, lon))
    return missing, ready, skipped


_END = object()


def run_pipeline(jobs, args, osmium: str, jobs_cap: int,
                 tile_jobs_fixed: int) -> dict[str, int]:
    """Overlapped stages: split ∥ vmap ∥ graph+DEM ∥ pack, with backpressure."""
    missing, ready, skipped = classify_cells(
        jobs, args.split_dir, args.force)
    if args.limit > 0:
        take = ready + [
            (a, b, args.split_dir / (deg_name(a, b) + ".osm.pbf"))
            for a, b in missing]
        take = take[:args.limit]
        ready = [x for x in take if pbf_complete(x[2])]
        missing = [(a, b) for a, b, p in take if not pbf_complete(p)]
    miss: deque = deque(missing)
    n_goal = skipped + len(ready) + len(miss)
    slots = max(1, pack_jobs_now(n_goal or 1, jobs_cap))
    w = max(1, min(6, slots))
    q_mid = 1 if slots <= 2 else 2
    tile_jobs = (tile_jobs_fixed if tile_jobs_fixed > 0
                 else default_tile_jobs(w))
    cpu = os.cpu_count() or 4
    dem_jobs = max(4, min(32, (cpu * 2) // max(1, w)))
    pack_jobs = max(1, min(8, cpu // max(1, w)))
    low_water = max(12, w * 6)

    ready_q: Queue = Queue()
    for cell in ready:
        ready_q.put(cell)
    pbf_q: Queue = Queue(maxsize=max(8, w + 2))
    vmap_q: Queue = Queue(maxsize=q_mid)
    graph_q: Queue = Queue(maxsize=q_mid)
    hud = Hud()
    hud.n_goal = n_goal
    hud.set_split_todo(len(miss))
    hud.add("skip", skipped)
    hud_stop = threading.Event()
    fatal: list[BaseException] = []
    vmap_left = [w]
    graph_left = [w]
    stage_lock = threading.Lock()

    args.out.mkdir(parents=True, exist_ok=True)
    global _QUIET
    _QUIET = True
    print("[china] stream pipeline  workers=%d/%d/%d  q=%d  "
          "tile-jobs=%d dem-jobs=%d pack-jobs=%d  cells=%d  prefetch<=%d"
          % (w, w, w, q_mid, tile_jobs, dem_jobs, pack_jobs, n_goal,
             low_water),
          file=sys.stderr, flush=True)

    def _drop(item: dict | None) -> None:
        if item and item.get("work"):
            shutil.rmtree(item["work"], ignore_errors=True)

    def _note(st: str) -> None:
        hud.add(st)
        if st.startswith("fail"):
            print("[china] fail %s" % st, file=sys.stderr, flush=True)

    def _last_poison(left: list[int], q: Queue) -> None:
        with stage_lock:
            left[0] -= 1
            do = left[0] == 0
        if do:
            for _ in range(w):
                q.put(None)

    def splitter():
        label = ""
        try:
            if args.skip_split:
                return
            while miss:
                while ready_q.qsize() > low_water:
                    time.sleep(0.4)
                wait_for_work_mem(SPLIT_GIB, "split")
                n = extract_batch_now(max(1, args.extract_batch))
                chunk = []
                while miss and len(chunk) < n:
                    chunk.append(miss.popleft())
                if not chunk:
                    break
                hud.set_split_todo(len(miss))
                label = "osmium×%d" % len(chunk)
                hud.begin("split", label)
                split_1deg(args.pbf, args.split_dir, osmium, chunk, n)
                hud.end("split", label)
                label = ""
                for lat, lon in chunk:
                    pbf = args.split_dir / (deg_name(lat, lon) + ".osm.pbf")
                    if not pbf.is_file():
                        _note("missing")
                        continue
                    ready_q.put((lat, lon, pbf))
        except BaseException as exc:
            fatal.append(exc)
        finally:
            if label:
                hud.end("split", label)
            ready_q.put(_END)

    def feeder():
        try:
            while True:
                cell = ready_q.get()
                if cell is _END:
                    break
                lat, lon, pbf = cell
                try:
                    sz = pbf.stat().st_size
                except OSError:
                    _note("missing")
                    continue
                if sz < MIN_PBF_BYTES:
                    _mark(pbf, "empty\n")
                    _note("empty")
                    continue
                pbf_q.put({
                    "lat": lat, "lon": lon, "pbf": pbf,
                    "name": deg_name(lat, lon),
                })
                hud.set_q(pbf=pbf_q.qsize())
            for _ in range(w):
                pbf_q.put(None)
        except BaseException as exc:
            fatal.append(exc)
            for _ in range(w):
                try:
                    pbf_q.put(None, timeout=2)
                except Exception:
                    pass

    def vmap_worker():
        try:
            while True:
                item = pbf_q.get()
                hud.set_q(pbf=pbf_q.qsize())
                if item is None:
                    return
                name = item["name"]
                hud.begin("vmap", name + " 等内存")
                wait_for_work_mem(WORKER_GIB, "vmap")
                hud.end("vmap", name + " 等内存")
                hud.begin("vmap", name)
                try:
                    st = stage_vmap(item, args.zoom, args.zh, tile_jobs)
                except Exception:
                    st = "fail-vmap"
                hud.end("vmap", name)
                if st != "ok":
                    _note(st)
                    _drop(item)
                    continue
                vmap_q.put(item)
                hud.set_q(vmap=vmap_q.qsize())
        finally:
            _last_poison(vmap_left, vmap_q)

    def graph_worker():
        try:
            while True:
                item = vmap_q.get()
                hud.set_q(vmap=vmap_q.qsize())
                if item is None:
                    return
                name = item["name"] + " DEM"
                hud.begin("graph", name)
                try:
                    st = stage_graph(item, args.zh, dem_jobs)
                except Exception:
                    st = "fail-graph"
                hud.end("graph", name)
                if st != "ok":
                    _note(st)
                    _drop(item)
                    continue
                graph_q.put(item)
                hud.set_q(graph=graph_q.qsize())
        finally:
            _last_poison(graph_left, graph_q)

    def pack_worker():
        while True:
            item = graph_q.get()
            hud.set_q(graph=graph_q.qsize())
            if item is None:
                return
            name = item["name"] + " 裁剪"
            hud.begin("pack", name)
            try:
                st = stage_pack(item, args.out, args.region_km, pack_jobs)
            except Exception:
                st = "fail-pack"
            hud.end("pack", name)
            _note(st)
            _drop(item)

    def hud_loop():
        while not hud_stop.wait(0.4):
            hud.render()
        hud.render(force_log=True)

    th = [
        threading.Thread(target=splitter, name="split", daemon=True),
        threading.Thread(target=feeder, name="feed", daemon=True),
    ]
    th += [threading.Thread(target=vmap_worker, name="vmap%d" % i, daemon=True)
           for i in range(w)]
    th += [threading.Thread(target=graph_worker, name="graph%d" % i, daemon=True)
           for i in range(w)]
    th += [threading.Thread(target=pack_worker, name="pack%d" % i, daemon=True)
           for i in range(w)]
    mon = threading.Thread(target=hud_loop, name="hud", daemon=True)
    mon.start()
    for t in th:
        t.start()
    for t in th:
        t.join()
    hud_stop.set()
    mon.join(timeout=2)
    hud.close()
    _QUIET = False
    if fatal:
        raise fatal[0]
    return dict(hud.counts)


def finish_portals(args, counts: dict[str, int]) -> None:
    do_portals = (not args.skip_portals and not args.limit
                  and any(args.out.glob("lon*/lat*/x*_y*.vpk")))
    if args.limit and not args.skip_portals:
        print("[china] --limit: skip portal rebuild; when finished run:",
              file=sys.stderr)
        print("[china]   %s %s -o %s --rebuild-portals"
              % (VENV_PY, PACK_MAP, args.out), file=sys.stderr)
    if do_portals:
        rc = run([VENV_PY, PACK_MAP, "-o", args.out, "--rebuild-portals",
                  "--jobs", str(auto_jobs_cap())])
        if rc != 0:
            print("[china] portal rebuild failed rc=%d" % rc, file=sys.stderr)
            sys.exit(rc)
    print("[china] done %s" % counts, file=sys.stderr)
    fails = sum(n for k, n in counts.items() if k.startswith("fail"))
    if fails:
        raise SystemExit("%d 1deg cell(s) failed" % fails)


def main():
    ap = argparse.ArgumentParser(
        description="Split China PBF into 1° cells, then pack tiles+graph+DEM.")
    ap.add_argument("--pbf", type=Path, default=CHINA_PBF)
    ap.add_argument("--split-dir", type=Path, default=SPLIT_DIR)
    ap.add_argument("--out", type=Path, default=OUT_MAP)
    ap.add_argument("--region-km", type=float, default=DEFAULT_REGION_KM)
    ap.add_argument("--zoom", type=int, default=14)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--status", action="store_true",
                    help="print 1° split/pack progress and exit")
    ap.add_argument("--all", action="store_true",
                    help="same as default pipeline (kept for compatibility)")
    ap.add_argument("--split-only", action="store_true",
                    help="only extract 1° PBFs; do not pack")
    ap.add_argument("--pack-only", action="store_true",
                    help="same as --skip-split: use existing china_1deg/*.osm.pbf")
    ap.add_argument("--skip-split", action="store_true",
                    help="use existing china_1deg/*.osm.pbf")
    ap.add_argument("--skip-portals", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="ignore *.osm.pbf.done markers")
    ap.add_argument("--only", action="append", default=[],
                    help="only these 1deg cells, e.g. N32E118 (repeatable)")
    ap.add_argument("--bbox", action="append", default=[],
                    metavar="W,S,E,N",
                    help="only 1deg cells intersecting this bbox (repeatable)")
    ap.add_argument("--province", action="append", default=[],
                    help="anhui,jiangsu,guangdong (repeatable or comma-separated)")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel 1° cells (0 = auto from CPU and MemAvailable)")
    ap.add_argument("--tile-jobs", type=int, default=0,
                    help="build_vmap workers per 1° cell (0 = CPU / region-jobs)")
    ap.add_argument("--extract-batch", type=int, default=EXTRACT_BATCH,
                    help="osmium extracts per national-PBF scan (default %d)"
                    % EXTRACT_BATCH)
    ap.add_argument("--limit", type=int, default=0,
                    help="process at most N 1deg cells this run")
    ap.add_argument(
        "--zh", choices=("hans", "hant"), default="hans",
        help="name script: hans=simplified (default), hant=traditional",
    )
    args = ap.parse_args()
    if args.pack_only:
        args.skip_split = True
    if args.split_only and args.skip_split:
        raise SystemExit(" --split-only and --pack-only/--skip-split conflict")
    if args.all and (args.split_only or args.skip_split):
        raise SystemExit(" --all cannot combine with --split-only / --pack-only")

    jobs, provs = select_jobs(args)
    n_deg = len(jobs)
    cell_lon, cell_lat = grid_cell_deg(GRID_GLOBAL_ANCHOR_LAT, args.region_km)
    ix0 = int((CHINA_WEST - GRID_GLOBAL_ORIGIN_LON) / cell_lon)
    ix1 = int((CHINA_EAST - GRID_GLOBAL_ORIGIN_LON) / cell_lon)
    iy0 = int((CHINA_SOUTH - GRID_GLOBAL_ORIGIN_LAT) / cell_lat)
    iy1 = int((CHINA_NORTH - GRID_GLOBAL_ORIGIN_LAT) / cell_lat)
    n_grid = (ix1 - ix0) * (iy1 - iy0)

    print("[china] land bbox %.0f..%.0fE %.0f..%.0fN" % (
        CHINA_WEST, CHINA_EAST, CHINA_SOUTH, CHINA_NORTH), file=sys.stderr)
    print("[china] 1deg extracts: %d (incl. 1° ring)" % n_deg, file=sys.stderr)
    print("[china] %.0fkm grid: ~%d cells in land rectangle "
          "(ocean / no-road skipped; path is lon*/lat*/x*_y*.vpk)"
          % (args.region_km, n_grid), file=sys.stderr)
    print("[china] pbf=%s" % args.pbf, file=sys.stderr)
    print("[china] split=%s" % args.split_dir, file=sys.stderr)
    print("[china] out=%s" % args.out, file=sys.stderr)
    print("[china] dem-cache=%s" % DEM_CACHE, file=sys.stderr)
    if provs:
        print("[china] provinces: %s" % ",".join(provs), file=sys.stderr)
    if args.only or args.bbox or provs:
        print("[china] cells: %s" % " ".join(deg_name(a, b) for a, b in jobs),
              file=sys.stderr)
    print_status(jobs, args.split_dir)
    if args.dry_run or args.status:
        if args.dry_run and not args.status:
            auto = (args.jobs if args.jobs > 0 else default_region_jobs(8))
            if args.split_only:
                print("[china] would split-only  extract-batch=%d  "
                      "avail=%.1f leave=%.1f"
                      % (args.extract_batch, avail_mem_gib(), reserve_gib()),
                      file=sys.stderr)
            elif args.skip_split:
                print("[china] would pack-only stream  extract-batch=%d "
                      "jobs<=%d  avail=%.1f leave=%.1f"
                      % (args.extract_batch, auto, avail_mem_gib(),
                         reserve_gib()), file=sys.stderr)
            else:
                print("[china] would stream: split+vmap+graph+pack overlap  "
                      "batch<=%d jobs<=%d tile-jobs<=%d  "
                      "avail=%.1fGiB leave=%.1fGiB"
                      % (args.extract_batch, auto, default_tile_jobs(auto),
                         avail_mem_gib(), reserve_gib()), file=sys.stderr)
        return

    if not args.pbf.is_file():
        raise SystemExit("missing %s" % args.pbf)
    osmium = shutil.which("osmium")
    if not osmium:
        raise SystemExit("need osmium CLI (apt install osmium-tool)")
    if not VENV_PY.is_file():
        raise SystemExit("need %s (run: make_cities_map.sh to create the venv)" % VENV_PY)
    if not PACK_MAP.is_file():
        raise SystemExit("need %s" % PACK_MAP)

    jobs_cap = args.jobs if args.jobs > 0 else auto_jobs_cap()
    jobs_cap = max(1, jobs_cap)
    print("[china] mode=%s  cpu=%d total=%.1fGiB avail=%.1fGiB leave=%.1fGiB "
          "(20%% or 3GiB)  → pack-jobs<=%d extract-batch<=%d"
          % ("split-only" if args.split_only else
             "pack-only" if args.skip_split else "stream",
             os.cpu_count() or 1, total_mem_gib(), avail_mem_gib(),
             reserve_gib(), jobs_cap, args.extract_batch),
          file=sys.stderr, flush=True)
    print("[china] pack jobs = min(cpu, cap, floor((avail-leave)/%.0fGiB))"
          % WORKER_GIB, file=sys.stderr, flush=True)

    if args.split_only:
        split_1deg(args.pbf, args.split_dir, osmium, jobs, args.extract_batch)
        print_status(jobs, args.split_dir)
        print("[china] split-only done; pack with:", file=sys.stderr)
        print("[china]   ./make_china_map.sh --pack-only", file=sys.stderr)
        return

    counts = run_pipeline(jobs, args, osmium, jobs_cap, args.tile_jobs)
    finish_portals(args, counts)


if __name__ == "__main__":
    main()
