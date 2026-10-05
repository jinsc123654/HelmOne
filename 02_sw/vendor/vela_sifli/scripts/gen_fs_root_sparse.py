#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Split fs_root.bin into non-blank segments for sparse flash / pack-sd-img.

For multi-GiB sparse LittleFS images, use Linux SEEK_DATA/SEEK_HOLE to skip
holes in O(extents) instead of reading every 128 KiB block.
"""

from __future__ import annotations

import argparse
import errno
import json
import os
import sys
from pathlib import Path

LFS_BLOCK_SIZE = 131072


def block_is_blank(data: bytes) -> bool:
    """Host image holes are 0x00; flashed/erased NAND blocks are 0xFF."""
    if not data:
        return True
    return all(b == 0xFF for b in data) or all(b == 0x00 for b in data)


def _merge_segments(segments: list[tuple[int, int]]) -> list[tuple[int, int]]:
    if not segments:
        return []
    out: list[tuple[int, int]] = [segments[0]]
    for off, length in segments[1:]:
        prev_off, prev_len = out[-1]
        if off <= prev_off + prev_len:
            end = max(prev_off + prev_len, off + length)
            out[-1] = (prev_off, end - prev_off)
        else:
            out.append((off, length))
    return out


def find_segments_seek_data(image: Path) -> tuple[int, list[tuple[int, int]]] | None:
    """Fast path: walk allocated extents via SEEK_DATA / SEEK_HOLE."""
    if not hasattr(os, "SEEK_DATA") or not hasattr(os, "SEEK_HOLE"):
        return None

    size = image.stat().st_size
    segments: list[tuple[int, int]] = []

    with image.open("rb") as fh:
        fd = fh.fileno()
        pos = 0
        while pos < size:
            try:
                data_off = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError as exc:
                if exc.errno in (errno.ENXIO, errno.EINVAL):
                    break
                return None
            if data_off < 0 or data_off >= size:
                break
            try:
                hole_off = os.lseek(fd, data_off, os.SEEK_HOLE)
            except OSError as exc:
                if exc.errno in (errno.ENXIO, errno.EINVAL):
                    hole_off = size
                else:
                    return None
            if hole_off < 0 or hole_off > size:
                hole_off = size
            if hole_off > data_off:
                segments.append((data_off, hole_off - data_off))
            pos = max(hole_off, data_off + 1)

    return size, _merge_segments(segments)


def find_segments_streaming(
    image: Path, block_size: int
) -> tuple[int, list[tuple[int, int]]]:
    """Fallback: scan block-by-block (slow on multi-GiB images)."""
    segments: list[tuple[int, int]] = []
    size = image.stat().st_size
    seg_start: int | None = None
    pos = 0

    with image.open("rb") as fh:
        while pos < size:
            to_read = min(block_size, size - pos)
            data = fh.read(to_read)
            if len(data) != to_read:
                raise RuntimeError(f"short read at offset {pos}")

            blank = block_is_blank(data)
            if not blank and seg_start is None:
                seg_start = pos
            elif blank and seg_start is not None:
                segments.append((seg_start, pos - seg_start))
                seg_start = None

            pos += to_read

    if seg_start is not None:
        segments.append((seg_start, size - seg_start))

    return size, segments


def find_segments(
    image: Path, block_size: int
) -> tuple[int, list[tuple[int, int]], str]:
    fast = find_segments_seek_data(image)
    if fast is not None:
        return fast[0], fast[1], "SEEK_DATA"
    size, segs = find_segments_streaming(image, block_size)
    return size, segs, "scan"


def write_segments(
    image: Path,
    base_addr: int,
    out_dir: Path,
    manifest: Path,
    *,
    block_size: int,
) -> dict:
    image_size, segments, mode = find_segments(image, block_size)
    print(
        f"[sparse] scan={mode} extents={len(segments)} "
        f"image={image_size} bytes",
        file=sys.stderr,
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    if manifest.is_file():
        manifest.unlink()

    for old in out_dir.glob("seg_*.bin"):
        old.unlink()

    entries: list[dict] = []
    total_payload = 0

    with image.open("rb") as fh:
        for idx, (off, length) in enumerate(segments):
            name = f"seg_{idx:03d}.bin"
            seg_path = out_dir / name
            fh.seek(off)
            remaining = length
            with seg_path.open("wb") as out:
                while remaining > 0:
                    chunk = fh.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise RuntimeError(f"short read copying segment @{off}")
                    out.write(chunk)
                    remaining -= len(chunk)

            flash_addr = base_addr + off
            entries.append(
                {
                    "file": str(seg_path.resolve()),
                    "name": name,
                    "image_offset": off,
                    "flash_addr": f"0x{flash_addr:X}",
                    "size": length,
                }
            )
            total_payload += length

    payload = {
        "version": 1,
        "source_image": str(image.resolve()),
        "image_size": image_size,
        "block_size": block_size,
        "base_addr": f"0x{base_addr:X}",
        "scan_mode": mode,
        "segment_count": len(entries),
        "payload_bytes": total_payload,
        "segments": entries,
    }
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--base-addr", type=lambda s: int(s, 0), required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument(
        "--block-size",
        type=int,
        default=LFS_BLOCK_SIZE,
        help=f"LFS erase block size (default {LFS_BLOCK_SIZE})",
    )
    args = parser.parse_args()

    if not args.image.is_file():
        print(f"ERROR: missing image: {args.image}", file=sys.stderr)
        return 1

    info = write_segments(
        args.image,
        args.base_addr,
        args.out_dir,
        args.manifest,
        block_size=args.block_size,
    )

    pct = (
        100.0 * info["payload_bytes"] / info["image_size"]
        if info["image_size"]
        else 0.0
    )
    print(
        f"Sparse manifest: {args.manifest}\n"
        f"  image={info['image_size']} bytes, payload={info['payload_bytes']} "
        f"bytes ({pct:.2f}%), segments={info['segment_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
