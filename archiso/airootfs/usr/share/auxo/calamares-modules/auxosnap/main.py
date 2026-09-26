#!/usr/bin/env python3
# Auxo Linux — Calamares job: time-travel snapshots
# On btrfs: configure snapper for /, enable snap-pac (pre/post snapshot on every
# pacman run) and grub-btrfs (snapshots appear in the GRUB menu), then take the
# first snapshot "Fresh Auxo install".
import libcalamares
from libcalamares.utils import target_env_call, debug, warning


def pretty_name():
    return "Enabling bootable btrfs snapshots"


def root_fs(gs):
    for p in gs.value("partitions") or []:
        if p.get("mountPoint") == "/":
            return p.get("fs", "")
    return ""


def run():
    gs = libcalamares.globalstorage
    fs = root_fs(gs)
    if fs != "btrfs":
        debug(f"auxosnap: root filesystem is '{fs}', snapshots skipped")
        return None
    rc = target_env_call(["auxo-tweak", "snapshots", "on", "--no-now", "--no-grub",
                          "--description", "Fresh Auxo install"])
    if rc != 0:
        warning(f"auxosnap: snapshot setup exited with {rc}")
        return None
    # regenerate GRUB so the snapshot submenu is present from the first boot
    target_env_call(["grub-mkconfig", "-o", "/boot/grub/grub.cfg"])
    return None
