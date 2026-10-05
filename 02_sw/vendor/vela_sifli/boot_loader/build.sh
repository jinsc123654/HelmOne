#!/usr/bin/env bash
# Build NAND boot chain for sf32lb52-nano_a128r16:
#   - bootloader: project/butterflmicro/ram_v2 (vendored scons)
#   - ftab.bin:   SiFli SDK flash_table + config/nsh/ptab.<medium>.json
#                 Boot copy size = ptab max_size (see board/main.c boot_img_copy_len)
#
# Config: boot_loader/config/nsh/{ptab.nand.json, ptab.sdmmc.json, boot.json, ...}
# Output: boot_loader/bin/{ftab.bin, bootloader.bin}
#
# Usage:
#   ./build.sh
#   ./build.sh help
#   ./build.sh --bootloader-only
#   SIFLI_SDK=/path/to/SDK/2.4 ./build.sh --no-prompt

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENVELA_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BOOT_CONFIG_DIR="${SCRIPT_DIR}/config/nsh"
STORAGE_CONF="${SCRIPT_DIR}/storage.conf"
BOOT_BIN_DIR="${BOOT_BIN_DIR:-${SCRIPT_DIR}/bin}"
BOOT_SCRIPTS="${SCRIPT_DIR}/scripts"
LOAD_CFG="${BOOT_SCRIPTS}/load_boot_config.py"
GEN_FTAB="${BOOT_SCRIPTS}/gen_ftab.py"
GEN_PTAB_TABLE="${BOOT_SCRIPTS}/gen_ptab_table.py"
PRINT_PTAB="${BOOT_SCRIPTS}/print_ptab_summary.py"
HELP_SH="${BOOT_SCRIPTS}/print_nand_boot_help.sh"

SDK_BOARD="$(python3 "${LOAD_CFG}" "${BOOT_CONFIG_DIR}" sdk_board)"
BOOT_PROJ_REL="$(python3 "${LOAD_CFG}" "${BOOT_CONFIG_DIR}" boot_loader_project)"
FTAB_PROJ_REL="$(python3 "${LOAD_CFG}" "${BOOT_CONFIG_DIR}" sdk_flash_table)"
NUTTX_OUT_DEFAULT="${OPENVELA_ROOT}/$(python3 "${LOAD_CFG}" "${BOOT_CONFIG_DIR}" nuttx_output)"
PTAB_SRC="${BOOT_CONFIG_DIR}/ptab.nand.json"
BOOT_PROJ="${SCRIPT_DIR}/${BOOT_PROJ_REL}"
BUILD_TAG="build_${SDK_BOARD}_hcpu"
DEFAULT_SDK="/home/jinsc/SDK/SiFli/SDK/2.4"
NUTTX_OUT_DIR="${NUTTX_OUT_DEFAULT}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 8)}"
NO_PROMPT=0
BOOTLOADER_ONLY=0
# 默认启动存储介质：环境变量优先，否则读取 storage.conf，最终回退 nand。
BOOT_STORAGE="${BOOT_STORAGE:-}"

