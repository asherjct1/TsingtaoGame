#!/usr/bin/env python3
"""
TSINGTAO EXPRESS '96 — Deterministic ZERO-AI PlayStation 1 Asset Compiler.

Inputs:
  assets/raw/tsingtao_logo.png   (genuine Tsingtao Brewery logo)
  assets/raw/tsingtao_can.png    (genuine green Tsingtao Beer can photo)
  assets/raw/tsingtao_bottle.png (genuine green Tsingtao Beer bottle photo)

Outputs (VRAM 1024x512 non-overlapping layout):
  assets/gen/real_tsingtao.tim   (8-bit CLUT, img 320,0 [128x128hw], clut 320,240 [256x1])
  assets/gen/sprites.tim         (8-bit CLUT, img 448,0 [128x240hw], clut 320,241 [256x1])
  assets/gen/env_ui.tim          (4-bit CLUT, img 576,0 [ 64x240hw], clut 576,240 [ 16x1])
  assets/gen/cards.tim           (8-bit CLUT, img 640,0 [128x240hw], clut 320,242 [256x1])
  assets/gen/assets_layout.h     (VRAM, TPage/CLUT, and UV definitions)
  assets/gen/tim_blobs.h/.c      (4-byte aligned C byte arrays of all 4 TIMs)
  assets/gen/mc_icon.h           (16x16 4-bit PS1 Memory Card Tsingtao Can icon)

Hardware Safety Guarantees:
  - Index 0 (RGB 0,0,0 -> 0x0000) is reserved strictly for PS1 transparency.
  - All opaque colors are clamped >= (8,8,8) (RGB555 >= 0x0421) with STP=1 so
    opaque sprites never punch accidental black holes AND semi-trans works on
    real PS1 GPUs.
  - Zero VRAM overlaps between FB0 (0,0..319,255), FB1 (0,256..319,511),
    all 4 texture sheets, and all 4 CLUTs.
"""

import argparse
import math
import struct
import sys
from collections import deque
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
RAW_DIR = ROOT / "assets" / "raw"
GEN_DIR = ROOT / "assets" / "gen"

RGB = Tuple[int, int, int]


# ---------------------------------------------------------------------------
# PS1 RGB555 & TIM Binary Helpers
# ---------------------------------------------------------------------------

def rgb_to_ps1_555(r: int, g: int, b: int, transparent: bool = False, stp: bool = True) -> int:
    """Convert 8-bit RGB to PS1 16-bit RGB5551 halfword (clamping opaque >= 8,8,8)."""
    if transparent:
        return 0x0000
    r5 = min(31, max(0, (r + 4) >> 3))
    g5 = min(31, max(0, (g + 4) >> 3))
    b5 = min(31, max(0, (b + 4) >> 3))
    if r5 == 0 and g5 == 0 and b5 == 0:
        r5 = g5 = b5 = 1
    bit15 = 0x8000 if stp else 0x0000
    return bit15 | (b5 << 10) | (g5 << 5) | r5


