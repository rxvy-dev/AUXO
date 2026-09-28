#!/usr/bin/env python3
# Auxo Linux — Calamares job: apply the user's choices
#   accent colour, login shell, kernel, desktop/rice, GPU drivers, zram
# Everything is delegated to `auxo-tweak` inside the target, so the installer
# and the post-install tool behave identically.
import os
import subprocess

import libcalamares
from libcalamares.utils import target_env_call, debug, warning

LOG = "/var/log/auxo-install.log"
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


def get_grub(root, key):
    try:
        for line in open(root + "/etc/default/grub"):
            line = line.strip()
            if line.startswith(key + "="):
                return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return ""


def del_grub(root, key):
    path = root + "/etc/default/grub"
    try:
        lines = open(path).read().splitlines()
    except OSError:
        return
    open(path, "w").write("\n".join(l for l in lines if not l.strip().startswith(key + "=")) + "\n")


def sh(script):
    """Run a shell snippet in the target, appending its output to the install log."""
    return target_env_call(["sh", "-c", f"{{ {script} ; }} >>{LOG} 2>&1"])


def boot_sanity(root, notes):
    """Last check before the initcpio/grubcfg/bootloader jobs: every kernel has its
    image in /boot, GRUB_TOP_LEVEL points at a real file, and grub-mkconfig runs
    cleanly. A failing grub-mkconfig makes the bootloader job abort the install,
    so anything we changed that could cause it is undone here."""
    # the kernel pacman hook normally does this; redo it for anything that slipped through
    sh('for d in /usr/lib/modules/*/; do [ -f "$d/pkgbase" ] && [ -f "$d/vmlinuz" ] || continue; '
       'k=$(cat "$d/pkgbase"); [ -f "/boot/vmlinuz-$k" ] || install -Dm644 "$d/vmlinuz" "/boot/vmlinuz-$k"; done')
    top = get_grub(root, "GRUB_TOP_LEVEL")
    if top and not os.path.exists(root + top):
        del_grub(root, "GRUB_TOP_LEVEL")
        notes.append(f"{top} was missing, so the stable kernel stays the default boot entry")
        top = ""
    sh("echo '== auxodesktop: grub-mkconfig test =='; cat /etc/default/grub")
    if sh("grub-mkconfig -o /tmp/auxo-grub-test.cfg") == 0:
        return
    warning(f"auxodesktop: grub-mkconfig test failed, see {LOG}")
    if top:
        del_grub(root, "GRUB_TOP_LEVEL")
        if sh("grub-mkconfig -o /tmp/auxo-grub-test.cfg") == 0:
            notes.append("GRUB could not use your chosen kernel as the default; the stable kernel is the default entry "
                         "(your kernel is under 'Advanced options')")
            return
    notes.append(f"grub-mkconfig reported an error during setup; details are in {LOG}")


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
    tweak("accent", accent, "--no-grub", "--no-initramfs", *u)
    # animated boot splash in the chosen accent; the initcpio + grubcfg jobs that
    # run after this pick up the plymouth hook and the "splash" kernel option
    tweak("splash", "on", "--no-initramfs", "--no-grub")

    libcalamares.job.setprogress(0.15)
    if shell != "zsh" and shell != "bash" and not online:
        notes.append(f"{shell} needs internet — kept zsh")
        shell = "zsh"
    if tweak("shell", shell, *u) != 0 and shell != "zsh":
        notes.append(f"{shell} could not be set up — zsh is your shell (switch later: auxo-tweak shell {shell})")
        tweak("shell", "zsh", *u)

    libcalamares.job.setprogress(0.25)
    if kernel != "linux":
        if online:
            if tweak("kernel", kernel, "--no-grub", timeout=1800) != 0:
                del_grub(root, "GRUB_TOP_LEVEL")
                notes.append(f"{kernel} failed to install — the stable kernel is used (try later: auxo-tweak kernel {kernel})")
        else:
            notes.append(f"{kernel} needs internet — installed the stable kernel")
    else:
        del_grub(root, "GRUB_TOP_LEVEL")

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

    libcalamares.job.setprogress(0.9)
    boot_sanity(root, notes)

    if notes:
        gs.insert("auxoNotes", notes)
        for n in notes:
            warning("auxodesktop: " + n)
    libcalamares.job.setprogress(1.0)
    return None
