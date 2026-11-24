#!/usr/bin/env python3
"""Offscreen tests for the desktop shell apps of the Mac/Windows rices: auxo_shell (app list,
search, launching, recent files), auxo-launcher (Launchpad + Start menu) and auxo-control
(Control Center + Quick settings, with a fake backend: nothing on the machine is changed).

  QT_QPA_PLATFORM=offscreen python3 tests/test-shell.py
"""
import importlib.machinery
import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(HERE, "packages/auxo-tools/files")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["AUXO_LAUNCHER_STAY"] = "1"
HOME = tempfile.mkdtemp(prefix="auxo-shell-")
os.environ["AUXO_HOME"] = HOME
os.environ["AUXO_LIB"] = os.path.join(PKG, "lib")
sys.path.insert(0, os.path.join(PKG, "lib"))

passed = failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  ✗", name)


def load(name):
    path = os.path.join(PKG, "bin", name)
    loader = importlib.machinery.SourceFileLoader(name.replace("-", "_"), path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


# ── a fake set of installed apps ──
apps_dir = os.path.join(HOME, "share/applications")
os.makedirs(apps_dir)
DESKTOP = {
    "firefox.desktop": "Name=Firefox\nExec=firefox %u\nIcon=firefox\nCategories=Network;WebBrowser;\n",
    "gimp.desktop": "Name=GIMP\nComment=Edit pictures\nExec=gimp %F\nIcon=gimp\nCategories=Graphics;2DGraphics;\n",
    "btop.desktop": "Name=btop\nExec=btop\nTerminal=true\nIcon=btop\nCategories=System;Monitor;\n",
    "hidden.desktop": "Name=Hidden\nExec=x\nNoDisplay=true\n",
    "kdeonly.desktop": "Name=KDE thing\nExec=y\nOnlyShowIn=KDE;\n",
    "missing.desktop": "Name=Gone\nExec=gone\nTryExec=/nonexistent/gone\n",
    "steam.desktop": "Name=Steam\nExec=steam %U\nIcon=steam\nCategories=Game;\nKeywords=games;valve;\n",
}
for f, body in DESKTOP.items():
    with open(os.path.join(apps_dir, f), "w") as fh:
        fh.write("[Desktop Entry]\nType=Application\n" + body)
os.environ["XDG_CURRENT_DESKTOP"] = "Hyprland"

import auxo_shell as S  # noqa: E402

apps = S.load_apps([os.path.join(HOME, "share")])
names = [a["name"] for a in apps]
check("apps parsed, sorted, hidden/other-desktop/missing skipped", names == ["btop", "Firefox", "GIMP", "Steam"])
by = {a["name"]: a for a in apps}
check("categories", by["GIMP"]["group"] == "Creativity" and by["Steam"]["group"] == "Entertainment"
      and by["Firefox"]["group"] == "Social" and by["btop"]["group"] == "Utilities")
check("search by keyword and comment", S.matches(by["Steam"], "valve") and S.matches(by["GIMP"], "edit pic")
      and not S.matches(by["GIMP"], "steam"))
check("field codes stripped", S.command(by["Firefox"]) == ["firefox"])
check("terminal apps open in a terminal", S.command(by["btop"])[-2:] == ["-e", "btop"])

# recent files
doc = os.path.join(HOME, "notes.md")
open(doc, "w").close()
os.makedirs(os.path.join(HOME, ".local/share"), exist_ok=True)
with open(os.path.join(HOME, ".local/share/recently-used.xbel"), "w") as fh:
    fh.write(f'<?xml version="1.0"?><xbel><bookmark href="file://{doc}" modified="2026-11-20T10:00:00Z"/>'
             f'<bookmark href="file:///nonexistent/x.txt" modified="2026-11-21T10:00:00Z"/></xbel>')
rec = S.recent_files()
check("recent files: existing only", [r["name"] for r in rec] == ["notes.md"])

# ── the launcher ──
from PyQt6.QtWidgets import QApplication  # noqa: E402

app = QApplication(sys.argv[:1])
L = load("auxo-launcher")
mode, pal, font = S.look()
lp = L.MacLauncher(apps, mode, pal, font)
check("Launchpad lists every app", lp.grid.count() == 4)
lp.search.setText("gimp")
check("Launchpad search filters", lp.grid.count() == 1 and lp.grid.item(0).text() == "GIMP")
lp.search.setText("")
for b in lp.group.buttons():
    if b.text() == "Entertainment":
        b.click()
check("Launchpad category chips filter", lp.grid.count() == 1 and lp.grid.item(0).text() == "Steam")

sm = L.StartMenu(apps, mode, pal, font)
pins = [sm.pinned.item(i).text() for i in range(sm.pinned.count())]
check("Start menu: default pins that are installed", pins == ["Firefox", "Steam", "GIMP", "btop"])
L.save_pinned(["gimp.desktop", "steam.desktop"])
check("Start menu: pins saved and read back", [a["name"] for a in L.load_pinned(apps)] == ["GIMP", "Steam"])
sm.search.setText("fire")
check("Start menu search", sm.stack.currentIndex() == 2 and sm.results.item(0).text() == "Firefox")
sm.search.setText("")
check("Start menu back to Pinned", sm.stack.currentIndex() == 0)

# ── the control center ──
C = load("auxo-control")
calls = []


class FakeBackend:
    def wifi(self): return True, "HomeNet"
    def set_wifi(self, on): calls.append(("wifi", on))
    def bluetooth(self): return False
    def set_bluetooth(self, on): calls.append(("bt", on))
    def airplane(self): return False
    def set_airplane(self, on): calls.append(("air", on))
    def focus(self): return None                      # mako not running: tile greyed out
    def set_focus(self, on): calls.append(("focus", on))
    def dark(self): return False
    def night(self): return False
    def gaming(self): return False
    def set_gaming(self, on): calls.append(("gaming", on))
    def set_night(self, on): calls.append(("night", on))
    def set_dark(self, on): calls.append(("dark", on))
    def brightness(self): return None                 # desktop: no backlight
    def set_brightness(self, pct): calls.append(("bright", pct))
    def volume(self): return 40, False
    def set_volume(self, pct): calls.append(("vol", pct))
    def toggle_mute(self): calls.append(("mute",))
    def playing(self): return {"title": "Song", "artist": "Band", "status": "Playing", "art": ""}
    def media(self, what): calls.append(("media", what))
    def battery(self): return 80, True


for style in ("mac", "windows"):
    calls.clear()
    w = C.Control(style, be=FakeBackend())
    w.show()
    app.processEvents()
    check(f"{style}: wifi on", w.toggles["wifi"].isChecked())
    check(f"{style}: focus greyed out without mako", not w.toggles["focus"].isEnabled())
    check(f"{style}: no brightness slider without a backlight", w.bright_box.isHidden())
    check(f"{style}: volume shown", w.vol.value() == 40)
    w.toggles["bt"].click()
    check(f"{style}: bluetooth toggle calls the backend", ("bt", True) in calls)
    w.toggles["gaming"].click()
    check(f"{style}: gaming mode toggle", ("gaming", True) in calls)
    w.toggles["night"].click()
    check(f"{style}: night light toggle", ("night", True) in calls)
    w.vol.setValue(70)
    check(f"{style}: volume slider", ("vol", 70) in calls)
    if style == "mac":
        check("mac: SSID under Wi-Fi", w.ssid.text() == "HomeNet")
        check("mac: now playing", w.song.text() == "Song")
    else:
        check("windows: battery in the footer", "80%" in w.batt.text())
        check("windows: panel fits its content", w.height() < 400)
    w.close()

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
