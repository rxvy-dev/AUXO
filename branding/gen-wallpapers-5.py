#!/usr/bin/env python3
"""Original 2560x1440 wallpapers for the AUXO 5.0 Hyprland styles (Neon, Zen, Candy).
Everything is drawn from code here: no outside images.

  python3 branding/gen-wallpapers-5.py [OUTDIR]   (default: the auxo-tools wallpaper folder)
"""
import math
import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 2560, 1440
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "../packages/auxo-tools/files/share/wallpapers")


def hexrgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], dtype=np.float32)


def vgradient(stops, h=H, w=W):
    """stops: [(position 0..1, '#rrggbb')] → float image (h, w, 3)."""
    y = np.linspace(0, 1, h, dtype=np.float32)
    out = np.zeros((h, 3), np.float32)
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        m = (y >= p0) & (y <= p1)
        t = ((y[m] - p0) / max(1e-6, p1 - p0))[:, None]
        out[m] = hexrgb(c0) * (1 - t) + hexrgb(c1) * t
    return np.repeat(out[:, None, :], w, axis=1)


def to_img(a):
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))


def grain(img, amount=4, seed=1):
    rng = np.random.default_rng(seed)
    a = np.asarray(img).astype(np.float32)
    a += rng.normal(0, amount, a.shape[:2])[:, :, None]
    return to_img(a)


# ── Neon: a synthwave horizon ─────────────────────────────────────────
def neon():
    horizon = int(H * 0.62)
    sky = vgradient([(0, "#05050d"), (0.45, "#1a0830"), (0.62, "#5a0f5c"), (0.6201, "#07060f"), (1, "#0b0718")])
    img = to_img(sky)
    rng = random.Random(7)
    d = ImageDraw.Draw(img)
    for _ in range(420):                                   # stars
        x, y = rng.randrange(W), rng.randrange(int(horizon * 0.8))
        r = rng.choice((1, 1, 1, 2))
        v = rng.randrange(120, 255)
        d.ellipse((x, y, x + r, y + r), fill=(v, v, min(255, v + 30)))
    # the sun: pink-to-amber, with gaps cut through its lower half
    sun = Image.new("L", (W, H), 0)
    sd = ImageDraw.Draw(sun)
    cx, cy, R = W // 2, horizon - 150, 330
    sd.ellipse((cx - R, cy - R, cx + R, cy + R), fill=255)
    for i in range(8):                                     # the gaps get wider towards the horizon
        y0 = cy - 30 + i * 36
        sd.rectangle((cx - R - 2, y0, cx + R + 2, y0 + 3 + i * 3), fill=0)
    sd.rectangle((0, horizon, W, H), fill=0)
    sun_col = vgradient([(0, "#ffd23f"), ((cy - R) / H, "#ffd23f"), (cy / H, "#ff5fa2"), (horizon / H, "#ff2bd6"),
                         (1, "#ff2bd6")])
    glow = sun.filter(ImageFilter.GaussianBlur(60))
    base = np.asarray(img).astype(np.float32)
    g = np.asarray(glow).astype(np.float32)[:, :, None] / 255
    base += g * np.array([255, 43, 214], np.float32) * 0.55
    m = np.asarray(sun).astype(np.float32)[:, :, None] / 255
    base = base * (1 - m) + sun_col * m
    img = to_img(base)
    # mountains: two dark ridges with a cyan rim
    d = ImageDraw.Draw(img)
    for layer, (col, rim, amp, seed) in enumerate((("#12072a", (34, 230, 255), 170, 3), ("#0a0518", (255, 43, 214), 110, 9))):
        rr = random.Random(seed)
        pts, x = [], 0
        while x <= W + 80:
            dist = abs(x - W / 2) / (W / 2)
            hgt = amp * (0.25 + dist ** 1.3) * rr.uniform(0.55, 1.0)
            pts.append((x, horizon - hgt))
            x += rr.randrange(70, 160)
        d.polygon(pts + [(W + 100, horizon), (-100, horizon)], fill=col)
        d.line(pts, fill=rim, width=2 + layer)
    # the grid floor, in perspective
    floor = Image.new("RGB", (W, H), (0, 0, 0))
    fd = ImageDraw.Draw(floor)
    vx = W / 2
    for i in range(-40, 41):
        x_far = vx + i * 22
        x_near = vx + i * 260
        fd.line((x_far, horizon, x_near, H), fill=(34, 230, 255), width=3)
    for k in range(1, 26):
        t = (k / 25) ** 2.2
        y = horizon + t * (H - horizon)
        fd.line((0, y, W, y), fill=(34, 230, 255), width=max(1, int(1 + 3 * t)))
    fade = vgradient([(0, "#000000"), (horizon / H, "#000000"), (horizon / H + 0.02, "#303030"), (1, "#ffffff")])[:, :, 0:1] / 255
    f = np.asarray(floor).astype(np.float32)
    glowf = np.asarray(floor.filter(ImageFilter.GaussianBlur(6))).astype(np.float32)
    base = np.asarray(img).astype(np.float32)
    base += (f * 0.85 + glowf * 1.4) * fade
    # a pink haze where the floor meets the sky
    yy = np.arange(H, dtype=np.float32)[:, None, None]
    haze = np.exp(-((yy - horizon) / 26) ** 2)
    base += haze * np.array([255, 43, 214], np.float32) * 0.35
    return grain(to_img(base), 3, 11)


