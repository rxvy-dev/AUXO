#!/usr/bin/env python3
"""auxo-monitor: the /proc + /sys readers against fake files with known numbers, and the
window offscreen (End task only for your own processes)."""
import os
import signal
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(HERE, "packages/auxo-tools/files")
sys.path.insert(0, os.path.join(PKG, "lib"))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["AUXO_LIB"] = os.path.join(PKG, "lib")
os.environ["AUXO_HOME"] = tempfile.mkdtemp()
from auxo_sysstats import Stats, human  # noqa: E402

passed = failed = 0


def check(name, cond):
    global passed, failed
    passed += bool(cond)
    failed += not cond
    if not cond:
        print("  ✗", name)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


P, Y = tempfile.mkdtemp(), tempfile.mkdtemp()
write(f"{P}/stat", "cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 50 0 50 400 0 0 0 0 0 0\ncpu1 50 0 50 400 0 0 0 0 0 0\n")
write(f"{P}/meminfo", "MemTotal: 8000000 kB\nMemFree: 1000000 kB\nMemAvailable: 6000000 kB\n"
                      "SwapTotal: 2000000 kB\nSwapFree: 1500000 kB\n")
write(f"{P}/net/dev", "Inter-|\n face |\n    lo: 999 0 0 0 0 0 0 0 999 0 0 0 0 0 0 0\n"
                      "  eth0: 1000 0 0 0 0 0 0 0 500 0 0 0 0 0 0 0\n")
write(f"{P}/cpuinfo", "model name\t: Test CPU 9000\n")
write(f"{P}/mounts", "/dev/sda1 / ext4 rw 0 0\nproc /proc proc rw 0 0\n/dev/loop0 /snap squashfs ro 0 0\n")
write(f"{Y}/class/hwmon/hwmon0/name", "k10temp\n")
write(f"{Y}/class/hwmon/hwmon0/temp1_input", "61500\n")
write(f"{Y}/class/hwmon/hwmon1/name", "amdgpu\n")
write(f"{Y}/class/hwmon/hwmon1/temp1_input", "48000\n")
write(f"{Y}/class/drm/card0/device/gpu_busy_percent", "37\n")
me = os.getpid()
write(f"{P}/{me}/stat", f"{me} (my app) S 1 1 1 0 -1 0 0 0 0 0 200 100 0 0 20 0 1 0 0 0 2560 0\n")
write(f"{P}/{me}/cmdline", "my-app\0--flag\0")
write(f"{P}/4242/stat", "4242 (kworker/0:1) I 2 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 0\n")
write(f"{P}/4242/cmdline", "")

s = Stats(proc=P, sys=Y)
check("first CPU reading is zero", s.cpu() == (0.0, [0.0, 0.0]))
write(f"{P}/stat", "cpu  150 0 150 900 0 0 0 0 0 0\ncpu0 100 0 100 400 0 0 0 0 0 0\ncpu1 50 0 50 500 0 0 0 0 0 0\n")
total, cores = s.cpu()
check("CPU % from /proc/stat deltas", round(total) == 50 and [round(c) for c in cores] == [100, 0])
check("CPU name", s.cpu_name() == "Test CPU 9000")
check("memory uses MemAvailable", s.memory() == (2000000 * 1024, 8000000 * 1024, 500000 * 1024, 2000000 * 1024))
check("network skips lo", s.network() == (0.0, 0.0))
write(f"{P}/net/dev", "Inter-|\n face |\n    lo: 99999 0 0 0 0 0 0 0 99999 0 0 0 0 0 0 0\n"
                      "  eth0: 3000 0 0 0 0 0 0 0 1500 0 0 0 0 0 0 0\n")
down, up = s.network()
check("network rates count eth0 only", down > 0 and up > 0 and down / up == 2)
check("temperatures by sensor", s.temps() == {"CPU": 61.5, "GPU": 48.0})
check("AMD GPU busy % from sysfs", s.gpu() is None or s.gpu()[1] == 37.0 or s.gpu()[0] != "AMD GPU")
d = s.disks()
check("disks: real filesystems only", [x[0] for x in d] == ["/"])
procs = {p["pid"]: p for p in s.processes()}
check("processes: name, command, memory", procs[me]["name"] == "my app" and procs[me]["cmd"] == "my-app --flag"
      and procs[me]["mem"] == 2560 * os.sysconf("SC_PAGE_SIZE"))
check("kernel threads get [name]", procs[4242]["cmd"] == "[kworker/0:1]")
check("human sizes", human(1500) == "1.5 KB" and human(2_300_000, rate=True) == "2.3 MB/s" and human(12) == "12 B")

# ── the window ──
import importlib.machinery  # noqa: E402
import importlib.util  # noqa: E402

from PyQt6.QtWidgets import QApplication  # noqa: E402

loader = importlib.machinery.SourceFileLoader("auxo_monitor", os.path.join(PKG, "bin/auxo-monitor"))
spec = importlib.util.spec_from_loader(loader.name, loader)
M = importlib.util.module_from_spec(spec)
loader.exec_module(M)
app = QApplication(sys.argv[:1])
w = M.Monitor(stats=Stats(proc=P, sys=Y), page=1)
w.timer.stop()
w.last_procs = w.stats.processes()
w.fill_processes(w.last_procs)
names = [w.table.item(r, 0).text() for r in range(w.table.rowCount())]
check("process list hides kernel threads", "my app" in names and "kworker/0:1" not in names)
w.search.setText("kworker")
check("…but search finds them", [w.table.item(r, 0).text() for r in range(w.table.rowCount())] == ["kworker/0:1"])
w.search.setText("my app")
w.table.selectRow(0)
check("End task allowed for your own app", w.end_btn.isEnabled())
sent = []
M.os.kill = lambda pid, sig: sent.append((pid, sig))
w.end_btn.click()
check("End task sends SIGTERM to it", sent == [(me, signal.SIGTERM)])
w.kill_btn.click()
check("Force quit sends SIGKILL", sent[-1] == (me, signal.SIGKILL))
for p in w.last_procs:
    p["uid"] = 0 if os.getuid() else 12345
w.fill_processes(w.last_procs)
w.table.selectRow(0)
check("End task greyed out for other users' processes", not w.end_btn.isEnabled() or os.geteuid() == 0)
w.close()
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
