#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Resolve NAND flash images + addresses from ptab.json / sftool_param.json.

Build writes a snapshot to cmake_out/<board>_<cfg>/flasher_args.json (IDF-style).
Flash reads flasher_args.json first, else recomputes from boot_loader config.
"""

from __future__ import annotations

import json
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

# Host pack-sd-img target card size when a region max_size=0 (FAT remainder).
# Typical marketed-16GB CSD (14832 MiB). FAT seed = card - 1 GiB ≈ 13.48 GiB.
# A 15 GiB pack will not mount on these cards (BPB totsec > window → EINVAL).
# Override with env SD_PACK_CARD_MIB or ptab FAT_REGION custom.SD_PACK_CARD_MIB.
SD_PACK_CARD_MIB_DEFAULT = 14832
SD_PACK_ERASE_ALIGN = 4096

FLASHER_ARGS_NAME = "flasher_args.json"
FS_ROOT_BIN_NAME = "fs_root.bin"
FS_ROOT_SPARSE_NAME = "fs_root.sparse.json"
KV_ROOT_BIN_NAME = "kv_root.bin"
KV_ROOT_SPARSE_NAME = "kv_root.sparse.json"
FAT_ROOT_BIN_NAME = "fat_root.bin"
FAT_ROOT_SPARSE_NAME = "fat_root.sparse.json"
FACTORY_BIN_NAME = "factory.bin"
BOOT_BIN_NAMES = ("ftab.bin", "bootloader.bin")
VOLUME_BIN_NAMES = (FS_ROOT_BIN_NAME, KV_ROOT_BIN_NAME, FAT_ROOT_BIN_NAME)
NUTTX_BIN_NAME = "nuttx.bin"
NUTTX_FLASH_BIN_NAME = "nuttx.flash.bin"
DEFAULT_FACTORY_BOARD_CONFIG = (
    "vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh-factory"
)

# sftool_param path basename -> ptab.json "img" key
_PATH_PTAB_IMG: dict[str, str] = {
    "ftab.bin": "ftab",
    "bootloader.bin": "bootloader",
    "nuttx.bin": "main",
    "fs_root.bin": "fs_root",
    "kv_root.bin": "kv_root",
    "fat_root.bin": "fat_root",
    "factory.bin": "factory",
}

# 三种启动介质各有一份分区表（boot_loader/config/nsh/）：
#   ptab.nand.json   — SPI NAND（flash2@0x62000000，ftab@0 稀疏块对齐）
#   ptab.sdmmc.json  — SD/eMMC（sd@0x62000000，MBR@0 + ftab@0x1000 紧凑排布）
#   ptab.nor.json    — 片内/外 NOR XIP（flash2@0x12000000，预留）
PTAB_NAND_NAME = "ptab.nand.json"
PTAB_SDMMC_NAME = "ptab.sdmmc.json"
PTAB_NOR_NAME = "ptab.nor.json"

PTAB_BY_STORAGE: dict[str, str] = {
    "nand": PTAB_NAND_NAME,
    "sd": PTAB_SDMMC_NAME,
    "emmc": PTAB_SDMMC_NAME,
    "nor": PTAB_NOR_NAME,
}


def is_fs_flash_entry(entry: dict) -> bool:
    """True for fs_root.bin or sparse segments expanded from it."""
    if entry.get("ptab_img") == "fs_root":
        return True
    name = Path(str(entry.get("filename") or entry.get("path") or "")).name
    return name == FS_ROOT_BIN_NAME


def is_kv_flash_entry(entry: dict) -> bool:
    """True for kv_root.bin or sparse segments expanded from it."""
    if entry.get("ptab_img") == "kv_root":
        return True
    name = Path(str(entry.get("filename") or entry.get("path") or "")).name
    return name == KV_ROOT_BIN_NAME


def is_fat_flash_entry(entry: dict) -> bool:
    """True for fat_root.bin or sparse segments expanded from it."""
    if entry.get("ptab_img") == "fat_root":
        return True
    name = Path(str(entry.get("filename") or entry.get("path") or "")).name
    return name == FAT_ROOT_BIN_NAME


def is_volume_flash_entry(entry: dict) -> bool:
    """True for kv/lfs/fat packed images or their sparse segments."""
    return (
        is_fs_flash_entry(entry)
        or is_kv_flash_entry(entry)
        or is_fat_flash_entry(entry)
    )


def is_fs_flash_path(path: str) -> bool:
    """True for fs_root.bin or sparse segment file paths."""
    p = path.replace("\\", "/")
    if Path(p).name == FS_ROOT_BIN_NAME:
        return True
    return "fs_root_sparse" in p


def is_kv_flash_path(path: str) -> bool:
    p = path.replace("\\", "/")
    if Path(p).name == KV_ROOT_BIN_NAME:
        return True
    return "kv_root_sparse" in p


def is_fat_flash_path(path: str) -> bool:
    p = path.replace("\\", "/")
    if Path(p).name == FAT_ROOT_BIN_NAME:
        return True
    return "fat_root_sparse" in p


def is_factory_flash_entry(entry: dict) -> bool:
    """True for the factory firmware slot (ptab img factory / factory.bin)."""
    if entry.get("ptab_img") == "factory":
        return True
    name = Path(str(entry.get("filename") or entry.get("path") or "")).name
    return name == FACTORY_BIN_NAME


def is_factory_flash_path(path: str) -> bool:
    """True for factory.bin or cmake_out/...nsh-factory/nuttx.flash.bin."""
    p = path.replace("\\", "/")
    name = Path(path).name
    if name == FACTORY_BIN_NAME:
        return True
    return "nsh-factory" in p and name in (NUTTX_FLASH_BIN_NAME, NUTTX_BIN_NAME)


def factory_out_dir(
    root: Path,
    factory_board_config: str = DEFAULT_FACTORY_BOARD_CONFIG,
) -> Path:
    """Factory variant cmake dir: cmake_out/my_vendor_nsh-factory/."""
    cfg = (root / factory_board_config.strip().rstrip("/")).resolve()
    return root / "cmake_out" / f"{cfg.parent.parent.name}_{cfg.name}"


def boot_config_dir(root: Path, boot_config: str) -> Path:
    p = Path(boot_config.strip())
    return p if p.is_absolute() else (root / p).resolve()


def boot_bin_dir(root: Path, boot_bin_dir_spec: str) -> Path:
    p = Path(boot_bin_dir_spec.strip())
    return p if p.is_absolute() else (root / p).resolve()


def load_ptab_img_addresses(ptab_path: Path) -> dict[str, int]:
    """Map ptab region ``img`` name -> absolute flash address (int)."""
    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    addrs: dict[str, int] = {}
    for mem in mems:
        base = int(mem["base"], 16)
        for reg in mem.get("regions", []):
            img = reg.get("img")
            if not img:
                continue
            offset = int(reg["offset"], 16)
            addrs[img] = base + offset
    return addrs


def load_ptab_img_sizes(ptab_path: Path) -> dict[str, int]:
    """Map ptab region ``img`` name -> max_size bytes (0 = remainder of medium)."""
    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    sizes: dict[str, int] = {}
    for mem in mems:
        for reg in mem.get("regions", []):
            img = reg.get("img")
            if not img:
                continue
            sizes[str(img)] = int(reg["max_size"], 0)
    return sizes


def fs_region_is_remainder(ptab_path: Path) -> bool:
    """True when FS_REGION max_size is 0 (runtime: rest of the SD card)."""
    return load_ptab_img_sizes(ptab_path).get("fs_root", -1) == 0


def fat_region_is_remainder(ptab_path: Path) -> bool:
    """True when FAT_REGION max_size is 0 (runtime: rest of the SD card)."""
    return load_ptab_img_sizes(ptab_path).get("fat_root", -1) == 0


def ptab_img_offset(ptab_path: Path, img: str) -> int:
    """Byte offset of ``img`` within the storage mem block."""
    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    for mem in mems:
        for reg in mem.get("regions", []):
            if reg.get("img") == img:
                return int(reg["offset"], 0)
    raise RuntimeError(f"ptab 中无 {img}: {ptab_path}")


def ptab_fs_offset(ptab_path: Path) -> int:
    """FS_REGION byte offset within the storage mem block."""
    return ptab_img_offset(ptab_path, "fs_root")


def sd_pack_remainder_offset(ptab_path: Path) -> int:
    """Card offset of the remainder window (FAT, else legacy FS)."""
    sizes = load_ptab_img_sizes(ptab_path)
    if sizes.get("fat_root", -1) == 0:
        return ptab_img_offset(ptab_path, "fat_root")
    if sizes.get("fs_root", -1) == 0:
        return ptab_fs_offset(ptab_path)
    raise RuntimeError(f"ptab 中无余量分区: {ptab_path}")


def ptab_sd_pack_card_mib(ptab_path: Path) -> int | None:
    """FAT_REGION custom.SD_PACK_CARD_MIB, if present."""
    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    for mem in mems:
        for reg in mem.get("regions", []):
            tags = [str(t) for t in (reg.get("tags") or [])]
            if reg.get("img") != "fat_root" and "FAT_REGION" not in tags:
                continue
            custom = reg.get("custom") or {}
            raw = custom.get("SD_PACK_CARD_MIB")
            if raw is None:
                return None
            return int(raw, 0) if isinstance(raw, str) else int(raw)
    return None


def sd_pack_card_bytes(ptab_path: Path | None = None) -> int:
    """Logical SD image size for pack-sd-img (default 14832 MiB / ~13.5 GiB FAT)."""
    raw = os.environ.get("SD_PACK_CARD_MIB")
    if raw:
        try:
            mib = int(raw, 0)
        except ValueError as exc:
            raise RuntimeError(f"invalid SD_PACK_CARD_MIB={raw!r}") from exc
    else:
        ptab_mib = ptab_sd_pack_card_mib(ptab_path) if ptab_path is not None else None
        mib = ptab_mib if ptab_mib is not None else SD_PACK_CARD_MIB_DEFAULT
    if mib < 1:
        raise RuntimeError(f"SD_PACK_CARD_MIB must be >= 1, got {mib}")
    return mib * 1024 * 1024


def sd_pack_seed_bytes(ptab_path: Path) -> int:
    """Remainder seed size for a pack-sd-img card: card capacity minus remainder offset."""
    off = sd_pack_remainder_offset(ptab_path)
    card = sd_pack_card_bytes(ptab_path)
    if card <= off:
        raise RuntimeError(
            f"SD_PACK_CARD_MIB card {card} bytes is not larger than "
            f"remainder offset {off:#x}"
        )
    seed = card - off
    seed -= seed % SD_PACK_ERASE_ALIGN
    if seed < 4 * SD_PACK_ERASE_ALIGN:
        raise RuntimeError(f"SD pack seed {seed} too small")
    return seed


def print_ptab_build_summary(
    ptab_path: Path,
    storage: str,
    *,
    stage: str = "build",
) -> None:
    """Print boot-chain flash addresses from a ptab.*.json (build / flash logs)."""
    if not ptab_path.is_file():
        print(f"[PTAB/{stage}] ERROR: missing {ptab_path}", file=sys.stderr)
        return

    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    mem0 = mems[0] if mems else {}
    mem_name = str(mem0.get("mem", "?"))
    mem_base = int(mem0.get("base", 0), 16) if mem0 else 0
    addrs = load_ptab_img_addresses(ptab_path)
    sftool_m = _STORAGE_TO_SFTOOL_MEM.get(storage.lower(), storage)

    bar = "=" * 72
    print(bar)
    print(f"[PTAB/{stage}] BOOT_STORAGE={storage}  sftool -m {sftool_m}")
    print(f"[PTAB/{stage}] json={ptab_path.name}")
    print(f"[PTAB/{stage}] path={ptab_path.resolve()}")
    print(f"[PTAB/{stage}] storage mem={mem_name}  base={fmt_addr(mem_base)}")
    print(f"[PTAB/{stage}] boot chain (img -> SBUS address, 烧录/ROM 查表用):")
    for img in (
        "ftab",
        "bootloader",
        "main",
        "factory",
        "dfu",
        "kv_root",
        "fs_root",
        "fat_root",
    ):
        if img in addrs:
            print(f"[PTAB/{stage}]   {img:<12} @ {fmt_addr(addrs[img])}")
    if "ftab" in addrs and "bootloader" in addrs:
        print(
            f"[PTAB/{stage}] SFBL 读 ftab @ {fmt_addr(addrs['ftab'])}，"
            f"再加载 2SFBL @ {fmt_addr(addrs['bootloader'])}"
        )
    print(bar)


def fmt_addr(addr: int) -> str:
    return f"0x{addr:08X}"


def nuttx_raw_bin(out: Path) -> Path:
    return out / NUTTX_BIN_NAME


def nuttx_flash_bin(out: Path) -> Path:
    return out / NUTTX_FLASH_BIN_NAME


def nuttx_firmware_for_flash(out: Path) -> Path:
    wrapped = nuttx_flash_bin(out)
    if wrapped.is_file():
        return wrapped
    return nuttx_raw_bin(out)


def resolve_image_path(
    path_spec: str,
    *,
    root: Path,
    out: Path,
    boot_bin: Path,
) -> Path:
    name = Path(path_spec).name
    if name in (*BOOT_BIN_NAMES, *VOLUME_BIN_NAMES):
        return (boot_bin / name).resolve()
    if name == FACTORY_BIN_NAME:
        return nuttx_firmware_for_flash(factory_out_dir(root)).resolve()
    if name == NUTTX_BIN_NAME:
        return nuttx_firmware_for_flash(out).resolve()
    p = Path(path_spec)
    if p.is_absolute():
        return p.resolve()
    for candidate in (out / path_spec, boot_bin / path_spec):
        if candidate.is_file():
            return candidate.resolve()
    return (out / path_spec).resolve()


def _ptab_img_for_item(item: dict) -> str | None:
    if "ptab_img" in item:
        return str(item["ptab_img"])
    return _PATH_PTAB_IMG.get(Path(item["path"]).name)


def _address_for_item(item: dict, ptab_addrs: dict[str, int]) -> str:
    ptab_img = _ptab_img_for_item(item)
    if ptab_img and ptab_img in ptab_addrs:
        return fmt_addr(ptab_addrs[ptab_img])
    if "address" in item:
        return str(item["address"])
    name = Path(item["path"]).name
    raise KeyError(
        f"无法解析烧录地址: {name}（在 ptab 中无对应 img，"
        f"sftool_param 也未提供 address/ptab_img）"
    )


def load_sftool_param(param_path: Path) -> dict:
    return json.loads(param_path.read_text(encoding="utf-8"))


# storage.conf 取值 -> sftool 的 -m 介质类型（sftool 仅支持 nor/nand/sd）。
# eMMC 与 SD 共用 SDIO/SDMMC 控制器，sftool 无 emmc 选项，统一用 sd 烧录。
# 与 boot_loader/storage.conf、build.sh、board/board.c 的 BOOT_DEFAULT_FROM_* 同源。
_STORAGE_TO_SFTOOL_MEM: dict[str, str] = {
    "nor": "nor",
    "nand": "nand",
    "sd": "sd",
    "emmc": "sd",
}


def boot_storage_value(cfg_dir: Path) -> str | None:
    """读取 boot_loader/storage.conf 的 BOOT_STORAGE 原值（nand|sd|emmc）。

    cfg_dir 为 boot 配置目录（config/nsh）；storage.conf 位于其上两级
    （boot_loader/）。返回 None 表示未配置。
    """
    storage_conf = cfg_dir.parent.parent / "storage.conf"
    if not storage_conf.is_file():
        return None
    value: str | None = None
    for line in storage_conf.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*BOOT_STORAGE\s*=\s*[\"']?([A-Za-z]+)", line)
        if m:
            value = m.group(1).lower()  # 最后一条生效
    return value


def boot_storage_memory(cfg_dir: Path) -> str | None:
    """映射 BOOT_STORAGE -> sftool -m 介质类型。

    返回 None 表示未配置或未知值，调用方应回退到 sftool_param.json 的 memory。
    """
    value = boot_storage_value(cfg_dir)
    if value is None:
        return None
    return _STORAGE_TO_SFTOOL_MEM.get(value)


def resolve_ptab_path(cfg_dir: Path, storage: str | None = None) -> Path:
    """按 BOOT_STORAGE 选择 ptab.<medium>.json；缺失时回退 ptab.nand.json。"""
    value = (storage or boot_storage_value(cfg_dir) or "nand").lower()
    name = PTAB_BY_STORAGE.get(value, PTAB_NAND_NAME)
    candidate = cfg_dir / name
    if candidate.is_file():
        return candidate
    fallback = cfg_dir / PTAB_NAND_NAME
    if fallback.is_file():
        return fallback
    return candidate


def normalize_storage_medium(value: str | None) -> str | None:
    """Normalize CLI/storage.conf value to nand|sd|nor (emmc -> sd)."""
    if value is None:
        return None
    v = value.strip().lower()
    if v == "emmc":
        return "sd"
    if v in _STORAGE_TO_SFTOOL_MEM:
        return v
    raise ValueError(f"未知烧录介质: {value!r}（可选: nand, sd, emmc, nor）")


def build_flash_file_entries(
    *,
    root: Path,
    out: Path,
    boot_config: str,
    boot_bin_dir_spec: str,
    include_fs: bool = True,
    fs_only: bool = False,
    include_factory: bool = False,
    include_fat: bool = False,
    storage: str | None = None,
) -> tuple[str, str, list[dict]]:
    """Return chip, memory, and resolved file entries for flash.

    include_factory: pack-sd-img / flash-all. Daily flash leaves it False so a
    missing nsh-factory build does not fail product-only writes. NAND/NOR ptab
    has no factory img; those entries are skipped even when include_factory.

    include_fat: pack-sd-img only. UART sftool must never write FAT_REGION
    (thousands of sparse segments blow argv). Use burn-sd for /mnt/fat.
    """
    cfg = boot_config_dir(root, boot_config)
    boot_bin = boot_bin_dir(root, boot_bin_dir_spec)
    storage_key = normalize_storage_medium(storage)
    ptab_path = resolve_ptab_path(cfg, storage=storage_key)
    param_path = cfg / "sftool_param.json"
    if not ptab_path.is_file():
        raise FileNotFoundError(f"未找到分区表: {ptab_path}")
    if not param_path.is_file():
        raise FileNotFoundError(f"未找到烧录配置: {param_path}")

    data = load_sftool_param(param_path)
    ptab_addrs = load_ptab_img_addresses(ptab_path)
    ptab_sizes = load_ptab_img_sizes(ptab_path)

    def _seed_ok(bin_name: str, sparse_name: str) -> bool:
        return (boot_bin / bin_name).is_file() or (boot_bin / sparse_name).is_file()

    fs_seed = _seed_ok(FS_ROOT_BIN_NAME, FS_ROOT_SPARSE_NAME)
    kv_seed = _seed_ok(KV_ROOT_BIN_NAME, KV_ROOT_SPARSE_NAME)
    fat_seed = _seed_ok(FAT_ROOT_BIN_NAME, FAT_ROOT_SPARSE_NAME)
    fs_remainder = ptab_sizes.get("fs_root", -1) == 0
    # Remainder windows are sized at runtime from CSD. A host seed (~13.5 GiB FAT)
    # is optional: include it when asked and the file exists.
    skip_fs = fs_remainder and not (include_fs and fs_seed)
    # FAT remainder is pack-sd-img / burn-sd only. Never UART.
    skip_fat = (not include_fat) or not fat_seed
    chip = data["chip"]
    memory = str(data["memory"]).lower()
    # storage.conf 为单一选择源：构建侧决定 2 级 bootloader 启动介质，烧录侧
    # 据此切换 sftool -m，避免两处分别维护（sftool 支持 nor/nand/sd）。
    # CLI --flash-medium 显式覆盖时优先。
    if storage_key:
        memory = _STORAGE_TO_SFTOOL_MEM[storage_key]
    else:
        storage_mem = boot_storage_memory(cfg)
        if storage_mem:
            memory = storage_mem
    entries: list[dict] = []

    for item in data.get("write_flash", {}).get("files", []):
        path_name = Path(item["path"]).name
        ptab_img = _ptab_img_for_item(item)
        is_fs = path_name == FS_ROOT_BIN_NAME or ptab_img == "fs_root"
        is_kv = path_name == KV_ROOT_BIN_NAME or ptab_img == "kv_root"
        is_fat = path_name == FAT_ROOT_BIN_NAME or ptab_img == "fat_root"
        is_factory = path_name == FACTORY_BIN_NAME or ptab_img == "factory"
        if ptab_img in ("kv_root", "fat_root", "factory") and ptab_img not in ptab_addrs:
            continue
        if is_fs and skip_fs:
            continue
        if is_fat and skip_fat:
            continue
        if is_kv and not kv_seed:
            continue
        if is_fat and not fat_seed:
            continue
        if fs_only:
            if not is_fs:
                continue
        elif (is_fs or is_kv or is_fat) and not include_fs:
            continue
        elif is_factory and not include_factory:
            continue

        resolved = resolve_image_path(
            item["path"], root=root, out=out, boot_bin=boot_bin,
        )
        address = _address_for_item(item, ptab_addrs)
        # 地址完全来自所选分区表：SD/eMMC 用 ptab.sdmmc.json，ftab 天然在 0x1000。
        entries.append(
            {
                "filename": path_name,
                "path": str(resolved),
                "address": address,
                "ptab_img": ptab_img,
                "exists": resolved.is_file(),
            }
        )

    if not entries:
        if skip_fs and (fs_only or not include_fs):
            return chip, memory, []
        if skip_fs and include_fs and not fs_only:
            raise RuntimeError(
                "烧录文件列表为空（boot 镜像缺失，且 FS_REGION 为卡剩余空间、不烧 fs_root）"
            )
        raise RuntimeError("烧录文件列表为空（检查 include_fs / fs_only 过滤）")

    entries = expand_fs_sparse_entries(entries, boot_bin)
    return chip, memory, entries


def entries_to_write_flash(entries: list[dict]) -> list[str]:
    return [f"{e['path']}@{e['address']}" for e in entries]


def fs_root_sparse_manifest(boot_bin: Path) -> Path:
    return boot_bin / FS_ROOT_SPARSE_NAME


# FAT is burn-sd only. A nationwide map has tens of thousands of extents;
# exploding fat_root.bin into seg_*.bin races with --repack and blows pack.
# pack-sd-img copies fat_root.bin with SEEK_DATA instead.
_VOLUME_SPARSE = (
    (FS_ROOT_BIN_NAME, "fs_root", FS_ROOT_SPARSE_NAME),
    (KV_ROOT_BIN_NAME, "kv_root", KV_ROOT_SPARSE_NAME),
)


def _expand_one_sparse(
    entries: list[dict],
    boot_bin: Path,
    filename: str,
    ptab_img: str,
    manifest_name: str,
) -> list[dict]:
    manifest = boot_bin / manifest_name
    if not manifest.is_file():
        return entries

    data = json.loads(manifest.read_text(encoding="utf-8"))
    segments = data.get("segments") or []
    if not segments:
        return entries

    out: list[dict] = []
    expanded = False
    for entry in entries:
        if entry.get("filename") != filename:
            out.append(entry)
            continue

        try:
            base = int(str(entry.get("address", "0")), 0)
        except ValueError:
            base = int(str(data.get("base_addr", "0")), 0)

        expanded = True
        for seg in segments:
            seg_path = Path(seg["file"])
            img_off = int(seg.get("image_offset", 0))
            out.append(
                {
                    "filename": seg_path.name,
                    "path": str(seg_path.resolve()),
                    "address": fmt_addr(base + img_off),
                    "ptab_img": ptab_img,
                    "exists": seg_path.is_file(),
                    "sparse": True,
                    "image_offset": img_off,
                    "size": seg.get("size"),
                }
            )

    if not expanded:
        return entries

    missing = [
        e["path"]
        for e in out
        if e.get("sparse") and e.get("ptab_img") == ptab_img and not e.get("exists", True)
    ]
    if missing:
        shown = missing[:8]
        extra = f"\n  ... 另有 {len(missing) - len(shown)} 个" if len(missing) > len(shown) else ""
        raise RuntimeError(
            f"{manifest_name} 指向的段文件缺失，请重新 build-fs:\n"
            + "\n".join(f"  - {p}" for p in shown)
            + extra
        )

    payload = int(data.get("payload_bytes", 0))
    total = int(data.get("image_size", 0))
    print(
        f"[flash] {ptab_img} sparse: {len(segments)} segments, "
        f"payload={payload} / image={total} bytes",
        file=sys.stderr,
    )
    return out


def expand_fs_sparse_entries(
    entries: list[dict],
    boot_bin: Path,
    *,
    force_full: bool = False,
) -> list[dict]:
    """Replace packed volume images with sparse segment entries when manifests exist."""
    if force_full:
        return entries

    out = entries
    for filename, ptab_img, manifest_name in _VOLUME_SPARSE:
        out = _expand_one_sparse(out, boot_bin, filename, ptab_img, manifest_name)
    return out


STUB_WARMUP_ERASE_SIZE = 0x200


def stub_warmup_erase_region(
    root: Path,
    boot_config: str,
    *,
    storage: str | None = None,
) -> str:
    """Return ``0xADDR:0xSIZE`` for a 512 B erase inside unused boot pad.

    USB-CDC (``/dev/ttyACM*``) needs sftool ``--compat true`` to download the
    RAM stub (Debug-IP 64 KiB frames time out).  That same flag also chunks
    ``write_flash`` payload at 256 B + 10 ms, ~20 KB/s.  A tiny erase in
    BOOT_RESERVE lets one sftool session download the stub with compat, then a
    second session ``--before no_reset_no_sync --compat false`` writes at the
    normal 80–100 KB/s.
    """
    cfg = boot_config_dir(root, boot_config)
    ptab_path = resolve_ptab_path(cfg, storage=normalize_storage_medium(storage))
    if not ptab_path.is_file():
        raise FileNotFoundError(f"未找到分区表: {ptab_path}")

    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    fallback: str | None = None
    for mem in mems:
        base = int(mem["base"], 16)
        for reg in mem.get("regions", []):
            size = int(reg.get("max_size", "0"), 16)
            if size < STUB_WARMUP_ERASE_SIZE:
                continue
            tags = [str(t) for t in (reg.get("tags") or [])]
            off = int(reg["offset"], 16)
            addr = base + off + size - STUB_WARMUP_ERASE_SIZE
            spec = f"0x{addr:08X}:0x{STUB_WARMUP_ERASE_SIZE:X}"
            if "BOOT_RESERVE" in tags:
                return spec
            if (
                fallback is None
                and not reg.get("img")
                and not reg.get("ftab")
                and "MBR" not in tags
                and "FS_REGION" not in tags
            ):
                fallback = spec
    if fallback:
        return fallback
    raise RuntimeError(f"ptab 中无 BOOT_RESERVE / 空垫区可供 stub 预热: {ptab_path}")


def fs_region_erase_region(
    root: Path,
    boot_config: str,
    *,
    storage: str | None = None,
) -> str:
    """Return sftool erase_region argument ``0xADDR:0xSIZE`` for fs_root / FS_REGION."""
    cfg = boot_config_dir(root, boot_config)
    ptab_path = resolve_ptab_path(cfg, storage=normalize_storage_medium(storage))
    if not ptab_path.is_file():
        raise FileNotFoundError(f"未找到分区表: {ptab_path}")

    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    for mem in mems:
        base = int(mem["base"], 16)
        for reg in mem.get("regions", []):
            if reg.get("img") != "fs_root":
                continue
            addr = base + int(reg["offset"], 16)
            size = int(reg["max_size"], 16)
            if size == 0:
                raise RuntimeError(
                    f"FS_REGION max_size=0 (remainder of card); "
                    f"cannot serial-erase whole LFS: {ptab_path}"
                )
            return f"0x{addr:08X}:0x{size:08X}"

    raise RuntimeError(f"ptab 中无 fs_root 分区: {ptab_path}")


def flash_args_include_fs(flash_args: list[str]) -> bool:
    """True when write_flash list contains fs_root.bin or sparse segments."""
    for spec in flash_args:
        path_part = spec.split("@", 1)[0]
        if is_fs_flash_path(path_part):
            return True
    return False


def reject_uart_fat_args(flash_args: list[str]) -> None:
    """UART sftool must not pass FAT remainder segments (argv / time)."""
    fat = [s for s in flash_args if is_fat_flash_path(s.split("@", 1)[0])]
    if not fat:
        return
    raise RuntimeError(
        "FAT 余量卷（/mnt/fat）不能走串口 sftool："
        f"清单里有 {len(fat)} 个 fat_root 段，会撑爆命令行。\n"
        "请用 pack-sd-img / burn-sd 写地图和字体。"
    )


def write_flasher_args(
    *,
    root: Path,
    out: Path,
    boot_config: str,
    boot_bin_dir_spec: str,
    dest: Path | None = None,
) -> Path:
    """Write flasher_args.json snapshot into the CMake output directory."""
    cfg = boot_config_dir(root, boot_config)
    dest = dest or (out / FLASHER_ARGS_NAME)
    chip, memory, entries = build_flash_file_entries(
        root=root,
        out=out,
        boot_config=boot_config,
        boot_bin_dir_spec=boot_bin_dir_spec,
        include_fs=True,
        fs_only=False,
        include_factory=True,
        include_fat=False,
    )
    payload = {
        "version": 1,
        "chip": chip,
        "memory": memory,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "sources": {
            "ptab": str(resolve_ptab_path(cfg).resolve()),
            "sftool_param": str((cfg / "sftool_param.json").resolve()),
        },
        "cmake_out": str(out.resolve()),
        "files": entries,
        "write_flash": entries_to_write_flash(entries),
    }
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")
    return dest


def _filter_entries(
    entries: list[dict],
    *,
    include_fs: bool,
    fs_only: bool,
    include_factory: bool = False,
    include_fat: bool = False,
) -> list[dict]:
    out: list[dict] = []
    for e in entries:
        is_fs = is_fs_flash_entry(e)
        is_kv = is_kv_flash_entry(e)
        is_fat = is_fat_flash_entry(e)
        is_factory = is_factory_flash_entry(e)
        # UART never writes the FAT remainder volume (4935+ argv slots).
        if is_fat and not include_fat:
            continue
        if fs_only:
            if is_fs:
                out.append(e)
            continue
        if (is_fs or is_kv) and not include_fs:
            continue
        if is_factory and not include_factory:
            continue
        out.append(e)
    return out


def load_flash_args(
    *,
    root: Path,
    out: Path,
    boot_config: str,
    boot_bin_dir_spec: str,
    include_fs: bool = True,
    fs_only: bool = False,
    include_factory: bool = False,
    include_fat: bool = False,
    storage: str | None = None,
) -> tuple[str, str, list[str]]:
    """Load flash argv: prefer out/flasher_args.json, else live from config.

    include_fat stays False for every UART path. pack-sd-img calls
    build_flash_file_entries(include_fat=True) directly.
    """
    storage_key = normalize_storage_medium(storage)
    flasher = out / FLASHER_ARGS_NAME
    if flasher.is_file() and storage_key is None:
        data = json.loads(flasher.read_text(encoding="utf-8"))
        snapshot = data.get("files", [])
        # Pre-factory snapshots omit the slot; recompute so flash-all/pack
        # actually write factory instead of silently skipping it.
        snapshot_ok = True
        if include_factory and not any(
            is_factory_flash_entry(e) for e in snapshot
        ):
            snapshot_ok = False
        if snapshot_ok:
            chip = data["chip"]
            memory = str(data["memory"]).lower()
            entries = _filter_entries(
                snapshot,
                include_fs=include_fs,
                fs_only=fs_only,
                include_factory=include_factory,
                include_fat=include_fat,
            )
            if entries:
                boot_bin = boot_bin_dir(root, boot_bin_dir_spec)
                entries = expand_fs_sparse_entries(entries, boot_bin)
                return chip, memory, entries_to_write_flash(entries)

    chip, memory, entries = build_flash_file_entries(
        root=root,
        out=out,
        boot_config=boot_config,
        boot_bin_dir_spec=boot_bin_dir_spec,
        include_fs=include_fs,
        fs_only=fs_only,
        include_factory=include_factory,
        include_fat=include_fat,
        storage=storage_key,
    )
    return chip, memory, entries_to_write_flash(entries)
