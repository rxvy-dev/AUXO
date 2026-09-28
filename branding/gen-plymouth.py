#!/usr/bin/env python3
"""Generate the Auxo Plymouth boot-splash images, one set per accent.
Output: packages/auxo-tools/files/share/auxo/plymouth/accents/<accent>/*.png
Needs rsvg-convert (librsvg). Run from the repo root."""
import os, subprocess, sys, tempfile
sys.path.insert(0, "packages/auxo-tools/files/lib")
from auxo_accents import ACCENTS

OUT = "packages/auxo-tools/files/share/auxo/plymouth"
S = 1.5                     # logo scale: 128 → 192 px
def svg(w, h, body, defs=""):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}"><defs>{defs}</defs>{body}</svg>'
def render(name, text):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(text); p = f.name
    subprocess.run(["rsvg-convert", p, "-o", name], check=True); os.unlink(p)

GRAD = '<linearGradient id="g" x1="0" y1="1" x2="1" y2="0"><stop offset="0" stop-color="{a}"/><stop offset="1" stop-color="{b}"/></linearGradient>'
for acc, (a, b) in ACCENTS.items():
    d = f"{OUT}/accents/{acc}"; os.makedirs(d, exist_ok=True)
    g = GRAD.format(a=a, b=b)
    # logo without the summit dot (the dot is animated separately); 16 px padding
    render(f"{d}/logo.png", svg(224, 224,
        f'<g transform="translate(16 16) scale({S})" fill="none" stroke="url(#g)" stroke-linecap="round" stroke-linejoin="round">'
        '<path d="M16 112 L64 22 L112 112" stroke-width="12"/><path d="M40 96 L56 78 L68 90 L86 68" stroke-width="7.5"/></g>', g))
    # summit dot with soft halo
    render(f"{d}/summit.png", svg(48, 48,
        f'<circle cx="24" cy="24" r="22" fill="url(#h)"/><circle cx="24" cy="24" r="10.5" fill="{b}"/>',
        f'<radialGradient id="h"><stop offset="0" stop-color="{b}" stop-opacity=".55"/><stop offset="1" stop-color="{b}" stop-opacity="0"/></radialGradient>'))
    # climber: bright point that runs up the trail
    render(f"{d}/climber.png", svg(40, 40,
        f'<circle cx="20" cy="20" r="18" fill="url(#h)"/><circle cx="20" cy="20" r="5.5" fill="#ffffff"/>',
        f'<radialGradient id="h"><stop offset="0" stop-color="{a}" stop-opacity=".9"/><stop offset="1" stop-color="{a}" stop-opacity="0"/></radialGradient>'))
    # background glow behind the logo
    render(f"{d}/glow.png", svg(520, 520,
        '<circle cx="260" cy="260" r="260" fill="url(#r)"/>',
        f'<radialGradient id="r"><stop offset="0" stop-color="{a}" stop-opacity=".22"/><stop offset="1" stop-color="{a}" stop-opacity="0"/></radialGradient>'))
    # progress bar fill (scaled at runtime)
    render(f"{d}/bar.png", svg(240, 3, '<rect width="240" height="3" rx="1.5" fill="url(#l)"/>',
        f'<linearGradient id="l"><stop offset="0" stop-color="{a}"/><stop offset="1" stop-color="{b}"/></linearGradient>'))
os.makedirs(f"{OUT}/theme", exist_ok=True)
render(f"{OUT}/theme/wordmark.png", svg(160, 44,
    '<text x="80" y="34" text-anchor="middle" font-family="Inter, DejaVu Sans, sans-serif" font-weight="800" '
    'font-size="34" letter-spacing="-1" fill="#eeedf5">auxo</text>'))
render(f"{OUT}/theme/track.png", svg(240, 3, '<rect width="240" height="3" rx="1.5" fill="#23232e"/>'))
print("ok")
