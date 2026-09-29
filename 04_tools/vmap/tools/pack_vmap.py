#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Pack per-tile VTIL blobs into size-limited .vpk shards for MCU / LittleFS.

Input (build tree):
  packed  <src>/<z>/tiles.idx + pNNN.vpk   (build_vmap default)
  loose   <src>/<z>/<x>/<y>.vt             (web tile cache)

Output (device map via pack_map.py): map.idx + rNNN.vpk
This module also writes per-zoom VTIX packs used as the convert intermediate.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

VTIL_MAGIC = b"VTIL"
VTIX_MAGIC = b"VTIX"
VPKG_MAGIC = b"VPKG"

VMAP_VERSION = 2
VMAP_TILES_INDEX_VERSION = 2
VMAP_TILES_INDEX_VERSION_V1 = 1
VMAP_PACK_VERSION = 1

VMAP_VPK_HDR_SIZE = 16
DEFAULT_PACK_BYTES = 500 * 1024

VMAP_TILES_INDEX_FILE = "tiles.idx"
VMAP_SHARD_EXT = "vpk"


@dataclass
class TileBlob:
    z: int
    x: int
    y: int
    data: bytes


def collect_packed_zoom(z_dir: Path, z: int) -> list[TileBlob]:
    """Read <z>/tiles.idx + pNNN.vpk (build_vmap packed output)."""
    idx_path = z_dir / VMAP_TILES_INDEX_FILE
    if not idx_path.is_file():
        return []
    raw = idx_path.read_bytes()
    if len(raw) < 8 or raw[:4] != VTIX_MAGIC:
        return []
    _ver, _z_file, count = struct.unpack_from("<BBH", raw, 4)
    recs: list[tuple[int, int, int, int, int]] = []
    pack_ids: set[int] = set()
    off = 8
    ver = raw[4]
    if ver >= VMAP_TILES_INDEX_VERSION:
        rec_fmt = "<HHHII"
    else:
        rec_fmt = "<HHHIH"
    rec_size = struct.calcsize(rec_fmt)
    for _ in range(count):
        if off + rec_size > len(raw):
            break
        x, y, pid, offset, size = struct.unpack_from(rec_fmt, raw, off)
        recs.append((x, y, pid, offset, size))
        pack_ids.add(pid)
        off += rec_size

    payloads: dict[int, bytes] = {}
    for pid in pack_ids:
        p = z_dir / f"p{pid:03d}.{VMAP_SHARD_EXT}"
        if not p.is_file():
            print(f"WARN: missing shard {p}", file=sys.stderr)
            continue
        payloads[pid] = p.read_bytes()

    tiles: list[TileBlob] = []
    for x, y, pid, offset, size in recs:
        blob = payloads.get(pid)
        if not blob:
            continue
        start = VMAP_VPK_HDR_SIZE + offset
        data = blob[start:start + size]
        if len(data) != size or data[:4] != VTIL_MAGIC:
            print(f"WARN: bad packed tile z={z} {x}/{y}", file=sys.stderr)
            continue
        tiles.append(TileBlob(z=z, x=x, y=y, data=data))
    return tiles


def collect_tiles(src: Path) -> list[TileBlob]:
    """Accept packed zoom dirs (<z>/tiles.idx) or loose <z>/<x>/<y>.vt."""
    tiles: list[TileBlob] = []
    if not src.is_dir():
        return tiles
    for z_dir in sorted(p for p in src.iterdir() if p.is_dir() and p.name.isdigit()):
        z = int(z_dir.name)
        packed = collect_packed_zoom(z_dir, z)
        if packed:
            tiles.extend(packed)
            continue
        for x_dir in sorted(p for p in z_dir.iterdir() if p.is_dir() and p.name.isdigit()):
            x = int(x_dir.name)
            for vt in sorted(x_dir.glob("*.vt")):
                y = int(vt.stem)
                data = vt.read_bytes()
                if len(data) < 20 or data[:4] != VTIL_MAGIC:
                    print(f"WARN: skip invalid tile {vt}", file=sys.stderr)
                    continue
                tiles.append(TileBlob(z=z, x=x, y=y, data=data))
    return tiles


