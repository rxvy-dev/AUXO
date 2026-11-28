"""Look settings: one per-user file that drives every desktop Auxo sets up.

~/.config/auxo/look.conf holds the choices (light or dark, wallpaper, fonts, gaps, corners,
borders, blur, transparency, animations, bar position). generate() turns them into small
include files in ~/.config/auxo/ that the Auxo rices load last, so a change here overrides
the rice defaults without editing the user's own config files:

  Hyprland   ~/.config/auxo/look-hyprland.conf   (source = … at the end of hyprland.conf)
  Sway / i3  ~/.config/auxo/look-sway, look-i3     (include … at the end of the config)
  kitty      look-kitty.conf, foot look-foot.ini, alacritty look-alacritty.toml
  Waybar     look.css (fonts, corners, opacity)   Polybar look-polybar.ini
  picom      ~/.config/picom/picom.conf (corners, blur, fading, opacity) — written whole
  GTK apps   ~/.config/gtk-3.0/settings.ini, gtk-4.0/settings.ini (tiling desktops only)
  Chrono     ~/.config/chrono/themes/auxo.theme + [general]/[panel]/[desktop] in chrono.conf

GNOME, Cinnamon, Xfce and KDE Plasma get the same settings through their own tools
(gsettings, xfconf-query, kdeglobals) from auxo_theme.session_apply().
"""
import os
import re

from auxo_accents import ACCENTS, DEFAULT, hex_to_rgb
from auxo_core import R, write_file

WALLDIR = "/usr/share/wallpapers"

DEFAULTS = {
    "mode": "dark",              # dark | light
    "wallpaper": "accent",       # accent (the Auxo wallpaper in your accent) | /path/to/image
    "ui_font": "Inter",
    "mono_font": "Fira Code",
    "font_size": "10",           # points
    "gaps": "8",                 # pixels between windows (tiling desktops); the screen edge gets more
    "rounding": "12",            # window / bar corner radius in pixels
    "border": "2",               # window border width in pixels
    "blur": "on",
    "transparency": "on",        # see-through bars and terminals, slightly faded unfocused windows
    "animations": "on",
    "bar": "top",                # top | bottom
}

# what each value may be: (kind, choices-or-range)
RULES = {
    "mode": ("choice", ("dark", "light")),
    "wallpaper": ("wallpaper", None),
    "ui_font": ("font", None),
    "mono_font": ("font", None),
    "font_size": ("int", (7, 24)),
    "gaps": ("int", (0, 40)),
    "rounding": ("int", (0, 30)),
    "border": ("int", (0, 8)),
    "blur": ("choice", ("on", "off")),
    "transparency": ("choice", ("on", "off")),
    "animations": ("choice", ("on", "off")),
    "bar": ("choice", ("top", "bottom")),
}

# which desktops each setting reaches (shown by `auxo-tweak look` and the settings app)
REACH = {
    "mode": "every desktop",
    "wallpaper": "every desktop",
    "ui_font": "every desktop",
    "mono_font": "every desktop",
    "font_size": "every desktop",
    "gaps": "Hyprland, Sway, i3",
    "rounding": "Hyprland, i3, Chrono, the Hyprland/Sway/i3 bars",
    "border": "Hyprland, Sway, i3, Chrono",
    "blur": "Hyprland, i3",
    "transparency": "Hyprland, i3, terminals, bars",
    "animations": "every desktop",
    "bar": "Plasma, Xfce, Cinnamon, Chrono, Hyprland, Sway, i3",
}

LABELS = {
    "mode": "Light or dark", "wallpaper": "Wallpaper", "ui_font": "Interface font", "mono_font": "Terminal font",
    "font_size": "Font size", "gaps": "Gaps", "rounding": "Rounded corners", "border": "Window borders",
    "blur": "Blur", "transparency": "Transparency", "animations": "Animations", "bar": "Bar position",
}


class LookError(ValueError):
    pass


def path(home):
    return f"{home}/.config/auxo/look.conf"


def load(home):
    look = dict(DEFAULTS)
    try:
        for line in open(R(path(home))):
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            k, v = k.strip(), v.strip().strip('"')
            if k in DEFAULTS:
                try:
                    look[k] = validate(k, v)
                except LookError:
                    pass                  # a bad hand edit falls back to the default
    except OSError:
        pass
    return look


