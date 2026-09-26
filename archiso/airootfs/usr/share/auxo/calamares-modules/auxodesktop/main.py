#!/usr/bin/env python3
# Auxo Linux — Calamares job: apply the user's choices
#   accent colour, login shell, kernel, desktop/rice, GPU drivers, zram
# Everything is delegated to `auxo-tweak` inside the target, so the installer
# and the post-install tool behave identically.
import subprocess

import libcalamares
from libcalamares.utils import target_env_call, debug, warning

ONLINE_ONLY_DESKTOPS = {"gnome", "xfce", "cinnamon", "hyprland", "sway", "i3"}


def pretty_name():
    return "Setting up your Auxo desktop"


def choice(gs, key, default):
    v = gs.value("packagechooser_" + key)
    if isinstance(v, (list, tuple)):
        v = v[0] if v else ""
    v = (v or "").split(",")[0].strip()
    return v or default


def tweak(*args, timeout=None):
    cmd = ["auxo-tweak", *args]
    debug("auxodesktop: " + " ".join(cmd))
    try:
        rc = target_env_call(cmd, "", timeout or 0)
    except subprocess.TimeoutExpired:
        rc = 124
    if rc != 0:
        warning(f"auxodesktop: '{' '.join(cmd)}' exited with {rc}")
    return rc


def set_grub(root, key, value):
    path = root + "/etc/default/grub"
    try:
        lines = open(path).read().splitlines()
    except OSError:
        return
    out, done = [], False
    for line in lines:
        if line.lstrip("#").strip().startswith(key + "="):
            if not done:
                out.append(f'{key}="{value}"')
                done = True
            continue
        out.append(line)
    if not done:
        out.append(f'{key}="{value}"')
    open(path, "w").write("\n".join(out) + "\n")


def run():
    gs = libcalamares.globalstorage
    root = gs.value("rootMountPoint")
    user = gs.value("username") or ""
    online = bool(gs.value("hasInternet"))
    desktop = choice(gs, "desktop", "plasma")
    accent = choice(gs, "accent", "violet")
    kernel = choice(gs, "kernel", "linux")
    shell = choice(gs, "shell", "zsh")
    notes = []
    debug(f"auxodesktop: user={user} desktop={desktop} accent={accent} kernel={kernel} shell={shell} online={online}")

    u = ["--user", user] if user else []

    # accent first: the rices read ~/.config/auxo/*
    libcalamares.job.setprogress(0.05)
    tweak("accent", accent, "--no-grub", *u)

    libcalamares.job.setprogress(0.15)
    if shell != "zsh" and shell != "bash" and not online:
        notes.append(f"{shell} needs internet — kept zsh")
        shell = "zsh"
    tweak("shell", shell, *u)

    libcalamares.job.setprogress(0.25)
    if kernel != "linux":
        if online:
            tweak("kernel", kernel, "--no-grub", timeout=1800)
        else:
            notes.append(f"{kernel} needs internet — installed the stable kernel")

    libcalamares.job.setprogress(0.45)
    if desktop in ONLINE_ONLY_DESKTOPS and not online:
        notes.append(f"{desktop} needs internet — installed KDE Plasma instead (switch later: auxo-tweak desktop {desktop})")
        desktop = "plasma"
    if desktop == "plasma":
        tweak("desktop", "plasma", *u)
    else:
        rc = tweak("desktop", desktop, "--replace", *u, timeout=3600)
        if rc != 0:
            notes.append(f"{desktop} failed to install — KDE Plasma kept")
            tweak("desktop", "plasma", *u)

    libcalamares.job.setprogress(0.75)
    tweak("drivers", "--no-initramfs", "--prune", timeout=1800)

    libcalamares.job.setprogress(0.85)
    tweak("zram", "on")

    # welcome app on first login
    if user:
        target_env_call(["install", "-Dm644", "/usr/share/applications/auxo-welcome.desktop",
                         f"/home/{user}/.config/autostart/auxo-welcome.desktop"])
        target_env_call(["chown", "-R", f"{user}:{user}", f"/home/{user}/.config"])

    # GRUB: detect other OSes (dual boot), Auxo theme, quick timeout
    set_grub(root, "GRUB_DISABLE_OS_PROBER", "false")
    set_grub(root, "GRUB_TIMEOUT", "5")
    set_grub(root, "GRUB_DISTRIBUTOR", "Auxo")
    set_grub(root, "GRUB_TERMINAL_OUTPUT", "gfxterm")

    if notes:
        gs.insert("auxoNotes", notes)
        for n in notes:
            warning("auxodesktop: " + n)
    libcalamares.job.setprogress(1.0)
    return None
