#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
"""Draw the wallpaper and icon Home Native ships (pkg/assets/wallpaper.png, pkg/icon.png).

Original art, made only from code: a near-black night with soft aurora ribbons low on the
screen. Dark where the clock and the rows sit, and dim overall, which suits an OLED. The
picture is smooth, so it is worked out at a quarter of the size, scaled up, and dithered.
Plain Python, no dependencies: python3 art/generate.py
"""
import math, random, struct, zlib, os

def png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

# ribbons: (height across the screen at x in 0..1, thickness, colour, strength)
RIBBONS = [
    (lambda x: 0.66 + 0.07 * math.sin(5.1 * x + 0.6) + 0.03 * math.sin(13 * x), 0.050, (0.48, 0.25, 0.95), 0.95),
    (lambda x: 0.72 + 0.05 * math.sin(3.7 * x + 2.2) + 0.02 * math.sin(17 * x + 1), 0.035, (0.08, 0.70, 0.78), 0.75),
    (lambda x: 0.60 + 0.08 * math.sin(2.3 * x + 4.0), 0.090, (0.78, 0.20, 0.55), 0.45),
]

def sky(x, y):
    """linear RGB 0..1 at x, y in 0..1 (y down)"""
    r, g, b = 0.010 + 0.006 * y, 0.011 + 0.008 * y, 0.022 + 0.020 * y  # night, a touch bluer low down
    for f, th, col, k in RIBBONS:
        d = (y - f(x)) / th
        # a ribbon: bright core, a long faint tail upwards (light rising off it), fading at the ends
        a = math.exp(-d * d) + 0.3 * (math.exp(d * 0.6) if d < 0 else math.exp(-d * d * 3))
        a *= k * (0.35 + 0.65 * math.sin(math.pi * min(1, max(0, x * 1.1 - 0.05))) ** 0.7)
        r += col[0] * a * 0.42; g += col[1] * a * 0.42; b += col[2] * a * 0.42
    glow = math.exp(-(((x - 0.5) / 0.45) ** 2 + ((y - 1.05) / 0.30) ** 2))  # horizon light
    r += 0.04 * glow; g += 0.025 * glow; b += 0.07 * glow
    return r, g, b

def render(w, h, sx, sy, stars, seed):
    rnd = random.Random(seed)
    lw, lh = w // sx, h // sy
    low = [[sky((i + 0.5) / lw, (j + 0.5) / lh) for i in range(lw)] for j in range(lh)]
    star = {}
    for _ in range(stars):  # a few dim stars in the upper half, none behind the clock
        x, y = rnd.random(), rnd.random() * 0.55
        if x < 0.25 and y < 0.2: continue
        star[(int(x * w), int(y * h))] = 0.25 + 0.6 * rnd.random() ** 3
    rows = []
    for py in range(h):
        fy = (py + 0.5) / sy - 0.5; j0 = max(0, min(lh - 2, int(fy))); ty = min(1, max(0, fy - j0))
        row = bytearray(w * 3)
        for px in range(w):
            fx = (px + 0.5) / sx - 0.5; i0 = max(0, min(lw - 2, int(fx))); tx = min(1, max(0, fx - i0))
            a, b, c, d = low[j0][i0], low[j0][i0 + 1], low[j0 + 1][i0], low[j0 + 1][i0 + 1]
            s = star.get((px, py), 0)
            for ch in range(3):
                v = (a[ch] * (1 - tx) + b[ch] * tx) * (1 - ty) + (c[ch] * (1 - tx) + d[ch] * tx) * ty + s
                v = 1 - math.exp(-v * 1.3)                      # soft highlight roll-off
                v = 255 * v + rnd.random() - 0.5                 # dither against banding
                row[px * 3 + ch] = max(0, min(255, int(v + 0.5)))
        rows.append(row)
    return rows

here = os.path.dirname(os.path.abspath(__file__))
pkg = os.path.join(here, '..', 'pkg')
png(os.path.join(pkg, 'assets', 'wallpaper.png'), 1920, 1080, render(1920, 1080, 4, 4, 260, 1))
png(os.path.join(pkg, 'icon.png'), 130, 130, render(130, 130, 2, 2, 0, 2))
print('wrote pkg/assets/wallpaper.png and pkg/icon.png')