def save(home, look):
    lines = ["# Auxo look settings — change them with Auxo Settings or `auxo-tweak look set KEY=VALUE`"]
    lines += [f'{k}="{look[k]}"' for k in DEFAULTS]
    write_file(path(home), "\n".join(lines) + "\n")


def validate(key, value):
    if key not in RULES:
        raise LookError(f"unknown setting '{key}'. settings: {', '.join(DEFAULTS)}")
    kind, arg = RULES[key]
    value = str(value).strip()
    if kind == "choice":
        v = value.lower()
        aliases = {"true": "on", "yes": "on", "1": "on", "false": "off", "no": "off", "0": "off"}
        v = aliases.get(v, v) if arg == ("on", "off") else v
        if v not in arg:
            raise LookError(f"{key} must be one of: {', '.join(arg)}")
        return v
    if kind == "int":
        try:
            n = int(float(value))
        except ValueError:
            raise LookError(f"{key} must be a number") from None
        lo, hi = arg
        if not lo <= n <= hi:
            raise LookError(f"{key} must be between {lo} and {hi}")
        return str(n)
    if kind == "font":
        if not value or not re.fullmatch(r"[\w .+\-&']{1,64}", value):
            raise LookError(f"{key}: not a font name")
        return value
    if kind == "wallpaper":
        if value.lower() in ("accent", "default", "auxo"):
            return "accent"
        p = os.path.expanduser(value)
        if not os.path.isabs(p):
            raise LookError("wallpaper: give the full path to an image, or 'accent'")
        if not re.search(r"\.(png|jpe?g|webp|bmp)$", p, re.I):
            raise LookError("wallpaper: use a PNG, JPEG, WebP or BMP image")
        return p
    return value


# ── palettes ──────────────────────────────────────────────────────────
DARK = {"bg": "#0b0b10", "bg2": "#15151e", "fg": "#e8e8f0", "muted": "#6b6b80", "urgent": "#fb7185",
        "inactive": "#2a2a38"}
LIGHT = {"bg": "#f5f5f8", "bg2": "#e8e8ef", "fg": "#1b1b26", "muted": "#6b6b80", "urgent": "#e11d48",
         "inactive": "#c9c9d6"}

# accents tuned to stay readable on a light background (same hue, darker)
LIGHT_ACCENT = {"violet": ("#7c3aed", "#0891b2"), "cyan": ("#0891b2", "#4f46e5"),
                "emerald": ("#059669", "#65a30d"), "amber": ("#d97706", "#e11d48"),
                "rose": ("#e11d48", "#9333ea"), "blue": ("#2563eb", "#059669"),
                "mono": ("#374151", "#6b7280")}


def palette(accent, mode="dark"):
    accent = accent if accent in ACCENTS else DEFAULT
    a, b = LIGHT_ACCENT[accent] if mode == "light" else ACCENTS[accent]
    base = LIGHT if mode == "light" else DARK
    return {"accent": a, "accent2": b, **base}


def wallpaper(look, accent):
    w = look.get("wallpaper", "accent")
    if w != "accent" and os.path.exists(R(w)):
        return w
    return f"{WALLDIR}/auxo-{accent if accent in ACCENTS else DEFAULT}.png"


def on(look, key):
    return look.get(key) == "on"


# ── the include files ─────────────────────────────────────────────────
def _hypr(look):
    g = int(look["gaps"])
    t = on(look, "transparency")
    return f"""# generated by auxo-tweak look — overrides the rice defaults; change with Auxo Settings
general {{
    gaps_in = {max(0, g // 2) if g else 0}
    gaps_out = {g * 2 if g else 0}
    border_size = {look['border']}
}}
decoration {{
    rounding = {look['rounding']}
    active_opacity = 1.0
    inactive_opacity = {'0.94' if t else '1.0'}
    blur {{
        enabled = {'true' if on(look, 'blur') else 'false'}
    }}
}}
animations {{
    enabled = {'true' if on(look, 'animations') else 'false'}
}}
"""


def _swayish(look, wm):
    g = int(look["gaps"])
    lines = [f"# generated by auxo-tweak look — overrides the rice defaults; change with Auxo Settings",
             f"font pango:{look['ui_font']} {look['font_size']}",
             f"default_border pixel {look['border']}",
             f"default_floating_border pixel {look['border']}",
             f"gaps inner {max(0, g // 2) if g else 0}",
             f"gaps outer {g if g else 0}"]
    return "\n".join(lines) + "\n"


