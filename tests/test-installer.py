#!/usr/bin/env python3
"""Tests for the Auxo installer.

  python3 tests/test-installer.py            logic tests + a full TUI walkthrough in a virtual terminal
  python3 tests/test-installer.py --shots D  also save PNG screenshots of each screen into D

The TUI test runs the real `auxo-installer --dry-run` in a pseudo-terminal, types
through every page like a user would, and checks what's on screen (via pyte).
"""
import json
import os
import pty
import select
import struct
import sys
import tempfile
import time
import fcntl
import termios

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "installer"))
os.environ.setdefault("AUXO_LIB", os.path.join(HERE, "packages/auxo-tools/files/lib"))

from auxo_install.config import InstallConfig, validate, validate_user  # noqa: E402
from auxo_install.disks import parse_lsblk, part_path, human  # noqa: E402
from auxo_install.plan import build_plan, Env, Cmd, Write, Py, rsync_percent  # noqa: E402
from auxo_install.runner import Context, execute, InstallError  # noqa: E402

passed = failed = 0


def check(name, cond):
    global passed, failed
    print(("  \033[32m✓\033[0m " if cond else "  \033[31m✗\033[0m ") + name)
    passed += bool(cond)
    failed += not cond


def dry_run(cfg, env, prepare=None):
    T = tempfile.mkdtemp(prefix="auxo-t-")
    env.target = T
    os.makedirs(f"{T}/etc/default", exist_ok=True)
    open(f"{T}/etc/default/libc-locales", "w").write("#en_US.UTF-8 UTF-8\n#de_DE.UTF-8 UTF-8\n")
    open(f"{T}/etc/rc.conf", "w").write('#KEYMAP="es"\n#HARDWARECLOCK="UTC"\nTIMEZONE="x"\n')
    open(f"{T}/etc/group", "w").write("root:x:0:\nwheel:x:4:\naudio:x:12:\nvideo:x:13:\ninput:x:24:\n")
    open(f"{T}/etc/hosts", "w").write("127.0.0.1 localhost\n")
    os.makedirs(f"{T}/etc/auxo", exist_ok=True)
    open(f"{T}/etc/auxo/live", "w").write('LIVE_USER="anon"\n')
    if prepare:
        prepare(T)
    steps, notes = build_plan(cfg, env)
    log = os.path.join(T, "install.log")
    ctx = Context(T, dry_run=True, log_path=log)
    notes += execute(steps, ctx)
    lines = [l[9:] for l in open(log).read().splitlines()]
    return T, steps, notes, lines


def has(lines, text):
    return any(text in l for l in lines)


# ── config + disks ────────────────────────────────────────────────────
print("▲ config validation")
ok = InstallConfig(disk="/dev/sda", username="alex", password="x", hostname="auxo")
check("valid config passes", validate(ok) == [])
check("bad username rejected", validate_user(InstallConfig(username="Alex!", password="x")) != [])
check("reserved username rejected", any("reserved" in e for e in validate_user(InstallConfig(username="root", password="x"))))
check("mismatched passwords", any("match" in e for e in validate_user(InstallConfig(username="a", password="x"), "y")))
check("bad hostname rejected", validate_user(InstallConfig(username="a", password="x", hostname="-bad-")) != [])
check("manual mode needs EFI on UEFI", any("EFI" in e for e in validate(InstallConfig(mode="manual", root_part="/dev/sda2", username="a", password="x"), efi=True)))
check("manual mode on BIOS doesn't", validate(InstallConfig(mode="manual", root_part="/dev/sda2", username="a", password="x"), efi=False) == [])
check("passwords redacted in to_dict", InstallConfig(password="secret").to_dict()["password"] != "secret")
check("console keymap for UK", InstallConfig(keyboard="gb").console_keymap == "uk")

print("▲ disks")
fixture = json.load(open(os.path.join(HERE, "tests/fixtures/lsblk.json")))
ds = parse_lsblk(fixture, live_device="/dev/sdb1")
paths = [d["path"] for d in ds]
check("zram skipped", "/dev/zram0" not in paths)
check("live USB listed but unusable", next(d for d in ds if d["path"] == "/dev/sdb")["reason"] == "this is the Auxo USB")
check("NVMe usable with 3 partitions", next(d for d in ds if d["path"] == "/dev/nvme0n1")["usable"]
      and len(next(d for d in ds if d["path"] == "/dev/nvme0n1")["parts"]) == 3)
