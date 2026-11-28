"""Apps pinned to the taskbar (Windows style), the dock (Mac style) and the bar (Auxo style).

The list is ~/.config/auxo/bar-pins, one .desktop id per line, in order. From it Auxo writes
Waybar include files (~/.config/waybar/pins-<size>.jsonc), one per icon size the rices use;
each defines "group/pins" (an image per app) and "wlr/taskbar" (running windows, without the
pinned ones, so an app never shows twice). Changing the list reloads Waybar (SIGUSR2).

Click a pin to open the app, or to switch to it when it's already open. Right-click to unpin.
"""
import json
import os
import re
import shlex
import subprocess

import auxo_shell as S

PINS_FILE = os.path.join(S.HOME, ".config/auxo/bar-pins")
WAYBAR_DIR = os.path.join(S.HOME, ".config/waybar")
SIZES = (22, 30, 48)          # Auxo bar, Windows taskbar, Mac dock
DEFAULT = ["thunar.desktop", "org.xfce.thunar.desktop", "firefox.desktop", "kitty.desktop", "foot.desktop",
           "auxo-store.desktop", "steam.desktop", "com.discordapp.Discord.desktop", "auxo-settings.desktop"]
# never pinned twice: whichever one is installed
SAME = {"org.xfce.thunar.desktop": "thunar.desktop", "foot.desktop": "kitty.desktop"}
ICON_DIRS = ["icons/Papirus/64x64/apps", "icons/Papirus/48x48/apps", "icons/hicolor/scalable/apps",
             "icons/hicolor/256x256/apps", "icons/hicolor/128x128/apps", "icons/hicolor/64x64/apps",
             "icons/hicolor/48x48/apps", "icons/Adwaita/scalable/apps", "pixmaps"]
FALLBACK_ICON = "/usr/share/icons/Papirus/64x64/apps/application-x-executable.svg"


def load(apps=None):
    """The pinned ids, in order (only apps that are installed)."""
    apps = apps if apps is not None else S.load_apps()
    have = {a["id"] for a in apps}
    try:
        ids = [l.strip() for l in open(PINS_FILE) if l.strip() and not l.startswith("#")]
    except OSError:
        ids = DEFAULT
    out, seen = [], set()
    for i in ids:
        key = SAME.get(i, i)
        if i in have and key not in seen:
            seen.add(key)
            out.append(i)
    return out


def save(ids):
    os.makedirs(os.path.dirname(PINS_FILE), exist_ok=True)
    clean = []
    for i in ids:
        if i and i not in clean:
            clean.append(i)
    with open(PINS_FILE, "w") as f:
        f.write("# Taskbar / dock pins, in order. Change them: right-click a pin, the Start menu or Launchpad,\n"
                "# Auxo Settings > Taskbar & dock, or: auxo-pins add|remove|move ID\n" + "\n".join(clean) + "\n")
    return clean


def app_id(name, apps=None):
    """'firefox', 'Firefox' or 'firefox.desktop' → the installed app's id (or None)."""
    apps = apps if apps is not None else S.load_apps()
    n = name.strip()
    ids = {a["id"]: a for a in apps}
    if n in ids:
        return n
    if n + ".desktop" in ids:
        return n + ".desktop"
    low = n.lower().removesuffix(".desktop")
    for a in apps:
        if a["id"].lower().removesuffix(".desktop").split(".")[-1] == low or a["name"].lower() == low:
            return a["id"]
    return None


def add(name, apps=None, at=None):
    apps = apps if apps is not None else S.load_apps()
    i = app_id(name, apps)
    if not i:
        raise ValueError(f"no installed app called '{name}' (see: auxo-pins apps)")
    ids = [x for x in load(apps) if x != i]
    ids.insert(len(ids) if at is None else max(0, min(at, len(ids))), i)
    return save(ids)


def remove(name, apps=None):
    apps = apps if apps is not None else S.load_apps()
    i = app_id(name, apps) or name
    return save([x for x in load(apps) if x != i])


def move(name, delta, apps=None):
    apps = apps if apps is not None else S.load_apps()
    i = app_id(name, apps) or name
    ids = load(apps)
    if i in ids:
        k = ids.index(i)
        j = max(0, min(len(ids) - 1, k + delta))
        ids.insert(j, ids.pop(k))
    return save(ids)


def icon_path(icon, dirs=None):
    """A file Waybar's image module can draw for an icon name from a .desktop file."""
    if icon and os.path.isabs(icon) and os.path.exists(icon):
        return icon
    for d in dirs or S.data_dirs():
        for sub in ICON_DIRS:
            for ext in (".svg", ".png"):
                p = os.path.join(d, sub, (icon or "") + ext)
                if icon and os.path.exists(p):
                    return p
    return FALLBACK_ICON


