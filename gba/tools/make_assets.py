#!/usr/bin/env python3
"""
Generate gba/elfin/source/assets.c from BrickEmuPy's Elfin Twins artwork.

Output (all const, in ROM):
  elfin_rom[]         the 32 KB game ROM (assets/ElfinTwins.bin)
  bg_bitmap[]         240x160 RGB15 image of the LCD panel, all segments off
  pix_*[]             every screen pixel touched by a segment or its shadow:
                      offset, background colour and a list of contributors
  seg_pix_*[]         for each of the 64*8 LCD RAM bits, the pixels to redraw

At run time a pixel is composed as
    colour = background
    for each shadow contributor:  colour darkened by alpha * intensity
    for each segment contributor: colour blended to ink by alpha * intensity
which reproduces the SVG renderer (plus BrickEmuPy's LCD drop shadow).

Usage: python3 gba/tools/make_assets.py   (needs cairosvg, pillow, numpy)
"""
import io
import os
import re
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import elfin_lcd  # noqa: E402

ROOT = elfin_lcd.ROOT
OUT_C = os.path.join(ROOT, "gba", "elfin", "source", "assets.c")
OUT_H = os.path.join(ROOT, "gba", "elfin", "source", "assets.h")
ROM = os.path.join(ROOT, "assets", "ElfinTwins.bin")
PREVIEW = os.path.join(ROOT, "gba", "elfin", "preview.png")

W, H = 240, 160
S = 6.0 / 11.0            # SVG units -> GBA pixels (dot pitch 11 units -> 6 px)
DOT_X0, DOT_Y0 = 34, 32   # screen position of the top-left dot
DOT_PITCH, DOT_SIZE = 6, 5
INK = (22, 24, 30)        # colour of a fully lit segment
SHADOW_OFFSET = (1, 1)    # BrickEmuPy draws a 10% shadow offset by 3 units
SHADOW_ALPHA = 0.10
MAX_CONTRIB = 6

# SVG origin chosen so that the dot matrix lands exactly on the pixel grid
FIRST_DOT = elfin_lcd.DOT_RECTS[elfin_lcd.MATRIX[(0, 0)]]
ORG_X = FIRST_DOT[0] - DOT_X0 / S
ORG_Y = FIRST_DOT[1] - DOT_Y0 / S


def render(opacity_of):
    import cairosvg
    svg = elfin_lcd._svg_with_opacity(opacity_of)
    svg = re.sub(r'<svg width="\d+" height="\d+" viewBox="[^"]+"',
                 '<svg width="%d" height="%d" viewBox="%f %f %f %f"'
                 % (W, H, ORG_X, ORG_Y, W / S, H / S), svg, count=1)
    png = cairosvg.svg2png(bytestring=svg.encode("utf-8"), output_width=W, output_height=H)
    return np.asarray(Image.open(io.BytesIO(png)).convert("RGB")).astype(np.float64)


def rgb15(c):
    r, g, b = (int(max(0, min(255, round(v)))) >> 3 for v in c)
    return r | (g << 5) | (b << 10)


