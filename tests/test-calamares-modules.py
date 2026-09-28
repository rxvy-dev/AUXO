#!/usr/bin/env python3
"""Runs Auxo's custom Calamares job modules against a mocked libcalamares.
   python3 tests/test-calamares-modules.py"""
import importlib.util
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS = os.path.join(HERE, "archiso/airootfs/usr/share/auxo/calamares-modules")
passed = failed = 0


def check(name, cond):
    global passed, failed
    print(("  \033[32m✓\033[0m " if cond else "  \033[31m✗\033[0m ") + name)
    passed += bool(cond)
    failed += not cond


class GS(dict):
    def value(self, k):
        return self.get(k)

    def insert(self, k, v):
        self[k] = v


def mock(gs, rc=lambda cmd: 0):
    calls, warnings = [], []
    lc = types.ModuleType("libcalamares")
    lc.globalstorage = gs
    lc.job = types.SimpleNamespace(setprogress=lambda p: None, configuration={})
    utils = types.ModuleType("libcalamares.utils")

    def tec(cmd, input="", timeout=0):
        assert isinstance(input, str), "input must be a str (pybind11 std::string)"
        calls.append(cmd)
        return rc(cmd)
    utils.target_env_call = tec
    utils.debug = lambda m: None
    utils.warning = warnings.append
    lc.utils = utils
    sys.modules["libcalamares"] = lc
    sys.modules["libcalamares.utils"] = utils
    return calls, warnings


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(MODS, name, "main.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def root_with_files():
    r = tempfile.mkdtemp()
    os.makedirs(r + "/etc/default")
    open(r + "/etc/pacman.conf", "w").write(
        "[options]\n#Color\n#ParallelDownloads = 5\n#VerbosePkgLists\n\n[core]\nInclude = /etc/pacman.d/mirrorlist\n\n"
        "#[multilib]\n#Include = /etc/pacman.d/mirrorlist\n")
    open(r + "/etc/default/grub", "w").write('GRUB_TIMEOUT=5\nGRUB_DISTRIBUTOR="Arch"\n#GRUB_DISABLE_OS_PROBER=false\n')
    return r


print("▲ auxoprepare (online)")
r = root_with_files()
calls, _ = mock(GS(rootMountPoint=r, hasInternet=True))
check("returns success", load("auxoprepare").run() is None)
pc = open(r + "/etc/pacman.conf").read()
check("keyring init + populate", ["pacman-key", "--init"] in calls and ["pacman-key", "--populate", "archlinux"] in calls)
check("Color + ILoveCandy", "\nColor\nILoveCandy\n" in pc)
check("ParallelDownloads = 8", "ParallelDownloads = 8" in pc)
check("multilib enabled", "\n[multilib]\nInclude" in pc)
check("reflector ran", any(c[0] == "reflector" for c in calls))
check("db synced", ["pacman", "-Sy", "--noconfirm"] in calls)

check("DNS checked inside chroot", ["getent", "hosts", "archlinux.org"] in calls)

print("▲ auxoprepare (online but DNS dead in chroot)")
r = root_with_files()
calls, _ = mock(GS(rootMountPoint=r, hasInternet=True), rc=lambda c: 2 if c[0] == "getent" else 0)
res = load("auxoprepare").run()
check("fails early with a readable message", isinstance(res, tuple) and "DNS" in res[0])
check("pacman -Sy not attempted", ["pacman", "-Sy", "--noconfirm"] not in calls)

print("▲ auxoprepare (offline)")
r = root_with_files()
calls, _ = mock(GS(rootMountPoint=r, hasInternet=False))
load("auxoprepare").run()
check("no reflector offline", not any(c[0] == "reflector" for c in calls))
check("keyring still initialised", ["pacman-key", "--init"] in calls)

print("▲ auxodesktop (online: hyprland, rose, zen, fish)")
r = root_with_files()
gs = GS(rootMountPoint=r, hasInternet=True, username="alex", packagechooser_desktop="hyprland",
        packagechooser_accent="rose", packagechooser_kernel="linux-zen", packagechooser_shell="fish")
calls, warns = mock(gs)
check("returns success", load("auxodesktop").run() is None)
flat = [" ".join(c) for c in calls]
check("accent applied first", flat[0] == "auxo-tweak accent rose --no-grub --no-initramfs --user alex")
check("boot splash enabled right after accent", flat[1] == "auxo-tweak splash on --no-initramfs --no-grub")
check("fish shell", "auxo-tweak shell fish --user alex" in flat)
check("zen kernel", "auxo-tweak kernel linux-zen --no-grub" in flat)
check("hyprland replaces plasma", "auxo-tweak desktop hyprland --replace --user alex" in flat)
check("drivers pruned", "auxo-tweak drivers --no-initramfs --prune" in flat)
check("zram on", "auxo-tweak zram on" in flat)
check("welcome autostart for user", any("auxo-welcome.desktop" in f and "/home/alex/.config/autostart" in f for f in flat))
g = open(r + "/etc/default/grub").read()
check("os-prober enabled for dual boot", 'GRUB_DISABLE_OS_PROBER="false"' in g and "#GRUB_DISABLE_OS_PROBER" not in g)
check("GRUB distributor Auxo", 'GRUB_DISTRIBUTOR="Auxo"' in g)
check("no warnings", not warns)

print("▲ auxodesktop (offline: hyprland + zen requested)")
r = root_with_files()
gs = GS(rootMountPoint=r, hasInternet=False, username="sam", packagechooser_desktop="hyprland",
        packagechooser_accent="cyan", packagechooser_kernel="linux-zen", packagechooser_shell="fish")
calls, warns = mock(gs)
load("auxodesktop").run()
flat = [" ".join(c) for c in calls]
check("falls back to plasma", "auxo-tweak desktop plasma --user sam" in flat and not any("hyprland" in f for f in flat))
check("no kernel install offline", not any("kernel" in f for f in flat))
check("fish downgraded to zsh offline", "auxo-tweak shell zsh --user sam" in flat)
check("notes stored for the user", len(gs.get("auxoNotes", [])) == 3)

print("▲ auxodesktop (desktop install fails → plasma kept)")
r = root_with_files()
gs = GS(rootMountPoint=r, hasInternet=True, username="alex", packagechooser_desktop="gnome",
        packagechooser_accent="violet", packagechooser_kernel="linux", packagechooser_shell="zsh")
calls, warns = mock(gs, rc=lambda c: 1 if c[:3] == ["auxo-tweak", "desktop", "gnome"] else 0)
load("auxodesktop").run()
flat = [" ".join(c) for c in calls]
check("retries with plasma", "auxo-tweak desktop plasma --user alex" in flat)
check("records failure note", any("gnome failed" in n for n in gs.get("auxoNotes", [])))

print("▲ auxodesktop (missing choices → defaults)")
r = root_with_files()
calls, _ = mock(GS(rootMountPoint=r, hasInternet=True, username="alex"))
load("auxodesktop").run()
flat = [" ".join(c) for c in calls]
check("defaults: violet / zsh / plasma", "auxo-tweak accent violet --no-grub --no-initramfs --user alex" in flat
      and "auxo-tweak shell zsh --user alex" in flat and "auxo-tweak desktop plasma --user alex" in flat)

print("▲ auxodesktop (lts + fish both fail → safe fallbacks)")
r = root_with_files()
open(r + "/etc/default/grub", "a").write('GRUB_TOP_LEVEL="/boot/vmlinuz-linux-lts"\n')
gs = GS(rootMountPoint=r, hasInternet=True, username="alex", packagechooser_desktop="plasma",
        packagechooser_accent="violet", packagechooser_kernel="linux-lts", packagechooser_shell="fish")
calls, warns = mock(gs, rc=lambda c: 1 if c[:2] in (["auxo-tweak", "kernel"], ["auxo-tweak", "shell"]) and "zsh" not in c else 0)
check("returns success", load("auxodesktop").run() is None)
flat = [" ".join(c) for c in calls]
check("shell falls back to zsh", "auxo-tweak shell zsh --user alex" in flat)
check("GRUB_TOP_LEVEL removed after failed kernel", "GRUB_TOP_LEVEL" not in open(r + "/etc/default/grub").read())
check("notes explain both", any("linux-lts failed" in n for n in gs["auxoNotes"]) and any("fish could not" in n for n in gs["auxoNotes"]))
check("grub-mkconfig tested before bootloader", any("grub-mkconfig -o /tmp/auxo-grub-test.cfg" in f for f in flat))

print("▲ auxodesktop (GRUB_TOP_LEVEL points at a missing file)")
r = root_with_files()
os.makedirs(r + "/boot")
open(r + "/etc/default/grub", "a").write('GRUB_TOP_LEVEL="/boot/vmlinuz-linux-hardened"\n')
gs = GS(rootMountPoint=r, hasInternet=True, username="alex", packagechooser_kernel="linux-hardened")
calls, warns = mock(gs)
load("auxodesktop").run()
check("dangling GRUB_TOP_LEVEL removed", "GRUB_TOP_LEVEL" not in open(r + "/etc/default/grub").read())
check("user told why", any("vmlinuz-linux-hardened was missing" in n for n in gs.get("auxoNotes", [])))

print("▲ auxodesktop (grub-mkconfig fails only with GRUB_TOP_LEVEL)")
r = root_with_files()
os.makedirs(r + "/boot"); open(r + "/boot/vmlinuz-linux-zen", "w").write("k")
open(r + "/etc/default/grub", "a").write('GRUB_TOP_LEVEL="/boot/vmlinuz-linux-zen"\n')
gs = GS(rootMountPoint=r, hasInternet=True, username="alex", packagechooser_kernel="linux-zen")
state = {"n": 0}
def rc_mk(c):
    if "grub-mkconfig -o /tmp" in " ".join(c):
        return 1 if "GRUB_TOP_LEVEL" in open(r + "/etc/default/grub").read() else 0
    return 0
calls, warns = mock(gs, rc=rc_mk)
load("auxodesktop").run()
check("retried without GRUB_TOP_LEVEL", "GRUB_TOP_LEVEL" not in open(r + "/etc/default/grub").read()
      and sum("grub-mkconfig -o /tmp" in " ".join(c) for c in calls) == 2)

print("▲ auxosnap")
calls, _ = mock(GS(partitions=[{"mountPoint": "/", "fs": "btrfs"}, {"mountPoint": "/boot/efi", "fs": "fat32"}]))
load("auxosnap").run()
check("btrfs → splash check, snapshots on + grub-mkconfig", calls and calls[0] == ["auxo-tweak", "splash", "on"] and calls[1][:3] == ["auxo-tweak", "snapshots", "on"]
      and any("grub-mkconfig -o /boot/grub/grub.cfg" in " ".join(c) for c in calls))
calls, _ = mock(GS(partitions=[{"mountPoint": "/", "fs": "ext4"}]))
load("auxosnap").run()
check("ext4 → no snapshots (splash check + disk flush only)", calls == [["auxo-tweak", "splash", "on"], ["sync"]])

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