check("partition names", part_path("/dev/sda", 2) == "/dev/sda2" and part_path("/dev/nvme0n1", 1) == "/dev/nvme0n1p1"
      and part_path("/dev/mmcblk0", 1) == "/dev/mmcblk0p1")
check("human sizes", human(1000204886016) == "1.0 TB")
check("rsync progress parse", rsync_percent("  1,234,567,890  45%   88.12MB/s    0:00:13 (xfr#12, to-chk=0/9)") == 45)

# ── plans ─────────────────────────────────────────────────────────────
print("▲ plan: UEFI, NVMe, btrfs, Plasma (offline)")
cfg = InstallConfig(disk="/dev/nvme0n1", username="alex", password="pw", fullname="Alex", hostname="peak",
                    keyboard="de", locale="de_DE.UTF-8", timezone="Europe/Berlin", accent="rose")
T, steps, notes, L = dry_run(cfg, Env(efi=True, online=False))
check("GPT: 1 GiB EFI + root", has(L, "sgdisk -n1:0:+1G -t1:ef00 -c1:EFI -n2:0:0 -t2:8300 -c2:Auxo /dev/nvme0n1"))
check("btrfs on p2, FAT32 on p1", has(L, "mkfs.btrfs -f -L Auxo /dev/nvme0n1p2") and has(L, "mkfs.vfat -F32 -n EFI /dev/nvme0n1p1"))
check("five subvolumes", sum(1 for l in L if "btrfs subvolume create" in l) == 5)
check("/boot kept uncompressed", has(L, f"chattr +m {T}/boot"))
check("live system copied with rsync", has(L, "rsync -aAXH --info=progress2") and has(L, "--exclude=/proc/*"))
fstab = open(f"{T}/etc/fstab").read()
check("fstab by UUID, subvolumes + EFI", "UUID=UUID-OF-nvme0n1p2 /home btrfs noatime,compress=zstd:1,subvol=@home" in fstab
      and "UUID=UUID-OF-nvme0n1p1 /boot/efi vfat" in fstab)
check("chroot: /run not bound (live services stay out)", has(L, "mount --rbind /dev") and not has(L, "mount --rbind /run"))
check("live user removed", has(L, "userdel -r anon"))
check("fresh machine-id for every install", has(L, f"rm -f {T}/etc/machine-id {T}/var/lib/dbus/machine-id")
      and has(L, "dbus-uuidgen --ensure=/etc/machine-id"))
check("hostname", open(f"{T}/etc/hostname").read() == "peak\n")
check("hosts has the computer name", "peak.localdomain peak" in open(f"{T}/etc/hosts").read())
check("locale uncommented + locale.conf", "de_DE.UTF-8 UTF-8" in open(f"{T}/etc/default/libc-locales").read().replace("#de", "X")
      and "LANG=de_DE.UTF-8" in open(f"{T}/etc/locale.conf").read())
check("timezone link", has(L, f"ln -sf /usr/share/zoneinfo/Europe/Berlin {T}/etc/localtime"))
rc = open(f"{T}/etc/rc.conf").read()
check("rc.conf keymap set once", rc.count("KEYMAP=") == 1 and 'KEYMAP="de"' in rc and 'TIMEZONE="x"' in rc)
check("keyboard for X11, Wayland and Plasma", '"XkbLayout" "de"' in open(f"{T}/etc/X11/xorg.conf.d/00-keyboard.conf").read()
      and "XKB_DEFAULT_LAYOUT=de" in open(f"{T}/etc/environment").read()
      and "LayoutList=de" in open(f"{T}/etc/xdg/kxkbrc").read())
check("user gets only groups that exist", has(L, "useradd -m -G wheel,audio,video,input -s /bin/bash -c Alex alex"))
check("password set via stdin, never logged", has(L, "chpasswd -c SHA512 <<< [hidden]") and not has(L, "alex:pw"))
check("root locked by default (hard step)", has(L, "passwd -l root")
      and not next(a for st in steps for a in st.actions if getattr(a, "argv", [])[-3:] == ["passwd", "-l", "root"]).soft)