usage() {
  cat <<EOF
用法: $(basename "$0") [选项]

  构建 NAND 启动镜像，安装到 boot_loader/bin/（供 build_board.py flash 使用）。

配置文件 ${STORAGE_CONF}:
  BOOT_STORAGE=nand|sd|emmc|nor
      选择 2 级 bootloader 默认启动存储介质。build.sh 据此向 bootloader 注入
      BOOT_DEFAULT_FROM_* 宏（board/board.c::board_boot_from 固定启动源）。
      可用环境变量临时覆盖本文件: 请直接编辑本文件（build.sh 以 storage.conf 为唯一来源，
      不再读取 shell 残留的 BOOT_STORAGE 环境变量）。

配置目录 ${BOOT_CONFIG_DIR}/:
  boot.json
      本脚本的构建描述（sdk_board、SDK flash_table 路径、nuttx 输出目录等）。
      build.sh 启动时通过 load_boot_config.py 读取，一般无需手改。

  ptab.nand.json / ptab.sdmmc.json / ptab.nor.json
      三种启动介质的分区表（由 storage.conf 的 BOOT_STORAGE 自动选用）。
      构建时同步所选表到 \$SIFLI_SDK/customer/boards/<sdk_board>/ptab.json，
      并用于 gen_ftab.py 生成 ftab.c → ftab.bin。
      另由 gen_ptab_table.py 解析为 project/butterflmicro/board/ptab_table.{c,h}
      （全分区 C 数组，含 FS_REGION/KVDB/DFU_* 等不进 ftab.bin 的区域），随
      bootloader 一起编译，供 2SFBL 运行期查询（ptab_find_img / ptab_find_tag）。
      改分区布局后必须重新完整构建（不能只烧 nuttx）。

  sftool_param.json
      UART 烧录清单（chip、memory、各 bin 的 NAND 地址）。
      由 build_board.py flash 读取；镜像路径解析为:
        ftab.bin / bootloader.bin → boot_loader/bin/
        nuttx.bin                 → cmake_out/my_vendor_nsh/（或 --nuttx-dir）

  jlink_nand.conf
      J-Link 调试/烧录参考配置（SF32LB52X_NAND），可选；
      日常烧录用 sftool，不经过本 build.sh。

工程与产物:
  工程目录  ${SCRIPT_DIR}/project/     vendored bootloader (scons)
  输出目录  ${BOOT_BIN_DIR}/           ftab.bin, bootloader.bin
  nuttx     ${NUTTX_OUT_DEFAULT}/nuttx.bin  （须先 build_board.py build）

选项:
  -j, --jobs N          并行编译任务数 (默认: ${BUILD_JOBS})
      --nuttx-dir DIR   nuttx.bin 所在目录 (默认: ${NUTTX_OUT_DEFAULT})
      --bootloader-only 只编 bootloader.bin，不生成 ftab.bin
      --no-prompt       不交互询问 SIFLI_SDK
  -h, --help, help      显示此帮助

openvela 封装: python3 vendor/my_vendor/build_board.py build-boot
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -j|--jobs)
      BUILD_JOBS="$2"
      shift 2
      ;;
    --nuttx-dir)
      NUTTX_OUT_DIR="$2"
      shift 2
      ;;
    --bootloader-only)
      BOOTLOADER_ONLY=1
      shift
      ;;
    --no-prompt)
      NO_PROMPT=1
      shift
      ;;
    -h|--help|help)
      usage
      exit 0
      ;;
    *)
      echo "未知参数: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

NUTTX_BIN="${NUTTX_OUT_DIR}/nuttx.bin"

load_storage_config() {
  # 单一来源：storage.conf（勿依赖 shell 里残留的 BOOT_STORAGE 环境变量）。
  if [[ -f "${STORAGE_CONF}" ]]; then
    BOOT_STORAGE="$(sed -n 's/^[[:space:]]*BOOT_STORAGE[[:space:]]*=[[:space:]]*"\{0,1\}'"'"'\{0,1\}\([A-Za-z]*\).*/\1/p' "${STORAGE_CONF}" | tail -n1)"
  fi
  BOOT_STORAGE="${BOOT_STORAGE:-nand}"
  BOOT_STORAGE="$(echo "${BOOT_STORAGE}" | tr '[:upper:]' '[:lower:]')"
  case "${BOOT_STORAGE}" in
    nand|sd|emmc|nor) ;;
    *)
      echo "ERROR: 非法 BOOT_STORAGE='${BOOT_STORAGE}'（应为 nand|sd|emmc|nor），见 ${STORAGE_CONF}" >&2
      exit 1
      ;;
  esac
  # 按 BOOT_STORAGE 选用 ptab.<medium>.json（NAND/SD/eMMC/NOR 布局互不相同）。
  case "${BOOT_STORAGE}" in
    sd|emmc)
      PTAB_SRC="${BOOT_CONFIG_DIR}/ptab.sdmmc.json"
      ;;
    nor)
      PTAB_SRC="${BOOT_CONFIG_DIR}/ptab.nor.json"
      ;;
    *)
      PTAB_SRC="${BOOT_CONFIG_DIR}/ptab.nand.json"
      ;;
  esac
  if [[ ! -f "${PTAB_SRC}" ]]; then
    echo "ERROR: 分区表不存在: ${PTAB_SRC}（BOOT_STORAGE=${BOOT_STORAGE}）" >&2
    exit 1
  fi
  # 导出给 bootloader 工程 board/SConscript，注入 BOOT_DEFAULT_FROM_* 宏。
  export BOOT_STORAGE
}

