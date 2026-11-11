"""Turn an InstallConfig into the list of install steps. Pure: nothing here touches the
system, so the tests check every command the installer would run.

Disk layout (GPT, boots on both BIOS and UEFI machines):
  p1  freebsd-boot  512 KB   BIOS boot code (gptzfsboot / gptboot)
  p2  efi           260 MB   EFI system partition (loader.efi)
  p3  freebsd-swap  N GB
  p4  freebsd-zfs   rest     pool "zroot" with boot environments (or freebsd-ufs)

Ops in a step:
  ("run", argv)                    a command on the live system (fails the install if it fails)
  ("try", argv)                    the same, but failures are only logged
  ("chroot", argv)                 a command inside the new system
  ("chroot-try", argv)
  ("write", path, text, mode)      a file in the new system (path relative to the target)
  ("copy",)                        the live system → target, with progress
"""
import shlex
from dataclasses import dataclass, field

TARGET = "/mnt"
SRC = "/tmp/auxo-src"           # the live image without its writable layers (nullfs of /)
POOL = "zroot"

# copied from the image but not wanted on an installed system
COPY_EXCLUDES = ["./dev/*", "./proc/*", "./tmp/*", "./auxolive", "./usr/local/share/auxo/live",
                 "./var/run/*", "./boot/loader.conf", "./etc/fstab", "./etc/rc.conf"]


@dataclass
class Step:
    title: str
    ops: list = field(default_factory=list)
    weight: int = 1                 # rough share of the progress bar


def labels(cfg):
    d = cfg.disk
    return {"boot": f"auxoboot-{d}", "efi": f"auxoefi-{d}", "swap": f"auxoswap-{d}", "root": f"auxoroot-{d}"}


def partition_ops(cfg):
    L, d = labels(cfg), cfg.disk
    bootcode = "/boot/gptzfsboot" if cfg.fs == "zfs" else "/boot/gptboot"
    roottype = "freebsd-zfs" if cfg.fs == "zfs" else "freebsd-ufs"
    ops = [
        ("try", ["sh", "-c", f"swapoff /dev/gpt/{L['swap']} 2>/dev/null; true"]),
        ("try", ["gpart", "destroy", "-F", d]),
        ("run", ["dd", "if=/dev/zero", f"of=/dev/{d}", "bs=1m", "count=2"]),   # old labels/pool headers
        ("run", ["gpart", "create", "-s", "gpt", d]),
        ("run", ["gpart", "add", "-a", "4k", "-s", "512k", "-t", "freebsd-boot", "-l", L["boot"], d]),
        ("run", ["gpart", "bootcode", "-b", "/boot/pmbr", "-p", bootcode, "-i", "1", d]),
        ("run", ["gpart", "add", "-a", "1m", "-s", "260m", "-t", "efi", "-l", L["efi"], d]),
        ("run", ["newfs_msdos", "-F", "32", "-c", "1", "-L", "EFISYS", f"/dev/gpt/{L['efi']}"]),
    ]
    if cfg.swap_gb > 0:
        ops.append(("run", ["gpart", "add", "-a", "1m", "-s", f"{cfg.swap_gb}g", "-t", "freebsd-swap",
                            "-l", L["swap"], d]))
    ops.append(("run", ["gpart", "add", "-a", "1m", "-t", roottype, "-l", L["root"], d]))
    return ops


def filesystem_ops(cfg):
    L = labels(cfg)
    if cfg.fs == "ufs":
        return [
            ("run", ["newfs", "-U", "-L", "auxoroot", f"/dev/gpt/{L['root']}"]),
            ("run", ["mount", f"/dev/gpt/{L['root']}", TARGET]),
        ]
    z = ["zfs", "create"]
    return [
        ("run", ["kldload", "-n", "zfs"]),
        ("run", ["zpool", "create", "-f", "-o", f"altroot={TARGET}", "-O", "compress=lz4", "-O", "atime=off",
                 "-m", "none", POOL, f"/dev/gpt/{L['root']}"]),
        ("run", z + ["-o", "mountpoint=none", f"{POOL}/ROOT"]),
        ("run", z + ["-o", "mountpoint=/", "-o", "canmount=noauto", f"{POOL}/ROOT/default"]),
        ("run", ["zfs", "mount", f"{POOL}/ROOT/default"]),
        ("run", z + ["-o", "mountpoint=/home", f"{POOL}/home"]),
        ("run", z + ["-o", "mountpoint=/tmp", "-o", "exec=on", "-o", "setuid=off", f"{POOL}/tmp"]),
        ("run", z + ["-o", "mountpoint=/usr", "-o", "canmount=off", f"{POOL}/usr"]),
        ("run", z + ["-o", "setuid=off", f"{POOL}/usr/src"]),
        ("run", z + ["-o", "mountpoint=/var", "-o", "canmount=off", f"{POOL}/var"]),
        ("run", z + ["-o", "exec=off", "-o", "setuid=off", f"{POOL}/var/audit"]),
        ("run", z + ["-o", "exec=off", "-o", "setuid=off", f"{POOL}/var/crash"]),
        ("run", z + ["-o", "exec=off", "-o", "setuid=off", f"{POOL}/var/log"]),
        ("run", z + ["-o", "atime=on", f"{POOL}/var/mail"]),
        ("run", z + ["-o", "setuid=off", f"{POOL}/var/tmp"]),
        ("run", ["zpool", "set", f"bootfs={POOL}/ROOT/default", POOL]),
        ("run", ["mkdir", "-p", f"{TARGET}/boot/zfs"]),
        ("run", ["zpool", "set", f"cachefile={TARGET}/boot/zfs/zpool.cache", POOL]),
    ]