def _kitty(look):
    size = round(int(look["font_size"]) * 1.15, 1)
    return (f"# generated by auxo-tweak look\nfont_family {look['mono_font']}\nfont_size {size}\n"
            f"background_opacity {'0.92' if on(look, 'transparency') else '1.0'}\n")


def _foot(look):
    size = round(int(look["font_size"]) * 1.1)
    return (f"# generated by auxo-tweak look\n[main]\nfont={look['mono_font']}:size={size}\n\n"
            f"[colors]\nalpha={'0.93' if on(look, 'transparency') else '1.0'}\n")


def _alacritty(look):
    size = round(int(look["font_size"]) * 1.1)
    return (f"# generated by auxo-tweak look\n[window]\nopacity = {'0.93' if on(look, 'transparency') else '1.0'}\n\n"
            f"[font]\nnormal = {{ family = \"{look['mono_font']}\" }}\nsize = {size}\n")


def _waybar_css(look):
    px = round(int(look["font_size"]) * 1.3)
    alpha = "0.82" if on(look, "transparency") else "1"
    return (f"/* generated by auxo-tweak look */\n"
            f"* {{ font-family: \"{look['ui_font']}\", \"Font Awesome 6 Free\", \"Font Awesome 6 Brands\", "
            f"\"{look['mono_font']}\"; font-size: {px}px; }}\n"
            f"window#waybar {{ border-radius: {look['rounding']}px; background: alpha(@bg, {alpha}); }}\n")


def _polybar(look):
    size = look["font_size"]
    return (f"; generated by auxo-tweak look\n[look]\nbottom = {'true' if look['bar'] == 'bottom' else 'false'}\n"
            f"radius = {look['rounding']}\nfont = {look['ui_font']}:size={size};2\n"
            f"mono = {look['mono_font']}:size={size};2\n")


def picom_conf(look):
    t, b, a = on(look, "transparency"), on(look, "blur"), on(look, "animations")
    out = ["# generated by auxo-tweak look — change with Auxo Settings",
           'backend = "glx";', "vsync = true;",
           f"corner-radius = {look['rounding']};",
           "rounded-corners-exclude = [ \"class_g = 'Polybar'\" ];",
           "shadow = true;", "shadow-radius = 16;", "shadow-opacity = 0.45;",
           f"fading = {'true' if a else 'false'};", "fade-delta = 5;",
           f"inactive-opacity = {'0.95' if t else '1.0'};"]
    if b:
        out += ["blur-method = \"dual_kawase\";", "blur-strength = 5;",
                "blur-background-exclude = [ \"window_type = 'dock'\", \"window_type = 'desktop'\" ];"]
    return "\n".join(out) + "\n"


def gtk_settings(look, gtk4=False):
    dark = look["mode"] == "dark"
    lines = ["[Settings]", f"gtk-font-name={look['ui_font']} {look['font_size']}",
             f"gtk-icon-theme-name={'Papirus-Dark' if dark else 'Papirus-Light'}",
             f"gtk-application-prefer-dark-theme={'1' if dark else '0'}",
             "gtk-cursor-theme-name=Adwaita", "gtk-cursor-theme-size=24",
             f"gtk-enable-animations={'1' if on(look, 'animations') else '0'}"]
    if not gtk4:
        lines.insert(1, f"gtk-theme-name={'Adwaita-dark' if dark else 'Adwaita'}")
    return "# generated by auxo-tweak look\n" + "\n".join(lines) + "\n"


