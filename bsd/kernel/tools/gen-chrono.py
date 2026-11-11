#!/usr/bin/env python3
"""Compile Chrono theme files (chrono/themes/*.theme, from github.com/rxvy-dev/chrono) into
include/chrono_themes.h for the kernel's Chrono desktop.

Defaults and derived colours follow Chrono's own loader (common/theme.c): unset window colours
follow bg/fg, unset title_*2 follow title_*, everything else starts from the Midnight defaults.

  tools/gen-chrono.py [THEME_DIR] [OUT.h]
  tools/gen-chrono.py --check     exit 1 if the committed header is out of date
"""
import colorsys
import configparser
import glob
import os
import sys

KDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THEMES = os.path.join(KDIR, "chrono", "themes")
OUT = os.path.join(KDIR, "include", "chrono_themes.h")

DECOR = ["flat", "bevel", "platinum", "pixel", "tab", "motif", "cube", "meadow", "pinstripe", "glass", "neon"]
SHAPES = ["flat", "bevel", "circle", "gel", "square", "roundrect"]
BUTTONS = {"close": 1, "maximize": 2, "minimize": 3, "menu": 4}
PANEL = ["flat", "bevel", "floating", "glass", "pixel", "gloss", "meadow", "neon", "tab", "platinum"]
PATTERNS = ["solid", "gradient", "waves", "topo", "blobs", "material", "scanlines", "circuit", "dunes", "rings",
            "checker", "pinstripe", "dots", "platinum", "weave", "stars", "grid", "hills", "aurora"]

COLORS = ["bg", "fg", "accent", "accent_fg", "selection", "window_bg", "window_fg", "title_active", "title_active2",
          "title_active_fg", "title_inactive", "title_inactive2", "title_inactive_fg", "border_active",
          "border_inactive", "bevel_light", "bevel_dark", "bevel_shadow", "button_bg", "button_fg", "close_bg",
          "panel_bg", "panel_fg", "panel_border", "desktop1", "desktop2"]


def color(s):
    """#rgb, #rrggbb, #rrggbbaa → 0xAARRGGBB"""
    s = s.strip().lstrip("#")
    if len(s) == 3:
        s = "".join(c * 2 for c in s)
    if len(s) == 6:
        s += "ff"
    if len(s) != 8:
        raise ValueError(s)
    r, g, b, a = (int(s[i:i + 2], 16) for i in (0, 2, 4, 6))
    return (a << 24) | (r << 16) | (g << 8) | b


def defaults():
    c = {k: color(v) for k, v in {
        "bg": "#1e2128", "fg": "#e6e8ee", "accent": "#5b9cff", "accent_fg": "#ffffff", "selection": "#5b9cff66",
        "title_active": "#2a2e37", "title_inactive": "#23262d", "title_inactive_fg": "#8a8f9c",
        "border_active": "#3a3f4b", "border_inactive": "#2c3038", "bevel_light": "#ffffff", "bevel_dark": "#808080",
        "bevel_shadow": "#000000", "button_bg": "#3a3f4b", "close_bg": "#e5484d", "panel_bg": "#181a20e6",
        "panel_border": "#ffffff14", "desktop1": "#141821", "desktop2": "#2b3550"}.items()}
    c["window_bg"], c["window_fg"] = c["bg"], c["fg"]
    c["title_active2"], c["title_active_fg"] = c["title_active"], c["fg"]
    c["title_inactive2"] = c["title_inactive"]
    c["button_fg"], c["panel_fg"] = c["fg"], c["fg"]
    return {"colors": c, "decor": "flat", "title_height": 30, "border_width": 1, "corner_radius": 10,
            "title_center": True, "title_bold": True, "buttons": "menu:minimize,maximize,close",
            "button_shape": "circle", "button_size": 14, "shadow": True, "shadow_size": 18, "shadow_alpha": 0.45,
            "panel_position": "bottom", "panel_height": 40, "panel_style": "flat", "launcher_label": "",
            "wallpaper": "gradient", "name": "Midnight", "era": "modern", "description": "", "variant": "dark"}