def ps1_555_to_rgb(val: int) -> Tuple[int, int, int, int]:
    """Decode PS1 16-bit RGB5551 to 8-bit RGBA for verification & previews."""
    if val == 0x0000:
        return (0, 0, 0, 0)
    r5 = val & 0x1F
    g5 = (val >> 5) & 0x1F
    b5 = (val >> 10) & 0x1F
    return (r5 * 255 // 31, g5 * 255 // 31, b5 * 255 // 31, 255)


def write_tim(
    path: Path,
    bpp: int,
    img_x: int,
    img_y: int,
    clut_x: int,
    clut_y: int,
    palette_rgb: List[RGB],
    indices: List[List[int]],
) -> bytes:
    """Encode a valid little-endian PlayStation 1 .TIM file (4-bit or 8-bit CLUT)."""
    assert bpp in (4, 8), "Only 4-bit and 8-bit CLUT TIMs supported"
    h = len(indices)
    w = len(indices[0])
    if bpp == 4:
        assert w % 4 == 0, f"4-bit TIM width {w} must be multiple of 4"
        assert len(palette_rgb) == 16
        flags = 0x00000008
        clut_w = 16
        img_w_hw = w // 4
    else:
        assert w % 2 == 0, f"8-bit TIM width {w} must be multiple of 2"
        assert len(palette_rgb) == 256
        flags = 0x00000009
        clut_w = 256
        img_w_hw = w // 2

    assert img_x % 64 == 0, f"IMG X ({img_x}) should be aligned to 64-hw TPage boundary"
    assert clut_x % 16 == 0, f"CLUT X ({clut_x}) must be a multiple of 16"

    clut_words = [0x0000]
    for r, g, b in palette_rgb[1:]:
        word = rgb_to_ps1_555(r, g, b, transparent=False, stp=True)
        assert (word & 0x7FFF) != 0 and (word & 0x8000) != 0
        clut_words.append(word)

    clut_payload = struct.pack(f"<{len(clut_words)}H", *clut_words)
    clut_bnum = 12 + len(clut_payload)
    clut_block = struct.pack("<IHHHH", clut_bnum, clut_x, clut_y, clut_w, 1) + clut_payload

    pix_bytes = bytearray()
    if bpp == 4:
        for row in indices:
            for x in range(0, w, 2):
                lo = row[x] & 0x0F
                hi = row[x + 1] & 0x0F
                pix_bytes.append((hi << 4) | lo)
    else:
        for row in indices:
            pix_bytes.extend(v & 0xFF for v in row)

    img_bnum = 12 + len(pix_bytes)
    img_block = struct.pack("<IHHHH", img_bnum, img_x, img_y, img_w_hw, h) + bytes(pix_bytes)

    tim_data = struct.pack("<II", 0x00000010, flags) + clut_block + img_block
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(tim_data)
    return tim_data


def decode_tim_to_rgba(tim_data: bytes) -> Tuple[Image.Image, Dict[str, int]]:
    """Parse a .TIM binary back into an RGBA Pillow image + VRAM metadata."""
    magic, flags = struct.unpack_from("<II", tim_data, 0)
    assert magic == 0x10, f"Bad TIM magic: {magic:#x}"
    bpp_mode = flags & 0x07
    has_clut = bool(flags & 0x08)
    assert has_clut and bpp_mode in (0, 1)
    off = 8
    clut_bnum, cx, cy, cw, ch = struct.unpack_from("<IHHHH", tim_data, off)
    clut_words = struct.unpack_from(f"<{cw * ch}H", tim_data, off + 12)
    for ci, wval in enumerate(clut_words):
        if ci == 0:
            assert wval == 0x0000, f"Index 0 must be transparent 0x0000, got {wval:#x}"
        else:
            assert (wval & 0x7FFF) != 0, f"CLUT[{ci}] is black-hole {wval:#x}"
            assert (wval & 0x8000) != 0, f"CLUT[{ci}] missing STP=1 ({wval:#x})"

    off += clut_bnum
    img_bnum, ix, iy, iw_hw, ih = struct.unpack_from("<IHHHH", tim_data, off)
    pix = tim_data[off + 12 : off + img_bnum]

    pw = iw_hw * 4 if bpp_mode == 0 else iw_hw * 2
    im = Image.new("RGBA", (pw, ih), (0, 0, 0, 0))
    px = im.load()
    if bpp_mode == 0:
        for y in range(ih):
            row_off = y * (pw // 2)
            for x in range(0, pw, 2):
                b = pix[row_off + (x // 2)]
                px[x, y] = ps1_555_to_rgb(clut_words[b & 0x0F])
                px[x + 1, y] = ps1_555_to_rgb(clut_words[(b >> 4) & 0x0F])
    else:
        for y in range(ih):
            row_off = y * pw
            for x in range(pw):
                px[x, y] = ps1_555_to_rgb(clut_words[pix[row_off + x]])

    meta = {
        "bpp": 4 if bpp_mode == 0 else 8,
        "img_x": ix, "img_y": iy, "img_w_hw": iw_hw, "img_h": ih,
        "clut_x": cx, "clut_y": cy, "clut_w_hw": cw, "clut_h": ch,
        "pixel_w": pw, "pixel_h": ih,
    }
    return im, meta


# ---------------------------------------------------------------------------
# Part 1: Real Tsingtao Photo Segmentation & 8-bit Retro Quantization
# ---------------------------------------------------------------------------

def _sat(r: int, g: int, b: int) -> float:
    mx = max(r, g, b)
    mn = min(r, g, b)
    return 0.0 if mx == 0 else (mx - mn) / mx


def _largest_component_mask(fg: bytearray, w: int, h: int) -> bytearray:
    visited = bytearray(w * h)
    best_comp: List[int] = []
    for y in range(h):
        for x in range(w):
            idx = y * w + x
            if fg[idx] and not visited[idx]:
                comp = []
                q = deque([(x, y)])
                visited[idx] = 1
                while q:
                    cx, cy = q.popleft()
                    comp.append(cy * w + cx)
                    for nx, ny in ((cx - 1, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)):
                        if 0 <= nx < w and 0 <= ny < h:
                            nidx = ny * w + nx
                            if fg[nidx] and not visited[nidx]:
                                visited[nidx] = 1
                                q.append((nx, ny))
                if len(comp) > len(best_comp):
                    best_comp = comp
    out = bytearray(w * h)
    for idx in best_comp:
        out[idx] = 1
    return out


def _fill_internal_holes(fg: bytearray, w: int, h: int) -> bytearray:
    ext = bytearray(w * h)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            idx = y * w + x
            if not fg[idx] and not ext[idx]:
                ext[idx] = 1
                q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            idx = y * w + x
            if not fg[idx] and not ext[idx]:
                ext[idx] = 1
                q.append((x, y))
    while q:
        cx, cy = q.popleft()
        for nx, ny in ((cx - 1, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                nidx = ny * w + nx
                if not fg[nidx] and not ext[nidx]:
                    ext[nidx] = 1
                    q.append((nx, ny))
    out = bytearray(w * h)
    for i in range(w * h):
        out[i] = 0 if ext[i] else 1
    return out


def extract_real_logo(path: Path) -> Image.Image:
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            px[x, y] = (0, 0, 0, 0) if a < 110 else (r, g, b, 255)
    bbox = im.getbbox()
    return im.crop(bbox) if bbox else im


def extract_real_can(path: Path) -> Image.Image:
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    px = im.load()
    if min(px[x, y][3] for y in range(0, h, 4) for x in range(0, w, 4)) < 128:
        bbox = im.getbbox()
        return im.crop(bbox) if bbox else im

    visited = bytearray(w * h)
    bg = bytearray(w * h)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            idx = y * w + x
            if not visited[idx]:
                visited[idx] = 1
                q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            idx = y * w + x
            if not visited[idx]:
                visited[idx] = 1
                q.append((x, y))

    while q:
        x, y = q.popleft()
        r, g, b, _ = px[x, y]
        s = _sat(r, g, b)
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        is_bg = (s < 0.16 and lum > 138) or (s < 0.21 and lum > 170 and (r - g) > -4)
        if y > int(h * 0.77) and s < 0.15 and lum > 110:
            is_bg = True
        if not is_bg:
            continue
        bg[y * w + x] = 1
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                nidx = ny * w + nx
                if not visited[nidx]:
                    visited[nidx] = 1
                    q.append((nx, ny))

    fg = bytearray(0 if b else 1 for b in bg)
    fg = _largest_component_mask(fg, w, h)
    fg = _fill_internal_holes(fg, w, h)

    row_widths = [sum(fg[y * w + x] for x in range(w)) for y in range(h)]
    max_w = max(row_widths) if row_widths else 1
    for y in range(h - 1, int(h * 0.75), -1):
        if row_widths[y] < int(max_w * 0.35):
            for x in range(w):
                fg[y * w + x] = 0
        else:
            break

    out = im.copy()
    opx = out.load()
    for y in range(h):
        for x in range(w):
            if not fg[y * w + x]:
                opx[x, y] = (0, 0, 0, 0)
    bbox = out.getbbox()
    return out.crop(bbox) if bbox else out


def extract_real_bottle(path: Path) -> Image.Image:
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    px = im.load()
    if min(px[x, y][3] for y in range(0, h, 4) for x in range(0, w, 4)) < 128:
        bbox = im.getbbox()
        return im.crop(bbox) if bbox else im

    visited = bytearray(w * h)
    bg = bytearray(w * h)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            idx = y * w + x
            if not visited[idx]:
                visited[idx] = 1
                q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            idx = y * w + x
            if not visited[idx]:
                visited[idx] = 1
                q.append((x, y))

    while q:
        x, y = q.popleft()
        r, g, b, _ = px[x, y]
        s = _sat(r, g, b)
        is_wall = (s < 0.22 and b >= g - 6 and g <= r + 8 and 55 < max(r, g, b) < 210)
        is_shelf = (y > int(h * 0.72) and g < r - 5 and g < b - 4 and s < 0.45)
        if not (is_wall or is_shelf):
            continue
        bg[y * w + x] = 1
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                nidx = ny * w + nx
                if not visited[nidx]:
                    visited[nidx] = 1
                    q.append((nx, ny))

    fg = bytearray(0 if b else 1 for b in bg)
    fg = _largest_component_mask(fg, w, h)

    centers = []
    for y in range(int(h * 0.54), int(h * 0.85)):
        xs = [
            x for x in range(w)
            if (px[x, y][1] > px[x, y][0] + 14 and px[x, y][1] > px[x, y][2] + 14)
            or (px[x, y][0] > px[x, y][1] + 28 and px[x, y][0] > px[x, y][2] + 20)
        ]
        if len(xs) > w * 0.35:
            centers.append(0.5 * (xs[0] + xs[-1]))
    cx = sum(centers) / len(centers) if centers else w * 0.5

    raw_halfs = []
    for y in range(h):
        xs = []
        for x in range(w):
            if not fg[y * w + x]:
                continue
            r, g, b, _ = px[x, y]
            s = _sat(r, g, b)
            is_shelf = (y > int(h * 0.72) and g < r - 5 and g < b - 4 and s < 0.45)
            is_grey = (s < 0.18 and b >= g - 5 and g <= r + 8)
            if not is_shelf and not is_grey:
                xs.append(x)
        if not xs:
            raw_halfs.append(0.0)
            continue
        left_d = max(0.0, cx - xs[0])
        right_d = max(0.0, xs[-1] - cx)
        half = 0.5 * (left_d + right_d) if y < int(h * 0.12) else (left_d if left_d > 0 else right_d)
        raw_halfs.append(half)

    barrel_vals = sorted(v for v in raw_halfs[int(h * 0.52) : int(h * 0.78)] if v > 0)
    max_barrel = barrel_vals[len(barrel_vals) // 2] + 2.0 if barrel_vals else w * 0.45
    raw_halfs = [min(v, max_barrel) for v in raw_halfs]

    for y in range(h):
        if raw_halfs[y] <= 0.0:
            for x in range(w):
                fg[y * w + x] = 0
            continue
        win = sorted(raw_halfs[max(0, y - 3) : min(h, y + 4)])
        half = win[len(win) // 2]
        x0 = max(0, int(round(cx - half)))
        x1 = min(w - 1, int(round(cx + half)))
        for x in range(w):
            fg[y * w + x] = 1 if (x0 <= x <= x1) else 0

    fg = _largest_component_mask(fg, w, h)
    fg = _fill_internal_holes(fg, w, h)

    out = im.copy()
    opx = out.load()
    for y in range(h):
        for x in range(w):
            if not fg[y * w + x]:
                opx[x, y] = (0, 0, 0, 0)
    bbox = out.getbbox()
    return out.crop(bbox) if bbox else out


def fit_sprite_rgba(
    src: Image.Image,
    target_w: int,
    target_h: int,
    pad: int = 0,
    boost_sat: float = 1.12,
    boost_con: float = 1.08,
    alpha_thresh: int = 115,
) -> Image.Image:
    """Resize RGBA cutout cleanly into (target_w, target_h) preserving aspect ratio & crisp alpha."""
    avail_w = max(1, target_w - pad * 2)
    avail_h = max(1, target_h - pad * 2)
    sw, sh = src.size
    scale = min(avail_w / sw, avail_h / sh)
    nw = max(1, int(round(sw * scale)))
    nh = max(1, int(round(sh * scale)))

    rgb = src.convert("RGB")
    rgb = ImageEnhance.Color(rgb).enhance(boost_sat)
    rgb = ImageEnhance.Contrast(rgb).enhance(boost_con)
    alpha = src.split()[3]

    rgb_small = rgb.resize((nw, nh), Image.Resampling.LANCZOS)
    rgb_small = rgb_small.filter(ImageFilter.UnsharpMask(radius=0.75, percent=115, threshold=2))
    alpha_small = alpha.resize((nw, nh), Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
    ox = (target_w - nw) // 2
    oy = (target_h - nh) // 2
    rp = rgb_small.load()
    ap = alpha_small.load()
    cp = canvas.load()
    for y in range(nh):
        for x in range(nw):
            if ap[x, y] >= alpha_thresh:
                r, g, b = rp[x, y]
                if r < 8 and g < 8 and b < 8:
                    r = g = b = 8
                cp[ox + x, oy + y] = (r, g, b, 255)
    return canvas


BAYER_4X4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]


def quantize_real_sheet_8bit(sheet_rgba: Image.Image) -> Tuple[List[RGB], List[List[int]]]:
    """Quantize composite RGBA real-photo sheet to 255 opaque colors + index 0 transparent."""
    w, h = sheet_rgba.size
    px = sheet_rgba.load()

    opaque_rgb = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a >= 128:
                opaque_rgb.append((r, g, b))

    strip_w = max(1, min(256, len(opaque_rgb)))
    strip_h = (len(opaque_rgb) + strip_w - 1) // strip_w
    strip = Image.new("RGB", (strip_w, strip_h), opaque_rgb[0] if opaque_rgb else (16, 144, 64))
    sp = strip.load()
    for i, c in enumerate(opaque_rgb):
        sp[i % strip_w, i // strip_w] = c

    q_img = strip.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    raw_pal = q_img.getpalette()[: 255 * 3]
    palette: List[RGB] = [(0, 0, 0)]
    for i in range(255):
        r, g, b = raw_pal[i * 3], raw_pal[i * 3 + 1], raw_pal[i * 3 + 2]
        if r < 8 and g < 8 and b < 8:
            r = g = b = 8
        palette.append((r & 0xF8, g & 0xF8, b & 0xF8))

    cache: Dict[RGB, int] = {}
    indices = [[0] * w for _ in range(h)]
    pal_entries = list(enumerate(palette[1:], start=1))

    for y in range(h):
        bayer_row = BAYER_4X4[y & 3]
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                indices[y][x] = 0
                continue
            d = (bayer_row[x & 3] - 7.5) * 0.70
            dr = min(255, max(8, int(r + d)))
            dg = min(255, max(8, int(g + d)))
            db = min(255, max(8, int(b + d)))
            key = (dr >> 2, dg >> 2, db >> 2)
            idx = cache.get(key)
            if idx is None:
                best_i = 1
                best_d = 1 << 30
                for pi, (pr, pg, pb) in pal_entries:
                    dist = 2 * (dr - pr) ** 2 + 4 * (dg - pg) ** 2 + 3 * (db - pb) ** 2
                    if dist < best_d:
                        best_d = dist
                        best_i = pi
                idx = best_i
                cache[key] = idx
            indices[y][x] = idx

    return palette, indices


def build_real_tsingtao_sheet() -> Tuple[List[RGB], List[List[int]]]:
    """Assemble the 256x128 8-bit sheet of real digitized Tsingtao assets."""
    logo_cut = extract_real_logo(RAW_DIR / "tsingtao_logo.png")
    can_cut = extract_real_can(RAW_DIR / "tsingtao_can.png")
    bottle_cut = extract_real_bottle(RAW_DIR / "tsingtao_bottle.png")

    sheet = Image.new("RGBA", (256, 128), (0, 0, 0, 0))

    # 1. Title Logo (128x64 at 0,0)
    sheet.paste(fit_sprite_rgba(logo_cut, 128, 64, pad=1, boost_sat=1.10, boost_con=1.08), (0, 0))
    # 2. Hero Shell-Game Can (32x48 at 128,0)
    sheet.paste(fit_sprite_rgba(can_cut, 32, 48, pad=1, boost_sat=1.18, boost_con=1.10), (128, 0))
    # 3. Hero Title Bottle (24x60 at 164,0)
    sheet.paste(fit_sprite_rgba(bottle_cut, 24, 60, pad=1, boost_sat=1.18, boost_con=1.10), (164, 0))
    # 4. Gameplay Can (12x18 at 192,0) — rescaled to realistic human proportion!
    sheet.paste(fit_sprite_rgba(can_cut, 12, 18, pad=0, boost_sat=1.24, boost_con=1.14, alpha_thresh=105), (192, 0))
    # 5. Gameplay Bottle (12x22 at 208,0) — rescaled to realistic human proportion!
    sheet.paste(fit_sprite_rgba(bottle_cut, 12, 22, pad=0, boost_sat=1.24, boost_con=1.14, alpha_thresh=105), (208, 0))
    # 6. Bonus/HUD Mini Can (10x14 at 224,0)
    sheet.paste(fit_sprite_rgba(can_cut, 10, 14, pad=0, boost_sat=1.22, boost_con=1.14, alpha_thresh=100), (224, 0))
    # 7. Showcase Can (40x60 at 128,64)
    sheet.paste(fit_sprite_rgba(can_cut, 40, 60, pad=1, boost_sat=1.18, boost_con=1.10), (128, 64))

    return quantize_real_sheet_8bit(sheet)


# ---------------------------------------------------------------------------
# Part 2: Indexed Pixel Canvas & 256-Color Palette Builder
# ---------------------------------------------------------------------------

class Palette256:
    """Automatic 255-color PS1 RGB555 palette registry (index 0 = transparent)."""

    def __init__(self):
        self.colors: List[RGB] = [(0, 0, 0)]
        self.map: Dict[RGB, int] = {}

    def c(self, rgb: RGB) -> int:
        r, g, b = rgb
        r5 = min(31, max(0, (r + 4) >> 3))
        g5 = min(31, max(0, (g + 4) >> 3))
        b5 = min(31, max(0, (b + 4) >> 3))
        if r5 == 0 and g5 == 0 and b5 == 0:
            r5 = g5 = b5 = 1
        snapped = (r5 << 3, g5 << 3, b5 << 3)
        idx = self.map.get(snapped)
        if idx is None:
            if len(self.colors) >= 256:
                raise RuntimeError("Palette256 exceeded 255 opaque entries")
            idx = len(self.colors)
            self.colors.append(snapped)
            self.map[snapped] = idx
        return idx

    def finalize(self) -> List[RGB]:
        out = list(self.colors)
        while len(out) < 256:
            out.append((8, 8, 8))
        return out


class PixelCanvas:
    """Indexed-color pixel buffer with crisp pixel-art drawing primitives."""

    def __init__(self, w: int, h: int):
        self.w = w
        self.h = h
        self.grid = [[0] * w for _ in range(h)]

    def get(self, x: int, y: int) -> int:
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.grid[y][x]
        return 0

    def px(self, x: int, y: int, c: int):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.grid[y][x] = c

    def rect(self, x: int, y: int, w: int, h: int, c: int):
        for yy in range(max(0, y), min(self.h, y + h)):
            row = self.grid[yy]
            for xx in range(max(0, x), min(self.w, x + w)):
                row[xx] = c

    def hline(self, x0: int, x1: int, y: int, c: int):
        if 0 <= y < self.h:
            a, b = (x0, x1) if x0 <= x1 else (x1, x0)
            for x in range(max(0, a), min(self.w, b + 1)):
                self.grid[y][x] = c

    def vline(self, x: int, y0: int, y1: int, c: int):
        if 0 <= x < self.w:
            a, b = (y0, y1) if y0 <= y1 else (y1, y0)
            for y in range(max(0, a), min(self.h, b + 1)):
                self.grid[y][x] = c

    def line(self, x0: int, y0: int, x1: int, y1: int, c: int):
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        x, y = x0, y0
        while True:
            self.px(x, y, c)
            if x == x1 and y == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x += sx
            if e2 <= dx:
                err += dx
                y += sy

    def ellipse(self, cx: float, cy: float, rx: float, ry: float, c: int):
        x0 = max(0, int(math.floor(cx - rx - 1)))
        x1 = min(self.w - 1, int(math.ceil(cx + rx + 1)))
        y0 = max(0, int(math.floor(cy - ry - 1)))
        y1 = min(self.h - 1, int(math.ceil(cy + ry + 1)))
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = (x - cx) / max(0.001, rx)
                dy = (y - cy) / max(0.001, ry)
                if dx * dx + dy * dy <= 1.0:
                    self.grid[y][x] = c

    def ring(self, cx: float, cy: float, r_out: float, r_in: float, c: int):
        x0 = max(0, int(math.floor(cx - r_out - 1)))
        x1 = min(self.w - 1, int(math.ceil(cx + r_out + 1)))
        y0 = max(0, int(math.floor(cy - r_out - 1)))
        y1 = min(self.h - 1, int(math.ceil(cy + r_out + 1)))
        ro2 = r_out * r_out
        ri2 = r_in * r_in
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                d2 = (x - cx) ** 2 + (y - cy) ** 2
                if ri2 <= d2 <= ro2:
                    self.grid[y][x] = c

    def blit_ascii(self, ox: int, oy: int, rows: List[str], cmap: Dict[str, int]):
        for dy, line in enumerate(rows):
            for dx, ch in enumerate(line):
                c = cmap.get(ch, 0)
                if c != 0:
                    self.px(ox + dx, oy + dy, c)

    def outline_region(self, x0: int, y0: int, w: int, h: int, outline_c: int):
        """Add a crisp 1px dark outline around non-zero pixels strictly inside [x0..x0+w, y0..y0+h]."""
        snap = [self.grid[y0 + dy][x0 : x0 + w] for dy in range(h)]
        for dy in range(h):
            for dx in range(w):
                if snap[dy][dx] == 0:
                    for nx, ny in ((dx - 1, dy), (dx + 1, dy), (dx, dy - 1), (dx, dy + 1)):
                        if 0 <= nx < w and 0 <= ny < h and snap[ny][nx] != 0:
                            self.grid[y0 + dy][x0 + dx] = outline_c
                            break


# ---------------------------------------------------------------------------
# Part 3: Sheet 2 — sprites.tim (8-bit 256-color CLUT, 256x240)
# ---------------------------------------------------------------------------

def build_sprites_palette(p: Palette256) -> Dict[str, int]:
    """Curated 256-color pixel-art ramps for characters, drinks, bubbles & VFX."""
    raw = {
        ".": None, " ": None,
        # Outlines & Whites
        "K": (16, 16, 24),
        "K2": (32, 24, 40),
        "KG": (12, 48, 28),
        "KB": (56, 32, 24),
        "KR": (88, 16, 24),
        "W": (248, 248, 248),
        "WF": (232, 240, 248),
        # Silver / Aluminum / Plastic
        "SH": (216, 224, 236),
        "S": (176, 192, 208),
        "SM": (128, 144, 168),
        "SD": (80, 96, 120),
        "SS": (48, 56, 76),
        # Skin tones (Warm Peach, Tan, Blush, Angry)
        "P0": (248, 224, 192),
        "P": (240, 192, 144),
        "P2": (216, 152, 104),
        "P3": (176, 112, 72),
        "P4": (128, 72, 44),
        "TN1": (232, 176, 128),
        "TN2": (200, 136, 88),
        "TN3": (152, 96, 56),
        "BL": (240, 128, 120),
        "AN1": (248, 136, 120),
        "AN2": (224, 72, 64),
        "AN3": (168, 40, 40),
        # Hair ramps
        "HB1": (64, 72, 104),
        "HB2": (32, 36, 56),
        "HR1": (168, 104, 56),
        "HR2": (120, 68, 32),
        "HR3": (72, 40, 20),
        "HY1": (248, 232, 112),
        "HY2": (232, 184, 48),
        "HY3": (176, 120, 24),
        "HG1": (184, 192, 208),
        "HG2": (120, 128, 144),
        # Tsingtao Emerald Greens
        "G0": (144, 248, 160),
        "L": (64, 216, 104),
        "G": (16, 160, 68),
        "G2": (12, 120, 48),
        "D": (8, 72, 32),
        # Crimson Reds
        "R0": (248, 120, 112),
        "R": (232, 40, 48),
        "R2": (184, 24, 36),
        "R3": (128, 16, 28),
        "BP": (248, 216, 212),  # pale pink tint inside angry bubbles
        # Gold / Beer Amber
        "Y0": (248, 248, 168),
        "Y": (248, 208, 56),
        "Y2": (232, 168, 32),
        "O": (208, 112, 24),
        "O2": (152, 72, 16),
        "O3": (104, 48, 16),
        # Royal Blues / Denim
        "U0": (104, 168, 248),
        "U1": (56, 112, 224),
        "U": (32, 76, 176),
        "U2": (24, 48, 120),
        # Cyans / Ice
        "C0": (200, 248, 248),
        "C": (88, 224, 248),
        "C2": (32, 176, 224),
        "C3": (24, 120, 168),
        # Neon Magenta / Orange Shirt / Cash
        "M0": (248, 144, 224),
        "M": (224, 56, 176),
        "M2": (168, 28, 128),
        "OR0": (248, 168, 72),
        "OR1": (232, 112, 32),
        "OR2": (176, 72, 20),
        "KH0": (208, 192, 152),
        "KH1": (168, 144, 104),
        "KH2": (120, 96, 64),
        "CG0": (144, 240, 152),
        "CG1": (56, 184, 88),
        "CG2": (28, 128, 56),
        # Phase 2 Clarity: Crimson-Orange Hazard, Cracked Glass, Emerald-Cyan Shield & Additive Halos
        "DR": (248, 64, 32),
        "DR2": (216, 40, 24),
        "GL0": (216, 248, 240),
        "GL1": (152, 216, 204),
        "GL2": (104, 168, 160),
        "EC0": (160, 248, 216),
        "EC": (40, 216, 168),
        "EC2": (16, 136, 128),
        "H0": (224, 240, 248),
        "H1": (152, 200, 224),
        "H2": (88, 136, 168),
        "H3": (48, 80, 104),
    }
    out: Dict[str, int] = {}
    for k, rgb in raw.items():
        out[k] = 0 if rgb is None else p.c(rgb)
    return out


def draw_mascot_28x36(cv: PixelCanvas, ox: int, oy: int, pose: str, C: Dict[str, int]):
    """Draw shaded 256-color Tsingtao Mascot Bartender inside 28x36 cell."""
    dy = 1 if pose == "idle2" else (-1 if pose == "celebrate" else 0)
    dx = -1 if pose in ("dash", "dash2") else 0

    # --- CAP (facing left) ---
    cx = ox + 7 + dx
    cy = oy + 1 + dy
    cv.rect(cx + 2, cy, 10, 5, C["G"])
    cv.hline(cx + 3, cx + 10, cy, C["L"])
    cv.hline(cx + 4, cx + 8, cy + 1, C["G0"])
    cv.vline(cx + 11, cy + 1, cy + 4, C["G2"])
    # Red & Gold Tsingtao cap crest
    cv.rect(cx + 4, cy + 1, 4, 3, C["R"])
    cv.rect(cx + 5, cy + 2, 2, 1, C["Y"])
    # Curved shaded visor facing left
    cv.hline(cx - 3, cx + 11, cy + 4, C["G2"])
    cv.hline(cx - 2, cx + 10, cy + 3, C["G"])
    cv.hline(cx - 3, cx + 1, cy + 5, C["D"])
    cv.hline(cx - 2, cx + 2, cy + 4, C["Y"])  # gold visor piping!

    # --- HEAD & FACE ---
    hx = ox + 8 + dx
    hy = oy + 6 + dy
    cv.rect(hx, hy, 11, 9, C["P"])
    cv.hline(hx + 1, hx + 8, hy, C["P2"])     # cap shadow on brow
    cv.vline(hx + 10, hy + 1, hy + 7, C["P2"])
    cv.hline(hx + 2, hx + 9, hy + 8, C["P2"]) # chin shadow
    cv.px(hx + 1, hy + 4, C["P0"])            # nose bridge highlight
    cv.px(hx + 2, hy + 5, C["P2"])            # nose tip shadow
    cv.px(hx + 7, hy + 5, C["BL"])            # healthy cheek tone
    # Hair tuft & ear on right
    cv.rect(hx + 9, hy, 3, 5, C["HB2"])
    cv.px(hx + 10, hy + 1, C["HB1"])
    cv.rect(hx + 11, hy + 3, 2, 3, C["P"])
    cv.px(hx + 11, hy + 4, C["P3"])

    # Sleek 90s Aviator Sunglasses
    cv.rect(hx - 1, hy + 2, 5, 4, C["K"])
    cv.rect(hx + 5, hy + 2, 5, 4, C["K"])
    cv.hline(hx - 1, hx + 10, hy + 2, C["K"])
    cv.rect(hx, hy + 3, 3, 2, C["HB2"])
    cv.rect(hx + 6, hy + 3, 3, 2, C["HB2"])
    gl_off = 1 if pose == "idle2" else 0
    cv.px(hx + gl_off, hy + 3, C["C"])
    cv.px(hx + 1 + gl_off, hy + 3, C["W"])
    cv.px(hx + 6 + gl_off, hy + 3, C["C"])
    cv.px(hx + 7 + gl_off, hy + 3, C["W"])

    # Expressive Mouth
    if pose == "celebrate":
        cv.rect(hx + 2, hy + 6, 5, 2, C["W"])
        cv.px(hx + 1, hy + 6, C["P3"])
    elif pose == "pour":
        cv.rect(hx + 2, hy + 6, 4, 2, C["W"])
        cv.px(hx + 3, hy + 7, C["R"])
    elif pose == "foam":
        cv.rect(hx + 2, hy + 6, 3, 2, C["R2"])
        cv.px(hx + 3, hy + 7, C["R3"])
    else:
        cv.hline(hx + 2, hx + 5, hy + 7, C["W"])
        cv.px(hx + 6, hy + 6, C["P3"])

    # --- TORSO (White shirt, Emerald Vest, Gold Piping, Red Bowtie, Belt) ---
    tx = ox + 7 + dx
    ty = oy + 15 + dy
    cv.rect(tx + 1, ty, 12, 11, C["WF"])
    cv.rect(tx + 4, ty, 6, 9, C["W"])
    cv.rect(tx, ty, 4, 11, C["G"])
    cv.rect(tx + 10, ty, 4, 11, C["G"])
    cv.vline(tx, ty + 1, ty + 10, C["G2"])
    cv.vline(tx + 13, ty + 1, ty + 10, C["G2"])
    cv.vline(tx + 1, ty + 1, ty + 8, C["L"])
    cv.vline(tx + 3, ty + 1, ty + 9, C["Y"])    # gold lapel piping
    cv.vline(tx + 10, ty + 1, ty + 9, C["Y"])
    # Bowtie
    cv.rect(tx + 5, ty + 1, 4, 2, C["R"])
    cv.px(tx + 6, ty + 1, C["R0"])
    cv.px(tx + 7, ty + 2, C["R2"])
    # Belt + Gold Buckle
    cv.rect(tx, ty + 10, 14, 2, C["K"])
    cv.rect(tx + 5, ty + 10, 4, 2, C["Y"])
    cv.px(tx + 6, ty + 10, C["Y0"])

    # --- ARMS ---
    if pose in ("idle", "idle2"):
        # Left arm poised forward, right arm holding folded white bar towel!
        cv.rect(tx - 3, ty + 2, 3, 6, C["W"])
        cv.vline(tx - 3, ty + 3, ty + 7, C["SH"])
        cv.rect(tx - 4, ty + 7, 3, 3, C["P"])
        cv.px(tx - 4, ty + 9, C["P2"])
        cv.rect(tx + 14, ty + 2, 3, 6, C["W"])
        cv.rect(tx + 14, ty + 6, 3, 5, C["WF"])  # bar towel!
        cv.hline(tx + 14, tx + 16, ty + 10, C["G"])  # green stripe on towel
        cv.rect(tx + 14, ty + 7, 2, 2, C["P"])
    elif pose == "pour":
        # Outstretched serving arm sliding drink left!
        cv.rect(tx - 6, ty + 2, 7, 3, C["W"])
        cv.hline(tx - 6, tx, ty + 4, C["SH"])
        cv.rect(tx - 8, ty + 2, 3, 3, C["P"])
        cv.px(tx - 7, ty + 1, C["C"])
        cv.px(tx - 6, ty, C["W"])
        cv.rect(tx + 13, ty + 3, 3, 6, C["W"])
        cv.rect(tx + 14, ty + 8, 2, 2, C["P"])
    elif pose == "dash":
        # Stride 1 arms + speed streaks
        cv.rect(tx - 5, ty + 1, 6, 3, C["W"])
        cv.rect(tx - 7, ty + 1, 3, 3, C["P"])
        cv.rect(tx + 13, ty + 5, 5, 3, C["W"])
        cv.rect(tx + 17, ty + 6, 2, 2, C["P"])
        cv.hline(tx + 17, tx + 20, ty - 2, C["C"])
        cv.hline(tx + 18, tx + 21, ty + 2, C["W"])
    elif pose == "dash2":
        # Stride 2 opposite arm swing!
        cv.rect(tx - 3, ty + 5, 4, 3, C["W"])
        cv.rect(tx - 4, ty + 6, 2, 2, C["P"])
        cv.rect(tx + 13, ty + 1, 5, 3, C["W"])
        cv.rect(tx + 17, ty + 1, 2, 3, C["P"])
        cv.hline(tx + 17, tx + 20, ty + 5, C["C"])
        cv.hline(tx + 18, tx + 21, ty - 1, C["W"])
    elif pose == "celebrate":
        # Both fists raised high + victory stars!
        cv.rect(tx - 3, ty - 6, 3, 8, C["W"])
        cv.rect(tx - 3, ty - 9, 3, 3, C["P"])
        cv.rect(tx + 14, ty - 6, 3, 8, C["W"])
        cv.rect(tx + 14, ty - 9, 3, 3, C["P"])
        cv.px(tx - 5, ty - 10, C["Y0"])
        cv.px(tx - 4, ty - 11, C["Y"])
        cv.px(tx + 18, ty - 10, C["Y0"])
        cv.px(tx + 17, ty - 11, C["Y"])
    elif pose == "foam":
        # Defensive arms raised
        cv.rect(tx - 3, ty, 3, 7, C["W"])
        cv.rect(tx - 4, ty - 2, 3, 3, C["P"])
        cv.rect(tx + 14, ty, 3, 7, C["W"])
        cv.rect(tx + 14, ty - 2, 3, 3, C["P"])

    # --- LEGS & SNEAKERS ---
    ly = oy + 27 + (0 if pose != "celebrate" else -1)
    if pose == "dash":
        cv.rect(tx, ly, 4, 5, C["U1"])
        cv.rect(tx + 10, ly, 4, 5, C["U"])
        cv.rect(tx - 2, ly + 5, 6, 2, C["W"])
        cv.hline(tx - 2, tx + 3, ly + 6, C["G"])
        cv.rect(tx + 11, ly + 5, 6, 2, C["W"])
        cv.hline(tx + 11, tx + 16, ly + 6, C["G"])
    elif pose == "dash2":
        cv.rect(tx + 1, ly + 1, 4, 4, C["U"])
        cv.rect(tx + 9, ly, 4, 5, C["U1"])
        cv.rect(tx + 1, ly + 5, 5, 2, C["W"])
        cv.hline(tx + 1, tx + 5, ly + 6, C["G"])
        cv.rect(tx + 9, ly + 5, 6, 2, C["W"])
        cv.hline(tx + 9, tx + 14, ly + 6, C["G"])
    else:
        cv.rect(tx + 1, ly, 5, 5, C["U1"])
        cv.vline(tx + 5, ly, ly + 4, C["U2"])
        cv.rect(tx + 8, ly, 5, 5, C["U1"])
        cv.vline(tx + 12, ly, ly + 4, C["U2"])
        cv.rect(tx, ly + 5, 6, 2, C["W"])
        cv.hline(tx + 1, tx + 5, ly + 6, C["G"])
        cv.rect(tx + 8, ly + 5, 6, 2, C["W"])
        cv.hline(tx + 9, tx + 13, ly + 6, C["G"])

    # --- FOAM GAG OVERLAY ---
    if pose == "foam":
        cv.ellipse(hx + 4, hy + 2, 6.0, 4.2, C["W"])
        cv.ellipse(hx + 7, hy + 4, 4.2, 3.2, C["WF"])
        cv.ellipse(hx + 1, hy + 5, 3.2, 2.5, C["SH"])
        cv.rect(hx, hy + 6, 2, 5, C["W"])
        cv.rect(hx + 6, hy + 6, 2, 6, C["WF"])
        cv.rect(hx + 3, hy + 8, 2, 4, C["W"])
        cv.px(hx - 2, hy + 1, C["W"])
        cv.px(hx - 3, hy + 4, C["C0"])
        cv.px(hx + 12, hy, C["W"])
        # Sunglasses peeking through the foam
        cv.hline(hx, hx + 3, hy + 3, C["K"])
        cv.hline(hx + 6, hx + 8, hy + 3, C["K"])

    cv.outline_region(ox, oy, 28, 36, C["K"])


def draw_customer_24x34(cv: PixelCanvas, ox: int, oy: int, archetype: str, frame: str, C: Dict[str, int]):
    """Draw shaded 256-color customer (24x34, facing RIGHT toward bartender)."""
    styles = {
        "local":      {"wide": 0, "shirt": C["W"],   "shirt2": C["C"],   "pants": C["KH1"], "pants2": C["KH2"], "shades": False, "skin": "peach"},
        "foodie":     {"wide": 2, "shirt": C["OR1"], "shirt2": C["OR2"], "pants": C["SS"],  "pants2": C["SD"],  "shades": False, "skin": "tan"},
        "bottle_vip": {"wide": 0, "shirt": C["U1"],  "shirt2": C["U2"],  "pants": C["U2"],  "pants2": C["HB2"], "shades": True,  "skin": "peach"},
        "bag_vip":    {"wide": 0, "shirt": C["C2"],  "shirt2": C["C3"],  "pants": C["U"],   "pants2": C["U2"],  "shades": False, "skin": "peach"},
        "rusher":     {"wide": 0, "shirt": C["M"],   "shirt2": C["M2"],  "pants": C["HB2"], "pants2": C["K"],   "shades": True,  "skin": "peach"},
    }
    st = styles[archetype]
    w_ex = st["wide"]
    dy = 1 if frame == "walk2" else (-1 if frame == "angry" else 0)
    lean = 1 if (archetype == "rusher" and frame in ("walk1", "walk2")) else 0

    # Skin palette selection
    if frame == "angry":
        sk_hi, sk_md, sk_sh = C["AN1"], C["AN2"], C["AN3"]
    elif st["skin"] == "tan":
        sk_hi, sk_md, sk_sh = C["TN1"], C["TN2"], C["TN3"]
    else:
        sk_hi, sk_md, sk_sh = C["P0"], C["P"], C["P2"]

    # --- HEAD ---
    hx = ox + 6 + lean
    hy = oy + 4 + dy
    hw = 10 + (1 if archetype == "foodie" else 0)
    cv.rect(hx, hy, hw, 9, sk_md)
    cv.vline(hx, hy + 1, hy + 7, sk_sh)
    cv.hline(hx + 2, hx + hw - 2, hy + 8, sk_sh)
    cv.px(hx + hw - 2, hy + 4, sk_hi)  # nose/cheek light
    cv.rect(hx - 1, hy + 3, 2, 3, sk_md)  # left ear
    cv.px(hx - 1, hy + 4, sk_sh)

    # Hair / Hat
    if archetype == "local":
        cv.rect(hx, hy - 1, hw, 3, C["HB2"])
        cv.hline(hx + 1, hx + hw - 2, hy - 1, C["HB1"])
        cv.px(hx, hy + 2, C["HG1"])  # friendly uncle silver temple!
        cv.hline(hx + 5, hx + hw - 1, hy + 6, C["HB2"])  # friendly mustache!
    elif archetype == "foodie":
        cv.rect(hx - 1, hy - 1, hw + 2, 3, C["HB2"])
        cv.hline(hx, hx + hw - 1, hy - 1, C["HB1"])
        cv.hline(hx - 1, hx + hw, hy + 1, C["R"])      # red BBQ headband!
        cv.px(hx - 2, hy + 1, C["R"])
        cv.px(hx - 3, hy + 2, C["R2"])                 # headband tail
    elif archetype == "bottle_vip":
        cv.rect(hx - 1, hy - 2, hw + 1, 4, C["K"])     # slicked back hair
        cv.hline(hx + 1, hx + hw - 2, hy - 1, C["HB1"])
        cv.px(hx + 2, hy, C["SH"])
    elif archetype == "bag_vip":
        cv.rect(hx - 1, hy - 2, hw + 2, 4, C["Y2"])    # Qingdao golden sun cap
        cv.hline(hx, hx + hw - 1, hy - 2, C["Y"])
        cv.hline(hx + 3, hx + hw + 2, hy + 1, C["Y"])  # visor facing right!
        cv.hline(hx + 3, hx + hw + 1, hy + 2, C["O"])
    elif archetype == "rusher":
        cv.rect(hx - 1, hy - 1, hw + 1, 3, C["HY2"])
        cv.px(hx, hy - 2, C["HY1"])
        cv.px(hx + 3, hy - 3, C["HY1"])
        cv.px(hx + 6, hy - 3, C["HY1"])
        cv.px(hx + 9, hy - 2, C["Y0"])

    # Eyes / Shades / Expression
    if st["shades"] and frame != "angry":
        cv.rect(hx + 3, hy + 3, 7, 2, C["K"])
        cv.px(hx + 4, hy + 3, C["C"])
        cv.px(hx + 8, hy + 3, C["W"])
    else:
        # Expressive eyes
        cv.px(hx + 4, hy + 4, C["W"])
        cv.px(hx + 5, hy + 4, C["K"])
        cv.px(hx + 8, hy + 4, C["W"])
        cv.px(hx + 9, hy + 4, C["K"])
        if frame == "angry":
            cv.hline(hx + 3, hx + 5, hy + 3, C["K"])
            cv.px(hx + 5, hy + 4, C["K"])  # furrowed V-brow!
            cv.hline(hx + 8, hx + 9, hy + 3, C["K"])
            cv.rect(hx + 5, hy + 6, 4, 2, C["W"])  # yelling mouth!
            cv.px(hx + 6, hy + 7, C["R3"])
            # Comic red anger pop above head!
            cv.px(hx + 1, hy - 3, C["R"])
            cv.px(hx + 2, hy - 2, C["Y"])
        elif frame == "drink":
            cv.rect(hx + 6, hy + 6, 3, 2, C["R2"])  # happy gulping mouth!
            cv.px(hx + 4, hy + 5, C["BL"])
        else:
            cv.hline(hx + 6, hx + 8, hy + 7, C["P3"])
            if archetype == "foodie":
                cv.px(hx + 3, hy + 5, C["BL"])

    # --- TORSO ---
    tx = ox + 5 - (w_ex // 2) + lean
    ty = oy + 13 + dy
    tw = 12 + w_ex
    cv.rect(tx, ty, tw, 10, st["shirt"])
    cv.vline(tx, ty, ty + 9, st["shirt2"])
    cv.hline(tx + 1, tx + tw - 2, ty + 9, st["shirt2"])

    if archetype == "local":
        cv.hline(tx + 1, tx + tw - 2, ty + 2, C["C"])
        cv.hline(tx + 1, tx + tw - 2, ty + 5, C["C"])
        cv.hline(tx + 1, tx + tw - 2, ty + 8, C["C"])
    elif archetype == "foodie":
        cv.rect(tx + 4, ty, 4, 8, C["W"])     # open shirt showing white tee
        cv.vline(tx + 3, ty, ty + 8, C["OR0"])
        cv.vline(tx + 8, ty, ty + 8, C["OR0"])
        cv.hline(tx, tx + tw - 1, ty + 9, C["K"])  # belt
        cv.rect(tx + 5, ty + 9, 3, 1, C["Y"])
    elif archetype == "bottle_vip":
        cv.rect(tx + 4, ty, 4, 5, C["W"])     # crisp white V-shirt
        cv.vline(tx + 6, ty + 1, ty + 5, C["Y"])  # gold tie!
        cv.px(tx + 2, ty + 2, C["R"])         # red pocket square
        cv.vline(tx + 1, ty + 1, ty + 8, C["U0"])
    elif archetype == "bag_vip":
        cv.hline(tx + 1, tx + tw - 2, ty + 3, C["L"])
        cv.hline(tx + 1, tx + tw - 2, ty + 4, C["W"])
        cv.vline(tx + 6, ty, ty + 9, C["U0"])
    elif archetype == "rusher":
        cv.rect(tx + 4, ty, 4, 9, C["W"])
        cv.vline(tx + 3, ty, ty + 8, C["K"])
        cv.vline(tx + 8, ty, ty + 8, C["K"])
        cv.px(tx + 1, ty + 2, C["C"])         # neon pin

    # --- ARMS ---
    if frame == "drink":
        # Raising drink up to mouth!
        cv.rect(tx + tw - 1, ty + 1, 3, 6, st["shirt"])
        drink_x = tx + tw
        drink_y = ty - 4
        if archetype == "bag_vip":
            cv.rect(drink_x, drink_y + 1, 4, 4, C["Y"])
            cv.px(drink_x + 1, drink_y, C["W"])
            cv.px(drink_x + 2, drink_y - 1, C["R"])
        elif archetype == "bottle_vip":
            cv.rect(drink_x + 1, drink_y - 1, 2, 2, C["G"])
            cv.rect(drink_x, drink_y + 1, 4, 5, C["G"])
            cv.hline(drink_x, drink_x + 3, drink_y + 3, C["R"])
        else:
            cv.rect(drink_x, drink_y + 1, 4, 5, C["G"])
            cv.hline(drink_x, drink_x + 3, drink_y, C["W"])  # foam!
            cv.hline(drink_x, drink_x + 3, drink_y + 3, C["R"])
        cv.rect(tx - 2, ty + 3, 2, 5, sk_md)
    elif frame == "angry":
        cv.rect(tx - 3, ty - 3, 3, 6, st["shirt"])
        cv.rect(tx - 3, ty - 5, 3, 2, sk_md)
        cv.rect(tx + tw, ty - 3, 3, 6, st["shirt"])
        cv.rect(tx + tw, ty - 5, 3, 2, sk_md)
    elif frame == "walk1":
        cv.rect(tx - 2, ty + 2, 2, 6, st["shirt"])
        cv.rect(tx - 2, ty + 7, 2, 2, sk_md)
        cv.rect(tx + tw, ty + 1, 3, 5, st["shirt"])
        cv.rect(tx + tw + 1, ty + 5, 2, 2, sk_md)
    else:  # walk2
        cv.rect(tx - 2, ty + 1, 2, 5, st["shirt"])
        cv.rect(tx - 2, ty + 5, 2, 2, sk_md)
        cv.rect(tx + tw, ty + 3, 2, 5, st["shirt"])
        cv.rect(tx + tw, ty + 7, 2, 2, sk_md)

    # --- LEGS & SHOES ---
    ly = oy + 23 + dy
    shoe_l = C["W"] if archetype == "rusher" else C["HR2"]
    shoe_r = C["W"] if archetype == "rusher" else C["HR1"]
    if frame == "walk1":
        cv.rect(tx, ly, 4, 6, st["pants"])
        cv.rect(tx + tw - 4, ly, 4, 6, st["pants2"])
        cv.rect(tx - 1, ly + 6, 5, 2, shoe_l)
        cv.rect(tx + tw - 3, ly + 6, 5, 2, shoe_r)
    elif frame == "walk2":
        cv.rect(tx + 2, ly, 4, 5, st["pants2"])
        cv.rect(tx + tw - 5, ly + 1, 4, 5, st["pants"])
        cv.rect(tx + 2, ly + 5, 5, 2, shoe_r)
        cv.rect(tx + tw - 4, ly + 6, 5, 2, shoe_l)
    else:
        cv.rect(tx + 1, ly, 4, 6, st["pants"])
        cv.rect(tx + tw - 5, ly, 4, 6, st["pants2"])
        shoe_c = C["W"] if archetype == "rusher" else C["K"]
        cv.rect(tx + 1, ly + 6, 5, 2, shoe_c)
        cv.rect(tx + tw - 5, ly + 6, 5, 2, shoe_c)

    cv.outline_region(ox, oy, 24, 34, C["K"])


def build_sprites_sheet() -> Tuple[List[RGB], List[List[int]]]:
    """Assemble the upgraded 256x240 8-bit 256-color sprites.tim sheet."""
    pal = Palette256()
    C = build_sprites_palette(pal)
    cv = PixelCanvas(256, 240)

    # 1. Mascot Bartender (28x36 each, v=0, stride=32)
    mascot_poses = ["idle", "pour", "dash", "dash2", "celebrate", "foam", "idle2"]
    for i, pose in enumerate(mascot_poses):
        draw_mascot_28x36(cv, i * 32, 0, pose, C)

    # 2. Bonus Round Mischievous Shaker Hand (24x24 at 224,0)
    hand_rows = [
        "......GGGGGGG...........",
        ".....GGLLLLLGG..........",
        ".....GGYYRYYGG..........",
        ".....DDDDDDDDD..........",
        "....WWWWWWWWWWW.........",
        "...WWWWWWWWWWWWW........",
        "..WWSSWWWWWWWSSWW.......",
        ".WWWWWWWWWWWWWWWWW......",
        ".WWWSWWWWWWWWWSWWW......",
        ".WWWSMWWWSMWWWSMWW......",
        ".WWWSMWWWSMWWWSMWW......",
        ".WWWSMWWWSMWWWSMWW......",
        ".WWWSMWWWSMWWWSMWW......",
        ".WWWSMWWWSMWWWSMWW......",
        "..WWSMWWWSMWWWSMW.......",
        "...SS.SSS.SSS.SS........",
    ]
    hand_map = {
        ".": 0, "G": C["G"], "L": C["L"], "D": C["D"], "Y": C["Y"], "R": C["R"],
        "W": C["W"], "S": C["SH"], "M": C["SM"],
    }
    cv.blit_ascii(224, 4, hand_rows, hand_map)
    cv.outline_region(224, 0, 24, 24, C["K"])

    # 3. 5 Customer Archetypes (24x34, stride 28x36, rows v=40,76,112,148,184)
    archetypes = ["local", "foodie", "bottle_vip", "bag_vip", "rusher"]
    frames = ["walk1", "walk2", "drink", "angry"]
    for r, arch in enumerate(archetypes):
        for f, fr in enumerate(frames):
            draw_customer_24x34(cv, f * 28, 40 + r * 36, arch, fr, C)

    # 4. Drinks / Empties / Tips (row v=40)
    # 4a. SPR_BEER_BAG (120, 40, 14, 20) — Qingdao knotted plastic beer bag + straw
    bag_rows = [
        ".........RW...",
        "........WR....",
        "...SS..RW.....",
        "..S..SWR.SS...",
        "..S..SSRWS.S..",
        "...SSWWRSSS...",
        "....SSWRSS....",
        "...SSWWWRSS...",
        "..SWWWWWWWWS..",
        ".SWWWWWWWWWWS.",
        ".SWWYYYYYYWWS.",
        ".SWYYYYYYYYWS.",
        ".SYYWYYYYYYYS.",
        ".SYYYYOYYYYYS.",
        ".SYYWYYYYOYYS.",
        ".SYYYYOYYOYYS.",
        ".SOYYYYYYOOYS.",
        "..SOOOOOOOOS..",
        "...SSSSSSSS...",
    ]
    bag_map = {
        ".": 0, "R": C["R"], "W": C["W"], "S": C["S"], "Y": C["Y"], "O": C["O"],
    }
    cv.blit_ascii(120, 40, bag_rows, bag_map)
    cv.outline_region(120, 40, 14, 20, C["K"])

    # 4b. SPR_EMPTY_CAN (140, 40, 14, 12) — Silver dented aluminum can w/ bold crimson-orange warning rim!
    empty_can_rows = [
        "...ORRRRRRO...",
        ".ORWWWWWWWWR..",
        ".RHWWHWWHWWSR.",
        "ORWSSWWWWSHSSR",
        "RRWSKSWWSSWMDR",
        "RSHSSMMMWKSMDR",
        ".RSHMSDDDDDDR.",
        ".2SDDDDDDDDD2.",
        "..2RRRRRRRR2..",
        "...22222222...",
    ]
    can_map = {
        ".": 0, "S": C["S"], "H": C["SH"], "W": C["W"], "M": C["SM"], "D": C["SD"],
        "K": C["SS"], "R": C["DR"], "2": C["DR2"], "O": C["OR0"],
    }
    cv.blit_ascii(140, 41, empty_can_rows, can_map)

    # 4c. SPR_EMPTY_BOTTLE (160, 40, 16, 12) — Bright silver/cracked-glass bottle w/ crimson-orange warning rim!
    empty_bottle_rows = [
        "..ORRRRRRRRO....",
        ".ORWWWWWWWWRO...",
        "ORWCCWSSWHCCRO..",
        "RSHCWSKWWSHCRRRO",
        "RSHCWKWGGGGWWCCR",
        "RSHGSKKGGGGSMSSR",
        "RSGMSKWMMMMMRRRO",
        ".R2MMMMMMMMR2...",
        "..2RRRRRRRR2....",
    ]
    btl_map = {
        ".": 0, "W": C["W"], "C": C["C0"], "H": C["SH"], "S": C["S"], "M": C["SM"],
        "G": C["GL1"], "K": C["SS"], "R": C["DR"], "2": C["DR2"], "O": C["OR0"],
    }
    cv.blit_ascii(160, 41, empty_bottle_rows, btl_map)

    # 4d. SPR_CASH_TIP (180, 40, 16, 12) — Gleaming GOLD Yuan coin & gold-ingot stack (ZERO green!)
    cash_rows = [
        "..000000........",
        ".0WYYYY2........",
        ".22222O.........",
        ".0000000..000...",
        ".0YYYYY3.0WYY0..",
        ".Y2222230WOYOY2.",
        ".2OOOOO30YYOYY2.",
        "........2YOOOYO.",
        ".........2YOYO..",
        "..........O2O...",
    ]
    cash_map = {
        ".": 0, "W": C["W"], "0": C["Y0"], "Y": C["Y"], "2": C["Y2"],
        "O": C["O"], "3": C["O3"],
    }
    cv.blit_ascii(180, 41, cash_rows, cash_map)
    cv.outline_region(180, 40, 16, 12, C["K"])

    # 5. VFX Sprites (16x16 at v=76)
    # 5a. SPR_FX_FOAM1 (120,76) & SPR_FX_FOAM2 (140,76) — rich multi-tone frothy bubbles!
    foam1_rows = [
        "......WW........",
        "....WWWWWW......",
        "...WW00WWWW..W..",
        "..WWSSWWWWWW....",
        ".WW0WWWWWW0WW...",
        ".WWSSWWWWSSWW...",
        ".WWWWWWWWWWWW...",
        "..WWSSWWSSWW....",
        "...WWWWWWWW..W..",
        "....WWSSWW......",
        "..W...WW...WW...",
    ]
    foam2_rows = [
        "..W......WW.....",
        ".......WWWW...W.",
        ".WW...WWSSWW....",
        "......WW0WWW....",
        "...W....WW....W.",
        "................",
        ".W...WW....WW...",
        "....WW0W..WW0W..",
        "....WSSW..WSSW..",
        ".....WW....WW...",
        "..W............W",
        ".......WW.......",
        "......WWWW......",
        ".......WW.......",
    ]
    foam_map = {".": 0, "W": C["W"], "0": C["WF"], "S": C["SH"]}
    cv.blit_ascii(120, 78, foam1_rows, foam_map)
    cv.outline_region(120, 76, 16, 16, C["C2"])
    cv.blit_ascii(140, 77, foam2_rows, foam_map)
    cv.outline_region(140, 76, 16, 16, C["C2"])

    # 5b. SPR_FX_ICE1 (160,76) & SPR_FX_ICE2 (180,76) — crisp 8-point ice crystals!
    ice1_rows = [
        ".......C........",
        ".......W........",
        "......CWC.......",
        "..C...CWC...C...",
        "...C..CWC..C....",
        "....CCWWWCC.....",
        "..CCWWWWWWWCC...",
        "CWWWWWWWWWWWWWC.",
        "..CCWWWWWWWCC...",
        "....CCWWWCC.....",
        "...C..CWC..C....",
        "..C...CWC...C...",
        "......CWC.......",
        ".......W........",
        ".......C........",
    ]
    ice2_rows = [
        "..C..........C..",
        "...C...C....C...",
        "....C..W...C....",
        ".....CCWC.C.....",
        "......CWWC......",
        "..CCWWWWWWWWCC..",
        "....CWW00WWC....",
        "..CWWWWWWWWWWC..",
        "....CWW00WWC....",
        "..CCWWWWWWWWCC..",
        "......CWWC......",
        ".....C.CWCC.....",
        "....C...W..C....",
        "...C....C...C...",
        "..C..........C..",
    ]
    ice_map = {".": 0, "W": C["W"], "0": C["C0"], "C": C["C"]}
    cv.blit_ascii(160, 76, ice1_rows, ice_map)
    cv.blit_ascii(180, 76, ice2_rows, ice_map)

    # 5c. SPR_FX_SMASH (200,76) — glass shards & golden splash!
    smash_rows = [
        "..G.......L.....",
        "...G..W..L..Y...",
        "....GLWLG..Y....",
        ".Y...WYW..G.....",
        "..Y.WWYWWG...L..",
        "...WWYYYWW..L...",
        "..L.WYYYW..G....",
        ".G...WYW..Y.....",
        "....G.W.GY......",
        "..L..G.L..Y.....",
        ".L..G...L..Y....",
        "....D....G......",
    ]
    smash_map = {".": 0, "G": C["G"], "L": C["L"], "W": C["W"], "Y": C["Y"], "D": C["D"]}
    cv.blit_ascii(200, 78, smash_rows, smash_map)

    # 6. Speech Bubbles (18x18 at v=104 and v=126)
    def draw_bubble_frame(bx: int, by: int, rim_c: int, fill_c: int, bevel_c: int, angry_rim: bool = False):
        cv.rect(bx + 2, by + 1, 14, 12, fill_c)
        cv.rect(bx + 1, by + 2, 16, 10, fill_c)
        cv.hline(bx + 2, bx + 15, by + 12, bevel_c)
        # Speech tail bottom-left
        cv.rect(bx + 3, by + 13, 3, 2, fill_c)
        cv.px(bx + 2, by + 15, fill_c)
        cv.outline_region(bx, by, 18, 18, rim_c)
        if angry_rim:
            # Bold crimson inner rim inside crisp dark outer border so angry state pops!
            cv.hline(bx + 2, bx + 15, by + 1, C["R"])
            cv.hline(bx + 2, bx + 15, by + 12, C["R"])
            cv.vline(bx + 1, by + 2, by + 11, C["R"])
            cv.vline(bx + 16, by + 2, by + 11, C["R"])

    def draw_bubble_drink_icon(bx: int, by: int, kind: str):
        # Centered cleanly inside 18x18 bubble; toggleable button badge overlays independently in C
        if kind == "can":
            cv.rect(bx + 6, by + 3, 6, 8, C["G"])
            cv.vline(bx + 7, by + 4, by + 9, C["L"])
            cv.vline(bx + 11, by + 4, by + 9, C["G2"])
            cv.hline(bx + 6, bx + 11, by + 3, C["SH"])
            cv.hline(bx + 6, bx + 11, by + 10, C["SD"])
            cv.rect(bx + 6, by + 6, 6, 2, C["R"])
            cv.px(bx + 8, by + 6, C["Y"])
            cv.px(bx + 9, by + 7, C["Y"])
        elif kind == "bottle":
            cv.rect(bx + 8, by + 2, 2, 3, C["G"])
            cv.hline(bx + 8, bx + 9, by + 2, C["R"])
            cv.rect(bx + 7, by + 5, 4, 1, C["G"])
            cv.rect(bx + 7, by + 6, 4, 5, C["G"])
            cv.vline(bx + 7, by + 5, by + 10, C["L"])
            cv.vline(bx + 10, by + 5, by + 10, C["G2"])
            cv.hline(bx + 7, bx + 10, by + 7, C["R"])
            cv.px(bx + 8, by + 8, C["Y"])
            cv.px(bx + 9, by + 8, C["R"])
        elif kind == "bag":
            cv.px(bx + 11, by + 2, C["R"])
            cv.px(bx + 10, by + 3, C["R"])
            cv.rect(bx + 7, by + 4, 5, 2, C["SH"])
            cv.hline(bx + 7, bx + 11, by + 4, C["SM"])
            cv.rect(bx + 6, by + 6, 6, 5, C["Y"])
            cv.vline(bx + 7, by + 6, by + 9, C["Y0"])
            cv.hline(bx + 6, bx + 11, by + 10, C["O"])
            cv.vline(bx + 11, by + 6, by + 10, C["O2"])

    # Normal bubbles
    for bx, kind in ((120, "can"), (140, "bottle"), (160, "bag")):
        draw_bubble_frame(bx, 104, C["K"], C["W"], C["SH"], angry_rim=False)
        draw_bubble_drink_icon(bx, 104, kind)

    # RED-rimmed Angry bubbles that STILL SHOW the wanted drink icon + crisp dark border!
    for bx, kind in ((180, "can"), (200, "bottle"), (220, "bag")):
        draw_bubble_frame(bx, 104, C["K"], C["BP"], C["R0"], angry_rim=True)
        draw_bubble_drink_icon(bx, 104, kind)

    # Happy (120, 126)
    draw_bubble_frame(120, 126, C["K"], C["W"], C["SH"])
    cv.rect(120 + 5, 126 + 4, 3, 2, C["R"])
    cv.rect(120 + 10, 126 + 4, 3, 2, C["R"])
    cv.rect(120 + 5, 126 + 5, 8, 2, C["R"])
    cv.rect(120 + 6, 126 + 7, 6, 2, C["R"])
    cv.rect(120 + 8, 126 + 9, 2, 1, C["R"])
    cv.px(120 + 6, 126 + 4, C["W"])

    # Generic Angry (140, 126)
    draw_bubble_frame(140, 126, C["K"], C["Y"], C["O"], angry_rim=True)
    cv.rect(140 + 8, 126 + 3, 2, 5, C["R"])
    cv.rect(140 + 8, 126 + 9, 2, 2, C["R"])

    # 7. Phase 2 Buff Pickups, Danger Alert & Additive Halos (u=120..240, v=148..200)
    # 7a. SPR_BUFF_FROST (120, 148, 16x16) — Radiant Ice-Cyan diamond frost crystal with crisp bright core
    frost_buff_rows = [
        ".......KK.......",
        "......KWWK......",
        ".....KW00WK.....",
        "....KW0CC2WK....",
        "...KW0CCWC22K...",
        "..KW0CCWWW222K..",
        ".KW0CCWWWWW223K.",
        "KW0CCWWWWWWW233K",
        "KW0CCWWWWWWW233K",
        ".K0CCCWWWWW223K.",
        "..K2CCCWWW223K..",
        "...K2CCWC223K...",
        "....K2CC223K....",
        ".....K2223K.....",
        "......K33K......",
        ".......KK.......",
    ]
    frost_buff_map = {
        ".": 0, "K": C["K"], "W": C["W"], "0": C["C0"],
        "C": C["C"], "2": C["C2"], "3": C["C3"],
    }
    cv.blit_ascii(120, 148, frost_buff_rows, frost_buff_map)

    # 7b. SPR_BUFF_STAR (140, 148, 16x16) — Radiant Gold-White 5-point multiplier star
    star_buff_rows = [
        ".......KK.......",
        "......KWWK......",
        ".....KW00WK.....",
        ".....K0YY0K.....",
        ".KKKKK0YY0KKKKK.",
        "KW0000YWWY0000YK",
        "K0YYYYWWWWYYYY2K",
        ".K0YYWWWWWWYY2K.",
        "..KYYWWWWWWY2K..",
        "...KYYWWWWY2K...",
        "...KYY0YY0Y2K...",
        "..KYY2KYYK22OK..",
        "..KY2K.KK.K2OK..",
        ".KY2K......K2OK.",
        ".KKK........KKK.",
        "................",
    ]
    star_buff_map = {
        ".": 0, "K": C["K"], "W": C["W"], "0": C["Y0"],
        "Y": C["Y"], "2": C["Y2"], "O": C["O"],
    }
    cv.blit_ascii(140, 148, star_buff_rows, star_buff_map)

    # 7c. SPR_BUFF_SHIELD (160, 148, 16x16) — Radiant Emerald-Cyan protective foam shield bubble
    shield_buff_rows = [
        ".....KKKKKK.....",
        "...KKE0000EKK...",
        "..KE0WWCCCCCEK..",
        ".KE0WCMMMMMCCEK.",
        ".K0CMWWWWWWWWCK.",
        "KE0CMW00CC00WMK.",
        "K0CCMW0CCCC0WM2K",
        "K0CMMW0CCEC0WMDK",
        "KCCMMWWCEECWWMDK",
        "KCCMMMWCEECWMMDK",
        ".KCMMMMWCCWMMDK.",
        ".KCCMMMMWWMMMDK.",
        ".KCCMMMMMMMMDDK.",
        "..KCCDDDDDDCDK..",
        "...KKDDDDDDKK...",
        ".....KKKKKK.....",
    ]
    shield_buff_map = {
        ".": 0, "K": C["K"], "W": C["W"], "0": C["C0"],
        "C": C["C"], "E": C["EC0"], "M": C["EC"], "D": C["EC2"], "2": C["C2"],
    }
    cv.blit_ascii(160, 148, shield_buff_rows, shield_buff_map)

    # 7d. SPR_DANGER_ALERT (180, 148, 16x16) — Crisp Crimson/Yellow '!' warning triangle badge
    danger_alert_rows = [
        ".......KK.......",
        "......K0RK......",
        ".....K0YYRK.....",
        ".....KRYYRK.....",
        "....K0YKKYRK....",
        "....KRYKKYRK....",
        "...K0YYKKYYRK...",
        "...KRYYKKYYRK...",
        "..K0YYYKKYYYRK..",
        "..KRYYYYYYYYRK..",
        ".K0YYYYKKYYYYRK.",
        ".KRYYYYKKYYYYRK.",
        "KRRYYYYYYYYYYRRK",
        "K22RRRRRRRRRR22K",
        ".KKKKKKKKKKKKKK.",
        "................",
    ]
    danger_alert_map = {
        ".": 0, "K": C["K"], "0": C["R0"], "R": C["R"], "2": C["R2"], "Y": C["Y"],
    }
    cv.blit_ascii(180, 148, danger_alert_rows, danger_alert_map)

    # 7e. SPR_BUFF_HALO (120, 172, 20x20) — Bright soft radial ring/halo for GFX_TRANS_ADD under Buffs
    for dy in range(20):
        for dx in range(20):
            dist = math.hypot(dx - 9.5, dy - 9.5)
            if 5.6 <= dist <= 6.7:
                c = C["W"]
            elif 4.9 <= dist < 5.6 or 6.7 < dist <= 7.4:
                c = C["H0"]
            elif 3.9 <= dist < 4.9 or 7.4 < dist <= 8.3:
                c = C["H1"]
            elif 2.8 <= dist < 3.9 or (8.3 < dist <= 9.2 and (dx + dy) % 2 == 0):
                c = C["H2"]
            elif 1.8 <= dist < 2.8 or (9.2 < dist <= 9.8 and (dx % 2 == 0) and (dy % 2 == 0)):
                c = C["H3"]
            else:
                c = 0
            if c != 0:
                cv.px(120 + dx, 172 + dy, c)

    # 7f. SPR_DANGER_HALO (144, 172, 20x20) — Spiky crimson-orange hazard ring for GFX_TRANS_ADD
    for dy in range(20):
        for dx in range(20):
            vx = dx - 9.5
            vy = dy - 9.5
            dist = math.hypot(vx, vy)
            ang = math.atan2(vy, vx)
            spike = (0.5 + 0.5 * math.cos(8.0 * ang)) ** 1.35
            r_max = 6.2 + 3.6 * spike
            r_mid = 5.2 + 1.5 * spike
            if dist > r_max:
                continue
            if dist > r_max - 1.3 and spike > 0.62:
                c = C["OR0"]
            elif 4.8 <= dist <= r_mid + 0.9:
                c = C["DR"]
            elif dist > r_mid:
                c = C["R"]
            elif dist >= 3.2:
                c = C["R2"]
            elif dist >= 1.8 and (dx + dy) % 2 == 0:
                c = C["R3"]
            else:
                c = 0
            if c != 0:
                cv.px(144 + dx, 172 + dy, c)

    # 8. Glossy 256-Color Button Glyph Atlas (u=120..238, v=196..239)
    def draw_glossy_cap_14x14(bx: int, by: int, rim_hi: int, body_hi: int, body_mid: int, body_lo: int, art: List[str], sym_c: int, sym_hi: int):
        cap_rows = [
            "....KKKKKK....",
            "..KKHHHHHHKK..",
            ".KHHMMMMMMBBK.",
            ".KHMMMMMMMMBK.",
            "KHMMMMMMMMMMLK",
            "KMMMMMMMMMMMLK",
            "KMMMMMMMMMMMLK",
            "KMMMMMMMMMMMLK",
            "KMMMMMMMMMMMLK",
            "KBMMMMMMMMMLLK",
            ".KBMMMMMMLLLLK",
            ".KBBMLLLLLLLK.",
            "..KKLLLLLLKK..",
            "....KKKKKK....",
        ]
        cap_map = {".": 0, "K": C["K"], "H": rim_hi, "B": body_hi, "M": body_mid, "L": body_lo}
        cv.blit_ascii(bx, by, cap_rows, cap_map)
        # Subtle specular top-left dome glint
        cv.px(bx + 4, by + 2, rim_hi)
        cv.px(bx + 5, by + 2, C["W"])
        # Drop shadow + crisp inset symbol (6x6 or 7x7 strictly inside dome)
        ox = bx + (14 - len(art[0])) // 2
        oy = by + (14 - len(art)) // 2
        for dy, row in enumerate(art):
            for dx, ch in enumerate(row):
                if ch == "#":
                    cv.px(ox + dx + 1, oy + dy + 1, C["K"])
        for dy, row in enumerate(art):
            for dx, ch in enumerate(row):
                if ch == "#":
                    cv.px(ox + dx, oy + dy, sym_hi if dy <= 1 else sym_c)

    # Row 1 (v=196): Glossy PS1 Face Buttons (14x14)
    draw_glossy_cap_14x14(120, 196, C["SH"], C["SD"], C["SS"], C["K2"], [
        "#.....#",
        ".#...#.",
        "..#.#..",
        "...#...",
        "..#.#..",
        ".#...#.",
        "#.....#",
    ], C["C"], C["C0"])  # Cross (Cyan)
    draw_glossy_cap_14x14(136, 196, C["SH"], C["SD"], C["SS"], C["K2"], [
        "..###..",
        ".#...#.",
        "#.....#",
        "#.....#",
        "#.....#",
        ".#...#.",
        "..###..",
    ], C["R"], C["R0"])  # Circle (Coral Red)
    draw_glossy_cap_14x14(152, 196, C["SH"], C["SD"], C["SS"], C["K2"], [
        "#######",
        "#.....#",
        "#.....#",
        "#.....#",
        "#.....#",
        "#.....#",
        "#######",
    ], C["M"], C["M0"])  # Square (Magenta)
    draw_glossy_cap_14x14(168, 196, C["SH"], C["SD"], C["SS"], C["K2"], [
        "...#...",
        "..#.#..",
        "..#.#..",
        ".#...#.",
        ".#...#.",
        "#.....#",
        "#######",
    ], C["L"], C["G0"])  # Triangle (Emerald Mint)

    # Metallic Pill Bumpers L1/R1 (18x12 at v=197) & LB/RB (18x12 at v=215)
    def draw_glossy_bumper_18x12(bx: int, by: int, art: List[str]):
        bumper_rows = [
            "..KKKKKKKKKKKKKK..",
            ".KHHHHHHHHHHHHHHK.",
            "KHMMMMMMMMMMMMMMMK",
            "KMMMMMMMMMMMMMMMMK",
            "KMMMMMMMMMMMMMMMMK",
            "KMMMMMMMMMMMMMMMMK",
            "KMMMMMMMMMMMMMMMMK",
            "KMMMMMMMMMMMMMMMMK",
            "KLLLLLLLLLLLLLLLLK",
            "KLLLLLLLLLLLLLLLLK",
            ".KKKKKKKKKKKKKKKK.",
            "..................",
        ]
        bmap = {".": 0, "K": C["K"], "H": C["SH"], "M": C["SD"], "L": C["SS"]}
        cv.blit_ascii(bx, by, bumper_rows, bmap)
        ox = bx + 4
        oy = by + 3
        for dy, row in enumerate(art):
            for dx, ch in enumerate(row):
                if ch == "#":
                    cv.px(ox + dx + 1, oy + dy + 1, C["K"])
        for dy, row in enumerate(art):
            for dx, ch in enumerate(row):
                if ch == "#":
                    cv.px(ox + dx, oy + dy, C["W"] if dy <= 1 else C["SH"])

    draw_glossy_bumper_18x12(184, 197, [
        "#......##.",
        "#.....#.#.",
        "#.......#.",
        "#.......#.",
        "####..###.",
    ])  # L1
    draw_glossy_bumper_18x12(204, 197, [
        "###....##.",
        "#..#..#.#.",
        "###.....#.",
        "#.#.....#.",
        "#..#..###.",
    ])  # R1

    # D-Pad Glyph (14x14 at 224,196)
    dpad_rows = [
        "....KKKKKK....",
        "....KHHHHK....",
        "....KHMMMK....",
        "....KMMMMK....",
        "KKKKKMMMMKKKKK",
        "KHMMWMMMMWMMMK",
        "KMMMWWMMWWMMMK",
        "KMMMMMMMMMMMMK",
        "KLLLLLLLLLLLLK",
        "KKKKKLLLLKKKKK",
        "....KLLLLK....",
        "....KLLLLK....",
        "....KLLLLK....",
        "....KKKKKK....",
    ]
    dpad_map = {".": 0, "K": C["K"], "H": C["SH"], "M": C["SM"], "L": C["SD"], "W": C["Y"]}
    cv.blit_ascii(224, 196, dpad_rows, dpad_map)
    cv.px(224 + 6, 196 + 2, C["Y"]); cv.px(224 + 7, 196 + 2, C["Y"])
    cv.px(224 + 6, 196 + 11, C["Y"]); cv.px(224 + 7, 196 + 11, C["Y"])

    # Row 2 (v=214): Glossy 3D Xbox Buttons (14x14) + LB/RB (18x12 at v=215)
    draw_glossy_cap_14x14(120, 214, C["G0"], C["L"], C["G"], C["G2"], [
        "..##...",
        ".#..#..",
        "#....#.",
        "######.",
        "#....#.",
        "#....#.",
    ], C["W"], C["W"])  # Xbox A (Green -> Cross/Can)
    draw_glossy_cap_14x14(136, 214, C["R0"], C["R0"], C["R"], C["R2"], [
        "#####..",
        "#....#.",
        "#####..",
        "#....#.",
        "#....#.",
        "#####..",
    ], C["W"], C["W"])  # Xbox B (Red -> Circle/Bottle)
    draw_glossy_cap_14x14(152, 214, C["C0"], C["U0"], C["U1"], C["U2"], [
        "#....#.",
        ".#..#..",
        "..##...",
        "..##...",
        ".#..#..",
        "#....#.",
    ], C["W"], C["W"])  # Xbox X (Blue -> Square/Bag)
    draw_glossy_cap_14x14(168, 214, C["Y0"], C["Y0"], C["Y"], C["O"], [
        "#....#.",
        ".#..#..",
        "..##...",
        "..##...",
        "..##...",
        "..##...",
    ], C["W"], C["W"])  # Xbox Y (Gold -> Triangle/Rush)

    draw_glossy_bumper_18x12(184, 215, [
        "#.....###.",
        "#.....#..#",
        "#.....###.",
        "#.....#..#",
        "####..###.",
    ])  # LB
    draw_glossy_bumper_18x12(204, 215, [
        "###...###.",
        "#..#..#..#",
        "###...###.",
        "#.#...#..#",
        "#..#..###.",
    ])  # RB

    # Row 3 (v=230): Crisp 10x10 Mini Drink Badges (native 1:1 on drinks & speech bubbles)
    def draw_mini_badge_10x10(bx: int, by: int, rim_hi: int, body: int, body_sh: int, art: List[str], sym_c: int):
        rows10 = [
            "..KKKKKK..",
            ".KHHHHHHK.",
            "KHBBBBBBBK",
            "KBBBBBBBBS",
            "KBBBBBBBBS",
            "KBBBBBBBBS",
            "KBBBBBBBBS",
            "KBBSSSSSSK",
            ".KSSSSSSK.",
            "..KKKKKK..",
        ]
        mmap = {".": 0, "K": C["K"], "H": rim_hi, "B": body, "S": body_sh}
        cv.blit_ascii(bx, by, rows10, mmap)
        ox = bx + 2
        oy = by + 2
        for dy, row in enumerate(art):
            for dx, ch in enumerate(row):
                if ch == "#":
                    cv.px(ox + dx, oy + dy, sym_c)

    # PS1 Mini Badges (v=230)
    draw_mini_badge_10x10(120, 230, C["SH"], C["SS"], C["K2"], [
        "#....#", ".#..#.", "..##..", "..##..", ".#..#.", "#....#",
    ], C["C0"])
    draw_mini_badge_10x10(132, 230, C["SH"], C["SS"], C["K2"], [
        ".####.", "#....#", "#....#", "#....#", "#....#", ".####.",
    ], C["R0"])
    draw_mini_badge_10x10(144, 230, C["SH"], C["SS"], C["K2"], [
        "######", "#....#", "#....#", "#....#", "#....#", "######",
    ], C["M0"])
    # Xbox Mini Badges (v=230)
    draw_mini_badge_10x10(160, 230, C["G0"], C["G"], C["G2"], [
        "..##..", ".#..#.", "#....#", "######", "#....#", "#....#",
    ], C["W"])
    draw_mini_badge_10x10(172, 230, C["R0"], C["R"], C["R2"], [
        "#####.", "#....#", "#####.", "#....#", "#....#", "#####.",
    ], C["W"])
    draw_mini_badge_10x10(184, 230, C["C0"], C["U1"], C["U2"], [
        "#....#", ".#..#.", "..##..", "..##..", ".#..#.", "#....#",
    ], C["W"])

    return pal.finalize(), cv.grid


# ---------------------------------------------------------------------------
# Part 4: Sheet 3 — env_ui.tim (4-bit CLUT, 256x240)
# ---------------------------------------------------------------------------

ENV_UI_PALETTE: List[RGB] = [
    (0, 0, 0),       # 0  . Transparent
    (16, 16, 28),    # 1  K Deep Outline / Night Silhouette / Mortar
    (248, 248, 248), # 2  W Pure White (Bitmap Font — RGB tintable!)
    (136, 72, 48),   # 3  H Rich Mahogany Highlight (soft satin sheen)
    (112, 56, 40),   # 4  M Rich Mahogany Mid (warm calm base so sprites & glows pop!)
    (80, 40, 28),    # 5  D Rich Mahogany Deep Grain / Seam
    (248, 208, 56),  # 6  Y Brass / Gold Bright
    (192, 128, 24),  # 7  O Brass / Gold Dark
    (232, 40, 48),   # 8  R Lantern Red / Circle Button / Curtain Red
    (248, 120, 32),  # 9  F Lantern Glow Orange
    (24, 168, 72),   # 10 G Tsingtao Green / Triangle Button / Awning
    (64, 224, 248),  # 11 C Neon Cyan / Cross Button / Frost Ice
    (32, 80, 168),   # 12 U Deep Blue / Frost Bar / Harbor
    (232, 72, 176),  # 13 P Neon Pink / Square Button
    (176, 192, 208), # 14 S Steel / Silver / Stone Highlight
    (64, 72, 104),   # 15 N Slate Stone / Dark Steel / Skyline Mid
]

ENV_CMAP = {
    ".": 0, " ": 0,
    "K": 1, "W": 2, "H": 3, "M": 4, "D": 5, "Y": 6, "O": 7,
    "R": 8, "F": 9, "G": 10, "C": 11, "U": 12, "P": 13, "S": 14, "N": 15,
}

# Crisp 8x8 Arcade Bitmap Font (ASCII 32..95)
# Note: '$' (36) = crisp Yuan '¥' glyph; '*' (42) = crisp multiply '×' glyph!
FONT_8X8: Dict[str, List[str]] = {
    " ": ["........", "........", "........", "........", "........", "........", "........", "........"],
    "!": ["...##...", "...##...", "...##...", "...##...", "...##...", "........", "...##...", "........"],
    '"': [".##..##.", ".##..##.", ".##..##.", "........", "........", "........", "........", "........"],
    "#": [".##..##.", "########", ".##..##.", ".##..##.", "########", ".##..##.", "........", "........"],
    "$": ["##....##", ".##..##.", "..####..", ".######.", "...##...", ".######.", "...##...", "........"],  # Yuan ¥
    "%": [".##...##", ".##..##.", "....##..", "...##...", "..##....", ".##..##.", "##...##.", "........"],
    "&": ["..####..", ".##..##.", "..####..", ".##.##..", "##...##.", "##..###.", ".###..##", "........"],
    "'": ["...##...", "...##...", "..##....", "........", "........", "........", "........", "........"],
    "(": ["....##..", "...##...", "..##....", "..##....", "..##....", "...##...", "....##..", "........"],
    ")": ["..##....", "...##...", "....##..", "....##..", "....##..", "...##...", "..##....", "........"],
    "*": ["........", ".##..##.", "..####..", "...##...", "..####..", ".##..##.", "........", "........"],  # Multiply ×
    "+": ["........", "...##...", "...##...", ".######.", "...##...", "...##...", "........", "........"],
    ",": ["........", "........", "........", "........", "........", "...##...", "...##...", "..##...."],
    "-": ["........", "........", "........", ".######.", "........", "........", "........", "........"],
    ".": ["........", "........", "........", "........", "........", "..##....", "..##....", "........"],
    "/": [".....##.", "....##..", "...##...", "..##....", ".##.....", "##......", "........", "........"],
    "0": [".######.", "##...###", "##..####", "##.##.##", "####..##", "###...##", ".######.", "........"],
    "1": ["...##...", "..###...", ".####...", "...##...", "...##...", "...##...", ".######.", "........"],
    "2": [".######.", "##....##", ".....##.", "..####..", ".##.....", "##......", "########", "........"],
    "3": [".######.", "##....##", ".....##.", "..#####.", ".....##.", "##....##", ".######.", "........"],
    "4": ["....###.", "...####.", "..##.##.", ".##..##.", "########", ".....##.", ".....##.", "........"],
    "5": ["########", "##......", "#######.", "......##", "......##", "##....##", ".######.", "........"],
    "6": ["..#####.", ".##.....", "##......", "#######.", "##....##", "##....##", ".######.", "........"],
    "7": ["########", "##....##", ".....##.", "....##..", "...##...", "...##...", "...##...", "........"],
    "8": [".######.", "##....##", "##....##", ".######.", "##....##", "##....##", ".######.", "........"],
    "9": [".######.", "##....##", "##....##", ".#######", "......##", ".....##.", ".#####..", "........"],
    ":": ["........", "...##...", "...##...", "........", "...##...", "...##...", "........", "........"],
    ";": ["........", "...##...", "...##...", "........", "...##...", "...##...", "..##....", "........"],
    "<": ["....##..", "...##...", "..##....", ".##.....", "..##....", "...##...", "....##..", "........"],
    "=": ["........", "........", ".######.", "........", ".######.", "........", "........", "........"],
    ">": ["..##....", "...##...", "....##..", ".....##.", "....##..", "...##...", "..##....", "........"],
    "?": [".######.", "##....##", ".....##.", "...###..", "...##...", "........", "...##...", "........"],
    "@": [".######.", "##....##", "##.#####", "##.##.##", "##.####.", "##......", ".######.", "........"],
    "A": ["..####..", ".##..##.", "##....##", "########", "##....##", "##....##", "##....##", "........"],
    "B": ["#######.", "##....##", "##....##", "#######.", "##....##", "##....##", "#######.", "........"],
    "C": [".######.", "##....##", "##......", "##......", "##......", "##....##", ".######.", "........"],
    "D": ["######..", "##...##.", "##....##", "##....##", "##....##", "##...##.", "######..", "........"],
    "E": ["########", "##......", "##......", "######..", "##......", "##......", "########", "........"],
    "F": ["########", "##......", "##......", "######..", "##......", "##......", "##......", "........"],
    "G": [".######.", "##....##", "##......", "##..####", "##....##", "##....##", ".######.", "........"],
    "H": ["##....##", "##....##", "##....##", "########", "##....##", "##....##", "##....##", "........"],
    "I": [".######.", "...##...", "...##...", "...##...", "...##...", "...##...", ".######.", "........"],
    "J": ["..######", ".....##.", ".....##.", ".....##.", "##...##.", "##...##.", ".#####..", "........"],
    "K": ["##...##.", "##..##..", "##.##...", "####....", "##.##...", "##..##..", "##...##.", "........"],
    "L": ["##......", "##......", "##......", "##......", "##......", "##......", "########", "........"],
    "M": ["##....##", "###..###", "########", "##.##.##", "##....##", "##....##", "##....##", "........"],
    "N": ["##....##", "###...##", "####..##", "##.##.##", "##..####", "##...###", "##....##", "........"],
    "O": [".######.", "##....##", "##....##", "##....##", "##....##", "##....##", ".######.", "........"],
    "P": ["#######.", "##....##", "##....##", "#######.", "##......", "##......", "##......", "........"],
    "Q": [".######.", "##....##", "##....##", "##....##", "##.##.##", "##..###.", ".#####.#", "........"],
    "R": ["#######.", "##....##", "##....##", "#######.", "##.##...", "##..##..", "##...##.", "........"],
    "S": [".######.", "##....##", "##......", ".######.", "......##", "##....##", ".######.", "........"],
    "T": ["########", "...##...", "...##...", "...##...", "...##...", "...##...", "...##...", "........"],
    "U": ["##....##", "##....##", "##....##", "##....##", "##....##", "##....##", ".######.", "........"],
    "V": ["##....##", "##....##", "##....##", "##....##", ".##..##.", "..####..", "...##...", "........"],
    "W": ["##....##", "##....##", "##....##", "##.##.##", "########", "###..###", "##....##", "........"],
    "X": ["##....##", ".##..##.", "..####..", "...##...", "..####..", ".##..##.", "##....##", "........"],
    "Y": ["##....##", "##....##", ".##..##.", "..####..", "...##...", "...##...", "...##...", "........"],
    "Z": ["########", ".....##.", "....##..", "...##...", "..##....", ".##.....", "########", "........"],
    "[": ["..#####.", "..##....", "..##....", "..##....", "..##....", "..##....", "..#####.", "........"],
    "\\": [".##.....", "..##....", "...##...", "....##..", ".....##.", "......##", "........", "........"],
    "]": [".#####..", "....##..", "....##..", "....##..", "....##..", "....##..", ".#####..", "........"],
    "^": ["...##...", "..####..", ".##..##.", "##....##", "........", "........", "........", "........"],
    "_": ["........", "........", "........", "........", "........", "........", "########", "........"],
}


def draw_font_sheet(cv: PixelCanvas):
    for code in range(32, 96):
        ch = chr(code)
        idx = code - 32
        gx = (idx & 15) * 8
        gy = (idx >> 4) * 8
        for y, row in enumerate(FONT_8X8[ch]):
            for x, bit in enumerate(row):
                if bit == "#":
                    cv.px(gx + x, gy + y, 2)


def draw_small_text(cv: PixelCanvas, ox: int, oy: int, text: str, color: int, shadow: int = 0):
    for i, ch in enumerate(text.upper()):
        glyph = FONT_8X8.get(ch, FONT_8X8[" "])
        gx = ox + i * 8
        for y, row in enumerate(glyph):
            for x, bit in enumerate(row):
                if bit == "#":
                    if shadow:
                        cv.px(gx + x + 1, oy + y + 1, shadow)
                    cv.px(gx + x, oy + y, color)


def build_env_ui_sheet() -> Tuple[List[RGB], List[List[int]]]:
    """Assemble the 256x240 4-bit Bar Environment, 2.5D Textures, Skyline, Font & UI sheet."""
    cv = PixelCanvas(256, 240)

    # 1. 8x8 ASCII Font (0,0 .. 127,31)
    draw_font_sheet(cv)

    # 2. PS1 Controller Face Buttons (12x12 at v=0) — cleanly inset inside rim
    def draw_ps1_btn(bx: int, by: int, art: List[str], sym_c: int):
        cv.ellipse(bx + 5.5, by + 5.5, 5.5, 5.5, 1)
        cv.ellipse(bx + 5.5, by + 5.5, 4.5, 4.5, 14)
        cv.ellipse(bx + 5.5, by + 5.5, 3.8, 3.8, 15)
        for dy, row in enumerate(art):
            for dx, ch in enumerate(row):
                if ch == "#":
                    cv.px(bx + 3 + dx, by + 3 + dy, sym_c)

    draw_ps1_btn(128, 0, [
        "#....#", ".#..#.", "..##..", "..##..", ".#..#.", "#....#",
    ], 11)
    draw_ps1_btn(144, 0, [
        ".####.", "#....#", "#....#", "#....#", "#....#", ".####.",
    ], 8)
    draw_ps1_btn(160, 0, [
        "######", "#....#", "#....#", "#....#", "#....#", "######",
    ], 13)
    draw_ps1_btn(176, 0, [
        "..##..", "..##..", ".#..#.", ".#..#.", "#....#", "######",
    ], 10)

    # PS1 Shoulder Buttons L1 (128,28,12,12) & R1 (144,28,12,12)
    def draw_shoulder_btn(bx: int, by: int, glyph: List[str]):
        cv.rect(bx + 1, by + 1, 10, 10, 1)
        cv.rect(bx, by + 2, 12, 8, 1)
        cv.rect(bx + 1, by + 2, 10, 8, 15)
        cv.hline(bx + 2, bx + 9, by + 2, 14)
        cv.hline(bx + 2, bx + 9, by + 9, 1)
        for dy, row in enumerate(glyph):
            for dx, ch in enumerate(row):
                if ch == "#":
                    cv.px(bx + 2 + dx, by + 4 + dy, 2)

    draw_shoulder_btn(128, 28, [
        "#....#..",
        "#...##..",
        "#....#..",
        "#....#..",
        "###.###.",
    ])
    draw_shoulder_btn(144, 28, [
        "###...#.",
        "#..#.##.",
        "###...#.",
        "#.#...#.",
        "#..#.###",
    ])

    # 3. HUD Icons (v=0) & Frost Bar (v=16)
    life_can = [
        "..SSSSSS..", ".SWWWWWWS.", ".GGGGGGGG.", ".GWWGGGGG.",
        ".GRRRRRRG.", ".GRWYYWRG.", ".GRRRRRRG.", ".GGGGGGGG.",
        ".GGYGGYGG.", ".GGGGGGGG.", "..SSSSSS..",
    ]
    cv.blit_ascii(192, 1, life_can, ENV_CMAP)
    cv.outline_region(192, 0, 10, 14, 1)

    frost_icon = [
        ".....CC.....", "..C..WW..C..", "...C.WW.C...", ".C..CWWC..C.",
        "..CCWWWWCC..", "CWWWWWWWWWWC", "CWWWWWWWWWWC", "..CCWWWWCC..",
        ".C..CWWC..C.", "...C.WW.C...", "..C..WW..C..", ".....CC.....",
    ]
    cv.blit_ascii(208, 0, frost_icon, ENV_CMAP)

    star_art = [
        "...Y....", "...W....", "..YWY...", "YWWWWWY.", "..YWY...", "...W....", "...Y....",
    ]
    cv.blit_ascii(224, 0, star_art, ENV_CMAP)

    # Frost frame L(128,16), M(136,16), R(144,16), Fill(156,16)
    cv.rect(129, 16, 7, 12, 14)
    cv.rect(128, 17, 8, 10, 14)
    cv.rect(130, 17, 6, 10, 15)
    cv.rect(131, 18, 5, 8, 1)
    cv.hline(129, 135, 16, 2)
    cv.rect(136, 16, 8, 12, 14)
    cv.rect(136, 17, 8, 10, 15)
    cv.rect(136, 18, 8, 8, 1)
    cv.hline(136, 143, 16, 2)
    cv.rect(144, 16, 7, 12, 14)
    cv.rect(144, 17, 8, 10, 14)
    cv.rect(144, 17, 6, 10, 15)
    cv.rect(144, 18, 5, 8, 1)
    cv.hline(144, 150, 16, 2)

    cv.rect(156, 16, 8, 8, 11)
    cv.hline(156, 163, 16, 2)
    cv.hline(156, 163, 17, 2)
    cv.hline(156, 163, 22, 12)
    cv.hline(156, 163, 23, 12)
    cv.px(158, 19, 2)
    cv.px(161, 18, 2)

    # Lantern Glow Halo (24x24 at 168,16)
    for dy in range(24):
        for dx in range(24):
            dist = math.hypot(dx - 11.5, dy - 11.5)
            if dist < 4.5:
                cv.px(168 + dx, 16 + dy, 6)
            elif dist < 8.5 and (dx + dy) % 2 == 0:
                cv.px(168 + dx, 16 + dy, 9)
            elif dist < 11.2 and (dx % 2 == 0) and (dy % 2 == 0):
                cv.px(168 + dx, 16 + dy, 8)

    # 4. 2.5D Low-Poly Textures (v=40)
    # 4a. SPR_TEX_WOOD (0,40,32,32) — seamless rich warm mahogany wood w/ subtle gentle grain
    for y in range(32):
        for x in range(32):
            plank = y // 16
            local_y = y & 15
            # Seamless wraparound gentle horizontal mahogany grain (no noisy blobs or stripes!)
            wave = math.sin(2.0 * math.pi * (x + plank * 13) / 32.0) * 0.85
            grain = math.sin(2.0 * math.pi * (y + wave) / 8.0)
            if local_y == 0:
                c = 5  # subtle continuous plank seam
            elif local_y == 1:
                c = 4  # buffer band so highlight never sits directly on seam
            elif local_y in (4, 11) and grain > 0.55:
                c = 3  # soft elongated satin wood sheen
            elif local_y in (7, 13) and grain < -0.58:
                c = 5  # gentle elongated mahogany grain wisp
            else:
                c = 4
            cv.px(0 + x, 40 + y, c)

    # 4b. SPR_TEX_WOOD_FRONT (32,40,32,32) — calm vertical mahogany wainscoting paneling
    for y in range(32):
        for x in range(32):
            col = x % 16
            if y < 2:
                c = 5  # soft shadow beneath the 3D bevelled brass counter lip
            elif y in (2, 29):
                c = 4
            elif y in (30, 31):
                c = 5
            elif y in (3, 28):
                c = 3 if col not in (0, 15) else 4  # subtle molded mahogany trim
            elif col in (0, 15):
                c = 5  # recessed panel seam
            elif col in (1, 14):
                c = 4  # gentle bevel transition
            elif col == 2 and (y % 2 == 0):
                c = 3  # subtle satin panel catchlight
            else:
                c = 4
            cv.px(32 + x, 40 + y, c)

    # 4c. SPR_TEX_FLOOR (64,40,32,32) — night-market stone floor tile (matte seamless 16x16 pavers!)
    for y in range(32):
        for x in range(32):
            row_tile = y // 16
            sx = (x + (8 if row_tile == 1 else 0)) % 16
            sy = y % 16
            if sx == 0 or sy == 0:
                c = 1  # dark stone mortar joint
            elif (sx == 1 or sy == 1) and ((x + y) % 3 == 0):
                c = 12  # soft muted bevel without high-contrast dots
            elif ((x * 5 + y * 9) % 19) == 0:
                c = 12  # subtle matte slate mineral grain
            else:
                c = 15
            cv.px(64 + x, 40 + y, c)

    # 4d. SPR_TEX_CURTAIN (96,40,16,24) — red Qingdao doorway noren curtain
    cv.rect(96, 40, 16, 24, 8)
    # Top bamboo/wood rod
    cv.rect(96, 40, 16, 2, 5)
    cv.hline(96, 111, 40, 3)
    # Cloth vertical fold shading on 2 flaps (x=0..6 and x=9..15)
    for y in range(42, 64):
        cv.px(96, y, 5)
        cv.px(111, y, 5)
        cv.px(99, y, 9)
        cv.px(107, y, 9)
        # Center seam (transparent slit at bottom 5px)
        if y >= 59:
            cv.px(103, y, 0)
            cv.px(104, y, 0)
        else:
            cv.px(103, y, 1)
            cv.px(104, y, 1)
    # Gold Tsingtao trim along bottom & mini crest circles on each flap
    cv.hline(96, 102, 62, 6)
    cv.hline(105, 111, 62, 6)
    cv.rect(98, 48, 3, 3, 6)
    cv.px(99, 49, 2)
    cv.rect(106, 48, 3, 3, 6)
    cv.px(107, 49, 2)

    # 5. Bar Props (v=40)
    # 5a. SPR_BEER_TAP (116,40,16,28)
    tap_art = [
        "......GGGG......", ".....GWWWWG.....", ".....GRRRRG.....", "......GGGG......",
        "......YYYY......", ".......YY.......", "......YYYY......", ".....YYYYOO.....",
        "....YYWWWWOO....", "..SSYYYYYYOO....", ".SSWYYYYYYOO....", ".SS.SYYYYYOO....",
        ".S...YYYYYOO....", ".....YYYYYOO....", ".....YYWYYOO....", ".....YYYYYOO....",
        ".....YYYYYOO....", ".....YYWYYOO....", ".....YYYYYOO....", "....YYYYYYYOO...",
        "...YYWWWWYYYOO..", "..YYYYYYYYYYYOO.", "..OOOOOOOOOOOOO.", ".SSSSSSSSSSSSSS.",
        ".NNNNNNNNNNNNNN.",
    ]
    cv.blit_ascii(116, 42, tap_art, ENV_CMAP)
    cv.outline_region(116, 40, 16, 28, 1)

    # 5b. SPR_COUNTER_LEFT (136,40), MID (152,40), RIGHT (168,40) (16x24 each)
    def draw_counter_tile(cx: int, cy: int, kind: str):
        x0 = 2 if kind == "left" else 0
        x1 = 13 if kind == "right" else 15
        cv.rect(cx + x0, cy + 1, x1 - x0 + 1, 8, 3)
        cv.hline(cx + x0, cx + x1, cy + 1, 6)
        cv.hline(cx + x0, cx + x1, cy + 4, 4)
        cv.hline(cx + x0, cx + x1, cy + 7, 4)
        cv.hline(cx + x0, cx + x1, cy + 8, 6)
        cv.hline(cx + x0, cx + x1, cy + 9, 7)
        cv.rect(cx + x0, cy + 10, x1 - x0 + 1, 13, 4)
        cv.hline(cx + x0, cx + x1, cy + 10, 5)
        cv.hline(cx + x0, cx + x1, cy + 11, 5)
        for px_off in (3, 7, 11):
            if x0 + 1 <= px_off <= x1 - 1:
                cv.vline(cx + px_off, cy + 12, cy + 21, 5)
                cv.vline(cx + px_off + 1, cy + 12, cy + 21, 3)
        cv.hline(cx + x0, cx + x1, cy + 22, 5)
        if kind == "left":
            cv.vline(cx + 1, cy + 2, cy + 8, 4)
            cv.vline(cx + 1, cy + 9, cy + 21, 5)
            cv.vline(cx, cy + 2, cy + 22, 1)
        elif kind == "right":
            cv.vline(cx + 14, cy + 2, cy + 8, 4)
            cv.vline(cx + 14, cy + 9, cy + 21, 5)
            cv.vline(cx + 15, cy + 2, cy + 22, 1)
        cv.hline(cx + x0, cx + x1, cy, 1)
        cv.hline(cx + x0, cx + x1, cy + 23, 1)

    draw_counter_tile(136, 40, "left")
    draw_counter_tile(152, 40, "mid")
    draw_counter_tile(168, 40, "right")

    # 5c. SPR_WOOD_POST (188,40,12,32)
    cv.rect(189, 40, 10, 32, 4)
    cv.vline(190, 40, 71, 3)
    cv.vline(191, 40, 71, 3)
    cv.vline(197, 40, 71, 5)
    cv.vline(198, 40, 71, 5)
    cv.hline(189, 198, 44, 6)
    cv.hline(189, 198, 66, 6)
    cv.vline(188, 40, 71, 1)
    cv.vline(199, 40, 71, 1)

    # 5d. SPR_LANTERN_OFF (204,40,16,24) & SPR_LANTERN_ON (224,40,16,24)
    def draw_lantern(lx: int, ly: int, lit: bool):
        cv.vline(lx + 7, ly, ly + 2, 1)
        cv.vline(lx + 8, ly, ly + 2, 1)
        cv.rect(lx + 5, ly + 3, 6, 2, 6 if lit else 7)
        cv.ellipse(lx + 7.5, ly + 11.5, 6.2, 6.2, 8)
        if lit:
            cv.ellipse(lx + 7.5, ly + 11.5, 4.5, 5.0, 9)
            cv.ellipse(lx + 7.5, ly + 11.0, 2.5, 3.2, 6)
            cv.px(lx + 7, ly + 10, 2)
            cv.px(lx + 8, ly + 10, 2)
        else:
            cv.ellipse(lx + 6.5, ly + 10.5, 3.5, 4.0, 9)
        for ry in (ly + 8, ly + 11, ly + 14):
            cv.hline(lx + 2, lx + 13, ry, 5 if not lit else 8)
        cv.rect(lx + 5, ly + 18, 6, 2, 6 if lit else 7)
        cv.rect(lx + 7, ly + 20, 2, 3, 6)
        cv.px(lx + 6, ly + 22, 8)
        cv.px(lx + 9, ly + 22, 8)
        cv.outline_region(lx, ly, 16, 24, 1)

    draw_lantern(204, 40, lit=False)
    draw_lantern(224, 40, lit=True)

    # 6. Neon "TSINGTAO '96" Bar Sign (128x28 at 0,80)
    cv.rect(2, 82, 124, 24, 1)
    cv.rect(3, 83, 122, 22, 15)
    cv.rect(4, 84, 120, 20, 1)
    cv.hline(2, 125, 81, 10)
    cv.hline(2, 125, 106, 10)
    cv.vline(1, 82, 105, 10)
    cv.vline(126, 82, 105, 10)
    for cx, cy in ((2, 82), (125, 82), (2, 105), (125, 105)):
        cv.px(cx, cy, 6)
    cv.hline(5, 122, 84, 8)
    cv.hline(5, 122, 103, 8)
    draw_small_text(cv, 16, 87, "TSINGTAO", color=2, shadow=8)
    draw_small_text(cv, 84, 87, "'96", color=6, shadow=9)
    cv.hline(16, 108, 96, 11)
    cv.hline(18, 106, 97, 12)
    cv.rect(6, 89, 5, 7, 6)
    cv.hline(6, 10, 88, 2)
    cv.px(11, 91, 6)
    cv.px(11, 93, 6)
    cv.rect(116, 89, 5, 7, 10)
    cv.hline(116, 120, 88, 14)
    cv.hline(116, 120, 92, 8)

    # 7. Qingdao Zhanqiao Pier Skyline (160x48 at 0,120)
    base_y = 164
    for x in range(160):
        hill_h = int(8 + 6 * math.sin(x * 0.045) + 3 * math.cos(x * 0.11))
        cv.vline(x, base_y - hill_h, base_y - 1, 15)
    px_c = 80
    cv.vline(px_c, 122, 126, 6)
    cv.px(px_c, 122, 2)
    for row_i, half_w in enumerate([3, 6, 10, 14, 18, 16]):
        cv.hline(px_c - half_w, px_c + half_w, 127 + row_i, 1)
        if row_i == 4:
            cv.px(px_c - half_w - 1, 127 + row_i - 1, 1)
            cv.px(px_c + half_w + 1, 127 + row_i - 1, 1)
            cv.hline(px_c - half_w + 1, px_c + half_w - 1, 127 + row_i, 6)
    cv.rect(px_c - 12, 133, 25, 7, 1)
    for wx in (-8, -3, 2, 7):
        cv.rect(px_c + wx, 135, 3, 4, 6)
    for row_i, half_w in enumerate([15, 19, 24, 28, 25]):
        cv.hline(px_c - half_w, px_c + half_w, 140 + row_i, 1)
        if row_i == 3:
            cv.px(px_c - half_w - 1, 140 + row_i - 1, 1)
            cv.px(px_c + half_w + 1, 140 + row_i - 1, 1)
            cv.hline(px_c - half_w + 2, px_c + half_w - 2, 140 + row_i, 9)
    cv.rect(px_c - 20, 145, 41, 11, 1)
    for wx in (-15, -9, -3, 3, 9, 14):
        cv.rect(px_c + wx, 148, 3, 6, 9)
    cv.rect(0, 156, 160, 4, 1)
    cv.hline(0, 159, 156, 15)
    for lamp_x in (14, 34, 52, 108, 126, 146):
        cv.vline(lamp_x, 149, 155, 1)
        cv.px(lamp_x, 148, 6)
        cv.px(lamp_x, 147, 2)
    for x in range(0, 160, 16):
        cv.rect(x, 160, 5, 8, 1)
        cv.hline(x + 5, x + 15, 160, 1)
        cv.px(x + 5, 161, 1)
        cv.px(x + 15, 161, 1)
    for x in range(6, 154, 10):
        cv.hline(x, x + 4, 165, 12)
        if abs(x - px_c) < 35:
            cv.hline(x + 1, x + 3, 166, 6)

    # 8. Harbor City Skyline (96x48 at 160,120)
    buildings = [
        (0, 22, 14, True), (14, 16, 16, False), (30, 28, 12, True),
        (42, 12, 18, False), (60, 24, 14, True), (74, 18, 22, False),
    ]
    for bx, top_y, bw, pitched in buildings:
        cv.rect(160 + bx, 120 + top_y, bw, 48 - top_y, 1)
        if pitched:
            for i in range(bw // 2):
                cv.hline(160 + bx + i, 160 + bx + bw - 1 - i, 120 + top_y - 1 - i // 2, 8)
        else:
            cv.hline(160 + bx, 160 + bx + bw - 1, 120 + top_y, 15)
        for wy in range(top_y + 4, 42, 5):
            for wx in range(bx + 2, bx + bw - 2, 4):
                if (wx + wy) % 3 != 0:
                    cv.rect(160 + wx, 120 + wy, 2, 2, 6 if (wx % 2 == 0) else 11)
    cv.rect(160, 164, 96, 4, 12)

    # 9. Striped Awning Tile (32x16 at 0,176)
    for x in range(32):
        stripe_c = 10 if ((x // 8) % 2 == 0) else 8
        hi_c = 2 if (x % 8 == 1) else stripe_c
        local_x = x % 8
        bottom_y = 15 if 2 <= local_x <= 5 else (14 if local_x in (1, 6) else 12)
        for y in range(0, bottom_y):
            cv.px(x, 176 + y, hi_c if y < 2 else stripe_c)
        cv.px(x, 176 + bottom_y, 6)
        if bottom_y + 1 < 16:
            cv.px(x, 176 + bottom_y + 1, 1)

    return ENV_UI_PALETTE, cv.grid


# ---------------------------------------------------------------------------
# Part 5: Sheet 4 — cards.tim (8-bit 256-color CLUT, 256x240)
# ---------------------------------------------------------------------------

def build_cards_palette(p: Palette256) -> Dict[str, int]:
    raw = {
        ".": None,
        "K": (16, 16, 28),
        "W": (248, 248, 248),
        "IV": (240, 236, 224),   # Ivory card stock
        "IV2": (216, 208, 192),
        "IV3": (176, 168, 152),
        "S0": (224, 232, 240),
        "S": (176, 192, 208),
        "S2": (120, 136, 160),
        "S3": (72, 84, 108),
        "G0": (128, 248, 152),
        "G1": (48, 208, 96),
        "G": (24, 152, 68),
        "G2": (16, 104, 48),
        "G3": (12, 64, 32),
        "Y0": (248, 240, 144),
        "Y": (248, 200, 48),
        "Y2": (216, 144, 24),
        "Y3": (152, 88, 16),
        "R0": (248, 112, 112),
        "R": (224, 40, 48),
        "R2": (160, 24, 36),
        "R3": (104, 16, 28),
        "R4": (64, 12, 24),
        "C0": (200, 248, 248),
        "C": (72, 224, 248),
        "C2": (32, 160, 224),
        "C3": (24, 96, 160),
        "U1": (64, 120, 232),
        "U2": (32, 64, 160),
        "U3": (20, 32, 88),
        "V0": (200, 120, 248),
        "V1": (152, 64, 224),
        "V2": (96, 32, 160),
        "V3": (48, 20, 88),
        "O0": (248, 160, 64),
        "O1": (224, 104, 24),
        "O2": (160, 64, 20),
        "PK0": (248, 184, 200),
        "PK1": (232, 128, 152),
        "PK2": (184, 80, 112),
        "BR1": (184, 120, 64),
        "BR2": (128, 72, 32),
        "BR3": (72, 40, 20),
    }
    return {k: (0 if rgb is None else p.c(rgb)) for k, rgb in raw.items()}


def draw_card_frame_48x64(cv: PixelCanvas, ox: int, oy: int, kind: str, C: Dict[str, int]):
    """Draw a 48x64 Balatro-style PS1 card frame (rounded corners, beveled border, emblem recess)."""
    themes = {
        "common": {
            "rim": C["S2"], "rim_hi": C["W"], "face": C["IV"], "face2": C["IV2"],
            "trim": C["G"], "win_bg": C["IV2"], "win_rim": C["G2"], "pip": C["G"],
        },
        "uncommon": {
            "rim": C["G1"], "rim_hi": C["G0"], "face": C["G2"], "face2": C["G3"],
            "trim": C["S0"], "win_bg": C["G3"], "win_rim": C["G0"], "pip": C["S0"],
        },
        "rare": {
            "rim": C["Y"], "rim_hi": C["Y0"], "face": C["R2"], "face2": C["R3"],
            "trim": C["Y"], "win_bg": C["R4"], "win_rim": C["Y0"], "pip": C["Y0"],
        },
        "trick": {
            "rim": C["C"], "rim_hi": C["C0"], "face": C["V2"], "face2": C["V3"],
            "trim": C["C"], "win_bg": C["U3"], "win_rim": C["C0"], "pip": C["C0"],
        },
        "back": {
            "rim": C["Y2"], "rim_hi": C["Y0"], "face": C["G2"], "face2": C["G3"],
            "trim": C["Y"], "win_bg": C["R2"], "win_rim": C["Y0"], "pip": C["Y"],
        },
    }
    th = themes[kind]

    # Rounded card silhouette (48x64 with 2px clipped corners)
    cv.rect(ox + 2, oy, 44, 64, C["K"])
    cv.rect(ox, oy + 2, 48, 60, C["K"])
    cv.rect(ox + 1, oy + 1, 46, 62, C["K"])

    # Outer beveled border
    cv.rect(ox + 2, oy + 1, 44, 62, th["rim"])
    cv.rect(ox + 1, oy + 2, 46, 60, th["rim"])
    cv.hline(ox + 3, ox + 44, oy + 1, th["rim_hi"])
    cv.vline(ox + 1, oy + 3, oy + 59, th["rim_hi"])

    # Inner card face
    cv.rect(ox + 3, oy + 3, 42, 58, th["face"])
    # Subtle pattern texture on face
    for y in range(4, 60):
        for x in range(4, 44):
            if kind == "back":
                if ((x + y) % 6 == 0) or ((x - y) % 6 == 0):
                    cv.px(ox + x, oy + y, th["face2"])
            elif kind in ("uncommon", "rare", "trick"):
                if (x + y) % 4 == 0:
                    cv.px(ox + x, oy + y, th["face2"])
            elif y >= 55 or x >= 41:
                cv.px(ox + x, oy + y, th["face2"])

    # Inner hairline trim
    cv.hline(ox + 5, ox + 42, oy + 5, th["trim"])
    cv.hline(ox + 5, ox + 42, oy + 58, th["trim"])
    cv.vline(ox + 5, oy + 5, oy + 58, th["trim"])
    cv.vline(ox + 42, oy + 5, oy + 58, th["trim"])

    # Corner pips / rivets
    for px_x, px_y in ((7, 7), (39, 7), (7, 55), (39, 55)):
        cv.rect(ox + px_x, oy + px_y, 2, 2, th["pip"])

    if kind == "back":
        # Iconic Tsingtao Pagoda Crest medallion in center of card back!
        cv.ellipse(ox + 23.5, oy + 31.5, 13.0, 13.0, C["K"])
        cv.ellipse(ox + 23.5, oy + 31.5, 11.8, 11.8, C["Y"])
        cv.ellipse(ox + 23.5, oy + 31.5, 9.8, 9.8, C["R"])
        cv.ellipse(ox + 23.5, oy + 31.5, 7.5, 7.5, C["R2"])
        # Golden pagoda silhouette inside crest
        cv.hline(ox + 21, ox + 26, oy + 26, C["Y0"])
        cv.hline(ox + 19, ox + 28, oy + 29, C["Y0"])
        cv.hline(ox + 18, ox + 29, oy + 32, C["Y"])
        cv.rect(ox + 21, oy + 33, 6, 4, C["W"])
        cv.hline(ox + 18, ox + 29, oy + 37, C["Y"])
    else:
        # Central framed emblem medallion recess (around center 24, 31)
        cv.rect(ox + 8, oy + 14, 32, 34, th["win_rim"])
        cv.rect(ox + 9, oy + 15, 30, 32, th["win_bg"])
        # Inner corner studs on emblem frame
        for sx, sy in ((9, 15), (38, 15), (9, 46), (38, 46)):
            cv.px(ox + sx, oy + sy, th["rim_hi"])


def draw_coaster_base(cv: PixelCanvas, ox: int, oy: int, bg_c: int, rim_c: int, rim2_c: int, dark_c: int):
    """Draw a 24x24 scalloped/circular beer coaster base medallion."""
    cv.ellipse(ox + 11.5, oy + 11.5, 10.8, 10.8, rim2_c)
    cv.ellipse(ox + 11.5, oy + 11.5, 9.6, 9.6, rim_c)
    cv.ellipse(ox + 11.5, oy + 11.5, 8.2, 8.2, bg_c)
    # Subtle inner stitch dots around coaster perimeter
    for a in range(0, 360, 45):
        rad = math.radians(a)
        cv.px(int(round(ox + 11.5 + 8.8 * math.cos(rad))), int(round(oy + 11.5 + 8.8 * math.sin(rad))), rim2_c)
    cv.outline_region(ox, oy, 24, 24, dark_c)


def draw_20_coasters(cv: PixelCanvas, C: Dict[str, int]):
    """Hand-craft all 20 unique 24x24 Coaster Emblems at v=68 (0..9) and v=92 (10..19)."""
    def cell(cid: int) -> Tuple[int, int]:
        return ((cid % 10) * 24, 68 + (cid // 10) * 24)

    # 0: Cold Foam — overflowing frothy mug
    x, y = cell(0)
    draw_coaster_base(cv, x, y, C["C3"], C["C"], C["W"], C["K"])
    cv.rect(x + 7, y + 10, 9, 8, C["Y"])
    cv.rect(x + 16, y + 12, 2, 4, C["S0"])
    cv.ellipse(x + 11.5, y + 8.5, 5.5, 3.0, C["W"])
    cv.rect(x + 7, y + 10, 2, 4, C["W"])
    cv.px(x + 13, y + 10, C["W"])

    # 1: Green Glass — emerald Tsingtao bottle
    x, y = cell(1)
    draw_coaster_base(cv, x, y, C["G3"], C["G1"], C["Y"], C["K"])
    cv.rect(x + 10, y + 4, 4, 4, C["G"])
    cv.hline(x + 10, x + 13, y + 4, C["R"])
    cv.rect(x + 8, y + 8, 8, 10, C["G"])
    cv.vline(x + 9, y + 9, y + 17, C["G0"])
    cv.rect(x + 8, y + 11, 8, 3, C["R"])
    cv.px(x + 11, y + 12, C["Y0"])

    # 2: Plastic Hustle — knotted golden beer bag + straw
    x, y = cell(2)
    draw_coaster_base(cv, x, y, C["U3"], C["Y"], C["O1"], C["K"])
    cv.line(x + 12, y + 10, x + 16, y + 4, C["R"])
    cv.rect(x + 8, y + 7, 8, 3, C["S0"])
    cv.rect(x + 7, y + 10, 10, 7, C["Y"])
    cv.hline(x + 8, x + 15, y + 15, C["O1"])
    cv.hline(x + 8, x + 15, y + 10, C["W"])

    # 3: Tap Rhythm — brass tap + music note
    x, y = cell(3)
    draw_coaster_base(cv, x, y, C["R4"], C["Y"], C["Y2"], C["K"])
    cv.rect(x + 7, y + 6, 3, 10, C["Y"])
    cv.hline(x + 6, x + 10, y + 16, C["S"])
    cv.vline(x + 15, y + 7, y + 14, C["C0"])
    cv.hline(x + 15, x + 17, y + 7, C["C"])
    cv.rect(x + 13, y + 13, 3, 3, C["C0"])

    # 4: Mixtape Pour — 90s cassette tape
    x, y = cell(4)
    draw_coaster_base(cv, x, y, C["V3"], C["V0"], C["C"], C["K"])
    cv.rect(x + 5, y + 8, 14, 9, C["K"])
    cv.rect(x + 6, y + 9, 12, 5, C["G1"])
    cv.rect(x + 8, y + 10, 2, 2, C["W"])
    cv.rect(x + 14, y + 10, 2, 2, C["W"])
    cv.hline(x + 7, x + 16, y + 15, C["S"])

    # 5: Glass Juggler — 3 orbiting cans/bottles
    x, y = cell(5)
    draw_coaster_base(cv, x, y, C["U2"], C["C"], C["W"], C["K"])
    cv.rect(x + 6, y + 10, 4, 6, C["G1"])
    cv.hline(x + 6, x + 9, y + 12, C["R"])
    cv.rect(x + 10, y + 5, 4, 6, C["G0"])
    cv.hline(x + 10, x + 13, y + 7, C["R"])
    cv.rect(x + 14, y + 11, 4, 6, C["Y"])
    cv.hline(x + 14, x + 17, y + 13, C["R"])

    # 6: Tip Jar — clear glass jar with green Yuan bill & overflowing gold coins
    x, y = cell(6)
    draw_coaster_base(cv, x, y, C["G3"], C["Y"], C["G1"], C["K"])
    cv.rect(x + 7, y + 9, 10, 9, C["C0"])
    cv.rect(x + 8, y + 10, 8, 7, C["G1"])
    cv.rect(x + 8, y + 6, 8, 2, C["S2"])
    cv.hline(x + 8, x + 15, y + 6, C["W"])
    cv.rect(x + 9, y + 11, 6, 5, C["G0"])
    cv.ellipse(x + 11.5, y + 13.5, 2.5, 2.5, C["Y"])
    cv.px(x + 11, y + 13, C["Y0"])
    cv.px(x + 10, y + 8, C["Y0"])
    cv.px(x + 13, y + 8, C["Y"])

    # 7: Top Shelf — gold crown on crimson velvet
    x, y = cell(7)
    draw_coaster_base(cv, x, y, C["R3"], C["Y0"], C["Y2"], C["K"])
    cv.rect(x + 6, y + 12, 12, 4, C["Y"])
    cv.vline(x + 6, y + 8, y + 12, C["Y0"])
    cv.vline(x + 11, y + 7, y + 12, C["Y0"])
    cv.vline(x + 12, y + 7, y + 12, C["Y0"])
    cv.vline(x + 17, y + 8, y + 12, C["Y0"])
    cv.px(x + 9, y + 13, C["R"])
    cv.px(x + 14, y + 13, C["C"])

    # 8: Street Side — green/red awning + glowing lantern
    x, y = cell(8)
    draw_coaster_base(cv, x, y, C["U3"], C["R"], C["G1"], C["K"])
    for sx in range(6, 18, 3):
        cv.rect(x + sx, y + 6, 2, 3, C["R"] if ((sx // 3) % 2 == 0) else C["G1"])
    cv.ellipse(x + 11.5, y + 13.5, 3.5, 3.5, C["R"])
    cv.ellipse(x + 11.5, y + 13.5, 1.8, 2.0, C["Y0"])

    # 9: Big Appetite — BBQ skewer + hearty beer mug
    x, y = cell(9)
    draw_coaster_base(cv, x, y, C["O2"], C["O0"], C["Y"], C["K"])
    cv.line(x + 6, y + 17, x + 17, y + 6, C["S0"])
    cv.rect(x + 8, y + 12, 3, 3, C["R"])
    cv.rect(x + 11, y + 9, 3, 3, C["BR1"])
    cv.rect(x + 14, y + 6, 3, 3, C["Y"])

    # 10: Qingdao Trio — Can + Bottle + Bag mini trio crest!
    x, y = cell(10)
    draw_coaster_base(cv, x, y, C["U3"], C["Y"], C["G1"], C["K"])
    # Mini Can (top-left)
    cv.rect(x + 6, y + 7, 4, 6, C["G1"])
    cv.hline(x + 6, x + 9, y + 9, C["R"])
    # Mini Bottle (top-right)
    cv.rect(x + 14, y + 6, 2, 2, C["G0"])
    cv.rect(x + 13, y + 8, 4, 5, C["G"])
    cv.hline(x + 13, x + 16, y + 10, C["R"])
    # Mini Golden Bag (bottom-center)
    cv.rect(x + 9, y + 13, 6, 5, C["Y"])
    cv.hline(x + 9, x + 14, y + 13, C["W"])
    cv.px(x + 13, y + 12, C["R"])

    # 11: VIP Velvet — purple velvet badge & crisp gold VIP lettering/star
    x, y = cell(11)
    draw_coaster_base(cv, x, y, C["V3"], C["Y"], C["V0"], C["K"])
    cv.rect(x + 5, y + 8, 14, 8, C["R2"])
    cv.hline(x + 5, x + 18, y + 8, C["Y0"])
    cv.hline(x + 5, x + 18, y + 15, C["Y"])
    # Gold 'V I P' 3px micro-glyphs
    cv.px(x + 7, y + 10, C["Y0"]); cv.px(x + 9, y + 10, C["Y0"]); cv.px(x + 8, y + 12, C["Y0"])
    cv.px(x + 7, y + 11, C["Y"]);  cv.px(x + 9, y + 11, C["Y"])
    cv.vline(x + 11, y + 10, y + 12, C["Y0"])
    cv.vline(x + 13, y + 10, y + 12, C["Y0"]); cv.px(x + 14, y + 10, C["Y0"]); cv.px(x + 14, y + 11, C["Y"])

    # 12: Frost Bite — sharp ice fangs & snowflake
    x, y = cell(12)
    draw_coaster_base(cv, x, y, C["U3"], C["C0"], C["C2"], C["K"])
    cv.vline(x + 11, y + 6, y + 17, C["W"])
    cv.hline(x + 6, x + 17, y + 11, C["W"])
    cv.line(x + 8, y + 8, x + 15, y + 15, C["C"])
    cv.line(x + 15, y + 8, x + 8, y + 15, C["C"])
    cv.px(x + 9, y + 16, C["C0"])
    cv.px(x + 14, y + 16, C["C0"])

    # 13: Clean Apron — crisp white apron + sparkle
    x, y = cell(13)
    draw_coaster_base(cv, x, y, C["G2"], C["W"], C["G0"], C["K"])
    cv.rect(x + 8, y + 7, 8, 4, C["W"])
    cv.rect(x + 6, y + 11, 12, 6, C["W"])
    cv.hline(x + 6, x + 17, y + 11, C["G"])
    cv.px(x + 15, y + 7, C["Y0"])
    cv.px(x + 7, y + 7, C["Y0"])

    # 14: Piggy Bank — pink piggy + dropping gold coin
    x, y = cell(14)
    draw_coaster_base(cv, x, y, C["V3"], C["PK0"], C["Y"], C["K"])
    cv.ellipse(x + 11.5, y + 13.0, 5.5, 4.2, C["PK1"])
    cv.ellipse(x + 10.5, y + 11.8, 3.0, 2.0, C["PK0"])
    cv.rect(x + 16, y + 12, 2, 2, C["PK2"])  # snout
    cv.rect(x + 8, y + 16, 2, 2, C["PK2"])
    cv.rect(x + 13, y + 16, 2, 2, C["PK2"])
    cv.ellipse(x + 11.5, y + 6.5, 2.0, 2.0, C["Y0"])  # gold coin!

    # 15: Speed Sneakers — winged green arcade sneaker
    x, y = cell(15)
    draw_coaster_base(cv, x, y, C["U2"], C["G0"], C["W"], C["K"])
    cv.rect(x + 7, y + 9, 5, 6, C["G1"])
    cv.rect(x + 7, y + 13, 10, 3, C["G"])
    cv.hline(x + 7, x + 16, y + 16, C["W"])
    cv.line(x + 6, y + 9, x + 10, y + 6, C["Y0"])  # golden wing!

    # 16: Golden Tap — solid gold dragon draft tap
    x, y = cell(16)
    draw_coaster_base(cv, x, y, C["R4"], C["Y0"], C["Y2"], C["K"])
    cv.rect(x + 10, y + 6, 4, 11, C["Y"])
    cv.vline(x + 11, y + 7, y + 16, C["Y0"])
    cv.hline(x + 7, x + 13, y + 9, C["Y0"])
    cv.hline(x + 8, x + 15, y + 17, C["Y2"])

    # 17: Ice King — crystalline ice crown with royal sapphire gem
    x, y = cell(17)
    draw_coaster_base(cv, x, y, C["U3"], C["C0"], C["U1"], C["K"])
    cv.rect(x + 6, y + 12, 12, 5, C["C"])
    cv.hline(x + 6, x + 17, y + 16, C["C2"])
    cv.vline(x + 6, y + 7, y + 12, C["W"])
    cv.vline(x + 7, y + 9, y + 12, C["C0"])
    cv.vline(x + 11, y + 5, y + 12, C["W"])
    cv.vline(x + 12, y + 5, y + 12, C["C0"])
    cv.vline(x + 16, y + 9, y + 12, C["C0"])
    cv.vline(x + 17, y + 7, y + 12, C["W"])
    cv.rect(x + 11, y + 13, 2, 2, C["U1"])

    # 18: Bag Splash — bursting golden wave from beer bag
    x, y = cell(18)
    draw_coaster_base(cv, x, y, C["U2"], C["Y0"], C["C"], C["K"])
    cv.rect(x + 9, y + 8, 6, 8, C["Y"])
    cv.hline(x + 9, x + 14, y + 8, C["W"])
    cv.line(x + 5, y + 12, x + 8, y + 10, C["C0"])
    cv.line(x + 15, y + 10, x + 18, y + 12, C["C0"])
    cv.px(x + 5, y + 14, C["W"])
    cv.px(x + 18, y + 14, C["W"])

    # 19: Glass Cannon — heavy cannon + fiery golden blast
    x, y = cell(19)
    draw_coaster_base(cv, x, y, C["R4"], C["R0"], C["Y"], C["K"])
    cv.rect(x + 6, y + 10, 9, 5, C["G"])
    cv.hline(x + 6, x + 14, y + 10, C["G0"])
    cv.ellipse(x + 9.0, y + 15.5, 2.2, 2.2, C["Y2"])
    cv.rect(x + 15, y + 10, 3, 4, C["Y0"])
    cv.px(x + 18, y + 9, C["R"])
    cv.px(x + 18, y + 14, C["R"])


def draw_drink_cards_and_tricks(cv: PixelCanvas, C: Dict[str, int]):
    """Draw 3 Drink Recipe Emblems (24x24 at v=120), 6 Pocket Trick Icons (16x16 at v=148), and Boss Seal (120,148)."""
    # --- 3 Drink Recipe Emblems (24x24 at v=120) ---
    # 0: Can (0, 120)
    cv.ellipse(11.5, 120 + 11.5, 10.0, 10.0, C["G3"])
    cv.rect(7, 125, 10, 14, C["G"])
    cv.hline(7, 16, 125, C["S0"])
    cv.hline(7, 16, 138, C["S2"])
    cv.vline(8, 126, 137, C["G0"])
    cv.rect(7, 130, 10, 4, C["R"])
    cv.px(11, 129, C["Y0"])
    cv.px(12, 129, C["Y0"])
    cv.outline_region(0, 120, 24, 24, C["K"])

    # 1: Bottle (24, 120)
    cv.ellipse(24 + 11.5, 120 + 11.5, 10.0, 10.0, C["U3"])
    cv.rect(24 + 10, 123, 4, 5, C["G"])
    cv.hline(24 + 10, 24 + 13, 123, C["R"])
    cv.rect(24 + 8, 128, 8, 12, C["G"])
    cv.vline(24 + 9, 129, 139, C["G0"])
    cv.rect(24 + 8, 132, 8, 4, C["R"])
    cv.px(24 + 11, 133, C["Y0"])
    cv.outline_region(24, 120, 24, 24, C["K"])

    # 2: Beer Bag (48, 120)
    cv.ellipse(48 + 11.5, 120 + 11.5, 10.0, 10.0, C["R4"])
    cv.line(48 + 12, 128, 48 + 16, 122, C["R"])
    cv.rect(48 + 8, 125, 8, 3, C["S0"])
    cv.rect(48 + 6, 128, 12, 11, C["Y"])
    cv.hline(48 + 6, 48 + 17, 128, C["W"])
    cv.hline(48 + 7, 48 + 16, 138, C["O1"])
    cv.outline_region(48, 120, 24, 24, C["K"])

    # --- 6 Pocket Trick Icons (16x16 at v=148, stride=16) ---
    # 0: Cold Mug (+1 Life)
    cv.rect(3, 148 + 5, 8, 9, C["Y"])
    cv.rect(11, 148 + 7, 2, 4, C["S0"])
    cv.hline(3, 10, 148 + 4, C["W"])
    cv.rect(5, 148 + 8, 4, 4, C["R"])
    cv.px(6, 148 + 7, C["R"])
    cv.px(8, 148 + 7, C["R"])
    cv.outline_region(0, 148, 16, 16, C["K"])

    # 1: Happy Bell (2x Mult)
    cv.rect(16 + 6, 148 + 3, 4, 2, C["Y0"])
    cv.rect(16 + 4, 148 + 5, 8, 6, C["Y"])
    cv.rect(16 + 3, 148 + 10, 10, 2, C["Y2"])
    cv.rect(16 + 7, 148 + 12, 2, 2, C["W"])
    cv.px(16 + 2, 148 + 6, C["C"])
    cv.px(16 + 13, 148 + 6, C["C"])
    cv.outline_region(16, 148, 16, 16, C["K"])

    # 2: Bouncer (Gloved Fist)
    cv.rect(32 + 4, 148 + 4, 8, 6, C["O0"])
    cv.rect(32 + 5, 148 + 10, 6, 3, C["G1"])
    cv.hline(32 + 4, 32 + 11, 148 + 6, C["O2"])
    cv.px(32 + 13, 148 + 6, C["C0"])
    cv.px(32 + 14, 148 + 8, C["C"])
    cv.outline_region(32, 148, 16, 16, C["K"])

    # 3: Ice Bucket (100% Frost)
    cv.rect(48 + 4, 148 + 3, 4, 4, C["C0"])
    cv.rect(48 + 8, 148 + 4, 4, 4, C["C"])
    cv.rect(48 + 3, 148 + 6, 10, 7, C["S"])
    cv.hline(48 + 3, 48 + 12, 148 + 7, C["S0"])
    cv.hline(48 + 4, 48 + 11, 148 + 12, C["S2"])
    cv.outline_region(48, 148, 16, 16, C["K"])

    # 4: Foam Catch (Magnetic foam)
    cv.rect(64 + 4, 148 + 4, 2, 7, C["R"])
    cv.rect(64 + 10, 148 + 4, 2, 7, C["R"])
    cv.hline(64 + 5, 64 + 10, 148 + 11, C["R"])
    cv.hline(64 + 4, 64 + 5, 148 + 4, C["S0"])
    cv.hline(64 + 10, 64 + 11, 148 + 4, C["S0"])
    cv.px(64 + 7, 148 + 3, C["W"])
    cv.px(64 + 8, 148 + 5, C["C0"])
    cv.outline_region(64, 148, 16, 16, C["K"])

    # 5: VIP Coupon
    cv.rect(80 + 2, 148 + 4, 12, 8, C["Y"])
    cv.rect(80 + 3, 148 + 5, 10, 6, C["R2"])
    cv.px(80 + 7, 148 + 7, C["Y0"])
    cv.px(80 + 8, 148 + 7, C["Y0"])
    cv.hline(80 + 6, 80 + 9, 148 + 8, C["Y"])
    cv.outline_region(80, 148, 16, 16, C["K"])

    # --- Boss Seal (24x24 at 120, 148) ---
    bx, by = 120, 148
    cv.ellipse(bx + 11.5, by + 11.5, 10.5, 10.5, C["Y2"])
    cv.ellipse(bx + 11.5, by + 11.5, 9.2, 9.2, C["Y0"])
    cv.ellipse(bx + 11.5, by + 11.5, 7.5, 7.5, C["R"])
    # Crown/Star inside Boss Seal
    cv.rect(bx + 7, by + 12, 10, 3, C["Y"])
    cv.vline(bx + 7, by + 8, by + 12, C["Y0"])
    cv.vline(bx + 11, by + 7, by + 12, C["Y0"])
    cv.vline(bx + 12, by + 7, by + 12, C["Y0"])
    cv.vline(bx + 16, by + 8, by + 12, C["Y0"])
    cv.outline_region(bx, by, 24, 24, C["K"])


def build_cards_sheet() -> Tuple[List[RGB], List[List[int]]]:
    """Assemble the NEW 256x240 8-bit cards.tim sheet."""
    pal = Palette256()
    C = build_cards_palette(pal)
    cv = PixelCanvas(256, 240)

    for i, kind in enumerate(["common", "uncommon", "rare", "back", "trick"]):
        draw_card_frame_48x64(cv, i * 48, 0, kind, C)

    draw_20_coasters(cv, C)
    draw_drink_cards_and_tricks(cv, C)

    return pal.finalize(), cv.grid


# ---------------------------------------------------------------------------
# Part 6: 16x16 4-bit PS1 Memory Card Icon (Tsingtao Green Can)
# ---------------------------------------------------------------------------

MC_ICON_PALETTE: List[RGB] = [
    (16, 24, 40), (16, 16, 24), (248, 248, 248), (192, 208, 224),
    (16, 144, 64), (64, 216, 104), (8, 88, 40), (224, 32, 40),
    (248, 200, 40), (32, 104, 216), (128, 144, 168), (40, 64, 112),
    (248, 128, 40), (64, 224, 248), (160, 24, 32), (248, 232, 128),
]

MC_ICON_ART = [
    "....KSSSSSSK....", "...KSSWWWWSSK...", "...KGGGGGGGGK...", "..KGGLLGGGGGGK..",
    "..KGGYYYYYYGGK..", "..KGYGGBBGGYGK..", "..KGRRBBBBBRRK..", "..KGRRWWWWWRRK..",
    "..KGRRRRRRRRRK..", "..KGRRYYYYRRRK..", "..KGGLLGGGGGGK..", "..KGGLLGYYGGDK..",
    "..KGGGGGGGGDDK..", "...KGGDDDDDDK...", "...KAAASSSAAK...", "....KKKKKKKK....",
]

MC_CMAP = {".": 0, "K": 1, "W": 2, "S": 3, "G": 4, "L": 5, "D": 6, "R": 7, "Y": 8, "B": 9, "A": 10}


def build_mc_icon() -> Tuple[List[int], List[int], Image.Image]:
    clut_555 = [rgb_to_ps1_555(r, g, b, transparent=False, stp=False) for (r, g, b) in MC_ICON_PALETTE]
    bitmap: List[int] = []
    preview = Image.new("RGBA", (16, 16))
    ppx = preview.load()
    for y, row in enumerate(MC_ICON_ART):
        assert len(row) == 16
        for x in range(0, 16, 2):
            lo, hi = MC_CMAP[row[x]], MC_CMAP[row[x + 1]]
            bitmap.append((hi << 4) | lo)
            ppx[x, y] = ps1_555_to_rgb(clut_555[lo])
            ppx[x + 1, y] = ps1_555_to_rgb(clut_555[hi])
    return clut_555, bitmap, preview


# ---------------------------------------------------------------------------
# Part 7: C Header & Blob Generators + Layout Verification
# ---------------------------------------------------------------------------

def verify_uv_layout() -> None:
    """Assert zero internal UV overlaps and U/V <= 255 across all 4 sheets."""
    sheets: Dict[str, List[Tuple[str, int, int, int, int]]] = {
        "real_tsingtao": [
            ("SPR_REAL_LOGO", 0, 0, 128, 64),
            ("SPR_REAL_HERO_CAN", 128, 0, 32, 48),
            ("SPR_REAL_HERO_BOTTLE", 164, 0, 24, 60),
            ("SPR_REAL_CAN", 192, 0, 12, 18),
            ("SPR_REAL_BOTTLE", 208, 0, 12, 22),
            ("SPR_REAL_MINI_CAN", 224, 0, 10, 14),
            ("SPR_REAL_SHOWCASE_CAN", 128, 64, 40, 60),
        ],
        "sprites": [
            *[(f"SPR_MASCOT_{i}", i * 32, 0, 28, 36) for i in range(7)],
            ("SPR_BONUS_HAND", 224, 0, 24, 24),
            *[(f"SPR_CUST_a{a}_f{f}", f * 28, 40 + a * 36, 24, 34) for a in range(5) for f in range(4)],
            ("SPR_BEER_BAG", 120, 40, 14, 20),
            ("SPR_EMPTY_CAN", 140, 40, 14, 12),
            ("SPR_EMPTY_BOTTLE", 160, 40, 16, 12),
            ("SPR_CASH_TIP", 180, 40, 16, 12),
            ("SPR_FX_FOAM1", 120, 76, 16, 16),
            ("SPR_FX_FOAM2", 140, 76, 16, 16),
            ("SPR_FX_ICE1", 160, 76, 16, 16),
            ("SPR_FX_ICE2", 180, 76, 16, 16),
            ("SPR_FX_SMASH", 200, 76, 16, 16),
            ("SPR_BUBBLE_CAN", 120, 104, 18, 18),
            ("SPR_BUBBLE_BOTTLE", 140, 104, 18, 18),
            ("SPR_BUBBLE_BAG", 160, 104, 18, 18),
            ("SPR_BUBBLE_ANGRY_CAN", 180, 104, 18, 18),
            ("SPR_BUBBLE_ANGRY_BOTTLE", 200, 104, 18, 18),
            ("SPR_BUBBLE_ANGRY_BAG", 220, 104, 18, 18),
            ("SPR_BUBBLE_HAPPY", 120, 126, 18, 18),
            ("SPR_BUBBLE_ANGRY", 140, 126, 18, 18),
            ("SPR_BUFF_FROST", 120, 148, 16, 16),
            ("SPR_BUFF_STAR", 140, 148, 16, 16),
            ("SPR_BUFF_SHIELD", 160, 148, 16, 16),
            ("SPR_DANGER_ALERT", 180, 148, 16, 16),
            ("SPR_BUFF_HALO", 120, 172, 20, 20),
            ("SPR_DANGER_HALO", 144, 172, 20, 20),
            ("SPR_GLYPH_PS_CROSS", 120, 196, 14, 14),
            ("SPR_GLYPH_PS_CIRCLE", 136, 196, 14, 14),
            ("SPR_GLYPH_PS_SQUARE", 152, 196, 14, 14),
            ("SPR_GLYPH_PS_TRIANGLE", 168, 196, 14, 14),
            ("SPR_GLYPH_PS_L1", 184, 197, 18, 12),
            ("SPR_GLYPH_PS_R1", 204, 197, 18, 12),
            ("SPR_GLYPH_DPAD", 224, 196, 14, 14),
            ("SPR_GLYPH_XB_A", 120, 214, 14, 14),
            ("SPR_GLYPH_XB_B", 136, 214, 14, 14),
            ("SPR_GLYPH_XB_X", 152, 214, 14, 14),
            ("SPR_GLYPH_XB_Y", 168, 214, 14, 14),
            ("SPR_GLYPH_XB_LB", 184, 215, 18, 12),
            ("SPR_GLYPH_XB_RB", 204, 215, 18, 12),
            ("SPR_MINI_PS_CROSS", 120, 230, 10, 10),
            ("SPR_MINI_PS_CIRCLE", 132, 230, 10, 10),
            ("SPR_MINI_PS_SQUARE", 144, 230, 10, 10),
            ("SPR_MINI_XB_A", 160, 230, 10, 10),
            ("SPR_MINI_XB_B", 172, 230, 10, 10),
            ("SPR_MINI_XB_X", 184, 230, 10, 10),
        ],
        "env_ui": [
            ("FONT_8X8", 0, 0, 128, 32),
            ("SPR_BTN_CROSS", 128, 0, 12, 12),
            ("SPR_BTN_CIRCLE", 144, 0, 12, 12),
            ("SPR_BTN_SQUARE", 160, 0, 12, 12),
            ("SPR_BTN_TRIANGLE", 176, 0, 12, 12),
            ("SPR_BTN_L1", 128, 28, 12, 12),
            ("SPR_BTN_R1", 144, 28, 12, 12),
            ("SPR_HUD_LIFE_CAN", 192, 0, 10, 14),
            ("SPR_HUD_FROST_ICON", 208, 0, 12, 12),
            ("SPR_HUD_STAR", 224, 0, 8, 8),
            ("SPR_FROST_FRAME_L", 128, 16, 8, 12),
            ("SPR_FROST_FRAME_M", 136, 16, 8, 12),
            ("SPR_FROST_FRAME_R", 144, 16, 8, 12),
            ("SPR_FROST_FILL", 156, 16, 8, 8),
            ("SPR_LANTERN_GLOW", 168, 16, 24, 24),
            ("SPR_TEX_WOOD", 0, 40, 32, 32),
            ("SPR_TEX_WOOD_FRONT", 32, 40, 32, 32),
            ("SPR_TEX_FLOOR", 64, 40, 32, 32),
            ("SPR_TEX_CURTAIN", 96, 40, 16, 24),
            ("SPR_BEER_TAP", 116, 40, 16, 28),
            ("SPR_COUNTER_LEFT", 136, 40, 16, 24),
            ("SPR_COUNTER_MID", 152, 40, 16, 24),
            ("SPR_COUNTER_RIGHT", 168, 40, 16, 24),
            ("SPR_WOOD_POST", 188, 40, 12, 32),
            ("SPR_LANTERN_OFF", 204, 40, 16, 24),
            ("SPR_LANTERN_ON", 224, 40, 16, 24),
            ("SPR_NEON_SIGN", 0, 80, 128, 28),
            ("SPR_SKYLINE_ZHANQIAO", 0, 120, 160, 48),
            ("SPR_SKYLINE_CITY", 160, 120, 96, 48),
            ("SPR_AWNING_TILE", 0, 176, 32, 16),
        ],
        "cards": [
            ("SPR_CARD_COMMON", 0, 0, 48, 64),
            ("SPR_CARD_UNCOMMON", 48, 0, 48, 64),
            ("SPR_CARD_RARE", 96, 0, 48, 64),
            ("SPR_CARD_BACK", 144, 0, 48, 64),
            ("SPR_CARD_TRICK", 192, 0, 48, 64),
            *[(f"SPR_COASTER_{i}", (i % 10) * 24, 68 + (i // 10) * 24, 24, 24) for i in range(20)],
            *[(f"SPR_DRINK_CARD_{d}", d * 24, 120, 24, 24) for d in range(3)],
            *[(f"SPR_TRICK_ICON_{t}", t * 16, 148, 16, 16) for t in range(6)],
            ("SPR_BOSS_SEAL", 120, 148, 24, 24),
        ],
    }
    bounds = {"real_tsingtao": (256, 128), "sprites": (256, 240), "env_ui": (256, 240), "cards": (256, 240)}
    for sname, boxes in sheets.items():
        sw, sh = bounds[sname]
        for name, u, v, w, h in boxes:
            assert 0 <= u and u + w <= sw and u + w - 1 <= 255, f"{sname}:{name} U out of range"
            assert 0 <= v and v + h <= sh and v + h - 1 <= 255, f"{sname}:{name} V out of range"
        for i in range(len(boxes)):
            n1, x1, y1, w1, h1 = boxes[i]
            for j in range(i + 1, len(boxes)):
                n2, x2, y2, w2, h2 = boxes[j]
                if (x1 < x2 + w2) and (x2 < x1 + w1) and (y1 < y2 + h2) and (y2 < y1 + h1):
                    raise AssertionError(f"{sname}: UV overlap {n1} and {n2}")


def emit_assets_layout_h(path: Path):
    content = """\
#ifndef TSINGTAO_ASSETS_LAYOUT_H
#define TSINGTAO_ASSETS_LAYOUT_H

/*
 * TSINGTAO EXPRESS '96 — Single Source of Truth VRAM & Sprite UV Layout
 * Auto-generated by tools/build_assets.py (ZERO AI-generated graphics).
 *
 * VRAM Map (1024 x 512 16-bit halfwords):
 *   Framebuffer 0     : (  0,   0) size 320 x 256  [x:   0..319, y:   0..255]
 *   Framebuffer 1     : (  0, 256) size 320 x 256  [x:   0..319, y: 256..511]
 *   real_tsingtao.tim : (320,   0) size 128 x 128hw (256x128 8-bit) [x: 320..447, y:   0..127]
 *   real_tsingtao CLUT: (320, 240) size 256 x   1hw                 [x: 320..575, y: 240]
 *   sprites.tim       : (448,   0) size 128 x 240hw (256x240 8-bit) [x: 448..575, y:   0..239]
 *   sprites CLUT      : (320, 241) size 256 x   1hw                 [x: 320..575, y: 241]
 *   env_ui.tim        : (576,   0) size  64 x 240hw (256x240 4-bit) [x: 576..639, y:   0..239]
 *   env_ui CLUT       : (576, 240) size  16 x   1hw                 [x: 576..591, y: 240]
 *   cards.tim         : (640,   0) size 128 x 240hw (256x240 8-bit) [x: 640..767, y:   0..239]
 *   cards CLUT        : (320, 242) size 256 x   1hw                 [x: 320..575, y: 242]
 */

/* --- Sheet VRAM & Texture Page Coordinates --- */
#define VRAM_REAL_X         320
#define VRAM_REAL_Y         0
#define VRAM_REAL_TP        1   /* 8-bit CLUT */
#define VRAM_REAL_CLUT_X    320
#define VRAM_REAL_CLUT_Y    240

#define VRAM_SPRITES_X      448
#define VRAM_SPRITES_Y      0
#define VRAM_SPRITES_TP     1   /* 8-bit CLUT */
#define VRAM_SPRITES_CLUT_X 320
#define VRAM_SPRITES_CLUT_Y 241

#define VRAM_ENV_UI_X       576
#define VRAM_ENV_UI_Y       0
#define VRAM_ENV_UI_TP      0   /* 4-bit CLUT */
#define VRAM_ENV_UI_CLUT_X  576
#define VRAM_ENV_UI_CLUT_Y  240

#define VRAM_CARDS_X        640
#define VRAM_CARDS_Y        0
#define VRAM_CARDS_TP       1   /* 8-bit CLUT */
#define VRAM_CARDS_CLUT_X   320
#define VRAM_CARDS_CLUT_Y   242

/* =========================================================================
 * Sheet 1: real_tsingtao.tim (GFX_TEX_REAL, 8-bit CLUT, 256x128)
 * Genuine real-world Tsingtao Beer logo, green can, and green bottle
 * ========================================================================= */
#define SPR_REAL_LOGO_U          0
#define SPR_REAL_LOGO_V          0
#define SPR_REAL_LOGO_W          128
#define SPR_REAL_LOGO_H          64

#define SPR_REAL_HERO_CAN_U      128
#define SPR_REAL_HERO_CAN_V      0
#define SPR_REAL_HERO_CAN_W      32
#define SPR_REAL_HERO_CAN_H      48

#define SPR_REAL_HERO_BOTTLE_U   164
#define SPR_REAL_HERO_BOTTLE_V   0
#define SPR_REAL_HERO_BOTTLE_W   24
#define SPR_REAL_HERO_BOTTLE_H   60

#define SPR_REAL_CAN_U           192
#define SPR_REAL_CAN_V           0
#define SPR_REAL_CAN_W           12
#define SPR_REAL_CAN_H           18

#define SPR_REAL_BOTTLE_U        208
#define SPR_REAL_BOTTLE_V        0
#define SPR_REAL_BOTTLE_W        12
#define SPR_REAL_BOTTLE_H        22

#define SPR_REAL_MINI_CAN_U      224
#define SPR_REAL_MINI_CAN_V      0
#define SPR_REAL_MINI_CAN_W      10
#define SPR_REAL_MINI_CAN_H      14

#define SPR_REAL_SHOWCASE_CAN_U  128
#define SPR_REAL_SHOWCASE_CAN_V  64
#define SPR_REAL_SHOWCASE_CAN_W  40
#define SPR_REAL_SHOWCASE_CAN_H  60

/* =========================================================================
 * Sheet 2: sprites.tim (GFX_TEX_SPRITES, 8-bit 256-color CLUT, 256x240)
 * Hand-crafted deterministic pixel art characters, beer bag, bubbles & VFX
 * ========================================================================= */

/* Mascot Bartender (28x36 each, row v=0, stride 32) */
#define SPR_MASCOT_W             28
#define SPR_MASCOT_H             36
#define SPR_MASCOT_IDLE_U        0
#define SPR_MASCOT_IDLE_V        0
#define SPR_MASCOT_POUR_U        32
#define SPR_MASCOT_POUR_V        0
#define SPR_MASCOT_DASH_U        64
#define SPR_MASCOT_DASH_V        0
#define SPR_MASCOT_DASH2_U       96
#define SPR_MASCOT_DASH2_V       0
#define SPR_MASCOT_CELEBRATE_U   128
#define SPR_MASCOT_CELEBRATE_V   0
#define SPR_MASCOT_FOAM_U        160
#define SPR_MASCOT_FOAM_V        0
#define SPR_MASCOT_IDLE2_U       192
#define SPR_MASCOT_IDLE2_V       0

/* Bonus Round Mischievous Hand (24x24) */
#define SPR_BONUS_HAND_U         224
#define SPR_BONUS_HAND_V         0
#define SPR_BONUS_HAND_W         24
#define SPR_BONUS_HAND_H         24

/* Customer Archetypes (24x34 each; cell stride 28x36)
 *   type 0 = Regular Local      (v=40)
 *   type 1 = Big Foodie         (v=76)
 *   type 2 = Bottle VIP         (v=112)
 *   type 3 = Bag VIP            (v=148)
 *   type 4 = Late-Night Rusher  (v=184)
 *   frame 0=walk1 (u=0), 1=walk2 (u=28), 2=drink (u=56), 3=angry (u=84)
 */
#define SPR_CUST_W               24
#define SPR_CUST_H               34
#define SPR_CUST_FRAME_WALK1     0
#define SPR_CUST_FRAME_WALK2     1
#define SPR_CUST_FRAME_DRINK     2
#define SPR_CUST_FRAME_ANGRY     3
#define SPR_CUST_U(frame)        ((frame) * 28)
#define SPR_CUST_V(archetype)    (40 + (archetype) * 36)

#define SPR_CUST_LOCAL_V         40
#define SPR_CUST_FOODIE_V        76
#define SPR_CUST_BOTTLE_VIP_V    112
#define SPR_CUST_BAG_VIP_V       148
#define SPR_CUST_RUSHER_V        184

/* Drinks, Empties & Tips */
#define SPR_BEER_BAG_U           120
#define SPR_BEER_BAG_V           40
#define SPR_BEER_BAG_W           14
#define SPR_BEER_BAG_H           20

#define SPR_EMPTY_CAN_U          140
#define SPR_EMPTY_CAN_V          40
#define SPR_EMPTY_CAN_W          14
#define SPR_EMPTY_CAN_H          12

#define SPR_EMPTY_BOTTLE_U       160
#define SPR_EMPTY_BOTTLE_V       40
#define SPR_EMPTY_BOTTLE_W       16
#define SPR_EMPTY_BOTTLE_H       12

#define SPR_CASH_TIP_U           180
#define SPR_CASH_TIP_V           40
#define SPR_CASH_TIP_W           16
#define SPR_CASH_TIP_H           12

/* VFX Sprites (16x16 at v=76) */
#define SPR_FX_FOAM1_U           120
#define SPR_FX_FOAM1_V           76
#define SPR_FX_FOAM1_W           16
#define SPR_FX_FOAM1_H           16

#define SPR_FX_FOAM2_U           140
#define SPR_FX_FOAM2_V           76
#define SPR_FX_FOAM2_W           16
#define SPR_FX_FOAM2_H           16

#define SPR_FX_ICE1_U            160
#define SPR_FX_ICE1_V            76
#define SPR_FX_ICE1_W            16
#define SPR_FX_ICE1_H            16

#define SPR_FX_ICE2_U            180
#define SPR_FX_ICE2_V            76
#define SPR_FX_ICE2_W            16
#define SPR_FX_ICE2_H            16

#define SPR_FX_SMASH_U           200
#define SPR_FX_SMASH_V           76
#define SPR_FX_SMASH_W           16
#define SPR_FX_SMASH_H           16

/* Order Speech-Bubble Icons (18x18 at v=104 & v=126) */
#define SPR_BUBBLE_W             18
#define SPR_BUBBLE_H             18
#define SPR_BUBBLE_CAN_U         120
#define SPR_BUBBLE_CAN_V         104
#define SPR_BUBBLE_BOTTLE_U      140
#define SPR_BUBBLE_BOTTLE_V      104
#define SPR_BUBBLE_BAG_U         160
#define SPR_BUBBLE_BAG_V         104
#define SPR_BUBBLE_ANGRY_CAN_U   180
#define SPR_BUBBLE_ANGRY_CAN_V   104
#define SPR_BUBBLE_ANGRY_BOTTLE_U 200
#define SPR_BUBBLE_ANGRY_BOTTLE_V 104
#define SPR_BUBBLE_ANGRY_BAG_U   220
#define SPR_BUBBLE_ANGRY_BAG_V   104
#define SPR_BUBBLE_HAPPY_U       120
#define SPR_BUBBLE_HAPPY_V       126
#define SPR_BUBBLE_ANGRY_U       140
#define SPR_BUBBLE_ANGRY_V       126

/* Phase 2 Buff Pickups, Danger Alert & Additive Halos */
#define SPR_BUFF_W               16
#define SPR_BUFF_H               16
#define SPR_BUFF_FROST_U         120
#define SPR_BUFF_FROST_V         148
#define SPR_BUFF_STAR_U          140
#define SPR_BUFF_STAR_V          148
#define SPR_BUFF_SHIELD_U        160
#define SPR_BUFF_SHIELD_V        148

#define SPR_DANGER_ALERT_U       180
#define SPR_DANGER_ALERT_V       148
#define SPR_DANGER_ALERT_W       16
#define SPR_DANGER_ALERT_H       16

#define SPR_BUFF_HALO_U          120
#define SPR_BUFF_HALO_V          172
#define SPR_BUFF_HALO_W          20
#define SPR_BUFF_HALO_H          20

#define SPR_DANGER_HALO_U        144
#define SPR_DANGER_HALO_V        172
#define SPR_DANGER_HALO_W        20
#define SPR_DANGER_HALO_H        20

/* Glossy 256-Color Button Glyph Atlas (PS1 + Xbox + 10x10 Mini Drink Badges) */
#define SPR_GLYPH_W              14
#define SPR_GLYPH_H              14
#define SPR_GLYPH_SHLD_W         18
#define SPR_GLYPH_SHLD_H         12
#define SPR_GLYPH_PS_CROSS_U     120
#define SPR_GLYPH_PS_CROSS_V     196
#define SPR_GLYPH_PS_CIRCLE_U    136
#define SPR_GLYPH_PS_CIRCLE_V    196
#define SPR_GLYPH_PS_SQUARE_U    152
#define SPR_GLYPH_PS_SQUARE_V    196
#define SPR_GLYPH_PS_TRIANGLE_U  168
#define SPR_GLYPH_PS_TRIANGLE_V  196
#define SPR_GLYPH_PS_L1_U        184
#define SPR_GLYPH_PS_L1_V        197
#define SPR_GLYPH_PS_R1_U        204
#define SPR_GLYPH_PS_R1_V        197
#define SPR_GLYPH_DPAD_U         224
#define SPR_GLYPH_DPAD_V         196

#define SPR_GLYPH_XB_A_U         120
#define SPR_GLYPH_XB_A_V         214
#define SPR_GLYPH_XB_B_U         136
#define SPR_GLYPH_XB_B_V         214
#define SPR_GLYPH_XB_X_U         152
#define SPR_GLYPH_XB_X_V         214
#define SPR_GLYPH_XB_Y_U         168
#define SPR_GLYPH_XB_Y_V         214
#define SPR_GLYPH_XB_LB_U        184
#define SPR_GLYPH_XB_LB_V        215
#define SPR_GLYPH_XB_RB_U        204
#define SPR_GLYPH_XB_RB_V        215

#define SPR_MINI_GLYPH_W         10
#define SPR_MINI_GLYPH_H         10
#define SPR_MINI_PS_CROSS_U      120
#define SPR_MINI_PS_CROSS_V      230
#define SPR_MINI_PS_CIRCLE_U     132
#define SPR_MINI_PS_CIRCLE_V     230
#define SPR_MINI_PS_SQUARE_U     144
#define SPR_MINI_PS_SQUARE_V     230
#define SPR_MINI_XB_A_U          160
#define SPR_MINI_XB_A_V          230
#define SPR_MINI_XB_B_U          172
#define SPR_MINI_XB_B_V          230
#define SPR_MINI_XB_X_U          184
#define SPR_MINI_XB_X_V          230

/* =========================================================================
 * Sheet 3: env_ui.tim (GFX_TEX_ENV_UI, 4-bit CLUT, 256x240)
 * Arcade 8x8 Font, PS1 Buttons, HUD, 2.5D Textures, Props & Skyline
 * ========================================================================= */

/* 8x8 ASCII Arcade Bitmap Font (ASCII 32..95, 16x4 at 0,0; '$'='¥', '*'='×') */
#define FONT_FIRST_CHAR          32
#define FONT_LAST_CHAR           95
#define FONT_CHAR_W              8
#define FONT_CHAR_H              8
#define FONT_ORIGIN_U            0
#define FONT_ORIGIN_V            0
#define FONT_CHAR_U(c)           ((((c) - 32) & 15) * 8)
#define FONT_CHAR_V(c)           ((((c) - 32) >> 4) * 8)

/* PS1 Controller Button Glyphs (12x12) */
#define SPR_BTN_W                12
#define SPR_BTN_H                12
#define SPR_BTN_CROSS_U          128
#define SPR_BTN_CROSS_V          0
#define SPR_BTN_CIRCLE_U         144
#define SPR_BTN_CIRCLE_V         0
#define SPR_BTN_SQUARE_U         160
#define SPR_BTN_SQUARE_V         0
#define SPR_BTN_TRIANGLE_U       176
#define SPR_BTN_TRIANGLE_V       0
#define SPR_BTN_L1_U             128
#define SPR_BTN_L1_V             28
#define SPR_BTN_R1_U             144
#define SPR_BTN_R1_V             28

/* HUD Icons & Frost Gauge */
#define SPR_HUD_LIFE_CAN_U       192
#define SPR_HUD_LIFE_CAN_V       0
#define SPR_HUD_LIFE_CAN_W       10
#define SPR_HUD_LIFE_CAN_H       14

#define SPR_HUD_FROST_ICON_U     208
#define SPR_HUD_FROST_ICON_V     0
#define SPR_HUD_FROST_ICON_W     12
#define SPR_HUD_FROST_ICON_H     12

#define SPR_HUD_STAR_U           224
#define SPR_HUD_STAR_V           0
#define SPR_HUD_STAR_W           8
#define SPR_HUD_STAR_H           8

#define SPR_FROST_FRAME_L_U      128
#define SPR_FROST_FRAME_L_V      16
#define SPR_FROST_FRAME_M_U      136
#define SPR_FROST_FRAME_M_V      16
#define SPR_FROST_FRAME_R_U      144
#define SPR_FROST_FRAME_R_V      16
#define SPR_FROST_FRAME_W        8
#define SPR_FROST_FRAME_H        12

#define SPR_FROST_FILL_U         156
#define SPR_FROST_FILL_V         16
#define SPR_FROST_FILL_W         8
#define SPR_FROST_FILL_H         8

/* 2.5D Low-Poly Textures & Bar Props */
#define SPR_TEX_WOOD_U           0
#define SPR_TEX_WOOD_V           40
#define SPR_TEX_WOOD_W           32
#define SPR_TEX_WOOD_H           32

#define SPR_TEX_WOOD_FRONT_U     32
#define SPR_TEX_WOOD_FRONT_V     40
#define SPR_TEX_WOOD_FRONT_W     32
#define SPR_TEX_WOOD_FRONT_H     32

#define SPR_TEX_FLOOR_U          64
#define SPR_TEX_FLOOR_V          40
#define SPR_TEX_FLOOR_W          32
#define SPR_TEX_FLOOR_H          32

#define SPR_TEX_CURTAIN_U        96
#define SPR_TEX_CURTAIN_V        40
#define SPR_TEX_CURTAIN_W        16
#define SPR_TEX_CURTAIN_H        24

#define SPR_BEER_TAP_U           116
#define SPR_BEER_TAP_V           40
#define SPR_BEER_TAP_W           16
#define SPR_BEER_TAP_H           28

#define SPR_COUNTER_W            16
#define SPR_COUNTER_H            24
#define SPR_COUNTER_LEFT_U       136
#define SPR_COUNTER_LEFT_V       40
#define SPR_COUNTER_MID_U        152
#define SPR_COUNTER_MID_V        40
#define SPR_COUNTER_RIGHT_U      168
#define SPR_COUNTER_RIGHT_V      40

#define SPR_WOOD_POST_U          188
#define SPR_WOOD_POST_V          40
#define SPR_WOOD_POST_W          12
#define SPR_WOOD_POST_H          32

#define SPR_LANTERN_W            16
#define SPR_LANTERN_H            24
#define SPR_LANTERN_OFF_U        204
#define SPR_LANTERN_OFF_V        40
#define SPR_LANTERN_ON_U         224
#define SPR_LANTERN_ON_V         40

#define SPR_LANTERN_GLOW_U       168
#define SPR_LANTERN_GLOW_V       16
#define SPR_LANTERN_GLOW_W       24
#define SPR_LANTERN_GLOW_H       24

/* Neon "TSINGTAO '96" Bar Sign (128x28) */
#define SPR_NEON_SIGN_U          0
#define SPR_NEON_SIGN_V          80
#define SPR_NEON_SIGN_W          128
#define SPR_NEON_SIGN_H          28

/* Qingdao Zhanqiao Pier Pavilion & Harbor City Skyline */
#define SPR_SKYLINE_ZHANQIAO_U   0
#define SPR_SKYLINE_ZHANQIAO_V   120
#define SPR_SKYLINE_ZHANQIAO_W   160
#define SPR_SKYLINE_ZHANQIAO_H   48

#define SPR_SKYLINE_CITY_U       160
#define SPR_SKYLINE_CITY_V       120
#define SPR_SKYLINE_CITY_W       96
#define SPR_SKYLINE_CITY_H       48

/* Striped Qingdao Street-Bar Awning Valence (32x16) */
#define SPR_AWNING_TILE_U        0
#define SPR_AWNING_TILE_V        176
#define SPR_AWNING_TILE_W        32
#define SPR_AWNING_TILE_H        16

/* =========================================================================
 * Sheet 4: cards.tim (GFX_TEX_CARDS, 8-bit 256-color CLUT, 256x240)
 * 3D Roguelike Cards, 20 Coaster Emblems, 3 Drink Recipes, 6 Tricks & Seal
 * ========================================================================= */
#define SPR_CARD_FRAME_W         48
#define SPR_CARD_FRAME_H         64
#define SPR_CARD_COMMON_U        0
#define SPR_CARD_COMMON_V        0
#define SPR_CARD_UNCOMMON_U      48
#define SPR_CARD_UNCOMMON_V      0
#define SPR_CARD_RARE_U          96
#define SPR_CARD_RARE_V          0
#define SPR_CARD_BACK_U          144
#define SPR_CARD_BACK_V          0
#define SPR_CARD_TRICK_U         192
#define SPR_CARD_TRICK_V         0

#define SPR_COASTER_ICON_W       24
#define SPR_COASTER_ICON_H       24
#define SPR_COASTER_ICON_U(id)   (((id) % 10) * 24)
#define SPR_COASTER_ICON_V(id)   (68 + ((id) / 10) * 24)

#define SPR_DRINK_CARD_W         24
#define SPR_DRINK_CARD_H         24
#define SPR_DRINK_CARD_U(d)      ((d) * 24)
#define SPR_DRINK_CARD_V(d)      120

#define SPR_TRICK_ICON_W         16
#define SPR_TRICK_ICON_H         16
#define SPR_TRICK_ICON_U(t)      ((t) * 16)
#define SPR_TRICK_ICON_V(t)      148

#define SPR_BOSS_SEAL_U          120
#define SPR_BOSS_SEAL_V          148
#define SPR_BOSS_SEAL_W          24
#define SPR_BOSS_SEAL_H          24

#endif /* TSINGTAO_ASSETS_LAYOUT_H */
"""
    path.write_text(content, encoding="utf-8")


def emit_tim_blobs(blobs: Dict[str, bytes], h_path: Path, c_path: Path):
    h_lines = [
        "#ifndef TSINGTAO_TIM_BLOBS_H",
        "#define TSINGTAO_TIM_BLOBS_H",
        "",
        "#include <stdint.h>",
        "",
        "/* 4-byte aligned embedded PlayStation 1 .TIM textures (zero mid-game CD reads) */",
    ]
    for name in blobs:
        h_lines.append(f"extern const uint8_t g_tim_{name}[];")
        h_lines.append(f"extern const uint32_t g_tim_{name}_size;")
    h_lines.extend(["", "#endif /* TSINGTAO_TIM_BLOBS_H */", ""])
    h_path.write_text("\n".join(h_lines), encoding="utf-8")

    c_lines = [
        '#include "tim_blobs.h"',
        "",
        "/* Auto-generated by tools/build_assets.py — 4-byte aligned for MIPS R3000A */",
        "",
    ]
    for name, data in blobs.items():
        c_lines.append(f"const uint32_t g_tim_{name}_size = {len(data)}u;")
        c_lines.append(f"const uint8_t g_tim_{name}[{len(data)}] __attribute__((aligned(4))) = {{")
        for i in range(0, len(data), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in data[i : i + 16])
            c_lines.append(f"    {chunk},")
        c_lines.append("};")
        c_lines.append("")
    c_path.write_text("\n".join(c_lines), encoding="utf-8")


def emit_mc_icon_h(clut_555: List[int], bitmap: List[int], path: Path):
    clut_hex = ", ".join(f"0x{w:04X}u" for w in clut_555)
    bmp_lines = [
        "    " + ", ".join(f"0x{b:02X}u" for b in bitmap[i : i + 8]) + ","
        for i in range(0, len(bitmap), 8)
    ]
    content = f"""\
#ifndef TSINGTAO_MC_ICON_H
#define TSINGTAO_MC_ICON_H

#include <stdint.h>

/*
 * PlayStation 1 Memory Card Icon (16x16 4-bit + 16-color RGB555 CLUT)
 * Displays an authentic green Tsingtao Beer Can inside the real PS1 BIOS
 * Memory Card Manager. Auto-generated by tools/build_assets.py.
 */
#define MC_ICON_CLUT_COUNT  16
#define MC_ICON_CLUT_BYTES  32
#define MC_ICON_DATA_BYTES  128

static const uint16_t MC_ICON_CLUT[16] = {{
    {clut_hex}
}};

static const uint8_t MC_ICON_BITMAP[128] = {{
{chr(10).join(bmp_lines)}
}};

#endif /* TSINGTAO_MC_ICON_H */
"""
    path.write_text(content, encoding="utf-8")


def verify_vram_no_overlaps(metas: Dict[str, Dict[str, int]]):
    rects = [
        ("FB0", 0, 0, 320, 256),
        ("FB1", 0, 256, 320, 256),
    ]
    for name, m in metas.items():
        rects.append((f"{name}.img", m["img_x"], m["img_y"], m["img_w_hw"], m["img_h"]))
        rects.append((f"{name}.clut", m["clut_x"], m["clut_y"], m["clut_w_hw"], m["clut_h"]))

    for name, x, y, w, h in rects:
        assert 0 <= x and x + w <= 1024, f"{name} X out of VRAM bounds: {x}+{w}"
        assert 0 <= y and y + h <= 512, f"{name} Y out of VRAM bounds: {y}+{h}"

    for i in range(len(rects)):
        n1, x1, y1, w1, h1 = rects[i]
        for j in range(i + 1, len(rects)):
            n2, x2, y2, w2, h2 = rects[j]
            if (x1 < x2 + w2) and (x2 < x1 + w1) and (y1 < y2 + h2) and (y2 < y1 + h1):
                raise AssertionError(
                    f"VRAM OVERLAP between {n1} ({x1},{y1},{w1}x{h1}) and {n2} ({x2},{y2},{w2}x{h2})"
                )
    print("[ok] VRAM overlap check PASSED across all framebuffers, textures, and CLUTs:")
    for name, x, y, w, h in rects:
        print(f"     {name:20s}: ({x:3d}, {y:3d}) size {w:3d} x {h:3d} hw -> [{x}..{x+w-1}, {y}..{y+h-1}]")


def make_checker(w: int, h: int, cell: int = 8) -> Image.Image:
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (38, 42, 54, 255) if ((x // cell + y // cell) % 2 == 0) else (28, 32, 42, 255)
    return im


def export_previews(
    preview_dir: Path,
    real_im: Image.Image,
    spr_im: Image.Image,
    env_im: Image.Image,
    cards_im: Image.Image,
    mc_im: Image.Image,
):
    preview_dir.mkdir(parents=True, exist_ok=True)

    def save_sheet(im: Image.Image, fname: str, scale: int = 3):
        bg = make_checker(im.width * scale, im.height * scale, cell=8 * scale)
        up = im.resize((im.width * scale, im.height * scale), Image.Resampling.NEAREST)
        bg.alpha_composite(up)
        bg.save(preview_dir / fname)

    save_sheet(real_im, "preview_real_tsingtao.png", scale=3)
    save_sheet(spr_im, "preview_sprites.png", scale=3)
    save_sheet(env_im, "preview_env_ui.png", scale=3)
    save_sheet(cards_im, "preview_cards.png", scale=3)

    comp = Image.new("RGBA", (1280, 960), (20, 24, 34, 255))
    draw = ImageDraw.Draw(comp)

    def paste_up(src: Image.Image, box: Tuple[int, int, int, int], dx: int, dy: int, scale: int):
        crop = src.crop((box[0], box[1], box[0] + box[2], box[1] + box[3]))
        up = crop.resize((box[2] * scale, box[3] * scale), Image.Resampling.NEAREST)
        chk = make_checker(up.width, up.height, cell=max(4, scale * 2))
        chk.alpha_composite(up)
        comp.paste(chk, (dx, dy))

    draw.text((20, 10), "TSINGTAO EXPRESS '96 v2 -- ZERO-AI PS1 ASSET SHOWCASE (Decoded Straight From .TIM Binaries)", fill=(248, 208, 56, 255))

    # 1. Real Assets
    draw.text((20, 34), "1. REAL DIGITIZED TSINGTAO (real_tsingtao.tim -- 8-bit, rescaled 12x18 can & 12x22 bottle)", fill=(120, 232, 160, 255))
    paste_up(real_im, (0, 0, 128, 64), 20, 52, 3)
    paste_up(real_im, (128, 0, 32, 48), 424, 52, 4)
    paste_up(real_im, (164, 0, 24, 60), 570, 52, 3)
    paste_up(real_im, (192, 0, 12, 18), 662, 52, 5)
    paste_up(real_im, (208, 0, 12, 22), 742, 52, 5)
    paste_up(real_im, (224, 0, 10, 14), 822, 52, 5)
    mc_up = mc_im.resize((96, 96), Image.Resampling.NEAREST)
    comp.paste(mc_up, (920, 58))

    # 2. 256-color Mascot, Drinks, Empties, Bubbles
    draw.text((20, 252), "2. 256-COLOR MASCOT (28x36 x 7) + BAG, HORIZONTAL EMPTIES, YUAN TIP, VFX & RED-RIMMED ANGRY BUBBLES", fill=(248, 200, 96, 255))
    for i in range(7):
        paste_up(spr_im, (i * 32, 0, 28, 36), 20 + i * 74, 270, 2)
    paste_up(spr_im, (224, 0, 24, 24), 544, 278, 2)
    paste_up(spr_im, (120, 40, 14, 20), 610, 274, 3)
    paste_up(spr_im, (140, 40, 14, 12), 666, 286, 4)
    paste_up(spr_im, (160, 40, 16, 12), 736, 286, 4)
    paste_up(spr_im, (180, 40, 16, 12), 812, 286, 4)
    for i, ux in enumerate([120, 140, 160, 180, 200, 220]):
        paste_up(spr_im, (ux, 104, 18, 18), 896 + i * 60, 270, 3)
    for i, ux in enumerate([120, 140, 160, 180, 200]):
        paste_up(spr_im, (ux, 76, 16, 16), 896 + i * 60, 330, 3)
    # Phase 2 Buffs, Danger Alert & Halos
    for i, (ux, vy, w, h) in enumerate([
        (120, 148, 16, 16), (140, 148, 16, 16), (160, 148, 16, 16),
        (180, 148, 16, 16), (120, 172, 20, 20), (144, 172, 20, 20),
    ]):
        paste_up(spr_im, (ux, vy, w, h), 580 + i * 52, 330, 2)

    # 3. 5 Customer Archetypes (24x34)
    draw.text((20, 392), "3. 256-COLOR CUSTOMERS (24x34: Local / Foodie / Bottle VIP / Bag VIP / Rusher)", fill=(120, 224, 248, 255))
    for r in range(5):
        for c in range(4):
            paste_up(spr_im, (c * 28, 40 + r * 36, 24, 34), 20 + r * 244 + c * 56, 412, 2)

    # 4. env_ui.tim (Textures, Props, Font w/ ¥ & ×, L1/R1)
    draw.text((20, 492), "4. 2.5D TEXTURES (WOOD, FRONT, FLOOR, CURTAIN), PROPS, FONT ('¥' '$' & '×' '*') & L1/R1 (env_ui.tim)", fill=(248, 136, 184, 255))
    paste_up(env_im, (0, 40, 32, 32), 20, 512, 2)
    paste_up(env_im, (32, 40, 32, 32), 92, 512, 2)
    paste_up(env_im, (64, 40, 32, 32), 164, 512, 2)
    paste_up(env_im, (96, 40, 16, 24), 236, 512, 2)
    paste_up(env_im, (0, 0, 128, 32), 290, 512, 2)
    for i, (ux, vy) in enumerate([(128, 0), (144, 0), (160, 0), (176, 0), (128, 28), (144, 28)]):
        paste_up(env_im, (ux, vy, 12, 12), 570 + i * 36, 512, 2)
    paste_up(env_im, (0, 80, 128, 28), 800, 512, 2)

    # 5. cards.tim (Frames + 20 Coasters + 3 Drinks + 6 Tricks + Boss Seal)
    draw.text((20, 594), "5. 8-BIT ROGUELIKE CARDS SHEET (cards.tim -- 5 Frames, 20 Coasters, 3 Drinks, 6 Tricks, Boss Seal)", fill=(248, 208, 56, 255))
    for i in range(5):
        paste_up(cards_im, (i * 48, 0, 48, 64), 20 + i * 110, 616, 2)
    for i in range(20):
        cx = 20 + (i % 10) * 58
        cy = 760 + (i // 10) * 58
        paste_up(cards_im, ((i % 10) * 24, 68 + (i // 10) * 24, 24, 24), cx, cy, 2)
    for d in range(3):
        paste_up(cards_im, (d * 24, 120, 24, 24), 630 + d * 60, 760, 2)
    for t in range(6):
        paste_up(cards_im, (t * 16, 148, 16, 16), 630 + t * 44, 830, 2)
    paste_up(cards_im, (120, 148, 24, 24), 920, 760, 3)

    comp_path = preview_dir / "tsingtao_assets_showcase.png"
    comp.save(comp_path)
    print(f"[ok] Exported showcase preview -> {comp_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build PS1 .TIM assets and C headers for TSINGTAO EXPRESS '96.")
    parser.add_argument("--preview", type=str, default="", help="Optional directory to write composite preview PNGs.")
    args = parser.parse_args()

    for req in ("tsingtao_logo.png", "tsingtao_can.png", "tsingtao_bottle.png"):
        p = RAW_DIR / req
        if not p.exists():
            print(f"[error] Missing {p}. Run tools/fetch_real_assets.py first!", file=sys.stderr)
            return 1

    GEN_DIR.mkdir(parents=True, exist_ok=True)
    verify_uv_layout()

    print("[build] Quantizing real Tsingtao photos -> assets/gen/real_tsingtao.tim (8-bit CLUT)...")
    real_pal, real_idx = build_real_tsingtao_sheet()
    real_tim = write_tim(
        GEN_DIR / "real_tsingtao.tim", bpp=8,
        img_x=320, img_y=0, clut_x=320, clut_y=240,
        palette_rgb=real_pal, indices=real_idx,
    )

    print("[build] Compiling 256-color pixel sprites -> assets/gen/sprites.tim (8-bit CLUT)...")
    spr_pal, spr_idx = build_sprites_sheet()
    spr_tim = write_tim(
        GEN_DIR / "sprites.tim", bpp=8,
        img_x=448, img_y=0, clut_x=320, clut_y=241,
        palette_rgb=spr_pal, indices=spr_idx,
    )

    print("[build] Compiling 2.5D bar textures, skyline, font & UI -> assets/gen/env_ui.tim (4-bit CLUT)...")
    env_pal, env_idx = build_env_ui_sheet()
    env_tim = write_tim(
        GEN_DIR / "env_ui.tim", bpp=4,
        img_x=576, img_y=0, clut_x=576, clut_y=240,
        palette_rgb=env_pal, indices=env_idx,
    )

    print("[build] Compiling 3D roguelike cards, coasters & tricks -> assets/gen/cards.tim (8-bit CLUT)...")
    cards_pal, cards_idx = build_cards_sheet()
    cards_tim = write_tim(
        GEN_DIR / "cards.tim", bpp=8,
        img_x=640, img_y=0, clut_x=320, clut_y=242,
        palette_rgb=cards_pal, indices=cards_idx,
    )

    real_im, real_meta = decode_tim_to_rgba(real_tim)
    spr_im, spr_meta = decode_tim_to_rgba(spr_tim)
    env_im, env_meta = decode_tim_to_rgba(env_tim)
    cards_im, cards_meta = decode_tim_to_rgba(cards_tim)

    verify_vram_no_overlaps({
        "real_tsingtao": real_meta,
        "sprites": spr_meta,
        "env_ui": env_meta,
        "cards": cards_meta,
    })

    emit_assets_layout_h(GEN_DIR / "assets_layout.h")
    emit_tim_blobs(
        {
            "real_tsingtao": real_tim,
            "sprites": spr_tim,
            "env_ui": env_tim,
            "cards": cards_tim,
        },
        GEN_DIR / "tim_blobs.h",
        GEN_DIR / "tim_blobs.c",
    )

    mc_clut, mc_bmp, mc_im = build_mc_icon()
    emit_mc_icon_h(mc_clut, mc_bmp, GEN_DIR / "mc_icon.h")

    total_tim = len(real_tim) + len(spr_tim) + len(env_tim) + len(cards_tim)
    print(
        f"[ok] Generated TIMs: real_tsingtao={len(real_tim)}B, sprites={len(spr_tim)}B, "
        f"env_ui={len(env_tim)}B, cards={len(cards_tim)}B "
        f"(total={total_tim}B / {total_tim/1024:.1f} KB)"
    )
    print("[ok] Generated headers: assets_layout.h, tim_blobs.h/.c, mc_icon.h")

    if args.preview:
        export_previews(Path(args.preview), real_im, spr_im, env_im, cards_im, mc_im)

    return 0


if __name__ == "__main__":
    sys.exit(main())
