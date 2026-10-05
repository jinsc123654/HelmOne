#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Pack mkfs/{kv,lfs,fat} into three images:
#   kv/  -> kv_root.bin   LittleFS @ KV_REGION  (256 MiB)
#   lfs/ -> fs_root.bin   LittleFS @ FS_REGION  (SD: 512 MiB; NAND: ptab size)
#   fat/ -> fat_root.bin  FAT32    @ FAT_REGION (SD remainder seed; skipped on NAND)
#                         volume root: map/ (tiles) + fonts/ (TTF)
#
# Usage (from openvela root):
#   vendor/my_vendor/scripts/build_fs_root.sh
#   vendor/my_vendor/scripts/build_fs_root.sh /path/to/custom/mkfs
#
# BUILD_FAT=1 (default) packs fat_root.bin. Regular `build` sets BUILD_FAT=0
# and only refreshes kv/lfs; FAT is packed by build-fs / build-all.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
MYV="${ROOT}/vendor/my_vendor"
STAGING="${1:-${MYV}/boards/sf32lb52/my_vendor/mkfs}"
BIN_DIR="${MYV}/boot_loader/bin"
LFS_VER="v2.5.1"
LFS_CACHE="${MYV}/.cache/littlefs-${LFS_VER}"
MKLFS_BUILD="${MYV}/.cache/mklfs_disk-build"
MKLFS="${MKLFS_BUILD}/mklfs_disk"
SCRIPTS="${MYV}/scripts"

if [[ ! -d "${STAGING}" ]]; then
  echo "ERROR: mkfs dir missing: ${STAGING}" >&2
  exit 1
fi

BUILD_FAT="${BUILD_FAT:-1}"

eval "$(
  ROOT="${ROOT}" PYTHONPATH="${SCRIPTS}${PYTHONPATH:+:${PYTHONPATH}}" python3 - <<'PY'
import os
import sys
from pathlib import Path
import flash_args_lib as fal
import json

root = Path(os.environ["ROOT"])
cfg = root / "vendor/my_vendor/boot_loader/config/nsh"
storage = fal.boot_storage_value(cfg) or "nand"
ptab = fal.resolve_ptab_path(cfg, storage=storage)
addrs = fal.load_ptab_img_addresses(ptab)
sizes = fal.load_ptab_img_sizes(ptab)
mems = json.loads(ptab.read_text(encoding="utf-8"))

def region_size(img: str) -> int:
    return int(sizes.get(img, 0))

fs_size = region_size("fs_root")
kv_size = region_size("kv_root")
fat_size = region_size("fat_root")
seed_note = ""

if fs_size == 0 and storage in ("sd", "emmc"):
    fs_size = fal.sd_pack_seed_bytes(ptab)
    seed_note = " (legacy FS remainder seed)"

if fat_size == 0 and "fat_root" in addrs and storage in ("sd", "emmc"):
    fat_size = fal.sd_pack_seed_bytes(ptab)
    card = fal.sd_pack_card_bytes(ptab)
    env_mib = os.environ.get("SD_PACK_CARD_MIB")
    ptab_mib = fal.ptab_sd_pack_card_mib(ptab)
    seed_mib = env_mib or (str(ptab_mib) if ptab_mib is not None else str(fal.SD_PACK_CARD_MIB_DEFAULT))
    seed_note += (
        f" (fat seed; card={card / (1024**3):.2f} GiB, "
        f"SD_PACK_CARD_MIB={seed_mib})"
    )

if "fs_root" not in addrs:
    raise SystemExit(f"ptab has no fs_root: {ptab}")

fs_base = fal.fmt_addr(addrs["fs_root"])
kv_base = fal.fmt_addr(addrs["kv_root"]) if "kv_root" in addrs else "0x0"
fat_base = fal.fmt_addr(addrs["fat_root"]) if "fat_root" in addrs else "0x0"

print(
    f"[build-fs] BOOT_STORAGE={storage} ptab={ptab.name} "
    f"LFS@{fs_base} size={fal.fmt_addr(fs_size)} "
    f"KV@{kv_base} size={fal.fmt_addr(kv_size)} "
    f"FAT@{fat_base} size={fal.fmt_addr(fat_size)}"
    f"{seed_note}",
    file=sys.stderr,
)
print(f"FS_BASE={fs_base}")
print(f"FS_SIZE={fs_size:#x}")
print(f"KV_BASE={kv_base}")
print(f"KV_SIZE={kv_size:#x}")
print(f"FAT_BASE={fat_base}")
print(f"FAT_SIZE={fat_size:#x}")
print(f"FS_STORAGE={storage}")
print(f"HAS_KV={1 if 'kv_root' in addrs and kv_size else 0}")
print(f"HAS_FAT={1 if 'fat_root' in addrs else 0}")
PY
)"