def copy_ops():
    return [
        ("run", ["mkdir", "-p", SRC]),
        ("try", ["umount", "-f", SRC]),
        ("run", ["mount", "-t", "nullfs", "-o", "ro", "/", SRC]),
        ("copy",),
        ("run", ["umount", SRC]),
        ("run", ["sh", "-c", f"mkdir -p {TARGET}/dev {TARGET}/proc {TARGET}/tmp {TARGET}/var/tmp {TARGET}/var/run "
                             f"&& chmod 1777 {TARGET}/tmp {TARGET}/var/tmp"]),
    ]


def boot_ops(cfg):
    L = labels(cfg)
    efi = f"{TARGET}/boot/efi"
    ops = [
        ("run", ["mkdir", "-p", efi]),
        ("run", ["mount", "-t", "msdosfs", f"/dev/gpt/{L['efi']}", efi]),
        ("run", ["mkdir", "-p", f"{efi}/EFI/BOOT", f"{efi}/EFI/auxobsd"]),
        ("run", ["cp", f"{TARGET}/boot/loader.efi", f"{efi}/EFI/BOOT/BOOTX64.EFI"]),
        ("run", ["cp", f"{TARGET}/boot/loader.efi", f"{efi}/EFI/auxobsd/loader.efi"]),
    ]
    if cfg.firmware == "uefi":
        ops.append(("try", ["efibootmgr", "--create", "--activate", "--label", "AuxoBSD",
                            "--loader", f"{efi}/EFI/auxobsd/loader.efi"]))
    return ops


def fstab(cfg):
    L = labels(cfg)
    lines = ["# Device                     Mountpoint  FStype   Options     Dump Pass"]
    if cfg.fs == "ufs":
        lines.append(f"/dev/gpt/{L['root']:<20} /           ufs      rw          1    1")
    lines.append(f"/dev/gpt/{L['efi']:<20} /boot/efi   msdosfs  rw          2    2")
    if cfg.swap_gb > 0:
        lines.append(f"/dev/gpt/{L['swap']:<20} none        swap     sw          0    0")
    lines += ["fdesc                        /dev/fd     fdescfs  rw          0    0",
              "proc                         /proc       procfs   rw          0    0"]
    return "\n".join(lines) + "\n"


def loader_conf(cfg):
    out = ['# AuxoBSD boot loader settings', 'autoboot_delay="3"', 'loader_logo="auxo"',
           'loader_brand="auxobsd"', 'kern.vty="vt"', 'fusefs_load="YES"', 'cpuctl_load="YES"',
           'kern.geom.label.disk_ident.enable="0"', 'kern.geom.label.gptid.enable="0"']
    if cfg.fs == "zfs":
        out += ['zfs_load="YES"', 'cryptodev_load="YES"']
    return "\n".join(out) + "\n"


def rc_conf(cfg):
    out = [
        "# AuxoBSD system settings (see rc.conf(5); auxo-tweak edits this too)",
        f'hostname="{cfg.hostname}"',
        f'keymap="{cfg.keymap}"',
        'ifconfig_DEFAULT="DHCP"',
        'background_dhclient="YES"',
        'sshd_enable="NO"',
        'ntpd_enable="YES"',
        'ntpd_sync_on_start="YES"',
        'powerd_enable="YES"',
        'powerd_flags="-a hiadaptive -b adaptive"',
        'moused_nondefault_enable="NO"',
        'dbus_enable="YES"',
        'sddm_enable="YES"',
        'seatd_enable="YES"',
        'dumpdev="AUTO"',
        'clear_tmp_enable="YES"',
        'syslogd_flags="-ss"',
        'sendmail_enable="NONE"',
        'update_motd="NO"',
        'osrelease_enable="NO"',          # /etc/os-release is AuxoBSD's own
    ]
    if cfg.fs == "zfs":
        out.append('zfs_enable="YES"')
    return "\n".join(out) + "\n"


SYSCTL = """# AuxoBSD desktop tuning
net.local.stream.recvspace=65536
net.local.stream.sendspace=65536
kern.ipc.shm_allow_removed=1
vfs.usermount=1
kern.sched.preempt_thresh=224
hw.snd.latency=7
"""


def xorg_keyboard(cfg):
    var = f'\n    Option "XkbVariant" "{cfg.keyboard_variant}"' if cfg.keyboard_variant else ""
    return ('# AuxoBSD: keyboard picked in the installer\nSection "InputClass"\n    Identifier "keyboard"\n'
            f'    MatchIsKeyboard "on"\n    Option "XkbLayout" "{cfg.keyboard}"{var}\nEndSection\n')


