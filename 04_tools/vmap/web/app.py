#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
app.py — web UI for the bicycle vmap generator.

Draw / type a WGS84 bbox on a Leaflet map, pick zoom + region size, hit
生成, and the server runs the offline toolchain (osmium extract -> build_vmap
-> build_vgraph -> pack_map) producing a device-ready map/ bundle under
web/out/<name>/map/.

Run:
    ../.venv/bin/pip install -r requirements.txt   # once (adds flask)
    ../.venv/bin/python app.py                     # http://127.0.0.1:5000
or just:  ./run.sh
"""

from __future__ import annotations

import re
import threading
import uuid
from pathlib import Path

from flask import (Flask, jsonify, request, send_from_directory,
                   render_template, abort)

import vmap_pipeline as vp

app = Flask(__name__)

JOBS: dict[str, vp.Job] = {}
_LOCK = threading.Lock()

_NAME_RE = re.compile(r"[^A-Za-z0-9_.-]+")


def _safe_name(raw: str) -> str:
    name = _NAME_RE.sub("_", (raw or "").strip()).strip("._") or "region"
    return name[:64]


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/config")
def api_config():
    return jsonify({
        "china_pbf": str(vp.DEFAULT_CHINA_PBF),
        "china_pbf_exists": vp.DEFAULT_CHINA_PBF.is_file(),
        "grid_anchor_lat": vp.GRID_ANCHOR_LAT,
        "grid_origin_lon": vp.GRID_ORIGIN_LON,
        "grid_origin_lat": vp.GRID_ORIGIN_LAT,
        "default_region_km": vp.DEFAULT_REGION_KM,
        "max_bbox_deg": vp.MAX_BBOX_DEG,
        "out_root": str(vp.OUT_ROOT),
    })


@app.route("/api/grid", methods=["POST"])
def api_grid():
    d = request.get_json(force=True)
    try:
        cells = vp.cells_for_bbox(
            float(d["west"]), float(d["south"]),
            float(d["east"]), float(d["north"]),
            float(d.get("region_km") or vp.DEFAULT_REGION_KM))
    except (KeyError, ValueError, TypeError) as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify({"cells": cells, "count": len(cells)})


@app.route("/api/generate", methods=["POST"])
def api_generate():
    d = request.get_json(force=True)
    try:
        params = {
            "name": _safe_name(d.get("name", "")),
            "west": float(d["west"]), "south": float(d["south"]),
            "east": float(d["east"]), "north": float(d["north"]),
            "zooms": [int(z) for z in (d.get("zooms") or [14])],
            "region_km": float(d.get("region_km") or vp.DEFAULT_REGION_KM),
            "build_graph": bool(d.get("build_graph", True)),
        }
    except (KeyError, ValueError, TypeError) as exc:
        return jsonify({"error": f"参数错误: {exc}"}), 400

    job_id = uuid.uuid4().hex[:12]
    job = vp.Job(job_id=job_id, params=params)
    with _LOCK:
        JOBS[job_id] = job
    threading.Thread(target=vp.run_pipeline, args=(job,), daemon=True).start()
    return jsonify({"job_id": job_id})


@app.route("/api/job/<job_id>")
def api_job(job_id: str):
    job = JOBS.get(job_id)
    if not job:
        return jsonify({"error": "job not found"}), 404
    frm = request.args.get("from", default=0, type=int)
    return jsonify({
        "job_id": job.job_id,
        "status": job.status,
        "progress": job.progress,
        "stage": job.stage,
        "log": job.log[frm:],
        "log_len": len(job.log),
        "result": job.result,
        "params": job.params,
    })


@app.route("/api/outputs")
def api_outputs():
    items = []
    if vp.OUT_ROOT.is_dir():
        for d in sorted(vp.OUT_ROOT.iterdir()):
            mapd = d / "map"
            if not mapd.is_dir():
                continue
            vpks = list(mapd.rglob("*.vpk"))
            items.append({
                "name": d.name,
                "map_dir": str(mapd),
                "vpk_count": len(vpks),
                "total_kb": round(sum(f.stat().st_size for f in mapd.rglob("*")
                                      if f.is_file()) / 1024, 1),
            })
    return jsonify({"outputs": items})


@app.route("/out/<path:relpath>")
def out_file(relpath: str):
    base = vp.OUT_ROOT.resolve()
    target = (base / relpath).resolve()
    if not str(target).startswith(str(base)) or not target.is_file():
        abort(404)
    return send_from_directory(base, relpath, as_attachment=True)


if __name__ == "__main__":
    vp.OUT_ROOT.mkdir(parents=True, exist_ok=True)
    import os
    host = os.environ.get("VMAP_WEB_HOST", "127.0.0.1")
    port = int(os.environ.get("VMAP_WEB_PORT", "5000"))
    print(f"vmap generator web  ->  http://{host}:{port}")
    app.run(host=host, port=port, threaded=True, debug=False)
