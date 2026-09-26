"""Shared helpers for auxo-tools.

Environment knobs (used by the test-suite and the installer):
  AUXO_ROOT=/path   prefix every file path we read/write (default: /)
  AUXO_DRY_RUN=1    print commands instead of running them
"""
import os
import pwd
import shlex
import shutil
import subprocess
import sys

ROOT = os.environ.get("AUXO_ROOT", "/")
DRY = os.environ.get("AUXO_DRY_RUN") == "1"
CONF = "/etc/auxo/auxo.conf"
SHARE = "/usr/share/auxo"

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


def run(cmd, check=True, capture=False, user=None, env=None):
    """Run a command (list). Honors dry-run. Returns stdout when capture=True."""
    if user and user != "root":
        cmd = ["runuser", "-u", user, "--"] + cmd
    if DRY:
        print("[dry-run] " + " ".join(shlex.quote(c) for c in cmd), flush=True)
        return "" if capture else 0
    e = dict(os.environ)
    if env:
        e.update(env)
    if capture:
        r = subprocess.run(cmd, check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=e)
        return r.stdout
    r = subprocess.run(cmd, check=False, env=e)
    if check and r.returncode != 0:
        raise AuxoError(f"command failed ({r.returncode}): {' '.join(cmd)}")
    return r.returncode


def have(binary):
    return shutil.which(binary) is not None


def pacman(*args, needed=True):
    """pacman wrapper. For plain installs (-S) packages that are already present are
    skipped up-front, so this also works offline (e.g. inside the installer, where the
    sync databases are absent)."""
    args = list(args)
    cmd = ["pacman", "--noconfirm"]
    if args and args[0] == "-S":
        pkgs = [p for p in dict.fromkeys(args[1:]) if p.startswith("-") or not installed(p)]
        if not [p for p in pkgs if not p.startswith("-")]:
            return 0
        args = ["-S"] + pkgs
    if needed and args and args[0].startswith("-S"):
        cmd.append("--needed")
    return run(cmd + args)


def installed(pkg):
    if DRY:
        return os.path.isdir(R("/var/lib/pacman/local")) and any(
            d.rsplit("-", 2)[0] == pkg for d in os.listdir(R("/var/lib/pacman/local")))
    return subprocess.run(["pacman", "-Qq", pkg], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def online(host="archlinux.org"):
    if DRY:
        return True
    import socket
    try:
        socket.create_connection((host, 443), timeout=4).close()
        return True
    except OSError:
        return False


def require_root():
    if os.geteuid() != 0 and not DRY:
        os.execvp("sudo", ["sudo", sys.executable] + sys.argv)


# ── tiny KEY=value config ─────────────────────────────────────────────
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
        f.write("# Auxo Linux system settings — managed by auxo-tweak\n")
        for k in sorted(data):
            f.write(f'{k}="{data[k]}"\n')


def set_kv(path, key, value, quote=True):
    """Set KEY=value in a shell-style file (e.g. /etc/default/grub), adding it if missing."""
    p = R(path)
    lines = open(p).read().splitlines() if os.path.exists(p) else []
    val = f'"{value}"' if quote else value
    out, done = [], False
    for line in lines:
        s = line.lstrip("#").strip()
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


def get_kv(path, key, default=""):
    try:
        for line in open(R(path)):
            line = line.strip()
            if line.startswith(key + "="):
                return line.split("=", 1)[1].strip().strip('"')
    except FileNotFoundError:
        pass
    return default


def write_file(path, content, mode=0o644, owner=None):
    p = R(path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write(content)
    os.chmod(p, mode)
    if owner:
        chown(path, owner)


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
    """Real login users (uid 1000-59999) — used when --user is not given."""
    users = []
    try:
        for line in open(R("/etc/passwd")):
            parts = line.split(":")
            if len(parts) > 6 and 1000 <= int(parts[2]) < 60000 and not parts[6].strip().endswith("nologin"):
                users.append(parts[0])
    except FileNotFoundError:
        pass
    return users


def invoking_user():
    return os.environ.get("SUDO_USER") or os.environ.get("PKEXEC_UID_USER") or (
        pwd.getpwuid(os.getuid()).pw_name if os.getuid() != 0 else None)


def systemctl_enable(unit, now=False):
    cmd = ["systemctl", "enable"] + (["--now"] if now else []) + [unit]
    return run(cmd, check=False)


def systemctl_disable(unit, now=False):
    cmd = ["systemctl", "disable"] + (["--now"] if now else []) + [unit]
    return run(cmd, check=False)