def kxkbrc(cfg):
    return ("[Layout]\nUse=true\nResetOldOptions=false\n"
            f"LayoutList={cfg.keyboard}\nVariantList={cfg.keyboard_variant}\n")


def auxo_conf(cfg):
    """Settings auxo-tweak reads; `auxo-tweak accent` and `drivers` add to it in the new system."""
    return ("# AuxoBSD system settings — managed by auxo-tweak\n"
            f'ACCENT="{cfg.accent}"\n'
            f'NVIDIA_MODE="{cfg.nvidia_mode}"\n'
            f'SNAPSHOTS="{"on" if cfg.fs == "zfs" else "off"}"\n')


def configure_ops(cfg):
    home = f"/home/{cfg.username}"
    gecos = cfg.fullname or cfg.username
    shells = {"zsh": "/usr/local/bin/zsh", "fish": "/usr/local/bin/fish", "bash": "/usr/local/bin/bash"}
    tz = cfg.timezone or "UTC"
    env = ["env", "AUXO_IN_INSTALLER=1"]
    ops = [
        ("write", "/etc/fstab", fstab(cfg), 0o644),
        ("write", "/boot/loader.conf", loader_conf(cfg), 0o644),
        ("write", "/etc/rc.conf", rc_conf(cfg), 0o644),
        ("write", "/etc/sysctl.conf", SYSCTL, 0o644),
        ("write", "/var/db/zoneinfo", tz + "\n", 0o644),
        ("run", ["cp", f"{TARGET}/usr/share/zoneinfo/{tz}", f"{TARGET}/etc/localtime"]),
        ("write", "/usr/local/etc/X11/xorg.conf.d/20-keyboard.conf", xorg_keyboard(cfg), 0o644),
        ("write", "/usr/local/etc/sudoers.d/10-wheel", "%wheel ALL=(ALL:ALL) ALL\n", 0o440),
        ("write", "/etc/auxo/auxo.conf", auxo_conf(cfg), 0o644),
        ("run", ["cp", "/etc/resolv.conf", f"{TARGET}/etc/resolv.conf"]),
        ("run", ["mount", "-t", "devfs", "devfs", f"{TARGET}/dev"]),
        ("chroot", ["pwd_mkdb", "-p", "/etc/master.passwd"]),
        ("chroot", ["pw", "usermod", "root", "-h", "-"]),     # root locked; use sudo
        ("chroot", ["pw", "useradd", "-n", cfg.username, "-c", gecos, "-m", "-G", "wheel,operator,video",
                    "-s", shells.get(cfg.shell, "/bin/sh"), "-h", "0"], cfg.password + "\n"),
        ("write", f"{home}/.config/kxkbrc", kxkbrc(cfg), 0o644),
        ("chroot", ["chown", "-R", f"{cfg.username}:{cfg.username}", f"{home}/.config"]),
        ("chroot", env + ["auxo-tweak", "shell", cfg.shell, "--user", cfg.username]),
        ("chroot", env + ["auxo-tweak", "accent", cfg.accent, "--user", cfg.username]),
        ("chroot-try", env + ["auxo-tweak", "drivers", "--prune"]),
        ("chroot-try", ["fc-cache", "-s"]),
    ]
    if cfg.xbps:
        ops.append(("chroot-try", env + ["auxo-tweak", "xbps", "on"]))
    return ops


def finish_ops(cfg):
    ops = [("try", ["umount", f"{TARGET}/dev"]), ("try", ["umount", f"{TARGET}/boot/efi"])]
    if cfg.fs == "ufs":
        ops.append(("try", ["umount", TARGET]))
    else:
        ops.append(("try", ["zfs", "umount", "-a"]))
    return ops


def build(cfg):
    return [
        Step(f"Partitioning {cfg.disk} (GPT, BIOS + UEFI)", partition_ops(cfg)),
        Step("Creating the ZFS pool and datasets" if cfg.fs == "zfs" else "Creating the UFS file system",
             filesystem_ops(cfg)),
        Step("Copying AuxoBSD to the disk", copy_ops(), weight=20),
        Step("Installing the boot loader", boot_ops(cfg)),
        Step("Setting up your system, account and graphics drivers", configure_ops(cfg), weight=4),
        Step("Finishing", finish_ops(cfg)),
    ]


def describe(steps):
    """Readable script of the plan (the installer's "show commands" view and the tests)."""
    out = []
    for s in steps:
        out.append(f"# {s.title}")
        for op in s.ops:
            if op[0] in ("run", "try"):
                out.append(shlex.join(op[1]) + ("    # may fail" if op[0] == "try" else ""))
            elif op[0].startswith("chroot"):
                out.append(f"chroot {TARGET} " + shlex.join(op[1]) + ("  < password" if len(op) > 2 else ""))
            elif op[0] == "write":
                out.append(f"write {TARGET}{op[1]}")
            elif op[0] == "copy":
                out.append(f"copy {SRC} → {TARGET}")
    return "\n".join(out)
