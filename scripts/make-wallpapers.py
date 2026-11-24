#!/usr/bin/env python3
"""Draws the wallpapers of the Mac-style and Windows-style rices (original artwork, made in code).

  python3 scripts/make-wallpapers.py [OUTDIR]   (default: packages/auxo-tools/files/share/wallpapers)

auxo-lagoon.jpg  bright sky with glassy ribbons (Mac-style, Liquid Glass)
auxo-bloom.jpg   soft folded petals in blue-grey (Windows-style)
Needs numpy and Pillow.
"""
import os
import sys

import numpy as np
from PIL import Image

W, H = 2560, 1440
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "packages/auxo-tools/files/share/wallpapers")
rng = np.random.default_rng(7)
y, x = np.mgrid[0:H, 0:W].astype(np.float32)
u, v = x / W, y / H


def mix(a, b, t):
    t = np.clip(t, 0, 1)[..., None]
    return a * (1 - t) + b * t


def col(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], np.float32)


def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def save(img, name):
    img = img + rng.normal(0, 1.2, img.shape)           # a little grain: no colour banding
    # JPEG: the grain that prevents banding makes PNGs ten times bigger
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(OUT, name), quality=93, optimize=True,
                                                                  subsampling=0)
    print("wrote", name)


def lagoon():
    # sky: deep blue at the top, pale near the horizon, warm glow low on the right
    img = mix(col("#3d7be0")[None, None], col("#cfe8ff")[None, None], smooth(v * 1.15))
    glow = np.exp(-(((u - 0.72) / 0.28) ** 2 + ((v - 0.78) / 0.22) ** 2))
    img = mix(img, col("#fff1e0")[None, None], glow * 0.55)
    # glassy ribbons: soft bands with a bright edge, each tinted differently
    ribbons = [(0.58, 0.10, 2.1, 0.0, "#7fd3ff", 0.55), (0.66, 0.085, 1.6, 1.7, "#6aa8ff", 0.5),
               (0.75, 0.075, 2.6, 3.1, "#9b8cff", 0.42), (0.84, 0.07, 1.9, 0.8, "#5fe0d0", 0.5),
               (0.93, 0.065, 2.3, 2.4, "#4f8df0", 0.6)]
    for center, thick, freq, phase, tint, alpha in ribbons:
        mid = center + 0.05 * np.sin(u * np.pi * freq + phase) + 0.02 * np.sin(u * np.pi * freq * 2.7 + phase * 1.3)
        d = (v - mid) / thick
        body = np.exp(-d ** 2 * 1.6) * (d > -1.2)
        edge = np.exp(-((d + 0.75) / 0.12) ** 2)           # the bright rim on top of each ribbon
        img = mix(img, col(tint)[None, None], body * alpha)
        img = mix(img, col("#ffffff")[None, None], edge * 0.55)
    # soft vignette
    vig = 1 - 0.12 * (((u - 0.5) * 1.4) ** 2 + ((v - 0.45) * 1.2) ** 2)
    save(img * vig[..., None], "auxo-lagoon.jpg")


def bloom():
    img = mix(col("#e4e9ef")[None, None], col("#a9b8c8")[None, None], smooth(v * 0.9 + u * 0.15))
    cx, cy = 0.5, 0.5
    dx, dy = (u - cx) * (W / H), (v - cy)
    r = np.sqrt(dx ** 2 + dy ** 2)
    a = np.arctan2(dy, dx)
    # two rings of folded petals, back to front: each petal is a teardrop in polar space with a
    # crisp anti-aliased edge, shaded from a darker crease on one side to a light middle
    rings = [(5, 0.44, 0.25, "#6f8ca9", "#dfe8f1"), (5, 0.32, 0.25 + np.pi / 5, "#4f7196", "#cfdceb"),
             (5, 0.2, 0.25, "#3b5d84", "#b9cde2")]
    for n, size, offset, dark, light in rings:
        for k in range(n):
            ang = offset + k * 2 * np.pi / n
            da = np.angle(np.exp(1j * (a - ang)))                   # angle from the petal's axis
            reach = size * np.clip(np.cos(da * 1.35), 0, 1) ** 0.75  # teardrop outline
            inside = np.clip((reach - r) / 0.0035 + 0.5, 0, 1)       # 1 inside, soft 1-px edge
            across = np.clip(da / 1.1 + 0.5, 0, 1)                   # 0 = crease side, 1 = lit side
            along = np.clip(r / max(size, 1e-3), 0, 1)
            shade = mix(col(dark)[None, None], col(light)[None, None], smooth(across) * 0.85 + along * 0.15)
            rim = np.exp(-((reach - r) / 0.006) ** 2) * (r < reach + 0.01)
            shade = mix(shade, col("#ffffff")[None, None], rim * 0.35)
            img = mix(img, shade, inside)
    core = np.exp(-(r / 0.05) ** 2)
    img = mix(img, col("#2c4a6e")[None, None], core * 0.6)
    save(img, "auxo-bloom.jpg")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    lagoon()
    bloom()