# ── Zen: mountains in mist ────────────────────────────────────────────
def zen():
    sky = vgradient([(0, "#f3eee4"), (0.55, "#e9e3d6"), (1, "#d7dccd")])
    img = to_img(sky)
    base = np.asarray(img).astype(np.float32)
    # a pale sun
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    cx, cy, R = W * 0.68, H * 0.30, 120
    dist = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    disc = np.clip((R - dist) / 2.0, 0, 1)[:, :, None]
    halo = np.exp(-(dist / 420) ** 2)[:, :, None]
    base = base * (1 - disc * 0.55) + hexrgb("#e8b9a0") * disc * 0.55
    base += halo * 10
    img = to_img(base)
    layers = [("#c7cfbf", 0.50, 70, 2), ("#aab8a5", 0.60, 110, 5), ("#8a9c88", 0.70, 140, 8),
              ("#6b7f6c", 0.80, 120, 13), ("#4c5d4f", 0.90, 90, 21)]
    for col, ybase, amp, seed in layers:
        rng = np.random.default_rng(seed)
        x = np.arange(W + 1, dtype=np.float32)
        y = np.zeros_like(x)
        for octave in range(5):                      # soft ridgelines from a few sine waves
            f = (octave + 1) * rng.uniform(0.6, 1.4) / W * 2 * math.pi * 1.6
            y += np.sin(x * f + rng.uniform(0, 6.28)) * amp / (octave + 1) ** 1.3
        y = ybase * H - np.abs(y)
        mask = Image.new("L", (W, H), 0)
        ImageDraw.Draw(mask).polygon([(i, float(y[i])) for i in range(0, W + 1, 4)] + [(W, H), (0, H)], fill=255)
        mask = mask.filter(ImageFilter.GaussianBlur(1.2))
        m = np.asarray(mask).astype(np.float32)[:, :, None] / 255
        # mist: each ridge fades into the haze below it
        mist = np.clip((yy - (ybase * H - amp * 0.4)) / (H * 0.16), 0, 1)[:, :, None]
        colour = hexrgb(col) * (1 - mist * 0.45) + hexrgb("#e9e3d6") * mist * 0.45
        base = np.asarray(img).astype(np.float32)
        img = to_img(base * (1 - m) + colour * m)
    d = ImageDraw.Draw(img)
    rng = random.Random(4)
    for i in range(3):                                # three birds, far away
        bx, by = W * 0.30 + i * 46 + rng.randrange(-10, 10), H * 0.24 + rng.randrange(-20, 20)
        d.arc((bx - 14, by - 6, bx, by + 8), 200, 340, fill=(80, 86, 78), width=3)
        d.arc((bx, by - 6, bx + 14, by + 8), 200, 340, fill=(80, 86, 78), width=3)
    return grain(img, 2.5, 5)


