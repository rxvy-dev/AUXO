"""Open windows and workspaces from the running window manager (Hyprland, Sway, i3), for
auxo-overview. Each backend returns plain dicts, so tests can feed it fake JSON."""
import json
import os
import shutil
import subprocess


def _run(argv, timeout=3):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return r.stdout if r.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


class Hyprland:
    name = "hyprland"

    def __init__(self, query=None):
        self.query = query or (lambda what: _run(["hyprctl", what, "-j"]))

    def windows(self):
        """[{id, title, app, workspace, x, y, w, h, visible}]"""
        try:
            clients = json.loads(self.query("clients") or "[]")
            mons = json.loads(self.query("monitors") or "[]")
        except ValueError:
            return []
        shown = {m.get("activeWorkspace", {}).get("id") for m in mons}
        out = []
        for c in clients:
            ws = c.get("workspace", {})
            if not c.get("mapped", True) or ws.get("id", 0) < 0 and not ws.get("name", "").startswith("special"):
                continue
            (x, y), (w, h) = c.get("at", [0, 0]), c.get("size", [0, 0])
            out.append({"id": c.get("address"), "title": c.get("title") or c.get("class", ""),
                        "app": c.get("class") or c.get("initialClass", ""), "workspace": ws.get("name") or str(ws.get("id")),
                        "ws_id": ws.get("id", 0), "x": x, "y": y, "w": w, "h": h,
                        "visible": ws.get("id") in shown and not c.get("hidden")})
        return out

    def workspaces(self):
        try:
            mons = json.loads(self.query("monitors") or "[]")
            wss = json.loads(self.query("workspaces") or "[]")
        except ValueError:
            return [], None
        active = next((m.get("activeWorkspace", {}).get("name") for m in mons if m.get("focused")), None)
        names = sorted((w.get("name") for w in wss if w.get("id", 0) > 0), key=lambda n: (not n.isdigit(), int(n) if n.isdigit() else 0, n))
        return names, active

    def focus(self, wid):
        _run(["hyprctl", "dispatch", "focuswindow", f"address:{wid}"])

    def close(self, wid):
        _run(["hyprctl", "dispatch", "closewindow", f"address:{wid}"])

    def go_workspace(self, name):
        _run(["hyprctl", "dispatch", "workspace", name])


class Sway:
    name = "sway"
    cmd = "swaymsg"

    def __init__(self, tree=None, workspaces=None):
        self.tree = tree or (lambda: _run([self.cmd, "-t", "get_tree"]))
        self.wsq = workspaces or (lambda: _run([self.cmd, "-t", "get_workspaces"]))

    def _walk(self, node, ws=None, visible=False, out=None, shown=()):
        out = [] if out is None else out
        if node.get("type") == "workspace":
            ws, visible = node.get("name"), node.get("name") in shown
            if ws == "__i3_scratch":
                ws, visible = "scratchpad", False
        if node.get("type") in ("con", "floating_con") and not node.get("nodes") and (node.get("app_id") or node.get("window")):
            r = node.get("rect", {})
            props = node.get("window_properties") or {}
            out.append({"id": node.get("id"), "title": node.get("name") or "", "app": node.get("app_id") or props.get("class", ""),
                        "workspace": ws or "?", "ws_id": 0, "x": r.get("x", 0), "y": r.get("y", 0), "w": r.get("width", 0),
                        "h": r.get("height", 0), "visible": bool(visible)})
        for child in node.get("nodes", []) + node.get("floating_nodes", []):
            self._walk(child, ws, visible, out, shown)
        return out

    def _workspaces(self):
        try:
            return json.loads(self.wsq() or "[]")
        except ValueError:
            return []

    def windows(self):
        # the tree doesn't say which workspaces are on screen; get_workspaces does
        shown = {w["name"] for w in self._workspaces() if w.get("visible")}
        try:
            return self._walk(json.loads(self.tree() or "{}"), shown=shown)
        except ValueError:
            return []

    def workspaces(self):
        wss = self._workspaces()
        return [w["name"] for w in wss], next((w["name"] for w in wss if w.get("focused")), None)

    def focus(self, wid):
        _run([self.cmd, f"[con_id={wid}] focus"])

    def close(self, wid):
        _run([self.cmd, f"[con_id={wid}] kill"])

    def go_workspace(self, name):
        _run([self.cmd, "workspace", name])


class I3(Sway):
    name = "i3"
    cmd = "i3-msg"


def detect():
    if os.environ.get("HYPRLAND_INSTANCE_SIGNATURE") and shutil.which("hyprctl"):
        return Hyprland()
    if os.environ.get("SWAYSOCK") and shutil.which("swaymsg"):
        return Sway()
    if os.environ.get("I3SOCK") or (os.environ.get("DISPLAY") and shutil.which("i3-msg")):
        return I3()
    return None


def screenshot(path, wm):
    """The visible screen(s), for thumbnails: grim on Wayland, maim/import on X11."""
    if wm.name in ("hyprland", "sway") and shutil.which("grim"):
        # JPEG is quicker, but not every grim build has it
        return _run(["grim", "-t", "jpeg", "-q", "80", path], timeout=4) is not None \
            or _run(["grim", "-t", "png", "-l", "1", path], timeout=4) is not None
    for argv in (["maim", "-m", "6", path], ["import", "-window", "root", path]):
        if shutil.which(argv[0]):
            return _run(argv, timeout=4) is not None
    return False
