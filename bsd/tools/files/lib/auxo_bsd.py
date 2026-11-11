"""Shared helpers for the AuxoBSD tools (FreeBSD base: pkg, rc.conf, ZFS boot environments).

Same conventions as the Auxo Linux tools (packages/auxo-tools/files/lib/auxo_core.py):
  AUXO_ROOT=/path   prefix every file path we read/write (default: /)
  AUXO_DRY_RUN=1    print commands instead of running them (the test-suite uses this)
"""
import os
import pwd
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.environ.get("AUXO_ROOT", "/")
DRY = os.environ.get("AUXO_DRY_RUN") == "1"
CONF = "/etc/auxo/auxo.conf"
SHARE = "/usr/local/share/auxo"
WALLDIR = "/usr/local/share/wallpapers/auxo"
RC_CONF = "/etc/rc.conf"
LOADER_CONF = "/boot/loader.conf"

RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"


class AuxoError(Exception):
    pass


def R(path):
    """Map an absolute system path onto AUXO_ROOT."""
    return os.path.join(ROOT, path.lstrip("/"))


def log(msg):
    print(f"{BOLD}::{RESET} {msg}", flush=True)


def warn(msg):
    print(f"\033[33m!!\033[0m {msg}", file=sys.stderr, flush=True)


def run(cmd, check=True, capture=False, user=None, env=None, input=None):
    """Run a command (list). Honors dry-run. Returns stdout when capture=True."""
    if user and user != "root":
        pre = ["sudo", "-u", user, "-H", "--"]
        if env:
            pre += ["env"] + [f"{k}={v}" for k, v in env.items()]
        cmd, env = pre + cmd, None
    if DRY:
        print("[dry-run] " + " ".join(shlex.quote(c) for c in cmd), flush=True)
        return "" if capture else 0
    e = dict(os.environ)
    if env:
        e.update(env)
    if capture:
        r = subprocess.run(cmd, check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=e,
                           input=input)
        return r.stdout
    r = subprocess.run(cmd, check=False, env=e, text=True, input=input)
    if check and r.returncode != 0:
        raise AuxoError(f"command failed ({r.returncode}): {' '.join(cmd)}")
    return r.returncode


def have(binary):
    return shutil.which(binary) is not None


def require_root():
    if os.geteuid() != 0 and not DRY:
        os.execvp("sudo", ["sudo", sys.executable] + sys.argv)


def online(host="pkg.freebsd.org"):
    if DRY:
        return True
    import socket
    try:
        socket.create_connection((host, 443), timeout=4).close()
        return True
    except OSError:
        return False


# ── packages (pkg) ────────────────────────────────────────────────────
TEST_PKGDB = "/var/db/pkg/.auxo-test"
LOCAL_REPO = "AuxoLocal"                          # /usr/local/etc/pkg/repos/AuxoLocal.conf
LOCAL_REPO_DIR = "/usr/local/share/auxo/pkgs"


