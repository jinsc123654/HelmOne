#!/usr/bin/env python3
"""Serve the packed map/ directory plus a browser viewer.

  ./view_map.sh                    # 内置演示包（../map，北京 64 块）
  ./view_map.sh /path/to/map
  python3 view_map.py --dir /path/to/map
"""

from __future__ import annotations

import argparse
import errno
import json
import mimetypes
import os
import sys
import urllib.error
import urllib.request
import webbrowser
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
from urllib.parse import unquote, urlparse

HERE = Path(__file__).resolve().parent
# Demo pack bundled with this toolchain (lon*/lat*/x*_y*.vpk), so the viewer
# runs with no arguments. $VMAP_CHINA_MAP still overrides.
_MAP_ENV = os.environ.get("VMAP_CHINA_MAP")
DEFAULT_MAP = Path(_MAP_ENV) if _MAP_ENV else HERE.parent / "map"

# Keep in sync with vmap_format.h
GRID_ORIGIN_LON = 0.0
GRID_ORIGIN_LAT = 0.0
GRID_CELL_LAT = 0.026949335249730506
GRID_CELL_LON = 0.032899063849730509
GRID_BUCKET = 4


def list_grid_cells(map_dir: Path) -> list[dict]:
    cells = []
    if not map_dir.is_dir():
        return cells
    for p in sorted(map_dir.glob("lon*/lat*/x*_y*.vpk")):
        stem = p.stem
        if not stem.startswith("x") or "_y" not in stem:
            continue
        xs, ys = stem.split("_y", 1)
        try:
            ix = int(xs[1:])
            iy = int(ys)
        except ValueError:
            continue
        west = GRID_ORIGIN_LON + ix * GRID_CELL_LON
        south = GRID_ORIGIN_LAT + iy * GRID_CELL_LAT
        rel = str(p.relative_to(map_dir)).replace("\\", "/")
        cells.append({
            "id": (iy << 16) | ix,
            "ix": ix,
            "iy": iy,
            "neighbors": 0,
            "west": west,
            "south": south,
            "east": west + GRID_CELL_LON,
            "north": south + GRID_CELL_LAT,
            "vpk": rel,
            "bytes": p.stat().st_size,
        })
    return cells


def make_handler(web_dir: Path, map_dir: Path):
    web_dir = web_dir.resolve()
    map_dir = map_dir.resolve()

    class Handler(SimpleHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            sys.stderr.write("[view] " + (fmt % args) + "\n")

        def _send_bytes(self, data: bytes, ctype: str, code: int = 200) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        def _send_path(self, path: Path, ctype: str | None = None) -> None:
            if not path.is_file():
                self.send_error(404, "not found")
                return
            if ctype is None:
                ctype = mimetypes.guess_type(str(path))[0] or "application/octet-stream"
            data = path.read_bytes()
            self._send_bytes(data, ctype)

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            path = unquote(parsed.path)

            if path in ("/", "/index.html", "/view.html", "/view"):
                self._send_path(web_dir / "view.html", "text/html; charset=utf-8")
                return

            if path == "/api/info":
                idx = map_dir / "map.idx"
                cells = list_grid_cells(map_dir)
                body = json.dumps({
                    "map_dir": str(map_dir),
                    "has_idx": idx.is_file(),
                    "idx_bytes": idx.stat().st_size if idx.is_file() else 0,
                    "grid": len(cells) > 0,
                    "vpk": sum(1 for _ in map_dir.rglob("*.vpk")) if map_dir.is_dir() else 0,
                }).encode()
                self._send_bytes(body, "application/json; charset=utf-8")
                return

            if path == "/api/catalog":
                cells = list_grid_cells(map_dir)
                body = json.dumps({
                    "originLon": GRID_ORIGIN_LON,
                    "originLat": GRID_ORIGIN_LAT,
                    "cellLon": GRID_CELL_LON,
                    "cellLat": GRID_CELL_LAT,
                    "regions": cells,
                }).encode()
                self._send_bytes(body, "application/json; charset=utf-8")
                return

            if path.startswith("/static/"):
                target = (web_dir / path.lstrip("/")).resolve()
                if not str(target).startswith(str(web_dir)) or not target.is_file():
                    self.send_error(404)
                    return
                ctype = mimetypes.guess_type(str(target))[0] or "text/plain"
                if target.suffix == ".js":
                    ctype = "text/javascript; charset=utf-8"
                elif target.suffix == ".css":
                    ctype = "text/css; charset=utf-8"
                self._send_path(target, ctype)
                return

            if path.startswith("/map/"):
                rel = path[5:]
                if not rel or ".." in rel.split("/"):
                    self.send_error(400)
                    return
                target = (map_dir / rel).resolve()
                if not str(target).startswith(str(map_dir)) or not target.is_file():
                    self.send_error(404)
                    return
                self._send_path(target, "application/octet-stream")
                return

            self.send_error(404)

    return Handler


def peek_viewer(host: str, port: int) -> dict | None:
    url = f"http://{host}:{port}/api/info"
    try:
        with urllib.request.urlopen(url, timeout=0.5) as resp:
            return json.loads(resp.read().decode())
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError):
        return None


def maybe_open(url: str, no_open: bool) -> None:
    if no_open:
        return
    try:
        webbrowser.open(url)
    except Exception:
        pass


def main() -> None:
    ap = argparse.ArgumentParser(description="Preview a packed VREG map/ in the browser")
    ap.add_argument("--dir", type=Path, default=DEFAULT_MAP,
                    help="packed map directory (lon*/lat*/x*_y*.vpk, no map.idx)")
    ap.add_argument("--host", default=os.environ.get("VMAP_VIEW_HOST", "127.0.0.1"))
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("VMAP_VIEW_PORT", "8766")))
    ap.add_argument("--no-open", action="store_true")
    args = ap.parse_args()

    if not args.dir.is_dir():
        print(f"warning: map dir missing: {args.dir}", file=sys.stderr)
    else:
        cells = list_grid_cells(args.dir)
        if not cells and not (args.dir / "map.idx").is_file():
            print(f"warning: no x*_y*.vpk or map.idx in {args.dir}", file=sys.stderr)
        elif cells:
            print(f"grid cells  ->  {len(cells)}  (lon*/lat*/x*_y*.vpk)", file=sys.stderr)

    handler = make_handler(HERE, args.dir)
    want = args.dir.resolve()
    port = args.port
    httpd = None
    for _ in range(20):
        try:
            httpd = ThreadingHTTPServer((args.host, port), handler)
            break
        except OSError as exc:
            if exc.errno != errno.EADDRINUSE:
                raise
            info = peek_viewer(args.host, port)
            existing = Path(info["map_dir"]).resolve() if info and info.get("map_dir") else None
            if existing == want:
                url = f"http://{args.host}:{port}/"
                print(f"vmap viewer already running  ->  {url}", flush=True)
                print(f"map dir      ->  {existing}", flush=True)
                maybe_open(url, args.no_open)
                return
            print(f"port {port} in use, trying {port + 1}", flush=True)
            port += 1
    if httpd is None:
        raise SystemExit(f"could not bind {args.host}:{args.port}..{port}")

    url = f"http://{args.host}:{port}/"
    print(f"vmap viewer  ->  {url}", flush=True)
    print(f"map dir      ->  {args.dir.resolve()}", flush=True)
    maybe_open(url, args.no_open)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped", file=sys.stderr)


if __name__ == "__main__":
    main()
