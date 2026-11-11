#!/usr/bin/env python3
"""AuxoBSD installer tests: validation, system parsers, the install plan, the runner (dry run)
and the Qt wizard rendered offscreen. No FreeBSD, root or disks needed.

  python3 bsd/tests/test-installer.py
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BSD = os.path.dirname(HERE)
TOP = os.path.dirname(BSD)
sys.path[:0] = [os.path.join(BSD, "installer"), os.path.join(BSD, "tools", "files", "lib"),
                os.path.join(TOP, "packages", "auxo-tools", "files", "lib")]

from auxo_bsdinstall import config, plan, runner, system  # noqa: E402

passed = failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
        print(f"  \033[32m✓\033[0m {name}")
    else:
        failed += 1
        print(f"  \033[31m✗\033[0m {name}")


def cfg(**kw):
    c = config.InstallConfig(disk="ada0", disk_bytes=64 * 1024 ** 3, username="ada", password="secret",
                             fullname="Ada Lovelace", timezone="America/Vancouver")
    for k, v in kw.items():
        setattr(c, k, v)
    return c


def flat(steps):
    """Every op of a plan as a string, in order."""
    out = []
    for s in steps:
        for op in s.ops:
            if op[0] in ("run", "try"):
                out.append(" ".join(op[1]))
            elif op[0].startswith("chroot"):
                out.append("chroot " + " ".join(op[1]))
            elif op[0] == "write":
                out.append("write " + op[1])
            else:
                out.append(op[0])
    return out


def written(steps, path):
    for s in steps:
        for op in s.ops:
            if op[0] == "write" and op[1] == path:
                return op[2]
    return None


print("▲ validation")
check("a complete config is valid", cfg().errors() == [])
check("no disk", any("disk" in e for e in cfg(disk="").errors()))
check("disk too small", any("at least" in e for e in cfg(disk_bytes=8 * 1024 ** 3).errors()))
check("bad username", any("Usernames" in e for e in cfg(username="Ada!").errors()))
check("reserved username", any("reserved" in e for e in cfg(username="root").errors()))
check("bad hostname", any("computer name" in e for e in cfg(hostname="-bad-").errors()))
check("empty password", any("password" in e.lower() for e in cfg(password="").errors()))
check("name with a colon", any("':'" in e for e in cfg(fullname="A:B").errors()))

print("▲ system")
GEOM = """Geom name: nda0
Providers:
1. Name: nda0
   Mediasize: 1000204886016 (932G)
   descr: Samsung SSD 990 PRO 1TB

Geom name: da0
Providers:
1. Name: da0
   Mediasize: 15376000000 (14G)
   descr: SanDisk Ultra

Geom name: cd0
Providers:
1. Name: cd0
   Mediasize: 4700000000 (4.4G)
   descr: VBOX CD-ROM
