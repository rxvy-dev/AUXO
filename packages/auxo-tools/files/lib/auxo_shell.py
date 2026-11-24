"""Shared pieces for Auxo's desktop shell apps (auxo-launcher, auxo-control): installed apps,
recent files, launching, power actions and the colours of the current look."""
import configparser
import os
import re
import shlex
import shutil
import subprocess
import urllib.parse
from xml.etree import ElementTree

HOME = os.environ.get("AUXO_HOME") or os.path.expanduser("~")

# Launchpad-style categories, from the freedesktop Categories= key
CATEGORIES = [
    ("Productivity", {"Office", "WordProcessor", "Spreadsheet", "Presentation", "Calendar", "ContactManagement",
                      "Email", "Development", "IDE", "TextEditor", "Finance", "ProjectManagement"}),
    ("Utilities", {"Utility", "System", "Settings", "FileManager", "TerminalEmulator", "Monitor", "Archiving",
                   "Filesystem", "Accessibility", "PackageManager", "HardwareSettings", "Security"}),
    ("Social", {"Network", "Chat", "InstantMessaging", "IRCClient", "VideoConference", "Telephony", "WebBrowser",
                "News", "Feed"}),
    ("Entertainment", {"Game", "AudioVideo", "Audio", "Video", "Player", "Music", "TV", "Recorder"}),
    ("Creativity", {"Graphics", "2DGraphics", "3DGraphics", "RasterGraphics", "VectorGraphics", "Photography",
                    "Publishing", "AudioVideoEditing", "Art"}),
    ("Information", {"Education", "Science", "Documentation", "Dictionary", "Geography", "Maps", "Literature",
                     "Viewer"}),
]
FIELD_CODES = re.compile(r"\s*%[fFuUdDnNickvm]")


def data_dirs():
    dirs = [os.environ.get("XDG_DATA_HOME") or os.path.join(HOME, ".local/share")]
    dirs += (os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":")
    dirs += [os.path.join(HOME, ".local/share/flatpak/exports/share"), "/var/lib/flatpak/exports/share"]
    out = []
    for d in dirs:
        if d and d not in out:
            out.append(d)
    return out


def _desktop_names():
    return {x.lower() for x in re.split(r"[:;]", os.environ.get("XDG_CURRENT_DESKTOP", "")) if x}


def load_apps(dirs=None):
    """Installed apps that belong in a menu, sorted by name. Earlier directories win (your own
    ~/.local/share/applications overrides a system file of the same name)."""
    seen, apps = set(), []
    current = _desktop_names()
    for d in dirs or data_dirs():
        root = os.path.join(d, "applications")
        if not os.path.isdir(root):
            continue
        for base, _dirs, files in os.walk(root):
            for f in sorted(files):
                if not f.endswith(".desktop"):
                    continue
                rel = os.path.relpath(os.path.join(base, f), root).replace("/", "-")
                if rel in seen:
                    continue
                seen.add(rel)
                app = parse_desktop(os.path.join(base, f), current)
                if app:
                    app["id"] = rel
                    apps.append(app)
    apps.sort(key=lambda a: a["name"].lower())
    return apps


def parse_desktop(path, current=None):
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.optionxform = str
    try:
        cp.read(path, encoding="utf-8")
        e = cp["Desktop Entry"]
    except (configparser.Error, KeyError, UnicodeDecodeError, OSError):
        return None
    if e.get("Type", "Application") != "Application" or e.get("NoDisplay", "false").lower() == "true" \
            or e.get("Hidden", "false").lower() == "true" or not e.get("Exec"):
        return None
    current = current if current is not None else _desktop_names()
    only = {x.lower() for x in e.get("OnlyShowIn", "").split(";") if x}
    notin = {x.lower() for x in e.get("NotShowIn", "").split(";") if x}
    if (only and current and not only & current) or (notin & current):
        return None
    if e.get("TryExec") and not (shutil.which(e["TryExec"]) or os.path.exists(e["TryExec"])):
        return None
    cats = {c for c in e.get("Categories", "").split(";") if c}
    return {"name": e.get("Name", os.path.basename(path)[:-8]), "comment": e.get("Comment", ""),
            "icon": e.get("Icon", "application-x-executable"), "exec": e["Exec"],
            "terminal": e.get("Terminal", "false").lower() == "true", "categories": cats,
            "group": category_of(cats), "keywords": e.get("Keywords", ""), "path": path}


