#!/bin/bash
# 出 **16G 卡** 的 SD 整盘 img（逻辑大小 16384 MiB）。
# 终端直接跑：bash docs/tools/make_sd_16g.sh
# 想换输出路径： bash docs/tools/make_sd_16g.sh -o /tmp/helm_16g.img
#
# 注：ptab 默认的 SD_PACK_CARD_MIB=14832 是"典型 16G 卡的 CSD"，
# 所以刻完写卡后设备看到的规模以卡自身 CSD 为准；本脚本按 16384 MiB 出镜像，
# 与 16G 标称卡一致。要严格贴 14832，直接跑：
#   bash make_sd_img.sh 14832M
set -euo pipefail
TREE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "$TREE/make_sd_img.sh" 16G "$@"
