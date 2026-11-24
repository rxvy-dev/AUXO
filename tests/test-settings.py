#!/usr/bin/env python3
"""Opens Auxo Settings offscreen against a fake root and clicks through it (needs PyQt6).

  python3 tests/test-settings.py [--shots DIR]
"""
import json
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
open(f"{root}/etc/auxo/auxo.conf", "w").write('ACCENT="emerald"\nDESKTOP="hyprland"\nKERNEL="linux"\nFIREWALL="on"\nAI="on"\n')
shutil.copytree(f"{PKG}/share/wallpapers", f"{root}/usr/share/wallpapers")
# a stand-in for auxo-tweak and pkexec that records what the app asks for
calls = os.path.join(home, "calls")
bindir = tempfile.mkdtemp()
fake = os.path.join(bindir, "fake-tweak")
MONS = json.dumps({"source": "hyprland", "desktop": "hyprland", "saved": {}, "detected": [
    {"name": "DP-1", "desc": "LG 27GP850", "enabled": True, "width": 2560, "height": 1440, "refresh": 144, "x": 0, "y": 0,
     "scale": 1, "rotate": "0", "vrr": False, "modes": ["2560x1440@144", "2560x1440@60", "1920x1080@60"]},
    {"name": "HDMI-A-1", "desc": "Dell", "enabled": True, "width": 1920, "height": 1080, "refresh": 60, "x": 0, "y": 0,
     "scale": 1, "rotate": "0", "vrr": False, "modes": ["1920x1080@60"]}]})
DOCTOR = ('[{"id": "login-vt", "title": "Login screen has its own console", "problem": "shares tty1", '
          '"fixable": true, "hint": "moves it"}, {"id": "disk", "title": "Free space on /", "problem": null, '
          '"fixable": true, "hint": ""}]')
open(fake, "w").write("#!/bin/sh\n"
                      f'echo "$@" >> {calls}\n'
                      f"if [ \"$1 $2\" = \"doctor --json\" ]; then echo '{DOCTOR}'; exit 0; fi\n"
                      f"if [ \"$1 $2\" = \"monitors --json\" ]; then echo '{MONS}'; exit 0; fi\n"
                      'echo ":: working"\n')
os.chmod(fake, 0o755)
ai = os.path.join(bindir, "fake-ai")
open(ai, "w").write("#!/bin/sh\n"
                    f"cat > {home}/ai-in.json\n"
                    "printf 'Turn on Bluetooth with:\\n\\n```\\nsudo auxo-tweak service bluetooth on\\n```\\n'\n")
os.chmod(ai, 0o755)
pk = os.path.join(bindir, "fake-pkexec")
open(pk, "w").write(f'#!/bin/sh\nshift\necho "root $@" >> {calls}\n')
os.chmod(pk, 0o755)
os.environ.update(QT_QPA_PLATFORM="offscreen", AUXO_ROOT=root, AUXO_HOME=home, AUXO_LIB=f"{PKG}/lib",
                  AUXO_TWEAK=fake, AUXO_PKEXEC=pk, AUXO_AI=ai)
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


check("opens with seven pages", win.nav.count() == 7 and win.pages.count() == 7)
check("reads the accent from auxo.conf", S.accent_hex() == S.ACCENTS["emerald"])
names = [b.toolTip() for b in win.swatches.buttons()]
check("one swatch per accent, current one ticked", names == list(S.ACCENTS) and
      [b for b in win.swatches.buttons() if b.isChecked()][0].toolTip() == "emerald")
check("wallpaper grid lists the Auxo wallpapers", len(win.wall_btns.buttons()) >= 8)
wait(); shot("look")

win.swatches.buttons()[0].click(); wait_proc()
check("accent swatch runs pkexec auxo-tweak accent", f"root accent {names[0]}" in log())

for i, name in ((1, "windows"), (2, "desktop"), (3, "displays"), (4, "system")):
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
mac = next((b for b in win.findChildren(QPushButton) if b.text() == "Mac" and b.toolTip().startswith("Liquid Glass")), None)
winb = next((b for b in win.findChildren(QPushButton) if b.text() == "Windows" and b.toolTip().startswith("Taskbar")), None)
check("Hyprland card offers the Windows style", winb is not None)
check("Hyprland card offers the Mac style", mac is not None)
mac.click(); wait_proc()
check("Mac style runs pkexec auxo-tweak rice hyprland-mac", "root rice hyprland-mac" in log())

# Displays
win.nav.setCurrentRow(3); win.load_displays(); wait(800); shot("displays")
check("Displays finds both screens, side by side", set(win.disp) == {"DP-1", "HDMI-A-1"}
      and win.disp["HDMI-A-1"]["x"] == win.disp["DP-1"]["w"])
