#!/usr/bin/env python3
"""Build a tiny UBX-MGA assistance file for MAX-M10S (MTP into /mnt/lfs/eph/mga_<utc>.ubx).

Contains TIME_UTC + POS_LLH + one dummy GPS-EPH (svId=1).  TIME/POS should be
accepted; the dummy ephemeris is for protocol bring-up and may be NAK'd.
"""

from __future__ import annotations

import argparse
import struct
import time
from pathlib import Path


def ubx(cls: int, msg_id: int, payload: bytes) -> bytes:
    body = bytes([cls, msg_id, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload
    cka = ckb = 0
    for b in body:
        cka = (cka + b) & 0xFF
        ckb = (ckb + cka) & 0xFFzedddd 
    return bytes([0xB5, 0x62]) + body + bytes([cka, ckb])


def mga_time_utc(ts: time.struct_time, tacc_s: int = 7200) -> bytes:
    pl = bytearray(24)
    pl[0] = 0x10
    pl[3] = 18  # GPS-UTC leap seconds (unknown would be 0x80)
    struct.pack_into("<H", pl, 4, ts.tm_year)
    pl[6] = ts.tm_mon
    pl[7] = ts.tm_mday
    pl[8] = ts.tm_hour
    pl[9] = ts.tm_min
    pl[10] = ts.tm_sec
    struct.pack_into("<H", pl, 16, tacc_s)
    return ubx(0x13, 0x40, bytes(pl))


def mga_pos_llh(lat_deg: float, lon_deg: float, alt_m: float, acc_m: float) -> bytes:
    pl = bytearray(20)
    pl[0] = 0x01
    struct.pack_into("<i", pl, 4, int(round(lat_deg * 1e7)))
    struct.pack_into("<i", pl, 8, int(round(lon_deg * 1e7)))
    struct.pack_into("<i", pl, 12, int(round(alt_m * 100)))
    struct.pack_into("<I", pl, 16, int(round(acc_m * 100)))
    return ubx(0x13, 0x40, bytes(pl))


def mga_gps_eph_dummy(sv_id: int = 1) -> bytes:
    pl = bytearray(68)
    pl[0] = 0x01  # type = EPH
    pl[2] = sv_id
    return ubx(0x13, 0x00, bytes(pl))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("-o", "--output", type=Path, required=True)
    p.add_argument("--lat", type=float, default=31.2989, help="WGS84 latitude (default Suzhou)")
    p.add_argument("--lon", type=float, default=120.5853, help="WGS84 longitude")
    p.add_argument("--alt", type=float, default=10.0, help="altitude meters")
    p.add_argument("--acc", type=float, default=100000.0, help="position stddev meters")
    args = p.parse_args()

    utc = time.gmtime()
    blob = (
        mga_time_utc(utc)
        + mga_pos_llh(args.lat, args.lon, args.alt, args.acc)
        + mga_gps_eph_dummy(1)
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(
        f"wrote {args.output} ({len(blob)} bytes) "
        f"TIME_UTC={time.strftime('%Y-%m-%dT%H:%M:%SZ', utc)} "
        f"POS={args.lat:.4f},{args.lon:.4f} acc={args.acc:.0f}m"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