check("sudo for wheel", "%wheel ALL=(ALL:ALL) ALL" in open(f"{T}/etc/sudoers.d/10-auxo-wheel").read())
check("auxo-tweak sets accent/shell, no boot splash", has(L, "auxo-tweak accent rose --no-grub --no-initramfs --user alex")
      and not has(L, "auxo-tweak splash") and has(L, "auxo-tweak shell zsh --user alex"))
check("auxo-tweak runs in installer mode", has(L, "env AUXO_IN_INSTALLER=1 auxo-tweak"))
check("Plasma kept (no --replace)", has(L, "auxo-tweak desktop plasma --user alex") and not has(L, "--replace"))
check("extras: drivers + zram", has(L, "auxo-tweak drivers --prune") and has(L, "auxo-tweak zram on"))
check("dracut, then GRUB for UEFI (+ fallback copy)", has(L, "dracut --regenerate-all --force")
      and has(L, "grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=Auxo --recheck")
      and has(L, "--removable"))
check("grub.cfg generated", has(L, "grub-mkconfig -o /boot/grub/grub.cfg"))
check("first snapshot after boot setup", L.index(next(l for l in L if "snapshots on" in l)) > L.index(next(l for l in L if "grub-install" in l)))
check("welcome app on first login", os.path.exists(f"{T}/home/alex/.config/autostart/auxo-welcome.desktop"))
check("unmounted at the end", L[-1].endswith(f"umount -R {T}"))

T2, _, _, L2 = dry_run(InstallConfig(disk="/dev/sda", username="z", password="p", filesystem="xfs"), Env(efi=True),
                       prepare=lambda T: open(f"{T}/etc/default/live.conf", "w").write("USERNAME=liveguy\n"))
check("live user name read from live.conf", has(L2, "userdel -r liveguy"))
check("xfs root: no fsck pass", "/ xfs defaults,noatime 0 0" in open(f"{T2}/etc/fstab").read())

print("▲ plan: offline fallbacks")
cfg2 = InstallConfig(disk="/dev/sda", username="sam", password="pw", desktop="hyprland", kernel="linux-lts",
                     filesystem="ext4", extras=["snapshots", "gaming", "zram"])
T, steps, notes, L = dry_run(cfg2, Env(efi=False, online=False))
check("hyprland → Plasma offline, with a note", has(L, "auxo-tweak desktop plasma") and any("hyprland needs internet" in n for n in notes))
check("LTS → stable kernel offline", not has(L, "auxo-tweak kernel") and any("linux-lts needs internet" in n for n in notes))
check("snapshots skipped on ext4", not has(L, "snapshots on") and any("need btrfs" in n for n in notes))
check("gaming skipped offline", not has(L, "gaming on"))
check("BIOS: 1 MiB bios_grub partition", has(L, "-n1:0:+1M -t1:ef02 -c1:BIOS"))
check("BIOS: no EFI formatting or mounting", not has(L, "mkfs.vfat") and not has(L, "/boot/efi"))
check("BIOS: GRUB into the disk", has(L, "grub-install --target=i386-pc --recheck /dev/sda"))
check("ext4 root in fstab", f"/ ext4 defaults,noatime 0 1" in open(f"{T}/etc/fstab").read())

print("▲ plan: online, other desktop, manual partitions (dual boot)")
cfg3 = InstallConfig(mode="manual", root_part="/dev/nvme0n1p3", efi_part="/dev/nvme0n1p1", username="kim", password="pw",
                     desktop="sway", kernel="linux-mainline", shell="fish", root_password="toor",
                     extras=["snapshots", "firewall", "flatpak", "gaming"])