print_ptab_layout() {
  local stage="${1:-build-boot}"
  python3 "${PRINT_PTAB}" "${BOOT_STORAGE}" "${PTAB_SRC}" "${stage}"
}

prompt_sifli_sdk() {
  local input
  if [[ -n "${SIFLI_SDK:-}" ]]; then
    echo "当前 SIFLI_SDK=${SIFLI_SDK}"
    read -r -p "按 Enter 使用当前值，或输入新的 SDK 路径: " input
    if [[ -n "${input}" ]]; then
      SIFLI_SDK="${input}"
    fi
  else
    echo "需要 SiFli SDK 2.4 路径（含 export.sh）。"
    read -r -p "请输入 SIFLI_SDK [${DEFAULT_SDK}]: " input
    SIFLI_SDK="${input:-${DEFAULT_SDK}}"
  fi
  export SIFLI_SDK
}

resolve_sdk_root() {
  if [[ -z "${SIFLI_SDK:-}" ]]; then
    for candidate in \
      "${OPENVELA_ROOT}/../SiFli/SDK/2.4" \
      "${HOME}/SDK/SiFli/SDK/2.4" \
      "${DEFAULT_SDK}"; do
      if [[ -d "${candidate}" && -f "${candidate}/export.sh" ]]; then
        SIFLI_SDK="${candidate}"
        break
      fi
    done
  fi
  if [[ ! -d "${SIFLI_SDK:-}" ]]; then
    echo "ERROR: SIFLI_SDK 未设置且未找到 SDK。" >&2
    exit 1
  fi
  if [[ ! -f "${SIFLI_SDK}/export.sh" ]]; then
    echo "ERROR: 未找到 ${SIFLI_SDK}/export.sh" >&2
    exit 1
  fi
  export SIFLI_SDK
}

sync_ptab() {
  local ptab_dst="${SIFLI_SDK}/customer/boards/${SDK_BOARD}/ptab.json"
  print_ptab_layout "sync-ptab"
  cp -f "${PTAB_SRC}" "${ptab_dst}"
  echo "[PTAB/sync-ptab] SDK copy -> ${ptab_dst}"
}

# 把本次启动选用的 ptab.json 解析成 C 数组，编译进二级 bootloader。
# 与 ftab.bin 不同，这张表含所有分区（FS_REGION / KVDB / DFU_* 等），让 2SFBL
# 拥有完整、与所烧存储一致的分区视图（board/ptab_table.{c,h} 由 board/SConscript
# 的 Glob('*.c') 自动编译；头文件在同目录、已在 CPPPATH 上）。
gen_ptab_table() {
  local board_dir
  board_dir="$(dirname "${BOOT_PROJ}")/board"
  echo "=== [0/3] Generating ptab_table.c/.h (compiled-in partition table) ==="
  print_ptab_layout "gen-ptab-table"
  python3 "${GEN_PTAB_TABLE}" \
    --ptab "${PTAB_SRC}" \
    --storage "${BOOT_STORAGE}" \
    --out-c "${board_dir}/ptab_table.c" \
    --out-h "${board_dir}/ptab_table.h"
}

