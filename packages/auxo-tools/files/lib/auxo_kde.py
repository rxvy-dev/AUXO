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

# fixed Auxo palette
BG0, WINDOW, VIEW, VIEW_ALT = "#0b0b10", "#111118", "#0e0e14", "#15151d"
BUTTON, BUTTON_ALT, TOOLTIP = "#1b1b25", "#22222e", "#1b1b25"
FG, FG_DIM = "#e8e8f0", "#8b8ba0"
NEG, NEU, POS, VISITED = "#f87171", "#fbbf24", "#34d399", "#c4b5fd"


def rgb(h):
    return ",".join(str(x) for x in hex_to_rgb(h))


def _mix(a, b, t):
    """Blend colour a toward b by t (0..1)."""
    ra, rb = hex_to_rgb(a), hex_to_rgb(b)
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(ra, rb))


def _group(bg, bg_alt, accent, accent2, fg=FG):
    return {
        "BackgroundNormal": rgb(bg), "BackgroundAlternate": rgb(bg_alt),
        "DecorationFocus": rgb(accent), "DecorationHover": rgb(accent),
        "ForegroundNormal": rgb(fg), "ForegroundInactive": rgb(FG_DIM),
        "ForegroundActive": rgb(accent2), "ForegroundLink": rgb(accent),
        "ForegroundVisited": rgb(VISITED), "ForegroundNegative": rgb(NEG),
        "ForegroundNeutral": rgb(NEU), "ForegroundPositive": rgb(POS),
    }


def sections(accent):
    """The colour sections of the scheme, with this accent baked in."""
    a, a2 = ACCENTS[accent]
    sel = _group(a, _mix(a, BG0, 0.35), a, a2, fg=BG0)
    sel["ForegroundInactive"] = rgb(_mix(BG0, a, 0.3))
    return {
        "ColorEffects:Disabled": {"Color": "56,56,56", "ColorAmount": "0", "ColorEffect": "0",
                                  "ContrastAmount": "0.65", "ContrastEffect": "1",
                                  "IntensityAmount": "0.1", "IntensityEffect": "2"},
        "ColorEffects:Inactive": {"ChangeSelectionColor": "true", "Color": "112,111,110",
                                  "ColorAmount": "0.025", "ColorEffect": "2", "ContrastAmount": "0.1",
                                  "ContrastEffect": "2", "Enable": "false",
                                  "IntensityAmount": "0", "IntensityEffect": "0"},
        "Colors:Button": _group(BUTTON, BUTTON_ALT, a, a2),
        "Colors:Complementary": _group(BG0, WINDOW, a, a2),
        "Colors:Header": _group(BG0, WINDOW, a, a2),
        "Colors:Header][Inactive": _group(BG0, WINDOW, a, a2),
        "Colors:Selection": sel,
        "Colors:Tooltip": _group(TOOLTIP, BUTTON_ALT, a, a2),
        "Colors:View": _group(VIEW, VIEW_ALT, a, a2),
        "Colors:Window": _group(WINDOW, BUTTON, a, a2),
        "WM": {"activeBackground": rgb(BG0), "activeBlend": rgb(BG0), "activeForeground": rgb(FG),
               "inactiveBackground": rgb(BG0), "inactiveBlend": rgb(BG0), "inactiveForeground": rgb(FG_DIM)},
    }


def scheme_text(accent):
    """A complete .colors file (what the Colours settings page lists)."""
    s = sections(accent)
    s["General"] = {"ColorScheme": SCHEME, "Name": SCHEME_NAME, "shadeSortColumn": "true"}
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


def merge_kdeglobals(path, accent):
    """Write the Auxo theme + accent into a kdeglobals file, keeping everything else."""
    cp = _parser()
    p = R(path)
    if os.path.exists(p):
        try:
            cp.read(p)
        except configparser.Error:
            shutil.copy2(p, p + ".auxo-bak")
            cp = _parser()
    secs = sections(accent)
    secs["General"] = {"ColorScheme": SCHEME, "AccentColor": rgb(ACCENTS[accent][0])}
    secs["KDE"] = {"LookAndFeelPackage": LAF, "widgetStyle": "Breeze"}
    secs["Icons"] = {"Theme": "breeze-dark"}
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


def apply_user(user, accent, reset_layout=False):
    """Auxo theme + accent for one user. reset_layout also puts the Auxo panel layout
    back (old panel config kept as *.auxo-bak); otherwise the user's panels stay."""
    home = home_of(user)
    merge_kdeglobals(f"{home}/.config/kdeglobals", accent)
    if reset_layout:
        for f in ("plasma-org.kde.plasma.desktop-appletsrc", "plasmashellrc"):
            p = R(f"{home}/.config/{f}")
            if os.path.exists(p):
                os.replace(p, p + ".auxo-bak")
    chown(f"{home}/.config", user, recursive=True)
    log(f"{user}: KDE Plasma → Auxo global theme ({accent})")