def wm_class(app):
    """What the app's windows are called, so wlr/taskbar can leave pinned ones out."""
    for line in open(app["path"], encoding="utf-8", errors="replace"):
        if line.startswith("StartupWMClass="):
            return line.split("=", 1)[1].strip()
    try:
        exe = os.path.basename(shlex.split(S.FIELD_CODES.sub("", app["exec"]))[0])
    except (ValueError, IndexError):
        exe = ""
    if exe == "flatpak":
        return app["id"].removesuffix(".desktop")
    return exe or app["id"].removesuffix(".desktop")


def module_name(app_id_):
    return "image#pin-" + re.sub(r"[^A-Za-z0-9_-]", "-", app_id_.removesuffix(".desktop"))


def waybar_modules(size, apps=None, extra_ignore=()):
    """The include file's content for one icon size."""
    apps = apps if apps is not None else S.load_apps()
    by_id = {a["id"]: a for a in apps}
    ids = load(apps)
    mods = {"group/pins": {"orientation": "inherit", "modules": [module_name(i) for i in ids] or ["custom/pins-empty"]},
            "custom/pins-empty": {"format": "", "tooltip": False}}
    ignore = list(extra_ignore)
    for i in ids:
        a = by_id[i]
        mods[module_name(i)] = {"path": icon_path(a["icon"]), "size": size, "tooltip": True,
                                "tooltip-format": a["name"],
                                "on-click": f"auxo-pins launch {shlex.quote(i)}",
                                "on-click-right": f"auxo-pins menu {shlex.quote(i)}",
                                "on-click-middle": f"auxo-pins remove {shlex.quote(i)}"}
        for c in {wm_class(a), wm_class(a).lower()}:
            if c and c not in ignore:
                ignore.append(c)
    mods["wlr/taskbar"] = {"format": "{icon}", "icon-size": size, "icon-theme": "Papirus-Dark",
                           "tooltip-format": "{title}", "on-click": "activate", "on-click-middle": "close",
                           "ignore-list": ignore + ["auxo-start", "auxo-quick", "auxo-launchpad", "auxo-control",
                                                    "auxo-sound"]}
    return mods


def write_waybar(apps=None):
    apps = apps if apps is not None else S.load_apps()
    os.makedirs(WAYBAR_DIR, exist_ok=True)
    written = []
    for size in SIZES:
        p = os.path.join(WAYBAR_DIR, f"pins-{size}.jsonc")
        with open(p, "w") as f:
            f.write("// written by auxo-pins from ~/.config/auxo/bar-pins: change pins there, not here\n")
            json.dump(waybar_modules(size, apps), f, indent=2)
            f.write("\n")
        written.append(p)
    return written


def _waybars(proc="/proc"):
    """[(pid, argv)] of this user's running Waybars."""
    out = []
    for d in os.listdir(proc):
        if not d.isdigit():
            continue
        try:
            if os.stat(f"{proc}/{d}").st_uid != os.getuid():
                continue
            argv = [a.decode() for a in open(f"{proc}/{d}/cmdline", "rb").read().split(b"\0") if a]
        except OSError:
            continue
        if argv and os.path.basename(argv[0]) == "waybar":
            out.append((int(d), argv))
    return out


def reload_waybar():
    """Restart Waybar with the same arguments. (A SIGUSR2 reload doesn't re-read include files in
    every Waybar version, so the pins wouldn't change.)"""
    import signal
    import time
    bars = _waybars()
    for pid, _argv in bars:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    if bars:
        time.sleep(0.3)
    for argv in {tuple(a) for _p, a in bars}:
        S.spawn(list(argv))


def running_window(app):
    """The address of an open window of this app (Hyprland), or None."""
    try:
        out = subprocess.run(["hyprctl", "clients", "-j"], capture_output=True, text=True, timeout=2).stdout
        clients = json.loads(out or "[]")
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    want = wm_class(app).lower()
    for c in sorted(clients, key=lambda c: c.get("focusHistoryID", 99)):
        if want and want in (c.get("class", "").lower(), c.get("initialClass", "").lower()):
            return c.get("address")
    return None


def launch(name, apps=None):
    apps = apps if apps is not None else S.load_apps()
    i = app_id(name, apps)
    app = next((a for a in apps if a["id"] == i), None)
    if not app:
        return False
    addr = running_window(app)
    if addr:
        subprocess.run(["hyprctl", "dispatch", "focuswindow", f"address:{addr}"], stdout=subprocess.DEVNULL)
        return True
    return S.spawn(S.command(app))