# ── Candy: pastel clouds and glossy shapes ────────────────────────────
def candy():
    base = vgradient([(0, "#ffe3f0"), (0.5, "#efe3ff"), (1, "#d9f7ee")])
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    blobs = [(0.15, 0.25, 700, "#ffb3d6"), (0.85, 0.20, 650, "#c9b6ff"), (0.55, 0.85, 800, "#a8ecd6"),
             (0.05, 0.90, 520, "#ffd3b3"), (0.95, 0.80, 560, "#ffc2e2"), (0.45, 0.35, 500, "#d9ccff")]
    for bx, by, r, col in blobs:
        w = np.exp(-(((xx - bx * W) ** 2 + (yy - by * H) ** 2) / (r * r)))[:, :, None]
        base = base * (1 - w * 0.75) + hexrgb(col) * w * 0.75
    img = to_img(base)
    # soft clouds
    clouds = Image.new("L", (W, H), 0)
    cd = ImageDraw.Draw(clouds)
    rng = random.Random(12)
    for cxf, cyf, scale in ((0.22, 0.62, 1.0), (0.74, 0.55, 1.25), (0.50, 0.18, 0.8)):
        cx, cy = cxf * W, cyf * H
        for _ in range(9):
            r = rng.uniform(70, 150) * scale
            ox, oy = rng.uniform(-220, 220) * scale, rng.uniform(-40, 40) * scale
            cd.ellipse((cx + ox - r, cy + oy - r, cx + ox + r, cy + oy + r * 0.8), fill=255)
    clouds = clouds.filter(ImageFilter.GaussianBlur(18))
    m = np.asarray(clouds).astype(np.float32)[:, :, None] / 255
    a = np.asarray(img).astype(np.float32)
    a = a * (1 - m * 0.65) + 255 * m * 0.65
    # glossy spheres
    for sx, sy, r, col in ((0.33, 0.40, 90, "#ff8fc6"), (0.66, 0.30, 60, "#a98bff"), (0.80, 0.68, 120, "#6fdcbf"),
                           (0.12, 0.45, 50, "#ffc38f"), (0.58, 0.72, 40, "#ff8fc6")):
        cx, cy = sx * W, sy * H
        dist = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
        inside = np.clip((r - dist) / 1.5, 0, 1)[:, :, None]
        shade = np.clip(1 - ((xx - (cx - r * 0.35)) ** 2 + (yy - (cy - r * 0.35)) ** 2) / (r * r * 2.2), 0, 1)[:, :, None]
        colr = hexrgb(col) * (0.82 + 0.18 * shade) + 255 * (shade ** 6) * 0.55
        shadow = np.exp(-(((xx - cx) ** 2) / (r * r * 0.9) + ((yy - (cy + r * 1.25)) ** 2) / (r * r * 0.06))) * 0.10
        a = a * (1 - shadow[:, :, None]) + hexrgb("#a98bff") * 0.6 * shadow[:, :, None]
        a = a * (1 - inside) + colr * inside
    img = to_img(a)
    d = ImageDraw.Draw(img)
    for _ in range(70):                                 # sparkles
        x, y = rng.randrange(W), rng.randrange(H)
        s = rng.randrange(4, 11)
        d.polygon([(x, y - s), (x + s * 0.28, y - s * 0.28), (x + s, y), (x + s * 0.28, y + s * 0.28), (x, y + s),
                   (x - s * 0.28, y + s * 0.28), (x - s, y), (x - s * 0.28, y - s * 0.28)], fill=(255, 255, 255))
    return grain(img, 2, 8)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, fn in (("neon", neon), ("zen", zen), ("candy", candy)):
        im = fn()
        assert im.size == (W, H)
        p = os.path.join(OUT, f"auxo-{name}.jpg")
        im.save(p, quality=90, optimize=True, progressive=True)
        print(p, os.path.getsize(p) // 1024, "KB")
