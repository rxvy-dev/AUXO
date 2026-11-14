#!/usr/bin/env python3
"""Boot AuxoBSD in QEMU (headless) and drive auxsh over the serial port:
auxo-fetch, auxo-tweak, auxo-motd and the built-in commands.

  python3 tests/test-shell.py build/auxobsd.iso
"""
import json
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "❯ ".encode()     # matched after colour codes are stripped


class Shell:
    """QEMU with the guest's COM1 on a Unix socket (QEMU waits for us to connect, so no output
    is lost). Talking over a socket instead of QEMU's stdio keeps the test independent of how a
    given QEMU build treats pipes; QEMU's own messages go to qemu-out.log."""

    def __init__(self, iso):
        qemu = os.environ.get("QEMU", "qemu-system-x86_64")
        out = os.path.dirname(os.path.abspath(iso))
        self.qlog = os.path.join(out, "qemu-debug.log")
        self.olog = os.path.join(out, "qemu-out.log")
        self.sockdir = tempfile.mkdtemp(prefix="auxobsd-serial-")
        sock = os.path.join(self.sockdir, "ttyS0")
        for f in (self.qlog, self.olog):
            if os.path.exists(f):
                os.remove(f)
        args = [qemu, "-cdrom", iso, "-display", "none", "-m", "1G", "-no-reboot",
                "-chardev", f"socket,id=ser0,path={sock},server=on,wait=on", "-serial", "chardev:ser0",
                "-monitor", "none", "-d", "cpu_reset,guest_errors", "-D", self.qlog]
        # Software CPU by default: that's what the suite is verified with. AUXO_KVM=1 opts into KVM.
        if os.environ.get("AUXO_KVM") == "1" and os.access("/dev/kvm", os.W_OK):
            args += ["-enable-kvm", "-cpu", "host"]
        self.args = args
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=open(self.olog, "wb"),
                                  stderr=subprocess.STDOUT)
        self.raw = b""
        self.s = None
        for _ in range(200):                      # QEMU creates the socket, then waits for us
            if self.p.poll() is not None:
                break
            try:
                c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                c.connect(sock)
                self.s = c
                break
            except OSError:
                time.sleep(0.05)

    def _read_until(self, start, token, timeout):
        if not self.s:
            return False
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.s], [], [], 0.1)
            if r:
                d = self.s.recv(65536)
                if not d:
                    return False
                self.raw += d
                if token in ANSI.sub(b"", self.raw[start:]):
                    return True
        return False

    def boot(self, timeout=60):
        ok = self._read_until(0, PROMPT, timeout)
        return ok, self.text(0)

    def send(self, keys, until=PROMPT, timeout=10):
        start = len(self.raw)
        if self.s:
            self.s.sendall(keys.encode())
        self._read_until(start, until, timeout)
        return self.raw[start:]

    def run(self, cmd, **kw):
        return clean(self.send(cmd + "\r", **kw))

    def text(self, start):
        return clean(self.raw[start:])

    def diagnose(self):
        """Why the boot failed: QEMU's state, its messages and the tail of its reset/error log."""
        rc = self.p.poll()
        out = [f"qemu {'exited with ' + str(rc) if rc is not None else 'still running'}"
               + ("" if self.s else ", serial socket never connected") + f": {' '.join(self.args)}"]
        for name, path, n in (("qemu-out.log", self.olog, 15), ("qemu-debug.log", self.qlog, 25)):
            try:
                tail = open(path, errors="replace").read().splitlines()[-n:]
                out += [f"{name}:"] + tail if tail else [f"{name} is empty"]
            except OSError:
                out.append(f"no {name}")
        return "\n".join(out)

    def close(self):
        if self.s:
            self.s.close()
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        shutil.rmtree(self.sockdir, ignore_errors=True)


def clean(b):
    return ANSI.sub(b"", b).replace(b"\r", b"").decode("utf-8", "replace")


passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  \033[32m✓\033[0m {name}")
    else:
        failed += 1
        print(f"  \033[31m✗\033[0m {name}")
        if detail:
            print("      " + detail.strip().replace("\n", "\n      ")[:1500])


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    if not shutil.which(os.environ.get("QEMU", "qemu-system-x86_64")):
        print("qemu-system-x86_64 not found")
        return 2
    sh = Shell(iso)
    try:
        print("▲ boot")
        ok, log = sh.boot()
        check("reaches the auxsh prompt", ok, (log[-800:] or "(no serial output at all)") + "\n" + sh.diagnose())
        if not ok:
            print(f"\n{passed} passed, {failed} failed (stopped: the kernel never reached the shell)")
            return 1
        check("Auxo banner", "AuxoBSD 0.1.0" in log and "part of Auxo" in log and "auxolinux.com" in log, log)
        check("boot log shows the accent", "accent violet, hostname auxobsd" in log, log)
        check("MOTD greets on start", "Welcome to AuxoBSD, auxo." in log and "auxo-fetch for a summary" in log, log)
        check("prompt in Auxo style", "▲ auxo@auxobsd ❯" in log, log[-200:])

        print("▲ auxo-fetch")
        out = sh.run("auxo-fetch")
        for label in ("os", "kernel", "uptime", "procs", "shell", "boot", "cpu", "memory", "heap", "accent"):
            check(f"auxo-fetch shows {label}", re.search(rf"\b{label}\s+\S", out), out)
        check("auxo-fetch user line + logo", "auxo@auxobsd" in out and "/__/ /  \\ \\__\\" in out, out)
        check("auxo-fetch accent is violet", "● violet" in out, out)
        check("auxo-fetch draws the palette strip", "█" * 24 in out, out)
        raw = sh.send("auxo-fetch\r")
        check("auxo-fetch colours the logo with the violet gradient", b"\x1b[1;38;2;167;139;250m" in raw)
        out = sh.run("auxo-fetch --small")
        check("auxo-fetch --small: short logo, no cpu line", "/_/\\_\\" in out and "cpu" not in out, out)
        out = sh.run("auxo-fetch --no-logo")
        check("auxo-fetch --no-logo", "/__/" not in out and "auxo@auxobsd" in out, out)
        out = sh.run("auxo-fetch --json")
        m = re.search(r"\{.*\}", out, re.S)
        data = None
        try:
            data = json.loads(m.group(0)) if m else None
        except ValueError:
            pass
        check("auxo-fetch --json is valid JSON", isinstance(data, dict), out)
        if data:
            check("json has the Auxo fields", data.get("accent") == "violet" and data.get("shell") == "auxsh"
                  and data.get("os", "").startswith("AuxoBSD") and len(data.get("memory_mib", [])) == 2, out)
        out = sh.run("auxo-fetch --accent amber")
        check("auxo-fetch --accent draws in another accent", "● amber" in out, out)
        out = sh.run("auxo-tweak info")
        check("…without changing the setting", re.search(r"accent\s+violet", out), out)
        out = sh.run("auxo-fetch --accent nope")
        check("auxo-fetch rejects an unknown accent", "unknown accent 'nope'" in out, out)
        check("fetch alias", "auxo@auxobsd" in sh.run("fetch"))

        print("▲ auxo-tweak")
        out = sh.run("auxo-tweak accent")
        check("accent list marks the current one", "● violet" in out and "○ rose" in out and "#a78bfa" in out, out)
        out = sh.run("auxo-tweak accent rose")
        check("accent rose", "accent: rose" in out, out)
        out = sh.run("auxo-tweak info")
        check("info shows rose", re.search(r"accent\s+rose", out), out)
        raw = sh.send("auxo-fetch\r")
        check("auxo-fetch follows the new accent", b"\x1b[1;38;2;251;113;133m" in raw and "● rose" in clean(raw))
        out = sh.run("auxo-tweak accent nope")
        check("unknown accent is refused", "unknown accent 'nope'" in out and "emerald" in out, out)
        out = sh.run("auxo-tweak hostname summit")
        check("hostname summit", "hostname: summit" in out and "auxo@summit ❯" in out, out)
        out = sh.run("auxo-tweak hostname bad_name!")
        check("bad hostname is refused", "isn't a valid hostname" in out, out)
        out = sh.run("auxo-tweak motd off")
        check("motd off", "motd: off" in out, out)
        out = sh.run("auxo-tweak motd maybe")
        check("motd needs on/off", "expected on or off" in out, out)
        out = sh.run("auxo-tweak persist")
        check("persist prints the GRUB line",
              "multiboot2 /boot/auxobsd.elf accent=rose hostname=summit motd=0" in out, out)
        out = sh.run("auxo-tweak desktop hyprland")
        check("Linux-only sub-commands explain themselves", "auxo-tweak desktop is part of Auxo Linux" in out, out)
        out = sh.run("auxo-tweak nvidia on")
        check("…nvidia too", "auxo-tweak nvidia is part of Auxo Linux" in out, out)
        out = sh.run("auxo-tweak frobnicate")
        check("unknown sub-command shows usage", "unknown command 'frobnicate'" in out and "auxo-tweak accent" in out, out)
        out = sh.run("auxo-tweak help")
        check("help", "interactive menu" in out and "auxo-tweak persist" in out, out)

        sh.send("auxo-tweak\r", until=b"choose: ")
        sh.send("1\r", until=b"accent: ")
        out = clean(sh.send("cyan\r", until=b"choose: "))
        check("interactive menu: accent cyan", "accent: cyan" in out, out)
        out = clean(sh.send("q\r"))
        check("menu quits back to the prompt", out.rstrip().endswith("❯"), out)

        out = sh.run("auxo-tweak color off")
        raw = sh.send("auxo-fetch\r")
        check("color off: no escapes on serial", b"\x1b" not in raw and b"auxo@summit" in raw, raw[:300].decode("utf-8", "replace"))
        sh.run("auxo-tweak color on")
        out = sh.run("auxo-tweak reset")
        check("reset", "settings: defaults" in out and "auxo@auxobsd ❯" in out, out)
        out = sh.run("auxo-tweak info")
        check("…back to violet / motd on", re.search(r"accent\s+violet", out) and re.search(r"motd\s+on", out), out)

        print("▲ auxo-motd")
        out = sh.run("auxo-motd")
        check("auxo-motd", "Welcome to AuxoBSD, auxo." in out and ("tip:" in out or "terryfox.org" in out), out)
        out = sh.run("auxo-motd --give")
        check("auxo-motd --give points to the Terry Fox Foundation", "https://terryfox.org/?form=GENERAL" in out, out)
        out = sh.run("auxo-motd --tip")
        check("auxo-motd --tip is one line", len([l for l in out.splitlines()[1:] if l.strip() and "❯" not in l]) == 1, out)

        print("▲ auxsh")
        out = sh.run("uname -a")
        check("uname -a", "AuxoBSD auxobsd 0.1.0 x86_64" in out, out)
        out = sh.run("ps")
        check("ps lists idle + auxsh", "idle" in out and "auxsh" in out, out)
        out = sh.run("free")
        check("free", "Mem:" in out and "Heap:" in out, out)
        out = sh.run("uptime")
        check("uptime", out.count("up ") >= 1 and "threads" in out, out)
        out = sh.run("demo", timeout=15)
        check("demo: threads + int 0x80 syscalls", "[worker 3]" in out and "write(2) from a kernel thread" in out, out)
        out = sh.run("echo hello \"auxo world\"")
        check("echo with quotes", "hello auxo world" in out, out)
        out = clean(sh.send("auxo-tw\t info\r"))
        check("Tab completes auxo-tweak", "Auxo settings" in out, out)
        out = clean(sh.send("\x1b[A\r"))
        check("Up arrow recalls the last command", "Auxo settings" in out, out)
        out = sh.run("nosuchcmd")
        check("unknown command", "nosuchcmd: command not found" in out, out)
        out = sh.run("help")
        check("help lists the Auxo tools", "auxo-fetch" in out and "auxo-tweak" in out and "auxo-motd" in out, out)

        if "files from the root filesystem image" in log:
            print("▲ Linux programs (root filesystem)")
            out = sh.run("busybox echo hello from busybox", timeout=30)
            check("BusyBox runs (static Linux binary)", "hello from busybox" in out, out)
            out = sh.run("sh -c 'echo piped | busybox tr a-z A-Z'", timeout=30)
            check("busybox sh with a pipe", "PIPED" in out, out)
            out = sh.run("gcc --version", timeout=60)
            check("gcc --version", "gcc" in out and "13." in out, out)
            out = sh.run("gcc hello.c -o hello", timeout=180)
            check("gcc compiles and links hello.c", "error" not in out.lower() and "segfault" not in out, out)
            out = sh.run("./hello", timeout=30)
            check("…and the program runs", "Hello, world!" in out, out)
            out = sh.run("ls hello", timeout=10)
            check("the binary is in the filesystem", "hello" in out, out)

            print("▲ CPU and memory accounting (System Monitor, /proc)")
            out = sh.run("ps")
            idle = re.search(r"^0\s+\S+\s+(\d+)\s+idle", out, re.M)
            check("waiting for a key leaves the CPU idle (idle thread gets the time)", idle and int(idle.group(1)) > 50, out)
            out = sh.run("busybox sh -c 'head -1 /proc/stat; busybox sleep 2; head -1 /proc/stat'", timeout=30)
            cpu = [list(map(int, l.split()[1:5])) for l in out.splitlines() if l.startswith("cpu ")]
            check("/proc/stat: idle time grows while nothing runs", len(cpu) == 2 and cpu[1][3] - cpu[0][3] >= 100, out)
            out = sh.run("busybox sh -c 'i=0; while [ $i -lt 30000 ]; do i=$((i+1)); done; cat /proc/$$/stat'", timeout=120)
            line = next((l for l in out.splitlines() if re.match(r"^\d+ \(", l)), "")
            f = line.split(") ")[-1].split() if line else []
            check("/proc/PID/stat: a busy program has CPU time and a resident set", len(f) > 22 and int(f[11]) > 0 and int(f[21]) > 0, out)
            out = sh.run("chrono open sysmon")
            check("chrono open sysmon (System Monitor)", "no app" not in out, out)
        else:
            print("▲ Linux programs: skipped (no build/rootfs.tar; run make rootfs)")

        print("▲ power")
        sh.s.sendall(b"poweroff\r")
        try:
            sh.p.wait(timeout=10)
            check("poweroff stops the machine", True)
        except subprocess.TimeoutExpired:
            check("poweroff stops the machine", False, sh.text(len(sh.raw) - 300))
    finally:
        sh.close()

    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
