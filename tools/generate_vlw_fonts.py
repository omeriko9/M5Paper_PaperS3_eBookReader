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


# Fonts to convert: (output_path, pixel_size, [ (source_path, [ranges]) ])
FONT_JOBS = [
    ("spiffs_image/fonts/Hebrew-Merged.vlw", 24, [
        ("regular-fonts/NotoSansHebrew-Regular.ttf", [range(0x0590, 0x0600)]), # Hebrew
        ("regular-fonts/Roboto-Regular.ttf", [
            range(32, 127),         # ASCII
            range(160, 256),        # Latin-1
            range(0x2000, 0x2070),  # Punctuation
            range(0x20A0, 0x20D0),  # Currency
            range(0x2100, 0x2150),  # Letterlike
            range(0x2190, 0x2200),  # Arrows
        ])
    ]),
    ("spiffs_image/fonts/Roboto-Regular.vlw", 24, [
        ("regular-fonts/Roboto-Regular.ttf", [
            range(32, 127),
            range(160, 256),
            range(0x2000, 0x2070)
        ])
    ]),
    ("spiffs_image/fonts/Arabic-Merged.vlw", 24, [
        ("Ramis Arabic.otf", [
            range(0x0600, 0x0700),  # Arabic
            range(0x0750, 0x0780),  # Arabic Supplement
            range(0x08A0, 0x08FF),  # Arabic Extended-A
            range(0xFB50, 0xFDFF),  # Arabic Presentation Forms-A
            range(0xFE70, 0xFEFF),  # Arabic Presentation Forms-B
        ]),
        ("regular-fonts/Roboto-Regular.ttf", [
            range(32, 127),         # ASCII
            range(160, 256),        # Latin-1
            range(0x2000, 0x2070),  # Punctuation
        ])
    ]),
]

def build_vlw(out_path: Path, size: int, sources: List[Tuple[str, List[Iterable[int]]]]) -> None:
    # Load all source fonts
    loaded_fonts = []
    for src_path, ranges in sources:
        try:
            font = ImageFont.truetype(src_path, size)
            loaded_fonts.append((font, ranges))
        except Exception as e:
            print(f"Error loading {src_path}: {e}")
            return

    # Determine global metrics (max ascent/descent)
    max_ascent = 0
    max_descent = 0
    for font, _ in loaded_fonts:
        a, d = font.getmetrics()
        max_ascent = max(max_ascent, a)
        max_descent = max(max_descent, d)
    
    y_advance = max_ascent + max_descent

    dummy = Image.new("L", (1, 1))
    draw = ImageDraw.Draw(dummy)

    glyphs: List[Tuple[int, int, int, int, int, int, bytes]] = []
    processed_cps = set()

    # Process fonts in order
    for font, ranges in loaded_fonts:
        # Flatten ranges
        cps = []
        for r in ranges:
            cps.extend(list(r))
        
        for cp in sorted(set(cps)):
            if cp in processed_cps:
                continue
            
            ch = chr(cp)
            try:
                mask = font.getmask(ch, mode="L")
            except Exception:
                continue
                
            w, h = mask.size
            if w == 0 or h == 0:
                continue  # skip empty glyphs
            
            bbox = draw.textbbox((0, 0), ch, font=font)
            x_advance = round(font.getlength(ch))
            
            # Use font-specific ascent for d_y calculation to align baselines
            ascent, _ = font.getmetrics()
            d_y = ascent - bbox[1]
            g_dx = bbox[0]
            
            glyphs.append((cp, h, w, x_advance, d_y, g_dx, bytes(mask)))
            processed_cps.add(cp)

    glyphs.sort(key=lambda x: x[0]) # Sort by codepoint

    buf = bytearray()
    # Header: glyph count, encoder version (unused), yAdvance, reserved, ascent, descent
    buf += struct.pack(">6I", len(glyphs), 0, y_advance, 0, max_ascent, max_descent)

    # Glyph table (7 * 4 bytes each, big endian)
    for cp, h, w, x_adv, d_y, g_dx, _ in glyphs:
        buf += struct.pack(">IIIIIii", cp, h, w, x_adv, d_y, g_dx, 0)

    # Bitmap data (1 byte per pixel, no padding)
    for _, h, w, _, _, _, bm in glyphs:
        buf += bm

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as f:
        f.write(buf)
    print(f"Wrote {out_path} (glyphs: {len(glyphs)}, bytes: {len(buf)})")


def main() -> None:
    for dst, size, sources in FONT_JOBS:
        out = Path(dst)
        build_vlw(out, size, sources)


if __name__ == "__main__":
    main()