T, steps, notes, L = dry_run(cfg3, Env(efi=True, online=True))
check("no repartitioning in manual mode", not has(L, "sgdisk") and not has(L, "wipefs -af /dev/nvme0n1\n"))
check("only the chosen root is formatted", has(L, "mkfs.btrfs -f -L Auxo /dev/nvme0n1p3") and not has(L, "mkfs.vfat"))
check("existing EFI partition mounted", has(L, f"mount /dev/nvme0n1p1 {T}/boot/efi"))
check("sway replaces the live Plasma", has(L, "auxo-tweak desktop sway --replace --user kim"))
check("mainline kernel", has(L, "auxo-tweak kernel linux-mainline"))
check("fish shell", has(L, "auxo-tweak shell fish --user kim"))
check("root password set, not locked", not has(L, "passwd -l root") and sum(1 for l in L if "chpasswd" in l) == 2)
check("extras: firewall, flatpak, gaming", has(L, "auxo-tweak firewall on") and has(L, "auxo-tweak flatpak on")
      and has(L, "auxo-tweak gaming on --user kim"))

print("▲ runner")
T = tempfile.mkdtemp()
ctx = Context(T, dry_run=False, log_path=os.path.join(T, "log"))
from auxo_install.plan import Step  # noqa: E402
try:
    execute([Step("x", "x", [Cmd(["false"], soft=True), Cmd(["sh", "-c", "echo hi"])])], ctx)
    check("soft failure becomes a note", len(ctx.notes) == 1 and "failed" in ctx.notes[0])
except InstallError:
    check("soft failure becomes a note", False)
try:
    execute([Step("x", "x", [Cmd(["false"])])], ctx)
    check("hard failure stops the install", False)
except InstallError:
    check("hard failure stops the install", True)
seen = []
execute([Step("x", "x", [Cmd(["sh", "-c", "printf '  10%% \\r  55%% \\r 100%% \\n'"], progress="rsync")])], ctx,
        on_progress=lambda p: seen.append(round(p, 2)))
check("rsync progress drives the bar", 0.55 in seen and seen[-1] == 1.0)
execute([Step("x", "x", [Cmd(["sh", "-c", "cat"], input="secret\n")])], ctx)
check("stdin input works and isn't logged as text", "<<< [hidden]" in open(os.path.join(T, "log")).read())