# scons timestamps a 0-byte .o as "already built". An interrupted compile
# (or a sandbox truncate) leaves empty mbedtls/pin_const objects; the link
# then reports undefined mbedtls_pk_* / pin_pad_func_*. Drop them so they
# are rebuilt.
purge_empty_objs() {
  local dir="$1"
  local n=0
  [[ -d "${dir}" ]] || return 0
  while IFS= read -r -d '' f; do
    rm -f "${f}"
    n=$((n + 1))
  done < <(find "${dir}" \( -name '*.o' -o -name '*.obj' \) -size 0 -print0 2>/dev/null)
  if [[ "${n}" -gt 0 ]]; then
    echo "  removed ${n} empty object file(s) under ${dir}"
  fi
}

build_bootloader() {
  echo "=== [1/3] Building bootloader (${BOOT_PROJ_REL}) ==="
  purge_empty_objs "${BOOT_PROJ}/${BUILD_TAG}"
  (
    cd "${BOOT_PROJ}"
    scons --board="${SDK_BOARD}" -j"${BUILD_JOBS}"
  )

  local boot_bin="${BOOT_PROJ}/${BUILD_TAG}/main.bin"
  if [[ ! -f "${boot_bin}" ]]; then
    echo "ERROR: bootloader not found: ${boot_bin}" >&2
    exit 1
  fi

  mkdir -p "${BOOT_BIN_DIR}"
  cp -f "${boot_bin}" "${BOOT_BIN_DIR}/bootloader.bin"
  echo "  -> ${BOOT_BIN_DIR}/bootloader.bin ($(stat -c%s "${BOOT_BIN_DIR}/bootloader.bin") bytes)"
}

build_ftab() {
  if [[ ! -f "${NUTTX_BIN}" ]]; then
    echo "ERROR: ${NUTTX_BIN} not found." >&2
    echo "请先: build_board.py build" >&2
    exit 1
  fi

  local ftab_proj="${SIFLI_SDK}/${FTAB_PROJ_REL}"
  local ftab_build="${ftab_proj}/${BUILD_TAG}"
  local ftab_c="${ftab_build}/board/ftab.c"
  local boot_bin="${BOOT_PROJ}/${BUILD_TAG}/main.bin"

  echo "=== [2/3] Generating ftab.c (ptab partition copy sizes) ==="
  print_ptab_layout "gen-ftab"
  python3 "${GEN_FTAB}" \
    --ptab "${PTAB_SRC}" \
    --out "${ftab_c}" \
    --main "${NUTTX_BIN}" \
    --bootloader "${boot_bin}"

  echo "=== [3/3] Building ftab.bin (SDK ${FTAB_PROJ_REL}) ==="
  purge_empty_objs "${ftab_build}"
  (
    cd "${ftab_proj}"
    scons --board="${SDK_BOARD}" -j"${BUILD_JOBS}"
  )

  local ftab_bin="${ftab_build}/main.bin"
  if [[ ! -f "${ftab_bin}" ]]; then
    echo "ERROR: ftab not found: ${ftab_bin}" >&2
    exit 1
  fi

  cp -f "${ftab_bin}" "${BOOT_BIN_DIR}/ftab.bin"
  echo "  -> ${BOOT_BIN_DIR}/ftab.bin ($(stat -c%s "${BOOT_BIN_DIR}/ftab.bin") bytes)"
  verify_ftab_bin "${BOOT_BIN_DIR}/ftab.bin"
}

