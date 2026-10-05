#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate a C partition table (ptab_table.c/.h) from the active ptab.json.

The second-stage bootloader (2SFBL) gets a full, compiled-in view of the build's
partition layout — including regions that never reach ftab.bin (FS_REGION, KVDB,
DFU_* ...). build.sh selects ptab.nand.json / ptab.sdmmc.json / ptab.nor.json (per BOOT_STORAGE)
passes it here so the generated table always matches the storage being booted.

Each ptab region becomes one struct ptab_entry:
    mem    : memory block name ("sd", "flash2", "psram1", ...)
    img    : image name ("main"/"bootloader"/"ftab"/"dfu"/"fs_root") or ""
    tags   : all region tags joined by '|' ("FS_REGION", "FLASH_TABLE", ...) or ""
    base   : memory block base address
    offset : region offset within the block
    addr   : absolute address (base + offset)
    size   : region max_size

Usage:
    gen_ptab_table.py --ptab <ptab.json> --storage <nand|sd|emmc> \
                      --out-c <board/ptab_table.c> --out-h <board/ptab_table.h>
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _int(value: object) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def parse_entries(ptab: Path) -> list[dict]:
    """Flatten every region of every mem block into a list of dicts."""
    mems = json.loads(ptab.read_text(encoding="utf-8"))
    entries: list[dict] = []
    for mem in mems:
        mem_name = str(mem.get("mem", ""))
        base = _int(mem.get("base", 0))
        for region in mem.get("regions", []):
            offset = _int(region.get("offset", 0))
            size = _int(region.get("max_size", 0))
            if size > 0xFFFFFFFF:
                print(
                    f"WARN: {mem_name}+{offset:#x} max_size={size:#x} "
                    f"exceeds uint32; compiling as 0 (CSD remainder)",
                    file=sys.stderr,
                )
                size = 0
            img = region.get("img") or ""
            tags = region.get("tags") or []
            entries.append(
                {
                    "mem": mem_name,
                    "img": str(img),
                    "tags": "|".join(str(t) for t in tags),
                    "base": base,
                    "offset": offset,
                    "addr": base + offset,
                    "size": size,
                }
            )
    return entries


