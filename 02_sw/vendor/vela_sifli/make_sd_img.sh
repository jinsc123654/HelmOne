#!/bin/bash
# ============================================================================
# make_sd_img.sh <SD 卡容量>   —— 一条命令：编出固件 + 打包成 SD 整盘 .img
#
# 例：
#   ./make_sd_img.sh 32G          # 32 GiB 卡
#   ./make_sd_img.sh 32768M       # 同上（也可直接写 MiB）
#   ./make_sd_img.sh 64G -o /tmp/helm_sd.img
#
# 做什么：
#   1) main 槽固件（configs/nsh）
#   2) factory 槽固件（configs/nsh-factory；pack-sd-img 需要它）
#   3) 若 boot_loader/project 在（完整树），顺带重建二级 boot；不在就用随仓的 bin
#   4) pack_sd_img.py --card-mib <容量> --full-size  → 逻辑大小 = 你给的卡容量
#
# 只产出 .img，不烧录、不监视（烧录用 build_board.py burn-sd / flash-all）。
# ============================================================================
set -euo pipefail

SIZE="${1:-}"
shift || true

OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out) OUT="${2:-}"; shift 2 ;;
    *) echo "未知参数: $1（可用: -o <输出路径>）" >&2; exit 2 ;;
  esac
done

if [ -z "$SIZE" ]; then
  echo "用法: $0 <SD 卡容量，如 32G / 32768M> [-o 输出.img]" >&2
  exit 2
fi

case "$SIZE" in
  *[Gg]) MIB=$(( ${SIZE%[Gg]} * 1024 )) ;;
  *[Mm]) MIB=${SIZE%[Mm]} ;;
  *[0-9]) MIB="$SIZE" ;;
  *) echo "容量写法: 32G 或 32768M（也可以直接给 MiB 数字）" >&2; exit 2 ;;
esac
if [ "$MIB" -lt 4096 ]; then
  echo "卡容量太小（${MIB} MiB）：分区表按 ~15 GiB 布局，建议 ≥ 16G" >&2
  exit 2
fi

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"           # openvela 工作区根
TOOL="$ROOT/vendor/my_vendor/build_board.py"
[ -f "$TOOL" ] || { echo "找不到 $TOOL（脚本要放在 vendor/my_vendor/ 下）" >&2; exit 1; }
cd "$ROOT"

echo "=== 1/3 main 槽固件 ==="
python3 "$TOOL" build

echo "=== 2/3 factory 槽固件 ==="
python3 "$TOOL" build-factory

if [ -d "$ROOT/vendor/my_vendor/boot_loader/project" ]; then
  echo "=== 3/3 二级 boot（重建 ftab.bin + bootloader.bin）==="
  python3 "$TOOL" build-boot
else
  echo "=== 3/3 二级 boot：用随仓的 boot_loader/bin 预编译件（本树未含 project）==="
fi

IMG="${OUT:-$ROOT/vendor/my_vendor/boot_loader/bin/my_vendor_sd.img}"
echo "=== pack-sd-img: 卡容量 ${MIB} MiB -> $IMG ==="
python3 "$ROOT/vendor/my_vendor/scripts/pack_sd_img.py" \
  --card-mib "$MIB" --full-size -o "$IMG"

ls -lh "$IMG"
echo
echo "完成：$IMG"
echo "写入整盘 SD（慎用，会覆盖目标盘）："
echo "  sudo dd if='$IMG' of=/dev/sdX bs=4M status=progress conv=fsync"
