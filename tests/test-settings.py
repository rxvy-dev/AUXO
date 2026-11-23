#!/usr/bin/env python3
"""Opens Auxo Settings offscreen against a fake root and clicks through it (needs PyQt6).

  python3 tests/test-settings.py [--shots DIR]
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(HERE, "packages/auxo-tools/files")
passed = failed = 0


def check(label, ok):
    global passed, failed
    print(f"  {'\033[32m✓' if ok else '\033[31m✗'}\033[0m {label}")
    passed, failed = passed + bool(ok), failed + (not ok)


print("▲ Auxo Settings (offscreen)")
try:
    import PyQt6  # noqa: F401
except ImportError:
    print("  (PyQt6 not installed — skipping: pip install PyQt6)")
    sys.exit(0)

root = tempfile.mkdtemp()
home = tempfile.mkdtemp()
os.makedirs(f"{root}/etc/auxo")
open(f"{root}/etc/auxo/auxo.conf", "w").write('ACCENT="emerald"\nDESKTOP="hyprland"\nKERNEL="linux"\nFIREWALL="on"\n')
shutil.copytree(f"{PKG}/share/wallpapers", f"{root}/usr/share/wallpapers")
# a stand-in for auxo-tweak and pkexec that records what the app asks for
calls = os.path.join(home, "calls")
bindir = tempfile.mkdtemp()
fake = os.path.join(bindir, "fake-tweak")
open(fake, "w").write(f'#!/bin/sh\necho "$@" >> {calls}\necho ":: working"\n')
os.chmod(fake, 0o755)
pk = os.path.join(bindir, "fake-pkexec")
open(pk, "w").write(f'#!/bin/sh\nshift\necho "root $@" >> {calls}\n')
os.chmod(pk, 0o755)
os.environ.update(QT_QPA_PLATFORM="offscreen", AUXO_ROOT=root, AUXO_HOME=home, AUXO_LIB=f"{PKG}/lib",
                  AUXO_TWEAK=fake, AUXO_PKEXEC=pk)
sys.path.insert(0, f"{PKG}/lib")
import importlib.machinery  # noqa: E402
import importlib.util  # noqa: E402
loader = importlib.machinery.SourceFileLoader("auxo_settings", f"{PKG}/bin/auxo-settings")
spec = importlib.util.spec_from_loader("auxo_settings", loader)
S = importlib.util.module_from_spec(spec)
loader.exec_module(S)
from PyQt6.QtCore import QEventLoop, QTimer  # noqa: E402
from PyQt6.QtWidgets import QApplication, QCheckBox, QPushButton  # noqa: E402

app = QApplication(sys.argv)
win = S.Settings()
win.resize(1100, 800)
win.show()
shots = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None


def wait(ms=300):
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()


def wait_proc():
    for _ in range(50):
        if not win.proc or win.proc.state() == win.proc.ProcessState.NotRunning:
            return
        wait(100)


def shot(name):
    if shots:
        os.makedirs(shots, exist_ok=True)
        win.grab().save(os.path.join(shots, f"settings-{name}.png"))


def log():
    return open(calls).read() if os.path.exists(calls) else ""


check("opens with four pages", win.nav.count() == 4 and win.pages.count() == 4)
check("reads the accent from auxo.conf", S.accent_hex() == S.ACCENTS["emerald"])
names = [b.toolTip() for b in win.swatches.buttons()]
check("one swatch per accent, current one ticked", names == list(S.ACCENTS) and
      [b for b in win.swatches.buttons() if b.isChecked()][0].toolTip() == "emerald")
check("wallpaper grid lists the Auxo wallpapers", len(win.wall_btns.buttons()) >= 8)
wait(); shot("look")

win.swatches.buttons()[0].click(); wait_proc()
check("accent swatch runs pkexec auxo-tweak accent", f"root accent {names[0]}" in log())

for i, name in ((1, "windows"), (2, "desktop"), (3, "system")):
    win.nav.setCurrentRow(i); wait(); shot(name)

win.nav.setCurrentRow(1)
win.s_gaps.setValue(20); win.s_round.setValue(4)
check("preview follows the sliders at once", win.preview.look["gaps"] == "20" and win.preview.look["rounding"] == "4")
wait(700); wait_proc()
check("slider changes are batched into one look set", "look set gaps=20 rounding=4" in log())
before = log().count("look set")
win.set_look("bar", "bottom", now=True); wait_proc()
check("bar position applies right away", "look set bar=bottom" in log() and log().count("look set") == before + 1)
win.set_look("mode", "light", now=True); wait_proc(); wait(); shot("windows-light")
check("light mode restyles the app", "#f5f5f8" in win.styleSheet())

win.nav.setCurrentRow(2); wait()
cards = [w for w in win.findChildren(QPushButton) if w.text().startswith(("Switch to", "Re-apply"))]
check("a card for every desktop, Chrono included", len(cards) == 8 and any("Chrono" in c.text() for c in cards))
check("the current desktop offers to re-apply its rice", any(c.text() == "Re-apply the Auxo rice" for c in cards))
next(c for c in cards if c.text() == "Re-apply the Auxo rice").click(); wait_proc()
check("re-apply runs pkexec auxo-tweak rice hyprland", "root rice hyprland" in log())

win.nav.setCurrentRow(3); wait()
fw = next(b for b in win.findChildren(QCheckBox) if b.text() == "Firewall")
check("switches show the current state", fw.isChecked())
fw.setChecked(False); wait_proc()
check("a switch runs pkexec auxo-tweak firewall off", "root firewall off" in log())
check("app bundles are listed", len(win.app_boxes) >= 8)
win.app_boxes[0].setChecked(True); win.install_apps(); wait_proc()
check("installing apps runs pkexec auxo-tweak apps", f"root apps {win.app_boxes[0].property('bundle')}" in log())
check("status line reports success", win.status.text().startswith("✓"))

desk = subprocess.run(["desktop-file-validate", f"{PKG}/applications/auxo-settings.desktop"], capture_output=True,
                      text=True) if shutil.which("desktop-file-validate") else None
check("has a menu entry", os.path.exists(f"{PKG}/applications/auxo-settings.desktop") and
      (desk is None or desk.returncode == 0))
print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