def _c_str(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_header(storage: str, ptab_name: str, count: int) -> str:
    return f"""/**
 * @file ptab_table.h
 * @brief 编译进固件的分区表（由 ptab JSON 生成，勿手改）。
 *
 * 源：{ptab_name}（BOOT_STORAGE={storage}）。生成脚本：
 * `boot_loader/scripts/gen_ptab_table.py`。SD 布局见 `docs/sd_partition.md`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __PTAB_TABLE_H__
#define __PTAB_TABLE_H__

#include <stdint.h>

#define PTAB_BOOT_STORAGE  {_c_str(storage)}  /**< 启动介质名（nand/sd/emmc）。 */
#define PTAB_SOURCE_JSON   {_c_str(ptab_name)}  /**< 生成此表的 JSON 文件名。 */
#define PTAB_ENTRY_COUNT   {count}u  /**< `g_ptab_table[]` 元素个数。 */

/**
 * @brief 分区表一行（一个 mem 块里的一个 region）。
 */
struct ptab_entry
{{
    const char *mem;     /**< 存储块名，如 "sd" / "flash2" / "psram1"。 */
    const char *img;     /**< 镜像名或 ""（如 "main" / "bootloader" / "fs_root"）。 */
    const char *tags;    /**< 区域 tag，以 '|' 拼接，或 ""。 */
    uint32_t    base;    /**< 存储块基址。 */
    uint32_t    offset;  /**< 块内偏移（卡上字节偏移）。 */
    uint32_t    addr;    /**< 绝对地址（base + offset）。 */
    uint32_t    size;    /**< region max_size（字节）；0 表示运行时余量。 */
}};

extern const struct ptab_entry g_ptab_table[PTAB_ENTRY_COUNT];
extern const unsigned g_ptab_table_count;

/**
 * @brief 按镜像名查找（如 "fs_root"、"main"）。
 * @param img 镜像名；NULL 则返回 NULL。
 * @return 第一处匹配（ptab.json 中靠前的存储块优先），没有则为 NULL。
 */
const struct ptab_entry *ptab_find_img(const char *img);

/**
 * @brief 按 tag 查找（如 "FS_REGION"、"KV_REGION"）。
 *
 * 匹配 `tags` 里以 '|' 分隔的某一个 token。
 *
 * @param tag tag 字符串；NULL 或空串则返回 NULL。
 * @return 第一处匹配，没有则为 NULL。
 */
const struct ptab_entry *ptab_find_tag(const char *tag);

#endif /* __PTAB_TABLE_H__ */
"""


def render_source(storage: str, ptab_name: str, entries: list[dict]) -> str:
    rows = []
    for e in entries:
        rows.append(
            "    {{ {mem}, {img}, {tags}, "
            "0x{base:08X}u, 0x{offset:08X}u, 0x{addr:08X}u, 0x{size:08X}u }},".format(
                mem=_c_str(e["mem"]),
                img=_c_str(e["img"]),
                tags=_c_str(e["tags"]),
                base=e["base"],
                offset=e["offset"],
                addr=e["addr"],
                size=e["size"],
            )
        )
    body = "\n".join(rows)
    return f"""/**
 * @file ptab_table.c
 * @brief 编译进固件的分区表实现（由 ptab JSON 生成，勿手改）。
 *
 * 源：{ptab_name}（BOOT_STORAGE={storage}）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "ptab_table.h"

const struct ptab_entry g_ptab_table[PTAB_ENTRY_COUNT] =
{{
{body}
}};

const unsigned g_ptab_table_count = PTAB_ENTRY_COUNT;

/**
 * @brief 按镜像名查找。
 * @param img 镜像名；NULL 则返回 NULL。
 * @return 第一处匹配，没有则为 NULL。
 */
const struct ptab_entry *ptab_find_img(const char *img)
{{
    unsigned i;

    if (img == NULL)
    {{
        return NULL;
    }}

    for (i = 0; i < PTAB_ENTRY_COUNT; i++)
    {{
        if (g_ptab_table[i].img[0] != '\\0' &&
            strcmp(g_ptab_table[i].img, img) == 0)
        {{
            return &g_ptab_table[i];
        }}
    }}

    return NULL;
}}

/**
 * @brief 按 tag 查找（匹配 tags 中以 '|' 分隔的一个 token）。
 * @param tag tag 字符串；NULL 或空串则返回 NULL。
 * @return 第一处匹配，没有则为 NULL。
 */
const struct ptab_entry *ptab_find_tag(const char *tag)
{{
    unsigned i;
    size_t taglen;

    if (tag == NULL || tag[0] == '\\0')
    {{
        return NULL;
    }}

    taglen = strlen(tag);

    for (i = 0; i < PTAB_ENTRY_COUNT; i++)
    {{
        const char *p = g_ptab_table[i].tags;

        while (p != NULL && *p != '\\0')
        {{
            const char *sep = strchr(p, '|');
            size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);

            if (len == taglen && strncmp(p, tag, taglen) == 0)
            {{
                return &g_ptab_table[i];
            }}

            if (sep == NULL)
            {{
                break;
            }}

            p = sep + 1;
        }}
    }}

    return NULL;
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ptab", required=True, type=Path)
    parser.add_argument("--storage", default="nand")
    parser.add_argument("--out-c", required=True, type=Path)
    parser.add_argument("--out-h", required=True, type=Path)
    args = parser.parse_args()

    if not args.ptab.is_file():
        print(f"ERROR: missing ptab json: {args.ptab}", file=sys.stderr)
        return 1

    storage = args.storage.strip().lower() or "nand"
    ptab_name = args.ptab.name
    entries = parse_entries(args.ptab)

    args.out_c.parent.mkdir(parents=True, exist_ok=True)
    args.out_h.parent.mkdir(parents=True, exist_ok=True)
    args.out_h.write_text(
        render_header(storage, ptab_name, len(entries)), encoding="utf-8"
    )
    args.out_c.write_text(
        render_source(storage, ptab_name, entries), encoding="utf-8"
    )

    _scripts = Path(__file__).resolve().parents[2] / "scripts"
    sys.path.insert(0, str(_scripts))
    import flash_args_lib as fal  # noqa: E402

    fal.print_ptab_build_summary(args.ptab.resolve(), storage, stage="gen-ptab-table")
    print(f"Generated {args.out_c} and {args.out_h}")
    print(f"  entries = {len(entries)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
