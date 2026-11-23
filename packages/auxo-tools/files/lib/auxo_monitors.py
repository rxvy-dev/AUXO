"""Monitors: one settings file, written into Hyprland, Sway and i3.

~/.config/auxo/monitors.conf holds a section per output:

    [DP-1]
    mode = 2560x1440@144      ; WxH, WxH@Hz, preferred, highrr (highest refresh) or highres
    scale = 1.25
    position = 0x0            ; XxY in pixels, or auto
    rotate = 0                ; 0, 90, 180 or 270
    vrr = off                 ; variable refresh rate (FreeSync / G-Sync)
    enabled = on
    workspaces = 1-5          ; workspaces that open on this monitor (1-5 or 1,3,5)

Outputs that aren't listed get the rice's default: highest refresh rate, placed automatically.
Plasma, GNOME, Xfce, Cinnamon and Chrono have their own display settings and don't read this.
"""
import json
import os
import re
import shutil
import subprocess

from auxo_core import R, write_file

FILE = ".config/auxo/monitors.conf"
KEYS = ("mode", "scale", "position", "rotate", "vrr", "enabled", "workspaces")
DEFAULTS = {"mode": "highrr", "scale": "auto", "position": "auto", "rotate": "0", "vrr": "off", "enabled": "on",
            "workspaces": ""}
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,40}$")


class MonitorError(ValueError):
    pass


# ── settings ──────────────────────────────────────────────────────────
def validate(key, value):
    v = str(value).strip()
    if key == "mode":
        if v in ("preferred", "highrr", "highres") or re.fullmatch(r"\d{3,5}x\d{3,5}(@\d{2,3}(\.\d+)?)?", v):
            return v
        raise MonitorError("mode is WIDTHxHEIGHT or WIDTHxHEIGHT@HZ (like 2560x1440@144), or preferred / highrr")
    if key == "scale":
        if v == "auto":
            return v
        try:
            f = float(v)
        except ValueError:
            raise MonitorError("scale is a number like 1, 1.25 or 2 (or auto)") from None
        if not 0.5 <= f <= 3:
            raise MonitorError("scale goes from 0.5 to 3")
        return f"{f:g}"
    if key == "position":
        if v == "auto" or re.fullmatch(r"-?\d{1,5}x-?\d{1,5}", v):
            return v
        raise MonitorError("position is XxY in pixels (like 2560x0) or auto")
    if key == "rotate":
        if v in ("0", "90", "180", "270"):
            return v
        raise MonitorError("rotate is 0, 90, 180 or 270")
    if key in ("vrr", "enabled"):
        b = {"on": "on", "off": "off", "yes": "on", "no": "off", "true": "on", "false": "off", "1": "on", "0": "off"}
        if v.lower() in b:
            return b[v.lower()]
        raise MonitorError(f"{key} is on or off")
    if key == "workspaces":
        return ",".join(str(n) for n in workspace_list(v)) if v else ""
    raise MonitorError(f"unknown setting {key}. settings: {', '.join(KEYS)}")


def workspace_list(text):
    out = []
    for part in str(text).replace(" ", "").split(","):
        if not part:
            continue
        m = re.fullmatch(r"(\d+)-(\d+)", part)
        nums = range(int(m.group(1)), int(m.group(2)) + 1) if m else [int(part)] if part.isdigit() else None
        if nums is None:
            raise MonitorError("workspaces are numbers like 1-5 or 1,3,5")
        for n in nums:
            if not 1 <= n <= 10:
                raise MonitorError("workspaces go from 1 to 10")
            if n not in out:
                out.append(n)
    return out


