#!/bin/bash
# 出 **8G 卡** 的 SD 整盘 img（逻辑大小 8192 MiB）。
# 终端直接跑：bash docs/tools/make_sd_8g.sh
# 想换输出路径： bash docs/tools/make_sd_8g.sh -o /tmp/helm_8g.img
set -euo pipefail
TREE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "$TREE/make_sd_img.sh" 8G "$@"