check("modes listed with refresh rates", any("144 Hz" in win.disp_mode.itemText(i) for i in range(win.disp_mode.count())))
win.canvas.on_select("HDMI-A-1"); win.canvas.sel = "HDMI-A-1"
win.disp_rot.setCurrentIndex(win.disp_rot.findData("90")); win.disp_ws.setText("6-9"); win.disp_ws.textEdited.emit("6-9")
check("rotating swaps width and height", win.disp["HDMI-A-1"]["w"] == 1080 and win.disp["HDMI-A-1"]["h"] == 1920)
check("Apply turns on after a change", win.disp_apply.isEnabled())
win.apply_displays()
for _ in range(30):
    wait_proc(); wait(100)
    if "monitors set HDMI-A-1" in log() and "monitors set DP-1" in log():
        break
wait_proc()
check("Apply saves every screen with auxo-tweak monitors set",
      "monitors set DP-1 mode=highrr scale=auto position=0x0" in log() and "rotate=90" in log()
      and "workspaces=6-9" in log())
check("only the last save reloads the desktop", log().count("--no-apply") == 1)
a = {"name": "A", "x": 0, "y": 0, "w": 100, "h": 50}
b = {"name": "B", "x": 130, "y": 400, "w": 80, "h": 50}
S.snap(b, [a])
check("dropped screens snap against their neighbour", (b["x"], b["y"]) in ((0, 50), (100, 0), (100, 49)))

win.nav.setCurrentRow(4); wait()
fw = next(b for b in win.findChildren(QCheckBox) if b.text() == "Firewall")
check("switches show the current state", fw.isChecked())
fw.setChecked(False); wait_proc()
check("a switch runs pkexec auxo-tweak firewall off", "root firewall off" in log())
check("app bundles are listed", len(win.app_boxes) >= 8)
win.app_boxes[0].setChecked(True); win.install_apps(); wait_proc()
check("installing apps runs pkexec auxo-tweak apps", f"root apps {win.app_boxes[0].property('bundle')}" in log())
check("status line reports success", win.status.text().startswith("✓"))

# search jumps to the right page
win.jump("bluetooth"); wait()
check("search: “bluetooth” opens System → Services", win.nav.currentRow() == 4)
win.jump("gaps"); wait()
check("search: “gaps” opens Windows & bar", win.nav.currentRow() == 1)
win.jump("password"); wait()
check("search: “password” opens Health", win.nav.currentRow() == 5)
win.jump("refresh rate"); wait()
check("search: “refresh rate” opens Displays", win.nav.currentRow() == 3)

# presets + importing a rice
win.nav.setCurrentRow(1); wait()
next(b for b in win.findChildren(QPushButton) if b.text() == "Minimal").click(); wait_proc()
check("a preset runs look preset", "look preset minimal" in log())
rice = os.path.join(home, "friend.auxorice")
open(rice, "w").write("[auxo]\nname = Friend's rice\naccent = rose\n[look]\ngaps = 3\n")
from PyQt6.QtWidgets import QFileDialog, QMessageBox  # noqa: E402
QFileDialog.getOpenFileName = staticmethod(lambda *a, **k: (rice, ""))
QMessageBox.question = staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes)
win.import_rice(); wait_proc(); wait(300); wait_proc()
check("importing a rice runs look import, then offers its accent", f"look import {rice}" in log() and "root accent rose" in log())

# health
win.nav.setCurrentRow(5); win.check_health(); wait(800); shot("health")
check("health lists problems and the fix", "shares tty1" in win.health_box.itemAt(0).widget().text() and win.fix_btn.isEnabled())
check("sidebar shows how many to fix", "1 to fix" in win.nav.item(5).text())
win.fix_btn.click(); wait_proc()
check("Fix everything runs pkexec auxo-tweak doctor --fix", "root doctor --fix" in log())

# Ask Auxo
win.nav.setCurrentRow(6); wait()
check("AI on: no setup card, chat ready", not win.ai_setup.isVisible() and win.ask.isEnabled())
win.ask.setText("how do I turn on bluetooth?"); win.send_question()
for _ in range(40):
    wait(100)
    if win.ai_proc.state() == win.ai_proc.ProcessState.NotRunning:
        break
wait(200); shot("ask-auxo")
check("the question goes to auxo-ai", "bluetooth" in open(os.path.join(home, "ai-in.json")).read())
check("the answer is shown", "Turn on Bluetooth" in win.chat_view.toPlainText())
check("suggested commands become Run links", "Run: auxo-tweak service bluetooth on" in win.chat_view.toPlainText())
win.run_suggested("auxo-tweak service bluetooth on"); wait_proc()
check("running a suggestion uses pkexec", "root service bluetooth on" in log())
win.run_suggested("auxo-tweak look preset glass"); wait_proc()
check("look suggestions run without a password", "\nlook preset glass" in "\n" + log())

desk = subprocess.run(["desktop-file-validate", f"{PKG}/applications/auxo-settings.desktop"], capture_output=True,
                      text=True) if shutil.which("desktop-file-validate") else None
check("has a menu entry", os.path.exists(f"{PKG}/applications/auxo-settings.desktop") and
      (desk is None or desk.returncode == 0))
print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
