#!/usr/bin/env python3
"""auxo-store: xbps/flatpak output parsing, the commands it runs, and the window offscreen
(search, install through a fake pkexec, button states, the Flathub prompt)."""
import importlib.machinery
import importlib.util
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(HERE, "packages/auxo-tools/files")
sys.path.insert(0, os.path.join(PKG, "lib"))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["AUXO_LIB"] = os.path.join(PKG, "lib")
os.environ["AUXO_HOME"] = tempfile.mkdtemp()
import auxo_store  # noqa: E402
from auxo_store import Store  # noqa: E402

passed = failed = 0


def check(name, cond):
    global passed, failed
    passed += bool(cond)
    failed += not cond
    if not cond:
        print("  ✗", name)


state = {"void": {"firefox"}, "flat": {"com.discordapp.Discord": "Discord"}, "flathub": True}


def run(argv, timeout=60):
    if argv == ["xbps-query", "-l"]:
        return 0, "".join(f"ii {n}-1.0_1  something\n" for n in sorted(state["void"]))
    if argv[:2] == ["xbps-query", "-Rs"]:
        return 0, ("[-] minetest-5.10.0_1    Infinite-world block sandbox game\n"
                   "[-] minetest-devel-5.10.0_1  headers\n"
                   "[*] firefox-145.0_1  Mozilla Firefox web browser\n")
    if argv[:2] == ["flatpak", "remotes"]:
        return 0, "flathub\n" if state["flathub"] else ""
    if argv[:3] == ["flatpak", "list", "--app"]:
        return 0, "".join(f"{i}\t{n}\t1.0\n" for i, n in state["flat"].items())
    if argv[:2] == ["flatpak", "search"]:
        return 0, "org.prismlauncher.PrismLauncher\tPrism Launcher\tA launcher for Minecraft\n"
    return 1, ""


s = Store(runner=run, have=lambda p: True)
check("installed Void packages parsed (name, not version)", s.void_installed() == {"firefox"})
check("installed Flathub apps", s.flat_installed() == {"com.discordapp.Discord": "Discord"})
cat = dict(s.catalog())
ff = [a for a in cat["Popular"] if a["name"] == "Firefox"][0]
dc = [a for a in cat["Popular"] if a["name"] == "Discord"][0]
check("catalog marks what's installed", ff["installed"] and dc["installed"])
res = s.search("mine")
names = [a["name"] for a in res]
check("search: Void + Flathub, no -devel", "minetest" in names and "Prism Launcher" in names
      and "minetest-devel" not in names)
check("search: installed flag from xbps [*]", [a for a in res if a["name"] == "firefox"][0]["installed"])
check("Void install = pkexec xbps-install -Sy", s.command({"id": "gimp", "source": "void"}, "install", pkexec="pk")
      == ["pk", "xbps-install", "-Sy", "gimp"])
check("Void remove = pkexec xbps-remove -Ry", s.command({"id": "gimp", "source": "void"}, "remove", pkexec="pk")
      == ["pk", "xbps-remove", "-Ry", "gimp"])
check("Flathub install = flatpak install flathub ID", s.command({"id": "a.b.C", "source": "flathub"}, "install")
      == ["flatpak", "install", "-y", "--noninteractive", "flathub", "a.b.C"])
check("Steam = auxo-tweak gaming on", s.command({"id": "gaming on", "source": "auxo"}, "install", pkexec="pk",
                                                tweak="tw") == ["pk", "tw", "gaming", "on"])
voidnames = [pid for _c, apps in auxo_store.CATALOG for pid, src, *_ in apps if src == "void"]
known = "/tmp/void-pkgs.txt"
if os.path.exists(known):
    allp = set(open(known).read().split())
    check("every catalog Void package exists in Void", all(p in allp for p in voidnames))

# ── the window ──
from PyQt6.QtWidgets import QApplication, QMessageBox  # noqa: E402

bindir = tempfile.mkdtemp()
pk = os.path.join(bindir, "fake-pkexec")
calls = os.path.join(bindir, "calls")
open(pk, "w").write(f'#!/bin/sh\necho "$@" >> {calls}\necho "installing $3"\n')
os.chmod(pk, 0o755)
os.environ["AUXO_PKEXEC"] = pk
loader = importlib.machinery.SourceFileLoader("auxo_store_app", os.path.join(PKG, "bin/auxo-store"))
spec = importlib.util.spec_from_loader(loader.name, loader)
A = importlib.util.module_from_spec(spec)
loader.exec_module(A)
app = QApplication(sys.argv[:1])
w = A.StoreWindow(store=s)
check("Discover lists the catalog", len(w.cards) == sum(len(a) for _c, a in auxo_store.CATALOG))
gimp = [c for c in w.cards if c.app["id"] == "gimp"][0]
check("not installed → Install button", gimp.btn.text() == "Install")


def wait(cond, secs=8):
    end = time.time() + secs
    while time.time() < end and not cond():
        app.processEvents()
        time.sleep(0.02)
    return cond()


real_cmd = s.command


def install_and_mark(a, action, **kw):
    state["void"].add(a["id"]) if action == "install" else state["void"].discard(a["id"])
    return real_cmd(a, action, **kw)


s.command = install_and_mark
gimp.btn.click()
check("installing shows Working…", gimp.btn.text() == "Working…" and not gimp.btn.isEnabled())
check("…then Remove when done", wait(lambda: gimp.btn.text() == "Remove"))
check("ran pkexec xbps-install -Sy gimp", "xbps-install -Sy gimp" in open(calls).read())
check("status says where to find it", "find it in your app menu" in w.status.text())
A.QMessageBox.question = lambda *a, **k: QMessageBox.StandardButton.Yes
gimp.btn.click()
check("remove asks, runs xbps-remove, flips back to Install",
      wait(lambda: gimp.btn.text() == "Install") and "xbps-remove -Ry gimp" in open(calls).read())

w.search.setText("mine")
w.do_search()
check("search page fills in the background", wait(lambda: any(c.app["name"] == "Prism Launcher" for c in w.cards
                                                                if not c.isHidden())))
state["flathub"] = False
w.check_flathub()
check("no Flathub: offers to add it", not w.flathub_btn.isHidden())
shown = []
A.QMessageBox.information = lambda *a, **k: shown.append(a[2])
spot = [c for c in w.cards if c.app["name"] == "Prism Launcher"][0]
spot.btn.click()
check("Flathub app without Flathub: says to add it first", shown and "Add Flathub first" in shown[0])
w.proc = None
w.close()
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
