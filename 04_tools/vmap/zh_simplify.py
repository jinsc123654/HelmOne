#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Traditional-to-simplified Chinese for OSM names (OpenCC TSCharacters)."""

from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_TS_PATH = os.path.join(_HERE, "data", "TSCharacters.txt")

_NAME_EXACT = {
    "name",
    "official_name",
    "short_name",
    "loc_name",
    "alt_name",
    "int_name",
}

# Prefer script-specific tags, then generic name / English.
NAME_KEYS_HANS = ("name:zh-Hans", "name:zh-CN")
NAME_KEYS_HANT = ("name:zh-Hant", "name:zh-TW", "name:zh-HK")
NAME_KEYS_ANY = ("name:zh", "name", "official_name", "name:en", "int_name")

SCRIPT_HANS = "hans"
SCRIPT_HANT = "hant"

_ts_map: dict[str, str] | None = None
_st_map: dict[str, str] | None = None
_script = SCRIPT_HANS


def _load_ts_map() -> dict[str, str]:
    global _ts_map, _st_map
    if _ts_map is not None:
        return _ts_map
    ts: dict[str, str] = {}
    st: dict[str, str] = {}
    if not os.path.isfile(_TS_PATH):
        print("[zh] missing %s" % _TS_PATH, file=sys.stderr)
        _ts_map, _st_map = ts, st
        return ts
    with open(_TS_PATH, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            trad, simp = parts[0], parts[1]
            if not trad or not simp or simp == trad:
                continue
            ts.setdefault(trad, simp)
            st.setdefault(simp, trad)
    _ts_map, _st_map = ts, st
    return ts


def _load_st_map() -> dict[str, str]:
    _load_ts_map()
    return _st_map or {}


def set_script(script: str) -> None:
    """hans = default simplified; hant = special traditional conversion."""
    global _script
    s = (script or SCRIPT_HANS).lower()
    if s not in (SCRIPT_HANS, SCRIPT_HANT):
        raise ValueError("zh script must be hans or hant, got %r" % script)
    _script = s


def add_zh_argument(ap) -> None:
    ap.add_argument(
        "--zh",
        choices=(SCRIPT_HANS, SCRIPT_HANT),
        default=SCRIPT_HANS,
        help="name script: hans=simplified (default), hant=traditional (special)",
    )


def name_keys(allow_ref: bool = False) -> tuple[str, ...]:
    preferred = NAME_KEYS_HANT if _script == SCRIPT_HANT else NAME_KEYS_HANS
    keys = preferred + NAME_KEYS_ANY
    if allow_ref:
        keys += ("ref",)
    return keys


def convert_name(text: str) -> str:
    """Default hans (t2s). Traditional is --zh hant (s2t)."""
    if _script == SCRIPT_HANT:
        return to_traditional(text)
    return to_simplified(text)


def is_name_tag(key: str) -> bool:
    return key in _NAME_EXACT or key.startswith("name:")


def to_simplified(text: str) -> str:
    """Map each convertible traditional character to simplified."""
    if not text:
        return text
    m = _load_ts_map()
    if not m:
        return text
    return "".join(m.get(c, c) for c in text)


def to_traditional(text: str) -> str:
    """Map each convertible simplified character to traditional."""
    if not text:
        return text
    m = _load_st_map()
    if not m:
        return text
    return "".join(m.get(c, c) for c in text)


def is_device_font_char(ch: str) -> bool:
    """Keep glyphs a Chinese map TTF can actually render (CJK + Latin + punct)."""
    o = ord(ch)
    if 0x20 <= o <= 0x7E:
        return True
    if 0xA0 <= o <= 0x24F:  # Latin-1 / Extended-A (pinyin-ish)
        return True
    if 0x1E00 <= o <= 0x1EFF:
        return True
    if 0x2000 <= o <= 0x206F:
        return True
    if 0x2E80 <= o <= 0x2EFF:
        return True
    if 0x2F00 <= o <= 0x2FDF:
        return True
    if 0x3000 <= o <= 0x303F:
        return True
    if 0x3040 <= o <= 0x30FF:
        return True
    if 0x3100 <= o <= 0x312F:
        return True
    if 0x31C0 <= o <= 0x31EF:
        return True
    if 0x3400 <= o <= 0x4DBF:
        return True
    if 0x4E00 <= o <= 0x9FFF:
        return True
    if 0xF900 <= o <= 0xFAFF:
        return True
    if 0xFF00 <= o <= 0xFFEF:
        return True
    if 0x20000 <= o <= 0x2A6DF:
        return True
    if 0x2A700 <= o <= 0x2B73F:
        return True
    if 0x2B740 <= o <= 0x2B81F:
        return True
    if 0x2B820 <= o <= 0x2CEAF:
        return True
    if 0x2EBF0 <= o <= 0x2EE5F:
        return True
    if 0x30000 <= o <= 0x3134F:
        return True
    return False


def drop_convertible_traditional(chars: set[str]) -> set[str]:
    """Keep simplified forms only; drop traditional chars that have a mapping."""
    m = _load_ts_map()
    out: set[str] = set()
    for c in chars:
        s = m.get(c, c)
        if s != c:
            out.update(s)
        else:
            out.add(c)
    return out


class NameScan:
    """Collect printable chars from OSM name tags after t2s."""

    def __init__(self) -> None:
        self.chars: set[str] = set()
        self.n_tags = 0
        self.n_changed = 0

    def add_text(self, text: str) -> str:
        if not text:
            return text
        self.n_tags += 1
        converted = convert_name(text)
        if converted != text:
            self.n_changed += 1
        for c in converted:
            if c.isprintable():
                self.chars.add(c)
        return converted

    def add_tags(self, tags: dict) -> None:
        for k, v in tags.items():
            if v and is_name_tag(k):
                self.add_text(v)

    def finish(self, device_font: bool = True) -> set[str]:
        if _script == SCRIPT_HANS:
            self.chars = drop_convertible_traditional(self.chars)
        if device_font:
            self.chars = {c for c in self.chars if is_device_font_char(c)}
        return self.chars


def scan_osm_file(
    osm_path: str,
    progress_every: int = 2_000_000,
    device_font: bool = True,
) -> NameScan:
    """Tags-only scan of .osm / .pbf name keys (no geometry)."""
    import osmium

    scan = NameScan()
    n_obj = [0]

    class Handler(osmium.SimpleHandler):
        def node(self, n):
            self._eat(n)

        def way(self, w):
            self._eat(w)

        def relation(self, r):
            self._eat(r)

        def _eat(self, obj):
            n_obj[0] += 1
            if progress_every and n_obj[0] % progress_every == 0:
                print(
                    "[zh] scanned %d objects, %d name tags, %d chars"
                    % (n_obj[0], scan.n_tags, len(scan.chars)),
                    file=sys.stderr,
                    flush=True,
                )
            for t in obj.tags:
                if t.v and is_name_tag(t.k):
                    scan.add_text(t.v)

    print("[zh] scan %s" % osm_path, file=sys.stderr, flush=True)
    Handler().apply_file(osm_path, locations=False)
    scan.finish(device_font=device_font)
    print(
        "[zh] done objects=%d names=%d converted=%d chars=%d"
        % (n_obj[0], scan.n_tags, scan.n_changed, len(scan.chars)),
        file=sys.stderr,
        flush=True,
    )
    return scan


def write_chars_file(path: str, scan: NameScan) -> None:
    chars = "".join(sorted(scan.chars, key=lambda c: ord(c)))
    cjk = sum(1 for c in chars if "\u4e00" <= c <= "\u9fff")
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(
            "# OSM names after %s: %d chars (%d CJK), %d name tags, "
            "%d converted\n"
            % (_script, len(chars), cjk, scan.n_tags, scan.n_changed)
        )
        fp.write(chars)
        fp.write("\n")
    print(
        "[zh] %s %d/%d name tags changed, charset %d (%d CJK) -> %s"
        % (_script, scan.n_changed, scan.n_tags, len(chars), cjk, path),
        file=sys.stderr,
    )
