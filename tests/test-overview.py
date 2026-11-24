#!/usr/bin/env python3
"""auxo-overview: window lists from fake Hyprland / Sway JSON, and the overview offscreen
(filter, jump, close, workspace switch)."""
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
import auxo_windows as W  # noqa: E402

passed = failed = 0


def check(name, cond):
    global passed, failed
    passed += bool(cond)
    failed += not cond
    if not cond:
        print("  ✗", name)


HYPR = {
    "clients": [
        {"address": "0xa1", "title": "Firefox — News", "class": "firefox", "workspace": {"id": 1, "name": "1"},
         "at": [10, 40], "size": [900, 600], "mapped": True, "hidden": False},
        {"address": "0xa2", "title": "kitty", "class": "kitty", "workspace": {"id": 2, "name": "2"},
         "at": [10, 40], "size": [900, 600], "mapped": True},
        {"address": "0xa3", "title": "Spotify", "class": "Spotify", "workspace": {"id": -98, "name": "special:magic"},
         "at": [0, 0], "size": [800, 500], "mapped": True},
    ],
    "monitors": [{"name": "DP-1", "focused": True, "activeWorkspace": {"id": 1, "name": "1"}}],
    "workspaces": [{"id": 2, "name": "2"}, {"id": 1, "name": "1"}, {"id": -98, "name": "special:magic"}],
}
h = W.Hyprland(query=lambda what: json.dumps(HYPR[what]))
wins = {w["id"]: w for w in h.windows()}
check("Hyprland: all windows incl. minimised", set(wins) == {"0xa1", "0xa2", "0xa3"})
check("Hyprland: only the shown workspace is visible", wins["0xa1"]["visible"] and not wins["0xa2"]["visible"])
check("Hyprland: workspaces in order, active one", h.workspaces() == (["1", "2"], "1"))

TREE = {"type": "root", "nodes": [{"type": "output", "name": "HEADLESS-1", "nodes": [
    {"type": "workspace", "name": "1", "nodes": [
        {"type": "con", "id": 11, "name": "Notes", "app_id": "foot", "rect": {"x": 0, "y": 0, "width": 600, "height": 700},
         "nodes": []},
        {"type": "con", "id": 12, "nodes": [
            {"type": "con", "id": 13, "name": "Files", "app_id": "thunar", "rect": {"x": 600, "y": 0, "width": 600,
                                                                                   "height": 700}, "nodes": []}]}],
     "floating_nodes": []},
    {"type": "workspace", "name": "2", "nodes": [
        {"type": "con", "id": 21, "name": "Game", "window_properties": {"class": "steam_app_1"}, "window": 5,
         "rect": {"x": 0, "y": 0, "width": 1200, "height": 700}, "nodes": []}], "floating_nodes": []}]}]}
WS = [{"name": "1", "visible": True, "focused": True}, {"name": "2", "visible": False, "focused": False}]
s = W.Sway(tree=lambda: json.dumps(TREE), workspaces=lambda: json.dumps(WS))
sw = {w["id"]: w for w in s.windows()}
check("Sway: nested windows found, containers skipped", set(sw) == {11, 13, 21})
check("Sway: X11 apps named by class", sw[21]["app"] == "steam_app_1")
check("Sway: visibility from get_workspaces", sw[11]["visible"] and sw[13]["visible"] and not sw[21]["visible"])
check("Sway: workspaces", s.workspaces() == (["1", "2"], "1"))

# ── the overview ──
from PyQt6.QtGui import QColor, QImage  # noqa: E402
from PyQt6.QtWidgets import QApplication  # noqa: E402

loader = importlib.machinery.SourceFileLoader("auxo_overview", os.path.join(PKG, "bin/auxo-overview"))
spec = importlib.util.spec_from_loader(loader.name, loader)
O = importlib.util.module_from_spec(spec)
loader.exec_module(O)
app = QApplication(sys.argv[:1])
calls = []
s.focus = lambda wid: calls.append(("focus", wid))
s.close = lambda wid: calls.append(("close", wid))
s.go_workspace = lambda name: calls.append(("ws", name))
shot = QImage(1200, 700, QImage.Format.Format_RGB32)
shot.fill(QColor("#336699"))
ov = O.Overview(s, shot)
ov.resize(1400, 900)
ov.build()
check("cards for every window", len(ov.cards) == 3)
vis = [c for c in ov.cards if c.win["id"] == 11][0]
off = [c for c in ov.cards if c.win["id"] == 21][0]
check("visible windows get a thumbnail, others an icon", vis.pix is not None and not vis.pix.isNull() and off.pix is None)
ov.search.setText("game")
check("typing filters", [c.win["id"] for c in ov.cards] == [21])
ov.search.returnPressed.emit()
check("Enter jumps to the window", calls[-1] == ("focus", 21))
ov = O.Overview(s, shot)
ov.build()
ov.close_window(ov.cards[0].win)
check("× closes the window and drops its card", calls[-1][0] == "close" and len(ov.cards) == 2)
ov.go("2")
check("workspace button switches", calls[-1] == ("ws", "2"))
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