def pack_zoom(tiles: list[TileBlob], dst_z: Path, pack_bytes: int) -> None:
    dst_z.mkdir(parents=True, exist_ok=True)

    shards: list[bytearray] = []
    index: list[tuple[int, int, int, int, int]] = []  # x,y,packId,offset,size

    def new_shard() -> tuple[int, bytearray]:
        shards.append(bytearray())
        return len(shards) - 1, shards[-1]

    pack_id, payload = new_shard()

    for tile in sorted(tiles, key=lambda t: (t.y, t.x)):
        size = len(tile.data)
        if size > pack_bytes:
            raise ValueError(
                f"tile z={tile.z} x={tile.x} y={tile.y} is {size} B, "
                f"larger than pack limit {pack_bytes}"
            )

        if len(payload) > 0 and len(payload) + size > pack_bytes:
            pack_id, payload = new_shard()

        offset = len(payload)
        payload.extend(tile.data)
        index.append((tile.x, tile.y, pack_id, offset, size))

    for pid, payload in enumerate(shards):
        out = dst_z / f"p{pid:03d}.{VMAP_SHARD_EXT}"
        header = struct.pack(
            "<4sBBHI I",
            VPKG_MAGIC,
            VMAP_PACK_VERSION,
            tiles[0].z,
            pid,
            len(payload),
            0,
        )
        out.write_bytes(header + payload)

    idx_path = dst_z / VMAP_TILES_INDEX_FILE
    idx_buf = bytearray()
    idx_buf.extend(
        struct.pack(
            "<4sBBH",
            VTIX_MAGIC,
            VMAP_TILES_INDEX_VERSION,
            tiles[0].z,
            len(index),
        )
    )
    for x, y, pid, offset, size in index:
        idx_buf.extend(struct.pack("<HHHII", x, y, pid, offset, size))

    idx_path.write_bytes(idx_buf)

    total_payload = sum(len(s) for s in shards)
    print(
        f"  z={tiles[0].z}: {len(tiles)} tiles -> "
        f"{len(shards)} .{VMAP_SHARD_EXT}, {VMAP_TILES_INDEX_FILE} "
        f"{idx_path.stat().st_size} B, payload {total_payload} B"
    )


def pack_tree(src: Path, dst: Path, pack_bytes: int, clean: bool) -> None:
    tiles = collect_tiles(src)
    if not tiles:
        raise SystemExit(f"no .vt tiles under {src}")

    by_zoom: dict[int, list[TileBlob]] = {}
    for t in tiles:
        by_zoom.setdefault(t.z, []).append(t)

    if clean and dst.exists():
        import shutil

        shutil.rmtree(dst)
    dst.mkdir(parents=True, exist_ok=True)

    print(f"Packing {len(tiles)} tiles from {src} -> {dst} "
          f"(~{pack_bytes // 1024} KiB/.{VMAP_SHARD_EXT})")
    for z in sorted(by_zoom):
        pack_zoom(by_zoom[z], dst / str(z), pack_bytes)

    shard_files = list(dst.rglob(f"*.{VMAP_SHARD_EXT}"))
    idx_files = list(dst.rglob(VMAP_TILES_INDEX_FILE))
    print(
        f"Done: {len(shard_files)} shard(s), {len(idx_files)} {VMAP_TILES_INDEX_FILE}, "
        f"{len(tiles)} tiles total"
    )


def main() -> None:
    ap = argparse.ArgumentParser(description="Pack vmap tiles into .vpk shards")
    ap.add_argument(
        "--input",
        "-i",
        type=Path,
        required=True,
        help="Build tree (<z>/<x>/<y>.vt)",
    )
    ap.add_argument(
        "--output",
        "-o",
        type=Path,
        required=True,
        help="Device map directory",
    )
    ap.add_argument(
        "--pack-size",
        type=int,
        default=DEFAULT_PACK_BYTES,
        help=f"Max payload bytes per .vpk (default {DEFAULT_PACK_BYTES})",
    )
    ap.add_argument(
        "--clean",
        action="store_true",
        help="Remove output directory before writing",
    )
    args = ap.parse_args()

    if not args.input.is_dir():
        raise SystemExit(f"input not found: {args.input}")

    pack_tree(args.input, args.output, args.pack_size, args.clean)


if __name__ == "__main__":
    main()
