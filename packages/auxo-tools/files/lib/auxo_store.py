"""Auxo App Store backend: Void packages (xbps) and Flathub apps (flatpak), behind one list.

Every app is a dict: {id, name, summary, source: "void"|"flathub"|"auxo", installed, icon}.
id is the package name (void), the Flathub app ID (flathub), or an auxo-tweak command (auxo).
Commands run through `runner` so tests can fake them."""
import os
import re
import shutil
import subprocess

# Hand-picked apps for the Discover page. Void names checked against Void's package list;
# apps Void doesn't ship (or can't: Discord is restricted) come from Flathub.
CATALOG = [
    ("Popular", [
        ("firefox", "void", "Firefox", "The web browser that puts privacy first", "firefox"),
        ("com.discordapp.Discord", "flathub", "Discord", "Voice, video and text chat with friends", "discord"),
        ("gaming on", "auxo", "Steam", "Valve's game store, with Proton for Windows games (+ GameMode, MangoHud)",
         "steam"),
        ("com.spotify.Client", "flathub", "Spotify", "Music and podcasts", "spotify"),
        ("obs", "void", "OBS Studio", "Record and stream your screen", "com.obsproject.Studio"),
        ("vscode", "void", "Code - OSS", "VS Code, built from the open-source code", "code-oss"),
    ]),
    ("Games", [
        ("org.prismlauncher.PrismLauncher", "flathub", "Prism Launcher", "Minecraft: Java and modpacks, many instances",
         "org.prismlauncher.PrismLauncher"),
        ("lutris", "void", "Lutris", "One library for games from every store", "lutris"),
        ("com.heroicgameslauncher.hgl", "flathub", "Heroic", "Epic, GOG and Amazon games on Linux",
         "com.heroicgameslauncher.hgl"),
        ("com.usebottles.bottles", "flathub", "Bottles", "Run Windows apps and games with Wine",
         "com.usebottles.bottles"),
        ("net.davidotek.pupgui2", "flathub", "ProtonUp-Qt", "Install Proton-GE and other Proton builds",
         "net.davidotek.pupgui2"),
        ("gamescope", "void", "gamescope", "Valve's gaming compositor: scaling, FPS limits, HDR", "input-gaming"),
    ]),
    ("Create", [
        ("gimp", "void", "GIMP", "Edit photos and images", "gimp"),
        ("krita", "void", "Krita", "Digital painting and drawing", "krita"),
        ("inkscape", "void", "Inkscape", "Vector graphics: logos, icons, illustrations", "inkscape"),
        ("blender", "void", "Blender", "3D modelling, animation and rendering", "blender"),
        ("kdenlive", "void", "Kdenlive", "Video editing", "kdenlive"),
        ("audacity", "void", "Audacity", "Record and edit audio", "audacity"),
        ("godot", "void", "Godot", "Make 2D and 3D games", "godot"),
        ("darktable", "void", "darktable", "Photo workflow and RAW editing", "darktable"),
    ]),
    ("Work", [
        ("libreoffice", "void", "LibreOffice", "Documents, spreadsheets and presentations", "libreoffice-startcenter"),
        ("thunderbird", "void", "Thunderbird", "Email and calendar", "thunderbird"),
        ("md.obsidian.Obsidian", "flathub", "Obsidian", "Notes that link together", "md.obsidian.Obsidian"),
        ("dev.zed.Zed", "flathub", "Zed", "A fast code editor", "dev.zed.Zed"),
        ("keepassxc", "void", "KeePassXC", "A password manager that stays on your PC", "keepassxc"),
        ("okular", "void", "Okular", "PDFs and documents", "okular"),
        ("xournalpp", "void", "Xournal++", "Handwritten notes and PDF annotation", "xournalpp"),
    ]),
    ("Chat & media", [
        ("telegram-desktop", "void", "Telegram", "Messages and channels", "telegram"),
        ("org.signal.Signal", "flathub", "Signal", "Private messages and calls", "org.signal.Signal"),
        ("element-desktop", "void", "Element", "Matrix chat", "element"),
        ("vlc", "void", "VLC", "Plays almost any video or music file", "vlc"),
        ("mpv", "void", "mpv", "A minimal, fast video player", "mpv"),
        ("strawberry", "void", "Strawberry", "Music player for your collection", "strawberry"),
    ]),
    ("Tools", [
        ("qbittorrent", "void", "qBittorrent", "Torrent downloads", "qbittorrent"),
        ("syncthing", "void", "Syncthing", "Sync folders between your devices", "syncthing"),
        ("gparted", "void", "GParted", "Partition disks", "gparted"),
        ("virt-manager", "void", "Virtual Machines", "Run other operating systems (QEMU/KVM)", "virt-manager"),
        ("handbrake", "void", "HandBrake", "Convert videos", "fr.handbrake.ghb"),
        ("easyeffects", "void", "EasyEffects", "Equalizer and audio effects", "com.github.wwmm.easyeffects"),
        ("flameshot", "void", "Flameshot", "Screenshots with arrows and text", "flameshot"),
    ]),
]


def _run(argv, timeout=60):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""


