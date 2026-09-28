"""Auxo GNOME setup: extensions + system-wide defaults (dconf).

Extensions come from the Arch repos where they exist; Dash to Dock and Blur my
Shell aren't packaged in the official repos, so they're fetched from
extensions.gnome.org for the installed GNOME Shell version. Everything is
optional: a missing extension is skipped, never breaks the desktop.

Defaults live in /etc/dconf/db/local.d/00-auxo, so they apply to every user
until that user changes a setting themselves.
"""
import io
import json
import os
import re
import shutil
import urllib.request
import zipfile

from auxo_accents import ACCENTS
from auxo_core import R, DRY, log, warn, run, have, pacman, installed, online, write_file
from auxo_desktops import GNOME_ACCENT

EXT_DIR = "/usr/share/gnome-shell/extensions"
DCONF_PROFILE = "/etc/dconf/profile/user"
DCONF_FILE = "/etc/dconf/db/local.d/00-auxo"
WALLDIR = "/usr/share/wallpapers"

# (uuid, repo package or None → extensions.gnome.org)
EXTENSIONS = [
    ("appindicatorsupport@rgcjonas.gmail.com", "gnome-shell-extension-appindicator"),  # tray icons (Discord, Steam…)
    ("caffeine@patapon.info", "gnome-shell-extension-caffeine"),                       # keep the screen awake
    ("user-theme@gnome-shell-extensions.gcampax.github.com", "gnome-shell-extensions"),
    ("dash-to-dock@micxgx.gmail.com", None),                                           # a real dock
    ("blur-my-shell@aunetx", None),                                                    # blurred panel + overview
]
EGO = "https://extensions.gnome.org"


def shell_major():
    if DRY:
        return "50"
    out = run(["pacman", "-Q", "gnome-shell"], capture=True, check=False) or ""
    m = re.search(r"gnome-shell (?:\d+:)?(\d+)", out)
    return m.group(1) if m else ""


def fetch_ego(uuid, version):
    """Download + unpack one extension from extensions.gnome.org into EXT_DIR."""
    if DRY:
        log(f"[dry-run] fetch {uuid} for GNOME {version}")
        return True
    try:
        q = f"{EGO}/extension-info/?uuid={uuid}&shell_version={version}"
        with urllib.request.urlopen(q, timeout=20) as r:
            info = json.load(r)
        url = info.get("download_url")
        if not url:
            raise ValueError("no build for this GNOME version")
        with urllib.request.urlopen(EGO + url, timeout=60) as r:
            data = r.read()
        dst = R(f"{EXT_DIR}/{uuid}")
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.makedirs(dst)
        zipfile.ZipFile(io.BytesIO(data)).extractall(dst)
        # only keep a build that says it supports this GNOME version; an extension
        # built for another version could break the shell
        meta = json.load(open(f"{dst}/metadata.json"))
        if str(version) not in [str(v).split(".")[0] for v in meta.get("shell-version", [])]:
            shutil.rmtree(dst)
            raise ValueError(f"build supports GNOME {meta.get('shell-version')}, not {version}")
        for base, dirs, files in os.walk(dst):
            for d in dirs:
                os.chmod(os.path.join(base, d), 0o755)
            for f in files:
                os.chmod(os.path.join(base, f), 0o644)
        if os.path.isdir(f"{dst}/schemas") and have("glib-compile-schemas"):
            run(["glib-compile-schemas", f"{dst}/schemas"], check=False)
        log(f"GNOME extension {uuid} installed")
        return True
    except Exception as e:  # network, API change, bad zip: skip this one
        warn(f"could not install GNOME extension {uuid}: {e}")
        return False


def install_extensions():
    pkgs = [p for _, p in EXTENSIONS if p]
    try:
        pacman("-S", *pkgs)
    except Exception as e:
        warn(f"some GNOME extension packages failed: {e}")
    version = shell_major()
    if version and (DRY or online()):
        for uuid, pkg in EXTENSIONS:
            if pkg is None:
                fetch_ego(uuid, version)


def present():
    if DRY:
        return [u for u, _ in EXTENSIONS]
    return [u for u, _ in EXTENSIONS
            if os.path.isdir(R(f"{EXT_DIR}/{u}")) or os.path.isdir(R(f"/usr/share/gnome-shell/extensions/{u}"))]


