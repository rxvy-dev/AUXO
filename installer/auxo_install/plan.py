"""Turn an InstallConfig into an ordered list of steps.

Nothing here touches the system: a step is just data (commands to run, files to
write, small Python actions that need runtime values such as UUIDs). The runner
executes them; the tests inspect them.

The install follows the same sequence as Void's own installer (copy the live
system, then configure it), with Auxo's choices applied through auxo-tweak so the
installer and the post-install tool share one code path.
"""
import os
import re
from dataclasses import dataclass, field

from .disks import part_path

TARGET = "/mnt/target"
BTRFS_OPTS = "noatime,compress=zstd:1"
SUBVOLS = [("@", ""), ("@home", "home"), ("@log", "var/log"), ("@cache", "var/cache"), ("@snapshots", ".snapshots")]
USER_GROUPS = ["wheel", "audio", "video", "input", "storage", "network", "lp", "scanner", "plugdev",
               "kvm", "bluetooth", "_seatd"]
ONLINE_DESKTOPS = {"gnome", "xfce", "cinnamon", "hyprland", "sway", "i3"}
RSYNC_EXCLUDES = ["/dev/*", "/proc/*", "/sys/*", "/run/*", "/tmp/*", "/mnt/*", "/media/*", "/lost+found",
                  "/var/cache/xbps/*", "/swapfile"]
# live-session files: from the Auxo ISO overlay, plus what void-mklive's boot hooks create
LIVE_FILES = ["/etc/auxo/live", "/etc/xdg/autostart/auxo-live-welcome.desktop",
              "/usr/share/applications/auxo-install.desktop", "/etc/profile.d/auxo-live.sh",
              "/etc/sudoers.d/99-void-live", "/etc/polkit-1/rules.d/void-live.rules",
              "/etc/sddm.conf", "/etc/default/live.conf"]


@dataclass
class Env:
    efi: bool = True
    efi_bits: int = 64
    online: bool = True
    target: str = TARGET


@dataclass
class Cmd:
    argv: list
    input: str = None       # sent to stdin (passwords for chpasswd); never logged
    soft: bool = False      # failure is reported as a note, not fatal
    progress: str = None    # "rsync": parse rsync --info=progress2 percentages
    desc: str = ""

    def show(self):
        a = " ".join(self.argv)
        return a + (" <<< [hidden]" if self.input else "")


@dataclass
class Write:
    path: str
    content: str
    mode: int = 0o644
    append: bool = False

    def show(self):
        return f"{'append' if self.append else 'write'} {self.path}"


@dataclass
class Py:
    fn: object              # fn(ctx) -> list of Cmd/Write (expanded at run time) or None
    desc: str = ""

    def show(self):
        return self.desc


@dataclass
class Step:
    key: str
    title: str
    actions: list = field(default_factory=list)
    weight: int = 1         # share of the progress bar


def chroot(T, *argv):
    return ["chroot", T, *argv]


def tweak(T, *args):
    # AUXO_IN_INSTALLER keeps auxo-tweak from talking to the live system's services
    return Cmd(chroot(T, "env", "AUXO_IN_INSTALLER=1", "auxo-tweak", *args), soft=True,
               desc="auxo-tweak " + " ".join(args))


# ── runtime helpers (need the real system) ────────────────────────────
def _fstab(cfg, layout):
    def fn(ctx):
        root_uuid = ctx.uuid(layout["root"])
        lines = ["# /etc/fstab — written by the Auxo installer", "# <device> <mountpoint> <type> <options> <dump> <pass>"]
        if cfg.filesystem == "btrfs":
            for sv, mp in SUBVOLS:
                lines.append(f"UUID={root_uuid} /{mp} btrfs {BTRFS_OPTS},subvol={sv} 0 0")
        else:
            opts = "defaults,noatime"
            passno = 0 if cfg.filesystem == "xfs" else 1   # like Void's installer: no fsck pass for xfs
            lines.append(f"UUID={root_uuid} / {cfg.filesystem} {opts} 0 {passno}")
        if layout.get("efi"):
            lines.append(f"UUID={ctx.uuid(layout['efi'])} /boot/efi vfat defaults,umask=0077 0 2")
        lines.append("tmpfs /tmp tmpfs defaults,nosuid,nodev 0 0")
        return [Write(f"{ctx.target}/etc/fstab", "\n".join(lines) + "\n")]
    return Py(fn, "write /etc/fstab (by UUID)")


