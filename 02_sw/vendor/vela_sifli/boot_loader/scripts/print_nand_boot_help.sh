#!/usr/bin/env bash
# Print NAND boot artifact usage (sourced by boot_loader/build.sh).

print_nand_boot_help() {
  local mode="${1:-full}"
  local openvela_root="${2:?openvela root required}"
  local boot_bin_dir="${3:-${openvela_root}/vendor/my_vendor/boot_loader/bin}"
  local extra="${4:-}"

  local nuttx_out_dir="${openvela_root}/cmake_out/my_vendor_nsh"
  local boot_src=""
  if [[ "${mode}" == "bootloader" ]]; then
    boot_src="${extra}"
  elif [[ -n "${extra}" ]]; then
    nuttx_out_dir="${extra}"
  fi

  # 启动介质（由 build.sh 导出 BOOT_STORAGE：nand|sd|emmc|nor）。SBUS 基址因介质而异：
  #   NAND  (ptab.nand.json)   flash2@0x62000000，ftab@0、bootloader@0x80000、main@0xA0000
  #   SD/eMMC(ptab.sdmmc.json)  sd@0x62000000，MBR@0 + ftab@0x1000 + bl@0x11000 + pad-to-1MiB + main@0x100000
  #   NOR   (ptab.nor.json)    flash2@0x12000000，ftab@0、bootloader@0x10000、main@0x20000（XIP）
  local storage="${BOOT_STORAGE:-nand}"
  local stg_label ftab_addr bl_addr main_addr stg_note
  case "${storage}" in
    sd)
      stg_label="SD"
      ftab_addr="0x62001000"; bl_addr="0x62011000"; main_addr="0x62100000"
      stg_note="SD 启动: 分区表 ptab.sdmmc.json（MBR@0 + ftab@0x1000）；烧录 -m sd。boot 区留 1MiB（代码 160KiB + BOOT_RESERVE），main@1MiB，KV 256MiB，coredump 128MiB；用户 LFS 从 512MiB 起到卡尾。"
      ;;
    emmc)
      stg_label="eMMC"
      ftab_addr="0x62001000"; bl_addr="0x62011000"; main_addr="0x62100000"
      stg_note="eMMC 启动: 分区表 ptab.sdmmc.json；烧录 -m sd（共用 SDMMC）。"
      ;;
    nor)
      stg_label="NOR"
      ftab_addr="0x12000000"; bl_addr="0x12010000"; main_addr="0x12020000"
      stg_note="NOR 启动: 分区表 ptab.nor.json（XIP@0x12000000）；烧录 -m nor。"
      ;;
    *)
      stg_label="NAND"
      ftab_addr="0x62000000"; bl_addr="0x62080000"; main_addr="0x620A0000"
      stg_note="NAND 启动: 分区表 ptab.nand.json（ftab@0 稀疏块对齐）；烧录 -m nand。"
      ;;
  esac

  cat <<EOF

================================================================================
${stg_label} 启动镜像说明 (sf32lb52-nano_a128r16, BOOT_STORAGE=${storage})
================================================================================

启动链: Mask ROM → SFBL → 二级 Bootloader → OVNX 校验 → NuttX @ PSRAM 0x10000000

App 打包 (见 scripts/nuttx_ovnx_image.md):
  app_version.txt → [0..1KiB] 文件头(构建时间/版本/整段长度)
  [1024..] payload → PSRAM dest+0；[末尾4B] 仅 payload CRC32
  boot: 先 UART 打印元数据，再 CRC 校验(成败都打印)后 run_img

镜像目录:
  boot 链:  ${boot_bin_dir}/
  nuttx:    ${nuttx_out_dir}/nuttx.bin + nuttx.flash.bin

  文件              ${stg_label} 地址      来源
  ----------------  -------------  ------------------------------------------
  ftab.bin          ${ftab_addr}     boot_loader/build.sh (SDK flash_table + ptab)
  bootloader.bin    ${bl_addr}     boot_loader/project → bin/
  nuttx.flash.bin   ${main_addr}     vela_my_vendor_tools.py build/flash (非 nuttx.bin)
${stg_note:+
注意: ${stg_note}}
EOF

  if [[ "${mode}" == "bootloader" && -n "${boot_src}" ]]; then
    cat <<EOF
本次产物 (scons main.bin):
  ${boot_src}

已安装:
  ${boot_bin_dir}/bootloader.bin

完整烧录 (flash 自动从 boot_loader/bin 读取 ftab + bootloader):
  cd ${openvela_root}
  ./vela_my_vendor_tools.py build-boot    # 更新 bootloader（含 OVNX 校验）
  ./vela_my_vendor_tools.py build flash

仅烧 app 时须为 nuttx.flash.bin（带 OVNX trailer）；裸 nuttx.bin 会 OVNXFAIL。
二级 boot 启动 HCPU 前 UART 打印 build_date、payload_len、copy_len、payload_crc。

LittleFS /mnt/nad 在 FS_REGION 数据区（地址见所选 ptab），与上述启动镜像分区无关。
================================================================================
EOF
  else
    cat <<EOF
build-boot 已写入 ${boot_bin_dir}/:
  ftab.bin / bootloader.bin

推荐流程:
  ./vela_my_vendor_tools.py build          # nuttx.bin + 自动 nuttx.flash.bin
  ./vela_my_vendor_tools.py build-boot     # 改过 boot/main.c 或 ptab 时
  ./vela_my_vendor_tools.py flash

单独 wrap:
  ./vela_my_vendor_tools.py wrap
  vendor/my_vendor/scripts/wrap_nuttx_image.sh ${nuttx_out_dir}

SDK 路径: vela_my_vendor_tools.py 顶部 SIFLI_SDK
================================================================================
EOF
  fi
}