def load(home):
    mons, cur = {}, None
    try:
        for line in open(R(f"{home}/{FILE}")):
            line = line.split(";")[0].split("#")[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                name = line[1:-1].strip()
                cur = name if NAME_RE.match(name) else None
                if cur:
                    mons.setdefault(cur, dict(DEFAULTS))
            elif cur and "=" in line:
                k, v = (x.strip() for x in line.split("=", 1))
                if k in KEYS:
                    try:
                        mons[cur][k] = validate(k, v)
                    except MonitorError:
                        pass
    except OSError:
        pass
    return mons


def save(home, mons, owner=None):
    lines = ["# Auxo monitors — change them in Auxo Settings → Displays, or with: auxo-tweak monitors set",
             "# Used by the Auxo Hyprland, Sway and i3 rices. Outputs not listed: highest refresh rate, placed automatically."]
    for name in sorted(mons):
        lines.append(f"\n[{name}]")
        for k in KEYS:
            if mons[name].get(k, DEFAULTS[k]) != DEFAULTS[k]:
                lines.append(f"{k} = {mons[name][k]}")
    write_file(f"{home}/{FILE}", "\n".join(lines) + "\n", owner=owner)


def update(mons, name, changes):
    if not NAME_RE.match(name or ""):
        raise MonitorError(f"“{name}” isn't an output name (they look like DP-1, HDMI-A-1, eDP-1)")
    m = dict(mons.get(name, DEFAULTS))
    for k, v in changes.items():
        m[k] = validate(k, v)
    out = dict(mons)
    out[name] = m
    # a workspace lives on one monitor only
    mine = set(workspace_list(m["workspaces"]))
    if mine:
        for other, om in out.items():
            if other != name and om.get("workspaces"):
                om = dict(om)
                om["workspaces"] = ",".join(str(n) for n in workspace_list(om["workspaces"]) if n not in mine)
                out[other] = om
    still_on = (any(v.get("enabled", "on") == "on" for v in out.values())
                or any(n not in out for n in detected_names_cache))      # unlisted monitors stay on
    if not still_on:
        raise MonitorError("at least one monitor has to stay on")
    return out


detected_names_cache = []   # filled by detect(); used to refuse turning every screen off


# ── writing the window-manager configs ────────────────────────────────
def _mode_parts(mode):
    m = re.fullmatch(r"(\d+)x(\d+)(?:@([\d.]+))?", mode)
    return (int(m.group(1)), int(m.group(2)), m.group(3)) if m else (None, None, None)


def hyprland(mons):
    out = ["# written by Auxo (Auxo Settings → Displays / auxo-tweak monitors) — edit monitors.conf instead"]
    for name in sorted(mons):
        m = mons[name]
        if m["enabled"] == "off":
            out.append(f"monitor = {name}, disable")
            continue
        line = f"monitor = {name}, {m['mode']}, {m['position']}, {m['scale']}"
        if m["rotate"] != "0":
            line += f", transform, {int(m['rotate']) // 90}"
        if m["vrr"] == "on":
            line += ", vrr, 1"
        out.append(line)
        for i, ws in enumerate(workspace_list(m["workspaces"])):
            out.append(f"workspace = {ws}, monitor:{name}" + (", default:true" if i == 0 else ""))
    return "\n".join(out) + "\n"


def sway(mons):
    out = ["# written by Auxo (Auxo Settings → Displays / auxo-tweak monitors) — edit monitors.conf instead"]
    for name in sorted(mons):
        m = mons[name]
        if m["enabled"] == "off":
            out.append(f"output {name} disable")
            continue
        parts = [f"output {name} enable"]
        w, h, hz = _mode_parts(m["mode"])
        if w:
            parts.append(f"mode {w}x{h}" + (f"@{hz}Hz" if hz else ""))
        if m["scale"] != "auto":
            parts.append(f"scale {m['scale']}")
        if m["position"] != "auto":
            x, y = m["position"].split("x")
            parts.append(f"position {x} {y}")
        if m["rotate"] != "0":
            parts.append(f"transform {m['rotate']}")
        parts.append(f"adaptive_sync {m['vrr']}")
        out.append(" ".join(parts))
        out += [f"workspace {ws} output {name}" for ws in workspace_list(m["workspaces"])]
    return "\n".join(out) + "\n"


XRANDR_ROT = {"0": "normal", "90": "right", "180": "inverted", "270": "left"}


def xrandr_script(mons):
    out = ["#!/bin/sh", "# written by Auxo (Auxo Settings → Displays / auxo-tweak monitors) — edit monitors.conf instead",
           "# X11 has no per-monitor scale: use Auxo Settings → Look → font size instead."]
    for name in sorted(mons):
        m = mons[name]
        if m["enabled"] == "off":
            out.append(f"xrandr --output {name} --off")
            continue
        cmd = f"xrandr --output {name}"
        w, h, hz = _mode_parts(m["mode"])
        cmd += f" --mode {w}x{h}" + (f" --rate {hz}" if hz else "") if w else " --auto"
        if m["position"] != "auto":
            cmd += f" --pos {m['position']}"
        cmd += f" --rotate {XRANDR_ROT[m['rotate']]}"
        out.append(cmd + " || true")
    return "\n".join(out) + "\n"


def i3(mons):
    out = ["# written by Auxo (Auxo Settings → Displays / auxo-tweak monitors) — edit monitors.conf instead",
           "exec_always --no-startup-id ~/.config/auxo/monitors-xrandr.sh"]
    for name in sorted(mons):
        if mons[name]["enabled"] == "on":
            out += [f"workspace {ws} output {name}" for ws in workspace_list(mons[name]["workspaces"])]
    return "\n".join(out) + "\n"


def write(home, owner=None):
    """Write the generated files (always, even with no monitors set, so the rices' include lines never break)."""
    mons = load(home)
    cfg = f"{home}/.config/auxo"
    write_file(f"{cfg}/monitors-hyprland.conf", hyprland(mons), owner=owner)
    write_file(f"{cfg}/monitors-sway", sway(mons), owner=owner)
    write_file(f"{cfg}/monitors-i3", i3(mons), owner=owner)
    write_file(f"{cfg}/monitors-xrandr.sh", xrandr_script(mons), mode=0o755, owner=owner)
    return [".config/auxo/monitors-hyprland.conf", ".config/auxo/monitors-sway", ".config/auxo/monitors-i3",
            ".config/auxo/monitors-xrandr.sh"]


HOOKS = [
    (".config/hypr/hyprland.conf", "source = ~/.config/auxo/monitors-hyprland.conf"),
    (".config/sway/config", "include ~/.config/auxo/monitors-sway"),
    (".config/i3/config", "include ~/.config/auxo/monitors-i3"),
]


# ── finding the connected monitors ────────────────────────────────────
def _run(argv, env=None, user=None):
    if user and os.geteuid() == 0:
        argv = ["runuser", "-u", user, "--"] + argv
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=8, env=env)
        return p.stdout if p.returncode == 0 else ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def _mode_str(w, h, hz):
    return f"{w}x{h}@{round(float(hz))}" if hz else f"{w}x{h}"


