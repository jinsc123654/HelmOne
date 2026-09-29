#!/usr/bin/env bash
# Preview a packed map/ in the browser (lon*/lat*/x*_y*.vpk, no map.idx).
#   ./view_map.sh                 # 内置演示包 ../map（北京 64 块）
#   ./view_map.sh /path/to/map
#   VMAP_CHINA_MAP=/path/to/map ./view_map.sh
#   ./view_map.sh --no-open --port 8791      # 其余参数原样透给 view_map.py
set -euo pipefail
WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
MAP_DIR="${VMAP_CHINA_MAP:-${WEB_DIR}/../map}"
# 首个参数不是选项时按地图目录处理（老用法），其余继续透传。
if [[ $# -gt 0 && "${1:0:1}" != "-" ]]; then
  MAP_DIR="$1"
  shift
fi
exec python3 "${WEB_DIR}/view_map.py" --dir "${MAP_DIR}" "$@"
