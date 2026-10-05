#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Downsample 三幅.png to 80x80 and emit 2SFBL + bicycle splash C arrays.

LANCZOS + premultiplied alpha so the white ring / moon / tail keep
anti-aliased edges. 2SFBL gets RGB565 composited on black (fast blit);
bicycle keeps RGB565A8 with a separate alpha plane.

Usage (from openvela root):
  python3 vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/tools/gen_boot_logo.py
"""

from __future__ import annotations

import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
BICYCLE_DIR = os.path.normpath(os.path.join(HERE, ".."))
# tools → bicycle → ui → board my_vendor → sf32lb52 → boards → vendor my_vendor
VENDOR_MY = os.path.normpath(os.path.join(HERE, *([os.pardir] * 6)))

SRC_DEFAULT = "/home/jinsc/Pictures/三幅.png"
SIZE = 80
# Keep ~1 px of empty margin so the outer ring AA is not clipped.
PAD_FRAC = 0.012
# LANCZOS rings below this; would speckle the black bezel on the LCD.
ALPHA_CUT = 12
ALPHA_OPAQUE = 250

SPLASH_C = os.path.join(
    VENDOR_MY, "boot_loader/project/butterflmicro/board/boot_splash_img.c"
)
LOGO_C = os.path.join(BICYCLE_DIR, "src/lvgl_page/startup/img_src_boot_logo.c")
PNG_80 = os.path.join(HERE, "boot_logo_80.png")
PNG_80_EXPORT = "/home/jinsc/Pictures/三幅_80.png"
PREVIEW = os.path.join(HERE, "boot_logo_preview_80.png")
PREVIEW_PX = os.path.join(HERE, "boot_logo_preview_80x4.png")


def _split_rgba(im: Image.Image):
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    rgb = Image.new("RGB", (w, h))
    alpha = Image.new("L", (w, h))
    q, a = rgb.load(), alpha.load()
    for y in range(h):
        for x in range(w):
            r, g, b, aa = px[x, y]
            if aa == 0:
                q[x, y] = (0, 0, 0)
            elif aa == 255:
                q[x, y] = (r, g, b)
            else:
                q[x, y] = (r * aa // 255, g * aa // 255, b * aa // 255)
            a[x, y] = aa
    return rgb, alpha


def downsample(src_path: str, size: int) -> Image.Image:
    src = Image.open(src_path).convert("RGBA")
    w, h = src.size
    pad = max(1, int(round(min(w, h) * PAD_FRAC)))
    canvas = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    canvas.paste(src, (pad, pad), src)

    pm, alpha = _split_rgba(canvas)
    pm80 = pm.resize((size, size), Image.Resampling.LANCZOS)
    a80 = alpha.resize((size, size), Image.Resampling.LANCZOS)

    out = Image.new("RGBA", (size, size))
    p, s = a80.load(), pm80.load()
    for y in range(size):
        for x in range(size):
            aa = p[x, y]
            r, g, b = s[x, y]
            if aa < ALPHA_CUT:
                out.putpixel((x, y), (0, 0, 0, 0))
            elif aa >= ALPHA_OPAQUE:
                if aa == 255:
                    out.putpixel((x, y), (r, g, b, 255))
                else:
                    out.putpixel(
                        (x, y),
                        (
                            min(255, (r * 255 + aa // 2) // aa),
                            min(255, (g * 255 + aa // 2) // aa),
                            min(255, (b * 255 + aa // 2) // aa),
                            255,
                        ),
                    )
            else:
                out.putpixel(
                    (x, y),
                    (
                        min(255, (r * 255 + aa // 2) // aa),
                        min(255, (g * 255 + aa // 2) // aa),
                        min(255, (b * 255 + aa // 2) // aa),
                        aa,
                    ),
                )
    return out


def rle_argb8888(im: Image.Image) -> bytes:
    """SGL Image To Array: [count][B][G][R][A], count 1..255."""
    w, h = im.size
    px = im.load()
    out = bytearray()
    run_c = 0
    run_px = (0, 0, 0, 0)

    def flush():
        nonlocal run_c
        if run_c:
            r, g, b, a = run_px
            out.extend((run_c, b, g, r, a))
            run_c = 0

    for y in range(h):
        for x in range(w):
            pix = px[x, y]
            if run_c == 0:
                run_px = pix
                run_c = 1
            elif pix == run_px and run_c < 255:
                run_c += 1
            else:
                flush()
                run_px = pix
                run_c = 1
    flush()
    return bytes(out)


def rgb565_le(r: int, g: int, b: int) -> tuple[int, int]:
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return c & 0xFF, (c >> 8) & 0xFF


def rgb565_on_black(im: Image.Image) -> bytes:
    """Straight RGB565, pre-composited on black (2SFBL page is black)."""
    w, h = im.size
    black = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    rgb = Image.alpha_composite(black, im.convert("RGBA")).convert("RGB")
    px = rgb.load()
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            lo, hi = rgb565_le(r, g, b)
            out[i] = lo
            out[i + 1] = hi
            i += 2
    return bytes(out)


def rgb565a8_map(im: Image.Image) -> bytes:
    w, h = im.size
    px = im.load()
    color = bytearray(w * h * 2)
    alpha = bytearray(w * h)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            lo, hi = rgb565_le(r, g, b)
            color[i] = lo
            color[i + 1] = hi
            alpha[y * w + x] = a
            i += 2
    return bytes(color) + bytes(alpha)


def c_bytes(data: bytes, per_line: int = 16) -> str:
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def write_splash(path: str, rgb565: bytes, w: int, h: int) -> None:
    body = f"""/**
 * @file boot_splash_img.c
 * @brief {w}x{h} RGB565 splash (black-composited). From 三幅.png (LANCZOS).
 *
 * Uncompressed so the enter animation can blit without RLE+alpha per strip.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <sgl_core.h>

/* pic1 - {w}x{h} RGB565 little-endian, pre-composited on black */
static const uint8_t pic1_data[{len(rgb565)}] = {{
{c_bytes(rgb565)}
}};

