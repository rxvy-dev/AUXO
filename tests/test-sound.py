#!/usr/bin/env python3
"""auxo-sound: pactl JSON parsing, the commands it runs, and the panel offscreen.
  python3 tests/test-sound.py [--shots DIR]"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(HERE, "packages/auxo-tools/files")
sys.path.insert(0, os.path.join(PKG, "lib"))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["AUXO_LIB"] = os.path.join(PKG, "lib")
os.environ["AUXO_HOME"] = tempfile.mkdtemp()
os.environ["AUXO_LAUNCHER_STAY"] = "1"
from auxo_audio import Audio  # noqa: E402

SHOTS = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
passed = failed = 0


def check(name, cond):
    global passed, failed
    passed += bool(cond)
    failed += not cond
    if not cond:
        print("  ✗", name)


def vol(p):
    return {"front-left": {"value_percent": f"{p}%"}, "front-right": {"value_percent": f"{p}%"}}


SINKS = [{"name": "alsa_output.pci.analog-stereo", "description": "Family 17h HD Audio Controller Analog Stereo",
          "volume": vol(64), "mute": False, "ports": [{"name": "analog-output-headphones"}],
          "active_port": "analog-output-headphones"},
         {"name": "alsa_output.hdmi", "description": "RTX 5080 HDMI / DisplayPort 1", "volume": vol(100), "mute": False,
          "ports": [], "active_port": None}]
SOURCES = [{"name": "alsa_output.pci.analog-stereo.monitor", "description": "Monitor of Analog", "volume": vol(100),
            "mute": False},
           {"name": "alsa_input.usb-blue-yeti", "description": "Yeti Stereo Microphone", "volume": vol(80), "mute": True}]
APPS = [{"index": 42, "volume": vol(90), "mute": False,
         "properties": {"application.name": "Firefox", "media.name": "YouTube", "application.icon_name": "firefox"}},
        {"index": 43, "volume": vol(50), "mute": False,
         "properties": {"application.name": "Spotify", "media.name": "Spotify"}}]
calls = []
defaults = {"sink": "alsa_output.pci.analog-stereo", "source": "alsa_input.usb-blue-yeti"}


def run(argv, timeout=4):
    if argv[:3] == ["pactl", "--format=json", "list"]:
        return 0, json.dumps({"sinks": SINKS, "sources": SOURCES, "sink-inputs": APPS}[argv[3]])
    if argv[1].startswith("get-default-"):
        return 0, defaults[argv[1].split("-")[-1]] + "\n"
    calls.append(argv[1:])
    if argv[1].startswith("set-default-"):
        defaults[argv[1].split("-")[-1]] = argv[2]
    return 0, ""


a = Audio(runner=run)
outs = a.outputs()
check("outputs parsed, default marked", [o["default"] for o in outs] == [True, False] and outs[0]["volume"] == 64)
check("friendly device names", outs[0]["label"] == "Built-in Analog Stereo")
check("headphones detected", outs[0]["headphones"] and not outs[1]["headphones"])
ins = a.inputs()
check("microphones without monitor sources", [i["label"] for i in ins] == ["Yeti Stereo Microphone"] and ins[0]["muted"])
apps = a.apps()
check("apps playing", [x["label"] for x in apps] == ["Firefox", "Spotify"] and apps[0]["volume"] == 90)
a.set_volume("sink", "alsa_output.hdmi", 140)
a.set_volume("sink", "alsa_output.hdmi", 999)
check("volume set (capped at 150%)", calls[-2] == ["set-sink-volume", "alsa_output.hdmi", "140%"]
      and calls[-1] == ["set-sink-volume", "alsa_output.hdmi", "150%"])
a.set_mute("app", 43, True)
check("app mute", calls[-1] == ["set-sink-input-mute", "43", "1"])
calls.clear()
a.set_default("sink", "alsa_output.hdmi")
check("switching output moves what's playing", calls[0] == ["set-default-sink", "alsa_output.hdmi"]
      and ["move-sink-input", "42", "alsa_output.hdmi"] in calls and ["move-sink-input", "43", "alsa_output.hdmi"] in calls)
defaults["sink"] = "alsa_output.pci.analog-stereo"

# ── the panel ──
from PyQt6.QtWidgets import QApplication, QPushButton, QSlider  # noqa: E402

loader = importlib.machinery.SourceFileLoader("auxo_sound", os.path.join(PKG, "bin/auxo-sound"))
spec = importlib.util.spec_from_loader(loader.name, loader)
M = importlib.util.module_from_spec(spec)
loader.exec_module(M)
app = QApplication(sys.argv[:1])
for style in ("mac", "windows"):
    calls.clear()
    w = M.Sound(style, audio=Audio(runner=run))
    w.timer.stop()
    w.show()
    app.processEvents()
    sliders = w.findChildren(QSlider)
    check(f"{style}: output, mic and 2 app sliders", len(sliders) == 4)
    devices = [b for b in w.findChildren(QPushButton) if b.objectName() == "device"]
    check(f"{style}: both outputs listed, current ticked", any(b.isChecked() and "Built-in" in b.text() for b in devices)
          and any("HDMI" in b.text() for b in devices))
    w.out_slider.setValue(30)
    check(f"{style}: output slider sets the volume", ["set-sink-volume", "alsa_output.pci.analog-stereo", "30%"] in calls)
    w.mic_mute.setChecked(False)
    check(f"{style}: mic unmute", ["set-source-mute", "alsa_input.usb-blue-yeti", "0"] in calls)
    if SHOTS:
        os.makedirs(SHOTS, exist_ok=True)
        w.grab().save(os.path.join(SHOTS, f"sound-{style}.png"))
    before = len(w.findChildren(QSlider))
    w.refresh()
    check(f"{style}: no rebuild when nothing changed", len(w.findChildren(QSlider)) == before)
    w.close()
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