def main():
    print("rendering background ...")
    bg = render(lambda k: 0.0)

    # alpha masks: dots are exact squares, icons come from the SVG
    masks = {}
    for (cx, cy), key in elfin_lcd.MATRIX.items():
        m = np.zeros((H, W))
        x, y = DOT_X0 + cx * DOT_PITCH, DOT_Y0 + cy * DOT_PITCH
        m[y:y + DOT_SIZE, x:x + DOT_SIZE] = 1.0
        masks[key] = m
    for key in elfin_lcd.ICONS:
        print("rendering icon", elfin_lcd.ICON_NAMES[key], "...")
        im = render(lambda k, key=key: 1.0 if k == key else 0.0)
        # black ink over the background: im = bg * (1 - a)
        with np.errstate(divide="ignore", invalid="ignore"):
            a = 1.0 - im.max(axis=2) / np.maximum(bg.max(axis=2), 1.0)
        a[a < 0.03] = 0.0
        masks[key] = np.clip(a, 0.0, 1.0)

    # per-pixel contributors
    keys = sorted(masks)                       # (byte, bit)
    seg_index = {k: k[0] * 8 + k[1] for k in keys}
    contrib = {}                               # pixel -> list of (seg, kind, alpha)
    for k in keys:
        m = masks[k]
        ys, xs = np.nonzero(m)
        for y, x in zip(ys, xs):
            contrib.setdefault(y * W + x, []).append((seg_index[k], 0, m[y, x]))
            sy, sx = y + SHADOW_OFFSET[1], x + SHADOW_OFFSET[0]
            if sy < H and sx < W:
                contrib.setdefault(sy * W + sx, []).append((seg_index[k], 1, m[y, x] * SHADOW_ALPHA))
    pixels = sorted(contrib)
    for p in pixels:
        c = contrib[p]
        # keep the strongest contributors; shadows first so segments paint over them
        c.sort(key=lambda t: -t[2])
        c = c[:MAX_CONTRIB]
        c.sort(key=lambda t: -t[1])
        contrib[p] = c
    print("%d segments, %d pixels, %d contributions"
          % (len(keys), len(pixels), sum(len(contrib[p]) for p in pixels)))

    seg_pixels = {i: [] for i in range(64 * 8)}
    for pi, p in enumerate(pixels):
        for seg, kind, a in contrib[p]:
            if pi not in seg_pixels[seg] or not seg_pixels[seg] or seg_pixels[seg][-1] != pi:
                seg_pixels[seg].append(pi)
    for s in seg_pixels:
        seg_pixels[s] = sorted(set(seg_pixels[s]))

    # ------------------------------------------------------------ preview
    prev = bg.copy()
    for p in pixels:
        y, x = divmod(p, W)
        col = bg[y, x].copy()
        for seg, kind, a in contrib[p]:
            if kind:
                col *= (1 - a)
            else:
                col = col * (1 - a) + np.array(INK) * a
        prev[y, x] = col
    Image.fromarray(prev.astype(np.uint8)).save(PREVIEW)

    # ------------------------------------------------------------ C output
    rom = open(ROM, "rb").read()
    bgw = [rgb15(bg[y, x]) for y in range(H) for x in range(W)]

    def arr(ctype, name, data, per_line=16, fmt="0x%04X"):
        lines = []
        for i in range(0, len(data), per_line):
            lines.append("    " + ", ".join(fmt % v for v in data[i:i + per_line]) + ",")
        return "const %s %s[%d] __attribute__((aligned(4))) = {\n%s\n};\n" % (
            ctype, name, len(data), "\n".join(lines))

    pix_off = [p for p in pixels]
    pix_bg = [bgw[p] for p in pixels]
    pix_first, pix_n, c_seg, c_alpha = [], [], [], []
    for p in pixels:
        pix_first.append(len(c_seg))
        pix_n.append(len(contrib[p]))
        for seg, kind, a in contrib[p]:
            c_seg.append(seg | (kind << 15))
            c_alpha.append(int(round(a * 255)))
    seg_start, seg_list = [], []
    for s in range(64 * 8):
        seg_start.append(len(seg_list))
        seg_list.extend(seg_pixels[s])
    seg_start.append(len(seg_list))

    with open(OUT_H, "w") as f:
        f.write("""/* Generated by gba/tools/make_assets.py - do not edit. */
#ifndef ASSETS_H
#define ASSETS_H
#include <stdint.h>

#define ELFIN_ROM_SIZE %d
#define PIX_COUNT %d
#define CONTRIB_COUNT %d
#define SEG_COUNT 512
#define INK_R %d
#define INK_G %d
#define INK_B %d

extern const uint8_t elfin_rom[ELFIN_ROM_SIZE];
extern const uint16_t bg_bitmap[240 * 160];
extern const uint16_t pix_off[PIX_COUNT];
extern const uint16_t pix_bg[PIX_COUNT];
extern const uint16_t pix_first[PIX_COUNT];
extern const uint8_t pix_n[PIX_COUNT];
extern const uint16_t c_seg[CONTRIB_COUNT];   /* bit 15 = shadow */
extern const uint8_t c_alpha[CONTRIB_COUNT];
extern const uint16_t seg_start[SEG_COUNT + 1];
extern const uint16_t seg_list[];

#endif
""" % (len(rom), len(pixels), len(c_seg), INK[0], INK[1], INK[2]))

    with open(OUT_C, "w") as f:
        f.write("/* Generated by gba/tools/make_assets.py from BrickEmuPy's\n"
                " * assets/ElfinTwins.svg and assets/ElfinTwins.bin - do not edit. */\n")
        f.write('#include "assets.h"\n\n')
        f.write(arr("uint8_t", "elfin_rom", list(rom), 16, "0x%02X"))
        f.write(arr("uint16_t", "bg_bitmap", bgw))
        f.write(arr("uint16_t", "pix_off", pix_off))
        f.write(arr("uint16_t", "pix_bg", pix_bg))
        f.write(arr("uint16_t", "pix_first", pix_first))
        f.write(arr("uint8_t", "pix_n", pix_n, 32, "%d"))
        f.write(arr("uint16_t", "c_seg", c_seg))
        f.write(arr("uint8_t", "c_alpha", c_alpha, 32, "%d"))
        f.write(arr("uint16_t", "seg_start", seg_start))
        f.write(arr("uint16_t", "seg_list", seg_list))
    print("wrote", OUT_C, "and", PREVIEW)


if __name__ == "__main__":
    main()