def gv(v):
    """Python value → GVariant text for a dconf keyfile."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return str(v)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(gv(x) for x in v) + "]"
    return "'" + str(v).replace("\\", "\\\\").replace("'", "\\'") + "'"


KB = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings"


def defaults(accent, exts):
    a, a2 = ACCENTS[accent]
    wall = f"file://{WALLDIR}/auxo-{accent}.png"
    s = {
        "org/gnome/shell": {
            "enabled-extensions": exts,
            "disable-user-extensions": False,
            "favorite-apps": ["org.gnome.Nautilus.desktop", "firefox.desktop", "org.gnome.Console.desktop",
                              "org.gnome.Settings.desktop"],
        },
        "org/gnome/desktop/interface": {
            "color-scheme": "prefer-dark", "gtk-theme": "Adwaita-dark", "accent-color": GNOME_ACCENT[accent],
            "clock-show-weekday": True, "show-battery-percentage": True,
        },
        "org/gnome/desktop/background": {"picture-uri": wall, "picture-uri-dark": wall, "picture-options": "zoom"},
        "org/gnome/desktop/screensaver": {"picture-uri": wall},
        "org/gnome/desktop/wm/preferences": {"button-layout": "appmenu:minimize,maximize,close"},
        "org/gnome/desktop/wm/keybindings": {"close": ["<Super>q", "<Alt>F4"]},
        "org/gnome/mutter": {"center-new-windows": True},
        "org/gnome/settings-daemon/plugins/media-keys": {
            "custom-keybindings": [f"{KB}/auxo-terminal/", f"{KB}/auxo-tweak/"]},
        KB.lstrip("/") + "/auxo-terminal": {"name": "Terminal", "command": "kgx", "binding": "<Super>Return"},
        KB.lstrip("/") + "/auxo-tweak": {"name": "Auxo Tweak", "command": "kgx -- auxo-tweak", "binding": "<Super>t"},
    }
    if "dash-to-dock@micxgx.gmail.com" in exts:
        s["org/gnome/shell/extensions/dash-to-dock"] = {
            "dock-position": "BOTTOM", "dash-max-icon-size": 42, "dock-fixed": False,
            "intellihide-mode": "FOCUS_APPLICATION_WINDOWS", "click-action": "minimize-or-previews",
            "show-trash": False, "show-mounts": False, "apply-custom-theme": False,
            "custom-background-color": True, "background-color": "#0b0b10",
            "transparency-mode": "FIXED", "background-opacity": 0.6, "custom-theme-shrink": True,
            "running-indicator-style": "DOTS", "custom-theme-customize-running-dots": True,
            "custom-theme-running-dots-color": a, "custom-theme-running-dots-border-color": a2,
        }
    if "blur-my-shell@aunetx" in exts:
        s["org/gnome/shell/extensions/blur-my-shell/panel"] = {"blur": True, "static-blur": True}
        s["org/gnome/shell/extensions/blur-my-shell/dash-to-dock"] = {"blur": True}
    return s


def keyfile(settings):
    out = ["# Auxo GNOME defaults — written by auxo-tweak (your own changes always win)"]
    for sec, kv in settings.items():
        out.append(f"\n[{sec}]")
        out += [f"{k}={gv(v)}" for k, v in kv.items()]
    return "\n".join(out) + "\n"


def write_defaults(accent):
    write_file(DCONF_PROFILE, "user-db:user\nsystem-db:local\n")
    write_file(DCONF_FILE, keyfile(defaults(accent, present())))
    if have("dconf") or DRY:
        run(["dconf", "update"], check=False)


def setup(accent):
    """Install extensions and write the Auxo GNOME defaults."""
    install_extensions()
    write_defaults(accent)
    log("GNOME: dock, tray icons, blur, dark theme and your accent set up "
        "(manage extensions in the Extensions app)")


def refresh_accent(accent):
    """Called by `auxo-tweak accent`: keep the GNOME defaults in step with the accent."""
    if os.path.exists(R(DCONF_FILE)):
        write_defaults(accent)
