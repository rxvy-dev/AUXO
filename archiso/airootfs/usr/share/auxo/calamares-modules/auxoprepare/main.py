#!/usr/bin/env python3
# Auxo Linux — Calamares job: prepare pacman in the target
#   * initialise the pacman keyring (the live one lives in tmpfs)
#   * pacman.conf: colour, parallel downloads, multilib (Steam/Wine ready)
#   * rank mirrors with reflector when online
import subprocess

import libcalamares
from libcalamares.utils import target_env_call, debug, warning


def pretty_name():
    return "Preparing the package manager"


def _run(cmd, timeout=None):
    debug("auxoprepare: " + " ".join(cmd))
    try:
        return target_env_call(cmd, "", timeout or 0)
    except subprocess.TimeoutExpired:
        warning("auxoprepare: timed out: " + " ".join(cmd))
        return 124


def tweak_pacman_conf(path):
    try:
        with open(path) as f:
            txt = f.read()
    except OSError as e:
        warning(f"auxoprepare: cannot read {path}: {e}")
        return
    txt = txt.replace("#Color\n", "Color\nILoveCandy\n", 1)
    txt = txt.replace("#ParallelDownloads = 5", "ParallelDownloads = 8", 1)
    txt = txt.replace("#VerbosePkgLists", "VerbosePkgLists", 1)
    txt = txt.replace("#[multilib]\n#Include = /etc/pacman.d/mirrorlist", "[multilib]\nInclude = /etc/pacman.d/mirrorlist", 1)
    if "\n[multilib]" not in txt:
        txt += "\n[multilib]\nInclude = /etc/pacman.d/mirrorlist\n"
    if "[auxo]" not in txt:
        txt += ("\n# Auxo Linux repository (auxo-tools updates). Uncomment once it is published.\n"
                "#[auxo]\n#SigLevel = Optional TrustAll\n#Server = https://auxolinux.com/repo/$arch\n")
    with open(path, "w") as f:
        f.write(txt)


def run():
    gs = libcalamares.globalstorage
    root = gs.value("rootMountPoint")
    online = bool(gs.value("hasInternet"))

    libcalamares.job.setprogress(0.1)
    _run(["pacman-key", "--init"])
    _run(["pacman-key", "--populate", "archlinux"])

    libcalamares.job.setprogress(0.4)
    tweak_pacman_conf(root + "/etc/pacman.conf")

    if online and _run(["getent", "hosts", "archlinux.org"], timeout=15) != 0:
        return ("No DNS inside the installer",
                "The live system is online, but name lookups fail inside the new system, so packages "
                "can't be downloaded. Check that systemd-resolved is running (systemctl status "
                "systemd-resolved), or go back and install offline.")

    if online:
        libcalamares.job.setprogress(0.6)
        rc = _run(["reflector", "--protocol", "https", "--latest", "20", "--age", "24", "--sort", "rate",
                   "--save", "/etc/pacman.d/mirrorlist"], timeout=120)
        if rc != 0:
            warning("auxoprepare: reflector failed; keeping the default mirrorlist")
        _run(["pacman", "-Sy", "--noconfirm"], timeout=300)
    else:
        debug("auxoprepare: offline install — skipping mirror ranking")

    libcalamares.job.setprogress(1.0)
    return None
