#!/usr/bin/env python3
"""Every Auxo rice has the same keys: Hyprland (Auxo, Mac, Windows styles), Sway and i3.
Prints the differences and exits 1 if a key exists in one and not another.
A style may rebind a key to the same job with a different look (Super+A: Launchpad / Start)."""
import os
import re
import sys

R = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "packages/auxo-tools/files/share/auxo/rices")


def hypr(*files):
    keys = {}
    for p in files:
        for line in open(p):
            m = re.match(r"^(bind[a-z]*|unbind)\s*=\s*([^,]*),\s*([^,]+?)\s*(?:,|$)", line.strip())
            if not m:
                continue
            kind, mods, key = m.groups()
            if key.startswith(("XF86", "mouse")) or kind == "bindm" or (kind == "bindr" and key == "SUPER_L"):
                continue          # media keys, mouse, and "tap Super" (Windows style extra)
            k = (" ".join(sorted(mods.replace("$mod", "SUPER").upper().split())), key.lower())
            if kind == "unbind":
                keys.pop(k, None)
            else:
                keys[k] = line.strip()
    return keys


def sway(p):
    keys = {}
    for line in open(p):
        m = re.match(r"^bindsym\s+(?:--\S+\s+)?(\S+)\s+", line.strip())
        if not m:
            continue
        parts = m.group(1).split("+")
        if parts[-1].lower().startswith(("xf86", "button")):
            continue          # media keys and the mouse wheel (Hyprland's mouse binds are skipped too)
        mods = " ".join(sorted(x.replace("$mod", "SUPER").replace("Mod1", "ALT").upper() for x in parts[:-1]))
        keys[(mods, parts[-1].lower())] = line.strip()
    return keys


base = f"{R}/hyprland/hypr/hyprland.conf"
maps = {"Hyprland (Auxo)": hypr(base),
        "Hyprland (Mac)": hypr(base, f"{R}/hyprland-mac/hypr/style.conf"),
        "Hyprland (Windows)": hypr(base, f"{R}/hyprland-win/hypr/style.conf"),
        "Sway": sway(f"{R}/sway/sway/config"),
        "i3": sway(f"{R}/i3/i3/config")}
ref = set(maps["Hyprland (Auxo)"])
bad = 0
for name, m in maps.items():
    missing, extra = sorted(ref - set(m)), sorted(set(m) - ref)
    if missing or extra:
        bad = 1
        print(f"{name}: missing {missing}  extra {extra}")
# a key must do the same job: the Windows/Mac styles may only restyle these
same_job = {("SUPER", "a"): "auxo-launcher", ("SUPER", "c"): "auxo-control", ("SUPER", "space"): ("wofi", "$menu", "rofi")}
for name, m in maps.items():
    for k, want in same_job.items():
        want = want if isinstance(want, tuple) else (want,)
        if k in m and not any(w in m[k] for w in want):
            bad = 1
            print(f"{name}: {k} does something else: {m[k]}")
print(f"{len(ref)} keys, the same in all {len(maps)} rices" if not bad else "key maps differ")
sys.exit(bad)
