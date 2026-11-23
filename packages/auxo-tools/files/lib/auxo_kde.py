"""Auxo global theme for KDE Plasma.

The look-and-feel package lives in /usr/share/plasma/look-and-feel/org.auxolinux.desktop
(window style, icons, cursor, splash and lock screen fall back to Breeze). The colour
scheme "Auxo Dark" is Auxo's near-black palette with the user's accent baked in.

KDE apps read their colours from ~/.config/kdeglobals, not from the scheme file, so the
scheme is also written straight into kdeglobals. That way the theme is right from the
very first login, before any Plasma tool has run.
"""
import configparser
import os
import shutil

from auxo_accents import ACCENTS, hex_to_rgb
from auxo_core import R, log, write_file, chown, home_of

LAF = "org.auxolinux.desktop"
SCHEME = "AuxoDark"
SCHEME_NAME = "Auxo Dark"

# fixed Auxo palettes: dark (Auxo Dark) and light (Auxo Light)
PALETTES = {
    "dark": dict(BG0="#0b0b10", WINDOW="#111118", VIEW="#0e0e14", VIEW_ALT="#15151d", BUTTON="#1b1b25",
                 BUTTON_ALT="#22222e", TOOLTIP="#1b1b25", FG="#e8e8f0", FG_DIM="#8b8ba0",
                 NEG="#f87171", NEU="#fbbf24", POS="#34d399", VISITED="#c4b5fd"),
    "light": dict(BG0="#f5f5f8", WINDOW="#f0f0f4", VIEW="#ffffff", VIEW_ALT="#f5f5f8", BUTTON="#e6e6ee",
                  BUTTON_ALT="#dcdce6", TOOLTIP="#ffffff", FG="#1b1b26", FG_DIM="#6b6b80",
                  NEG="#dc2626", NEU="#b45309", POS="#059669", VISITED="#7c3aed"),
}
SCHEMES = {"dark": ("AuxoDark", "Auxo Dark"), "light": ("AuxoLight", "Auxo Light")}
_P = PALETTES["dark"]
BG0, WINDOW, VIEW, VIEW_ALT = _P["BG0"], _P["WINDOW"], _P["VIEW"], _P["VIEW_ALT"]
BUTTON, BUTTON_ALT, TOOLTIP = _P["BUTTON"], _P["BUTTON_ALT"], _P["TOOLTIP"]
FG, FG_DIM = _P["FG"], _P["FG_DIM"]
NEG, NEU, POS, VISITED = _P["NEG"], _P["NEU"], _P["POS"], _P["VISITED"]


def rgb(h):
    return ",".join(str(x) for x in hex_to_rgb(h))


def _mix(a, b, t):
    """Blend colour a toward b by t (0..1)."""
    ra, rb = hex_to_rgb(a), hex_to_rgb(b)
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(ra, rb))


def _group(bg, bg_alt, accent, accent2, fg=None, p=None):
    p = p or PALETTES["dark"]
    return {
        "BackgroundNormal": rgb(bg), "BackgroundAlternate": rgb(bg_alt),
        "DecorationFocus": rgb(accent), "DecorationHover": rgb(accent),
        "ForegroundNormal": rgb(fg or p["FG"]), "ForegroundInactive": rgb(p["FG_DIM"]),
        "ForegroundActive": rgb(accent2), "ForegroundLink": rgb(accent),
        "ForegroundVisited": rgb(p["VISITED"]), "ForegroundNegative": rgb(p["NEG"]),
        "ForegroundNeutral": rgb(p["NEU"]), "ForegroundPositive": rgb(p["POS"]),
    }


def sections(accent, mode="dark"):
    """The colour sections of the scheme (dark or light), with this accent baked in."""
    import auxo_look
    p = PALETTES["light" if mode == "light" else "dark"]
    a, a2 = auxo_look.LIGHT_ACCENT[accent] if mode == "light" else ACCENTS[accent]
    BG0, WINDOW, VIEW, VIEW_ALT = p["BG0"], p["WINDOW"], p["VIEW"], p["VIEW_ALT"]
    BUTTON, BUTTON_ALT, TOOLTIP, FG, FG_DIM = p["BUTTON"], p["BUTTON_ALT"], p["TOOLTIP"], p["FG"], p["FG_DIM"]
    on_accent = "#ffffff" if mode == "light" else BG0
    sel = _group(a, _mix(a, BG0, 0.35), a, a2, fg=on_accent, p=p)
    sel["ForegroundInactive"] = rgb(_mix(on_accent, a, 0.3))
    return {
        "ColorEffects:Disabled": {"Color": "56,56,56", "ColorAmount": "0", "ColorEffect": "0",
                                  "ContrastAmount": "0.65", "ContrastEffect": "1",
                                  "IntensityAmount": "0.1", "IntensityEffect": "2"},
        "ColorEffects:Inactive": {"ChangeSelectionColor": "true", "Color": "112,111,110",
                                  "ColorAmount": "0.025", "ColorEffect": "2", "ContrastAmount": "0.1",
                                  "ContrastEffect": "2", "Enable": "false",
                                  "IntensityAmount": "0", "IntensityEffect": "0"},
        "Colors:Button": _group(BUTTON, BUTTON_ALT, a, a2, p=p),
        "Colors:Complementary": _group(BG0, WINDOW, a, a2, p=p),
        "Colors:Header": _group(BG0, WINDOW, a, a2, p=p),
        "Colors:Header][Inactive": _group(BG0, WINDOW, a, a2, p=p),
        "Colors:Selection": sel,
        "Colors:Tooltip": _group(TOOLTIP, BUTTON_ALT, a, a2, p=p),
        "Colors:View": _group(VIEW, VIEW_ALT, a, a2, p=p),
        "Colors:Window": _group(WINDOW, BUTTON, a, a2, p=p),
        "WM": {"activeBackground": rgb(BG0), "activeBlend": rgb(BG0), "activeForeground": rgb(FG),
               "inactiveBackground": rgb(BG0), "inactiveBlend": rgb(BG0), "inactiveForeground": rgb(FG_DIM)},
    }


