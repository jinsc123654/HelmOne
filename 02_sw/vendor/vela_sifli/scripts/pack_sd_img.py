#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pack SD/eMMC boot images into a raw disk .img for Etcher / dd / Rufus (DD).

Card byte offset = SBUS address - mem base (0x62000000). Layout from
ptab.sdmmc.json: MBR@0 + ftab@0x1000 + 160KiB bootloader + pad-to-1MiB + nuttx
+ 4 MiB factory + 128MiB coredump + pad-to-256MiB + 256MiB KV LittleFS
+ 512 MiB user LittleFS + FAT map remainder at 1 GiB. Host pack-sd-img seeds
the map for a typical 16GB CSD (14832 MiB card, ~13.5 GiB FAT). Larger
cards leave unused map space until factory reformat. The .img is trimmed
to the last used byte (sparse).

The first 9 MiB (MBR + ftab + 128 KiB bootloader slot + 4 MiB main + 4 MiB
factory) is filled with 0xFF so ``dd conv=sparse`` cannot skip NUL gaps.
Those gaps were why a packed card failed to boot while sftool (file-accurate
writes) succeeded.

Prefer: python3 vela_my_vendor_tools.py burn-sd --sd /dev/sdX
"""

from __future__ import annotations

import argparse
import errno
import json
import os
import sys
import time
from pathlib import Path

SCRIPT = Path(__file__).resolve()
sys.path.insert(0, str(SCRIPT.parent))
import flash_args_lib as fal  # noqa: E402

# 相对**本树根**（VENDOR_ROOT = <vendor>/scripts/ 往上两级）—— 这棵树在 vendor/ 下
# 叫什么都行；解析统一由 flash_args_lib 的 boot_config_dir / boot_bin_dir 完成。
VENDOR_ROOT = SCRIPT.parent.parent
BOOT_CONFIG = "boot_loader/config/nsh"
BOOT_BIN_DIR = "boot_loader/bin"
DEFAULT_OUT_NAME = "my_vendor_sd.img"
SECTOR = 512
# Boot band 1 MiB + main 4 MiB + factory 4 MiB. 0xFF like erased flash.
BOOT_APP_DENSE_BYTES = 0x900000
FILL_CHUNK = 1024 * 1024


def _mem_base(ptab_path: Path) -> int:
    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    for mem in mems:
        name = str(mem.get("mem", "")).lower()
        if name in ("sd", "emmc", "sdmmc"):
            return int(mem["base"], 16)
    return int(mems[0]["base"], 16)


def _fs_region_end(ptab_path: Path) -> int:
    """Absolute SBUS address just past FS_REGION (or FS start if remainder)."""
    mems = json.loads(ptab_path.read_text(encoding="utf-8"))
    for mem in mems:
        base = int(mem["base"], 16)
        for reg in mem.get("regions", []):
            if reg.get("img") == "fs_root":
                return base + int(reg["offset"], 16) + int(reg["max_size"], 16)
    raise RuntimeError(f"ptab 中无 fs_root: {ptab_path}")


def _align_up(n: int, align: int) -> int:
    return (n + align - 1) // align * align


def _copy_sparse_into(src: Path, dst_fh, dst_off: int) -> int:
    """Copy allocated extents of ``src`` into ``dst_fh`` at ``dst_off``.

    Nationwide FAT images are 10+ GiB sparse; a linear read would write holes
    as zeros and balloon the .img. SEEK_DATA/SEEK_HOLE keeps both sparse.
    """
    size = src.stat().st_size
    copied = 0
    last_report = 0
    t0 = time.monotonic()
    show = size >= 64 * 1024 * 1024

    def _report(force: bool = False) -> None:
        nonlocal last_report
        if not show:
            return
        if not force and copied - last_report < 256 * 1024 * 1024:
            return
        last_report = copied
        pct = copied * 100 // size if size else 100
        elapsed = time.monotonic() - t0
        print(
            f"\r  + {src.name:<24} {copied / (1024**2):.0f}/{size / (1024**2):.0f} MiB "
            f"({pct:3d}%) {elapsed:0.0f}s",
            file=sys.stderr,
            end="",
            flush=True,
        )

    with src.open("rb") as fh:
        fd = fh.fileno()
        pos = 0
        used_seek = hasattr(os, "SEEK_DATA") and hasattr(os, "SEEK_HOLE")
        if used_seek:
            while pos < size:
                try:
                    data_off = os.lseek(fd, pos, os.SEEK_DATA)
                except OSError as exc:
                    if exc.errno in (errno.ENXIO, errno.EINVAL):
                        break
                    used_seek = False
                    break
                if data_off < 0 or data_off >= size:
                    break
                try:
                    hole_off = os.lseek(fd, data_off, os.SEEK_HOLE)
                except OSError as exc:
                    if exc.errno in (errno.ENXIO, errno.EINVAL):
                        hole_off = size
                    else:
                        used_seek = False
                        break
                if hole_off < 0 or hole_off > size:
                    hole_off = size
                length = hole_off - data_off
                fh.seek(data_off)
                dst_fh.seek(dst_off + data_off)
                left = length
                while left:
                    chunk = fh.read(min(1024 * 1024, left))
                    if not chunk:
                        raise RuntimeError(f"short read {src} @{data_off}")
                    dst_fh.write(chunk)
                    left -= len(chunk)
                    copied += len(chunk)
                    _report()
                pos = max(hole_off, data_off + 1)

        if not used_seek:
            fh.seek(0)
            dst_fh.seek(dst_off)
            copied = 0
            while True:
                chunk = fh.read(1024 * 1024)
                if not chunk:
                    break
                dst_fh.write(chunk)
                copied += len(chunk)
                _report()

    if show:
        _report(force=True)
        print(file=sys.stderr)
    return copied


def _fill_range(fh, offset: int, size: int, byte: int = 0xFF) -> None:
    """Write ``size`` bytes of ``byte`` at ``offset`` (no holes)."""
    if size <= 0:
        return
    chunk = bytes([byte]) * min(size, FILL_CHUNK)
    fh.seek(offset)
    left = size
    while left:
        n = min(left, len(chunk))
        fh.write(chunk[:n])
        left -= n


def _entry_span(entries: list[dict], mem_base: int) -> int:
    """Highest card offset covered by any payload (exclusive)."""
    end = 0
    for e in entries:
        path = Path(e["path"])
        size = int(e["size"]) if e.get("size") else path.stat().st_size
        abs_addr = int(str(e["address"]), 0)
        end = max(end, abs_addr - mem_base + size)
    return end


def _pack_entries(
    entries: list[dict],
    *,
    mem_base: int,
    img_path: Path,
    image_size: int,
    dense: bool,
) -> int:
    """Write entries; return total payload bytes written."""
    img_path.parent.mkdir(parents=True, exist_ok=True)
    if img_path.exists():
        img_path.unlink()

    payload = 0
    dense_end = min(BOOT_APP_DENSE_BYTES, image_size)
    with open(img_path, "wb") as fh:
        if dense:
            fh.truncate(image_size)
        elif image_size > 0:
            # Sparse file: one byte at the end creates the logical size.
            fh.seek(image_size - 1)
            fh.write(b"\0")

        # Materialize boot+main+factory so sparse dd cannot skip MBR/padding.
        _fill_range(fh, 0, dense_end, 0xFF)
        print(
            f"  + [boot+main+factory 0xFF fill] -> img+0x0 "
            f"({dense_end} bytes)",
            file=sys.stderr,
        )

        for e in entries:
            path = Path(e["path"])
            if not path.is_file():
                raise FileNotFoundError(f"缺少镜像: {path}")
            abs_addr = int(str(e["address"]), 0)
            if abs_addr < mem_base:
                raise RuntimeError(
                    f"地址 {e['address']} 低于 mem base {fal.fmt_addr(mem_base)}"
                )
            offset = abs_addr - mem_base
            file_size = path.stat().st_size
            end = offset + file_size
            if end > image_size:
                raise RuntimeError(
                    f"{path.name} @ {fal.fmt_addr(abs_addr)} 超出镜像 "
                    f"({end} > {image_size})"
                )
            copied = _copy_sparse_into(path, fh, offset)
            payload += copied
            print(
                f"  + {path.name:<24} -> img+{fal.fmt_addr(offset)} "
                f"({copied} bytes, sbus {e['address']})",
                file=sys.stderr,
            )
    return payload


def _verify_packed_ovnx(
    img_path: Path,
    entries: list[dict],
    mem_base: int,
    ptab_img: str,
) -> None:
    """Fail pack if the named slot is not a valid OVNX image."""
    slot = next((e for e in entries if e.get("ptab_img") == ptab_img), None)
    if slot is None:
        print(f"WARN: 镜像中无 {ptab_img}，未做 OVNX 校验", file=sys.stderr)
        return
    src = Path(slot["path"])
    size = src.stat().st_size
    offset = int(str(slot["address"]), 0) - mem_base
    with open(img_path, "rb") as fh:
        fh.seek(offset)
        data = fh.read(size)
    if len(data) != size:
        raise RuntimeError(
            f"pack 校验失败: {ptab_img} 只读到 {len(data)} / {size} bytes "
            f"@ {offset:#x}"
        )
    from wrap_nuttx_image import verify_image

    meta = verify_image(data)
    print(
        f"  OVNX {ptab_img:<8}: {src.name} @ img+{fal.fmt_addr(offset)} "
        f"v{meta.get('version')} {meta.get('build_date')} "
        f"crc=0x{meta.get('payload_crc', 0):08X} ({size} bytes)",
        file=sys.stderr,
    )


def pack_sd_img(
    root: Path,
    out_dir: Path,
    *,
    dest: Path | None = None,
    dense: bool = False,
    use_full_fs: bool = False,
    full_size: bool = False,
    card_mib: int | None = None,
    fat_image: Path | None = None,
) -> Path:
    """Build raw SD card image.

    Defaults:
      - LittleFS via sparse segments
      - .img trimmed to last used byte (sparse hole up to FS at 512 MiB)
      - FAT seed is 14 GiB (15 GiB card); runtime MTD window still follows CSD
    """
    if card_mib is not None:
        os.environ["SD_PACK_CARD_MIB"] = str(card_mib)

    cfg = fal.boot_config_dir(root, BOOT_CONFIG)
    storage = fal.boot_storage_value(cfg) or "nand"
    if storage not in ("sd", "emmc"):
        raise RuntimeError(
            f"当前 BOOT_STORAGE={storage}，pack-sd-img 仅用于 sd/emmc。"
            " 请先在 boot_loader/storage.conf 设 BOOT_STORAGE=sd"
        )

    ptab = fal.resolve_ptab_path(cfg, storage=storage)
    mem_base = _mem_base(ptab)
    fs_remainder = fal.fs_region_is_remainder(ptab)
    fat_remainder = fal.fat_region_is_remainder(ptab)
    remainder = fat_remainder or fs_remainder
    fs_region_bytes = _fs_region_end(ptab) - mem_base
    pack_card = fal.sd_pack_card_bytes(ptab) if remainder else fs_region_bytes
    seed_bytes = fal.sd_pack_seed_bytes(ptab) if remainder else 0

    chip, memory, entries = fal.build_flash_file_entries(
        root=root,
        out=out_dir,
        boot_config=BOOT_CONFIG,
        boot_bin_dir_spec=BOOT_BIN_DIR,
        include_fs=True,
        fs_only=False,
        include_factory=True,
        include_fat=True,
        storage=storage,
    )
    if memory != "sd":
        raise RuntimeError(f"内部错误: memory={memory}（期望 sd）")

    if fat_image is not None:
        fat_path = Path(fat_image).expanduser().resolve()
        if not fat_path.is_file():
            raise FileNotFoundError(f"未找到 FAT 镜像: {fat_path}")
        replaced = False
        for entry in entries:
            if entry.get("ptab_img") == "fat_root":
                entry["path"] = str(fat_path)
                entry["filename"] = fat_path.name
                entry["exists"] = True
                replaced = True
        if not replaced:
            addrs = fal.load_ptab_img_addresses(ptab)
            if "fat_root" not in addrs:
                raise RuntimeError("ptab 无 fat_root，无法嵌入 --fat")
            entries.append(
                {
                    "filename": fat_path.name,
                    "path": str(fat_path),
                    "address": fal.fmt_addr(addrs["fat_root"]),
                    "ptab_img": "fat_root",
                    "exists": True,
                }
            )

    if use_full_fs:
        boot_bin = fal.boot_bin_dir(root, BOOT_BIN_DIR)
        fs_bin = boot_bin / fal.FS_ROOT_BIN_NAME
        if not fs_bin.is_file():
            raise FileNotFoundError(
                f"未找到 {fs_bin}，请先: python3 vendor/my_vendor/build_board.py build-fs"
            )
        addrs = fal.load_ptab_img_addresses(ptab)
        fs_addr = fal.fmt_addr(addrs["fs_root"])
        packed: list[dict] = [
            e for e in entries if not fal.is_fs_flash_entry(e)
        ]
        packed.append(
            {
                "filename": fal.FS_ROOT_BIN_NAME,
                "path": str(fs_bin.resolve()),
                "address": fs_addr,
                "ptab_img": "fs_root",
                "exists": True,
            }
        )
        entries = packed
    elif not any(fal.is_fs_flash_entry(e) for e in entries):
        raise RuntimeError(
            "无 fs_root sparse 段，请先: "
            "python3 vendor/my_vendor/build_board.py build-fs"
        )

    content_end = _align_up(_entry_span(entries, mem_base), SECTOR)
    if content_end <= 0:
        raise RuntimeError("没有可打包的内容")

    if full_size:
        # Explicit full-card image (15 GiB by default). Not the pack default:
        # git-era images were content-trimmed (~payload); padding to 15 GiB
        # only makes ls -lh look huge (the file is still sparse).
        image_size = pack_card if remainder else fs_region_bytes
        size_mode = (
            f"{pack_card / (1024**3):.2f} GiB card"
            if remainder
            else "full FS_REGION"
        )
    else:
        image_size = content_end
        size_mode = "content-only (trim to last used byte)"

    dest = (dest or (root / BOOT_BIN_DIR / DEFAULT_OUT_NAME)).resolve()

    print(
        f"=== pack-sd-img chip={chip} storage={storage} "
        f"base={fal.fmt_addr(mem_base)} ===\n"
        f"  remainder ptab : "
        + (
            (
                f"{'FAT_REGION' if fat_remainder else 'FS_REGION'} "
                f"(runtime CSD); host seed "
                f"{seed_bytes / (1024**3):.2f} GiB for "
                f"{pack_card / (1024**3):.2f} GiB image\n"
            )
            if remainder
            else (
                f"{fs_region_bytes / (1024**3):.2f} GiB "
                f"({fs_region_bytes} bytes) — 运行时挂载几何，不必写入空块\n"
            )
        )
        + f"  image mode     : {size_mode}, "
        f"{'dense' if dense else 'sparse file'}\n"
        f"  image size     : {image_size} bytes "
        f"({image_size / (1024 * 1024):.1f} MiB)\n"
        f"  fs packing     : "
        + (
            f"volume seed geometry {seed_bytes / (1024**3):.2f} GiB "
            f"(15 GiB card / 14 GiB FAT); payload sparse, img trimmed"
            if remainder
            else ("full volume bins" if use_full_fs else "sparse kv/lfs/fat segments")
        ),
        file=sys.stderr,
    )
    fal.print_ptab_build_summary(ptab, storage, stage="pack-sd-img")
    if not any(e.get("ptab_img") == "factory" for e in entries):
        raise RuntimeError(
            "pack-sd-img 需要 factory 槽（ptab factory）。请先: "
            "python3 vendor/my_vendor/build_board.py build-all"
        )
    payload = _pack_entries(
        entries,
        mem_base=mem_base,
        img_path=dest,
        image_size=image_size,
        dense=dense,
    )
    _verify_packed_ovnx(dest, entries, mem_base, "main")
    _verify_packed_ovnx(dest, entries, mem_base, "factory")

    st = dest.stat()
    try:
        blocks = os.stat(dest).st_blocks * 512
    except (AttributeError, OSError):
        blocks = st.st_size
    print(
        f"\nWrote {dest}\n"
        f"  payload      : {payload} bytes "
        f"({payload / (1024 * 1024):.2f} MiB)\n"
        f"  logical size : {st.st_size} bytes "
        f"({st.st_size / (1024 * 1024):.1f} MiB)\n"
        f"  on-disk      : ~{blocks} bytes "
        f"({blocks / (1024 * 1024):.1f} MiB)\n"
        f"\n下一步：把镜像刻录到 SD 卡（确认设备节点，会覆盖卡头）\n"
        f"  python3 vela_my_vendor_tools.py burn-sd --sd /dev/sdX\n"
        f"  python3 vela_my_vendor_tools.py burn-sd --sd /dev/sdX --yes\n"
        f"  python3 vela_my_vendor_tools.py burn-sd --sd /dev/sdX --repack --yes\n"
          f"  # burn-sd 先密写前 9 MiB（boot+main+factory），再从 256 MiB 起 sparse 写 kv/lfs/fat\n"
          f"  # 或 balenaEtcher / Rufus DD / Win32 Disk Imager\n"
          f"\n说明: 前 9 MiB 为 0xFF 实心（ftab/bootloader/main/factory），避免 dd sparse 跳过 0 间隙。\n"
        f"      KV 256 MiB @ 256 MiB；用户 LFS 512 MiB @ 512 MiB；地图 FAT 从 1 GiB 起到卡末。\n"
        f"      15 GiB 打包卡的地图种子是 14 GiB；更大的卡需工厂重格式化才能用满余量。\n"
        f"      烧录卡须 ≥15 GiB（部分标称 16GB 卡只有 ~14.5 GiB，请用 32GB）。\n"
        f"      ls 看到的逻辑大小含空洞，du 才是实际占用。\n"
        f"      需要整卡 15 GiB 镜像时加 --full-size。"
        f"不要用 Ventoy「拷文件启动」。不带 --sd 运行 burn-sd 可列出候选盘。\n",
        file=sys.stderr,
    )
    return dest


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Pack SD boot firmware + LittleFS seed into a raw .img "
            "(default: trim to last used byte)"
        )
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=SCRIPT.parent.parent.parent.parent,
        help="openvela root",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="CMake out dir (for nuttx.flash.bin)",
    )
    parser.add_argument(
        "-o",
        "--image",
        type=Path,
        default=None,
        help=f"output .img path (default: boot_loader/bin/{DEFAULT_OUT_NAME})",
    )
    parser.add_argument(
        "--dense",
        action="store_true",
        help="pre-zero entire image (no holes; larger on disk)",
    )
    parser.add_argument(
        "--full-size",
        action="store_true",
        help="logical .img size = full 15 GiB card (or FS_REGION). "
             "Default trims to last used byte like the old ~10 MiB packs.",
    )
    parser.add_argument(
        "--card-mib",
        type=int,
        default=None,
        help="SD remainder image capacity in MiB (default 15360 = 15 GiB / "
             "14 GiB FAT, or ptab custom.SD_PACK_CARD_MIB / env)",
    )
    parser.add_argument(
        "--fat",
        type=Path,
        default=None,
        help="override fat_root.bin (e.g. empty 8GB fat_root_8g.bin)",
    )
    parser.add_argument(
        "--full-fs",
        action="store_true",
        help="embed entire fs_root.bin instead of sparse LFS segments",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    # 输出目录带本树目录名（本树在 vendor/my_vendor 时仍是 cmake_out/my_vendor_nsh）
    out = (args.out or (root / "cmake_out" / f"{VENDOR_ROOT.name}_nsh")).resolve()

    pack_sd_img(
        root,
        out,
        dest=args.image,
        dense=args.dense,
        use_full_fs=args.full_fs,
        full_size=args.full_size,
        card_mib=args.card_mib,
        fat_image=args.fat,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
