#!/usr/bin/env python3
"""The AUXO 5.0 app icons: one family, a logo for every Auxo app.

Every icon is a rounded tile in the app's own two colours, a soft light from the top, a white
symbol, and the Auxo peak in the corner. Plain SVG, drawn here from code (no outside artwork).

  python3 branding/gen-app-icons.py [OUTDIR] [--preview FILE.png]
  default OUTDIR: packages/auxo-tools/files/share/icons/hicolor/scalable/apps
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = next((a for a in sys.argv[1:] if not a.startswith("--") and not a.endswith(".png")),
           os.path.join(HERE, "../packages/auxo-tools/files/share/icons/hicolor/scalable/apps"))

# white symbols on a 128×128 tile (strokes unless said otherwise)
def stroke(w=8):
    return f'fill="none" stroke="#fff" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"'


S = stroke()
F = 'fill="#fff"'
GLYPHS = {
    # gear: a ring with eight teeth
    "settings": f'<g {F}>' + "".join(
        f'<rect x="58" y="22" width="12" height="18" rx="3" transform="rotate({a} 64 64)"/>' for a in range(0, 360, 45))
    + f'</g><circle cx="64" cy="64" r="26" {stroke(12)}/><circle cx="64" cy="64" r="8" {F}/>',
    # shopping bag with a handle
    "store": f'<path d="M34 50 H94 L89 100 Q88 106 82 106 H46 Q40 106 39 100 Z" {F}/>'
             f'<path d="M50 52 V42 Q50 28 64 28 Q78 28 78 42 V52" {S}/>'
             '<circle cx="50" cy="60" r="4" fill="#0004"/><circle cx="78" cy="60" r="4" fill="#0004"/>',
    # a pulse line through a frame
    "monitor": f'<rect x="24" y="30" width="80" height="62" rx="10" {stroke(7)}/>'
               f'<path d="M32 66 H48 L56 48 L66 80 L74 60 H96" {stroke(7)}/>'
               f'<path d="M50 104 H78" {stroke(7)}/>',
    # a terminal prompt
    "tweak": f'<rect x="22" y="28" width="84" height="72" rx="12" {stroke(7)}/>'
             f'<path d="M40 54 L54 66 L40 78" {S}/><path d="M62 80 H86" {S}/>',
    # two arrows in a circle
    "update": f'<path d="M92 56 A30 30 0 0 0 38 46" {S}/><path d="M36 72 A30 30 0 0 0 90 82" {S}/>'
              f'<path d="M30 34 L36 48 L50 42" {stroke(7)}/><path d="M98 94 L92 80 L78 86" {stroke(7)}/>',
    # the Auxo peak, big
    "welcome": f'<path d="M24 100 L64 30 L104 100" {stroke(11)}/>'
               f'<path d="M44 86 L57 72 L67 82 L82 64" {stroke(7)}/><circle cx="64" cy="20" r="6" {F}/>',
    # a disk with an arrow going in
    "installer": f'<path d="M64 20 V62" {S}/><path d="M46 46 L64 64 L82 46" {S}/>'
                 f'<rect x="24" y="74" width="80" height="30" rx="9" {stroke(7)}/><circle cx="88" cy="89" r="5" {F}/>',
    # a four-point sparkle and a small one
    "ai": f'<path d="M58 22 Q62 52 92 58 Q62 64 58 96 Q54 64 24 58 Q54 52 58 22 Z" {F}/>'
          f'<path d="M94 22 Q95 31 104 32 Q95 33 94 42 Q93 33 84 32 Q93 31 94 22 Z" {F}/>',
    # windows from above (overview)
    "overview": f'<g {F}><rect x="24" y="28" width="36" height="30" rx="6"/><rect x="68" y="28" width="36" height="30" rx="6"/>'
                '<rect x="24" y="66" width="36" height="34" rx="6"/><rect x="68" y="66" width="36" height="34" rx="6" opacity=".55"/></g>',
    # a framed mountain picture
    "wallpaper": f'<rect x="22" y="28" width="84" height="72" rx="10" {stroke(7)}/>'
                 f'<path d="M30 92 L54 62 L68 78 L78 68 L98 92 Z" {F}/><circle cx="84" cy="48" r="7" {F}/>',
    # a game controller
    "gamemode": f'<path d="M30 52 Q32 40 44 40 H84 Q96 40 98 52 L104 84 Q106 98 94 96 Q88 95 84 86 L80 78 H48 L44 86 '
                f'Q40 95 34 96 Q22 98 24 84 Z" {F}/>'
                '<path d="M44 56 V68 M38 62 H50" stroke="#0005" stroke-width="5" stroke-linecap="round"/>'
                '<circle cx="80" cy="58" r="4" fill="#0005"/><circle cx="88" cy="66" r="4" fill="#0005"/>',
    # a clipboard
    "clipboard": f'<rect x="32" y="28" width="64" height="78" rx="10" {stroke(7)}/>'
                 f'<rect x="48" y="20" width="32" height="16" rx="6" {F}/>'
                 f'<path d="M46 60 H82 M46 76 H82 M46 92 H68" {stroke(6)}/>',
    # a speaker with sound waves
    "sound": f'<path d="M26 54 H42 L62 36 V92 L42 74 H26 Z" {F}/>'
             f'<path d="M76 50 Q84 64 76 78" {stroke(7)}/><path d="M88 40 Q102 64 88 88" {stroke(7)}/>',
    # a shield with a key hole (VPN)
    "vpn": f'<path d="M64 20 L98 32 V60 Q98 92 64 108 Q30 92 30 60 V32 Z" {F}/>'
           '<circle cx="64" cy="58" r="9" fill="#0005"/><path d="M64 64 V82" stroke="#0005" stroke-width="8" stroke-linecap="round"/>',
    # a grid of apps (Start / Launchpad)
    "launcher": f'<g {F}>' + "".join(f'<rect x="{x}" y="{y}" width="22" height="22" rx="6"/>'
                                     for x in (24, 53, 82) for y in (24, 53, 82)) + '</g>',
    # widgets: a clock card and two small cards
    "widgets": f'<rect x="22" y="24" width="50" height="50" rx="12" {F}/>'
               '<path d="M47 36 V50 L56 56" stroke="#0006" stroke-width="5" stroke-linecap="round" fill="none"/>'
               f'<rect x="80" y="24" width="26" height="50" rx="10" {F} opacity=".7"/>'
               f'<rect x="22" y="82" width="84" height="22" rx="10" {F} opacity=".85"/>',
    # pinned apps: a pin
    "pins": f'<path d="M50 22 H78 L74 50 L88 64 H40 L54 50 Z" {F}/><path d="M64 64 V104" {S}/>',
}
# app → (glyph, top colour, bottom colour, .desktop name)
APPS = {
    "auxo-settings": ("settings", "#8b5cf6", "#4f46e5"),
    "auxo-store": ("store", "#38bdf8", "#2563eb"),
    "auxo-monitor": ("monitor", "#34d399", "#0f766e"),
    "auxo-tweak": ("tweak", "#475569", "#1e1b4b"),
    "auxo-update": ("update", "#fbbf24", "#ea580c"),
    "auxo-welcome": ("welcome", "#c084fc", "#db2777"),
    "auxo-installer": ("installer", "#22d3ee", "#4f46e5"),
    "auxo-ai": ("ai", "#f472b6", "#7c3aed"),
    "auxo-overview": ("overview", "#818cf8", "#3730a3"),
    "auxo-wallpaper": ("wallpaper", "#fb7185", "#f97316"),
    "auxo-gamemode": ("gamemode", "#f43f5e", "#9d174d"),
    "auxo-clipboard": ("clipboard", "#2dd4bf", "#0e7490"),
    "auxo-sound": ("sound", "#60a5fa", "#1d4ed8"),
    "auxo-vpn": ("vpn", "#4ade80", "#15803d"),
    "auxo-launcher": ("launcher", "#a78bfa", "#0ea5e9"),
    "auxo-widgets": ("widgets", "#facc15", "#d97706"),
    "auxo-pins": ("pins", "#f87171", "#b91c1c"),
}


def svg(app):
    glyph, top, bottom = APPS[app]
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128">
  <!-- {app}: AUXO 5.0 app icon (branding/gen-app-icons.py) -->
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0.35" y2="1">
      <stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/>
    </linearGradient>
    <linearGradient id="shine" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#fff" stop-opacity=".28"/><stop offset=".55" stop-color="#fff" stop-opacity="0"/>
    </linearGradient>
    <filter id="sh" x="-20%" y="-20%" width="140%" height="140%">
      <feDropShadow dx="0" dy="2.5" stdDeviation="2.5" flood-color="#000" flood-opacity=".28"/>
    </filter>
  </defs>
  <rect x="4" y="4" width="120" height="120" rx="28" fill="url(#bg)"/>
  <rect x="4" y="4" width="120" height="120" rx="28" fill="url(#shine)"/>
  <rect x="4.75" y="4.75" width="118.5" height="118.5" rx="27.3" fill="none" stroke="#fff" stroke-opacity=".22" stroke-width="1.5"/>
  <g filter="url(#sh)">{GLYPHS[glyph]}</g>
  <path d="M100 118 L109 102 L118 118" fill="none" stroke="#fff" stroke-opacity=".55" stroke-width="3"
        stroke-linecap="round" stroke-linejoin="round"/>
</svg>
'''


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for app in APPS:
        with open(os.path.join(OUT, f"{app}.svg"), "w") as f:
            f.write(svg(app))
    print(f"{len(APPS)} icons in {OUT}")
    if "--preview" in sys.argv:
        out = sys.argv[sys.argv.index("--preview") + 1]
        tmp = os.path.join(os.path.dirname(out) or ".", "_icons")
        os.makedirs(tmp, exist_ok=True)
        pngs = []
        for app in APPS:
            p = os.path.join(tmp, f"{app}.png")
            subprocess.run(["rsvg-convert", "-w", "128", "-h", "128", os.path.join(OUT, f"{app}.svg"), "-o", p], check=True)
            pngs.append(p)
        subprocess.run(["montage", *pngs, "-tile", "9x", "-geometry", "+12+12", "-background", "#1b1b24", out], check=True)
        print("preview:", out)