const sgl_pixmap_t pic1_pixmap = {{
    .width = {w},
    .height = {h},
    .bitmap.array = pic1_data,
    .format = SGL_PIXMAP_FMT_RGB565,
}};
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(body)


def write_lvgl(path: str, blob: bytes, w: int, h: int) -> None:
    body = f"""/**
 * @file img_src_boot_logo.c
 * @brief {w}x{h} RGB565A8 splash. Same art as 2SFBL pic1 RGB565.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "img_src_boot_logo.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t img_src_boot_logo_map[] = {{
{c_bytes(blob)}
}};

const lv_image_dsc_t img_src_boot_logo = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.flags = 0,
    .header.w = BOOT_LOGO_W,
    .header.h = BOOT_LOGO_H,
    .header.stride = BOOT_LOGO_W * 2,
    .data_size = sizeof(img_src_boot_logo_map),
    .data = img_src_boot_logo_map,
}};
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(body)


def rle_pixel_count(rle: bytes) -> int:
    i = 0
    n = 0
    while i < len(rle):
        n += rle[i]
        i += 5
    return n


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else SRC_DEFAULT
    if not os.path.isfile(src):
        print(f"missing source: {src}", file=sys.stderr)
        return 1

    im = downsample(src, SIZE)
    rgb565 = rgb565_on_black(im)
    blob = rgb565a8_map(im)
    if len(rgb565) != SIZE * SIZE * 2:
        print(f"RGB565 size {len(rgb565)}", file=sys.stderr)
        return 1
    if len(blob) != SIZE * SIZE * 3:
        print(f"RGB565A8 size {len(blob)}", file=sys.stderr)
        return 1

    write_splash(SPLASH_C, rgb565, SIZE, SIZE)
    write_lvgl(LOGO_C, blob, SIZE, SIZE)

    im.save(PNG_80, format="PNG")
    try:
        im.save(PNG_80_EXPORT, format="PNG")
    except OSError as exc:
        print(f"skip {PNG_80_EXPORT}: {exc}", file=sys.stderr)

    black = Image.new("RGBA", im.size, (0, 0, 0, 255))
    Image.alpha_composite(black, im).save(PREVIEW)
    im.resize((SIZE * 4, SIZE * 4), Image.Resampling.NEAREST).save(PREVIEW_PX)

    print(f"source {src}")
    print(f"rgb565 {len(rgb565)} bytes -> {SPLASH_C}")
    print(f"lvgl   {len(blob)} bytes -> {LOGO_C}")
    print(f"png    {PNG_80}")
    if os.path.isfile(PNG_80_EXPORT):
        print(f"png    {PNG_80_EXPORT}")
    print(f"preview {PREVIEW}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
