#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Wrap nuttx.bin → nuttx.flash.bin (1 KiB OVNX header + payload + 4B CRC)."""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

from ovnx_version import (
    CRC_SIZE,
    FILE_HDR_SIZE,
    DEFAULT_DEFCONFIG,
    fw_ota_basename,
    make_file_header,
    parse_file_header,
    read_defconfig_string,
    resolve_app_version,
    write_generated_version,
)

DATE_LEN = 12
DEFAULT_INPUT = "nuttx.bin"
DEFAULT_OUTPUT = "nuttx.flash.bin"


def crc32_ieee(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def build_timestamp() -> tuple[int, bytes]:
    if "SOURCE_DATE_EPOCH" in os.environ:
        ts = int(os.environ["SOURCE_DATE_EPOCH"])
    else:
        ts = int(time.time())

    # 这一串会被 2SFBL 原样打到启动日志里（boot_ovnx.c 只 memcpy + puts，
    # 不解析），所以按**本地时间**格式化，和 format_build_local() 口径一致。
    #
    # 原来这里是 tz=timezone.utc：build_unix 是对的，但镜像头里那 12 字节
    # build_date 是 UTC，板上打出来会被当成构建时间不对（团队在 UTC+8，
    # 实际差 8 小时）。host 侧打印另有 format_build_local()，所以在开发机上
    # 看不出问题，只有板上那行会误导。
    #
    # DATE_LEN=12 只够 YYYYMMDDHHMM，塞不下时区后缀，因此只能二选一。
    # 取舍：若将来启用 SOURCE_DATE_EPOCH（可复现构建），本地时间会随构建机
    # 时区变化而破坏逐字节可复现，届时这里要改回固定时区并在字符串里带标志。
    # 目前全树没有任何地方设置该变量（grep 过），所以选本地时间。
    dt = datetime.fromtimestamp(ts)
    date_ascii = dt.strftime("%Y%m%d%H%M").encode("ascii")
    if len(date_ascii) > DATE_LEN:
        date_ascii = date_ascii[:DATE_LEN]
    return ts, date_ascii.ljust(DATE_LEN, b"\0")


def format_build_local(unix: int) -> str:
    return datetime.fromtimestamp(unix).strftime("%Y-%m-%d %H:%M:%S")


def wrap_payload(payload: bytes, version: str | None = None) -> tuple[bytes, str, dict]:
    ver = version or resolve_app_version()
    build_unix, build_date = build_timestamp()
    payload_len = len(payload)
    image_len = FILE_HDR_SIZE + payload_len + CRC_SIZE
    file_hdr = make_file_header(ver, build_unix, build_date, payload_len, image_len)
    payload_crc = crc32_ieee(payload)
    tail_crc = struct.pack("<I", payload_crc)
    wrapped = file_hdr + payload + tail_crc
    meta = {
        "version": ver,
        "build_unix": build_unix,
        "build_date": build_date.rstrip(b"\0").decode("ascii"),
        "image_len": image_len,
        "payload_len": payload_len,
        "payload_crc": payload_crc,
    }
    return wrapped, ver, meta


def verify_image(data: bytes) -> dict:
    if len(data) < FILE_HDR_SIZE + CRC_SIZE:
        raise ValueError(f"image too short ({len(data)} B)")
    meta = parse_file_header(data[:FILE_HDR_SIZE])
    payload = data[FILE_HDR_SIZE:-CRC_SIZE]
    stored_crc = struct.unpack("<I", data[-CRC_SIZE:])[0]
    if meta["payload_len"] != len(payload):
        raise ValueError(
            f"payload_len mismatch: hdr={meta['payload_len']} actual={len(payload)}"
        )
    expect_len = FILE_HDR_SIZE + len(payload) + CRC_SIZE
    if meta["image_len"] != expect_len:
        raise ValueError(
            f"image_len mismatch: hdr={meta['image_len']} expect={expect_len}"
        )
    if meta["image_len"] != len(data):
        raise ValueError(
            f"image_len mismatch: hdr={meta['image_len']} file={len(data)}"
        )
    calc_crc = crc32_ieee(payload)
    if stored_crc != calc_crc:
        raise ValueError(
            f"CRC32 mismatch: stored=0x{stored_crc:08X} calc=0x{calc_crc:08X}"
        )
    meta["payload_crc"] = stored_crc
    meta["payload_crc_calc"] = calc_crc
    return meta


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", "-i", type=Path, help="raw nuttx.bin")
    parser.add_argument("--output", "-o", type=Path, help="nuttx.flash.bin")
    parser.add_argument("build_dir", nargs="?", type=Path)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--version-file", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.build_dir is not None:
        build_dir = args.build_dir.resolve()
        in_path = (args.input or build_dir / DEFAULT_INPUT).resolve()
        out_path = (args.output or build_dir / DEFAULT_OUTPUT).resolve()
        gen_dir = build_dir
    else:
        in_path = (args.input or Path(DEFAULT_INPUT)).resolve()
        out_path = (args.output or Path(DEFAULT_OUTPUT)).resolve()
        gen_dir = out_path.parent

    defconfig = None
    if args.build_dir is not None:
        cfg = args.build_dir.resolve() / ".config"
        if cfg.is_file():
            defconfig = cfg

    version = resolve_app_version(args.version_file, defconfig=defconfig)

    if args.verify:
        if args.output is not None:
            target = args.output.resolve()
        elif args.input is not None:
            target = args.input.resolve()
        elif args.build_dir is not None:
            bd = args.build_dir.resolve()
            target = bd if bd.is_file() else (bd / DEFAULT_OUTPUT).resolve()
        else:
            target = Path(DEFAULT_OUTPUT).resolve()
        if not target.is_file():
            print(f"ERROR: file not found: {target}", file=sys.stderr)
            return 1
        try:
            meta = verify_image(target.read_bytes())
            print(f"OK: {target}")
            print(f"  version     {meta['version']}")
            print(f"  build_date  {meta['build_date']}  {format_build_local(meta['build_unix'])} (unix {meta['build_unix']})")
            print(f"  image_len   {meta['image_len']} B (0x{meta['image_len']:X})")
            print(f"  payload     {meta['payload_len']} B (0x{meta['payload_len']:X})")
            print(f"  CRC32       0x{meta['payload_crc']:08X}")
            return 0
        except ValueError as exc:
            print(f"ERROR: {target}: {exc}", file=sys.stderr)
            return 1

    if not in_path.is_file():
        print(f"ERROR: input not found: {in_path}", file=sys.stderr)
        return 1

    payload = in_path.read_bytes()
    wrapped, ver, meta = wrap_payload(payload, version)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(wrapped)
    gen_path = write_generated_version(
        gen_dir, ver, meta["build_unix"], meta["build_date"]
    )

    print(f"Input : {in_path} ({len(payload)} B, unchanged)")
    print(f"Output: {out_path} ({len(wrapped)} B, +{FILE_HDR_SIZE}+{CRC_SIZE} B OVNX)")
    print(f"  version     {ver}  (CONFIG_MYVENDOR_PRODUCT_VERSION)")
    print(f"  generated   {gen_path}")
    print(f"  build_date  {meta['build_date']}  {format_build_local(meta['build_unix'])} (unix {meta['build_unix']})")
    print(f"  image_len   {meta['image_len']} B (0x{meta['image_len']:X})")
    print(f"  payload     {meta['payload_len']} B (0x{meta['payload_len']:X})")
    print(f"  CRC32       0x{meta['payload_crc']:08X}")
    product = read_defconfig_string(
        defconfig or DEFAULT_DEFCONFIG, "MYVENDOR_PRODUCT_NAME"
    )
    ota_name = fw_ota_basename(ver, product)
    ota_dir = gen_dir / "fw"
    ota_dir.mkdir(parents=True, exist_ok=True)
    ota_path = ota_dir / ota_name
    if ota_path.resolve() != out_path.resolve():
        # 这里曾经用 os.link（省一次 3 MB 拷贝）。**不能再用**：out_path
        # （nuttx.flash.bin）每次构建是**原地重写**同一个 inode，于是所有旧版本名
        # 的硬链接会一起变成新镜像 —— fw/1.4.0-Helm-One.bin 的 OVNX 头真的读出了
        # 1.4.1，而 Kconfig 明写 "/fw/*.bin 留作 rollback"（2026-09-25 实测踩到）。
        # 顺带修掉一个下游：_latest_ota_fw() 按 st_mtime 挑文件，同 inode 时几个
        # 版本的 mtime 完全相同 ⇒ 选择结果不确定。
        # ⇒ 版本化产物必须是**独立内容的不可变拷贝**。
        try:
            if ota_path.exists() or ota_path.is_symlink():
                ota_path.unlink()
            ota_path.write_bytes(wrapped)
        except OSError as exc:
            print(f"OTA pack failed: {ota_path}: {exc}", file=sys.stderr)
            raise
    print(f"OTA pack: {ota_path}")
    print(f"  upload as   /mnt/kv/fw/{ota_name}")
    sidecar = ota_path.with_suffix(".txt")
    sidecar.write_text(
        (
            f"product={product or ''}\n"
            f"version={ver}\n"
            f"ota_name={ota_name}\n"
            f"size={len(wrapped)}\n"
            f"crc32=0x{meta['payload_crc']:08X}\n"
            f"build_date={meta['build_date']}\n"
            f"build_local={format_build_local(meta['build_unix'])}\n"
            f"build_unix={meta['build_unix']}\n"
            f"upload=/mnt/kv/fw/{ota_name}\n"
        ),
        encoding="utf-8",
    )
    print(f"  sidecar     {sidecar}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