def scheme_text(accent, mode="dark"):
    """A complete .colors file (what the Colours settings page lists)."""
    s = sections(accent, mode)
    scheme, name = SCHEMES["light" if mode == "light" else "dark"]
    s["General"] = {"ColorScheme": scheme, "Name": name, "shadeSortColumn": "true"}
    s["KDE"] = {"contrast": "4"}
    return _render(s)


def _render(secs):
    out = []
    for name, kv in secs.items():
        out.append(f"[{name}]")
        out += [f"{k}={v}" for k, v in kv.items()]
        out.append("")
    return "\n".join(out)


def _parser():
    cp = configparser.RawConfigParser(strict=False, interpolation=None, delimiters=("=",))
    cp.optionxform = str  # KDE keys are case-sensitive
    return cp


def merge_kdeglobals(path, accent, look=None):
    """Write the Auxo theme + accent (and the look settings: light/dark, fonts, animations)
    into a kdeglobals file, keeping everything else."""
    import auxo_look
    look = look or auxo_look.DEFAULTS
    mode = "light" if look.get("mode") == "light" else "dark"
    cp = _parser()
    p = R(path)
    if os.path.exists(p):
        try:
            cp.read(p)
        except configparser.Error:
            shutil.copy2(p, p + ".auxo-bak")
            cp = _parser()
    secs = sections(accent, mode)
    a = auxo_look.LIGHT_ACCENT[accent][0] if mode == "light" else ACCENTS[accent][0]
    ui, mono, size = look["ui_font"], look["mono_font"], look["font_size"]
    font = auxo_look.font_spec(ui, size)
    secs["General"] = {"ColorScheme": SCHEMES[mode][0], "AccentColor": rgb(a),
                       "font": font, "menuFont": font, "toolBarFont": font,
                       "smallestReadableFont": auxo_look.font_spec(ui, max(7, int(size) - 2)),
                       "fixed": auxo_look.font_spec(mono, size)}
    secs["WM"] = dict(secs.get("WM", {}), activeFont=auxo_look.font_spec(ui, size))
    secs["KDE"] = {"LookAndFeelPackage": LAF, "widgetStyle": "Breeze",
                   "AnimationDurationFactor": "1" if look.get("animations", "on") == "on" else "0"}
    secs["Icons"] = {"Theme": "breeze-dark" if mode == "dark" else "breeze"}
    for name, kv in secs.items():
        if not cp.has_section(name):
            cp.add_section(name)
        for k, v in kv.items():
            cp.set(name, k, v)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        cp.write(f, space_around_delimiters=False)


def apply_system(accent):
    """System-wide default for every account (the live user and new users included)."""
    if not os.path.isdir(R(f"/usr/share/plasma/look-and-feel/{LAF}")):
        return
    merge_kdeglobals("/etc/xdg/kdeglobals", accent)
    write_file("/usr/share/color-schemes/AuxoDark.colors", scheme_text(accent))
    write_file("/usr/share/color-schemes/AuxoLight.colors", scheme_text(accent, "light"))


def apply_user(user, accent, reset_layout=False):
    """Auxo theme + accent for one user. reset_layout also puts the Auxo panel layout
    back (old panel config kept as *.auxo-bak); otherwise the user's panels stay."""
    import auxo_look
    home = home_of(user)
    merge_kdeglobals(f"{home}/.config/kdeglobals", accent, auxo_look.load(home))
    if reset_layout:
        for f in ("plasma-org.kde.plasma.desktop-appletsrc", "plasmashellrc"):
            p = R(f"{home}/.config/{f}")
            if os.path.exists(p):
                os.replace(p, p + ".auxo-bak")
    chown(f"{home}/.config", user, recursive=True)
    log(f"{user}: KDE Plasma → Auxo global theme ({accent})")
