#!/usr/bin/env python3
# Auxo Linux — Calamares job: time-travel snapshots
# On btrfs: configure snapper for /, enable snap-pac (pre/post snapshot on every
# pacman run) and grub-btrfs (snapshots appear in the GRUB menu), then take the
# first snapshot "Fresh Auxo install".
import os
import libcalamares
from libcalamares.utils import target_env_call, debug, warning


def pretty_name():
    return "Enabling bootable btrfs snapshots"


def root_fs(gs):
    for p in gs.value("partitions") or []:
        if p.get("mountPoint") == "/":
            return p.get("fs", "")
    return ""


def _flush():
    """Make sure kernels, initramfs and grub.cfg are really on disk before the
    installer unmounts and the user reboots (a half-written vmlinuz makes GRUB
    stop with "premature end of file")."""
    target_env_call(["sync"])
    try:
        os.sync()
    except Exception:
        pass


def run():
    try:
        # last check that the boot splash survived the initcpio/grubcfg jobs: only
        # rebuilds the initramfs / grub.cfg if the hook or "splash" option went missing
        target_env_call(["auxo-tweak", "splash", "on"])
        return _run()
    finally:
        _flush()


def _run():
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
    if target_env_call(["sh", "-c", "grub-mkconfig -o /boot/grub/grub.cfg >>/var/log/auxo-install.log 2>&1"]) != 0:
        # the bootloader job already wrote a working grub.cfg; only the snapshot submenu is missing
        warning("auxosnap: grub-mkconfig failed (see /var/log/auxo-install.log); snapshot menu appears after the next update")
    return None
