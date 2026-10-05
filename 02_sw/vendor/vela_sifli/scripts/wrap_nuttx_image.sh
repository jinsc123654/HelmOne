#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Wrap nuttx.bin → nuttx.flash.bin (1KiB OVNX header + payload + 4B CRC). nuttx.bin unchanged.
# CLion Post-build: wrap_nuttx_image.sh cmake_out/my_vendor_nsh  (see nuttx_ovnx_image.md)
#
# Usage:
#   wrap_nuttx_image.sh [cmake_out/my_vendor_nsh]
#   wrap_nuttx_image.sh --input nuttx.bin --output nuttx.flash.bin
#   wrap_nuttx_image.sh --verify cmake_out/my_vendor_nsh/nuttx.flash.bin
#
# Output default: nuttx.flash.bin alongside nuttx.bin (for sftool / NAND boot).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${SCRIPT_DIR}/wrap_nuttx_image.py" "$@"
