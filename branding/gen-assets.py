#!/usr/bin/env python3
"""Generate every Auxo branding asset from code (logo, wallpapers, boot splash,
installer images, accent swatches).  Requires: rsvg-convert, python3-pil.

    python3 branding/gen-assets.py            # writes into branding/out and copies into the profile
"""
import os
import random
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "branding", "out")
sys.path.insert(0, os.path.join(ROOT, "packages", "auxo-tools", "files", "lib"))
from auxo_accents import ACCENTS  # noqa: E402  single source of truth for accent colors

BG = "#0b0b10"
FONT = "Inter, 'Noto Sans', sans-serif"


def logo_mark(size=128, color="url(#g)", x=0, y=0, stroke=13):
    s = size / 128.0
    return f'''<g transform="translate({x},{y}) scale({s})" fill="none" stroke="{color}" stroke-linecap="round" stroke-linejoin="round">
  <path d="M16 112 L64 22 L112 112" stroke-width="{stroke}"/>
  <path d="M40 96 L56 78 L68 90 L86 68" stroke-width="{stroke*0.62:.1f}"/>
  <circle cx="64" cy="9" r="6.5" fill="{color}" stroke="none"/>
</g>'''


def grad(a, b, gid="g"):
    return f'<linearGradient id="{gid}" x1="0" y1="1" x2="1" y2="0"><stop offset="0" stop-color="{a}"/><stop offset="1" stop-color="{b}"/></linearGradient>'


def svg_logo(accent, accent2):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="512" height="512">
<defs>{grad(accent, accent2)}</defs>{logo_mark()}</svg>'''


def svg_wordmark(accent, accent2, w=720, h=200):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">
<defs>{grad(accent, accent2)}</defs>
{logo_mark(160, x=20, y=20)}
<text x="210" y="138" font-family="{FONT}" font-weight="800" font-size="118" letter-spacing="-4" fill="#f4f4f8">auxo</text>
</svg>'''


def ridge(rng, w, h, base, amp, n=18):
    pts = [(0, h)]
    step = w / n
    y = base
    for i in range(n + 1):
        y = base - rng.uniform(0, amp) if i % 2 else base - rng.uniform(0, amp * 0.35)
        pts.append((i * step, y))
    pts.append((w, h))
    return "M" + " L".join(f"{px:.0f} {py:.0f}" for px, py in pts) + " Z"


def svg_wallpaper(accent, accent2, w=2560, h=1440, seed=7):
    rng = random.Random(seed)
    layers = []
    for i, (base, amp, op) in enumerate([(0.62, 0.22, 0.10), (0.72, 0.20, 0.16), (0.82, 0.16, 0.26), (0.92, 0.12, 0.55)]):
        layers.append(f'<path d="{ridge(rng, w, h, h*base, h*amp, 14 + i*4)}" fill="url(#m{i})" opacity="{op}"/>')
    mdefs = "".join(
        f'<linearGradient id="m{i}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{accent}"/><stop offset="1" stop-color="{BG}"/></linearGradient>'
        for i in range(4))
    stars = "".join(
        f'<circle cx="{rng.uniform(0,w):.0f}" cy="{rng.uniform(0,h*0.55):.0f}" r="{rng.uniform(0.6,2.2):.1f}" fill="#fff" opacity="{rng.uniform(0.15,0.7):.2f}"/>'
        for _ in range(220))
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">
<defs>{grad(accent, accent2)}{mdefs}
<radialGradient id="glow" cx="0.5" cy="0.42" r="0.55"><stop offset="0" stop-color="{accent}" stop-opacity="0.28"/><stop offset="1" stop-color="{accent}" stop-opacity="0"/></radialGradient></defs>
<rect width="{w}" height="{h}" fill="{BG}"/>
<rect width="{w}" height="{h}" fill="url(#glow)"/>
{stars}
{"".join(layers)}
{logo_mark(h*0.16, x=w/2 - h*0.08, y=h*0.30, stroke=11)}
</svg>'''


def svg_splash(accent, accent2, w=640, h=480):
    """syslinux background: menu text is drawn on top, keep the middle dark."""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">
<defs>{grad(accent, accent2)}</defs>
<rect width="{w}" height="{h}" fill="{BG}"/>
<rect y="{h-6}" width="{w}" height="6" fill="url(#g)"/>
{logo_mark(70, x=w/2-35, y=34, stroke=12)}
</svg>'''


