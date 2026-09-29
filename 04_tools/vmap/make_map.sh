#!/usr/bin/env bash
#
# Compatibility entry.  Chuzhou generation now uses the fixed-grid dataset
# under /home/jinsc/SDK/vela/osm_data/chuzhou.
#
set -euo pipefail

OSM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
echo "[make_map] forwarding to make_chuzhou_map.sh" >&2
exec "${OSM_DIR}/make_chuzhou_map.sh" "$@"
