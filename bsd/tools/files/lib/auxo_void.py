"""xbps on AuxoBSD: a Void Linux userland in /compat/void, run by FreeBSD's Linux
compatibility layer (linux64.ko). FreeBSD's own packages stay on pkg; xbps-install puts
Void Linux packages (the same repositories as Auxo Linux) into /compat/void, and void-run
starts their programs.

What works: most command-line tools and many X11 apps. What doesn't: anything that needs
Linux kernel modules, systemd/runit services, or GPU drivers built for Linux.
"""
import os

from auxo_bsd import R, run, write_file, read_file, set_kv, log

VOID = "/compat/void"
LIBEXEC = "/usr/local/libexec/auxo"
STATIC = f"{LIBEXEC}/xbps-static"
REPO = "https://repo-default.voidlinux.org/current"
STATIC_URL = "https://repo-default.voidlinux.org/static/xbps-static-latest.x86_64-musl.tar.xz"
BASE = "base-voidstrap"
TOOLS = ["xbps-install", "xbps-remove", "xbps-query", "xbps-reconfigure", "xbps-pkgdb", "xbps-alternatives"]

FSTAB_MARK = "# auxo: Void Linux userland for xbps (auxo-tweak xbps)"
FSTAB = f"""{FSTAB_MARK}
devfs      {VOID}/dev      devfs      rw,late                    0 0
tmpfs      {VOID}/dev/shm  tmpfs      rw,late,size=1g,mode=1777  0 0
fdescfs    {VOID}/dev/fd   fdescfs    rw,late,linrdlnk           0 0
linprocfs  {VOID}/proc     linprocfs  rw,late                    0 0
linsysfs   {VOID}/sys      linsysfs   rw,late                    0 0
/tmp       {VOID}/tmp      nullfs     rw,late                    0 0
/home      {VOID}/home     nullfs     rw,late                    0 0
"""

MOUNTS = f"""#!/bin/sh
# Mount what the Void userland needs (once). Called by the xbps wrappers and void-run.
for d in dev dev/shm dev/fd proc sys tmp home; do
    mount -p | awk '{{print $2}}' | grep -qx "{VOID}/$d" && continue
    mount -F /etc/fstab "{VOID}/$d" 2>/dev/null || true
done
cp -f /etc/resolv.conf {VOID}/etc/resolv.conf 2>/dev/null || true
"""

WRAPPER = f"""#!/bin/sh
# Void Linux packages on AuxoBSD: runs Void's xbps inside {VOID} (see auxo-tweak xbps).
# FreeBSD packages: use pkg.
[ -x {VOID}/usr/bin/xbps-install ] || {{ echo "xbps isn't set up yet: sudo auxo-tweak xbps on" >&2; exit 1; }}
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
kldstat -qm linux64 || kldload linux64
{LIBEXEC}/void-mounts
exec /usr/sbin/chroot {VOID} /usr/bin/env XBPS_ARCH=x86_64 /usr/bin/$(basename "$0") "$@"
"""

VOID_RUN = f"""#!/bin/sh
# void-run — start a program from the Void Linux userland as yourself.
#   void-run neofetch        void-run some-linux-app
[ $# -gt 0 ] || {{ echo "usage: void-run COMMAND [ARGS...]" >&2; exit 2; }}
me=$(id -un)
exec sudo -- sh -c '
    kldstat -qm linux64 || kldload linux64
    {LIBEXEC}/void-mounts
    u="$1"; shift
    exec /usr/sbin/chroot -u "$u" -G "$(id -Gn "$u" | tr " " ,)" {VOID} \\
        /usr/bin/env HOME="/home/$u" USER="$u" PATH=/usr/local/bin:/usr/bin:/bin \\
        DISPLAY="$DISPLAY" XAUTHORITY="$XAUTHORITY" WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \\
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" LANG="${{LANG:-C.UTF-8}}" "$@"
' sh "$me" "$@"
"""


def is_setup():
    return os.path.exists(R(f"{VOID}/usr/bin/xbps-install"))


def _fstab_add():
    cur = read_file("/etc/fstab") or ""
    if FSTAB_MARK in cur:
        return
    write_file("/etc/fstab", cur.rstrip("\n") + "\n\n" + FSTAB)


def _fstab_remove():
    cur = read_file("/etc/fstab") or ""
    if FSTAB_MARK not in cur:
        return
    keep, skip = [], False
    for line in cur.splitlines():
        if line == FSTAB_MARK:
            skip = True
            continue
        if skip and line.split() and line.split()[1].startswith(VOID + "/"):
            continue
        skip = False
        keep.append(line)
    write_file("/etc/fstab", "\n".join(keep).rstrip("\n") + "\n")


def setup():
    """Download xbps (static), bootstrap a minimal Void userland, install the wrappers."""
    log("enabling FreeBSD's Linux compatibility layer")
    set_kv("/etc/rc.conf", "linux_enable", "YES")
    run(["kldload", "-n", "linux64"], check=False)
    for d in ("dev/shm", "dev/fd", "proc", "sys", "tmp", "home", "etc"):
        run(["mkdir", "-p", f"{VOID}/{d}"])
    run(["mkdir", "-p", STATIC])
    log("downloading xbps (static build from repo-default.voidlinux.org)")
    run(["fetch", "-o", "/tmp/xbps-static.tar.xz", STATIC_URL])
    run(["tar", "-xf", "/tmp/xbps-static.tar.xz", "-C", STATIC])
    # static musl binaries carry no OS note: mark them as Linux programs
    run(["sh", "-c", f"for f in {STATIC}/usr/bin/xbps-*.static; do brandelf -t Linux \"$f\"; done"])
    write_file(f"{LIBEXEC}/void-mounts", MOUNTS, mode=0o755)
    _fstab_add()
    run([f"{LIBEXEC}/void-mounts"])
    log(f"installing the Void base ({BASE}) into {VOID}")
    run(["env", "XBPS_ARCH=x86_64", f"{STATIC}/usr/bin/xbps-install.static", "-S", "-y",
         "-r", VOID, "-R", REPO, BASE])
    for t in TOOLS:
        write_file(f"/usr/local/bin/{t}", WRAPPER, mode=0o755)
    write_file("/usr/local/bin/void-run", VOID_RUN, mode=0o755)
    log("done: xbps-install -S PACKAGE puts Void packages in /compat/void; void-run COMMAND starts them")


def remove(purge=False):
    for t in TOOLS + ["void-run"]:
        body = read_file(f"/usr/local/bin/{t}") or ""
        if "auxo-tweak xbps" in body or "void-run —" in body:     # only our wrappers
            os.remove(R(f"/usr/local/bin/{t}"))
    run(["sh", "-c", f"mount -p | awk '$2 ~ \"^{VOID}/\" {{print $2}}' | sort -r | xargs -n1 umount -f"],
        check=False)
    _fstab_remove()
    if purge:
        run(["chflags", "-R", "noschg", VOID], check=False)
        run(["rm", "-rf", VOID, STATIC], check=False)
        log(f"removed {VOID}")
    else:
        log(f"xbps wrappers removed; the Void userland stays in {VOID} (--purge deletes it)")


def status():
    if not is_setup():
        print("xbps: not set up (sudo auxo-tweak xbps on)")
        return
    out = run(["/usr/sbin/chroot", VOID, "/usr/bin/xbps-query", "-l"], capture=True, check=False) or ""
    n = len([l for l in out.splitlines() if l.strip()])
    print(f"xbps: on — Void Linux userland in {VOID}, {n} packages")