def svg_swatch(name, accent, accent2, w=640, h=360):
    """Accent preview used by the installer's accent chooser."""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">
<defs>{grad(accent, accent2)}</defs>
<rect width="{w}" height="{h}" rx="18" fill="#111118"/>
<rect x="24" y="24" width="{w-48}" height="36" rx="8" fill="#1b1b25"/>
<circle cx="46" cy="42" r="7" fill="{accent}"/><circle cx="68" cy="42" r="7" fill="#3a3a48"/><circle cx="90" cy="42" r="7" fill="#3a3a48"/>
<text x="{w-40}" y="48" text-anchor="end" font-family="JetBrains Mono, monospace" font-size="16" fill="#9a9aae">auxo-fetch</text>
{logo_mark(120, x=48, y=96, stroke=13)}
<g font-family="JetBrains Mono, monospace" font-size="19">
<text x="210" y="122" fill="{accent}" font-weight="700">you@auxo</text>
<text x="210" y="152" fill="#9a9aae">os     <tspan fill="#e8e8f0">Auxo Linux</tspan></text>
<text x="210" y="180" fill="#9a9aae">accent <tspan fill="{accent}">{name}</tspan></text>
<text x="210" y="208" fill="#9a9aae">shell  <tspan fill="#e8e8f0">zsh</tspan></text>
</g>
<rect x="48" y="262" width="{w-96}" height="14" rx="7" fill="#232330"/>
<rect x="48" y="262" width="{(w-96)*0.64:.0f}" height="14" rx="7" fill="url(#g)"/>
<g>{"".join(f'<rect x="{48+i*44}" y="300" width="36" height="28" rx="6" fill="{c}"/>' for i, c in enumerate([accent, accent2, "#e8e8f0", "#6b6b80", "#2a2a38"]))}</g>
</svg>'''


def svg_desktop(label, sub, accent, accent2, kind, w=640, h=360):
    """Stylised preview card for each desktop/WM choice."""
    rng = random.Random(label)
    body = ""
    if kind == "tiling":
        body = f'''<rect x="16" y="44" width="{w/2-24}" height="{h-60}" rx="10" fill="#161620" stroke="{accent}" stroke-width="2"/>
<rect x="{w/2}" y="44" width="{w/2-16}" height="{(h-60)/2-6}" rx="10" fill="#161620" stroke="#34344a" stroke-width="2"/>
<rect x="{w/2}" y="{44+(h-60)/2+6}" width="{w/2-16}" height="{(h-60)/2-6}" rx="10" fill="#161620" stroke="#34344a" stroke-width="2"/>
<rect x="16" y="12" width="{w-32}" height="24" rx="8" fill="#161620"/>
{"".join(f'<rect x="{26+i*22}" y="18" width="14" height="12" rx="3" fill="{accent if i==0 else "#34344a"}"/>' for i in range(5))}
{"".join(f'<rect x="36" y="{70+i*22}" width="{rng.randint(80, 240)}" height="9" rx="4" fill="{accent if i%4==0 else "#3a3a50"}"/>' for i in range(10))}'''
    elif kind == "none":
        body = f'''<rect x="16" y="16" width="{w-32}" height="{h-32}" rx="10" fill="#0f0f15" stroke="#2a2a3a"/>
<text x="40" y="70" font-family="JetBrains Mono, monospace" font-size="20" fill="{accent}">auxo login: _</text>
<text x="40" y="104" font-family="JetBrains Mono, monospace" font-size="16" fill="#6b6b80">Minimal install. Add any desktop later with auxo-tweak.</text>'''
    else:
        dock_y = h - 44 if kind == "dock" else 8
        body = f'''<rect x="80" y="70" width="{w*0.5}" height="{h*0.5}" rx="12" fill="#171722" stroke="#2f2f42"/>
