#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pack a directory tree into a sparse FAT32 image (/mnt/fat).

Host tools: dosfstools ``mkfs.vfat`` + mtools ``mcopy``. Names that
fit 8.3 (``map``, ``fonts``, ``lon861``) are stored as uppercase short
names without an LFN; the device needs ``CONFIG_FAT_LCNAMES`` so
``/mnt/fat/map`` still matches ``MAP``. Tile files such as
``x3443_y858.vpk`` (16 chars) also need ``CONFIG_FAT_MAXFNAME>=32``:
NuttX ``fs_fat32.h`` treats an unset ``FAT_MAXFNAME`` as 0 and clamps
it to 12, so ``stat``/``fopen`` of those names fails while ``ls`` of
8.3 directories still works. The image is pre-truncated so unused
clusters stay sparse; gen_fs_root_sparse.py then extracts only
allocated extents for pack-sd-img.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

_SECTOR = 512
# Largest first: fewer FAT entries when the tree is small. Nationwide maps
# have hundreds of thousands of small .vpk files; 32 KiB clusters waste a
# full cluster per file (~18 GiB for 580k tiles) and overflow a 14 GiB volume.
_SPC_CANDIDATES = (64, 32, 16, 8)  # 32 / 16 / 8 / 4 KiB


def _require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(
            f"未找到 {name}。请安装 dosfstools 与 mtools（mkfs.vfat / mcopy）。"
        )
    return path


def _tree_stats(src: Path) -> tuple[list[int], int]:
    sizes: list[int] = []
    ndirs = 0
    for dirpath, _dirs, names in os.walk(src):
        ndirs += 1
        for name in names:
            if name == ".gitkeep":
                continue
            sizes.append((Path(dirpath) / name).stat().st_size)
    return sizes, ndirs