def chrono_theme(look, accent):
    """An Auxo theme in Chrono's theme format, with this accent, mode and look baked in."""
    p = palette(accent, look["mode"])
    dark = look["mode"] == "dark"
    a, a2 = p["accent"], p["accent2"]
    win = "#111118" if dark else "#ffffff"
    title = "#15151e" if dark else "#ececf2"
    title_in = "#0f0f16" if dark else "#f3f3f7"
    panel = ("#0b0b10e0" if on(look, "transparency") else "#0b0b10ff") if dark else \
        ("#f5f5f8e6" if on(look, "transparency") else "#f5f5f8ff")
    accent_fg = "#0b0b10" if dark else "#ffffff"
    ui, mono, size = look["ui_font"], look["mono_font"], look["font_size"]
    return f"""# Chrono theme generated by Auxo (auxo-tweak look) — your accent, light/dark and look settings.
[theme]
name = Auxo
era = modern
variant = {'dark' if dark else 'light'}
description = Auxo's own look: near-black (or soft white), your accent colour, rounded windows.

[colors]
bg = {p['bg2']}
fg = {p['fg']}
accent = {a}
accent_fg = {accent_fg}
selection = {a}55
window_bg = {win}
window_fg = {p['fg']}
title_active = {title}
title_active2 = {title}
title_active_fg = {p['fg']}
title_inactive = {title_in}
title_inactive2 = {title_in}
title_inactive_fg = {p['muted']}
border_active = {a}
border_inactive = {p['inactive']}
button_bg = {p['inactive']}
button_fg = {p['fg']}
close_bg = {p['urgent']}
panel_bg = {panel}
panel_fg = {p['fg']}
panel_border = {a}44

[decor]
style = flat
title_height = 34
border_width = {max(1, int(look['border']))}
corner_radius = {look['rounding']}
title_center = true
buttons = :minimize,maximize,close
button_shape = circle
button_size = 14
shadow = true
shadow_size = 24
shadow_alpha = 0.5

[fonts]
ui = {ui}, Inter, Noto Sans, DejaVu Sans {size}
title = {ui}, Inter, Noto Sans, DejaVu Sans Bold {size}
mono = {mono}, Fira Code, DejaVu Sans Mono {size}

[panel]
position = {look['bar']}
height = 42
style = floating
dock = false

[desktop]
wallpaper = gradient
color1 = {p['bg']}
color2 = {a2}

[icons]
theme = {'Papirus-Dark' if dark else 'Papirus-Light'}
"""


def ini_set(text, section, key, value):
    """Set key = value in an INI text (Chrono style: spaces around '='), keeping everything else."""
    lines = text.splitlines()
    out, in_sec, done, found = [], False, False, False
    for line in lines:
        s = line.strip()
        if s.startswith("[") and "]" in s:
            if in_sec and not done:
                out.append(f"{key} = {value}")
                done = True
            in_sec = s[1:s.index("]")].strip() == section
            found = found or in_sec
        elif in_sec and not s.startswith((";", "#")) and s.split("=", 1)[0].strip() == key and "=" in s:
            if not done:
                out.append(f"{key} = {value}")
                done = True
            continue
        out.append(line)
    if not found:
        if out and out[-1].strip():
            out.append("")
        out.append(f"[{section}]")
    if not done:
        out.append(f"{key} = {value}")
    return "\n".join(out).strip("\n") + "\n"


def chrono_conf(existing, look, accent):
    t = existing or "# Chrono settings — Auxo sets the theme, panel position and wallpaper here\n"
    t = ini_set(t, "general", "theme", "auxo")
    if not re.search(r"(?m)^\s*terminal\s*=", t):
        t = ini_set(t, "general", "terminal", "foot")
    t = ini_set(t, "panel", "position", look["bar"])
    t = ini_set(t, "appearance", "accent", "theme")
    t = ini_set(t, "appearance", "mode", "theme")
    t = ini_set(t, "appearance", "reduce_motion", "false" if on(look, "animations") else "true")
    t = ini_set(t, "desktop", "wallpaper", wallpaper(look, accent))
    return t


def generate(look, accent):
    """name in ~/.config/auxo → content."""
    return {
        "look-hyprland.conf": _hypr(look),
        "look-sway": _swayish(look, "sway"),
        "look-i3": _swayish(look, "i3"),
        "look-kitty.conf": _kitty(look),
        "look-foot.ini": _foot(look),
        "look-alacritty.toml": _alacritty(look),
        "look.css": _waybar_css(look),
        "look-polybar.ini": _polybar(look),
    }


# lines the rices need so the look files take effect (added to older copies of the rices)
HOOKS = [
    (".config/hypr/hyprland.conf", "source = ~/.config/auxo/look-hyprland.conf"),
    (".config/sway/config", "include ~/.config/auxo/look-sway"),
    (".config/i3/config", "include ~/.config/auxo/look-i3"),
    (".config/kitty/kitty.conf", "include ~/.config/auxo/look-kitty.conf"),
]


