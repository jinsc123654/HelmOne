#!/usr/bin/env bash
# 一行启动地图预览器，直接看内置演示包（04_tools/vmap/map，北京 64 块）。
#
#   ./view_map.sh                 # 内置演示包
#   ./view_map.sh /path/to/map    # 任意 lon*/lat*/x*_y*.vpk 打包目录
#   VMAP_CHINA_MAP=/path/to/map ./view_map.sh
#
# 转发到 web/view_map.sh（真正的实现）。
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/web/view_map.sh" "$@"