def load(path):
    ini = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=None)
    ini.read(path)
    t = defaults()
    t["id"] = os.path.splitext(os.path.basename(path))[0]
    g = lambda sec, key: ini.get(sec, key, fallback=None)
    for k in ("name", "era", "description", "variant"):
        if g("theme", k):
            t[k] = g("theme", k)
    c = t["colors"]
    wall_accent = (g("colors", "accent") or "").strip().lower() == "wallpaper"
    for k in ("bg", "fg", "accent", "accent_fg", "selection"):
        v = g("colors", k)
        if v and not v.strip().lower().startswith("wallpaper"):
            c[k] = color(v)
    c["window_bg"], c["window_fg"] = c["bg"], c["fg"]          # derive, as Chrono does
    for k in COLORS:
        if k in ("desktop1", "desktop2"):
            continue
        v = g("colors", k)
        if v and not v.strip().lower().startswith("wallpaper"):
            c[k] = color(v)
            if k == "title_active" and not g("colors", "title_active2"):
                c["title_active2"] = c[k]
            if k == "title_inactive" and not g("colors", "title_inactive2"):
                c["title_inactive2"] = c[k]
    for k in ("style", "title_height", "border_width", "corner_radius", "title_center", "title_bold", "buttons",
              "button_shape", "button_size", "shadow", "shadow_size", "shadow_alpha"):
        v = g("decor", k)
        if v is None:
            continue
        key = "decor" if k == "style" else k
        if key in ("title_center", "title_bold", "shadow"):
            t[key] = v.strip().lower() in ("true", "yes", "1", "on")
        elif key in ("decor", "buttons", "button_shape"):
            t[key] = v.strip()
        elif key == "shadow_alpha":
            t[key] = float(v)
        else:
            t[key] = int(float(v))
    for k, key in (("position", "panel_position"), ("height", "panel_height"), ("style", "panel_style"),
                   ("launcher_label", "launcher_label")):
        v = g("panel", k)
        if v is not None:
            t[key] = int(v) if key == "panel_height" else v.strip()
    if g("desktop", "wallpaper"):
        t["wallpaper"] = g("desktop", "wallpaper").strip()
    if g("desktop", "color1"):
        c["desktop1"] = color(g("desktop", "color1"))
    c["desktop2"] = color(g("desktop", "color2")) if g("desktop", "color2") else c["desktop1"] if \
        t["wallpaper"] == "solid" else c["desktop2"]
    if wall_accent:
        accent_from_wallpaper(t)
    return t


