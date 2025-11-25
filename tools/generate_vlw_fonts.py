#!/usr/bin/env python3
"""
Convert TTF fonts into VLW bitmap fonts for LovyanGFX / M5GFX.

This is a minimal converter that produces the subset of glyphs we need
for the e-reader: ASCII, Latin-1 punctuation, and the full Hebrew block.

Usage (run from repo root):
    python tools/generate_vlw_fonts.py

Requires Pillow:
    pip install pillow
"""
import os
import struct
from pathlib import Path
from typing import Iterable, List, Tuple

try:
    from PIL import ImageFont, Image, ImageDraw
except ImportError as exc:
    raise SystemExit("Pillow is required. Install with: pip install pillow") from exc


# Fonts to convert: (source_path, output_path, pixel_size)
FONT_JOBS = [
    ("spiffs_image/fonts/NotoSansHebrew-Regular.ttf", "spiffs_image/fonts/NotoSansHebrew-Regular.vlw", 24),
    ("spiffs_image/fonts/Roboto-Regular.ttf", "spiffs_image/fonts/Roboto-Regular.vlw", 24),
]


def codepoint_ranges() -> List[int]:
    """Return the list of codepoints to bake into the VLW font."""
    cp: List[int] = []
    cp += list(range(32, 127))          # Basic ASCII
    cp += list(range(160, 256))         # Latin-1 punctuation / symbols
    cp += list(range(0x2000, 0x2070))   # Common punctuation (en dash, etc.)
    cp += list(range(0x590, 0x5FF))     # Hebrew
    return sorted(set(cp))


def build_vlw(ttf_path: Path, out_path: Path, size: int) -> None:
    font = ImageFont.truetype(str(ttf_path), size)
    ascent, descent = font.getmetrics()
    y_advance = ascent + descent

    dummy = Image.new("L", (1, 1))
    draw = ImageDraw.Draw(dummy)

    glyphs: List[Tuple[int, int, int, int, int, int, bytes]] = []
    for cp in codepoint_ranges():
        ch = chr(cp)
        mask = font.getmask(ch, mode="L")
        w, h = mask.size
        if w == 0 or h == 0:
            continue  # skip empty glyphs
        bbox = draw.textbbox((0, 0), ch, font=font)
        x_advance = bbox[2] - bbox[0]
        d_y = ascent  # baseline from top of bitmap
        g_dx = bbox[0]
        glyphs.append((cp, h, w, x_advance, d_y, g_dx, bytes(mask)))

    buf = bytearray()
    # Header: glyph count, encoder version (unused), yAdvance, reserved, ascent, descent
    buf += struct.pack(">6I", len(glyphs), 0, y_advance, 0, ascent, descent)

    # Glyph table (7 * 4 bytes each, big endian)
    for cp, h, w, x_adv, d_y, g_dx, _ in glyphs:
        buf += struct.pack(">IIIIIii", cp, h, w, x_adv, d_y, g_dx, 0)

    # Bitmap data (1 byte per pixel, no padding)
    for _, h, w, _, _, _, bm in glyphs:
        if len(bm) != w * h:
            raise ValueError(f"Bitmap size mismatch for codepoint {cp}")
        buf += bm

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as f:
        f.write(buf)
    print(f"Wrote {out_path} (glyphs: {len(glyphs)}, bytes: {len(buf)})")


def main() -> None:
    for src, dst, size in FONT_JOBS:
        ttf = Path(src)
        out = Path(dst)
        if not ttf.exists():
            print(f"Skip {ttf} (missing)")
            continue
        build_vlw(ttf, out, size)


if __name__ == "__main__":
    main()