# ftab.bin 内 ftab[0].base 必须与 BOOT_STORAGE 所选 ptab 一致（防 SD/NAND 混编）。
verify_ftab_bin() {
  local ftab_bin="$1"
  local expect_ftab
  local b4 b5 b6 b7

  if [[ ! -f "${ftab_bin}" ]]; then
    return 0
  fi

  # sec_configuration: magic@0, ftab[0].base@4 (little-endian)
  b4="$(od -An -tx1 -N1 -j4 "${ftab_bin}" | tr -d ' ')"
  b5="$(od -An -tx1 -N1 -j5 "${ftab_bin}" | tr -d ' ')"
  b6="$(od -An -tx1 -N1 -j6 "${ftab_bin}" | tr -d ' ')"
  b7="$(od -An -tx1 -N1 -j7 "${ftab_bin}" | tr -d ' ')"

  case "${BOOT_STORAGE}" in
    sd|emmc)
      expect_ftab="00 10 00 62"
      ;;
    nor)
      expect_ftab="00 00 00 12"
      ;;
    *)
      expect_ftab="00 00 00 62"
      ;;
  esac

  if [[ "${b4} ${b5} ${b6} ${b7}" != "${expect_ftab}" ]]; then
    echo "ERROR: ftab.bin ftab[0].base=0x${b7}${b6}${b5}${b4} 与 BOOT_STORAGE=${BOOT_STORAGE} 不匹配" >&2
    echo "       期望 0x$(echo "${expect_ftab}" | awk '{print $4$3$2$1}')（storage.conf=${BOOT_STORAGE}, ptab=$(basename "${PTAB_SRC}")）" >&2
    echo "       常见原因：此前用 SD 布局编过 ftab，请确认 storage.conf 后重新 build-boot" >&2
    exit 1
  fi
  echo "  ftab[0].base OK for BOOT_STORAGE=${BOOT_STORAGE} ($(basename "${PTAB_SRC}"))"
}

main() {
  load_storage_config

  if [[ "${NO_PROMPT}" -eq 0 ]]; then
    prompt_sifli_sdk
  fi
  resolve_sdk_root

  echo "SIFLI_SDK=${SIFLI_SDK}"
  echo "BOOT_CONFIG=${BOOT_CONFIG_DIR}"
  echo "BOOT_STORAGE=${BOOT_STORAGE}  (见 ${STORAGE_CONF})"
  echo "PTAB_SRC=${PTAB_SRC}"
  print_ptab_layout "build-boot"
  echo "BOOT_PROJ=${BOOT_PROJ}"
  echo "BOOT_BIN_DIR=${BOOT_BIN_DIR}"
  echo "NUTTX_BIN=${NUTTX_BIN}"

  if [[ ! -f "${BOOT_PROJ}/SConstruct" ]]; then
    echo "ERROR: 缺少 SConstruct: ${BOOT_PROJ}/SConstruct" >&2
    exit 1
  fi

  # shellcheck disable=SC1091
  source "${SIFLI_SDK}/export.sh"

  sync_ptab
  gen_ptab_table
  build_bootloader

  if [[ "${BOOTLOADER_ONLY}" -eq 0 ]]; then
    build_ftab
  fi

  # 烧录地址随 BOOT_STORAGE 选用的分区表而变（见 PTAB_SRC）。
  local addr_ftab addr_bl addr_main
  case "${BOOT_STORAGE}" in
    sd|emmc)
      addr_ftab="0x62001000"; addr_bl="0x62011000"; addr_main="0x62100000" ;;
    nor)
      addr_ftab="0x12000000"; addr_bl="0x12010000"; addr_main="0x12020000" ;;
    *)
      addr_ftab="0x62000000"; addr_bl="0x62080000"; addr_main="0x620A0000" ;;
  esac

  echo ""
  echo "Build OK — ${BOOT_BIN_DIR}/  (BOOT_STORAGE=${BOOT_STORAGE}, ptab=$(basename "${PTAB_SRC}"))"
  echo "  bootloader.bin @ ${addr_bl}"
  if [[ "${BOOTLOADER_ONLY}" -eq 0 ]]; then
    echo "  ftab.bin       @ ${addr_ftab}"
    echo "  nuttx.bin      @ ${addr_main}  [${NUTTX_BIN}]"
  fi

  if [[ -f "${HELP_SH}" ]]; then
    # shellcheck disable=SC1090
    source "${HELP_SH}"
    if [[ "${BOOTLOADER_ONLY}" -eq 1 ]]; then
      print_nand_boot_help bootloader "${OPENVELA_ROOT}" "${BOOT_BIN_DIR}" \
        "${BOOT_PROJ}/${BUILD_TAG}/main.bin"
    else
      print_nand_boot_help full "${OPENVELA_ROOT}" "${BOOT_BIN_DIR}" "${NUTTX_OUT_DIR}"
    fi
  fi
}

main "$@"
