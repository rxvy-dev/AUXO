"""Sound devices and app volumes for auxo-sound, through pactl (works on PipeWire via
pipewire-pulse). `runner` is swappable so tests can feed fake pactl output."""
import json
import re
import subprocess


def _run(argv, timeout=4):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""


def _volume(obj):
    """Average volume % of a pactl JSON object (sink/source/sink-input)."""
    vols = []
    for ch in (obj.get("volume") or {}).values():
        m = re.match(r"\s*(\d+)%", str(ch.get("value_percent", "")))
        if m:
            vols.append(int(m.group(1)))
    return round(sum(vols) / len(vols)) if vols else 0


def _nice(name, desc):
    """A friendlier device name: 'Family 17h HD Audio Controller Analog Stereo' → 'Analog Stereo'..."""
    d = desc or name
    d = re.sub(r"^(Built-in Audio|.*?HD Audio Controller|.*?High Definition Audio Controller)\s*", "Built-in ", d)
    return d.strip() or name


class Audio:
    def __init__(self, runner=None):
        self.run = runner or _run

    def _list(self, what):
        rc, out = self.run(["pactl", "--format=json", "list", what])
        try:
            return json.loads(out) if rc == 0 and out.strip() else []
        except ValueError:
            return []

    def _default(self, what):
        rc, out = self.run(["pactl", f"get-default-{what}"])
        return out.strip() if rc == 0 else ""

    def outputs(self):
        default = self._default("sink")
        return [{"name": s["name"], "label": _nice(s["name"], s.get("description")), "volume": _volume(s),
                 "muted": bool(s.get("mute")), "default": s["name"] == default,
                 "headphones": "headphone" in json.dumps(s.get("ports", [])).lower()
                 and "headphone" in str(s.get("active_port", "")).lower()}
                for s in self._list("sinks")]

    def inputs(self):
        default = self._default("source")
        return [{"name": s["name"], "label": _nice(s["name"], s.get("description")), "volume": _volume(s),
                 "muted": bool(s.get("mute")), "default": s["name"] == default}
                for s in self._list("sources") if not s["name"].endswith(".monitor")
                and (s.get("monitor_source") in (None, "", "n/a") or s.get("properties", {}).get("device.class") != "monitor")]

    def apps(self):
        out = []
        for a in self._list("sink-inputs"):
            p = a.get("properties", {})
            name = p.get("application.name") or p.get("media.name") or "App"
            if p.get("application.id") == "org.PulseAudio.pavucontrol":
                continue
            out.append({"index": a["index"], "label": name, "detail": p.get("media.name", ""),
                        "icon": p.get("application.icon_name") or p.get("application.process.binary") or "audio-x-generic",
                        "volume": _volume(a), "muted": bool(a.get("mute"))})
        return out

    # ── changes ──
    def set_default(self, kind, name):
        self.run(["pactl", f"set-default-{kind}", name])
        if kind == "sink":                     # move what's playing to the new output too
            for a in self.apps():
                self.run(["pactl", "move-sink-input", str(a["index"]), name])

    def set_volume(self, kind, target, pct):
        cmd = {"sink": "set-sink-volume", "source": "set-source-volume", "app": "set-sink-input-volume"}[kind]
        self.run(["pactl", cmd, str(target), f"{max(0, min(150, int(pct)))}%"])

    def set_mute(self, kind, target, muted):
        cmd = {"sink": "set-sink-mute", "source": "set-source-mute", "app": "set-sink-input-mute"}[kind]
        self.run(["pactl", cmd, str(target), "1" if muted else "0"])