def _alloc_estimate(sizes: list[int], ndirs: int, cluster: int, volume: int) -> int:
    alloc = 0
    for sz in sizes:
        if sz <= 0:
            continue
        alloc += ((sz + cluster - 1) // cluster) * cluster
    # Each dir at least one cluster; LFN names such as x3443_y858.vpk ~160 B.
    dir_bytes = ndirs * cluster + len(sizes) * 160
    dir_bytes = ((dir_bytes + cluster - 1) // cluster) * cluster
    nclus = max(1, volume // cluster)
    fat = nclus * 4 * 2
    return alloc + dir_bytes + fat


def _choose_spc(volume: int, sizes: list[int], ndirs: int) -> int:
    """Largest cluster that still leaves ~10% free on this tree."""
    usable = volume - volume // 10
    last_need = 0
    for spc in _SPC_CANDIDATES:
        cluster = _SECTOR * spc
        need = _alloc_estimate(sizes, ndirs, cluster, volume)
        last_need = need
        if need <= usable:
            print(
                f"[pack-fat] {len(sizes)} files {ndirs} dirs, "
                f"{cluster // 1024} KiB clusters "
                f"(est {need / (1024 ** 3):.2f} / {volume / (1024 ** 3):.2f} GiB)",
                file=sys.stderr,
            )
            return spc
    raise RuntimeError(
        f"FAT 装不下: {len(sizes)} 个文件估算 {last_need / (1024 ** 3):.2f} GiB，"
        f"卷只有 {volume / (1024 ** 3):.2f} GiB（4 KiB 簇仍不足）。"
        f"请加大 SD_PACK_CARD_MIB。"
    )


class _Bar:
    """stderr progress: TTY redraws in place; logs get a line every 5%."""

    def __init__(self, total: int, prefix: str = "[pack-fat]") -> None:
        self.total = max(int(total), 1)
        self.prefix = prefix
        self.t0 = time.monotonic()
        self.last_pct = -1

    def update(self, done: int, label: str = "", *, final: bool = False) -> None:
        done = min(max(int(done), 0), self.total)
        pct = done * 100 // self.total
        if not final and not sys.stderr.isatty() and pct < self.last_pct + 5:
            return
        self.last_pct = pct
        width = 28
        fill = width * done // self.total
        bar = "#" * fill + "-" * (width - fill)
        elapsed = time.monotonic() - self.t0
        eta = ""
        if 0 < done < self.total:
            remain = elapsed * (self.total - done) / done
            if remain >= 60:
                eta = f"  eta {int(remain // 60)}m{int(remain % 60):02d}s"
            else:
                eta = f"  eta {remain:0.0f}s"
        extra = f"  {label}" if label else ""
        line = (
            f"{self.prefix} [{bar}] {pct:3d}%  {done}/{self.total}{extra}{eta}"
        )
        if sys.stderr.isatty():
            print(f"\r{line:<120}", file=sys.stderr, end="" if not final else "\n", flush=True)
        else:
            print(line, file=sys.stderr, flush=True)


def _mtools_env() -> dict[str, str]:
    env = os.environ.copy()
    env["MTOOLS_SKIP_CHECK"] = "1"
    return env


def _run_mcopy(
    mcopy: str, image: Path, srcs: list[Path], dest_spec: str, env: dict[str, str]
) -> None:
    cmd = [mcopy, "-i", str(image), "-s", "-o"]
    cmd.extend(str(p) for p in srcs)
    cmd.append(dest_spec)
    subprocess.run(cmd, check=True, env=env)


def _copy_map_with_progress(
    mcopy: str, mmd: str, image: Path, map_dir: Path, env: dict[str, str]
) -> None:
    """Copy map/lon* in batches so a nationwide tree shows a live bar."""
    children = sorted(p for p in map_dir.iterdir() if p.name != ".gitkeep")
    subprocess.run([mmd, "-i", str(image), "::/MAP"], check=True, env=env)
    if not children:
        print("[pack-fat] map/ empty", file=sys.stderr)
        return

    # ~8 lon dirs per spawn: enough updates, not one exec per folder.
    batch = 8
    bar = _Bar(len(children))
    for i in range(0, len(children), batch):
        chunk = children[i : i + batch]
        _run_mcopy(mcopy, image, chunk, "::/MAP/", env)
        bar.update(min(i + len(chunk), len(children)), chunk[-1].name)
    bar.update(len(children), "map done", final=True)


def pack_fat_root(src: Path, dest: Path, size: int, *, label: str = "FAT") -> None:
    if size < 32 * 1024 * 1024:
        raise RuntimeError(f"FAT32 镜像过小: {size} bytes")
    if not src.is_dir():
        raise RuntimeError(f"源目录不存在: {src}")

    mkfs = _require_tool("mkfs.vfat")
    mcopy = _require_tool("mcopy")
    mmd = _require_tool("mmd")
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        dest.unlink()

    print("[pack-fat] scanning tree...", file=sys.stderr, flush=True)
    sizes, ndirs = _tree_stats(src)
    spc = _choose_spc(size, sizes, ndirs) if sizes else 64

    with dest.open("wb") as fh:
        fh.truncate(size)

    # Firmware classifies FatSz16==0 as FAT32 (vela_override/fs_fat32util.c).
    subprocess.run(
        [mkfs, "-F", "32", "-S", str(_SECTOR), "-s", str(spc), "-n", label[:11], str(dest)],
        check=True,
    )

    entries = [
        p
        for p in src.iterdir()
        if p.name not in (".", "..") and p.name != ".gitkeep"
    ]
    if not entries:
        print(f"[pack-fat] empty tree {src} -> {dest} ({size} bytes)", file=sys.stderr)
        return

    env = _mtools_env()
    map_dir = src / "map"
    others = [p for p in sorted(entries) if p.name != "map"]
    for p in others:
        print(f"[pack-fat] mcopy {p.name} -> ::/", file=sys.stderr, flush=True)
        _run_mcopy(mcopy, dest, [p], "::/", env)

    if map_dir.is_dir():
        print(
            f"[pack-fat] mcopy map/ ({sum(1 for _ in map_dir.iterdir() if _.name != '.gitkeep')} lon*)",
            file=sys.stderr,
            flush=True,
        )
        _copy_map_with_progress(mcopy, mmd, dest, map_dir, env)
    elif any(p.name == "map" for p in entries):
        _run_mcopy(mcopy, dest, [src / "map"], "::/", env)


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack a tree into a FAT32 image")
    parser.add_argument("-c", "--src", type=Path, required=True)
    parser.add_argument("-i", "--image", type=Path, required=True)
    parser.add_argument("-s", "--size", required=True, help="image size in bytes (int or 0x..)")
    parser.add_argument("-n", "--label", default="FAT")
    args = parser.parse_args()
    size = int(args.size, 0)
    pack_fat_root(args.src, args.image, size, label=args.label)
    st = args.image.stat()
    try:
        on_disk = os.stat(args.image).st_blocks * 512
    except (AttributeError, OSError):
        on_disk = st.st_size
    print(
        f"[pack-fat] wrote {args.image} logical={st.st_size} on-disk~{on_disk}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