"""
disks = system.parse_geom_disks(GEOM)
check("geom disk list parsed", [d["name"] for d in disks] == ["nda0", "da0", "cd0"] and disks[0]["bytes"] == 1000204886016)
boot = system.parse_boot_medium("iso9660/AUXOBSD  N/A  da0\ngpt/efi  N/A  nda0p1\n")
check("the USB stick we booted from is found", boot == "da0")
check("…and can't be picked, nor CD drives", [d["name"] for d in system.installable(disks, boot)] == ["nda0"])
check("human sizes", system.human(1000204886016) == "1.0 TB" and system.human(64424509440) == "64.4 GB")
z = system.timezones()
check("time zones load", "America" in z and "Vancouver" in z["America"] or z == {"Etc": ["UTC"]})
check("keyboard table: X layouts + console keymaps", all(k[3].endswith(".kbd") for k in system.KEYBOARDS))

print("▲ plan: ZFS (default)")
c = cfg()
steps = plan.build(c)
ops = flat(steps)
check("GPT with BIOS boot + EFI partitions", "gpart create -s gpt ada0" in ops
      and any("-t freebsd-boot" in o for o in ops) and any("-t efi" in o for o in ops))
check("ZFS boot code on the BIOS partition", "gpart bootcode -b /boot/pmbr -p /boot/gptzfsboot -i 1 ada0" in ops)
check("pool + boot environment layout", any(o.startswith("zpool create") and "zroot /dev/gpt/auxoroot-ada0" in o for o in ops)
      and "zfs create -o mountpoint=/ -o canmount=noauto zroot/ROOT/default" in ops
      and "zpool set bootfs=zroot/ROOT/default zroot" in ops)
check("the system is copied from a read-only view of the live image", "mount -t nullfs -o ro / /tmp/auxo-src" in ops
      and "copy" in ops)
check("live-only files are not copied", "./auxolive" in plan.COPY_EXCLUDES and "./etc/rc.conf" in plan.COPY_EXCLUDES)
check("UEFI: loader.efi in the ESP + boot entry", any("EFI/BOOT/BOOTX64.EFI" in o for o in ops)
      and any(o.startswith("efibootmgr") and "AuxoBSD" in o for o in ops))
rc = written(steps, "/etc/rc.conf")
check("rc.conf: hostname, ZFS, Plasma login, seatd", 'hostname="auxobsd"' in rc and 'zfs_enable="YES"' in rc
      and 'sddm_enable="YES"' in rc and 'dbus_enable="YES"' in rc and 'seatd_enable="YES"' in rc)
check("rc.conf keeps AuxoBSD's os-release", 'osrelease_enable="NO"' in rc)
lc = written(steps, "/boot/loader.conf")
check("loader.conf: Auxo logo + brand, ZFS", 'loader_logo="auxo"' in lc and 'loader_brand="auxobsd"' in lc and 'zfs_load="YES"' in lc)
fs = written(steps, "/etc/fstab")
check("fstab: ESP + swap by GPT label, no root line on ZFS", "/dev/gpt/auxoefi-ada0" in fs and "auxoswap-ada0" in fs
      and " / " not in fs.replace("/boot/efi", ""))
check("sysctl: KDE socket buffers", "net.local.stream.recvspace=65536" in written(steps, "/etc/sysctl.conf"))
ac = written(steps, "/etc/auxo/auxo.conf")
check("auxo.conf: accent, snapshots on", 'ACCENT="violet"' in ac and 'SNAPSHOTS="on"' in ac)
check("time zone", "cp /mnt/usr/share/zoneinfo/America/Vancouver /mnt/etc/localtime" in ops)
user_op = next(op for s in steps for op in s.ops if op[0] == "chroot" and op[1][:2] == ["pw", "useradd"])
check("user: wheel + video, password on stdin (not on the command line)", "wheel,operator,video" in user_op[1]
      and user_op[2] == "secret\n" and "secret" not in " ".join(user_op[1]))
check("root is locked (sudo for wheel)", "chroot pw usermod root -h -" in ops
      and "%wheel ALL=(ALL:ALL) ALL" in written(steps, "/usr/local/etc/sudoers.d/10-wheel"))
check("accent + shell + GPU drivers set up in the new system", "chroot env AUXO_IN_INSTALLER=1 auxo-tweak accent violet --user ada" in ops
      and "chroot env AUXO_IN_INSTALLER=1 auxo-tweak shell zsh --user ada" in ops
      and "chroot env AUXO_IN_INSTALLER=1 auxo-tweak drivers --prune" in ops)
check("drivers run after the copy and devfs mount", ops.index("copy") < ops.index("mount -t devfs devfs /mnt/dev")
      < ops.index("chroot env AUXO_IN_INSTALLER=1 auxo-tweak drivers --prune"))
check("no xbps unless asked", not any("xbps" in o for o in ops))
check("keyboard for Xorg and Plasma", 'XkbLayout" "us"' in written(steps, "/usr/local/etc/X11/xorg.conf.d/20-keyboard.conf")
      and "LayoutList=us" in written(steps, "/home/ada/.config/kxkbrc"))

print("▲ plan: options")
u = plan.build(cfg(fs="ufs", firmware="bios", swap_gb=0, xbps=True, shell="fish", accent="emerald",
                   keyboard="de", keymap="de.kbd", nvidia_mode="prime"))
uo = flat(u)
check("UFS: gptboot, newfs, root in fstab", "gpart bootcode -b /boot/pmbr -p /boot/gptboot -i 1 ada0" in uo
      and any(o.startswith("newfs -U") for o in uo) and "auxoroot-ada0" in written(u, "/etc/fstab"))
check("UFS: no zpool, no ZFS in rc.conf/loader.conf", not any(o.startswith("zpool") for o in uo)
      and "zfs_enable" not in written(u, "/etc/rc.conf") and "zfs_load" not in written(u, "/boot/loader.conf"))
check("UFS: snapshots off", 'SNAPSHOTS="off"' in written(u, "/etc/auxo/auxo.conf"))
check("BIOS boot: no efibootmgr (the ESP is still made for later)", not any(o.startswith("efibootmgr") for o in uo)
      and any("-t efi" in o for o in uo))
check("no swap partition when swap is 0", not any("freebsd-swap" in o for o in uo) and "swap" not in written(u, "/etc/fstab"))
check("xbps chosen → auxo-tweak xbps on (allowed to fail offline)",
      any(op[0] == "chroot-try" and op[1][-2:] == ["xbps", "on"] for s in u for op in s.ops))
check("fish shell + emerald + German keyboard", "/usr/local/bin/fish" in " ".join(uo) and
      'ACCENT="emerald"' in written(u, "/etc/auxo/auxo.conf") and 'keymap="de.kbd"' in written(u, "/etc/rc.conf"))
check("NVIDIA choice passed to auxo-tweak", 'NVIDIA_MODE="prime"' in written(u, "/etc/auxo/auxo.conf"))
check("readable command listing", "zpool create" in plan.describe(steps) and "< password" in plan.describe(steps))

print("▲ runner (dry run)")
seen, prog = [], []
r = runner.Runner(steps, on_step=lambda i, t: seen.append(t), on_progress=prog.append, dry_run=True)
r.run()
check("every step reported", seen == [s.title for s in steps])
check("progress ends at 100%", prog and abs(prog[-1] - 1.0) < 1e-9 and prog == sorted(prog))
check("chroot ops run inside /mnt", any(a[:2] == ["chroot", "/mnt"] for a in r.executed))
check("the copy is a tar pipe with the excludes", any("|" in a and "--exclude" in a for a in r.executed))

print("▲ GUI (offscreen)")
try:
    import PyQt6  # noqa: F401
    have_qt = True
except ImportError:
    have_qt = False
if not have_qt:
    print("  (PyQt6 not installed: pip install PyQt6 to test the window)")
else:
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    os.environ["AUXO_INSTALLER_FAKE"] = "1"
    os.environ["AUXO_PIXMAPS"] = os.path.join(TOP, "packages", "auxo-tools", "files", "share", "pixmaps")
    from auxo_bsdinstall import gui
    from PyQt6.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])
    out = tempfile.mkdtemp(prefix="auxobsd-shots-")
    w = gui.Installer(dry_run=True)
    check("window opens with fake hardware", w.stack.count() == len(gui.STEPS))
    w.go(gui.STEPS.index("Disk"))
    check("can't continue without a disk", w.page_errors(gui.STEPS.index("Disk")) != [])
    w.disk_list.setCurrentRow(1)
    check("…picking one fixes it", w.page_errors(gui.STEPS.index("Disk")) == [] and w.cfg.disk == "ada0")
    w.fullname.setText("Grace Hopper")
    check("username suggested from the name", w.username.text() == "grace")
    w.pw1.setText("a"); w.pw2.setText("b")
    check("password mismatch caught", any("match" in e for e in w.page_errors(gui.STEPS.index("Account"))))
    check("hybrid laptop: PRIME offered on the Graphics page", "prime" in w.nv_buttons)
    w.nv_buttons["primary"].setChecked(True)
    check("NVIDIA choice reaches the config", w.cfg.nvidia_mode == "primary")
    w.set_accent("amber")
    check("accent recolours the installer", "#fbbf24" in w.styleSheet())
    gui.screenshots(gui.Installer(dry_run=True), out)
    shots = sorted(os.listdir(out))
    check(f"every page renders ({len(shots)} screenshots in {out})",
          len(shots) == len(gui.STEPS) and all(os.path.getsize(os.path.join(out, f)) > 5000 for f in shots))

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