def _sorted_modes(modes):
    def key(s):
        w, h, hz = _mode_parts(s)
        return (-(w or 0) * (h or 0), -float(hz or 0))
    return sorted(dict.fromkeys(modes), key=key)


def parse_hyprctl(text):
    out = []
    for m in json.loads(text or "[]"):
        modes = []
        for s in m.get("availableModes", []):
            mm = re.fullmatch(r"(\d+)x(\d+)@([\d.]+)Hz", s)
            if mm:
                modes.append(_mode_str(mm.group(1), mm.group(2), mm.group(3)))
        out.append({"name": m["name"], "desc": m.get("description", "").strip() or m.get("model", ""),
                    "enabled": not m.get("disabled", False), "width": m.get("width", 0), "height": m.get("height", 0),
                    "refresh": round(m.get("refreshRate", 0)), "x": m.get("x", 0), "y": m.get("y", 0),
                    "scale": m.get("scale", 1), "rotate": str((m.get("transform", 0) % 4) * 90),
                    "vrr": bool(m.get("vrr")), "modes": _sorted_modes(modes)})
    return out


def parse_sway(text):
    out = []
    for o in json.loads(text or "[]"):
        cm = o.get("current_mode") or {}
        modes = [_mode_str(md["width"], md["height"], md.get("refresh", 0) / 1000) for md in o.get("modes", [])]
        rect = o.get("rect") or {}
        rot = {"normal": "0", "90": "90", "180": "180", "270": "270"}.get(o.get("transform", "normal"), "0")
        out.append({"name": o["name"], "desc": " ".join(x for x in (o.get("make"), o.get("model")) if x and x != "Unknown"),
                    "enabled": o.get("active", False), "width": cm.get("width", 0), "height": cm.get("height", 0),
                    "refresh": round(cm.get("refresh", 0) / 1000), "x": rect.get("x", 0), "y": rect.get("y", 0),
                    "scale": o.get("scale", 1), "rotate": rot, "vrr": o.get("adaptive_sync_status") == "enabled",
                    "modes": _sorted_modes(modes)})
    return out