def accent_from_wallpaper(t):
    """Chrono's Material-You rule (theme.c, chrono_theme_apply_accent "wallpaper")."""
    c = t["colors"]
    rgb = lambda v: (((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255)
    h1, l1, s1 = colorsys.rgb_to_hls(*rgb(c["desktop1"]))
    h2, l2, s2 = colorsys.rgb_to_hls(*rgb(c["desktop2"]))
    h, sat = (h2 if s2 > s1 else h1), max(s1, s2, 0.45)
    dark = t.get("variant", "dark") == "dark"
    r, g_, b = colorsys.hls_to_rgb(h, 0.68 if dark else 0.42, min(sat, 0.85))
    to = lambda x: max(0, min(255, round(x * 255)))
    old = c["accent"]
    c["accent"] = 0xff000000 | to(r) << 16 | to(g_) << 8 | to(b)
    lum = 0.2126 * r + 0.7152 * g_ + 0.0722 * b
    c["accent_fg"] = 0xff000000 if lum > 0.55 else 0xffffffff
    c["selection"] = (0x66 << 24) | (c["accent"] & 0xffffff)
    if abs((c["border_active"] & 0xffffff) - (old & 0xffffff)) < 0x030303:
        c["border_active"] = (c["border_active"] & 0xff000000) | (c["accent"] & 0xffffff)


def buttons(spec):
    left, _, right = spec.partition(":")
    f = lambda s: [BUTTONS[b.strip()] for b in s.split(",") if b.strip() in BUTTONS][:4]
    return f(left), f(right)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(themes):
    rows = []
    for t in themes:
        bl, br = buttons(t["buttons"])
        wp = t["wallpaper"]
        pat = wp[8:] if wp.startswith("pattern:") else wp
        pat_i = PATTERNS.index(pat) if pat in PATTERNS else 1
        c = t["colors"]
        cols = ", ".join(f"0x{c[k]:08x}" for k in COLORS)
        rows.append(
            f"    {{ {cstr(t['id'])}, {cstr(t['name'])}, {cstr(t['description'][:120])}, {int(t['era'] == 'retro')},\n"
            f"      {{ {cols} }},\n"
            f"      {DECOR.index(t['decor']) if t['decor'] in DECOR else 0}, {t['title_height']}, {t['border_width']}, "
            f"{t['corner_radius']}, {int(t['title_center'])}, {int(t['title_bold'])}, "
            f"{{ {', '.join(map(str, bl + [0] * (4 - len(bl))))} }}, {{ {', '.join(map(str, br + [0] * (4 - len(br))))} }}, "
            f"{SHAPES.index(t['button_shape']) if t['button_shape'] in SHAPES else 2}, {t['button_size']}, "
            f"{int(t['shadow'])}, {t['shadow_size']}, {int(round(t['shadow_alpha'] * 255))},\n"
            f"      {int(t['panel_position'] == 'top')}, {t['panel_height']}, "
            f"{PANEL.index(t['panel_style']) if t['panel_style'] in PANEL else 0}, {cstr(t['launcher_label'])}, "
            f"{pat_i} }},")
    enum = lambda prefix, names: ", ".join(f"{prefix}_{n.upper()}" for n in names)
    return f"""/* Generated by tools/gen-chrono.py from the chrono/themes .theme files (github.com/rxvy-dev/chrono).
 * Do not edit: change the .theme files and run `make chrono-themes`. */
#ifndef CHRONO_THEMES_H
#define CHRONO_THEMES_H
#include <stdint.h>

enum {{ {enum('DECOR', DECOR)} }};
enum {{ {enum('BSHAPE', SHAPES)} }};
enum {{ BTN_NONE, BTN_CLOSE, BTN_MAXIMIZE, BTN_MINIMIZE, BTN_MENU }};
enum {{ {enum('PANEL', PANEL)} }};
enum {{ {enum('WP', PATTERNS)} }};
enum {{ {', '.join('C_' + k.upper() for k in COLORS)}, C_COUNT }};

struct chrono_theme {{
    const char *id, *name, *description;
    int retro;
    uint32_t c[C_COUNT];          /* 0xAARRGGBB, indexed by C_* */
    int decor, title_height, border_width, corner_radius, title_center, title_bold;
    int buttons_left[4], buttons_right[4];
    int button_shape, button_size, shadow, shadow_size, shadow_alpha;
    int panel_top, panel_height, panel_style;
    const char *launcher_label;
    int wallpaper;
}};

static const struct chrono_theme chrono_themes[] = {{
{chr(10).join(rows)}
}};

#define CHRONO_THEME_COUNT {len(themes)}
#define CHRONO_THEME_DEFAULT {next(i for i, t in enumerate(themes) if t['id'] == 'midnight')}   /* midnight */

#endif
"""


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if a != "--check"]
    src = args[0] if args else THEMES
    out = args[1] if len(args) > 1 else OUT
    files = sorted(glob.glob(os.path.join(src, "*.theme")))
    themes = [load(f) for f in files]
    # Chrono's own order: modern first, then retro, each alphabetically; Midnight leads
    themes.sort(key=lambda t: (t["era"] == "retro", t["id"] != "midnight", t["name"].lower()))
    text = render(themes)
    if check:
        if (open(out).read() if os.path.exists(out) else "") != text:
            print(f"{out} is out of date: run `make chrono-themes`", file=sys.stderr)
            return 1
        print(f"chrono_themes.h matches {len(themes)} Chrono themes")
        return 0
    open(out, "w").write(text)
    print(f"{len(themes)} Chrono themes → {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
