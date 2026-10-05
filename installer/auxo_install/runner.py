"""Execute a plan: run commands, write files, report progress, keep a log.

dry_run=True prints commands instead of running them but still writes files, so
the tests can point `target` at a temp directory and inspect what would be written.
"""
import os
import subprocess
import time

from .plan import Cmd, Write, Py, rsync_percent

LOG = "/tmp/auxo-install.log"


class InstallError(Exception):
    pass


class Context:
    def __init__(self, target, dry_run=False, log_path=LOG, on_line=None):
        self.target = target
        self.dry_run = dry_run
        self.log_path = log_path
        self.on_line = on_line or (lambda s: None)
        self.notes = []
        self._log = open(log_path, "a", buffering=1)

    def log(self, msg):
        self._log.write(time.strftime("%H:%M:%S ") + msg + "\n")
        self.on_line(msg)

    def read(self, path):
        try:
            with open(path, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def uuid(self, dev):
        if self.dry_run:
            return f"UUID-OF-{os.path.basename(dev)}"
        out = subprocess.run(["blkid", "-s", "UUID", "-o", "value", dev], capture_output=True, text=True).stdout.strip()
        if not out:
            raise InstallError(f"couldn't read the UUID of {dev}")
        return out

    def parent_disk(self, part):
        if self.dry_run:
            return part.rstrip("0123456789").rstrip("p") if "nvme" in part or "mmcblk" in part else part.rstrip("0123456789")
        out = subprocess.run(["lsblk", "-no", "PKNAME", part], capture_output=True, text=True).stdout.strip()
        return f"/dev/{out}" if out else part.rstrip("0123456789")


def run_action(ctx, a, on_fraction=None):
    if isinstance(a, Py):
        ctx.log(f"• {a.desc}")
        for sub in a.fn(ctx) or []:
            run_action(ctx, sub, on_fraction)
        return
    if isinstance(a, Write):
        ctx.log(f"$ {a.show()}")
        os.makedirs(os.path.dirname(a.path), exist_ok=True)
        with open(a.path, "a" if a.append else "w") as f:
            f.write(a.content)
        os.chmod(a.path, a.mode)
        return
    if not isinstance(a, Cmd):
        raise TypeError(a)
    ctx.log(f"$ {a.show()}")
    if ctx.dry_run:
        return
    try:
        p = subprocess.Popen(a.argv, stdin=subprocess.PIPE if a.input else subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    except FileNotFoundError:
        rc, tail = 127, f"{a.argv[0]}: command not found"
    else:
        if a.input:
            p.stdin.write(a.input)
            p.stdin.close()
        tail = ""
        buf = ""
        while True:
            ch = p.stdout.read(1)
            if not ch:
                break
            if ch in "\r\n":       # rsync updates its progress line with \r
                line, buf = buf.strip(), ""
                if not line:
                    continue
                tail = line
                if a.progress == "rsync":
                    pct = rsync_percent(line)
                    if pct is not None and on_fraction:
                        on_fraction(pct / 100)
                    continue       # don't flood the log with progress lines
                ctx.log("  " + line)
            else:
                buf += ch
        rc = p.wait()
    if rc != 0:
        msg = f"'{a.desc or ' '.join(a.argv[:3])}' failed (exit {rc}): {tail}"
        if a.soft:
            ctx.notes.append(msg)
            ctx.log("  ! " + msg)
        else:
            raise InstallError(msg)


def execute(steps, ctx, on_step=None, on_progress=None):
    """Run every step. on_step(i, step) when a step starts; on_progress(0..1) overall."""
    total = sum(s.weight for s in steps) or 1
    done = 0.0
    for i, s in enumerate(steps):
        if on_step:
            on_step(i, s)
        ctx.log(f"== {s.title}")
        n = max(1, len(s.actions))
        for j, a in enumerate(s.actions):
            def frac(f, j=j):
                if on_progress:
                    on_progress((done + s.weight * (j + f) / n) / total)
            run_action(ctx, a, frac)
            if on_progress:
                on_progress((done + s.weight * (j + 1) / n) / total)
        done += s.weight
    if on_progress:
        on_progress(1.0)
    return ctx.notes