def ensure_line(file, line):
    """Append line to file if the file exists and doesn't have it yet. True if changed."""
    p = R(file)
    if not os.path.exists(p):
        return False
    text = open(p).read()
    if line in text.splitlines():
        return False
    with open(p, "a") as f:
        f.write(("" if text.endswith("\n") or not text else "\n") + f"\n# Auxo look settings (Auxo Settings / auxo-tweak look)\n{line}\n")
    return True


def set_waybar_position(file, bar):
    """Move a Waybar bar to the top or bottom (and flip its margin so it keeps its gap)."""
    p = R(file)
    if not os.path.exists(p):
        return False
    text = open(p).read()
    if "auxo: bars stay where they are" in text:      # e.g. the Mac style's menu bar + dock
        return False
    new = re.sub(r'"position"\s*:\s*"(top|bottom)"', f'"position": "{bar}"', text, count=1)
    new = re.sub(r'"margin"\s*:\s*"8 14 0 14"|"margin"\s*:\s*"0 14 8 14"',
                 '"margin": "8 14 0 14"' if bar == "top" else '"margin": "0 14 8 14"', new, count=1)
    if new != text:
        with open(p, "w") as f:
            f.write(new)
        return True
    return False


def fix_wofi_imports(home):
    """wofi resolves a relative @import from the folder it was started in, not from the CSS file,
    so the rices' `@import url("../auxo/colors.css")` found nothing and the launcher had no colours.
    Point it at this user's colour file by its full path."""
    import glob
    out = []
    for p in glob.glob(R(f"{home}/.config/wofi/*.css")):
        text = open(p).read()
        new = text.replace('@import url("../auxo/colors.css");', f'@import url("{home}/.config/auxo/colors.css");')
        if new != text:
            with open(p, "w") as f:
                f.write(new)
            out.append(os.path.relpath(p, R(home)))
    return out


def write_user(home, look, accent, desktop=None):
    """Write every look file for one user. Returns the list of files touched (relative to home)."""
    touched = []
    cfg = f"{home}/.config/auxo"
    for name, content in generate(look, accent).items():
        write_file(f"{cfg}/{name}", content)
        touched.append(f".config/auxo/{name}")
    import auxo_monitors
    touched += auxo_monitors.write(home)
    touched += fix_wofi_imports(home)
    for rel, line in HOOKS + auxo_monitors.HOOKS:
        if ensure_line(f"{home}/{rel}", line):
            touched.append(rel)
    for rel in (".config/waybar/config.jsonc", ".config/waybar/config"):
        if set_waybar_position(f"{home}/{rel}", look["bar"]):
            touched.append(rel)
    if os.path.isdir(R(f"{home}/.config/picom")) or desktop == "i3":
        write_file(f"{home}/.config/picom/picom.conf", picom_conf(look))
        touched.append(".config/picom/picom.conf")
    if desktop not in ("plasma", "gnome", "cinnamon", "xfce"):
        # tiling desktops and Chrono have no settings daemon: GTK apps read these files
        write_file(f"{home}/.config/gtk-3.0/settings.ini", gtk_settings(look))
        write_file(f"{home}/.config/gtk-4.0/settings.ini", gtk_settings(look, gtk4=True))
        touched += [".config/gtk-3.0/settings.ini", ".config/gtk-4.0/settings.ini"]
        # X11/XWayland apps and anything without its own setting use ~/.icons/default
        write_file(f"{home}/.icons/default/index.theme", "[Icon Theme]\nName=Default\nInherits=Adwaita\n")
        touched.append(".icons/default/index.theme")
    if desktop == "chrono" or os.path.isdir(R(f"{home}/.config/chrono")):
        write_file(f"{home}/.config/chrono/themes/auxo.theme", chrono_theme(look, accent))
        conf = R(f"{home}/.config/chrono/chrono.conf")
        old = open(conf).read() if os.path.exists(conf) else ""
        write_file(f"{home}/.config/chrono/chrono.conf", chrono_conf(old, look, accent))
        touched += [".config/chrono/themes/auxo.theme", ".config/chrono/chrono.conf"]
    return touched


def font_spec(family, size):
    """KDE/Qt font string."""
    return f"{family},{size},-1,5,400,0,0,0,0,0,0,0,0,0,0,1"