# ── the real TUI in a virtual terminal ───────────────────────────────
def tui_walkthrough(shots_dir=None):
    import pyte
    cols, rows = 100, 32
    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)
    env = dict(os.environ, TERM=os.environ.get("AUXO_TEST_TERM", "screen-256color"), AUXO_TEST_DISKS=os.path.join(HERE, "tests/fixtures/lsblk.json"),
               ESCDELAY="25", LANG="C.UTF-8", LC_ALL="C.UTF-8")
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(sys.stdout.fileno(), termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.execvpe(sys.executable, [sys.executable, os.path.join(HERE, "installer/auxo-installer"),
                                    "--dry-run", "--offline"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    shots = []

    def pump(t=0.5):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return False
                if not data:
                    return False
                stream.feed(data)
        return True

    def text():
        return "\n".join(screen.display)

    def snap(name):
        shots.append((name, [list(screen.buffer[y][x] for x in range(cols)) for y in range(rows)]))

    def send(keys, t=0.35):
        os.write(fd, keys.encode() if isinstance(keys, str) else keys)
        pump(t)

    # send exactly what this terminal type sends for each key (from terminfo)
    import curses
    curses.setupterm(env["TERM"], sys.__stdout__.fileno() if sys.__stdout__.isatty() else os.open(os.devnull, os.O_WRONLY))
    k = lambda cap: curses.tigetstr(cap).decode()
    ENTER, TAB, ESC = "\r", "\t", "\x1b"
    DOWN, UP, END = k("kcud1"), k("kcuu1"), k("kend")
    pump(1.5)
    res = {}
    res["welcome"] = "Welcome to Auxo Linux" in text() and "DRY RUN" in text()
    snap("01-welcome")
    send(ENTER)
    res["keyboard"] = "Keyboard layout" in text() and "English (US)" in text()
    send("germ", 0.4)
    res["search"] = "German" in text() and "French" not in text()
    snap("02-keyboard")
    send(ENTER)
    res["language"] = "Language" in text()
    send(ENTER)
    res["timezone"] = "Time zone" in text()
    send(END); send(ENTER)                     # UTC
    res["disk"] = "Where should Auxo go?" in text() and "too small (needs 20.0 GB)" in text()
    snap("03-disk")
    send(DOWN); send(ENTER)                    # /dev/sda (2 TB)
    res["filesystem"] = "Filesystem" in text() and "btrfs" in text()
    send(ENTER)
    res["desktop"] = "Desktop" in text() and "Hyprland" in text() and "needs internet" in text() and "Auxo global theme" in text()
    snap("04-desktop")
    send(ENTER)                                # Plasma
    send(DOWN); send(ENTER)                    # accent: cyan
    res["accent"] = "Kernel" in text()
    snap("05-kernel")
    send(ENTER); send(ENTER)                   # kernel + shell
    res["account"] = "Your account" in text()
    send("Alex Doe" + TAB + "alex" + TAB + "pw" + TAB + "pX" + TAB + TAB, 0.3)   # → root password field
    send(ENTER)                                # submit from the last field
    res["mismatch"] = "don't match" in text()
    send(UP + UP, 0.3); send("\x7f" + "w", 0.3)   # "Password again": pX → pw
    send(TAB + TAB, 0.2)
    snap("06-account")
    send(ENTER)
    res["extras"] = "Extras" in text() and "[x]" in text()
    snap("07-extras")
    send(ENTER)
    res["review"] = "Ready to install" in text() and "ERASE /dev/sda" in text() and "KDE Plasma" in text() and "German" in text()
    snap("08-review")
    send(ENTER)
    res["confirm"] = "Install Auxo now?" in text() and "ALL DATA on /dev/sda" in text()
    snap("09-confirm")
    send("y", 3.0)
    res["installed"] = "Auxo is installed" in text()
    snap("10-done")
    send(ENTER, 1.0)
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    os.waitpid(pid, 0)
    if shots_dir:
        render(shots, shots_dir, cols, rows)
    return res


def render(shots, out, cols, rows):
    from PIL import Image, ImageDraw, ImageFont
    os.makedirs(out, exist_ok=True)
    font = None
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf"):
        if os.path.exists(f):
            font = ImageFont.truetype(f, 16)
    font = font or ImageFont.load_default()
    cw, ch = 10, 20
    named = {"black": (18, 18, 18), "red": (248, 113, 113), "green": (52, 211, 153), "brown": (251, 191, 36),
             "yellow": (251, 191, 36), "blue": (96, 165, 250), "magenta": (167, 139, 250), "cyan": (34, 211, 238),
             "white": (229, 229, 229), "default": (18, 18, 18)}

    def col(c, fg):
        if c == "default":
            return (232, 232, 240) if fg else (18, 18, 18)
        if c in named:
            return named[c]
        try:
            return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))
        except (ValueError, TypeError):
            return (232, 232, 240) if fg else (18, 18, 18)
    for name, buf in shots:
        img = Image.new("RGB", (cols * cw, rows * ch), (18, 18, 18))
        d = ImageDraw.Draw(img)
        for y, line in enumerate(buf):
            for x, c in enumerate(line):
                fg, bg = col(c.fg, True), col(c.bg, False)
                if c.reverse:
                    fg, bg = bg, fg
                if bg != (18, 18, 18):
                    d.rectangle([x * cw, y * ch, (x + 1) * cw - 1, (y + 1) * ch - 1], fill=bg)
                if c.data.strip():
                    d.text((x * cw, y * ch + 1), c.data, font=font, fill=fg)
        img.save(os.path.join(out, name + ".png"))


print("▲ TUI walkthrough (real installer in a virtual terminal)")
shots = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
try:
    import pyte  # noqa: F401
    r = tui_walkthrough(shots)
    labels = {"welcome": "welcome screen (dry-run badge)", "keyboard": "keyboard list", "search": "type-to-search filters",
              "language": "language page", "timezone": "time zone page", "disk": "disk list, small USB marked unusable",
              "filesystem": "filesystem choice", "desktop": "desktops with 'needs internet' when offline",
              "accent": "accent → kernel", "account": "account form", "mismatch": "password mismatch caught",
              "extras": "extras checklist", "review": "review shows the disk to erase", "confirm": "red confirmation dialog",
              "installed": "dry-run install reaches the finish screen"}
    for k, label in labels.items():
        check(label, r.get(k, False))
except ImportError:
    print("  (pyte not installed — skipping: pip install pyte)")

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