def installed(pkg):
    if DRY:  # the test-suite marks packages as installed with empty files
        return os.path.exists(R(f"{TEST_PKGDB}/{pkg}"))
    return subprocess.run(["pkg", "info", "-e", pkg], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def pkg_install(*pkgs):
    """Install packages that aren't installed yet. Raises AuxoError on failure."""
    todo = [p for p in dict.fromkeys(pkgs) if p and not installed(p)]
    if not todo:
        return 0
    try:
        rc = run(["pkg", "install", "-y"] + todo)
    except AuxoError:
        # offline (or the FreeBSD mirror is down): the ISO ships a local repository with
        # the graphics drivers and a few extras, so driver changes still work
        if not os.path.exists(R(f"{LOCAL_REPO_DIR}/meta.conf")) and not DRY:
            raise
        warn("FreeBSD package servers unreachable; trying the AuxoBSD local repository")
        rc = run(["pkg", "install", "-y", "-r", LOCAL_REPO] + todo)
    if DRY:
        os.makedirs(R(TEST_PKGDB), exist_ok=True)
        for p in todo:
            open(R(f"{TEST_PKGDB}/{p}"), "w").close()
    return rc


def pkg_remove(*pkgs):
    """Remove installed packages. Never raises: returns the exit code."""
    todo = [p for p in dict.fromkeys(pkgs) if installed(p)]
    if not todo:
        return 0
    rc = run(["pkg", "delete", "-y"] + todo, check=False)
    if DRY:
        for p in todo:
            os.remove(R(f"{TEST_PKGDB}/{p}"))
    return rc


# ── shell-style KEY="value" files (auxo.conf, rc.conf, loader.conf) ───
def read_conf(path=CONF):
    data = {}
    try:
        with open(R(path)) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    data[k.strip()] = v.strip().strip('"')
    except FileNotFoundError:
        pass
    return data


def write_conf(updates, path=CONF):
    data = read_conf(path)
    data.update(updates)
    os.makedirs(os.path.dirname(R(path)), exist_ok=True)
    with open(R(path), "w") as f:
        f.write("# AuxoBSD system settings — managed by auxo-tweak\n")
        for k in sorted(data):
            f.write(f'{k}="{data[k]}"\n')


def set_kv(path, key, value, quote=True):
    """Set KEY=value in a shell-style file, adding it if missing (what sysrc does)."""
    p = R(path)
    lines = open(p).read().splitlines() if os.path.exists(p) else []
    val = f'"{value}"' if quote else value
    out, done = [], False
    for line in lines:
        s = line.strip()
        if s.startswith(key + "="):
            if not done:
                out.append(f"{key}={val}")
                done = True
            continue
        out.append(line)
    if not done:
        out.append(f"{key}={val}")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write("\n".join(out) + "\n")


def del_kv(path, key):
    p = R(path)
    if not os.path.exists(p):
        return False
    lines = open(p).read().splitlines()
    out = [ln for ln in lines if not ln.strip().startswith(key + "=")]
    if len(out) == len(lines):
        return False
    with open(p, "w") as f:
        f.write("\n".join(out) + "\n")
    return True


def get_kv(path, key, default=""):
    try:
        for line in open(R(path)):
            line = line.strip()
            if line.startswith(key + "="):
                return line.split("=", 1)[1].strip().strip('"')
    except FileNotFoundError:
        pass
    return default


def rc_set(key, value):
    set_kv(RC_CONF, key, value)


def rc_get(key, default=""):
    return get_kv(RC_CONF, key, default)


def rc_list_add(key, *items):
    """Add words to a list variable such as kld_list="i915kms"."""
    cur = rc_get(key).split()
    for i in items:
        if i not in cur:
            cur.append(i)
    rc_set(key, " ".join(cur))


def rc_list_remove(key, *items):
    cur = [w for w in rc_get(key).split() if w not in items]
    if cur:
        rc_set(key, " ".join(cur))
    else:
        del_kv(RC_CONF, key)


# ── services (rc.d) ───────────────────────────────────────────────────
def _live_system():
    return not DRY and ROOT == "/" and os.environ.get("AUXO_IN_INSTALLER") != "1"


def service_enable(name, now=False):
    rc_set(f"{name}_enable", "YES")
    if DRY:
        print(f"[dry-run] enable service {name}", flush=True)
    if now and _live_system():
        run(["service", name, "restart"], check=False)
    return 0


def service_disable(name, now=False):
    if now and _live_system():
        run(["service", name, "onestop"], check=False)
    del_kv(RC_CONF, f"{name}_enable")
    if DRY:
        print(f"[dry-run] disable service {name}", flush=True)
    return 0


def service_enabled(name):
    return rc_get(f"{name}_enable").upper() in ("YES", "TRUE", "ON", "1")


# ── files and users ───────────────────────────────────────────────────
def read_file(path):
    try:
        with open(R(path)) as f:
            return f.read()
    except (FileNotFoundError, IsADirectoryError):
        return None


def write_file(path, content, mode=0o644, owner=None):
    p = R(path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write(content)
    os.chmod(p, mode)
    if owner:
        chown(path, owner)


def symlink(target, path):
    p = R(path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if os.path.lexists(p):
        os.remove(p)
    os.symlink(target, p)


def chown(path, user, recursive=False):
    if DRY or os.geteuid() != 0:
        return
    try:
        pw = pwd.getpwnam(user)
    except KeyError:
        return
    p = R(path)
    targets = [p]
    if recursive:
        for base, dirs, files in os.walk(p):
            targets += [os.path.join(base, x) for x in dirs + files]
    for t in targets:
        try:
            os.lchown(t, pw.pw_uid, pw.pw_gid)
        except OSError:
            pass


def home_of(user):
    if ROOT != "/":
        return f"/home/{user}"
    try:
        return pwd.getpwnam(user).pw_dir
    except KeyError:
        return f"/home/{user}"


def normal_users():
    """Real login users (uid 1001-59999 on FreeBSD; 1000 too, for safety)."""
    users = []
    try:
        for line in open(R("/etc/passwd")):
            parts = line.split(":")
            if len(parts) > 6 and 1000 <= int(parts[2]) < 60000 and not parts[6].strip().endswith("nologin"):
                users.append(parts[0])
    except (FileNotFoundError, ValueError):
        pass
    return users


def invoking_user():
    return os.environ.get("SUDO_USER") or (pwd.getpwuid(os.getuid()).pw_name if os.getuid() != 0 else None)


# ── system facts ──────────────────────────────────────────────────────
def sysctl(name, default=""):
    if DRY:
        return os.environ.get("AUXO_TEST_SYSCTL_" + name.replace(".", "_"), default)
    out = run(["sysctl", "-n", name], capture=True, check=False)
    return (out or "").strip() or default


def kenv(name, default=""):
    if DRY:
        return os.environ.get("AUXO_TEST_KENV_" + name.replace(".", "_"), default)
    out = run(["kenv", "-q", name], capture=True, check=False)
    return (out or "").strip() or default


def root_fstype():
    """'zfs', 'ufs', 'cd9660' (live ISO), ..."""
    if DRY:
        return os.environ.get("AUXO_TEST_ROOTFS", "zfs")
    for line in (run(["mount", "-p"], capture=True, check=False) or "").splitlines():
        f = line.split()
        if len(f) >= 3 and f[1] == "/":
            return f[2]
    return ""


def is_live():
    return root_fstype() == "cd9660"


def parse_pciconf(txt):
    """[(vendor, device, class, description)] from `pciconf -lv` output. Handles both the
    current `vendor=0xVVVV device=0xDDDD` form and the older `chip=0xDDDDVVVV`."""
    devs = []
    for line in txt.splitlines():
        if "@pci" in line:
            cls = re.search(r"class=0x([0-9a-f]{6})", line)
            v = re.search(r"\bvendor=0x([0-9a-f]{4})", line)
            d = re.search(r"\bdevice=0x([0-9a-f]{4})", line)
            chip = re.search(r"chip=0x([0-9a-f]{4})([0-9a-f]{4})", line)
            if v and d:
                vid, did = v.group(1), d.group(1)
            elif chip:
                vid, did = chip.group(2), chip.group(1)
            else:
                continue
            devs.append([vid, did, cls.group(1) if cls else "", ""])
        elif devs and re.match(r"\s+device\s*=", line):
            devs[-1][3] = line.split("=", 1)[1].strip().strip("'")
    return [tuple(x) for x in devs]


def pci_devices():
    if DRY:
        out = []
        for spec in filter(None, os.environ.get("AUXO_TEST_PCI", "").split(";")):
            v, d, cls, desc = (spec.split(":", 3) + ["", "", ""])[:4]
            out.append((v, d, cls, desc))
        return out
    return parse_pciconf(run(["pciconf", "-lv"], capture=True, check=False) or "")


def gpus():
    """PCI display devices (class 03xxxx)."""
    return [d for d in pci_devices() if d[2].startswith("03")]