<rect x="80" y="70" width="{w*0.5}" height="28" rx="12" fill="#1f1f2e"/>
<circle cx="100" cy="84" r="5" fill="{accent}"/>
<rect x="{w*0.38}" y="{h*0.36}" width="{w*0.46}" height="{h*0.42}" rx="12" fill="#1a1a26" stroke="{accent}" stroke-opacity="0.7"/>
<rect x="{w/2-150 if kind=="dock" else 0}" y="{dock_y}" width="{300 if kind=="dock" else w}" height="36" rx="{12 if kind=="dock" else 0}" fill="#12121a" opacity="0.95"/>
{"".join(f'<rect x="{(w/2-130 if kind=="dock" else 12)+i*40}" y="{dock_y+7}" width="24" height="22" rx="6" fill="{accent if i==0 else "#3a3a50"}"/>' for i in range(7))}'''
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">
<defs>{grad(accent, accent2)}
<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#15121f"/><stop offset="1" stop-color="{BG}"/></linearGradient></defs>
<rect width="{w}" height="{h}" rx="16" fill="url(#bg)"/>
{body}
<text x="{w-24}" y="{h-58 if kind=="dock" else h-40}" text-anchor="end" font-family="{FONT}" font-weight="800" font-size="30" fill="#f4f4f8">{label}</text>
<text x="{w-24}" y="{h-36 if kind=="dock" else h-18}" text-anchor="end" font-family="{FONT}" font-size="14" fill="#9a9aae">{sub}</text>
</svg>'''


DESKTOPS = [
    ("plasma", "KDE Plasma", "the live desktop — works offline", "dock"),
    ("gnome", "GNOME", "clean, gesture-driven", "dock"),
    ("xfce", "Xfce", "light and classic", "panel"),
    ("cinnamon", "Cinnamon", "familiar layout", "panel"),
    ("hyprland", "Hyprland", "Auxo rice · animated tiling", "tiling"),
    ("sway", "Sway", "Auxo rice · i3 on Wayland", "tiling"),
    ("i3", "i3", "Auxo rice · X11 tiling", "tiling"),
    ("none", "No desktop", "TTY only", "none"),
]


def render(svg, path, w=None, h=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".svg"
    with open(tmp, "w") as f:
        f.write(svg)
    if path.endswith(".svg"):
        os.replace(tmp, path)
        return
    cmd = ["rsvg-convert", tmp, "-o", path]
    if w:
        cmd += ["-w", str(w)]
    if h:
        cmd += ["-h", str(h)]
    subprocess.run(cmd, check=True)
    os.remove(tmp)


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    va, vb = ACCENTS["violet"]
    render(svg_logo(va, vb), f"{OUT}/auxo-logo.svg")
    render(svg_logo(va, vb), f"{OUT}/auxo-logo.png", 256, 256)
    render(svg_logo(va, vb), f"{OUT}/auxo-logo-64.png", 64, 64)
    render(svg_wordmark(va, vb), f"{OUT}/auxo-wordmark.png")
    render(svg_wordmark(va, vb), f"{OUT}/auxo-wordmark.svg")
    render(svg_splash(va, vb), f"{OUT}/splash.png")
    for name, (a, b) in ACCENTS.items():
        render(svg_wallpaper(a, b), f"{OUT}/wallpapers/auxo-{name}.png")
        render(svg_swatch(name, a, b), f"{OUT}/accents/{name}.png")
    for i, (did, label, sub, kind) in enumerate(DESKTOPS):
        render(svg_desktop(label, sub, va, vb, kind), f"{OUT}/desktops/{did}.png")

    # ── copy into the auxo-tools package (the live ISO's boot menu is void-mklive's) ──
    T = os.path.join(ROOT, "packages", "auxo-tools", "files")
    os.makedirs(f"{T}/share/wallpapers", exist_ok=True)
    for f in os.listdir(f"{OUT}/wallpapers"):
        shutil.copy(f"{OUT}/wallpapers/{f}", f"{T}/share/wallpapers/{f}")
    print("assets written to", OUT)


if __name__ == "__main__":
    main()