def _locale(cfg):
    def fn(ctx):
        path = f"{ctx.target}/etc/default/libc-locales"
        txt = ctx.read(path)
        out, found = [], False
        for line in txt.splitlines():
            if line.lstrip("#").startswith(cfg.locale + " "):
                line = line.lstrip("#")
                found = True
            out.append(line)
        if not found:
            out.append(f"{cfg.locale} UTF-8")
        return [Write(path, "\n".join(out) + "\n"),
                Write(f"{ctx.target}/etc/locale.conf", f"LANG={cfg.locale}\nLC_COLLATE=C\n"),
                Cmd(chroot(ctx.target, "xbps-reconfigure", "-f", "glibc-locales"), soft=True)]
    return Py(fn, f"locale {cfg.locale}")


def _rc_conf(cfg):
    def fn(ctx):
        path = f"{ctx.target}/etc/rc.conf"
        lines = ctx.read(path).splitlines()
        want = {"KEYMAP": cfg.console_keymap, "HARDWARECLOCK": "UTC"}
        out = []
        for line in lines:
            key = line.lstrip("#").split("=", 1)[0].strip()
            if key in want:
                continue
            out.append(line)
        out += [f'{k}="{v}"' for k, v in want.items()]
        return [Write(path, "\n".join(out) + "\n")]
    return Py(fn, f"console keymap {cfg.console_keymap}")


def _environment(cfg):
    def fn(ctx):
        path = f"{ctx.target}/etc/environment"
        lines = [l for l in ctx.read(path).splitlines() if not l.startswith("XKB_DEFAULT_LAYOUT=")]
        lines.append(f"XKB_DEFAULT_LAYOUT={cfg.keyboard}")
        return [Write(path, "\n".join(lines) + "\n")]
    return Py(fn, f"keyboard {cfg.keyboard} for Wayland compositors")


def _hosts(cfg):
    def fn(ctx):
        path = f"{ctx.target}/etc/hosts"
        lines = [l for l in ctx.read(path).splitlines() if not l.startswith("127.0.1.1")]
        lines.append(f"127.0.1.1\t{cfg.hostname}.localdomain {cfg.hostname}")
        return [Write(path, "\n".join(lines) + "\n")]
    return Py(fn, "hosts")


def _useradd(cfg):
    def fn(ctx):
        groups = ctx.read(f"{ctx.target}/etc/group")
        have = {l.split(":", 1)[0] for l in groups.splitlines() if l}
        g = [x for x in USER_GROUPS if x in have] or ["wheel"]
        argv = chroot(ctx.target, "useradd", "-m", "-G", ",".join(g), "-s", "/bin/bash")
        if cfg.fullname:
            argv += ["-c", cfg.fullname]
        return [Cmd(argv + [cfg.username])]
    return Py(fn, f"create user {cfg.username}")


def _live_user(ctx):
    """Remove the live session's user. void-mklive records it in /etc/default/live.conf
    (USERNAME=, changeable with the live.user= boot option); /etc/auxo/live is the fallback."""
    user = "anon"
    for path, key in ((f"{ctx.target}/etc/auxo/live", "LIVE_USER="), (f"{ctx.target}/etc/default/live.conf", "USERNAME=")):
        for line in ctx.read(path).splitlines():
            if line.startswith(key):
                user = line.split("=", 1)[1].strip().strip('"') or user
    return [Cmd(chroot(ctx.target, "userdel", "-r", user), soft=True)]


def _bios_disk(cfg):
    def fn(ctx):
        disk = cfg.disk or ctx.parent_disk(cfg.root_part)
        return [Cmd(chroot(ctx.target, "grub-install", "--target=i386-pc", "--recheck", disk))]
    return Py(fn, "install GRUB to the disk's boot sector")