def category_of(cats):
    for name, keys in CATEGORIES:
        if cats & keys:
            return name
    return "Other"


def matches(app, text):
    t = text.lower().strip()
    if not t:
        return True
    hay = " ".join((app["name"], app["comment"], app["keywords"], " ".join(app["categories"]))).lower()
    return all(w in hay for w in t.split())


def command(app):
    argv = shlex.split(FIELD_CODES.sub("", app["exec"]).replace("%%", "%"))
    if app.get("terminal"):
        term = os.environ.get("TERMINAL") or next((t for t in ("kitty", "foot", "alacritty", "xterm")
                                                   if shutil.which(t)), "xterm")
        argv = [term, "-e"] + argv
    return argv


def spawn(argv):
    """Start something detached from us, so it keeps running after the launcher closes."""
    try:
        subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True, cwd=HOME)
        return True
    except OSError:
        return False


def recent_files(limit=6, path=None):
    """Recently used files (the shared GTK/KDE list), newest first, only ones that still exist."""
    path = path or os.path.join(HOME, ".local/share/recently-used.xbel")
    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError):
        return []
    items = []
    for b in root.iter("bookmark"):
        href = b.get("href", "")
        if not href.startswith("file://"):
            continue
        f = urllib.parse.unquote(href[7:])
        if os.path.isfile(f):
            items.append((b.get("modified") or b.get("visited") or "", f))
    items.sort(reverse=True)
    out, seen = [], set()
    for when, f in items:
        if f not in seen:
            seen.add(f)
            out.append({"path": f, "name": os.path.basename(f), "when": when[:10]})
        if len(out) >= limit:
            break
    return out


POWER = {
    "lock": [["loginctl", "lock-session"]],
    "logout": [["hyprctl", "dispatch", "exit"], ["swaymsg", "exit"], ["i3-msg", "exit"]],
    "restart": [["loginctl", "reboot"]],
    "shutdown": [["loginctl", "poweroff"]],
}


def power(action):
    for argv in POWER[action]:
        if shutil.which(argv[0]):
            if action == "logout" and argv[0] == "hyprctl" and not os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
                continue
            if action == "logout" and argv[0] == "swaymsg" and not os.environ.get("SWAYSOCK"):
                continue
            return spawn(argv)
    return False


def look():
    """(mode, palette, ui_font) of the current Auxo look."""
    try:
        import auxo_look
        lk = auxo_look.load(HOME)
        acc = "violet"
        try:
            for line in open(os.environ.get("AUXO_ROOT", "").rstrip("/") + "/etc/auxo/auxo.conf"):
                if line.startswith("ACCENT="):
                    acc = line.split("=", 1)[1].strip().strip('"')
        except OSError:
            pass
        return lk["mode"], auxo_look.palette(acc, lk["mode"]), lk["ui_font"]
    except Exception:  # noqa: BLE001 — never let a missing look file stop the menu from opening
        return "dark", {"bg": "#0b0b10", "bg2": "#15151e", "fg": "#e8e8f0", "muted": "#8b8ba0",
                        "accent": "#a78bfa", "accent2": "#22d3ee", "inactive": "#2a2a38"}, "Inter"


def user_name():
    import pwd
    try:
        pw = pwd.getpwuid(os.getuid())
        return (pw.pw_gecos.split(",")[0] or pw.pw_name).strip()
    except KeyError:
        return os.environ.get("USER", "you")
