#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""OVNX 1 KiB file header: version, build time, image length (CRC at file tail)."""

from __future__ import annotations

import os
import re
import struct
import subprocess
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
APP_VERSION_TXT = SCRIPT_DIR / "app_version.txt"
DEFAULT_DEFCONFIG = (
    SCRIPT_DIR.parent
    / "boards"
    / "sf32lb52"
    / "my_vendor"
    / "configs"
    / "nsh"
    / "defconfig"
)
GENERATED_NAME = "app_version.generated.txt"
VERSION_MAX_LEN = 256
DATE_LEN = 12
# Matches struct boot_ovnx_file_hdr in boot_loader/.../boot_ovnx.h (packed, LE)
FILE_HDR_MAGIC = b"OVNXAPP\x00"
FILE_HDR_DESC_SIZE = 8 + VERSION_MAX_LEN + 4 + DATE_LEN + 4 + 4 + 4 + 4  # 296
FILE_HDR_SIZE = 1024
CRC_SIZE = 4


def read_version_txt(path: Path | None = None) -> str:
    src = path or APP_VERSION_TXT
    if not src.is_file():
        return "0.0.0"
    for line in src.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        return line
    return "0.0.0"


def git_describe_short(root: Path | None = None) -> str | None:
    if os.environ.get("OVNX_VERSION_NO_GIT", "").strip():
        return None
    try:
        root = root or SCRIPT_DIR.parents[2]
        out = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        if out.returncode == 0:
            rev = out.stdout.strip()
            if rev:
                return rev
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def read_defconfig_string(path: Path, symbol: str) -> str | None:
    """Read CONFIG_<symbol>=\"...\" from a NuttX defconfig."""
    needle = f"CONFIG_{symbol}="
    if not path.is_file():
        return None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith(needle):
            continue
        val = line[len(needle) :]
        if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
            return val[1:-1]
        return val
    return None


def resolve_product_version(defconfig: Path | None = None) -> str:
    dc = defconfig or DEFAULT_DEFCONFIG
    ver = read_defconfig_string(dc, "MYVENDOR_PRODUCT_VERSION")
    if ver:
        return ver
    return read_version_txt(APP_VERSION_TXT)


def resolve_app_version(
    version_txt: Path | None = None,
    repo_root: Path | None = None,
    defconfig: Path | None = None,
) -> str:
    if version_txt is not None:
        base = read_version_txt(version_txt)
    else:
        base = resolve_product_version(defconfig)
    rev = git_describe_short(repo_root)
    if rev:
        combined = f"{base}-g{rev}"
    else:
        combined = base
    if len(combined) > VERSION_MAX_LEN:
        return combined[:VERSION_MAX_LEN]
    return combined


def fw_upload_basename(version: str) -> str:
    """Filename for /mnt/kv/fw/<version>.bin (BLE OTA / factory MTP)."""
    out: list[str] = []
    for c in version:
        if c.isalnum() or c in ".-_":
            out.append(c)
        else:
            out.append("_")
    s = "".join(out).strip("._") or "fw"
    if not s.lower().endswith(".bin"):
        s += ".bin"
    return s


def sanitize_fw_token(text: str) -> str:
    """Keep MTP / LittleFS-safe characters; spaces become '-'."""
    out: list[str] = []
    for c in (text or "").strip():
        if c.isalnum() or c in ".-_":
            out.append(c)
        elif c in " \t":
            out.append("-")
        else:
            out.append("_")
    s = "".join(out)
    while "--" in s:
        s = s.replace("--", "-")
    return s.strip("._-")


def product_version_xyz(version: str) -> str:
    """Leading X.Y.Z from an OVNX version string (strip v prefix and -gHASH)."""
    s = (version or "").strip()
    if s[:1] in "vV" and len(s) > 1 and s[1].isdigit():
        s = s[1:]
    m = re.match(r"(\d+)(?:\.(\d+))?(?:\.(\d+))?", s)
    if not m:
        return "0.0.0"
    major, minor, patch = m.group(1), m.group(2) or "0", m.group(3) or "0"
    return f"{major}.{minor}.{patch}"


def fw_ota_basename(version: str, product: str | None = None) -> str:
    """OTA upload name that 2SFBL can rank: must start with X.Y.Z.

    Example: Helm One + 1.0.0-gabc → ``1.0.0-Helm-One.bin``.
    C ``BOOT_FW_NAME_MAX`` is 64; fall back to ``1.0.0.bin`` if longer.
    """
    xyz = product_version_xyz(version)
    slug = sanitize_fw_token(product) if product else ""
    stem = f"{xyz}-{slug}" if slug else xyz
    name = fw_upload_basename(stem)
    if len(name) >= 64:
        name = fw_upload_basename(xyz)
    return name


def version_field_bytes(version: str) -> bytes:
    raw = version.encode("ascii", errors="replace")
    if len(raw) > VERSION_MAX_LEN:
        raw = raw[:VERSION_MAX_LEN]
    return raw.ljust(VERSION_MAX_LEN, b"\0")


def make_file_header(
    version: str,
    build_unix: int,
    build_date: bytes,
    payload_len: int,
    image_len: int,
) -> bytes:
    if len(FILE_HDR_MAGIC) != 8:
        raise RuntimeError("bad FILE_HDR_MAGIC size")
    if len(build_date) != DATE_LEN:
        raise ValueError(f"build_date must be {DATE_LEN} bytes")
    desc = struct.pack(
        "<8s256sI12sIIII",
        FILE_HDR_MAGIC,
        version_field_bytes(version),
        build_unix,
        build_date,
        image_len,
        payload_len,
        FILE_HDR_SIZE,
        0,
    )
    if len(desc) != FILE_HDR_DESC_SIZE:
        raise RuntimeError("bad FILE_HDR_DESC_SIZE")
    return desc.ljust(FILE_HDR_SIZE, b"\0")


def parse_file_header(hdr: bytes) -> dict:
    if len(hdr) < FILE_HDR_DESC_SIZE:
        raise ValueError("file header too short")
    magic, version_raw, build_unix, build_date, image_len, payload_len, hdr_size, flags = (
        struct.unpack("<8s256sI12sIIII", hdr[:FILE_HDR_DESC_SIZE])
    )
    if magic != FILE_HDR_MAGIC:
        raise ValueError(f"bad file header magic {magic!r}")
    if hdr_size != FILE_HDR_SIZE:
        raise ValueError(f"hdr_size {hdr_size} != {FILE_HDR_SIZE}")
    return {
        "magic": magic,
        "version": version_raw.rstrip(b"\0").decode("ascii", errors="replace"),
        "build_unix": build_unix,
        "build_date": build_date.rstrip(b"\0").decode("ascii", errors="replace"),
        "image_len": image_len,
        "payload_len": payload_len,
        "hdr_size": hdr_size,
        "flags": flags,
    }


def write_generated_version(
    out_dir: Path,
    version: str | None = None,
    build_unix: int | None = None,
    build_date: str | None = None,
) -> Path:
    ver = version or resolve_app_version()
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / GENERATED_NAME
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        f"version={ver}\n",
        f"generated_utc={now}\n",
        f"source=CONFIG_MYVENDOR_PRODUCT_VERSION\n",
    ]
    if build_unix is not None:
        lines.append(f"build_unix={build_unix}\n")
    if build_date:
        lines.append(f"build_date={build_date}\n")
    path.write_text("".join(lines), encoding="utf-8")
    return path
