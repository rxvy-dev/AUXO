"""Run install steps from plan.build(), reporting progress.

Callbacks:  on_step(index, title)   on_progress(fraction 0..1)   on_log(line)
With dry_run=True nothing is executed (used by the tests and the --dry-run flag)."""
import os
import shlex
import subprocess

from .plan import TARGET, SRC, COPY_EXCLUDES


class InstallError(Exception):
    pass


class Runner:
    def __init__(self, steps, on_step=None, on_progress=None, on_log=None, dry_run=False,
                 filecount_path="/usr/local/share/auxo/files.count"):
        self.steps = steps
        self.on_step = on_step or (lambda i, t: None)
        self.on_progress = on_progress or (lambda f: None)
        self.on_log = on_log or (lambda line: None)
        self.dry = dry_run
        self.total_weight = sum(s.weight for s in steps) or 1
        self.done_weight = 0
        self.filecount_path = filecount_path
        self.executed = []            # (kind, argv) — what ran, for the tests

    # ── helpers ───────────────────────────────────────────────────────
    def _progress(self, within=0.0, weight=0):
        self.on_progress(min(1.0, (self.done_weight + within * weight) / self.total_weight))

    def _exec(self, argv, check=True, input=None):
        self.on_log("$ " + shlex.join(argv))
        self.executed.append(argv)
        if self.dry:
            return 0
        r = subprocess.run(argv, input=input, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for line in (r.stdout or "").splitlines()[-40:]:
            self.on_log("  " + line)
        if check and r.returncode != 0:
            raise InstallError(f"'{shlex.join(argv)}' failed (exit {r.returncode})")
        return r.returncode

    def _write(self, path, text, mode):
        full = TARGET + path
        self.on_log(f"write {full}")
        self.executed.append(["write", full])
        if self.dry:
            return
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)
        os.chmod(full, mode)

    def _copy(self, weight):
        """tar the read-only image into the target, counting files for the progress bar."""
        try:
            total = int(open(self.filecount_path).read().strip())
        except (OSError, ValueError):
            total = 250000
        excl = sum((["--exclude", e] for e in COPY_EXCLUDES), [])
        src = ["tar", "-C", SRC, "-cf", "-"] + excl + ["."]
        dst = ["tar", "-C", TARGET, "-xpvf", "-"]
        self.on_log("$ " + shlex.join(src) + " | " + shlex.join(dst))
        self.executed.append(src + ["|"] + dst)
        if self.dry:
            self._progress(1.0, weight)
            return
        p1 = subprocess.Popen(src, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        p2 = subprocess.Popen(dst, stdin=p1.stdout, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        p1.stdout.close()
        n = 0
        for line in p2.stderr:        # bsdtar -v lists each file on stderr ("x ./path")
            n += 1
            if n % 500 == 0:
                self._progress(min(n / total, 0.99), weight)
                self.on_log(f"  {n:,} files")
            if not line.startswith("x "):
                self.on_log("  " + line.rstrip())
        r2, r1 = p2.wait(), p1.wait()
        if r1 != 0 or r2 != 0:
            raise InstallError(f"copying the system failed (tar exit {r1}/{r2})")
        self.on_log(f"  copied {n:,} files")

    # ── main loop ─────────────────────────────────────────────────────
    def run(self):
        for i, step in enumerate(self.steps):
            self.on_step(i, step.title)
            self.on_log(f"── {step.title}")
            for op in step.ops:
                kind = op[0]
                if kind == "run":
                    self._exec(op[1])
                elif kind == "try":
                    self._exec(op[1], check=False)
                elif kind in ("chroot", "chroot-try"):
                    self._exec(["chroot", TARGET] + op[1], check=kind == "chroot",
                               input=op[2] if len(op) > 2 else None)
                elif kind == "write":
                    self._write(op[1], op[2], op[3])
                elif kind == "copy":
                    self._copy(step.weight)
                else:
                    raise InstallError(f"unknown step {kind}")
            self.done_weight += step.weight
            self._progress()
        self.on_progress(1.0)
        self.on_log("── done")
