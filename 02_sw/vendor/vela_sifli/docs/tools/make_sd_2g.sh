#!/bin/bash
# 出 **2G 卡** 的 SD 整盘 img（逻辑大小 2048 MiB）。
# 终端直接跑：bash docs/tools/make_sd_2g.sh
# 想换输出路径： bash docs/tools/make_sd_2g.sh -o /tmp/helm_2g.img
set -euo pipefail
TREE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "$TREE/make_sd_img.sh" 2G "$@"
