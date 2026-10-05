#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Host-side helpers for board `test fault` (NSH) + vela_elf_resolve.py.

Usage (openvela root):
  python3 vendor/my_vendor/scripts/test_fault_harness.py list
  python3 vendor/my_vendor/scripts/test_fault_harness.py nsh
  python3 vendor/my_vendor/scripts/test_fault_harness.py decode crash.log \\
      --elf cmake_out/my_vendor_nsh/nuttx
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from pathlib import Path

# Sync with test_fault.c g_fault_cases[] (trigger cases only).
FAULT_CASES: tuple[str, ...] = (
    "assert",
    "panic",
    "verify",
    "abort",
    "sigabrt",
    "sigsegv",
    "sigfpe",
    "sigill",
    "nullwr",
    "nullrd",
    "badcode",
    "execbad",
    "badread",
    "badwrite",
    "unaligned",
    "undef",
    "bkpt",
    "div0",
    "execram",
    "stack",
    "stackhw",
    "stackovf",
    "heapdf",
    "heapuaf",
    "heapovf",
    "childassert",
    "childnull",
    "childundef",
    "childdiv0",
    "null",
    "heap",
)

RE_FAULT_CASE = re.compile(r"^FAULT_CASE=(\w+)\s*$")
RE_FAULT_BEGIN = re.compile(r"^FAULT_BEGIN=(\w+)\s*$")
RE_FAULT_CASES_BEGIN = re.compile(r"^FAULT_CASES_BEGIN\s*$")
RE_FAULT_CASES_END = re.compile(r"^FAULT_CASES_END\s*$")

SCRIPT = Path(__file__).resolve()
OPENVELA = SCRIPT.parent.parent.parent


def _load_elf_resolve():
    path = SCRIPT.parent / "vela_elf_resolve.py"
    spec = importlib.util.spec_from_file_location("vela_elf_resolve", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def cmd_list() -> int:
    print("FAULT_CASES_BEGIN")
    for name in FAULT_CASES:
        print(name)
    print("FAULT_CASES_END")
    return 0


def cmd_nsh() -> int:
    print("# NSH — one case per reset for trapping cases")
    print("test fault bt")
    for name in FAULT_CASES:
        print(f"test fault {name}")
    return 0


def cmd_decode(elf: Path, log_path: Path | None) -> int:
    mod = _load_elf_resolve()
    if log_path is None:
        text = sys.stdin.read()
    else:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    resolver = mod.ElfSymbolResolver(elf)
    decoder = mod.PanicSerialDecoder(resolver)
    sys.stdout.write(decoder.feed(text))
    sys.stdout.write(decoder.flush())
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Host helper for test fault")
    parser.add_argument(
        "--elf",
        type=Path,
        default=OPENVELA / "cmake_out/my_vendor_nsh/nuttx",
        help="nuttx ELF for decode",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="Print FAULT_CASES_BEGIN block")
    sub.add_parser("nsh", help="Print NSH commands for each case")
    p_dec = sub.add_parser("decode", help="Decode log (FAULT_* markers + backtrace)")
    p_dec.add_argument("log", nargs="?", type=Path, help="log file (default stdin)")

    args = parser.parse_args()
    if args.cmd == "list":
        return cmd_list()
    if args.cmd == "nsh":
        return cmd_nsh()
    if args.cmd == "decode":
        return cmd_decode(args.elf, args.log)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
