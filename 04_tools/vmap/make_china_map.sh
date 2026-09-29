#!/usr/bin/env bash
#
# 全国地图（滁州 / 广州同一套算法）：瓦片 + 路网 + 海拔
#   1) 先把 china-latest.osm.pbf 按 1° 切开（不切完无法打全国）
#   2) 每个 1°：build_vmap + build_vgraph(Terrarium 海拔) + pack_map
#      3 km 固定网格 → lon*/lat*/x*_y*.vpk
#   3) 全部 1° 打完后一次 --rebuild-portals
#
# 默认流水线（另一台更强的机器也直接跑这个，不必手写 --jobs）：
#   切块 / 切片 / 路网+DEM / 裁剪打包 同时转（队列反压，内存留余量）。
#   终端前台刷新进度条；tee 日志里每 15 秒一行快照。
#   并发按本机 CPU + MemAvailable 算：
#     留量 = max(3GiB, 总内存×20%)
#     打包路数 = min(CPU, 12, floor((可用内存-留量)/4GiB))
#     切批大小 = 4～16（内存越大越宽）
#   内存紧就等一会儿，或自动降路数 / 缩小切批。
#
# 用法：
#   ./make_china_map.sh                            # 自动：流水线一直转
#   ./make_china_map.sh --jobs 4                   # 可选：打包路数上限（仍看余量）
#   ./make_china_map.sh --split-only               # 只切
#   ./make_china_map.sh --pack-only                # 只打已切好的
#   ./make_china_map.sh --dry-run
#   ./make_china_map.sh --status
#
# 可断点续跑：已切好的 1°.osm.pbf 会跳过；打成功的格子留 *.osm.pbf.done。
# 硬上限默认 12，可用 VMAP_CHINA_MAX_JOBS 改。
#
# 默认写出 osm_data/map_china/，不覆盖现有城市包 osm_data/map/。
# 要合进城市包：VMAP_CHINA_MAP=/home/jinsc/SDK/vela/osm_data/map ./make_china_map.sh
#
# 环境变量：
#   VMAP_CHINA_PBF   全国 PBF（默认 osm_data/china-latest.osm.pbf）
#   VMAP_CHINA_1DEG  1° 切块目录（默认 osm_data/china_1deg）
#   VMAP_CHINA_MAP   设备包目录（默认 osm_data/map_china）
#   VMAP_CHINA_LOG   日志（默认 osm_data/map_build_china.log）
#   VMAP_DEM_CACHE   Terrarium 缓存（默认 docs/osm/cache/dem）
#
set -euo pipefail

OSM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PY="${OSM_DIR}/.venv/bin/python"
REQ="${OSM_DIR}/requirements.txt"
DATA_ROOT="${VMAP_DATA_ROOT:-/home/jinsc/SDK/vela/osm_data}"
export VMAP_CHINA_PBF="${VMAP_CHINA_PBF:-${DATA_ROOT}/china-latest.osm.pbf}"
export VMAP_CHINA_1DEG="${VMAP_CHINA_1DEG:-${DATA_ROOT}/china_1deg}"
export VMAP_CHINA_MAP="${VMAP_CHINA_MAP:-${DATA_ROOT}/map_china}"
export VMAP_DEM_CACHE="${VMAP_DEM_CACHE:-${OSM_DIR}/cache/dem}"
LOG="${VMAP_CHINA_LOG:-${DATA_ROOT}/map_build_china.log}"

# osmium / tempfile 不要写进 tmpfs，多核打包时 /tmp 只有几 GB。
export TMPDIR="${TMPDIR:-${DATA_ROOT}/tmp}"
mkdir -p "${TMPDIR}" "${VMAP_CHINA_1DEG}" "${VMAP_CHINA_MAP}" "$(dirname "${LOG}")" \
         "${VMAP_DEM_CACHE}"

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

MEM_AVAIL="$(awk '/MemAvailable:/ {printf "%.1fGiB", $2/1024/1024}' /proc/meminfo)"
{
  echo "===== $(date -Iseconds) nationwide vmap start ====="
  echo "args=$*"
  echo "jobs-arg=$(printf '%s ' "$@" | grep -o -- '--jobs [^ ]*' || echo auto)"
  echo "TMPDIR=${TMPDIR}"
  echo "pbf=${VMAP_CHINA_PBF}"
  echo "split=${VMAP_CHINA_1DEG}"
  echo "out=${VMAP_CHINA_MAP}"
  echo "dem=${VMAP_DEM_CACHE}"
  echo "cpu=$(nproc) mem=${MEM_AVAIL}"
} | tee -a "${LOG}"

export PYTHONUNBUFFERED=1
set +e
# HUD 写 /dev/tty；tee 只收日志行，进度条不被管道挡住。
"${PY}" "${OSM_DIR}/make_china_map.py" "$@" 2>&1 | tee -a "${LOG}"
rc=${PIPESTATUS[0]}
set -e
echo "===== $(date -Iseconds) nationwide vmap end rc=${rc} =====" | tee -a "${LOG}"
exit "${rc}"