class Store:
    def __init__(self, runner=None, have=None):
        self.run = runner or _run
        self.have = have or (lambda prog: shutil.which(prog) is not None)
        self._void_installed = None
        self._flat_installed = None

    # ── what's installed ──
    def void_installed(self, refresh=False):
        if self._void_installed is None or refresh:
            rc, out = self.run(["xbps-query", "-l"])
            names = set()
            for line in out.splitlines():
                m = re.match(r"^\S+\s+(\S+?)-[^-\s]+_\d+\s", line)
                if m:
                    names.add(m.group(1))
            self._void_installed = names
        return self._void_installed

    def flathub_ready(self):
        if not self.have("flatpak"):
            return False
        rc, out = self.run(["flatpak", "remotes", "--columns=name"])
        return rc == 0 and "flathub" in out.split()

    def flat_installed(self, refresh=False):
        if self._flat_installed is None or refresh:
            ids = {}
            if self.have("flatpak"):
                rc, out = self.run(["flatpak", "list", "--app", "--columns=application,name,version"])
                for line in out.splitlines():
                    parts = line.split("\t")
                    if parts and parts[0]:
                        ids[parts[0]] = parts[1] if len(parts) > 1 else parts[0]
            self._flat_installed = ids
        return self._flat_installed

    def is_installed(self, app):
        if app["source"] == "void":
            return app["id"] in self.void_installed()
        if app["source"] == "flathub":
            return app["id"] in self.flat_installed()
        if app["source"] == "auxo" and app["id"].startswith("gaming"):
            return "steam" in self.void_installed() or "com.valvesoftware.Steam" in self.flat_installed()
        return False

    def refresh(self):
        self.void_installed(refresh=True)
        self.flat_installed(refresh=True)

    # ── lists ──
    def catalog(self):
        out = []
        for cat, apps in CATALOG:
            items = []
            for pid, source, name, summary, icon in apps:
                a = {"id": pid, "source": source, "name": name, "summary": summary, "icon": icon}
                a["installed"] = self.is_installed(a)
                items.append(a)
            out.append((cat, items))
        return out

    def installed(self):
        """Your apps: Flathub apps, and Void packages that have a menu entry (not libraries)."""
        apps = [{"id": i, "source": "flathub", "name": n, "summary": i, "icon": i, "installed": True}
                for i, n in sorted(self.flat_installed().items(), key=lambda kv: kv[1].lower())]
        names = self.void_installed()
        known = {pid: (name, summary, icon) for _c, items in CATALOG for pid, src, name, summary, icon in items
                 if src == "void"}
        for pid in sorted(names & set(known)):
            n, s, i = known[pid]
            apps.append({"id": pid, "source": "void", "name": n, "summary": s, "icon": i, "installed": True})
        return apps

    def search(self, text):
        text = text.strip()
        if not text:
            return []
        results, seen = [], set()
        rc, out = self.run(["xbps-query", "-Rs", text])
        for line in out.splitlines():
            m = re.match(r"^\[(.)\]\s+(\S+?)-[^-\s]+_\d+\s+(.*)$", line)
            if not m:
                continue
            name = m.group(2)
            if name.endswith(("-devel", "-dbg", "-32bit", "-doc")) or name in seen:
                continue
            seen.add(name)
            results.append({"id": name, "source": "void", "name": name, "summary": m.group(3).strip(), "icon": name,
                            "installed": m.group(1) == "*"})
        if self.flathub_ready():
            rc, out = self.run(["flatpak", "search", "--columns=application,name,description", text])
            for line in out.splitlines():
                parts = line.split("\t")
                if len(parts) < 2 or "." not in parts[0] or parts[0] in seen:
                    continue
                seen.add(parts[0])
                results.append({"id": parts[0], "source": "flathub", "name": parts[1],
                                "summary": parts[2] if len(parts) > 2 else "", "icon": parts[0],
                                "installed": parts[0] in self.flat_installed()})
        t = text.lower()
        results.sort(key=lambda a: (a["name"].lower() != t, not a["name"].lower().startswith(t), a["source"] != "flathub"
                                    if " " in a["name"] else a["source"] == "flathub", a["name"].lower()))
        return results[:80]

    # ── changes: the command to run (the window runs it and shows progress) ──
    def command(self, app, action, pkexec="pkexec", tweak="auxo-tweak"):
        if app["source"] == "void":
            return ([pkexec, "xbps-install", "-Sy", app["id"]] if action == "install"
                    else [pkexec, "xbps-remove", "-Ry", app["id"]])
        if app["source"] == "flathub":
            # flatpak asks for permission itself (polkit) when it needs to
            return (["flatpak", "install", "-y", "--noninteractive", "flathub", app["id"]] if action == "install"
                    else ["flatpak", "uninstall", "-y", "--noninteractive", app["id"]])
        if app["source"] == "auxo":
            state = "on" if action == "install" else "off"
            return [pkexec, tweak, *app["id"].split()[:-1], state]
        raise ValueError(app["source"])

    def update_command(self, pkexec="pkexec"):
        return [pkexec, "auxo-update", "--yes"] if self.have("auxo-update") else [pkexec, "xbps-install", "-Suy"]

    @staticmethod
    def launch_command(app):
        if app["source"] == "flathub":
            return ["flatpak", "run", app["id"]]
        for d in (os.path.expanduser("~/.local/share/applications"), "/usr/share/applications"):
            for name in (f"{app['icon']}.desktop", f"{app['id']}.desktop"):
                if os.path.exists(os.path.join(d, name)):
                    return ["gtk-launch", name[:-8]] if shutil.which("gtk-launch") else None
        return [app["id"]] if shutil.which(app["id"]) else None