def parse_xrandr(text):
    out, cur = [], None
    for line in (text or "").splitlines():
        m = re.match(r"^(\S+) (connected|disconnected)( primary)?(?: (\d+)x(\d+)\+(-?\d+)\+(-?\d+))?(?: (left|right|inverted|normal))?", line)
        if m:
            cur = None
            if m.group(2) == "connected":
                rot = {"left": "270", "right": "90", "inverted": "180"}.get(m.group(8) or "", "0")
                cur = {"name": m.group(1), "desc": "", "enabled": bool(m.group(4)), "width": int(m.group(4) or 0),
                       "height": int(m.group(5) or 0), "refresh": 0, "x": int(m.group(6) or 0), "y": int(m.group(7) or 0),
                       "scale": 1, "rotate": rot, "vrr": False, "modes": []}
                out.append(cur)
            continue
        mm = re.match(r"^\s+(\d+)x(\d+)i?\s+(.*)$", line)
        if cur is not None and mm:
            # "  2560x1440    143.97*+  119.99    59.95 +" — * marks the current rate, + the preferred one
            for rate, flags in re.findall(r"([\d.]+)\s*([*+]*)", mm.group(3)):
                cur["modes"].append(_mode_str(mm.group(1), mm.group(2), rate))
                if "*" in flags:
                    cur["refresh"] = round(float(rate))
    for o in out:
        o["modes"] = _sorted_modes(o["modes"])
    return out


def parse_sysfs(root="/sys/class/drm"):
    out = []
    try:
        names = sorted(os.listdir(root))
    except OSError:
        return out
    for d in names:
        m = re.match(r"card\d+-(.+)$", d)
        if not m:
            continue
        try:
            if open(f"{root}/{d}/status").read().strip() != "connected":
                continue
            modes = [l.strip() for l in open(f"{root}/{d}/modes") if l.strip()]
        except OSError:
            continue
        w, h = (int(x) for x in modes[0].split("x")) if modes else (0, 0)
        out.append({"name": m.group(1), "desc": "", "enabled": True, "width": w, "height": h, "refresh": 0, "x": 0,
                    "y": 0, "scale": 1, "rotate": "0", "vrr": False, "modes": _sorted_modes(modes)})
    return out


def detect(env=None, user=None):
    """Connected monitors, from the running session when there is one (it knows refresh rates and
    positions), else from the kernel. → (source, [monitor dicts])"""
    env = env or os.environ
    if env.get("HYPRLAND_INSTANCE_SIGNATURE") and shutil.which("hyprctl"):
        found = parse_hyprctl(_run(["hyprctl", "monitors", "all", "-j"], env, user) or "[]")
        src = "hyprland"
    elif env.get("SWAYSOCK") and shutil.which("swaymsg"):
        found = parse_sway(_run(["swaymsg", "-t", "get_outputs", "-r"], env, user) or "[]")
        src = "sway"
    elif env.get("DISPLAY") and shutil.which("xrandr"):
        found = parse_xrandr(_run(["xrandr", "--query"], env, user))
        src = "xrandr"
    else:
        found, src = [], ""
    if not found:
        found, src = parse_sysfs(R("/sys/class/drm")), "kernel"
    detected_names_cache[:] = [m["name"] for m in found]
    return src, found
