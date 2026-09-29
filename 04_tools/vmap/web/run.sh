#!/usr/bin/env bash
#
# run.sh — launch the vmap web generator using the shared ../.venv.
#
#   ./run.sh                 # http://127.0.0.1:5000
#   VMAP_WEB_PORT=8080 ./run.sh
#   VMAP_CHINA_PBF=/path/china-latest.osm.pbf ./run.sh
#
set -euo pipefail

WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
OSM_DIR="$(cd "${WEB_DIR}/.." && pwd -P)"
PY="${OSM_DIR}/.venv/bin/python"
PIP="${OSM_DIR}/.venv/bin/pip"

if [[ ! -x "${PY}" ]]; then
  echo "error: venv not found at ${OSM_DIR}/.venv — create it first:" >&2
  echo "  python3 -m venv ${OSM_DIR}/.venv && ${OSM_DIR}/.venv/bin/pip install -r ${OSM_DIR}/requirements.txt" >&2
  exit 1
fi

if ! "${PY}" -c "import flask" 2>/dev/null; then
  echo "[web] installing flask into shared venv"
  "${PIP}" install -r "${WEB_DIR}/requirements.txt"
fi

if ! command -v osmium >/dev/null 2>&1; then
  echo "warning: 'osmium' CLI not found — needed for bbox extract (apt install osmium-tool)" >&2
fi

cd "${WEB_DIR}"
exec "${PY}" app.py