# ── the plan ──────────────────────────────────────────────────────────
def build_plan(cfg, env):
    """Returns (steps, notes). notes explain choices the installer had to change."""
    T = env.target
    steps, notes = [], []
    desktop, kernel = cfg.desktop, cfg.kernel
    if desktop in ONLINE_DESKTOPS and not env.online:
        notes.append(f"{desktop} needs internet — KDE Plasma installed instead (switch later: auxo-tweak desktop {desktop})")
        desktop = "plasma"
    if kernel != "linux" and not env.online:
        notes.append(f"{kernel} needs internet — the stable kernel is installed (switch later: auxo-tweak kernel {kernel})")
        kernel = "linux"
    extras = list(cfg.extras)
    if "snapshots" in extras and cfg.filesystem != "btrfs":
        notes.append("snapshots need btrfs — skipped")
        extras.remove("snapshots")
    for x in ("flatpak", "gaming"):
        if x in extras and not env.online:
            notes.append(f"{x} needs internet — skipped (turn it on later: auxo-tweak {x} on)")
            extras.remove(x)

    # 1 ── partitions
    if cfg.mode == "erase":
        d = cfg.disk
        layout = {"root": part_path(d, 2), "efi": part_path(d, 1) if env.efi else None}
        first = (["-n1:0:+1G", "-t1:ef00", "-c1:EFI"] if env.efi else ["-n1:0:+1M", "-t1:ef02", "-c1:BIOS"])
        steps.append(Step("partition", f"Partitioning {d}", [
            Cmd(["wipefs", "-af", d]),
            Cmd(["sgdisk", "--zap-all", d]),
            Cmd(["sgdisk", *first, "-n2:0:0", "-t2:8300", "-c2:Auxo", d]),
            Cmd(["partprobe", d], soft=True),
            Cmd(["udevadm", "settle"], soft=True),
        ]))
        format_efi = env.efi
    else:
        layout = {"root": cfg.root_part, "efi": cfg.efi_part if env.efi else None}
        format_efi = env.efi and cfg.format_efi

    # 2 ── filesystems
    root = layout["root"]
    mk = {"btrfs": ["mkfs.btrfs", "-f", "-L", "Auxo", root],
          "ext4": ["mkfs.ext4", "-F", "-L", "Auxo", root],
          "xfs": ["mkfs.xfs", "-f", "-L", "Auxo", root]}[cfg.filesystem]
    acts = [Cmd(["wipefs", "-af", root]), Cmd(mk)]
    if format_efi:
        acts.append(Cmd(["mkfs.vfat", "-F32", "-n", "EFI", layout["efi"]]))
    steps.append(Step("format", "Creating filesystems", acts))

    # 3 ── mount
    acts = [Cmd(["mkdir", "-p", T])]
    if cfg.filesystem == "btrfs":
        acts += [Cmd(["mount", "-o", BTRFS_OPTS, root, T])]
        acts += [Cmd(["btrfs", "subvolume", "create", f"{T}/{sv}"]) for sv, _ in SUBVOLS]
        acts += [Cmd(["umount", T]),
                 Cmd(["mount", "-o", f"{BTRFS_OPTS},subvol=@", root, T])]
        for sv, mp in SUBVOLS[1:]:
            acts += [Cmd(["mkdir", "-p", f"{T}/{mp}"]),
                     Cmd(["mount", "-o", f"{BTRFS_OPTS},subvol={sv}", root, f"{T}/{mp}"])]
        # GRUB can't always read zstd-compressed kernels: keep /boot uncompressed
        acts += [Cmd(["mkdir", "-p", f"{T}/boot"]),
                 Cmd(["chattr", "+m", f"{T}/boot"], soft=True),
                 Cmd(["btrfs", "property", "set", f"{T}/boot", "compression", "none"], soft=True)]
    else:
        acts += [Cmd(["mount", root, T])]
    if layout.get("efi"):
        acts += [Cmd(["mkdir", "-p", f"{T}/boot/efi"]), Cmd(["mount", layout["efi"], f"{T}/boot/efi"])]
    steps.append(Step("mount", "Mounting", acts))

    # 4 ── copy the live system
    rsync = ["rsync", "-aAXH", "--info=progress2", "--no-inc-recursive", "--one-file-system"]
    rsync += [f"--exclude={e}" for e in RSYNC_EXCLUDES] + ["/", f"{T}/"]
    steps.append(Step("copy", "Copying Auxo to your disk", [Cmd(rsync, progress="rsync")], weight=12))

    # 5 ── chroot setup + live leftovers
    acts = [_fstab(cfg, layout)]
    for fs in ("dev", "proc", "sys"):  # not /run: the live system's services must stay out of reach
        acts += [Cmd(["mkdir", "-p", f"{T}/{fs}"]), Cmd(["mount", "--rbind", f"/{fs}", f"{T}/{fs}"]),
                 Cmd(["mount", "--make-rslave", f"{T}/{fs}"], soft=True)]
    acts += [Cmd(["cp", "-L", "/etc/resolv.conf", f"{T}/etc/resolv.conf"], soft=True),
             Py(_live_user, "remove the live user"),
             # every install needs its own machine id (the ISO's is baked in by dbus at build time)
             Cmd(["rm", "-f", f"{T}/etc/machine-id", f"{T}/var/lib/dbus/machine-id"]),
             Cmd(chroot(T, "dbus-uuidgen", "--ensure=/etc/machine-id")),
             Cmd(["ln", "-sf", "/etc/machine-id", f"{T}/var/lib/dbus/machine-id"], soft=True),
             Cmd(["rm", "-f", *[f"{T}{p}" for p in LIVE_FILES]]),
             Cmd(["sed", "-i", 's/GETTY_ARGS="--noclear -a [^"]*"/GETTY_ARGS="--noclear"/',
                  f"{T}/etc/sv/agetty-tty1/conf"], soft=True),
             Cmd(["xbps-remove", "-r", T, "-Ry", "auxo-installer"], soft=True)]
    steps.append(Step("prepare", "Preparing the new system", acts))

    # 6 ── basic settings
    steps.append(Step("settings", "Language, keyboard and time", [
        Write(f"{T}/etc/hostname", cfg.hostname + "\n"),
        _hosts(cfg),
        _locale(cfg),
        Cmd(["ln", "-sf", f"/usr/share/zoneinfo/{cfg.timezone}", f"{T}/etc/localtime"]),
        _rc_conf(cfg),
        _environment(cfg),
        Write(f"{T}/etc/X11/xorg.conf.d/00-keyboard.conf",
              'Section "InputClass"\n    Identifier "system-keyboard"\n    MatchIsKeyboard "on"\n'
              f'    Option "XkbLayout" "{cfg.keyboard}"\nEndSection\n'),
        Write(f"{T}/etc/xdg/kxkbrc", f"[Layout]\nLayoutList={cfg.keyboard}\nUse=true\n"),
    ]))

    # 7 ── users
    acts = [_useradd(cfg),
            Cmd(chroot(T, "chpasswd", "-c", "SHA512"), input=f"{cfg.username}:{cfg.password}\n"),
            Write(f"{T}/etc/sudoers.d/10-auxo-wheel", "# written by the Auxo installer\n%wheel ALL=(ALL:ALL) ALL\n", mode=0o440)]
    if cfg.root_password:
        acts.append(Cmd(chroot(T, "chpasswd", "-c", "SHA512"), input=f"root:{cfg.root_password}\n"))
    else:
        # the live system's root password ("voidlinux") was copied over: locking root is required
        acts.append(Cmd(chroot(T, "passwd", "-l", "root")))
    steps.append(Step("users", f"Creating {cfg.username}", acts))

    # 8 ── Auxo: the same commands the user can run later
    u = ["--user", cfg.username]
    acts = [tweak(T, "accent", cfg.accent, "--no-grub", "--no-initramfs", *u),
            tweak(T, "shell", cfg.shell, *u)]
    if kernel != "linux":
        acts.append(tweak(T, "kernel", kernel, "--no-grub", "--no-initramfs"))
    if desktop == "plasma":
        acts.append(tweak(T, "desktop", "plasma", *u))
    else:
        acts.append(tweak(T, "desktop", desktop, "--replace", *u))
    if "drivers" in extras:
        acts.append(tweak(T, "drivers", "--prune", "--no-initramfs"))
    for x in ("zram",):
        if x in extras:
            acts.append(tweak(T, x, "on"))
    if "firewall" in extras:
        acts.append(tweak(T, "firewall", "on"))
    if "flatpak" in extras:
        acts.append(tweak(T, "flatpak", "on"))
    if "gaming" in extras:
        acts.append(tweak(T, "gaming", "on", *u))
    acts.append(Write(f"{T}/home/{cfg.username}/.config/autostart/auxo-welcome.desktop",
                      "[Desktop Entry]\nType=Application\nName=Welcome to Auxo\nExec=auxo-welcome\n"
                      "X-GNOME-Autostart-enabled=true\n"))
    acts.append(Cmd(chroot(T, "chown", "-R", f"{cfg.username}:{cfg.username}", f"/home/{cfg.username}"), soft=True))
    steps.append(Step("auxo", "Setting up your desktop", acts, weight=8))

    # 9 ── boot
    acts = [Cmd(chroot(T, "dracut", "--regenerate-all", "--force"))]
    if env.efi:
        tgt = "i386-efi" if env.efi_bits == 32 else "x86_64-efi"
        acts += [Cmd(chroot(T, "grub-install", f"--target={tgt}", "--efi-directory=/boot/efi",
                            "--bootloader-id=Auxo", "--recheck")),
                 # a fallback copy for firmware that forgets boot entries
                 Cmd(chroot(T, "grub-install", f"--target={tgt}", "--efi-directory=/boot/efi",
                            "--removable", "--recheck"), soft=True)]
    else:
        acts.append(_bios_disk(cfg))
    acts.append(Cmd(chroot(T, "grub-mkconfig", "-o", "/boot/grub/grub.cfg")))
    steps.append(Step("boot", "Installing the boot loader", acts, weight=3))

    # 10 ── snapshots last, so the first one is the finished system
    if "snapshots" in extras:
        steps.append(Step("snapshots", "Taking the first snapshot", [
            tweak(T, "snapshots", "on", "--no-now", "--no-grub", "--description", "Fresh Auxo install"),
            Cmd(chroot(T, "grub-mkconfig", "-o", "/boot/grub/grub.cfg"), soft=True),
        ]))

    # 11 ── finish
    steps.append(Step("finish", "Finishing up", [
        Cmd(["cp", "/tmp/auxo-install.log", f"{T}/var/log/auxo-install.log"], soft=True),
        Cmd(["sync"]),
        Cmd(["umount", "-R", T], soft=True),
    ]))
    return steps, notes


def describe(cfg, env):
    """Human summary lines for the review screen."""
    if cfg.mode == "erase":
        disk = f"ERASE {cfg.disk} — everything on it is deleted"
    else:
        disk = f"{cfg.root_part} as /" + (f", {cfg.efi_part} as EFI" if env.efi else "")
    return [("Disk", disk), ("Filesystem", cfg.filesystem), ("Desktop", cfg.desktop), ("Accent", cfg.accent),
            ("Kernel", cfg.kernel), ("Shell", cfg.shell), ("User", f"{cfg.username} ({cfg.fullname or '-'})"),
            ("Computer", cfg.hostname), ("Keyboard", cfg.keyboard), ("Language", cfg.locale),
            ("Time zone", cfg.timezone), ("Extras", ", ".join(cfg.extras) or "none"),
            ("Boot", "UEFI" if env.efi else "BIOS (legacy)")]


def rsync_percent(line):
    """'  1,234,567  45%   12.3MB/s ...' → 45 (rsync --info=progress2)."""
    m = re.search(r"\s(\d{1,3})%\s", " " + line + " ")
    return int(m.group(1)) if m else None
