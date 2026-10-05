#!/bin/bash
# 出 **4G 卡** 的 SD 整盘 img（逻辑大小 4096 MiB）。
# 终端直接跑：bash docs/tools/make_sd_4g.sh
# 想换输出路径： bash docs/tools/make_sd_4g.sh -o /tmp/helm_4g.img
set -euo pipefail
TREE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "$TREE/make_sd_img.sh" 4G "$@"
