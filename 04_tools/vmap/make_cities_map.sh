#!/usr/bin/env bash
#
# Build Chuzhou / Nanjing / Guangzhou into one national-grid pack.
# Extract/vmap/graph run in parallel (--jobs). Names default to simplified.
#
#   ./make_cities_map.sh --only guangzhou --reuse
#   ./make_cities_map.sh --jobs 3
#
set -euo pipefail
OSM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PY="${OSM_DIR}/.venv/bin/python"
REQ="${OSM_DIR}/requirements.txt"

if [[ ! -x "${PY}" ]]; then
  python3 -m venv "${OSM_DIR}/.venv"
  "${PY}" -m pip install -U pip
  "${PY}" -m pip install -r "${REQ}"
elif ! "${PY}" -c "import osmium, shapely, requests, PIL" 2>/dev/null; then
  "${PY}" -m pip install -r "${REQ}"
fi

if ! command -v osmium >/dev/null 2>&1; then
  echo "error: osmium CLI not found (apt install osmium-tool)" >&2
  exit 1
fi

exec "${PY}" "${OSM_DIR}/make_cities_map.py" "$@"
