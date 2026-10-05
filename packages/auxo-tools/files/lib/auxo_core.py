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


def run(cmd, check=True, capture=False, user=None, env=None, input=None):
    """Run a command (list). Honors dry-run. Returns stdout when capture=True.
    input: text sent to the command's stdin."""
    if user and user != "root":
        cmd = ["runuser", "-u", user, "--"] + cmd
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


# ── packages (xbps) ───────────────────────────────────────────────────
_synced = False


def _sync():
    """Refresh the repo index once per run. xbps refuses to install anything while
    xbps itself is out of date, so update it first when needed."""
    global _synced
    if _synced:
        return
    _synced = True
    run(["xbps-install", "-S"], check=False)
    run(["xbps-install", "-uy", "xbps"], check=False)


def xbps_install(*pkgs):
    """Install packages that aren't installed yet. Raises AuxoError on failure."""
    todo = [p for p in dict.fromkeys(pkgs) if p and not installed(p)]
    if not todo:
        return 0
    _sync()
    rc = run(["xbps-install", "-y"] + todo)
    if DRY:  # remember what a dry run "installed", like a real system would
        os.makedirs(R("/var/db/xbps/.auxo-test"), exist_ok=True)
        for p in todo:
            open(R(f"/var/db/xbps/.auxo-test/{p}"), "w").close()
    return rc


def xbps_remove(*pkgs, recursive=True):
    """Remove installed packages (and, by default, dependencies nothing else needs).
    Never raises: returns the exit code."""
    todo = [p for p in dict.fromkeys(pkgs) if installed(p) or DRY]
    if not todo:
        return 0
    rc = run(["xbps-remove", "-y"] + (["-R"] if recursive else []) + todo, check=False)
    if DRY:
        for p in todo:
            if os.path.exists(R(f"/var/db/xbps/.auxo-test/{p}")):
                os.remove(R(f"/var/db/xbps/.auxo-test/{p}"))
    return rc


def installed(pkg):
    if DRY:  # the test-suite marks packages as installed with empty files
        return os.path.exists(R(f"/var/db/xbps/.auxo-test/{pkg}"))
    return subprocess.run(["xbps-query", pkg], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def repo_enable(*repos):
    """Enable Void's extra repos: nonfree, multilib, multilib-nonfree."""
    pkgs = [f"void-repo-{r}" for r in repos]
    if all(installed(p) for p in pkgs):
        return
    xbps_install(*pkgs)
    global _synced
    _synced = False  # new repo: index needs a refresh


def regen_initramfs():
    """Rebuild every installed kernel's initramfs (Void uses dracut)."""
    return run(["dracut", "--regenerate-all", "--force"], check=False)


def online(host="repo-default.voidlinux.org"):
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


def del_kv(path, key):
    """Remove an active KEY=... line from a shell-style file (commented lines are kept)."""
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


# ── services (runit) ──────────────────────────────────────────────────
# Services live in /etc/sv/<name>; a service is enabled by linking it into the
# default runlevel. This works the same on a running system and in a chroot.
SVDIR = "/etc/runit/runsvdir/default"


def _booted_runit():
    # inside the installer's chroot there's no running runit to talk to
    if os.environ.get("AUXO_IN_INSTALLER") == "1":
        return False
    return not DRY and ROOT == "/" and os.path.isdir("/run/runit")


def sv_enabled(name):
    return os.path.lexists(R(f"{SVDIR}/{name}"))


def sv_enable(name, now=False):
    if not (os.path.isdir(R(f"/etc/sv/{name}")) or DRY):
        warn(f"service {name} is not installed")
        return 1
    link = R(f"{SVDIR}/{name}")
    os.makedirs(os.path.dirname(link), exist_ok=True)
    if not os.path.lexists(link):
        os.symlink(f"/etc/sv/{name}", link)
    if DRY:
        print(f"[dry-run] enable service {name}", flush=True)
    # runsvdir only rescans every 5 s: wait for it to supervise the new service first
    if now and _booted_runit():
        import time
        for _ in range(16):
            if os.path.exists(f"/var/service/{name}/supervise/ok"):
                break
            time.sleep(0.5)
        run(["sv", "-w", "10", "up", name], check=False)
    return 0


def sv_disable(name, now=False):
    link = R(f"{SVDIR}/{name}")
    if now and _booted_runit() and os.path.lexists(link):
        run(["sv", "down", name], check=False)
    if os.path.lexists(link):
        os.remove(link)
    if DRY:
        print(f"[dry-run] disable service {name}", flush=True)
    return 0
