"""System numbers for auxo-monitor, read straight from /proc and /sys (no psutil needed).
Every reader takes the root it reads from, so tests can hand it a fake one."""
import os
import pwd
import shutil
import subprocess
import time


def _read(path, default=""):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


class Stats:
    def __init__(self, proc="/proc", sys="/sys"):
        self.proc, self.sys = proc, sys
        self._cpu_prev = None
        self._net_prev = None
        self._proc_prev = {}
        self._proc_time = None
        self.ncpu = os.cpu_count() or 1
        self.hz = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100
        self._users = {}

    # ── CPU ──
    def cpu(self):
        """(total %, [per-core %]) since the last call (the first call returns zeros)."""
        rows = [line.split() for line in _read(f"{self.proc}/stat").splitlines() if line.startswith("cpu")]
        now = [(sum(map(int, r[1:9])), int(r[4]) + int(r[5])) for r in rows]      # (total, idle+iowait)
        prev, self._cpu_prev = self._cpu_prev, now
        if not prev or len(prev) != len(now):
            return 0.0, [0.0] * max(0, len(now) - 1)
        pct = []
        for (t1, i1), (t0, i0) in zip(now, prev):
            dt = t1 - t0
            pct.append(0.0 if dt <= 0 else max(0.0, min(100.0, 100.0 * (1 - (i1 - i0) / dt))))
        return pct[0], pct[1:]

    def cpu_name(self):
        for line in _read(f"{self.proc}/cpuinfo").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
        return "CPU"

    # ── memory ──
    def memory(self):
        """(used bytes, total bytes, swap used, swap total)."""
        m = {}
        for line in _read(f"{self.proc}/meminfo").splitlines():
            k, _, v = line.partition(":")
            m[k] = int(v.split()[0]) * 1024 if v.split() else 0
        total = m.get("MemTotal", 0)
        used = total - m.get("MemAvailable", m.get("MemFree", 0))
        st = m.get("SwapTotal", 0)
        return used, total, st - m.get("SwapFree", 0), st

    # ── disks ──
    def disks(self):
        """[(mountpoint, used, total)] for real filesystems."""
        out, seen = [], set()
        for line in _read(f"{self.proc}/mounts").splitlines():
            parts = line.split()
            if len(parts) < 3 or not parts[0].startswith("/dev/") or parts[2] in ("squashfs", "iso9660"):
                continue
            if parts[0] in seen:
                continue
            seen.add(parts[0])
            mnt = parts[1].replace("\\040", " ")
            try:
                u = shutil.disk_usage(mnt)
            except OSError:
                continue
            out.append((mnt, u.used, u.total))
        return out

    # ── network ──
    def network(self):
        """(download bytes/s, upload bytes/s) since the last call, all interfaces but lo."""
        rx = tx = 0
        for line in _read(f"{self.proc}/net/dev").splitlines()[2:]:
            name, _, data = line.partition(":")
            if name.strip() == "lo" or not data.split():
                continue
            f = data.split()
            rx += int(f[0])
            tx += int(f[8])
        now = (time.monotonic(), rx, tx)
        prev, self._net_prev = self._net_prev, now
        if not prev:
            return 0.0, 0.0
        dt = max(1e-3, now[0] - prev[0])
        return max(0.0, (rx - prev[1]) / dt), max(0.0, (tx - prev[2]) / dt)

    # ── temperatures ──
    def temps(self):
        """{'CPU': °C, 'GPU': °C, ...} from hwmon (k10temp/coretemp/amdgpu/nvme...)."""
        names = {"k10temp": "CPU", "coretemp": "CPU", "zenpower": "CPU", "cpu_thermal": "CPU", "amdgpu": "GPU",
                 "nouveau": "GPU", "nvme": "Disk", "acpitz": "Board"}
        out = {}
        base = f"{self.sys}/class/hwmon"
        for h in sorted(os.listdir(base)) if os.path.isdir(base) else []:
            name = _read(f"{base}/{h}/name").strip()
            label = names.get(name)
            if not label or label in out:
                continue
            try:
                out[label] = int(_read(f"{base}/{h}/temp1_input", "0").strip() or 0) / 1000
            except ValueError:
                pass
        return {k: v for k, v in out.items() if v > 0}

    # ── GPU ──
    def gpu(self):
        """(name, busy %, °C or None) or None. NVIDIA through nvidia-smi, AMD through sysfs."""
        if shutil.which("nvidia-smi"):
            try:
                r = subprocess.run(["nvidia-smi", "--query-gpu=name,utilization.gpu,temperature.gpu",
                                    "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=2)
                name, util, temp = [x.strip() for x in r.stdout.splitlines()[0].split(",")]
                return name, float(util), float(temp)
            except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
                pass
        drm = f"{self.sys}/class/drm"
        for card in sorted(os.listdir(drm)) if os.path.isdir(drm) else []:
            busy = _read(f"{drm}/{card}/device/gpu_busy_percent").strip()
            if busy.isdigit():
                return "AMD GPU", float(busy), self.temps().get("GPU")
        return None

    # ── processes ──
    def _user(self, uid):
        if uid not in self._users:
            try:
                self._users[uid] = pwd.getpwuid(uid).pw_name
            except KeyError:
                self._users[uid] = str(uid)
        return self._users[uid]

    def processes(self):
        """[{pid, name, user, cpu %, mem bytes, cmd}] with CPU % since the last call."""
        now = time.monotonic()
        dt = (now - self._proc_time) if self._proc_time else None
        self._proc_time = now
        page = os.sysconf("SC_PAGE_SIZE") if hasattr(os, "sysconf") else 4096
        out, ticks = [], {}
        for pid in (d for d in os.listdir(self.proc) if d.isdigit()):
            stat = _read(f"{self.proc}/{pid}/stat")
            if not stat:
                continue
            try:
                name = stat[stat.index("(") + 1:stat.rindex(")")]
                f = stat[stat.rindex(")") + 2:].split()
                t = int(f[11]) + int(f[12])                 # utime + stime
                rss = int(f[21]) * page
                uid = os.stat(f"{self.proc}/{pid}").st_uid
            except (ValueError, IndexError, OSError):
                continue
            ticks[pid] = t
            cpu = 0.0
            if dt and pid in self._proc_prev:
                cpu = max(0.0, 100.0 * (t - self._proc_prev[pid]) / self.hz / dt / self.ncpu)
            cmd = _read(f"{self.proc}/{pid}/cmdline").replace("\0", " ").strip()
            out.append({"pid": int(pid), "name": name, "user": self._user(uid), "uid": uid, "cpu": cpu, "mem": rss,
                        "cmd": cmd or f"[{name}]"})
        self._proc_prev = ticks
        return out


def human(n, rate=False):
    units = ["B", "KB", "MB", "GB", "TB"]
    i = 0
    n = float(n)
    while n >= 1000 and i < len(units) - 1:
        n /= 1000
        i += 1
    s = f"{n:.0f}" if i == 0 or n >= 100 else f"{n:.1f}"
    return f"{s} {units[i]}" + ("/s" if rate else "")