def rgb_csv(h):
    return ",".join(str(x) for x in hex_to_rgb(h))


# ── presets: one click, a whole look (fonts and wallpaper stay yours unless the preset needs them) ──
PRESETS = {
    "auxo": ("Auxo", "The default: floating windows, soft corners, blur", {
        "gaps": "8", "rounding": "12", "border": "2", "blur": "on", "transparency": "on", "animations": "on"}),
    "glass": ("Glass", "Big corners, see-through everything, lots of blur", {
        "gaps": "12", "rounding": "18", "border": "1", "blur": "on", "transparency": "on", "animations": "on"}),
    "minimal": ("Minimal", "Tight gaps, square corners, nothing see-through", {
        "gaps": "4", "rounding": "0", "border": "1", "blur": "off", "transparency": "off", "animations": "on"}),
    "cozy": ("Cozy", "Roomy gaps, round corners, bigger text", {
        "gaps": "16", "rounding": "22", "border": "3", "font_size": "11", "blur": "on", "transparency": "on",
        "animations": "on"}),
    "retro": ("Retro", "Square, solid and monospaced, like a terminal from the 80s", {
        "gaps": "0", "rounding": "0", "border": "2", "blur": "off", "transparency": "off", "animations": "off",
        "ui_font": "Fira Code"}),
    "mac": ("Mac-style", "Liquid Glass: generous gaps, round corners, thin borders, lots of blur", {
        "gaps": "10", "rounding": "16", "border": "1", "blur": "on", "transparency": "on", "animations": "on"}),
    "win": ("Windows-style", "Small gaps, 8px corners, thin borders, a little blur", {
        "gaps": "6", "rounding": "8", "border": "1", "blur": "on", "transparency": "on", "animations": "on"}),
    "neon": ("Neon", "Cyberpunk: sharp corners, glowing borders, monospaced", {
        "gaps": "6", "rounding": "2", "border": "2", "blur": "on", "transparency": "on", "animations": "on",
        "ui_font": "Fira Code"}),
    "zen": ("Zen", "Calm: wide gaps, soft corners, no borders, no blur", {
        "gaps": "18", "rounding": "10", "border": "0", "blur": "off", "transparency": "off", "animations": "on"}),
    "candy": ("Candy", "Pastel and round: big corners, soft blur", {
        "gaps": "12", "rounding": "20", "border": "2", "blur": "on", "transparency": "on", "animations": "on"}),
    "lite": ("Lite", "No blur, transparency or animations: the fastest on older PCs", {
        "gaps": "6", "rounding": "8", "border": "2", "blur": "off", "transparency": "off", "animations": "off"}),
}


def preset(look, name):
    if name not in PRESETS:
        raise LookError(f"no preset named {name}. presets: {', '.join(PRESETS)}")
    out = dict(look)
    out.update(PRESETS[name][2])
    return out


# ── sharing a rice: a small text file anyone can import ──
RICE_EXT = ".auxorice"


def rice_export(look, accent, desktop, name="My Auxo rice"):
    lines = ["# Auxo rice — import it with Auxo Settings (Look → Import a rice) or:",
             "#   auxo-tweak look import THIS-FILE", "[auxo]", f"name = {name}", f"accent = {accent}",
             f"desktop = {desktop or ''}", "[look]"]
    for k in DEFAULTS:
        v = look[k]
        if k == "wallpaper" and v != "accent" and not v.startswith(WALLDIR + "/"):
            lines.append("# wallpaper: your own image isn't included, share it alongside this file")
            v = "accent"
        lines.append(f"{k} = {v}")
    return "\n".join(lines) + "\n"


def rice_import(text):
    """→ (look changes, accent or None, desktop or None, name). Unknown or invalid lines are skipped."""
    section, meta, changes = "", {}, {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        k, v = (x.strip() for x in line.split("=", 1))
        if section == "auxo":
            meta[k] = v
        elif section == "look" and k in DEFAULTS:
            try:
                changes[k] = validate(k, v)
            except LookError:
                pass
    if not changes and not meta:
        raise LookError("that isn't an Auxo rice file")
    acc = meta.get("accent") if meta.get("accent") in ACCENTS else None
    return changes, acc, meta.get("desktop") or None, meta.get("name", "Auxo rice")