if [[ "${FS_STORAGE}" == "nand" ]]; then
  LFS_PROG=2048
  LFS_BLOCK=131072
else
  LFS_PROG=512
  LFS_BLOCK=4096
fi

if [[ ! -d "${LFS_CACHE}" ]]; then
  echo "=== Cloning littlefs ${LFS_VER} ==="
  git clone --depth 1 --branch "${LFS_VER}" \
    https://github.com/littlefs-project/littlefs.git "${LFS_CACHE}"
fi

mkdir -p "${MKLFS_BUILD}" "${BIN_DIR}"
gcc -O2 -Wall -Wextra -I"${LFS_CACHE}" \
  "${MYV}/scripts/mklfs_disk/mklfs_disk.c" \
  "${LFS_CACHE}/lfs.c" \
  "${LFS_CACHE}/lfs_util.c" \
  -o "${MKLFS}"

pack_lfs() {
  local tree="$1" out="$2" size="$3" base="$4" sparse_dir="$5" sparse_man="$6" label="$7"
  if [[ ! -d "${tree}" ]]; then
    echo "ERROR: missing ${label} tree: ${tree}" >&2
    exit 1
  fi
  echo "=== Packing LittleFS ${label} from ${tree} (size=${size}," \
       "read/prog=${LFS_PROG}, block=${LFS_BLOCK}) ==="
  "${MKLFS}" -c "${tree}" -i "${out}" -s "${size}" \
    -r "${LFS_PROG}" -b "${LFS_BLOCK}"
  ls -lh "${out}"
  python3 "${MYV}/scripts/gen_fs_root_sparse.py" \
    --image "${out}" \
    --base-addr "${base}" \
    --block-size "${LFS_BLOCK}" \
    --out-dir "${sparse_dir}" \
    --manifest "${sparse_man}"
}

LFS_TREE="${STAGING}/lfs"
if [[ ! -d "${LFS_TREE}" ]]; then
  echo "ERROR: ${STAGING}/lfs missing — mkfs must contain kv/, lfs/, fat/" >&2
  exit 1
fi

pack_lfs "${LFS_TREE}" "${BIN_DIR}/fs_root.bin" "${FS_SIZE}" "${FS_BASE}" \
  "${BIN_DIR}/fs_root_sparse" "${BIN_DIR}/fs_root.sparse.json" "lfs"

if [[ "${HAS_KV}" == "1" ]]; then
  KV_TREE="${STAGING}/kv"
  mkdir -p "${KV_TREE}/fw"
  pack_lfs "${KV_TREE}" "${BIN_DIR}/kv_root.bin" "${KV_SIZE}" "${KV_BASE}" \
    "${BIN_DIR}/kv_root_sparse" "${BIN_DIR}/kv_root.sparse.json" "kv"
fi

if [[ "${HAS_FAT}" == "1" && "${FS_STORAGE}" != "nand" ]]; then
  if [[ "${BUILD_FAT}" != "1" ]]; then
    echo "Skipping fat_root.bin (BUILD_FAT=${BUILD_FAT}; use build-fs / build-all)"
  else
    FAT_TREE="${STAGING}/fat"
    mkdir -p "${FAT_TREE}/map" "${FAT_TREE}/fonts"
    if compgen -G "${FAT_TREE}/fonts/*.ttf" > /dev/null; then
      echo "Including vector fonts on FAT volume:"
      ls -lh "${FAT_TREE}"/fonts/*.ttf
    else
      echo "WARN: mkfs/fat/fonts/*.ttf empty — /mnt/fat/fonts will miss TTF" >&2
    fi
    echo "=== Packing FAT32 from ${FAT_TREE} (size=${FAT_SIZE}) ==="
    python3 "${MYV}/scripts/pack_fat_root.py" \
      -c "${FAT_TREE}" -i "${BIN_DIR}/fat_root.bin" -s "${FAT_SIZE}" -n FAT
    ls -lh "${BIN_DIR}/fat_root.bin"
  fi
fi

echo "Flash: python3 vendor/my_vendor/build_board.py pack-sd-img / burn-sd"
echo "  (sparse segments; fat is remainder, lfs is 512 MiB LittleFS)"
